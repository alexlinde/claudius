# gm-s3

ESP-IDF v6.0 firmware for the GeekMagic-S3 device (ESP32-S3, ST7789 240x240, capacitive touch button) with an LVGL 9 UI and a native macOS SDL2 simulator.

## Hardware

| Feature       | Detail                                       |
|---------------|----------------------------------------------|
| SoC           | ESP32-S3 (dual-core 240 MHz, 8 MB PSRAM)    |
| Flash         | 16 MB quad SPI                               |
| Display       | ST7789 240x240 SPI (80 MHz)                  |
| Touch         | Capacitive touch button on GPIO9 (TOUCH CH9) |
| Backlight     | GPIO14 (active-LOW)                          |

### Pin map

| Signal | GPIO |
|--------|------|
| MOSI   | 11   |
| SCLK   | 12   |
| DC     | 7    |
| RST    | 6    |
| CS     | n/c  |
| BL     | 14   |
| TOUCH  | 9    |

## Project structure

```
├── CMakeLists.txt            # ESP-IDF root project
├── sdkconfig.defaults        # Target & peripheral config
├── partitions.csv            # 16 MB partition table (factory + 2x OTA + storage)
├── lv_conf.h                 # Shared LVGL config (firmware & simulator)
├── main/
│   ├── main.c                # app_main entry point
│   ├── bsp_display.c/.h      # SPI + ST7789 + LVGL port init
│   ├── bsp_touch.c/.h        # Capacitive touch driver (CH9)
│   └── idf_component.yml     # Component dependencies
├── components/
│   └── ui/                   # Platform-agnostic LVGL UI (shared)
│       ├── ui.c              # Screen, encoder indev + focus group
│       ├── include/ui.h
│       ├── assets/
│       │   └── Ubuntu-Medium.ttf
│       ├── embed_binary.py   # Build-time TTF -> C array codegen
│       └── include/font_ubuntu_medium.h
└── sim/                      # macOS SDL2 simulator
    ├── CMakeLists.txt
    └── main_sim.c
```

The UI also uses `lv_tiny_ttf` to render labels at runtime-chosen point sizes from an embedded TTF; the CMake build codegen's the font into a C array so both firmware and simulator link against the same glyph data.

## Building the firmware

Prerequisites: ESP-IDF v6.0 installed via [EIM](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/index.html).

```bash
source ~/.espressif/tools/activate_idf_v6.0.sh
idf.py set-target esp32s3
idf.py build
```

### Flash & monitor

```bash
idf.py -p /dev/cu.usbserial-* flash monitor
```

Press `Ctrl+]` to exit the monitor.

## Building the simulator (macOS)

