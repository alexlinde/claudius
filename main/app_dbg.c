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

static void ensure_mu(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
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

int app_dbg_copy_lines(uint16_t from, char lines[][APP_DBG_LINE_LEN], int max_lines,
                       uint16_t *out_seq, uint16_t *out_from)
{
    ensure_mu();
    int n = 0;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    uint16_t seq = s_seq;
    if (seq > APP_DBG_LINES && from < seq - APP_DBG_LINES) {
        from = (uint16_t)(seq - APP_DBG_LINES);
    }
    for (uint16_t i = from; i < seq && n < max_lines; i++) {
        memcpy(lines[n], s_buf[i % APP_DBG_LINES], APP_DBG_LINE_LEN);
        n++;
    }
    if (out_seq) *out_seq = seq;
    if (out_from) *out_from = from;
    xSemaphoreGive(s_mu);
    return n;
}
