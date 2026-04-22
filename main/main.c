#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "bsp_display.h"
#include "bsp_touch.h"
#include "ui.h"

static const char *TAG = "gm_s3";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing display...");
    ESP_ERROR_CHECK(bsp_display_init());

    ESP_LOGI(TAG, "Initializing touch...");
    ESP_ERROR_CHECK(bsp_touch_init());

    ESP_LOGI(TAG, "Starting UI...");
    lvgl_port_lock(0);
    ui_init(lv_screen_active());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Ready.");
}
