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
#define WS_AUTH_RETRY_MS     60000
#define WS_HELLO_GRACE_MS    3000
#define WS_URI_LEN           96
#define WS_RX_MAX            8192
#define WS_RX_QUEUE_LEN      6
#define WS_DEFAULT_PORT      8765
#define WS_OP_CONT           0x0
#define WS_OP_TEXT           0x1

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
static volatile bool s_transport_ever_up;
static bool s_hello_sent;
static bool s_need_hello;
static bool s_need_auth;
static char s_auth_reply[160];
static bool s_auth_failed;
static int64_t s_auth_failed_ms;
static volatile int64_t s_connect_ms;
static volatile bool s_ui_offline;
static volatile bool s_ui_online;
static char s_last_ip[48];
static uint16_t s_last_port = WS_DEFAULT_PORT;
static bool s_have_last;
static int64_t s_last_companion_ms;
static int64_t s_last_active_ms;
static status_snapshot_t s_last_status;
static QueueHandle_t s_rxq;

/* Reassembly of the message currently arriving. */
static char *s_rx_buf;
static int s_rx_len;
static int s_rx_base;
static bool s_rx_active;
static bool s_rx_drop;
static int64_t s_rx_drop_log_ms;

typedef struct {
    uint16_t len;
    char data[]; /* NUL-terminated */
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

    /* A companion with a secret challenges the moment we connect; one without
     * never speaks first and waits for our subscribe. So when only we hold a
     * secret, silence means the latter — subscribe instead of deadlocking. */
    if (!s_hello_sent && !s_need_hello && s_connect_ms && g_cfg.companion_secret[0] &&
        (now_ms() - s_connect_ms) >= WS_HELLO_GRACE_MS) {
        s_need_hello = true;
        app_dbg_log("ws: no challenge – subscribing anyway");
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
    bool prev_active = s_last_status.any_active;
    bool was_connected = s_last_status.connected;
    bool was_sleeping = ui_is_sleeping();

    s_last_status = *st;
    ui_set_status(st);

    if (st->any_active) {
        touch_active();
    }

    if ((was_sleeping && !was_connected) ||
        (st->any_active && !prev_active)) {
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

    /* Anything the companion sends past the challenge means it accepted us,
     * so an earlier rejection is stale — release the latch and the UI badge. */
    if (s_auth_failed && msg->kind != APP_PROTO_UNAUTHORIZED &&
        msg->kind != APP_PROTO_CHALLENGE) {
        s_auth_failed = false;
        s_ui_online = true;
    }

    switch (msg->kind) {
    case APP_PROTO_UNAUTHORIZED:
        app_dbg_log("ws: unauthorized – check companion secret");
        s_auth_failed = true;
        s_auth_failed_ms = now_ms();
        s_connected = false;
        s_ui_online = false;
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

static void rx_reset(void)
{
    s_rx_len = 0;
    s_rx_base = 0;
    s_rx_active = false;
    s_rx_drop = false;
}

/* Discard the whole message, logging at most once per 10s so a companion
 * spraying oversize frames can't flood the debug ring. */
static void rx_drop_oversize(void)
{
    if (s_rx_drop) return;
    s_rx_drop = true;
    int64_t t = now_ms();
    if (t - s_rx_drop_log_ms >= 10000) {
        s_rx_drop_log_ms = t;
        app_dbg_log("ws: message over %d bytes – dropped", WS_RX_MAX);
    }
}

/* One message can span many DATA events: the client posts one per transport
 * read, walking payload_offset through the frame, and a fragmented message
 * adds whole continuation frames (op_code 0) after the initial text frame.
 * Assemble here and hand the rx queue only complete messages. */
static void rx_feed(const esp_websocket_event_data_t *ev)
{
    if (!ev || !s_rx_buf) return;
    if (ev->op_code != WS_OP_TEXT && ev->op_code != WS_OP_CONT) return;

    if (ev->payload_offset == 0) {
        if (ev->op_code == WS_OP_TEXT) {
            rx_reset();
            s_rx_active = true;
        }
        s_rx_base = s_rx_len;
    }
    if (!s_rx_active) return; /* continuation of a message we never started */

    if (ev->data_ptr && ev->data_len > 0 && !s_rx_drop) {
        int at = s_rx_base + ev->payload_offset;
        /* Bound 'at' before subtracting from it — payload_offset comes off the
         * wire and must not be trusted to keep the sum in range. */
        if (at < 0 || at >= WS_RX_MAX || ev->data_len >= WS_RX_MAX - at) {
            rx_drop_oversize();
        } else {
            memcpy(s_rx_buf + at, ev->data_ptr, (size_t)ev->data_len);
            s_rx_len = at + ev->data_len;
        }
    }

    if (ev->payload_offset + ev->data_len < ev->payload_len) return;
    if (!ev->fin) return;

    if (!s_rx_drop && s_rx_len > 0 && s_rxq) {
        ws_rx_msg_t *msg = malloc(sizeof(*msg) + (size_t)s_rx_len + 1);
        if (msg) {
            msg->len = (uint16_t)s_rx_len;
            memcpy(msg->data, s_rx_buf, (size_t)s_rx_len);
            msg->data[s_rx_len] = '\0';
            if (xQueueSend(s_rxq, &msg, 0) != pdTRUE) {
                free(msg);
            }
        }
    }
    rx_reset();
}

static void ws_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        /* Flags only — no UI, no send, from this task. s_auth_failed stays put
         * until a frame proves the companion accepted us. */
        s_hello_sent = false;
        s_need_auth = false;
        s_connected = true;
        s_transport_ever_up = true;
        s_ui_online = true;
        s_need_hello = !g_cfg.companion_secret[0];
        s_connect_ms = now_ms();
        rx_reset();
        touch_active();
        touch_companion();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        /* Clean companion quit sends a WS close → CLOSED (not DISCONNECTED).
         * Ignoring CLOSED leaves s_connected true forever and blocks rediscovery. */
        if (s_connected || s_auth_failed) {
            app_dbg_log("ws: disconnected%s%s",
                        s_auth_failed ? " (auth)" : "",
                        id == WEBSOCKET_EVENT_CLOSED ? " (closed)" : "");
        }
        s_connected = false;
        s_hello_sent = false;
        s_need_hello = false;
        s_need_auth = false;
        s_connect_ms = 0;
        s_ui_offline = true;
        rx_reset();
        touch_companion();
        break;

    case WEBSOCKET_EVENT_DATA:
        rx_feed(data);
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
    s_connect_ms = 0;
    rx_reset();
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
        .disable_pingpong_discon = false,
        .network_timeout_ms = 10000,
        .buffer_size = 8192,
        .task_stack = 8192,
        .ping_interval_sec = 20,
        /* Default library timeout is 120s; tighten so a crashed companion
         * (no close frame) flips the UI offline within ~1 minute. */
        .pingpong_timeout_sec = 40,
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

/* Split an optional ":port" suffix off the configured host so "host:9000"
 * doesn't compose into a double-port URI. */
static void split_host_port(const char *in, char *host, size_t host_len, uint16_t *port)
{
    snprintf(host, host_len, "%s", in);
    char *colon = strrchr(host, ':');
    if (!colon || colon == host || !colon[1]) return;
    for (const char *p = colon + 1; *p; p++) {
        if (*p < '0' || *p > '9') return;
    }
    unsigned long v = strtoul(colon + 1, NULL, 10);
    if (v < 1 || v > 65535) return;
    *colon = '\0';
    *port = (uint16_t)v;
}

/* Back off after a rejection instead of latching offline forever — the secret
 * may have been corrected on the companion since. */
static bool auth_backoff_active(void)
{
    return s_auth_failed && (now_ms() - s_auth_failed_ms) < WS_AUTH_RETRY_MS;
}

static void try_discover(void)
{
    if (auth_backoff_active()) return;

    if (g_cfg.companion_host[0]) {
        char host[APP_HOST_LEN];
        uint16_t port = WS_DEFAULT_PORT;
        split_host_port(g_cfg.companion_host, host, sizeof(host), &port);
        if (host[0]) begin_ws(host, port);
        return;
    }

    char ip[48];
    uint16_t port = WS_DEFAULT_PORT;
    if (app_mdns_discover_companion(ip, sizeof(ip), &port, 2000)) {
        begin_ws(ip, port);
    }
}

static void flush_ui_flags(void)
{
    /* Hold the online transition while a rejection stands: it clears the auth
     * badge, and a bare transport connect is no proof the secret works. */
    if (s_ui_online && !s_auth_failed) {
        s_ui_online = false;
        ui_set_auth_failed(false);
        if (!s_last_status.connected) {
            status_snapshot_t st = s_last_status;
            st.connected = true;
            apply_status(&st);
        }
        ui_wake();
        app_dbg_log("ws: connected");
    }
    if (s_ui_offline) {
        s_ui_offline = false;
        /* Drop cached status so a later reconnect wake doesn't revive
         * stale usage bars before the companion pushes a fresh snapshot. */
        memset(&s_last_status, 0, sizeof(s_last_status));
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
    /* One buffer reused for every message; reassembly runs in the client task
     * and must not allocate per event. */
    s_rx_buf = malloc(WS_RX_MAX);
    if (!s_rx_buf) app_dbg_log("ws: rx buffer alloc failed");
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

        bool transport_up = s_client && esp_websocket_client_is_connected(s_client);

        /* Safety net: if the WS task died after a clean close but we missed
         * the event, don't keep the UI stuck on "connected". */
        if (s_connected && s_client && !transport_up) {
            s_connected = false;
            s_hello_sent = false;
            s_need_hello = false;
            s_need_auth = false;
            s_connect_ms = 0;
            s_ui_offline = true;
            touch_companion();
            app_dbg_log("ws: disconnected (stale)");
            flush_ui_flags();
        }

        if (s_connected && transport_up) {
            /* Healthy session — don't let the discover timer expire. */
            last_discover = now_ms();
        }

        /* After a clean close the client handle can linger stopped with
         * is_connected==false; tear it down and rediscover promptly. */
        bool need_discover = !auth_backoff_active() &&
            (!s_client || (!s_connected && !transport_up)) &&
            (now_ms() - last_discover) >= WS_DISCOVER_MS;

        if (need_discover) {
            last_discover = now_ms();
            destroy_client();
            try_discover();
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
