#include "app_ws.h"
#include "app_config.h"
#include "app_dbg.h"
#include "app_mdns.h"
#include "app_proto.h"
#include "app_wifi.h"
#include "ui.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_ws";

#define WS_DISCOVER_MS       15000
#define WS_RECONNECT_MS      15000
#define WS_URI_LEN           96

static esp_websocket_client_handle_t s_client;
static bool s_connected;
static bool s_hello_sent;
static bool s_auth_failed;
static bool s_request_reconnect;
static char s_last_ip[48];
static uint16_t s_last_port = 8765;
static bool s_have_last;
static int64_t s_last_companion_ms;
static int64_t s_last_active_ms;
static status_snapshot_t s_last_status;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void touch_companion(void)
{
    s_last_companion_ms = now_ms();
}

static void touch_active(void)
{
    s_last_active_ms = now_ms();
}

static void send_txt(const char *msg)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return;
    esp_websocket_client_send_text(s_client, msg, strlen(msg), pdMS_TO_TICKS(2000));
}

static void send_hello(void)
{
    if (s_hello_sent) return;
    char buf[64];
    app_proto_build_subscribe(buf, sizeof(buf));
    send_txt(buf);
    s_hello_sent = true;
    app_dbg_log("ws: subscribed as screen");
}

static void apply_config(const app_proto_config_t *cfg)
{
    if (cfg->have_utc_offset) {
        ui_set_utc_offset(cfg->utc_offset);
        app_dbg_log("ws: utc_offset=%ld", cfg->utc_offset);
    }
    ui_set_agent_logos(cfg->logos, cfg->logo_count);
    app_dbg_log("ws: config logos=%d", cfg->logo_count);
}

static void apply_status(const status_snapshot_t *st)
{
    agent_status_t prev = s_last_status.status;
    bool was_connected = s_last_status.connected;
    bool was_sleeping = ui_is_sleeping();

    s_last_status = *st;
    ui_set_status(st);

    if (st->status == AGENT_STATUS_WORKING || st->status == AGENT_STATUS_WAITING) {
        touch_active();
    }

    if ((was_sleeping && !was_connected) ||
        (st->status != AGENT_STATUS_IDLE && st->status != prev)) {
        ui_wake();
    }
}

static void on_text(const char *data, int len)
{
    app_proto_msg_t msg;
    if (!app_proto_parse(data, (size_t)len, &msg)) return;

    switch (msg.kind) {
    case APP_PROTO_UNAUTHORIZED:
        app_dbg_log("ws: unauthorized – check companion secret");
        s_auth_failed = true;
        s_connected = false;
        ui_set_auth_failed(true);
        if (s_client) esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
        break;

    case APP_PROTO_CHALLENGE:
        if (g_cfg.companion_secret[0]) {
            char reply[160];
            if (app_proto_build_auth_hmac(g_cfg.companion_secret, msg.nonce,
                                          reply, sizeof(reply)) > 0) {
                send_txt(reply);
                send_hello();
            }
        } else {
            /* Companion challenged but we have no secret — will fail. */
            app_dbg_log("ws: challenge received but no secret configured");
        }
        break;

    case APP_PROTO_CONFIG:
        send_hello();
        apply_config(&msg.config);
        break;

    case APP_PROTO_STATUS:
        send_hello();
        apply_status(&msg.status);
        break;

    case APP_PROTO_IGNORE:
        send_hello();
        break;
    }
}

static void ws_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        app_dbg_log("ws: connected");
        s_connected = true;
        s_auth_failed = false;
        s_hello_sent = false;
        touch_companion();
        touch_active();
        ui_set_auth_failed(false);
        ui_wake();
        /* Unsecured daemon: hello on first data. Secured: wait for challenge. */
        if (!g_cfg.companion_secret[0]) {
            send_hello();
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (s_connected || s_auth_failed) {
            app_dbg_log("ws: disconnected%s", s_auth_failed ? " (auth)" : "");
        }
        s_connected = false;
        s_hello_sent = false;
        touch_companion();
        if (s_auth_failed) {
            ui_set_auth_failed(true);
        } else {
            ui_set_connected(false);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {
            /* Text frame — may be fragmented; only handle complete messages. */
            if (data->payload_offset == 0 && data->payload_len == data->data_len) {
                on_text(data->data_ptr, data->data_len);
            }
        }
        break;

    default:
        break;
    }
}

static void destroy_client(void)
{
    if (!s_client) return;
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
}

static bool begin_ws(const char *host, uint16_t port)
{
    destroy_client();

    char uri[WS_URI_LEN];
    snprintf(uri, sizeof(uri), "ws://%s:%u/", host, (unsigned)port);
    app_dbg_log("ws: connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = WS_RECONNECT_MS,
        .network_timeout_ms = 10000,
        .buffer_size = 4096,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return false;
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        destroy_client();
        return false;
    }
    snprintf(s_last_ip, sizeof(s_last_ip), "%s", host);
    s_last_port = port;
    s_have_last = true;
    return true;
}

static void try_discover(void)
{
    if (s_auth_failed) return;

    if (g_cfg.companion_host[0]) {
        begin_ws(g_cfg.companion_host, 8765);
        return;
    }

    char ip[48];
    uint16_t port = 8765;
    if (app_mdns_discover_companion(ip, sizeof(ip), &port, 2000)) {
        begin_ws(ip, port);
    }
}

static void ws_task(void *arg)
{
    (void)arg;
    s_last_companion_ms = now_ms();
    s_last_active_ms = now_ms();

    /* Initial discovery */
    try_discover();
    int64_t last_discover = now_ms();

    for (;;) {
        if (!app_wifi_is_sta_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (s_request_reconnect) {
            s_request_reconnect = false;
            s_auth_failed = false;
            destroy_client();
            try_discover();
            last_discover = now_ms();
        }

        bool need_discover = !s_client ||
            (!s_connected && !s_auth_failed &&
             (now_ms() - last_discover) >= WS_DISCOVER_MS);

        if (need_discover) {
            last_discover = now_ms();
            if (!s_connected) {
                destroy_client();
                try_discover();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t app_ws_start(void)
{
    if (!app_wifi_is_sta_connected()) {
        app_dbg_log("ws: not starting (AP mode)");
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(ws_task, "app_ws", 6144, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void app_ws_request_reconnect(void)
{
    s_request_reconnect = true;
    s_auth_failed = false;
}

bool app_ws_is_connected(void)
{
    return s_connected;
}

int64_t app_ws_last_companion_ms(void)
{
    return s_last_companion_ms;
}

int64_t app_ws_last_active_ms(void)
{
    return s_last_active_ms;
}
