#include "app_wifi.h"
#include "app_config.h"
#include "app_dbg.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "app_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static app_wifi_mode_t s_mode = APP_WIFI_MODE_AP;
static esp_netif_t *s_netif;
static int s_retry;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < 5) {
            s_retry++;
            esp_wifi_connect();
            app_dbg_log("wifi: retry %d", s_retry);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        app_dbg_log("wifi: got IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t start_ap(void)
{
    s_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, APP_AP_SSID, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = strlen(APP_AP_SSID);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_mode = APP_WIFI_MODE_AP;
    app_dbg_log("wifi: AP mode %s / 192.168.4.1", APP_AP_SSID);
    return ESP_OK;
}

static bool try_sta(void)
{
    if (g_cfg.wifi_count == 0) return false;

    s_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    for (uint8_t i = 0; i < g_cfg.wifi_count; i++) {
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, g_cfg.wifi[i].ssid, sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, g_cfg.wifi[i].password, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        s_retry = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        app_dbg_log("wifi: trying %s", g_cfg.wifi[i].ssid);

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            s_mode = APP_WIFI_MODE_STA;
            /* Disable modem sleep for responsive push updates. */
            esp_wifi_set_ps(WIFI_PS_NONE);
            return true;
        }

        esp_wifi_stop();
        app_dbg_log("wifi: failed %s", g_cfg.wifi[i].ssid);
    }
    return false;
}

esp_err_t app_wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    if (try_sta()) return ESP_OK;

    /* Tear down STA netif before AP if we created one. */
    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    return start_ap();
}

bool app_wifi_is_sta_connected(void)
{
    return s_mode == APP_WIFI_MODE_STA;
}

app_wifi_mode_t app_wifi_mode(void)
{
    return s_mode;
}

esp_netif_t *app_wifi_netif(void)
{
    return s_netif;
}
