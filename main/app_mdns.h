#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_mdns_start(void);

/* Query _claudius._tcp. If companion_name is set in config, only that
 * instance matches. Returns true and fills ip/port on success. */
bool app_mdns_discover_companion(char *ip_out, size_t ip_len, uint16_t *port_out,
                                 int timeout_ms);

#ifdef __cplusplus
}
#endif
