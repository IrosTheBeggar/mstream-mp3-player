# v0.8.0 pre-release device checks

The candidate: `main` at 5b126bd (#7, the metadata work, and #8, the boot
screen, merged). What's new since 0.7.0 (the release notes' "What's new in
0.8.0"):

- the library from tags: the card worker, the validation walk, the tag
  scan, the update step and its fence, `library.idx` v6, the sector cache,
  Output's Library row and Rescan, the Library's status line, the "Disc N"
  dividers (#7, docs/METADATA.md);
- the queue's 5,000 cap, and the push-out of heard tracks
  (QUEUE-MODES.md 15);
- the patched SD driver (`lib/SD`) and the card guard (#7);
- the boot screen with the logo and the version, and Output > About >
  Device info (#8).

Run on 2026-10-09 on the user's Core2 v1.3 (COM3), through the serial
daemon, with the user's own card (113 tracks; FAT32). Silent mode (`z`) was
sent after every boot before anything played. The user's state was
recorded first and put back at the end (section 7). Every flash wrote the
four built files with `--flash-mode keep --flash-freq keep --flash-size
keep` and `--after no-reset`, so each first boot was logged.

**Result: no failure, no crash, no underrun, no card error, no code
change.** Two figures are recorded as findings (section 8): MP3 decodes
2-4 % slower than 0.7.0, and one decode pass of 30.005 ms against G6's
30 ms (0.7.0's soak had passes up to 32.7 ms).

The metadata work's own device batch (docs/METADATA.md 6.3, L0-L5, on
the synthetic 20k card of `tools/synthcard.py`, 2026-10-08 and 09) is not
repeated here: see METADATA.md and PR #7. It covered the 20k boots, the
sector cache's 30-minute write soak (1,283,498 hits verified, 0 stale), a
card pulled and put back, the scan, the update step at 20k with its
hearing-safety checks, and the card worker with a Bluetooth stream up
(digital silence only).

## 1. The build

Host tests (Git Bash, `pio test -e native`): **1,499 of 1,499 passed** on
5f440fb, whose tree is 5b126bd's (`git diff 5f440fb 5b126bd` is empty).
CI's `build` job passed on both PRs.

Firmware, from PowerShell with MSYSTEM removed, `local.ini` copied in:

| | `core2` (QIO: the main image) | `core2-dio` (the `-dio-full.bin`) |
|---|---|---|
| Build | SUCCESS in 5:46.6 | SUCCESS in 5:40.2 |
| `version:` | `v0.7.0-40-g5b126bd`, ELF 7e58c88a | the same version, ELF 5e8ce921 |
| `iram_diet:` | 51 of 51 libc objects moved, the MP3 hot set pinned | the same |
| `cache_guard:` | ok: at most 2 hot lines in a set (bench and playback) | the same |
| `flash_guard:` | `app 2.51 MB = 42% of the 6.00 MB slot; ... flash QIO at 80 MHz` | the same, `flash DIO at 40 MHz` |
| RAM, Flash | 57,176 B; 2,554,171 B (40.6 %) | 57,176 B; 2,554,203 B (40.6 %) |
| `firmware.bin`, `firmware.factory.bin` | 2,633,744 B, 2,699,280 B | 2,633,776 B, 2,699,312 B |
| `bootloader.bin` | 24,992 B, header `e9 03 02 4f` | 23,520 B, header `e9 03 02 40` |
| `firmware.bin` header | `e9 06 02 4f` | `e9 06 02 40` |
| `firmware.parts.json` | `flash_mode: qio`, `flash_freq: 80m` | `flash_mode: dio`, `flash_freq: 40m` |
| sha256 `firmware.bin` | `5e8a72d43891789ead23310caaebbef0299df0c14d012956be6bbb388b80a65b` | `c7950426ebee0f55f62759ae3013408de51ec8a86f1e8aa6b6fcfbe877f0ec07` |
| sha256 `bootloader.bin` | `3ae837b726c1d3e860c24391b70624d39b4715524cd7509c164d859bb8290914` | `8b956c0cdaccc35f0df06f12e717ac2e465cee3a59d890b035d651638492c3ff` |
| sha256 `partitions.bin` | `d978ecf9a50caff5c13b95dec5b09b14691241c3067edba7e03bf1398366ef31` | the same |

The bootloaders and the partition table are 0.7.0's, byte for byte.

## 2. The first boot of 0.8.0 on a card 0.7.0 used (QIO)

The card had only run 0.7.0's library code (`library.idx` v5, no
`/.player/tags.bin`).

| Check | Result |
|---|---|
| ROM, banner | `rst:0x1 (POWERON_RESET)`, `mode:DIO, clock div:1`; `v0.7.0-40-g5b126bd (commit 5b126bd, ...), ELF 7e58c88a`; `running ota_0`; `[nvs] schema 2` (no migration) |
| The boot screen | `[boot] the boot screen: the logo (240 x 47 px, 2182 B of runs) and the version in 41 ms` |
| The card | `the sector cache: on (256 sectors, 135168 B of PSRAM)`; the card's identity recorded |
| The update from 0.7.0 | `/music walked: 120 files in 57 ms`; `/.player/device.txt written`; `D none (0 records, 0 journal chunks)`; `library.idx outdated: walk /music (an older version's, and no records) in 63 ms`; 113 tracks, 7 artists, 10 albums; **browsable 157 ms after the mount** |
| The queue | `restored 10 of 10 tracks from /.player/queue.txt`, at its place |
| Memory | `[heap] PSRAM's lowest free: 2652004 B over setup()`; `[diag] Library SD, 113 tracks, names from the files` |
| The background work | The walk 36 s after the UI (Bluetooth paged the remembered headphones first): `113 added`; the scan; `compaction done ... 0.2 s`; the update step: `built in 76 ms on the card worker (the loop live; the fence up 151 ms)`, `library.idx saved in 35 ms`, `carried through queue.txt: 10 of 10 tracks still there`, "Library updated". No card error, no LOW stack line |
| The records | `D: 113 records ... walked`; rows 105 Scanned, 8 Unreadable. `gt` on each test file: the 8 Unreadable are the test folder's Ogg Vorbis files named `.opus` (one in "Joins and refusals", seven "Stop (Ogg Vorbis)" in "Seeks on each kind"); the surround and 2.5 ms files read as Scanned (they are refused at play time, not at the scan). The worker's stack 2,184 B left at least |

## 3. The second boot (QIO)

`library.idx matches the card: load (its inputs are the card's) in 10 ms`,
browsable 39 ms after the mount; the queue at its place (now "from NVS");
`[diag] Library SD, 113 tracks, 92% tagged`; the walk later: `0 added, 0
changed, 0 gone`, 20 steps in 88 ms; no scan, no update step. The Output
page lists 14 rows (the Library row added). The user looked at the boot
screen and Device info on the boot-logo build (3ea7a5d) on 2026-10-09.

## 4. The benches (QIO, silent mode, by path, playback stopped)

| Track | 0.8.0 candidate | 0.7.0 |
|---|---|---|
| A 44.1 kHz MP3, twice | `4.31 s = 4.6x`, `4.30 s = 4.7x` realtime (21.5 % of a core); `libmad's state: pinned at 0x3f82d7e0, PSRAM, its lower 2 MB` | `4.20 s = 4.8x`, at 0x3f808e80 |
| A 44.1 kHz FLAC | `4.27 s = 4.7x` (21.3 %), decode stack free 13,096 | `4.28 s = 4.7x` |
| `ms128k.opus`, decode only | `5.72 s = 3.5x` (28.5 %), stack free 4,296, 0 concealed, opened from `opus.idx`'s record | `5.68 s = 3.5x` |
| The same, decoded and converted to 44.1 kHz | `7.48 s = 37.3 % of a core`, the converter 8.8 % | `7.30 s = 36.5 %`, 8.1 % |

The libmad state moved up in PSRAM (the sector cache and #8's fonts are
allocated before it) and stays in the fast lower 2 MB, as #8 asked.

## 5. The DIO image

Flashed, then one boot: `mode:DIO, clock div:2`, ELF 5e8ce921, ota_0,
schema 2, `library.idx matches the card: load` in 10 ms, browsable 44 ms
after the mount, the queue at its place.

| Track | DIO | 0.7.0's DIO |
|---|---|---|
| The MP3 | `4.83 s = 4.2x` (24.1 %) | `4.66 s = 4.3x` |
| The FLAC | `4.36 s = 4.6x` | `4.35 s = 4.6x` |
| `ms128k.opus`, decode only | `6.42 s = 3.1x` | `6.30 s = 3.2x` |
| Decoded and converted | `8.33 s = 41.5 %`, converter 9.5 % | 39.9 % |

Two minutes of the test folder's mixed album (MP3, FLAC, Opus) on the
speaker: 25 `[stats]` lines playing, `underruns=0` in all; the Opus
tracks ended on their EOS trims, 0 concealed; no `[E]` line.

Then QIO was flashed back (`clock div:1`, ELF 7e58c88a) for section 6.

## 6. Playing, with the library work under music (QIO, about 8 minutes)

The queue: an MP3 album, a FLAC album and the test folder's mixed album
(35 tracks), repeat all, the speaker in silent mode.

| Check | Result |
|---|---|
| MP3 playing | `buf=1439ms`, `load=` 33-34 %, `pass_max` 13.5 ms, `underruns=0` |
| `gb` while the MP3 plays (L4.1-mp3 at 113 tracks; twice) | `built in 149 ms` / `151 ms on the card worker (the loop live; the fence up 238 ms` / `286 ms)`, `saved in 57 ms`, `carried through queue.txt: 35 of 35 tracks still there, at 1`; the track played on, 0 underruns |
| Rescan while a FLAC plays | The Rescan's compaction, the scan of 113 files, the update step at the scan's end: `built in 133 ms (the fence up 230 ms)`, `35 of 35 ... at 15`, "Library updated"; 0 underruns |
| The mixed album, 5 minutes | `G`: joins 5 continuous, 2 after the tail, cuts 0, too late 0, opens failed 0 |
| The whole of section 6 | 92 `[stats]` lines playing: `underruns=0` in every one; `ram=` median 61 KB, `min=` 50 KB at the lowest (G5's floor: 42 KB); `pmin=` 2,308 KB at the lowest; `stack_free` 4,616 B at the lowest; `pass_max` 30,005 us at the highest (section 8); the card worker's stack 2,108 B left at least, internal RAM 55,372 B at the lowest while a step ran. No Guru, panic, backtrace, reset, LOW stack line or `sd_diskio.cpp` line |

A test command sent by mistake turned gapless off (`G0`) for 22 s while
the same track played; it was turned on again (`G1`). It is not saved.

## 7. The user's state, put back

The user's queue as recorded on 2026-10-08 (10 tracks of one album, at
entry 10, stopped, no resume or start point, shuffle off, repeat off) was
rebuilt: `qx`, the album, `qR0`, `i9` in silent mode, `b9`, `qs0`. `q`:
`10 tracks ..., at 10, 0 up next; shuffle off, repeat off`; `resume point
saved: none; start point waiting: none`. The settings in NVS are put back
from the user's flash backup when the released image is installed.

## 8. Findings

- **MP3 is 2-4 % slower than 0.7.0** (QIO 4.6-4.7x against 4.8x; DIO
  4.2x against 4.3x). FLAC and Opus are level. The MP3 state stays in
  PSRAM's lower 2 MB. Code layout alone has measured about ±3 % before;
  4.6x realtime leaves the decoder plenty of headroom. Not a release
  blocker.
- **One decode pass of 30.005 ms** (G6 wants under 30 ms) in section 6,
  with 0 underruns. 0.7.0's soak had passes up to 32.7 ms.

## 9. Not run

- **The seek bar's touch checks** (0.7.0's section 4): its code didn't
  change; the fence's "a seek waits" ran on the synthetic card (L4.2).
- **The Bluetooth link idle in this run:** the card worker with an A2DP
  stream up was checked on 0f1b4a5 with digital silence only (METADATA.md
  6.3).
- **The plan's part 8** (the 20k boot, the cap and the dividers on the
  synthetic card, another card while on): covered by L0-L5, except the
  card guard's "another card" case on a device, which rests on
  `test_sector_cache` and `test_fat_model`.
- **Screenshots:** the user looked at the screens.
- **The user's listening checks:** every agent run is silent.
