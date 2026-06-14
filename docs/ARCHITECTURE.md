# Architecture

A portable ESP32-S3 music player that plays from a local SD card and, when
dropped into a USB-C dock, presents that card to a host running
[mStream](https://mstream.io) as a mass-storage device for sync.

The guiding rule: **portable logic knows nothing about hardware.** Everything
device-specific sits behind a small HAL interface, so the interesting logic
(transport, playlist, dock handoff) is built and unit-tested on the laptop while
the dev board ships, and runs unchanged in the Wokwi simulator.

## Layers

```
              +-----------------------------------------------+
  lib/core/   |  PlaybackController     DockController         |   framework-agnostic C++17
  (portable)  |  Track                  (injected clock +      |   -> also compiles for `native`
              |                          callbacks)            |      host unit tests
              +----------------+------------------+------------+
                               | HAL interfaces   |
                               v                  v
              IAudioBackend   IStorage          IDock
                               |                  |
              +----------------+------------------+------------+
  src/        |  SimAudioBackend  SdStorage   SimDock          |   Arduino / ESP32 only
  (hardware)  |  Controls         DisplayView (TFT_eSPI)       |   built for the board + Wokwi
              +-----------------------------------------------+
```

- **`lib/core/`** — `PlaybackController`, `DockController`, `Track`, and the HAL
  interface headers (`hal/`). No `Arduino.h`, no globals, time is injected via
  `update(nowMs)`. This is what `pio test -e native` compiles and tests.
- **`src/`** — the Arduino entry point (`main.cpp`) plus the concrete HAL
  implementations and the UI/input code. Not compiled for `native`.

## HAL seams

| Interface | Sim implementation (Wokwi) | Real board (next) |
|-----------|----------------------------|-------------------|
| `IAudioBackend` | `SimAudioBackend` — advances a virtual playback head, no sound | `I2sAudioBackend` — decode MP3/FLAC → PCM5102A over I2S |
| `IStorage` | `SdStorage` over shared SPI | same, but 4-bit SDIO (`SD_MMC`) for throughput |
| `IDock` | `SimDock` — DOCK button toggles dock state | USB VBUS / host enumeration + TinyUSB MSC class |
| `IDiscovery` | scripted sightings in unit tests | `MdnsDiscovery` — browses `_mstream._tcp` via ESP32 mDNS |

### Discovery (Slice 1)

The player finds the user's server over the network with zero config. The
brain — dedupe, TTL pruning, stable sort, selection cursor — lives in
`DiscoveryController` (`lib/core`, host-tested); the radio-specific
`MdnsDiscovery` (`src/net`) browses `_mstream._tcp` and maps each result's
TXT records into a base-URL-shaped `ServerCandidate`. The server side is the
mStream `feat/mdns-discovery` PR.

Caveat: Wokwi's virtual network may not forward mDNS multicast to the host LAN,
so the sim can't always *see* a real server. The controller is fully unit-tested
regardless, and `-DDISCOVERY_FALLBACK_URL="host:port"` injects a known server so
the discovery UI can be exercised in simulation against a Dockerized mStream.

### What can't be emulated (and why it's fine)

- **Real I2S audio output** — no simulator drives an audio DAC. The decode
  pipeline can still *run* in sim; only the final PCM sink is stubbed.
- **USB-OTG device mode (the MSC dock handoff)** — no simulator models it. The
  `IDock` seam keeps this stubbed so the *state machine* around it is still
  fully testable (see `test/test_dock`).

Both wait for the physical board, and neither blocks UI/logic/sync development.

## Dock state machine

`DockController` owns the handoff so playback/storage stay decoupled:

```
        Inserted                         Removed
Undocked --------> [onDock(): stop +    Docked --------> [reclaimMassStorage();
                    release SD to host]                   onUndock(): remount +
         exposeMassStorage()                              rescan library]
         state = Docked                                   state = Undocked
```

Side effects are injected as `onDock` / `onUndock` callbacks (wired in
`main.cpp`), so the controller itself depends on nothing but `IDock`.

## Pin map (Wokwi sim)

Set via build flags in `platformio.ini`; mirrored for editors in `include/Pins.h`.

| Function | GPIO | Notes |
|----------|------|-------|
| TFT SCLK / MOSI / MISO | 12 / 11 / 13 | SPI bus, shared with SD |
| TFT CS / DC / RST / BL | 10 / 9 / 8 / 14 | |
| SD CS | 7 | shares the SPI bus above |
| Encoder CLK / DT / SW | 4 / 5 / 6 | KY-040 |
| Buttons Play / Prev / Next | 15 / 16 / 17 | active-low, `INPUT_PULLUP` |
| DOCK (sim dock toggle) | 18 | becomes USB detection on hardware |

On the real board the SD card moves to dedicated 4-bit SDIO pins; the display is
an ST7789 (`-DST7789_DRIVER`, `env:hardware`) instead of Wokwi's ILI9341.

## Roadmap

1. **Real decode in sim** — split `IAudioBackend` into a decoder + an `IPcmSink`
   so MP3/FLAC decode runs and is verifiable in Wokwi (sink = null), and the
   same decoder feeds I2S on hardware.
2. **`I2sAudioBackend`** on the PCM5102A.
3. **USB-MSC dock** via TinyUSB; real VBUS/enumeration detection in `IDock`.
4. **mStream sync** — auto-detect the docked player and reconcile the library
   (server side develops independently against a USB stick today).
5. **Power-path / battery UI** — TP4056 + load-sharing, gauge on the now-playing
   screen.
