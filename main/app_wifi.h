#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_AP_SSID "claudius-setup"

typedef enum {
    APP_WIFI_MODE_STA = 0,
    APP_WIFI_MODE_AP,
} app_wifi_mode_t;

esp_err_t app_wifi_start(void);
bool app_wifi_is_sta_connected(void);
app_wifi_mode_t app_wifi_mode(void);
esp_netif_t *app_wifi_netif(void);

#ifdef __cplusplus
}
#endif
