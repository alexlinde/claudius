#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_display.h"
#include "bsp_touch.h"
#include "ui.h"

#include "app_config.h"
#include "app_dbg.h"
#include "app_mdns.h"
#include "app_webcfg.h"
#include "app_wifi.h"
#include "app_ws.h"

static const char *TAG = "gm_s3";

#define SLEEP_AFTER_MS      (10ULL * 60ULL * 1000ULL)
#define IDLE_SLEEP_AFTER_MS (60ULL * 60ULL * 1000ULL)

static void brightness_cb(int percent)
{
    esp_err_t err = bsp_display_set_backlight_percent(percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set backlight %d%% failed: %s", percent, esp_err_to_name(err));
    }
}

static void show_boot_hint(void)
{
    status_snapshot_t snap = {0};
    snap.connected = false;
    if (app_wifi_mode() == APP_WIFI_MODE_AP) {
        snprintf(snap.weekly_title, sizeof(snap.weekly_title), "Setup mode");
        snap.weekly_pct = 0;
        snprintf(snap.weekly_reset, sizeof(snap.weekly_reset), "AP");
        snprintf(snap.session_title, sizeof(snap.session_title), APP_AP_SSID);
        snprintf(snap.session_reset, sizeof(snap.session_reset), "cfg");
        snap.session_pct = 0;
        snap.session_count = 0;
        snprintf(snap.agent_display, sizeof(snap.agent_display), "Setup");
    }
    ui_set_status(&snap);
}

static void sleep_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        ui_tick((uint32_t)now);

        if (!ui_is_sleeping()) {
            bool disc_sleep = g_cfg.sleep_on_disconnect
                && app_wifi_is_sta_connected()
                && !app_ws_is_connected()
                && (now - app_ws_last_companion_ms()) >= (int64_t)SLEEP_AFTER_MS;
            bool idle_sleep = g_cfg.sleep_on_idle
                && app_ws_is_connected()
                && (now - app_ws_last_active_ms()) >= (int64_t)IDLE_SLEEP_AFTER_MS;

            /* Only idle-sleep when last known status was idle — approximated
             * by last_active not advancing. disc_sleep is clear. */
            if (disc_sleep || idle_sleep) {
                if (!ui_is_washing()) {
                    app_dbg_log("sleep: starting (%s)", disc_sleep ? "disconnect" : "idle");
                    ui_sleep_start();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "claudius S3 screen starting…");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(app_config_load());

    ESP_LOGI(TAG, "Initializing display…");
    ESP_ERROR_CHECK(bsp_display_init());

    ESP_LOGI(TAG, "Initializing touch…");
    ESP_ERROR_CHECK(bsp_touch_init());

    ESP_LOGI(TAG, "Starting UI…");
    lvgl_port_lock(0);
    ui_init(lv_screen_active(), brightness_cb);
    lvgl_port_unlock();
    ui_set_title(g_cfg.device_name);
    ui_set_brightness(g_cfg.brightness);

    app_dbg_log("boot: wifi…");
    ESP_ERROR_CHECK(app_wifi_start());
    show_boot_hint();

    ESP_ERROR_CHECK(app_webcfg_start());
    ESP_ERROR_CHECK(app_mdns_start());
    ESP_ERROR_CHECK(app_ws_start());

    xTaskCreate(sleep_supervisor_task, "sleep_sup", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Ready.");
    app_dbg_log("boot: ready");
}
