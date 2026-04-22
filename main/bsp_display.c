#include "bsp_display.h"

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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BSP_LCD_SPI_HOST     SPI3_HOST
#define BSP_LCD_MOSI         GPIO_NUM_11
#define BSP_LCD_SCLK         GPIO_NUM_12
#define BSP_LCD_DC           GPIO_NUM_7
#define BSP_LCD_RST          GPIO_NUM_6
#define BSP_LCD_BL           GPIO_NUM_14
#define BSP_LCD_H_RES        240
#define BSP_LCD_V_RES        240
#define BSP_LCD_SPI_FREQ_HZ  (80 * 1000 * 1000)
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

    ESP_LOGI(TAG, "Display ready (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}
