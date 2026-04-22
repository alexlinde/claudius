#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui.h"

#define HOR_RES 240
#define VER_RES 240

/* Must match main/bsp_touch.c so the sim behaves like the device. */
#define TAP_SHORT_PRESS_MS  180
#define TAP_LONG_PRESS_MS   800

int main(void)
{
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(HOR_RES, VER_RES);
    (void)disp;
    lv_sdl_mouse_create();

    ui_init(lv_screen_active());

    printf("gm-s3 simulator running.\n");
    printf("  SPACE = touch pad (press/release to tap; rapid presses form\n");
    printf("          a burst; hold %u ms for long press)\n", TAP_LONG_PRESS_MS);
    printf("  Close window to exit.\n");

    /* Mirror the device: count each press-down immediately (live feedback),
     * and fire ui_on_tap_burst(N) only after the trailing pause to apply the
     * double-tap bonus / relabel. Holding SPACE past the long-press threshold
     * cancels the in-flight burst and resets the counter. */
    bool     space_prev       = false;
    uint32_t space_down_ms    = 0;
    uint32_t last_release_ms  = 0;
    int      burst_count      = 0;
    bool     long_press_fired = false;

    for (;;) {
        uint32_t ms  = lv_timer_handler();
        uint32_t now = SDL_GetTicks();

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        bool space_now = keys[SDL_SCANCODE_SPACE];

        if (space_now && !space_prev) {
            space_down_ms    = now;
            long_press_fired = false;
            ui_on_tap();              /* immediate +1 per press-down */
            burst_count++;
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
        }

        if (!space_now && burst_count >= 2 &&
            (now - last_release_ms) >= TAP_SHORT_PRESS_MS) {
            ui_on_tap_burst(burst_count);
            burst_count = 0;
        } else if (!space_now && burst_count == 1 &&
                   (now - last_release_ms) >= TAP_SHORT_PRESS_MS) {
            burst_count = 0; /* single tap settled; already counted on press */
        }

        space_prev = space_now;

        if (ms < 1) ms = 1;
        usleep(ms * 1000);
    }

    return 0;
}
