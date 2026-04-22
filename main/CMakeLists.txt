idf_component_register(
    SRCS "main.c" "bsp_display.c" "bsp_touch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES "driver" "esp_lcd" "esp_timer" "esp_driver_ledc" "esp_driver_touch_sens" "ui"
)
