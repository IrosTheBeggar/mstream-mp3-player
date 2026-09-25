# Architecture

A portable music player on the M5Stack Core2 (original ESP32, 16 MB flash,
8 MB PSRAM). It plays MP3 and FLAC from local storage to Bluetooth headphones
or the built-in speaker, and will keep its library in sync with an
[mStream](https://mstream.io) server over WiFi.

The guiding rule is unchanged from the first scaffold: **portable logic knows
nothing about hardware.** Anything that can be tested on the laptop lives in
`lib/core/` and is covered by `pio test -e native`; `src/` holds the Core2 side.

## Layers

```
              +---------------------------------------------------------------+
  lib/core/   |  PlaybackController   PcmRing   TransportSync   ToneGen       |  portable C++17,
  (portable)  |  Track, hal/IAudioBackend, hal/IStorage                       |  host-tested
              +------------------------------+--------------------------------+
                                             |
              +------------------------------+--------------------------------+
  src/        |  audio/  Core2AudioBackend (decode task), RingOutput,         |  Arduino-ESP32 3.x
  (Core2)     |          BtSink (ESP32-A2DP source), SpeakerSink (M5.Speaker) |  (pioarduino),
              |  storage/LocalStorage   ui/DisplayView   app/SerialConsole    |  M5Unified/M5GFX,
              |  main.cpp: buttons, Bluetooth events, rendering               |  ESP8266Audio
              +---------------------------------------------------------------+
```

## Audio pipeline

```
 source:  /music on LittleFS (SD card later: same fs::FS code)  |  built-in tone: tracks
            └ AudioFileSourceFS (+ID3 for MP3)                   |    └ ToneGen
                └ AudioGeneratorMP3 (libmad) | AudioGeneratorFLAC (libFLAC)
 decode task (core 1, prio 2, 16 KB internal stack) ─► RingOutput ─► PcmRing (PSRAM, 64k frames ≈ 1.5 s)
                                                                        ├─► BtSink: ESP32-A2DP data callback (Bluetooth task, 44.1 kHz)
                                                                        └─► SpeakerSink: pump task ─► M5.Speaker.playRaw (44.1 kHz out, mono)
```

The rules that keep it deadlock- and glitch-free:

- **One ring, two consumers, one active at a time.** `PcmRing::setConsumer()`
  hands the right to read to one output; the other reads nothing. Switching
  output is a consumer handover: the decoder never touches M5.Speaker, and
  neither `M5.Speaker.end()` nor ESP32-A2DP's `end()` is ever called.
- **The Bluetooth callback never blocks or logs.** It takes what the ring has,
  pads with silence, and only *tries* the ring's mutex (taken for real only by
  `setConsumer()` and `discardAll()`).
- **Pause happens in the outputs.** They play silence without reading, so pause
  is instant and the buffered audio waits for resume.
- **Track changes don't wait for the outputs.** A skip calls `discardAll()`;
  a natural end drains the ring first (`finished()` = end of file *and* ring
  empty), so a new track's sample rate never plays into the old track's tail.
- **The decoder never blocks inside an output.** `RingOutput::ConsumeSample`
  returns false when the ring is full or the pass's budget (1024 frames) is
  spent; the generator keeps that sample and retries it on its next `loop()`.
  The decode task re-checks requests and yields between passes.
- **Requests are generations.** `play()`/`stop()` post a new generation to
  `TransportSync`; the decode task's progress reports for anything older are
  dropped, so a stale "ended" can't skip the track that was just requested.
- **Bluetooth is 44.1 kHz only.** ESP-IDF's SBC source takes nothing else, so
  other rates fail on Bluetooth (the player skips them) until a resampler lands.
  The speaker takes any rate: `RingOutput` keeps the rate in an `int` because
  ESP8266Audio's base class stores it in a `uint16_t`.

## Tasks and cores

| Task | Core | Priority | Notes |
|---|---|---|---|
| Bluetooth controller + host (Bluedroid) | 0 | high | ~70 KB internal RAM, claimed at boot |
| A2DP data callback | 0 (BT task) | — | every ~30 ms, pulls 44.1 kHz stereo |
| decode | 1 | 2 | 16 KB stack in internal RAM (flash reads can't use a PSRAM stack) |
| speaker pump | 1 | 3 | three 1024-frame buffers, release-callback handshake |
| M5.Speaker | 1 | 2 | mixes/resamples to 44.1 kHz mono |
| Arduino loop (UI, console, buttons) | 1 | 1 | redraws at 4 Hz |

## Bluetooth

`BtSink` connects to the first headphones whose name contains the configured
name (`BT_SINK_NAME` build flag, or the console's `c<name>`, saved in NVS).
Without a name it only accepts a device practically touching the Core2;
signal strength alone once picked a TV in the next room. It reconnects to the
last device for ~30 s after boot, then scans again. On connect the output
switches to Bluetooth; a real disconnect pauses playback.

## Storage

`LocalStorage` mounts the SD card (shared SPI bus with the LCD, 25 MHz) if one
is present, otherwise the ~11.9 MB LittleFS partition, and lists the `.mp3` and
`.flac` files under `/music` (recursive, sorted, capped at 200). The planned
library index and sync from mStream replace this scan (see Roadmap).

## Build notes

- **pioarduino** (Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5) instead of the official
  platform, which is stuck on Arduino 2.0.17. On Windows, build from PowerShell
  with `MSYSTEM` unset.
- **IRAM is the scarcest resource** on the original ESP32 (128 KB). Bluetooth
  plus M5Unified overflowed it with the prebuilt Arduino libraries. Two fixes:
  `lib_archive = yes` (pioarduino otherwise links every library object), and
  `tools/iram_diet.py`, which moves the libc functions that the rev-1 PSRAM
  workaround pins in IRAM back to flash (this rev-3 chip doesn't need it).
  About 7 KB of IRAM is left. Adding WiFi will need more: likely pioarduino's
  `custom_sdkconfig` to rebuild the framework without the workaround.

## Roadmap

1. **Library index + sync over WiFi.** mStream exports a manifest and compact
   index files (tracks, albums, artists, strings) for the synced selection; the
   player mirrors files to the SD card under the server's paths, downloads with
   resumable requests, and swaps the index in atomically. First fill by card
   reader; WiFi for updates. WiFi and Bluetooth don't share the radio well, so
   sync is its own mode.
2. **Browsing UI:** artists, albums (92 px covers from mStream's thumbnails),
   playlists, queue, resume after power-off.
3. **AutoDJ:** mStream precomputes a similar-tracks table (top-K neighbours per
   synced track, from its 1280-d embeddings) that the player walks with
   mStream's session-centroid scoring plus its BPM/key/artist filters.
4. **Server discovery without mDNS** (it doesn't work in Docker installs), then
   the device-code pairing flow.
5. Headphone buttons (AVRCP) mapped to the player, a resampler for 48 kHz on
   Bluetooth, the RCA/3.5 mm module (`cfg.external_speaker.module_rca`), SD card
   verification, power management.
