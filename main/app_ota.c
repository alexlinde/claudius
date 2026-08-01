#include "app_ota.h"
#include "app_config.h"
#include "app_dbg.h"

#include <string.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Max time to wait for forward progress on the upload body. A stalled
 * client must not be able to wedge esp_http_server's single handler task
 * (and its held esp_ota handle) for the rest of uptime. */
#define OTA_RECV_STALL_MS 30000

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* Constant-time compare against the configured secret, so a wrong guess
 * takes the same time regardless of which byte first differs. No secret
 * configured -> open (the setup flow relies on this). */
static bool secret_hdr_ok(httpd_req_t *req)
{
    if (g_cfg.companion_secret[0] == '\0') return true;

    char hdr[APP_SECRET_LEN];
    if (httpd_req_get_hdr_value_str(req, "X-Claudius-Secret", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    size_t slen = strlen(g_cfg.companion_secret);
    size_t hlen = strlen(hdr);
    if (hlen != slen) return false;

    volatile uint8_t diff = 0;
    for (size_t i = 0; i < slen; i++) {
        diff |= (uint8_t)hdr[i] ^ (uint8_t)g_cfg.companion_secret[i];
    }
    return diff == 0;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!secret_hdr_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota = 0;
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    app_dbg_log("ota: starting, size=%u", (unsigned)req->content_len);
    bool started = false;
    int remaining = req->content_len;
    char buf[1024];
    TickType_t last_progress = xTaskGetTickCount();

    /* Simple path: raw .bin body. Also tolerate multipart by finding the
     * first 0xE9 image magic after headers. */
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int got = httpd_req_recv(req, buf, to_read);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                TickType_t stalled_ms = (xTaskGetTickCount() - last_progress) * portTICK_PERIOD_MS;
                if (stalled_ms < OTA_RECV_STALL_MS) continue;
                app_dbg_log("ota: stalled %lus, aborting", (unsigned long)(stalled_ms / 1000));
                if (started) esp_ota_abort(ota);
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Upload stalled");
                return ESP_FAIL;
            }
            if (started) esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        remaining -= got;
        last_progress = xTaskGetTickCount();

        const char *payload = buf;
        int payload_len = got;

        if (!started) {
            /* Look for ESP image magic 0xE9 */
            int magic_at = -1;
            for (int i = 0; i < got; i++) {
                if ((uint8_t)buf[i] == 0xE9) {
                    magic_at = i;
                    break;
                }
            }
            if (magic_at < 0) {
                /* Still in multipart headers */
                continue;
            }
            payload = buf + magic_at;
            payload_len = got - magic_at;
            esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                return err;
            }
            started = true;
        }

        esp_err_t err = esp_ota_write(ota, payload, payload_len);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return err;
        }
    }

    if (!started) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware image found");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_end(ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return err;
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return err;
    }

    app_dbg_log("ota: success, rebooting");
    httpd_resp_sendstr(req, "OK – rebooting");
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t app_ota_register(httpd_handle_t server)
{
    httpd_uri_t uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &uri);
}
