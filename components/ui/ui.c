#include "ui.h"
#include "font_ubuntu_medium.h"

#include <stdio.h>

static lv_obj_t *label_counter = NULL;
static lv_obj_t *label_last_gesture = NULL;
static int press_count = 0;

static lv_font_t *font_title   = NULL;
static lv_font_t *font_counter = NULL;
static lv_font_t *font_footer  = NULL;

static void update_counter_label(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", press_count);
    lv_label_set_text(label_counter, buf);
}

static void set_last_gesture(const char *name)
{
    if (label_last_gesture) {
        lv_label_set_text(label_last_gesture, name);
    }
}

static void create_fonts(void)
{
    if (font_title) return;
    font_title   = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 24);
    font_counter = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 72);
    font_footer  = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 14);
}

void ui_init(lv_obj_t *parent)
{
    create_fonts();

    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label_hello = lv_label_create(parent);
    lv_label_set_text(label_hello, "Tap Me");
    lv_obj_set_style_text_font(label_hello, font_title, 0);
    lv_obj_set_style_text_color(label_hello, lv_color_white(), 0);

    label_counter = lv_label_create(parent);
    lv_obj_set_style_text_font(label_counter, font_counter, 0);
    lv_obj_set_style_text_color(label_counter, lv_color_white(), 0);
    update_counter_label();

    label_last_gesture = lv_label_create(parent);
    lv_obj_set_style_text_font(label_last_gesture, font_footer, 0);
    lv_obj_set_style_text_color(label_last_gesture, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(label_last_gesture, "-");
}

void ui_on_tap(void)
{
    press_count += 1;
    update_counter_label();
    set_last_gesture("tap");
}

void ui_on_tap_burst(int count)
{
    /* Called after the trailing pause of a multi-tap burst. Each press-down
     * in the burst has already applied +1 via ui_on_tap(), so here we only
     * apply the extra bonus that distinguishes a double-tap from an N>=3
     * burst, and update the label to reflect the final gesture kind. */
    if (count < 2) return;

    if (count == 2) {
        press_count += 8; /* +1+1 from taps, +8 bonus -> +10 for a double */
        update_counter_label();
        set_last_gesture("double");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "burst x%d", count);
        set_last_gesture(buf);
    }
}

void ui_on_long_press(void)
{
    press_count = 0;
    update_counter_label();
    set_last_gesture("long (reset)");
}
