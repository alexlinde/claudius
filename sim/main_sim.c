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

static void fill_mock(status_snapshot_t *s, agent_status_t st, bool connected)
{
    memset(s, 0, sizeof(*s));
    s->connected = connected;
    s->auth_failed = false;
    s->status = st;
    s->sessions = connected ? 2 : 0;
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
        fill_mock(&snap, AGENT_STATUS_WORKING, true);
        printf("[sim] mock WORKING\n");
        ui_set_status(&snap);
        ui_wake();
        break;
    case SDL_SCANCODE_2:
        fill_mock(&snap, AGENT_STATUS_WAITING, true);
        printf("[sim] mock WAITING\n");
        ui_set_status(&snap);
        ui_wake();
        break;
    case SDL_SCANCODE_3:
        fill_mock(&snap, AGENT_STATUS_IDLE, true);
        printf("[sim] mock IDLE\n");
        ui_set_status(&snap);
        break;
    case SDL_SCANCODE_0:
        fill_mock(&snap, AGENT_STATUS_OFFLINE, false);
        printf("[sim] mock OFFLINE\n");
        ui_set_status(&snap);
        break;
    case SDL_SCANCODE_4:
        memset(&snap, 0, sizeof(snap));
        snap.auth_failed = true;
        snap.status = AGENT_STATUS_AUTH_FAILED;
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
    ui_set_utc_offset(-7 * 3600); /* PT for local testing */

    status_snapshot_t initial;
    fill_mock(&initial, AGENT_STATUS_IDLE, true);
    ui_set_status(&initial);

    printf("gm-s3 simulator — codelight status UI\n");
    printf("  SPACE = touch pad (tap / burst / long-press)\n");
    printf("  1/2/3 = WORKING / WAITING / IDLE\n");
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
