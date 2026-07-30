#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_STR_LEN   48
#define STATUS_TITLE_LEN 32
#define STATUS_RESET_LEN 16
#define STATUS_AGENT_LEN 24
#define LOGO_W           48
#define LOGO_H           48
#define LOGO_BYTES       (LOGO_W * LOGO_H / 8)
#define MAX_AGENT_LOGOS  6

typedef enum {
    AGENT_STATUS_IDLE = 0,
    AGENT_STATUS_WORKING,
    AGENT_STATUS_WAITING,
    AGENT_STATUS_OFFLINE,
    AGENT_STATUS_AUTH_FAILED,
} agent_status_t;

typedef struct {
    float weekly_pct;   /* 0.0–1.0 */
    float session_pct;  /* 0.0–1.0 */
    char weekly_reset[STATUS_RESET_LEN];
    char session_reset[STATUS_RESET_LEN];
    char weekly_title[STATUS_TITLE_LEN];
    char session_title[STATUS_TITLE_LEN];
    char agent_display[STATUS_AGENT_LEN];
    char agent_id[STATUS_AGENT_LEN];
    int sessions;
    agent_status_t status;
    bool connected;
    bool auth_failed;
} status_snapshot_t;

typedef struct {
    uint16_t color_rgb565;
    uint8_t bits[LOGO_BYTES];
} agent_logo_t;

#ifdef __cplusplus
}
#endif
