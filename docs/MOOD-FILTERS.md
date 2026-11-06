# Mood filters

Mood filters narrow the Albums list and Auto DJ's picks by mood, using the scales and tags of mStream's other clients. The player works them out from the card. It needs no network while playing, and a filter change reads nothing from the card.

## 1. Status

**Status (2026-11-04):** planned. Nothing is built, flashed or merged. A read-only study found it possible, and this document is the build plan.

**Picked by the user, on the design canvas:**
- The editor: the one-screen grid of 7.1.
- The way in on Albums: the quiet first row and the dot of 7.2.

**Assumed:** everything else. Section 3 lists each default with its alternative, and the user can overrule any of them. Where the plan differs from a drawn frame, 3.2 says so.

Line numbers are at these commits:
- firmware a1566fe
- mStream master 97a97ca7 (v6.32.3)
- terminal player `claude/card-waveforms` 64437c2
- phone app d17c3fa5

Paths with no repository named are the firmware's.

Main has moved since: PRs #37 to #42 merged on 2026-11-05 (main 305b893). They change nothing in this plan, but 26 of the files it cites changed, so read a cited line with `git show a1566fe:<path>` and find it again by name.

Labels:
- **READ:** from the code or docs.
- **MEASURED:** on the device, quoted from the docs.
- **MEASURED-HOST:** a planning script run on a PC.
- **ESTIMATED:** the docs' own word.
- **MODELLED:** arithmetic or a port, not run on the device.
- **SYNTHETIC:** simulated on a made-up library.
- **GUESS:** judgement.

## 2. What the user gets

**On Albums.**
- With mood data on the card, the list gains a first row: a funnel, "Filters", the album count. It scrolls with the list.
- A tap opens the editor. Choices apply at once, and the cross closes it.
- The list then shows only albums where most analysed tracks fit.
- The first row says what is set and how many albums remain. A cross on it clears the filter.
- Scrolled past the first row, a dot on the Albums segment says a filter is on. A tap on the segment goes to the top.
- A boot forgets the filter.
- On a card with no mood data nothing changes: no row.

**On Auto DJ.**
- Output > Auto DJ gains "Smart filters" as its fourth row. It opens the same editor, in blue, counting tracks.
- Every pick then stays inside the mood down the whole ladder: neighbours first, then any track that fits, then repeats, oldest first.
- If no track fits, Auto DJ adds nothing, reads Waiting and says why.
- The filter is saved.
- With no filter set, the stream of picks is byte for byte today's.

**The same as the server** (READ, mStream `src/api/mood-filter.js:18-19, 78, 149-182`):
- the song predicate at the three bars, ties and NULLs included;
- a level alone filters nothing;
- the album rule `2*pass > analysed`;
- unanalysed songs match nothing;
- Auto DJ never relaxes the mood (mStream `src/api/random.js:857`).

**Where it differs:**

| Topic | Server | Player | Source |
|---|---|---|---|
| Album | `album_id`, shown grouped by name, year, art | The folder | `lib/core/LibraryIndex.h:28`; mStream `src/api/db.js:571` |
| Album's denominator | Songs the caller can see, every copy | Files on this card | mStream `src/api/mood-filter.js:30-34` |
| Hand-copied or edited files | Scored | No T record, no moods, until the next transfer | terminal `src/device/card/waves.rs:16-17` |
| A peer's tracks on the card | Scored on the peer | No scores in v1: they match nothing, so an album of them goes under any filter (3.2) | terminal `src/device/card/sync.rs:1456-1457` is the same rule for neighbours |
| Freshness | Live | The last sync that read the scores (5.6) | mStream `src/api/sync.js:40` |
| Auto DJ's pool | Any song above a cosine floor | The row's stored neighbours, then a jump (6.2) | about 8-18 % of the server's pool in Follow the vibe, 5-10 % in Stay on seed (SYNTHETIC, rough) |
| Filtered views | Albums, Artists, Discover | Albums and Auto DJ (D11) | |
| Play all on a filtered list | Offered by other clients | Not in v1 (D7) | |

A listener's own seed is not tested, as on the server (mStream `src/api/random.js:638-711`). It plays and steers. Picks after it obey.

## 3. Decisions

### 3.1 The design and the study's questions

| Id | Decision | Status | Alternative |
|---|---|---|---|
| P1 | Editor: the one-screen grid | **user's pick** | Overview rows; flat checklist |
| P2 | Way in: quiet first row, dot when scrolled past | **user's pick** | Fixed bottom bar; status band |
| D1 | A dedicated route, `POST /api/v1/db/mood-scores` | assumed | A field on `metadata/batch`: no allowlist line, but a separate all-paths read, about 1.5 KB a track, about 30 MB a refresh at 20k (MODELLED) |
| D2 | Stay on seed under a narrow filter re-pins | assumed | Seed hop; plain jumps. Plain jumps drop S4's commit 5 and its sessions |
| D3 | Nothing in the mood: no pick, a new Waiting hold with a reason. Amends "never stops" at `docs/AUTODJ.md:309` | assumed | Fall back to no filter |
| D4 | A saved Auto DJ filter on a card with no mood data: picks unfiltered, the row dims and says so | assumed | Hold instead |
| D5 | A queued pick outside a new filter plays once | assumed | Remove it. Meets the gapless caveat at `lib/core/PlaybackController.h:131-133`, not traced |
| D6 | The Albums filter is not saved across a boot. Auto DJ's is | assumed | Save it in `ui/albummoods`, u16 |
| D7 | No Play all on a filtered list in v1 | assumed | Add it: transient ids, at most 81,924 B (MODELLED) |
| D8 | Three levels, 2 B a track | assumed | Standard only, 1 B, about 20 KB less |
| D9 | The same licence gate as Auto DJ: off on a card "for someone else" | assumed | Scores on every card |
| D10 | Firmware first, against a synthetic card. The writer stacks on terminal #56/#68. mStream independent | assumed | Wait for the stack to land |
| D11 | No Artists view in v1 | assumed | Add later, cheap (GUESS) |
| D12 | Line-form empty state on Albums | assumed | Full empty state (frame A-empty-full) |
| D13 | First row hidden on Albums with no mood data | assumed | Dim with a note (frame A-nodata-dim) |
| D14 | No long-press accelerator in v1 | assumed | Long press on the segment |

### 3.2 Smaller defaults made by this plan

All assumed.

| Area | Default | Alternative |
|---|---|---|
| Frames | Toasts follow the code, not frames dj-toast-few, -jump, -once. They draw two lines there. `src/ui/Overlays.cpp:101` splits only at ": " and only when too wide, so all three are one Body line in English (269, 275, 260 of 284, MEASURED-HOST), and a split drops the colon | A forced split: a `Toast` flag, and "Plays once" needs a colon |
| Frames | The canvas's 10x10 cross becomes `kCross`, 12x12 (`tools/ui_icons.py:168`) | Draw a 10x10 |
| Frames | Frame A-partial is left out of v1 | "mood data 92%" at rest |
| Frames | Cross at x 284 at the scrollbar's width, as the canvas's rule gives. The brief names only 232 and 292 | Keep 292, which reads over the scrollbar |
| Frames | The status row with a filter on and no usable index has no frame. 4.4 defines it in words | Draw one first |
| Editor | Level cycles Strict > Standard > Loose > Strict. Clear stays open and resets the level. The row's cross has no Undo | |
| Editor | A toast still up when the editor opens goes, as `src/ui/Ui.cpp:566-570` does for the Dance tab, its Undo with it | Keep it, and draw a frame for it over the title row |
| Dot | Shown only while Albums is the segment on show. A held filter says nothing from Artists or Folders | Also while those show: one branch at `src/ui/LibraryPage.cpp:344-345` |
| Count | The long count falls back to `queue.summary_position` when it doesn't fit (ru, pl at 5 digits) | New wording |
| Engine | Re-pin only onto the playing row. A jump keeps the classes. A fresh track with no row goes before repeats. An only-fitting track repeats itself | |
| Engine | No toast for a re-pin, or when the queue runs out under Waiting | A note |
| Toasts | The three new toasts rank below every old one, so the old order stands. A new one waits when something above it is said in that pass (4.5) | Few above the old toasts, with a carry |
| Auto DJ row | Behind the fence it shows its last values, and a tap says `library.updating_wait` | Dim |
| Auto DJ row | The dim row's note says to sync from mStream, also on a card for someone else, where a sync adds nothing. This plan gives the player no way to tell that card apart | A second wording, and a way to tell |
| Release | S5 merges but stays out of a release until a writer build carries scores (9.2) | Ship the dim row first |
| Data | Scores outside 0-1 clamp. `h[22]`, `h[23]` in the header. The block is taken lazily at the first record. Moods live in `moods.json`, not `vectors.json`. A T record UNREADABLE without FROM_API loses its word | |
| Data | A reader ignores an order bit whose value byte is 255 | Make that tail invalid |
| Writer | Asks the own server only. A peer's rows stay at 8 zero bytes | Peers too: `PeerSide.moods`, `moods_from`, a version rule, about 0.25-0.5 day (GUESS) |
| Writer | No "mood scores to add" offer and no `moodsNone` state. The sync asks anyway, and the device's dim row says to sync | Both, as `waves_none` (terminal `src/device/card/state.rs:95-100`) |
| Writer | A null row clears. Whole or nothing per read. `NoHooks` keeps scores. No progress phase, English notes, no commit hold-back | |
| Writer | The gate is `features.moodScores` alone (5.2) | Also `moodFilters` |
| Route | Lives in `db.js`. All-null with `analyzeMoods` off. `model` is an object. No per-path cap. A jukebox session is allowed. No `license` field. `heads` grows without a bump | |
| Route | Stays on the federation allowlist, so a later peer read needs no server release. Tested by key over HTTP, not over real iroh, until a client asks peers | No allowlist line until then |
| NVS | Shares schema 5 with #27 if both ship together, else 6 | |
| Console | `gm`, `uiM...`, `Dq<hex>` (9.3) | |

## 4. The behaviour, precisely

### 4.1 The filter's rules

Four groups. A group left out does not filter, and groups are ANDed.

| Group | Values | Tap |
|---|---|---|
| Valence | Happy, Neutral, Sad | Pick one. A second tap lets it go |
| Energy | Relaxed, Neutral, Aggressive | Pick one. A second tap lets it go |
| Voice | Instrumental, Vocals | Pick one. A second tap lets it go |
| Tags | Acoustic, Party, Danceable | Pick any. All must hold |

**Level.** Always one of Strict, Standard (the default), Loose. Alone it filters nothing.

**Clear** empties everything and returns the level to Standard.

Bars, on / mirror: Strict .7/.3, Standard .5/.5, Loose .3/.7 (phone app `lib/objects/mood_filter.dart:51`).

| Predicate | Holds when |
|---|---|
| Happy, Relaxed | the head >= on, and >= the other head (a tie goes to these) |
| Sad, Aggressive | the head >= on, and > the other head |
| Neutral | both heads < mirror |
| Instrumental, a tag | the head >= on |
| Vocals | instrumental < mirror |

A NULL head fails only its own scale. A song with no row matches nothing. An album matches when `2*pass > analysed`, counting only analysed songs on this card. An album with no analysed song never matches.

### 4.2 The Albums list

| State | Trigger | Drawn | A tap |
|---|---|---|---|
| No data | `hasMoods()` false, or synthetic browse | Today's list, no row. A filter still held is dropped at `enter()` | As today |
| At rest | Moods, no active filter | First row: funnel DIM, "Filters", "1,741 albums", chevron | Row opens the editor |
| Filter on, at top | Active filter, offset under 42 | Funnel in the accent on a 40x40 plate, summary in the accent, "312 of 1,741 albums", cross on a disc | Cross clears, list reloads. The rest opens the editor |
| Scrolled past | Active, offset >= 42 | Dot on the Albums segment | Segment goes to the top (`src/ui/LibraryPage.cpp:351`) |
| Widths | Filtered rows | 284 over 30 rows with a rail (`src/ui/ListView.h:91`), 312 with a scrollbar, 320 with neither | Cross x follows |
| Nothing fits | Active, 0 albums | First row and "No albums match these filters", Body DIM centred, y 150 | Row as above |
| Updating | `index()` null, behind the fence | "Updating the library…", no row. Filter kept, array dropped | None |
| Update ends | `libraryChanged()` | Array recomputed in `enter()`. No moods now: the filter goes | |
| Status band | The card worker speaks | Band 148 px. The row is a list item, nothing else changes | |
| No memory | Array can't be carved | Filter reverts, `toast.no_memory` | |
| Hidden album | Go to album, a playing album filtered out | Opens it; no tint on the list. Back returns to the filtered list | |
| Other segments | Artists, Folders | Unfiltered, no dot. The filter stays for Albums | |

### 4.3 The editor

| State | Trigger | Drawn | A tap |
|---|---|---|---|
| Nothing set | Open, no filter | Count "1,741 albums" SOFT, Clear FAINT, level cell dim | |
| Set | A value picked | Accent fill, DARK text, same font. Count "312 of 1,741 albums". Clear TXT, level cell live | Second tap on a picked tray segment lets it go. A chip toggles |
| Press | Finger down | That tray or chip alone: BTN_HI, or SOFT if picked | Acts on Tap |
| Level alone | Level moved, nothing set | Level cell dim, label kept. Clear TXT. Count as nothing set | Cycles. Filters nothing |
| Zero | Count 0 | Count amber. Stays open | Everything still works |
| From Auto DJ | `OutputPage` owner | Blue accent, "1,204 of 19,412 tracks" | Applied and saved at each tap |
| Wide language | pl, ru | Small for the whole grid. One ru tag on two Small lines | |
| A toast at opening | A toast is up as it opens | It goes, its Undo with it | |
| A toast while up | A toast arrives while it is up | Toast over y 36-71, as over any sheet | A Down above y 72 goes nowhere, so an unseen Clear can't be hit. The sleep timer's buttons stay first (`src/ui/Ui.cpp:1654`) |
| Closed from outside | Tab tap, language, fence, a dialog or a sheet opening | Closes through `closeModal(true)`, owner told (`src/ui/Ui.cpp:535, 589, 981, 1020, 1089`) | |
| Suspended | `suspend()`: `closeModal(false)` (`src/ui/Ui.cpp:1886`) | Closes, owner not told, no reload under `suspended_`. The word is already applied. `enter()` at resume reads it | |
| Cross | Tap at x >= 250 in the title row | Closes. There is no Cancel | List reloads once if the filter changed |

