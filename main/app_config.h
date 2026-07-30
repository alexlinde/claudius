#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MAX_WIFI_NETWORKS 3
#define APP_SSID_LEN          33
#define APP_PASS_LEN          65
#define APP_NAME_LEN          64
#define APP_HOST_LEN          64
#define APP_SECRET_LEN        64

typedef struct {
    char ssid[APP_SSID_LEN];
    char password[APP_PASS_LEN];
} app_wifi_entry_t;

typedef struct {
    app_wifi_entry_t wifi[APP_MAX_WIFI_NETWORKS];
    uint8_t wifi_count;
    char device_name[APP_NAME_LEN];
    char companion_name[APP_NAME_LEN];
    char companion_host[APP_HOST_LEN];
    char companion_secret[APP_SECRET_LEN];
    bool sleep_on_disconnect;
    bool sleep_on_idle;
} app_config_t;

extern app_config_t g_cfg;

void app_config_set_defaults(void);
esp_err_t app_config_load(void);
esp_err_t app_config_save(void);

#ifdef __cplusplus
}
#endif
