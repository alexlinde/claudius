#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    agent_logo_t logos[MAX_AGENT_LOGOS];
    int logo_count;
    long utc_offset;
    bool have_utc_offset;
} app_proto_config_t;

typedef enum {
    APP_PROTO_IGNORE = 0,
    APP_PROTO_CHALLENGE,
    APP_PROTO_CONFIG,
    APP_PROTO_STATUS,
    APP_PROTO_UNAUTHORIZED,
} app_proto_kind_t;

typedef struct {
    app_proto_kind_t kind;
    char nonce[96];
    status_snapshot_t status;
    app_proto_config_t config;
} app_proto_msg_t;

/* Parse a WebSocket text frame. Returns true if msg was filled. */
bool app_proto_parse(const char *json, size_t len, app_proto_msg_t *out);

/* Build auth / subscribe messages into buf. Returns bytes written (excl NUL). */
int app_proto_build_auth_hmac(const char *secret, const char *nonce,
                              char *buf, size_t buf_len);
int app_proto_build_subscribe(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
