#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_STR_LEN       48
#define STATUS_TITLE_LEN     32
#define STATUS_RESET_LEN     16
#define STATUS_AGENT_LEN     24
#define LOGO_W               48
#define LOGO_H               48
#define LOGO_BYTES           (LOGO_W * LOGO_H / 8)
#define MAX_AGENT_LOGOS      6
#define MAX_SESSIONS         8
#define SESSION_NAME_LEN     40
#define SESSION_KIND_LEN     16
#define SESSION_ID_LEN       16
#define SESSION_UUID_LEN     40          /* full sessionId UUID from CLI */
#define SESSION_STATE_LEN    24
#define SESSION_WAITING_LEN  48
#define SESSION_CWD_LEN      40
#define SESSION_KEY_LEN      48          /* stable selection key on device */

/* One entry from `claude agents --json`, compacted by the companion. */
typedef struct {
    char name[SESSION_NAME_LEN];
    char kind[SESSION_KIND_LEN];
    char id[SESSION_ID_LEN];             /* short background id */
    char session_id[SESSION_UUID_LEN];   /* sessionId — preferred stable key */
    char state[SESSION_STATE_LEN];       /* working|blocked|done|failed|stopped */
    char status[SESSION_STATE_LEN];      /* busy|waiting|… while process alive */
    char waiting_for[SESSION_WAITING_LEN];
    char cwd[SESSION_CWD_LEN];
} agent_session_t;

typedef struct {
    float weekly_pct;   /* 0.0–1.0 */
    float session_pct;  /* 0.0–1.0 */
    char weekly_reset[STATUS_RESET_LEN];
    char session_reset[STATUS_RESET_LEN];
    char weekly_title[STATUS_TITLE_LEN];
    char session_title[STATUS_TITLE_LEN];
    char agent_display[STATUS_AGENT_LEN];
    char agent_id[STATUS_AGENT_LEN];
    agent_session_t sessions[MAX_SESSIONS];
    int session_count;
    bool any_active;    /* any session working/blocked/busy/waiting */
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