`closeModal(false)` has five other callers (`src/ui/Ui.cpp:1106, 1332, 1341, 1766, 1775`). Each runs only with a dialog, the volume sheet or the jump grid up, and each of those opens through `closeModal(true)`, so the editor is gone by then.

### 4.4 The Auto DJ page

Rows: On, Anchor, Strictness, **Smart filters**, BPM, Key, Status.

| State | Trigger | Drawn | A tap |
|---|---|---|---|
| No filter | Moods, `q` inactive | "Smart filters", value "Any", hint "keeps every pick inside these moods" | Opens the editor |
| Filter on | Active | Value is the summary, beside the title if it fits, else on line 2, cut at 288 | Opens the editor |
| No mood data | No block | Dim, value "No mood data". Saved filter kept, picks unfiltered | Amber note, `toast.autodj_no_mood`. This branch comes before the `toast_autodj_needs_index` check at `src/ui/OutputPage.cpp:1136-1139` |
| Moods, no index | Word block, no usable MPDJ | This row live, the others dim. Picks random inside the filter | Opens the editor |
| Behind the fence | `index()` null | Last value, last status | `library.updating_wait` |
| Status: filter off, no data | | As today, byte for byte | |
| Status: filter off, data | | Line 2 adds mood: "BPM 95% · key 97% · mood 92%" | |
| Status: fit > 50 | Index Ready | "1,204 of 19,412 tracks fit" / coverage | |
| Status: 1-50 | `kTrackCooldown` (`lib/core/AutoDjEngine.h:142`), index Ready | Amber "Only 37 tracks fit" / "They will repeat · BPM 95% · key 97%" | |
| Status: 0 | `Hold::NoFit`, index Ready | Amber "Waiting: no track fits the filters" / coverage. Root "Waiting · Follow the vibe" | |
| Status: filter on, no usable index | Index None, Bad, Unreadable, TooBig or NoMemory | The fit, few or waiting line over the index's first line, the held form of `lib/core/AutoDjView.cpp:129-135`. No coverage: `Maps::withBpm`, `withKey` exist only after a join (`lib/core/AutoDjJoin.h:150-151`). No new string. No frame on the canvas | |
| Status: Found, Reading | Index on the card, not joined | As today | |
| Other holds | Repeat One, sleep timer, failures | Why over the index's first line, as today | |
| "..." sheet | Filter on | Detail "On · Relaxed · Instrumental" in the accent. Amber "Waiting" on NoFit. DIM otherwise | As today |
| Empty states | `moodOn && fit == 0` | "No track on this card fits these filters", [Open Library], no second button | |

A filtered repeat is not a neighbour repeat. Today, with every neighbour cooled, the draw repeats a neighbour (`lib/core/AutoDjEngine.cpp:497, 620-626`). Under a filter the pick jumps first, then repeats the oldest track in the mood, listed or not (6.2).

### 4.5 Toasts

| Toast | Kind | Fires | De-duplication |
|---|---|---|---|
| Only {n} tracks fit: they will repeat | warn | While on and 1 <= fit <= 50: the editor closing or `Dq` with a changed filter; `setOn(true)`; the first appended pick after a boot or a `playFrom` switch-on | `fewSaid_`, set only when raised. A changed filter clears it first. Also cleared when few goes false and at switch off, never at `sessionBegan` |
| No close match: any track that fits | toast | An appended pick with `jump != None`, tested before `noIndex` at `lib/core/AutoDjView.cpp:151` | `jumpSaid_`, set only when raised, reset at `sessionBegan` and at a settled change |
| Plays once, then the filters apply | toast | A settled change where `pendingTrack()` fails the new filter | Once a settle |
| No mood data: sync this card from mStream | warn | A tap on the dim row | None |
| Not enough memory for that | toast | The Albums array can't be carved | None |

- News bits: `kNewsFew=32`, `kNewsJump=64`, `kNewsOnce=128` (`lib/core/AutoDjView.h:132`). The byte is then full.
- Priority: Paused, Cued, SeedMoved, NoNeighbours, Random, Few, Once, Jump. The old five keep their order, so with no filter `newsToast` answers as today.
- `newsToast` returns one toast a pass and drops the other bits (`lib/core/AutoDjView.cpp:162-178`). So Few, Once and Jump are raised only in a pass with no bit above them. An unraised Few or Jump stays unsaid and tries again at the next appended pick. An unraised Once goes: its moment has passed.
- So a card with no index and 37 tracks that fit says Random first, because `NewsWatch` marks it said at once (`:151-153`), and Few at the next append.
- `Toast` gains a count, because `src/ui/Ui.cpp:429-431` passes a bare id.
- While the editor is up, `Ui::autoDjNews` holds all news, Paused included. That is a change from `src/ui/Ui.cpp:420`. `src/ui/Ui.cpp:1398` gains the same condition.
- A settle at the fence says nothing, because its fit count is old.
- Two-line behaviour follows the code (3.2).

### 4.6 Persistence

| What | Where | Life |
|---|---|---|
| Albums filter | `LibraryPage::filter_`, RAM | Survives `leave()`, `enter()`, `resetLibraryTab()`. Not a boot (D6) |
| Filtered ids | One block of u32s, RAM | Only while a filter is on. Dropped at `libraryUpdating()` and `libraryChanged()`. `buildStamp` can't guard it: it holds no moods (`lib/core/LibraryIndex.h:451-453`) |
| Auto DJ filter | NVS `autodj/moods`, u16, via `nvslater::putU16` | Saved. Absent or invalid reads as no filter |
| Word | `library.idx` v7 | Rebuilt from T |
| Scores | T's tail on the card | Until a writer changes them |

## 5. The data

### 5.1 The route

`POST /api/v1/db/mood-scores`, behind the wall, read-only.

Request: `{ "filepaths": [string, 1..2000] }`, Joi strict. A bare array, unknown key, empty list, 2001 paths or a non-string gives 400. A body over `maxRequestSize` (1 MB, mStream `src/state/config.js:898`; parser at `src/server.js:425`, mapped at `src/util/web-error.js:76, 106`) gives 413 before the route runs. The writer halves on that.

```json
{ "model": {"id":"effnet-discogs","version":"1"}, "moodVersion":"1",
  "heads":["danceable","happy","sad","relaxed","aggressive","party","acoustic","instrumental"],
  "tracks":{
    "music/Artist A/scored.flac":[0.81,0.29,0,0.7,0.1234,1,0.57,0.58],
    "music/Artist A/null-head.flac":[0.81,0.29,null,0.7,0.1234,1,0.57,0.58],
    "music/Artist A/all-null.flac":[null,null,null,null,null,null,null,null],
    "music/Artist A/no-row.flac":null,
    "hidden/Artist B/x.flac":null }}
```

| Case | `tracks[path]` | Rule |
|---|---|---|
| Visible, scores | Array in `heads` order | The stored double, unrounded. 0 and 1 print as integers |
| Visible, NULL head | `null` at that index | Never 0. Fails only its own scale, stays in an album's denominator |
| Visible, all NULL | Array of nulls | Must differ from no row, for the same denominator reason |
| Visible, no row | `null` | |
| Not visible, unknown library, `..`, deleted, never existed | `null` | Must be identical to no row. Status stays 200: `getVPathInfo` throws 404s (mStream `src/util/vpath.js:31, 36`), and the batch catches them per path (`src/api/db.js:522`). The route keeps that catch |
| `analyzeMoods` off, or empty table | Every path `null` | Follows `moodFiltersReady()` (mStream `src/api/mood-filter.js:117-120`). `moodVersion` null when off |

- Every path asked for gets a key, spelled as sent. Build with `Object.fromEntries`, so a path named `__proto__` still gets one.
- Clients index by name in `heads`. `MOOD_COLUMNS` is mStream `src/db/mood-sync.js:29`. A reply lacking any of the eight names reads as an error.
- `moodVersion` is the active pin, not the row's: `activeMoodVersion(scanOptions)` (mStream `src/db/discovery-features-lib.js:331-334`).
- `model` is `{id, version}`, from `EMBEDDING_MODELS[scanOptions.discoveryModel]` (mStream `src/db/discovery-features-lib.js:91`), `id` being the table's key. Not `modelBlock` (`src/api/discovery.js:183`), which reads the loaded discovery index. `moodFiltersReady()` can be true with none loaded.
- The route adds no rounding and no `|| null`. A `||` would turn a 0 score into NULL. The server writes `Math.round(mean*1e4)/1e4` (mStream `src/db/discovery-features-lib.js:472`).
- Flag: `features.moodScores: 1` on `/api/` and ping, a version fact present whether or not scores exist.
- Allowlist: one line in `ALLOWED_EXACT` (mStream `src/api/federation-auth.js:57`). The same table is the proxy's outbound screen (mStream `src/api/federation-browse.js:287`).
- Size: 101-171 B a track for paths of 40-110 characters. That is 2.0-3.4 MB at 20k, 202-342 KB a 2000-path call (MODELLED).

### 5.2 Who asks, and when

**The gate is a flag, never a version or a probe.** The writer asks its own server when `features.moodScores` is present. A 403 or 404 still reads as no moods and never fails a sync.

The writer does not read `moodFilters`. The field is not on the stack's `LayeredFeatures` (terminal `src/api/types.rs:166-190`), and terminal main already has one at `:180`. Adding it would make a duplicate. The route already follows `moodFiltersReady()`. The cost: a server with the flag and no rows is asked at each sync, about 1.0-2.4 MB at 20k (MODELLED).

**The fetch rule**, at the final commit, in `impl Hooks for SyncHooks`:

1. Card for someone else: `Moods::Clear`. No fetch, kept bytes go. Not gated on `settings.auto_dj`.
2. `no_moods` or stop set: `Keep`.
3. Some record scored, `same_files`, not `refresh`: `Keep`. `refresh` is terminal `src/device/card/sync.rs:1001`. With no record scored it asks, so the first sync after an upgrade needs no whole read.
4. Otherwise fetch from the own server, origin 0: records with `l.server == server` and a non-empty `server_path`, no hash filter. No flag: `Keep`.

| Reply | Result |
|---|---|
| `Ok(None)` or `Err` | Rows keep their bytes, plus a note |
| A row | `encode`, by head name |
| `null` | `[0;8]`: clears |
| Key missing, or a row whose length differs from `heads` | Keep |
| `model`, `moodVersion` or `heads` moving between chunks | `ApiError::Decode`, all rows kept |

The overlay runs inside `if last`, before `unchanged()` (terminal `src/device/card/engine/run.rs:1281`). So equal scores make no commit.

### 5.3 MPTG minor 1 and the tail

With a RECS stride of 80 or more, bytes 72-79 of a record hold the mood scores. A stride under 80 means no record has one. **72 stays the smallest stride a reader accepts.** Raising `kRecsStride` (`lib/core/CardTags.h:43`) would make every existing card fail `lib/core/CardContainer.cpp:157`. The terminal has the same check at `src/device/card/contract/container.rs:184`. Only sources 2 and 3 write a tail. D stays at 72. The reader does not consult the minor.

| Off | Field | Meaning |
|---|---|---|
| 72 | moodFlags | bit 0 PRESENT; bit 1 sad beat happy; bit 2 aggressive beat relaxed; bits 3-7 are 0 |
| 73 | valence | the larger of happy and sad, 0-100; 255 if either is NULL |
| 74 | energy | the larger of relaxed and aggressive, the same |
| 75 | instrumental | 0-100; 255 NULL |
| 76 | acoustic | 0-100; 255 NULL |
| 77 | party | 0-100; 255 NULL |
| 78 | danceable | 0-100; 255 NULL |
| 79 | reserved | 0 |

- **A value** is `min(100, max(0, floor(score*100 + 1e-9)))` in f64. Plain floor is wrong at 0.29, 0.57, 0.58. The slack form had 0 wrong of 10,001 scores, 0 within ±4 ULP, 0 of 100,001 on a 5-decimal grid (MEASURED-HOST, CPython f64, SQLite 3.37.2). So serde_json's default parse (terminal `Cargo.toml:68`) cannot move a byte.
- **Order bits** come from the raw doubles: bit 1 when sad > happy, bit 2 when aggressive > relaxed. A tie is happy, relaxed. A writer writes 0 when either head is NULL. A reader ignores an order bit whose value byte is 255.
- **No server row:** 8 zero bytes.
- **Validity.** A reader takes a tail only if bit 0 is set, bits 3-7 are 0, byte 79 is 0, and each of bytes 73-78 is 0-100 or 255. Otherwise that record has no scores, and the record and the file are fine. So `mptg-minor-1`'s 0xAB tail (`tools/card_fixtures.py:490`, flag bits 0xA8) reads as its base (MEASURED-HOST).
- **Reserved:** bits 3-7 and byte 79 never get a meaning in major 1. A later field goes from byte 80.
- **Writing:** stride 80 and minor 1 when a record has a tail, else stride 72 and minor 0. A card with no moods stays byte for byte.

METADATA needs three amendments: `docs/METADATA.md:752` (the missing tail, and the 72 minimum), `:766` (the reserved bits are an exception to "new flag bits are ignored"), `:788` (the minor follows the data).

### 5.4 The word

`library.idx` v7 holds a u16 per track id, built from the tail when T's record is the one chosen.

| Bits | Field |
|---|---|
| 15 | analysed |
| 14 | voice NULL |
| 13-11 | valence |
| 10-8 | energy |
| 7-6 | instrumental |
| 5-4 | acoustic |
| 3-2 | party |
| 1-0 | danceable |

