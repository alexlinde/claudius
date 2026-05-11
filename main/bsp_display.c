#include "bsp_display.h"

#include <stdatomic.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#define BSP_LCD_SPI_HOST     SPI3_HOST
#define BSP_LCD_MOSI         GPIO_NUM_11
#define BSP_LCD_SCLK         GPIO_NUM_12
#define BSP_LCD_DC           GPIO_NUM_7
#define BSP_LCD_RST          GPIO_NUM_6
#define BSP_LCD_BL           GPIO_NUM_14
#define BSP_LCD_H_RES        240
#define BSP_LCD_V_RES        240
/* 40 MHz is the conservative ST7789 sweet spot for this board: signal
 * integrity over the flying-lead wiring is marginal at 80 MHz, and at
 * 80 MHz under sustained load (WiFi + Octal-PSRAM at 80 MHz + cJSON/TLS)
 * the panel IO `on_color_trans_done` ISR occasionally stops firing,
 * leaving LVGL's `wait_for_flushing` busy-spinning until the task
 * watchdog resets the device. 40 MHz is well within ST7789 spec and a
 * 240x240 RGB565 frame still pushes in ~6 ms, plenty fast for the
 * single-screen UX here. */
#define BSP_LCD_SPI_FREQ_HZ  (40 * 1000 * 1000)
#define BSP_LCD_CMD_BITS     8
#define BSP_LCD_PARAM_BITS   8
#define BSP_LCD_SPI_MODE     3

/* Backlight PWM. 10-bit resolution (1024 steps) for smooth low-end dimming;
 * the panel's backlight net is active-LOW, but we invert at the LEDC output
 * (flags.output_invert) so duty maps forward: 0 = off, max = full. */
#define BSP_BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_TIMER    LEDC_TIMER_0
#define BSP_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BSP_BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BSP_BL_DUTY_MAX      ((1u << 10) - 1u)  /* 1023 */
#define BSP_BL_FREQ_HZ       5000

static const char *TAG = "bsp_display";

/* ---------------------------------------------------------------------------
 * Flush-health instrumentation.
 *
 * For SPI panel IO esp_lvgl_port leaves LVGL's `flush_wait_cb` unset, so
 * `wait_for_flushing()` busy-spins until the panel IO ISR clears
 * `disp->flushing` via lv_disp_flush_ready. If the SPI completion ISR ever
 * stops firing (clock too high, bus contention, peripheral hang, etc.), the
 * busy loop is permanent and starves IDLE -> task watchdog. We hook the
 * LVGL flush-wait events to expose how long the in-progress wait has been
 * pending; the heap monitor in main.c samples this and warns on hang.
 *
 * Atomics: flush events fire on the LVGL render thread; the sampler reads
 * them from an esp_timer task. uint32 wraparound is fine for counts; the
 * pending-us field uses a single-writer/multi-reader pattern with
 * memory_order_relaxed which is enough since we're only ever asking
 * "approximately how long has the current wait been outstanding". */

static _Atomic uint32_t s_flush_starts;
static _Atomic uint32_t s_flush_finishes;
static _Atomic uint32_t s_wait_starts;
static _Atomic uint32_t s_wait_finishes;
static _Atomic int64_t  s_wait_start_us;   /* 0 when not inside a wait */

static void on_flush_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_FLUSH_START:
        atomic_fetch_add_explicit(&s_flush_starts, 1, memory_order_relaxed);
        break;
    case LV_EVENT_FLUSH_FINISH:
        atomic_fetch_add_explicit(&s_flush_finishes, 1, memory_order_relaxed);
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        atomic_store_explicit(&s_wait_start_us,
                              esp_timer_get_time(), memory_order_relaxed);
        atomic_fetch_add_explicit(&s_wait_starts, 1, memory_order_relaxed);
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        atomic_store_explicit(&s_wait_start_us, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_wait_finishes, 1, memory_order_relaxed);
        break;
    default:
        break;
    }
}

