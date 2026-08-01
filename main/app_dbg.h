#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DBG_LINES    40
#define APP_DBG_LINE_LEN 128

void app_dbg_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Ring buffer for /api/debug/log, sized for one-line-at-a-time streaming
 * (avoids a full APP_DBG_LINES-line buffer on the caller's stack).
 *
 * app_dbg_range() reports the live sequence number and clamps 'from' to
 * the oldest retained line. app_dbg_copy_line() snapshots a single line
 * by absolute index (mod APP_DBG_LINES) under the mutex. Callers stream
 * lines from the clamped 'from' up to (but excluding) 'seq'. */
void app_dbg_range(uint16_t from, uint16_t *out_seq, uint16_t *out_from);
bool app_dbg_copy_line(uint16_t idx, char line[APP_DBG_LINE_LEN]);

#ifdef __cplusplus
}
#endif
