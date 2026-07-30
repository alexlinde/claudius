# gm-claude (GeekMagic-S3 claudius screen)

ESP-IDF v6.0 firmware for the **GeekMagic-S3** (ESP32-S3, ST7789 240×240, capacitive
touch) that renders live Claude Code status on a desk display — **claudius**.

Protocol and layout are inspired by
[codelight](https://github.com/henrikekblad/codelight)’s ESP8266 `screen/` firmware.

The device is a WebSocket client: it discovers the companion via mDNS
(`_claudius._tcp`) or a configured host, subscribes as `client: screen`, and
shows usage bars plus each Claude session’s exact `state` / `status` /
`waitingFor` from `claude agents --json`. Tap cycles between sessions.

## Hardware

| Feature   | Detail                                       |
|-----------|----------------------------------------------|
| SoC       | ESP32-S3 (dual-core 240 MHz, 8 MB PSRAM)     |
| Flash     | 16 MB quad SPI                               |
| Display   | ST7789 240×240 SPI (80 MHz)                  |
| Touch     | Capacitive touch button on GPIO9 (TOUCH CH9) |
| Backlight | GPIO14 (active-LOW)                          |

## Architecture

```
claude agents --json
        │ poll (~2s)
        ▼
Claudius.app / claudius.py (:8765, mDNS _claudius._tcp)
        │ WebSocket push (sessions[] + usage)
        ▼
GeekMagic-S3 firmware (claudius)
  WiFi → mDNS/host → WS auth → LVGL status UI (tap cycles sessions)
```

## Companion (Claude status → screen)

### macOS app (recommended)

A menu-bar companion lives in [`macos/Claudius/`](macos/Claudius/):

```bash
cd macos/Claudius
xcodegen generate   # once, or after editing project.yml
open Claudius.xcodeproj
# Product → Run, or:
xcodebuild -scheme Claudius -configuration Release -destination 'platform=macOS' build

# Signed installable DMG (see macos/Claudius/README.md):
./release.sh --skip-notarize
```

On launch the mDNS name defaults to this Mac’s hostname (override in Preferences to match the screen). Optional shared secret goes in Preferences and must match the screen config. Enable **Open at Login** to keep the companion running.

The app advertises `_claudius._tcp`, serves WebSocket status on port **8765** (configurable), polls `claude agents --json`, and pushes usage bars from Claude OAuth credentials.

### Python daemon (Linux / headless fallback)

```bash
cd companion
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 claudius.py --name my-laptop
# optional shared secret (must match the screen config):
python3 claudius.py --name my-laptop --secret mypassword
```

It polls `claude agents --json` for live session state, advertises
`_claudius._tcp` via mDNS, and pushes per-session `state` / `status` /
`waitingFor` plus usage bars to the GeekMagic-S3.

## Building the firmware

Prerequisites: ESP-IDF v6.0 via [EIM](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/index.html).

```bash
source ~/.espressif/tools/activate_idf_v6.0.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

Press `Ctrl+]` to exit the monitor.

## First-time WiFi setup

On first boot (or when no configured network is reachable) the device starts an
AP and shows setup mode on screen:

1. Join WiFi **`claudius-setup`**
2. Open `http://192.168.4.1`
3. Enter WiFi credentials (up to 3 networks), device name, companion name/host/secret
4. Save & apply — the device reboots, joins your LAN, and connects to the companion

Afterwards the config page is also at `http://<device-name>.local/`.

## Building the simulator (macOS)

Iterate on the status UI without flashing:

```bash
brew install sdl2 cmake
git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git sim/third_party/lvgl

cmake -S sim -B sim/build
cmake --build sim/build -j
./sim/build/gm_s3_sim
```

| Key   | Action                          |
|-------|---------------------------------|
| SPACE | Touch pad (tap cycles sessions) |
| 1/2/3 | Mock working / waiting / idle   |
| 0     | Mock OFFLINE                    |
| 4     | Mock AUTH FAIL                  |
| S     | Toggle screensaver              |

## UI and gestures

| Gesture    | Effect                                      |
|------------|---------------------------------------------|
| Tap        | Wake from screensaver, else next session    |
| Double tap | Previous session                            |
| Long press | Wake from screensaver                       |

Screensaver starts after 10 minutes without a companion (if enabled) or after
1 hour without active sessions. Active (`working` / `blocked` / `busy` /
`waiting`) activity wakes the dashboard. Agent logos from the companion
`config` message bounce on the sleep screen.

## OTA

Upload a firmware `.bin` from the config page (**Firmware update**) or:

```bash
curl -F "firmware=@build/gm_s3.bin" http://<device-name>.local/api/ota
```

## Project structure

```
├── main/                 # BSP, WiFi, mDNS, WebSocket, config HTTP, OTA
├── components/ui/        # Shared LVGL status dashboard (+ simulator)
├── companion/            # Python Claude status daemon (Linux/fallback)
├── macos/Claudius/       # Native macOS menu-bar companion (recommended)
├── sim/                  # macOS SDL2 simulator
├── partitions.csv        # 16 MB: factory + 2× OTA + storage
└── lv_conf.h             # Shared LVGL config
```

## Hardware notes (display / touch)

See the original [gm-s3](https://github.com/alexlinde/gm-s3) README for ST7789
SPI Mode 3 / SPI3_HOST / active-LOW backlight and touch calibration details —
unchanged in this client.
