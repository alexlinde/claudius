#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the WebSocket client task. Discovers companion via mDNS (or uses
 * companion_host) and maintains the connection. */
esp_err_t app_ws_start(void);

/* Force a reconnect on the next discovery tick. */
void app_ws_request_reconnect(void);

/* True while a live authenticated/open WebSocket session is up. */
bool app_ws_is_connected(void);

/* Last time (esp_timer ms) we had a companion connection or activity. */
int64_t app_ws_last_companion_ms(void);
int64_t app_ws_last_active_ms(void);

#ifdef __cplusplus
}
#endif
