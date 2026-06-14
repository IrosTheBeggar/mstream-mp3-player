# mstream-mp3-player

An experimental portable MP3/FLAC player built on the **ESP32-S3**, designed to
dock to a host running [mStream](https://mstream.io) over USB-C and sync its
library as a mass-storage device.

This repo is the firmware + dev harness. You can build and run the whole UI and
logic **today on this laptop** in the [Wokwi](https://wokwi.com) simulator — no
hardware required — while the dev board ships.

> **Status:** scaffold. UI, playlist/transport, and the dock state machine work
> in simulation. Real audio output and the USB mass-storage handoff are stubbed
> behind a HAL until the physical board arrives. See
> [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## What's emulated vs. not

| | Where | Status |
|---|---|---|
| UI, menus, encoder/button nav, playlist, transport | Wokwi | ✅ works in sim |
| Dock-detect + MSC handoff **state machine** | Wokwi (DOCK button) | ✅ works in sim |
| mStream server + sync target | Docker | ✅ runs locally |
| Real I2S audio (you can hear it) | physical board | ⛔ needs hardware |
| USB-OTG mass-storage dock handoff | physical board | ⛔ needs hardware |

## Quick start

### 1. Install PlatformIO

```powershell
# via pipx (recommended) or pip
pip install -U platformio
pio --version
```

Or install the **PlatformIO IDE** + **Wokwi** extensions in VS Code.

### 2. Build the firmware

```powershell
pio run -e esp32-s3-wokwi
```

### 3. Run it in the simulator

- **VS Code:** open the folder, press `F1` → *Wokwi: Start Simulator*
  (uses `wokwi.toml` + `diagram.json`).
- The simulated device boots into the **Library** screen. Turn the encoder to
  scroll, press it (or **PLAY**) to start a track → **Now Playing** with a live
  progress bar. **PREV/NEXT** change tracks. Press the **DOCK** button to toggle
  the USB mass-storage "docked" screen.

No SD files needed — a built-in demo library loads if the card is empty. To use
real files, drop them in [`sd_card/`](sd_card/README.md).

### 4. Run the host unit tests

The portable core (`lib/core`) is tested off-target — no board, no emulator:

```powershell
pio test -e native
```

(Needs a host C/C++ compiler — MinGW-w64 or MSVC on Windows.)

### 5. Start the mStream dev server (optional, for the sync side)

```powershell
docker compose -f docker/docker-compose.yml up -d
# http://localhost:3000
```

Point a USB stick or a local folder at `docker/dev-data/music` to stand in for
the docked player. See [`docker/docker-compose.yml`](docker/docker-compose.yml).

## Layout

```
platformio.ini        Build envs: esp32-s3-wokwi | hardware | native
wokwi.toml            Wokwi <-> firmware binding
diagram.json          Simulated board: S3 + ILI9341 + KY-040 + microSD + buttons
include/Pins.h        Central pin map (build flags are source of truth)
lib/core/             Portable, framework-agnostic logic (compiles for native)
  PlaybackController  Transport + playlist
  DockController      Dock-detect + MSC handoff state machine
  hal/                IAudioBackend, IStorage, IDock interfaces
src/                  Hardware side (Arduino/ESP32 only)
  audio/ storage/ dock/ input/ ui/   HAL impls + UI/input
  main.cpp            Wires core to hardware, runs the UI loop
test/                 Host unit tests (Unity)
docker/               mStream server for the sync side
docs/ARCHITECTURE.md  Layering, HAL seams, pin map, roadmap
```

## Hardware (target prototype, ~$65–80)

ESP32-S3-DevKitC-1 (N16R8) · PCM5102A I2S DAC · microSD (4-bit SDIO) · 1.9"
ST7789 TFT · rotary encoder + 3 buttons · 1500mAh LiPo + TP4056 + load-sharing.
The "dock" is just a USB-C cable to any machine running the mStream image.
