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
| Flick-scrolling a list while audio streams from the SD card on the LCD's SPI bus | Scroll lab | `w0`-`w3` |
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
  VLW fonts (`VlwFonts.cpp`, from `tools/vlw_font.py`), and `Spike`, which owns
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

### Input (with the user, still to do)

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
