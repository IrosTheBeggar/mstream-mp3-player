# v0.15.0 pre-release device checks

**Sections 1 and 3 run on 2026-11-25 (the owner's "Upgrade check
only"); sections 4 and 5 not run (section 8).** Sections 1 and 3
to 5 say what to run and what must show; their results go in beside
them, as in docs/RELEASE-0.14.0-CHECKS.md. Section 2 lists what the
PRs' own device passes already covered, with their records. Once these
have run, the release notes' "Tested on" and "Not checked on the Core2"
bullets are brought up to date from them.

The candidate: `main` at e8515f2, the merge of #69. Its firmware code is
4c3e613's, S3b's last code commit (F1's fix): every commit after it
changes docs alone (`git diff 4c3e613 e8515f2` touches
`docs/LISTENING-STATS.md` only), and main 0efc69c's CI image, which ran
S5a and S5b on the owner's music card, is the same code.
`release/0.15.0` adds the release notes and this file. These PRs were
merged since 0.14.0 (the release notes' "What's new in 0.15.0"):

- #58: `NEXT_RELEASE` 0.15.0;
- #60: docs only (METADATA part 2: the transfer software's side of the
  play log); no firmware change;
- #62: Bluetooth: connectable and paging 0.3 s after the profiles are
  set up (no 10 s sleep), a burst's pages back to back, 8 tries for a
  listener's ask (Output's "try 2 of 8"), `PlayGate`'s backstop 65 s,
  each page's outcome in the log;
- #59: the listening stats' S0: the plan, the contract,
  `tools/statsref.py`;
- #63: the hole hint: a nearly full card's queue saves no longer walk
  the whole FAT;
- #64: the card worker's work held through a headphones' radio link's
  set-up and until AVRCP is up, then 3 s; malloc's internal limit
  4,095 B (each File's 4 KB buffer to PSRAM first);
- #61: the listening stats' S1: `/.player/stats.bin` and its base, the
  folds, the saves, the trim only once the stats hold a file's plays,
  the seq rule (5.6), the console's `H`;
- #65: Now Playing's empty state drawn once; `-Werror=shadow=local`;
- #66: S2: the rankers and the library walk; `Hr`, `Hl`, `Hw`, `Hp`;
- #67: S3a: the visits' asks, the snapshot in the Dance tab's block,
  the tag scan's rest gate, the lit cap; `Hv`, `Hn`, `Hu`, `Hk`, `Hc`;
- #68: S3b: the screens (the entry row, the hub, the shelves, Last 7
  days, the empty state), `uiS` and the made-up views, 84 strings;
- #69: docs: S5a and S5b, run on the owner's music card.

**The rules for every run.** The Core2 v1.3 on the PC's USB serial
port, through the console, in silent mode (`z` after every boot,
before anything could play), one boot a run. No Bluetooth, power,
forget or settings command (`B`, `P`, `c`, `f`, `uiL`, `ah`, `ar`,
`qR`, `D0`/`D1` and the like), no `@time` and no `@format`: the
settings checked are the ones the board has. Then:

- **Which card.** The **spare** 32 GB card for anything that boots
  0.14.0 or plays (sections 3, 4 and 5). The owner's **music card** only
  with main's own build (this candidate), with nothing played (no
  `qp`, `n`, `p`, space or tap that plays), and only on the owner's
  word: never 0.14.0 on it since S5a (on a synced card 0.14.0 clears
  the history's acknowledged files without counting them). The board
  holds the music card at the start (left so after S5b): the owner
  takes it out at 3.0, before any image is written, and it goes back
  only at section 6. For each step that names the spare, the owner moves
  it between the board and the PC's reader. Every swap is made with the
  board parked in its ROM bootloader (esptool `--after no-reset`,
  "Staying in bootloader"), as S3b's and S5a's runs did, never while a
  track plays.
- **The settings move forward only.** Before the first write the
  settings partition (0xC10000, 64 KB) and the flash's first 0x610000
  bytes are read and verified into the owner's backup folder, off the
  repository, with their SHA-256s: for the record. Unlike 0.14.0's
  section 4, **no settings partition is ever written back**: an older
  copy would roll the play log's seq back under plays already on a
  card (5.6's seq rule, C5; docs/LISTENING-STATS.md 9.9: "never to be
  written back"). Every image is a full image written at 0x0 with no
  erase (the release notes' Update command, plus `--after no-reset`)
  and checked by `verify-flash`, both with `--after no-reset`, so
  nothing boots until the step's card is in and each first boot is
  logged: esptool writes nothing past 0x610000, so each firmware boots
  on the NVS the last one left.
- **Predicted first.** Before each boot that changes the spare's
  `/.player`, copies of its `/.player` and of its root (`/.mstream`) are
  taken on the PC, read-only, SHA-256 checked, and the outcome worked
  out as S5a did (9.9): `tools/playlog.py read` on the log's files (the
  slots, devices, epochs), the root's ACKS read from the `/.mstream`
  copy and given to statsref as `--acks`, and `tools/statsref.py boot`
  (what 0.15.0's boot folds, saves and trims, HEAD, the next seq). For
  0.14.0's boots, by METADATA's "Trimming by ACKS": a file whose every
  play slot ACKS acknowledges is deleted, uncounted.
- **Nothing private in this file:** no device id, epoch, card id or
  headphones' name; the logs, the copies and the screenshots stay off
  the repository.

## 1. The build (run 2026-11-25: main e8515f2's CI build)

Either main e8515f2's CI build (`firmware.yml`'s core2 image) or a
local build at `release/0.15.0`'s head, from PowerShell with MSYSTEM
removed and no `local.ini`; both are the same firmware code. Read from
main e8515f2's CI run (`firmware.yml`, the build job's log and its
artifact, every file checked against its `SHA256SUMS`); the image that
sections 3 to 5 write is that artifact's `-full.bin`.

| | Expected (from S3b's builds; `firmware.bin` also from this candidate's CI build) | Result |
|---|---|---|
| Build, `core2` and `core2-dio` | SUCCESS, every guard: `iram_diet` 51 of 51, `cache_guard` and `flash_guard` ok, `shadow_guard` | Both SUCCESS (166 s each), every guard as expected: `iram_diet` 51 of 51 (`sections.ld` 140,237 to 121,839 bytes), `cache_guard` ok, `flash_guard` ok, `shadow_guard` on 227 files; the host tests before them: 2,531 of 2,531 |
| `version:` | `v0.14.0-24x-g<sha>`, its ELF | `v0.14.0-242-ge8515f2`, ELF b1d7416b (core2-dio: ELF 2020f38f); the ELF file's SHA-256 begins b1d7416b |
| `flash_guard:` | 75 % of the 6 MB slot (74 % at main 37ed055) | 4.48 MB, 75 % of the 6.00 MB slot; `firmware.factory.bin` ends at 0x48a850 (core2-dio 0x48a870); QIO at 80 MHz (core2-dio DIO at 40 MHz) |
| `firmware.bin` | 4,696,144 B in main e8515f2's CI build (`flash_guard`: `firmware.factory.bin` ends at 0x48a850, the app from 0x10000; core2-dio 4,696,176 B; 0efc69c's the same). S3b measured 4,696,128 B at 4f67d31 (`docs/LISTENING-STATS.md` section 1). F1 adds 56 B of `.flash.text` over that figure, and 7248aae's clean builds give `.flash.rodata` the same total as the figure. The release's build at the tag may read 16 B less: 0.14.0's candidate 5ea8d34 built 4,523,920 B both locally and in CI, and its published `-app.bin` is 4,523,904 B. | The artifact's `-app.bin` 4,696,144 B (core2-dio's image 32 B longer), as expected |
| RAM | `.dram0.data` 25,112 B and `.dram0.bss` 36,392 B, S3b's (main 37ed055's) | The CI ELF: `.dram0.data` 25,112 B, `.dram0.bss` 36,392 B (PlatformIO's RAM line: 61,504 B used, their sum) |

The notes' "about 4.70 MB, 75 %" is checked against this row; the
release workflow's own build gives the figure that ships. Checked:
4,696,144 B is 4.70 MB (`flash_guard`'s 4.48 counts MiB) and 75 % of
the slot, so the notes' figures hold.

## 2. Already run on the device (each PR's own pass)

Every run on the owner's Core2 v1.3. All but #62's ran in silent mode
unless the row says a real stream, with nobody's finger on the screen;
#62's asks were taps on the Output card. Not repeated here.

| PR | The record | Card | Build, and its control | What it covered |
|---|---|---|---|---|
| #62 | `docs/ARCHITECTURE.md`, Bluetooth: the burst's "Measured on the device" (2026-11-17) | none read (a temporary patch kept the builds off the card) | the branch, and the old Bluetooth code flashed beside it | linked 6.6-7.1 s after a restart (old 14.7-17.3 s), 4.5-5.2 s after a minute off (old 14.6-15.6 s); the phone case with 3 and 6 tries, not 8 |
| #63 | `docs/METADATA.md` 3.2.4, "The hole hint" (2026-11-18) | spare, one free cluster | the branch; main 89188f4 | 8 queue saves: main walked the FAT every 2nd save (5,285-5,295 ms); the fix 135-146 ms |
| #64 | `docs/OPUS.md` 2.1, "G5 since the card worker"; `docs/METADATA.md` 3.3.9 (2026-11-19) | spare | a9c4636, 81402fd; main 89188f4 | internal RAM's low 35,916 B to 45,124 B in the play-before-link order; AVRCP's dip 3 KB; decode cost (`b` bench) |
| #65 | `docs/ARCHITECTURE.md`, UI: Now Playing's empty state (2026-11-20) | spare | 4b1b602; 985f4a9 | 261 slow passes to 2; 16 to 123-140 passes a second; the join 5,332 to 1,642 ms |
| #61 (S1) | `docs/LISTENING-STATS.md` section 1, status of 2026-11-19; 5.14's "Its device runs" | spare | 985f4a9 (main 987216f merged in); main 987216f (earlier c3f1f09, 94cdf0c, d4a5357 and 6e09a55, against 218822a and 89188f4) | E: the save before the trim; K: rotations, 31 slots, none lost or twice; a full log's first boot (13,106 slots: save 2,487-2,689 ms, a start held 4.39-4.75 s); the next boot's compaction 1,336-1,442 ms; visit B, a full card (98-191 ms a failed save); visit C; G5 with a link, 50,764 B on a real stream |
| #66 (S2) | 9.6, "The device pass (2026-11-20)" | spare | the branch; main 645679f | all seven lists equal statsref's; `Hr` 0.7-1.0 s, `Hw` 0.65-0.9 s; PSRAM main's outside an ask |
| #67 (S3a) | 9.7, "S3a: the device pass" (2026-11-21) and "S3a: V8-V10's device pass", its "As run 2026-11-22" | spare | b16a189, 8d56155; main de4f52f | the lit holds and the cap; the loop's pieces under 35 ms; the borrowed snapshot main's to the byte; the rest gate (a visit sent 3.9 s after a boot waited 57.6 s in all, its take 15.7 s after the walk's line); the Dance tab; G5 on a real stream 50,552 B |
| #68 (S3b) | 9.7, "S3b: device runs", its "As run 2026-11-25" | spare | a064a19, then 4c3e613 (this candidate's code) for 3b-f1 and 3b-x; main 37ed055 | 13 scripted runs and 79 shots; the hub's push 46.9 ms idle, its ask 1,602 ms idle and 2,331 ms with an MP3; the lists let go and asked again; F1 fixed; the fence with a snapshot held; the short-RCNT header; a scroll kept |
| #69 (S5a, S5b) | 9.9, "As run 2026-11-25" | the owner's music card | main 0efc69c's CI image (this candidate's code) | 15 slots folded, generation 2 compacted, as statsref predicted field for field; the owner's sync; the next boot's trim; the six lists equal statsref's; `Hw` 19,671 tracks in 319 ms |

## 3. The upgrade: the published 0.14.0, then 0.15.0 (run 2026-11-25; the spare)

The question: does a board and a card 0.14.0 used take 0.15.0 cleanly,
with the settings, the headphones, the queue and the library kept, and
the stats counted from the play history 0.14.0 left? Unlike 0.14.0's
section 3, a card is in the slot throughout.

| Step | Do | Must show | Result |
|---|---|---|---|
| 3.0 The board, read | The settings partition and the image area read and verified into the backup folder (the rules), the board left in its bootloader; then the music card taken out | Two files and their SHA-256s; never written back | Read and verified on 2026-11-25 with the board parked on main 0efc69c's CI image (both reads' `verify-flash` digests matched; this board's MAC; "Staying in bootloader"); their SHA-256s in the backup folder's README, off the repository; never written back. The music card then taken out, the board parked |
| 3.1 The spare, as 0.14.0 left cards | In the PC's reader: copies taken (the rules); then `stats.bin`, `stats-*.bin` and any `stats.tmp` or `stats.bad` moved off the card into that copy, so it holds the play history and no stats, as a card 0.14.0 used does | The card's `/.player` without stats files; `playlog.py read`'s slots, and ACKS's seq from the root's copy | The spare proven (cardcheck, and its volume serial) and copied (10 files, each copy's SHA-256 checked); `stats.bin` (61,056 B) and `stats-00000002.bin` (457,968 B) moved off, each copy proven equal first, the card flushed. The history: 6,680 slots in 2 files (6,537 made-up plays of another device; this board's own 142 test plays, to seq 737; a drop note); ACKS this board's to seq 519, the other's to 1,000: no file ACKed whole, so 0.14.0 trims none |
| 3.2 0.14.0 written | The full image attached to the v0.14.0 GitHub release, its `SHA256SUMS` line checked, written at 0x0 with no erase | "Hash of data verified" | `v0.14.0`'s `-full.bin` (its `SHA256SUMS` line checked) written at 0x0: "Hash of data verified", and `verify-flash`'s "digest matched"; this board's MAC; left in its bootloader |
| 3.3 0.14.0's boot | The spare in, boot, `z`, `@status`, `q`, `s`; then three tracks by `qp<n>`: two past 30 s, one ended by `n` under 30 s | `v0.14.0 (commit bc29b54 ...)`; `[nvs] schema 6`, no migration; the settings lines (language, repeat, Auto DJ, haptics, Touch when off), the paired headphones and the clock as the board had them; the play log's line, its next seq at or over the board's last; any trim as ACKS predicts; three `[plays] seq` lines | One boot: `v0.14.0 (commit bc29b54, 2026-11-10), ELF 08a671e0`; `[nvs] schema 6`, no migration; the settings lines recorded (English; CPU 240 MHz; Bluetooth power Normal; the touch controller; the idle power-off 10 min; repeat off; Auto DJ's; calibrated, haptics and rail ticks on; off after 15 s, Touch when off: Track; the clock from the RTC), the remembered headphones first paged 11.3 s into the boot (the board's own stamp; 0.14.0's 10 s wait); `ready: boot 180 ..., the next seq 738; ... 6680 slots of 2 files ...; 0 files trimmed`, as predicted (NVS 737, the log 737). Long Silence: seq 738 skipped at 0:36 and 739 at 0:35 (plays), 740 at 0:15 (a skip); track 4 paused, its resume point 0:05; the settle (`gs`: the scan idle, the worker not running) before the board was parked; no BAD line, no person. 21 checks PASS |
| 3.4 Predicted | The spare in the PC's reader: copies again; `playlog.py read`, `statsref.py boot` | The slots 0.15.0's first boot will fold, its save (generation 1 and its base), its trim, HEAD (plays, skips, time, since day), the next seq | 0.14.0's three slots appended to `plays.jnl` (seqs 738-740, as its log), the rest of both log files byte for byte; no stats file; `queue.txt` and `device.txt` (firmware 0.14.0) its only other writes; `/.mstream` untouched. statsref (the board's clock taken as 22:01 UTC): 6,682 slots folded (0 twice, gap 1,519); generation 1, 51,136 B (RCNT 3,962), compacted into `stats-00000001.bin` (2,510 rows, 60,312 B); no trim; HEAD 5,889 plays, 777 skips, 289 h 56 min, 6,682 slots, since 31 Oct; MARK 740 for this board; the next seq 741 or more. The log alone gives the same figures |
| 3.5 The candidate written | Section 1's image, at 0x0 with no erase | "Hash of data verified" | main e8515f2's CI `-full.bin` written at 0x0: "Hash of data verified", `verify-flash`'s "digest matched"; this board's MAC |
| 3.6 Its first boot | The spare in, boot, `z`, `@status`, `q`, `H`, `Hm`, `Hd` | The version; `[nvs] schema 6`, no migration; every settings line as in 3.3; the queue as 0.14.0 left it; `library.idx` read, no build; `[listen]`: `stats.bin` absent, every slot statsref named folded, 0 twice, saved, then the trim statsref named, with each line's ms; the next seq over MARK's and ACKS's; `H` and `Hm` statsref's; the Listening stats row once the fold ends; no `[heap] FAILED`, Guru, panic, backtrace, `task_wdt` or LOW stack | One boot: `v0.14.0-242-ge8515f2 (commit e8515f2)`, ELF b1d7416b; `[nvs] schema 6`, no migration; every settings line as 0.14.0's, the headphones' paging aside: the same headphones, first paged 1.6 s into the boot (the board's stamp), each page's outcome logged (#62); the queue restored, 7 of 7 at 4 of 7, the resume point 0:05; `library.idx` loaded (1,504 ms), no build. `[listen]`: "none on the card ...: a fresh start" (5 ms); the fold 6,682 slots, 0 twice, gap 1,519, in 53 passes and 1,695 ms on the loop with the save (S1's full log: 3,773 ms with its 2,584 ms save; the overflow's peak 262,080 B); saved (boot) generation 1, 51,136 B, in 1,071 ms, compacted (2,510 rows, 60,312 B); then `ready: boot 181 ..., the next seq 741 ...; 0 files trimmed`. `H`'s HEAD, since and rows and `Hm` statsref's to the line (MARK 740, the card's the same, ACKS 519); `H`'s RAM and last save the boot's own figures (the records 0, MARK 2 rows; boot, 51,136 B, generation 1); the Listening stats row "289 h 56 min since 31 Oct"; nothing played; the settle; no BAD line, no person. 50 checks PASS, 1 LOOK (the paging's lines: #62) |
| 3.7 The files compared | The spare in the PC's reader: `stats.bin` and its base copied | Equal to statsref's prediction field for field (as S5a's boot 1); the other `/.player` files byte for byte 3.4's copies but for `device.txt`'s firmware line and the trimmed files | Taken after 3.8 (one card swap fewer; 3.8 wrote no stats file): `stats.bin` and `stats-00000001.bin` byte for byte statsref's (SHA-256); both log files unchanged; no other stats or log file; `device.txt` (its firmware line) the one other change, `queue.txt` unchanged; `/.mstream` untouched. 10 checks PASS |
| 3.8 A second boot | The spare in, boot, `z`, `H` | Nothing folded; `[listen] saved (boot): nothing new`, so no compaction (a compaction is a save's step) | Run right after 3.6: generation 1 loaded (121 ms); 0 slots folded; "saved (boot): nothing new since the card's stats.bin (nothing written; 0 ms)"; 0 trimmed; the next seq 741 (boot 182); `H`'s HEAD, rows and since and `Hm`'s MARK rows boot 1's to the line (its own gap 0); the queue and every setting boot 1's; the settle. 36 checks PASS |

A full history's first start (13,106 slots) is not repeated here: S1
measured it with a seeded log (section 2). If wanted on this build,
the owner's word: `tools/statsref.py seed --slots 13090 --replace` on
the spare at 3.1. That is visit A's count (5.14; without its base or
its new root), 16 slots under the log's cap, so 3.3's three plays on
0.14.0 don't rotate it, and neither do the later plays in these checks
(seven in all). `--full` fills both files to the cap, and 0.14.0's
rotation at its first play would delete one file's slots uncounted.
Then the first boot's `[listen]` lines and a `qp` sent during its scan
are read against S1's figures.

## 4. The stats pages, a quick look (not run: section 8)

| Step | Do | Must show | Result |
|---|---|---|---|
| 4.1 The entry row | `ui1`, lit (after its wake), `uiS`, `X` | The Listening stats row: `H`'s lifetime ("N h M min since D Mon"), or the time alone; the fixed bars | |
| 4.2 The hub | `D`; `uiSh` lit; `uiS` after the landing; `X`; `D` | The push's ms "(its first frame drawn)" (3b-2: 46.9 ms idle); the ask's hold about 1.6 s (3b-2: 1,602 ms; 3b-4: 1,595-1,640 ms); the six rows landed, Last 7 days' line `Hd`'s; PSRAM's free and largest the same in both `D` lines (3b-4) | |
| 4.3 The shelves and Week | `uiS0`-`uiS4` and `uiSw`, `X` each; Most played's windows the counting days offer (`uiS00`, `uiS01`, `uiS02`) | Each shelf's rows `Hl<shelf>`'s, and statsref's over 3.7's copies (the comparison S3b skipped); Week's figures `Hd`'s; the shots as built (beside 3b-1's) | |
| 4.4 A play, then a visit | `ui0`; a track past 30 s (`qp<n>`), then `n`; `ui1`, `uiSh` | The play's `[plays] seq` line; released at Now Playing, asked on the return (3b-4); the new ask counts the play (Last 7 days' plays one more; Recently played's first row that track, with the clock believed) | |

Then, if the owner wants it and only with the owner's word, the same
look on the **music card** after section 6, with nothing played: the
candidate's boot, `z`, `H`, then 4.1-4.3 without `D`'s comparison and
without statsref's (3.7's copies are the spare's); the shots kept off
the repository.

## 5. Going back to 0.14.0, and returning (not run: section 8)

| Step | Do | Must show | Result |
|---|---|---|---|
| 5.1 Copies | The spare in the PC's reader: copies, `stats.bin` and its base by SHA-256 | | |
| 5.2 0.14.0 again | 3.2's image, at 0x0 with no erase; the spare in, boot, `z`, `s`; two tracks past 30 s by `qp<n>` | `[nvs] schema 6`; the settings as in 3.3; two `[plays] seq` lines, the seq over 4.4's (4.4's play stored its seq in NVS before its slot, and 0.14.0 takes NVS's and the log's: METADATA 2.20.4) | |
| 5.3 A made-up sync (optional, the owner's word) | In the PC's reader: a new root by `tools/synthtransfer.py` whose ACKS (`--acks`) covers 5.2's plays; predicted by METADATA's "Trimming by ACKS"; then 0.14.0 booted again, `z` | 0.14.0's trim deletes the acknowledged files, 5.2's plays uncounted; any library update the new root brings | |
| 5.4 The stats left alone | The spare in the PC's reader: `stats.bin` and its base | Byte for byte 5.1's; copies taken and `statsref.py boot` run for 5.6 | |
| 5.5 The candidate again | Section 1's image, at 0x0 with no erase | "Hash of data verified" | |
| 5.6 Its boot | The spare in, boot, `z`, `H`, `Hm`, then `ui1`, `uiSh`, `uiS` | `[nvs] schema 6`, the settings as before; with 5.3: nothing of 4.4's or 5.2's plays folded (4.4's were counted live only: no save came before the card left the board), HEAD's gap their slots, none counted twice; without it: 4.4's and 5.2's plays folded; each as statsref predicts; the hub's Last 7 days and Recently played as `H` and `Hd` say | |

## 6. The board at the end

- The candidate's image written and verified; the settings partition
  as these runs left it, moved forward only (3.0's copy never written
  back).
- The spare's state recorded (its stats are test data: 9.1); the owner
  puts the music card back. The music card may boot the candidate
  (main's code, as since S5a), never 0.14.0.
- The release notes' "Tested on" bullet gains sections 3-5 as run
  (dates, what each showed), and "Not checked" loses what ran.

As run (2026-11-25): the candidate's image written and verified (3.5)
and the board parked on it after 3.8; the settings partition moved
forward only (boot 182 of its epoch, the next seq 741; 3.0's copy
never written back); the spare left with 0.15.0's stats of its test
history (6,537 made-up slots and the board's own 145) and the queue
at track 4; the owner puts the music card back. The release notes' bullets brought up to date from section 3.

## 7. Optional: the owner's hands or word

None of these holds the release by the plan's text; each is the
owner's to run or leave:

- 3b-8: a finger on Most played's tray on the true panel (`u`'s
  `[input]` raw x at each split), and the stats pages' haptics;
- 3b-9: the card pulled and put back with a shelf, Week and the hub
  open; a first visit made with the card out; 3b-4's empty card (a play
  counted on the empty hub: a card with nothing played);
- the clock set while the hub, Week or Most played shows (the MP3
  Player tab's `@time`), a UTC midnight on a page (counting days 6 and
  29), and the no-clock session order (9.7's owed runs);
- the nine other languages on the panel (`uiL<tag>` writes NVS:
  English picked again at the end, a forward write);
- #62's ask with 8 tries, the headphones connected to a phone (a tap
  on Output's Connect; each `[bt] page:` line), and a play waiting for
  the headphones (`PlayGate`, its 65 s backstop);
- 3b-10: a stats visit while Bluetooth streams (G5: `gs`'s lowest
  internal RAM at or over 43,008 B).

## 8. Not run for this release

- Sections 4 and 5 (the stats pages' quick look, and going back to
  0.14.0 and returning): the owner's "Upgrade check only" (the stats
  pages ran in S3b, section 2; going back is from the code alone);
- Section 7's, where the owner leaves it;
- a full history's first start on this build, and a boot save with a
  track playing through it (S1 measured the first with a seeded log
  and a held start, section 2);
- a stats save on Bluetooth, and S1's loop work meeting a headphones'
  link set-up (5.14's "Not covered");
- a card past 8 MARK rows; a remount with a track starting in its
  re-read;
- a fresh card's or a Rescan's tag-scan wait for the stats pages
  (minutes, ESTIMATED), and the battery floor (host-tested only);
- a queue of over one cluster (about 400 tracks) on a nearly full card,
  a second boot of #63's hint, and cards other than the 32 GB one;
- the headphones opening their own link after the burst's back-off
  (#64's set-up wait: host-tested only);
- Now Playing's glance (Touch when off: Track) over the empty state
  (#65: host harness only);
- the idle power-off on battery, a long listening session;
- the `core2-dio` image (built by section 1, not booted).
