# v0.10.0 pre-release device checks

The candidate: `main` at 996d33c. These PRs were merged since 0.9.0
(the release notes' "What's new in 0.10.0"):

- #17: `NEXT_RELEASE` 0.10.0;
- #18: multi-language support, phase 3 (docs/I18N.md):
  - the UI in ten languages, chosen in Output > Language;
  - a new unit's first-boot language picker;
  - NVS schema 3;
  - "Various Artists" as a flag in `library.idx` (rules 5);
  - layouts that fit every language;
  - the Queue header's summary with the position first.

Run on 2026-10-12 on the user's Core2 v1.3 (COM3), through the serial
daemon, with the user's own card. The card holds 113 tracks, is FAT32, and
0.9.0 was the last version to use it. Silent mode (`z`) was sent after
every boot, before anything played. The user's state was recorded first
and put back at the end (section 7). Every flash wrote the four built
files with `--flash-mode keep --flash-freq keep --flash-size keep` and
`--after no-reset`, so each first boot was logged.

**Result: no failure, no crash, no underrun, no card error, no code
change.** Every figure is level with 0.9.0's checks or better: the
longest decode pass is 29.6 ms, under G6's 30 ms (0.9.0's: 30.8 ms).

These earlier device runs are not repeated here:

- **Phase 3 in all ten languages:** PR #18's two comments and
  docs/I18N.md's device checks. They covered:
  - every main screen;
  - the Language page, About, the tips and the first-boot picker;
  - the machine-translation note;
  - the fixes the first run called for, checked again on
    `v0.9.0-14-g2acd180`.
- **The synthetic 20k card:** 0.9.0's and phase 2's runs (the CJK names,
  the scrolling, the rebuild at 19,410 tracks).

## 1. The build

**Host tests:** CI's `build` job on 996d33c ran `pio test -e native`:
**1,586 of 1,586 passed**. Both images built there too.

**Firmware:** built locally from a clean checkout of 996d33c, from
PowerShell with MSYSTEM removed, with no `local.ini`:

| | `core2` (QIO: the main image) | `core2-dio` (the `-dio-full.bin`) |
|---|---|---|
| Build | SUCCESS in 10:12 | SUCCESS in 14:44 |
| `version:` | `v0.9.0-15-g996d33c`, ELF 814d3499 | the same version, ELF 31fc518b |
| `iram_diet:` | 51 of 51 libc objects moved, the MP3 hot set pinned | the same |
| `cache_guard:` | ok: at most 2 hot lines in a set (bench and playback) | the same |
| `flash_guard:` | `app 4.08 MB = 68% of the 6.00 MB slot; ... flash QIO at 80 MHz` | the same, `flash DIO at 40 MHz` |
| RAM, Flash | 57,448 B; 4,190,119 B (66.6 %) | 57,448 B; 4,190,151 B (66.6 %) |
| `firmware.bin`, `firmware.factory.bin` | 4,274,480 B, 4,340,016 B | 4,274,512 B, 4,340,048 B |
| `bootloader.bin` | 24,992 B, header `e9 03 02 4f` | 23,520 B, header `e9 03 02 40` |
| `firmware.bin` header | `e9 06 02 4f` | `e9 06 02 40` |
| sha256 `firmware.bin` | `37e00e767e9ec5e2c152d143c2ee6135e6337577bcd31791ed842d5e217d69c1` | `e2158843f165de9252cabedf5a32ff830ea705f0bcb98c386d243be4352eb57e` |
| sha256 `bootloader.bin` | `3ae837b726c1d3e860c24391b70624d39b4715524cd7509c164d859bb8290914` | `8b956c0cdaccc35f0df06f12e717ac2e465cee3a59d890b035d651638492c3ff` |
| sha256 `partitions.bin` | `d978ecf9a50caff5c13b95dec5b09b14691241c3067edba7e03bf1398366ef31` | the same |

The bootloaders and the partition table are 0.9.0's, byte for byte.

The app grew by 147 KB from 0.9.0's 4.13 MB:
- 126 KB is the nine languages' texts;
- the rest is phase 3's code and the layout fixes.

Static RAM is 8 B smaller.

## 2. The first boot of 0.10.0 on a card 0.9.0 used (QIO)

| Check | Result |
|---|---|
| ROM, banner | `rst:0x1 (POWERON_RESET)`, `mode:DIO, clock div:1`; `v0.9.0-15-g996d33c (commit 996d33c, ...), ELF 814d3499`; `running ota_0` |
| The settings | `[nvs] schema 2 -> 3: migrated (the UI's language: English, as before)`; `[ui] language: en (English)`; no language picker (an updated unit) |
| The boot screen | The logo and the version in 41 ms |
| The update from 0.9.0 | `library.idx outdated: build from the records (an older version's) in 62 ms`: 113 tracks, 105 from the device's records and 8 by their paths, so no file was read again; 7 artists, 10 albums; **browsable 164 ms after the mount** |
| The queue | `restored 10 of 10 tracks from /.player/queue.txt`, at its place |
| Memory | `[heap] PSRAM's lowest free: 2633092 B over setup()`; `[diag] Library SD, 113 tracks, 92% tagged` |

## 3. The second boot (QIO)

- `[nvs] schema 3`.
- `library.idx matches the card: load (its inputs are the card's) in 10 ms`, browsable 38 ms after the mount (0.9.0: 10 ms and 40 ms).
- The queue was at its place.

