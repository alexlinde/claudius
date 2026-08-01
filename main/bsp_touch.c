#include "bsp_touch.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "button_types.h"

#include "app_config.h"
#include "app_dbg.h"
#include "ui.h"

#define TOUCH_CHAN_ID           9
#define TOUCH_INIT_SCAN_TIMES  3
/* Relative threshold = smooth − benchmark. 2% was too eager in heat (baseline
 * drift / humidity) and could hold "pressed" long enough to factory-reset. */
#define TOUCH_THRESH_RATIO     0.05f
#define TOUCH_DEBOUNCE_CNT     5
#define TOUCH_DENOISE_LVL      2

/* iot_button timings (ms). short_press_time also defines the inter-tap window
 * within a multi-tap burst. Single- and double-tap commit only after the
 * trailing pause (no intermediate flicker); a 3rd tap reclassifies the burst
 * live via BUTTON_PRESS_REPEAT and each subsequent tap updates immediately. */
#define BTN_SHORT_PRESS_MS      180
#define BTN_LONG_PRESS_MS       800   /* wake from screensaver */
#define BTN_RESET_SHOW_MS      3000   /* start showing hold-to-reset UI */
#define BTN_FACTORY_RESET_MS   8000   /* commit factory reset */

static const char *TAG = "bsp_touch";
static _Atomic bool s_pressed;
static _Atomic bool s_reset_armed;

static bool on_touch_active(touch_sensor_handle_t sens_handle,
                            const touch_active_event_data_t *event,
                            void *user_ctx)
{
    atomic_store(&s_pressed, true);
    return false;
}

static bool on_touch_inactive(touch_sensor_handle_t sens_handle,
                              const touch_inactive_event_data_t *event,
                              void *user_ctx)
{
    atomic_store(&s_pressed, false);
    return false;
}

static uint8_t touch_get_key_level(button_driver_t *driver)
{
    (void)driver;
    return atomic_load(&s_pressed) ? 1 : 0;
}

static button_driver_t s_touch_btn_driver = {
    .enable_power_save = false,
    .get_key_level     = touch_get_key_level,
    .enter_power_save  = NULL,
    .del               = NULL,
};

/* ui_on_* functions are atomic event publishers, so no LVGL lock needed. */

static void on_single_click(void *btn, void *usr)
{
    (void)btn; (void)usr;
    ui_on_tap();
}

static void on_double_click(void *btn, void *usr)
{
    (void)btn; (void)usr;
    ui_on_tap_burst(2);
}

static void on_press_repeat(void *btn, void *usr)
{
    (void)usr;
    /* count < 3 may still settle as a single/double, so wait for SINGLE_CLICK
     * or DOUBLE_CLICK instead. At count == 3 the UI catches up; beyond that
     * is a steady per-tap update. */
    uint8_t count = iot_button_get_repeat((button_handle_t)btn);
    if (count < 3) return;
    ui_on_tap_burst((int)count);
}

static void on_long_press_start(void *btn, void *usr)
{
    (void)btn; (void)usr;
    ui_on_long_press();
}

static void on_long_press_hold(void *btn, void *usr)
{
    (void)usr;
    if (atomic_load(&s_reset_armed)) return;

    uint32_t ms = iot_button_get_pressed_time((button_handle_t)btn);
    if (ms < BTN_RESET_SHOW_MS) {
        ui_set_reset_progress(0);
        return;
    }
    if (ms >= BTN_FACTORY_RESET_MS) return;

    int pct = (int)((ms - BTN_RESET_SHOW_MS) * 100 /
                    (BTN_FACTORY_RESET_MS - BTN_RESET_SHOW_MS));
    if (pct < 1) pct = 1;
    if (pct > 99) pct = 99;
    ui_set_reset_progress(pct);
}

static void factory_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(900));
    app_config_factory_reset();
    app_dbg_log("factory reset: rebooting to setup");
    esp_restart();
}

static void on_factory_reset(void *btn, void *usr)
{
    (void)btn; (void)usr;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_reset_armed, &expected, true)) {
        return;
    }
    ESP_LOGW(TAG, "Factory reset triggered (%dms hold)", BTN_FACTORY_RESET_MS);
    app_dbg_log("factory reset: hold complete");
    ui_set_reset_progress(100);
    xTaskCreate(factory_reset_task, "factory_rst", 4096, NULL, 5, NULL);
}

static void on_press_up(void *btn, void *usr)
{
    (void)btn; (void)usr;
    if (!atomic_load(&s_reset_armed)) {
        ui_set_reset_progress(0);
    }
}

