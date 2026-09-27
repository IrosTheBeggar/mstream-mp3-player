# UI spike: measuring the tab bar design's risks

The browsing UI will follow the **tab bar** design (a 36 px bar of five icon
tabs at the top; the three touch buttons are transport only). Before any real
screen is built, this spike measures on the Core2 the parts of that design
most likely to fail. It adds tools, not screens: everything is reachable from
the serial console and can stay in the firmware as diagnostics.

| Risk (from the design review) | Probe | Console |
|---|---|---|
| Hold versus click on BtnA/B/C: how long people press, where the thresholds go | Input lab, button practice | `u2` |
| Glass touches near the bottom edge versus the touch buttons below it; the dead band | Input lab, target practice | `u1` |
| The haptic tick: which length and strength can be felt | Input lab, haptics | `u3` |
| Flick-scrolling a list while audio streams from the SD card on the LCD's SPI bus | Scroll lab (round 2: hardware scroll, boost, gentle refill; the boost was removed after it) | `w0`-`w3`, `wm`, `wp` |
| The library at 2,000-10,000 tracks: build time, sort time, memory | Library index | `g0`, `g<n>` |
| Internal RAM (about 48-50 KB free while playing) | every probe logs it | |
| Names that aren't ASCII ("Can’T", "T‐Pain", "Pénélope"): fonts, speed, flash | Font probe | `e`, `e1`-`e5` |
| Album covers as list thumbnails: decode time, decoder RAM, a cache | Thumbnail probe | `j`, `jw`, `ja` |

Each command is a letter, an optional argument and **Enter** (the console
collects the argument up to Enter). `e`, not `f`, is the font probe: `f`
forgets the headphones and restarts. A spike screen owns the display while it
is up: the now-playing and dance screens stop drawing, and come back when it
closes (the same command again, or its `q`). The input lab also takes the
three buttons (they are logged, not acted on); in the scroll lab they keep
working as transport.

Everything logs to the serial console (115200 baud). The scratchpad's serial
daemon (`sdaemon.py` / `dc.sh`) keeps COM3 open with DTR and RTS low, so the
Core2 isn't reset. `X` takes a whole-screen screenshot of any of these screens
(`dshot.py`).

## What was built

Portable, in `lib/core/`, host-tested (`pio test -e native`: 27 new cases, 311
in all):

- **`LibraryIndex`**: the future single store of the library (below).
- **`LibrarySynth`**: a deterministic made-up library of any size.
- **`TextFold`**: UTF-8 to ASCII folding for the 7-bit GFX fonts, the library's
  sort order (case- and accent-insensitive) and the A-Z rail's keys.
- **`TouchGesture`**: tap / hold / drag / flick from a touch's samples, with the
  spec's thresholds (12 px slop, 500 ms hold, release velocity).
- **`KineticScroll`**: 1:1 drag, a fling with the spec's friction (0.92 per
  15 fps frame, whatever the real frame rate), always coming to rest on a row.
- **`ScrollGovernor`**: how hard a list may use the bus, from the ring fill.
- **`Percentiles`**: the summaries in the logs.

On the Core2, in `src/`:

