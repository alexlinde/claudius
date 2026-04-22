#include "ui.h"
#include "font_ubuntu_medium.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Encoder-style input device fed by external gesture callbacks.
 *
 * Gesture sources (device iot_button / sim key events) push events via the
 * ui_on_* functions; those only touch atomics, so they're safe from any task
 * context. LVGL's timer handler invokes our read_cb on its own thread to
 * drain those atomics into lv_indev_data_t.
 * ------------------------------------------------------------------------- */
static _Atomic int  s_enc_diff;
static _Atomic bool s_enc_click_pending;   /* true -> next read should PRESS */
static bool         s_enc_click_active;    /* true -> next read should RELEASE */

static lv_indev_t *s_indev;
static lv_group_t *s_group;
static lv_obj_t   *s_slider;
static lv_obj_t   *s_label_percent;

static lv_font_t  *s_font_title;
static lv_font_t  *s_font_value;
static lv_font_t  *s_font_footer;

static ui_brightness_cb_t s_brightness_cb;

#define UI_DEFAULT_BRIGHTNESS 20

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->enc_diff = atomic_exchange(&s_enc_diff, 0);

    if (s_enc_click_active) {
        /* Deliver the release half of a momentary click. */
        data->state        = LV_INDEV_STATE_RELEASED;
        s_enc_click_active = false;
    } else if (atomic_exchange(&s_enc_click_pending, false)) {
        /* Deliver the press; next tick the release finishes the click. */
        data->state        = LV_INDEV_STATE_PRESSED;
        s_enc_click_active = true;
    } else {
        data->state        = LV_INDEV_STATE_RELEASED;
    }
}

static void slider_value_changed_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)lv_slider_get_value(s_slider);
    lv_label_set_text_fmt(s_label_percent, "%d%%", v);
    if (s_brightness_cb) s_brightness_cb(v);
}

static void reset_clicked_cb(lv_event_t *e)
{
    (void)e;
    lv_slider_set_value(s_slider, UI_DEFAULT_BRIGHTNESS, LV_ANIM_ON);
    /* set_value is silent; emit so our handler + any listeners react. */
    lv_obj_send_event(s_slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static void create_fonts(void)
{
    if (s_font_title) return;
    s_font_title  = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 22);
    s_font_value  = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 44);
    s_font_footer = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 14);
}

void ui_init(lv_obj_t *parent, ui_brightness_cb_t brightness_cb)
{
    create_fonts();

    s_brightness_cb = brightness_cb;

    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_set_style_pad_row(parent, 8, 0);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Title ---------------------------------------------------------------- */
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_font(title, s_font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    /* Big percent readout -------------------------------------------------- */
    s_label_percent = lv_label_create(parent);
    lv_obj_set_style_text_font(s_label_percent, s_font_value, 0);
    lv_obj_set_style_text_color(s_label_percent, lv_color_white(), 0);
    lv_label_set_text_fmt(s_label_percent, "%d%%", UI_DEFAULT_BRIGHTNESS);

    /* Slider (focusable, editable) ---------------------------------------- */
    s_slider = lv_slider_create(parent);
    lv_obj_set_width(s_slider, 200);
    lv_slider_set_range(s_slider, 0, 100);
    lv_slider_set_value(s_slider, UI_DEFAULT_BRIGHTNESS, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_value_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Reset button (focusable, clickable) --------------------------------- */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_pad_hor(btn, 16, 0);
    lv_obj_set_style_pad_ver(btn, 6,  0);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text_fmt(btn_label, "Reset %d%%", UI_DEFAULT_BRIGHTNESS);
    lv_obj_set_style_text_font(btn_label, s_font_footer, 0);
    lv_obj_add_event_cb(btn, reset_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Focus group + encoder indev. LVGL cycles NEXT/PREV on enc_diff, and
     * an ENTER click toggles edit mode on editable widgets (slider) or
     * fires CLICKED on non-editable focusable widgets (button). */
    s_group = lv_group_create();
    lv_group_add_obj(s_group, s_slider);
    lv_group_add_obj(s_group, btn);
    lv_group_set_default(s_group);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_indev, read_cb);
    lv_indev_set_group(s_indev, s_group);

    /* Sync backlight to the initial slider value. This runs under the LVGL
     * port lock, so we can't force a synchronous flush here (would deadlock
     * against the esp_lvgl_port task) - the first real frame lands on the
     * panel within a tick of releasing the lock, and the brief moment
     * between backlight-on and first-flush is not visible in practice. */
    if (s_brightness_cb) s_brightness_cb((int)lv_slider_get_value(s_slider));
}

void ui_on_tap(void)
{
    atomic_fetch_add(&s_enc_diff, +1);
}

void ui_on_tap_burst(int count)
{
    if (count == 2)       atomic_fetch_add(&s_enc_diff, -1);
    else if (count == 3)  atomic_fetch_add(&s_enc_diff, +3);
    else if (count >= 4)  atomic_fetch_add(&s_enc_diff, +1);
}

void ui_on_long_press(void)
{
    atomic_store(&s_enc_click_pending, true);
}
