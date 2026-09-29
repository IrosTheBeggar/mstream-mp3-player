# mstream-mp3-player

An experimental portable MP3/FLAC player on the **M5Stack Core2** that will
keep a copy of part of your [mStream](https://mstream.io) library and sync it
over WiFi. Listening is Bluetooth-first (A2DP headphones), with the built-in
speaker as a fallback.

> **Status:** proof of concept. Plays MP3 and FLAC from the SD card (or the
> Core2's internal flash) to Bluetooth headphones or the speaker. The UI is a
> tab bar (Now Playing, Library, Queue, Dance, Output), all five built:
> Now Playing with the album cover, the Library (artists, albums with
> covers, folders, the A-Z jump grid), the Queue (editing, Undo), Output
> (Bluetooth pairing, per-output volume, settings, About); library sync and
> AutoDJ come next. The library is indexed from the card (and cached there), and the
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
tapping a row). A long A-Z list (over 30) has a letter rail on the right
(hidden while the list moves): slide a finger down it to go through the
list, or tap it for a grid of the letters; a letter with many entries
opens a second grid of its two-letter starts ("Ka", "Ke", "Ki"...). What the
pages do:

- **Now Playing**: the album's cover, the title, the artist and the album
  (tap either, or the cover, to open it in the Library, scrolled to the
  playing track), the progress, the volume (tap: a slider), prev /
  play-pause / next, and "..." for Go to artist, Go to album, Show in
  folders.
- **Library**: three lists at the top: **Artists**, **Albums** (with their
  covers) and **Folders** (the card's folders; only audio files are listed,
  the others counted: "14 audio files, 1 other"). An artist opens its
  albums and "All tracks"; an album or a folder its tracks. Play / Play
  next / + Queue sit at the top of each (at the Folders' top: "Play all N"
  and + Queue), and under a track once you tap it (Play on a track plays its
  album or folder from there); a long press on any row offers the same
  without opening it. What plays is tinted in every list. Every change to
  the queue shows a message at the top with **Undo** (and **View** after an
  add: the Queue at the added tracks). Two levels down, "‹ Library" in the
  header goes straight back to the lists.
- **Queue**: the queue, opened on the playing track, with what's left in
  the header ("4 of 16 · 12 up next · 49 min": lengths are learned as
  tracks play, "49+ min" until they all are). Tap a track for Play now /
  Play next / Remove. After Play next or + Queue in the Library, the next
  visit shows the added tracks, highlighted. **Edit** (or a long press on
  a track) selects: tap tracks, or All; then Remove, Play next, or
  Clear... ("Clear up next" keeps the playing song; "Clear queue" stops the
  music, and asks first). Every change can be undone from the message at
  the top. A track that couldn't be played is skipped with a note, and
  keeps a small "!". Tap the "Queue" title to go to the playing track, the
  top, the end, in turn. Empty, it offers Open Library and Shuffle all.
- **Dance**: the dancer (below).
- **Output**: a card for the Bluetooth headphones (connected, with the
  codec and delay; connecting, "try 2 of 3", with Cancel; failed, with Try
  again; Disconnect; Forget, which takes a second tap within 3 s, or,
  while connected, is in the "..." sheet and asks first) and one for the
  speaker; each shows its own volume (tap it for a slider). Tap a card to
  make it the output: the music stays where it is until the headphones
  are connected ("Now playing on ..." says when), and it pauses whenever
  it leaves them. Forgotten headphones stay forgotten (not even looked
  for by name) until you pair some again.
  **Pair new headphones** lists the audio devices in pairing mode nearby
  (with their signal); tap one to pair it, in place of the ones paired
  before (they stay if the new pairing fails). Then the line-out module's
  place (not fitted yet), **Haptics** on/off, **Screen off after**,
  **Brightness**, **Turn off when idle**, **CPU speed** (160 MHz saves
  battery, 240 MHz is smoother; it takes a restart, asked first, and the
  music stays paused after it) and **Bluetooth power** (Low, Normal,
  High), **Touch calibration**, and **About** (battery, storage, library,
  headphones, CPU speed and Bluetooth power, memory, version, and the tips
  again).

The first time, two tips show what the three red buttons do and that
tapping the tab you're on goes back to its start (console `uic` shows them
again). With no microSD card (and no music on the flash), the pages say so
and offer **Try again** (with a card in, the player restarts to use it);
a card without music offers the same, which looks through `/music` again.
If the headphones drop out while playing, the music pauses (it never
carries on out loud) and a message follows their reconnecting, with **Use
speaker** or **OK** to keep waiting.
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
An album's cover is the `cover.jpg` in its folder (else `folder.jpg`,
`front.jpg`, or the largest `.jpg` there). It is made into thumbnails the
first time it shows, which are kept in `/.player/thumbs` (delete that folder
to have them made again, after replacing a cover under the same name). A
progressive JPEG can't be decoded on the Core2: its album shows a note
instead.
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
| `d` / `v` | the Dance tab (again: back) / per-beat log | `ui` (`ui0`-`ui4`, `uib`) | the UI's navigation state: each tab's stack, scroll positions, frames, bus holds, the loop's stack; `ui<n>` taps tab n, `uib` goes back; a scripted finger for tests: `uit<x>,<y>` tap, `uih<x>,<y>` long press, `uis<x0>,<y0>,<x1>,<y1>,<ms>` swipe (a fling when fast), `uid...` drag, `uip<x>,<ms>` a press on the button strip (y >= 240 is the strip in all of them); `uil<n>` the Library shows a made-up library of n tracks (look only, to see the lists at scale), `uil0` the card's again; `uic` the coach cards, `uiT` decode the covers again (timings), `uiV` the volume HUD, `uiF<c/s/p/l/n>` show a faked Bluetooth (connecting, searching, pairing, lost) or no-card state for screenshots, `uiF0` the real one |
| `m` | next dancer: crab (default) / stick figure | | |
| `x` / `X` | screenshot of the dancer / whole screen (base64 RGB565) | `q...` | the queue: `q` status, `qa` play everything, `qb` the built-in tracks, `ql` list albums, `qp<n>` / `qn<n>` / `q+<n>` album n: play / play next / add, `qr<n>` remove entry n, `qc` clear up next, `qx` clear, `qu` undo |
| | | `P...` | power measurement ([ARCHITECTURE.md](docs/ARCHITECTURE.md#power-measurement)): `P` a line (5 s of the power chip's readings: USB in, battery, the state), `Pl` one every 5 s, `Pw` to `/.player/power.csv`, `Pm<name>` a marker, `Pq1` the coulomb counter; A/B knobs (`P?`): backlight, screen off, CPU clock, Bluetooth TX power, 5 V boost, LED, IMU, speaker amp, loop delay, the dance tracker, the background reconnect; `Pz` plays an hour of silence |
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
  DanceRate           The Dance tab's frame rate: 10 fps idle, 24-30 dancing
  LibraryIndex        The library in a few PSRAM blocks: string arena, records,
                      sorted views, A-Z buckets, folder tree; saved and loaded
                      as one file (LibrarySynth: made-up libraries of any size)
  TextFold            UTF-8 to ASCII for the GFX fonts; the library's sort order
  JumpIndex           The A-Z jump grid: each letter's first row, and a big
                      letter's two-letter starts
  ThumbCache, ThumbScaler, JpegInfo
                      Album covers as thumbnails: the PSRAM LRU and what's
                      asked, the card's .565 files; the box-filter scaler;
                      a JPEG's size and whether it is progressive
  TouchCalibration    The touch correction: a monotonic piecewise-linear table
                      per axis, its fit to taps, and its bytes for NVS
  TouchRecognizer, StripButtons, ButtonGesture, ButtonPolicy, InputEvent
                      The glass's tap/hold/drag/fling events; the touch
                      buttons (only a touch that went down on one, and
                      stays), their click/hold/repeat, and what they do
  PowerWindow         The power chip's (AXP192) ADC registers decoded, and
                      their mean/min/max over a window (console P)
  OutputModel         The Output tab's Bluetooth card (its state for every
                      link state, a connection that failed), what the
                      listener asked for (BtSession), Forget's second
                      tap, the pairing scan's list
  UiText              The UI's fixed texts next to their room (the host
                      tests measure them with the firmware's fonts)
  QueueView           The Queue's summary from learned track lengths, the
                      mark on what a Library add put in, the failed-track
                      ring, Shuffle all
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
                      hardware scroll), Overlays (toast, HUD, sheet, volume
                      sheet, jump grid, dialog, coach cards), EmptyState,
                      the pages (NowPlaying, Library, Queue, Dance, Output
                      with Pair and About), Thumbs (album covers:
                      a worker task decodes them below the loop),
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
                      SerialConsole, Diagnostics, DanceMode, Screenshot, Haptics,
                      PowerProbe + PowerLab (power measurement and its knobs)
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
