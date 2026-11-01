# v0.11.0 pre-release device checks

The candidate: `main` at 7cdfb88. These PRs were merged since 0.10.0
(the release notes' "What's new in 0.11.0"):

- #20: `NEXT_RELEASE` 0.11.0;
- #22: Auto DJ (docs/AUTODJ.md):
  - the AutoDJ index's format, MPDJ v2 (docs/AUTODJ-INDEX.md), and
    `read.mpdj=2` in `/.player/device.txt`;
  - the engine, the join and the queue hooks;
  - its UI in ten languages: the "..." row, Output > Auto DJ, the empty
    states' button, "Auto DJ from here", the Queue's DJ marks;
  - NVS schema 4 (the `autodj` namespace);
  - the Undo of a Play or a Clear never starting sound;
  - messages on two lines placed by their letters;
- #21: the console's `gs` reads a piece a loop pass, and a list on a dark
  screen holds no background work.

Run on 2026-10-16 on the user's Core2 v1.3 (COM3), through the serial
daemon, with the user's own card. The card holds 113 tracks, is FAT32, has
no transfer data (no `/.mstream`, so no Auto DJ index), and 0.10.0 was the
last version to use it. Silent mode (`z`) was sent after every boot,
before anything played. No Bluetooth or power command was sent (`B`, `P`,
`c`, `f`). The user's state was recorded first and put back at the end
(section 8). Every flash wrote the four built files with
`--flash-mode keep --flash-freq keep --flash-size keep` and
`--after no-reset`, so each first boot was logged.

**Result: no failure, no crash, no underrun, no card error, no code
change.** Auto DJ without an index behaved as designed (section 5). Three
figures are recorded as findings (section 9):

- Opus decoding is 4.5-4.6 % slower than 0.10.0's in both flash modes.
  G1 still passes on QIO, at 2.60x realtime.
- A FLAC album outside the template's queue had passes of 41-47 ms
  against G6's 30 ms, with 0 underruns.
- The loop task's stack is 36-180 B lower than 0.10.0's, far from LOW.

Two wordings were found: one in the release notes (fixed before the
release), one on the console.

These earlier device runs are not repeated here:

- **Auto DJ with an index** (docs/AUTODJ.md section 14): three device
  checks on the synthetic 19,412-track card, with an index made by the
  repository's tools, on 2026-10-13 to 16 (the builds of `09f91a1`,
  `a7ee54b` and `314c624`). They covered:
  - the join's time and memory, the pick's CPU at 240 and 160 MHz, and
    loosening;
  - a two-hour soak of 7,520 picks, and `queue.txt` keeping up;
  - library updates during a session, the card pulled and put back, and
    the index replaced, damaged, broken and missing;
  - the end of the queue, and Undo from paused, stopped and playing
    queues;
  - the UI in all ten languages, the screens without an index included.
- **#21's `gs` and the dark list** (docs/METADATA.md 3.8, on the
  synthetic card, 2026-10-16): every `gs` that met a compaction waited it
  out, the `[stats]` line kept its spacing inside every `gs`, and a
  library update with the screen off saved 1.8 s after its build.

## 1. The build

