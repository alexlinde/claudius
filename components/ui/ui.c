#include "ui.h"
#include "font_ubuntu_medium.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef GM_S3_SIM
#include "esp_lvgl_port.h"
#endif

/* ---------------------------------------------------------------------------
 * Layout (240×240) — inspired by codelight ESP8266 screen/src/display.cpp
 * ------------------------------------------------------------------------- */
#define X_MARGIN     6
#define Y_TITLE      2
#define H_LABEL      16
#define H_BAR        20
#define Y_WMETER     22
#define Y_WBAR       (Y_WMETER + H_LABEL + 2)   /* 40 */
#define Y_SMETER     (Y_WBAR + H_BAR + 5)       /* 65 */
#define Y_SBAR       (Y_SMETER + H_LABEL + 2)   /* 83 */
#define Y_SESSIONS   (Y_SBAR + H_BAR + 4)       /* 107 */
#define Y_DIVIDER    (Y_SESSIONS + H_LABEL + 3) /* 126 */
#define Y_BOX        (Y_DIVIDER + 2)            /* 128 */
#define BOX_SIZE     (240 - Y_BOX - 4)          /* 108 */

#define COL_BG       0x000000
#define COL_TITLE    0xFFFFFF
#define COL_LABEL    0xC0C0C0
#define COL_BAR_BG   0x212121
#define COL_RESET    0x808080
#define COL_GREEN    0x00C800
#define COL_YELLOW   0xFFFF00
#define COL_ORANGE   0xFF8C00
#define COL_RED      0xFF2200
#define COL_OFFLINE  0x424242

#define UI_DEFAULT_BRIGHTNESS 50
#define UI_SLEEP_BRIGHTNESS   8
#define SLEEP_FRAME_MS        40
#define MAX_SLEEP_SPRITES     4

/* ---------------------------------------------------------------------------
 * Encoder indev (gestures) — kept for wake / future settings
 * ------------------------------------------------------------------------- */
static _Atomic int s_enc_diff;
static _Atomic bool s_enc_click_pending;
static bool s_enc_click_active;

static lv_indev_t *s_indev;
static lv_group_t *s_group;

static lv_font_t *s_font_title;
static lv_font_t *s_font_value;
static lv_font_t *s_font_small;

static ui_brightness_cb_t s_brightness_cb;
static int s_user_brightness = UI_DEFAULT_BRIGHTNESS; /* preferred level (not sleep) */
static int s_brightness = UI_DEFAULT_BRIGHTNESS;      /* currently applied */

static status_snapshot_t s_snap;
static int s_session_idx;
static bool s_sleeping;
static long s_utc_offset;
static uint32_t s_last_clock_sec = UINT32_MAX;
static uint32_t s_last_sleep_frame_ms;

static agent_logo_t s_logos[MAX_AGENT_LOGOS];
static int s_logo_count;

/* Dashboard widgets */
static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_clock;
static lv_obj_t *s_w_label;
static lv_obj_t *s_w_reset;
static lv_obj_t *s_w_bar;
static lv_obj_t *s_w_pct;
static lv_obj_t *s_s_label;
static lv_obj_t *s_s_reset;
static lv_obj_t *s_s_bar;
static lv_obj_t *s_s_pct;
static lv_obj_t *s_sessions_count;
static lv_obj_t *s_sessions_name;
static lv_obj_t *s_divider;
static lv_obj_t *s_status_box;
static lv_obj_t *s_status_state;
static lv_obj_t *s_status_waiting;

/* Sleep overlay */
static lv_obj_t *s_sleep_layer;
static lv_obj_t *s_sleep_clock;
static lv_obj_t *s_sleep_logo_imgs[MAX_SLEEP_SPRITES];
static lv_image_dsc_t s_sleep_logo_dsc[MAX_SLEEP_SPRITES];
static uint8_t s_sleep_logo_px[MAX_SLEEP_SPRITES][LOGO_W * LOGO_H * 2];
static float s_spr_x[MAX_SLEEP_SPRITES];
static float s_spr_y[MAX_SLEEP_SPRITES];
static float s_spr_vx[MAX_SLEEP_SPRITES];
static float s_spr_vy[MAX_SLEEP_SPRITES];
static int s_spr_count;
static bool s_spr_is_clock[MAX_SLEEP_SPRITES];

