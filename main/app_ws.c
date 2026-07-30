#include "app_ws.h"
#include "app_config.h"
#include "app_dbg.h"
#include "app_mdns.h"
#include "app_proto.h"
#include "app_wifi.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_ws";

#define WS_DISCOVER_MS       15000
#define WS_URI_LEN           96
#define WS_RX_MAX            2048
#define WS_RX_QUEUE_LEN      6

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
static volatile bool s_transport_ever_up;
static bool s_hello_sent;
static bool s_need_hello;
static bool s_need_auth;
static char s_auth_reply[160];
static bool s_auth_failed;
static bool s_request_reconnect;
static volatile bool s_ui_offline;
static volatile bool s_ui_online;
static char s_last_ip[48];
static uint16_t s_last_port = 8765;
static bool s_have_last;
static int64_t s_last_companion_ms;
static int64_t s_last_active_ms;
static status_snapshot_t s_last_status;
static QueueHandle_t s_rxq;

typedef struct {
    uint16_t len;
    char data[WS_RX_MAX];
} ws_rx_msg_t;

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

static void flush_pending_sends(void)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return;

    if (s_need_auth) {
        s_need_auth = false;
        send_txt(s_auth_reply);
        s_need_hello = true;
    }

    if (s_need_hello && !s_hello_sent) {
        char buf[64];
        app_proto_build_subscribe(buf, sizeof(buf));
        send_txt(buf);
        s_hello_sent = true;
        s_need_hello = false;
        app_dbg_log("ws: subscribed as screen");
    }
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
    /* Heap-allocate parse result — logos make this too big for small stacks. */
    app_proto_msg_t *msg = calloc(1, sizeof(*msg));
    if (!msg) return;
    if (!app_proto_parse(data, (size_t)len, msg)) {
        free(msg);
        return;
    }

    if (!s_connected) {
        s_connected = true;
        s_ui_online = true;
        app_dbg_log("ws: connected");
    }
    touch_companion();

    switch (msg->kind) {
    case APP_PROTO_UNAUTHORIZED:
        app_dbg_log("ws: unauthorized – check companion secret");
        s_auth_failed = true;
        s_connected = false;
        s_ui_offline = true;
        if (s_client) esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
        break;

    case APP_PROTO_CHALLENGE:
        if (g_cfg.companion_secret[0]) {
            if (app_proto_build_auth_hmac(g_cfg.companion_secret, msg->nonce,
                                          s_auth_reply, sizeof(s_auth_reply)) > 0) {
                s_need_auth = true;
            }
        } else {
            app_dbg_log("ws: challenge received but no secret configured");
        }
        break;

    case APP_PROTO_CONFIG:
        if (!s_hello_sent) s_need_hello = true;
        apply_config(&msg->config);
        break;

    case APP_PROTO_STATUS:
        if (!s_hello_sent) s_need_hello = true;
        apply_status(&msg->status);
        break;

    case APP_PROTO_IGNORE:
        if (!s_hello_sent) s_need_hello = true;
        break;
    }
    free(msg);
}

static void ws_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        /* Flags only — no UI, no send, from this task. */
        s_auth_failed = false;
        s_hello_sent = false;
        s_need_auth = false;
        s_connected = true;
        s_transport_ever_up = true;
        s_ui_online = true;
        s_need_hello = !g_cfg.companion_secret[0];
        touch_active();
        touch_companion();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (s_connected || s_auth_failed) {
            app_dbg_log("ws: disconnected%s", s_auth_failed ? " (auth)" : "");
        }
        s_connected = false;
        s_hello_sent = false;
        s_need_hello = false;
        s_need_auth = false;
        s_ui_offline = true;
        touch_companion();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x1 && data->data_ptr && data->data_len > 0 &&
            data->payload_offset == 0 && data->payload_len == data->data_len &&
            data->data_len < WS_RX_MAX && s_rxq) {
            ws_rx_msg_t *msg = malloc(sizeof(*msg));
            if (!msg) break;
            msg->len = (uint16_t)data->data_len;
            memcpy(msg->data, data->data_ptr, (size_t)data->data_len);
            msg->data[data->data_len] = '\0';
            if (xQueueSend(s_rxq, &msg, 0) != pdTRUE) {
                free(msg);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        if (data && data->data_ptr && data->data_len > 0) {
            int n = data->data_len > 60 ? 60 : data->data_len;
            app_dbg_log("ws: err %.*s", n, data->data_ptr);
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
    s_need_hello = false;
    s_need_auth = false;
    if (s_rxq) {
        ws_rx_msg_t *msg;
        while (xQueueReceive(s_rxq, &msg, 0) == pdTRUE) {
            free(msg);
        }
    }
}

static bool begin_ws(const char *host, uint16_t port)
{
    destroy_client();

    static char uri[WS_URI_LEN];
    snprintf(uri, sizeof(uri), "ws://%s:%u/", host, (unsigned)port);
    app_dbg_log("ws: connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
        .disable_auto_reconnect = true,
        .disable_pingpong_discon = true,
        .network_timeout_ms = 10000,
        .buffer_size = 4096,
        .task_stack = 8192,
        .ping_interval_sec = 30,
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

static void flush_ui_flags(void)
{
    if (s_ui_online) {
        s_ui_online = false;
        ui_set_auth_failed(false);
        if (!s_last_status.connected) {
            status_snapshot_t st = s_last_status;
            st.connected = true;
            if (st.status == AGENT_STATUS_OFFLINE) st.status = AGENT_STATUS_IDLE;
            apply_status(&st);
        }
        ui_wake();
        app_dbg_log("ws: connected");
    }
    if (s_ui_offline) {
        s_ui_offline = false;
        if (s_auth_failed) {
            ui_set_auth_failed(true);
        } else if (!s_connected) {
            ui_set_connected(false);
        }
    }
}

static void ws_task(void *arg)
{
    (void)arg;
    /* Queue of pointers — keep item size small. */
    s_rxq = xQueueCreate(WS_RX_QUEUE_LEN, sizeof(ws_rx_msg_t *));
    s_last_companion_ms = now_ms();
    s_last_active_ms = now_ms();

    try_discover();
    int64_t last_discover = now_ms();

    for (;;) {
        if (!app_wifi_is_sta_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ws_rx_msg_t *rx;
        while (s_rxq && xQueueReceive(s_rxq, &rx, 0) == pdTRUE) {
            on_text(rx->data, rx->len);
            free(rx);
        }

        flush_pending_sends();
        flush_ui_flags();

        if (s_request_reconnect) {
            s_request_reconnect = false;
            s_auth_failed = false;
            destroy_client();
            try_discover();
            last_discover = now_ms();
        }

        bool transport_up = s_client && esp_websocket_client_is_connected(s_client);
        if (s_connected || transport_up) {
            /* Healthy session — don't let the discover timer expire. */
            last_discover = now_ms();
        }

        bool need_discover = !s_client ||
            (!s_connected && !transport_up && !s_auth_failed &&
             (now_ms() - last_discover) >= WS_DISCOVER_MS);

        if (need_discover) {
            last_discover = now_ms();
            if (!s_connected && !transport_up) {
                destroy_client();
                try_discover();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t app_ws_start(void)
{
    if (!app_wifi_is_sta_connected()) {
        app_dbg_log("ws: not starting (AP mode)");
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(ws_task, "app_ws", 12288, NULL, 5, NULL);
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