**Host tests:** CI's `build` job on 7cdfb88 ran `pio test -e native`:
**1,772 of 1,772 passed** (0.10.0's: 1,586). Both images built there
too.

**Firmware:** built locally from a fresh clone of 7cdfb88, from
PowerShell with MSYSTEM removed, with no `local.ini`. The packages and
the dependency graph are 0.10.0's.

| | `core2` (QIO: the main image) | `core2-dio` (the `-dio-full.bin`) |
|---|---|---|
| Build | SUCCESS in 12:34 | SUCCESS in 11:24 |
| `version:` | `v0.10.0-16-g7cdfb88`, ELF 071ad41a | the same version, ELF fbe38934 |
| `iram_diet:` | 51 of 51 libc objects moved, the MP3 hot set pinned | the same |
| `cache_guard:` | ok: at most 2 hot lines in a set (bench and playback) | the same |
| `flash_guard:` | `app 4.16 MB = 69% of the 6.00 MB slot; ... flash QIO at 80 MHz` | the same, `flash DIO at 40 MHz` |
| RAM, Flash | 57,680 B; 4,269,083 B (67.9 %) | 57,680 B; 4,269,115 B (67.9 %) |
| `firmware.bin`, `firmware.factory.bin` | 4,358,544 B, 4,424,080 B | 4,358,576 B, 4,424,112 B |
| `bootloader.bin` | 24,992 B, header `e9 03 02 4f` | 23,520 B, header `e9 03 02 40` |
| `firmware.bin` header | `e9 06 02 4f` | `e9 06 02 40` |
| sha256 `firmware.bin` | `f1725d037add4ed5cb31d73355e6c8cff70ad381702ca6f27da1d773c3298d85` | `6faf3074e4b38f97d56be3758f9df86aa899ca6178f523f1c0f11158e69f7ccd` |
| sha256 `bootloader.bin` | `3ae837b726c1d3e860c24391b70624d39b4715524cd7509c164d859bb8290914` | `8b956c0cdaccc35f0df06f12e717ac2e465cee3a59d890b035d651638492c3ff` |
| sha256 `partitions.bin` | `d978ecf9a50caff5c13b95dec5b09b14691241c3067edba7e03bf1398366ef31` | the same |

Each image was its own `pio run`, and each one's time includes its
libraries' download into the fresh clone (0.10.0's two took 10:12 and
14:44 in one run).

The bootloaders and the partition table are 0.10.0's, byte for byte.

The app grew by 84,064 B from 0.10.0's 4.27 MB, to 4.36 MB: Auto DJ (its
engine, the join, its UI and its 51 texts in ten languages) and #21's
stepped `gs`. Static RAM grew by 232 B.

## 2. The first boot of 0.11.0 on a card 0.10.0 used (QIO)

| Check | Result |
|---|---|
| ROM, banner | `rst:0x1 (POWERON_RESET)`, `mode:DIO, clock div:1`; `v0.10.0-16-g7cdfb88 (commit 7cdfb88, 2026-10-16), ELF 071ad41a`; `running ota_0` |
| The settings | `[nvs] schema 3 -> 4: migrated`; `[ui] language: en (English)`; no language picker |
| The boot screen | The logo and the version in 41 ms |
| The update from 0.10.0 | `/.player/device.txt written (firmware 0.10.0-16-g7cdfb88)`: its text changed (the version, and `read.mpdj=2` where 0.10.0 wrote `read.mpdj=1`). `library.idx matches the card: load (its inputs are the card's) in 10 ms`: no rebuild, as the library's rules didn't change; 113 tracks, 7 artists, 10 albums; **browsable 81 ms after the mount**, the `device.txt` write included |
| Auto DJ | `[dj] settings: Auto DJ off, Follow the vibe, strictness 55 %, BPM continuity off, Harmonic mixing off (none saved: the defaults)`; `[dj] index: none (no transfer data on the card): picks at random`; `D`: `the engine 19016 B, the join 0 B` |
| The queue | `restored 10 of 10 tracks from /.player/queue.txt (0 no longer there), at 1 of 10` |
| Memory | <ul><li>`[heap] PSRAM's lowest free: 2609632 B over setup()`: 23,460 B under 0.10.0's 2,633,092 B, mostly Auto DJ's engine (19,016 B) and its glue (3,680 B).</li><li>`[heap] ui internal free=79K min=75K`, as 0.10.0.</li><li>`[diag] Library SD, 113 tracks, 92% tagged`.</li><li>The loop task's stack: 3,952 B the lowest (section 9).</li></ul> |
| The background walk | `0 added, 0 changed, 0 gone`, 20 steps in 90 ms; no scan, no update step |

## 3. The second boot (QIO)