- Buckets: 0 under 30, 1 for 30-49, 2 for 50-69, 3 for 70 and up.
- A scale field is `side<<2 | bucket`. 4 means NULL. Bucket 0 stores 0.
- A NULL tag stores 0, which never holds.
- Word 0: no row. No word on any track: no block, 0 B.
- Reading a level, `need/under`: Strict 3/1, Standard 2/2, Loose 1/3.
- Exact at the three bars: 677,490 checks against mood-filter.js's SQL text run by SQLite 3.37.2, 0 wrong (MEASURED-HOST).

Header: `kVersion` 7, `kCountWords` 24 (`lib/core/LibraryIndex.cpp:155, 160`). `h[22]` is mood words (0 or the track count), `h[23]` tracks with a word. The words go last, before the sum. `computeStamp` is unchanged: a word doesn't change what a track id means.

**The filter's 11 bits (`q`):**
- 0-1 valence: 0 none, 1 Happy, 2 Neutral, 3 Sad
- 2-3 energy: 0 none, 1 Relaxed, 2 Neutral, 3 Aggressive
- 4-5 voice: 0 none, 1 Instrumental, 2 Vocals, 3 invalid
- 6-8 tags: 1 Acoustic, 2 Party, 4 Danceable
- 9-10 level: 0 Standard, 1 Strict, 2 Loose, 3 invalid

Any bit 11-15, voice 3 or level 3 makes the whole word invalid, and it reads as 0. An absent key is 0: no filter, Standard.

### 5.5 Compatibility

| Combination | Result |
|---|---|
| Released firmware, stride 80 | Reads 72, skips the rest (`lib/core/CardTags.cpp:275, 315, 542`) |
| New firmware, stride 72 | No moods, reads as no data |
| Older terminal rewriting T | Drops the tails, legal (`docs/METADATA.md:755-759`). The next new run derives them |
| Other readers, stride 80 | Accepted by prefix (terminal `src/device/card/contract/container.rs:184`) |
| New writer, old server | No flag, no call |
| `library.idx` v6 on new firmware | `Load::Outdated`, one rebuild (`lib/core/LibraryIndex.cpp:1682`) |
| `library.idx` v7 on old firmware | Corrupt, rebuilds its own |
| A unit alternating old and new firmware | A rebuild at every boot: old reads v7 as Corrupt and saves v6, new reads v6 as Outdated and saves v7. 11.3 s at 20k (MEASURED). No data risk |
| NVS, either way | Absent key reads as no filter. Old firmware never reads it |
| A sync that changes only scores | One commit, one device rebuild at the next boot: 11.3 s at 20k (MEASURED, `docs/METADATA.md:4531`) |

### 5.6 Freshness

Scores are a snapshot. `manifestRevision` ignores moods (mStream `src/api/sync.js:40`). A mood-only change is caught at the next whole read: 7 days or the 10th run (terminal `src/device/card/sync.rs:115`).

On a card with a peer read, `refresh` holds every run, so the own server is asked every run. The backfill takes `discoveryPerRun`, 50 a pass (mStream `src/state/config.js:224`), so a backlog or a head bump means several commits, not one.

A cut run's checkpoint carries rows as held, so two head versions can sit in one T. There is no per-row marker.

### 5.7 A worked example

Made-up track `music/Artist A/song.flac`. In `MOOD_COLUMNS` order: `[0.7012, 0.3104, 0.5803, 0.7, 0.2999, null, 0.4999, 0.29]`.

| Step | Result |
|---|---|
| Flags | present 1; sad .5803 > happy .3104 sets bit 1; aggressive .2999 is not > relaxed .7. `0x03` |
| Valence | larger of happy and sad, .5803: 58, `0x3A` |
| Energy | larger of relaxed and aggressive, .7: 70, `0x46` |
| Instrumental | .29: `floor(28.999999999999996 + 1e-9)` = 29, `0x1D` (plain floor gives 28) |
| Acoustic | .4999: 49, `0x31` |
| Party | NULL: `0xFF` |
| Danceable | .7012: 70, `0x46` |
| Tail 72-79 | `03 3A 46 1D 31 FF 46 00` |
| Word | analysed 1, voice NULL 0, valence 6 (side 1, bucket 2), energy 3, instrumental 0, acoustic 1, party 0, danceable 3: `0xB313` |

Filter `0x127`: Sad, Relaxed, Vocals, Danceable at Standard (need 2, under 2).

| Clause | Word | SQL |
|---|---|---|
| Sad | bucket 2 >= 2, side 1 | `sad >= 0.5 AND sad > happy` |
| Relaxed | bucket 3 >= 2, side 0 | `relaxed >= 0.5 AND relaxed >= aggressive` |
| Vocals | bucket 0 < 2 | `instrumental < 0.5` |
| Danceable | bucket 3 >= 2 | `danceable >= 0.5` |

Word true, SQL 1. Seven such rows times 33 checks: 231, 0 wrong (MEASURED-HOST).

An album with 5 passing of 9 analysed matches, since 10 > 9. 2 of 4 does not. 0 of 0 does not.

## 6. The engine

### 6.1 Predicate placement

`buildRun` (`lib/core/AutoDjEngine.cpp:379`) skips a row whose first track fails. A run then holds only in-mood rows, so `merge`, census, gate, `closest` and `choose` need no change.

```cpp
struct MoodView { const uint16_t* words; uint32_t tracks; uint16_t q; moodfilter::Filter f;
                  bool on() const; bool passes(uint32_t track) const; };
```

- `Maps::mood` goes after `lib/core/AutoDjJoin.h:154`.
- `PickIn::mood` too (`lib/core/AutoDjEngine.h:179`), because `byId` runs with `maps` null (`lib/core/AutoDjEngine.cpp:525`).
- `Join` is untouched. `AutoDj::index()` (`src/app/AutoDj.cpp:292`) copies its maps into a member and sets `.mood`.
- `MapsPrint` (`lib/core/AutoDjEngine.h:410`) gains `moods` and `q`, set only when `on()`, else null/0. So with no filter the print is today's.
- A changed `q` makes runs stale through `runFresh` (`lib/core/AutoDjEngine.cpp:361`).
- `mapsChanged()` covers an index refilled at the same address.

### 6.2 Fallback order, filter on

Sites are in `lib/core/AutoDjEngine.cpp`.

| Order | What | Site |
|---|---|---|
| 1 | Gate | `:594`, unchanged |
| 2 | Dry 50 | `:616-619`, unchanged |
| 3 | Loosen 50 | `:620-626`, unchanged |
| 4a | No in-mood candidate: re-pin, else `jump = NoCandidate`, `randomPick` | `:613` |
| 4b | All cooled, `closest(true)` found 0: as 4a with `AllCooled` | `:621-625` |
| 5 | `randomPick`'s first line diverts to `moodPick`. Also reached at `:574` | `:644` |
| 6 | The scan. A fresh row in the sample: `choose()`, `finish()` | new |
| 7 | No fresh row: a fresh track with no row, by id, `jump = NoRow` | new |
| 8 | Repeats, oldest first in `ses_.cool`: first row not playing, with a track, passing. `repeated = true`, level 2 | new |
| 9 | `byId`'s first line diverts to `moodById`: fresh, else oldest in `ses_.ids`, else the playing track, else `ok=false, noFit=true` | `:741` |
| 10 | The Waiting hold, before any pick | 6.5 |

`:663`'s fall-through to `byId` cannot be reached with a filter on, since `:644` diverts first. `Pick` gains `jump`, `repinned`, `repeated`, `noFit`. `Stats` gains `jumps`, `repins`, `moodRepeats`, `noFit`.

Step 4b changes the repeat rule under a filter. With no filter, `closest(true)` finding nothing leaves `cand_` and the counts as the cooled pool, and `choose()` repeats a neighbour (`:497`, `:620-626`). `docs/AUTODJ.md` 3.8 says so once this is built.

### 6.3 The scan

One pass in track order over `words` and `trackToRow`, skipping the playing row and the anchors.

- The first 64 hits go into `keys_`. After that, `j = rng.below(seen)` replaces one if `j < 64`.
- No count pass and no cached state.
- For each kept track: `r = trackToRow[t]`, kept only if `rowToTrack[r]` passes and `r` is not already kept. `trackToRow` is many-to-one, and `finish` plays `rowToTrack[row]` (`lib/core/AutoDjEngine.cpp:727`), so testing the track alone could leak.
- Then `sc.bits()` and `choose()`, so a jump keeps the artist cooldown and the BPM and key tiers.

Cost: about 2,434 sequential lines plus at most about 192 random reads, 2.4-8.5 ms (MODELLED, never run). No allocation.

**The Auto DJ button.** Both paths branch on `mood.on()`. With no filter they are today's code, untouched: `Engine::surprise`'s 64 draws (`lib/core/AutoDjEngine.cpp:785-792`) and `Driver::surpriseTrack`'s 8 tries, each a path hash and a DJPH lookup (`lib/core/AutoDjDriver.cpp:419`, `src/app/AutoDj.cpp:285-286`). `test/test_autodj_driver/test_autodj_driver.cpp:1543-1550` keeps pinning those.

With a filter on:
- `Engine::surprise` uses the scan with no exclusions, then `cand_[below(n)]`.
- `surpriseTrack` does one pass over the words, 2-5 ms (MODELLED), with a reservoir of `kSurpriseTries` passing tracks, then `hasRow` on each: at most 8 lookups, so playback still never waits for a join.

### 6.4 The re-pin rule

At 4a or 4b, in `Mode::Locked`, once a pick, if `playingRow` exists, is not the pin, is not damaged and passes: `ses_.pin = playingRow`, `ses_.pinMoved = true`, `out.repinned = true`, and the anchors run again.

- **No re-indent:** the body of `Engine::pick` (`:523-642`) becomes `pickOnce(s, in, out, &again)`, and a small `pick()` calls it at most twice. No recursion. Round 1 draws no random number before re-pinning.
- **Budget:** round 2 shares `out.reads`. If the budget is spent or the read fails, `nu == 0` at `:569`, then the scan. The moved pin stays for the next read ahead.
- **Round 2 finds nothing:** a jump, with both `repinned` and `jump` set.
- **Quick skip:** in `rowLeft` (`:239`), after the counter, `if (ses_.pinMoved && row != kNoRow && row == ses_.pin) { ses_.pin = kNoRow; ses_.pinMoved = false; }`. A seed's pin stays, as today. `modeChanged` clears `pinMoved`.

Numbers (SYNTHETIC, 19,410 rows). Jump % at f = 5 %, ρ .7, in-mood seed:

| Strategy | Jump % |
|---|---|
| Plain ladder | 70 |
| Re-pin | 0 |
| Seed hop | 7 |

Re-pin's honest cost: cosine to the seed falls from .69 to .47 at f = 5 %. Stay on seed drifts toward Follow the vibe.

### 6.5 The hold and the fit count

- `Wait::NoFit` goes last in `lib/core/AutoDjTrigger.h:46-58`, `Hold::NoFit` in `:62-67`, `TriggerIn::noFit`.
- `lib/core/AutoDjTrigger.cpp` gets the name "no track fits the filters", last in the hold chain, the wait after Failures.
- `owed` needs nothing.
- `rowState` returns Waiting (`lib/core/AutoDjView.cpp:26`).
- `noFit` is `mood.on() && fit == 0`, so a hold comes only from real data.

`AutoDj::stepMood()` runs first in `step()` (`src/app/AutoDj.cpp:322`) and synchronously in `setMoods`. It recounts, 1,217 lines, 2-5 ms (MODELLED), then calls `mapsChanged()`, in two cases:
- `q` changed;
- `lib_.index()` went from null to non-null: the first pass, and the end of each fence.

`buildStamp()`, `trackCount()` and the pointer's value cannot be the key. `Library::index()` returns the same object before and after a fence (`src/app/Library.h:61`, `lib/core/LibraryUpdate.h:361`), the stamp hashes names and folders only (`lib/core/LibraryIndex.cpp:1423-1438`), and a score-only commit leaves the count alone. Those keys would keep the old card's `fit_` and `Hold::NoFit`.

Behind the fence the view goes off but `moodData_`, `fit_` and `moodPct_` keep their values. Otherwise the row would read "No mood data" for 15-18 s.

### 6.6 No-filter parity

Three layers:

1. Every new branch is guarded by `mood.on()`, `pinMoved` or a default-false field, the Auto DJ button's two paths included (6.3). No `below()` is added off those paths. The print normalises to today's.
2. The 26 sessions replay, plus a hash lock (S4).
3. MEASURED-HOST: a prototype over `tools/autodj_ref.py` with the filter off gave 26 of 26 sessions identical, new counters zero, no-data equal to control over 30 picks. Its predicate was a toy, so re-check with real words.

## 7. The UI

### 7.1 The editor's geometry

Opaque panel, y 36-239, card CARD with top r 12, tab bar stays. It paints its own card, since `sheetCard`'s handle (`src/ui/Overlays.cpp:37-41`) would hit the title row.

| Element | Geometry |
|---|---|
| Clear pill | x 12, y 40, w word+24, h 24, r 12, BTN. Small. TXT if anything is set or the level isn't Standard, else FAINT |
| Cross pill | x 262, y 40, 50x24, r 12, BTN; icon at 287, 52 |
| Count | Small, centred between Clear and x 256, y 52, room `256 - (12+pw) - 8`. SOFT, amber at 0 |
| Rows | 36 tall, pitch 44, y 68, 112, 156, 200, x 8-311 |
| Row 1, 2 tray | w 304, r 10, BTN. Inner width 300 from x 10 |
| Row 3 tray | Voice, w `304 - 6 - lw`. Inner width w-4 from x 10 |
| Level cell | w `lw = max(widest Small level word + 14, 70)`, x `8 + trayW + 6`, r 10, FAINT outline. Label Small y+12. Three 5 px dots at y+27, dx -10, 0, 10. Accent when live, DIM when dim, FAINT for the others |
| Row 4 chips | Total 304 from x 8, gap 6, r 18, BTN |
| Picked segment | `(ix, y+2, iw, 32)`, r 8, accent, DARK text. Pressed SOFT. Unpicked pressed BTN_HI |
| Separators | 1 px DIV, y+10 to y+25, only between two unpicked neighbours |
| Picked chip | Accent, DARK. Pressed SOFT. Unpicked pressed BTN_HI |

