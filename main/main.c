#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "bsp_display.h"
#include "bsp_touch.h"
#include "ui.h"

static const char *TAG = "gm_s3";

/* Adapts esp_err_t-returning BSP API to the ui_brightness_cb_t signature. */
static void brightness_cb(int percent)
{
    esp_err_t err = bsp_display_set_backlight_percent(percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set backlight %d%% failed: %s", percent, esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing display...");
    ESP_ERROR_CHECK(bsp_display_init());

    ESP_LOGI(TAG, "Initializing touch...");
    ESP_ERROR_CHECK(bsp_touch_init());

    ESP_LOGI(TAG, "Starting UI...");
    lvgl_port_lock(0);
    ui_init(lv_screen_active(), brightness_cb);
    lvgl_port_unlock();

    /* The initial brightness_cb fires from inside ui_init while the port
     * lock is held, so the backlight turns on ~1 LVGL tick before the
     * first frame actually reaches the panel and a brief flash of
     * power-on VRAM is visible. If we ever want a pristine boot we'd
     * need to defer that first brightness_cb to here (post-unlock) and
     * wait for a flush - e.g. expose ui_sync_initial_brightness() that
     * main calls after a short vTaskDelay or an explicit flush-ready
     * signal from esp_lvgl_port. Not worth the extra machinery today. */

    ESP_LOGI(TAG, "Ready.");
}
