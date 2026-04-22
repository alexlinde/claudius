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
│       ├── ui.c
│       └── include/ui.h
└── sim/                      # macOS SDL2 simulator
    ├── CMakeLists.txt
    └── main_sim.c
```

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

Press **SPACE** in the simulator window to simulate a touch button press on GPIO9.

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

### Display: backlight is active-LOW

GPIO14 drives the backlight with inverted logic: LOW = on, HIGH = off. The LEDC PWM duty of 0 gives full brightness.

### Touch: calibration scan is mandatory

The ESP-IDF v6.0 `touch_sens` driver requires an initial calibration scan before the active threshold has any meaning. Without it, the benchmark is uninitialized and touches are never detected. The correct sequence is:

1. Create controller and channel with estimated threshold
2. Set `charge_speed = TOUCH_CHARGE_SPEED_7` and `init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT` in the channel config (omitting these causes measurement timeouts)
3. Enable, run `touch_sensor_trigger_oneshot_scanning` 3 times, disable
4. Read the benchmark via `touch_channel_read_data(..., TOUCH_CHAN_DATA_TYPE_BENCHMARK, ...)`
5. Set `active_thresh = benchmark * 0.02` (2% of benchmark)
6. Reconfig the channel, register callbacks, enable, start continuous scanning

### Touch: ISR callback context

The `on_active` callback runs in ISR context. You cannot call LVGL functions directly from it. Use `vTaskNotifyGiveFromISR` to wake a dedicated task that acquires the LVGL mutex before updating the UI.
