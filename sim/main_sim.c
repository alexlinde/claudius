#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui.h"

#define HOR_RES 240
#define VER_RES 240

int main(void)
{
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(HOR_RES, VER_RES);
    (void)disp;
    lv_sdl_mouse_create();

    ui_init(lv_screen_active());

    printf("gm-s3 simulator running.\n");
    printf("  SPACE = single tap   (+1)\n");
    printf("  D     = double tap   (+10)\n");
    printf("  L     = long press   (reset)\n");
    printf("  Close window to exit.\n");

    bool space_prev = false;
    bool d_prev = false;
    bool l_prev = false;

    for (;;) {
        uint32_t ms = lv_timer_handler();

        const Uint8 *keys = SDL_GetKeyboardState(NULL);

        bool space_now = keys[SDL_SCANCODE_SPACE];
        if (space_now && !space_prev) {
            ui_on_tap();
        }
        space_prev = space_now;

        bool d_now = keys[SDL_SCANCODE_D];
        if (d_now && !d_prev) {
            ui_on_double_tap();
        }
        d_prev = d_now;

        bool l_now = keys[SDL_SCANCODE_L];
        if (l_now && !l_prev) {
            ui_on_long_press();
        }
        l_prev = l_now;

        if (ms < 1) ms = 1;
        usleep(ms * 1000);
    }

    return 0;
}
