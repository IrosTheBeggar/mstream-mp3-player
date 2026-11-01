# v0.9.0 pre-release device checks

The candidate: `main` at 7fede49. These PRs were merged since 0.8.0
(the release notes' "What's new in 0.9.0"):

- #11: the computer's status questions (docs/HOST-STATUS.md);
- #12 to #14: multi-language support, phases 0, 1 and 2a
  (docs/I18N.md):
  - names in Greek, Cyrillic and more Latin;
  - Chinese, Japanese and Korean names from a built-in 16 px glyph store;
  - the order across scripts;
- #15: the console flood that restarted the player.

Run on 2026-10-11 on the user's Core2 v1.3 (COM3), through the serial
daemon, with the user's own card. The card holds 113 tracks, is FAT32, and
0.8.0 was the last version to use it. Silent mode (`z`) was sent after
every boot, before anything played. The user's state was recorded first
and put back at the end (section 7). Every flash wrote the four built
files with `--flash-mode keep --flash-freq keep --flash-size keep` and
`--after no-reset`, so each first boot was logged.

**Result: no failure, no crash, no underrun, no card error, no code
change.** One figure is recorded as a finding (section 8): one decode pass
of 30.8 ms, against G6's 30 ms. 0.8.0's checks had one of 30.005 ms.

These earlier device runs are not repeated here:

- **Multi-language** (on the synthetic 20k card of `tools/synthcard.py`):
  #12's and #13's bodies and #14's comment, and docs/I18N.md's device
  checks. They covered:
  - the rebuilds for the new order;
  - Greek, Cyrillic, kana and Hangul on the panel;
  - the lists' scrolling against the build before;
  - the loop task's stack.
- **The console flood** (#15): reproduced on 0.8.0's line and gone on the
  fix.

## 1. The build

**Host tests:** CI's `build` job on 7fede49 ran `pio test -e native`:
**1,560 of 1,560 passed**. Both images built there too.

**Firmware:** built locally from a fresh clone of 7fede49, from PowerShell
with MSYSTEM removed, with no `local.ini`:

| | `core2` (QIO: the main image) | `core2-dio` (the `-dio-full.bin`) |
|---|---|---|
| Build | SUCCESS in 11:26 | SUCCESS in 13:09 |
| `version:` | `v0.8.0-26-g7fede49`, ELF a7d0c27d | the same version, ELF 41fb4bd9 |
| `iram_diet:` | 51 of 51 libc objects moved, the MP3 hot set pinned | the same |
| `cache_guard:` | ok: at most 2 hot lines in a set (bench and playback) | the same |
| `flash_guard:` | `app 3.94 MB = 66% of the 6.00 MB slot; ... flash QIO at 80 MHz` | the same, `flash DIO at 40 MHz` |
| RAM, Flash | 57,456 B; 4,046,559 B (64.3 %) | 57,456 B; 4,046,591 B (64.3 %) |
| `firmware.bin`, `firmware.factory.bin` | 4,127,664 B, 4,193,200 B | 4,127,696 B, 4,193,232 B |
| `bootloader.bin` | 24,992 B, header `e9 03 02 4f` | 23,520 B, header `e9 03 02 40` |
| `firmware.bin` header | `e9 06 02 4f` | `e9 06 02 40` |
| `firmware.parts.json` | `flash_mode: qio`, `flash_freq: 80m` | `flash_mode: dio`, `flash_freq: 40m` |
| sha256 `firmware.bin` | `327fee855b2cf22c26fbac27e84074a9dce3c8e946e8440464a4b511301ef58c` | `d7a21202ae81378c6bd6dff891974bb173d8a23f3d5f9053be017afcc6b36a0d` |
| sha256 `bootloader.bin` | `3ae837b726c1d3e860c24391b70624d39b4715524cd7509c164d859bb8290914` | `8b956c0cdaccc35f0df06f12e717ac2e465cee3a59d890b035d651638492c3ff` |
| sha256 `partitions.bin` | `d978ecf9a50caff5c13b95dec5b09b14691241c3067edba7e03bf1398366ef31` | the same |

The bootloaders and the partition table are 0.8.0's, byte for byte.

The app grew by 1.49 MB from 0.8.0's 2.63 MB:
- 1.1 MB is the CJK glyph store;
- 0.37 MB is phase 1's fonts.

Static RAM grew by 280 B.

## 2. The first boot of 0.9.0 on a card 0.8.0 used (QIO)

| Check | Result |
|---|---|
| ROM, banner | `rst:0x1 (POWERON_RESET)`, `mode:DIO, clock div:1`; `v0.8.0-26-g7fede49 (commit 7fede49, ...), ELF a7d0c27d`; `running ota_0`; `[nvs] schema 2` (no migration) |
| The boot screen | The logo and the version in 41 ms |
| The update from 0.8.0 | `/.player/device.txt written`; `library.idx outdated: build from the records (an older version's) in 62 ms`: 113 tracks, 105 from the device's records and 8 by their paths, so no file was read again; 7 artists, 10 albums; **browsable 178 ms after the mount** |
| The queue | `restored 10 of 10 tracks from /.player/queue.txt`, at its place |
| Memory | `[heap] PSRAM's lowest free: 2634244 B over setup()`; `[diag] Library SD, 113 tracks, 92% tagged` |
| The background walk | `0 added, 0 changed, 0 gone`, 20 steps in 88 ms; no scan, no update step |

## 3. The second boot (QIO)

- `library.idx matches the card: load (its inputs are the card's) in 10 ms`, browsable 40 ms after the mount (0.8.0: 10 ms and 39 ms).
- The queue was at its place.
- `[diag] Library SD, 113 tracks, 92% tagged`.

## 4. The benches (QIO, silent mode, by path, playback stopped)

The same four files as 0.8.0's checks.

| Track | 0.9.0 candidate | 0.8.0 |
|---|---|---|
| A 44.1 kHz MP3, twice | `4.30 s = 4.7x`, `4.28 s = 4.7x` realtime (21.4 % and 21.3 % of a core); `libmad's state: pinned at 0x3f831c20, PSRAM, its lower 2 MB` | `4.31 s = 4.6x`, `4.30 s = 4.7x`, at 0x3f82d7e0 |
| A 44.1 kHz FLAC | `4.31 s = 4.7x` (21.5 %), decode stack free 12,828 | `4.27 s = 4.7x` |
| `ms128k.opus`, decode only | `5.68 s = 3.5x` (28.4 %), stack free 4,284, 0 concealed | `5.72 s = 3.5x` |
| The same, decoded and converted to 44.1 kHz | `7.33 s = 36.6 % of a core`, the converter 8.2 % | `7.48 s = 37.3 %`, 8.8 % |

The 1.1 MB glyph store, which sits in flash, didn't move the decoders:
- the MP3 state stays in PSRAM's fast lower 2 MB;
- `cache_guard`'s pinned hot set is unchanged.

## 5. The DIO image

Flashed, then one boot:
- `mode:DIO, clock div:2`, ELF 41fb4bd9, ota_0, schema 2;
- `library.idx matches the card: load` in 10 ms, browsable 46 ms after the mount;
- the queue at its place.

| Track | DIO | 0.8.0's DIO |
|---|---|---|
| The MP3 | `4.79 s = 4.2x` (23.9 %) | `4.83 s = 4.2x` (24.1 %) |
| The FLAC | `4.40 s = 4.6x` (21.9 %) | `4.36 s = 4.6x` |
| `ms128k.opus`, decode only | `6.31 s = 3.2x` (31.5 %) | `6.42 s = 3.1x` |
| Decoded and converted | `8.09 s = 40.4 %`, converter 8.9 % | `8.33 s = 41.5 %`, 9.5 % |

**Playback:** two minutes of the test folder's mixed album (MP3, FLAC,
Opus) on the speaker:
- 25 `[stats]` lines while playing, `underruns=0` in all;
- the Opus tracks ended on their EOS trims, 0 concealed;
- no `[E]` line.

This run did section 6 on the QIO image before the DIO image, so the DIO
image came last.

## 6. Playing, with the library work under music (QIO, about 8 minutes)

The queue: an MP3 album, a FLAC album and the test folder's mixed album
(35 tracks), repeat all, the speaker in silent mode.

| Check | Result |
|---|---|
| MP3 playing | `buf=1439ms`, `load=` 33-35 %, `pass_max` 25.4 ms, `underruns=0` |
| `gb` while the MP3 plays (twice) | `built in 142 ms` / `122 ms on the card worker (the loop live; the fence up 242 ms` / `210 ms)`, `carried through queue.txt: 35 of 35 tracks still there, at 1`; the track played on, 0 underruns |
| Rescan (`gr`) while a FLAC plays | <ul><li>The Rescan's compaction (113 records, 18 folders), then the scan of 113 files.</li><li>The update step at the scan's end: `built in 98 ms (the fence up 195 ms)`, `35 of 35 ... at 15`, "Library updated", `library.idx saved in 65 ms`.</li><li>0 underruns.</li></ul> |
| The mixed album, 5 minutes | `G`: joins 5 continuous, 2 after the tail, cuts 0, too late 0, opens failed 0 (0.8.0: the same). The Opus tracks ended on their EOS trims, 0 concealed |
| The whole of section 6 | <ul><li>94 `[stats]` lines while playing, `underruns=0` in every one.</li><li>`ram=` median 60 KB; `min=` 47 KB at the lowest (G5's floor: 42 KB).</li><li>`pmin=` 2,164 KB at the lowest.</li><li>`stack_free` 4,284 B at the lowest.</li><li>`pass_max` 30,770 us at the highest (section 8).</li><li>`load=` at most 44 %.</li><li>No Guru, panic, backtrace, reset, `task_wdt`, LOW stack line or `sd_diskio.cpp` line.</li></ul> |

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

**The firmware and settings:** the flash (0x0-0x440000) and the NVS were
backed up first, and both backups were verified. The user asked to keep
0.9.0 on the Core2 afterwards: the released v0.9.0 image is installed with
the settings (NVS) the user had before the run.

## 8. Findings

- **One decode pass of 30.8 ms** (G6 wants under 30 ms) in section 6,
  with 0 underruns. 0.8.0's checks had one of 30.005 ms, and 0.7.0's soak
  had passes up to 32.7 ms.
- **Memory is a little lower than 0.8.0's checks, and inside its limits.**
  - Internal RAM's lowest was 47 KB (0.8.0: 50 KB; G5's floor: 42 KB).
  - PSRAM's lowest while playing was 2,164 KB (0.8.0: 2,308 KB).
  - The glyph store takes no RAM: its index and bitmaps are read from flash.
- **The decoders' speeds are level with 0.8.0** in both flash modes.

## 9. Not run

- **Names in other scripts on this card:** the user's card has none.
  Greek, Cyrillic, kana and Hangul ran on the synthetic card (section 1's
  links); docs/I18N.md lists what is still to check (Bluetooth output,
  CJK lists under a real decode, multi-line CJK titles).
- **The Bluetooth link:** no headphones were used.
- **The seek bar's touch checks** (0.7.0's section 4): its code didn't
  change.
- **Screenshots on this card:** the multi-language ones are in #13 and
  #14.
- **The user's listening checks:** every agent run is silent.
