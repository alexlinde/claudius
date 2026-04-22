#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_display_init(void);

/* Set the LCD backlight. percent is clamped to 0..100;
 * 0 = fully off, 100 = fully on. */
esp_err_t bsp_display_set_backlight_percent(int percent);

#ifdef __cplusplus
}
#endif
