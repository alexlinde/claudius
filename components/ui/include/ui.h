#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(lv_obj_t *parent);

/* Gesture entry points. Caller must hold the LVGL lock. */
void ui_on_tap(void);          /* single tap: +1 */
void ui_on_double_tap(void);   /* double tap: +10 */
void ui_on_long_press(void);   /* long press: reset to 0 */

#ifdef __cplusplus
}
#endif
