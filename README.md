# mstream-mp3-player

An experimental portable MP3/FLAC player on the **M5Stack Core2** that will
keep a copy of part of your [mStream](https://mstream.io) library and sync it
over WiFi. Listening is Bluetooth-first (A2DP headphones), with the built-in
speaker as a fallback.

> **Status:** proof of concept. Plays MP3 and FLAC from the SD card (or the
> Core2's internal flash) to Bluetooth headphones or the speaker. The UI is a
> tab bar (Now Playing, Library, Queue, Dance, Output) with first versions of
> its pages; the full browsing screens come next, then library sync and
> AutoDJ. The library is indexed from the card (and cached there), and the
> play queue survives a restart. Measurements: [docs/POC-RESULTS.md](docs/POC-RESULTS.md).
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

The three touch buttons under the screen do the same on every screen:
**prev** (hold: volume down 5 %, again every 0.2 s while held),
**play/pause** (hold: switch between speaker and Bluetooth; switching to the
speaker pauses first, so a slow press meant as a pause never moves the music
out loud: play/pause starts it again), **next** (hold: volume up). A hold
acts at half a second. The vibration motor confirms: one tick for a click
(or a tap on the screen that did something), a double tick the moment a hold
is recognised (a button's, or a long press on a list row that has one;
console `ah0` turns that off). A slow press on anything else on the screen
still counts as a tap. Volume is per output: the speaker and
Bluetooth keep their own. A hold shows the volume (or where the output
went) over the tab bar for a moment.

**The screen** has a tab bar at the top, on every page: **Now Playing** (its
icon's bars move while playing, with a progress line under them),
**Library**, **Queue** (a badge with the number of tracks up next), **Dance**,
and **Output**, which also shows the volume and the battery, and whose icon
is cyan with the headphones connected, amber while connecting, red when they
dropped out. Tap a tab to go there: each tab keeps its place (the Library
where you left it, the lists where they were scrolled). Tap the tab you are
on to go back to its start, "‹" in a page's header to go back one step.
Lists scroll with a finger and fling (a touch stops a moving list without
tapping a row); slide a finger down the letter rail on the right of a long
A-Z list (hidden while the list moves) to go through it. What the pages do
for now:

- **Now Playing**: the title, the artist and the album (tap either to open it
  in the Library, at the playing track), the progress, and prev / play-pause /
  next; "..." for "Go to artist / album", "Show in the queue".
- **Library**: artists A-Z, an artist's albums, an album's tracks. Play /
  Play next / + Queue sit at the top of an artist or an album, and under a
  track once you tap it (Play on a track plays its album from there). A long
  press on an artist or an album offers the same without opening it. Every
  change to the queue shows a message at the top with **Undo** (and **View**
  after an add: the Queue at the added tracks). Two levels down, "Artists"
  in the header goes straight back to the list of artists.
- **Queue**: the queue, opened on the playing track. Tap a track for Play /
  Play next / Remove; a long press (or "Select") selects several to remove.
  Tap the "Queue" title to go to the playing track, the top, the end, in
  turn.
- **Dance**: the dancer (below).
- **Output**: Bluetooth or the speaker, the volume, the touch calibration,
  and "Forget headphones".
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

The music is the `.mp3` and `.flac` files under `/music`
(`/music/Artist/Album/NN - Title.mp3`), indexed at boot; the index is cached
in `/.player` on the card and rebuilt when anything under `/music` changes.
The first time, the queue is the whole library (artist, album, track order)
followed by three built-in test tones and six click tracks (60 s at 90-174
BPM, for the beat tracker). The queue and its position are saved on the card:
after a restart it's where it was, stopped. The Library and Queue tabs edit
it, and so do the console's `q` commands (play an album, play it next, add
it, remove, clear, undo).

**Touch correction.** The Core2's touch panel reads touches on the right
half of the screen too far right (about 20 px at x 190, 35-45 px from x 240,
and it stops at 319), the same with any finger. Every touch is corrected
before anything looks at it, with a table fitted to measured taps. If the
default doesn't suit your Core2, `a` + Enter opens a calibration screen: tap
9 crosshairs (`a5` for 5), then Save; `ac` checks the result (a dot where the
Core2 reads each tap), `ad` goes back to the default table.

**Dancing crab** (proof of concept, [docs/MASCOT-POC.md](docs/MASCOT-POC.md)):
on the Dance tab (or send `d`), a pixel-art crab
dances to what's playing: it lands, squashes and snaps a claw on every beat.
The Core2 finds the beat itself (tempo and phase from the audio, no metadata
needed), over Bluetooth and on the speaker, and the crab idles (breathing,
blinking, glancing around, in dimmed colours) when there's no beat to follow.
A tap on the crab, or `m`, swaps it for the first proof of concept, a stick
figure, and back.

![The crab over two beats, phase 0/8 to 7/8 of each](docs/img/crab-phases.png)

The serial console (115200 baud) is there for scripted testing:

| Key | Action | Key + Enter | Action |
|---|---|---|---|
| `n` / `p` | next / previous | `i<n>` | play queue entry n (0-based) |
| space | play / pause | `b<n>` | benchmark decoding track n |
| `o` | switch output | `c<name>` | headphones to connect to |
| `+` / `-` | volume | `h<n>` | Bluetooth headroom -n dB, 0-12 (default 2, not saved) |
| `s` / `l` | stats / list the queue | `t<bpm>` | tempo prior for the dance (`t` clears) |
| `f` | forget the paired headphones and restart | `y<ms>` | dance latency offset (not saved) |
| `z` | silent test mode: speaker at volume 0, Bluetooth doesn't take over (until restart) | `k<n>` | freeze the dance pose, 0-15 (`k` unfreezes) |
| `d` / `v` | the Dance tab (again: back) / per-beat log | `ui` (`ui0`-`ui4`, `uib`) | the UI's navigation state: each tab's stack, scroll positions, frames, bus holds, the loop's stack; `ui<n>` taps tab n, `uib` goes back; a scripted finger for tests: `uit<x>,<y>` tap, `uih<x>,<y>` long press, `uis<x0>,<y0>,<x1>,<y1>,<ms>` swipe (a fling when fast), `uid...` drag |
| `m` | next dancer: crab (default) / stick figure | | |
| `x` / `X` | screenshot of the dancer / whole screen (base64 RGB565) | `q...` | the queue: `q` status, `qa` play everything, `qb` the built-in tracks, `ql` list albums, `qp<n>` / `qn<n>` / `q+<n>` album n: play / play next / add, `qr<n>` remove entry n, `qc` clear up next, `qx` clear, `qu` undo |
| | | `a...` | touch and haptics: `a` touch calibration (9 crosshairs; `a5`-`a9` for fewer), `ac` check the touch, `as` status, `ad` the default table, `ah0` / `ah1` haptics off / on, `ar0` / `ar1` the A-Z rail's ticks off / on, `aq` close (saved on the device) |

The UI spike's tools ([docs/UI-SPIKE.md](docs/UI-SPIKE.md)) measure the
browsing UI's risks before its screens are built. Each is a letter, an
optional argument and Enter:

| Command | Tool |
|---|---|
| `u` (`u0`-`u3`, `us`, `uh<ms>`, `ut<ms>`) | input lab: button and glass-touch logging, tab target practice, button practice, haptic ticks, percentile summary |
| `w` (`w0`-`w3`, `wv` `wf` `wh` `wc` `wd` `wk` `wg` `wm` `wp` `ws`) | scroll lab: a flick-scrolled library list, interactive or a 60 s stress while audio plays (hardware scroll and 30 fps by default) |
| `g` (`g0`, `g<n>`) | library index: report, rebuild from the card (the queue follows by path), or a synthetic library of n tracks for the labs (the player keeps the real one) |
| `e` (`e1`-`e5`) | font probe: list rows in FreeSans (folded) and the UI's VLW font, timed (efont only in a build with `-DUI_SPIKE_EFONT=1`) |
| `j` (`j<n>`, `jw<n>`, `ja`) | thumbnail probe: cover.jpg to 40x40 / 80x80, decoder RAM, PSRAM and .565 caches |

## Layout

```
platformio.ini        Build envs: core2 | native; local*.ini holds per-developer overrides
partitions.csv        4 MB app + ~11.9 MB LittleFS (test audio) + coredump
lib/core/             Portable logic, framework-agnostic (also compiled for native)
  PlaybackController  Transport over the queue; skips tracks that fail
  QueueModel          The play queue: track ids in PSRAM, current position,
                      stable keys, one level of undo
  QueueText           The queue saved as paths (survives a library rebuild)
  TrackCatalog        Track ids to paths and names: the index's tracks and
                      the built-in ones
  ByteStream          Byte sinks and sources for what is saved and loaded
  HeadsetKeys         What the headphones' transport keys do (never start music)
  PcmRing             PCM ring between the decode task and the active output
  TransportSync       Generation-tagged decode progress (no stale "track ended")
  ToneGen, ClickGen   Built-in test tones; click tracks with a known beat
  AudioTap, TapReader What an output played, placed in its track; audible-time clock
  BeatTracker         Tempo, phase and confidence from the audio (onsets, ACF, PLL)
  DancePose           The stick figure's joints as functions of the beat phase
  CrabPose, CrabArt   The crab's layer frames and offsets per beat phase; its
                      pixel art (generated from tools/art/crab.json)
  DanceSkin           Which dancer is on screen (crab, stick)
  LibraryIndex        The library in a few PSRAM blocks: string arena, records,
                      sorted views, A-Z buckets, folder tree; saved and loaded
                      as one file (LibrarySynth: made-up libraries of any size)
  TextFold            UTF-8 to ASCII for the GFX fonts; the library's sort order
  TouchCalibration    The touch correction: a monotonic piecewise-linear table
                      per axis, its fit to taps, and its bytes for NVS
  TouchRecognizer, ButtonGesture, ButtonPolicy, InputEvent
                      The glass's tap/hold/drag/fling events; the touch
                      buttons' click/hold/repeat, and what they do
  TouchGesture, KineticScroll, ScrollGovernor
                      Tap/hold/drag/flick; inertial list scrolling (flings
                      capped at 2,000 px/s); how hard a list may use the SPI
                      bus, from the audio buffer's fill
  VScrollMap          The LCD's hardware vertical scroll: which lines to draw
  RefillPacer         The decoder's gentle refill after a track start (on)
  BtControl           Bluetooth decisions: media stream (StreamControl), volume
                      (AbsVolumePolicy, GainRamp), reconnect (ReconnectPlanner)
  hal/                IAudioBackend, IStorage
src/                  Core2 firmware
  audio/              Core2AudioBackend (decode task), RingOutput, BtSink, SpeakerSink
  storage/            LocalStorage: SD card if present, else LittleFS; FileStream
  ui/                 Ui (the one owner of the display: navigation, tab bar,
                      overlays, pages), TabBar, ListView (lists on the
                      hardware scroll), Overlays (toast, HUD, sheet, dialog),
                      the pages (NowPlaying, Library, Queue, Dance, Output),
                      Fonts + VlwFonts (DejaVu, anti-aliased), Icons +
                      IconData, Gfx (pushes through the scroll and the
                      bus lock), Theme; Input (the one input layer: corrected
                      touches and the buttons as events, haptic feedback,
                      settings in NVS); CalibrationScreen (touch calibration);
                      BootScreen (the boot diagnostics); DanceView;
                      ListScroller (hardware scroll); LcdLock (times how long
                      the LCD holds the SPI bus)
  app/                Library (the index at boot: cache or build), QueueStore
                      (the queue on the card, its position in NVS), Psram,
                      SerialConsole, Diagnostics, DanceMode, Screenshot, Haptics
  spike/              UI spike tools: input lab, scroll lab, font and thumbnail
                      probes (docs/UI-SPIKE.md)
  main.cpp            Wires it together; input events (the buttons' policy),
                      Bluetooth events, what the UI reads (UiHost), the
                      console's queue and touch commands
data/                 LittleFS image source (data/music is gitignored)
tools/                make_test_audio.py; iram_diet.py (build post-script);
                      crab_art.py + art/crab.json (the crab's art -> lib/core/CrabArt.*);
                      vlw_font.py (the UI's DejaVu VLW fonts -> src/ui/VlwFonts.cpp);
                      ui_icons.py (the UI's 1-bit icons -> src/ui/IconData.cpp)
test/                 Host unit tests (Unity)
docker/               mStream dev server
docs/                 ARCHITECTURE.md, POC-RESULTS.md, MASCOT-POC.md, UI-SPIKE.md
LICENSES/             DejaVu-Fonts.txt (the fonts' licence)
```

## License

GPL-3.0 (see [LICENSE](LICENSE)), matching mStream and the audio libraries
this builds on. The UI's fonts are rasterised from DejaVu Sans, under the
Bitstream Vera fonts licence (DejaVu's changes are public domain): see
[LICENSES/DejaVu-Fonts.txt](LICENSES/DejaVu-Fonts.txt).
