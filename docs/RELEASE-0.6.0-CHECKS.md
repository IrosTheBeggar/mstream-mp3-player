# v0.6.0 pre-release device checks

The candidate: the dev branch at 8fd7a78 (the beat-tracker rework merged into the
visualizer, power audit 2, QIO by default, gapless, the pinned libmad state,
exact resume and seeks, and the speaker start fix). Run on 2026-10-02 on the
user's Core2 (COM3), `pio run -e core2 -t upload`, through the serial daemon.
Silent mode (`z`) was on after every boot before anything played. The user's
queue was noted first and restored at the end (see the end of this file).

**Result: every check passed. No errors, no underruns, no code change.**
Bluetooth wasn't checked: the headphones weren't around (see 2).

## 1. The boot (QIO)

- The build: `version: v0.5.0-19-g8fd7a78`, `flash_guard: ... flash QIO at
  80 MHz`, `firmware.parts.json` `flash_mode: qio`, `flash_freq: 80m`. The app
  is 2.18 MB, 36 % of the 6 MB slot.
- `bootloader.bin` header: `e9 03 02 4f`, so DIO and 16 MB at 80 MHz. That is
  expected: the ROM loads the QIO bootloader in DIO, and the bootloader switches
  the flash to quad itself (ENERGY.md P2). For the same reason the ROM prints
  `mode:DIO, clock div:1` at every boot.
- 3 cold resets (RTS, `rst:0x1 POWERON_RESET`), all the same: the banner
  `mstream-mp3-player v0.5.0-19-g8fd7a78 (commit 8fd7a78, 2026-10-02), ELF
  50a34d52`, `[flash] running ota_0`, `[nvs] schema 1`, CPU 240 MHz (the
  default), the library (77 tracks) and the queue (27 of 27, at 9, the resume
  point 0:55) restored. The console was up 2.8 s after the reset. Internal RAM
  after the UI: 80K free (min 77K).
- The version says v0.5.0-19 because the build isn't tagged yet. The release
  tag gives it v0.6.0.

## 2. The soak: 64 minutes on the speaker, silent mode

Daft Punk's *Discovery* (14 MP3s) then Kavinsky's *OutRun* (FLAC), queued by
`qp0` and `q+4`, played through from the first track. The screen was kept lit
(`Ps1` every 15 s). The Dance tab was shown for 10 + 15 + 19 of the 64 minutes,
and Now Playing for the rest.

| | Result |
|---|---|
| Length | 64.1 min (832 stats lines while playing), tracks 1-16 |
| Underruns | **0** |
| The ring (`buf=`) | 1416-1462 ms, mean 1442 ms; never below 1416 |
| Decode load (`load=`) | 19.8-49.9 %, mean 40.9 % (MP3 with the Dance tab ~39-43 %; FLAC 20-30 %; the 49.9 % is one line at the end of a FLAC while the next one opened) |
| Internal RAM free | 61-65K while playing; the lowest (`min=`) **54K** |
| Decode task stack | `stack_free` 13652 B at the lowest (of 16 KB) |
| Gapless joins | **15 of 15 continuous** (13 MP3 to MP3, Too Long to Prélude MP3 to FLAC, one FLAC to FLAC); decoded ahead with 1406-1446 ms of the track before left, opened in 14-19 ms; 0 cuts, 0 too late, 0 retried, 0 failed opens |
| Dance tab | 26.5-29 fps in the 5 s windows spent dancing (30 fps target; 10 fps idle while unlocked); `tracker=` at most 17.3 %; 43 locks and 40 losses ("lost the beat") over 3 tracker resets (one per time the tab came on) |
| Errors | none (no Guru, panic, abort, `[E]` or failure line) |

**The tracker's locks.** It locked within 3-5 s of each Dance tab reset on One
More Time (122.6 BPM), Short Circuit (109 BPM) and the OutRun tracks. It let go
and relocked often on Aerodynamic and Digital Love, with long beatless or soft
stretches, and on Too Long (63 / 126 BPM). That is the course BEAT-TRACKER-EVAL.md
describes (the joins relock on the next track, and soft passages let go). It's
not a regression.

**Bluetooth: not run.** The headphones (SPYDRONE) never linked. The reconnect
paged them 3 times and then rested. The 10 minutes of silence over Bluetooth
weren't done.

## 3. Resume after a reset

Paused, reset through the daemon (`!reset`), `z`, then play:

| Track | Paused at (saved) | After the reset | The start |
|---|---|---|---|
| Blizzard (FLAC) | 2:40.592, `FLAC sample 7082132` | `resume point: 2:40 into 16 ... anchor: FLAC sample 7082132` | `FLAC: starting 2:40.592 in (libFLAC's seek to sample 7082132, 86 ms)` |
| Aerodynamic (MP3, VBR) | 0:12.981, `MP3 frame at 378002 + 3345 samples, preroll 374974, exact` | the same anchor | `MP3: starting 0:12.981 in, of 3:27 (its resume anchor; exact): byte 374974, frame 378002 + 3345 samples` |

Both landed on the paused sample.

## 4. Seeks with `qs`

| Track | `qs` | The start | Errors |
|---|---|---|---|
| Aerodynamic (MP3) | `qs90` while paused | `MP3: starting 1:30.000 in, of 3:27 (LAME's TOC inverted; the time asked): byte 2500756`, ring steady in 1.6 s | none |
| ProtoVision (FLAC) | `qs120` | `FLAC: starting 2:00.000 in (libFLAC's seek to sample 5292000, 125 ms)` | none |
| ProtoVision (FLAC) | `qs200` | `FLAC: starting 3:20.000 in (libFLAC's seek to sample 8820000, 83 ms)`, then a gapless join into Odd Look (`heard: the joined track plays`) | none |

## 5. The UI

Screenshots (`X`) of every tab were taken and looked at: Now Playing (Odd Look,
cover, paused 18 of 27, Speaker), Library (Artists, Albums and Folders),
Queue (18 of 27, 9 up next, the playing row marked), Dance (the crab, "no
beat" while paused), Output (SPYDRONE not connected, Speaker in silent test
mode, Line out), the Output list's end (CPU speed 240 MHz, Bluetooth power
Normal, Touch calibration, About). Nothing was clipped, overlapping or
missing. Silent mode shows the volume as 0 %.

**About:** Battery 100 %, microSD card 1023.6 GB, 77 tracks / 6 artists / 6
albums, headphones paired, **Version (2026-10-02, ELF 50a34d52)
v0.5.0-19-g8fd7a78**, the same as the banner. The licence row and the source
link are shown too.

## The user's state

| | Before | After |
|---|---|---|
| Queue | 27 tracks (Graduation + Mountain of Memory), at 9, 18 up next | the same list, line for line, at 9, 18 up next |
| Resume point | 0:55.000 into 9, anchor none, start point waiting 0:55 | the same (rebuilt with `qp1`, `q+3`, `i8`, a pause, `qs55` in silent mode) |
| Now Playing | track id 37, entry 8, 55000 / 237506 ms | the same |
| Touch, haptics | calibrated, haptics on, rail ticks on | the same |
| Bluetooth | SPYDRONE remembered, scan by name, signal pairing off | the same |
| Settings | speaker, volume 30 % (Bluetooth 30 %), sleep timer off, idle off after 20 min, CPU 240 from boot, screen Medium | the same |

The queue file's generation went from 166 to 168 (the rebuild). The device was
left on the candidate, after a normal boot (no silent mode), stopped.
