#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(lv_obj_t *parent);
void ui_on_button_pressed(void);

#ifdef __cplusplus
}
#endif
