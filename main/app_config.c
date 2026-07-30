#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "app_config";
static const char *NVS_NS = "claudius";
static const char *NVS_NS_LEGACY = "codelight"; /* pre-rename */

app_config_t g_cfg;

void app_config_set_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.device_name, sizeof(g_cfg.device_name), "claudius");
    g_cfg.sleep_on_disconnect = true;
    g_cfg.sleep_on_idle = true;
    g_cfg.brightness = 50;
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static esp_err_t load_from_handle(nvs_handle_t h)
{
    get_str(h, "device_name", g_cfg.device_name, sizeof(g_cfg.device_name));
    get_str(h, "comp_name", g_cfg.companion_name, sizeof(g_cfg.companion_name));
    get_str(h, "comp_host", g_cfg.companion_host, sizeof(g_cfg.companion_host));
    get_str(h, "comp_secret", g_cfg.companion_secret, sizeof(g_cfg.companion_secret));

    uint8_t u8 = 1;
    if (nvs_get_u8(h, "sleep_disc", &u8) == ESP_OK) g_cfg.sleep_on_disconnect = u8 != 0;
    u8 = 1;
    if (nvs_get_u8(h, "sleep_idle", &u8) == ESP_OK) g_cfg.sleep_on_idle = u8 != 0;
    u8 = 50;
    if (nvs_get_u8(h, "brightness", &u8) == ESP_OK) {
        if (u8 < 1) u8 = 1;
        if (u8 > 100) u8 = 100;
        g_cfg.brightness = u8;
    }

    uint8_t count = 0;
    if (nvs_get_u8(h, "wifi_count", &count) == ESP_OK && count > APP_MAX_WIFI_NETWORKS) {
        count = APP_MAX_WIFI_NETWORKS;
    }
    g_cfg.wifi_count = count;
    for (uint8_t i = 0; i < count; i++) {
        char kssid[16], kpass[16];
        snprintf(kssid, sizeof(kssid), "ssid%u", i);
        snprintf(kpass, sizeof(kpass), "pass%u", i);
        get_str(h, kssid, g_cfg.wifi[i].ssid, sizeof(g_cfg.wifi[i].ssid));
        get_str(h, kpass, g_cfg.wifi[i].password, sizeof(g_cfg.wifi[i].password));
    }
    return ESP_OK;
}

esp_err_t app_config_load(void)
{
    app_config_set_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    bool from_legacy = false;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_open(NVS_NS_LEGACY, NVS_READONLY, &h);
        from_legacy = (err == ESP_OK);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config; using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    load_from_handle(h);
    nvs_close(h);

    if (g_cfg.device_name[0] == '\0') {
        snprintf(g_cfg.device_name, sizeof(g_cfg.device_name), "claudius");
    }
    ESP_LOGI(TAG, "Loaded config: device=%s wifi=%u companion=%s host=%s%s",
             g_cfg.device_name, g_cfg.wifi_count,
             g_cfg.companion_name[0] ? g_cfg.companion_name : "(any)",
             g_cfg.companion_host[0] ? g_cfg.companion_host : "(mdns)",
             from_legacy ? " (migrated)" : "");

    if (from_legacy) {
        /* Persist under the new namespace so the next boot doesn't need legacy. */
        app_config_save();
    }
    return ESP_OK;
}

esp_err_t app_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "device_name", g_cfg.device_name);
    nvs_set_str(h, "comp_name", g_cfg.companion_name);
    nvs_set_str(h, "comp_host", g_cfg.companion_host);
    nvs_set_str(h, "comp_secret", g_cfg.companion_secret);
    nvs_set_u8(h, "sleep_disc", g_cfg.sleep_on_disconnect ? 1 : 0);
    nvs_set_u8(h, "sleep_idle", g_cfg.sleep_on_idle ? 1 : 0);
    nvs_set_u8(h, "brightness", g_cfg.brightness);

    if (g_cfg.wifi_count > APP_MAX_WIFI_NETWORKS) {
        g_cfg.wifi_count = APP_MAX_WIFI_NETWORKS;
    }
    nvs_set_u8(h, "wifi_count", g_cfg.wifi_count);
    for (uint8_t i = 0; i < g_cfg.wifi_count; i++) {
        char kssid[16], kpass[16];
        snprintf(kssid, sizeof(kssid), "ssid%u", i);
        snprintf(kpass, sizeof(kpass), "pass%u", i);
        nvs_set_str(h, kssid, g_cfg.wifi[i].ssid);
        nvs_set_str(h, kpass, g_cfg.wifi[i].password);
    }

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved");
    return err;
}