### 7.2 The first row's geometry

An album row's metrics: 42 tall, scrolls with the list, divider ROW_DIV at y+41. `right` is 284, 312 or 320.

| State | Geometry |
|---|---|
| At rest | Funnel at 26, y+21, DIM. "Filters" Body x 54, y+13, room `right-80`. Count Small x 54, y+31, DIM. Chevron FAINT at `right-14` |
| On | Plate x 6, y+1, 40x40, r 4, BTN_HI; funnel in the accent. Disc r 14, BTN, at `cx` = 232, 284, 292; cross TXT. Summary x 54, y+13, in the accent: Body, else Small, then cut, room `cx - 72` = 160, 212, 220. Count Small DIM, same room |
| Pressed | ROW_SEL |
| Dot | `fillRoundRect(203, 15, 6, 6, 3, accent::Library)` in strip coordinates, so y 54 on screen. `fillCircle` draws 7 px wide, the frame's r 3 at x 205.5 is 6. Gap to the label: ru 1.5 px, es 4.5, others 9 and up (MEASURED-HOST) |

Summary order: valence, energy, voice, tags, then the level if not Standard, joined by " · ". Worst case ru 186 B, so the buffer is 200 B.

The row's long count falls back to the short form when over its room: ru albums at 9,999 of 9,999 is 165 in 160. Counts use `uistr::grouped`.

### 7.3 Layout rules

The canvas's rules, in integer arithmetic, in `lib/core/FilterGrid`.

- **Grid font:** Body when every row fits (`sum(word + 12) + gaps <= total`), else Small for the whole grid. Only pl and ru take Small (MEASURED-HOST).
- **Widths per row:** equal while each word + 12 fits. Otherwise proportional: `need = word + 12`, each cell `need + spare/n`. If Small still overflows, cells scale to the row (`voice` in pl, ru).
- **The ru tag:** a tag over 80 px Small with a space wraps at the space. Its need is its wider line + 12. Glyphs only (fg == bg, as `src/ui/Overlays.cpp:220-223`), at y+10 and y+26.
- **Goldens to pin:**

| Language | Row | Cells (x, w) |
|---|---|---|
| en | valence | (10,100)(110,100)(210,100) |
| en | voice + level | (10,136)(146,86), level 240/72 |
| en | tags | (8,97)(111,98)(215,97) |
| ru | valence | (10,86)(96,120)(216,94) |
| ru | energy | (10,91)(101,106)(207,103) |
| ru | voice + level | (10,144)(154,80), level 242/70 |
| ru | tags | (8,103)(117,80)(203,109), wrap Для / вечеринки |
| pl | voice + level | (10,110)(120,85), level 213/99 |
| ja | tags | (8,140)(154,92)(252,60) |

### 7.4 Hit tests

y reads true (`lib/core/TouchCalibration.h:30`). x reads right.

- **Bands:** title under y 66; rows at 66, 110, 154, 198 to the edge.
- **x splits:** cell boundary + `skew(x)`, where skew is 0 under x 150, 6 under x 190, else 13. A gap between chips splits at its middle. The last cell takes clamped readings (`lib/core/InputEvent.h:56`).
- **Audit:** every cell centre in every language hits its own cell on the lab panel and on a true panel, smallest margin 20 px (MEASURED-HOST). Plain boundaries also pass, but on the lab panel 26 px of a middle cell hits the third. The host test requires at least 16.
- **Title row:** Clear up to its pill's right edge + skew. Cross from x 250, as `kSheetCrossHitX` (`src/ui/Overlays.cpp:370`). The count is dead.
- **First row:** cross zone raw 224, 276, 284 to the edge, stopping at the rail's 280 (`src/ui/ListView.cpp:710`). On the lab panel the last 14-18 px of summary text hits the cross. That is accepted: the hit is on the row's own text, not on an album. The rest opens the editor.
- **Settle:** a Down within `Sheet::kSettleMs` of opening goes nowhere. Today that test names two sheets only (`src/ui/Ui.cpp:1660-1661`). `openFilters()` sets `sheetOpenedMs_`, and the test gains `filters_.up()`.

### 7.5 Redraw rules

