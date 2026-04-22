#include "ui.h"
#include <stdio.h>

static lv_obj_t *label_counter = NULL;
static int press_count = 0;

static void update_counter_label(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "Presses: %d", press_count);
    lv_label_set_text(label_counter, buf);
}

void ui_init(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label_hello = lv_label_create(parent);
    lv_label_set_text(label_hello, "Hello geekmagic-s3");
    lv_obj_set_style_text_font(label_hello, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_hello, lv_color_white(), 0);

    label_counter = lv_label_create(parent);
    lv_obj_set_style_text_color(label_counter, lv_color_white(), 0);
    update_counter_label();
}

void ui_on_button_pressed(void)
{
    press_count++;
    if (label_counter) {
        update_counter_label();
    }
}