## 4. The benches (QIO, silent mode, by path, playback stopped)

The same four files as 0.9.0's checks.

| Track | 0.10.0 candidate | 0.9.0 |
|---|---|---|
| A 44.1 kHz MP3, twice | `4.11 s = 4.9x`, `4.09 s = 4.9x` realtime (20.5 % and 20.4 % of a core); `libmad's state: pinned at 0x3f831c20, PSRAM, its lower 2 MB` | `4.30 s = 4.7x`, `4.28 s = 4.7x` |
| A 44.1 kHz FLAC | `4.33 s = 4.6x` (21.6 %), decode stack free 12,840 | `4.31 s = 4.7x` |
| `ms128k.opus`, decode only | `5.80 s = 3.5x` (29.0 %), stack free 4,296 | `5.68 s = 3.5x` |
| The same, decoded and converted to 44.1 kHz | `7.51 s = 37.5 % of a core`, the converter 8.6 % | `7.33 s = 36.6 %`, 8.2 % |

The differences are within the ±3 % that code layout has moved these
figures before.

## 5. The DIO image

Flashed, then one boot:
- `mode:DIO, clock div:2`, ELF 31fc518b, ota_0;
- `[nvs] schema 3`;
- `library.idx matches the card: load` in 10 ms, browsable 43 ms after the mount.

| Track | DIO | 0.9.0's DIO |
|---|---|---|
| The MP3 | `4.49 s = 4.5x` (22.4 %) | `4.79 s = 4.2x` (23.9 %) |
| The FLAC | `4.45 s = 4.5x` (22.2 %) | `4.40 s = 4.6x` |
| `ms128k.opus`, decode only | `6.59 s = 3.0x` (32.9 %) | `6.31 s = 3.2x` |
| Decoded and converted | `8.46 s = 42.2 %`, converter 9.3 % | `8.09 s = 40.4 %`, 8.9 % |

**Playback:** two minutes of the test folder's mixed album (MP3, FLAC,
Opus) on the speaker:
- 25 `[stats]` lines while playing, `underruns=0` in all;
- the Opus tracks ended on their EOS trims;
- no `[E]` line.

As in 0.9.0's run, section 6 ran on the QIO image before the DIO image.

## 6. Playing, with the library work under music (QIO, about 8 minutes)

The queue: an MP3 album, a FLAC album and the test folder's mixed album
(35 tracks), repeat all, the speaker in silent mode.

| Check | Result |
|---|---|
| `gb` while the MP3 plays (twice) | `built in 125 ms` / `124 ms on the card worker (the loop live; the fence up 216 ms` / `191 ms)`, `library.idx saved in 56 ms` / `63 ms`, `carried through queue.txt: 35 of 35 tracks still there, at 1`; the track played on, 0 underruns |
| Rescan (`gr`) while a FLAC plays | <ul><li>The Rescan's compaction (113 records, 18 folders), then the scan of 113 files.</li><li>The update step at the scan's end: `built in 97 ms (the fence up 196 ms)`, `35 of 35 ... at 15`, "Library updated".</li><li>0 underruns.</li></ul> |
| The mixed album, 5 minutes | `G`: joins 5 continuous, 2 after the tail, cuts 0, too late 0, opens failed 0 (0.9.0: the same). The Opus tracks ended on their EOS trims, 0 concealed |
| The whole of section 6 | <ul><li>87 `[stats]` lines while playing, `underruns=0` in every one.</li><li>`ram=` median 61 KB; `min=` 47 KB at the lowest (G5's floor: 42 KB).</li><li>`pmin=` 2,163 KB at the lowest.</li><li>`stack_free` 4,296 B at the lowest.</li><li>`pass_max` 29,595 us at the highest.</li><li>No Guru, panic, backtrace, reset, `task_wdt`, LOW stack line or `sd_diskio.cpp` line.</li></ul> |

## 7. The user's state, put back

**The queue:** the user's queue as recorded at the start of the run (10 tracks
of one album, at entry 1, stopped, no resume or start point, shuffle
off, repeat off) was rebuilt on the DIO image, in silent mode:
1. `qx`, then the album;
2. `qR0`;
3. `i0`, then `b0` (the bench stops the player);
4. `qs0`.

Afterwards, `q` showed `10 tracks ..., at 1, ... shuffle off, repeat off`
and `resume point saved: none; start point waiting: none`. The track list
matched the one recorded at the start, path for path and in the same order.

**The firmware and settings:** the flash (0x0-0x440000) and the NVS had
been backed up and verified before the phase 3 checks, and were still the
same before this run (verified again). The user asked to keep each release
on the Core2: the released v0.10.0 image is installed with the settings
(NVS, schema 2) the user had before the run, so its first boot migrates
them as an update from 0.9.0 does.

## 8. Findings

None. Every figure is level with 0.9.0's checks or better. The decode
speeds moved within the usual code-layout range: MP3 faster in both flash
modes, Opus 2-4 % slower on DIO.

## 9. Not run

- **The ten languages on this card:** they ran in #18's two device checks
  (listed at the top). This run used English.
- **The Bluetooth link:** no headphones were used.
- **A downgrade to 0.9.0:** it ran in #18's checks, where 0.9.0 rebuilt
  the library in 63 ms and kept the queue and the settings.
- **The seek bar's touch checks** (0.7.0's section 4): its code didn't
  change.
- **The user's listening checks:** every agent run is silent.
