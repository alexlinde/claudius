#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui.h"

#define HOR_RES 240
#define VER_RES 240

/* Must match main/bsp_touch.c so the sim behaves like the device. */
#define TAP_SHORT_PRESS_MS  180
#define TAP_LONG_PRESS_MS   800

static int s_last_brightness = -1;
static void sim_brightness_cb(int percent)
{
    if (percent == s_last_brightness) return;
    s_last_brightness = percent;
    printf("[sim] backlight -> %d%%\n", percent);
}

static void add_session(status_snapshot_t *s,
                        const char *name, const char *kind,
                        const char *state, const char *status,
                        const char *waiting_for, const char *cwd)
{
    if (s->session_count >= MAX_SESSIONS) return;
    agent_session_t *a = &s->sessions[s->session_count++];
    if (name) snprintf(a->name, sizeof(a->name), "%s", name);
    if (kind) snprintf(a->kind, sizeof(a->kind), "%s", kind);
    if (state) snprintf(a->state, sizeof(a->state), "%s", state);
    if (status) snprintf(a->status, sizeof(a->status), "%s", status);
    if (waiting_for) snprintf(a->waiting_for, sizeof(a->waiting_for), "%s", waiting_for);
    if (cwd) snprintf(a->cwd, sizeof(a->cwd), "%s", cwd);
    if ((state && (strcmp(state, "working") == 0 || strcmp(state, "blocked") == 0)) ||
        (status && (strcmp(status, "busy") == 0 || strcmp(status, "waiting") == 0))) {
        s->any_active = true;
    }
}

static void fill_mock_base(status_snapshot_t *s, bool connected)
{
    memset(s, 0, sizeof(*s));
    s->connected = connected;
    s->auth_failed = false;
    snprintf(s->agent_display, sizeof(s->agent_display), "Claude");
    snprintf(s->agent_id, sizeof(s->agent_id), "claude");
    if (connected) {
        snprintf(s->weekly_title, sizeof(s->weekly_title), "Claude Weekly");
        snprintf(s->session_title, sizeof(s->session_title), "Claude Session");
        s->weekly_pct = 0.42f;
        s->session_pct = 0.67f;
        snprintf(s->weekly_reset, sizeof(s->weekly_reset), "3d 1h");
        snprintf(s->session_reset, sizeof(s->session_reset), "2h 15m");
    }
}

static void apply_key_mock(SDL_Scancode key)
{
    status_snapshot_t snap;
    switch (key) {
    case SDL_SCANCODE_1:
        fill_mock_base(&snap, true);
        add_session(&snap, "Review SPEC.md…", "background",
                    "working", "busy", NULL, "toy-jepa");
        add_session(&snap, "gm-claude-8a", "interactive",
                    NULL, NULL, NULL, "gm-claude");
        add_session(&snap, "aescape-admin-38", "interactive",
                    NULL, NULL, NULL, "aescape-admin");
        printf("[sim] mock WORKING + 3 sessions\n");
        ui_set_status(&snap);
        ui_wake();
        break;
    case SDL_SCANCODE_2:
        fill_mock_base(&snap, true);
        add_session(&snap, "gm-claude-8a", "interactive",
                    "blocked", "waiting", "permission prompt", "gm-claude");
        add_session(&snap, "Review SPEC.md…", "background",
                    "working", "busy", NULL, "toy-jepa");
        printf("[sim] mock WAITING (permission prompt)\n");
        ui_set_status(&snap);
        ui_wake();
        break;
    case SDL_SCANCODE_3:
        fill_mock_base(&snap, true);
        add_session(&snap, "gm-claude-8a", "interactive",
                    NULL, NULL, NULL, "gm-claude");
        add_session(&snap, "old-job", "background",
                    "done", NULL, NULL, "toy-jepa");
        printf("[sim] mock interactive + done\n");
        ui_set_status(&snap);
        break;
    case SDL_SCANCODE_0:
        fill_mock_base(&snap, false);
        printf("[sim] mock OFFLINE\n");
        ui_set_status(&snap);
        break;
    case SDL_SCANCODE_4:
        memset(&snap, 0, sizeof(snap));
        snap.auth_failed = true;
        printf("[sim] mock AUTH FAIL\n");
        ui_set_status(&snap);
        break;
    case SDL_SCANCODE_S:
        if (ui_is_sleeping()) {
            printf("[sim] wake\n");
            ui_wake();
        } else {
            printf("[sim] sleep\n");
            ui_sleep_start();
        }
        break;
    default:
        break;
    }
}

int main(void)
{
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(HOR_RES, VER_RES);
    (void)disp;
    lv_sdl_mouse_create();

    ui_init(lv_screen_active(), sim_brightness_cb);
    ui_set_title("claudius");
    ui_set_utc_offset(-7 * 3600); /* PT for local testing */

    status_snapshot_t initial;
    fill_mock_base(&initial, true);
    add_session(&initial, "gm-claude-8a", "interactive",
                NULL, NULL, NULL, "gm-claude");
    ui_set_status(&initial);

    printf("gm-s3 simulator — claudius status UI\n");
    printf("  SPACE = touch pad (tap cycles sessions / double = prev)\n");
    printf("  1     = working + 3 sessions\n");
    printf("  2     = waitingFor permission prompt\n");
    printf("  3     = interactive + done\n");
    printf("  0     = OFFLINE\n");
    printf("  4     = AUTH FAIL\n");
    printf("  S     = toggle screensaver\n");
    printf("  Close window to exit.\n");

    bool     space_prev       = false;
    uint32_t space_down_ms    = 0;
    uint32_t last_release_ms  = 0;
    int      burst_count      = 0;
    bool     burst_committed  = false;
    bool     long_press_fired = false;
    bool     key_prev[SDL_NUM_SCANCODES] = {0};

    for (;;) {
        uint32_t ms  = lv_timer_handler();
        uint32_t now = SDL_GetTicks();
        ui_tick(now);

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        bool space_now = keys[SDL_SCANCODE_SPACE];

        if (space_now && !space_prev) {
            space_down_ms    = now;
            long_press_fired = false;
            burst_count++;
            if (burst_count >= 3) {
                ui_on_tap_burst(burst_count);
                burst_committed = true;
            }
        } else if (!space_now && space_prev) {
            if (!long_press_fired) {
                last_release_ms = now;
            }
        }

        if (space_now && !long_press_fired &&
            (now - space_down_ms) >= TAP_LONG_PRESS_MS) {
            ui_on_long_press();
            long_press_fired = true;
            burst_count      = 0;
            burst_committed  = false;
        }

        if (!space_now && burst_count > 0 &&
            (now - last_release_ms) >= TAP_SHORT_PRESS_MS) {
            if (!burst_committed) {
                if (burst_count == 1)      ui_on_tap();
                else if (burst_count == 2) ui_on_tap_burst(2);
            }
            burst_count     = 0;
            burst_committed = false;
        }

        space_prev = space_now;

        static const SDL_Scancode mocks[] = {
            SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2,
            SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_S,
        };
        for (size_t i = 0; i < sizeof(mocks) / sizeof(mocks[0]); i++) {
            SDL_Scancode sc = mocks[i];
            bool down = keys[sc];
            if (down && !key_prev[sc]) apply_key_mock(sc);
            key_prev[sc] = down;
        }

        if (ms < 1) ms = 1;
        usleep(ms * 1000);
    }

    return 0;
}
