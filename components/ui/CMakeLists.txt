set(_ttf_c "${CMAKE_CURRENT_BINARY_DIR}/font_ubuntu_medium.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS "ui.c" "${_ttf_c}"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES "esp_lvgl_port"
    )
endif()

# Skip the code-generation step during ESP-IDF's early-expansion pass (which
# only collects component requirements and does not allow add_custom_command).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(_ttf_in   "${CMAKE_CURRENT_SOURCE_DIR}/assets/Ubuntu-Medium.ttf")
    set(_embed_py "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary.py")

    if(ESP_PLATFORM)
        add_custom_command(
            OUTPUT  "${_ttf_c}"
            COMMAND python3 "${_embed_py}" "${_ttf_in}" "${_ttf_c}" font_ubuntu_medium
            DEPENDS "${_ttf_in}" "${_embed_py}"
            COMMENT "Embedding Ubuntu-Medium.ttf as C array"
            VERBATIM
        )
    else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_custom_command(
            OUTPUT  "${_ttf_c}"
            COMMAND ${Python3_EXECUTABLE} "${_embed_py}" "${_ttf_in}" "${_ttf_c}" font_ubuntu_medium
            DEPENDS "${_ttf_in}" "${_embed_py}"
            COMMENT "Embedding Ubuntu-Medium.ttf as C array"
            VERBATIM
        )
        add_library(ui STATIC ui.c "${_ttf_c}")
        target_include_directories(ui PUBLIC include)
        target_link_libraries(ui PUBLIC lvgl)
    endif()
endif()