- `[nvs] schema 4`.
- `library.idx matches the card: load (its inputs are the card's) in 10 ms`, browsable 40 ms after the mount (0.10.0: 10 ms and 38 ms).
- No `device.txt` write: its text was the same.
- The queue was at its place; the boot screen in 42 ms.

## 4. The benches (QIO, silent mode, by path, playback stopped)

The same four files as 0.10.0's checks. The figures are the second run's,
once the boot's walk was done and Bluetooth was resting.

| Track | 0.11.0 candidate | 0.10.0 |
|---|---|---|
| A 44.1 kHz MP3, twice | `4.22 s = 4.8x` realtime both times (21.0 % of a core); `libmad's state: pinned at 0x3f831c20, PSRAM, its lower 2 MB` | `4.11 s = 4.9x`, `4.09 s = 4.9x` |
| A 44.1 kHz FLAC | `4.29 s = 4.7x` (21.4 %); decode stack free 12,836 in the first run, before any Opus | `4.33 s = 4.6x`, 12,840 |
| `ms128k.opus`, decode only | `6.06 s = 3.3x` (30.2 %), stack free 4,292 | `5.80 s = 3.5x` |
| The same, decoded and converted to 44.1 kHz | `7.72 s = 38.5 % of a core`, the converter 8.3 % | `7.51 s = 37.5 %`, 8.6 % |

A first run right after the boot (Bluetooth paging during the MP3s, the
card walk during the Opus decode) was 0.01-0.02 s slower: the MP3 4.24 s,
the FLAC 4.31 s, the Opus 6.07 s, converted 7.73 s.

- The FLAC is 0.9 % faster.
- The MP3 is 2.9 % slower and the converted Opus 2.8 % slower: within
  the ±3 % that code layout has moved these figures before.
- The Opus decode alone is 4.5 % slower, outside that range (section 9).
  G1 (decoded and converted at least 2.5x realtime, at most 40 % of a
  core) still passes: 38.5 % is 2.60x.

## 5. Auto DJ without an index (QIO)

What every card gets until the transfer software is out. This is
docs/AUTODJ.md section 14's check 14 (the user's own card, no transfer
data). The screen was woken with `uiq` and driven from the console (`ui`
to read the page, scripted drags and taps, `X` for screenshots); no tap
went near Output's Pair new headphones row.

