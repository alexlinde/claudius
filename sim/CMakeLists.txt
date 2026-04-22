cmake_minimum_required(VERSION 3.16)
project(gm_s3_sim C)

set(CMAKE_C_STANDARD 11)

# ── LVGL from local third_party directory ──────────────────────────────
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/lvgl)

# Remove ARM NEON/Helium assembly that cannot compile on macOS
get_target_property(_lvgl_srcs lvgl SOURCES)
list(FILTER _lvgl_srcs EXCLUDE REGEX "\\.(S|s)$")
set_target_properties(lvgl PROPERTIES SOURCES "${_lvgl_srcs}")

# ── SDL2 ───────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

target_compile_definitions(lvgl PUBLIC
    GM_S3_SIM
    LV_CONF_INCLUDE_SIMPLE
)
target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})

# ── Shared UI component ───────────────────────────────────────────────
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../components/ui ui_build)
target_compile_definitions(ui PUBLIC GM_S3_SIM)

# ── Simulator executable ──────────────────────────────────────────────
add_executable(gm_s3_sim main_sim.c)

target_compile_definitions(gm_s3_sim PRIVATE GM_S3_SIM)
target_include_directories(gm_s3_sim PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(gm_s3_sim PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(gm_s3_sim PRIVATE ui lvgl ${SDL2_LIBRARIES})
