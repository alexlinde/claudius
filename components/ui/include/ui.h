#pragma once

#include "lvgl.h"
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional backlight sink (percent 0..100). May be NULL. */
typedef void (*ui_brightness_cb_t)(int percent);

/* Build the status dashboard. brightness_cb is invoked when the UI dims
 * for sleep or restores on wake (and once at init with the default level). */
void ui_init(lv_obj_t *parent, ui_brightness_cb_t brightness_cb);

/* Apply a full status snapshot. Safe to call from any task; internally
 * marshals onto the LVGL thread when built for firmware. */
void ui_set_status(const status_snapshot_t *snap);

/* Connection helpers that mutate only connection-related fields. */
void ui_set_connected(bool connected);
void ui_set_auth_failed(bool failed);

/* Replace the agent logo set used by the screensaver (up to MAX_AGENT_LOGOS). */
void ui_set_agent_logos(const agent_logo_t *logos, int count);

/* Timezone offset in seconds east of UTC (from companion config). */
void ui_set_utc_offset(long offset_sec);

/* Screensaver control. */
void ui_sleep_start(void);
void ui_wake(void);
bool ui_is_sleeping(void);

/* Tick the sleep animation / clock. Call ~every frame from the main loop
 * (or from an LVGL timer). now_ms is a monotonic millisecond clock. */
void ui_tick(uint32_t now_ms);

/* Gesture entry points (same semantics as the brightness demo).
 * Tap while sleeping wakes the dashboard. */
void ui_on_tap(void);
void ui_on_tap_burst(int count);
void ui_on_long_press(void);

/* Default / current brightness percent the UI last requested. */
int ui_get_brightness(void);
void ui_set_brightness(int percent);

#ifdef __cplusplus
}
#endif