| Check | Result |
|---|---|
| Output's row | "Auto DJ" over "Off" |
| Output > Auto DJ | <ul><li>`ui`: `page: Output (AutoDj): Auto DJ Off, Follow the vibe, 55 % (shown 55 %), BPM off, key off; the index none (not usable: the settings dim)`.</li><li>On the screen: Anchor, Match strictness, BPM continuity and Harmonic mixing dim, "needs an Auto DJ index" under each, their values kept beside the titles ("Follow the vibe", "55% or closer").</li><li>The status: "No Auto DJ index on this card" over "picks are random".</li><li>A tap on the dim Anchor: the amber note `Needs an Auto DJ index: a transfer to the card adds one`, the value unchanged.</li></ul> |
| The empty states | After `qx`: the Queue shows "Your queue is empty" with [Open Library] [Shuffle all], and Now Playing "Nothing playing" with [Open Library] [Shuffle all]. No Auto DJ button |
| `D1` on a queue of 4 tracks | `[dj] Auto DJ on: it adds a track when the queue's last one starts (a finished queue: one cued, paused) (saved)`, a new session with no seed track, and at the same moment `[ui] toast: No Auto DJ index: picks are random` |
| The picks | <ul><li>`i3` (the last entry): a pick 0.56 s later, once its 500 ms were buffered (at 492 ms): a 44.1 kHz FLAC, `[by track id: no index]`, 0 reads, 0.07 ms.</li><li>`n` to that pick, now the last entry: a second pick 0.28 s later, by track id, 0 reads, 0.05 ms.</li><li>No second toast.</li><li>`D`: `picks 2 ... reads at a pick 0/1/2 2/0/0 ... random 2 (by id 2)`, `cooled 0 rows, 0 artists, 3 by id`, `DJ marks 2`, `memory: the engine 19016 B, the join 0 B`.</li><li>`q` kept `undo: add` (the listener's own) through both additions.</li></ul> |
| `D0` | `[dj] Auto DJ off: what it added stays and plays out (saved)`. Both later boots: `[dj] settings: Auto DJ off, ... (saved)` |

The second pick added a track that was still waiting in the queue (an
entry `i3` had jumped over, so it never started). That is the design: the
cooldown by track id holds the tracks played or picked (docs/AUTODJ.md
3.2, 3.12).

## 6. Playing, with the library work under music (QIO, about 9.5 minutes)

The queue: an MP3 album, a FLAC album and the test folder's mixed album
(35 tracks), repeat all, the speaker in silent mode, Auto DJ off. The
screen was dark throughout, on Now Playing.

| Check | Result |
|---|---|
| `gb` while the MP3 plays (twice) | `built in 138 ms` / `141 ms on the card worker (the loop live; the fence up 227 ms` / `221 ms)`, `library.idx saved in 66 ms` / `60 ms`, `carried through queue.txt: 35 of 35 tracks still there, at 1`, `(Auto DJ's join gave back 0 B)`; the track played on, 0 underruns (0.10.0: 125 / 124 ms, the fence up 216 / 191 ms, saved in 56 / 63 ms) |
| Rescan (`gr`) while a FLAC plays | <ul><li>The Rescan's compaction (113 records, 18 folders), then the scan of 113 files and a second compaction (2 chunks merged).</li><li>The update step at the scan's end: `built in 107 ms (the fence up 210 ms)`, `35 of 35 ... at 15`, "Library updated" 3.65 s after `gr`, `library.idx saved in 38 ms` (0.10.0: 97 ms, the fence up 196 ms).</li><li>0 underruns.</li></ul> |
| `gs` twice (a FLAC playing, then an MP3) | <ul><li>From its first lines to the device's tags file checked (the D line): 0.62 s and 0.58 s. From the command to its last line: 0.89 s and 0.90 s.</li><li>The `[stats]` lines around each came 5.00 s apart, as everywhere in the section (4.98-5.04 s), and `pass_max` didn't move.</li><li>It read `rows: 105 Scanned, 0 Software, 0 Pending, 8 Unreadable` and `T: no transfer data`; the card worker's 6 KB stack's least left 2,180 B; internal RAM's lowest while a step ran 53,012 B.</li></ul> |
| The mixed album, 5 minutes | `G`: joins 5 continuous, 2 after the tail, cuts 0, too late 0, opens failed 0 (0.10.0: the same). The Opus tracks ended on their EOS trims, 0 concealed, exactly their lengths |
| The whole of section 6 | <ul><li>114 `[stats]` lines while playing, `underruns=0` in every one (0.10.0: 87, over a shorter run).</li><li>`ram=` median 60 KB; `min=` 47 KB at the lowest (G5's floor: 42 KB).</li><li>`pmin=` 2,140 KB at the lowest (0.10.0: 2,163 KB).</li><li>`stack_free` 4,292 B at the lowest (0.10.0: 4,296 B).</li><li>`pass_max` 29,238 us at the highest, on the mixed album's first FLAC (track 32 of 35), where 0.10.0's 29,595 us came too.</li><li>`load=` at most 47.1 %.</li><li>No Guru, panic, backtrace, reset, `task_wdt`, LOW stack line or `sd_diskio.cpp` line.</li></ul> |

Each `library.idx` save came 0.2 s or less after its build, the screen
dark. The 29.2 ms covers the 35-track queue: a FLAC album played after it
had longer passes (section 9).

## 7. The DIO image

Flashed, then one boot:
- `mode:DIO, clock div:2`, ELF fbe38934, ota_0;
- `[nvs] schema 4`; `[dj] settings: Auto DJ off, ... (saved)`;
- `library.idx matches the card: load` in 10 ms, browsable 45 ms after the mount (0.10.0's DIO: 43 ms);
- the queue the QIO session left (48 tracks) restored at its place, with its resume point.

| Track | DIO | 0.10.0's DIO |
|---|---|---|
| The MP3, twice | `4.70 s = 4.3x` (23.4 %) | `4.49 s = 4.5x` (22.4 %) |
| The FLAC | `4.37 s = 4.6x` (21.8 %) | `4.45 s = 4.5x` (22.2 %) |
| `ms128k.opus`, decode only | `6.89 s = 2.9x` (34.4 %) | `6.59 s = 3.0x` (32.9 %) |
| Decoded and converted | `8.72 s = 43.6 %`, converter 9.1 % | `8.46 s = 42.2 %`, converter 9.3 % |

The FLAC is 1.8 % faster and the converted Opus 3.1 % slower. The MP3 is
4.7 % slower and the Opus decode alone 4.6 % slower (section 9); the MP3
is still faster than 0.9.0's 4.79 s.

**Playback:** two minutes of the test folder's mixed album (MP3, FLAC,
Opus) on the speaker:
- 26 `[stats]` lines while playing, `underruns=0` in all (0.10.0: 25);
- `pass_max` at most 21,989 us; `load=` 51.3-52.1 % on Opus (0.10.0:
  51.0-51.4 %), under G2's median of 55 %;
- the Opus tracks ended on their EOS trims, 0 concealed;
- no `[E]` line.

As in 0.10.0's run, the playing section ran on the QIO image before the
DIO image.

## 8. The user's state, put back

**The queue:** the user's queue as recorded at the start of the run (10
tracks of one album, at entry 1, stopped, no resume or start point,
shuffle off, repeat off) was rebuilt on the DIO image, in silent mode:
1. `qx`, then the album;
2. `qR0`;
3. `i0`, then `b0` (the bench stops the player);
4. `qs0`.

Afterwards, `q` showed `10 tracks ..., at 1, 9 up next; shuffle off,
repeat off` and `resume point saved: none; start point waiting: none`.
After a last reboot: `repeat: off (saved)`, `restored 10 of 10 tracks
from /.player/queue.txt (0 no longer there), at 1 of 10`, `undo: none`.
The track list matched the one recorded at the start, path for path and
in the same order.

**Auto DJ** is off, as the user had it: `D` says `now: off`. Its `autodj`
namespace holds the defaults with Auto DJ off, saved by `D1` and `D0`.

**The card:** nothing was written to it from the computer. The firmware
rewrote its `/.player` files as any use does: `device.txt` (it names the
candidate until the release boots), `library.idx` (saved by the two `gb`
and the Rescan), `tags.bin` (compacted by the Rescan) and `queue.txt`
(the same tracks).

**The firmware and settings:** the flash (0x0-0x440000) and the NVS
(schema 3) had been backed up on 0.10.0, and were still the same before
this run (verified again: "digest matched"). The user asked to keep each
release on the Core2: the released v0.11.0 image is installed with the
settings (NVS, schema 3) the user had before the run, so its first boot
migrates them as an update from 0.10.0 does.

## 9. Findings

- **Opus decoding is slower again,** beyond the ±3 % that code layout has
  moved the benches before. Not blocking:
  - The decode alone: QIO 5.68 → 5.80 → 6.06 s and DIO 6.31 → 6.59 →
    6.89 s (0.9.0, 0.10.0, 0.11.0).
  - G1 (decoded and converted at least 2.5x realtime, at most 40 % of a
    core) passes on QIO at 38.5 % = 2.60x, 1.5 points from its limit. It
    has crept up from 35.2 % at Opus's M0 (docs/OPUS.md), then 36.6 %
    (0.9.0) and 37.5 % (0.10.0).
  - DIO is at 43.6 % = 2.29x, in G1's 2.0-2.5x band, as it was in 0.9.0
    (40.4 %) and 0.10.0 (42.2 %): G1 was set on the QIO image.
  - The DIO MP3 is 4.7 % slower than 0.10.0's, but faster than 0.9.0's,
    so it looks like layout. The Opus drift is steady.
  - Playback held: Opus `load=` on DIO 51.3-52.1 %, 0 underruns.
  - The app grew by 84 KB; the cause wasn't looked into. Worth an issue
    before the next feature grows the image.
- **FLAC passes of 41-47 ms** (G6: at most 30 ms, for FLAC as for Opus:
  docs/OPUS.md 8.11) on a 44.1 kHz FLAC album outside the template's
  queue, with 0 underruns:
  - 46.7 ms on Auto DJ's pick in section 5, reached by `n`;
  - with Auto DJ off, 41.0 ms and 47.4 ms on two tracks of the album
    (`i38`, then `n`), the second the file Auto DJ had picked;
  - `pass_max` is the track's running maximum: each came within the
    first 1-5 s of its track;
  - docs/METADATA.md 3.3.9 reads a pass over 40 ms as the decoder
    waiting for the card; the cause wasn't looked into;
  - not Auto DJ's: its picks took 0.05-0.07 ms with 0 reads;
  - there is no 0.10.0 figure for that album: 0.10.0's run never played
    it. The template's 35 tracks stayed at 29.2 ms. Not blocking, as
    0.9.0's 30.8 ms wasn't.
- **The loop task's stack** has a little less left than in 0.10.0, of
  8 KB, with no LOW line (LOW is under 1,536 B):
  - the first boot (the migration and the `device.txt` write): 3,952 B
    at the lowest, against 4,132 B at the released 0.10.0's first boot
    (2026-10-13; the 0.10.0 candidate's first boot printed no figure);
  - the QIO run: 4,292 B (0.10.0: 4,328 B);
  - the DIO boot and playback: 4,252 B (0.10.0: 4,348 B).
- **Wordings:**
  - The draft release notes said the "No Auto DJ index: picks are random"
    toast came at Auto DJ's first pick after a switch-on. It comes at the
    switch-on itself, by design (`AutoDj::setOn()`), and at the first pick
    after a start with Auto DJ on. **Fixed in the notes.** So too their
    "`device.txt`, written at each start": it is checked at each start,
    and written when its text changes.
  - The console, on a card with no index: each library rebuild prints
    `[dj] the library was rebuilt: 0 DJ marks carried, the session kept;
    the index joins when Auto DJ is switched on` (`AutoDj::live()`), which
    doesn't look at whether there is an index. Console only; not fixed
    here.

## 10. Not run

- **Auto DJ with an index on the release build:** this card has no
  index. docs/AUTODJ.md section 14 asks the release to run its checks on
  the release build; only its check 14 (section 5) could run here.
  Checks 1-13 ran across its three device checks on the synthetic card
  (listed at the top), on the builds before the release's. Since the last
  of them (`314c624`), `a3bc6f0`'s two fixes are host-tested only: the
  dim Match strictness row's value in German, Spanish, Italian and
  Portuguese, and the console's line for a slot found damaged at a pick.
- **The ten languages on this card:** this run used English. Auto DJ's
  screens ran in all ten on the synthetic card (docs/AUTODJ.md 14.1 and
  14.2).
- **Now Playing's "..." menu and a long press without an index:** the
  "..." sheet's Auto DJ row ran in all ten languages on the synthetic
  card; the long-press sheet without "Auto DJ from here" wasn't looked at
  on either card.
- **Going back to 0.10.0:** no boot of 0.10.0 on schema-4 settings ran.
  The release notes' "Going back to 0.10.0 later" comes from the code:
  0.10.0 leaves a newer schema as it is and reads what it knows.
- **The Bluetooth link:** no headphones were used. The firmware paged the
  remembered headphones by itself at each boot, then rested, as 0.10.0
  does.
- **The seek bar's touch checks** (0.7.0's section 4): its code didn't
  change.
- **The user's listening checks:** every agent run is silent.
