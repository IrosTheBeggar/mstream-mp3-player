# v0.14.0 pre-release device checks

The candidate: `main` at 5ea8d34, the merge of #27. Its firmware code is
218822a's (the merge adds nothing else), which these checks ran. These
PRs were merged since 0.13.0 (the release notes' "What's new in
0.14.0"):

- #54: `NEXT_RELEASE` 0.14.0;
- #55: **Touch when off** (Output): a touch on the dark screen does
  nothing by default; Wake, and Track (the glance); NVS schema 6's
  `screen`/`touch_off`;
- #56: lists drawn on the panel's scan, text straight into the sprites,
  the A-Z rail's scrub a row a loop pass;
- #27: the card transfer's player half:
  - the board's clock: the RTC read at the boot, `@time`, `@status
    clock=`; NVS schema 6's `clock` namespace;
  - the play log, `/.player/plays.jnl` (docs/METADATA.md 2.20, 3.10),
    `history=1` in `device.txt`; NVS schema 6's `plays` namespace;
  - Format on the player: the empty state's Format..., `@format`, the
    two questions and the 2 s hold by hand, the format at the next boot.

Two parts: the checks run on 2026-11-10 on main's exact code (section
2), and the upgrade from a released 0.13.0's settings (section 3). Every
run was on the user's Core2 v1.3 (COM3), through the console, in silent
mode (`z` after every boot, before anything could play), on a spare
32 GB card: never the user's music card. No Bluetooth, power or
forget command was sent (`B`, `P`, `c`, `f`).

These earlier device runs are not repeated here: #55's (ENERGY.md
section 4, step 2, on that branch's build, 2026-11-09: the three
choices, the glance drawn and timed, the power button from it, the row
saved) and #56's (docs/UI-SPIKE.md's last section, on that branch's
build, 2026-11-09: the frame rates, the rail scrub, the fast text
checked pixel for pixel, a 5 minute soak, Bluetooth silence streamed).

## 1. The build

Built at 5ea8d34 (main, the candidate: `release/0.14.0` adds only
docs), from PowerShell with MSYSTEM removed, with no `local.ini` flags:

| | `core2` (QIO: the main image) |
|---|---|
| Build | SUCCESS |
| `version:` | `v0.13.0-47-g5ea8d34`, ELF 97b3a1f2 |
| `iram_diet:` | 51 of 51 libc objects moved, the MP3 hot set pinned |
| `cache_guard:` | ok: at most 2 hot lines in a set (bench and playback) |
| `flash_guard:` | `app 4.31 MB = 72% of the 6.00 MB slot; ... flash QIO at 80 MHz` |
| RAM, Flash | 59,064 B; 4,423,679 B (70.3 %) |
| `firmware.bin` | 4,523,920 B: 4.52 MB, 71.9 % of the 6 MB slot (0.13.0: 4,445,536 B, 4.45 MB) |

The app is byte for byte as large as 218822a's image, the one the
checks below ran: its code is the same.

## 2. Main's code on the Core2 (2026-11-10, build v0.13.0-46-g218822a)

| Check | Result |
|---|---|
| The boot on an NVS written by earlier builds (schema 6) | The play log's epoch, seq and boot count and the clock's phase were kept; the log went on from the next seq |
| The clock | Read from the RTC at the boot, its next tick caught within 6-7 ms. `@time <ms>` answered `@time ok`, and read back within 5 ms. Before the set the RTC was 74.6 s behind after about three weeks: about 3.5 s a day on this unit |
| The play log | Plays written in 14.8-16.6 ms, in silent mode. Tracks under a second are not logged |
| The format's questions, declined | Cancel on either step, and each step's minute, 15 times in all: nothing formatted |
| Scripted holds | A scripted 1.7 s hold let go at 1,874 ms. Scripted holds of 2.1, 2.6 and 3.0 s ended with `[format] a scripted finger never erases a card: hold it by hand`, also while tracks played back to back (slow passes up to 58 ms; no hold dropped for a stalled loop) |
| A real format, by hand | A tap on Format..., the red button held (`Format held 2006 ms`), `@format 0`, the restart, the format in 7.8 s, `@format done size=31914983424 free=31902269440 cluster=32768`, then `@status` with `card=fat32` and `cid=-`. Two touches on the dark screen before it were swallowed as wakes, as designed. Once more through a throwaway build that let the scripted finger confirm: 7.9 s |
| The formatted card on a computer (Windows) | One MBR partition of type 0x0C at 4 MiB, FAT32, 32 KB allocation units, 973,584 of them; `chkdsk` (read-only) found no problems; `/music` empty; `/.player/device.txt` with `history=1` |

## 3. The upgrade: a 0.13.0 unit becomes 0.14.0 (2026-11-10)

The question: does a board whose settings were written by the released
0.13.0 take the new firmware cleanly? Run on the same Core2, with the
user's settings backed up first and put back at the end (section 4).

**The card:** no card was in the player during this check. The spare
had been moved to a computer's reader for section 2's look before the
check began. The first `@status` still said `card=fat32` (an idle player
asks nothing of its card, so the running firmware hadn't noticed it was
gone); from the first restart on the slot was empty at every boot of
either firmware (`GO_IDLE_STATE failed`, then `[storage] no card
mounted; its first sectors: unreadable`; `@status card=none`). So this
check covers the settings and the clock, not the card: nothing was
played (its step 6 needs a card with tracks), and the play log stayed
off at each boot (`[plays] no play log: the player's records aren't on a
card`), so its new epoch on a 0.13.0 NVS wasn't seen.

**0.13.0** was built from its tag in a fresh clone (08596d2, no
`local.ini`): `version: the image says v0.13.0, ELF af94a510`, every
guard ok, `firmware.bin` 4,445,488 B (the release PR measured 4,445,536 B
at fdd4585, the same code: the size wasn't compared with the published
file, nor its bytes).

| Step | Result |
|---|---|
| A fresh 0.13.0 unit | The settings partition erased, 0.13.0's app written, the first boot logged from its reset: `v0.13.0 (commit 08596d2, 2026-11-08), ELF af94a510`; `[nvs] schema: none yet (a fresh NVS, or v0.5.0-beta.1's): 5 written`; `[ui] up: the first boot's language picker` |
| Settings, through the console | `uiLde` (`[ui] language: de (Deutsch), saved`), `qR1` (repeat all), `ah0` and `ar0` (haptics and the rail's ticks off), `D1`, `Dm1`, `Df65`, `Dt1`, `D0` (Auto DJ: Stay on seed, 65 %, BPM continuity on, then off); `[nvs] saved queue/repeat, autodj/on, autodj/mode, autodj/floor, autodj/match, autodj/moods (6 keys, 3 ms)`. The volume isn't a saved setting, and silent mode keeps the speaker at 0: not set |
| 0.13.0 again | `[nvs] schema 5`; `language: de (Deutsch)`; `repeat: all (saved)`; `[dj] settings: Auto DJ off, Stay on seed, strictness 65 %, BPM continuity on, Harmonic mixing off, mood filter none (saved)`; `haptics off, rail ticks off` |
| The upgrade | main's image (218822a) written, its first boot logged from the reset: `v0.13.0-46-g218822a`, ELF d5915c44; **`[nvs] schema 5 -> 6: migrated`**; every setting above as 0.13.0 left it; `touch when off: Nichts` (Nothing, the default an updated unit gets); `[clock] unset: the RTC says 2026-11-10 21:11:54 UTC, a time this player never set (another firmware's or the factory's); a computer's @time sets it` (0.13.0 never set the RTC, and this NVS has no mark) |
| `@status`, `@time` | `@status ... clock=unset ...`; `@time` answered `@time unset` |
| `@time <the computer's ms>` | `[clock] 2026-11-10 21:12:27.479 UTC, set by a computer's @time (it was unset); the RTC at its next second`, `@time ok`, then `[clock] the RTC set to 2026-11-10 21:12:28 UTC on its second (written 4 ms after it); its seconds begin +8 ms from the true ones (within 6 ms): kept for the boots`. A bare `@time` 3.5 s later read 4 ms behind the computer's stamp of its send; `@status ... clock=ok` |
| A reboot | `[nvs] schema 6`; `[clock] 2026-11-10 21:12:45 UTC, from the RTC (to the second; ...)`, then `at the RTC's tick, within 6 ms (its seconds +8 ms from the true ones, the last @time's); the boot's second was 0.364 s behind`; `@status ... clock=ok`; a bare `@time` 10 ms past the computer's stamp of its send. The settings as before |

No boot logged a Guru, a panic, a backtrace, a `task_wdt` or a LOW
stack; the only `[E]` lines were the SD driver's, for the empty slot,
and NVS's not-found lines on the erased partition. Every
boot was a single boot: no restart loop.

One console detail, not a defect of the release: on the erased NVS the
first boot's touch check stays up (nobody tapped its dots), the tab bar
isn't drawn, and `@status` says `bat=-` (the battery is read with the
tab bar); on the user's settings it said `bat=100`.

## 4. The user's state, put back

- **Backed up first**, on 218822a with the user's settings: the
  settings partition (0xC10000, 64 KB) and the flash's first 0x610000
  bytes (the bootloader, the partition table and the app), with their
  SHA-256s, kept off the repository.
- **Restored:** the settings partition written back with
  `--after no_reset` and read back before any boot: its SHA-256 matched
  the backup's. main's image (218822a) written again ("Hash of data
  verified"), then a boot: `[nvs] schema 6`, the user's language,
  repeat, Auto DJ, screen and Touch when off settings back, `[clock] ...
  from the RTC`, then caught at its tick; `z`; `@status
  fw=v0.13.0-46-g218822a elf=d5915c44 card=none ... clock=ok`.
- **Not seen:** the play log's line with the user's epoch and its boot
  count one higher (no card was in: it is read only with a card).
  The settings partition is byte for byte the backup, which holds that
  epoch and count.
- **The RTC** keeps the time this check's `@time` set (the right time,
  to a few ms); the restored settings keep their own, older phase
  (+12 ms, against the new write's +8 ms).
- **The card** was still in the computer's reader at the end: the slot
  is empty, see section 3.

## 5. Not run for this release

- exFAT and GPT cards formatted on the player (host-tested only, on
  simulated cards: `test_card_mkfs`);
- a card pulled during a format;
- a card pulled and put back around plays;
- the idle power-off on battery;
- Bluetooth playback;
- a long listening session;
- the user's 1 TB music card with this build;
- in the upgrade check (section 3): a card, so no play on 0.14.0 after
  the update, no new play-log epoch on a 0.13.0 NVS, and no
  `library.idx` read across the update (its format didn't change);
- going back from 0.14.0 to 0.13.0 (from the code: 0.13.0 leaves a
  schema-6 NVS as it is);
- a real finger on the lists (#56's own list);
- the `core2-dio` image (built and checked by #27's gate, not booted
  here).
