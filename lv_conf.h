#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_MEM_SIZE (64 * 1024)

/*====================
   DISPLAY SETTINGS
 *====================*/
#define LV_DPI_DEF 130

/*====================
   TICK
 *====================*/
/* Firmware: esp_lvgl_port supplies ticks via esp_timer.
 * Simulator: SDL supplies ticks. */
#ifdef GM_S3_SIM
    #define LV_TICK_CUSTOM 0
#else
    #define LV_TICK_CUSTOM 1
#endif

/*====================
   DRAW ENGINE
 *====================*/
#define LV_USE_DRAW_SW_ASM  LV_DRAW_SW_ASM_NONE
#define LV_USE_NATIVE_HELIUM_ASM 0

/*====================
   FONT SETTINGS
 *====================*/
#define LV_FONT_MONTSERRAT_14 1   /* kept as LV_FONT_DEFAULT bootstrap */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* lv_tiny_ttf: TrueType rasterizer for runtime-sized fonts. */
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0

/*====================
   WIDGETS
 *====================*/
#define LV_USE_LABEL   1
#define LV_USE_BTN     1
#define LV_USE_ARC     1
#define LV_USE_LINE    1
#define LV_USE_IMG     1

/*====================
   SDL DISPLAY DRIVER
   (simulator only)
 *====================*/
#ifdef GM_S3_SIM
    #define LV_USE_SDL 1
    #define LV_SDL_WINDOW_TITLE "gm-s3 sim"
    #define LV_SDL_HOR_RES  240
    #define LV_SDL_VER_RES  240
    #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
    #define LV_USE_EVDEV 0
    #define LV_USE_LIBINPUT 0
#endif

/*====================
   LOGGING
 *====================*/
#ifdef GM_S3_SIM
    #define LV_USE_LOG 1
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 1
#else
    #define LV_USE_LOG 0
#endif

#endif /* LV_CONF_H */
