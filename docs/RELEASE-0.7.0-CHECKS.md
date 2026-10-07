# v0.7.0 pre-release device checks

The candidate: the dev branch at 221d99d. What's new since 0.6.0 (the
release notes' "What's new in 0.7.0"):

- the Now Playing seek bar, and its same-second rule;
- Now Playing's menus, with shuffle and repeat (Off by default);
- the MP3 cache-layout pin and `tools/cache_guard.py`;
- the Pair screen staying lit while it searches;
- About's card size, and a message for each kind of card;
- the file-name rules (LibraryIndex version 5);
- Opus (OPUS.md).

Run on 2026-10-07 on the user's Core2 (COM3), through the serial daemon,
from 10:24 to 11:44. Both images were flashed with esptool: QIO, then DIO,
then QIO again. Silent mode (`z`) was on after every boot before anything
played. The user had just used the Core2 by hand, so their state was
noted first and restored at the end (see the end of this file).

**Result: every check passed. No firmware defect, no errors, no
underruns, no code change.** One fault was in the test procedure, not in
the firmware: the flash script rewrote the DIO image's header, so the DIO
image was flashed again as built and checked again (section 7).
The headphones were linked from the benches until 37 minutes into the
soak, but idle: silent mode keeps the music on the speaker.

## 1. The build

Host tests (Git Bash, `pio test -e native`): **1199 of 1199 passed**, in
61 suites, in 5:39.5. The new suites ran and passed: test_seek_bar,
test_ogg_opus, test_opus_open_cache, test_track_name and
test_library_index.

Firmware, from PowerShell with MSYSTEM removed, one build after the other:

| | `core2` (QIO: the main image) | `core2-dio` (the `-dio-full.bin`) |
|---|---|---|
| Build | SUCCESS in 7:08.8 | SUCCESS in 7:29.8 |
| `version:` | `v0.6.0-37-g221d99d (221d99d, 2026-10-07)`, ELF c221a08e | the same version, ELF b699f82f |
| `flash_guard:` | `app 2.31 MB = 39% of the 6.00 MB slot; ... flash QIO at 80 MHz` | the same, `flash DIO at 40 MHz` |
| RAM, Flash | 56,480 B; 2,357,031 B (37.5 %) | 56,480 B; 2,357,063 B (37.5 %) |
| `firmware.bin`, `firmware.factory.bin` | 2,422,240 B, 2,487,776 B | 2,422,272 B, 2,487,808 B |
| `bootloader.bin` | 24,992 B, header `e9 03 02 4f` | 23,520 B, header `e9 03 02 40` |
| `firmware.bin` header | `e9 06 02 4f` | `e9 06 02 40` |
| `firmware.parts.json` | `flash_mode: qio`, `flash_freq: 80m` | `flash_mode: dio`, `flash_freq: 40m` |
| sha256 `firmware.bin` | `6014a24ba8e986ead541092814d41f72f7320cbdf8d7a8abb2845ab519b42bc7` | `9baee39075a97071543d099db3245ebeafa9e8c99fc425c64988d36c0e900c0e` |
| sha256 `bootloader.bin` | `3ae837b726c1d3e860c24391b70624d39b4715524cd7509c164d859bb8290914` | `8b956c0cdaccc35f0df06f12e717ac2e465cee3a59d890b035d651638492c3ff` |
| sha256 `partitions.bin` | `d978ecf9a50caff5c13b95dec5b09b14691241c3067edba7e03bf1398366ef31` | the same |

- The other guards gave the same line on both builds:
  - `iram_diet: 51 of 51 libc objects moved to flash, the MP3 hot set
    pinned (9 code and 3 data lines; sections.ld 140237 -> 121839 bytes)`;
  - `cache_guard: ok: the MP3 synth loop's hot lines, at most 2 in a set
    (bench), 2 in a set (playback); pinned: code 0x400d0020-0x400d1b47
    (6951 B), data 0x3f400120-0x3f400a18 (2296 B)`.
- The QIO headers say DIO, 16 MB, 80 MHz. That is expected, as in the
  0.6.0 checks: the ROM loads the bootloader in DIO, and the bootloader
  switches the flash to quad itself. The DIO build's headers say DIO, 16
  MB, 40 MHz (`0x40`), as built.