- `ui/LcdLock`: a scoped `M5.Display.startWrite()`/`endWrite()` that times how
  long the LCD held the SPI bus (and so the SD card's mutex) per hold. Every
  spike screen draws band by band, each band (a tab bar, a row, 40 rows of a
  clear: `spike::fillLcd`) under its own `LcdLock`, never a whole screen in
  one hold.
- `app/Haptics`: vibration patterns on a FreeRTOS one-shot timer, so a 15 ms
  tick lasts 15 ms whatever the loop does.
- `Core2AudioBackend::bufferedMsNow()`, `underrunsNow()`,
  `decodeBusyUsTotal()`: the ring fill and the decoder's load, live, for the UI.
- `LocalStorage::forEachFile()`: an uncapped walk of `/music` that keeps nothing.
- `spike/`: the labs and probes (`InputLab`, `ScrollLab`, `FontProbe`,
  `ThumbProbe`), the tab bar stand-in and colours (`SpikeUi`), the generated
  VLW fonts (`VlwFonts.cpp`, from `tools/vlw_font.py`; since moved to
  `src/ui/`, the UI's), and `Spike`, which owns
  them. Every lab is created in PSRAM on first use (`psramNew`), and so are
  all their buffers and sprites.

## The probes

### Input lab (`u`)

`u` toggles the lab; `u0`-`u3` open a mode. While it's open the buttons and
the glass are logged, not acted on (a B-hold would switch the output mid-test).

Every button press logs (the log examples in this document show the format; their numbers are made up):

```
[btn] B down t=81234 touch=(161,262) finger down at y=236 40ms before (it was on the glass at y 220-239 first)
[btn] B m5 hold at 500 ms
[btn] B up t=81790 dur=556ms m5=hold (hold thresh 500ms) touch=(161,262) glass>=220=yes (y=236, same finger) asked=hold-B
[btn] B m5 decided 1 click
[btn] C: glass touch at y=238 0 ms after the release (the same finger, rolled up off the strip)
```

`touch` is the raw touch point (converted like M5Unified's own) that pressed
the button; `m5=` is what M5Unified made of it with its current thresholds
(`wasClicked` / `wasHold`); the `decided` line is its click-count decision
(`wasDecideClickCount`, `wasDoubleClicked`), which comes one hold threshold
after the release.

M5Unified makes the buttons out of the same touch points: a point at raw
y >= 240 that isn't moving presses the button of its third of the strip, and
the button lets go in the update where the point leaves the strip. So one
finger can never be on the glass *during* its own press. What the lab looks
for instead (`glass>=220`, a touch at y 220-239 around the press):

- the finger that pressed the button, tracked from its touch-down: it landed
  on the glass (its `finger down at y=`, and how long before the press) and
  rolled onto the strip, passing through y 220-239 (`same finger`);
- the same finger rolling back up onto the glass: the button releases in
  that very update, and the glass sample that follows within 300 ms is added
  to the press (the extra `[btn] C: glass touch ... after the release` line);
- any other touch at y 220-239 during the press or in the 300 ms before it.

Every touch logs its gesture, classified with the spec's rules:

```
[touch] tap zone=glass down=(270,221) up=(271,222) dur=95ms move=1px v=(10,-12)px/s samples=9
```

`zone=button-strip` marks a touch that started below the LCD (y >= 240).

- **`u0` free**: a live readout of the last five events, and a dot where the
  finger is. The line at y 220 marks the band `u2` watches.
- **`u1` tab target practice**: the tab bar from the spec (five 56 px tabs and
  the volume chip in the corner, 36 px tall) over one of three scenes: a list
  whose last row reaches the bottom edge (198-239), the Queue's edit bar
  (Remove 6-134, Play next 140-226, Clear all 232-314, all at 196-239), or an
  "Added 14 tracks" toast with **Undo** (230-309 / 199-235). The header names a
  target ("Tap: Clear all"); rounds of 16 prompts cover each tab and the chip
  once and each bottom-row target twice, shuffled. Each attempt waits 300 ms
  after the release for button events, then logs:

  ```
  [target] #7 Clear all (edit bar): HIT down=(270,221) off=(-3,+4) up=(271,222) tap dur=95ms move=1px hit=Clear all button=none
  [target] #8 Undo (toast undo): MISS down=(265,247) off=(-4,+30) ... hit=the button strip button=C (+12ms from the press)
  [target] #9 Remove (edit bar): MISS, no touch recorded, button A fired (140ms press) at (72,251) off=(+2,+34)
  ```

  `off` is the press point minus the target's centre; `button` is any BtnA/B/C
  down or up within 300 ms of the touch. A tap that lands in the button strip
  (y >= 240) is an attempt too, a miss with its offset, whether it fired a
  button (#8) or moved too much to (no button: it would otherwise be lost and
  the prompt left up); so the y offsets below the LCD's edge, the ones the
  dead band is sized from, are in the percentiles. The rare button with no
  touch record (#9) uses the button's raw point. The target's outline flashes
  green or red for 0.6 s.
- **`u2` button practice**: "Click B", "Hold A (about 1 s)"; a round is the
  three clicks, then the three holds, each in a shuffled order. The result
  shows under the prompt (green when M5Unified agreed).
- **`u3` haptics**: nine tiles: 15, 25 and 40 ms at medium (level 150, 2.2 V on the AXP192's LDO3)
  and strong (235, 3.3 V); the spec's double tick (2 x 20 ms, 80 ms apart), a
  80 ms buzz, and the "inert" double buzz (2 x 40 ms). A tap plays one and logs
  the motor's measured on-time. `uh<ms>[,<level>[,<count>,<gap>]]` plays any
  pattern from the console, in any mode. The level is `setVibration()`'s:
  M5Unified asks LDO3 for 480 + 12 x level mV, which the AXP192 rounds down to
  100 mV steps from 1.8 V and switches off below 1.8 V. So the lowest level
  that powers the motor at all is 110 (1.8 V, `Haptics::kSoft`); `uh` raises
  anything lower to 110 and says so, and the logs give the real LDO3 voltage.

Other commands: `us` prints the summary of everything logged since boot,
`ur` clears it, `ut<ms>` sets M5Unified's hold threshold for BtnA/B/C
(100-3000 ms; it is also the double-click window) to try other values live.

The summary (`us`):

- per button: durations when asked to click and when asked to hold (min,
  p10, p50, p90, p95, max), how many M5Unified read the other way, wrong
  buttons; unprompted clicks and holds; presses with a glass touch at y
  220-239 around them (and how many by the pressing finger itself), presses
  whose finger landed on the glass, and the pressing finger's touch-down y
  (percentiles);
- per target zone (tabs, volume chip, last list row, edit bar, toast Undo):
  hit rate, false-button rate (a button event within 300 ms), attempts that
  landed in the button strip, and the offsets from the centre (x and y
  percentiles, strip landings included); then per target;
- the lab's own screen draws: how many bus holds, and the longest;
- touches: counts by gesture, tap duration, tap and hold movement (to check the
  12 px slop), drag and flick release speeds.

**What it decides**: the click / hold thresholds for A/C (the review suggests
about 350 ms for the volume hold) and B (about 800 ms for the output switch),
the bottom dead band (the grafts propose ignoring y >= 232), whether the edit
bar, the toast Undo and the last row can stay at the bottom edge, and the
haptic tick's length and strength.

### Scroll lab (`w`)

A list screen in the tab bar style: the bar (Library active), a segmented
header (Artists | Albums | Tracks, tap to switch), four 42 px two-line rows
over y 72-239, and the A-Z rail on the right (290-319) for the sorted lists.
It reads the library index (the SD card's, or a synthetic one from `g<n>`).
Only the visible rows are drawn.

Rendering follows the spec's sketch: a row is drawn into an RGB565 row
sprite in PSRAM (320x42, 26.9 KB), then pushed in slices of `wh` rows (14 by
default), each slice under its own `LcdLock` (the bus and the SD card's mutex
held for that slice only), with a `taskYIELD()` between slices. `wc<n>` keeps
n row sprites (1 = the spec's single sprite: every row is redrawn every frame;
up to 6: a row still on screen is only pushed again).

**Degrading when the audio is at risk** (`ScrollGovernor`, host-tested),
from `bufferedMsNow()` (~1,450 ms when the ring is full) and the underrun
count:

| Level | When | Frames |
|---|---|---|
| normal | ring >= 900 ms | up to 15 fps (`wf` changes it) |
| reduced | ring < 900 ms, or an underrun in the last 2 s | 8 fps |
| whole-rows | ring < 500 ms | 4 fps, and the list only moves in whole rows |
| paused | ring < 250 ms | none until the ring recovers |

It gets worse at once and better one level per 2 s, with 200 ms of margin.
With nothing playing the level stays normal, and so it does while the ring
is *meant* to be low: from a track's start until the ring first holds 1 s
(it fills from empty), and while it drains at the end of a file
(`Core2AudioBackend::ringSteady()`). An underrun counts either way. (Before
this gate, every track change sent a flicking list to "paused" and then
"whole-rows" for about 4 s.) The level and the frame rate
show in the tab bar's corner.

Modes:

- `w0`: interactive. Drag, flick, scrub the rail (a tick at each new letter),
  tap the header to switch lists, tap the Library tab to go to the top.
- `w1`: a stress test with the governor on: `wd` seconds (60 by default) of
  flicks at 4,000 px/s, a new one every 0.7 s so the list never stops, changing
  direction every third flick, reversed the moment a flick reaches an end, and
  an A-Z jump every fifth action with a flick straight after it (once the
  jump's frame is out). It needs a list that scrolls at least 2,000 px
  (about 50 rows): on a shorter one (the card's 6 artists and 6 albums) it
  refuses and says so; under 4,000 px (the card's 77 tracks, `wv2`: 3,066
  px) it runs with a note, reversing at the ends more often. Build a
  synthetic library for the artists and albums lists (`g2000`: 120 artists,
  300 albums; `g10000`: 600 and 1,500).
- `w2`: the same with the governor off.
- `w3`: governor off and no frame cap: as fast as the bus goes.

Start a track first (`i<n>`, ideally a FLAC and an MP3 in turn, on Bluetooth
and in silent mode `z`): the stress warns when nothing plays. Each second:

```
[scroll] t=<s> fps=<n> moving=<%> fps_moving=<n> held=<ms> frame=<mean>/<max>ms draw=<ms> push=<ms> rows_drawn=<n> slices=<n> lock=<mean>/<max>ms ring_min=<ms> underruns=+<n> decode=<%> heap_min=<K> level=<level> audio=<steady|filling/draining|stopped> <phase>
```

`fps` counts the frames drawn that second; `moving` is the share of the
second the list wanted frames (in motion, or a frame still owed), and
`fps_moving` the frames per second of that time, so an idle list and a
throttled one read differently; `held` is how much of the moving time the
governor was above normal. `frame` is mean/max ms per frame (draw + push);
`draw` and `push` are per frame; `lock` is the mean/max time the bus was held per slice; `ring_min` the
lowest ring fill that second while it was steady (9999: not steady at all
that second: stopped, filling or draining); `decode` the decoder's share of core 1 (its busy
time, so it also counts waits for the SD card's mutex); `heap_min` the lowest
internal free heap. A stress ends with:

```
[scroll] stress done: w1 60s: moving ...% of the time (p10 ...%), fps while moving p10=... p50=... min=..., frame max ...ms, SPI hold max ...ms, ring min ...ms, underruns N, decode p50=...% max=...%, heap min ...K (ever ...K)
```

The stress calls the scroller's `fling()` itself: it loads the SPI bus and
the loop the way a real flick does, but not the touch panel's I2C reads of a
finger dragging (the FT6336U is only read while touched). `w0` with a real
finger covers that part.

Settings: `wv<0-2>` list, `wf<fps>` frame cap (0 = none), `wh<px>` slice
height (1-42), `wc<n>` row sprites (1-6), `wd<s>` stress length, `wg0`/`wg1`
the governor in `w0`, `ws` shows them and the last stress summary, `wq` closes.

**What it decides**: whether 15 fps flick-scrolling is safe during FLAC and
MP3 playback over Bluetooth; the slice height (how long a single bus hold may
be); one row sprite or a small cache; and whether the governor's thresholds
are needed or too cautious (compare `w1` and `w2`).

### Library index (`g`)

`LibraryIndex` (`lib/core`) is the store the browsing screens will read, and
later the queue: nothing is migrated to it yet (the player still has its
`std::vector<Track>` playlist).

> **Since the spike:** the player now plays from it. The queue and the player
> hold its track ids, the `std::vector<Track>` library and playlist are gone,
> and the index is loaded from a cache on the card when `/music` is
> unchanged ([ARCHITECTURE.md](ARCHITECTURE.md#library-and-queue)). `g`
> reports the app's index (and a synthetic one, if made); `g0` rebuilds the
> app's index, and the queue follows by path; `g<n>` builds the synthetic
> library into an index of the spike's own, which the scroll lab and the
> thumbnail probe use until `g0`, while the player keeps the real one. The
> last line below (today's track list) is no longer printed. What follows
> describes the spike as it was measured.

- **Layout**: one string arena (every name once), fixed records (track 24 B,
  artist 20 B, album 20 B, folder 24 B), and sorted views as arrays of 32-bit
  ids: artists A-Z, albums A-Z, each artist's albums, each album's tracks (so
  also each artist's tracks), each folder's folders and files; A-Z bucket
  starts for the rail. An album's tracks go folder by folder: the album
  folder's own files, then its subfolders A-Z (`CD1`, `CD2`: discs stay
  apart), each by number, then name. Artist and album are the folder names
  (`/music/Artist/Album/NN - Title.ext`), sharing the folders' strings; track
  number and title come from the file name.
- **Memory**: every block comes from an allocator hook (PSRAM in the firmware);
  there is no allocation per string or record (blocks double while building,
  and `finish()` trims them), and queries allocate nothing. The index object is
  itself in PSRAM. The build peak it reports is counted in the hook calls
  themselves: the most held at any moment, a block and its doubled or trimmed
  copy while both exist, and the sort's temporaries included.
- **Build**: `begin()`, `addFile(path)` per file (hash tables dedupe folders,
  artists and albums), `finish()` sorts with the library order and fills the
  views.

At boot the firmware builds it from the card (`[index]` lines, after
`[heap] dance`). Commands: `g` reports it; `g0` rebuilds from the card; `g<n>`
replaces it with a synthetic library of n tracks (6 artists and 15 albums per
100 tracks: 10,000 tracks, 600 artists, 1,500 albums), made by `LibrarySynth`
and fed through the same `addFile()`.

```
[index] SD card /music: <n> files seen, <n> tracks, <n> artists, <n> albums, <n> folders
[index] time: walk ... ms (the file system), add ... ms, finish ... ms (sort + views + trim)
[index] PSRAM: ... B held (... B/track): strings ..., tracks ..., artists ..., albums ..., folders ..., views ...; build peak ... B; PSRAM free fell ... B
[index] internal RAM: free ... B before, +0 B after the build, lowest ... B during; the index object itself is in PSRAM (... B)
[index] today's track list (std::vector<Track>): <n> tracks; internal RAM: `library` measured ... B (counted ... B), the playlist copy measured ... B (counted ... B): ... B/track for both (...)
```

The last line is today's cost for comparison: `main.cpp` measures the free
internal heap around building `library` and around the player's copy
(other tasks allocate meanwhile, Bluetooth at boot, so these deltas are
rough), and `Spike::estimateBytes()` counts the blocks the vectors and their
strings hold (strings longer than libstdc++'s 15-byte in-place buffer, plus
ESP-IDF's 8-byte block header) that are really in internal RAM: a block of
4 KB or more goes to PSRAM (`SPIRAM_MALLOC_ALWAYSINTERNAL`), and with 77
tracks both vectors' own buffers (76 B a `Track`) are there, so only the
strings count. The first version counted the vectors too (16,340 B for
`library` instead of 6,604).

### Font probe (`e`)

The same list rows (title + artist in a 42 px row, drawn into the PSRAM row
sprite) with the five sample titles from the card: "06 Can’T Tell Me
Nothing", "05 Good Life Feat. T‐Pain", "10 Le voyage de Pénélope", "01 La
demme d'argent", "Selected Ambient Works 85-92". Options:

| | Primary / secondary | Non-ASCII |
|---|---|---|
| `e1` | FreeSans 9 pt / Font2 (GFX, 7-bit) | folded to ASCII (’ → ', ‐ → -, é → e) |
| `e2` | FreeSans 12 pt / Font2 | folded |
| `e3` | efontCN_16 / efontCN_12 (M5GFX's smallest Unicode bitmap fonts; their Latin is GB2312's, which the device showed lacks É and ‐: see the results) | drawn as is |
| `e4` | VLW DejaVu Sans 16 px / 13 px, anti-aliased, from flash (ASCII, Latin-1, common Latin Extended-A, General Punctuation: 256 glyphs) | drawn as is |
| `e5` | the spec's rule: punctuation folded; FreeSans 9 when that leaves ASCII, efont 16/12 for that row otherwise | per row |

Every option does a list row's whole job: the text is fitted to the column in
its own font (a width pass, and "..." when it is too long; `e1`/`e2` fold
first, `e3`/`e4` measure the UTF-8 as it is, `e5` folds punctuation, picks the
font, then fits). `e` (or `e0`) measures every option compiled in and leaves
`e1` on screen; `e<n>` measures and shows one option. Per sample: one **cold**
draw, right after 64 KB of PSRAM is read to flush the CPU cache (the ESP32's
32 KB cache holds flash and PSRAM alike; a scrolling list draws each row once,
likely with the font's glyphs evicted), then 20 warm draws (mean and best),
and one push. A tap, `e` or `eq` closes the page.

M5GFX 0.2.30 ships no small Unicode font other than efont (CN, JA, KR, TW; the
CN set, 7,545 glyphs, is the smallest) and the Japanese IPA fonts. Only
`efontCN_16` and `efontCN_12` are referenced, and the linker keeps only those
arrays. The VLW fonts are made by `python tools/vlw_font.py` (Pillow) from
DejaVu Sans; M5GFX keeps their glyph tables in PSRAM.

Compile-time switches, for the flash cost: `UI_SPIKE_EFONT` (options 3 and 5)
and `UI_SPIKE_VLW` (option 4), both 1 by default (`src/spike/VlwFonts.h`).
(Since the UI framework: the fonts are the UI's, `src/ui/VlwFonts.*`, always
in the build, and `UI_SPIKE_EFONT` is 0 by default, in `FontProbe.h`.)

### Thumbnail probe (`j`)

For the first album (albums A-Z) whose folder has a `cover.jpg` (or
`Cover.jpg`, `folder.jpg`, `front.jpg`), or album n with `j<n>`:

1. the file read into PSRAM, 4 KB per `f.read` (size, dimensions, baseline
   or progressive: TJpgDec can't decode progressive JPEGs);
2. decoded with M5GFX's `drawJpg` into 40x40 and 80x80 RGB565 PSRAM sprites,
   both straight from the SD card (the real case) and from the PSRAM copy
   (decode time alone). `drawJpg` decodes at 1/8 or 1/4 scale first, then
   scales the rest;
3. the internal RAM the decoder takes: the free internal heap is sampled at
   every read the decoder makes (its 3,900-byte TJpgDec pool is malloc'd, so
   it lands in internal RAM);
4. the thumbnails copied to a PSRAM cache, and redrawn from it (`pushImage`,
   timed, with the SPI hold);
5. with `jw<n>` also written to the card as raw `.565` files (an 8-byte
   header, then the pixels, big-endian RGB565) in `/uispike/`, read back and
   redrawn: the per-row cost of a card cache. Delete `/uispike` afterwards.

`ja` decodes every album's cover to 40x40 from the card and prints the
percentiles (a background thumbnail pass would take about the total). A tap,
`j` or `jq` closes the page.

The card's side of each step is measured as well. FatFs holds the volume for
a whole `f.read`, and the SD driver holds the SPI bus (the LCD's mutex) for a
whole multi-sector read, so a cover read in one call would lock the decoder
out for 100 ms or more. Every card access here is at most 4 KB per call
(`ThumbProbe::kChunk`; the decoder's own reads are smaller), each call is
timed, and the ring fill and underruns are watched across each step:

```
[thumb] SD side, decoding from the card (40 and 80): 94 card calls (<= 4096 B each), longest 3.1 ms; ring min 1390 ms, underruns +0
```

## Build and host results (September 2026)

Measured with this code on the build host; no device results yet.

**Host** (`pio test -e native`, the laptop): all 311 cases pass. The
synthetic 10,000-track library (600 artists, 1,500 albums) builds in
about 25-30 ms and finishes (sorts and views) in about 20 ms on the laptop,
and holds 790,522 bytes, 79.1 B a track: strings 355,294, tracks 240,000,
artists 12,000, albums 30,000, folders 50,424, views 102,804. The build's
peak is 1,320,818 bytes: the allocator's own high-water mark (the host test
checks the two agree), with doubling slack, hash tables, a block and its
copy while both exist, and the sort's temporaries.

**Firmware** (`pio run -e core2`, pioarduino 55.03.312-1, the same libraries).
Five builds were measured: HEAD before the spike (743a13c, `git archive`, in a
scratch folder), and the spike with both font switches on (the default), with
`-DUI_SPIKE_EFONT=0`, with `-DUI_SPIKE_VLW=0`, and with both at 0 (the
variants through `PLATFORMIO_BUILD_FLAGS`, each in its own build folder).
Flash is `.flash.text` + `.flash.rodata` + `.eh_frame`.

| Build | IRAM (vectors + text) | Internal `.dram0.data` + `.bss` | Flash |
|---|---|---|---|
| HEAD 743a13c | 124,035 B | 51,056 B | 1,549,596 B |
| spike, fonts off (`EFONT=0 VLW=0`) | 124,035 B | 51,384 B | 1,643,240 B (+93,644) |
| spike, efont off (`EFONT=0`) | 124,035 B | 51,384 B | 1,710,636 B |
| spike, VLW off (`VLW=0`) | 124,035 B | 51,384 B | 2,176,664 B |
| spike, default (both on) | 124,035 B | 51,384 B | 2,243,940 B (+694,344) |
| spike, default, after the review fixes | 124,035 B | 51,384 B | 2,250,696 B (+701,100) |
| spike, default, after the measurement stage's fixes (the one on the device) | 124,035 B | 51,392 B | 2,251,508 B (+701,912) |

- **IRAM: unchanged**, the same 124,035 B. A first build had 48 B more:
  `Haptics` looked itself up with `pvTimerGetTimerID()`, which ESP-IDF places
  in IRAM; it now keeps a static pointer instead.
- **Internal static RAM: +328 B** (the `Spike` and `Haptics` objects and
  `LcdLock`'s totals). Everything else the spike uses is allocated in PSRAM
  when a tool is first used.
- **Flash per font option**:
  - (a) FreeSans 9 pt: 2,314 B (glyph table 1,140 + bitmaps 1,150 + 24);
    12 pt: 3,133 B (1,140 + 1,969 + 24). Neither was linked before (the old
    screens use Font2 and Font4).
  - (b) efontCN_16 + efontCN_12: **+533,304 B** (the build with
    `UI_SPIKE_EFONT=0` against the default): the two glyph arrays are
    318,199 + 213,444 B, the U8g2 renderer the rest. efontCN_10 (158 KB) is
    the smallest M5GFX font with Latin-1 and punctuation; CN is the smallest of
    the four efont sets (KR 12 px 226 KB, TW 386 KB, JA 308 KB).
  - (c) VLW DejaVu Sans 16 + 13 px (256 glyphs each): **+67,276 B**; the
    arrays are 36,985 + 26,461 B, the VLW renderer the rest.
- The rest of the spike (the labs and probes, LibraryIndex, TextFold, the
  other GFX fonts the screens use: FreeSans Bold 9/12/18, and TJpgDec,
  3,007 B) is the +93,644 B of the fonts-off build.
- The app partition is 4 MB: the default build uses 2.25 MB of it.
- The review fixes (banded screen draws, chunked card reads, the input lab's
  finger tracking, the font probe's fit and cold draw) added 6,756 B of flash
  and nothing to IRAM or internal static RAM; the three variant builds were
  not rebuilt after them, so their deltas are from before (the font costs
  don't change).


## Device results (26 September 2026)

Measured on the Core2 v1.3 (COM3), in silent test mode (`z`) throughout:
the speaker at volume 0 and Bluetooth kept off the output. Audio came from
the SD card: a FLAC (index 70, Kavinsky, 44.1 kHz) and an MP3 (index 26,
Daft Punk). **Bluetooth output was not measured**: silent mode keeps the
audio on the speaker, and it stays on for every test while the user may
be wearing the headphones. The final firmware is the last row of the build
table above. The w2, w3 and slice-height runs ran on an intermediate build.
Its rendering is the same; it lacks the governor's steady gate, and that
gate made no difference because the ring never dipped in those runs. The
serial logs are in the session scratchpad (`f_w1_*.log`, `w*_mp3_10k*.log`,
`font_idle.log`, `thumb_*.log`, `run*.log`).

Fixed during this stage:

- The stress tests stopped in the same pass they started. `open()` stamps
  its start with `millis()`, but the main loop's `now` had been read before
  the console ran, so `now - start` wrapped. `ScrollLab::loop` now clamps
  `now` to its last pass.
- The w1-w3 threshold was lowered so the card's 77 tracks can be stressed
  (see the scroll lab section).
- The governor's false alarm at every track change: it now asks
  `ringSteady()`.
- `estimateBytes()` counted the vectors' own buffers as internal RAM, but
  they are in PSRAM.
- The stress summary was cut off (its buffer was 200 B).
- Two captions ran off the screen.

### Internal RAM (free heap, `[heap]` and `[stats]`)

| When | Free | Lowest so far |
|---|---|---|
| boot (after `M5.begin`) | 195 KB | 194 KB |
| after the SD scan (`library`) | 187 KB | 181 KB |
| after `audio.begin` (Bluetooth up) | 89 KB (largest block 79 KB) | 88 KB |
| after the dance tracker | 76 KB | 74 KB |
| after the library index is built (end of boot) | 76 KB | 71 KB |
| idle, nothing playing, no lab opened yet | 77 KB | 71 KB |
| FLAC playing | 60-61 KB | |
| MP3 playing | 57-58 KB | |
| MP3 playing or paused, after every lab and probe was used | 57 KB | |
| the lowest seen all session | | **46 KB**: a cover decoded from the SD card during MP3 playback |

The spike's own internal cost is small. The labs are created in PSRAM,
and the free heap while playing moved about 1 KB between before and after
the labs were used. The transient costs were:

- the JPEG decode from the card, about 8.6 KB;
- the card walk of `g0`, about 1 KB (File objects);
- Bluetooth's reconnect attempts, which move the figure by ±3 KB every
  few seconds.

Playback itself costs 16-20 KB (the decoder and its buffers).

### Library index

| Library | Build: walk / add | Finish (sort + views + trim) | PSRAM held | B/track | Build peak | Internal RAM change |
|---|---|---|---|---|---|---|
| SD card, 77 tracks (83 files, 13 folders) | walk 466-487 ms (975 ms during MP3 playback) / add 8.5-9.7 ms | 1.3-1.5 ms | 5,190 B | 67.4 | 24,284 B | +0 B (lowest -600 B during the walk) |
| synthetic 2,000 (120 artists, 300 albums) | add 206 ms idle, 454 ms during MP3 (paths generated included) | 78 ms idle, 156 ms during MP3 | 158,513 B | 79.3 | 255,977 B | +0 B |
| synthetic 10,000 (600 artists, 1,500 albums) | add 1,113-1,277 ms idle, 2,008 ms during MP3 | 608-613 ms idle, 976 ms during MP3 | 790,522 B | 79.1 | 1,320,818 B (PSRAM free fell 800,024 B) | +0 B (one run read -3,252 B, which was Bluetooth reconnecting; repeated: +0) |
| today's track list, 77 tracks (for comparison) | | | internal: `library` 6,604 B, the playlist copy 6,696 B (the blocks counted where they really are; measured deltas 7,812 and 11,072 B) | 86 per copy, 173 for both | | |

The index's sizes on the device match the host exactly: 790,522 B and a
1,320,818 B peak for 10,000 tracks. It holds nothing in internal RAM. The
index object itself is 428 B, in PSRAM.

The CPU work is not the cost. Sorting and building the views for 10,000
tracks takes 0.6 s (1 s while an MP3 plays). **The file system walk is the
cost**: 83 files and 13 folders take 470-490 ms, about 5.7 ms per file.
At that rate a 10,000-file card would take about a minute to walk. That is
an extrapolation; there is no big card to test on.

Today's `std::vector<Track>` works differently. The vectors' own buffers
are 4 KB or more, so they go to PSRAM, but every string longer than 15 bytes
is a separate internal-RAM block: about 86 B per track, for each of the two
copies. At 2,000 tracks that would be about 350 KB of internal RAM, which is
impossible. So the queue and playlist must move to the index before the
card holds more than a few hundred tracks.

### Scroll lab

`w1` as the doc describes it: full-speed flicks (4,000 px/s) with the
governor on, 1 row sprite, 14 px slices, a 15 fps cap, and 60 s unless
noted. The artists list was used for the synthetic libraries and the
tracks list (`wv2`) for the card. `fps` is frames per second while moving
(the list moved 100% of the time in every run). The frame, draw and push
columns are per frame, as p50 mean / max. `hold` is how long the SPI bus
was held per slice, as p50 mean / max.

| Library | Audio | fps p10 / p50 | Frame ms | Draw ms | Push ms | Hold ms | Ring min | Underruns | Decode p50 / max | Heap min |
|---|---|---|---|---|---|---|---|---|---|---|
| 10,000 | none (`w1`, 30 s) | 14.7 / 14.8 | 35.6 / 39.3 | 13.6 | 20.7 | 1.55 / 2.9 | n/a | 0 | 0 | 76.2 K |
| 10,000 | none (`w3`, uncapped, 30 s) | 23.5 / 23.8 | 35.4 / 40.2 | 13.7 | 20.7 | 1.56 / 2.9 | n/a | 0 | 0 | 73.2 K |
| 10,000 | FLAC | 11.4 / 11.8 | 56.7 / 79.9 | 24.0 | 28.8 | 2.16 / 32.8 | 1,439 ms | 0 | 31 / 35% | 58.4 K |
| 10,000 | MP3 | 10.3 / 10.7 | 80.6 / 104.9 | 35.8 | 40.7 | 3.03 / 23.8 | 1,439 ms | 0 | 40 / 44% | 55.7 K |
| 2,000 | FLAC | 11.5 / 11.9 | 54.9 / 81.6 | 23.0 | 29.9 | 2.23 / 32.2 | 1,439 ms | 0 | 32 / 35% | 61.3 K |
| 2,000 | MP3 | 9.8 / 10.7 | 81.1 / 107.9 | 36.5 | 40.1 | 3.01 / 23.9 | 1,439 ms | 0 | 41 / 45% | 55.7 K |
| card, 77 tracks | FLAC | 10.8 / 11.8 | 59.9 / 116.7 | 24.2 | 33.8 | 2.59 / 32.3 | 1,439 ms | 0 | 32 / 37% | 57.7 K |
| card, 77 tracks | MP3 | 9.5 / 10.3 | 85.6 / 118.4 | 36.7 | 46.2 | 3.56 / 23.9 | 1,439 ms | 0 | 40 / 44% | 55.2 K |
| 10,000, `w2` (no governor) | MP3 | 9.7 / 10.6 | 81.0 / 103.6 | 37.0 | 40.0 | 2.94 / 24.1 | 1,439 ms | 0 | 40 / 44% | 55.8 K |
| 10,000, `w3` (no governor, no cap) | MP3 | 10.3 / 10.7 | 80.5 / 106.3 | 36.2 | 40.1 | 2.99 / 24.1 | 1,439 ms | 0 | 40 / 44% | 55.7 K |
| 10,000, `wh42` (whole-row slices, 30 s) | MP3 | 9.8 / 10.6 | 81.8 / 105.6 | 38.2 | 41.2 | 7.33 / 27.6 | 1,439 ms | 0 | 41 / 45% | 55.5 K |
| 10,000, `wh7` (7 px slices, 30 s) | MP3 | 10.1 / 10.4 | 84.1 / 108.3 | 38.6 | 38.9 | 1.56 / 22.5 | 1,439 ms | 0 | 41 / 44% | 55.7 K |
| 10,000 tracks list, `wc6`, two track skips (30 s) | MP3 | 8.2 / 9.3 | 95.4 / 820 | 48.4 | 43.7 | 3.36 / 34.9 | 1,439 ms | 0 | 38 / 65% | 58.4 K |

In about 14 minutes of stress over all these runs there were **no underruns**.
The steady ring never went below 1,346 ms of the ~1,460 ms full: one second
at a track start read 1,346 ms, and every other second read 1,439 ms. The
governor never had to act: every run stayed at `normal`, and with it off
(`w2`) or uncapped (`w3`) the ring stayed just as full. The decoder runs at
priority 2 on core 1, the UI loop at priority 1, so the audio takes the CPU
it needs and the list gets the rest.

What that costs the list:

- **Frame rate while audio plays: 10-12 fps, not 15.** The limit is CPU,
  not the bus. With no audio, a frame is 35 ms (draw 13.6 + push 20.7) and
  the cap holds 15 fps (24 fps uncapped). During playback the same frame
  takes 55-60 ms with FLAC and 80-86 ms with MP3, because the decoder
  preempts the loop on core 1. Even uncapped (`w3`) the rate stays at 10.7
  fps with MP3 playing.
- **The push is bus-bound.** A full list viewport (290-320 x 168 px) takes
  about 20.7 ms at 40 MHz, close to the 21.5 ms that 107 KB of RGB565 takes
  on the wire. So a full redraw can never run faster than about 45 fps,
  whatever the CPU does.
- **Drawing a row** takes about 2.8 ms in the artists list with no audio,
  and 5-7 ms during playback. Each frame of a flick draws 4-5 rows.
- **A row cache (`wc6`) doesn't help flicks.** At 4,000 px/s every frame
  shows new rows, so every row is drawn anyway. It can only help slow
  drags; test that in `w0`.
- **Slice height** changes the length of each hold, not the frame rate.
  - Mean hold: 1.56 ms at 7 px slices, 2.2-3.0 ms at 14 px, 7.3 ms at
    42 px.
  - Frame rate: about 10.5 fps with MP3 playing in all three.
- **The hold maximum** runs 15-33 ms with audio playing, against 2.9 ms
  without, so the slice size alone does not bound it.
  - Cause: the decode task preempts the loop in the middle of a slice
    while the loop holds the bus.
  - Why it is harmless: it is the decoder's own time. If the decoder then
    needs the card, it blocks on the mutex. Priority inheritance lifts the
    loop, which finishes its slice (about 1.5 ms) and releases the bus.
    The ring figures confirm it.
- **A track start or skip stalls the UI for 0.6-0.8 s.**
  - What was measured: one frame of 600 ms, 750 ms and 820 ms after
    `i70`, `i26` and `n`, and 3-4 fps in that second.
  - Cause: the decoder opens the file and then refills the whole 1.4 s
    ring as fast as it can, passing only `vTaskDelay(1)` between passes.
    At priority 2 that starves the loop for most of the refill.
  - The UI can't fix this on its own side; see the recommendations.
- **The governor used to give a false alarm at every track change.**
  - What happened: at the end of a file the ring drains to 0 on purpose,
    then refills. The governor read that as danger and took the list to
    `paused` and `whole-rows` for about 4 s (seen in a `wc6` run that
    crossed the end of track 26).
  - The fix: it now only acts while `ringSteady()` says the ring matters.
  - Checked: a run with two skips (the last row) stayed `normal`
    throughout.

### Fonts

The five sample rows, drawn into the 320x42 PSRAM row sprite. Each figure is
the mean over the five rows, in ms per row. "Cold" is one draw after a 64 KB
cache flush; "warm" is the mean of 20 draws. The push column is the
full-row push (26.9 KB). The idle columns were measured with nothing
playing, the last column during MP3 playback.

| Option | Draw cold / warm, idle | Push, idle | Flash | Draw cold / warm, during MP3 |
|---|---|---|---|---|
| `e1` FreeSans 9, folded | 3.40 / 2.27 | 5.72 | 2,314 B | 10.6 / 5.3 |
| `e2` FreeSans 12, folded | 3.85 / 2.66 | 5.71 | 3,133 B | 9.4 / 6.3 |
| `e3` efont 16/12 | 13.18 / 12.16 (2.0-3.6 per row, except 53 ms for the row with a missing glyph) | 5.77 | +533,304 B | 23.3 / 25.2 |
| `e4` VLW DejaVu 16/13 (anti-aliased) | 4.66 / 3.40 | 5.73 | +67,276 B (and 4,840 B of PSRAM for the glyph tables, loaded in 2.15 ms) | 12.5 / 8.0 |
| `e5` the spec's rule | 3.40 / 2.21 | 5.74 | e1 + e3 | 7.6 / 5.0 |

What the screenshots show at 1x (`uispike_shots/font_e1.png` to
`font_e5.png`, and `zoom_e3.png` / `zoom_e4.png` at 3x):

- **e1 (FreeSans 9 pt)** is clean and easy to read. All five titles fit.
  Folding loses the accents ("Penelope", "Emilie Simon, Vegetal"), but ’
  and ‐ read naturally as ' and -.
- **e2 (FreeSans 12 pt)** is larger and still readable, but "Selected
  Ambient Works 8..." is truncated. At 12 pt a 42 px row fits about 22
  characters.
- **e3 (efont)** fails. U+2010 (the hyphen in "T‐Pain") is missing from
  efontCN_16 and U+00C9 (É in "Émilie") from efontCN_12: both draw as
  tofu boxes. efontCN's Latin coverage is GB2312's (é but no É, no ‐), not
  Latin-1. Each missing glyph costs about 50 ms of lookup, which is the
  53 ms row. It is also a monospaced pixel face that looks out of place
  next to FreeSans.
- **e4 (VLW DejaVu Sans)** looks best by a clear margin. The anti-aliased
  text is smooth at 16 px, and every sample character is right: ’, ‐, é,
  É.
- **e5 (the spec's rule)** mixes faces in one list. The row with é
  switches to efont, drawn smaller and monospaced, and it shows the 12 px
  É tofu.

### Thumbnails

The cover was `/music/Daft Punk/Discovery/cover.jpg`: 47,861 B, 650x565,
baseline. The times are from the first `j` run each time (ms).

| | Idle | During FLAC playback | During MP3 playback |
|---|---|---|---|
| read the file into PSRAM (4 KB reads) | 39 ms (1.2 MB/s) | 46 ms | 83 ms |
| decode from SD to 40x40 / 80x80 | 166 / 169 | 265-271 / 272-279 | 290 / 300 |
| decode from PSRAM to 40x40 / 80x80 | 125 / 128 | 193-197 | 218 / 219 |
| decoder's internal RAM (peak) | 8,648 B from SD; 3,972 B from PSRAM (TJpgDec's pool is 3,900 B; the rest is the card read path) | 8,660 B | 8,656 B |
| SD side: longest card call, ring min, underruns | 3.4 ms, 1,461 ms (nothing playing), 0 | 26-27 ms (waiting for the decoder's own reads), 1,439 ms, 0 | 12.6 ms, 1,439 ms, 0 |
| redraw from the PSRAM cache, 40x40 / 80x80 | 0.89 / 2.71 (SPI held at most 2.7 ms) | 0.96 / 3.00 | 1.25 / 2.76 |
| redraw from a `.565` file on SD, 40x40 / 80x80 (`jw`) | 5.8 (4.9 to read back + 0.9 to push) / 14.5 (11.6 + 2.9); write 19.5 / 22.5 | | |
| all covers to 40x40 (`ja`), p50 / p90 / max, total | 98 / 216 / 250 ms, 0.7 s | 173 / 347 / 394 ms, 1.1 s | 183 / 389 / 445 ms, 1.2 s |

`ja` found 6 covers: 5 decoded and 1 was **progressive**, which TJpgDec
can't decode, so it was skipped. The decode time barely depends on the
target size, because `drawJpg` entropy-decodes the whole JPEG even when it
scales by 1/8. No run had an underrun or moved the ring.

### Recommendations for the real UI

**Scroll technique.** Keep the spec's approach as measured here:

- one PSRAM row sprite, drawn row by row;
- pushed in 14 px slices (7 px if a lower mean hold is wanted; it costs
  no frame rate), each under its own `LcdLock`;
- a yield between slices;
- the governor kept as a safety net, with the steady gate.

It is safe for the audio: no underruns, and the ring stays full at any frame
rate. Its limit is the frame rate:

- about 15 fps with nothing playing (24 uncapped);
- about 11-12 fps while a FLAC plays;
- about 10-11 fps while an MP3 plays;
- a 0.6-0.8 s stall at every track start or skip.

Whether 10-12 fps feels acceptable under a finger is for the `w0` session
with the user. If it doesn't, these are the options, best first:

1. **Throttle the decoder's refill burst** once the ring holds about 500
   ms: a longer `vTaskDelay` per pass while filling. This removes the
   track-start stall. It is an audio-path change and needs its own soak.
2. **Move the list's drawing to its own task on core 0.** This must be
   measured with Bluetooth output on, because the A2DP stack runs on
   core 0.
3. **Try the ILI9342C's hardware vertical scrolling** (VSCRSADD, with the
   bar and header as the fixed top area). A slow drag would then push only
   the newly exposed lines instead of the whole 21 ms viewport. It is not
   tried here; it would be a small spike of its own.
4. **A row cache (`wc`)** only pays off for slow drags. Leave it at 1
   unless `w0` shows it helps.

**Before any of it: repeat the stress with Bluetooth output** (a session
where the user takes the headphones off). Bluetooth moves the sink onto
core 0 and adds the SBC encoder's load. That is the one unmeasured risk
that could change the verdict.

**Font: VLW (anti-aliased, from flash)**, drawn from UTF-8 as it is.

- Use DejaVu Sans 16/13 as generated, or regenerate another face with
  `tools/vlw_font.py` for the product's look.
- Cost against FreeSans 9: +67 KB of flash, about +1.1 ms per row idle
  (+2.7 during playback), and 4.8 KB of PSRAM.
- Keep `TextFold` for sorting and the A-Z rail's keys.
- Keep the ASCII folding as the fallback for a character missing from the
  VLW set. Check what M5GFX's VLW renderer draws for a missing glyph first.
- Drop efont entirely: its glyphs are missing (tofu), it costs 533 KB of
  flash, and each miss takes 50 ms.
- FreeSans 9 stays as the ASCII-only fallback. FreeSans 12 truncates too
  much for a 42 px two-line row.
- The Bitstream Vera licence text must be added to the repo before a VLW
  DejaVu build is distributed.

**Thumbnails.** The device should not decode JPEG covers while browsing.

- A decode takes 125-300 ms and 8.6 KB of internal RAM.
- Progressive files fail.
- A 40x40 thumbnail needs the whole cover entropy-decoded.

Best: the sync generates the thumbnails. mStream (or the sync step) writes
40x40 and 80x80 RGB565 thumbnails into one packed file per library, indexed
by album id. The device reads one thumbnail in a single 3.2 KB read, with
no file open per thumbnail. That costs at most the 4.9 ms seen for a
`.565` read-back, and most of that was the FAT open.

Pending that, decode on the device:

- from a background task below the loop's priority, never during a
  scroll;
- into a PSRAM LRU cache: 40x40 is 3.2 KB, so 150 albums fit in about
  480 KB, while all of 1,500 albums would be 4.8 KB x 1,500, too much for
  PSRAM;
- persisted as `.565` files, which saves the next boot the decode.

A redraw from PSRAM is about 1 ms (40x40) or 3 ms (80x80), cheap enough for
a list row.

**Index: adopt `LibraryIndex` as designed.**

- Memory: 79 B per track in PSRAM, 0 in internal RAM; 790 KB for 10,000
  tracks.
- Build: a 1.3 MB peak, 0.6 s of sorting.
- Two changes for the real build:
  1. **Don't walk the card at every boot.** Build the index from the sync
     manifest, or save it to the card after a build (strings, records and
     views in one file). Loading it back is a sequential read of about
     0.8 MB, roughly 0.7 s at the measured 1.2 MB/s. Walking the card
     costs about 5.7 ms per file, which is a minute at 10,000 files, and it
     already adds about 0.5 s to boot today.
  2. **Pre-size the blocks** from a count to cut the build peak (1.3 MB
     for a 0.8 MB result).
- Migrate the player's `std::vector<Track>` next (the planned next stage).
  Today's list costs about 86 B of internal RAM per track, per copy.

**Internal-RAM budget for the real UI.**

| State | Free | Notes |
|---|---|---|
| boot, idle | 76-77 KB | lowest 71 KB during boot |
| playing | 55-61 KB | FLAC 60-61 KB, MP3 57-58 KB; minimums 52-58 KB under a stress |
| worst seen | 46 KB | a JPEG decode from the card during MP3 playback |

Rules for the real build:

- Keep at least 40 KB free at all times.
- Put every UI buffer, sprite, cache and table in PSRAM. Use
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)` for anything under 4 KB, and call
  `setPsram(true)` before `createSprite`.
- Treat these as the known transients: a JPEG decode is 8.6 KB, a card
  walk about 1 KB, and Bluetooth reconnects ±3 KB.
- Once the playlist is on the index, the 13 KB the track list holds today
  (77 tracks, both copies) comes back.

### Input (with the user)

> **Since:** the user's session gave the button timings, the touch error
> and the haptic choice; what was built from them is in "After the spike:
> the input layer" at the end. The tables below were left blank.

The automated stage took screenshots only (`uispike_shots/input_u0.png`,
`input_u1_*.png`, `input_u2.png`, `input_u3.png`, and `sheet_labs.png`
with the scroll lab's three lists). All of them render as designed.

| | A | B | C |
|---|---|---|---|
| click duration p50 / p95 / max | | | |
| hold duration (asked) min / p10 / p50 | | | |
| clicks read as holds at 500 ms | | | |
| presses with a glass touch at y 220-239 around them (same finger) | | | |
| pressing finger's touch-down y p10 / p50 | | | |

| Zone | Hit rate | Offset y p50 (p10-p90) | False-button rate | Landed in the strip |
|---|---|---|---|---|
| tabs | | | | |
| volume chip | | | | |
| last list row | | | | |
| edit bar | | | | |
| toast Undo | | | | |

Haptics: the tick the user preferred, and whether 15 ms can be felt at all.
Scroll feel: whether `w0` at 10-12 fps during playback feels acceptable, and
whether `wc6` helps slow drags.

## Scroll round 2: hardware scroll, interaction boost, gentle refill

**Status: measured on the device (26 September 2026), silent, speaker
path; the user's visual check and a Bluetooth run are still to do.** The
sections up to "Build and host results" are the design and its estimates,
written before the device runs. The results and the recommendation are in
"On the device" at the end. In short: the hardware scroll delivers for
drags (13.9 fps instead of 10.8 with an MP3 playing, 40 uncapped), the
gentle refill halves the track-start stall, and the interaction boost
makes things worse and should be dropped.

### Why

In a session with the user on the device (Bluetooth output, an MP3 playing,
`w0` with a finger), scrolling felt "not great":

- fps while moving: p50 12.6;
- frame p50 76.7 ms (draw 37 ms, push 33 ms);
- the audio was perfect: ring minimum 1,409 ms of about 1,450, no underruns,
  decode 35-43 %.

So the decoder has plenty of slack, and the list is short of CPU and bus
time. Round 2 adds three ways to give it more, each a scroll lab option so
it can be A/B measured against the spike-1 path:

1. hardware vertical scroll (`wm1`);
2. an interaction boost (`wb1`, or `wm2` with the hardware scroll);
3. a gentler ring refill at track starts (`wp1`).

A correction to the brief: the lab's list band is **168 lines** (y 72-239),
under the 36 px tab bar and the 36 px segmented header, not 204. So the
fixed top area here is 72 lines (the bar and the header). A 204-line list
(the bar alone fixed) works the same way; the host tests cover both shapes.

### Can the ILI9342C scroll the list vertically in our orientation? Yes

The evidence, from the ILI9342C datasheet (ILITEK, v1.00, 235 pages; the
copy M5Stack hosts), the ILI9342E datasheet (the later Core2 panel, which
M5GFX 0.2.30 detects) and M5GFX 0.2.30's source:

- **The controller's scroll axis is its 240 frame-memory lines.**
  - VSCRDEF (33h, §8.2.26) takes TFA, VSA and BFA "in No. of lines of the
    Frame Memory". Its reset default is 0 / 240 / 0.
  - §9.2.2 states that scrolling is undefined unless TFA + VSA + BFA = 240.
  - VSCRSADD (37h, §8.2.30) takes the frame-memory line shown right after
    the top fixed area. A new value takes effect at the next panel scan.
  - The ILI9342E lists the same two commands, with 9-bit parameters.
- **The frame memory is 320 x 240, and those 240 lines are the page (row)
  addresses when MADCTL's MV bit is 0.** The reset defaults are:
  - CASET (2Ah): EC = 013Fh (320 columns) with MV = 0;
  - PASET (2Bh): EP = 00EFh (240 pages) with MV = 0.

  The out-of-range notes in those two sections contradict the defaults.
  They are left over from the ILI9341 datasheet, a portrait panel, and
  can be ignored.
- **M5GFX drives the Core2's panel with MV = 0.**
  - `Panel_ILI9342` sets memory and panel to 320 x 240.
  - `Panel_M5StackCore2_T` sets `offset_rotation = 3` and a default
    `_rotation = 1`. The firmware never calls `setRotation`.
  - `Panel_LCD::setRotation` computes the internal rotation:
    (1 + 3) & 3 = 0, with no flip bit.
  - `getMadCtl(0)` is 0. MADCTL is therefore only the BGR bit: no MV, no
    MX, no MY, no ML. `_colstart` and `_rowstart` are 0.
- So screen row y is page y, which is frame-memory line y, top first. ML = 0
  means TFA counts from the top.
- **The controller's scroll axis is the screen's vertical.**
- **M5GFX has no hardware scroll.** Its `scroll()` and `copyRect()` copy
  GRAM through reads and writes. It never sends 33h or 37h, and it caches
  only CASET and PASET, which these commands don't touch. Raw commands
  through `writeCommand` / `writeData16` inside a `startWrite()` are safe.

The one orientation risk is a rotation change, for example 3 (upside down,
which sets MY and ML). That would reverse the line mapping and make TFA count
from the bottom. `ListScroller::begin()` refuses anything but the Core2 in
rotation 1, and says so.

### What was built

Portable, in `lib/core/`, host-tested (`test/test_ui_scroll2`, 13 new
cases, 324 in all):

- **`VScrollMap`**: the bookkeeping of the hardware scroll.
  - Content line c lives at GRAM line top + ((c - base) mod height).
  - The start address is top + ((offset - base) mod height).
  - So screen line top + k always shows content line offset + k.
  - `plan(offset)` returns the newly exposed content lines as at most two
    GRAM spans, split at the wrap, for a move of up to `maxStep` lines.
  - A bigger move, or an `invalidate()`, is a **full redraw in place**: it
    re-picks `base` so the start address stays what the panel already
    has. The new content is then written straight into the screen lines
    where it will show, top to bottom, like spike 1's redraw, and never
    shows wrapped or shifted while it is drawn.
  - It also maps lines both ways, from screen to GRAM and back.
  - The tests check against a fake panel that applies the datasheet's
    rule. A 3,000-step random walk of drags, flick frames, jumps and
    invalidates (with the default `maxStep` and the lab's 84) must always
    show the right content, and draw exactly |d| lines per incremental
    step and the whole band otherwise. Another test checks, line by line,
    that a full redraw lands in place under the panel's current address.
- **`UiBoost`**: the boost rule, a Schmitt trigger on the ring (below).
- **`RefillPacer`**: the gentle refill's sleep per pass (below).

On the Core2:

- **`src/ui/ListScroller`**: the reusable part for the real UI.
  - `begin(top, height)` sends VSCRDEF, then VSCRSADD = top, so nothing
    moves yet.
  - `begin(top, height, stats, maxStep)`: the lab uses a `maxStep` of 84
    lines (2 rows), which bounds a move's single bus hold (below).
  - `scrollTo(offset, painter)` calls back a `ListScroller::Painter`:
    - for a move of up to `maxStep` lines: `prepare()` (render the row
      sprites of the new lines) and `prepareFixed()` (render the rail),
      both off the bus; then, in **one bus hold**, `push()` the new lines,
      send the new start address (2 bytes), and `pushFixed()` the rail
      back to its place;
    - for a full redraw in place: `push()` in slices under their own
      locks, with yields, as spike 1 did; the address doesn't change.
  - `pushAtScreen(sprite, x, y)` puts something that must not move (the
    rail, an overlay, a toast) where the panel currently shows that screen
    position. Its lines go to the GRAM lines on screen there, in at most
    two pieces.
  - `gramLineForScreen(y)` is a static helper for screenshots.
  - `end()` sets VSCRSADD back to the top, then sends Normal Display Mode
    On (13h), which leaves scroll mode (§9.2.2).
  - Why a move is one hold: the GRAM lines the new lines go into are the
    ones leaving at the far edge, and they are **still on screen** until
    the address changes. Pushing, sending and putting the rail back back
    to back limits that wrong strip, and the rail's displacement, to the
    bus time (about 0.125 ms a 290 px line, plus ~2 ms for the rail), or
    a little more if the decoder (priority 2) preempts the loop in the
    middle; with the boost on it can't.
- **The scroll lab's hardware path** (`wm1`):
  - Rows are drawn into the same PSRAM row sprites, but only the newly
    exposed lines of them are pushed: whole, in the move's one hold, or in
    `wh` slices on a full redraw.
  - It uses all 6 row sprites (`wc` is ignored): a move renders every row
    its new lines come from (3 at most with the 84-line step) before it
    pushes any, and a row that is half-exposed stays cached for the next
    frame.
  - The A-Z rail is inside the scrolled band, so the panel moves it with
    the list. It is redrawn into a 30 x 168 PSRAM sprite before the hold
    and put back with `pushAtScreen` inside it, at every move.
  - The tab bar, the header and the fps chip are in the fixed area and are
    drawn as before.
  - On close (and on `w`, `wq`, or another spike screen), the band is
    cleared first, so the GRAM is uniform and nothing shows rotated, and
    then `end()`. Every later screen draws at the identity, as before.
- **Screenshots** (`X`, `app/Screenshot`):
  - Each screen row is read from the GRAM row the panel shows there, in
    runs of consecutive rows.
  - The format line says "hardware scroll active: rows read back through
    the scroll offset, start address n, so this is what the panel shows".
- **`Core2AudioBackend`**: the boost, the pacer and the start timing, all
  off by default. Only the lab turns them on. See the header's comments.

### 1. Hardware vertical scroll: what it should save

These are estimates from spike 1's measurements, not device results:

- a push costs about 0.43 us per pixel (a 290 x 168 viewport in 20.7 ms);
- a row costs 2.8 ms to draw idle and 5-7 ms during playback.

| At 15 fps | Lines per frame | Push | Rows drawn per frame | Frame, idle (spike 1: 35 ms) |
|---|---|---|---|---|
| slow drag, 300 px/s | 20 | 2.5 ms, + 2.1 ms rail | ~0.5 | ~6-8 ms |
| drag, 1,000 px/s | 67 | 8.2 ms, + 2.1 ms rail | ~1.6 | ~15-20 ms |
| over ~1,260 px/s (more than 84 lines a frame) | 168 (a full redraw in place) | 20.7 ms, + 2.1 ms rail | 4-5 | ~37 ms, the same as spike 1 plus the rail |

- **Drags and the tail of every flick get much cheaper.** Those are the
  "finger scrolling" of the user's complaint.
- **A fast flick doesn't.** Above 84 lines a frame the move is a full
  redraw in place, to bound the move's bus hold at ~12 ms (84 lines + the
  rail; spike 1's holds reached 15-33 ms while playing). At 4,000 px/s a
  frame moves 267 lines anyway, more than the band. That is the whole `w1`
  stress, so `w1` with `wm1` should read about like `wm0`, or 2 ms worse
  for the rail. Use `w0` with a finger, or a slower stress, to see the
  gain.
- The frame rate should rise with it: cheaper frames mean fewer lines per
  frame, which means cheaper frames again.
- The tracks list has no rail. Its frames are the line pushes alone.

Known artifacts, to check on the device:

- **Tearing.** The Core2 doesn't wire the TE pin, so the new lines and the
  address can land mid-scan. It is the same as spike 1's pushes, but it
  now involves a shift of the whole band.
- **A wrong strip at the far edge during a move.** The new lines are
  pushed into GRAM lines still on screen at the other edge (the top d
  lines show the rows about to appear at the bottom, or the reverse)
  until the address changes, in the same hold: for the push's bus time,
  ~2.5-10.5 ms for a 20-84 line move, longer if the decoder preempts.
- **The rail is displaced after the address** until it is pushed back,
  in the same hold, ~2 ms later.
- A full redraw (a jump, a rail scrub, a view change, a fast flick) fills
  in top to bottom in place, exactly like spike 1's redraw.

### 2. Interaction boost (`wb1`, `wm2`)

**Mechanism: lower the decoder, don't raise the loop.** Core 1 runs:

| Task | Priority |
|---|---|
| the speaker pump | 3 |
| M5.Speaker's own task (`task_priority` 2, pinned to core 1 by `SpeakerSink`) | 2 |
| the decoder | 2 |
| the Arduino loop | 1 |

Raising the loop to 3 would starve M5.Speaker's task during every frame,
and tie with the pump. Instead, while boosted, the decode task drops to
priority 0 (`kDecodePriorityYielding`), below the loop. It then runs only
while the loop sleeps: in its `delay(5)` per pass, while it waits for the
frame cap, and in a guaranteed rest after each frame (below). The outputs
and the speaker tasks keep their priorities.

At priority 0 the decoder shares core 1 with IDLE1: FreeRTOS time-slices
equal priorities (`configUSE_TIME_SLICING` 1), and IDLE1 doesn't yield
(`configIDLE_SHOULD_YIELD` 0) but waits for the next tick. So the decoder
gets **about half** of the loop's sleep, not all of it. (Priority 1, level
with the loop, would give the loop no more than it has unboosted.)

**The rule** (`UiBoost`, host-tested):

- wanted while the lab reports activity (finger down, list moving, a
  stress running), and for 300 ms after;
- on from a ring of at least 1,200 ms (under the ring's full level at
  48 kHz too: 65,536 frames are ~1.32 s full there, ~1.44 s at 44.1 kHz);
- off the moment the ring is below 900 ms, and on again only from
  1,200 ms **and** at least 300 ms after the drop (fewer, longer on and
  off periods instead of a flip every few hundred ms);
- with nothing playing (stopped, paused), on whenever wanted;
- a pending track start (Pending) counts as an empty ring, so the decoder
  gets to a skip at once.

**Enforcement from core 0.** A 10 ms `esp_timer` applies the rule
(`vTaskPrioritySet`). It runs on the esp_timer task on core 0, so it keeps
running however busy core 1 is. Even if the loop never slept, the ring
could only fall one check (10 ms of audio) below the floor before the
decoder is back at priority 2. The ring can't approach 500 ms because of the
boost. The host test runs a simulated minute of a finger on the glass with
the boosted decoder producing 0x (starved outright), 0.15x, 0.5x and 0.9x
realtime: the ring never goes below 890 ms, and the hold-off is kept after
every drop.

**The UI must still sleep.** While boosted, the loop rests at least 20 ms
after each frame (`kBoostRestMs`), measured from the frame's end, on top
of the frame cap (from its start) and even in `w3`, and it keeps its
`delay(5)`. The decoder gets about half of that rest.

**Logging.**

- `[boost] on|off at t=<ms>: <why>, ring <ms> ms` at each change;
- `boost=<ms>` in each `[scroll]` second: time boosted;
- in the stress summary: the share of time boosted, and the longest loop
  gap.

**What to expect.**

- With spike 1's full redraw (`wm0` `wb1`), a frame needs 35-46 ms of
  CPU. At the 66 ms cap that leaves 20-31 ms of sleep a frame, about half
  of it for the decoder: ~15-23 % of the core against the ~40 % an MP3
  needs, so the ring drains at about 0.5x (about 0.85x in `w3`, with only
  the 20 ms rest). The boost borrows the ~540 ms between full and the
  floor: about 1 s of heavy scrolling (0.6 s in `w3`). Then it is off for
  at least 300 ms while the decoder refills flat out, and on again from
  1,200 ms for another ~0.6 s: the frame rate alternates between boosted
  and not about once a second. These are estimates; the device run says.
- With the hardware scroll (`wm2`), drag frames are a fraction of that, so
  the loop sleeps most of each frame, the decoder gets most of what it
  needs, and the ring drains slowly or not at all while boosted.

**Costs.**

- `vTaskPrioritySet`, `esp_timer_create`/`start_periodic`/`stop` were
  already linked, so IRAM is unchanged.
- `xTaskDelayUntil` is not linked, and would land in IRAM. It is not used.
- The boost's mutex allocates a FreeRTOS semaphore (about 100 B internal)
  the first time the boost is enabled.

### 3. Gentle refill (`wp1`)

**What the decoder does now.** At a start or a skip it refills the empty
ring flat out, with only `vTaskDelay(1)` per 1,024-frame pass (23.2 ms of
audio).

- An MP3 pass takes about 9.3 ms, 40 % of realtime, so "flat out" is only
  about 2.3x realtime. That means a 2-3x cap would change nothing for
  MP3.
- The refill takes about 0.7 s and leaves the loop almost nothing: spike 1
  measured frames of 600-820 ms.

**With pacing on (`RefillPacer`, host-tested):**

- below 500 ms buffered, flat out, exactly as now;
- from 500 ms, the decode task sleeps after each pass so it produces at most
  **1.5x realtime** (`wp15`, the default cap; `wp<15-40>` sets x/10). 1.5x
  is also the lowest cap applied (`RefillPacer::kMinCapX10`, in the pacer,
  the backend and the console): near 1x, with the sleep rounded up, the
  paced ring would stop growing and settle at 500 ms for the whole track.
- only during the fill after a start or skip: once the ring has been full
  once, the pacing is off until the next start.
- For an MP3, that is a 7 ms sleep per 9.3 ms pass. The decoder's share of
  the core falls from about 90-100 % to about 57 %, and the ring still
  grows at +0.5x.

The simulated refill from empty:

| | Pacing off | Pacing on (1.5x) |
|---|---|---|
| To 500 ms buffered | ~0.4 s | ~0.4 s (the same) |
| Decoder's share of the core, 500 ms to full | ~90-100 % | ~57 % |
| Ring full | ~0.7 s | ~2.5 s |

**Time to first audio doesn't change.** The outputs read the ring as soon
as it has frames, and the pacing starts at 500 ms.

**Risk.** Above 500 ms the paced ring always grows (at least ~1.4x
production against 1x playback, with the rounding; a host test checks it
for MP3- and FLAC-like passes). A Bluetooth pull burst takes tens of ms,
far from 500. The ring's end-of-track drain and the governor's steady gate
are unchanged.

**The pacing applies only to the fill after a start or skip** (the backend
paces while that start's "ring full" time is still unset). A dip later in
the track (an SD stall, a Bluetooth burst, a boost drop to 900 ms) refills
flat out, as without pacing, so it doesn't eat into the margin the rest
of the design assumes.

**Measurement.** Both lines are new:

```
[audio] refill: first audio in the ring <ms> ms after the request, 500 ms buffered at <ms> ms, 1000 ms (steady) at <ms> ms, full at <ms> ms (-1: not reached); pacing off|<x>x from 500 ms
[scroll] track start: in the 3 s after it, longest frame <ms> ms, longest loop gap <ms> ms, <n> frames (<path>, boost on|off, refill pacing ...)
```

- The first comes from the backend, once per start, on any screen.
- The second comes from the lab, for each start while it is open. The loop
  gap catches a stall that falls between two frames.
- Spike 1's figure (a 600-820 ms frame) is the "before".

`wp` stays set after the lab closes, so a start can be timed from the
now-playing screen too. `wp0` turns it off.

### Console (scroll lab)

| Command | What |
|---|---|
| `wm0` | full redraw, spike 1 (the default then) |
| `wm1` | hardware vertical scroll (the default since the input stage) |
| `wm2` | hardware scroll + boost (removed since: now the same as `wm1`) |
| `wb0` / `wb1` | boost off / on, on either path (removed since) |
| `wp0` / `wp1` / `wp<15-40>` | refill pacing off / on / on at x/10 realtime (1.5x at least) |
| `wk<px/s>` | the stress's flick speed, 200-6,000 (default 4,000, spike 1's); `wk1000` is a fast drag, the hardware path's case |
| `ws` | shows all of them |

- `wm` works live: switching back to `wm0` clears the band (its GRAM is
  in the rotated order), ends the scroll and redraws in full at the
  identity.
- The console's `d` and `m` (the dance screen) refuse while a spike screen
  is up: the dance screen drew over the lab, and with the hardware scroll
  on the lab never redrew those lines (and the panel showed them rotated).
- The `[scroll]` line gains four fields: `hw|redraw`, `lines=` (lines drawn
  on the hardware path), `boost=` and `gap_max=`.

### Build and host results

- `pio test -e native`: **324 cases pass**, the 13 new included.
- `pio run -e core2` (pioarduino 55.03.312-1) builds.

| | IRAM (vectors + text) | Internal `.dram0.data` + `.bss` | Flash |
|---|---|---|---|
| round 1 on the device (the table above) | 124,035 B | 51,392 B | 2,251,508 B |
| round 2, after its review fixes | 124,035 B (unchanged) | 51,504 B (+112) | 2,260,212 B (+8,704) |

The new buffers are in PSRAM: the rail sprite (10 KB), and the `ListScroller`
inside the lab, which lives in PSRAM itself.

### On the device (26 September 2026)

Measured on the Core2 v1.3 (COM3) in silent test mode (`z`, confirmed after
every boot: `out=speaker(silent test mode)`), so the audio went to the muted
speaker. **Bluetooth output was not measured**: the headphones were connected
but silent mode kept them off the output. Audio came from the SD card: MP3
index 26 (Daft Punk) and FLAC index 70 (Kavinsky). The lists were the
synthetic 10,000-track library (`g10000`, artists view, with the A-Z rail)
and the card's 77 tracks (`g0`, tracks view `wv2`, no rail). Logs:
`r2_*.log` in the session scratchpad.

Changes made for the measurement (all in the lab or its logging):

- **`wk<px/s>`, the stress's flick speed** (200-6,000, default 4,000). At
  spike 1's 4,000 px/s nearly every frame moves more than the 84-line step,
  so `w1` could never exercise the hardware path. `wk1000` (1,000 px/s,
  slowing to ~400 before the next flick) is a fast drag: 20-60 lines per
  frame.
- **The `[scroll]` per-second line printed four strings into three `%s`**,
  so `lines=` showed a pointer and `boost=` / `gap_max=` showed the next
  field. Fixed (one `%s` added).
- **The `[audio] refill` line printed at once with every field -1.** The
  loop's `nowMs` is read before the console makes the request, so
  `nowMs - requestMs_` wrapped (the same bug as the stress's start in spike
  1). Now a signed difference.
- **The `[scroll] track start` watch missed the stall.** The decode task
  counts the start when it takes the request, while the loop is mid-frame,
  and then refills at priority 2: the stall is in the frame that is
  already running when the lab sees the new start. The watch read ~90 ms
  for an 800 ms stall. Fixed after these runs: the watch now includes the
  frame and loop gap that just ended. It was checked on the final build
  (`wm1`, `wp0`, three MP3 skips): the watch read 633, 416 and 506 ms, the
  same as the per-second maxima. The track-start figures below come from
  the per-second `frame=` maximum and `gap_max=` in the 4 s after each `n`,
  which catch it on every build.

The final build (all four changes): IRAM 124,035 B and internal
`.dram0.data` + `.bss` 51,504 B, both unchanged from the review build; flash
+~40 B.

#### 1. `w1` stress: `wm0` / `wm1` / `wm2`

`w1` as in spike 1 (governor on, 15 fps cap, 14 px slices, 30 s; 20 s for
the idle rows). fps is while moving (100 % of the time), p10 / p50. Frame
is p50 / max, draw and push are per-frame p50 of the per-second means, hold
is the SPI hold per lock, mean p50 / max. Boost is the share of time boosted
and the on/off count. No run had an underrun.

**10,000 tracks, artists view (rail), nothing playing:**

| Path, flick | fps p50 | Frame ms | Draw | Push | Hold ms | Heap min |
|---|---|---|---|---|---|---|
| `wm0`, 4,000 | 14.8 | 35.0 / 44.4 | 13.6 | 20.4 | 1.54 / 2.8 | 62 K |
| `wm1`, 4,000 | 14.5 | 41.8 / 49.8 | 19.3 | 22.4 | 1.44 / 12.2 | 67 K |
| `wm0`, 1,000 | 14.7 | 34.7 / 37.6 | 13.8 | 20.4 | 1.55 / 2.8 | 67 K |
| `wm1`, 1,000 | 14.9 | **13.9** / 42.3 | 5.7 | 8.0 | 7.61 / 12.2 | 67 K |

**10,000 tracks, artists view (rail), MP3 playing:**

| Path, flick | fps p10 / p50 | Frame ms | Draw | Push | Hold ms | Ring min | Decode p50 / max | Heap min | Boost |
|---|---|---|---|---|---|---|---|---|---|
| `wm0`, 4,000 | 9.7 / 10.6 | 80.5 / 104.9 | 34.5 | 40.1 | 2.99 / 23.0 | 1,439 | 40 / 42 % | 49.5 K | |
| `wm1`, 4,000 | 8.7 / 9.7 | 83.7 / 111.9 | 40.6 | 40.3 | 2.81 / 31.0 | 1,439 | 39 / 44 % | 49.4 K | |
| `wm2`, 4,000 | 5.8 / 6.4 | 79.7 / **421** | 47.4 | 30.3 | 1.92 / 52.9 | **888** | 91 / 115 % | 49.4 K | 85 %, 23 on / 25 off |
| `wm0`, 1,000 | 9.4 / 10.8 | 79.8 / 102.0 | 37.7 | 38.5 | 2.91 / 23.5 | 1,439 | 38 / 44 % | 49.5 K | |
| `wm1`, 1,000 | **13.7 / 13.9** | **25.8** / 91.7 | 10.2 | 14.2 | 14.4 / 33.8 | 1,439 | 38 / 41 % | 49.1 K | |
| `wm2`, 1,000 | 6.4 / 7.0 | 48.2 / **370** | 22.6 | 18.2 | 2.37 / 52.8 | **882** | 91 / 96 % | 49.2 K | 92 %, 18 on / 18 off |

**10,000 tracks, artists view (rail), FLAC playing:**

| Path, flick | fps p10 / p50 | Frame ms | Draw | Push | Hold ms | Ring min | Decode p50 / max | Heap min | Boost |
|---|---|---|---|---|---|---|---|---|---|
| `wm0`, 4,000 | 10.8 / 11.7 | 54.8 / 80.4 | 23.5 | 28.8 | 2.17 / 32.2 | 1,439 | 32 / 36 % | 51.9 K | |
| `wm1`, 4,000 | 10.6 / 11.0 | 62.8 / 88.7 | 30.0 | 28.5 | 2.01 / 38.0 | 1,439 | 32 / 36 % | 51.8 K | |
| `wm2`, 4,000 | 6.0 / 7.7 | 59.3 / **327** | 29.6 | 28.4 | 1.67 / 32.6 | **899** | 83 / 97 % | 51.9 K | 94 %, 16 on / 16 off |
| `wm0`, 1,000 | 10.7 / 11.7 | 55.2 / 80.0 | 25.2 | 28.2 | 2.19 / 33.0 | 1,422 | 32 / 36 % | 51.8 K | |
| `wm1`, 1,000 | **11.7 / 12.6** | **22.8** / 79.6 | 9.6 | 10.9 | 7.31 / 38.3 | 1,439 | 32 / 34 % | 51.5 K | |
| `wm2`, 1,000 | 8.7 / 14.0 | 18.1 / **215** | 7.7 | 9.4 | 4.87 / 37.7 | **882** | 89 / 100 % | 51.5 K | 98 %, 6 on / 6 off |

**The card's 77 tracks, tracks view (no rail), MP3 playing:**

| Path, flick | fps p10 / p50 | Frame ms | Draw | Push | Hold ms | Ring min | Decode p50 / max | Boost |
|---|---|---|---|---|---|---|---|---|
| `wm0`, 4,000 | 9.5 / 10.3 | 82.5 / 114.1 | 35.0 | 43.5 | 3.35 / 24.2 | 1,439 | 39 / 43 % | |
| `wm1`, 4,000 | 8.9 / 9.9 | 80.5 / 125.7 | 36.5 | 40.2 | 3.33 / 25.7 | 1,439 | 39 / 44 % | |
| `wm0`, 1,000 | 9.4 / 10.0 | 84.9 / 113.8 | 37.1 | 45.2 | 3.50 / 24.1 | 1,439 | 39 / 42 % | |
| `wm1`, 1,000 | **13.7 / 13.9** | **22.3** / 98.0 | 8.5 | 12.3 | 12.2 / 24.3 | 1,439 | 37 / 41 % | |
| `wm2`, 1,000 | 6.4 / 7.0 | 50.8 / **366** | 20.5 | 17.9 | 2.71 / 55.9 | **888** | 91 / 99 % | 17 on / 18 off |

**The frame cap is now the limit** (MP3 playing, 10,000 tracks, rail):

| Run | fps p10 / p50 | Frame ms |
|---|---|---|
| `w3` (no cap, no governor), `wm0`, 1,000 | 10.5 / 10.7 | 79.1 / 103.3 |
| `w3`, `wm1`, 1,000 | **37.8 / 40.6** | 14.4 / 88.2 (draw 4.7, push 8.3) |
| `w1` `wf30`, `wm0`, 1,000 | 9.8 / 10.9 | 78.7 / 100.6 |
| `w1` `wf30`, `wm1`, 1,000 | **23.7 / 25.0** | 18.2 / 91.7 |
| `w1` `wf30`, `wm1`, 2,000 | 14.5 / 16.5 | 46.5 / 96.9 |
| `w1` `wf30`, `wm1`, 4,000 | 9.7 / 10.2 | 82.4 / 111.1 |

All with ring minimum 1,439 ms and no underruns.

What this shows:

- **The hardware scroll works as predicted for drags.** At 1,000 px/s a
  frame costs 22-26 ms during playback instead of 80 (MP3) or 55 (FLAC),
  and 13.9 ms idle instead of 35. At the 15 fps cap that gives 13.9 fps
  with an MP3 against 10.8, and 12.6 with a FLAC against 11.7. Uncapped,
  the same drag runs at 40 fps with an MP3 playing (the full redraw: 10.7).
- **It doesn't help fast flicks, as predicted, and costs a little.** At
  4,000 px/s the lines per frame exceed the 84-line step, every frame is a
  full redraw in place, and the rail sprite is extra: 9.7 fps against 10.6
  with an MP3, 11.0 against 11.7 with a FLAC. Idle it costs ~7 ms a frame
  (draw 19.3 against 13.6: the rail sprite is drawn at every frame).
- **There is a cliff between the two.** Once a frame moves more than 84
  lines it costs a full redraw, which makes the next frame later and
  longer still (`wf30` at 2,000 px/s: 16.5 fps, most frames full).
- **The move's single bus hold** (new lines, the address, the rail) is
  7-14 ms on average during drags and at most 34-38 ms with audio (12.2 ms
  idle): the decoder preempts the loop inside the hold. The ring never
  noticed: 1,439 ms minimum in every non-boosted run.
- **The interaction boost (`wm2`) makes everything worse.** Frame rate
  halves (7.0 fps against 13.9 for `wm1` with an MP3), stalls of 215-421 ms
  appear, and the ring cycles between 1,200 and ~885 ms, the boost turning
  on and off every ~1.5 s (18-25 times in 28 s). The decoder's "decode %"
  reads 90 % because its passes are spread out by preemption, not because
  it works harder. Why:
  - boosted, the decoder (priority 0) only runs while the loop sleeps,
    and shares that time with IDLE1; the loop's sleeps (the frame cap,
    the 20 ms rest, `delay(5)`) are therefore half wasted on the idle
    task;
  - so the decoder falls behind, the ring drains to the 900 ms floor, the
    boost drops, and the decoder refills at priority 2 flat out
    (about 2.3x realtime), which stalls the UI for 200-400 ms;
  - over each cycle the decoder still needs its ~40 % of core 1: the boost
    cannot create CPU, only move it, and it moves it badly.
- **The ring is never the constraint without the boost.** 1,439 ms (of
  ~1,450) in every run, as in spike 1.
- The heap minimum is unchanged (49-52 K while playing, as spike 1).

#### 2. Track starts: the gentle refill (`wp1`)

`w1` at 1,000 px/s, 40 s, three `n` skips 10 s apart, after an `i26` (MP3)
or `i70` (FLAC) start. The stall is the longest frame and the longest loop
gap in the 4 s after each skip. The refill timings are the backend's
`[audio] refill` line, in ms after the request. No underruns in any run.

| Audio, path | Pacing | Longest frame per skip (ms) | Longest loop gap (ms) | Lowest fps in a second |
|---|---|---|---|---|
| MP3, `wm0` | off | 809, 790, 788 | 819, 810, 809 | 2.7 |
| MP3, `wm0` | 1.5x | 326, 337, 378 | 359, 355, 393 | 6.2 |
| MP3, `wm1` | off | 710, 325, 364 | 725, 345, 388 | 3.0 |
| MP3, `wm1` | 1.5x | **235, 146, 156** | 258, 204, 165 | 10.0 |
| FLAC, `wm0` | off | 621, 635, 673 | 745, 661, 710 | 4.3 |
| FLAC, `wm0` | 1.5x | 353, 355, 252 | 391, 362, 317 | 7.9 |
| FLAC, `wm1` | off | 545, 388, 433 | 576, 440, 439 | 3.9 |
| FLAC, `wm1` | 1.5x | **311, 292, 300** | 319, 316, 309 | 8.8 |

| Audio | Pacing | First audio in the ring | 500 ms buffered | 1,000 ms (steady) | Full |
|---|---|---|---|---|---|
| MP3 | off (12 starts) | 20-30 ms | 283-325 ms | 556-628 ms | 810-886 ms |
| MP3 | 1.5x (14 starts) | 18-32 ms | 284-333 ms | 1,373-1,449 ms | 2,341-2,426 ms |
| FLAC | off (4 starts) | 85-96 ms | 358-394 ms | 571-705 ms | 777-988 ms |
| FLAC | 1.5x (4 starts) | 82-99 ms | 322-383 ms | 2,005-2,707 ms | 3,595-4,652 ms |

- **The stall is halved or better**: 790-810 ms to 150-380 ms with an MP3,
  620-670 ms to 250-355 ms with a FLAC. What is left is the flat-out fill
  to 500 ms (~300 ms), which is by design.
- **Time to first audio doesn't move** (MP3 20-30 ms, FLAC 85-99 ms), nor
  does the time to 500 ms buffered.
- **A FLAC's paced fill is slower than modelled**: full at 3.6-4.7 s, not
  ~2.5 s, so it grows at about 1.25x rather than 1.5x. It still always
  grows, and the ring minimum after each start (counted once steady) was
  981-1,021 ms. It means the ring spends 2-4 s under 1 s after each FLAC
  start: fine for the speaker, to be checked over Bluetooth.

#### 3. Soak: 10 minutes, `wm1` + `wp1`, 30 fps cap

The configuration recommended below: `wm1` (hardware scroll), `wp1` (1.5x
pacing), boost off, `wf30` (30 fps cap), `w1` at `wk2000` (2,000 px/s
flicks: a mix of incremental moves and full redraws, plus an A-Z jump every
5th action), 10,000 tracks, artists view with the rail. Two 300 s stresses
back to back: from `i26` (MP3, then tracks 27-36, all MP3) and from `i70`
(FLAC, tracks 70-76, then the card's tone and click test tracks 77-82),
with `n` every 30 s (20 skips, plus two ends of track).

| Half | fps while moving p10 / p50 / min | Frame max | SPI hold max | Ring min (steady) | Underruns | Decode p50 / max | Heap min |
|---|---|---|---|---|---|---|---|
| MP3, 290 s | 13.0 / 15.8 / 6.6 | 381 ms (a skip) | 71.9 ms | 1,021 ms | **0** | 39 / 65 % | 48.6 K |
| FLAC then tones, 294 s | 17.9 / 20.6 / 8.6 | 350 ms (a skip) | 45.7 ms | 981 ms | **0** | 31 / 62 % | 51.0 K |

- **No underrun in 10 minutes**, and none in any other run of this round
  (about 35 minutes of stress with audio playing in all).
- The stall at each skip: 156-381 ms with an MP3 (9 skips), 183-350 ms with
  a FLAC (6); 46-153 ms into the tone tracks, which fill in ~0.2 s.
- Every start's first audio came 21-32 ms (MP3) and 84-97 ms (FLAC) after
  the request; 500 ms was buffered at 284-392 ms.
- The ring minimum (981-1,021 ms) is the paced fill passing 1,000 ms, when
  the ring starts to count as steady. Mid-track the ring stayed at 1,439 ms.
- The longest SPI hold was 72 ms: the loop's hold preempted by the
  decoder's flat-out fill to 500 ms after a skip (spike 1 saw 15-33 ms for
  the same reason). The ring didn't notice.

#### Screenshots and what only the user can check

**The screenshot's reconstruction is consistent.** After the soak the lab
was left at a row-aligned offset with the panel's start address at 211. That
is a shift of 139 lines, so the band's wrap was at screen line 101, inside
the second row. `X` logged "hardware scroll active: rows read back through
the scroll offset, start address 211", and the picture showed the list
straight (four artist rows, the rail's L). Then `wm0` (which clears the
band, returns the address to the identity and redraws in full) and `X`
again: **the list band (lines 72-239, rail included) was identical, pixel
for pixel**. Only the fps chip in the tab bar differed (92 px). Files:
`r2_shot_wm1.png`, `r2_shot_wm0.png`.

What that proves: `ListScroller::gramLineForScreen()` (the screenshot) and
`VScrollMap`'s placement (the drawing) agree on where every line is, across
a wrap, and leaving the hardware path leaves nothing rotated in GRAM. What
it can't prove: that the panel really shows GRAM that way. A readback reads
GRAM, and the reconstruction assumes the datasheet's rule. Only eyes can
check that.

**The user's visual check (about 2 minutes).** Over the serial console,
with the Core2 in front of you:

1. `z` (silent mode), `g10000` + Enter (the synthetic library), `i26` +
   Enter (an MP3, silent).
2. `wm1` + Enter, then `w0` + Enter: the scroll lab on the hardware path.
   With a finger, in the Artists list, slow drags up and down, then fast
   drags, then flicks. Look for:
   - **the rows move smoothly and in order**: no row appears twice, none
     is missing, nothing jumps back;
   - **the tab bar and the Artists / Albums / Tracks header stay still**,
     and nothing of the list is drawn over them;
   - **no band of wrong rows** at the top or bottom of the list during a
     move (a strip showing rows from the other end, or a line of
     misplaced pixels), and no seam that stays on screen;
   - **the A-Z rail on the right stays in place**, perhaps with a brief
     flicker as the list moves (it is put back after each move). A rail
     that visibly travels with the list is a fault;
   - dragging the rail itself (a scrub) makes the list jump and redraw
     top to bottom, as before.
3. `wv2` + Enter (the tracks list, no rail): the same drags.
4. `X`: a screenshot. It must match what you see.
5. `wm0` + Enter: the same drags on the old path, to compare the feel.
   Expected: visibly choppier drags with the music playing.
6. `wq` + Enter: the lab closes. The next screen must draw straight, with
   no part of it shifted or rotated. Then space (pause).

Tearing: the Core2 doesn't wire the panel's TE pin, so a diagonal tear in a
fast move is possible on both paths. Note whether it is worse on `wm1`.

#### Recommendation for the real UI

1. **Use the hardware scroll (`ListScroller`) for the real UI's lists**,
   once the user's visual check passes. For finger drags (the complaint)
   it cuts a frame during MP3 playback from ~80 ms to ~25 ms: 13.9 fps
   against 10.8 at the 15 fps cap, and 40 fps uncapped. It costs the audio
   nothing (the ring stayed at 1,439 ms) and no internal RAM.
2. **Raise the list's frame cap to 30 fps with it** (25 fps measured at
   `wf30`, 40 uncapped). Schedule frames from the deadline rather than
   from the pass that drew the last one, to get closer to 30: today a
   frame slips by up to a `delay(5)` plus a decoder pass.
3. **Remove the fast-flick cliff.** Above 84 lines a frame, a move becomes
   a full redraw, which makes the next frame later and longer still.
   Either cap the list's fling speed at about 2,000 px/s (at 25-30 fps
   that stays under 84 lines), or raise `maxStep` to the band height minus
   one: the bounded hold it buys (~12 ms) isn't needed, since holds of
   34-72 ms occurred in this round with no effect on the ring. Measure it
   with `w1` `wf30` `wk2000` / `wk4000`.
4. **Turn the refill pacing on for good (`wp1`, 1.5x).** It halves the
   stall at every start and skip (MP3 ~800 ms to ~150-380; FLAC ~650 to
   ~250-350), leaves the first audio where it was (20-30 ms MP3, 85-99 ms
   FLAC), and had no underrun in 41 paced starts (33 of them MP3 or FLAC
   files). The stall left is the flat-out fill to 500 ms (~300 ms). Starting the gentle phase
   lower (say 300 ms) would shorten it, but needs a Bluetooth check first.
5. **Drop the interaction boost.** In every configuration it made the list
   slower (7 fps against 13.9), added 200-420 ms stalls, and cycled the
   ring down to ~885 ms every 1.5 s. Lowering the decoder to priority 0
   hands the loop's sleep half to IDLE1, and the decoder still needs its
   ~40 % of core 1. The decoder's slack is better spent by making frames
   cheaper (point 1). Keep `UiBoost` off (it is by default), and remove it
   from the backend when the real UI lands, unless a different design
   (for example the decoder at priority 1 only during a frame) is measured
   first.
6. **Before shipping, repeat `w1` (`wm1`, `wp1`, `wf30`) and the skip test
   with Bluetooth output**, in a session where the user takes the
   headphones off. None of this round ran over Bluetooth, and the A2DP pull
   is burstier than the speaker's. The paced FLAC fill spends 2-4 s under
   1 s of ring after each start: that is the case to watch.

## After the spike: the input layer and scroll polish

The user tried the input lab and the scroll lab on the device. What they
measured, and what the firmware does about it now (the browsing screens
themselves come next). Nothing below has been run on the device yet.

**What the user measured:**

- **Buttons:** clicks lasted 17-143 ms, holds 509-2,383 ms. The 500 ms hold
  threshold separates them: kept (not the 800 ms the design review proposed
  for B). The red dots never registered as glass touches, and glass taps at
  the bottom edge never fired a button: no dead band is needed.
- **Touch x error** (`u1`, thumb and index finger alike, so the sensor):
  about 0 at x 60-150, about +20 px at x 190, +35-45 px from x 240, clamped
  at 319; a target at x 27 read 0. The volume chip in the tab bar's corner
  (280-319) took every tap meant for the Output tab next to it (224-279):
  those taps read 276-298.
- **Haptics:** a tap is 33 ms at level 235 (strong); a hold, once
  recognised, a double tick (2 x 33 ms, 80 ms apart). Nothing on scroll
  frames; a tick per new letter on the A-Z rail is optional.
- **Scrolling:** the hardware scroll (`wm1`) is "much smoother"; the A-Z
  rail on the right jittered while swiping.

**What was built** ([ARCHITECTURE.md](ARCHITECTURE.md#input)):

- **One input layer** (`ui/Input`) that alone reads the panel and the
  buttons and hands out events: the glass's Down, Tap, LongPress, Release,
  DragStart/Move/End and Fling (`TouchRecognizer`), the buttons' Click,
  Hold (500 ms), Repeat (A and C, 200 ms) and HoldEnd (`ButtonGesture`).
  What the buttons do is `ButtonPolicy`: the same on every screen, and a
  B-hold to the speaker pauses first.
- **Touch correction** (`TouchCalibration`): a monotonic piecewise-linear
  table per axis, applied to every touch before any hit test. The default x
  table is the least-squares fit of the 72 `u1` taps (the "last list row"
  taps left out: that target is the whole width). Per target, the mean
  corrected position is within 6.2 px of every target but the volume chip
  (Play next +18.5 px raw, +0.2 corrected; Output tab +35.0 / +6.2; Undo
  +36.3 / +2.4; Clear all +41.8 / +5.3); the rms error over all 72 taps is
  26.4 px raw, 9.0 px corrected, most of what is left being the clamped
  readings at 319 (the volume chip's taps: +13 raw, -23 corrected).

  | Raw x | 0 | 40 | 80 | 120 | 160 | 200 | 240 | 280 | 319 |
  |---|---|---|---|---|---|---|---|---|---|
  | Corrected | 25.6 | 43.2 | 78.5 | 122.7 | 154.4 | 181.9 | 212.3 | 253.1 | 281.3 |

  A reading clamped at 319 lands at ~282 (between the targets at 273 and
  299 whose taps read 319), so a control at the right edge must reach the
  screen's edge and be ~40 px wide; the events also flag a clamped reading.
  The volume chip goes: the design will show the volume inside the Output
  tab instead, with a HUD for the A/C holds (the policy leaves the feedback
  for it).
- **A calibration screen** (console `a`, `a5`-`a9`): tap the crosshairs, a
  new table is fitted the same way, Save keeps it in NVS, `ac` checks it,
  `ad` goes back to the default.
- **`u1` now also logs the corrected press:** each `[target]` line ends with
  `cal=(x,y) cal_off=(dx,dy) cal_hit=yes|no` for the table in use (the
  lab's own coordinates stay raw). A new `u1` round is the check of the
  default table.
- **Haptics** as chosen: a tick on each button click and glass tap, a
  double tick at each recognised hold; `ah0` turns them off, `ar0` the
  rail's ticks.

**Scroll lab polish:**

- The **interaction boost is removed** (`UiBoost`, the backend's 10 ms timer,
  `wm2`, `wb`): it measured worse in every configuration. **Refill pacing
  is on by default** (1.5x from 500 ms; `wp0` turns it off). `RefillPacer`
  now lives in its own file.
- **Defaults:** the hardware scroll (`wm1`), a **30 fps** cap (`wf30`), and
  the stress's flicks at 2,000 px/s (`wk`; the stress may still go faster
  to measure it).
- **Frames on deadlines:** each frame is due a period after the previous
  one's deadline, not after the pass that drew it, so a late pass (the
  `delay(5)`, a decoder pass) no longer pushes every later frame back;
  after an idle spell the cadence starts over.
- **Flings capped at 2,000 px/s** (the input layer's release velocity and
  `KineticScroll`): above ~2,500 px/s every frame at 25-30 fps moved more
  than the 84-line step and was a full redraw.
- **The A-Z rail no longer jitters:** it sits inside the hardware-scrolled
  band, so each move shifted it with the list until it was pushed back. It
  is now hidden while the list moves (its column cleared once, the rows
  pushed full width over it) and drawn again when the list comes to rest,
  or at once when a finger touches the right edge (a scrub keeps it up and
  puts it back after each jump, as before). Its touch zone is x 280 to the
  screen's edge.
- The lab's touches come from the input layer, corrected.

**To check on the device:**

1. `w0` (`g10000` first): drag and flick the artists list; the rail should
   vanish while the list moves and come back where it stops, with no
   jitter; a touch on the right edge brings it back at once and scrubs.
   Flicks should feel capped but not sluggish.
2. `w1` with an MP3 playing: fps while moving (expect ~25-30 on drags), the
   ring minimum and underruns (expect none).
3. `u1`: the `cal_hit` rate against the raw `hit` rate, per zone.
4. `a`: the calibration screen, Save, then `ac`.
5. Buttons: holds on A and C step the volume 5 % every 0.2 s with a double
   tick at the start; a B-hold on Bluetooth while playing pauses, then
   switches to the speaker.

## After the spike: the UI framework

The tab bar framework the screens will sit on is built on what the spike
measured ([ARCHITECTURE.md](ARCHITECTURE.md#ui)): the lists use
`ListScroller` (the hardware scroll) with the polish above (the rail and the
scrollbar hidden while a list moves, flings capped at 2,000 px/s, a 30 fps
cap on deadlines, the governor as the safety net), the text is the VLW
DejaVu of `e4` (16 and 13 px, plus DejaVu Sans Bold 16 and 22, with the
licence in `LICENSES/`), efont is out of the default build (the font probe
keeps it behind `-DUI_SPIKE_EFONT=1`), and every overlay and page draws
through the scroll mapping and in short bus holds. The spike tools stay as
diagnostics: opening one suspends the UI (and frees the hardware scroll for
the scroll lab); closing it brings the UI back as it was.

### The framework on the device (27 September 2026)

A smoke test of the framework (the data layer, the input layer and the tab
UI), run from the console in silent test mode (speaker at volume 0; the
headphones were connected but never the output). The pages were driven with
a scripted finger (`uit`/`uis`/`uid`, see ARCHITECTURE "UI"), which goes
through the same recogniser and hit tests as a real touch but not through
the panel or its calibration. The library is the card's: 77 tracks,
6 artists, 6 albums.

**Boot and memory** (internal RAM free, from the `[heap]` and `[stats]`
lines):

| | now | the spike (round 2) |
|---|---|---|
| after the library and queue | 91K (the library + queue cost 4.2 KB of internal RAM; the old two-copy track list cost ~13 KB for 77 tracks) | 71K after the track list |
| idle, UI up, headphones linked | 81K (min 75K) | 76-77K |
| playing MP3 | 63K (min 58K) | 55-61K |
| playing FLAC | 66-75K (72K at its first `[heap] playing`) | |

The UI itself costs no internal RAM at start (`[ui] internal RAM 92620 B
free before the UI, 92620 B after`); PSRAM free drops from 3588K to 3260K.
The library loads from its cache in 5 ms (83 files walked in 56-58 ms);
the queue came back after a reset (`restored 11 of 11 ... at 2 of 11
(position from NVS)`).

**The tabs** (screenshots `X`, compared with the design's mockups): every
tab renders. Fixed: the active tab's label was cut ("Playi…": "Playing" is
48 px and had 46; the plate is now the cell's width less 2 px), the battery's
"100%" was cut to "10…" (36 px in 34), the Output page's "Forget
headphones" link was cut (now "Forget pairing") and its speaker line (now
"(silent)" in test mode).

**The track length** on Now Playing was wrong: 7:37 for a 5:20 VBR MP3
15 s in, and 3:30 for a 3:13 FLAC (the read-rate estimate assumes a
constant bitrate). The backend now reads the length when a track opens:
FLAC from STREAMINFO, MP3 from the Xing/Info or VBRI header (new
`TrackProgress` functions, host-tested); both then read exactly (5:20.9,
7:09.9, 3:13.4, 3:15.2).

**Library and queue, by touch** (all worked): artists > artist > album >
a track's inline bar; Play next (toast "Plays next: …" with View and Undo,
the Queue badge flashing 62 → 63), + Queue then Undo on the toast (88 → 87
tracks), View (the Queue opened scrolled to the added entry, 3486 px down),
Play from a track (the album from that track, 14 entries at 2), the
Queue's row bar Remove, selection mode (2 selected, Remove in the header:
"Removed 2 tracks"), and a reset (the queue and its position restored).

**Scrolling** while an MP3 plays (the new `[ui] scroll:` line per motion;
the real lists are the Queue, 86 rows, and an artist's 14 tracks: the card
has no alphabetical list long enough for the A-Z rail, so the rail itself
is still to see, see below): drags and flings at 23-30 fps (30 once a
motion is under way; the short ones count the first frame from the press),
draw time per frame mean 3-16 ms, ring minimum 1439 ms (full), no
underruns, the governor never left "normal". About 1 motion in 5 has one
frame of 50-78 ms: a very fast drag (the finger isn't capped, only the
fling) moves more than the 84-line step in a frame, which is a full redraw.
Bus holds over the whole session: mean 2.8 ms, max 51 ms.

**Dance:** the 128 BPM click track locked at 128.05 BPM 2.6 s after the
tab opened (median phase error 2.5-3.0 ms), the dancer at 30.5 fps.

**Soak:** 10 minutes (618 s, 18 rounds of tab switches, Library
drill-downs and back, 68 scrolls on the Queue and the album list; MP3 for
the first half, FLAC for the second): no crash, 0 underruns, ring minimum
1129 ms (at the FLAC's start), scroll fps mean 26.8, the governor always
"normal", the loop task's stack never below 5404 B unused of 8 KB. No heap
drift: the same MP3 track afterwards showed ram=63K min=58K psram=3235K,
as before the soak.

**Not explained:** once, early in the session, the player jumped from queue
entry 24 to entry 2 while a screenshot was being read (nothing in the log
said why; no button or console command). It didn't happen again in ~15
minutes of the same actions. The log now has a line for every touch
(`[touch] ...`, "scripted" for the test finger's) and every move of the
current entry (`[queue] now at ...`), so a repeat will show its cause.

**Still for the user, by hand:**

1. Touch: `ac` (the check page) with the default table, then `a` to
   calibrate, Save, `ac` again; tabs at the right edge land on Output.
2. Haptics: a tap tick on tabs, rows and buttons (not on empty space); a
   double tick on a long press of an artist or album (its sheet) and on
   A/C holds; nothing while scrolling.
3. Real fingers on the lists: flicks feel capped but not sluggish; the
   scrollbar on the Queue vanishes while it moves and comes back when it
   stops, with no jitter.
4. The A-Z rail needs a long alphabetical list (over 30 artists): with a
   bigger card, drag the Library's artists and the right edge (the rail
   hides while moving, comes back on a touch at the edge and scrubs with a
   tick per letter).
5. A/C holds: the volume HUD over the tab bar, 5 % every 0.2 s; B hold on
   headphones while playing: pause, then the speaker.
6. The headphones' own volume keys: the same HUD.
7. Now Playing's times and progress line on your own files.

## After the framework: Now Playing, the Library and covers

The next stage builds the two screens the design centres on, on the
framework ([ARCHITECTURE.md](ARCHITECTURE.md#ui)), and fixes what the
framework's smoke test left. Built and host-tested only (the host tests,
and a firmware build); **nothing below had run on the device yet** (it has since: "Stage 2 on the device", at the end).

**What the smoke test left, and what was done:**

- **The play-next toast cut the name** ("Plays next: Aer…", View and Undo
  took the right half): View and Undo are compact pills now (54 px each,
  their hit areas unchanged in reach: Undo still to the screen's edge),
  and a "Plays next: <name>" that doesn't fit takes two lines, "Plays next"
  small over the name, which then has 164 px ("Aerodynamic" is 106).
- **A very fast drag made a 50-78 ms frame** (1 motion in 5): a move is now
  drawn in steps of at most 84 lines a frame (`ListLayout::stepToward()`),
  the hardware scroll's one-hold step. A finger over ~2,500 px/s is
  followed a frame or two late and caught up as it slows; flings (capped at
  2,000 px/s, 67 lines a frame at 30 fps) never meet the step, and jumps
  still draw at once. The host test runs a 4,000 px/s drag: 84 lines a
  frame, caught up within 7 frames of the finger stopping.
- **The tab bar's battery text was cut** before a late fix: every text the
  bar can show is now measured in a host test with the firmware's own font
  data (`src/ui/VlwFonts.cpp`): the five labels on their plates, the volume
  and the battery at every level 0-100 % (the widest, "100%", is 36 px:
  exactly the volume's room, 2 px under the battery's), the Queue badge at
  every count. The room each gets is in `TabBarModel` now, where the drawing
  and the test read it.
- **The unexplained queue jump** (entry 24 to 2 during a screenshot): every
  path that moves the current entry was reviewed (ARCHITECTURE "UI",
  "Console"). The only one that can go to entry 2 with the queue's size
  unchanged and leave no line in the log was the Queue's row-bar Play; the
  same log shows the Dance tab opening 2.5 s later with no console command,
  so fingers were on the glass. It is logged now, with every tab change and
  every play from Now Playing and the Library.

**What was built:** Now Playing with the cover, the 40 px artist and album
bands, the volume sheet and the "..." sheet (Go to artist, Go to album,
Show in folders); the Library's Artists, Albums (with covers) and Folders
(non-audio files counted, the path on one thin line, "Play all N" at the
root), the inline action row and a long press on any row, "‹ Library" on
deep levels, the A-Z rail's jump grid with a second level; album covers
decoded by a worker task below the loop into a PSRAM LRU and `.565` files
on the card. The choices (why a task, what it costs in RAM) are in
ARCHITECTURE "UI", "Album covers".

**To check on the device:**

1. Boot: the library cache is rebuilt once (its version changed: folders
   now count their other files and pick a cover). `[lib] ... built (the
   cache was unreadable)` then, next boot, `loaded from the cache`.
2. Now Playing: the cover appears within ~0.3 s of the page (a
   `[thumb] /music/.../cover.jpg: 650x565 at 1/4, 40 + 96 px in N ms` line
   the first time; nothing the next boot: the card's copy). The album with
   the progressive cover keeps the note and logs it once (`progressive
   JPEG ... (remembered on the card)`), and not again after a reboot. Tap
   the artist, the album, the cover; "..." > Show in folders.
3. `ui` after browsing the Albums list: the `[thumb]` lines: decodes' mean
   and max, the worker's least stack left (of 6,144 B: trim it if it's
   far from full) and the lowest internal RAM free during its jobs (the
   spike's drawJpg path took it to 46 KB; this path should stay near the
   playing figure, 55-63 KB). After 10 s without covers to make, `worker
   not running`.
4. Scrolling the Albums list while an MP3 plays: `[ui] scroll:` lines as
   before (no new job starts while it moves; the covers fill in when it
   stops, a row at a time), ring minimum and underruns unchanged.
5. Very fast drags: no frame over ~35 ms in the `[ui] scroll:` max.
6. `uil10000`: the Library shows 600 artists and 1,500 albums; tap the rail
   for the grid, a big letter for its "Ka/Ke/..." level, drag the rail to
   scrub (a tick per letter); `uil0` goes back.
7. The volume sheet: drag the slider with the headphones on (the value
   follows the finger; the headphones' level follows in 5 % steps), and the
   A/C holds while it's open (it shows their steps).
8. The toast after Play next on a long title: two lines, the whole name.

## After the Library: the Queue, Output, states and the coach cards

The rest of the tab bar design, on the framework
([ARCHITECTURE.md](ARCHITECTURE.md#ui)). Built and host-tested only (the
host tests, and a firmware build); **nothing below had run on the device
yet** (it has since: "Stage 2 on the device", at the end).

**What was built:**

- **The Queue** (mockups 16-18): the summary "4 of 16 · 12 up next · 49
  min" in the header (track lengths learned as they play; "49+ min" while
  some aren't known), Play now / Play next / Remove under a tapped row,
  the added tracks shown and highlighted on the visit after a Library
  add, selection mode with All / None and a bar under a shorter list
  (Remove, Play next, Clear...: "Clear up next" keeps the playing song,
  "Clear queue" stops it after a red confirmation), Undo on every edit,
  an amber "!" on a track that couldn't be played, and the empty state
  (Open Library, Shuffle all).
- **Output** (mockups 19-21): the Bluetooth card in all its states
  (`OutputModel`: not paired, off, connecting "try 2 of 3" with Cancel,
  searching, pairing, connected with the codec and the delay, failed with
  Try again, lost), Disconnect, Forget on a second tap within 3 s (red
  "Tap again"), the speaker card, each with its own volume sheet, the
  line-out placeholder, Pair new headphones (a scan list with the
  devices' kind and signal, a tap pairs; the new pair replaces the old one
  only once it is connected), Haptics on/off, Touch calibration, About.
  The audio stays where it is until the headphones asked for are up
  (the card, Connect and the B hold only connect them), and every move
  off them pauses first.
- **States**: no card (Try again: looks for a card and restarts the
  player to use it), no music (Try again: walks /music again), a track
  that failed (an amber note and the "!"), the headphones lost (mockup
  23's dialog, following their reconnecting). Now Playing with nothing
  queued: "Nothing playing", Open Library, Shuffle all.
- **The coach cards** (first boot, once; About and `uic` show them
  again): the three red buttons' clicks and holds, then "tap the tab
  you're on again: back to its start".

**The stage-1 leftovers** were fixed with the Library (the section above:
the two-line play-next toast, the per-frame step for very fast drags, the
tab bar's texts measured in a host test); their host tests still pass. The
queue-jump review holds: every new path that moves the current entry logs
a line (`[ui] queue: removed 2 (the playing one too: the next plays)`,
`[ui] queue: cleared (16), stopped`, `[ui] shuffle all: 77 tracks`, the
row bar's Play as before), next to `[queue] now at ...`.

**To check on the device:**

1. First boot after flashing: the two tips; "Next", then "Got it"; a
   reboot doesn't show them again; Output > About > "Show the tips again"
   does. The arrows on the first card should point at the three red dots.
2. Queue: the header's summary (after a few tracks have played, the
   minutes); Edit, select three, the bar: Remove (Undo on the note), Play
   next; Clear... > Clear up next (the playing song keeps playing) and
   Clear queue (the red confirmation, the music stops, Undo brings the
   queue back stopped). While selecting, scroll the list: the bar must
   not move or flicker (the LCD's scroll band ends above it), and the
   scrollbar must stop at the bar. Leave the tab while selecting: the
   next page's list uses the whole band again.
3. Library > an album > + Queue, then the Queue tab: it opens at the added
   tracks, highlighted (a dot); the next visit doesn't highlight them.
4. A file that can't be played (rename a .txt to .mp3 in an album): the
   amber note, the next track plays, the row keeps its "!".
5. Output with the headphones: Disconnect (the music, if it played on
   them, pauses and moves to the speaker; nothing reconnects until
   Connect), Connect ("Connecting... try 1 of 3", then Connected with the
   codec and the delay), Cancel while connecting (the speaker plays on if
   it was playing), Forget (one tap: "Tap again" in red for 3 s; a second:
   "No headphones paired"), the volume chips (each output's own sheet).
   Hold B on the speaker with the headphones off: the icon turns amber,
   the music stays on the speaker; hold B again: cancelled.
6. Pair new headphones with another headset in pairing mode: the list
   fills in (kind, signal bars); tap it, confirm: the card shows
   "Pairing...", then Connected; after a reboot it reconnects to the new
   one. With a device that won't pair (not in pairing mode): "Couldn't
   pair", and the old headphones are still remembered (Connect reaches
   them). Watch `[stats]` for underruns if headphones were streaming
   while the scan ran.
7. Switch the headphones off while playing on them: the dialog, its line
   following the reconnecting ("try 2 of 3", "Looking for them..."), Use
   speaker (paused), or switch them on again (the dialog closes, still
   paused).
8. Without a card (or with an empty /music): the no-card state on the
   Library, the Queue and Now Playing (with nothing queued), Try again
   with and without a card.
9. `ui` after all this: the Bluetooth link and session line, and the
   loop task's unused stack (the Output page's rows and the empty states
   draw with a few hundred bytes of locals).

### The review's fixes (before the device)

A review of the uncommitted stage found these; each was checked against
the code first. Host-tested where it is portable; nothing here has run on
the device yet either.

- **Bluetooth.** Picking the headphones linked now on the Pair screen
  (multipoint sets stay discoverable) moved the audio to the speaker and
  left a drop expected for ever, so a real drop later went unnoticed:
  they are left out of the list, picking them anyway only makes them the
  output, and a drop is expected for 10 s at most while the link stays
  up. Pairing new headphones while others were linked was undone on the
  next loop pass (the old link still up looked like the answer): the
  pairing now waits for a new link, and the Connected event and the
  link's phase answer the same in either order, so the new name is kept.
  Disconnect while the old link was being let go for a pairing left
  "Pairing..." up for good: it ends the pairing now. Forget from the
  screen was undone by the next boot or B hold (a scan by the saved
  name): it now stops every scan by name until the next pairing. A
  connect with none remembered showed "No headphones paired" with no
  Cancel and waited for ever: "Looking for..." with Cancel, failed after
  30 s. A B hold that cancelled a connection paused music that was still
  on the speaker: it plays on now, as the Speaker row does.
- **Feedback.** "Now playing on SPYDRONE" and two ticks when the
  headphones asked for connect; a long buzz with the lost dialog; inert
  A/B/C clicks (nothing queued) buzz twice, short, instead of the tap
  tick; the volume sheet ticks on every control tap and when a tap above
  it closes it.
- **Touch.** The volume sheet changed the volume on touch-down, so a
  quick second tap on the Speaker chip landed on its slider (80-100 %):
  it changes on a tap or a drag only, and ignores a touch in its first
  300 ms. The toast over the header ate taps on ‹: the header's ‹ (and
  its pill, beside a toast with no buttons) works through it.
- **Covers.** A worker exiting could be handed a job in the gap between
  its two stores and delete itself with it queued (no cover again until
  a reboot): it says it's gone before it says it's idle. A frame header
  past the first 64 KB (EXIF, XMP, a Photoshop block, an ICC profile)
  marked a decodable cover undecodable for good: the whole file is
  walked.
- **Lists.** In whole-row mode (the audio short of time) the rounding
  after the per-frame cap could make a 125-line step, a full redraw:
  it's part of the step now (`ListLayout::stepTowardRows`, tested over a
  grid of offsets).
- **The Queue's minutes.** A skip changed the entry before the backend
  started it, so the old track's length could be noted for the new one:
  a length is noted only once this entry has started.
- **Drawing.** A dialog closing over the coach cards drew the page's
  header over them: whatever a modal covered is drawn again (the coach,
  the jump grid, or the page, and the toast). The Folders header's counts
  ran under "‹ Library" two levels down: they end before the pill.
- **Cut texts** (measured with the firmware's fonts, now host tests):
  "Remove 2" (Remove is wider; a big count drops the icon, never the
  number), "Open Lib…" (the primary button is wider), Now Playing's empty
  line, the coach card's title and "Play / pa…", "Tap again", the volume
  chip at 100 %, the Bluetooth status lines, "Here until SPYDRONE
  connects" (falls back to "Waits for SPYDRONE"), the Pair hint, the
  line-out line, About's long values (Small when Body doesn't fit), the
  lost dialog's title (a long name goes into the body), the Queue header
  ("4 of 16 · 49 min" keeps the position when the long form doesn't
  fit), and the play-next toast's name (two lines with the buttons as
  icons: 216 px).
- **Design grafts built.** The Bluetooth "..." sheet (Disconnect, Pair
  new headphones, Forget in red with a dialog) instead of Forget beside
  Disconnect and the volume; sheets' ✕ pill, and the first row no longer
  looks like the main choice (only the Library's Play is); Now Playing
  names the output again ("4 of 16 · SPYDRONE").

**Also to check on the device** (with the list above):

10. Pair screen with the headphones connected: they aren't listed (a
    multipoint set). Pair other headphones while connected: the card
    goes "Pairing...", then Connected to the new ones, and a reboot
    finds the new ones (the saved name). Cancel during the change-over:
    the card says "Not connected", not "Pairing...".
11. Forget (Output > "..." > Forget > Forget), reboot: nothing
    reconnects; hold B: "No headphones paired yet". Pair them again:
    back to normal.
12. The play-next toast with a long title: two lines, the Undo arrow and
    the Queue icon at the right; both work, and a tap on the name only
    dismisses it. With a toast up on a Library page, ‹ goes back.
13. Double-tap the Speaker volume chip: the sheet opens and the volume
    doesn't move.
14. Connect the headphones from the card: "Now playing on ..." and two
    ticks when the music moves. With nothing queued, A/B/C buzz twice.

## Stage 2 on the device (27 September 2026)

The Now Playing, Library, Queue and Output screens, the states and the
coach cards, run on the Core2 for the first time. Everything was driven
from the console in silent test mode (`z`: the speaker at volume 0, the
headphones connected but never the output) with the scripted finger
(`uit`/`uih`/`uis`/`uid`). The card's library is 77 tracks, 6 artists, 6
albums; `uil10000` and `uil2000` stood in for a big one. Screenshots of
every screen are in the session's `ui_shots2/` (with a contact sheet).
Nothing was paired, forgotten, disconnected or connected: the states a
test can't cause safely were shown with a display-only fake (`uiF`,
below).

**Boot and memory** (internal RAM free):

| | stage 2 | stage 1 |
|---|---|---|
| after the library and queue | 91K (min 85K) | 91K |
| after the UI | 89K (the UI takes 0.7 KB; PSRAM free 2914K, the covers' LRU included) | 90K |
| idle, UI up, stopped | 83-86K (min 78K); 76K just as the headphones link | 81K (min 75K) |
| playing MP3 | 63K (min 56-57K) | 63K (min 58K) |
| playing FLAC | 64-65K | 66-75K |
| lowest seen, 10-minute soak | 50K | 58K |

The lowest point is new. Stepped through by hand, it came from three
things at once: the cover worker's 6 KB stack (alive while it reads or
writes the card), a queue save (a Library Play next, then Undo) and a
Folders page. Together they reached 51K; each alone stays at 55-57K. The
worker now ends 3 s after its last cover instead of 10, which narrows
the window. Its stack's high-water mark over the whole session was
2.3 KB of 6 KB. The stack was kept at 6 KB because the largest-JPEG path
(folders without a well-named cover) never ran.

**Screens** (compared with the mockups in `navui/tabs`):

- **Now Playing:** playing, paused and empty; its "..." sheet; the volume
  sheet and the volume HUD; Dance.
- **Library:**
  - the Artists list, with the card's library and with 10,000 synthetic
    tracks;
  - the jump grid, its second level ("L: 18" > La/Li/Lu) and the list
    after a jump;
  - an artist, an album, the inline actions, a long-press sheet;
  - the play-next and added toasts;
  - Albums with covers, Folders, and a folder's files.
- **Queue:** the added tracks' dots, selection mode, its Remove toast, the
  Clear sheet, the red confirmation, the queue after Clear up next, and
  the empty queue.
- **Output:**
  - connected, and the lower rows (line out, Pair, Haptics, Calibration);
  - About (two screens) and the Bluetooth "..." sheet;
  - connecting, searching and pairing (faked);
  - Pair headphones, searching (nothing else was in pairing mode nearby).
- **Other states:** no card (faked, on Now Playing), the headphones-lost
  dialog (faked), both coach cards.

Three differences from the mockups are by design: each tab has its own
accent colour (purple Library, teal Queue), the toast sits at the top,
and sheets have no row icons.

**Fixed on the device:**

- **The Folders header:**
  - the second line's fill cut the title's descenders ("Discoverv");
  - two levels down, it showed "/Daft…" beside "14 audio files, 1…".

  Now the counts are always drawn whole. The path is drawn only when at
  least 60 px are left beside them. The title is drawn last.
- **The two-line play-next toast:** each line's fill cut the other line's
  descenders and the box's border. Its lines are now drawn as glyphs only
  (`Fonts::draw` with bg == fg: no fill, blended with the sprite).
- **Queue selection mode:** the playing row had no checkbox, because the
  row was refilled after its ring was drawn. The row is now tinted through
  `tinted()`, like the other rows.
- **Long list frames while an MP3 plays.** The per-frame step works: no
  move is over 84 lines. But most motions still had frames of 35-90 ms.
  The `[ui] slow frame` log showed the cause: one row's render (~5 ms)
  stretched to 20-39 ms. The audio decoder, which runs above the loop,
  ran in the middle of it. **`ListView::renderAhead()`** now renders the
  next row in the direction of travel between frames, when the next frame
  is 15 ms or more away. The frame that shows that row then only pushes
  it. Queue, MP3 playing:

  | | before | after |
  |---|---|---|
  | slow drag (250 px/s) | mean 8.4, max 35 ms | mean 2.4-3.8, max 11-13 ms |
  | fling (1,000 px/s) | mean 10.7, max 42-88 ms, ~12 frames over 30 ms | mean 3.4-3.9, max 19-21 ms, none over 35 |
  | fast fling (2,300-2,800 px/s) | max 41-54 ms | max 46-48 ms, 1 frame over 35 |
  | very fast drag (3,500-5,000 px/s) | max 41-60 ms | max 36-53 ms, 1 frame (two new rows at once) |

  With FLAC playing, drags and flings peak at 11-29 ms; fast flings have 2
  frames of 36-38 ms.
- **Covers took 0.6-2.6 s each while an MP3 played.** The worker ran at
  priority 0, where it shared with the idle task whatever CPU the loop and
  the decoder left. It now runs at the loop's priority while nothing
  moves, and drops to priority 0 as soon as a list moves. The decode
  times below come from `uiT`, which decodes the covers again. The
  `[thumb]` lines now split the time into read, decode and card copy.

  | cover | before | after |
  |---|---|---|
  | Aphex Twin, 200 x 197, 8 KB | 894 ms | 364 ms |
  | Moon Safari, 200 x 200, 32 KB | 1348 ms | 490-800 ms |
  | Graduation, 250 x 250, 20 KB | 1264 ms | 559-838 ms |
  | OutRun, 1000 x 1000 at 1/8, 59 KB | 1898 ms | 809-983 ms |
  | Discovery, 650 x 565 at 1/4, 46 KB | 2619 ms | 1062-1473 ms |

  While the covers decoded, the Albums list scrolled with frames of at
  most 25 ms, no underruns and the ring full (1439 ms). Most of the time
  is the decode itself. The 96 px size decodes at 1/2 or 1/4, which runs
  the IDCT that the spike's 1/8 skipped. Writing the card copy costs
  70-280 ms. Only the first view of each album pays this; the next boot
  reads the card's copies.
- **Two MP3s started 4 s late and froze the UI** (the soak found it). Air's
  "New Star in the Sky" and "Le voyage de Pénélope" have 351 KB ID3 tags
  (a picture). ESP8266Audio's ID3 reader walks a tag one byte per read, on
  the decode task, which runs above the loop. A tag over 16 KB is now
  skipped (the library has the title), and both tracks start in 23-30 ms.
  The problem predates stage 2.

**Flows by scripted touch** (all behaved as designed):

- **Library:**
  - Play from an album's hold sheet: 14 tracks, from the one tapped.
  - Play next: the toast and the badge flash; the Queue opens on the
    added track, marked with a dot.
  - + Queue, then View on the toast: the Queue scrolls to the added
    tracks, marked.
  - Folders "Play all 77".
  - The A-Z grid on the synthetic library, to a letter and to a
    second-level key.
  - Long-press sheets on albums and tracks.
- **Queue:**
  - Remove in selection mode, with Undo.
  - Clear up next: the playing track stays.
  - Clear queue: the red confirmation, then the music stops; Undo brings
    the queue back, stopped.
  - Shuffle all, with Undo.
- **Now Playing:**
  - "..." > Show in folders: the folder opens on the playing file,
    tinted.
  - The volume sheet opened and closed. Its controls weren't touched,
    because they would move the headphones' volume.
- **Output:**
  - The Bluetooth "..." sheet opened and closed.
  - Pair opened (the scan started) and left (`scan stopped`).

**10-minute soak** (`soak2.py`, 11 cycles, MP3 then FLAC). Each cycle ran:

- every tab, including Dance, and Output scrolled;
- Now Playing's two sheets;
- Queue flings and very fast drags;
- the Library's three lists and drill-downs, a hold sheet, Play next +
  Undo, and Folders;
- a skip;
- every third cycle, the covers decoded again;
- for four cycles, the synthetic 2,000-track library with its jump grid.

Results:

- No crash and 0 underruns. The ring stayed at 1428 ms or more during
  scrolls, and tracks started in 25-57 ms.
- 65 motions and 2,229 frames at 22.9 fps on average. The short drags
  pull that down; flings run at 29-30 fps.
- 20 frames over 35 ms (0.9 %): the first frame of each very fast drag,
  and one in some long flings.
- The governor stayed "normal" throughout.
- Stacks: the loop's never went below 5,272 B unused of 8 KB; the
  worker's never below 3,844 B unused of 6 KB.
- No heap drift. Internal RAM with FLAC playing was 65K before and 64K
  after. PSRAM free was 2814K before and 2811K after the synthetic library
  was dropped (`g0`).

The soak ran on the firmware from before the worker's 3 s exit; the
flashed firmware is otherwise the same.

**Diagnostics added:**

- `uiT`: decode the covers again, with their timings.
- `uiV`: show the volume HUD.
- `uiF<c/s/p/l/n>`: show a faked Bluetooth or no-card state (display
  only); `uiF0` goes back to the real one.
- The `[ui] scroll:` line now counts frames over 35 ms.
- `[ui] slow frame`: logged for a frame over 50 ms, with its move and its
  renders.
- `ui` reports the rows rendered ahead.

**Still open:**

- `sdCommand(): crc error` appeared twice in the session, both while the
  cover worker was writing card copies, and none in the final soak. The
  SD driver's retry recovered each time. If it comes back, suspect the
  card copies' writes: they are the one new card write while music plays.
- About reads "microSD card, 1023.6 GB" (from `SD.totalBytes()`): check
  that against the card's label.
- The Queue's minutes read "N+ min" until each track has played once.
- The first frame of a very fast drag, which renders two rows at once,
  still takes 35-55 ms.
- Only the user can check:
  - the haptics;
  - real fingers (the scripted finger bypasses the panel and its
    calibration);
  - the headphones' own volume keys;
  - everything in the checklists above that needs pairing, forgetting, a
    second headset or pulling the card.

## Waiting for the headphones (27 September 2026)

The user found the player "stuck in connecting" after the Core2 sat all night: the
headphones had dropped while idle, the output stayed Bluetooth, and Play went to
"Playing" at 0:00 with no timeout. Fixed in 26c50fd (PlayGate and a Waiting
player state; see ARCHITECTURE.md). Checked by hand with the Powerbeats:

| Step | Result |
|---|---|
| Play with the headphones just out of the case | waiting, paged at once (try 1 of 3) |
| The link came up | after 6.8 s; playing on the headphones, resumed at 18.5 s, "Now playing on SPYDRONE" |
| Stream start | 1.9 s after the link |
| Late AVRCP: dip, then the headphones' volume | 30 %, 4.5 s after the link |
| Headphones into the case while playing | they suspended the stream first: paused at once; then the link closed |
| The lost dialog | shown; OK closes it; output stays Bluetooth, background reconnect running |
| Underruns | 0 |

Earlier, with the headphones unreachable (speaker at 0 %): the 20 s give-up and
its notice, Cancel, [Play on speaker] from both places (the background search
keeps running), a skip while waiting, and Play during a background scan (the
status reads "try 1 of 3" at once).

## The button strip: no more presses from swipes (27 September 2026)

The user saw random pauses while scrolling the Queue. The log caught it: a
downward flick ran off the screen into the touch-button strip, and M5Unified
turned the end of it into BtnB (and, sliding right, BtnC) clicks. The buttons
are now derived by `StripButtons` (lib/core) from touches that go down in the
strip and stay within 20 px; glass-started touches, strip touches that move,
and strip touches re-found within the bounce window after a swipe never press
a button, and each rejection logs `[button] ignored: ...`. M5.BtnA/B/C are no
longer read outside the input lab.

On the device: the captured swipe replayed, 2 minutes of scripted flicks
ending in the strip (72 flings, 33 re-found presses) gave 0 button events;
deliberate presses, holds and A/C repeats unchanged. By hand, the user's
flicks off the bottom edge were all ignored (11 touches: 9 strip-started and
moved, 2 swipes from the glass that ended on a red dot) and playback never
paused. Swipes that start on the strip didn't scroll the list: the next section.

## Swipes from the strip scroll (27 September 2026)

In that hand test the user's scrolls often began on the strip, just below the
list ("ignored: B at 174,243: moved off the button (35 px)"), so the list
didn't move. Now a strip touch that moves beyond the 20 px slop **upward** (at
least as much up as sideways, or onto the glass by then) is handed to the glass
recogniser as a drag (`StripButtons` says `scroll`, Input calls
`TouchRecognizer::fromStrip()`): a DragStart at the hand-over point with no Down
before it, so the list moves by the finger's movement from there (no jump by
the slop), then DragMove, DragEnd and a Fling capped at 2,000 px/s as usual.
Every event of it carries `InputEvent::fromStrip`; it never becomes a Tap or a
LongPress, presses no button and plays no tick. The log says `[button] B at
x,y (raw): a swipe from the strip (n px): scrolling` instead of the ignored
line.

What is drawn just above the strip must not take it for a press: the Ui routes
it to the page only (nobody's while a sheet, dialog, the volume slider, the jump
grid or a tip is up: none of them scrolls, and the slider must not jump), the
list pages hand it to their `ListView` (the Queue's edit bar and the A-Z rail
aren't pressed; the list starts its drag there, stopping a fling like a finger
landing), and Now Playing ignores it (its transport is the bottom row). What
stays: a slide along or down the strip is ignored; a still press is a click or a
hold, including one that rolls up to 20 px (even across y 240); a press that has
held (500 ms) never scrolls; glass-started swipes that end in the strip press
nothing, and a bounced strip touch presses nothing (but a swipe up from it still
scrolls: quick repeated flicks from the strip). Host tests replay each case
(`test_touch_input`). The scripted finger takes the same path:
`uis160,265,160,100,120` flings the list from the strip. Not yet tried by hand.
