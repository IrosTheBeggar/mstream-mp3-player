# Listening stats

Listening stats count the plays on the card and show them in the Library: a hub with the time listened, the last 7 days, and shelves of tracks to play (Most played, New finds, Old favourites, Not played yet, Recently played), with a first row on Now Playing's navigation sheet. The player works them out from its own play log. It needs no network, writes nothing more to the card for a play than the play's own slot, and a tap never reads the card.

## 1. Status

**Status (2026-11-10):** nothing is built. This document is the build plan, read and written at firmware bc29b54 (main; v0.14.0 released). PR #58 moves NEXT_RELEASE to 0.15.0 (`tools/version.py:54`), so the stats ship in 0.15.0 at the earliest.

**Designed** on the Claude Design canvas, https://claude.ai/artifact/MhGiBt7yLecJZEErFQgSTq. Its final frames are Entry, Main, MostPlayed, NewFinds, NoClock, EarlyDays, NoPlays, NowPlayingSheet and Week. AlbumPlays was dropped by the owner (decision O3). The A- (Glance), B- (Recap) and C- frames are the directions not taken (14). The final spec lived in a scratch folder that Windows cleans up after about a week, so its essentials are inlined here (sections 2, 4, 5, 7, 8). Where this plan departs from it, 1.1 and 3.2 say so. The nine final frames are in [docs/img/listening-stats](img/listening-stats/README.md), captured from the canvas (C1).

**Picked by the owner, 2026-11-10:** O1-O4 and O6 of 3.1. **O5 is open.**

**Assumed:** everything else. 3.2 lists each default with its alternative, and the owner can overrule any of them.

Line numbers are at these commits:
- firmware bc29b54
- phone app (`mstream_music`) d17c3fa5
- terminal player origin/main 410444f, and `claude/card-transfer` 3edd4a4 (PR #56)
- `docs/METADATA.md` as this branch holds it: main 241642d's, with C2's edits and S0's review (main since bc29b54 changed only `tools/version.py` and docs, so the firmware's lines hold at 241642d too)

Paths with no repository named are the firmware's.

Labels:
- **READ:** from the code or docs.
- **MEASURED:** on the device, quoted from the docs.
- **MEASURED-HOST:** a script or test run on a PC.
- **ESTIMATED:** the docs' own word, or a scaling of their figures.
- **MODELLED:** arithmetic, not run.
- **GUESS:** judgement.

### 1.1 Where the spec and the design run's maps disagree with the code

The code wins. Each item is READ at bc29b54.

- **The spec's "the card worker saves stats before `finishScan()` trims" has no window to run in.** `finishScan()` runs on the loop, called from the scan's last piece (`lib/core/PlayLog.cpp:315`), and in one call it stores the boot count, trims and sets Ready (`:318-347`). The save runs there, on the loop (5.5).
- **A journal state between the scan and Ready would drop plays.** `append()` returns Off for any state but Ready (`:523`), and the Outbox then drops that play for good (`:724-729`). `Outbox::flush()` would spin to its deadline (`:738-744`), since `scan()` returns true for any state but Scanning (`:246`). So no new state.
- **The scan reads nothing on a new epoch with no ACKS** (`:162-164`). After an NVS erase the old epoch's slots, and other players' (O2), would never be folded. With stats on, the scan always reads (5.6).
- **The scan reads the newest file first:** plays.jnl, then plays.1.jnl, then plays.2-9 (`:81-86`, `:249-256`). Fold rules that depend on order take it from the slot's own fields, never from the scan's order (5.6).
- **The spec's three files renamed one by one** (stats.bin, stats-trks.bin, stats-trks.d.bin) can disagree after a cut between two renames: rows counted twice or lost. This plan has one commit file that names its base file (5.2, 5.3).
- **"Skip the trim when the save fails"** (spec 8.3.3) can deadlock a full card: no save because it is full, no trim because the save failed. This plan trims a file when the MARK already on the card covers it (5.5).
- **The rotation deletes `plays.1.jnl` mid-session** with no ACKS check (`:411-463`, the delete at `:437`). The spec doesn't mention it. On a card never synced it is the only deletion. This plan saves first when that file holds a play the card's MARK doesn't cover (5.5).
- **A resident table keyed by track id can't name the rankers' hashes.** RCNT and TRKS are keyed by path hash, so the 7- and 30-day tops, New finds and Recently played come out as hashes. This plan keeps no per-track table resident (6.1).
- **The spec's walk cost** ("1.5 s idle, 3 s playing" with Auto DJ off) is the whole join's. A walk for stats is Folders and Hash only: no card reads, no sort. ESTIMATED 0.1-0.2 s of CPU (6.3).
- **The spec's redraw costs are low.** A row redrawn in place is about 11 ms with its push (`src/ui/ListView.h:273-274`), not 3 ms; a header push (320 x 36) about 5 ms, not 1. Section 7.10 uses these.
- **METADATA 3.10 says the play log was "not flashed"** (`docs/METADATA.md:7233`). The 0.14.0 checks ran it on the device: slots written in 14.8-16.6 ms (`docs/RELEASE-0.14.0-CHECKS.md:60`). S1's docs commit corrects it.
- **The terminal's time zone on Windows.** The design run's process map said `src/admin/tz.rs` gives no zone on Windows. Terminal origin/main reads the zone Windows is set to since 5eac90c (2026-10-29; terminal `src/admin/tz.rs:1-10, 32-49`). #56's branch (3edd4a4) doesn't have 5eac90c: there a Windows machine gets no zone (its `src/admin/tz.rs:6-7`). No branch sends it in `@time` yet (10).
- **ScanScheduler's jobs** are at `lib/core/ScanScheduler.h:101-102` (`kJobs` 8), not `:119-120`.
- **The log prefix.** `[stats]` is the audio's line (console `s`). The stats' lines are `[listen]`.

## 2. What the listener gets

**The way in.** Library > Artists gains a first row: seven small bars for the last seven days, "Listening stats", and "7 h 37 min in the last 7 days". It scrolls with the list. A tap opens the hub. From Albums or Folders: a tap on Artists brings its list back, a second scrolls it to the top, where the row is. Now Playing's navigation sheet (a tap on the cover or the title) gains a first row, "Go to Most played" with "9 plays · last Sun · 1 skip" under it. It opens the shelf at that track, tinted.

**The hub**, Listening stats. Under its title, the time listened since counting began ("74 h 10 min since 1 Sep"). Six rows follow, each with an icon, a title and a line:
- Last 7 days: "7 h 37 min · 118 plays"
- Most played: the top three titles
- New finds: "41 tracks first heard in 30 days"
- Old favourites: "38 tracks, played often, not lately"
- Not played yet: "612 albums since 1 Sep"
- Recently played: "Slow Lantern · 25 min ago"

**Last 7 days** is a page of its own: the time listened and the plays, seven bars with today in violet, the day listened most ("Saturday was your big day: 2 h 1 min"), and how often Auto DJ's picks became plays.

**The shelves** are Library lists that play like any list.
- Most played opens on All time, with a tray for 7 days and 30 days (O1). Each row: its rank, the title, "Artist · 12 plays".
- New finds: tracks first heard in the last 30 days, and who found them ("found by Auto DJ").
- Old favourites: tracks with 5 plays or more and none in 30 days.
- Not played yet: albums with no play, A-Z, with covers.
- Recently played: tracks newest first, under day headings.

A tap on a track opens its bar (Play, Play next, + Queue); a hold opens its sheet, as on an album page. The top bar plays the shelf.

**Early and odd states.**
- Nothing played yet: the hub is an empty state with Shuffle all.
- The clock never set: the totals and Most played (All time) work; the rest that needs dates says so, with the fix ("Plug in to the MP3 Player tab").
- The first month: New finds, Old favourites and Not played yet say when they open ("Opens 6 Dec, after a month of plays"), and Most played's tray grows with the days counted.
- No card: the row goes, and an open hub says plays can't be counted.

**What doesn't change.**
- A, B and C stay prev / vol-, play-pause / output, next / vol+.
- Album and artist pages stay as they are (O3).
- The play log's slots, files and ACKS rules stay as METADATA 2.20 has them, but for these device-side changes (5.5, 5.6): the boot's scan reads on a new epoch; a log file is trimmed only once the stats hold its plays; a rotation may save the stats before its delete; the boot's seq rule also takes the stats' and ACKS's highest seq for this device and epoch.

**The same as mStream:** a play is `playedMs >= min(30 s, half the track)` (mStream decides by this rule: `docs/METADATA.md` 2.20.6, "The post"); its id stays the slot's (2.20.4).

**Where it differs:**

| Topic | mStream | The player |
|---|---|---|
| Whose plays | The account's, from every client | The plays on this card, from every player that wrote them (O2) |
| Which plays | Those posted | The slots on the card. Plays under 1 s were never logged (`lib/core/PlayLog.h:377`) |
| Days | Its own time zone | UTC days until the terminal's zone arrives (O4) |
| History | Retention, 24 months by default | Since counting began on this card |
| A renamed or converted file | The server's track | A new path hash: its old plays stay in the totals and leave the shelves (6.6) |

## 3. Decisions

### 3.1 The owner's (2026-11-10)

| Id | Decision | Status | Alternative |
|---|---|---|---|
| O1 | Most played opens on All time. Its tray still offers 7 days and 30 days | **owner's pick** | Open on 30 days (the spec) |
| O2 | Every player's plays on this card count: no deviceId filter | **owner's pick** | This player's only |
| O3 | Album pages stay as they are: no plays line on album track rows. Per-track plays show in the shelves and the Now Playing sheet's first row only | **owner's pick** | The plays line (frame AlbumPlays) |
| O4 | Build on UTC days now. The terminal's time zone in `@time` comes later, applied at display only | **owner's pick** | Wait for the zone |
| O5 | When Not played yet opens | **OPEN.** Assumed: after 30 days of counting; sooner once mStream's counts reach the card (later, not v1: 5.11, 14). With the clock unset (counting days unknown): disabled, as New finds (4.4) | From the first day |
| O6 | The console's silent test mode `z` keeps logging plays, and they count | **owner's pick** | Not folded |

O5 is one constant in one function, `statsview::notPlayedOpen(countingDays, dated)` (7.11), so the answer changes one line and its test. Its no-clock case goes to the owner with it.

### 3.2 Smaller defaults made by this plan

All are the plan's, and the owner can overrule each.

