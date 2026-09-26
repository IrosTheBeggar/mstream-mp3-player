# mstream-mp3-player

An experimental portable MP3/FLAC player on the **M5Stack Core2** that will
keep a copy of part of your [mStream](https://mstream.io) library and sync it
over WiFi. Listening is Bluetooth-first (A2DP headphones), with the built-in
speaker as a fallback.

> **Status:** proof of concept. Plays MP3 and FLAC from the Core2's internal
> flash to Bluetooth headphones or the speaker, with a touch-button now-playing
> screen. SD card support is coded but untested. Library sync, browsing and
> AutoDJ come next. Measurements: [docs/POC-RESULTS.md](docs/POC-RESULTS.md).
> Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Hardware

M5Stack Core2 (tested on v1.3): original ESP32 (dual core, 240 MHz), 16 MB
flash, 8 MB PSRAM (4 MB usable), AXP192 power chip, 320×240 touch LCD with
three touch buttons, 1 W mono speaker, microSD. There is no headphone jack:
use Bluetooth headphones, the speaker, or (later) M5Stack's RCA/3.5 mm module.

## Quick start (Windows)

1. **PlatformIO Core 6.2.0 or newer:** `pip install -U platformio`.
2. **Build and flash from PowerShell** (not Git Bash: the ESP-IDF tools that
   pioarduino uses refuse to run under MSYS). The first build downloads the
   pioarduino platform and toolchain (about 1 GB) and takes a while.

   ```powershell
   Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
   pio run -e core2 -t upload
   ```

3. **Test audio.** Make known-pitch test files, plus optional excerpts of your
   own tracks, then write them to the Core2's flash filesystem:

   ```powershell
   python -m venv .venv-tools
   .venv-tools\Scripts\pip install imageio-ffmpeg
   .venv-tools\Scripts\python tools\make_test_audio.py --mp3 song.mp3 --flac song.flac
   pio run -e core2 -t uploadfs
   ```

4. **Headphones.** Tell the Core2 their Bluetooth name, either in a gitignored
   `local.ini`:

   ```ini
   [local]
   build_flags = -DBT_SINK_NAME=\"My Headphones\"
   ```

   or at runtime with the serial console command `c<name>` (saved on the
   device). Put the headphones in pairing mode near the Core2; it connects,
   switches its output to Bluetooth and remembers them: after a drop or a
   restart it tries them for ~30 s, then scans for a minute, then tries them
   again, and the headphones can also reconnect by themselves whenever they
   wake up.

5. **Host unit tests** for the portable core (needs a host C++ compiler, e.g.
   MinGW-w64): `pio test -e native`.

## Using it

The three touch buttons under the screen: **prev** (hold: volume down),
**play/pause** (hold: switch between speaker and Bluetooth), **next** (hold:
volume up). Volume is per output: the speaker and Bluetooth keep their own.
The headphones' own buttons work too (play, pause, next, previous, volume),
but never start music that wasn't playing: their play resumes paused
playback (not from stopped), and their next/previous while paused or stopped
only select the track: the Core2's play starts it, or the headphones' play
when paused.
Headphones with AVRCP absolute volume (most current ones) take over the
Bluetooth volume when they connect: the Core2 and the headphones show the same
value, and changing it on either side changes both. (Headphones whose remote
control only comes up after playback started take over mid-song: the music
goes silent for about a second, then fades back in over ~2 s, so the
change of their level is never heard as a jump.)

The playlist is the files under `/music` followed by three built-in
test tones.

The serial console (115200 baud) is there for scripted testing:

| Key | Action | Key + Enter | Action |
|---|---|---|---|
| `n` / `p` | next / previous | `i<n>` | play track n (0-based) |
| space | play / pause | `b<n>` | benchmark decoding track n |
| `o` | switch output | `c<name>` | headphones to connect to |
| `+` / `-` | volume | `h<n>` | Bluetooth headroom -n dB, 0-12 (default 2, not saved) |
| `s` / `l` | stats / list tracks | | |
| `f` | forget the paired headphones and restart | | |

## Layout

```
platformio.ini        Build envs: core2 | native; local*.ini holds per-developer overrides
partitions.csv        4 MB app + ~11.9 MB LittleFS (test audio) + coredump
lib/core/             Portable logic, framework-agnostic (also compiled for native)
  PlaybackController  Playlist + transport; skips tracks that fail
  HeadsetKeys         What the headphones' transport keys do (never start music)
  PcmRing             PCM ring between the decode task and the active output
  TransportSync       Generation-tagged decode progress (no stale "track ended")
  ToneGen             Built-in test tones
  BtControl           Bluetooth decisions: media stream (StreamControl), volume
                      (AbsVolumePolicy, GainRamp), reconnect (ReconnectPlanner)
  hal/                IAudioBackend, IStorage
src/                  Core2 firmware
  audio/              Core2AudioBackend (decode task), RingOutput, BtSink, SpeakerSink
  storage/            LocalStorage: SD card if present, else LittleFS
  ui/                 DisplayView (M5GFX): bring-up and now-playing screens
  app/                SerialConsole, Diagnostics
  main.cpp            Wires it together; buttons, Bluetooth events, rendering
data/                 LittleFS image source (data/music is gitignored)
tools/                make_test_audio.py; iram_diet.py (build post-script)
test/                 Host unit tests (Unity)
docker/               mStream dev server
docs/                 ARCHITECTURE.md, POC-RESULTS.md
```

## License

GPL-3.0 (see [LICENSE](LICENSE)), matching mStream and the audio libraries
this builds on.