static void apply_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    if (s_brightness_cb) s_brightness_cb(percent);
}

static void lock_ui(void)
{
#ifndef GM_S3_SIM
    lvgl_port_lock(0);
#endif
}

static void unlock_ui(void)
{
#ifndef GM_S3_SIM
    lvgl_port_unlock();
#endif
}

static lv_color_t rgb(uint32_t hex)
{
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static lv_color_t usage_color(float pct)
{
    static const uint32_t stops[4] = { COL_GREEN, COL_YELLOW, COL_ORANGE, COL_RED };
    static const float edges[4] = { 0.0f, 0.5f, 0.75f, 1.0f };
    if (pct <= 0.0f) return rgb(stops[0]);
    if (pct >= 1.0f) return rgb(stops[3]);
    for (int i = 0; i < 3; i++) {
        if (pct <= edges[i + 1]) {
            float t = (pct - edges[i]) / (edges[i + 1] - edges[i]);
            lv_color_t c0 = rgb(stops[i]);
            lv_color_t c1 = rgb(stops[i + 1]);
            return lv_color_mix(c1, c0, (uint8_t)(t * 255));
        }
    }
    return rgb(stops[3]);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->enc_diff = atomic_exchange(&s_enc_diff, 0);

    if (s_enc_click_active) {
        data->state = LV_INDEV_STATE_RELEASED;
        s_enc_click_active = false;
    } else if (atomic_exchange(&s_enc_click_pending, false)) {
        data->state = LV_INDEV_STATE_PRESSED;
        s_enc_click_active = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void create_fonts(void)
{
    if (s_font_title) return;
    s_font_title = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 16);
    s_font_value = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 22);
    s_font_small = lv_tiny_ttf_create_data(font_ubuntu_medium, font_ubuntu_medium_len, 13);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int y)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 240 - X_MARGIN * 2 - 34, H_BAR);
    lv_obj_set_pos(bar, X_MARGIN, y);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, rgb(COL_BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, rgb(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    return bar;
}

static void set_meter_visible(bool weekly, bool visible)
{
    lv_obj_t *label = weekly ? s_w_label : s_s_label;
    lv_obj_t *reset = weekly ? s_w_reset : s_s_reset;
    lv_obj_t *bar = weekly ? s_w_bar : s_s_bar;
    lv_obj_t *pct = weekly ? s_w_pct : s_s_pct;
    if (visible) {
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(pct, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pct, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_meter(bool weekly, const char *title, float pct, const char *reset)
{
    bool show = title && title[0];
    set_meter_visible(weekly, show);
    if (!show) return;

    lv_obj_t *label = weekly ? s_w_label : s_s_label;
    lv_obj_t *reset_l = weekly ? s_w_reset : s_s_reset;
    lv_obj_t *bar = weekly ? s_w_bar : s_s_bar;
    lv_obj_t *pct_l = weekly ? s_w_pct : s_s_pct;

    lv_label_set_text(label, title);
    lv_label_set_text(reset_l, reset && reset[0] ? reset : "--");
    int v = (int)(pct * 100.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    lv_bar_set_value(bar, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, usage_color(pct), LV_PART_INDICATOR);
    lv_label_set_text_fmt(pct_l, "%d%%", v);
}

static void upper_copy(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    if (!src) src = "";
    for (; src[i] && i + 1 < dst_len; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    dst[i] = '\0';
}

static uint32_t color_for_session(const agent_session_t *s)
{
    if (!s) return COL_GREEN;
    if (s->waiting_for[0] ||
        strcmp(s->status, "waiting") == 0 ||
        strcmp(s->state, "blocked") == 0) {
        return COL_RED;
    }
    if (strcmp(s->state, "working") == 0 || strcmp(s->status, "busy") == 0) {
        return COL_ORANGE;
    }
    if (strcmp(s->state, "failed") == 0) return COL_RED;
    if (strcmp(s->state, "done") == 0 || strcmp(s->state, "stopped") == 0) {
        return COL_OFFLINE;
    }
    return COL_GREEN;
}

/* Prefer exact CLI fields: state, else live status, else kind. */
static void session_primary_label(const agent_session_t *s, char *out, size_t out_len)
{
    if (!s) {
        snprintf(out, out_len, "IDLE");
        return;
    }
    if (s->state[0]) {
        upper_copy(out, out_len, s->state);
        return;
    }
    if (s->status[0]) {
        upper_copy(out, out_len, s->status);
        return;
    }
    if (s->kind[0]) {
        upper_copy(out, out_len, s->kind);
        return;
    }
    snprintf(out, out_len, "—");
}

static const agent_session_t *selected_session(void)
{
    if (s_snap.session_count <= 0) return NULL;
    if (s_session_idx < 0) s_session_idx = 0;
    if (s_session_idx >= s_snap.session_count) {
        s_session_idx = s_snap.session_count - 1;
    }
    return &s_snap.sessions[s_session_idx];
}

static void clamp_session_idx(void)
{
    if (s_snap.session_count <= 0) {
        s_session_idx = 0;
        return;
    }
    if (s_session_idx < 0) s_session_idx = 0;
    if (s_session_idx >= s_snap.session_count) {
        s_session_idx = s_snap.session_count - 1;
    }
}

static void cycle_session(int delta)
{
    if (s_snap.session_count <= 0) {
        s_session_idx = 0;
        return;
    }
    int n = s_snap.session_count;
    s_session_idx = (s_session_idx + delta) % n;
    if (s_session_idx < 0) s_session_idx += n;
}

static void update_status_box(void)
{
    uint32_t color;
    char state_buf[SESSION_STATE_LEN];

    if (s_snap.auth_failed) {
        color = COL_RED;
        lv_label_set_text(s_status_state, "AUTH FAIL");
        lv_label_set_text(s_status_waiting, "");
    } else if (!s_snap.connected) {
        color = COL_OFFLINE;
        lv_label_set_text(s_status_state, "OFFLINE");
        lv_label_set_text(s_status_waiting, "");
    } else if (s_snap.session_count <= 0) {
        color = COL_GREEN;
        lv_label_set_text(s_status_state, "IDLE");
        lv_label_set_text(s_status_waiting, "no sessions");
    } else {
        const agent_session_t *s = selected_session();
        color = color_for_session(s);
        session_primary_label(s, state_buf, sizeof(state_buf));
        lv_label_set_text(s_status_state, state_buf);
        lv_label_set_text(s_status_waiting,
                          (s && s->waiting_for[0]) ? s->waiting_for : "");
    }

    lv_obj_set_style_bg_color(s_status_box, rgb(color), 0);
    lv_obj_set_style_text_color(s_status_state, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_status_waiting, lv_color_black(), 0);
}

static void update_sessions(void)
{
    if (!s_snap.connected) {
        lv_label_set_text(s_sessions_count, "");
        lv_label_set_text(s_sessions_name, "");
        return;
    }
    if (s_snap.session_count <= 0) {
        lv_label_set_text(s_sessions_count, "0");
        lv_label_set_text(s_sessions_name, "sessions");
        lv_obj_set_pos(s_sessions_name, X_MARGIN + 18, Y_SESSIONS);
        lv_obj_set_width(s_sessions_name, 240 - X_MARGIN * 2 - 18);
        return;
    }

    clamp_session_idx();
    const agent_session_t *s = &s_snap.sessions[s_session_idx];
    const char *name = s->name[0] ? s->name : (s->cwd[0] ? s->cwd : "session");

    lv_label_set_text_fmt(s_sessions_count, "%d/%d",
                          s_session_idx + 1, s_snap.session_count);
    lv_obj_update_layout(s_sessions_count);
    int count_w = (int)lv_obj_get_width(s_sessions_count);
    if (count_w < 1) count_w = 28;
    int name_x = X_MARGIN + count_w + 8;
    int name_w = 240 - X_MARGIN - name_x;
    if (name_w < 40) name_w = 40;
    lv_obj_set_pos(s_sessions_name, name_x, Y_SESSIONS);
    lv_obj_set_width(s_sessions_name, name_w);
    lv_label_set_text(s_sessions_name, name);
}

static void apply_snapshot_locked(void)
{
    if (s_sleeping) return;
    update_meter(true, s_snap.weekly_title, s_snap.weekly_pct, s_snap.weekly_reset);
    update_meter(false, s_snap.session_title, s_snap.session_pct, s_snap.session_reset);
    update_sessions();
    update_status_box();
}

static void update_clock_locked(void)
{
    time_t now = time(NULL);
    if (now < 1000000000L) {
        lv_label_set_text(s_clock, "--:--:--");
        if (s_sleep_clock) lv_label_set_text(s_sleep_clock, "--:--");
        return;
    }
    /* Apply companion utc_offset manually so we don't need tzset. */
    time_t local = now + s_utc_offset;
    struct tm t;
    gmtime_r(&local, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(s_clock, buf);

    if (s_sleep_clock) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        lv_label_set_text(s_sleep_clock, buf);
    }
}

/* Convert 1-bit logo to RGB565 ARGB-ish buffer (fg on transparent black). */
static void logo_to_rgb565(const agent_logo_t *logo, uint8_t *out_px)
{
    uint16_t fg = logo->color_rgb565;
    for (int y = 0; y < LOGO_H; y++) {
        for (int x = 0; x < LOGO_W; x++) {
            int bit_index = y * LOGO_W + x;
            int byte_i = bit_index / 8;
            int bit_i = 7 - (bit_index % 8);
            bool on = (logo->bits[byte_i] >> bit_i) & 1;
            uint16_t c = on ? fg : 0x0000;
            out_px[(y * LOGO_W + x) * 2] = (uint8_t)(c & 0xFF);
            out_px[(y * LOGO_W + x) * 2 + 1] = (uint8_t)(c >> 8);
        }
    }
}

static float frand01(void)
{
    return (float)(lv_rand(0, 10000)) / 10000.0f;
}

static void sleep_reinit_sprites(void)
{
    s_spr_count = 0;
    int logos = s_logo_count < 3 ? s_logo_count : 3;
    for (int i = 0; i < logos; i++) {
        logo_to_rgb565(&s_logos[i], s_sleep_logo_px[i]);
        s_sleep_logo_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_sleep_logo_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_sleep_logo_dsc[i].header.w = LOGO_W;
        s_sleep_logo_dsc[i].header.h = LOGO_H;
        s_sleep_logo_dsc[i].header.stride = LOGO_W * 2;
        s_sleep_logo_dsc[i].data_size = LOGO_W * LOGO_H * 2;
        s_sleep_logo_dsc[i].data = s_sleep_logo_px[i];

        if (!s_sleep_logo_imgs[i]) {
            s_sleep_logo_imgs[i] = lv_image_create(s_sleep_layer);
        }
        lv_image_set_src(s_sleep_logo_imgs[i], &s_sleep_logo_dsc[i]);
        lv_obj_remove_flag(s_sleep_logo_imgs[i], LV_OBJ_FLAG_HIDDEN);
        s_spr_is_clock[i] = false;
        s_spr_x[i] = (float)lv_rand(0, 240 - LOGO_W);
        s_spr_y[i] = (float)lv_rand(0, 240 - LOGO_H);
        float ang = frand01() * 6.28318f;
        float spd = 0.6f + frand01() * 1.2f;
        s_spr_vx[i] = spd * (float)(ang < 3.14f ? 1 : -1) * (0.5f + frand01());
        s_spr_vy[i] = spd * (0.5f + frand01()) * (frand01() > 0.5f ? 1.0f : -1.0f);
        s_spr_count++;
    }
    for (int i = logos; i < MAX_SLEEP_SPRITES - 1; i++) {
        if (s_sleep_logo_imgs[i]) lv_obj_add_flag(s_sleep_logo_imgs[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Clock sprite */
    int ci = s_spr_count;
    s_spr_is_clock[ci] = true;
    s_spr_x[ci] = (float)lv_rand(20, 160);
    s_spr_y[ci] = (float)lv_rand(20, 200);
    s_spr_vx[ci] = 0.8f;
    s_spr_vy[ci] = 0.6f;
    if (!s_sleep_clock) {
        s_sleep_clock = make_label(s_sleep_layer, s_font_value, rgb(COL_TITLE));
    }
    lv_obj_remove_flag(s_sleep_clock, LV_OBJ_FLAG_HIDDEN);
    s_spr_count++;
}

static void sleep_animate(uint32_t now_ms)
{
    if (now_ms - s_last_sleep_frame_ms < SLEEP_FRAME_MS) return;
    s_last_sleep_frame_ms = now_ms;

    for (int i = 0; i < s_spr_count; i++) {
        int w = s_spr_is_clock[i] ? 80 : LOGO_W;
        int h = s_spr_is_clock[i] ? 28 : LOGO_H;
        s_spr_x[i] += s_spr_vx[i];
        s_spr_y[i] += s_spr_vy[i];
        bool bounce = false;
        if (s_spr_x[i] < 0) { s_spr_x[i] = 0; s_spr_vx[i] = -s_spr_vx[i]; bounce = true; }
        if (s_spr_x[i] > 240 - w) { s_spr_x[i] = (float)(240 - w); s_spr_vx[i] = -s_spr_vx[i]; bounce = true; }
        if (s_spr_y[i] < 0) { s_spr_y[i] = 0; s_spr_vy[i] = -s_spr_vy[i]; bounce = true; }
        if (s_spr_y[i] > 240 - h) { s_spr_y[i] = (float)(240 - h); s_spr_vy[i] = -s_spr_vy[i]; bounce = true; }
        if (bounce) {
            float j = 0.85f + frand01() * 0.3f;
            s_spr_vx[i] *= j;
            s_spr_vy[i] *= (0.85f + frand01() * 0.3f);
        }
        if (s_spr_is_clock[i]) {
            lv_obj_set_pos(s_sleep_clock, (int)s_spr_x[i], (int)s_spr_y[i]);
        } else {
            lv_obj_set_pos(s_sleep_logo_imgs[i], (int)s_spr_x[i], (int)s_spr_y[i]);
        }
    }
    update_clock_locked();
}

void ui_init(lv_obj_t *parent, ui_brightness_cb_t brightness_cb)
{
    create_fonts();
    s_brightness_cb = brightness_cb;
    s_scr = parent;

    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.connected = false;
    snprintf(s_snap.weekly_reset, sizeof(s_snap.weekly_reset), "--");
    snprintf(s_snap.session_reset, sizeof(s_snap.session_reset), "--");

    lv_obj_set_style_bg_color(parent, rgb(COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_title = make_label(parent, s_font_title, rgb(COL_TITLE));
    lv_label_set_text(s_title, "claudius");
    lv_obj_set_pos(s_title, X_MARGIN, Y_TITLE);
    /* Leave room for HH:MM:SS on the right. */
    lv_obj_set_width(s_title, 240 - X_MARGIN - 72);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);

    s_clock = make_label(parent, s_font_small, rgb(COL_TITLE));
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -X_MARGIN, Y_TITLE + 2);

    s_w_label = make_label(parent, s_font_small, rgb(COL_LABEL));
    lv_obj_set_pos(s_w_label, X_MARGIN, Y_WMETER);
    s_w_reset = make_label(parent, s_font_small, rgb(COL_RESET));
    lv_obj_align(s_w_reset, LV_ALIGN_TOP_RIGHT, -X_MARGIN, Y_WMETER);
    s_w_bar = make_bar(parent, Y_WBAR);
    s_w_pct = make_label(parent, s_font_small, rgb(COL_TITLE));
    lv_obj_align(s_w_pct, LV_ALIGN_TOP_RIGHT, -X_MARGIN, Y_WBAR + 2);

    s_s_label = make_label(parent, s_font_small, rgb(COL_LABEL));
    lv_obj_set_pos(s_s_label, X_MARGIN, Y_SMETER);
    s_s_reset = make_label(parent, s_font_small, rgb(COL_RESET));
    lv_obj_align(s_s_reset, LV_ALIGN_TOP_RIGHT, -X_MARGIN, Y_SMETER);
    s_s_bar = make_bar(parent, Y_SBAR);
    s_s_pct = make_label(parent, s_font_small, rgb(COL_TITLE));
    lv_obj_align(s_s_pct, LV_ALIGN_TOP_RIGHT, -X_MARGIN, Y_SBAR + 2);

    s_sessions_count = make_label(parent, s_font_small, rgb(COL_LABEL));
    lv_obj_set_pos(s_sessions_count, X_MARGIN, Y_SESSIONS);

    s_sessions_name = make_label(parent, s_font_small, rgb(COL_LABEL));
    lv_obj_set_pos(s_sessions_name, X_MARGIN + 36, Y_SESSIONS);
    lv_obj_set_width(s_sessions_name, 240 - X_MARGIN * 2 - 36);
    lv_label_set_long_mode(s_sessions_name, LV_LABEL_LONG_SCROLL);

    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, 240, 1);
    lv_obj_set_pos(s_divider, 0, Y_DIVIDER);
    lv_obj_set_style_bg_color(s_divider, rgb(COL_BAR_BG), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_set_style_pad_all(s_divider, 0, 0);
    lv_obj_remove_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    s_status_box = lv_obj_create(parent);
    lv_obj_set_size(s_status_box, 240, BOX_SIZE);
    lv_obj_set_pos(s_status_box, 0, Y_BOX);
    lv_obj_set_style_bg_color(s_status_box, rgb(COL_OFFLINE), 0);
    lv_obj_set_style_bg_opa(s_status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_box, 0, 0);
    lv_obj_set_style_radius(s_status_box, 0, 0);
    lv_obj_set_style_pad_all(s_status_box, 4, 0);
    lv_obj_remove_flag(s_status_box, LV_OBJ_FLAG_SCROLLABLE);

    s_status_state = make_label(s_status_box, s_font_value, lv_color_black());
    lv_obj_set_width(s_status_state, 232);
    lv_obj_set_style_text_align(s_status_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_status_state, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_status_state, "OFFLINE");
    lv_obj_align(s_status_state, LV_ALIGN_CENTER, 0, -8);

    s_status_waiting = make_label(s_status_box, s_font_small, lv_color_black());
    lv_obj_set_width(s_status_waiting, 232);
    lv_obj_set_style_text_align(s_status_waiting, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_status_waiting, LV_LABEL_LONG_SCROLL);
    lv_obj_align(s_status_waiting, LV_ALIGN_CENTER, 0, 18);

    /* Sleep overlay (hidden) */
    s_sleep_layer = lv_obj_create(parent);
    lv_obj_set_size(s_sleep_layer, 240, 240);
    lv_obj_set_pos(s_sleep_layer, 0, 0);
    lv_obj_set_style_bg_color(s_sleep_layer, rgb(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_sleep_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sleep_layer, 0, 0);
    lv_obj_set_style_pad_all(s_sleep_layer, 0, 0);
    lv_obj_remove_flag(s_sleep_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sleep_layer, LV_OBJ_FLAG_HIDDEN);

    s_group = lv_group_create();
    lv_group_set_default(s_group);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_indev, read_cb);
    lv_indev_set_group(s_indev, s_group);

    set_meter_visible(true, false);
    set_meter_visible(false, false);
    apply_snapshot_locked();
    update_clock_locked();
    apply_brightness(s_user_brightness);
}

void ui_set_title(const char *device_name)
{
    const char *device = (device_name && device_name[0]) ? device_name : "claudius";

    lock_ui();
    if (s_title) lv_label_set_text(s_title, device);
    unlock_ui();
}

void ui_set_status(const status_snapshot_t *snap)
{
    if (!snap) return;
    lock_ui();
    s_snap = *snap;
    clamp_session_idx();
    apply_snapshot_locked();
    unlock_ui();
}

void ui_set_connected(bool connected)
{
    lock_ui();
    s_snap.connected = connected;
    if (!connected) s_snap.auth_failed = false;
    apply_snapshot_locked();
    unlock_ui();
}

void ui_set_auth_failed(bool failed)
{
    lock_ui();
    s_snap.auth_failed = failed;
    if (failed) {
        s_snap.connected = false;
        s_snap.session_count = 0;
        s_snap.any_active = false;
    }
    apply_snapshot_locked();
    unlock_ui();
}

void ui_set_agent_logos(const agent_logo_t *logos, int count)
{
    lock_ui();
    s_logo_count = 0;
    if (logos && count > 0) {
        if (count > MAX_AGENT_LOGOS) count = MAX_AGENT_LOGOS;
        memcpy(s_logos, logos, (size_t)count * sizeof(agent_logo_t));
        s_logo_count = count;
    }
    unlock_ui();
}

void ui_set_utc_offset(long offset_sec)
{
    lock_ui();
    s_utc_offset = offset_sec;
    update_clock_locked();
    unlock_ui();
}

void ui_sleep_start(void)
{
    lock_ui();
    if (s_sleeping) {
        unlock_ui();
        return;
    }
    s_sleeping = true;
    lv_obj_remove_flag(s_sleep_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_sleep_layer);
    sleep_reinit_sprites();
    apply_brightness(UI_SLEEP_BRIGHTNESS);
    unlock_ui();
}

void ui_wake(void)
{
    lock_ui();
    if (!s_sleeping) {
        unlock_ui();
        return;
    }
    s_sleeping = false;
    lv_obj_add_flag(s_sleep_layer, LV_OBJ_FLAG_HIDDEN);
    apply_brightness(s_user_brightness);
    apply_snapshot_locked();
    update_clock_locked();
    unlock_ui();
}

bool ui_is_sleeping(void)
{
    return s_sleeping;
}

void ui_tick(uint32_t now_ms)
{
    lock_ui();
    time_t now = time(NULL);
    if ((uint32_t)now != s_last_clock_sec) {
        s_last_clock_sec = (uint32_t)now;
        if (!s_sleeping) update_clock_locked();
    }
    if (s_sleeping) sleep_animate(now_ms);
    unlock_ui();
}

void ui_on_tap(void)
{
    if (s_sleeping) {
        ui_wake();
        return;
    }
    lock_ui();
    cycle_session(+1);
    apply_snapshot_locked();
    unlock_ui();
}

void ui_on_tap_burst(int count)
{
    if (s_sleeping) {
        ui_wake();
        return;
    }
    lock_ui();
    if (count == 2) cycle_session(-1);
    else cycle_session(+1);
    apply_snapshot_locked();
    unlock_ui();
}

void ui_on_long_press(void)
{
    if (s_sleeping) {
        ui_wake();
        return;
    }
    /* Reserved — no settings UI yet. */
}

int ui_get_brightness(void)
{
    return s_user_brightness;
}

void ui_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lock_ui();
    s_user_brightness = percent;
    if (!s_sleeping) apply_brightness(percent);
    unlock_ui();
}