esp_err_t bsp_touch_init(void)
{
    touch_sensor_sample_config_t sample_cfg =
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2);
    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample_cfg);
    touch_sensor_handle_t sens_handle = NULL;
    ESP_RETURN_ON_ERROR(
        touch_sensor_new_controller(&sens_cfg, &sens_handle),
        TAG, "Touch controller init failed");

    touch_channel_config_t chan_cfg = {
        .active_thresh    = { 2000 },
        .charge_speed     = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    touch_channel_handle_t chan_handle = NULL;
    ESP_RETURN_ON_ERROR(
        touch_sensor_new_channel(sens_handle, TOUCH_CHAN_ID, &chan_cfg, &chan_handle),
        TAG, "Touch channel init failed");

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    filter_cfg.benchmark.denoise_lvl = TOUCH_DENOISE_LVL;
    filter_cfg.data.debounce_cnt = TOUCH_DEBOUNCE_CNT;
    ESP_RETURN_ON_ERROR(
        touch_sensor_config_filter(sens_handle, &filter_cfg),
        TAG, "Touch filter config failed");

    /* Initial scanning to calibrate the benchmark */
    ESP_RETURN_ON_ERROR(touch_sensor_enable(sens_handle), TAG, "Enable failed");
    for (int i = 0; i < TOUCH_INIT_SCAN_TIMES; i++) {
        ESP_RETURN_ON_ERROR(
            touch_sensor_trigger_oneshot_scanning(sens_handle, 2000),
            TAG, "Oneshot scan failed");
    }
    ESP_RETURN_ON_ERROR(touch_sensor_disable(sens_handle), TAG, "Disable failed");

    uint32_t benchmark = 0;
    ESP_RETURN_ON_ERROR(
        touch_channel_read_data(chan_handle, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark),
        TAG, "Read benchmark failed");

    uint32_t thresh = (uint32_t)(benchmark * TOUCH_THRESH_RATIO);
    if (thresh < 100) thresh = 100;
    ESP_LOGI(TAG, "CH%d benchmark=%" PRIu32 ", threshold=%" PRIu32 " (%.1f%%)",
             TOUCH_CHAN_ID, benchmark, thresh, TOUCH_THRESH_RATIO * 100);

    chan_cfg.active_thresh[0] = thresh;
    ESP_RETURN_ON_ERROR(
        touch_sensor_reconfig_channel(chan_handle, &chan_cfg),
        TAG, "Reconfig channel failed");

    touch_event_callbacks_t cbs = {
        .on_active   = on_touch_active,
        .on_inactive = on_touch_inactive,
    };
    ESP_RETURN_ON_ERROR(
        touch_sensor_register_callbacks(sens_handle, &cbs, NULL),
        TAG, "Touch callback register failed");

    ESP_RETURN_ON_ERROR(touch_sensor_enable(sens_handle), TAG, "Touch enable failed");
    ESP_RETURN_ON_ERROR(
        touch_sensor_start_continuous_scanning(sens_handle),
        TAG, "Touch scan start failed");

    button_config_t btn_cfg = {
        .long_press_time  = BTN_LONG_PRESS_MS,
        .short_press_time = BTN_SHORT_PRESS_MS,
    };
    button_handle_t btn_handle = NULL;
    ESP_RETURN_ON_ERROR(
        iot_button_create(&btn_cfg, &s_touch_btn_driver, &btn_handle),
        TAG, "iot_button_create failed");

    button_event_args_t wake_args = {
        .long_press = { .press_time = BTN_LONG_PRESS_MS },
    };
    button_event_args_t reset_args = {
        .long_press = { .press_time = BTN_FACTORY_RESET_MS },
    };

    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_SINGLE_CLICK,     NULL, on_single_click,     NULL),
        TAG, "register single_click failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_DOUBLE_CLICK,     NULL, on_double_click,     NULL),
        TAG, "register double_click failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_PRESS_REPEAT,     NULL, on_press_repeat,     NULL),
        TAG, "register press_repeat failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_LONG_PRESS_START, &wake_args, on_long_press_start, NULL),
        TAG, "register long_press_start failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_LONG_PRESS_START, &reset_args, on_factory_reset, NULL),
        TAG, "register factory_reset failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_LONG_PRESS_HOLD,  NULL, on_long_press_hold,  NULL),
        TAG, "register long_press_hold failed");
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(btn_handle, BUTTON_PRESS_UP,         NULL, on_press_up,         NULL),
        TAG, "register press_up failed");

    ESP_LOGI(TAG, "Touch button on CH%d (GPIO9) ready (short=%dms, long=%dms, reset=%dms)",
             TOUCH_CHAN_ID, BTN_SHORT_PRESS_MS, BTN_LONG_PRESS_MS, BTN_FACTORY_RESET_MS);
    return ESP_OK;
}
