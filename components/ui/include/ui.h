#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sink for brightness-slider changes. Invoked from LVGL context with
 * percent in [0, 100]. May be NULL. */
typedef void (*ui_brightness_cb_t)(int percent);

/* Build the screen, focus group, and the encoder-style input device that the
 * gesture callbacks below feed. brightness_cb (if non-NULL) is called once
 * with the initial slider value and then on every slider change. */
void ui_init(lv_obj_t *parent, ui_brightness_cb_t brightness_cb);

/* Gesture entry points. Safe to call from any task context - they only
 * mutate atomics that LVGL's own timer handler drains via the indev
 * read_cb, so no lvgl_port_lock() is required on the caller's side.
 *
 * Mapping onto LVGL's encoder/group model:
 *   ui_on_tap()            -> encoder diff +1
 *                             (nav: focus next; edit: value +1)
 *   ui_on_tap_burst(2)     -> encoder diff -1
 *                             (nav: focus prev; edit: value -1)
 *   ui_on_tap_burst(3)     -> encoder diff +3 (catches up two held-back taps
 *                             so a rapid triple fast-forwards the value)
 *   ui_on_tap_burst(N>=4)  -> encoder diff +1 per tap (live scrub)
 *   ui_on_long_press()     -> momentary ENTER click
 *                             (nav: enter edit mode on editables; also
 *                              fires CLICKED on buttons) */
void ui_on_tap(void);
void ui_on_tap_burst(int count);
void ui_on_long_press(void);

#ifdef __cplusplus
}
#endif