Prerequisites: [Homebrew](https://brew.sh), SDL2, CMake, and a local copy of LVGL 9 in `sim/third_party/lvgl`.

```bash
brew install sdl2 cmake
git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git sim/third_party/lvgl
```

Build and run:

```bash
cmake -S sim -B sim/build
cmake --build sim/build -j
./sim/build/gm_s3_sim
```

Only `SPACE` is wired in the sim - it simulates the capacitive touch button with the same timing-based state machine `main/bsp_touch.c` builds on top of `espressif/button` (`short_press_time = 180 ms`, `long_press_time = 800 ms`). Rapid presses form a burst; hold past 800 ms for a long press.

## UI and gesture model

The UI is driven by the standard LVGL single-button idiom: an `LV_INDEV_TYPE_ENCODER` input device feeding an `lv_group_t` of focusable widgets. Gestures push lock-free atomic events that the indev's `read_cb` drains, so gesture sources (device `iot_button` callbacks, simulator key events) never need to hold the LVGL port lock.

Gesture recognition settles single- and double-taps only after the trailing pause (no label flicker through intermediate states); a 3rd tap reclassifies the burst live and every further tap lands immediately.

| Gesture      | Navigation mode              | Edit mode (focused slider)       |
|--------------|------------------------------|----------------------------------|
| Tap          | focus next (`LV_KEY_NEXT`)   | value +1                         |
| Double tap   | focus prev (`LV_KEY_PREV`)   | value -1                         |
| Burst x3     | -                            | value +3 (catches up held taps)  |
| Burst xN (N>=4) | -                         | value +1 per tap (live scrub)    |
| Long press   | enter edit (slider) / click (button) | exit edit                |

The default screen is a brightness demo: title, big percent readout, slider (0-100), and a "Reset 50%" button. The slider's `VALUE_CHANGED` handler calls the `ui_brightness_cb_t` passed into `ui_init`, so the UI component has no dependency on the BSP. `main.c` wires that callback to `bsp_display_set_backlight_percent()`; the simulator plugs in a stub that just prints the new value.

## Shared UI

All UI code lives in `components/ui/` and depends only on the LVGL 9 API (no ESP-IDF headers). Both the firmware and the simulator link against the same source files so you can iterate on the UX natively on your Mac and then flash it to the device.

## Hardware notes

These were discovered during bring-up and are important if you ever need to reconfigure the display or touch drivers.

### Display: SPI Mode 3 required

The ST7789 variant on this board (ST7789_2) requires **SPI Mode 3** (CPOL=1, CPHA=1). The ESP-IDF `esp_lcd` examples default to Mode 0, which produces a blank screen on this hardware. This is configured via `.spi_mode = 3` in the `esp_lcd_panel_io_spi_config_t`.

### Display: SPI3_HOST required

The display must be driven on **SPI3_HOST** (not SPI2_HOST). SPI2_HOST fails silently on this board. In Arduino/TFT_eSPI terms, this corresponds to the `-D USE_HSPI_PORT` flag.

### Display: no CS pin

The display has no chip-select pin (CS is hardwired or not connected). Pass `.cs_gpio_num = -1` to the panel IO config.

### Display: backlight is active-LOW (hidden from callers)

GPIO14 drives the backlight with inverted logic (LOW = on, HIGH = off). We handle this at the peripheral with the LEDC channel's `flags.output_invert = 1`, so duty maps forward everywhere in code: 0 = off, max = full. The timer is configured at 10-bit resolution (1024 steps) for smooth low-end dimming.

The BSP stays policy-free: `bsp_display_init()` leaves the backlight off (LEDC `duty = 0`) and the application decides when to turn it on via `bsp_display_set_backlight_percent(0..100)`. Today `ui_init()` syncs the backlight to the slider's default (50%) at the end of its build; there is a brief flash of power-on VRAM before the first LVGL flush lands on the panel, documented in `main.c`.

### Touch: calibration scan is mandatory

The ESP-IDF v6.0 `touch_sens` driver requires an initial calibration scan before the active threshold has any meaning. Without it, the benchmark is uninitialized and touches are never detected. The correct sequence is:

1. Create controller and channel with estimated threshold
2. Set `charge_speed = TOUCH_CHARGE_SPEED_7` and `init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT` in the channel config (omitting these causes measurement timeouts)
3. Enable, run `touch_sensor_trigger_oneshot_scanning` 3 times, disable
4. Read the benchmark via `touch_channel_read_data(..., TOUCH_CHAN_DATA_TYPE_BENCHMARK, ...)`
5. Set `active_thresh = benchmark * 0.02` (2% of benchmark)
6. Reconfig the channel, register callbacks, enable, start continuous scanning

### Touch: bridging to `iot_button` via a custom driver

The ESP-IDF `touch_sens` callbacks (`on_active` / `on_inactive`) fire from the driver's own task and shouldn't do anything heavy. We just update an `_Atomic bool` with the current press state, then hand that to `iot_button` through a `button_driver_t` whose `get_key_level` reads that atomic. `iot_button` then runs its own state machine for single/double clicks, repeat counts, long-press, etc., and dispatches higher-level callbacks that push events into the UI's indev queue. This keeps all timing logic in one well-tested component instead of hand-rolled debouncing.
