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

/* Backlight is active-LOW: duty 255 = off, duty 0 = full brightness */
#define BSP_BL_DUTY_ON       0

static const char *TAG = "bsp_display";

static esp_err_t bsp_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 255,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");
    return ESP_OK;
}

static esp_err_t bsp_backlight_set(uint8_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "Set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "Update duty failed");
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

    ESP_LOGI(TAG, "Backlight on...");
    ESP_RETURN_ON_ERROR(bsp_backlight_set(BSP_BL_DUTY_ON), TAG, "Backlight on failed");

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
