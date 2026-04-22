if(ESP_PLATFORM)
    idf_component_register(
        SRCS "ui.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES "esp_lvgl_port"
    )
else()
    add_library(ui STATIC ui.c)
    target_include_directories(ui PUBLIC include)
    target_link_libraries(ui PUBLIC lvgl)
endif()
