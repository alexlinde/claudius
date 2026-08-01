#include "app_dbg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char s_buf[APP_DBG_LINES][APP_DBG_LINE_LEN];
static uint16_t s_seq;
static SemaphoreHandle_t s_mu;
static portMUX_TYPE s_mu_lock = portMUX_INITIALIZER_UNLOCKED;

/* Lazily create s_mu exactly once. Two first-callers could otherwise race
 * xSemaphoreCreateMutex() and each latch a different handle (one leaked,
 * callers serialized on separate objects). Create the semaphore outside
 * the critical section (it allocates), then swap it in under the
 * spinlock; the loser frees its unused copy. */
static void ensure_mu(void)
{
    if (s_mu) return;
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&s_mu_lock);
    if (!s_mu) {
        s_mu = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&s_mu_lock);
    if (created) vSemaphoreDelete(created);
}

void app_dbg_log(const char *fmt, ...)
{
    ensure_mu();
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[APP_DBG_LINE_LEN];
    time_t now = time(NULL);
    if (now > 1000000000L) {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s",
                 t.tm_hour, t.tm_min, t.tm_sec, msg);
    } else {
        snprintf(line, sizeof(line), "[+%lus] %s",
                 (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000), msg);
    }
    line[APP_DBG_LINE_LEN - 1] = '\0';

    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(s_buf[s_seq % APP_DBG_LINES], line, APP_DBG_LINE_LEN);
        s_seq++;
        xSemaphoreGive(s_mu);
    }
    ESP_LOGI("dbg", "%s", msg);
}

void app_dbg_range(uint16_t from, uint16_t *out_seq, uint16_t *out_from)
{
    ensure_mu();
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint16_t seq = s_seq;
        if (seq > APP_DBG_LINES && from < seq - APP_DBG_LINES) {
            from = (uint16_t)(seq - APP_DBG_LINES);
        }
        xSemaphoreGive(s_mu);
        if (out_seq) *out_seq = seq;
        if (out_from) *out_from = from;
        return;
    }
    /* Lock contention: report an empty range rather than block the caller. */
    if (out_seq) *out_seq = from;
    if (out_from) *out_from = from;
}

bool app_dbg_copy_line(uint16_t idx, char line[APP_DBG_LINE_LEN])
{
    ensure_mu();
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    memcpy(line, s_buf[idx % APP_DBG_LINES], APP_DBG_LINE_LEN);
    xSemaphoreGive(s_mu);
    return true;
}