- Down: the tray or chip alone.
- Tap, in order: new state, push the tray or chip, run the pass, push the count. The finger's answer never waits on the pass.
- Level cell and Clear pill are pushed only when "something set" flips. Clear redraws the whole grid (about 55 ms, by the sleep sheet's figure, MEASURED).
- Close with no change: the band pushed, no re-render. Under a modal the list draws no frames (`src/ui/Ui.cpp:1430-1432`).
- Close with a change: `reload(); scrollTo(0)`, `ref_->scrollPx = 0`, once. `setEdge()` then re-derives the width (`src/ui/ListView.cpp:85-94, 167-176`). `reload()` only marks the list, so a close through a tab tap wastes no draw.
- Dot: redrawn only when `!list.animating()`.
- `Ui::updateStatus` must not touch the band under a modal (S0).

### 7.6 Where the code goes

- **The editor is an Overlay,** not a pushed page. The panel sprite is exactly its size (`lib/core/SheetLayout.h:31`). `closeModal(true)` already fires from tab taps, language and the fence (`src/ui/Ui.cpp:535, 589`). `listMoving()` stays false under a modal (`src/ui/Ui.h:158`). A pushed page would need a new `PageKind`, two tab stacks and a whole re-attach on each close.
- **`lib/core/FilterGrid.{h,cpp}`:** pure and host-tested. Cell rects, font choice, wrap, hit test, tap semantics, summary, count text, first-row and dot rules. Takes `textfit::Font` views as `src/ui/LibraryPage.cpp:400` does.
- **`src/ui/FilterSheet.{h,cpp}`:** the drawing. A separate file stays out of #27's way in `src/ui/Overlays.cpp`. `panel_` (`:26-29`) gets a small accessor. `FilterOwner`: `filterWord()`, `setFilterWord()` returning bool, `filterFits()`, `filterTotal()`, `filterScope()`, `onFiltersClosed(changed)`, told only when `notify` (4.3).
- **The first row** reuses the `ListLayout` top-bar item with a new `Source` hook: `topBarIsRow()`, `drawTopRow(Row&)`, `onTopRowTap(x, rightEdge)`. A data row would break "the first `railRows()` rows are sorted" (`src/ui/ListView.h:178`). `renderBar` always paints the first button primary (`src/ui/ListView.cpp:288`), and `actions(-1)` answers Folders' Play all (`src/ui/LibraryPage.cpp:736`), so neither is called for it.
- **The empty line needs `ListView`.** `render` calls `drawEmpty` only at 0 items (`src/ui/ListView.cpp:348`), and the row is an item. With rows 0 under a top row, draw the line: strip at y 114, text at y 150.
- **`lib/core/FilteredAlbums.{h,cpp}`:** a subsequence of `albumsAZ()`, `albumCount` u32s, allocator hooks as `lib/core/BitSet.h:12-17`. `LibraryPage` owns it, and `rowAt`, `rows`, `railRows` read it (`src/ui/LibraryPage.cpp:96-98, 533, 560`). The rail, jump grid, covers and `playingRow` follow from `rowAt` (`:584-593`).

## 8. Strings

Rule: words from the phone app's ARB where it has them (`i18n/README.md:160-163`). A text a translation leaves out is English on the device, so a commit with English alone stays green (`i18n/README.md:43-44`). Each stage adds its own ids. Room is the font and px; for a word in a real cell, `test_ui_library` checks each language's real room.

### 8.1 From the ARB: no new translation owed

Source: phone app `lib/l10n/app_en.arb`.

| Id | English | Room | ARB key:line | Stage |
|---|---|---|---|---|
| mood.filters | Filters | Body 204 | moodFilters:887 | S3 |
| mood.happy / neutral / sad | Happy / Neutral / Sad | Body else Small 116 | :907, :911, :915 | S3 |
| mood.relaxed / aggressive | Relaxed / Aggressive | Body else Small 116 | :919, :923 | S3 |
| mood.instrumental / vocals | Instrumental / Vocals | Body else Small 140 | :927, :931 | S3 |
| mood.acoustic / party / danceable | Acoustic / Party / Danceable | Body else Small 136 | :935, :939, :943 | S3 |
| mood.strict / standard / loose | Strict / Standard / Loose | Small 93 | :947, :951, :955 | S3 |
| mood.clear | Clear | Small 84 | clear:1090, not `queue.clear_confirm_button` | S3 |
| mood.smart_filters | Smart filters | Body 288 | autoDjMoodTitle:728 | S5 |
| mood.any | Any | Small 288 | autoDjAny:5864 | S5 |
| mood.no_data | No mood data | Small 288 | autoDjNoMoodData:5906 | S5 |

No missing glyphs and no fullwidth forms in these words (MEASURED-HOST).

`THIRD-PARTY-NOTICES.md:461-463` names ARB commit bdb174e, which holds none of the keys sampled from this table (READ, `git show`). These were read at d17c3fa5, so S3's commit 2 changes that commit in the notice.

### 8.2 New: translation owed

Nine languages each, 126 texts, machine-translated, native review owed.

| Id | English | Room, buffer | Stage |
|---|---|---|---|
| mood.albums_of (plural) | {0} of {1} album(s) | Small 160 and 178, optional, 64 B | S3 |
| mood.tracks_of (plural) | {0} of {1} track(s) | Small 178, optional, 64 B | S3 |
| mood.no_albums | No albums match these filters | Body 300 (`kListLineW`), en 240 | S3 |
| autodj.status_waits_fit | Waiting: no track fits the filters | Small 288, 96 B | S4 |
| autodj.smart_sub | keeps every pick inside these moods | Small 288, en 242 | S5 |
| autodj.status_fits (plural) | {0} of {1} tracks fit | Small 288, 96 B | S5 |
| autodj.status_coverage_mood | BPM {0}% · key {1}% · mood {2}% | Small 288, en 233, 96 B | S5 |
| autodj.status_few (plural) | Only {0} tracks fit | Small 288, 96 B | S5 |
| autodj.status_few_sub | They will repeat · BPM {0}% · key {1}% | Small 288, en 245, 96 B | S5 |
| toast.autodj_few (plural) | Only {0} tracks fit: they will repeat | toast, en one line (269 of 284) | S5 |
| toast.autodj_no_close | No close match: any track that fits | toast, en one line (275) | S5 |
| toast.autodj_plays_once | Plays once, then the filters apply | toast, en one line (260) | S5 |
| toast.autodj_no_mood | No mood data: sync this card from mStream | toast, en two lines at ": ", `two_line_px` 284, 128 B | S5 |
| empty.no_track_fits | No track on this card fits these filters | Small 304 (`kEmptyLineW`), en 242 | S5 |

`toast.autodj_plays_once` has no ": ". A language that doesn't fit one Body line needs one, or `test_ui_strings` fails it.

Reused, no new text: `library.albums_n` (`i18n/en.json:2003`), `library.tracks_n` (`:1976`), `queue.summary_position` (`:2835`), `toast.no_memory` (`:274`), `library.updating_wait` (`:2530`), `output.autodj_sub_on` (`:3552`), `now_playing.autodj_waiting`.

## 9. The stages

### 9.1 Rules, and the dependency diagram

- Host tests stay green at every commit: `pio test -e native` on the firmware, from PowerShell without MSYSTEM.
- Build only one terminal worktree at a time, with `CARGO_INCREMENTAL=0`, and never share `CARGO_TARGET_DIR` across worktrees.
- A stage stops only at a PR boundary.
- Device checks run on a spare card until S8.

**Fixtures flow one way.**
- mStream's predicate text is copied, with its commit named, into `tools/test_mood_vectors.py`.
- `tools/card_fixtures.py` on the player's repo is the only generator of card fixtures.
- The terminal copies its output and never edits it.
- mStream takes no file from either. Its own test ports the byte rule.

```
S0 (free-standing)

S1 --> S2 --> S3 ---------+
 |      |                 +--> S5 ---------+
 |      +---> S4 ---------+                |
 |                                         v
 +--(spec, fixtures)--> S7 ------------>  S8
S6 --(route contract)-> S7                 ^
S6 ----------------------------------------+
terminal #56, #68 ----> S7 (stacked)
```

### 9.2 Where it can stop

| Stop after | The user sees | Useful as |
|---|---|---|
| S0 | A bug fix | Alone |
| S1 | Nothing | The contract the others copy |
| S2 | One `library.idx` rebuild at the first boot. Console only | Readers ready for scored cards |
| S3 | The first row, only on a card with moods. No real card has them until S7 | Album filters, end to end with S6 and S7 |
| S4 | Nothing: console `Dq` only | Engine proved on a synthetic card |
| S5 | A dim row on every card without moods, whose note says to sync from mStream | Merge, but **hold it out of a release until a writer build carries scores** (assumed) |
| S6 | Nothing: a route with no client | Independent |
| S7 | Scored cards. Older firmware ignores the tail | With S3, the whole album feature |

**The shortest useful path is S1, S2, S3, S6, S7, S8.** It gives album filtering without the engine, and gives the real mood shares before S4's synthetic numbers are trusted. Nothing forces S4 to wait for it.

**Release.** `.github/release-notes.md` is the template `tools/package_release.py:510-514` fills, and no stage may leave it behind. S2 owes a line on the first boot's one rebuild, as 0.11.0's notes did for 0.10.0's (`.github/release-notes.md:165-173`). S3 and S5 owe their rows and strings. The release that carries any of it gets its `docs/RELEASE-<v>-CHECKS.md`.

### 9.3 Console, one list

| Command | Stage | Does |
|---|---|---|
| `gm` | S2 | Block bytes, tracks with a word, coverage |
| `gm<hex>` | S2 | Tracks and albums that fit, µs for each pass. Behind the fence: says so, does nothing |
| `g`, `gt<path>` | S2 | `moods` figure; T's tail, valid or not, and the word |
| `uiM` | S3 | State, last pass time, carve |
| `uiMa<hex>`, `uiMa0` | S3 | Set or clear the Albums filter. Every pass logs its µs. Behind the fence: says so, does nothing |
| `uiMe` | S3 | Open the editor. Taps go through the scripted finger, `uit<x>,<y>`, at the goldens' cells (7.3) |
| `Dq<hex>`, `Dq0` | S4 | Set and save, or clear, Auto DJ's filter. Refuses `q800`, `q30`, `q600`. Behind the fence: saved, and the count waits for its end. Bare `q` stays Bad (`test/test_autodj_driver/test_autodj_driver.cpp:332`) |

`uiF` is taken (`src/main.cpp:2067-2068`). Letters t, h, s, d, p after `ui` are the scripted finger's, and `uiM...` starts with M. The dancers' top-level `M` is a different letter space.

### 9.4 S0: the status line under a modal

- **Step:** in `Ui::updateStatus`, move the modal check above the band change (`src/ui/Ui.cpp:1553-1563`). Today `setListBand` runs first, so a status line ending under a modal wipes it. READ, not run. The editor makes it much easier to hit.
- **Tests:** none on host (`src/ui`).
- **Done when:** `pio run -e core2` passes.
- **Device:** `uih` a row during `g0` as the status line ends. After the fix `statusBand_` stays true with an empty text under the modal, so `repaintUnder()` (`src/ui/Ui.h:356`) draws the page once in the short band, until the next `updateStatus` pass. Expect that one frame.
- **PR:** its own. Can land now.
- **Effort:** 0.1 (guess).

### 9.5 S1: the contract

**Commits:**

- **C0:** this document.
- **C1, heap model stand-in** (test only, numbers in the body).
  - `test_a_library_update_in_a_model_heap` builds from D alone (`test/test_autodj_join/test_autodj_join.cpp:2743-2747`), which never allocates the block. The other model-heap tests use literal heaps and need no change.
  - `libraryUpdates()` (`:2668`) takes one `cc::Source& records` and has no T today. It gains `MoodRun{transfer, standIn}`. The T source already exists elsewhere: `synthcard::tagsFile(card, tw)` over `cc::MemSource`, with D at `fullRecords=false` and rows `SameRows(Software, false, true)`, as `test/test_library_builder/test_library_builder.cpp:640-655`.
  - After the load (`:2699`): stand-in `g_mood = ModelHeap::alloc(trackCount*2)`. Released before `index.clear()` (`:2737`).
  - `MoodFacts : synthcard::Facts` takes the stand-in at its first `facts()`, because `enterFolders` calls it just before the first record (`lib/core/LibraryBuilder.cpp:267`).
  - Today's D-only run and asserts (`:2806-2821`) stay as the control.
- **C2, spec and fixtures.**
  - `docs/METADATA.md` text of 5.3 and 5.4, amending 2.4.5, 2.6.2, 2.6.4, 2.9, 2.17-2.19.
  - New `tools/moodfilter.py` (stdlib): tail, word, predicate. One Python source, imported by `card_fixtures`, `autodj_ref`, `synthtransfer`, `test_mood_vectors`.
  - `tools/card_fixtures.py`: `mptg()` takes `"scores"` per record, 8 numbers or null in `MOOD_COLUMNS` order, or null for no row.
  - **The generator's rule:** a tail is written only for a description whose records carry a `scores` key. Its stride then defaults to 80, and a null record gets 8 zero bytes. A description with no `scores` key and an explicit `rec_stride` keeps the 0xAB fill from byte 72 (`tools/card_fixtures.py:490`), so `mptg-minor-1` (`:2802-2805`) stays byte-identical, and so does the terminal's copy of it. With both, the real tail comes first and 0xAB from byte 80.
  - New golden `tags-moods.bin` and `libraries/tags-moods.json`: full, no row, NULL heads, all NULL, ties, float-trap values.
  - New hardening: `mptg-moods-longer` (stride 88, real tail plus 0xAB, same as `tags-moods.bin`); `mptg-tail-invalid` (stride 80 over `tags-0000002a`, each bad kind on a row); `mptg-stride-76` (no tail).
  - Reword `mptg-minor-1`'s `check` text only.
  - New `test/fixtures/card/moods.json`, about 40 rows, **made by `generate()`** (`files["moods.json"]`, as `vectors.json`, built with `tools/moodfilter.py`). `--check` fails on any file in the folder it did not make (`tools/card_fixtures.py:3220-3228`), so a hand-written file breaks CI's first commit. Keys: `heads`, `values`, `levels`, `rows[{note, scores, tail, word, holds}]`, `tails`, `filters`, `albums`, `bars`. `tails` holds an order bit beside a 255. Separate from `vectors.json` to stay out of #27's way.
  - The folder's `README.md` is the `README` string (`tools/card_fixtures.py:3077`): one bullet on the new files.
  - New `tools/test_mood_vectors.py`: stdlib SQLite with the predicate text spliced as mStream `src/api/mood-filter.js:149-169`, asserting every `holds`. Added to `.github/workflows/firmware.yml`, "Card fixtures".

**The heap model's pass rule** (with stand-in, T+D, Off / Placed / WhereThe):
- `Status` Ready
- `on.lowest >= kMargin`
- `on.fence == off.fence`
- `on.largest == off.largest`
- `on.givenBack == off.largest`
- `bootTopKept == 155284`

**A redesign is forced when:**
1. Ready or the margin fails, or `givenBack != off.largest`.
2. `joined` falls by more than 122,880 B.
3. `joined` drops below 245,764 B while the control is above it: 180,224 B (335,860 less FLAC's 155,636, `docs/AUTODJ.md:1507, 1509`) plus 65,540 B for Opus's block (`docs/OPUS.md:133`).

**The ladder if so:**
- (a) allocate at `finish()` with a build-time temp;
- (b) D8 down to Standard, 1 B, off the maps' 38,912 class;
- (c) read the block from `library.idx` at first use, which loses "no card read".

| Placement (MODELLED, a Python port of ModelHeap and Join, 30 shapes) | `joined` against control, min / median / max | Verdict |
|---|---|---|
| Lazy, at the first record | -106,496 / -40,960 / +49,152 | Plan on this |
| At `finish()`, with a temp | -114,688 / -40,960 / +49,152 | First fallback |
| Inside the views block | -110,592 / -40,960 / +8,192 | Rejected |
| Kept across `clear()` | -262,144 / -106,496 / 0 | Rejected |

No join failed, and lowest free stayed at or above 69,192 B. `joined` under 245,764 B: 3 of 30, against 1 of 30 for the control. The C++ test decides, not the port.

**Green at each commit:** the C++ golden loops use explicit lists (`test/test_card_files/test_card_files.cpp:627, 643`), so a new golden binds nothing until S2's C4. `test_hardening_files` walks the index, and the old reader reads every new file by its 72-byte prefix. That is the old-reader direction, proved by C2's own run.

**Done when:**
```
python tools/card_fixtures.py --check
python -m unittest discover -s tools -p "test_mood_vectors.py"
pio test -e native
git diff --name-status main -- test/fixtures/card
```
The last shows A for new files, M only for `hardening/index.json` and `README.md`. `mptg-minor-1.bin` and `vectors.json` are absent.

**PR:** one, C0-C2. **Effort:** 0.75-1 (guess). **Device:** none.

### 9.6 S2: the data path

Stacked on S1. No UI.

**C3, `lib/core/MoodFilter.{h,cpp}`.** No IRAM attribute, no index dependency.

```cpp
namespace moodfilter {
constexpr uint16_t kAnalysed = 0x8000, kVoiceNull = 0x4000;
enum class Scale : uint8_t { Any, First, Neutral, Second };   // happy|relaxed, sad|aggressive
enum class Voice : uint8_t { Any, Instrumental, Vocals };
enum Tag : uint8_t { kAcoustic = 1, kParty = 2, kDanceable = 4 };
enum class Level : uint8_t { Standard, Strict, Loose };
struct Filter { Scale valence, energy; Voice voice; uint8_t tags; Level level;
  bool active() const; uint16_t pack() const; static Filter unpack(uint16_t q); };
bool valid(uint16_t q);
bool tailValid(const uint8_t t[8]);
uint16_t wordOfTail(const uint8_t t[8]);
void tailOf(const double* scores, uint8_t out[8]);
bool matches(uint16_t word, const Filter& f);
struct Tally { uint32_t pass = 0, analysed = 0; };
Tally tally(const uint16_t* words, const uint32_t* ids, uint32_t n, const Filter& f);
inline bool majority(const Tally& t) { return uint64_t(t.pass) * 2 > t.analysed; }
uint32_t count(const uint16_t* words, uint32_t n, const Filter& f, uint32_t* analysed);
}
```

Tests: new `test_mood_filter`, every `moods.json` group, pack and unpack, every tail byte value without a crash.

**C4, `lib/core/CardTags.{h,cpp}`.**
- `kRecsStride` stays 72. Add `kRecsMoodStride = 80`.
- `Record` untouched, so D stays 72 (`lib/core/TagStore.cpp:49`).
- `Walker` gains `moodTail()`, read at `:273-277`, `:314-318`, copied at `:403`.
- `File::moodTail(i, out)` beside `:538-545`.
- `mptg::write`: `RecordIn::moods`, stride 80 when any is set (`:807, 819, 870`), refuses a tail failing `tailValid`, minor from `Meta`.
- Tests in `test_card_files`: `tags-moods` in `:643` and `kGoldenBinaries` `:627`; `TagsInput` reads scores; `Event` gains the word; new `test_mood_tails_read_back`; small buffers `:1570`; the hardening entries.

**C5, `lib/core/LibraryIndex.{h,cpp}` v7.**
- `Block<uint16_t> moodsB_`.
- `TagView.moods`. `noteMood(word)`: no block and word 0 means nothing; otherwise `reserve(moodsB_, tracksB_.cap)`, zero earlier tracks, push. Called after `push(tracksB_)` at `:1109`. `addFile` pushes 0 only if a block exists (`:799`).
- `finish()` checks the size, trims, sets `moods_` in `setReadPointers` (`:1412`).
- `clear()` always drops it. `Memory.moods` counts in the total.
- `save()`: words last before the sum. `load()`: Corrupt if `nM` is not 0 or nT, if `moodTracks > nM`, or if `nM > 0` with 0 tracks.
- Accessors: `hasMoods()`, `moodWord(id)`, `moodWords()`, `moodTracks()`, `forgetMoods()`.

Same commit, the literals that flip in `test/test_library_index/test_library_index.cpp`:
- `:1109` 6 becomes 7
- `:1120` adds 6u
- `:1660` `22*4` becomes `24*4`
- `:1674` `newer[4]=7` becomes 8, or the newer-version check reads Loaded

New tests: zero block without a word, first word mid-build, doubling, round trip, re-save identical, pinned layout, Corrupt cases, NoMemory, `forgetMoods`, stamp unchanged, not kept across `clear`.

**C6, builder and model flip.**
- `lib/core/LibraryBuilder.cpp:276-279`: build the view, then if `source == kFromTransfer` and a tail exists, `v.moods = wordOfTail(tail)`. `viewOf` stays 3-arg. `Result.withMoods`.
- `test/support/SynthCard.h`: `Write::moods`, `File::moods[8]`, deterministic from the path, about 92 % present.
- Flip S1's stand-in to `tw.moods = true`, delete the stand-in, compare with its numbers.
- `test_library_builder`: `:670` gains `forgetMoods`. The word follows the chosen record: fresh T, stale T, hand-copied, UNREADABLE T, T bad at end restarts from D with no block, D with a tail ignored. 20k run, MODELLED 90.6 B a track, peak about 2.02 MB.

**C7, console.**
- `lib/core/TagText.{h,cpp}`: `Command::Moods`, `gm`, `gm<hex>`. `m` is free (`lib/core/TagText.cpp:114-190`).
- `src/spike/Spike.cpp:140-155` routes to `Library::moodReport`.
- `g` figure, `gt` tail in `src/app/TagConsole.cpp`.
- Tests: `tagtext::parse("m")`, `"m127"` in `test_ui_library`. `gm127` is sad, relaxed, vocals, danceable at Standard.

**C8, `tools/synthtransfer.py --moods`.** Larger than the study said. T has zero records today (`tools/synthtransfer.py:11-14, 622-625`), and `walk_music` (`:188-200`) lists names only. With `--moods`, `tags_file()` must write:
- one record per track, with real `size`, `fatTime` and `qfp`. A made-up qfp makes T stale, and the word never reaches the index;
- names from `synthcard-files.jsonl` with FROM_API and the matching `known` bits, plus the picture anchor. Otherwise rule "one record, whole" (`docs/METADATA.md:1553-1557`) names tracks by path;
- scores from `Rng`, keyed by genre, artist, album and path, 4 places.

So `--moods` reads the sampled bytes of every file, 19,412 of them, at each `--replace`. Its run time on a real card is not determined. The first run measures it.

Flags: `--mood-coverage P` (default 92; 0 is the control card, T records at stride 72), `--mood-null P`. `--check --mood-filter 0x127` prints tracks and albums to compare with `gm127`. `tools/test_synthtransfer.py:329-331` stays for the default. Its C++ probe reads words.

**C9, docs with device results.** `docs/METADATA.md` 3.4.3, 3.5, the "v6" rows at `:3182-3186` last; `README.md`; `docs/ARCHITECTURE.md`; `docs/AUTODJ.md` budget; `.github/release-notes.md` (9.2).

**Control, must pass unchanged:** `test_card_contract`, `test_card_walk`, `test_card_jobs`, `test_card_io`, `test_tag_store`, `test_tag_scan`, `test_library_update`.

**Done when:**
```
python tools/card_fixtures.py --check
python -m unittest discover -s tools -p "test_mood_vectors.py"
python -m unittest discover -s tools -p "test_synthtransfer.py"
pio test -e native
pio run -e core2
```
The model prints D-only numbers equal to today's, and the T+D moods run is inside S1's rule.

**Device**, spare card:

| Run | Do | Must show |
|---|---|---|
| A | Flash, today's zero-record T | One rebuild; `g` moods 0; `D1` 655,348 B and `D0` 819,188 B unchanged |
| B | `--moods --mood-coverage 0 --replace` | Baseline: `g`, `D1`, `gb`, `Dc` lows. Required: T records move the baseline |
| C | `--moods --replace` | Boot rebuild against 11.3 s; moods 38,824 B at 19,412; `gm` equals `--check`; `gm127` equals `--check --mood-filter 0x127`, plus pass µs against 8-35 ms; `gt` |
| D | `D0`, `D1`; `gb` stopped, with a 320 kbps MP3, with a FLAC; `Dc` | "joined", no `[heap] FAILED`; largest against 335,860 / 221,172 / 155,636 B; `Dc` lowest over 64 KB (today 77,284 B) |
| E | After D's FLAC, play an Opus | Not skipped. Is a second hole >= 62,464 B there? |
| F | `--replace --moods --seed 2` | One rebuild; counts follow the host |
| G | Flip a T byte | "T left out", moods 0, library whole |

**PR:** one, C3-C9, merged after runs A-E. **Effort:** 2.75-4.5 (guess; C8 is 0.75-1.25). **Stops at:** a shippable tree, console only.

### 9.7 S3: Albums and the editor

Stacked on S2. One PR. Commits:

1. **`lib/core/FilterGrid`, catalogue.**
   - The rules of 7.3, 7.4 and 7.2.
   - `Cell {Happy, NeutralV, Sad, Relaxed, NeutralE, Aggressive, Instrumental, Vocals, Level, Acoustic, Party, Danceable, Clear, Cross}`.
   - `layout`, `hit`, `tap`, `summary`, `countText`, `crossX`, `crossHitX`, `textRoom`, `topHit`, `dotShown`.
   - `i18n/en.json` entries of 8 for S3, room constants in `lib/core/UiText.h`, ARB words copied to nine files, generated files regenerated (`lib/core/UiStringIds.h`, `lib/core/UiStringsData.cpp`, `test/test_ui_strings/catalogue.h`).
2. **Translations** of S3's three new ids; `@@note`; `THIRD-PARTY-NOTICES.md:461-463`, the new commit (8.1).
3. **Icon:** `kFunnel`, 14x14, in `tools/ui_icons.py`; `src/ui/IconData.cpp`; `src/ui/Icons.h`.
4. **`ListView`:** the `Source` hook, `renderItem` TopBar (`src/ui/ListView.cpp:303`), Down with button -1 (`:739`), `tapItem` (`:638`), `drawEmpty` (`:447`). No caller, no behaviour change.
5. **`FilteredAlbums` and `src/ui/FilterSheet`,** with:
   - `TouchOn::Filters` (`src/ui/Ui.h:335`), `modalUp` (`:368`);
   - `openFilters()` after `openSleepSheet`, which sets `sheetOpenedMs_` and hides a toast that is up;
   - `closeModal` (a new branch, the owner told only when `notify`), `applyCover`, `uncover` (`filters_.drawTop()` instead of `repaintHeader`), `redrawAll`;
   - `route`: the settle at `src/ui/Ui.cpp:1660` gains `filters_.up()`, and a Down above y 72 with a toast up goes to `TouchOn::None`;
   - `printState`, `command`, help text (`src/main.cpp:3583`).

   Console reaches it. No row yet.
6. **The row:** `LibraryPage` members `filter_`, `albums_`, `drawnDot_`; `topBar()` true when `real() && index() && hasMoods()`; `rowAt`, `rows`, `railRows`, `enter`, `switchSegment`, `update`, `drawSegments`, `emptyText`, `describe`; `drawTopRow`, `onTopRowTap`; `Ui::libraryUpdating()` and `libraryChanged()` drop the array first.
7. **Docs:** `src/ui/Pages.h`, `docs/ARCHITECTURE.md`, README's console table, `.github/release-notes.md` (9.2).

D7 removes the study's `container()`, `actions()`, `onAction` and `playAll_` work.

**Host tests:**
- New `test_ui_filter`: second tap lets go; tags any number; level alone inactive; Clear resets the level; equal, proportional, scaled and wrap branches; every panel point maps to at most one cell; title-row hits; first-row zones at 284, 312, 320; dot rule; summary order; `FilteredAlbums` subsequence, majority, drop, allocator failure, recompute.
- `test_ui_library`: `test_filter_grid_fits_every_language`, `test_filter_grid_survives_the_lab_panel` (margin >= 16), `test_filter_title_row_fits`, `test_filter_row_fits`, `test_filter_dot_clears_the_label`, `test_filter_empty_line_fits`, and the goldens of 7.3.
- `test_ui_strings`: runs over rooms, buffers, glyphs. Add an ink check that the ru tag's lines don't touch.

**Done when:**
```
pio test -e native -f test_ui_filter -f test_ui_library -f test_ui_strings -f test_ui_nav
python tools/gen_ui_strings.py --check
python -m unittest discover -s tools -p "test_gen_ui_strings.py"
pio run -e core2 -e core2-dio
pio test -e native
```
`python tools/ui_icons.py` leaves `src/ui/IconData.cpp` unchanged.

**Device**, `--moods` card:

| What | Commands |
|---|---|
| Pass time idle against 8-35 ms (MODELLED) | `uiMa<hex>` a few times; each pass logs its µs |
| With an MP3 against 17-70 ms, ring low | `qa`, `uiMa<hex>`, `s` |
| Tap and close latency | `ui`, `[ui] slow event`, `[ui] slow frame` |
| Third-column accuracy | `uk1`, `uiMe`, `uit260,86`, `uit276,174`, `uit263,218` (en, the goldens of 7.3), `uk0`, then a real finger |
| Widths and the empty line | `uiMa` with broad and narrow words, `ui`, `X` |
| Dot | `uis160,200,160,80,60`, `X`, `uit160,54` |
| Rail and jump grid under a narrow filter | `uiMa<hex>`, a tap on a letter that remains, and one that doesn't |
| Languages | `uiLru`, `uiLja`, `uiLpl`, `uiMe`, `X` |
| Fence with a filter on, hitch at the end | `uiMa<hex>`, `g0`, `ui` |
| Through dark | `uiMe`, `Ps0`, `Ps1` |
| Heap with the array held | `D` before and after `uiMa`; the join's blocks after an update with a filter on |
| Colours, redraw feel | The user's eye |

**Effort:** 3.5-5 (guess; device pass 0.5-1).

### 9.8 S4: the engine

Needs S2 only. May run beside S3. One PR, console only. Each commit changes reference, engine, tests and fixture together.

1. **Freeze.** `tools/test_autodj_ref.py::test_the_fixture_covers_the_rules` (`:734`) checks `sha256(json.dumps(x, separators=(",",":"), sort_keys=True))`:
   - first 26 sessions: `2764423233200b3e10f10804b03b987e5b14f5fd4d22b5e088fb3a290e66bf21`
   - seven old tables: `6f876196d0be133cf4bc669a8a5f6cbf70497fc7cf30b4e340af71e8d9a74e8b`

   (MEASURED-HOST at a1566fe.) No old session holds a new key. `stats_json` (`tools/autodj_ref.py:914`) writes new counters only when nonzero, else all 26 sessions' stats rewrite. Fixture bytes identical.
2. **`MoodView`, predicate, print, `filter` op.** Sessions mood-dry-loosen, mood-no-data and its control.
3. **Scan, repeats, by id, `jump`, table `islands`.** mood-rolling, mood-jump, mood-repeats, mood-no-index, mood-no-anchor.
4. **Surprise**, both paths behind `mood.on()`. mood-surprise. `test_autodj_driver.cpp:1543-1550` passes unchanged.
5. **Re-pin, quick skip.** mood-repin, mood-repin-edges, mood-index-away. Earlier hashes unchanged. If D2 is overruled to plain jumps, this commit and its sessions go.
6. **Hold** through trigger, driver, `rowState`; string `autodj.status_waits_fit` in ten languages.
7. **Settings.**
   - `Settings::moods`, `Driver::setMoods`, `pendingTrack()`.
   - `lib/core/NvsLayout.h:145-157`: `kAutoDjMoodsKey = "moods"`, stored and saved fields.
   - `src/app/AutoDjStore.cpp:41-76`.
   - Schema 5 here, `migrate` does nothing. `test_nvs_layout` `:38, :262`.
   - `Dq`, `stepMood` with the key of 6.5, `D` lines, `Dn` summary, pick-line tags.
   - `docs/AUTODJ.md` `:309`, 3.8 (a filtered repeat), new 3.14, 4.1, 4.2, the table at `:551-564`, 8, 10, 11.2.
   - `docs/QUEUE-MODES.md` 16.1's table (`:2141-2153`): a row for a filter nothing fits, "no (a hold)", Waiting.

New sessions (counts MEASURED-HOST with a toy predicate `(word & q) == q`; re-check with real words):

| Session | Table | Picks | Reaches |
|---|---|---|---|
| mood-dry-loosen | starve | 70 | dry 30, loosened 33 inside the mood |
| mood-no-data, -control | syn | 30+30 | a filter with no words gives identical picks |
| mood-rolling | syn | 65 | ladder, filter changed, off, on |
| mood-jump | islands | 150 | NoCandidate 1, AllCooled 4 |
| mood-repeats | syn | 100 | repeats 78; shortest gap equals tracks fitting |
| mood-no-index | none | 36 | by id, oldest first, only the playing track, 3 noFit |
| mood-no-anchor | syn | 24 | seedless, broken slot, damaged pin |
| mood-surprise | syn | 24 | the button inside the filter, and nothing fits |
| mood-repin | syn | 90 | 12 re-pins, 3 jumps |
| mood-repin-edges | syn | 70 | 8 re-pins, 4 quick-skipped, damaged and failing new pins |
| mood-index-away | syn | 18 | index away and back |

Total 707 picks, about 250 KB on a 563,529 B file (MEASURED-HOST).
- Tables below 300 rows limit this: `syn` in 7 of 12 clusters gave 0 jumps in 120 picks, hence `islands`.
- Keep Locked out of earlier commits' sessions. They would move at commit 5.
- `io` faults stay first-read-only (`tools/autodj_ref.py:346`), or a second Io hits #38's `IoTwice`, which Python does not model.
- `test/test_autodj_engine/test_autodj_engine.cpp:452` is at least 14 today and holds throughout. Raise it to 38 at commit 5. The Python check's count moves with each commit.

**Coverage required at commit 5:**
- inside a filter: `dry`, `loose`, `impT`, `impK`, `tb`, `kb`, classes 1 and 2;
- `jump` 1, 2, 3; `repin` with `reads` 0 and at least 1; `repin` plus `jump`;
- `rep` with a row and with `noIndex`; `noFit`;
- **every ok pick under a filter passes its word.**

**Tests:**
- `test_autodj_engine`: `test_a_filter_changed_remakes_the_runs`, `test_a_level_alone_is_no_filter`, `test_a_jump_keeps_the_classes`, `test_a_shared_row_is_tested_by_its_first_track`, `test_repeats_oldest_first`, `test_the_only_track_repeats`, `test_the_repin_stays_in_the_budget`, `test_a_moved_pin_goes_at_a_quick_skip`, `test_a_seed_pin_stays`, `test_no_allocation_in_a_jump`.
- `test_autodj_trigger`: the new condition, order (`:203`), `test_owed_only_while_joining` to 12 bits.
- `test_autodj_driver`: parse table, nothing fits holds and owes nothing, a filter change keeps the session, the button inside the filter, the button with no filter draws as today, `Dn` tallies.
- `test_nvs_layout`: the key, invalid values.

**Fixtures that move:** `test/fixtures/autodj/sessions.json`, additions only, and its README. Card goldens don't.

**Done when:**
```
python tools/autodj_ref.py --check
python -m unittest discover -s tools -p "test_autodj_ref.py"
pio test -e native -f test_autodj_engine -f test_autodj_trigger -f test_autodj_driver -f test_autodj_join -f test_ui_autodj -f test_nvs_layout
```

**Device**, scripts in silent test mode `z`:
- **No filter unchanged:** main and the new build on one card, `Ds717feda5`, `Dr`, `Dn200`. `[dj] Dn` lines match except times.
- **Pick cost:** `Dq<hex>`, `Dn200`, p50/p90/max, jumps, re-pins, repeats, idle and with an MP3, narrow and broad.
- **Jump time:** `Dp` under a mood the anchors miss.
- **Hold:** `Dq` with a zero-fit value, `D` says "now: no track fits the filters".
- **Boot:** a reset with a filter saved, `[dj] settings:`, `[nvs] schema 5`.
- **Fence:** `gb` with a filter on, and the fit count after a `--moods --seed 2` card with the same files.

**Effort:** 3-5 (guess). It is two implementations of the whole ladder, engine and `tools/autodj_ref.py`, plus 12 sessions.

### 9.9 S5: the Auto DJ page

Needs S3's editor and S4. One PR. Merged, then held out of a release until S7 (9.2).

1. **View.** `lib/core/AutoDjView.*`:
   - `State` gains `moods`, `moodOn`, `moodData`, `fit`, `moodPct`.
   - `PageRow` becomes `OnRow, AnchorRow, StrictRow, FilterRow, BpmRow, KeyRow, StatusRow` (`:91`).
   - `rowLive`: `FilterRow` live exactly when `moodData`, independent of `usable`.
   - Status lines of 4.4 in `indexLines` (`lib/core/AutoDjView.cpp:101`) and the held form (`:129-135`); `statusAmber(s)`; `sheetDetail`; `emptyNoFit(s)`; `emptyButton` adds `&& !(moodOn && fit == 0)`.
   - Strings of 8 for S5 in ten languages.
   - `test/test_ui_autodj/test_ui_autodj.cpp:116` must change: `FilterRow` is dim with `usable` true and no data.
2. **Page.**
   - `src/ui/OutputPage.cpp`: `drawAutoDj` (`:1058`), `onAutoDjTap` (`:1141`, with the `FilterRow` branch before the `rowLive` check at `:1136-1139`), `autoDjSig` (`:1036`), amber at `:1081-1086`.
   - `OutputPage` becomes a `FilterOwner`: `onFiltersClosed` calls `refreshAll()` and sets `drawnAutoDj_`.
   - `UiHost`, `src/main.cpp:976`: `autoDjSetMoods(q)` returns fit, `autoDjMoodSettled()`.
   - Empty states at `src/ui/NowPlayingPage.cpp:774-784, 851`, `src/ui/QueuePage.cpp:96, 284, 754-765`, or the `else if` at `:781` / `:762` puts Shuffle all back against frame dj-empty. `Sheet` gains a per-row ink (`src/ui/Overlays.cpp:410, 417`).
3. **Toasts.** The bits, `Toast` count, `fewSaid_`, `jumpSaid_`, the waiting rule of 4.5, the hold under the editor, `docs/AUTODJ.md` 9, `.github/release-notes.md` (9.2).

**Tests:** `test_ui_autodj` (status lines, the filter-on no-index lines, `statusAmber`, `sheetDetail`, `emptyButton`, news and de-dup, three toasts, and no index with 37 tracks fitting: Random at the first pick, Few at the next); `test_ui_library` near `test_auto_dj_texts_fit`; `test_ui_strings` through the catalogue.

**Done when:** `pio test -e native -f test_ui_autodj -f test_ui_library -f test_ui_strings`, `python tools/gen_ui_strings.py --check`, `pio run -e core2 -e core2-dio`, `pio test -e native`.

**Device:**
- touch on the fourth row, dim-row note;
- ten languages' status lines, sheet detail, toast placement;
- amber states through `Dq`;
- the status row with no index, which the canvas never drew: the user's eye;
- the editor from Output through `uiMe`.

**Effort:** 1.5-2.5 (guess, translations and device run included).

### 9.10 S6: the mStream route

Branch `feat/mood-scores` from master. Not stacked on #1072. No version bump. Commits:

1. `refactor(db): the batch's path resolver, on its own`. `resolveBatchPaths(filepaths, user)` from `src/api/db.js:518-529`, used by `pullMetaDataBatch` (`:503`). Its per-path try/catch (`:522`) moves with it. No behaviour change.
2. `feat(moods): POST /api/v1/db/mood-scores`.
   - `pullMoodScoresBatch` below `:563`: all values start null, early return when `!d || !moodFiltersReady()`, chunked at 500, `FROM tracks t LEFT JOIN track_moods tm ON tm.track_id = t.id WHERE (t.library_id, t.filepath) IN (...)`. The left join fixes tracks as the outer loop on `UNIQUE(filepath, library_id)` (`src/db/schema.js:847`).
   - Route after `:712`, `MOOD_SCORES_MAX_PATHS = 2000`. `model` and `moodVersion` as 5.1.
   - Allowlist line.
   - `moodScores: 1` in `src/api/server-info.js`, `moodScores: features.moodScores` in `src/api/playlist.js:43`. The drift lock fails without it (`test/integration/api-server-info.test.mjs:219`).
   - `docs/openapi.yaml`, with a header comment on no rounding and null for both no-row and not-visible.
3. `test(moods): the two-place floor agrees with the SQL's bars, on both runtimes`.

**Tests:**
- New `test/integration/mood-scores.test.mjs`: shape, a key for every path as sent (leading slash, `//`, duplicates, `__proto__`); exact values `[0, 1, 0.29, 0.57, 0.58, 0.7, 0.3, 0.5]` with `===`, raw text holds `0.29`; NULL head, all-NULL, no row; not visible `deepEqual` no row, status 200; Joi 400s; exactly 2000 paths; 413; a JS port of the byte rule matches `/db/recent/added` at .3, .5, .7; flag with an empty table; `analyzeMoods: false`; `model` with no discovery index loaded; key over HTTP.
- New `test/db/mood-bars.test.mjs` with a node leg and a Bun leg, following `test/db/sqlite-driver-errors.test.mjs`: 10,001 scores, bars 10-90, `moodPredicate` through the real driver against `floor(x*100+1e-9)`, asserts 0 wrong. This closes the runtime unknown on both. Shipped binaries are Bun builds (mStream `src/db/sqlite-driver.js:9-11`).
- Changed: `test/unit/federation-auth.test.mjs`, `test/integration/api-server-info.test.mjs`, `test/integration/federation-browse.test.mjs` (`:179-189`: a 404 for an unknown peer proves the screen passes, with no iroh).

**Done when:** `npm run lint`, `npm run docs:api:validate`, `node --test` on those files plus `mood-filters`, `playlist-load`, `metadata-album-artist`, `file-explorer-metadata`. `mood-bars` reports 0 on both runtimes, and the PR labelled `full-ci` is green.

**Device:** none. **Effort:** 0.5-1 (guess).

### 9.11 S7: the terminal writer

`git switch -c claude/card-moods claude/card-waveforms` in the existing worktree. One PR onto #68. No part can go to terminal main: `src/device/card` and `src/api/transfer.rs` aren't there. The real-card run owed on #68 (as of 2026-11-02) goes first, or failures are confounded.

Commits:

1. **`api: the mood scores call and flag`.**
   - `mood_scores: Option<u32>` at the end of `LayeredFeatures`.
   - `mood_scores_async` after `src/api/transfer.rs:1134`, the loop of `metadata_batch_within` (`:1008`), `chunks` (`:819`), 413 halves (`:1020`).
   - 403 and 404 give `Ok(None)`, with no splitting. Numbers through `real` (`:217`), which takes integers.
2. **`card kit: MPTG minor 1, fixtures`.**
   - New `contract/moods.rs`: `Scores`, `encode`, `read_tail`.
   - `contract/mptg.rs`: `RECS_STRIDE` stays 72 (`:34`), new `RECS_MOODS_STRIDE`. `Record.moods`, `has_moods()`. Decode reads the tail at `b.len() >= 80`, the one site that fills it. `:506`, `:595` pass 80 at stride >= 80. Write refuses a tail failing `read_tail`, moods in source 1, moods under minor 0. New `minor_of`. `to_input` (`:397`).
   - Fixtures copied in this same commit. `goldens_are_written_byte_for_byte` reads the whole directory and asserts `seen == goldens` (`contract/tests.rs:1185-1222`), so `tags-moods.bin`, `libraries/tags-moods.json` and `tags_input` reading `scores` (`:163-191`) land together, and 22 becomes 23. `README` paragraph updated. Names are the firmware's (S1), not new ones.
3. **`engine: overlay, read_again`.**
   - `Moods { Keep, Clear, Overlay(..) }`, `fn moods(&mut self, ..) -> Moods { Keep }`.
   - `engine/run.rs`: `let mut records` (`:1177`); `hooks.moods(&commit)` after `bound.check` (`:1237`); apply at the end of `if last`; `minor: minor_of(&records)` (`:1296`).
   - The spreads `..row.rec` and `..old.rec` (`:210`, `:1031`) carry scores once decode fills them. `read_again` (`:546-552`) builds a new record, so it adds `rec.moods = old.moods`, or every reread loses scores.
4. **`sync: fetch rule, gate, cost`.**
   - New `sync/moods.rs` with the rule of 5.2. `same_files` moves to a helper.
   - `TAGS_PER_TRACK` 345 to 353 (`plan.rs:617`), moving `:1038`, `:1089` in the same commit.
   - `Ask.no_moods`, `--moods`.
5. **`words: page, CLI, locales, contract`.**
   - `dev.card.conv_gift` changed: "for someone else: no Auto DJ index or mood scores, plays not sent" (wraps in ru), 10 locales.
   - English notes in the Details log. CLI help (`src/device/mod.rs:219`).
   - UX contract clause 51b after `docs/ux-contracts/mp3-player-screen.md:1235`.

**Tests:**
- `api/transfer.rs`: chunks under the body cap, 403 and 404, odd row, version moving.
- `contract/tests.rs`: goldens, hardening, truncation and mutation lists, round trip, refusals. New `moods_json_*`, `a_tail_counts_only_when_it_validates`, `the_stride_constant_stays_72`. The `mptg-minor-1` carry test (`:1525`) passes unchanged.
- `engine/tests.rs`: `equal_scores_make_no_commit`, `a_score_changed_alone_makes_one_commit`, `a_moved_file_keeps_its_scores`, `a_record_read_again_keeps_its_scores`, `a_rename_whose_size_moved_keeps_its_scores`, `a_checkpoint_carries_scores_and_adds_none`, `no_hooks_keep_the_scores`, `clear_goes_back_to_stride_72_and_minor_0`, `every_cut_of_a_run_that_only_adds_moods_is_finished`.
- New `sync/tests/moods.rs`: `a_server_without_the_flag_is_never_asked`, `a_304_on_a_card_with_none_asks_and_commits`, `a_304_on_a_card_with_scores_asks_nothing`, `one_changed_score_commits_once`, `a_failed_fetch_keeps_old_scores_and_the_run_ends_well`, `a_card_for_someone_else_asks_nothing_and_loses_what_it_had`, `auto_dj_off_still_gets_scores`, `a_null_row_clears_and_a_path_left_out_keeps`, `a_403_or_404_reads_as_none`, `a_peers_rows_stay_unscored`.

**Fixture lineage.** The terminal's copy is a scratch merge (`test/fixtures/card/README.md:10-16`). Until #27 lands, apply S1's tool commit on that merge in a scratch tree, run stdlib Python only, copy the output. `diff -rq` against the player's folder then lists only README.md, golden/device.txt, libraries/device.json, vectors.json, as it does today. If #27 lands first, copy from main, and only README.md differs.

**Done when:** `cargo test`, `cargo test --features desktop`, `cargo check --target wasm32-unknown-unknown` with and without `--features desktop` (`ci.yml:54, 65, 159, 164`).

**Device:** none here; S8. **Effort:** 2-2.75, plus 0.5-1.5 landing the stack (guess).

### 9.12 S8: the real sync

With the user's go-ahead, nothing playing on the music card. Needs S6 and S7, and at least S2 and S3 on the player.

- First sync: `cid=` generation rises, the build is timed against 11.3 s.
- A second sync with no change: `cid=` unchanged, no build.
- A card for someone else: `gm` reads 0.
- `gm` coverage against the query of section 12. Album share under a few filters.
- With S4 and S5 in: real jump share, re-pins, repeats through `Dn200`.

Results go into this document and `docs/AUTODJ.md`. **Effort:** 0.5-1 (guess).

**Total:** about 15-24 agent-days, a guess. The study had 8-18.

## 10. Conflicts with open PRs

Trial merges ran with `git merge-tree` on local refs at a1566fe. On 2026-11-05 #37 to #42 merged and #27 still conflicts with main.

| PR | Overlap (READ) | What to do |
|---|---|---|
| **#27** `feat/card-transfer-firmware` cc0ccb3 | Conflicts with main today in 19 files. Overlaps: `lib/core/NvsLayout.h` (also sets `kCurrent` 5), `test_nvs_layout`, `i18n/en.json` (`format.*`), generated strings, `src/ui/Overlays.cpp` Dialog only, `src/ui/Ui.cpp` route's Dialog branch, `src/ui/LibraryPage.cpp` empty state, `tools/card_fixtures.py` hunks at `:682-690`, `:1118-1123`, `vectors.json` | Nothing here waits for it. `moods.json` and `FilterSheet` avoid its lines. `mptg()` and `hardening()` don't overlap its hunks. Merge `en.json` by hand, regenerate tables. Same release: share schema 5, one paragraph, no bump. A release between: 6 |
| **#37** `claude/elated-newton-9b6dc2` | Merged 2026-11-05 (35662de). It held the same lines as S5's empty states (`src/ui/NowPlayingPage.cpp:774`, `src/ui/QueuePage.cpp:754` at a1566fe) and METADATA 3.2.2 | S5 puts its no-fit line after the queue's unread line. S2's "v6" wording edits last |
| **#38** `claude/card-guard-no-answer` | Merged 2026-11-05 (96ba08b). `lib/core/AutoDjEngine.cpp` `read` (a new `SlotResult::IoTwice`), `pickInto`, `lib/core/AutoDjView.{h,cpp}` (`Started::CardFailed`), `src/app/AutoDj.cpp`, `test_autodj_engine`, `test_ui_autodj`, `docs/AUTODJ.md`, METADATA 3.2 and later | Already under S4 and S5: `pickOnce` is a rename, so no re-indent. It didn't touch `sessions.json`, so the freeze hash holds |
| **#39 to #42** | Merged 2026-11-05: a second card-guard try, the card hold (`lib/core/PlaybackController.h`, `src/app/Library.h`, `lib/core/LibraryUpdate.*`), the queue file's memory and writes (`lib/core/TagStore.cpp`), a stopped track's bars on Now Playing. #40 gave `Ui` `noteCardHolds()` | None for the design. Re-read `lib/core/LibraryUpdate.cpp`'s room check before S2's budget line, and Now Playing before S5's empty-state edit |
| **#25** `claude/esp32-swap-mode-research-51c26d` 6220213 | Mergeable with main today. A write bench and swap-mode research. Overlaps S2's console: `lib/core/TagText.{h,cpp}` and `src/app/TagConsole.{h,cpp}` (it adds `gx`, S2 adds `gm`), then `src/main.cpp`, `test/test_ui_library/test_ui_library.cpp`, one line of `docs/METADATA.md` and `docs/ARCHITECTURE.md` | Nothing here waits for it. The second to land adds its letter beside the other's |
| **terminal #56, #68** `claude/card-transfer`, `claude/card-waveforms` | Against main: 170 behind, 107 and 108 ahead (local refs). #68 vs main: 8 conflict files. Local `main` is far behind `origin/main`, so never base on it | S7 stacks on #68. Cost: it can't merge first; a rebase of #68 replays through `run.rs`, `sync.rs`, `plan.rs`, `contract/mptg.rs`, locales. `mood_scores` joins the existing `types.rs` conflict hunk and adds none. Real scores come only from a local build until it lands |
| **terminal `claude/card-picks`** 3b69618 | 31 commits, unpushed per the last note. Vs #68: 7 conflict files (page/card.rs, UX contract, fatsim.rs, lines.rs, sync.rs, worker/tests.rs, device/mod.rs). Doesn't touch `contract/mptg.rs`, `engine/run.rs`, `plan.rs`, `autodj.rs`, `waves.rs` | Keep moods in `sync/moods.rs`. Moods will add to the sync, UX contract and device/mod.rs conflicts. No semantic clash seen, and no trial merge with moods exists. The second to land resolves |
| **mStream #1072** `claude/card-transfer-server` | Base predates moods: no `mood-sync.js`, no V80. Against master it conflicts once, in `src/api/server-info.js`'s imports (keep all). S6's planned hunks add no conflict (MEASURED-HOST, trial merge) | S6 goes from master. Don't tie moods to `instanceId` or `federationProxy`. A real sync needs master plus S6, plus #1072 if the writer wants those. Which build the user's server runs: not determined |

## 11. Budget

Device figures are on the synthetic card, 19,412 tracks. Re-measure every line marked with a run.

| Resource | Today | Addition | Label | Re-measure |
|---|---|---|---|---|
| Word block | 0 | 38,824 B carves 38,912+4 at 19,412; 40,000 B carves 40,960+4 at 20k. 0 with no moods | MODELLED | S2 C: `g` |
| Filtered ids | 0 | 6,964 B carves 7,040+4 at 1,741 albums, while a filter is on | MODELLED | S3: `D`, `uiM` |
| PSRAM free, MP3, Auto DJ on / off | 646,252 / 801,912 B (`docs/AUTODJ.md:909`, older than the waveform bar) | about -41 KB | MEASURED / MODELLED | S2 B, C |
| Largest, boot, off | 819,188 B (`docs/AUTODJ.md:961`) | -32,768 or -49,152 reported (class floors) | MODELLED | S2 A, C |
| Largest, joined at boot | 655,348 B (`docs/AUTODJ.md:1506`) | Same | MODELLED | `D1` |
| Largest, after update, joined | 335,860 B (`:1507`) | -106,496 / -40,960 / +49,152 (min, median, max) | MODELLED | S1 test, S2 D |
| Largest, MP3 after `gb` | 221,172 B (`:1508`) | Same | MODELLED | S2 D |
| Largest, FLAC after `gb` | 155,636 B (`:1509`) | Same. First Opus keeps blocks of 65,536 and 62,464 B (`docs/OPUS.md:133`). May hold one afterwards | MODELLED | S2 E |
| Lowest free in a join | 77,284 B, margin 64 KB (`:1512`) | Port's lowest 69,192 B | MODELLED | `Dc` |
| Join blocks | 155,284 B, 3 kept (`docs/AUTODJ.md:896`) | None | READ | `test_autodj_join` |
| Update step | 2.30 MB free, peak 1.89 MB (`docs/METADATA.md:4739`); check at `lib/core/LibraryUpdate.cpp:197` | Margin about -50.5 KB, not binding | MODELLED | `gb` |
| T on the card | About 6.9 MB (`docs/METADATA.md:2790`) | +160,000 B at 20k | MODELLED | `--check` |
| `library.idx` | | +40,000 B at 20k | MODELLED | `gm` |
| JSON over the wire | | 2.0-3.4 MB at 20k | MODELLED | S8 |
| Track ceiling | 23k-27k, ESTIMATED (`docs/METADATA.md:4830`) | about 22.9k-26.1k | ESTIMATED less MODELLED | |
| Album pass | | 8-35 ms idle, 17-70 ms with an MP3. Worst tap to frame about 180 ms | MODELLED | `uiMa<hex>` |
| Fit count | | 2-5 ms per change | MODELLED | `D` |
| Pick | p90 0.38-1.70 ms (`docs/AUTODJ.md:858`, ESTIMATED) | At or under inside a run; a jump adds 2.4-8.5 ms | ESTIMATED / MODELLED | `Dn200` |
| Boot load | 1.1-1.5 s at 20k, ESTIMATED | +0.02-0.03 s | MODELLED | |
| Build | 11.3 s at boot (`docs/METADATA.md:4531`) | +0.09-0.13 s | MEASURED / MODELLED | S2 C |
| Flash | About 4.36 MB, 69 %, at v0.11.0 (`.github/release-notes.md:162-164`, `docs/RELEASE-0.11.0-CHECKS.md:92`); guard warns at 80 % (`tools/flash_guard.py:49`). Size at a1566fe not determined | 10-20 KB code, about 5 KB strings | GUESS | `pio run` |
| IRAM | 5,176 B left (`docs/ARCHITECTURE.md:4019`) | 0 | READ | `iram_diet` |
| NVS | `kCurrent` 4 (`lib/core/NvsLayout.h:52`) | One u16 | READ | `test_nvs_layout` |
| Engine | 19,016 B | A few fields; `pickOnce` adds no recursion | READ | `D` |

A slow pass is a UI hitch, not an audio risk. The loop runs under the decoder and the sinks draw on a 1.5 s ring (`docs/ARCHITECTURE.md:75-79`).

## 12. Unknowns

| Unknown | Cheapest answer |
|---|---|
| Real share of moods and coverage | The query below, on the user's own `mstream.db`, opened read-only |
| Whether head files are on the release | `GET /api/` on the user's server shows `moodFilters`; the query's `analysed`. Not determinable here |
| Which build the user's server runs | The user looks at its version |
| Does the stored list cluster by mood (real ρ) | Once a card has moods, a host script over T and MPDJ using `tools/mpdj.py`. Not built |
| Album share, albums resting on few songs, folder against `album_id` | Same script |
| How many tracks on a card are a peer's | `gm` coverage against the card's track count at S8 |
| Real pass, pick and jump time | `uiMa<hex>`, `Dn200` |
| Heap with the block | S1's C++ test, then S2 D. The port's folders and string bytes are estimated |
| A second hole >= 62,464 B after FLAC and `gb` | S2 E |
| Opus plus Auto DJ at 20k | One device run. No figure exists |
| Hitch at the end of a fence | `uiMa<hex>`, `g0`, `ui` |
| node:sqlite and bun:sqlite at the bars | `test/db/mood-bars.test.mjs` |
| Whether scores stay in 0..1 | Not checked by the server (mStream `src/db/mood-sync.js:42`). The writer clamps |
| `--moods`' run time on a real card | S2's first run (C8) |
| Whether the user's unit already stored schema 5 | `[nvs] schema` at boot. Harmless either way: the step is empty |
| Flash size at a1566fe | `pio run -e core2` |
| Final widths of the new translations | `test_ui_library` once written |
| The gapless caveat under D5's alternative | Not traced |
| Whether scores count as derived data under the heads' licence | Not ours to settle. D9 keeps them off a card for someone else |

Text widths are advance sums, not drawn. Colours are 565 values, panel output not measured.

```sql
SELECT (SELECT COUNT(*) FROM tracks) AS tracks,
       COUNT(*) AS analysed,
       SUM(happy>=.5 AND happy>=sad) AS happy,
       SUM(sad>=.5 AND sad>happy) AS sad,
       SUM(happy<.5 AND sad<.5) AS neutral_v,
       SUM(relaxed>=.5 AND relaxed>=aggressive) AS relaxed,
       SUM(aggressive>=.5 AND aggressive>relaxed) AS aggressive,
       SUM(relaxed<.5 AND aggressive<.5) AS neutral_e,
       SUM(instrumental>=.5) AS instrumental,
       SUM(instrumental<.5) AS vocals,
       SUM(acoustic>=.5) AS acoustic,
       SUM(party>=.5) AS party,
       SUM(danceable>=.5) AS danceable
  FROM track_moods;
```

Standard is shown. For Strict use `>=.7` and `<.3`, for Loose `>=.3` and `<.7`.

## 13. Risks

What would change the plan:

- **S1's heap rule fails, or S2 E skips a track.** The ladder in 9.5: `finish()` placement, then D8 Standard only, then reading the block from the card.
- **D1 overruled.** S6 is replaced, and the writer needs a separate all-paths read.
- **D2 drifts too far on real data, or is overruled.** Drop S4's commit 5 and its sessions, or change `pickOnce`'s second round and 2 sessions.
- **D3 overruled.** Removes the hold and S5's zero states, and adds a fallback.
- **The scan takes too long on a device.** The fallback is an in-mood row list, which needs memory the plan avoids.
- **A pass over about 100 ms with an MP3 (GUESS threshold).** Move the count to a debounced pass on `tick()`.
- **A translation too wide for a tray even in Small.** New wording. The layout has no further fallback beyond scaling, and `test_ui_library` fails it. Text gives way last.
- **The tail is one-way.** Changing its meaning needs a new major. The word is private to `library.idx`, so a change there is a `kVersion` bump. So settle S1 before S7 copies it.
- **A score-only commit.**
  - `unchanged()` compares records whole (terminal `src/device/card/engine/run.rs:1392`), so one changed byte on one row makes a full commit: T rewritten, about 7 MB (MODELLED), a new root, a device rebuild at the next boot.
  - It happens once per whole read during a backlog or head bump, and can happen every run with a peer read (terminal `src/device/card/sync.rs:1001`).
  - `analyzeMoods` off clears every row in one commit.
  - No data or audio risk.
  - If real runs commit at every sync, add a hold-back, for example under 1 % of rows changed and under 7 days.
- **Two terminals of different versions alternating.** The old one drops tails, the new one refills. Flip-flop commits.
- **Partial analysis.** Albums with nothing analysed vanish silently under any filter. If that surprises in S8, frame A-partial returns.
- **A card with many peer tracks.** They match nothing in v1. If S8's coverage shows that matters, the peer read comes back: the client already goes through the proxy (terminal `src/api/transfer.rs:95, 109`), and the route is already on the allowlist.
- **`mood-bars` not 0 on Bun or on an ARM runner.** The literal parse differs at a bar. Fix the server to bind the bar, not the card rule. Unlikely (GUESS).
- **Head files not on the release.** The table stays empty, S3 and S5 never show on a real card, and the stages still pass on host.
- **Raw scores leave the server.** 4-place scores reach any authenticated caller and key holder for visible paths. 3-place `moodMatch` already leaves on `recent/added`, raw vectors on embeddings. Heads are CC-BY-NC-SA (mStream `src/db/discovery-features-lib.js:104-108, 132`).
- **The first boot after flashing** costs every user one rebuild: 11.3 s at 20k (MEASURED).
- **Translations** are machine-made, and native review is owed on 126 new texts.
- **#56, #68 or #27 landing or rebasing first.** Redo the lineage step of 9.11. Nothing else moves.

## 14. Considered and left out

**Other shapes for the data.**
- `/db/recent/added` with filters, no server change: 11 calls, about 163 MB at 20k (MODELLED), inexact at 3 places.
- The admin discovery export: admin only, coarse labels, no audio hash.
- Scores from the embedding: the heads were trained on raw patches, not the pooled vector.
- DJRF stride 8 to 16: any change rewrites a 10.6 MB file, and no index means no album filter.
- An own `moods-<gen>.bin`: a COMP row, a `read.*` key, licence strings, and a join or a rewrite with T anyway.
- A new MPTG section: works, but a second stream position during the build.
- Byte 71 of RECS: one threshold only, and breaks "new fields only at the end".
- A 16-bit word on the card: saves 120 KB of a card, loses the other 78 bars for good.
- Per-head `1 + q div 50`: loses head-against-head order inside a half-hundredth.
- Per-album data: can't answer combinations without 1,149 stored answers, and does nothing for Auto DJ.
- A mood term in `plan_key`: re-asks every entry's metadata, about 30 MB once (MODELLED).

**Other shapes for the engine.**
- A filter bit in the word, or an in-mood row list: memory the predicate in `buildRun` doesn't need.
- Relaxing to Loose or to no mood when dry: 71-96 % of Stay on seed picks outside the mood at f <= 5 % (SYNTHETIC), and the server never relaxes.
- A counted scan picking one nth track: loses the artist cooldown and the BPM and key tiers.

**Other shapes for the UI.**
- Overview rows, flat checklist, fixed bottom bar, status band: drawn on the canvas, not picked.
- A drawer inside the list: 5 rows of 42 fill the band, so its live results are off screen.
- Frame A-partial, Play all, Artists, a long-press way in: left out of v1 (3.1, 3.2).

**Narrower v1s, and fixes, not taken.**
- Dropping re-pin from v1: D2 is the stated default, and the plain ladder jumps 70 % at f = 5 % (SYNTHETIC). It is one commit, S4's 5, which goes if the user overrules.
- Dropping "No close match" and "Plays once" with their news plumbing: the picked design draws them (dj-toast-jump, -once), and the status row is seen only on its page.
- Keeping today's draws for the Auto DJ button under a filter: 8 tries fail 66 % at f = 5 % (uniform model). One pass over the words and at most 8 lookups does better, with a filter on only.
- Draws first and a scan only on failure, for the button: two paths for a tap that a single pass answers in 2-5 ms (MODELLED).
- A one-word ru tag in place of the two-line wrap: the picked design has the wrap, and the firmware takes the ARB's words.
- Keeping `uiMt<cell>`, `uiMp<n>`: `uit` with `uk1` at the goldens' cells and a µs line for every pass answer the same questions with no new command.
- Telling the owner from every `closeModal(false)`: only `suspend()` can have the editor up, and a reload there runs under `suspended_`.
- Carrying an unshown Few in `pendingNews_`: `src/ui/Ui.cpp:1398` would draw it the next pass, over the toast just shown. It waits for the next append instead.
- A new "{n} of {m} tracks fit · picks are random" string for the no-index status: the held form says it with no new text in nine languages.
- A frame for a toast over the editor's title row: the toast goes at opening, one line of code.
- A version check or a probe for the route: a flag is mStream's own rule (mStream `src/api/sync.js:18`).
- Citing `uiF` by its help text (`src/main.cpp:3587`): the handler at `:2067-2068` is what takes the letter.