- Warnings: 3, all in libraries (ESP8266Audio's AudioOutputPDM.cpp, a
  narrowing; ESP32-A2DP's "AudioTools library is not included", twice).
  None came from `src/`.
- The version says v0.6.0-37 because the build isn't tagged yet. The
  tag's CI build gives v0.7.0, with its own ELF digits and checksums.
- The worktree was still clean at 221d99d after the tests and both builds.

## 2. The boot (QIO)

- Flashed with esptool; the files' sha256 matched section 1. The first
  boot after a flash isn't logged: the flash script stops the daemon and
  esptool resets the board. On that boot the headphones (SPYDRONE, switched
  on) connected, with the output on Bluetooth while stopped. `z` then gave
  `[audio] output: speaker` and `[test] silent mode: speaker muted,
  bluetooth won't take over`.
- 3 cold resets (a raw `!reset`, then `z` the moment the console was up),
  all the same:
  - the ROM: `rst:0x1 (POWERON_RESET)` and `mode:DIO, clock div:1` (80
    MHz; DIO in the ROM is expected for a QIO build);
  - the banner `mstream-mp3-player v0.6.0-37-g221d99d (commit 221d99d,
    2026-10-07), ELF c221a08e`;
  - `[flash] running ota_0`, `[nvs] schema 2` (0.6.0 had schema 1; the
    repeat mode is new), CPU 240 MHz (the default);
  - the library: `120 files walked in 76 ms, index loaded from the cache in
    7 ms`, 113 tracks, 7 artists, 10 albums;
  - the queue: `repeat: off (saved)`, `restored 63 of 63 tracks ... at 63
    of 63`, no resume point (as noted before), `now at 63 of 63
    (stopped)`;
  - `[bt] headphones: paired on Output > Pair new headphones only (never
    picked by the Core2 itself)`: this build has no headphone name built
    in, as a release build has none.
- Time from the reset: the banner at 1.89-1.91 s, the console at
  3.08-3.09 s, the UI at 4.49-4.50 s (the 0.6.0 checks: the console at
  2.8 s). Internal RAM after the UI: 79K free (min 76K); 0.6.0 had 80K
  (min 77K).
- **The index rebuild wasn't seen.** No logged boot rebuilt the index. The
  card's cache was already at version 5, written by the earlier dev build
  the user was running, and no file had changed. The only boot that could
  have rebuilt it was the unlogged one after the flash.
- **An Opus resume after a reset.** A later QIO boot restored `resume
  point: 0:05 into 34 ... anchor: Opus sample 286545`. The play started
  `Opus: starting 0:05.969 in, of 0:52 (its resume anchor; exact)`, with
  the 600 ms preroll.

## 3. The benches (QIO, silent mode, by path)

The benches ran by path, so the queue didn't change. The headphones'
link was up but idle.

| Track | Result | Expected |
|---|---|---|
| One More Time (MP3) | `4.20 s = 4.8x realtime (20.9% of a core)`, twice; `libmad's state: pinned at 0x3f808e80, PSRAM` | 4.8x (0.6.0's figure, which the cache pin keeps) |
| Graduation's FLAC bench track | `4.28 s = 4.7x realtime (21.3% of a core)`, decode stack free 12996 | 4.7x |
| `ms128k.opus` (an mStream 128k transcode), decode only | `20.0 s of 48000 Hz audio in 5.68 s = 3.5x realtime (28.4% of a core)`, decode stack free 4292 | |
| The same, decoded and converted to 44.1 kHz | `7.30 s = 36.5% of a core at 240 MHz: the converter 8.1%` | |

- The first One More Time bench gave 4.7x (4.24 s), because the Bluetooth
  link came up during it. With the link settled, both runs gave 4.8x.
- The Opus open: `5 reads / 34351 B in 36 ms` (OPUS.md 10.5 expects ~5
  reads, ~33 KB, ~30 ms) and `length 209920 ms (exact, from the last
  page)`. 1002 packets, 0 concealed or malformed. The open cache was saved
  with 28 entries.

## 4. The seek bar's same-second rule (SEEK-BAR.md 11.16)

On an Opus track of 0:52 from the queue (5.64 px a second), in silent
mode on Now Playing. Before each touch the knob's x came from `ui` and the
position from `s`. Each touch aimed at the knob's x at the Down: the
knob + 1-2 px (the script was quicker than the doc's 0.6 s).

| | Touch | Result |
|---|---|---|
| Playing | `uid<k>,150,<k+14>,150,300` (a grab let go), 10 times over 21 s, so the second's phase went all the way round (k 42 to 163) | 10 of 10 `[ui] now playing: no seek (back where it plays)` |
| Playing | `uit177,150` (the knob at x 178) | `no seek (back where it plays)` |
| Playing | `uid<k>,150,<k+24>,150,300`, twice | `seek 0:32 -> 0:34 ... (drag from the knob, held 140 ms): plays from there`, then `Opus: starting 0:34.000 in ... (the time asked; exact) ... 200 ms preroll`; `seek 0:05 -> 0:07`, `starting 0:07.000 in`. The ring stayed at 1422 ms or more, with no underrun |
| Paused at 0:20.306 (the knob at x 126) | `uid126,150,140,150,300`, 10 times; `uit125,150` | 11 of 11 `no seek (back where it plays)`, and no `[queue] resume point saved` line after any of them |
| Paused | `uid126,150,150,150,300` | `seek 0:20 -> 0:22 ... : paused, the next play starts there`, then `resume point saved: 0:22 into 37` |

**22 of 22 touches on the second already playing didn't seek, and 3 of 3
forward drags sought 2 s on.** Before 221d99d, about half of such grabs
sought (SEEK-BAR.md 17).

## 5. The soak: 47 minutes on the speaker, silent mode

The queue was Discovery (MP3), Graduation (FLAC) and the mixed soak album
of the Opus test folder (Opus, MP3 and FLAC): 35 entries, on Repeat All
(`qR1`). Play started at Graduation's track 9 (`i22`). The screen was
kept lit (`Ps1` every 15 s) and `G` was read every 5 minutes. The Dance
tab was up for 18 of the 47 minutes: minutes 6-12 (FLAC) and 28-40
(Discovery). Now Playing was up for the rest.

| | Result |
|---|---|
| Length | 47.0 min (564 stats lines, all playing): Graduation 9-13, the mixed album 1-8, then Repeat All's wrap into Discovery 1-6 |
| Underruns | **0** |
| The ring (`buf=`, after a track's first 3 s) | 1370-1464 ms, mean 1444, median 1448. The 1370 came at the end of an Opus track, just before its join into an MP3. Track starts dip to 534 ms while the ring refills |
| Decode load (`load=`, after a track's first 3 s) | Graduation's FLACs 23.5-31.6 % (mean ~26.5 %); Opus 44.2-44.7 %; the mixed album's MP3s 33.4-39.0 % and FLACs 27.5-32.5 %; Discovery 32.5-40.2 % (~37 % with the Dance tab). The highest line was 57.5 %, at a FLAC to Opus join while the next track decoded ahead |
| Internal RAM free | Linked: `ram=` 46-54K, median **52K** (447 lines). After the headphones dropped: 53-57K, median 57K (117 lines). The lowest (`min=`) was **45K**, reached at the soak's start, with a FLAC playing and the link up, after the Opus bench and the seek tests |
| `pass_max` | FLAC at most 32,676 us; Opus 17.3-18.8 ms; MP3 14.0-30.5 ms |
| Decode task stack | `stack_free` 4292 B at the lowest, set by the Opus bench before the soak |
| Gapless joins | `joins 14 continuous, 4 after the tail (0 late); heard 18; cuts 0, too late 0, retried 0; opens failed 0, empty 0`. Continuous: FLAC to FLAC 5, Opus to Opus 2, MP3 to MP3 6, MP3 to FLAC 1. After the tail (a change of rate, 48 kHz Opus against 44.1 kHz): FLAC to Opus 2, Opus to MP3 1, and Repeat All's wrap from the last Opus track to One More Time. The next track opened in 10-47 ms, with 1349-1454 ms of the track before left |
| Dance tab | 216 five-second windows, 122 of them dancing; 30.25 fps median while dancing (15.6-31.0); 10 fps idle; `tracker=` at most 10.45 %; 45 locks, 43 losses ("lost the beat"), 2 tracker resets (one each time the tab came up) |
| Errors | none in the soak (no Guru, panic, abort, backtrace, reset or `[E]` line). The session's only `[E]` was the known `esp_avrc_tg_set_psth_cmd_filter` line at each of the 3 Bluetooth connects, all before the soak |
| Bluetooth | linked and idle until 37 minutes in, when the headphones switched themselves off (`[bt] A2DP down (closed)`). The reconnect paged them 3 times and then rested. The music stayed on the speaker and didn't stop |

## 6. The UI

Screenshots (`X`) were taken while playing in silent mode, and each one
was looked at. Nothing was clipped, overlapping or missing.

- **Now Playing:** a Discovery track with its cover, 0:13 / 3:57, "7 of
  35 · Speaker", the Repeat sign under "...". Silent mode shows the volume
  as 0 %.
- **Library:**
  - Artists: album and track counts, with Daft Punk tinted as what plays;
  - Albums: the covers, with Discovery tinted;
  - Folders: "Play all 113" and "+ Queue", then the folders.
- **Queue:** "Queue 7 of 35 · 25+ min", Edit, and the playing row marked.
  The tab's badge reads 28.
- **Dance:** the crab at 141 BPM, locked, confidence 100 %, "dancer Crab".
- **Output:**
  - during a reconnect: "Looking for SPYDRONE..." with Cancel;
  - resting: "Not connected", Connect, "They'll reconnect when switched
    on.";
  - the list's end: CPU speed 240 MHz, Bluetooth power Normal, Touch
    calibration "Calibrated on this Core2", About.

  The first Output shot had a 1-px dark line across Cancel. That came from
  the capture, not the drawing: a log line printed in the middle of the
  screenshot's data split one of its rows. The retake was clean.
- **About:**
  - Battery 100 %; Storage "microSD card, 1023.9 GB", now the card's own
    size;
  - 113 tracks, 7 artists, 10 albums; Headphones SPYDRONE; "240 MHz;
    Normal (-12..+3 dBm)";
  - Memory free "RAM 57 KB (low 45), PSRAM 2.6 MB";
  - **Version (2026-10-07, ELF c221a08e) v0.6.0-37-g221d99d**, the same as
    the banner;
  - the licence (GPL-3.0-or-later), the source link and "Show the tips
    again".
- **The Playback menu ("..."):** Shuffle Off, Repeat All, Sleep timer
  Off, as the log said (`the playback menu (shuffle off, repeat all, sleep
  timer Off)`).
- **The navigation menu** (a tap on the title): Go to artist (Daft Punk),
  Go to album (Discovery) and Go to folder, under the track's title.

Lists scrolled at 23-30 fps, with no frame over 35 ms. Each screenshot
took 23-32 s while playing, and the underruns stayed at 0.

## 7. The DIO image

**(a) Flashed by the flash script.** Two cold resets with `z` booted the
DIO build (ELF b699f82f): ota_0, schema 2, the index from the cache, the
queue and an MP3 resume point (`anchor: MP3 frame ... exact`) restored.
But the ROM printed `mode:DIO, clock div:1`, which is 80 MHz, not this
image's 40. Reading the flash back (read-only) showed why. The bootloader
at 0x1000 had the header `e9 03 02 4f`, where the built file has
`e9 03 02 40`. The script's `--flash-mode dio --flash-freq 80m` had
rewritten its frequency. The app's header was unchanged. The checks on
that header passed: One More Time 4.3x twice, the FLAC 4.6x, and 2
minutes of the mixed album (MP3, MP3, FLAC, FLAC, Opus) with 0 underruns
and 4 joins.

**(b) As built.** The same four files were flashed again with
`--flash-mode keep --flash-freq keep --flash-size keep`. Two cold resets
with `z`:

- **`mode:DIO, clock div:2`** (40 MHz, as built); the banner with ELF
  b699f82f, ota_0, schema 2, the index from the cache;
- the queue restored with `resume point: 0:06 into 34; anchor: Opus
  sample 298805`;
- the banner at 2.10 s and the console at 3.33-3.34 s.

| | DIO at 40 MHz | QIO (section 3) |
|---|---|---|
| One More Time (MP3) | 4.67 s and 4.66 s = 4.3x (23.3 %) | 4.8x |
| Graduation's FLAC | 4.35 s = 4.6x (21.7 %) | 4.7x |
| `ms128k.opus`, decode only | 6.30 s = 3.2x (31.4 %) | 3.5x |
| The same, converted to 44.1 kHz | 8.00 s = 39.9 % of a core (the converter 8.4 %) | 36.5 % |

2 minutes of the same five entries played with 0 underruns. The ring was
1416-1462 ms, the load 28.4-50.5 %, RAM 59-63K, with 3 continuous joins
and 1 after the tail. No Guru, panic, abort or `[E]` line came in any
DIO session. Then QIO was flashed back. Its header has nothing for those
flags to change. It booted at `clock div:1`, ELF c221a08e.

**The release isn't affected.** The released `-dio-full.bin` is the built
merged image, which `package_release.py` checks against the built
`bootloader.bin`. `esptool write-flash` keeps an image's header unless
it's told otherwise, and the release notes' lines don't tell it. The
parts zip's `flash_args.txt` holds only offsets and file names.

## 8. Covered by earlier runs

These ran on the device on earlier builds, with silent mode on and the
user's queue restored after each.

| What | Build | Result |
|---|---|---|
| Opus M0 gate, parts a and b (OPUS.md 4) | b6415d3 (feature/opus), 2026-10-04 | The speed, the stack, the exact lengths and the 30-min soak passed. G5 (RAM) and G6 (the longest pass) failed, and M2 fixed both. G7's MP3 slowdown wasn't Opus's: the cache pin fixed it |
| Opus M2, parts a and b (OPUS.md 8.11) | 68c6926 (feature/opus), 2026-10-06 | G5 56K steady with the headphones linked (the converter's table in PSRAM, Ot1). G6 10.3-26.5 ms. G6 at 160 MHz passed the 30 ms bar. The lengths, the joins, the refusals, the library and the seek bar passed, and so did a 20-min linked soak (22 of 22 joins continuous, 0 underruns) |
| Opus M3 (seeks and resume points) | 4e701c9 (feature/opus), 2026-10-06 | Every seek and resume landed on the exact sample. The latency (270-460 ms) and the damaged file's step (over budget) went to M4 |
| Opus M4 part a | 5339b08, 2026-10-06/07 | Passed: the exact landings, the host's check of the samples, the open cache, Repeat One, Repeat All and shuffle on Opus, G5 (63K steady, unlinked), G6 (at most 21.9 ms), MP3 4.8x and FLAC 4.7x. Failed: S4, S5 and F2dmg (see the notes below the table) |
| Opus M4 part b | 5339b08, 2026-10-07 | 60 min of the mixed album on Repeat All, with the Dance tab and the headphones linked: 87 of 87 joins as their rates say, 43 of 43 Opus ends exact, 0 underruns, `ram=` 54K steady (min 49K), load median 47.8 % |
| The seek bar (SEEK-BAR.md 11 and 16) | 68c342b, 2026-10-02 | No firmware defect |
| The MP3 cache pin | the fix's builds, 2026-10-04 | 4.8x on four forced layouts; 4.8x on 5339b08 and here |
| The Pair screen staying lit | a working build with the change, 2026-10-04 | It held lit through a whole 2-minute search (`held lit while the Pair screen searches`), then printed the search's summary |
| Now Playing's menus, shuffle and repeat (QUEUE-MODES.md 10) | 065ec1f; its fixes on 35e0841 | The first run found two faults (Shuffle all's Undo left shuffle on; Repeat One's sign didn't read). 2595d90 fixed them, and the run on 35e0841 checked the fixes |
| The card: About's size and the messages | c558ac6 | About's size comes from the card. The five card states (exFAT, NTFS, GPT, unreadable, no card) were shown with the console's simulated states and checked in screenshots |
| The file-name rules | 4300fdd | The Library's artists, albums and folders on the user's card, old build against new. The index rebuilt once at the first boot, then loaded from the cache |

The M4 part a failures:

- **S4:** the seek bar's same-second miss. 221d99d fixed it, and section 4
  checked the fix.
- **S5 (latency):** a 128k seek took a median 252 ms over 23 seeks
  (M3: 374), where OPUS.md 10.4's model expects ~165-215 ms.
- **F2dmg:** the longest step was 15.2-19.0 ms on two of the three files,
  against a 15 ms budget. `pass_max` was at most 19.95 ms, within G6's 30.

S5 and F2dmg missed their speed targets, but nothing went wrong: no
underrun, every landing exact. 221d99d's message gives the figures as
they are (a 128k seek ~250 ms).

## 9. Not run

- **The user's listening checks:**
  - SEEK-BAR.md 11.13-11.15;
  - the Opus gates' "attended" row: a seek sounds like the same second,
    and Repeat One's seam has no gap.

  Every agent run is silent.
- **Music over the headphones.** Silent mode keeps the output on the
  speaker, so in this run and in M2 and M4 the A2DP link was up but
  carried no music.
- **Pairing with the headphones in pairing mode.** The bond was kept, and
  the headphones reconnected by address.
- **Spare exFAT, NTFS or GPT cards.** The messages were checked with
  simulated card states only, and this run used the user's FAT32 card.
- **An update straight from the 0.6.0 image.** The card and NVS were
  already at this candidate's versions (index 5, schema 2), written by
  earlier dev builds. The one rebuild at the first boot, and the move from
  schema 1, weren't seen on 221d99d. The host tests cover the refused
  older index versions.
- **Going back to 0.6.0.**
- **SEEK-BAR.md 11.10 (Waiting) and 11.12 (the sleep timer), and 11.6's
  slow-motion video.**

## 10. Notes

- **The flash script** (section 7) should pass `keep` for the flash
  mode, frequency and size, as the copy used in 7(b) does. This affects
  the test procedure only.
- **The first boot after a flash isn't logged**, because the script
  stops the daemon and esptool resets the board as it finishes. Flashing
  with esptool's `--after no-reset`, then a `!reset` once the daemon is
  back, would catch it.
- **Internal RAM with the headphones linked:** a median of 52K free in
  the soak and a low of 45K. That is below the 0.6.0 soak (min 54K), which
  had no link and no Opus. It is within G5's floor (steady 50K or more,
  never under 42K) and close to M4 part b (54K, min 49K). The decode
  stack's low of 4292 B is Opus's (M2 measured 4,288 B); 0.6.0's 13652 B
  was MP3 and FLAC only.
- **A pause saves the resume point twice**, 31 ms apart, the second time
  with a later anchor. The run saw this on Opus and on MP3. It does no
  harm.
- **Other tasks' log lines get spliced into console output:** one split a
  screenshot's row, and a Bluetooth `[W]` line split a row of an `l`
  listing. This affects only the serial console, screenshots and scripts
  that parse them.
- **Scan by name:** `B` says "scan by name: no" on this build, where the
  user's earlier dev build said SPYDRONE. That build had a headphone
  name built in from a local setting; a release build has none. The
  remembered headphones still reconnect by address.

## The user's state

Noted at 10:24 on the build the user was running (5339b08), before
anything else. The user had added the Opus test folder (36 tracks) to the
queue, played and sought in it, and let it play to the end, all in silent
mode.

| | Before | After |
|---|---|---|
| Queue | 63 tracks: Graduation (0-12), Mountain of Memory (13-26), the Opus test folder's four albums (27-62); at 63, 0 up next, stopped at the end of entry 62 | the same list, line for line, at 63, 0 up next, stopped. Rebuilt with `qx`, `q+1`, `q+5`, `q+2`, `q+3`, `q+7`, `q+9`, then `i62` played silently to its end |
| Shuffle, repeat | off, off | the same |
| Resume point, start point | none, none | the same |
| Now Playing | track id 108, entry 62 of 63, 0 / 0 ms, the bar inert | the same |
| Output | speaker; 0 % in silent mode (speaker 0 %, Bluetooth 30 %) | speaker; 30 % (speaker 30 %, Bluetooth 30 %). The speaker's volume isn't saved: it's 30 % at every boot, and showed 0 % only in silent mode |
| Bluetooth | SPYDRONE remembered; scan by name SPYDRONE; signal pairing off; fresh-unit test off | remembered (the bond kept); scan by name: none in this build (see 10) |
| Settings | sleep timer off; idle off after 20 min; touch calibrated, haptics on, rail ticks on; gapless and LAME trim on; `O` at Ot1 (the default, the pinned PSRAM block); screen off after 30 s, brightness Medium; CPU 240 from boot; Bluetooth power Normal | the same, with the same touch table |
| NVS | 112 of 2016 entries used | the same |

What differs:

- The queue file's generation went from 250 to 252 (the rebuild).
- Silent mode, which no boot keeps, is off after the final normal boot.
- The Library tab's stack isn't saved across a boot.

The device was left on the candidate (QIO, ELF c221a08e), after a normal
boot, stopped. The headphones were off.
