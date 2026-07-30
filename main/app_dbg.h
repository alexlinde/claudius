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

/* Ring buffer for /api/debug/log. out_seq is the next sequence number. */
int app_dbg_copy_lines(uint16_t from, char lines[][APP_DBG_LINE_LEN], int max_lines,
                       uint16_t *out_seq, uint16_t *out_from);

#ifdef __cplusplus
}
#endif
