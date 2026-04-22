#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(lv_obj_t *parent);

/* Gesture entry points. Caller must hold the LVGL lock.
 *
 * Call pattern:
 *   - ui_on_tap()               once per physical press-down (+1 immediately
 *                               for live feedback; label "tap").
 *   - ui_on_tap_burst(count)    once after the trailing pause, where count is
 *                               the total presses in the burst (>= 2). Acts
 *                               as a post-hoc adjustment on top of the
 *                               already-applied ui_on_tap() deltas:
 *                                 count == 2  -> +8 bonus, label "double"
 *                                                (so pair totals +10)
 *                                 count >= 3  -> label "burst xN" only
 *                                                (each tap was already +1)
 *   - ui_on_long_press()        held past threshold: reset counter to 0. */
void ui_on_tap(void);
void ui_on_tap_burst(int count);
void ui_on_long_press(void);

#ifdef __cplusplus
}
#endif