void bsp_display_get_flush_stats(bsp_display_flush_stats_t *out)
{
    if (!out) return;
    out->flush_starts   = atomic_load_explicit(&s_flush_starts,   memory_order_relaxed);
    out->flush_finishes = atomic_load_explicit(&s_flush_finishes, memory_order_relaxed);
    out->wait_starts    = atomic_load_explicit(&s_wait_starts,    memory_order_relaxed);
    out->wait_finishes  = atomic_load_explicit(&s_wait_finishes,  memory_order_relaxed);
    int64_t ws = atomic_load_explicit(&s_wait_start_us, memory_order_relaxed);
    out->pending_us = ws ? (esp_timer_get_time() - ws) : 0;
}

/* ---------------------------------------------------------------------------
 * SPI-tx-failure-tolerant flush waiter.
 *
 * The default esp_lvgl_port flow for SPI panel IO leaves LVGL's flush_wait_cb
 * unset and relies on the panel-IO `on_color_trans_done` ISR to clear
 * `disp->flushing` via lv_disp_flush_ready(). When `esp_lcd_panel_draw_bitmap`
 * fails (e.g. spi_master::setup_dma_priv_buffer returns ENOMEM under transient
 * internal-SRAM pressure from a concurrent cJSON parse + TLS handshake), the
 * transaction is never queued, the ISR never fires, and LVGL falls into the
 * `while(disp->flushing);` busy-spin in lv_refr.c -> task watchdog.
 *
 * We replace the panel-IO color-trans-done callback with our own that gives a
 * binary semaphore (and still calls lv_disp_flush_ready for compatibility),
 * and install a flush_wait_cb that takes the semaphore with a bounded
 * timeout. On timeout we log and return; LVGL clears `disp->flushing` after
 * we return so the next frame can render. We accept potential one-frame
 * tearing in exchange for not wedging the device.
 * ------------------------------------------------------------------------- */

#define FLUSH_TIMEOUT_MS 200

static SemaphoreHandle_t s_flush_done_sem;
static _Atomic uint32_t  s_flush_timeouts;
static _Atomic uint32_t  s_flush_late_arrivals;

static bool IRAM_ATTR on_panel_io_color_done(esp_lcd_panel_io_handle_t io,
                                              esp_lcd_panel_io_event_data_t *e,
                                              void *user_ctx)
{
    BaseType_t hpw = pdFALSE;
    if (s_flush_done_sem) {
        /* Binary sem: a give while count==1 is a no-op, which is exactly what
         * we want if a previous wait_cb timed out and the ISR finally fired
         * after we'd already moved on. */
        xSemaphoreGiveFromISR(s_flush_done_sem, &hpw);
    }
    /* Keep esp_lvgl_port's invariant of clearing LVGL's flushing flag from
     * the ISR. With our flush_wait_cb installed LVGL will re-clear it after
     * wait_cb returns; double-clear is harmless. */
    lv_display_t *disp = (lv_display_t *)user_ctx;
    if (disp) lv_display_flush_ready(disp);
    return hpw == pdTRUE;
}

static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    if (!s_flush_done_sem) return;
    if (xSemaphoreTake(s_flush_done_sem, pdMS_TO_TICKS(FLUSH_TIMEOUT_MS)) == pdTRUE) {
        return;
    }
    /* Timeout: the on_color_trans_done ISR didn't fire within FLUSH_TIMEOUT_MS.
     * Most likely the SPI transaction was never queued because
     * esp_lcd_panel_draw_bitmap returned an error; the ISR will never come.
     * Drain any signal that *does* arrive late so it doesn't get counted
     * against the next flush. Counter is for the heap-monitor log. */
    atomic_fetch_add_explicit(&s_flush_timeouts, 1, memory_order_relaxed);

    /* Best-effort late-arrival drain: if the ISR fires within the next tick
     * (race window), consume it now so the next flush starts clean. */
    if (xSemaphoreTake(s_flush_done_sem, 0) == pdTRUE) {
        atomic_fetch_add_explicit(&s_flush_late_arrivals, 1, memory_order_relaxed);
    }
}

uint32_t bsp_display_flush_timeouts(void)
{
    return atomic_load_explicit(&s_flush_timeouts, memory_order_relaxed);
}

uint32_t bsp_display_flush_late_arrivals(void)
{
    return atomic_load_explicit(&s_flush_late_arrivals, memory_order_relaxed);
}

