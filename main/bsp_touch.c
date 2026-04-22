#include "bsp_touch.h"

#include <inttypes.h>
#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

#define TOUCH_CHAN_ID           9
#define TOUCH_INIT_SCAN_TIMES  3
#define TOUCH_THRESH_RATIO     0.02f

static const char *TAG = "bsp_touch";
static TaskHandle_t s_touch_task;

static bool on_touch_active(touch_sensor_handle_t sens_handle,
                            const touch_active_event_data_t *event,
                            void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_touch_task, &woken);
    return woken == pdTRUE;
}

static void touch_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lvgl_port_lock(0);
        ui_on_button_pressed();
        lvgl_port_unlock();
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

    xTaskCreate(touch_task, "touch", 3072, NULL, 5, &s_touch_task);

    touch_event_callbacks_t cbs = {
        .on_active = on_touch_active,
    };
    ESP_RETURN_ON_ERROR(
        touch_sensor_register_callbacks(sens_handle, &cbs, NULL),
        TAG, "Touch callback register failed");

    ESP_RETURN_ON_ERROR(touch_sensor_enable(sens_handle), TAG, "Touch enable failed");
    ESP_RETURN_ON_ERROR(
        touch_sensor_start_continuous_scanning(sens_handle),
        TAG, "Touch scan start failed");

    ESP_LOGI(TAG, "Touch button on CH%d (GPIO9) ready", TOUCH_CHAN_ID);
    return ESP_OK;
}
