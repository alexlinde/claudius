#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register POST /api/ota on the given server (multipart firmware upload). */
esp_err_t app_ota_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