static esp_err_t bsp_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BSP_BL_LEDC_MODE,
        .duty_resolution = BSP_BL_LEDC_RES,
        .timer_num       = BSP_BL_LEDC_TIMER,
        .freq_hz         = BSP_BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel    = BSP_BL_LEDC_CHANNEL,
        .timer_sel  = BSP_BL_LEDC_TIMER,
        .duty       = 0,                 /* start off until panel is ready */
        .hpoint     = 0,
        .flags      = { .output_invert = 1 },
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");
    return ESP_OK;
}

esp_err_t bsp_display_set_backlight_percent(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    /* Forward map (thanks to output_invert above): 0% -> 0, 100% -> DUTY_MAX. */
    uint32_t duty = ((uint32_t)percent * BSP_BL_DUTY_MAX + 50) / 100;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty),
        TAG, "Set duty failed");
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL),
        TAG, "Update duty failed");
    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    ESP_LOGI(TAG, "Backlight init (off during setup)...");
    ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "Backlight init failed");

    ESP_LOGI(TAG, "SPI bus init (SPI3_HOST, MOSI=%d, SCLK=%d)...",
             BSP_LCD_MOSI, BSP_LCD_SCLK);
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num     = BSP_LCD_SCLK,
        .mosi_io_num     = BSP_LCD_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "SPI bus init failed");

    ESP_LOGI(TAG, "Panel IO init (DC=%d, CS=-1, %d Hz, SPI Mode %d)...",
             BSP_LCD_DC, BSP_LCD_SPI_FREQ_HZ, BSP_LCD_SPI_MODE);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num     = BSP_LCD_DC,
        .cs_gpio_num     = -1,
        .pclk_hz         = BSP_LCD_SPI_FREQ_HZ,
        .lcd_cmd_bits    = BSP_LCD_CMD_BITS,
        .lcd_param_bits  = BSP_LCD_PARAM_BITS,
        .spi_mode        = BSP_LCD_SPI_MODE,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &io_cfg, &io_handle),
        TAG, "Panel IO init failed");

    ESP_LOGI(TAG, "ST7789 panel init (RST=%d)...", BSP_LCD_RST);
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = BSP_LCD_RST,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        /* RGB565 in RAM is LE; SPI sends low byte first. Match ST7789 RAMCTRL. */
        .data_endian     = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel  = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel),
        TAG, "Panel create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "Invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "Disp on failed");
    /* Backlight stays off (LEDC channel was configured with duty=0); the app
     * is expected to call bsp_display_set_backlight_percent() once its first
     * frame is on the panel so the user never sees uninitialised VRAM. */

    ESP_LOGI(TAG, "LVGL port init...");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle   = io_handle,
        .panel_handle = panel,
        .buffer_size  = BSP_LCD_H_RES * 20 * sizeof(lv_color16_t),
        .double_buffer = true,
        .hres         = BSP_LCD_H_RES,
        .vres         = BSP_LCD_V_RES,
        .monochrome   = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "LVGL display add failed");

    /* Replace esp_lvgl_port's panel-IO trans-done callback with ours so a
     * failed esp_lcd_panel_draw_bitmap doesn't deadlock LVGL forever. We
     * also install our own flush_wait_cb that yields with a bounded
     * timeout instead of letting LVGL fall into its default busy-spin. */
    s_flush_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done_sem, ESP_ERR_NO_MEM, TAG, "Flush sem alloc failed");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_panel_io_color_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, disp),
        TAG, "Panel IO callback register failed");

    /* Flush-health hooks. Registering each event individually instead of
     * LV_EVENT_ALL keeps our handler off the per-area redraw events that
     * fire dozens of times per frame. The LVGL task is already running
     * after lvgl_port_add_disp returns, so we must hold the LVGL lock
     * while mutating LVGL state. */
    if (lvgl_port_lock(0)) {
        lv_display_add_event_cb(disp, on_flush_event, LV_EVENT_FLUSH_START,       NULL);
        lv_display_add_event_cb(disp, on_flush_event, LV_EVENT_FLUSH_FINISH,      NULL);
        lv_display_add_event_cb(disp, on_flush_event, LV_EVENT_FLUSH_WAIT_START,  NULL);
        lv_display_add_event_cb(disp, on_flush_event, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
        lv_display_set_flush_wait_cb(disp, flush_wait_cb);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Display ready (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}