| Area | Default | Alternative |
|---|---|---|
| Data | One commit file, `stats.bin`, that names a base file of per-track rows (5.2, 5.3) | The spec's three files |
| Data | The save runs synchronously on the loop at the scan's end, before the trim (5.5) | A worker save, with a journal state between scan and Ready (1.1 says why not) |
| Data | A file is trimmed only when the card's MARK covers every play in it. A failed save still trims what the old MARK covers | Skip the trim when the save fails |
| Data | A rotation over a `plays.1.jnl` holding plays the card's MARK doesn't cover saves first, in the orderly end's flush too when 1 s of its budget is left. A failed save there doesn't hold the rotation back | Let those plays go uncounted |
| Data | The boot's seq rule takes the highest of NVS's seq, the card log's, MARK's and ACKS's for this device and epoch (5.6). 2.20.4 allows it: a device MAY reserve seqs | MARK only; or today's rule |
| Data | Drop notes are not folded. No "lost" figure in v1 | Fold their counts into a lost figure |
| Data | A slot with an unknown outcome reads as stopped; source 0 and above 3 count as "you" | |
| Data | HOUR keeps its newest 840 rows and DAYS its newest 400, never pruned by date; RCNT keeps 32 days counted back from the earlier of today and the newest dated day folded, at most 4,096 rows, and nothing by date with the clock unset (5.2) | The spec's 13 months of HOURS; pruning by date from today |
| Data | No listened time per track | 4 B more a row |
| Data | `lastOrd` is a fold round (one a boot fold, one a session), not a counter a play | A counter a play: 4 B more a row |
| Data | firstAt is a track's earliest dated play, whatever the order of its plays; a track with no dated play is never a New find (5.2) | The first play's time, dated or not |
| Data | MARK rows are kept for good: one a (deviceId, epoch) ever folded on the card (5.2) | ACKS's leave-out rule |
| Data | A boot whose scan couldn't open or read a log file whole: no save this session; the next boot folds again (5.6) | Save what was read |
| Data | A save is refused when the card's `stats.bin` isn't the copy RAM stands on (another player saved meanwhile, or it was deleted on a PC) (5.5) | Save over it |
| Data | A `stats.bin` unread at the boot: no fold, no save, no trim this session; read again (once, no waits) at a remount before the boot's first slot | Fold into a session-only view |
| Data | A corrupt `stats.bin` is renamed `stats.bad` and counting starts again from the log (the spec's rule) | Keep it and stop |
| Data | The base is compacted only at a boot's save while nothing plays, when the delta is over 32 KB or a quarter of the base; past 96 KB at a boot's save at the update step's safe point, else at the next boot's (5.3) | Any save |
| Data | A newer minor of `stats.bin` is read; a save over it drops what it added, so a minor adds only what a save can do without (5.12) | A newer minor holds the saves back |
| Map | The walk runs alone, through code shared with Auto DJ's join, never in the join's pass (6.2) | Ride the join while Auto DJ is on |
| Map | No resident per-track table: ids only for RCNT's hashes and the lists' | The spec's played-track table |
| UI | The hub's Most played line shows the All time top three, the shelf's opening window | The 30-day top three (spec 3.2) |
| UI | Most played's All time sub is "since 1 Sep" (rule 5.4 of the spec: every lifetime figure says its window) | "all time" (frame MostPlayed) |
| UI | A window with no plays keeps its tray and shows "Nothing in this period." under it | Fall back to All time |
| UI | The top bar's Play follows the house's container rule: with shuffle on it starts at random (`src/ui/LibraryPage.cpp:810-815`) | Always from No. 1 (spec 3.3) |
| UI | The NP sheet's row shows only when the target shelf holds the track: the All time top 50, else Recently played's 200 | Open Recently played at its top |
| UI | The NP sheet's row waits for the track's record: no row until it is read | The row without its skips |
| UI | Recently played's lines give relative times within 24 h, then the artist alone; no clock times while days are UTC | "· 21:40" (spec 3.4) |
| UI | Ranks on Most played, New finds and Old favourites; none on Recently played | |
| UI | "(UTC)" only in Week's sub | On every date |
| UI | The tray splits at each gap's middle plus `filtergrid::skew()` (7.9) | The spec's 114 / 220 |
| UI | The artist page's " · N plays" is left out, read as part of O3 | Add it: about 5 KB of sums |
| UI | Stats unread, newer or with no memory: no entry row, as with the log off | A line saying so |
| UI | Totals show once the boot's fold ends; names once the walk ends (4.4) | Totals from the card at once |
| Frames | "No plays yet" (the phone app's `listeningEmptyTitle`) for the frame's "Nothing played yet"; "Listening time" (`listeningTileTime`) for "Listened"; "Recently played" (`autoDjRecentlyPlayed`). No translation owed for them but the short forms in 8.1 | The frames' words, translated |
| Frames | NoClock's open Not played yet row is drawn disabled, with `stats.needs_dates` (O5's no-clock case, 4.4) | Open, as the frame draws it |
| Frames | Toasts follow the code: two lines only when the text is too wide for one Body line (`src/ui/Overlays.cpp:105`). "Needs a month of plays: Opens on 6 Dec" is one line in English | A forced split |
| Frames | The frames are browser renders (`.dc.html`): checked for layout and colour, not pixel for pixel | |
| Console | `H` for the stats (history), `uiS…` for the screens (9.3) | |
| Time zone | `@time`'s field 1, minutes east of UTC, kept in NVS `clock/tz` at schema 7 | In `stats.bin`'s HEAD |
| Release | S1 may ship on its own: counting starts with it. S5a runs before any build carrying S1 boots the owner's music card (9.9) | Hold until S3 |

## 4. The behaviour, precisely

### 4.1 Definitions

| Term | Rule |
|---|---|
| A slot that counts | Kind 1, valid (MPPL, major 1, its CRC), used (epoch not 0, seq at least 1, deviceId not all zeros: `docs/METADATA.md` 2.20.6 item 2), once per (deviceId, epoch, seq) |
| The line | `min(30,000 ms, durationMs / 2)`; 30,000 ms when durationMs is 0. The half is not rounded, as mStream's `decideCounted` takes it (playedMs ≥ 30,000, or playedMs ≥ durationMs × 0.5): in integers, a play is 2 × playedMs ≥ min(60,000, durationMs) |
| A play | A slot that counts with playedMs at or over the line, whatever its outcome |
| A skip | Outcome 2 (skipped) with playedMs under the line. Completed or stopped under the line is neither. Outcome 0 or above 3 reads as stopped (2.20.3) |
| Listened time | The sum of playedMs over every slot that counts: plays, skips and the rest |
| Source | 3 Auto DJ, 2 shuffle, anything else "you" (1 manual, 0 not said, above 3) |
| Dated | startedAt from 2026-01-01 to 2099-12-31 UTC (`wallclock::believable()`, `lib/core/WallClock.h:27-31`). Else undated: 0 while the clock was unset, or another device's clock |
| Its hour, its day | Of startedAt, UTC. hour = (startedAt − kMinMs) / 3,600,000; day = (startedAt − kMinMs) / 86,400,000; day 0 is 2026-01-01 |
| A time in a row | Seconds since 2026-01-01T00:00:00Z, plus 1. 0 means none (lastAt) or only undated plays (firstAt) |
| Today | The board's clock now, UTC. None while it is unset (`app/BoardClock`) |
| Counting since (sinceDay) | Set once, at a fresh `stats.bin`'s first save: the day of the earliest dated play folded, but not after today when today is known; else today; else the root's commitTime's day (`lib/core/CardManifest.h:68`, Unix seconds UTC: `docs/METADATA.md:843`), flagged; else unset until a save that knows a date |
| Counting days | today − sinceDay + 1, at least 1 |
| Windows | 7 days: today and the 6 days before. 30 days: today and the 29 before. All time |
| Times | `format.hours_minutes`, unpadded minutes: "7 h 37 min". Under an hour, `format.minutes`: "45 min" |

### 4.2 What each number is

| Where | Number | From |
|---|---|---|
| Entry line, hub row 1, Week's readouts | Time listened and plays in the 7-day window | HOUR, summed over the window's UTC days |
| Entry and hub bars (24 px), Week bars (44 px) | Each day's time listened; the top day is full height | HOUR by UTC day. No bar for a day before sinceDay, or with nothing |
| Hub header sub | Lifetime time listened, since sinceDay | HEAD |
| Hub, Most played's line | The All time top three's titles, cut with "…" like any names | The All time list |
| Hub, New finds' line | Tracks whose earliest dated play is in the 30-day window | TRKS |
| Hub, Old favourites' line | Tracks with 5 plays or more and none in the 30-day window | TRKS |
| Hub, Not played yet's line | Albums with no track played, since sinceDay | The album bits (6.1) |
| Hub, Recently played's line | The newest track's title · how long ago | The Recently played list |
| A shelf row | Its count in the shelf's window | RCNT (7 and 30 days) or TRKS (All time) |
| Week line 1 | The top day of the 7 by time listened; a tie goes to the later day | HOUR |
| Week line 2 | Auto DJ's plays / (its plays + skips) over the 7 days, rounded to a percent; shown only when the sum is 5 or more | DAYS |
| NP sheet detail | The track's plays, its last day, its skips (omitted at 0) | Its TRKS row (base ⊕ delta) |

### 4.3 The shelves

| Shelf | Rule | Order, ties | Cap |
|---|---|---|---|
| Most played | Plays in the window, at least 1 | Plays, most first; then the latest play (RCNT's day, or lastAt), newest first; then hash | 50 |
| New finds | firstAt dated and inside the 30-day window | Plays; then firstAt, newest first; then hash | 50 |
| Old favourites | Plays at least 5; lastAt dated and before the 30-day window | Plays; then lastAt, newest first; then hash | 200 |
| Not played yet | Albums of `albumsAZ()` with no track played | A-Z (`lib/core/LibraryIndex.h:399-400`) | All |
| Recently played | Plays at least 1 | lastAt, newest first; then rows with no dated play, by lastOrd, newest first; then hash | 200 |

- "Then hash": path hashes as unsigned 64-bit integers, the smaller first.
- A hash that names no track of this library (an orphan: 6.6) is left out of a shelf. The rankers keep 16 rows past each cap, so a few orphans don't shorten it.
- Recently played's headings are the UTC day of lastAt: "Today", "Yesterday", then "Sun 8 Nov" (`format.weekday_date`, 8.2). With no clock: "This session" (tracks played since the boot), then "Earlier".
- A track only skipped has no row on any shelf. Its album still counts as not played.

### 4.4 States

| State | Trigger | Entry row | Hub | Shelves, Week |
|---|---|---|---|---|
| Off | The log off (no card records, no PSRAM, NVS refusing: `src/app/PlayHistory.cpp:99-123`); `stats.bin` or its base unread after the retries (5.4); a newer major; no memory for the fold | Absent | Not reachable | — |
| No card | `cardAway()` or the card held (`src/app/PlayHistory.cpp:91-93`, `:185`) | Absent from the next `enter()` | The one line "No card: plays can't be counted" | The same line |
| Folding | Stats read, the boot's fold under way | "Counting plays…", no bars | No sub; every line "Counting plays…" | "Counting plays…" |
| Naming | The fold done; the lists or the walk not yet | Line and bars | Header, Last 7 days; the other lines "Counting plays…" | Week works; the shelves "Counting plays…" |
| Ready | | Line and bars | Everything | Everything |
| Empty | No counted play (HEAD's plays 0, none this session) | "Plays show up as you listen", no bars | The empty state (7.5) | — |
| No dates | The board's clock unset | "6 h 10 min since 5 Nov", no bars | Row 1 a status row (7.4); New finds and Old favourites disabled; Not played yet disabled with `stats.needs_dates` (counting days unknown; assumed with O5); Most played without its tray | Week not reachable; Recently played by session |
| First month | Counting days under 30 | As Ready | New finds, Old favourites and Not played yet (O5) disabled: "Opens 6 Dec, after a month of plays" | The tray by age (7.6) |
| Updating | Behind the update's fence (`Library::index()` null) | Absent: `topBar()` is false with no index (`src/ui/LibraryPage.cpp:585`) | Not reachable | Not reachable. The fence returns the Library tab to its root (`src/ui/Ui.cpp:748-755`) and Live does again (`:737`); the snapshots are dropped there. Now Playing's jump refuses with no index (`src/ui/NowPlayingPage.cpp:1140-1143`) |
| Synthetic library | `uil<n>` (`src/ui/LibraryPage.cpp:74`) | Absent | — | — |

- Whether the entry row is there is decided at `enter()`: adding or removing item 0 while Artists shows would move every artist by 42 px.
- A remount doesn't reset the Library tab (`src/main.cpp:3237-3284`); only a rebuild (`:1071`) and an update's Live (`:3467`) do, through `Ui::libraryChanged()` (`src/ui/Ui.cpp:731-746`). An open hub, shelf or Week follows the card state itself (the No card line), at its next `update()`.
- The status band (a scan, tags, an update) shows on the hub and the shelves as on any Library list. It never shows on Week, a page of its own.

### 4.5 Toasts

| Toast | Kind | When |
|---|---|---|
| "Needs dates: Plug in to the MP3 Player tab" | `Ui::warn` (amber, 1.8 s) | A tap on New finds or Old favourites, disabled for want of dates |
| "Needs a month of plays: Opens on 6 Dec" | `Ui::warn` | A tap on a row disabled in the first month |
| The existing `act()` toasts, with Undo and View | toast | Play next, + Queue on a shelf |
| `toast.shuffling` | toast | Shuffle all, the empty state's |
| `toast.no_memory` | toast | A shelf's snapshot can't be carved |

A disabled row's tap also gives the inert double buzz (`Input::missBuzz()`, `src/ui/Input.h:123`).

### 4.6 Persistence

| What | Where | Life |
|---|---|---|
| HEAD, MARK, HOUR, DAYS, RCNT, the TRKS delta | `/.player/stats.bin`, and RAM | Saved at a boot with new plays, before a rotation over plays not yet saved (in the end's flush too, with 1 s of its budget left), and by `Hs`; RAM in between |
| TRKS rows as of the last compaction | `/.player/stats-<gen>.bin` | Until the next compaction |
| The lists, the ids, the album bits | RAM | Remade after each boot and each update's Live |
| A shelf's window | RAM, a shelf at a time | The session. Most played starts on All time |
| NVS | Nothing (S6 adds `clock/tz`) | |

## 5. The data

### 5.1 The pipeline, and where the stats join it

As built (READ):
1. PlaybackController's word goes to the Tracker, a ring of 32 plays in PSRAM (`lib/core/PlayLog.h:362-427`). `tick()` counts playedMs and the pauses (`lib/core/PlayLog.cpp:593-618`). Plays under 1 s are dropped; a 33rd waiting play is lost (`:567-591`).
2. `stepPlays` (`src/main.cpp:3499-3518`) calls `PlayHistory::loop` every pass. It waits for the WriteGate (`lib/core/PlayLog.cpp:767-791`) and asks nothing while the card is held or away (`src/app/PlayHistory.cpp:181-192`).
3. Before Ready: a piece of the boot's scan, 128 slots (`src/app/PlayHistory.h:91`). After: one `Outbox::writeOne` a pass (`src/app/PlayHistory.cpp:214-217`): the f_stat, the slot, `Journal::append` (seq to NVS first, encode, write, f_sync: `lib/core/PlayLog.cpp:522-544`).
4. Written: `done()` counts it, calls `env_.tried`, then `pop()` (`:633-647`).

The stats add, all on the loop:
- **The live fold**, at `Outbox::done()` for a Written play only (5.6).
- **The boot's fold**, inside `Journal::scan()` for each valid slot (5.6).
- **The save**, inside `finishScan()` before the trim, and in `rotate()` before `plays.1.jnl` leaves the log's names (5.5).
- **The ranking and the per-track read**, on the card worker, after Ready (6.8).
- **The walk** that names the hashes, on the loop in slices (6.2).

### 5.2 `/.player/stats.bin`

A card container (`docs/METADATA.md` 2.4; `lib/core/CardContainer.h`):
- magic `MPST`, unused by any other file (READ: no other fourcc in `lib/core` or `src` spells it);
- major 1, minor 0;
- headerBytes 40, no type header;
- generation: the save count, from 1;
- cardId 0. The stats are never keyed to MSMF's cardId: a new set-up of `/.mstream` changes it while `/.player` stays (2.11's rows, `docs/METADATA.md:1789-1790`). The file is the card's because it is on the card.

Its sections, in this order. Every one is REQUIRED (a reader that doesn't know one leaves the file alone: 5.4).

| Type | Kind | Stride | Holds |
|---|---|---|---|
| `HEAD` | blob, 128 B | — | The totals and the file's state |
| `MARK` | array | 24 | The highest seq folded, per (deviceId, epoch) |
| `HOUR` | array | 8 | Each UTC hour with a slot that counts, the newest 840 |
| `DAYS` | array | 16 | Each UTC day with one, the newest 400 |
| `RCNT` | array | 12 | Plays per (UTC day, track), the last 32 days, at most 4,096 rows |
| `TRKD` | array | 24 | The TRKS delta: rows changed since the base |

All little-endian.

**Retention**, at each save:
- HOUR and DAYS are capped by rows, the oldest going first: HOUR at most 840 rows (35 days of every hour), DAYS at most 400. Neither is ever pruned by date, so no clock (unset, wrong or foreign) can prune a row of them. DAYS is primary data the log can't rebuild.
- RCNT keeps 32 days, at most 4,096 rows. Its days count back from the earlier of today and the newest dated day folded. With the clock unset or not believed (5.9), nothing is pruned by date at that save. A row newer than the reference is kept.
- So one foreign slot dated 2099 (believable: 4.1) moves nothing: sinceDay is never after today (4.1). A clock set a year ahead for one save, then corrected, costs RCNT's windows up to a month, never a lifetime figure, an hour or a day.
- Two cases still set a future sinceDay, for good: a board clock already fast at the first save, and, with the clock unset at that save, a foreign slot dated ahead (nothing then to cap it by). Then the first month's states last until the clock catches up.

**HEAD** (128 B):

| Off | Size | Field |
|---|---|---|
| 0 | 8 | plays |
| 8 | 8 | skips |
| 16 | 8 | listenedMs |
| 24 | 8 | slots: slots that count, folded |
| 32 | 48 | per source, "you", shuffle, Auto DJ: plays u32, skips u32, listenedMs u64 (16 B each) |
| 80 | 4 | undated: plays that weren't dated |
| 84 | 2 | sinceDay (0xFFFF: unset) |
| 86 | 2 | flags: bit 0 sinceDay came from the sync's commitTime; bit 1 a base was lost (5.4); the rest 0 |
| 88 | 4 | round: the last fold round given out (5.6) |
| 92 | 4 | baseGen: the base file's number (0: none) |
| 96 | 4 | baseRows |
| 100 | 4 | baseCrc: the base file's header CRC |
| 104 | 4 | twice: slots the boot's folds met a second time (a diagnostic) |
| 108 | 4 | gap: seqs ACKS covers above MARK that the log no longer holds, added up at each boot that finds some (plays a firmware without stats trimmed; an estimate). That boot's MARK then covers ACKS's seq, so the next boot doesn't count them again (5.6) |
| 112 | 16 | reserved, 0 |

**MARK** row: deviceId 6 B at 0; 2 B reserved; epoch u64 at 8; seq u32 at 16 (the highest folded, or counted in HEAD's gap); 4 B reserved. Strictly increasing by (deviceId's bytes, epoch). MARK rows are never dropped: one row (24 B) for each (deviceId, epoch) ever folded on this card. ACKS's leave-out rule (`docs/METADATA.md:1071-1073`) does not apply to MARK: the scan can miss slots that are on the card (an open the card fails reads as a missing file, a failed read ends a file: `lib/core/PlayLog.cpp:256-261, 302-306`), and a row dropped then would let the next boot fold those slots again. Never capped at ACKS's 16 rows.

**HOUR** row: hour u24 at 0 (UTC hours since 2026-01-01; 16.7 M hours); plays u8 at 3 (saturating); listenedMs u32 at 4 (a slot's whole time goes to the hour it started in). Strictly increasing by hour.

**DAYS** row: day u16 at 0; plays u16 at 2; skips u16 at 4; Auto DJ's plays u8 at 6 and skips u8 at 7; listenedMs u32 at 8; shuffle's plays u8 at 12 and skips u8 at 13; 2 B reserved. "You" is the rest. u8 counts saturate. Strictly increasing by day. This is the spec's DSRC with the day's totals added.

**RCNT** row: day u16 at 0; plays u16 at 2; path hash u64 at 4. Only plays. Strictly increasing by (day, hash). Past 4,096 rows whole days go, oldest first, until 4,096 or fewer rows are left (a day of more than 4,096 tracks goes whole too), and the 30-day window then holds fewer days (its tray cell says how many: 7.6).

**TRKS row** (TRKD's, and the base's TRKS):

| Off | Size | Field |
|---|---|---|
| 0 | 8 | pathHash (`docs/METADATA.md` 2.3.3; the slot's) |
| 8 | 2 | plays, saturating |
| 10 | 2 | skips, saturating |
| 12 | 4 | firstAt: the earliest dated counted play's time (4.1); 0 counted plays, none dated; 0xFFFFFFFF none (skips only) |
| 16 | 4 | lastAt: the latest dated counted play's time; 0 none |
| 20 | 3 | lastOrd: the fold round of the latest counted play |
| 23 | 1 | flags: bits 0-1 the source of the play firstAt keeps (0 you, 1 shuffle, 2 Auto DJ); bits 2-7 0 |

Strictly increasing by hash. A row exists for a track with a play or a skip.

**Merging two rows of one hash**, `a ⊕ b` with `a` the older (the base before the delta; a row before a slot). Every rule is order-free, so a row saved from live folds (a rotation's save, `Hs`) and the same row folded at a boot (newest file first) are the same but for lastOrd, a round (5.6: a live fold's is HEAD.round + 2, a boot's + 1), which the proof compares by order:
- plays, skips: sums, saturating;
- firstAt: the earlier dated time. 0 (undated) only when neither side has a dated one. 0xFFFFFFFF (none) loses to both. The source flag goes with the time kept; on equal times, two undated included, the smaller source code wins;
- lastAt, lastOrd: the larger.

A slot is folded into its row as the row of that one slot:
- a play: plays 1, firstAt and lastAt its time (firstAt 0 when undated, lastAt 0), lastOrd its round, its source;
- a skip: skips 1, firstAt 0xFFFFFFFF, lastAt 0, lastOrd 0.

### 5.3 The base file, `/.player/stats-<gen>.bin`

- `<gen>` is HEAD's baseGen in 8 lowercase hex digits.
- The same frame (magic `MPST`, major 1), generation baseGen, one REQUIRED section `TRKS` (array, stride 24), rows strictly increasing by hash.
- It is written under a name `stats.bin` doesn't name; a stale file there (left by a `stats.bin` deleted or corrupt, or by a base lost: 5.4, 5.7, which start baseGen again at 0) is replaced, as the reference does (created with `FA_CREATE_ALWAYS`). `stats.bin` names it, so the rename of `stats.bin` is the one commit point.
- So a compaction never writes the name the card's `stats.bin` names. After a base lost, baseGen starts again at 0, and its next, 1, may be the lost one's name: a cut after the create would leave a base shorter than its header under the name `stats.bin` names, Unread at every boot (5.4), the stats off for good. After a held tmp that compacted is promoted (5.5), the name is the held base's, and a failed save would lose it. That compaction waits for the next boot's save, whose `stats.bin` names another base or none. (As built, S1 C7: `tools/statsref.py` first compacted over a lost base 1's name; it now skips as the firmware does, and the `base-other` fixture is two boots, the second compacting over that file.)
- **Compaction** writes `base ⊕ delta`, a merge of two sorted streams, to `stats-<gen+1>.bin`. Then `stats.bin` names it with an empty TRKD. Then the old base is removed.
- When: at a boot's save, while nothing plays, when the delta is over 32 KB or over a quarter of the base (with no base, any delta: the first boot's save writes one). Forced past 96 KB, but only at a safe point: nothing playing, or the heard track with 20 s or more left and no seek in the last 2 s (the update step's own safe point, `src/main.cpp:3559-3563`). Otherwise at the next boot's save. The boot's save runs inside `finishScan()`, one synchronous call after a WriteGate checked once at the pass's start (`lib/core/PlayLog.cpp:767-791`: TrackEnd only under 5 s left), so the gate alone doesn't keep a compaction of 0.8-1.6 s out of the gapless window.
- A cut anywhere leaves `stats.bin` naming a whole base. A file it doesn't name is `stats-<gen−1>` or `stats-<gen+1>`, and goes at the next boot after a Loaded read (5.4). Nothing lists the folder.

### 5.4 Reading at the boot

In `PlayHistory::begin()`, before `journal_->begin()` (`src/app/PlayHistory.cpp:119-120`):

1. `tagstore::settle(fs, kStatsNames, &check)`, names `{"/.player/stats.bin", "/.player/stats.tmp", "/.player/stats"}` (twins `stats.xl1`-`.xl9`). `check` is `StatsCheck`, a real TmpCheck (`lib/core/TagStore.h:314-323`): the frame and every section's CRC. A null check takes a lone tmp for torn and removes it (`lib/core/TagStore.cpp:454, 463`): after a cut between the remove of `stats.bin` and the rename, that tmp is the only copy. `StatsCheck` answers:
   - **Whole** for a frame whose checks hold, and also for one whose open says Major or UnknownRequired: that tmp is promoted, then read below as Newer and left alone (5.12). `IdxCheck` counts an Outdated `library.idx` as whole the same way (`lib/core/LibraryUpdate.cpp:39-61`). Answered Torn, a lone tmp of a newer stats firmware (a downgrade, or an older second player: O2) would be its whole history deleted.
   - **Unknown** for a read the card fails, records that are damaged (`File::damaged()`: never readable, but the tmp may be the only copy), or no memory for the check: the tmp is kept.
   - **Torn** only for a frame that fails its own checks.

   Settle `Unread` or `Kept`: the verdict is Unread.
2. Open `stats.bin` with `openRead(path, &cardFailed)` (`lib/core/TagStore.h:206-215`). A null with the card failed: Unread. A null otherwise is Absent only when `look()` says Gone, the card read and no `stats.bin` there (`:233-237`). Unknown (the file system's records of the way to it damaged) is Unread.
3. `Container::open`, each section's CRC checked as it is loaded (`Stream::crc()`, `lib/core/CardContainer.h:237`: one pass, not `checkCrc` and then a load). Its `Why` (`lib/core/CardContainer.h:91-136`) decides. `open()` returns Short (`lib/core/CardContainer.cpp:89, 105`), Io, Magic, Major, HeaderBytes (`:102`), SectionCount (`:103`), FileBytes, HeaderCrc, Layout, UnknownRequired, Shape and Missing, a REQUIRED section that isn't there (`:165`). The load adds SectionCrc, Counts and the rows' order. Why's Missing is not the verdict Absent below:

| Why | Verdict | What follows |
|---|---|---|
| Ok, and the rows in order | Loaded | The fold goes on above its MARK |
| Io; FatRecords (`File::damaged()`, `lib/core/TagStore.h:143`) | Unread | At the boot, Io is read again after 100, 300 and 1,000 ms (`tagstore::kBootWaitsMs`, `lib/core/TagStore.h:349`); FatRecords once, as cardroot's rule has it: no read again changes it (`lib/core/CardRoot.cpp:31-36`). Still unread: stats Off this session; no fold, no save, **no trim** |
| Major, UnknownRequired | Newer | Stats Off this session; the file left as it is; no trim |
| Magic, Short, HeaderBytes, SectionCount, FileBytes, HeaderCrc, Layout, Missing (a required section absent), Shape, SectionCrc, Counts, any other Why, or a row out of order | Corrupt | Read once more after 100 ms. Still corrupt: renamed `stats.bad` (an older one removed first), then Absent. `stats.bad` never shares a chain (only the twins are names 2.12.6's rule knows, and it would one day free one that `stats.bad` holds): with another entry on `stats.bin`'s chain, `stats.bin` goes to a twin instead, and an older `stats.bad` on it too. A rename or remove the card fails: Unread |
| — (step 2: the card says there is no `stats.bin`) | Absent | A fresh start: MARK empty, so every slot on the card is folded |

4. Loaded: the base it names is opened with `openRead(path, &cardFailed)` and read whole, as the reference's `load()` does: its header, then its rows' CRC, order and count against baseRows, in one pass, no row kept (24 B a played track: 53 KB at 2,214, 467 KB at 19,478; as built, S1 C7, where this step had its header alone read). A card failure (cardFailed, Io), or rows whose CRC fails, is read again at `kBootWaitsMs`; damaged records once. If one persists, the verdict is Unread: stats Off this session, with no fold, no save and no trim. A base is lost (flags bit 1, baseGen 0) only when the card answers that it isn't there (`look()` Gone), or when its header reads whole with a CRC other than baseCrc. A base shorter than its header doesn't read whole: Unread, as one of any other length that fails its frame. Then the rows before it are lost and the totals stay, and on that boot no `stats-<n>.bin` is removed. A base whose header is the one named but whose rows fail their CRC or their order is a card fault too: Unread, never a lost base. `stats-<gen±1>.bin` are removed only after the named base checked whole.
5. The root's commitTime is kept for sinceDay (`lib/core/CardRoot.cpp:111-114` gains it in `cardroot::Root`).

A `stats.bin` (or a base) the card failed to read is never an absent one. Saving a log-only aggregate over it would erase the history the trims already removed (the queue's precedent: `src/app/QueueStore.cpp:181-186`). With MARK unknown, nothing is safe to trim.

**At a remount** (`remounted()`, 5.7): one read, no waits. The boot's waits are for the boot, where "nothing else is held up" (`lib/core/TagStore.h:345-349`); a remount runs on the loop (`src/main.cpp:3242`), perhaps minutes into the first track. Unread there: no fold, no save and no trim, and the read is tried again at the next remount. A `stats.bin` that reads corrupt there is not renamed on one read: Unread, left for the next boot's two. While the orderly end flushes (a remount inside `PlayHistory::flush()`, `src/app/PlayHistory.cpp:288-290`, within its 3 s, `src/app/PlayHistory.h:101`): no read at all; the folds are dropped and no save follows.

### 5.5 Writing: the save, and its order against the trim

**When.**
- At the boot's scan's end, in `finishScan()` (`lib/core/PlayLog.cpp:318-347`): after the boot rules and before the trim loop (`:332`). Also on the path where `storeBoots` fails (`:323-326`): the fold is whole there, and a save is still right. No slot exists yet (append is Off before Ready, `:523`), so nothing can change under the save.
- In `rotate()` (`:411-463`), after `unread()` (`:423-433`) and before the `shared()` question (`:434`), so it covers the twin rename (`:439-441`) as well as the delete (`:437`). It saves when `unread()`'s pass over `plays.1.jnl` (`:349-375`) met a play slot the card's MARK doesn't cover, or, in a session whose scan was whole, couldn't read the file whole. The rotation goes on whatever the save does: plays are never held back for the stats.
  - With the boot's save done, a rotation saves nothing: the file it deletes is the one that save covered, and `plays.jnl`, holding this session's plays, is renamed, not deleted (`:437, 451`). The rotation's save runs at a second rotation in one session (6,553 more plays), or after a boot whose save failed or was skipped.
  - In the orderly end's flush too: `Outbox::flush()` can rotate (`writeOne()` at `:750`, `append()` at `:701`, `place()` at `:516`, `rotate()` at `:505`), and a power-off or restart always follows the flush (`src/main.cpp:278-281`). There the rotation's save runs when at least 1 s of `kFlushBudgetMs` (`src/app/PlayHistory.h:101`) is left (a rotation's save never compacts). Otherwise the plays in the deleted file are lost to the stats.
- `Hs`, the console's (the rotation's path).

It is skipped, returning false:
- the boot's save, while the orderly end flushes (an "ending" flag set before `outbox_->flush()`, `src/app/PlayHistory.cpp:307`): the 3 s budget is the plays' (`src/app/PlayHistory.h:101`), and `Outbox::flush` can reach `finishScan()` (`lib/core/PlayLog.cpp:743`). The ending flag holds back the boot's save only; skipping it loses nothing (the next boot folds again);
- when the card is away, or another card is in the slot (`cardAway()`, `sectordisk::guard().foreign`): one card's stats never go onto another;
- when stats are Off, or the fold ran out of memory;
- every save of a session whose scan was partial (5.6);
- when the card's `stats.bin` is no longer the copy RAM stands on. The glue keeps what it read at the boot (or a remount's read) or last wrote: no `stats.bin` (Absent, nothing saved yet), or its generation and header CRC. In step 1, after `settle()`, it reads `stats.bin`'s header and compares: a file where none was expected, none where one was, or another header is a change. A change refuses this save and every later one this session, `[listen]` says why, and the next boot reads the card's copy. The card guard's "same card" is its size and two sectors' CRCs (`src/storage/SectorDisk.h:22-35`; `src/app/PlayHistory.h:84-88`): a card that went into another player and came back passes it. Without this check this RAM would be saved over that player's newer `stats.bin` (its plays, and the base it compacted to, lost: the next boot would find its base gone and drop every per-track row), or over a `/.player` deleted on a PC.

With nothing new since the card's copy, it returns true and writes nothing.

**The steps.** The queue's commit (`src/app/QueueStore.cpp:473-500`), not `LibraryUpdate::save`'s (`lib/core/LibraryUpdate.cpp:400-401`), which removes the tmp after a failed `replace()`, possibly the only copy (`replace()` may already have removed the file: `lib/core/TagStore.cpp:493-495`).
1. A tmp held by an earlier save (step 5) is promoted first with `replace()`, as `QueueStore::settleTemp` does (`src/app/QueueStore.cpp:456-467`; `replaceFile` at `:465`): false, the tmp still held. Only while its header is still the one written: another's (a player that had the card meanwhile) is left to `settle()`. The file then written takes the promoted one's generation + 1 (a held tmp promoted is a save that landed), and a remount's read promotes it first too (its `settle()` would drop it while `stats.bin` is there). Then `settle()` with the real check, the header check above, then `prepareTmp()`. A question the card fails, or a `settle()` that fails (a remove, or a rename to a twin): false, nothing done.
2. Compaction, when due (5.3): the new base written, closed, its size checked. A failure leaves the base as it was, and the save goes on.
3. `stats.tmp` created and written through a `ContainerWriter` in 4 KB aligned pieces (`lib/core/TagStore.h:184-195`), then closed.
4. The tmp's size on the card must equal what was written (`QueueStore::tmpWhole`, `src/app/QueueStore.cpp:372-392`). Not: the tmp removed, false.
5. `replace()`. False: the tmp is kept, whole, and held; the next save promotes it first (step 1). A boot's `settle()` takes a whole tmp when `stats.bin` is gone, and drops one while `stats.bin` is there (`lib/core/TagStore.cpp:454, 463`): a held tmp not promoted before a restart is lost, and with it the plays of a `plays.1.jnl` a rotation deleted after that save (13).
6. True: the card's MARK is now the one written, and an old base is removed.

The `ContainerWriter` is about 2.1 KB (`lib/core/CardContainer.h:281-313`: 64 section entries, two u32[64], a 256 B header). The loop's stack is 8 KB, with 2.5-3.8 KB left at its lowest (`docs/METADATA.md:6322`; `docs/AUTODJ.md:1802-1803`). So the writer and the save's buffers are in PSRAM, never on the loop's stack.

**The trim.**
- A log file goes when today's rule holds (every play acknowledged, at least one, a play last, no twin on its chain: `lib/core/PlayLog.cpp:292-297, 310-311, 332-344`) **and** every play slot in it is at or under the card's MARK for its (deviceId, epoch).
- After a save that succeeded, that MARK covers everything the scan read.
- After one that failed, or none (a partial scan, a changed `stats.bin`), it is the MARK read at the boot. So a full card still frees the files earlier boots folded.
- With stats on but no MARK known (Unread, Newer), nothing is trimmed. The log stays bounded: the rotation holds it to two files of 6,553 slots (`lib/core/PlayLog.h:61-64`).
- A fold that ran out of memory can't be saved; the trim then follows the MARK read at the boot, as after a failed save.
- With no stats (a `Folds` of null), the trim is today's.
- No ACKS, no trim, as today.

**Why not the worker.**
- No window: `finishScan()` is one synchronous call (1.1).
- The worker is busy with the post-sync rebuild just then (`docs/METADATA.md:1066-1068`).
- Its 7.5 KB stack is internal RAM, made for each burst of work (`src/app/CardWorker.h:38-61`).
- Folds while it wrote would need snapshots.

**Why not pieces across passes:** a state between the scan and Ready drops plays (1.1).

**What a failed save costs.** Nothing is lost: the card's MARK stays, and the next boot folds the same slots again. The exceptions have one shape: a rotation deletes (or moves to a twin) a `plays.1.jnl` holding plays the card's MARK doesn't cover, and no save lands before the restart. Then those plays are in no file the scan reads. It happens when:
- the rotation's save failed (a full or failing card), or its tmp was held and not promoted before the restart (step 5);
- the stats are Off this session (Unread, Newer, no memory for the fold, the folds dropped while ending);
- this session's saves are refused (a partial scan, a changed `stats.bin`);
- the rotation came in the end's flush with under 1 s of its budget left.

13 lists it.

### 5.6 The fold

**The live fold.**
- At `Outbox::done()`, only when `t.did == Did::Written`: after `append()` or `again()` said Ok, so synced (`lib/core/PlayLog.cpp:711-714`). It runs before `tracker_.pop()` (`:641-642`), so `p` is still the ring's oldest.
- Through a new `Outbox::Env::written(const Slot&, const Played&)`, beside `tried()` (`lib/core/PlayLog.h:486`). It has an empty default body, so test Envs compile unchanged.
- `done()` also runs for Dropped plays: Gone, Off, Nvs, Blocked, and given up after 3 tries (`:649-658, 676-679, 718-729`). Those are never folded.
- The fold takes `t.slot` exactly as encoded (`:702, 710`): `append()` fills its seq, epoch, deviceId and bootSeq (`:536-541`), and on a retry it is still the first try's slot. It is never dated again from `boardclock::now()` or `p.startMono`, so the next boot's fold of the same bytes gives the same result.
- From S2 (the lists and the map use ids; S1's fold is hash-keyed and asks for none): `p.file.path` names the file, and `LibraryIndex::findTrack()` gives its id (`lib/core/LibraryIndex.h:395-397`) when the index is readable; behind the fence it is not asked.
- RAM only: no card write and no NVS write for a play.

**The boot's fold.**
- `Journal::Config` gains a `Folds*` (null: no stats, today's behaviour; 9.5's C4 lists its calls).
- `startScan()` (`:153-165`) calls `scanBegan(again)`. A scan begun again after a remount (`:167-174`) has the glue read `stats.bin` again, dropping the folds made before. Without that, every slot read before the remount would count twice.
- Each slot `decode` says Ok goes to `slot(s, file)`, after the damaged-slot skip (`:285`). Drop notes (kind 2) are skipped by the stats: their playedMs is a dropped count and their seq 0 (`lib/core/PlayLog.h:82`; `lib/core/PlayLog.cpp:22-28`).
- A slot that counts is folded when its seq is above the MARK read at the boot for its (deviceId, epoch), from every deviceId (O2).
- **A partial scan.** Today the scan takes an open the card failed for a missing file (`fs_.open()` null, the next name: `:256-261`) and ends a file at a failed read (`:270-275, 302-306`). Raising MARK past slots it never read would lose them for good (once ACKS covers them, the trim deletes the file). So with folds the scan opens each log file with `openRead(name, &cardFailed)`; an open the card failed, or a read that failed, calls `Folds::partial()`. After a partial scan no save runs this session: the boot's, a rotation's and `Hs` all return false. The trim follows the MARK read at the boot, and keeps the file not read whole (as today's scan keeps a file it couldn't judge); the next boot folds again. A partial scan counts no gap (below): the slots it didn't read may hold those seqs. The same holds within a session: live folds raise MARK in RAM above an unread tail, and that MARK is never written.
- **Each slot once.** In one file, one device and epoch's seqs only rise, so a slot at or under the seq last folded for its row from that file is skipped. Across files, each row keeps the seq range each file read so far gave it, at most 10. A slot inside another file's range is skipped and counted in `twice`. Two files on one chain (a rename cut that `settle()` couldn't part: `:227-243`) give the same slots twice, and this keeps one. A conflict (one key, different bytes) keeps the first read; mStream's reader drops both (2.20.6 item 3), which a log with this seq rule can't produce. The reference (`tools/statsref.py`) keys each slot by (deviceId, epoch, seq): the first read kept, one met again from another file counted in `twice`. The ranges give the same on any log a device writes, and on every fixture: each fixture's log files keep one device and epoch's seqs in order, and `tools/test_statsref.py` checks that, and that the ranges choose the reference's slots, on every boot.
- **The gap.** For each ACKS row above the MARK read at the boot, the seqs it covers that no valid play slot of the log holds (a firmware without stats trimmed them) are added to HEAD's gap, and MARK rises to ACKS's seq: those seqs are gone for good (ACKS covers only seqs the software read from this card, and seqs only rise), and a MARK left under them would count them again at every boot.
- **Order** comes from the slot, never from the scan: firstAt and lastAt are times; a play's round is the boot's round (below).
- **The cost** is CPU and RAM only: no read beyond the scan's own. Inserting a row must not move the table (a sorted insert into 64 KB is about a millisecond of PSRAM copying), and a row a slot would make a full never-synced log's TRKD 314,544 B (13,106 x 24). So a slot finds its row by key:
  - During the boot's fold, TRKD and RCNT each carry a transient open-addressing index of u16 row numbers, a power of two at least twice the rows (16,384 slots, 32 KB, at 5,000 hashes; past half full it is carved again at twice the size and filled from the rows). It is carved at `scanBegan` and freed at the scan's end, just before the save (as built: `statsfold::Fold::endBoot()`, which the save's plan calls too). A slot merges (⊕) into its row or appends one, so the rows equal the distinct keys. With no memory for an index (or past the 65,535 rows a u16 names) the fold goes on without it, finding rows as live folds do: slower, the same rows.
  - Live folds binary-search each chunk's sorted rows and scan the rows appended since the last sort (at most 64: past that each chunk holding them is sorted again, in place).
  - The save sorts each chunk (below) and merges them into the `ContainerWriter`, within its own loop time. The rank's snapshot is merged from the sorted chunks on the loop as it is copied (6.8).
- **Memory.** TRKD, RCNT and hash → id (6.1), and the rank's snapshot, are chunked arrays of fixed 16,384 B chunks (carving 16,900 B, the played set's class: 6.5). None is ever one block, and none is realloc'd: a TLSF realloc to a larger size allocates, copies and frees, a carve the heap model can't foresee. HOUR and DAYS are carved once at their caps (6,720 B and 6,400 B): full, a new hour or day takes the oldest row's place, or goes when it is older than every row kept, so RAM holds the newest 840 and 400 that the save's retention keeps, whatever order the slots come in. MARK is in a block of 64 rows (1,536 B; RAM's row keeps the card's seq in the reserved u32) made larger only past 64 (deviceId, epoch) pairs. The boot's fold also carves, while it runs, the ranges (80 B a MARK row: 5,120 B for 64) and the ACKS rows above MARK (112 B each, at most 16).
  - The load takes the chunks `stats.bin` needs, plus one spare chunk for whichever of TRKD and RCNT a live fold fills first, in `PlayHistory::begin()` (`setup()`, `src/main.cpp:3908`, before the join can begin).
  - The boot's fold takes more only in a pass where Auto DJ's join isn't Working; otherwise the scan waits that pass. A live fold that uses the spare has it replaced at the next pass where the join isn't Working.

**The fold round.**
- The boot's fold gives its plays round `HEAD.round + 1`; live folds this session, `HEAD.round + 2`. The save writes the highest round used.
- Live plays the next boot folds again (not saved by a rotation) get that boot's round. It is higher than the round they had in RAM: the order holds, the number differs.
- So the proof (S1) compares counts byte for byte and rounds by order.
- 24 bits: 16.7 M rounds, saturating.

**The seq rule** (`lib/core/PlayLog.cpp:327-328`): `seq_ = max(NVS's seq, the card log's, MARK's, ACKS's)` for this device and epoch.
- Today a settings partition written back from a backup (the house's own end-of-check step: `docs/RELEASE-0.14.0-CHECKS.md:116-118`) puts NVS's seq behind.
- On a card whose log was trimmed, new plays then reuse seqs under ACKS: they read as acknowledged and are never sent.
- They are also under MARK, so the stats would skip them.
- 2.20.4 lets a device reserve seqs, so a higher start breaks no reader. Its own commit in S1.
- Two places state today's rule and change with it: METADATA 2.20.4's "At boot" sentence (`docs/METADATA.md:3112-3122`, C2) and the reference that writes the play log's goldens, `tools/playlog.py`'s `boot_seq()` (`:244-248`, used by `Device.boot` at `:531-545`; C5).

**The play log after S1**, against 0.14.0:
- the scan reads on a new epoch with no ACKS;
- the trim also needs the folds;
- the rotation may save first;
- the seq rule as above.

Slots, files, ACKS and the transfer's reading are unchanged. `test_play_log`'s 33 tests (`test/test_play_log/test_play_log.cpp:3064-3096`) pass with no `Folds`, with `atBoot`'s count from 5 to 6 (`:454-455`: the seq rule's new golden case, C5).

### 5.7 Card events

| Event | What happens |
|---|---|
| Remount during the scan | The scan begins again (`lib/core/PlayLog.cpp:167-174`); `stats.bin` is read again first (one read, no waits: 5.4), and a `stats.bin` unread at the boot is tried again there |
| Remount after Ready, before this boot's first slot | The scan begins again (`lib/core/PlayLog.cpp:171-173`; `src/app/PlayHistory.h:84-87`); `stats.bin` is read again (`scanBegan(again)`, one read, no waits) and the boot's save runs again (nothing new: it writes nothing) |
| Remount after the first slot | The live folds go on; the next save checks the card's copy first (5.5), so a card that was in another player meanwhile is never saved over |
| A remount inside the orderly end's flush (`src/app/PlayHistory.cpp:288-290`) | No read; the folds are dropped and no save follows (5.4) |
| A log file the boot couldn't read whole (an open or a read the card failed) | A partial scan: the fold goes on, no save this session, the trim by the MARK read at the boot; the next boot folds again (5.6) |
| Card pulled | No save, no trim. Plays not written were never folded |
| Another card in the slot | FatFs refuses writes (STA_PROTECT) and the guard restarts, or under the breaker it doesn't (`src/main.cpp:3199-3236`). The save checks `foreign` and `writesOff` and writes nothing. RAM is the first card's and is dropped at the restart |
| Card full | The save fails (5.5); the trim follows the old MARK |
| Read-only (`writesOff`) | Save and trim fail; nothing lost |
| Reformatted, or `/.player` deleted | `stats.bin` is absent with the log: counting starts again (a player that stayed on through it saves nothing more this session: 5.5) |
| Only `stats.bin` deleted | Counting starts from what the log still holds; older history is gone |
| NVS erased | A new epoch, a new MARK row; the old epoch's slots fold once under their own row |

### 5.8 Every player on the card, each slot once

- The stats count every deviceId (O2). ACKS's 16 rows govern only the trim (`lib/core/CardManifest.h:159-175`).
- A card used by two players: each folds every slot above the card's MARK and saves one `stats.bin`. A player's last session, not yet saved, is folded by whichever player boots with the card next.
- Each slot once:
  - one atomic file holds the aggregate and its MARK;
  - MARK rows are kept for good (5.2);
  - the boot folds only above MARK, each slot once (5.6);
  - a scan begun again drops its folds; a partial scan saves nothing (5.6);
  - a save over a `stats.bin` that isn't the copy RAM stands on is refused (5.5);
  - live folds are RAM only, of Written slots, with fresh seqs (`lib/core/PlayLog.cpp:531-535`);
  - the trim and the rotation delete only what the card's MARK covers, or what is saved first;
  - seqs only rise (`docs/METADATA.md:3109-3131`; and 5.6's rule).
- Rare cases that undercount by design:
  - a slot given up on (3 card failures) whose bytes had landed, under a MARK raised later that session: never folded;
  - a firmware without stats (0.14.0) trims ACKed files it never folded (`lib/core/PlayLog.cpp:332-344`). The next stats boot sees ACKS above MARK with those seqs gone, and HEAD's gap says how many.

### 5.9 The clock and the time zone

- **Unset** (startedAt 0, clock 0: `lib/core/PlayLog.cpp:692-693`): HOUR, DAYS and RCNT need a day, so an undated play counts only in HEAD (in `undated` too) and in its TRKS row (firstAt 0 only while the row has no dated play, lastAt untouched, lastOrd its round: 5.2's merge).
- **Set during a play:** the slot is dated from its start (`lib/core/WallClock.cpp:167-180`); earlier slots stay 0. Dating them back from a later slot of the same boot (2.20.6 item 7) is possible later, only if the live and the boot's folds do it alike.
- **The RTC not believed** at the boot (`wallclock::Boot::Foreign` and the rest: `lib/core/WallClock.h:101-119`): as unset.
- **Corrections:** a `@time` moves the clock either way (`src/app/BoardClock.cpp:363-366`); the RTC drifts about 3.5 s a day (`docs/RELEASE-0.14.0-CHECKS.md:59`). A later today than a play's day is fine; counting days never go under 1.
- **Another device's** startedAt outside 2026-2099 is undated: the u24 hour and u16 day would overflow or go negative.
- **UTC now (O4).** Hours and days are UTC. Every function that turns them into labels or windows (lib/core `StatsView`, `StatsText`) takes `tzOffsetMin`, 0 until S6.
- **The time zone (S6).**
  - `@time` parses only field 0 today (`lib/core/HostLink.cpp:327-339`). Extra fields are ignored (`lib/core/HostLine.h:91-98`), so 0.14.0 takes a terminal that sends more.
  - S6 reads an optional field 1, minutes east of UTC (−720 to +840), into `HostLink::Out` (`lib/core/HostLink.h:109`), keeps it in NVS, and shifts the display.
  - Nothing stored changes: HOUR gives local days for at least the last 35 days, and a change of zone (DST, travel) moves the whole display at once and misfiles nothing.

### 5.10 NVS

- No key for the stats, and no write a play (`src/app/NvsLater.h:19-21`). `plays` is untouched (`lib/core/NvsLayout.h:210-236`).
- S6's `clock/tz` (an i16) is schema 7 (`kCurrent` at `lib/core/NvsLayout.h:77`), with a `migrate(6, 7)` that does nothing (`src/app/NvsSchema.cpp:42-53`): no key reads as UTC. Older firmware never reads it.
- The Bluetooth EQ claims the next schema too (`docs/ARCHITECTURE.md:1866-1869`); whichever lands second takes the next number (10).

### 5.11 What the terminal may do

- It never writes `/.player` (2.11: read only). The listener may delete it whole (2.11's Listener column, `docs/METADATA.md:1790`), and a format the listener confirms (`@format`) clears it too: the stats start again with the log. For caches "the device rebuilds" holds; for the stats it doesn't, and 2.11's row says so (C2).
- It never reads or writes the stats files. `device.txt` gains no key.
- Its sync's ACKS lets the next boot trim, after the fold and the save.
- Later (not v1): it may write mStream's per-track counts as a gen-named file of its own (spec 8.6). Not played yet would then mean never, and the counts would include other clients.

### 5.12 Compatibility

`stats.bin` is primary data, not a cache. `library.idx` answers a version change with one rebuild (`Load::Outdated`); copying that for the stats would lose, for good, every play the trims already removed. So:
- a reader keeps reading every major it ever read;
- a newer major is left alone (Off);
- a minor adds sections or longer rows only for what a save by an older firmware can drop (re-derived, or counted again from the log). A section a minor adds is written OPTIONAL (flags without `kSectionRequired`, `lib/core/CardContainer.h:42`), so an older reader skips it instead of reading the file as UnknownRequired and turning the stats Off; only major 1's own sections are REQUIRED (5.2). Anything that must not be lost needs a new major.

| Combination | Result |
|---|---|
| 0.14.0 on a card with `stats.bin` | Never reads it. Trims ACKed files without folding them: those plays are missed when the card comes back. HEAD's gap counts them |
| Stats firmware after a 0.14.0 session | Folds what the log still holds above MARK |
| A newer major on the card | Stats Off this session; no trim; the log rotates as ever |
| A newer minor | Read by its known sections and row prefixes (its added sections are OPTIONAL, so they are skipped); a save writes this minor (drops the additions: the rule above) |
| `stats.bin` corrupt | `stats.bad`; counting starts again from the log |
| The base lost or wrong | Per-track rows before it lost (shelves thinner); totals intact; flags bit 1 |
| `/.player` deleted | Starts again |
| Two players alternating, either firmware with stats | Each slot once (5.8) |
| A unit alternating 0.14.0 and stats firmware | Undercounts what 0.14.0 trims; no double count |

### 5.13 A worked example

Device A, epoch E. `stats.bin` says MARK (A, E) = 41. `plays.1.jnl` holds seqs 30-45, `plays.jnl` 46-48. ACKS (A, E) = 45.

| Seq | Slot | Verdict |
|---|---|---|
| 42 | Track X, durationMs 200,000, playedMs 120,000, completed, Auto DJ, startedAt 2026-11-08T21:40:00Z | Play (line 30,000). Day 311, hour 7,485, time 26,948,401 |
| 43 | Track Y, durationMs 240,000, playedMs 12,000, skipped | Skip (line 30,000) |
| 44 | Track Y, durationMs 40,000, playedMs 25,000, stopped | Play (line 20,000) |
| 45 | durationMs 0, playedMs 8,000, stopped | Neither (line 30,000): 8 s listened |
| 46-48 | Three plays | Plays |
| 30-41 | | Under MARK: not folded |

- Day: January to October 2026 are 304 days, so 8 November is day 311 (2026-01-01 is 0). Hour: 311 x 24 + 21. Time: 311 x 86,400 + 21 x 3,600 + 40 x 60 + 1.
- Y's row: plays 1, skips 1, firstAt and lastAt 44's time.
- MARK becomes 48 and the save writes it.
- The trim: `plays.1.jnl` (30-45) is under the new MARK and under ACKS: it goes. `plays.jnl` (46-48) is under MARK but over ACKS: it stays.
- The next boot finds nothing above 48: no save.

## 6. The stats map

### 6.1 What needs ids

- Ranked rows and the shelves' snapshots need track ids: the rankers give hashes.
- Not played yet needs, for each album, whether any of its tracks has a play. That needs every played hash.
- The NP sheet needs neither: the playing track's hash is known at its start (`src/app/PlayHistory.cpp:149-152`), and its facts come from its TRKS row (6.8). Its jump uses `kShowPlaying` (`src/ui/Page.h:46`).
- A live play's id comes from `findTrack(p.file.path)` with no walk (from S2: C14).
- Track records hold no hash (`lib/core/LibraryIndex.h:182-195`), so a bare hash needs the walk.

**Resident:**
- a hash → id map for the hashes in RCNT and in the lists, sorted by hash, 12 B a row (u64, u32 id: no 65,534 cap, unlike Auto DJ's u16 maps, `lib/core/AutoDjJoin.h:120-122`);
- one bit an album: played or not.

**Transient, for a walk:**
- every hash with a play, sorted, 8 B each, in chunks of 2,048;
- the folder states.

### 6.2 The walk

- The Folders and Hash phases of Auto DJ's join (`lib/core/AutoDjJoin.cpp:584-647`: each folder's path hashed once, then each track's hash from its folder's) go into a portable stepper, `lib/core/PathHashWalk`. The join keeps using it with the same output, so there is one hashing rule, already tested against `trackPathHash()` (`:105-120`; `docs/AUTODJ.md:1499`).
- The stats run it alone, on the loop, in slices of up to 8 ms a pass, as `stepJoin` gives the join (`src/app/AutoDj.cpp:624-649`).
- Its gates: the UI started; the index readable; the card not held; no list moving; no update holding the index; the lists ranked; and never in a pass where `joinWorking()` is true. Two walks in one pass would stretch it from about 13.5 ms to about 21.
- While it works, the loop's idle wait is `kJoinLoopDelayMs` (`src/main.cpp:123`), as for the join (`:4255`).
- For each track it hashes: the hash is looked up in the played set (its album's bit set) and in the wanted set (its id kept).
- It aborts when the index isn't the one it began on (pointer, `ready()`, `trackCount()`, `buildStamp()`), as `Join::matches` does (`lib/core/AutoDjJoin.cpp:294-297`). It starts again; it never resumes.
- `Join::setPathHash()` is not the hook: any hook but the default turns off the folder states (`:342`), and the join would hash whole paths again (about twice its CPU).
- Riding the join while Auto DJ is on is left out (14).

### 6.3 Cost

- On the host, the hashing was 1.0 ms of a 5.8-6.1 ms join, and the device's join is 0.55-0.65 s of CPU (`docs/AUTODJ.md:977`). So the hashing is about 0.1 s of device CPU (ESTIMATED).
- With a lookup a track: 0.12-0.2 s of CPU, 15-25 passes of about 13.5 ms; about 0.2-0.4 s idle and 0.4-0.8 s with an MP3 (GUESS). S2 measures it (`Hw`).

### 6.4 When it walks

- After the boot's ranking.
- After every update's Live (`src/main.cpp:3460-3467`).
- When a list gains a hash with no id that isn't a live play's (a UTC day's change can bring one into a window).
- The cache key is (`buildStamp()`, `trackCount()`) (`lib/core/LibraryIndex.h:467-471`). The index is the same object across a rebuild (`src/app/Library.cpp:106-109`), so the pointer is no key. The stamp leaves out the mood words, so a score-only rebuild keeps the ids.

### 6.5 Sizes at 19,478 tracks

Carves are MODELLED: + 12 B, rounded up to the 1/32 class, + a 4 B header (`test/test_autodj_join/test_autodj_join.cpp:1892, 1910-1916`). Each figure is a block's footprint, its carve plus the header; the request is in brackets. A chunk of 16,384 B takes 16,900 B.

| Block | Life | P = 2,214 played | P = 9,739 | P = 19,478 |
|---|---|---|---|---|
| Played set, 8 B a hash, chunks of 2,048 (16,384 B) | Walk | 2 chunks: 33,800 B | 5: 84,500 B | 10: 169,000 B |
| Folder states, 10 B a folder | Walk | 25,092 B (24,840 B at 2,484 folders) | same | same |
| The rank's snapshot of TRKD and RCNT, chunks of 16,384 B (5.6) | A rank | 1-2 chunks: 16,900-33,800 B | up to 9: 152,100 B | up to 9: 152,100 B |
| Hash → id, 12 B a row (RCNT and lists: at most 4,792 rows), chunks of 16,384 B | Resident | 1 chunk: 16,900 B | up to 4: 67,600 B (57,504 B) | up to 4: 67,600 B |
| Album bits | Resident | 236 B (219 B at 1,747 albums) | same | same |
| Not played yet's list (u32 an album, as FilteredAlbums) | While that shelf shows | 7,044 B (6,988 B) | same | same |
| A shelf's snapshot (200 rows) | While it shows | 3,268 B (about 3.2 KB) | same | same |

- Every block goes through `psramAlloc` (`src/app/Psram.h:10-14`): a malloc under 4 KB would land in internal RAM.
- A dense 4 B x 19,478 array (77,912 B, carving 79,872) is avoided: it is the size class of Auto DJ's filters block, which once failed to place after an update.

### 6.6 Rebuilds, renames, orphans

- **An update step:** behind the fence `Library::index()` is null (`lib/core/LibraryUpdate.h:109-114`). The hash-keyed data (everything in `stats.bin`) is untouched. The ids, album bits and snapshots are dropped at `enterFence()` (`src/main.cpp:3412-3417`) and their bytes counted in `alsoFreed` (`:3570`). The walk runs after Live.
- **A rename, a conversion to Opus, a name in another normal form:** a new path hash. The old row stays in the totals and matches no track: an orphan, in no shelf. Following renames through ORIG's audioHash is later (spec 8.6).
- **A track gone from the card:** an orphan.
- **Two siblings equal under textfold:** `findTrack()` still finds each, since `textfold::compare()` falls back to the bytes (`lib/core/TextFold.cpp:631-632`) and the files are sorted by it (`lib/core/LibraryIndex.cpp:80-95, 1340-1345`).
- **A 64-bit collision:** the walk maps neither, as the join does (`lib/core/AutoDjJoin.cpp:805-809`).
- **The `/music/` prefix:** PlayHistory checks it case-sensitively (`src/app/PlayHistory.cpp:150-151`), `trackPathHash()` in any case. They agree because the index's root is the literal `/music` (`lib/core/LibraryUpdate.h:242`).

### 6.7 The heap model

- `test_a_library_update_in_a_model_heap` (`test/test_autodj_join/test_autodj_join.cpp:2789`) gains a `StatsRun`, beside `MoodRun` (`:2674-2680`), in the order the blocks come on the device:
  - S1 (C8), the hash-keyed half: the load's chunks at `PlayHistory::begin()`'s place, after Auto DJ's glue (`src/main.cpp:3901, 3908`); the boot fold's growth, up to the full-log case (TRKD about 120 KB and its 32 KB index; RCNT, pruned only at the save, about 160 KB in 10 chunks and its 64 KB index: 412,992 B held at the peak in `test_stats_fold`'s full log), after the boot's `joinNow()`;
  - S2 (C13): the id-keyed ones dropped at the fence and taken again after each Live; the rank's blocks (the snapshot, the played set's chunks, the lists, the sparse index) and the walk's folder states after `joinNow()`, at the boot and after each Live, held from the rank to the walk's end, never while the join's `begin()` holds its spacers (`lib/core/AutoDjJoin.h:102-105`). It asserts the join's lowest `>= kMargin` with them taken.
- It runs at P = 2,214 and at P = 19,478.
- It holds the mood plan's pass rule (`:2898-2925`): `lowest >= kMargin`, `fence == off.fence`, `givenBack == off.largest`, joined within 122,880 B of the control and at or over 245,764 B where the control is. Opus's 65,536 and 62,464 B blocks must still fit after a FLAC update.
- The only `GiveBackFn` is Auto DJ's (`src/main.cpp:3901`). The stats' resident blocks are held through every build and come out of the update's room. On the device that room is thin: joined after a FLAC update and the rejoin is 262,132-294,900 B (`docs/MOOD-FILTERS.md:932, 954`), 16,368-49,136 B over the 245,764 B line, less than the typical resident set (11); the mood words' 38,826 B block cost exactly one 32 KB class of joined (`:932`). So the resident blocks must stay out of the largest free block, or rule 3 forces a redesign (`docs/MOOD-FILTERS.md:768-771`).
- **The ladder**, in order, if joined falls by more than 122,880 B, or under 245,764 B where the control is above it:
  1. the resident tables in 16 KB chunks (5.6, the design from the start), so they fill the holes the boot leaves: the model shows whether they do;
  2. hash → id for the lists' rows only (696 rows, about 8.4 KB: one chunk), with a walk after any re-rank that brings a new hash, instead of covering RCNT's 4,096 rows as well (saves up to 49 KB);
  3. RCNT capped at 2,048 rows (24.6 KB).

### 6.8 The rankers and the per-track read (the card worker)

- **One new job**, `ScanScheduler::Job::Stats` (`kJobs` 8 to 9: `lib/core/ScanScheduler.h:101-102`), in two lanes of `ScanScheduler::decide()`. A job `decide()` doesn't name would fall to `priorityOf()`'s default of 0 with no rule (`lib/core/ScanScheduler.cpp:177`). The waveform read's lane is not the rank's: it starts before the update step's save and before any yield check (`:121-125`; `lib/core/ScanScheduler.h:31`, "The audio's yields below don't hold them"), at priority 1, and only background work waits on `updating` (`:134-136`; `.h:38-40`).
  - The **Track** read (one read of at most 1,536 B) runs beside Wave, after it: `In::statsTrack`, priority 1, 0 while a list moves.
  - The **Rank** is background work, after the walk and the compaction and before the scan: `In::statsRank`. It is held by `updating` (so it starts after `library.idx`'s save), by a list moving and by every yield (Input, Ring, Underrun, DecodePass, TrackChange, Seek, Bluetooth). Priority 0 while lit and 1 while dark, as Compact; one step, never cut. A rank reads the whole base (467 KB at 19,478 played, 11) while holding the SPI bus and FatFs's volume lock against the decoder (`src/storage/CardFat.h:33-35`), and the SD driver busy-waits (`src/app/CardWorker.h:24-31`): in the Wave lane it would time-slice with the loop at priority 1 for its whole step.
- **Two kinds of work:**
  - **Rank:** a snapshot of TRKD and RCNT, copied to PSRAM on the loop (merged from the sorted chunks as it is copied: 5.6; its µs on `Hr`), goes to the worker with today and sinceDay. The worker reads the base front to back, merged with the delta, and makes:
    - the lists of 4.3 (each cap plus 16);
    - the played set (6.1);
    - a sparse index of the base, every 64th hash (12 B each: 3,660 B at 19,478).
    The loop takes the result when the step ends, then walks.
  - **Track:** at a track's start (`PlayHistory::file()`, `src/app/PlayHistory.cpp:146-160`), its base row: one read of at most 64 rows (1,536 B) found through the sparse index. No base (baseGen 0): no read. The facts are that row ⊕ its TRKD row.
- **When the lists rank:** after the boot's Ready; after a Live; at a UTC day's change; when a stats page is entered after folds since the last rank. Results show at the next `enter()` of a shelf; the hub's lines refresh in place.
- **Against Auto DJ's join.** "After the boot's Ready; after a Live" are the moments the join begins (`stepJoin` at the first pass after Live or after the first frame: `src/app/AutoDj.cpp:636-642`), and the join's 64 KB margin covers only another task's small blocks, "the card worker's 4-16 KB buffers" (`:122-138`); a block another task takes inside `begin()` makes the placement fall short (`lib/core/AutoDjJoin.h:96-105`, `topShort`). The lowest PSRAM free in an update's join is 175,032-180,876 B on the device (`docs/MOOD-FILTERS.md:932`). So:
  - a rank starts only when Auto DJ's join is neither Working nor due: Auto DJ off, or its join Ready since the last Live or boot;
  - Auto DJ's `stepJoin` doesn't begin while a stats rank or walk holds blocks (a `Block` reason beside Build and Reads, `src/app/AutoDj.cpp:638`; `src/app/AutoDj.h:193`);
  - the loop carves every block the rank fills before `worker.start()`: the snapshot, the played set's chunks (at most HEAD.baseRows plus TRKD's rows), the lists and the sparse index. The worker allocates nothing.
- **Between ranks:** a live fold moves its track to Recently played's top and patches its counts in the lists. Nothing is re-sorted on the loop.
- **Compaction** never runs with a Stats step under way: it happens only in a boot's save, before Ready, when no Stats step has begun.

## 7. The UI

### 7.1 Pages and navigation

- **New PageKinds** `StatsHub`, `StatsShelf`, `StatsWeek`, after `AutoDj` (`src/ui/Page.h:20-36`). They are named in `pageKindName()` (`src/ui/Ui.cpp:46-64`).
- **`Ui::pageFor()`** (`src/ui/Ui.cpp:268-285`): Hub and Shelf go to `libraryPage_`. So the status band and `shownTracks()` work as on any Library list (`:578-582`, `:1926-1936`). Week goes to a new `statsWeekPage_` (`src/ui/Ui.h:590-595`).
- **Depths:** the root (Artists) is 1, the hub 2, a shelf or Week 3 (with the crumb: `src/ui/LibraryPage.cpp:387`), an album from Not played yet 4 (`kMaxDepth` 8: `lib/core/NavModel.h:28`). Back is "‹" (x 0-55), the crumb (x 240-319), or the Library tab.
- **PageRef.id** is `shelf * 4 + window`: shelves 0 Most played, 1 New finds, 2 Old favourites, 3 Not played yet, 4 Recently played; windows 0 seven days, 1 thirty, 2 All time. The window last used is kept per shelf for the session. `segment()` reads `id_ <= 2` (`src/ui/Pages.h:468`), so shelf ids 0-2 look like segments: keep every use of it behind `root()`.
- **From Now Playing:** `showLibrary(Artists, {StatsHub, StatsShelf(scrollPx = kShowPlaying)}, 2)` (`src/ui/Ui.cpp:639-656`). `showLibrary()` puts the root back on Artists (`:644-649`), as Go to artist does.

### 7.2 What ListLayout and ListView need

The list's items are 42 px (`lib/core/ListLayout.h:24`), with at most one item 0 (`:34-38`). Most played needs the tray and the top bar both.

1. **`Kind::TopRow`, a lead item ahead of the top bar.** `ListLayout::set(rows, topBar, topRow)` with a lead count of 0-2, used by `itemCount()` (`lib/core/ListLayout.h:38`), `item()` and `itemOfRow()` (`lib/core/ListLayout.cpp:6-41`). `ListView` handles it like `topRow(it)` today (`src/ui/ListView.h:424-427`): `renderItem` (`src/ui/ListView.cpp:423-473`), `tapItem` (`:1225-1266`), the Down (`:1307-1345`). Host tests beside `test/test_ui_nav/test_ui_nav.cpp:183`.
2. **Inert rows** for the disabled hub rows. `pressable()` false ignores a tap (`src/ui/ListView.cpp:1251`), and a Tap acts on `pressedItem_` (`:1373-1379`), which an unpressed row never sets. New `Source::inert(row)` and `onInertTap(row)`: the item is remembered at the Down without the press look; a Tap on it, the list still, calls `onInertTap()` with no tick.
3. **`refreshTopRow()`** beside `refreshRow()`, which takes data rows only (`:304-311`): the entry row's and the tray's redraw in place.
4. **`recountStaged()`** beside `reload()`, which always redraws the whole band (`:288-297`: 35-50 ms idle, 80-100 ms with an MP3: `src/ui/ListView.h:60-62`). It sets the layout, keeps the lead items, and pushes one stale on-screen item a pass. An edge change (312 to 320 px) falls back to `reload()`.
5. **The empty line under the lead items**, at `kTop + lead * 42` (`drawEmpty()`, `src/ui/ListView.cpp:584-601`). Today it draws only with no items or under a row-like item 0 (`:484`).

### 7.3 The way in: Library > Artists, item 0 (frame Entry)

`topBar()` and `topBarIsRow()` become true on the Artists root while the row is due (today only Albums has a top row: `src/ui/LibraryPage.cpp:584-591`); `drawTopRow()` and `onTopRowTap()` (`:1101-1152`) dispatch by segment. The geometry is FilterGrid's first row (`lib/core/FilterGrid.h:193-216`):

| Part | Geometry |
|---|---|
| Row | Item 0, 42 tall, scrolls with the list; 284 wide beside the rail; ROW_SEL when pressed; divider from x 8 |
| Bars (the icon) | 7 bars 4 px wide on a 5 px step, x 9-43, standing on y 32; 24 px is the top day; past days DIM, today violet (`accent::Library`); no plate |
| Title | "Listening stats", Body TXT, x 54, centred on y 13; room 204 |
| Line | Small DIM, y 31, room 204: `stats.entry_sub`, else `stats.entry_sub_short` (de needs it), else cut |
| Chevron | FAINT, at right − 14 |

- **Tap:** ROW_SEL at the Down; at the lift a 33 ms tick, then the hub.
- **No hold:** a long press ends as a tap (`src/ui/ListView.cpp:1380-1391`).
- **Hits:** the row is x 0-279; the rail keeps x 280 on (`:1308`).

### 7.4 The hub (frame Main)

- **Header**, depth 2: "Listening stats" Bold. The sub (Small DIM), "74 h 10 min since 1 Sep" (155 px), doesn't fit the 139 px beside the title and goes under it (`subBelow`: title y 10, sub y 26).
- **Rows:** six of 42 px, 312 wide; a thin scrollbar at x 314 (thumb 107 px).
- **The row recipe:** icon 1-bit SOFT, centred at (26, 21), drawn there directly (`ListView::icon` centres at x+22); title Body TXT x 54, y 13; line Small DIM y 31, room 232; chevron FAINT at x 295.

| Row | Icon (new unless said) | Line |
|---|---|---|
| Last 7 days | The 7 bars | `stats.time_plays`: "7 h 37 min · 118 plays" |
| Most played | Trophy 18 x 14 | The All time top three, " · " between, cut with "…" |
| New finds | Spark 18 x 18 | `stats.new_finds_sub` (de: the short form) |
| Old favourites | Heart 18 x 15 | `stats.old_favourites_sub` |
| Not played yet | Ring with a play mark 18 x 18 | `stats.since`: "612 albums since 1 Sep" |
| Recently played | Clock 18 x 18 | "Slow Lantern · 25 min ago" |

- **Disabled row:** icon FAINT, title and reason DIM, no chevron, no press look (the house never uses FAINT for text). A tap: the inert double buzz and the amber toast (4.5).
- **The status row** (no dates), in row 1's place: the clock icon in AMBER; line 1 Small AMBER "No dates yet: the clock isn't set" (206 of 250); line 2 Small DIM "Plug in to the MP3 Player tab" (186 of 250). No press, no tick, no chevron (the precedent: `src/ui/Pages.h:745-748`).
- **Taps:** a row ticks and pushes. Rows are hit across the whole width. No holds: `holds()` excludes the new kinds (today it is true for all but None and Disc: `src/ui/LibraryPage.cpp:951-954`).
- **Scrolling:** a swipe up from the strip scrolls the list, as on any list (`src/ui/ListView.cpp:1295-1305`). No haptics.

### 7.5 The empty state (frame NoPlays)

The hub with nothing played has 0 rows and no top bar, so it shows the firmware's empty state (`src/ui/EmptyState.cpp:26-80`):
- the header: "Listening stats" alone;
- a 48 px CARD disc at y 103 holding the chart icon (new, 18 x 18) in SOFT;
- "No plays yet" in Title (room 304);
- "Plays count from 30 s or half a track" and "Your stats stay on this card", Small DIM;
- one violet "Shuffle all" with `icons::kShuffle`, x 70-249, y 198-231. It calls `ui_.shuffleAll()` (`src/ui/Ui.cpp:433`), with its tick.

`onEmptyAction()` (`src/ui/LibraryPage.cpp:673-685`) branches on `kind_`: today's handler rescans the library or retries the card.

### 7.6 The shelves (frames MostPlayed, NewFinds, EarlyDays, NoClock)

**Header**, depth 3: the title Bold in x 28-207; the window's sub under it (`subBelow`), so it stays after the tray scrolls away; the crumb (99 x 26 at x 215).

| Shelf | Sub | Items before the rows | Rows |
|---|---|---|---|
| Most played | "last 7 days", "last 30 days", "since 1 Sep" | The tray (when offered), then the top bar (when there are rows) | Ranked: "Lumen Ferry · 12 plays" |
| New finds | "first heard in 30 days" | The top bar | Ranked: "Sefa Ilmar · found by Auto DJ · 9 plays" ("found by shuffle"; nothing when the listener picked it) |
| Old favourites | "not in 30 days" | The top bar | Ranked: "Halvard Moss · 9 plays · last 14 Sep" |
| Not played yet | "since 1 Sep" | None | Album rows with covers (`src/ui/LibraryPage.cpp:707-719`), A-Z, the rail over 30 rows. A tap opens the album; a hold, its sheet |
| Recently played | None | The top bar | Section rows ("Today", "Yesterday", "Sun 8 Nov" in `format.weekday_date`; "This session", "Earlier"), then rows "Lumen Ferry · 25 min ago" |

**The tray** (Most played; the root's segmented look, `src/ui/LibraryPage.cpp:334-363`):
- a CARD tray at (6, 7), 300 x 28, r10;
- three 96 px cells at x 8, 108, 208, or two of 148 px at x 8, 160;
- labels Bold centred on y 21 ("7 days", "30 days", "All time"), Small when Bold doesn't fit 92 (144 for two);
- the picked cell: a BTN_HI plate 96 x 24 r8 at y 9, its label TXT; the others DIM;
- a pressed cell: the tray's BG refilled and that cell alone lit, as the Filters row does its cross (`src/ui/LibraryPage.cpp:1120-1124`). Not `renderItem`'s whole-width ROW_SEL (`src/ui/ListView.cpp:430-433`).

| Counting days | Cells |
|---|---|
| Under 7, or no dates | No tray |
| 7-29 | "7 days", "All time" |
| 30 on | "7 days", "30 days", "All time" |

- A window kept for the shelf that isn't offered falls back to All time.
- RCNT capped (5.2) shortens the 30-day window: its cell then reads `stats.days_n` of the days it holds ("16 days").

**Ranked rows** (the album page's track recipe):
- rank "01", Small DIM, right-aligned at x 30, y 21 (`ListView::number()`, `src/ui/ListView.cpp:1405-1410`); Recently played leaves the column empty;
- title Body, x 40, y 13;
- line Small DIM, y 31, room 264: the count's part measured first, the artist cut to what is left, the count never cut;
- the playing row: ROW_SEL, the still violet EQ glyph in the rank's place (`:1412-1417`), the title Bold violet. `playing()` (`src/ui/LibraryPage.cpp:252-266`) learns the new kind, so `kShowPlaying` finds the row.

**Section rows** are drawn as Disc rows (`src/ui/LibraryPage.cpp:743-751`), Bold violet at x 12, y 25. They aren't pressable and have no hold.

**Behaviour:**
- A tap on a ranked row opens its inline bar (Play, Play next, + Queue). Play plays the shelf from that track.
- A hold (500 ms) gives a double tick and the track's sheet, "Auto DJ from here" first when it applies (`src/ui/LibraryPage.cpp:960-994`).
- The top bar acts on the whole list (`container()`, `pageTracks()`: `:160-217`), named by the shelf's title.
- A shelf with 0 rows (counting, or a window with nothing) has no top bar. Its one line draws under the tray: "Counting plays…" or "Nothing in this period.". Without the 7.2 change the line would never draw (`src/ui/ListView.cpp:484`).
- The rows are the page's snapshot, copied into a PSRAM buffer at `enter()` (as FilteredAlbums is: `lib/core/FilteredAlbums.h:11-25`), kept until the next `enter()`. A re-rank never moves rows under an open bar or a held sheet (`held_.index`: `src/ui/Pages.h:511-512`). A fold patches counts in place.
- The snapshot holds ids of the current index: dropped wherever `dropFilteredAlbums()` is called (`src/ui/Ui.cpp:731-767`).

**Disabled and early states** follow 4.4: the reasons are `stats.needs_dates` and `stats.warming` ("Opens 6 Dec, after a month of plays"; short form "Opens on 6 Dec").

### 7.7 Last 7 days (frame Week), a page of its own

`StatsWeekPage : Page`, modelled on DancePage (`src/ui/Pages.h:671-699`): `scrolls()` false, `hasHeader()` true. Its header copies `LibraryPage::onEvent()`'s header half (`src/ui/LibraryPage.cpp:497-539`) and `Header::hit()` (`src/ui/Ui.cpp:68-77`). Not a LibraryPage, so the status band never shows on it.

| y | Region |
|---|---|
| 36-71 | Header, depth 3: "Last 7 days" Bold; "4–10 Nov (UTC)" Small DIM beside it at x 140 when it fits 207, else under; the crumb |
| 72-121 | A: "Listening time" Small DIM at x 16, y 84; the stat value "7 h 37 min" on baseline y 106 (digits Title, units Small SOFT); at the right, "Plays" Small DIM over "118" Title, right-aligned at x 304 |
| 122-189 | B: 7 columns on a 41 px pitch from x 16; bars 24 wide on a 1 px DIV baseline at y 170; 44 px is the top day; past DIM, today violet; no bar for a day before counting; day names (3 letters, Small) centred on y 180, today's TXT |
| 190-239 | C: a 1 px DIV rule at y 193, x 16-304; line 1 Small SOFT at y 207, the big day ("Saturday was your big day: 2 h 1 min"); line 2 Small DIM at y 225, Auto DJ's rate, only when it started 5 or more tracks |

- **No targets** on the page body; back is "‹" or the crumb. The 41 px columns are narrower than the panel's skew past x 240.
- **Drawing:**
  - Three 56-row strips (`src/ui/Gfx.h:35`). The strip isn't kept between uses (`:43-44`), so the whole picture is drawn into each strip at its offset, as `drawEmptyState` does (`src/ui/EmptyState.cpp:68-80`).
  - "Listening time" (rows 76-92) and the value (85-111) share rows, and `Fonts::draw` fills its whole line (`src/ui/Fonts.h:77-84`). Both are drawn glyphs only (bg == fg, `src/ui/Fonts.cpp:585-589`) over a filled strip, the TitleLayout rule (`lib/core/TitleLayout.h:21-24`).
  - **The stat value:** `drawStat(canvas, x, baseline, align, pattern, args)` walks `tr(S::format_hours_minutes)`: placeholders in Title at baseline − 8, literal runs in Small at baseline − 5 (Polish "{0} godz. {1} min", `i18n/pl.json:946`). Its width function is in lib/core, so `test_ui_library` checks it in every language.
- Never reached without dates: the hub's row 1 is then the status row.

### 7.8 Now Playing's navigation sheet (frame NowPlayingSheet)

- `openNavMenu()` (`src/ui/NowPlayingPage.cpp:1039-1097`) opens 4 rows when `statsTrack()` has the facts and a target (the table below), else today's 3. The new row goes first, as "Auto DJ from here" did, so Go to artist, album and folder keep their places.
- With 4 rows the sheet's top is 240 − (36 + 160 + 4) = 40 (`lib/core/SheetLayout.h:25-39`). The new row is at screen y 76-115.
- Its label: "Go to Most played" (Body, 144 px) or "Go to Recently played".
- Its detail: "9 plays · last Sun · 1 skip" (Small DIM, 162 px). It doesn't fit the 128 px beside the label, so it goes under it (label y 13, detail y 31), the sheet's own rule (`src/ui/Overlays.cpp:416-420`).
- Buffers: a label 64 B, a detail 96 B (`src/ui/Overlays.h:241-242`). `@meta` sets `bytes` so `test_every_text_fits_its_buffers` catches a long translation.
- `onSheet()` (`src/ui/NowPlayingPage.cpp:1310-1320`) shifts choices by one when the row is up. Choice 0 calls `goToStats()`: `goToLibrary()`'s checks (`:1130-1147`), then the shelf at `kShowPlaying` (All time for Most played: its count is all-time).

| The track | Row 1 |
|---|---|
| Its row not read yet, or the lists not ranked since the boot | None (3 rows) |
| No counted play before this one | None. No inert row |
| In the All time top 50 | "Go to Most played" and its facts |
| Else in Recently played's 200 | "Go to Recently played" and its facts |
| Else | None |

- The detail: the plays (`stats.plays_n`); "last Sun" within 6 days, "today" and "yesterday", else "last 24 Oct" (UTC); the skips (`stats.skips_n`), left out at 0. No "last" part with no dated play.
- `kShowPlaying` opens at the track playing now, not at `navTrack_` (`src/ui/LibraryPage.cpp:298-310`). A track that changed while the sheet was up opens the shelf at the new one, or at its top.
- Also: the ✕ hit area runs from x 250; rows ignore taps in the first 300 ms; a tab tap closes the sheet as a cancel.

### 7.9 Hits and the touch skew

- x reads right on some panels: +20 px at x 190, +35-45 from 240 (`lib/core/FilterGrid.h:49-56`).
- Rows are hit across their whole width. Every new small target that reaches the right edge starts at least 40 px in.
- **The tray** splits at each gap's middle plus `filtergrid::skew()` (`lib/core/FilterGrid.h:105`: 0 under x 150, 6 under 190, else 13):
  - three cells: 106 + 0 = 106, and 206 + 13 = 219, so x 0-105, 106-218, 219-319;
  - two cells: 158 + 6 = 164;
  - a reading clamped at the edge is the last cell.
  The spec's 114 / 220 were drawn by eye. The host test requires each cell's centre to hit its cell on the lab panel and a true one, by at least 16 px, as the mood grid's.
- The top bar's thirds, the crumb (x 240 on), the sheet's ✕ (x 250 on) and the rail (x 280 on) are as today.

### 7.10 Redraw rules (no tap over 35 ms)

| Event | In its pass | Then |
|---|---|---|
| Push the hub or a shelf | The band: 35-50 ms idle, 80-100 ms with an MP3 (any Library push today) | — |
| Tap a tray cell | The tray via `refreshTopRow` (about 3.4 ms to render, 6 to push) and the header (about 5 ms) | `recountStaged`: one row a pass (about 11 ms each); at most 4 stale, since the tray is on screen. The scrollbar at rest |
| Push Week | The header and three 320 x 56 strips, about 36 ms | — |
| A fold while Week shows | Today's bar column and the two numbers through `gfx::pushRect` (`src/ui/Gfx.h:71`), 1-2 ms | Block C (one 320 x 18 strip, about 3 ms) only when its text changed |
| A fold while a list shows | That track's row, if on screen (about 11 ms) | — |
| The lists ranked or named | Rows whose text hash changed, on screen, one a pass | — |
| A new minute | Lines whose text changed ("25 min ago"), one a pass | — |
| A new UTC day | Week whole; the hub and entry row, staged | A re-rank |
| The NP sheet opens | 4 rows: 34-39 ms, as the playback sheet | — |

No animation. Covers come from the thumb cache. Nothing reads the card in `enter()` or on a tap.

### 7.11 Where the code goes

- **`lib/core/StatsView.{h,cpp}`** (pure, host-tested in `test_ui_stats`): the phase from its inputs; hub row kinds by state; which tray cells; `trayCellAt()`; Recently played's sections (as `librarytext::Discs`, `lib/core/LibraryText.h:120-140`); bar heights; the NP row's choice; `notPlayedOpen(countingDays, dated)` (O5; false while not dated).
- **`lib/core/StatsText.{h,cpp}`** (pure, host-tested): each line's form and its short forms; "artist cut, count kept"; relative times; day labels with `tzOffsetMin`; the stat value's segments and width.
- **`src/ui/LibraryStats.cpp`**: the stats half of `LibraryPage` (`LibraryPage.cpp` is 1,242 lines). New RowKinds `StatsShelf`, `StatsDisabled`, `StatsStatus`, `Ranked`, `Section` (`src/ui/Pages.h:457`); the branches of `rows`, `rowAt`, `topBar`, `topRow`, `drawRow`, `holds`, `pressable`, `onTap`, `header`, `emptyText`, `emptyState`, `onEmptyAction`, `update`, `enter`, `leave`, `describe`.
- **`src/ui/StatsWeekPage.cpp`**, and `drawStat` in `src/ui/Fonts.*`.
- **`src/ui/NowPlayingPage.cpp`**: `openNavMenu`, `onSheet`, `goToStats`; members beside `navTrack_` (`src/ui/Pages.h:327-330`): `navStats_`, `navShelf_`, `navStatsDetail_[96]`.
- **`src/ui/UiHost.h`:** `AppState` gains a `statsview::State stats` (phase, dated, countingDays, sinceDay, version, totals, the 7 days), as `autodjview::State autoDj` (`:145`). Host calls, RAM only: `statsShelf(shelf, window, out)`, `statsHub(out)`, `statsWeek(out)`, `statsTrack(out)`.
- **6 icons** (trophy, spark, heart, clock, chart, ring-play) made by `tools/ui_icons.py` into `src/ui/IconData.cpp`, declared in `src/ui/Icons.h:18-52`.

## 8. Strings

The rule: the phone app's ARB words where it has them (`i18n/README.md`, "Words"). A text a translation leaves out is English on the device, so a commit with English alone stays green.

### 8.1 From the ARB: no translation owed, but short forms

Phone app `lib/l10n/app_<lang>.arb` at d17c3fa5. All ten languages have the keys.

| Id | English | Room | ARB key:line (en) | Owed short forms | Stage |
|---|---|---|---|---|---|
| stats.title | Listening stats | Body 204 (entry), Bold 282 (header) | listeningTitle:4397 ("Listening Stats"; the house writes sentence case) | ru (234 px in Body 204) | S3 |
| stats.window_all | All time | Bold 92, else Small | listeningPeriodAll:4465 | es, fr, pt (98, 102, 97 px even in Small); it and ru take the Small fallback | S3 |
| stats.days_n (plural) | {0} days | Bold 92, else Small (tray) | listeningDays:4493 | | S3 |
| stats.plays_n (plural) | {0} plays | Small, inside 264 | listeningPlays:4549 | | S3 |
| stats.recent | Recently played | Body 232, Bold 179 | autoDjRecentlyPlayed:5912 | es, fr, pl, pt, ru (249, 180, 190, 254, 225 px in Bold 179) | S3 |
| stats.empty_title | No plays yet | Title 304 | listeningEmptyTitle:4565 | de, es, fr, it, pt, ru (314-369 px) | S3 |
| stats.today | Today | Bold (section) | listeningToday:4625 | | S3 |
| stats.yesterday | Yesterday | Bold (section) | listeningYesterday:4629 | | S3 |
| stats.plays_label | Plays | Small 100 | listeningTilePlays:4469 | es, ru (103, 110 px) | S3 |
| stats.listened | Listening time | Small 150 | listeningTileTime:4473 | ru (156 px) | S3 |
| stats.shelf_empty | Nothing in this period. | Body 300 (`kListLineW`) | listeningEmptyPeriod:4577 | | S3 |

ARB words keep their translations, with fullwidth punctuation changed to ASCII (`i18n/README.md`, "Characters"). `THIRD-PARTY-NOTICES.md:467` already names d17c3fa5.

The widths are the firmware's VLW fonts (`src/ui/VlwFonts.cpp`: Body Sans 16, Small Sans 13, Bold Sans Bold 16, Title Sans Bold 22, the glyphs' advances summed: MEASURED-HOST). A text that doesn't fit its room fails the check (`i18n/README.md:80-81`). The 18 short forms are written in C19 (drafted, reviewed, fit-passed), each noted in its file's `@@note` as departing from the ARB. Until then the id stays English in that language (C18).

### 8.2 New: translation owed

| Id | English | Room, buffer | Stage |
|---|---|---|---|
| stats.entry_sub | {0} in the last 7 days | Small 204 | S3 |
| stats.entry_sub_short | {0} · 7 days | Small 204, optional | S3 |
| stats.entry_empty | Plays show up as you listen | Small 204 | S3 |
| stats.counting | Counting plays… | Small 232; Body 300 as a list's line | S3 |
| stats.since | {0} since {1} | Small 282 (header sub), 232 (a line) | S3 |
| stats.since_only | since {0} | Small 179 | S3 |
| stats.last7 | Last 7 days | Body 232, Bold 106 (header) | S3 |
| stats.sub_last7 | last 7 days | Small 179 | S3 |
| stats.sub_last30 | last 30 days | Small 179 | S3 |
| stats.time_plays | {0} · {1} | Small 232 | S3 |
| stats.most_played | Most played | Body 232, Bold 179 | S3 |
| stats.new_finds | New finds | Body 232, Bold 179 | S3 |
| stats.new_finds_sub (plural) | {0} tracks first heard in 30 days | Small 232 | S3 |
| stats.new_finds_sub_short (plural) | {0} new in 30 days | Small 232, optional | S3 |
| stats.first_heard | first heard in 30 days | Small 179 | S3 |
| stats.found_autodj | found by Auto DJ | Small, inside 264 | S3 |
| stats.found_shuffle | found by shuffle | Small, inside 264 | S3 |
| stats.old_favourites | Old favourites | Body 232, Bold 179 | S3 |
| stats.old_favourites_sub (plural) | {0} tracks, played often, not lately | Small 232 | S3 |
| stats.not_in_30 | not in 30 days | Small 179 | S3 |
| stats.not_played | Not played yet | Body 232, Bold 179 | S3 |
| stats.ago_min | {0} min ago | Small | S3 |
| stats.ago_h | {0} h ago | Small | S3 |
| stats.last_on | last {0} | Small | S3 |
| stats.today_lc | today | Small (inside a line) | S4 |
| stats.yesterday_lc | yesterday | Small (inside a line) | S4 |
| stats.this_session | This session | Bold (section) | S3 |
| stats.earlier | Earlier | Bold (section) | S3 |
| stats.big_day | {0} was your big day: {1} | Small 288; {0} a full day name, reordered as the language wants | S3 |
| stats.autodj_right | Auto DJ got it right {0} of the time | Small 288; {0} `format.percent` | S3 |
| stats.no_dates | No dates yet: the clock isn't set | Small 250 | S3 |
| stats.clock_fix | Plug in to the MP3 Player tab | Small 250, Body 284 (toast) | S3 |
| stats.needs_dates | Needs dates | Small 250 | S3 |
| stats.warming | Opens {0}, after a month of plays | Small 250 | S3 |
| stats.warming_short | Opens on {0} | Small 250, Body 284 | S3 |
| toast.stats_needs_dates | Needs dates: {0} | toast, 128 B ({0} `stats.clock_fix`) | S3 |
| toast.stats_needs_month | Needs a month of plays: {0} | toast, 128 B ({0} `stats.warming_short`) | S3 |
| stats.empty_rule | Plays count from 30 s or half a track | Small 304 (`kEmptyLineW`) | S3 |
| stats.empty_rule_short | Plays count from 30 s | Small 304, optional (de is at its room: 303 px) | S3 |
| stats.empty_card | Your stats stay on this card | Small 304 | S3 |
| stats.no_card | No card: plays can't be counted | Body 300 | S3 |
| stats.utc | {0} (UTC) | Small 179 | S3 |
| stats.go_most_played | Go to Most played | Body 288, 64 B | S4 |
| stats.go_recent | Go to Recently played | Body 288, 64 B (Cyrillic is close) | S4 |
| stats.skips_n (plural) | {0} skips | Small, inside 96 B | S4 |
| format.day_month | {0} {1} | Small; day number and short month, ordered as the language wants | S3 |
| format.weekday_date | {0} {1} | Bold (section); {0} `day.*` short, {1} `format.day_month`, ordered as the language wants (ja and zh put the weekday last) | S3 |
| format.day_range | {0}–{1} | Small 179 | S3 |
| day.mon … day.sun | Mon … Sun | Small 41 (a column), 7 ids | S3 |
| day.monday … day.sunday | Monday … Sunday | Small, inside 288 (`stats.big_day`), 7 ids | S3 |
| month.jan … month.dec | Jan … Dec | Small, 12 ids | S3 |

**Counts:** 11 ARB ids and 74 new ones (48 messages and 26 day and month names): 85 ids. In nine languages, 666 texts are owed for the new ids, plus the 18 short forms of ARB words (8.1): 684, machine-made, with a native review owed. The spec's own count was "about 55 messages + 26 date names" with the album line's.

**Reused, no new text:**
- `format.hours_minutes` (`i18n/en.json:8254-8283`): new rooms (the entry line, the hub's sub, Week's value), and its worst case raised from 99 hours (`:8274`) to 99,999, re-measuring Device info's and the Queue's rooms;
- `format.minutes` (`:8199`), `format.percent` (`:8167`), `empty.shuffle_all` (`:3559`), `toast.no_memory` (`:274`), `library.albums_n`.

**Dropped with O3:** `stats.not_yet`, `stats.not_yet_since`.

**Rules that bite here:**
- A toast splits at its first ": " only when too wide for one Body line (`src/ui/Overlays.cpp:101-107`).
- No fullwidth punctuation in ja and zh.
- Plurals by CLDR (`i18n/README.md`, "Plurals").
- `stats.window_all`: es, fr, pt need short words; it and ru take the Small fallback (8.1). pl's ARB "Cały czas" fits Bold 92 (84 px); the spec measured "Cały okres".

**CJK.** The words (再生, 回, 時間, 分, 今日, 昨日, 月, 日, 播放, 次, 小时, 分钟, 周一-周日, 统计, 跳过) are in JIS X 0208 and GB2312 level 1, the built-in store (`lib/core/CjkGlyphs16.cpp:1-11`): no new glyphs expected. `python tools/gen_ui_strings.py --check` and `test_every_text_has_its_glyphs` decide. Regenerating the store needs the Noto Sans CJK font, which isn't in the repository (`tools/glyph_store.py:85-90`).

**Who translates:** a Fable agent drafts each language and a second reviews it, then the fit pass (`docs/I18N.md:1479-1503`). "The MP3 Player tab" uses the terminal's own name for that tab in each language (terminal `locales/<lang>.yml`). Each file's `@@note` gains a sentence on these texts.

## 9. The stages

### 9.1 Rules, and the dependency diagram

- Host tests stay green at every commit: `pio test -e native`, run from Git Bash on this Windows box (from PowerShell it dies with the MSYS2 DLL clash, 0xC0000139). Builds: `pio run -e core2 -e core2-dio` from PowerShell with MSYSTEM unset.
- A stage stops only at a PR boundary.
- **Device checks run on the spare 32 GB card only, until S5a.** Nothing is played or written on the owner's 1 TB music card before S5a, and S5 needs the owner's word. After S1 merges, the owner's music card boots no main build until S5a passes (the owner flashes main's images onto the board: `docs/RELEASE-0.14.0-CHECKS.md:116-124`), and no release check uses it (`:26`). Silent mode `z` after every boot, before anything plays; never `B`, `P`, `c` or `f` (`docs/RELEASE-0.14.0-CHECKS.md:22-27`). Since `z` keeps logging (O6), every console run adds plays to the card's stats: the spare's stats are test data.
- **The spare card is empty.** 0.14.0's checks formatted it (`docs/RELEASE-0.14.0-CHECKS.md:63-64`) and it is in the PC's reader. S1's device pass first rebuilds it with `tools/synthcard.py` and `tools/synthtransfer.py`, with the owner's go-ahead.
- **Settings are put back** at each device pass's end as `docs/RELEASE-0.14.0-CHECKS.md:110-131` does. S1's seq rule (5.6) keeps that safe for the spare's stats.
- **Fixtures flow one way.** `tools/statsref.py` is the reference. `tools/card_fixtures.py` makes every file in `test/fixtures/card`, and its `--check` fails on any file it didn't make.
- Feature branches never edit `.github/release-notes.md`; 9.2 keeps the lines owed.
- Every stage ends with a review. Its fixes are their own commit, "<Area>: what the review found".

```
S0 --> S1 --> S2 --> S3 --> S4
        |      |      |
        |      |      +--> S6 (the time zone; its terminal half needs nothing here)
        |      +---------> S5b (the owner's card: the lists, after S2)
        +----------------> S5a (the owner's card: the fold, the save, the trim; before any build with S1 boots it)
```

### 9.2 Where it can stop

| Stop after | The listener sees | Useful as |
|---|---|---|
| S0 | Nothing | The contract S1-S4 build to |
| S1 | Nothing on the screen. `/.player/stats.bin` on the card | **Counting starts.** Ship it alone and the history is there when S3 lands. The format is frozen from its first release |
| S2 | Nothing: console only | The lists, by name, on the console |
| S3 | The entry row, the hub, the shelves, Last 7 days | The feature, but the NP sheet's row |
| S4 | The NP sheet's row | The whole design |
| S5a, S5b | — | Checked on the owner's own card across a real sync (S5a gates any release or main build with S1 on that card) |
| S6 | Local days | O4's later half |

**The shortest useful path is S0, S1, S2, S3.**

**Owed to the release commit** (not written into `.github/release-notes.md` by any stage):
- S1:
  - "The player now keeps listening stats on the card (`/.player/stats.bin`): every play on the card, from every player that wrote it, counted from the play log. Nothing on the screen yet."
  - "The log's files are trimmed after a sync only once the stats hold their plays."
  - The downgrade line: "Going back to 0.14.0: it trims a synced card's log without counting those plays, so the stats miss them when the card comes back."
  - The first boot's cost: one fold of the log and one save (S1's device figures).
- S3:
  - The screens in a paragraph: the entry row, the hub, the shelves, Last 7 days; Most played opens on All time; days are UTC for now; the first month's states.
  - The strings' provenance: the ARB words at d17c3fa5 in all ten languages, but 18 short forms where they didn't fit (8.1); the rest machine-made in nine, native review owed.
- S4: the NP sheet's first row.
- S6: local days from the computer's time zone; NVS schema 7, nothing to do on the update; going back reads it as nothing (UTC).

### 9.3 Console, one list

| Command | Stage | Does |
|---|---|---|
| `H` | S1 | The state: phase; `stats.bin`'s bytes, saves and minor; HEAD's totals; since and counting days; the rows of each section and their bytes; RAM; the last save (why, ms, bytes) |
| `Hd` | S1 | The last 7 UTC days: time listened, plays, skips; DAYS by source |
| `Hm` | S1 | MARK's rows beside ACKS's and the log's highest seq per (deviceId, epoch), from the boot's scan; HEAD's twice and gap |
| `Ht<16 hex>` | S1 | A track's row by path hash (base ⊕ delta), read on the loop (console only) |
| `Hs` | S1 | Save now, the rotation's path, after the WriteGate as the real saves. Refused during the boot's scan, behind the fence, with the card away, after a partial scan or a changed `stats.bin` (5.5) |
| `Hr` | S2 | Rank now (the worker): µs, rows read, the lists' sizes |
| `Hl<shelf>[w]` | S2 | A list: hash, id, count, title. Shelf 0-4; window 0-2 |
| `Hw` | S2 | Walk now: passes, µs, played hashes matched and not |
| `Hp` | S2 | The playing track's facts (base ⊕ delta, the read's ms) |
| `Hp`, more | S4 | Also the NP row's choice (`statsview`'s, built in S3) |
| `uiS` | S3 | The stats view as the UI reads it |
| `uiSh`, `uiSw`, `uiS<shelf>[w]` | S3 | Open the hub, Week, a shelf at a window |
| `uiSx<n>` | S3 | A made-up stats view, look only (as `uil<n>`, `src/main.cpp:1505-1513`): 0 the card's own, 1 a full card, 2 nothing played, 3 no clock, 4 day 5 of counting, 5 folding, 6 no card |

- `H` is free at the top level (`src/app/SerialConsole.cpp:226-263`). `E` belongs to the ESP-NOW spike's branch, and `g`'s sub-letters overlap PR #25's `gx`.
- `uiS` (capital) is not `uis` (the scripted swipe: `src/main.cpp:1525-1531`), and Ui's own letters are r, B, v, f, b, c, L, P, T, M (`src/ui/Ui.cpp:2939-3030`).

### 9.4 S0: the contract

No firmware change, no device step.

- **C0: this document.**
- **C1: the frames.** PNG captures (320 x 240) of the nine final frames from the canvas, into `docs/img/listening-stats/`, with a README naming each as this plan does and saying they are browser-font renders. Not AlbumPlays, and none of the A-, B- or C- frames.
- **C2: the format text.**
  - `docs/METADATA.md` 2.12.6: a bullet for `stats.bin` (the queue's commit, the twins `stats.xl1`-`.xl9`, the only-tmp rule with a real check) and the base file (a fresh name, named by `stats.bin`).
  - 2.11's `/.player` row (`:1790`): deleting it loses the stats' history the trims removed; the software never reads the stats files.
  - 2.20.2 "Trimming by ACKS" (`:3034`): a device that keeps stats trims a file only once its stats hold its plays.
  - 2.20.4's "At boot" sentence (`:3112-3122`): the next seq also takes ACKS's highest seq for this device and epoch, and a device that keeps stats its stats' highest (MARK's): a reservation, which the same item allows.
  - 10.1's list (`:8595`).
  - The format itself is this document's 5.2-5.3: no software reads it.
- **C3: the reference and the fixtures.**
  - `tools/statsref.py` (stdlib, imports `tools/playlog.py`): the rules of 4.1, the fold of 5.6 with its dedupe, `stats.bin` and the base byte for byte, ⊕, the rankers of 4.3, the windows, and a `seed` command (writes `plays.jnl` and `plays.1.jnl` of a history: N days of dated plays over a card's `/music` listing, another deviceId, a full log; with `--base <rows>`, also a `stats.bin` and its base of that many rows; for S1-S3's device passes).
  - As built: `statsref.py dump` reads a `stats.bin` (and its base: by default the `stats-<gen>.bin` beside it; with none found, HEAD as read, never a lost base) as the device does; `statsref.py boot DIR --now --acks` runs a boot over a copy of a card's `/.player` (nothing written) and prints what it folds, saves and trims, HEAD, MARK and the lists: the expected values of S1-S3's device passes. `seed CARD` takes `--days` (40) and `--per-day` (25), or `--slots N`, or `--full` (13,106: both files at the cap; `--slots 13102 --device <id>` is run G's log, 4 slots under the cap), `--device`, `--epoch`, `--until`, `--distinct` (5,000), `--base ROWS` (`--base-days`, 365), `--seed`, `--dry-run`, and `--replace` (log files, and with `--base` stats files, already there are refused without it; a twin always is: the log's, `plays.xl1`-`.xl9`, and with `--base` `stats.bin`'s, `stats.xl1`-`.xl9`). The container's frame is `tools/mpwf.py`'s writer, the same bytes as `card_fixtures.build_container`'s.
  - `seed` refuses a card with no `SYNTHCARD.TXT` at its root, or whose `/.mstream` root `tools/synthtransfer.py` didn't write (its root's producer: `tools/synthtransfer.py:26-39`), and, when CARD is a drive's root, the system drive and any file system but FAT32/FAT16 (synthtransfer's `check_drive`, `:1365`); a synthcard tree in a folder, on C: too, passes, as with synthtransfer. It never touches a card without both: seeded plays on the owner's card would be posted to the owner's mStream at the next sync. `--dry-run` lists what it would write.
  - `tools/card_fixtures.py`'s `generate()` writes `test/fixtures/card/stats/`: scenarios (a log, a MARK, ACKS) and their expected `stats.bin`, base and lists. The worked example (5.13) is one. Every verdict of 5.4 has one (`newer`, `corrupt`, the base `base-lost`, `base-other`, `base-unread`, `base-short` and `base-neighbours`), as do 5.12's newer minor (`newer-minor`), a clock a year ahead then corrected (`clock-ahead`) and C8's three partial scans (`partial-open`, `partial-read`, `partial-tail`; the third's live play and `Hs` are a unit test). The cuts of C8 are not modelled: a cut leaves the old commit or the new one, which these files give.
  - `tools/test_statsref.py`, added to `.github/workflows/firmware.yml`'s "Card fixtures" step (`:115-121`), or it never runs in CI.

**Done when:**
```
python tools/card_fixtures.py --check
python -m unittest discover -s tools -p "test_statsref.py"
python -m unittest discover -s tools -p "test_playlog.py"
pio test -e native
```
`git diff --name-status main -- test/fixtures/card` shows only A under `stats/`, and M for the folder's README.

**Also at S0:** the host baseline on this box (cases and failures on clean main) and `pio run -e core2`'s flash, RAM and IRAM at bc29b54, with the link map's `libc_a-*` objects in `.iram0.text`. Every later stage compares against these.

**PR:** one, C0-C3. **Effort:** 1-1.5 (guess).

### 9.5 S1: the store

Needs S0. Console only.

- **C4: the play log's hooks** (`lib/core/PlayLog.{h,cpp}`).
  - `playlog::Folds`, an interface: `scanBegan(again)`, `slot(s, file)`, `partial()`, `covered(s)` (at or under the card's MARK), `markOf(dev, epoch)`, `save(Why)` (Why: Boot, Rotation). `Journal::Config::folds`, null by default (`lib/core/PlayLog.h:147-159`).
  - `startScan()`: `scanBegan`; the new-epoch shortcut (`:162-164`) only without folds.
  - `scan()`: each log file opened with `openRead(name, &cardFailed)`; an open the card failed, or a read that failed (`:302-306`), calls `Folds::partial()` (5.6), and the journal goes on as today. `slot()` after `:285`; `FileScan::allFolded` beside `allAcked` (`:292-297`), and a `foldedBefore_` mask beside `trimmable_` (`:310-311`).
  - `finishScan()`: `save(Boot)` after the boot rules and before the trim loop, and on the `storeBoots` failure path (`:322-326`); then `trimmable_ &= saved ? all : foldedBefore_`. After a partial scan no save runs this session: the boot's, a rotation's and `Hs` all return false.
  - `rotate()`: `unread()` also notes a play not `covered()`, and (in a session whose scan was whole) a read that left the file not read whole; if either, `save(Rotation)` after `unread()` (`:423-433`) and before the `shared()` question (`:434`), so it covers the twin rename (`:439-441`) as well as the delete (`:437`). The result ignored.
  - `Outbox::Env::written()`, empty by default, called in `done()` before `pop()` when Written (`:641`).
- **C5: the seq rule** (`:327-328`), with MARK's and ACKS's rows (5.6). Its own commit: it changes the play log for everyone.
  - `tools/playlog.py`'s `boot_seq()` (`:244-248`) and `Device.boot` (`:531-545`) take ACKS's row for this device and epoch, and MARK's when given (through statsref). `tools/test_playlog.py` gains the case.
  - `card_fixtures.py`'s `atBoot` gains a trimmed log with NVS behind ACKS (the NVS rollback), so `test_the_boot_rules`' count goes from 5 to 6 (`test/test_play_log/test_play_log.cpp:454-455`).
  - METADATA 2.20.4's "At boot" sentence (C2).
- **C6: `lib/core/StatsFold.{h,cpp}`.** The rules of 4.1; hours and days; the aggregate in RAM (HEAD, MARK, HOUR, DAYS, RCNT, TRKD) in the chunks of 5.6, with an allocator hook; the boot fold's index; ⊕ (order-free: 5.2); the boot's fold above the boot MARK with the ranges dedupe; the live fold; rounds; the retention rules (HOUR's 840 rows, DAYS' 400, RCNT's 32 days from its reference and 4,096 rows: 5.2); MARK kept for good; sinceDay. No IRAM attribute, no per-slot memmove. No libc object that the framework's `sections.ld` pins in IRAM and `iram_diet` leaves there, unless 0.14.0 already links it (11).
- **C7: `lib/core/StatsFile.{h,cpp}`.** `kStatsNames`; `StatsCheck` (a TmpCheck: Whole for Major or UnknownRequired, Unknown for a read the card fails or no memory, Torn only for a frame that fails its own checks: 5.4); the read and its verdicts (5.4), each section's CRC taken as it loads, with the boot's retries through a wait hook and none at a remount; `stats.bad`; the base check (a card fault is Unread, never a lost base) and orphans; the commit (5.5), with the held tmp promoted first and the check of the card's copy; compaction, its base created with `FA_CREATE_ALWAYS` (a stale file under its name is replaced: 5.3), never under the name `stats.bin` names; the writer in caller-given (PSRAM) memory.
- **C8: the proof** (new `test_stats_store`). The Journal with the real fold and file over `test/support/CutFs.h:42`, `FlakyFs` (`test/test_play_log/test_play_log.cpp:1700`) and `FaultFs` (`:1985`). Each scenario runs fold, save, trim and reboot, and compares with `tools/statsref.py`'s fixture: no loss, no double count. Cuts and faults at:
  - every write step of the save (tmp, close, the size check, `replace()`'s remove and rename); a held tmp promoted at the next save, and one dropped by a reboot's `settle()` while `stats.bin` is there;
  - compaction's three steps; a compaction over a stale `stats-00000001.bin` (after a `stats.bin` deleted or corrupt, or, at the boot after, a base lost: replaced, nothing removed; the boot that finds base 1 lost doesn't compact under the name `stats.bin` names: 5.3);
  - the trim;
  - a remount mid-scan, and one after Ready before the first slot;
  - a partial scan: `plays.1.jnl`'s open faulted while `plays.jnl` reads; a read fault midway through `plays.1.jnl`; a read fault in `plays.jnl`'s tail followed by `Hs`. Each: no save, then a whole boot folds every slot once (statsref's `partial-*` fixtures);
  - the base: an open fault, and a header read fault, at the boot (Unread, no base lost, no file removed);
  - a second Journal saving the card (with a compaction) between two saves of the first: the first's next save refused;
  - a rotation over an unsaved `plays.1.jnl`, with the save failing then a reboot (an undercount: asserted exactly); the same over a `plays.1.jnl` that shares a twin's chain (the save runs before the twin rename);
  - a rotation inside `flush()` over an unsaved `plays.1.jnl`, then a reboot: with 1 s of the budget left (saved: nothing lost), and with less (the exact count lost);
  - NVS rolled back after a trim;
  - a new epoch with no ACKS;
  - two devices alternating;
  - a 0.14.0 trim between (HEAD's gap);
  - firstAt: dated then undated; undated then dated; two undated plays with different sources, folded in both orders and through a rotation's save (the rows the same but lastOrd, compared by order);
  - retention: a save with the clock unset; a foreign slot dated 2099; today a year ahead at one save, then corrected (DAYS and HOUR untouched);
  - a full log never synced (13,106 slots): TRKD's rows equal the distinct hashes; the peak bytes and the µs a slot, printed.

  `test_autodj_join`'s heap model gains `StatsRun`'s hash-keyed half (6.7: the load's chunks after Auto DJ's glue, the boot fold's growth to the ceiling after the boot's `joinNow()`), with the mood plan's pass rule. `test_stats_file` gains: a lone major-2 `stats.tmp` survives `settle()` (promoted, then left alone as Newer).

  As built: `test/test_stats_store` drives the Journal, the Tracker and the Outbox with a `Folds` over the real Fold and Store, shaped as C9's glue (the read before `begin()`, the save's guards, the end's flush, the live fold at `written()`, Hs). `FlakyFs` and `FaultFs` moved to `test/support/FaultFs.h` (test_play_log uses them from there). Every boot of `stats/index.json` runs through it, chained, and so does each step of `stats/sessions.json`, a new set of statsref fixtures (`statsref.session_files()`: players' boots and plays on one card, the log as `playlog.Device` writes it, the stats as 5.5-5.8 keep them; 15 sessions, 154 steps), the card compared byte for byte after each step: the rotations unsaved (`rotation-*`, `flush-*`, `partial-rotation`: 4 slots lost where the plan says, none elsewhere), a second player (`second-player`), NVS put back (`nvs-back`; and `log-deleted`, where MARK alone holds the seq: both rest on C5), two players alternating, a 0.14.0 boot (`old-firmware`: HEAD's gap 3), a new epoch, firstAt through a rotation's save (`first-at-live`). The held tmp, the base faults and the cuts run on the index's boots (the cuts on ten of them, three ways, through the trim too; each question and each step the card fails, in turn, on four). The full log through the journal: 13,106 slots, 4,616 hashes, the fold's peak 396,608 B, 1.55 µs a slot on the host (the scan, the fold and the save). The heap model's finding is 6.7's.
- **C9: the glue.**
  - `src/app/ListeningStats.{h,cpp}`: the read at `PlayHistory::begin()` (`src/app/PlayHistory.cpp:97-142`) before `journal_->begin()`, with the load's chunks; the `Folds`; the save's guards (5.5), the card's copy checked first; the ending flag (`:307`: the boot's save only; a rotation in the flush saves with 1 s of its budget left); the read again in `remounted()` (`:324-333`): one read, no waits, none while ending (5.4); `written()` (hash-keyed; no id); `[listen]` lines.
  - `cardroot::Root` keeps commitTime (`lib/core/CardRoot.cpp:111-114`; `test_card_root`).
  - Console `H`, `Hd`, `Hm`, `Ht`, `Hs`.
  - `tools/synthtransfer.py --acks <dev>:<epoch>:<seq>[,...]`, through `card_fixtures.msmf_raw(acks=)` (`tools/card_fixtures.py:604`), for the trim's checks. `--acks` goes through `add_to`'s refusals unchanged (`tools/synthtransfer.py:1386`: only a root this tool wrote, with `--replace`).
  - The commit message gives `-fstack-usage`'s sums for `writeOne` → `rotate` → save and for `scan` → `finishScan` → save.
- **C10: docs.**
  - `docs/ARCHITECTURE.md`: the Layers diagram (`:44`, `:70`), Storage (`:1411-1430`), the play log's paragraph (`:1432-1460`), a Listening stats bullet (`:2654-2667`).
  - `docs/METADATA.md` 3.11 "As built: the listening stats", and 3.10's "not flashed" (`:7233`) corrected.
  - `README.md`'s console table.

**Tests:**
- New: `test_stats_fold`, `test_stats_file`, `test_stats_store`.
- In `test_play_log`: a new epoch read with folds; the trim waits for the folds; a failed save trims what the card covers; a rotation saves first; the end's flush never makes the boot's save, and makes a rotation's with 1 s left; a partial scan calls `partial()`; the seq rule's two sources.
- `test_card_root`: commitTime kept.
- `test_autodj_join`: `StatsRun`'s hash-keyed half (C8).

**Control, must pass unchanged:** `test_play_log`'s 33 (no `Folds`; `atBoot`'s count from 5 to 6 by C5), `test_tag_store`, `test_card_root`'s others, `test_card_files`, `test_queue`, `test_autodj_join`'s other cases.

**Done when:**
```
pio test -e native -f test_play_log -f test_stats_fold -f test_stats_file -f test_stats_store -f test_card_root -f test_autodj_join
python tools/card_fixtures.py --check
python -m unittest discover -s tools -p "test_statsref.py"
python -m unittest discover -s tools -p "test_playlog.py"
python -m unittest discover -s tools -p "test_synthtransfer.py"
pio test -e native
pio run -e core2 -e core2-dio
```
The link map's `libc_a-*` objects in `.iram0.text` equal S0's list, and IRAM left is S0's figure (11). `iram_diet: 51 of 51` alone can't catch a new pinned object.

**Device**, the spare card rebuilt (`synthcard`, `synthtransfer`), silent mode. Set-up: after synthcard and synthtransfer, `statsref.py seed` 40 days of another deviceId's plays, before the first boot (sinceDay is set once, at a fresh `stats.bin`'s first save: 4.1). Controls are main at bc29b54 in the same script.

| Run | Do | Must show |
|---|---|---|
| A | Flash, boot, `z`, `H` | `stats.bin` absent: a fresh start; the seed folded; the boot's fold and save lines, with ms and bytes; `[plays] ready` as before |
| B | Play 3 tracks past 30 s, skip one at 5 s, stop one at 10 s; `H`, `Hd` | plays 3, skips 1, time listened the sum; today's HOUR; no card write but the slots |
| C | A reset, `z`, `H` | The 5 slots folded once; B's figures again; a save, then nothing more at the next reset |
| D | Run A's boot, read back: `H`, `Hm`, `Hd` | The seed folded once (again after a reset); sinceDay the seed's first day; after B, two MARK rows; RCNT capped right |
| E | Two tracks past 30 s in silent mode, then the card to the PC; `synthtransfer --acks` covering both devices; boot | "[listen] saved" before "[plays] … trimmed"; the next boot folds and trims nothing |
| F | Two tracks past 30 s in silent mode, then the card to the PC; fill the card (a large file from the PC); boot | The save fails; only files the old MARK covers go; freed, the next boot saves and trims |
| G | A throwaway build of S1 with the boot's save compiled out (a `-D` flag, never merged). `statsref.py seed` writes a `plays.1.jnl` of a third deviceId and a `plays.jnl` 4 slots under the cap. Boot, `z`, play 5 tracks past 30 s | `[listen] saved (rotation)`, with its ms, before the rotation's delete of `plays.1.jnl`. Then S1's own build, a reset, `z`: `H` equals statsref's figures over the card's `stats.bin` before G, the seed and the 5 plays (none lost, none twice) |
| H | `Hs` idle, with an MP3 (`qp`), and with the owner's headphones if they link them. `Hs` waits for the WriteGate, as the real saves do | The save's ms; `s`'s underruns and breaks around it; no `[ui] slow pass` from a tap |
| I | The boot with a full seeded log (13,106 slots, about 5,000 hashes); then with an MP3 and with a FLAC playing as the scan ends, on a seeded base of about 19,000 rows (`statsref.py seed --base 19000`) | The fold's RAM (`H`), PSRAM free and largest (`D`), the scan's passes, against main at bc29b54 in the same script; the save's and the compaction's ms on the `[listen]` line; `s`'s underruns= and breaks=; `[ui] slow pass` |
| J | `gc2` (the sector cache's verify mode, `src/app/TagConsole.h:46-49`), run B's plays (3 past 30 s, a skip, a stop), then `Hs`; `gc1` after, back to the normal cache | The tmp's size check holds with the cache verifying; `gc` counts no mismatch |
| K | The loop stack's lowest at G's rotation save, and at a boot's save with DO low (METADATA 3.8's fault switch) | Each compared with 2,548 B, the house's lowest (`docs/METADATA.md:6322`) |
| L | `gb` stopped, then an MP3, then a FLAC, then an Opus; `D`, `Dc` | Against main at bc29b54 in the same script: no `[heap] FAILED`; joined at or over 245,764 B where main is, and within 122,880 B of it (6.7); the Opus plays with 0 skipped |

The failed rotation save (an undercount) is proved on the host only (C8). Then the settings put back (9.1).

**PR:** one, C4-C10. **Effort:** 5-7.5 (guess; C8 is 1.5-2.5, the device pass 0.75-1.25).

### 9.6 S2: the map and the rankers

Needs S1. Console only.

- **C11: `lib/core/PathHashWalk.{h,cpp}`**, from `lib/core/AutoDjJoin.cpp:584-647`; the join uses it. No change in the join's output: `test_autodj_join` passes unchanged, and every hash equals `trackPathHash()`.
- **C12: `lib/core/StatsRank.{h,cpp}`:** 4.3's lists over a sorted base stream ⊕ the delta and RCNT; the played set in chunks; the sparse index; the cap plus 16. `test_stats_rank` against `statsref`'s lists.
- **C13: `lib/core/StatsMap.{h,cpp}`:** the walk's consumer (album bits, hash → id for the wanted set, in 16 KB chunks), its abort and restart, ids u32. `test_stats_map`. The heap model's `StatsRun` (6.7) gains its id-keyed half, the rank's blocks and the walk's, in `test_autodj_join`, at P = 2,214 and P = 19,478.
- **C14: the firmware.**
  - `Job::Stats` (`lib/core/ScanScheduler.h:101-102`) in its two lanes, Track beside Wave and Rank as background work (6.8); `test_scan_scheduler`: a rank never starts under `updating` or any yield, never before a Wave or a Cover, and gets priority 0 while lit.
  - The rank's gate against Auto DJ's join, and the join's new `Block` reason while a rank or walk holds blocks (6.8); the loop carves the rank's blocks before `worker.start()`.
  - `stepStatsMap(now)` after `stepAutoDj` (`src/main.cpp:4046`), with its gates and the idle wait (`:4255`).
  - `enterFence()` drops the id-keyed blocks and adds their bytes to `alsoFreed` (`:3414-3417`, `:3570`); `leaveFence()` ranks and walks again (`:3460-3467`).
  - The live fold's `findTrack()` id for the lists and the map (5.6).
  - Console `Hr`, `Hl`, `Hw`, `Hp`.
- **C15: docs:** ARCHITECTURE's Auto DJ bullet (the shared walk) and Listening stats bullet (the rankers, `Job::Stats`, the map); `docs/AUTODJ.md` 7.4's memory if the join's blocks moved; README's console table (`Hr`, `Hl`, `Hw`, `Hp`).

**Tests:** new `test_stats_rank`, `test_stats_map`; `test_autodj_join` (unchanged, plus `StatsRun`'s second half); `test_scan_scheduler` (the ninth job and its lanes).

**Done when:**
```
pio test -e native -f test_autodj_join -f test_stats_rank -f test_stats_map -f test_scan_scheduler
pio test -e native
pio run -e core2 -e core2-dio
```

**Device**, the spare as S1's pass left it. The lists' expected values are statsref's over the card's own `stats.bin`, base and log, copied after the boot. Heap figures are compared only with a control in the same script (`docs/AUTODJ.md:1750`): the controls are main at bc29b54 and S1's build, both in the same script.

| Run | Do | Must show |
|---|---|---|
| A | Boot; `Hr`, `Hw`, `Hl0`-`Hl4` | Rank and walk times, idle and with an MP3, dark and lit; `Hr` prints the snapshot copy's µs (one loop pass); the lists equal `statsref`'s over the card's copies |
| B | `D` after the boot, with Auto DJ off and on (`D1`); `D` at the rank's peak, with Auto DJ on | PSRAM free and largest, against the controls; joined at or over 245,764 B where the control is, and within 122,880 B of it; otherwise the ladder of 6.7 |
| C | `gb` stopped, then with an MP3, then with a FLAC; `D`, `Dc`; `D` at the rank's peak, with Auto DJ on | No `[heap] FAILED`; largest against the controls; `Dc`'s lowest over 64 KB; joined at or over 245,764 B where the control is, and within 122,880 B of it; otherwise the ladder of 6.7 |
| D | After C's FLAC, an Opus | Plays, 0 skipped (its 65,536 and 62,464 B blocks fit) |
| E | `Hp` at track starts | The read's ms; the facts equal `Ht`'s |
| F | A reset with Auto DJ on: the join and the walk | Never in one pass (`[ui] slow pass`, the passes' ms) |

**PR:** one, C11-C15. **Effort:** 4-6 (guess; the device A/B 0.5-1).

### 9.7 S3: the Library screens and Last 7 days

Needs S2.

- **C16: ListLayout and ListView** (7.2): the lead item, inert rows, `refreshTopRow`, `recountStaged`, the line under the lead items. No caller, no change in behaviour. ListLayout's tests in `test_ui_nav`.
- **C17: `lib/core/StatsView`, `lib/core/StatsText`** (7.11). `test_ui_stats`.
- **C18: strings, English.** 8.1's and 8.2's S3 ids in `i18n/en.json` with `@meta` (rooms, placeholders and their worst, `bytes`); `format.hours_minutes`'s worst and rooms; the ARB words in all ten files, each measured at its room before it goes in (one that doesn't fit stays English until C19's short form: 8.1); `python tools/gen_ui_strings.py` (`lib/core/UiStringIds.h`, `lib/core/UiStringsData.cpp`, `test/test_ui_strings/catalogue.h`).
- **C19: translations** of the new ids in nine languages, and 8.1's 18 short forms of ARB words (drafted, reviewed, fit-passed); `@@note`, naming the short forms as departing from the ARB.
- **C20: the six icons** (`tools/ui_icons.py`, `src/ui/IconData.cpp`, `src/ui/Icons.h`).
- **C21: `drawStat` and `StatsWeekPage`.**
- **C22: the screens.**
  - `src/ui/LibraryStats.cpp`: the entry row, the hub, the shelves, the tray, the disabled and status rows, the empty state.
  - The PageKinds and `pageFor`; `UiHost`'s `AppState` and host calls; the drops beside `dropFilteredAlbums()`.
  - Console `uiS`, `uiSh`, `uiSw`, `uiS<shelf>[w]`, `uiSx<n>`.
- **C23: docs:** `src/ui/Pages.h`, ARCHITECTURE's Library (`:3851`) and console `ui` (`:4281`) paragraphs, README.

**Tests:**
- `test_ui_stats`: hub rows in each state of 4.4; the tray by counting days and its hits (every cell's centre at least 16 px inside, lab and true panels); sections; bars; the line forms and short forms; relative times; day labels with an offset; the stat value's segments.
- `test_ui_library` (the firmware's fonts, ten languages): the entry line in 204; hub lines in 232; header subs; tray labels in 92 and 144; shelf lines in 264, with the count kept; Week's texts; the stat value in Polish (as `:3824`, `:3925`, `:4857`, `:4944` do for theirs).
- `test_ui_nav`: the lead items.
- `test_ui_strings`: rooms, buffers, glyphs, as always.

**Done when:**
```
pio test -e native -f test_ui_stats -f test_ui_library -f test_ui_strings -f test_ui_nav
python tools/gen_ui_strings.py --check
python -m unittest discover -s tools -p "test_gen_ui_strings.py"
pio test -e native
pio run -e core2 -e core2-dio
```
`python tools/ui_icons.py` leaves `src/ui/IconData.cpp` unchanged (it needs Pillow).

**Device**, the spare as S1's pass left it (the lists' expected values statsref's over the card's own `stats.bin`, base and log, copied after the boot):

| What | Commands |
|---|---|
| Each screen against its frame (layout and colours) | `uiSx1`, `uiSh`, `uiS0`, `uiS1`, `uiS2`, `uiS3`, `uiS4`, `uiSw`; `X` each |
| Edge states | `uiSx2`, `uiSx3`, `uiSx4`, `uiSx5`, `uiSx6`; `X` each |
| Disabled rows' buzz and toast | `uiSx3`, `uiSh`, `uit160,177`; `uiSx4`, a tap on New finds |
| Tray hits | `uk1`, `uiS0`, `uit60,93`, `uit160,93`, `uit270,93`; `uk2` the same; `uk0`; then the input lab (`u`) and the `[input]` log's raw x at each split |
| Tap and push times | `ui`, `uiB`, `[ui] slow pass` (over 35 ms, `src/ui/Ui.cpp:1535`), idle and with an MP3 |
| Week's ink ("Plays" over "118") | `uiLde`, `uiLpl`, `uiLja`, `uiSw`, `X` |
| Languages | `uiLru`, `uiLja`, `uiLpl`, `uiLde` on the hub and Most played; `X` |
| A fold while a page shows | A track past 30 s with Week open, then Most played open |
| The fence | `g0` with a shelf open: the tab goes back to Artists at the fence (no entry row during it); after Live the entry row is back and the hub's totals are unchanged |
| Dark | A page up, the screen left to go dark on its own, then woken: drawn again at the wake (no `P`: 9.1) |

**PR:** one, C16-C23. **Effort:** 6-8.5 (guess; the strings and translations 1-1.5, the device pass 0.75-1).

### 9.8 S4: Now Playing's row

Needs S3 (the shelves) and S2 (the track's facts).

- **C24:** `openNavMenu`, `onSheet`, `goToStats` (7.8); `stats.go_most_played`, `stats.go_recent`, `stats.skips_n`, `stats.today_lc`, `stats.yesterday_lc` in ten languages; `test_ui_stats` (which row each case gets); `test_ui_library` (label and detail placement, as `:3925`).
- **C25:** docs (ARCHITECTURE's Now Playing, `:3546`).

**Tests:** `test_ui_stats` (which row each case gets); `test_ui_library` (label and detail placement); `test_ui_strings` (the five ids in ten languages). Control: `test_ui_nav`'s sheet cases unchanged.

**Done when:**
```
pio test -e native -f test_ui_stats -f test_ui_library -f test_ui_strings
python tools/gen_ui_strings.py --check
pio test -e native
pio run -e core2 -e core2-dio
```

**Device:** `ui0`; `uit60,80` (the cover) on a track in the top 50, then one only in Recently played, then one never played; `X` each; `uit160,96` (row 1) opens the shelf at the track; `ui` names the page; `Hp` names the row's choice (9.3).

**PR:** one. **Effort:** 1-1.5 (guess).

### 9.9 S5: the owner's card

S5a after S1 is merged and before any release or main build carrying S1 boots the music card; S5b (step 4) after S2. Only with the owner's go-ahead.

Before step 1: the board's settings and image backed up (9.1). After each boot, `z`. No taps, no `n`, `p` or space.

1. Copies of the card's `/.player/plays*.jnl`, read-only from a PC reader: `python tools/playlog.py read` gives the plays, deviceIds and epochs the first boot will fold.
2. The stats firmware boots with the card. **Nothing is played.** `H`, `Hm`: the fold equals the copies' count by 4.1's rules (`statsref`); the save; the trim of ACKed files only.
3. The owner syncs on the PC (the terminal writes ACKS); the next boot: no fold, the trim, `H` unchanged.
4. (S5b) `Hr`, `Hw`, `Hl0`: the lists on the real library (19.5k tracks); the walk's time; orphans.

After step 4: the board is left on the stats build, with the owner's word. A 0.14.0 image with this card undercounts (5.12).

**Effort:** 0.5-1 (guess; S5a 0.25-0.5, S5b 0.25-0.5), plus the owner's sync.

### 9.10 S6: the time zone

Free-standing from S3 on; two repositories.

- **C26, firmware:**
  - `HostLink::timeLine` reads an optional field 1 (−720 to +840; anything else: a range error) into `HostLink::Out`;
  - NVS `clock/tz`, an i16, schema 7, `migrate(6, 7)` doing nothing;
  - `tzOffsetMin` from it into `statsview`/`statstext`; Week's sub drops "(UTC)".
  - Tests: `test_host_link`, `test_nvs_layout`, `test_ui_stats`.
  - Docs: `docs/HOST-STATUS.md`'s `@time` (`:304-320`, "Always UTC").
- **C27, terminal:** `src/device/desk.rs` sends `@time <unix-ms> <minutes>` from `tz::local()` when it gives a zone: on terminal main after #56 lands, or on #56 once main (with 5eac90c) is merged into it. At 3edd4a4, `tz::local()` gives no zone on Windows (terminal `src/admin/tz.rs:6-7` there), and the owner's PC runs Windows. Firmware before S6 ignores the field.

**Done when:**
```
pio test -e native -f test_host_link -f test_nvs_layout -f test_ui_stats
pio test -e native
pio run -e core2 -e core2-dio
```
and the terminal's `cargo test` for `desk.rs`.

**Device** (the spare, silent mode): `@time <ms> 60`, then `@status`; Week's sub without "(UTC)"; a reset keeps `clock/tz`; `@time <ms>` with no field leaves it; `[nvs] schema 6 -> 7: ...` (`src/app/NvsSchema.cpp:85`) on the backed-up settings, then the settings put back (9.1).

**Effort:** 1.5-2.5 (guess, both repositories).

**Total:** about 19-28.5 agent-days, a guess (S0 1-1.5, S1 5-7.5, S2 4-6, S3 6-8.5, S4 1-1.5, S5a and S5b 0.5-1, S6 1.5-2.5). S1 grew with the review's cases (a partial scan, the base's faults, the card's copy changed, the end's rotation, the twin, firstAt, retention) and runs G (a throwaway build) and L; S2 with the scheduler's lanes and the join's gate. The mood plan said 15-24, and parallel sessions built its S0-S5 in about a calendar day.

## 10. Conflicts with open PRs and work in flight

Trial merges from the design run's process map (`git merge-tree`, READ there); not run again here.

| Work | Overlap | What to do |
|---|---|---|
| **#58** `chore/next-release-0.15.0` f0da06c | `README.md`, `docs/ARCHITECTURE.md`, `tools/version.py`: version lines only. Merge-tree clean | Land it first |
| **#25** `claude/esp32-swap-mode-research-51c26d` | `src/main.cpp`, `test/test_ui_library/test_ui_library.cpp`, a console under `g` (`gx`), `lib/core/TagText.*`, docs. Merge-tree clean | Nothing waits for it; the second to land adds its letter beside the other's |
| `origin/claude/ecstatic-bose-7e4267` (the ESP-NOW spike, no PR) | `src/app/SerialConsole.*` (top-level `E`), `src/main.cpp`; already conflicts with main | Don't use `E` |
| The Bluetooth EQ (no branch found) | NVS schema (`docs/ARCHITECTURE.md:1866-1869`) | S1-S4 need no key. S6 takes the next schema, or shares one release's with the EQ |
| Terminal #56 `claude/card-transfer` 3edd4a4 | `src/device/desk.rs` sends `@time <unix-ms>` (its tests at `:3381-3387`); `src/device/firmware.rs:783` already calls `tz::local()` | S6's terminal half goes on main after it lands, or there once main is merged in (the Windows zone: the next row). S1-S4 don't wait for it (O4) |
| Terminal main's `src/admin/tz.rs` (since 5eac90c) | Reads TZ, the TZif files, and on Windows the zone Windows is set to; not in #56's 3edd4a4 (5eac90c isn't its ancestor) | The zone's source exists on every OS on main; C27 needs main, or #56 with main merged in |
| Terminal `claude/card-picks` (unpushed, per the last notes) | None with the stats | — |
| `feat/card-transfer-firmware` 218822a (#27, merged) | None: in main | — |
| Issue #43 (the mood plan) | None | — |

**Files the stats touch** that open work also touches: `src/main.cpp`, `test/test_ui_library/test_ui_library.cpp`, the console, `i18n/*.json` with the generated tables. Merge `en.json` by hand and regenerate.

## 11. Budget

Re-measure every line marked with a run.

| Resource | Today | Addition | Label | Re-measure |
|---|---|---|---|---|
| Flash | 4,423,679 B (70.3 %), the app 72 % of its 6 MB slot (`docs/RELEASE-0.14.0-CHECKS.md:47-48`, the build at `:43`: the 5ea8d34 build, bc29b54's code); 4,423,691 B at S0 (below). The guard warns at 80 % (`tools/flash_guard.py:49`) | Code 25-40 KB; strings 20-30 KB across ten catalogues | GUESS | `pio run`, each stage |
| IRAM | 5,161 B left on 2026-11-06 (`docs/ARCHITECTURE.md:4513`), and again at S0 (below), after #27, #55, #56 | **0.** No `IRAM_ATTR`; nothing on an ISR. No libc object that the framework's `sections.ld` pins in IRAM and `iram_diet` leaves there (`tools/iram_diet.py:21-23`), unless 0.14.0 already links it. Among these: strtok_r, strsep, memrchr, strcasestr, strlwr, strupr, ldiv, memccpy, strncat (strspn, strcspn, div and strlcat are pinned too, but already linked: S0's list below); the console parsers (`Ht<16 hex>`, `Hl<shelf>[w]`, `uiS<shelf>[w]`, `uiSx<n>`) are where one would come in. (`iram_diet` moves gmtime and the rest of the time code to flash, `:58-66`; qsort and bsearch were never pinned.) The check at each stage: the link map's `libc_a-*` objects in `.iram0.text` equal S0's list, and IRAM left equals S0's figure; `iram_diet: 51 of 51` can't catch a new pinned object | READ | The link map and the memory summary, each stage |
| Internal RAM | Steady 50 KB, 42 KB the lowest, with Bluetooth (`docs/METADATA.md:6322`) | Under 1 KB of statics (pointers, flags). Every block through `psramAlloc` | GUESS | `s`'s `ram=`, `min=` |
| The loop's stack | 8 KB; lowest 3,480-3,788 B at a boot, 3,532 B in a join run, 2,548 B the lowest of all (a held boot under DO low) (`docs/METADATA.md:6322`; `docs/AUTODJ.md:1802-1803`) | The save's frames under 512 B more than a slot's write: the `ContainerWriter` (about 2.1 KB) and the buffers in PSRAM. The rotation's save is the deepest path: under `writeOne` → `append` → `put` → `place` (`rec[160]` and `tail[160]`: `lib/core/PlayLog.cpp:468, 487`) → `rotate` (`rec[160]`: `:458`), with FatFs's 512 B long-name buffer on the caller's stack for each call (`src/storage/CardFat.h:32-33`) | MODELLED | `-fstack-usage` (C9); S1 G and K |
| PSRAM, resident | — | In 16 KB chunks (5.6), footprints (6.5). Typical about 98-115 KB: HEAD and MARK 1.7, HOUR 6.8 and DAYS 6.5 (at their caps), RCNT one chunk 16.9, TRKD 1-2 chunks 16.9-33.8, the spare chunk 16.9, hash → id one chunk 16.9, lists 11.3, the rest 4. Ceiling about 267 KB: RCNT 3 chunks 50.7, TRKD 6 chunks 101.4 (forced compaction), hash → id 4 chunks 67.6, the rest as typical. 6.7's ladder if the model or S1 L / S2 B, C fail the pass rule | MODELLED | S1 `H`, L; S2 A/B |
| PSRAM, transient | — | The walk: played set 33.8-169 KB (2-10 chunks), folder states 25.1 KB. The rank's snapshot 16.9-152.1 KB (1-9 chunks). The boot fold's indexes: TRKD's 32 KB at 5,000 hashes, RCNT's as many entries as its rows ask. A full never-synced log's fold, MEASURED-HOST (`test_stats_fold`: 13,106 slots, 4,653 hashes): TRKD 8 chunks, RCNT 13,073 rows in 10 chunks (its rows are pruned only at the save), the indexes 32 KB and 64 KB, 412,992 B held at the peak with HOUR, DAYS, MARK, the ranges and the spare; this plan's model had left RCNT's growth out | MODELLED | S1 I; S2 A/B |
| PSRAM headroom | 0.34-0.66 MB (`docs/METADATA.md:5379`); largest free after an update with Auto DJ 180-295 KB (MOOD-FILTERS 9.6) | The resident blocks come out of every update's room (no give-back). Joined after a FLAC update and the rejoin is only 16-49 KB over the mood plan's 245,764 B line (6.7) | READ | S1 L; S2 C |
| Card writes, a play | The slot, 15-17 ms (MEASURED) | 0 | READ | — |
| Card writes, a boot with new plays | — | One `stats.bin`: typical 21-44 KB, ceiling about 160 KB. On the loop: 0.05-0.17 s typical, 0.19-0.4 s at 160 KB (0.5-1 MB/s writes, `docs/METADATA.md:4545-4546`, plus 30-80 ms of directory and FAT work), plus the save's sort | ESTIMATED | S1 A, H, I |
| Card writes, a compaction | — | A base of 24 B a played track (53 KB at 2,214; 467 KB at 19,478), read and written: 0.8-1.6 s idle at 19,478, longer with an MP3; rare, and only at a safe point (5.3) | ESTIMATED | S1 I |
| Card writes, a rotation | `plays.1.jnl`'s 524 KB read on the loop (today's) | A save when it holds plays not saved: never at a session's first rotation after the boot's save (5.5) | READ | S1 G |
| Card reads | — | `stats.bin` once at the boot: 30-110 ms typical, 0.1-0.27 s at 160 KB, one pass with the CRCs; and its base whole, its rows' CRC and order (24 B a played track: 53 KB at 2,214, 467 KB at 19,478; S1 C7, as the reference reads it); a remount before the boot's first slot repeats both on the loop (one read, no waits); the base once a rank, on the worker; 1,536 B a track start | ESTIMATED | S1 A, I; S2 A, E |
| Card space | — | `stats.bin` and its tmp during a save (32 KB clusters); two bases during a compaction | READ | — |
| Loop time | Passes about 13.5 ms with the join | The fold with the index 2-10 µs a slot (MODELLED); the save once a boot, its sort in ms (printed by S1 A and I); the rank's snapshot copy, one pass; the walk 15-25 passes of up to 8 ms, never with the join; UI as 7.10 | GUESS | S1 A, I; S2 A, F; S3 |
| Worker time | — | The rank: 0.1-1.5 s at priority 0 while lit (2-2.5x its 0.05-0.6 s of CPU: `lib/core/ScanScheduler.h:44-47`); a track's read 15-20 ms | GUESS | S2 A, E |
| Strings | 663 ids in `i18n/en.json` (MEASURED-HOST) | 85 ids; 666 texts owed in nine languages, plus 18 short forms of ARB words (8.1) | READ | `gen_ui_strings --report` |
| The track ceiling | 23k-27k (`docs/METADATA.md:5551-5554`, ESTIMATED) | About 1.1-1.3k fewer at 98-115 KB resident (90 B a track) | MODELLED | — |

A slow pass is a UI hitch, not an audio risk: the sinks draw on a ring of about 1.5 s (`docs/ARCHITECTURE.md:83`).

**S0 baselines** (MEASURED-HOST, 2026-11-11, on this Windows box). The tree was cad1449: main 241642d's code with C0-C2's docs. Main since bc29b54 changed only `tools/version.py` (#58, NEXT_RELEASE 0.15.0) and docs, so this is bc29b54's firmware with the next version's string. Every later stage compares against these.
- **Host tests:** `pio test -e native` from Git Bash (PlatformIO Core 6.2.0): 108 suites, 2,286 test cases, 2,286 succeeded, 0 failed, in 18 min 46 s.
- **`pio run -e core2`** (PowerShell, MSYSTEM unset; a clean build, 8 min 19 s): flash 4,423,691 B (70.3 % of 6,291,456; `flash_guard: ok: app 4.31 MB = 72% of the 6.00 MB slot`), 12 B over 5ea8d34's 4,423,679 B (the version string). RAM 59,064 B (PlatformIO's figure: `.dram0.data` 25,112 B and `.dram0.bss` 33,952 B; 65,512 B of `dram0_0_seg` left for the heap). IRAM 5,161 B left: `iram0_0_seg` ends at 0x400A0000 and `.iram0.text` at 0x4009EBD7. `iram_diet: 51 of 51 libc objects moved to flash`.
- **The link map's `libc_a-*` objects in `.iram0.text`** (`.pio/build/core2/firmware.map`, the `libc_a-<name>.o` inputs between `.iram0.text` and `.dram0.data` in its memory map), 25: bzero, div, envlock, getenv_r, isspace, memchr, memcmp, memcpy, memmove, memset, strcat, strchr, strcmp, strcpy, strcspn, strlcat, strlcpy, strlen, strncmp, strncpy, strnlen, strrchr, strspn, strstr, tolower. A stage that adds a name to this list has linked a pinned object; IRAM left must stay 5,161 B.

## 12. Unknowns

| Unknown | How to measure | Stage |
|---|---|---|
| The save's device time (tmp and replace, the sort), idle, with an MP3, on Bluetooth | A timing on the `[listen] saved` line; `Hs` | S1 (H, I) |
| Whether 0.2-0.4 s of loop at the scan's end (0.8-1.6 s more with a compaction), or at a rotation, shows as a UI stall or an audio break | `[ui] slow pass`; `s`'s underruns= and breaks= around the boot's save with an MP3 and a FLAC playing | S1 (I, H, G) |
| The loop stack's lowest at the rotation's save, and at a boot's save under DO low | `-fstack-usage` sums (C9); the stack's lowest against 2,548 B | S1 (G, K) |
| The boot fold's PSRAM peak on a full never-synced log | `test_stats_store`'s full-log case (printed: 396,608 B at 4,616 hashes, MEASURED-HOST); `H` and `D` after a seeded full log | S1 (C8, I) |
| The fold's CPU a slot | The scan's passes and µs with and without stats | S1 (I) |
| How often 6,553 plays pass between saves (the rotation's save) | The `[plays] ready` lines over weeks | S5, then use |
| Whether other players use the owner's cards | `Hm`; `tools/playlog.py read` on copies | S5 |
| Whether a card that drops writes at its end of life passes the size check with the sector cache serving the read | `gc2` (verify mode, `src/app/TagConsole.h:46-49`), then saves; `gc1` after | S1 (J) |
| The walk's CPU at 19.5k tracks | `Hw` idle and with an MP3, dark and lit | S2 (A), S5 |
| `findTrack()`'s cost at a live fold | µs on the `[listen]` fold line | S2 |
| The owner's distinct played tracks (P) and plays a day | `H` after the first boot; TRKS rows over weeks | S5 |
| PSRAM free and largest while playing at v0.14.0, Auto DJ off and on | Main at bc29b54 as a control in the same script as S1's and S2's builds | S1 (L), S2 (B, C) |
| Whether the resident chunks fill the boot's holes or come out of the largest free block (the pass rule, 6.7) | `StatsRun` at P = 2,214 and 19,478; `D` against main | S1 (C8, L), S2 (C13, B, C) |
| Internal RAM with Bluetooth at v0.14.0 | `s`'s ram= and min= over 10 minutes with the owner's headphones linked | S2, if the owner links them |
| Whether the resident and transient blocks keep Opus's blocks placeable after a FLAC update | The model's `StatsRun`; S2 C, D | S2 |
| How often a real sync changes `buildStamp` | The stamp logged at the boot and after each update | S5 |
| The orphan rate after real syncs | The walk's "matched no track" count | S5 |
| The tray's splits on this panel | The input lab (`u`), the `[input]` log's raw x; `uk1`, `uk2` | S3 |
| The real UI costs (a staged row, a tray tap's pass, Week's first draw, a glyphs-only line, the 4-row sheet) | `uiB`, timed `[ui]` lines, `[ui] slow pass`, idle and with an MP3 | S3, S4 |
| Ink between "Plays" and "118" on Week | `X` in en, de, pl, ja | S3 |
| ja and zh glyphs | `python tools/gen_ui_strings.py --check` with the translations in | S3 |
| Flash and IRAM headroom at bc29b54, and the `libc_a-*` objects in `.iram0.text`; the host baseline on this box | `pio run -e core2` and its link map; `pio test -e native` from Git Bash on clean main | S0: measured (11, "S0 baselines") |
| When Not played yet opens (O5), and its rule with no clock | The owner | Before S3 merges |
| The zone's form in `@time` and its home | The owner and the terminal | S6 |
| What the owner's music card's log holds now | `tools/playlog.py read` on copies, with the owner's OK | S5 |
| Whether the Bluetooth EQ exists anywhere | The owner | S6 |
| Which of #25 and the ESP-NOW spike lands first | The owner | Any |

## 13. Risks

What would change the plan:

- **The save is felt.** If S1 I or H shows the boot's save over 0.3 s, or a compaction over 1.5 s, with an MP3, or any break: move it to a worker job after Ready, and trim at a boot only what the card's MARK already covers. Each boot's slots are then trimmed one boot later, and folds during the worker's save need a frozen snapshot (its memory measured first).
- **The format is primary data.** A mistake found after S1's first release stays on cards: every later firmware reads v1. S0's reference and S1's proof are the guard. If S1 ships alone, that release is the first real-card run, so S5's steps 1-2 are S5a, run before that release.
- **A downgrade, or a 0.14.0 unit** with the card, trims without folding: an undercount, counted in HEAD's gap and said in the release notes. No double count.
- **The rotation hole.** A rotation deletes (or moves to a twin) a `plays.1.jnl` holding plays the card's MARK doesn't cover, and no save lands before the restart: those plays are lost to the stats (5.5). It happens when:
  - the rotation's save fails (a full or failing card), or its tmp is held and not promoted before the restart (5.5, step 5);
  - the stats are Off this session (Unread, Newer, no memory for the fold, the folds dropped while ending);
  - this session's saves are refused (a partial scan, a changed `stats.bin`);
  - the rotation comes in the orderly end's flush with under 1 s of its budget left (`src/app/PlayHistory.cpp:307`; `lib/core/PlayLog.cpp:505, 437`).
  Each needs the second rotation of a session (6,553 plays), or a boot whose save failed or was skipped. C8 asserts each count exactly.
- **PSRAM at the ceiling.** A card where nearly every track has been played takes about 267 KB resident (in chunks) and up to about 195 KB more during a walk. A failed allocation leaves names as "Counting plays…" and never loses data. Fallbacks: 6.7's ladder; u32 prefixes for the played set (4 B, confirmed by the full hash on a match), album bits from fewer chunks.
- **Orphans.** Renames, Opus conversions and NFC/NFD changes take plays out of the shelves until ORIG is followed. The totals stay right.
- **RCNT's cap** shortens the 30-day window for very heavy listening. Its cell says how many days.
- **UTC days** mark the wrong "today" in the listener's evening west of UTC (or morning east of it) until S6. O4 accepted that.
- **Translations** are machine-made; 684 texts owe a native review (666 new, 18 short forms of ARB words).
- **The tray's splits** are untested on this panel.
- **The scratch spec and frames** go with Windows' temp cleanup in about a week. This plan inlines the spec; S0's C1 copies the frames from the canvas, which persists.
- **A `stats.bin` the card fails to read at every boot** (a bad sector) keeps the stats off and the trims held. The log stays bounded by the rotation, which then deletes plays no MARK covers (the rotation hole); the console says why; a disk check is the fix.
- **Two hashings with Auto DJ on.** The library is hashed twice after a Live (the join's and the stats' walk): about 0.1 s more CPU. If S2 F shows it felt, the observer ride (14) comes back.

## 14. Considered and left out

**Other directions on the canvas.**
- A, Glance: a stats screen of tiles, a 4 x 64 px period tray at the right edge, a month view. Its data model, the entry row's bars, the empty state and the stat value were grafted into C.
- B, Recap: a story of pages (the week, the hours, the top, the find), reached from the Dance tab. Its big day, its "found by", its Auto DJ rate and the undated anchor to the sync were grafted. Its overlay band and its hash-to-track lookup were not.
- A dedicated tab: the tab bar's four tabs stay; the stats live where the listener browses.

**Taken out by the owner's decisions.**
- The plays line on album track rows (frame AlbumPlays; O3), and `stats.not_yet*`.
- A filter by deviceId (O2), and TRKS's "another device's" flag bit.
- Not folding `z`'s plays (O6).

**Other shapes for the data.**
- Three files renamed one by one: disagree after a cut (1.1).
- TRKS rewritten whole at every boot: up to 467 KB a boot at the ceiling.
- A log-only v1, counting the log at each boot: every trim and rotation loses history.
- Counters in NVS: a write a play, against `src/app/NvsLater.h:19-21`.
- DAYS alone, without HOUR: no local days when the zone arrives.
- HOURS for 13 months (the spec): nothing in v1 shows hours past a week; DAYS keeps the shape of a year at 6.4 KB.
- Folding drop notes into a lost figure: they carry no seq, MARK can't dedupe them, and on a card never synced each note counts plays already folded.
- A save mid-session on a timer: each costs a 0.05-0.4 s loop hold, and gains nothing the next boot's fold doesn't. The rotation's save and `Hs` are the only ones, both synchronous, so no fold happens under them.
- A staged boot fold beside the base: reading `stats.bin` again at a restarted scan does the same with no second copy.
- A resident TRKS: 53 KB typical but 467 KB at the ceiling.
- The worker saving before `finishScan()`: no window (1.1).

**Other shapes for the map.**
- The spec's id-keyed played-track table (8 B a played track, dense past half the library): can't name the rankers' hashes, and is dropped at every fence.
- A dense 4 B-a-track array: Auto DJ's filters block's size class, which once failed to place.
- Riding Auto DJ's join (an observer in `hashSlice`, `lib/core/AutoDjJoin.cpp:631-643`): saves about 0.1 s of CPU after a Live with Auto DJ on, which the owner keeps off (`docs/MOOD-FILTERS.md:1525`); at the boot the join starts before the stats' lists exist. The code is shared; the pass is not.
- `Join::setPathHash()` as the hook: it turns the folder states off (6.2).
- A walk on the card worker: the index isn't thread-safe (`lib/core/LibraryIndex.h:100`), and the fence clears it on the loop.

**Other shapes for the UI.**
- The artist page's " · N plays" sums: left out by this plan's reading of O3 (3.2), not by the owner.
- A "Today" window and a 4-cell tray: under the panel's right-edge skew.
- "Per day" filler rows, a "New to you" row with no list: dropped by the design's judges.
- An hour-of-day view: needs the zone, and a room the design didn't have.
- Swipes between windows: the house has no swipe on lists.
- Inert segments drawn dim: dropped; offered cells only.
- An inert NP row for a first play: no row instead.

**Later, not v1.**
- mStream's per-track counts on the card (spec 8.6): Not played yet means never, counts include other clients.
- An artist page "Top tracks" row.
- Following renames through ORIG's audioHash.
- Dating undated plays back from a later slot of the same boot (2.20.6 item 7), in the live and the boot's folds alike.
