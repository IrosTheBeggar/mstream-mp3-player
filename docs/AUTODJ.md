# Auto DJ

Auto DJ keeps the queue going. When the queue's last track starts, it adds
one more, picked on the device from the card's AutoDJ index. No network,
no server. This document is the engine as built: what it does and doesn't,
the pick rule step by step, when it adds a track, sessions, the index on
the device, what a pick costs, the settings, the UI, the console's `D`
command, the Python reference, the tests, the device test card, what is
deferred, and the device checks.

**Status (2026-10-16):** built for 0.11.0. Three device checks ran on a
synthetic 19,412-track card:

- **The first** (2026-10-13, `09f91a1`, console only) found five
  problems, fixed in `7c89673`; the UI came in `e5efd22`.
- **The second** (2026-10-14, `a7ee54b`) passed the pick's CPU,
  loosening, the queue's saves, the Auto DJ button and the UI in ten
  languages. It failed the join's time, two-line toasts in Japanese and
  Chinese, the Undo of a Play over a paused queue, the join's memory and
  a quick skip's wording (14.1). `314c624` fixed them: the toasts placed
  by their ink (9.6), Undo never louder than the edit found it (9.4), the
  quick skip's fourth and fifth wordings (3.2), the join's placement with
  a 64 KB margin and in smaller free blocks first (6.2), and its Folders
  phase, about half its CPU (7.3).
- **The third** (2026-10-15 and 16, `314c624`) passed those fixes. No
  Undo started sound. PSRAM's free stayed at least 86,868 B through every
  join. After a library update, the largest free block once the join is
  given back equals Auto DJ off's. The joins took 1.48 s idle, about
  3.0 s with an MP3 and 5.9-6.2 s after a library update: over the
  estimates, under 2026-10-14's. It also passed every card move and every
  bad index file (14.2).

Since then, two small fixes are host-tested only: a log line for a slot
found damaged at a pick (3.13), and Match strictness's value on the dim
rows in every language (9.2). Left for the user to judge: three Japanese
and Chinese toasts whose first line touches the card's outline (9.6).
Section 14 lists what the next release must run.

On 2026-11-02 a library update's rejoin was found failing for want of a
block with 816 KB of PSRAM free, a misreading of the heap that goes back
to the placement's first day. Fixed and checked on the device that day
(7.4, 14.3).

**Mood filters (2026-11-06):** a mood filter keeps every pick inside it
(3.14; docs/MOOD-FILTERS.md's stage S4), set on Output > Auto DJ's Smart
filters row (9.2; stage S5) or with `Dq`. Both were checked on the Core2
on 2026-11-06: the engine (MOOD-FILTERS.md 9.8) and the page (9.9). With
no filter, every pick, the Auto DJ button's draws and the toasts are as
before.

The other documents:

- **docs/AUTODJ-INDEX.md:** the file (MPDJ v2), for any transfer software.
  Its section 11 has the device notes; Appendix A plans mStream's exact DJ
  (minor 1, provisional).
- **docs/ARCHITECTURE.md:** "Library and queue" (the quiet append, the
  play epoch, `queue.txt`'s snapshot writes) and "UI" (the screens).
- **docs/QUEUE-MODES.md:** section 16, "Auto DJ with shuffle, repeat and
  the sleep timer": the modes and the finished queue with Auto DJ on.
- **docs/METADATA.md:** the card contract; 3.4.2 the library update step
  and its memory check; 3.5 the memory budgets.

Labels, as elsewhere in the repo:

- **READ:** from the code.
- **MEASURED:** on the device, with the date.
- **ESTIMATED:** worked out from measured rates, or from a host benchmark;
  not yet measured on the device.
- **MEASURED-SYN:** simulated sessions on the synthetic card's index.

"The design" below is the engine design of 2026-10-13 and the user's
decisions D1-D6 on it. Where they differ from the code, the code is what
this document describes.

---

## 0. In one screen

- **When:** the queue's last entry starts playing; Auto DJ appends one
  track. At a finished queue it appends one and cues it, paused: Play
  starts it. It never starts playback by itself and never touches the
  output or the volume.
- **What it uses:** the index's neighbour lists (one 512-byte slot a
  track) and, per row, the BPM, the Camelot key and an artist key (DJRF).
  Nothing else: no ratings, genres, years, folders or lengths.
- **Two modes** (the mobile app's names): **Follow the vibe** (the
  default): close to any of the last 5 tracks; **Stay on seed:** close to
  the session's seed track.
- **The pick:**
  1. the sound is a hard gate: a candidate's best listed level to an
     anchor must reach the strictness (55-75 %, never below the index's
     own floor);
  2. nothing passes: the 50 closest;
  3. everything in the pool played lately: the 50 closest that didn't
     (loosen before repeating);
  4. inside the pool, the cooldowns (50 tracks, 3 artists), then BPM
     continuity and Harmonic mixing (both off by default) put the
     candidates in classes;
  5. a missing BPM or key gets the same odds as the candidates that have
     one;
  6. one uniform draw in the best class. It never ranks by similarity.
- **A mood filter** (Smart filters on Output > Auto DJ, or `Dq`; saved):
  every pick stays inside it: the neighbours inside it, then any track
  that fits (a jump), then repeats, oldest first. No track fits: no pick,
  and Auto DJ reads Waiting (3.14).
- **No usable index:** random picks from the library, with a 50-track
  cooldown.
- **Cost:** at most 2 slot reads a pick, usually 0 (a read ahead between
  picks). The pick's CPU, p90: 0.38-1.70 ms at 240 MHz, 0.52-2.31 ms at
  160 MHz (MEASURED 2026-10-14; 5-9 ms before the optimisation).
- **Memory:** the engine's 19 KB of PSRAM from boot; the join's 155 KB at
  19,410 tracks, only while Auto DJ is on (on against off: 155 KB lower,
  MEASURED 2026-10-14).
- **The UI:** a 4th row in Now Playing's "..." menu; Output > Auto DJ,
  with Smart filters for the mood filter; an Auto DJ button on the empty
  Queue and Now Playing; "Auto DJ from here" in the Library's long-press
  sheet; a small DJ mark on the Queue rows it added.

---

## 1. What it does, and what it doesn't

### 1.1 It does

- Adds **one** track when the queue's last entry starts (section 4), as
  soon as the decoder has buffered its first 500 ms. The decoder then has
  the rest of the track to open the next file, and the join is gapless.
- Picks from the card's AutoDJ index (`/.mstream/autodj-<gen>.bin`), which
  the transfer software writes. The firmware asks for it in
  `/.player/device.txt` with `read.mpdj=2` (docs/AUTODJ-INDEX.md 7.4).
- Without a usable index, picks a random library track (3.12).
- Keeps every track the listener plays in its picture of the session
  (D1): the listener's own Play next and + Queue steer the next pick.
- Remembers its switch and its settings in NVS (section 8). After a boot
  it is on if it was, but nothing plays until Play.

### 1.2 It doesn't

| What | Why |
|---|---|
| Start playback, or play a cued pick by itself | The device never starts sound by itself after a silence. Switching Auto DJ on with nothing queued plays nothing; B stays inert on an empty queue |
| Filter by rating, genre, year, folder, length or keyword | The card has none of them for Auto DJ (DJRF holds BPM, key and an artist key only). The mood filter is the one filter: its words are `library.idx`'s (3.14) |
| Use tempo, key or the artist cooldown for tracks without a row | Their BPM and key would need a per-track block in `library.idx` or a tag read per pick (deferred, section 13) |
| Read two hops in Stay on seed | Deferred (D5, M7): at most the pin's slot a pick |
| Rank by similarity, or offer "More like this" | The pick is a uniform draw inside the gate, as in the mobile app; a ranked list is a later Library page |
| Keep its session, or the DJ marks, across a reboot | RAM only. The marks would need a `queue.txt` v3 |
| Show a glyph on Now Playing | No room in the shuffle and repeat group; the "..." row shows the state |
| Let the BPM tolerance or the artist cooldown be set | Fixed: 8 BPM (the mobile app's default), 3 artists |
| Go below 55 % or above 75 % strictness | Below the index's floor (140, 0.549) every listed entry passes; above 75 % too many picks land below the floor (docs/AUTODJ-INDEX.md 11.4) |

### 1.3 Where it differs from the mobile app

The mobile app's Auto DJ is the model (its names, its defaults, its pick
rule as mStream runs it). The device differs on purpose:

| | Mobile app | Device |
|---|---|---|
| Look-ahead | 4 tracks a fetch | 1 track, added as the last one starts (a local pick takes milliseconds) |
| What steers Follow the vibe | Auto DJ's picks | Every track that plays, Auto DJ's and the listener's; a quick skip excepted (D1) |
| The gate | Close to the centroid of the last 5 | Close to any one of the last 5 (a slot can't score a centroid) |
| Changing Anchor mid-session | Keeps the seed | Stay on seed takes the playing track as the new seed |
| Tracks with no BPM or key | (mStream) ranked below every known match | The same odds as the candidates that have one (D3) |
| Artist cooldown | None | The last 3 artists |
| An empty queue | Asks: Surprise me / Let me choose | An "Auto DJ" button on the empty states (D4) |
| A finished queue | Plays a pick at once | Cues a pick, paused |
| Strictness | A slider 0.30-0.80, and a sonic switch | 55-75 % by 5; always sonic with an index |

---

## 2. Words

| Word | Meaning |
|---|---|
| Row | A recording the index knows (it has an embedding). A library track maps to at most one row; a row may have several files, and maps to one track |
| Slot | A row's neighbour list in the index: up to k (169) entries, each a row and a level |
| Level | `round(cosine × 255)`, 0-255. 141 is 55 %, 192 is 75 % |
| Anchor | A row whose slot a pick reads: the ring's rows (Follow the vibe), or the pin (Stay on seed) |
| Ring | The session's last 5 rows that steer, newest last |
| Pin | Stay on seed's one anchor |
| Seed | The session's first track: its first anchor, and in Stay on seed its pin |
| Session | One run of picks: its rings, its pin, its key anchor and its random stream. Never saved |
| Candidate | A row listed in an anchor's slot that maps to a track of this library |
| Pool | The candidates one pick draws from: the gate's, the dry 50, or the loosened 50 |
| Class | `cooldown level × 8 + music rank`, 0-23. The draw takes the lowest class present |
| The join | The index's rows matched to this library's track ids, in PSRAM (section 6) |
| A filter word | A row's resident BPM, Camelot key and 16-bit artist key, 4 bytes (from DJRF) |

---

## 3. The pick, step by step

`lib/core/AutoDjEngine` (READ). `tools/autodj_ref.py` is the same rule in
Python, and the C++ engine matches it pick for pick (section 11).

### 3.1 The anchors

- **Follow the vibe:** the ring, the session's last 5 rows that steer
  (3.2). A row that comes back moves to the end.
- **Stay on seed:** the pin alone (3.3).

The playing track's row is never a candidate, nor is an anchor.

### 3.2 Which tracks steer (D1), and the quick skip

- **Auto DJ's picks join the ring when picked.** The pick made as track n
  starts steers the pick made as track n + 1 starts, as in the mobile app.
- **The listener's tracks join when they start:** the first loop pass in
  which the entry is current and the player is Playing (waiting for the
  headphones isn't a start). That is before the trigger, so the pick due
  in the same pass follows the listener's track.
- **A track without a row** never anchors.
- **The quick skip:** a track that leaves the current entry in its first
  30 s of play, not by its natural end, leaves the ring. It stays in both
  cooldowns. "Played" is the furthest position the backend reported for
  that entry. A skip, a jump, a removal: all the same. Not a quick skip:
  - a natural end (the player's natural-end counter moved);
  - a track that already played to its end while it stayed current
    (Repeat One's loop, the queue's end);
  - an entry that never started.
- **A pick taken out before it played** leaves the ring the same way (a
  skip at 0:00) and stays cooled. The playing entry is then last again,
  and Auto DJ picks another.
- **A pick the queue refused** (full of tracks not yet played, or no
  memory) leaves the ring too, and stays cooled.
- **The maps away** (after a library update's fence, until the join is
  ready again): the loop remembers each track's row, so a quick skip still
  takes the right row out. Rows don't change with the library's ids.

The console's line for a quick skip says which case it was
(`autodj::skipWhyText()`):

- "no longer steers": it was in the ring;
- "it has no row in the index: it never steered";
- "it wasn't among the anchors: nothing changes": it has a row, but
  started before the session (for example after `Dr` while it plays);
- "the index isn't joined yet: it never steered": its row is unknown,
  the maps weren't there while it was current (a skip between `D1` and
  "joined");
- "no index is joined: it never steered": the same with no join under
  way (no usable index, the join away or short of memory).

A track that is still current when the maps arrive steers from then (3.3),
so after "joined" its skip says "no longer steers". Before the device
check of 2026-10-14 the fourth case printed the second's wording for a
track that has a row. On 2026-10-15 the device printed the fourth case
for a skip 0.1 s after `D1`, and "no longer steers" after "joined".

### 3.3 The seed and the pin

- **The seed** is the session's track given when it began: the Play's
  first track, the "Auto DJ from here" track, the Auto DJ button's track.
  With a row, it is the ring's first entry, and the pin.
- **A session that began without one** (switched on, a boot, `Dr`):
  - the first track to start anchors the ring, but is no pin;
  - at the first pick, if nothing started yet, the playing track is the
    seed and the pin.
- **Stay on seed with no pin** (no seed, a seed without a row, the
  Anchor changed): the playing track's row at the next pick; else that
  pick's own row.
- **A seed given before the join** (the button and "Auto DJ from here"
  play at once and don't wait for the maps): its row becomes the pin when
  the maps arrive. Not after an Anchor change, a library rebuild, or once
  a pick has taken the pin. Once a session.
- **Changing Anchor** drops the pin: the next Stay on seed pick takes it
  from the playing track. The ring stays.
- **A damaged pin** gives way to the random pick it causes, whose row
  becomes the pin.
- **Under a mood filter** (3.14, docs/MOOD-FILTERS.md D2): when the pin's
  list has nothing inside the filter, or all of it played lately, the
  playing track's row becomes the pin, if it is inside the filter, isn't
  the pin and isn't damaged (the re-pin: once a pick, before any jump or
  draw). A moved pin goes at a quick skip of it (3.2) and at an Anchor
  change; a seed's pin stays at a quick skip, as with no filter.

### 3.4 Slots: the cache and the reads

- **The cache** holds 6 slots. The ring's rows and the pin are never
  evicted; the least recently used other slot is.
- **The read ahead:** a loop pass with no pick due reads one missing
  anchor slot: the newest ring row not cached (Follow the vibe), or the
  pin (Stay on seed). Never while a list moves, the card is held, the
  index joins or the decoder refills (4.5); 2 s after a read ahead that
  failed. So a pick usually reads nothing.
- **At the pick:** missing anchor slots are read now, newest first, at
  most **2 reads a pick** (a second read of a suspect slot included). The
  anchors left over are left out of this pick (counted: "left out").
- **The read path:** one read of `slotBytes` at `DJSL.offset + row ×
  slotBytes` on a raw FatFs file kept open (in PSRAM), then the slot's
  checks S1-S4 (docs/AUTODJ-INDEX.md 8.4). Never through the card
  worker's queue.
- **Runs:** each cached slot keeps its entries sorted by row, with each
  row's level and filter word, only the rows that map to a track. The run
  is made at the read ahead, so a pick reads no map but the chosen row's.
  When the maps change (the join ready again), the runs are made again,
  one a loop pass, before the next pick needs them.

### 3.5 The candidates

The union of the usable anchors' slots: each slot's first `min(count, k)`
entries, less the anchors, the playing track's row and rows that map to
no track. Each candidate keeps its **best level** (the highest over the
anchors that list it) and its **level sum**. They are taken in row order,
so the host and the device draw the same numbers.

### 3.6 The gate: strictness and the index's floor

A candidate passes when its best level reaches

    L = max((255 × p + 99) div 100, selFloor)

for the strictness p in percent and the index's `selFloor` (140 by the
writers' default):

| Match strictness | 55 % | 60 % | 65 % | 70 % | 75 % |
|---|---|---|---|---|---|
| Least level (selFloor 140) | 141 | 153 | 166 | 179 | 192 |

With an index whose `selFloor` is above the setting's level, the page
shows the strictness that acts (the smallest percent that reaches
`selFloor`), and a tap steps from it.

### 3.7 Dry: nothing passes the gate

The pool becomes the **50 candidates with the highest level sum**, ties
by row (the lower first). In Stay on seed that is the seed's 50 nearest.
With no mood filter Auto DJ never stops and never goes silent. A filter no
track fits holds it instead (4.2; docs/MOOD-FILTERS.md D3). Counted:
"dry".

### 3.8 Loosen before repeating

The user's decision of 2026-10-14. When **every row of the pool** (the
gate's, or dry's 50) is in the 50-track cooldown and **a candidate outside
it exists**, the pool becomes the **50 candidates outside the cooldown
with the highest level sum**, ties by row: the dry rule over the fresh
candidates, below the gate if need be. Counted: "loosened".

Only when every candidate is cooled does the pool stay as it was, and the
pick repeats a track. Under a mood filter (3.14) that repeat is not a
neighbour's: with every candidate inside the filter cooled, the pick
jumps to any fresh track that fits, and repeats only when none is left,
the oldest that fits first. Why: on the device check, a Stay on seed seed with 3
rows above 75 % repeated those 3 for 197 of 200 picks. Loosened, it walks
the seed's 169 listed rows before anything repeats.

### 3.9 The class: cooldowns, then tempo and key

`class = cooldown level × 8 + music rank`; the lowest class present wins.

**The cooldown level** (both cooldowns are soft):

| Level | The candidate |
|---|---|
| 0 | fresh |
| 1 | its artist is one of the last 3 artists (an artist key of 0, no artist, is never cooled) |
| 2 | its row is one of the last 50 rows played or picked this session (the seed included) |

- The track cooldown holds rows: the same recording at two paths counts
  once. A row that comes back moves to the end.
- The artist key is the low 16 bits of DJRF's artistKey (a hash of the
  first non-blank artist of the row's file), 0 made 1 when the key isn't
  0, so 0 alone means no artist. Two artists that share 16 bits cool each
  other, rarely.

**The music rank**, from two statuses. BPM continuity off, or a playing
track without a row or a BPM: the tempo status is "na" for this pick.
Harmonic mixing off: the key status is "na".

- **Tempo**, against the playing track's BPM `ref` (read again at every
  pick): **tight** within 8 BPM of `ref`, of `ref / 2` (rounded up) or of
  `2 × ref`; **wide** within 10 of one of them; **wrong** otherwise;
  **unknown** without a BPM. BPMs are whole numbers (DJRF's tenths
  rounded).
- **Key**, against the session's Camelot anchor: **good** one of its 6
  codes (the same number and one either side, 12 next to 1, A or B);
  **wrong** otherwise; **unknown** without a key. Before there is a key
  anchor, every known key is good.

| Both on | Key good | Key unknown | Key wrong |
|---|---|---|---|
| Tempo tight | 0 | 2 | 3 |
| Tempo wide | 1 | 4 | 4 |
| Tempo unknown | 5 | 6 | 7 |
| Tempo wrong | 7 | 7 | 7 |

- **BPM continuity only:** tight 0, wide 1, unknown 2, wrong 3.
- **Harmonic mixing only:** good 0, unknown 1, wrong 2.
- **Neither:** every candidate 0.

The ranks are mStream's waterfall for one pick (the step that first
admits a candidate, then its tier). One difference: before a key anchor
exists, an unknown tempo with a known key ranks 5, where mStream's "na"
key gives 6.

**The key anchor** is taken lazily, only while Harmonic mixing is on: at a
pick with none, the playing track's key; still none after the pick, the
pick's key. It stays for the session; turning Harmonic mixing off keeps
it.

### 3.10 A missing BPM or key: the same odds (D3)

When BPM continuity or Harmonic mixing is on, a pooled candidate with no
BPM gets a tempo status drawn with the odds of the pool's candidates that
have one: `u = below(tight + wide + wrong)` from the session's stream; `u
< tight` tight, `u < tight + wide` wide, else wrong. In row order, every
tempo draw before every key draw; a missing key the same way (good,
wrong). The drawn status sets its class.

- No candidate in the pool has a known value: the status stays unknown,
  and ranks as the tables say.
- A track without a BPM or key plays at its natural share: neither
  starved (as mStream's order would) nor favoured.
- **Breaks** (the console's counters, per mode): a **known** BPM outside
  the tight windows of a known reference; a **known** key outside the
  anchor's 6 codes. Drawn statuses never count.

### 3.11 The draw, and what the pick changes

- One `below(n)` over the pool's candidates in the lowest class.
- The pick joins the ring, the track cooldown, the artist cooldown and a
  50-entry cooldown by track id. It may become the key anchor and the pin
  (3.3, 3.9).
- The stream is PCG32 (`AutoDjRng.h`), seeded per session (`esp_random()`,
  or the console's `Ds`). The same seed and the same events give the same
  session, on the host and on the device.

### 3.12 Random picks, and picks without an index

- **A random pick** (an index, but no usable anchor: no anchor with a
  row, every anchor's slot damaged or unread, a broken file; or no
  candidate at all): 64 uniform draws over the rows, skipping rows that
  map to no track, the anchors, the playing track's row and rows already
  drawn. Then the classes as in 3.9 (tempo and key against the playing
  track), with no gate and no loosening, and the draw. The draw itself
  reads nothing (a random pick's reads, up to 2, are the anchors' slots
  that failed first).
- **A pick by track id** (no index; an index not open, of a major this
  build doesn't read, or for a library too big to join; or the join away:
  given back, waiting to retry, short of memory; or a random pick whose
  64 draws found no usable row): up to 16 draws of a library track,
  never the playing one; the first outside the 50-track cooldown by id is
  taken, else the last drawn. No tempo, key or artist data. The by-id
  cooldown is cleared when a library update renumbers the tracks. A
  track picked this way that has a row joins the ring when it starts, if
  the index has come since.

A session's first pick that is random because its track has no row is
said on the screen (9.6). Under a mood filter both kinds stay inside it
(3.14).

### 3.13 Damaged and broken slots

| What | Effect |
|---|---|
| A slot's CRC fails (S1) | Read once more, in this pick if the budget allows, else at its next read. Failing twice in a row it is **damaged**: into a ring of the last 8 damaged rows, never read again until the file is reopened. That anchor is left out; none left: a random pick |
| The read fails (`Why::Io`: the card pulled, a FIL a remount invalidated, a sector the card keeps failing) | Skipped for this pick. The same row twice in a row, no read of it that worked between: into the damaged ring too (`SlotResult::IoTwice`), until the file is reopened: each read costs the loop the driver's retries, up to 1.5 s under the volume's lock, and a read ahead would come back every 2 s. (A pin not yet read gives way by the ring's rule, a pull's too; a remount's reopen clears the ring, not that) |
| S2-S4 fail under a good CRC (**broken**: the writer's bug) | The file is treated as absent: every later pick is random, until a remount or the next boot. Logged once |

The console says each step for a damaged slot:

- a read ahead: "read ahead, row R: its CRC failed (read again later)",
  then "read ahead, row R: DAMAGED (its CRC failed twice: skipped from
  now)"; or "read ahead, row R: the read failed (the card?)", then "read
  ahead, row R: DAMAGED (the read failed twice (the card?): skipped from
  now)";
- a pick, when its read fails a second time in a row (both of its own
  two reads, or its one after a read ahead's failure), the CRC or the read:
  "read at the pick, row R: DAMAGED (...)" after the pick's line
  (`Pick::damagedRow`). Once a row, since a damaged row isn't read again.

Before 2026-10-16 a row damaged at a pick had no line: only `D`'s
"damaged" counter rose (the device check of 2026-10-16, 14.2).

### 3.14 The mood filter

docs/MOOD-FILTERS.md has the design (4.4, 6), its figures and its
sessions. What the engine does with a filter (READ; host-tested, and
checked on the Core2 2026-11-06, MOOD-FILTERS.md 9.8):

- **The filter:** 11 bits, `q` (MOOD-FILTERS.md 5.4), set by the Smart
  filters row's editor (9.2) or `Dq` and saved (section 8), over the library's mood words (`library.idx` v7, a
  u16 a track). It is on when the card has words and a group is set. A
  level alone is no filter, and nor is any filter on a card with no mood
  data: the picks are unfiltered there (D4).
- **The lists:** a row whose own track (`rowToTrack`) fails the filter is
  left out of the runs, so the gate, dry, loosening and the classes see
  only rows inside it.
- **The ladder:** the gate, dry and loosening as above. Nothing listed
  inside the filter (`Jump::NoCandidate`), or all of it cooled with
  nothing to loosen to (`Jump::AllCooled`): a **jump**, to the scan. One
  pass over the tracks keeps 64 inside the filter (a reservoir), each row
  once and only if its own track passes, the anchors and the playing row
  left out; then the classes and the draw over them, if one is fresh. None
  fresh: a track inside the filter with no row, by id (`Jump::NoRow`).
  None: a **repeat**, the oldest row of the track cooldown inside the
  filter that isn't playing. No index, or none of those: by id inside the
  filter, a fresh track first, then the by-id cooldown's oldest, then the
  playing track. None at all: no pick (`Pick::noFit`).
- **Stay on seed's re-pin** (3.3): at either jump the playing row may
  become the pin, and the anchors run again in the same 2 reads. Nothing
  there either: the jump, marked re-pinned too.
- **The Auto DJ button** (9.3) draws inside the filter. Joined: the scan
  with nothing left out, then one of its rows. Before the join: one pass
  over the words keeps 8 tracks inside it, looked up in a random order.
- **No track fits:** the trigger's ninth condition (4.1), a hold. Auto DJ
  adds nothing, reads Waiting, and the status says "Waiting: no track
  fits the filters". The glue counts the tracks inside the filter
  (`AutoDj::stepMood()`, one pass over the words, 4.8-5.6 ms at 19,413
  tracks, MEASURED) when `q` changes and when the library comes back (the first
  pass, each fence's end); not by the library's stamp, track count or
  pointer, which a score-only rebuild leaves as they were. Behind the
  fence the filter is off and the count kept.
- **A filter changed** keeps the session (section 5). A pick already
  queued plays once (D5), and a toast says so (9.6).
- **No filter:** every new branch is behind the filter being on, a moved
  pin or a field that is false without one, and adds no random draw. The
  26 sessions from before the filter replay identically and are held by
  hash (11.2); the button's draws are pinned too (`test_autodj_driver`).
- **The console** says it: `Dq`, `D`'s mood line, the pick line's tags and
  `Dn`'s line inside the filter (section 10).

---

## 4. When a track is added: the trigger

`lib/core/AutoDjTrigger` (`autodj::decide()`), read every loop pass in
Auto DJ's step (READ). It is level based: however the last entry became
current (a natural end, a skip, a removal of what followed, Auto DJ
switched on), it is seen the same way.

### 4.1 The conditions

One track is added when all of these hold. The first that fails is what
the console's `D` prints after "now:".

| # | Condition | `D` says when it fails |
|---|---|---|
| 1 | Auto DJ is on | off |
| 2 | The queue isn't empty | the queue is empty |
| 2 | The current entry is the last in play order (shuffled: the last of the shuffled order; an append ranks after the highest rank and is never shuffled in) | tracks up next |
| 3 | Playing, or waiting for the headphones; or a finished queue (4.3). Never a paused or just-restored queue | nothing plays |
| 4 | Not behind the library update's fence | the library update's fence |
| 5 | The card isn't held by its guard, and no restart is set (a CPU speed's) | the card is held |
| 1, 5 | The index isn't reopening or joining | the index joins |
| 6 | Repeat isn't One | Repeat One |
| 7 | The sleep timer isn't set to End of queue | the sleep timer's end of queue |
| 8 | Not paused by failures (4.4) | picks failed to play |
| 9 | Not under a mood filter that no library track fits (3.14) | no track fits the filters |

All hold: "due". The pick then goes through
`PlaybackController::addFromAutoDj()`: one append, no undo of its own
(the queue's quiet append), and the word on what follows goes to the
backend in the same action, so the pick joins gaplessly.

### 4.2 Holds: Waiting and Paused

Conditions 6-9 are holds. Auto DJ adds nothing; the "..." row reads
**Waiting** (Repeat One, the sleep timer, a mood filter no track fits) or
**Paused** (failures), and the Output page's status row says why. A hold
never blocks a library update (4.6).

### 4.3 The finished queue

With Repeat Off the player stops on the last entry at its end
(`PlaybackController::endedAtEnd()`). That happens with Auto DJ on when a
pick was blocked for the whole last track (a fence, the card held, the
join behind a very short track, the failure pause, the console blocking
the loop), after the sleep timer's End of queue, when every track of the
queue failed (the player stops on the last), and when Auto DJ is switched
on after an album has finished.

There Auto DJ appends its pick and **cues** it: the new entry becomes
current, paused at 0:00, marked as cued by Auto DJ. Nothing starts.
Play (B, the screen) starts the new track, not the old one again;
headphone Play doesn't start it. A toast says "Auto DJ: the next track is
ready, press play". After the sleep timer's End of queue, the pick is
cued once the timer's choice is over, so the morning's Play starts a new
track. docs/QUEUE-MODES.md section 16 has the modes.

### 4.4 The failure pause

- 3 of Auto DJ's picks in a row that fail to play pause Auto DJ (an amber
  note). A track of the listener's that fails neither counts nor ends the
  row.
- It clears when any track has played 500 ms, or when Auto DJ is switched
  off and on.

### 4.5 Waits and retries

| What | Rule |
|---|---|
| The decoder's refill | After a start or a skip, the decoder fills its ring from empty, flat out, above the loop. A due pick waits until the new track has 500 ms buffered, its source has ended (a track shorter than 500 ms), or 1.5 s have passed; nothing is read ahead meanwhile. A gapless join is no start: nothing waits. Counted: "waited for a refill". (A pick timed across the refill measured the decoder: 108 ms on 2026-10-13) |
| A pick that gave no track, or one the queue refused | Tried again after 5 s |
| A read ahead that failed, or a pick's read | The next read ahead 2 s after its end (a pick still reads what it needs); the same row's second goes in the damaged ring (3.13) |
| The index joining | No pick: it waits for the join (condition 1, 5), and the library update waits for it (4.6) |

### 4.6 The library update's fence

`LibraryUpdate`'s safe point gets one input from Auto DJ,
`UpdateEnv::autoDjOwed`: a pick is due (the entry is last, conditions 1-3
and 6-9 hold) but the index is joining or reopening. No fence goes up
then (the update step waits with `Wait::AutoDj`), so the pick lands
first. It is never set by a hold. docs/ARCHITECTURE.md's "Library and
queue" has the update step's order.

### 4.7 Repeat, shuffle, the sleep timer

| Mode | With Auto DJ on |
|---|---|
| Repeat Off | Auto DJ adds at the last entry; a finished queue gets a cued pick |
| Repeat All | The queue doesn't reach its end while Auto DJ adds, so it doesn't wrap. A pick blocked for a whole track lets it wrap as usual |
| Repeat One | Waiting: the track loops. A pick already queued after it plays once One is off |
| Shuffle | Picks rank last and are never shuffled in. Turning shuffle on reorders Auto DJ's entries with the rest; the trigger uses play order |
| Sleep timer, End of queue | Waiting while it is set: the queue's end is where the timer stops. After it, the next pick is cued. The other choices are unaffected |

docs/QUEUE-MODES.md section 16 describes each mode with Auto DJ.

---

## 5. Sessions

A session is one run of picks: the ring, the cooldowns, the pin, the key
anchor, the random stream. It is never saved.

| Event | Session | Seed |
|---|---|---|
| A boot with Auto DJ on | New | None (3.3). The restored queue waits for Play |
| Switched on (the "..." row, the Output page, `D1`) | New; the failure pause cleared | None |
| A new queue while on: every Play (the Library's, Play all, Shuffle all), a Clear, and the undo of either, wherever asked | New (`QueueModel::playEpoch()`: no caller says so) | The new current track (an empty queue: none) |
| "Auto DJ from here", the Auto DJ button (`Dh`, `De`) | New; Auto DJ on if it was off | That track |
| `Dr` | New | None |
| A remount that finds another index (or one now, or none now) while on; `Dc` too, but after a read of the root the card failed, only when that read finds one of these (and not after the boot's: 6.5) | New | None |
| Play next, + Queue, a jump in the Queue, Remove, a move, Clear up next, their undo | Kept | Played tracks join the ring as they start |
| Skip, previous, seek | Kept | A quick skip leaves the ring (3.2) |
| Anchor, strictness, BPM continuity, Harmonic mixing | Kept | From the next pick; Anchor drops the pin |
| The mood filter (`Dq`) | Kept | From the next pick; a pick already queued plays once (3.14) |
| A library update (the fence and the read-back) | Kept: rows don't change, only track ids | The by-id cooldown cleared |
| A remount with the same index | Kept | |
| Switched off | Ends. What it added stays and plays out | |

---

## 6. The index on the device

`src/app/AutoDj` (the glue) and `lib/core/AutoDjJoin` (READ).

### 6.1 At boot

- The library's root (`/.mstream/manifest.bin`) names the index in its
  COMP entry. The glue opens that file as a raw FatFs file kept open in
  PSRAM, checks its frame and header (docs/AUTODJ-INDEX.md 8.1, 8.2) and
  the root's entry (generation, size, header CRC, selection). One or two
  small reads: "opened in 5.5-6.5 ms" (MEASURED 2026-10-13).
- It does this **whether Auto DJ is on or off.** So the empty states'
  button, the long-press row and the Output page know there is an index
  without a join.
- The release build reads MPDJ major 2 and writes `read.mpdj=2` in
  `device.txt` (from 0.11.0). An index of another major is "not read".
- **Usable** means: the file is open and checked, not found broken, and
  the library has at most 65,534 tracks (the maps' ids are u16).

### 6.2 The join (only while Auto DJ is on)

The join turns the index's rows into this library's track ids, and DJRF
into one filter word a row.

- **When:** while Auto DJ is on, once the UI's first frame is up: at the
  switch, after every library update (its Live), and after a remount that
  changed the index. **Switching Auto DJ off gives the join back.** A
  listener who never uses Auto DJ never pays for it (the engine's 19 KB is
  held from boot either way).
- **The steps,** in the format's order:
  1. **Folders:** each library folder's path hashed once (FNV-1a 64 is
     sequential: a track's hash goes on from its folder's with "/" and
     its name), with its length: 10 bytes a folder in one block (25 KB at
     2,484 folders, the user's library's shape at 19,412 tracks), for a
     library of up to 8,192 folders (else each track's whole path is
     hashed, as before). Since the device check of 2026-10-14, whose join
     was CPU-bound (7.3).
  2. **Hash:** each library track's path hash (FNV-1a 64), from its
     folder's state, kept as its top 32 bits and its id, 6 bytes a track,
     in runs of 4,096 (24 KB blocks).
  3. **Sort** each run.
  4. **STRS** checked.
  5. **DJRF** streamed with its CRC: each row's filter word (BPM rounded
     to a whole number, the Camelot code, the artist key's low 16 bits).
  6. **DJPH** streamed with its CRC, in hash order: each entry's track
     found by a cursor in every run, an equal prefix confirmed by the full
     64-bit hash (from the folder's state too). A row maps to its present
     file with the smallest path hash. A hash two library tracks share
     maps neither.
  7. The runs and the folders' states freed: ready.
- **Slices:** 250 folders, 250 hashes, one run's sort, or a 4 KB piece of
  DJRF or DJPH (100 confirms at most) a slice. A loop pass takes slices
  for up to 8 ms and at most one card read. None while a list moves or
  the card is held for a card swap's restart (not while the card worker
  saves `library.idx`: the join goes on then, sharing the core). The
  loop's idle wait is 2 ms while it works. `D` names the phase ("hashing
  the library's folders" first).
- **Its checks are the file's:** a section that fails its CRC or its
  order makes the file absent until a remount or the next boot.
- **The library under it:** every slice checks that the library index is
  still the one it began on; otherwise it aborts and frees everything, so
  it never reads a freed index.
- **Placement:** PSRAM's largest free block is what a library update's
  new track table needs (6.3), so the join takes as little of it as it
  can (`Join::setPlacement()`, for ESP-IDF's TLSF heap, which rounds a
  request up to its size class, by up to a thirty-second of it, and carves
  a block of that size from the low end of the closest-fitting free block:
  7.4's rules):
  - each block (the filters, the two maps, then the runs, the folders'
    states and the buffer) goes where the heap puts it, unless that is
    the largest free block: then it is given back at once and goes to
    that block's top;
  - those go under spacers that hold the block's bottom: the short-lived
    ones first; once the spacers are given back, the kept ones go to the
    room left above them. What comes later is carved below them, so
    giving the join back leaves one block, as if it had never joined;
  - **a spacer's size:** what stays above it is at least the blocks' bytes,
    a thirty-second more and 1 KB (each block's rounding, its header and
    poisoning), wherever in its size class the free block is, which the
    heap's answer doesn't say. It is asked just under a class's boundary,
    so that it takes that boundary and no more. What stays can be over by
    a class of the spacer's and one of the block's, a sixteenth of it in
    all; a second spacer closes that in unless a smaller free block is the
    closer fit for it, when it is given back at once;
  - **the margin:** the spacers never leave less than 64 KB free
    (`kJoinMarginBytes` in `src/app/AutoDj.cpp`, a KB more for the heap's
    headers), counting the free outside the largest block and the room
    kept for the blocks still to come, each block's rounding and the
    spacer's own among them; when that falls short, the rest is left above
    the blocks, which then sit that much under the block's top. (Until
    2026-11-02 the rounding wasn't counted. On the heap's rules, where the
    margin bites, PSRAM's free could fall to about 60 KB with a free block
    of up to 420 KB and about 48 KB with one of up to 830 KB. No join has
    measured that: the lowest of any is 77,284 B, 14.3.) 64 KB is there
    for another task's block in those few milliseconds (a track's start:
    the MP3 probe's 4 KB, a seek's 8 KB; the Opus packet buffer's 60 KB;
    the card worker's buffers), but it counts free bytes, not one block:
    that 60 KB needs one free block of 62,464 B, which a heap of holes
    doesn't promise. It doesn't cover the first Opus track's 125 KB of
    buffers (once a boot) starting in the same milliseconds: that track
    would fail to open and be skipped. 128 KB would cover it, but in
    `test_autodj_join`'s model as it was until 2026-11-02 it put the
    filters 39 KB under the block's top after a library update (the runs
    had taken the holes; not measured again since): left at 64 KB, a
    visible trade-off;
  - **another task's block** can still take the room above the spacers in
    those milliseconds. The placement falls short then, not the join: the
    spacers go and the rest go where the heap puts them, and 'joined' says
    "the room at the top fell short". Until 2026-11-02 that was NO PSRAM;
  - before 2026-10-14 every block went to the top under spacers over the
    whole block: PSRAM's free fell to about 10 KB at every join, and
    after a library update the rejoin took about 170 KB of the rebuilt
    heap's largest free block. On 2026-10-15 it took 73,728 B, with
    PSRAM's free at least 177,388 B meanwhile; the lowest of any join,
    86,868 B, was a `D1` on a heap that a library update with Auto DJ
    off had rebuilt (7.4);
  - the 'joined' line says where they went ("300622 B at the top of the
    largest free one, 0 of 3 kept and 0 transient blocks in smaller
    ones" at a boot, "77640 B at the top of the largest free one, 2 of 3
    kept and 7 transient blocks in smaller ones" after a library update,
    or "all in smaller free blocks"), how long `begin()` took (it asks
    for the largest free block a few times, a walk of the heap each:
    10.7-25.9 ms on 2026-10-15, 10.7-35.0 ms on 2026-11-02), and "PSRAM
    free at least N B meanwhile".

### 6.3 The give-back to a library update

The library update's memory check (docs/METADATA.md 3.4.2) needs one block
for the new track table. When it fails only on that block
(`Short::Table`) and `gb!` didn't ask for the deferral, its hook
(`LibraryUpdate::setGiveBack()`) gives the join back, measures PSRAM again
and checks again, the join's bytes no longer counted in `alsoFreed`. Once
a step at most. Auto DJ joins again after the update's Live, or at once if
the update defers or fails. So Auto DJ never puts a library update off to
the next boot.

### 6.4 The fence and the read-back

- `enterFence()`: first the DJ marks' positions are noted (while the queue
  is there); then the join is given back before the old index goes. Its
  bytes are in the update step's `alsoFreed`.
- Behind the fence the loop step only answers the trigger ("the library
  update's fence"): no pick, nothing owed.
- `leaveFence()`: the marks, the playing entry and a pick not yet played
  follow their positions under the read-back's new keys (when the queue
  has the size it had; otherwise they are forgotten). The by-id cooldown
  is cleared. The session is kept. The join runs again.

### 6.5 Remounts

On every FatFs remount (the card's guard counts them), but for one taken for the
echo of a try the card failed (within 30 s of it: 6.6), the glue reads the root
again into its own copy (never the library's) and compares the index entry:

- **The same index:** the file reopened; the maps and the session kept; the
  damaged ring cleared. A join under way starts again. A reopen the card fails
  goes by 6.6's tries: the maps go, the session stays, and the join follows the
  try that opens it.
- **Another one, one now, or none now:** closed, opened, and, while Auto
  DJ is on, joined again and a new session (`Dc` starts one on or off).
- No PSRAM for the root: the index closed until the next remount, its
  entry kept.
- **The root unread** (the card failed a read of it: `cardroot::Root::readFailed()`,
  at a remount or at the boot): no index is taken to have gone. The file closed and
  the maps gone, the entry and the session kept, and the root read again by 6.6's
  tries (none for a boot whose card jobs run: 6.6); then as above, but for the boot's,
  which has no root to compare and keeps its session whatever it finds. Before
  2026-11-02 this read as no index now, with a new session, and `Dc` couldn't bring
  it back.
- **The card isn't walked again:** a remount reopens the records and the
  index, but only a boot walks the card. Music files added to a card
  swapped while the player runs wait for the next boot (12).

On 2026-10-16 (14.2) both cases ran with a card pulled and put back. The
same card: "the card was mounted again (the same index): the file
reopened, the maps and the session kept". A card whose index a PC had
replaced: "index /.mstream/autodj-00000002.bin (after a remount) ...
opened in 2.8 ms", a new session ("another index on the card"), joined
1.6 s later.

### 6.6 What makes the index unusable

| What | Effect | The Output page's status |
|---|---|---|
| No transfer data on the card, or the root names no index | Picks by id | No Auto DJ index on this card |
| An index of a major this build doesn't read | Picks by id | No Auto DJ index on this card |
| The file failed its checks (frame, header, the root's entry; a section in the join) | Absent until a remount or the next boot: picks by id | The Auto DJ index failed its checks |
| A broken slot (3.13) | Random picks over the index's rows until a remount or the next boot | The Auto DJ index failed its checks |
| The file the root names isn't on the card (FatFs's FR_NO_FILE or FR_NO_PATH), no PSRAM for the root | Absent until a remount or the next boot: picks by id | Can't read the Auto DJ index |
| Opening it, or reading the root, the card failed: a read, a pulled card's status and init, the volume's lock, or no PSRAM for its FIL | Picks by id; tried again after 2 s, then 10 s, then 30 s, the session kept; the last failing too, as the row above | Reading the Auto DJ index…, then Can't read the Auto DJ index |
| The boot's reads of the root failed, and its card jobs run (METADATA.md 3.2.2) | Picks by id; no tries: until a remount, `Dc` or the next boot | Can't read the Auto DJ index |
| A library over 65,534 tracks | Not joined | Too many tracks for the Auto DJ index |
| No free block holds one of the join's blocks, its spacers given back | Picks by id until the next library build, or until Auto DJ is switched off and on, which tries again | Not enough memory for the Auto DJ index |
| A read failed during the join | Picks by id; joined again 10 s later, on a new handle or at the remount a failed status brings; the third in a row, until the next library build, a remount, `Dc` or switching on (the empty states offer Shuffle all meanwhile, and no "Auto DJ from here") | Reading the Auto DJ index…, then Can't read the Auto DJ index |
| A read failed in the look-ups behind the Auto DJ button or "Auto DJ from here", twice in a row (Auto DJ off) | As the row above: Shuffle all in its place, no more of these reads, and no toast at the tap that found it | Can't read the Auto DJ index |
| The card kept going silent, at any reader's read or write (METADATA.md 3.8's too silent), with an index open or a try due | Absent at the next remount: picks by id until the next boot (`Dc` asks, until the remount after it). A card with no index says what it said | Can't read the Auto DJ index |
| No library: an update step failed, and `library.idx` with it | Not joined until there is one. (Behind a fence, as a join: one is coming) | "Auto DJ index: {rows} tracks" |
| The file older than the library | Rows whose files are gone map to no track; new files have no row | "{mapped} of {tracks} tracks have neighbours" |

The status's second line is "picks are random" under each reason but
"Reading" and the last two.

Switched off and on, the join is tried again at once, on the heap as it is
then: that helps once something has freed memory, and not before. Until
2026-11-02 this row also came up with plenty free (7.4's rejoin), and then it
went by where the largest free block had got to in its size class. `D0` and
`D1` a second after the failure failed the same way, twice, the block still
at 417,780 B; about 15 s later, the block at 303,092 B, they joined (MEASURED
2026-11-02, `e7623e0`).

A fault of the card says nothing of the file. A command that gets no answer sets
the SD driver's mark, and the wrapper looks at once, in that read's own disk call
(METADATA.md 3.8): the card back at its init, the same card, the read is asked once
more there (a one-off reads, and FatFs never hears of it), and all FatFs hears when
that fails too is that read's error, with nothing open lost. A read that fails any other way (a select
timeout, an R1 error, no data token, a CRC mismatch) sets nothing. Until 2026-11-02 one
such glitch in the boot's open left picks by id for the session. FatFs keeps a failed
read's error on its `FIL`, whoever read (a slot, a join, the lookup behind "Auto DJ from
here"), so the next pass goes on with a new one, the maps and the session kept
(`refreshFile()`). A `FIL` only a status failed, as on a plain pull, holds no error: it waits
for the remount's reopen, the maps kept; a read that failed on it makes the call of FatFs's that
brings that remount at the next pass (`echoNow()`), since an idle player makes none, and a card put
back would leave the button and "Auto DJ from here" dead until a track played. A pull inside a read does hold one, and the new `FIL`'s
open then goes by the tries, the maps gone. A read that keeps leaving the card silent stops being asked
after 4, whoever asks (METADATA.md 3.8's read stop): the blocks below and the join's
are lifted by a remount, and each such read makes one. A card put back asks all of those again. The tries (`lib/core/AutoDjOpen.h`,
test_autodj_open) are on the loop task, as the open is: a card that doesn't
answer costs about a second of the driver's retries each, a read of the root
about 2.5 s, its four calls of FatFs (ESTIMATED), so there are few, the first soon,
since most faults are a glitch. None starts before the UI's first frame, while a
list moves, while the card is held or behind the update's fence. `Dc`, or a read
of the root or an open that works, counts from the first try again, and so does a
remount when the last try was no fault of the card's.

A card that is out, or silent past its init, keeps the mark, and the next call of
FatFs, whoever's, once a card answers, is a remount: that read's echo. Taken for news of the card it would start the tries
over and read at once, get no answer, and so on at every pass, seconds of the loop
each, the waveform bar's reads of the root answering Auto DJ's and the other way
about. So within 30 s of a try the card failed a remount does nothing and the wait
stands; later than that it asks once, the count where it was. A card put back
within those 30 s of the last try waits for the next remount or boot. A reader of
the root makes that call itself, right after a read it failed
(`cardfat::FatFs::echoNow()`): left to the next caller, on a player whose calls come
far apart, the echo would come later than that, as news, a read and a new mark each
time, and the open of a track that made the call would be cut. On a card that is out
it costs one failed init a read: 0.62 s, three times over (MEASURED 2026-11-02). The
first status of a mounted volume, two rounds of CMD13 and that init, took 1.23 s.

A remount with a card job under way restarts the player (`stepCardGuard()`,
METADATA.md 3.8), and the job counts from the UI's first frame, when the walk is
armed. The first try falls there, so a card silent past its init would restart there,
before its walk began, at most twice a power-on since 2026-11-02 (the breaker, 3.8). So a boot that couldn't read the root and runs
its card jobs all the same (a library with no transfer in it, or one it had to build
or walk: METADATA.md 3.2.2) has no tries of the root. A remount or `Dc` still asks, once each. A boot that holds
its jobs has them, as a session whose boot read the root does: the guard doesn't
restart with no jobs, and at most once after a good boot, whose next boot is one of
these.

---

## 7. What it costs

All on the loop task, in Auto DJ's step.

### 7.1 A pick: card reads

| | Reads |
|---|---|
| Budget (READ) | At most 2 a pick; 1 read ahead a loop pass between picks |
| Follow the vibe, 2-hour soak (MEASURED 2026-10-13) | 7,519 of 7,520 picks read nothing, 1 read one slot |
| Dry runs, every setting (MEASURED 2026-10-13) | 0 reads at every pick |
| Picks over 2 reads, the whole check (MEASURED 2026-10-13) | 0 of 7,676 |
| Random picks, picks by id (READ) | 0 |

A slot read, when one is made (`Db200`, MEASURED 2026-10-13, 512-byte
slots of a 10 MB index):

| | p50 | p90 | max |
|---|---|---|---|
| With a 320 kbps MP3 playing (two runs) | 1.4 ms | 3.7-4.0 ms | 13.2 / 20.7 ms |
| With a FLAC playing (two runs, one while scrolling) | 1.2 ms | 3.3-3.5 ms | 17.0 / 17.3 ms |
| During the boot's card walk (contended) | 11.9 ms | 40.8 ms | 88.0 ms |

### 7.2 A pick: CPU

The first device check measured Follow the vibe at 2.4-4.5 times the 2 ms
target: PSRAM cache-line misses, a sort of the union and a scan of the 50
cooled rows for each candidate. `7c89673` made each cached slot keep its
sorted run (3.4): a pick merges up to 5 runs, walks the cooled rows beside
them, and reads no map but the chosen row's. The picks are the same:
960,000 picks on the synthetic card's index against the old engine, 0
differences. The modelled misses per Follow the vibe pick fell from 2,138
to 309.

Dry runs (`Dn200`), p90 of a pick:

| Setting | MEASURED 2026-10-13 (`09f91a1`) | MEASURED 2026-10-14 (`a7ee54b`) |
|---|---|---|
| Follow the vibe 55 / 65 / 75 %, matching off, 240 MHz | 7.7 / 6.0 / 4.8 ms | 1.33 / 1.17 / 1.08 ms |
| Follow the vibe 55 / 65 / 75 %, matching on, 240 MHz | 9.0 / 6.5 / 5.2 ms | 1.70 / 1.41 / 1.24 ms |
| Stay on seed 55 / 65 / 75 %, matching off, 240 MHz | 1.5 / 1.1 / 0.6 ms | 0.42 / 0.38 / 0.57 ms |
| Stay on seed 55 / 65 / 75 %, matching on, 240 MHz | 1.7 / 1.3 / 0.6 ms | 0.53 / 0.46 / 0.62 ms |
| Follow the vibe 55 % off / 75 % off / 55 % on, 160 MHz | 10.1 / 6.3 / 12.0 ms | 1.73 / 1.37 / 2.31 ms |
| Stay on seed 55 % matching off, 160 MHz | 1.9 ms | 0.52 ms |

- So the p90 is 0.38-1.70 ms at 240 MHz and 0.52-2.31 ms at 160 MHz,
  inside the targets (2 ms and 3 ms). Every run read 0 slots at its picks
  (200/0/0), and Follow the vibe's repeats, artists, classes and breaks
  were the host's (the same picks as before the optimisation). The host
  benchmark's estimates (1.1-1.65 ms at 240 MHz, 0.35 ms for Stay on
  seed, 1.45-2.3 ms at 160 MHz) were close, Stay on seed a little slower;
  at 75 % it takes longer than at 65 % since it loosens (188 of its 200
  picks).
- A live pick, less its reads (2026-10-13's seed and queue, `Ds717feda5`
  then `qp0`, the pick row 6880, 1 read): 1.47 ms at 240 MHz, 1.71 ms at
  160 MHz (MEASURED 2026-10-14; 3.13 ms on 2026-10-13).
- Live picks over 30 minutes of stubs (MEASURED 2026-10-14, Follow the
  vibe 55 %, matching off, 1,855 picks, all 0 reads): p50 1.32 ms, p90
  1.58 ms; max 16.18 ms, once, while an Opus 48 kHz stub played through
  the rate converter (the next slowest 2.17 ms). On 2026-10-13, before
  the optimisation: p50 7.9 ms, p90 8.4 ms, max 23.0 ms over 2 hours.

### 7.3 The join

| | |
|---|---|
| Reads (READ) | DJRF, DJPH and STRS once: 18 bytes a row, in 4 KB pieces (about 90 at 19,410 rows; 87 MEASURED) |
| Time, 1,000-hash slices one a pass (MEASURED 2026-10-13, `09f91a1`) | 2.08-2.12 s idle (112 slices), 2.46 s at 160 MHz; after a fence, 4.6 s with playback paused and 7.2 s with a 320 kbps MP3 and the screen dark (20 ms of idle wait a pass); a scroll adds its own time |
| Time, 8 ms of smaller slices a pass and a 2 ms wait (MEASURED 2026-10-14, `a7ee54b`) | Idle, lit or dark: 1.97-2.00 s (317 slices in 164 loop passes, about 12 ms a pass); at a boot 1.96-1.99 s; with a 320 kbps MP3: 3.77-3.93 s (182-188 passes); after the Auto DJ button (playing at once) 2.24-2.28 s; a scroll adds its own time (30 fps kept: 9.86 s with 5.8 s of scrolling). The estimate was 1.5 s idle and 2.5 s with the MP3: it took 164 passes of 8 ms of slices, not the 110-130 estimated |
| The rejoin after a library update (MEASURED 2026-10-14) | 6.6-6.9 s from the read-back's "the join again" to "joined", the MP3 playing and the screen dark (7.2 s before). The join went on throughout, but the card worker's save of `library.idx` (4.6-4.7 s from "the join again") runs at the loop's priority while the screen is dark (`ScanScheduler`) and shared core 1 with it and the decoder: 199-205 passes, about 32 ms a pass (12 idle, 20 with the MP3); "joined" came 2.0-2.2 s after "saved". So the join's CPU is the lever (the next row) |
| Why, and the Folders phase (READ; host, ESTIMATED for the device) | The join was CPU-bound: 164 passes of 8 ms of slices is about 1.3 s of CPU; hashing whole paths (the Hash phase's 19,412 and DJPH's confirms) was about two thirds of it. The Folders phase (6.2) hashes each folder's path once: on the host the hashing is 3.3 times faster (3.36 → 1.0 ms) and the 19,412-track join's CPU about 46 % less (11.2 → 5.8-6.1 ms quiet, 23.4 → 12.7 ms loaded), the maps identical. The pass rule replayed on the host's slice times, scaled so the old join takes the device's 164-177 passes: about 107-110 passes and 0.55-0.65 s of join CPU (1.0-1.25 s before). The estimate was about 1.1-1.3 s idle, 2.1-2.4 s with the MP3 dark, the rejoin about 4.5-5 s (MEASURED since: the next two rows). The pass's budget is unchanged (8 ms of slices, one read, the 2 ms wait). Not done: the save kept at priority 0 while a join runs (the join faster, the save slower), and runs of 2,048 tracks (a run's sort is one slice, about 20 ms on the device by estimate, over the 8 ms budget; as before) |
| Time, with the Folders phase (MEASURED 2026-10-15, `314c624`) | Idle, the screen dark: 1.48 s in 110 passes (twice); at a boot 1.54 s; 1.79 s with three `D`s printed during it. With the 320 kbps MP3, dark: 2.99-3.01 s (148-149 passes); dim: 3.46 s (155). With a FLAC, dark: 2.06 s (124). The passes came as estimated (105-115 idle), but each took about 13.5 ms, not 12: over the estimates by 0.2-0.4 s idle and 0.6-0.9 s with the MP3, and 20-25 % under 2026-10-14's. `D` during a join named "hashing the library's folders" first, then DJRF and DJPH; 87 reads every time |
| The rejoin after a library update (MEASURED 2026-10-15 and 16) | With the MP3 and the screen dark: 5.9-6.2 s by the 'joined' line (168-176 passes), 6.0-6.3 s from "the join again" (6.6-6.9 s on 2026-10-14; the estimate was 4.5-5 s). The card worker's save of `library.idx` took 4.4-4.8 s of it; "joined" came 1.4 s after "saved" (2.0-2.2 s before). One pass at the end of the second rejoin took 106 ms (the ring held 1.4 s: no underrun). After the boot's own update (2 files added, nothing playing): 4.74 s (167 passes), "joined" 2.2 s after "saved" (2.5 s) |

### 7.4 Memory

At 19,410 rows and 19,412 tracks, all in PSRAM:

| Item | Bytes | When |
|---|---|---|
| The engine: slot cache (7 × 512), runs, the pick's scratch (READ) | 19,016 (15,414 before `7c89673`, MEASURED) | From boot, on or off |
| The open index file (MEASURED) | 4,148 | While an index is open |
| The glue (MEASURED, the M4 build) | 2,968 | From boot |
| The join's maps and filters: 2 B a track + 2 B a row + 4 B a row (MEASURED) | 155,284 | While Auto DJ is on and joined |
| ... its largest block, the filters (MEASURED) | 77,640 | |
| The join's peak: the maps + runs at 6 B a track + a 4 KB buffer (MEASURED at `a7ee54b`) | 275,952 | A second or two while it joins |
| ... since, the folders' states too, 10 B a folder (MEASURED 2026-10-15 at the synthetic card's 2,477 folders: 275,952 + 24,770) | 300,722 | |
| `Dn`: a copy of the engine, 16 KB of tallies, 4 B a pick (READ) | about 35 KB + 4 B a pick | While a `Dn` runs |
| `Db`: 4 B a read and one slot buffer (READ) | | While a `Db` runs |
| Not Auto DJ's: the library's mood words, 2 B a track in `library.idx` v7 (docs/MOOD-FILTERS.md 5.4; MEASURED 2026-11-06 at 19,413 tracks, 17,954 with a word) | 38,826 | From boot, on a card whose T carries mood tails |
| ... PSRAM's largest free block with them, against a control in the same script (MEASURED 2026-11-06, 14.3) | -32,768 at every step: the boot off and joined, each update joined, `Dc` | |

- Internal RAM: main.cpp's pointer (READ). The pick allocates nothing; its
  frames are about 512 bytes on the loop stack, `pick()` and `pickOnce()`
  (MEASURED-HOST 2026-11-06: the ESP32 compiler's `-fstack-usage` at `-Os`;
  416 bytes before the re-pin split them, docs/MOOD-FILTERS.md 9.8).
- MEASURED 2026-10-13: the resident set was 177,814 B (with the old
  engine), against the design's 180 KB. The running player's lowest free
  PSRAM was 547 KB idle or with an MP3, 459 KB with a FLAC: the join's peak
  came after the UI's boot dip and never set it.
- Auto DJ on against off (MEASURED 2026-10-14, 2 minutes of a 320 kbps MP3
  each): `[stats]` psram= 631K on and joined, 783K off, in every line; `D`'s
  PSRAM free 646,252 B against 801,912 B: 155,660 B, the join's 155,284 B
  and 376 B more. The engine's 19,016 B either way.
- **The free dip (MEASURED 2026-10-14; fixed, MEASURED 2026-10-15):**
  every join (the switch-on, a boot's, a rejoin) took PSRAM's free down
  to about 10 KB for a moment (`[stats]` pmin= 679K → 10K at the first
  `D1`, and pmin= stuck there until a reboot): the spacers held the whole
  largest free block. Now they leave at least the 64 KB margin (6.2). In
  `test_autodj_join`'s model heap (ESP-IDF's TLSF: the size classes'
  rounding, the low-end carve, coalescing) the lowest free while placing
  was 11,316 B with the old spacers at a boot's join and is 168,136 B now,
  over a boot's join and the rejoins after two library updates; over 30
  heap shapes and margins and about 270 sizes of heap, at 20,000 and 6,001
  tracks, and about 700 sizes of free block with the kept blocks in holes,
  never under the margin (or under what the join itself leaves, when that
  is less).
  **MEASURED 2026-10-15** (`314c624`, 17 joins): "PSRAM free at least"
  170,416 B at a boot's join and at each `D1` before any library update,
  167,792 B with the MP3, 177,388 B at both rejoins after `gb`, 158,088 B
  with a screenshot's 154 KB buffer held. The lowest, 86,868 B, was the
  first join on a heap that a library update with Auto DJ off had
  rebuilt. `[stats]` pmin= read 166K after the boot's join, then 163K,
  then 84K from that join on: never under the 64 KB margin, never
  10-14K. Three joins given back mid-way (an Undo that switched Auto DJ
  off) gave back the peak, 300,722 B; the largest free block was then
  409,588 B, as before them.
- **The rebuilt heap (MEASURED 2026-10-14; fixed, MEASURED 2026-10-15):**
  after each library update with Auto DJ on, PSRAM's largest free block
  was 180,212 B and the join kept at 0x3f94becc, against 622,580 B and
  0x3fbd69bc before. A library rebuild itself fragments PSRAM, with Auto
  DJ off too (`main`'s own: in `test_a_library_update_in_a_model_heap`,
  the real `LibraryIndex` and `LibraryBuilder` at 19,412 tracks in main.cpp's
  order, the largest free block goes from 753,652 B at the boot to
  360,436 B after one update with Auto DJ off; the fence's frees leave
  the same free blocks either way). What Auto DJ added was the rejoin's
  placement: all 155 KB and about 20 KB of slack at the top of that
  block (on the device 180,212 B left, the join low in PSRAM where the
  rebuilt heap's largest block was). Now the maps and every transient
  block fit the rebuild's holes and, on the device, only the filters
  (77,640 B) go to its top (the model's rebuild has a hole for them too).
  Given back it is exactly Auto DJ off's (the give-back always restored
  the off figure, so a library update's memory check sees what it would
  with Auto DJ off, 6.3). While joined it can't be as large as off when
  no smaller block holds the 77.6 KB filters.
  **MEASURED 2026-10-15** (`314c624`, the 320 kbps MP3 playing): after
  `gb` with Auto DJ on, "77640 B at the top of the largest free one, 2 of
  3 kept and 7 transient blocks in smaller ones" (the model's words), and
  PSRAM's largest free block was 335,860 B while joined. It was
  409,588 B once given back (`D0`), and 409,588 B after `gb` with Auto DJ
  off: given back equals off exactly, joined is 73,728 B under it
  (2026-10-14: 180,212 B, about 175 KB under). The largest block went
  from 819,188 B at the boot (Auto DJ off) to 409,588 B after one update
  (the model: 753,652 → 360,436 B).
- **The rejoin's NO PSRAM (MEASURED 2026-11-02; fixed that day):** on the
  spare card, `gb` on a fresh boot, the player stopped: "[heap] FAILED:
  77640 B", largest block 73,716 B, PSRAM free 245,884 B of the 816,024 B
  free when the join began, then "the join: NO PSRAM" and picks at random.
  That was `e7623e0`. The same at `26eec75` and at `24b4cf0`, before the
  waveform bar, with 570,140 B taken every time.
  - **The heap's rules** (ESP-IDF 5.5, as the framework's `libheap.a` has
    `tlsf_malloc()`, `tlsf_fit_size()` and the poisoning's wrappers): a
    request gets 12 B of poisoning and 4-byte alignment and is rounded up
    to its size class's next boundary (32 classes a power of two). That
    rounded size is what is carved, with a 4 B header, not only what the
    search asks for. `heap_caps_get_largest_free_block()` answers with the
    largest request that succeeds: the largest free block rounded down to
    its class, less the 12 B, so it doesn't say where in its class the
    block is.
  - **What went wrong:** the spacer took what the blocks needed (81,090 B
    for the filters) off that answer. Of a block that read 417,780 B it
    asked 336,690 B and took 344,064 + 4 B, leaving 73,724 B and however
    far the block was over its class's floor, where the filters' 77,640 B
    take 77,824 B. So the rejoin failed with the block in the lowest
    4,100 B of its class's 8,192 B from 344,064 B to 524,288 B, the lowest
    4,100 B of 16,384 B up to 606,208 B and the lowest 12,292 B of
    16,384 B above that (below 344,064 B only the lowest 4 B): 56 % of the
    places from 262 KB to 1 MB.
  - **Why it passed in October:** neither `spacerFor()` (`314c624`) nor the
    framework has changed since. The rebuilt heap has: the 409,588 B of
    2026-10-15 must have been at least 413,700 B, in the upper part of its
    class, and its "73,728 B under" was this rounding. A few KB held
    elsewhere move the block across the edge. The model carved what was
    asked (81,088 B under), so it couldn't see this.
  - **The fix:** 6.2's spacer, sized from the rules. `test_autodj_join`'s
    model now keeps them and is pinned to the device's numbers (the
    570,140 B taken at every failure, the 73,716 B, 225,268 B and 335,860 B
    answers), and the rejoin is tried wherever the block is in its class, in
    six classes at three library sizes. A block that falls short anyway no
    longer costs the join. Results: 14.3.
  - **Also fixed by it:** `probe()` read the same answer to tell a block
    taken from the largest free block, so one smaller than the block's
    distance over its class's floor went unseen and stayed at its low end.
    At a boot that was the 4 KB buffer, "1 transient blocks in smaller
    ones" (its address now tells). Transient, so no harm done there.

---

## 8. The settings

| Setting | Where | Console | NVS key (u8 but `moods`) | Values | Default |
|---|---|---|---|---|---|
| Auto DJ | "..." row, Output > Auto DJ, the empty states' button, "Auto DJ from here" | `D0`, `D1` | `on` | 0 off, 1 on | off |
| Anchor | Output > Auto DJ | `Dm0`, `Dm1` | `mode` | 0 Follow the vibe, 1 Stay on seed | Follow the vibe |
| Match strictness | Output > Auto DJ | `Df55` … `Df75` | `floor` | 55, 60, 65, 70, 75 | 55 |
| BPM continuity | Output > Auto DJ | `Dt0`, `Dt1` | `match` bit 0 | | off (D2) |
| Harmonic mixing | Output > Auto DJ | `Dk0`, `Dk1` | `match` bit 1 | | off (D2) |
| Mood filter (3.14) | The console, until Output > Auto DJ's Smart filters row (docs/MOOD-FILTERS.md S5) | `Dq<hex>`, `Dq0` | `moods` (u16) | `q`'s 11 bits (MOOD-FILTERS.md 5.4) | none |

- **NVS:** the namespace `autodj`, from schema 4; `moods` from schema 5.
  Every change writes all five keys at once. Each is read with `isKey()`
  first: an absent key, or a value this firmware doesn't take (a later
  one's; a `q` that isn't a filter), reads as its default. `migrate(3, 4)`
  and `migrate(4, 5)` do nothing; an older firmware never reads the
  namespace, and 0.12.0 never reads `moods` (its picks are unfiltered).
  PR #27's card transfer takes schema 5 too: shipped together, they share
  it (MOOD-FILTERS.md 10). (`lib/core/NvsLayout`, `src/app/AutoDjStore`.)
- **Not settings:** 5 anchors, the 50-track and 3-artist cooldowns, the
  30 s quick skip, 8 / 10 BPM, 2 reads a pick, the 50 of a dry or loosened
  pool, 64 random draws, 16 tries by id, 3 failures for the pause.
- **Not saved:** the session, the DJ marks.
- **After a boot** with Auto DJ on: nothing plays; the restored queue waits
  for Play, and Auto DJ adds at its last entry.

---

## 9. The UI

The rules are in `lib/core/AutoDjView` (host-tested: `test_ui_autodj`);
the drawing in `src/ui`. docs/ARCHITECTURE.md's "UI" section has the
pages, the sheets and `UiHost`'s Auto DJ calls. 65 messages, in all ten
languages (machine translations, not yet reviewed by native speakers;
the mobile app's words where its English matches). 13 of them are the
mood filter's of 2026-11-06 (docs/MOOD-FILTERS.md 8, stage S5); the
editor's own words are MOOD-FILTERS.md's.

### 9.1 Now Playing's "..." menu

A 4th row, **Auto DJ**, its state the detail: **On**, **Off**,
**Waiting** (Repeat One, the sleep timer's End of queue, a mood filter no
track fits) or **Paused** (failures). A tap switches it in place, the
sheet staying, as Shuffle. The 4-row sheet rises into the header row.

Under a mood filter (3.14) the detail says what is set, "On · Relaxed ·
Instrumental", in the accent (whole words, as many as fit beside the
label); "Waiting" for want of a track that fits, in amber. Every other
detail is dim, as the other rows' (`autodjview::sheetDetail()`).

### 9.2 Output > Auto DJ

A row on Output's root: "Auto DJ" over its line, "On · Follow the vibe",
"Waiting · Stay on seed", "Off". The Anchor is in the line only when the
index is usable and the whole line fits the row's 242 px; otherwise the
state alone ("On", "Waiting", "Paused"). The page ("keeps the queue
going"):

| Row | Shows | A tap |
|---|---|---|
| Auto DJ | a switch; "adds a track as the last one starts" | switches it |
| Anchor | Follow the vibe / Stay on seed; its hint | swaps |
| Match strictness | "55% or closer"; "how similar each pick must sound" | the next step: 55, 60, 65, 70, 75, 55 (from the value shown) |
| Smart filters | "Any", or what is set ("Relaxed · Instrumental"); "keeps every pick inside these moods" | the mood filter's editor (below) |
| BPM continuity | a switch; "within 8 BPM, or half/double tempo" | switches it |
| Harmonic mixing | a switch; "keys that mix well" | switches it |
| Status | two lines (below) | nothing |

- **Without a usable index** Anchor, Match strictness and the two
  switches are dim, "needs an Auto DJ index" on their second line; a tap
  gives the amber note "Needs an Auto DJ index: a transfer to the card
  adds one". The switch, Smart filters and the status stay live.
- **Smart filters** (docs/MOOD-FILTERS.md 4.4) lives by the card's mood
  data, not the index: with none it is dim, "No mood data", and a tap
  gives the amber note "No mood data: sync this card from mStream" (a
  filter set before is kept, its picks unfiltered). With data and no
  index Auto DJ can use, it stays live and the picks are random inside the
  filter. Its value is "Any" with no group set (a level alone filters
  nothing), else what is set, beside the title when it fits whole, else
  on the second line in the hint's place, cut to it. A tap opens the
  Albums list's editor (one screen of moods, MOOD-FILTERS.md 4.3, 7.1) in
  Output's blue, counting tracks: "1,204 of 19,412 tracks". Each tap there
  is applied and saved at once, its count the glue's; the cross closes
  it, and a change settles then (9.6's toasts). Behind the library
  update's fence the row keeps its last value and a tap says "Updating the
  library: a moment".
- **A dim row keeps its value.** Anchor's and Match strictness's values
  sit beside the title when they fit there, as on a live row. Otherwise
  the value takes the second line, in the reason's place
  (`uitext::autoDjSecondLine()`). That happens for Match strictness in
  de, es, it and pt, where "55% or closer" in their words doesn't fit
  beside the title. The value and the reason together on that line fit
  only in it (271 of 288 px; de 324, es 334, pt 361 px), so that row
  shows the value alone. The rows above and below it, the status row and
  a tap still say why. Until 2026-10-16 the reason took that line and the
  value wasn't shown in those four languages (the device check of
  2026-10-16, 14.2).
- **The status row:**
  - joined: "{mapped} of {tracks} tracks have neighbours" over "BPM {n}% ·
    key {n}%" (the mapped tracks' coverage);
  - Auto DJ off: "Auto DJ index: {n} tracks" over "read when Auto DJ is
    switched on";
  - on with no library (6.6): "Auto DJ index: {n} tracks", alone;
  - joining, or a try at opening the file due (6.6), on or off: "Reading the
    Auto DJ index…";
  - no index, or none usable: why (6.6) over "picks are random";
  - held: "Waiting: Repeat One is on", "Waiting: sleep timer set to End of
    queue" or "Paused: tracks failed to play", over the index's first line;
    a filter no track fits: "Waiting: no track fits the filters", in
    amber, over the coverage once joined;
  - with mood data on the card, the coverage adds it: "BPM 95% · key 97%
    · mood 92%";
  - a mood filter on (3.14), joined or with no index Auto DJ can use:
    "1,204 of 19,412 tracks fit", or 1 to 50 (fewer than the track
    cooldown: they repeat) "Only 37 tracks fit" in amber, over the coverage
    ("They will repeat · BPM 95% · key 97%" for 1 to 50) once joined, else
    over the index's first line. Found and Reading say what the index does,
    as with no filter.
- The first line is amber for the failure pause and the mood filter's
  Waiting and few; the second is dim (`autodjview::statusAmber()`).
- Switching on here with nothing queued and a usable index: "Auto DJ on:
  it follows the next track you play". Nothing starts.

### 9.3 The empty states

The empty Queue and the empty Now Playing show **[Open Library] [Auto
DJ]** in Shuffle all's place when the index is usable and the library has
tracks, Auto DJ on or off. Without one, Shuffle all stays. B stays inert
on an empty queue.

The button:
1. draws a track that has neighbours: a row of the maps when joined (up to
   64 draws); else up to 8 library tracks, each looked up in the index's
   DJPH by its path hash (a binary search, a few small reads);
2. plays it as a one-track Play (a new queue);
3. switches Auto DJ on, a new session seeded by it. Playback doesn't wait
   for the join; the first pick does.

Under a mood filter its track is inside the filter (3.14). When no track
with neighbours fits, it plays nothing, as when the draws find none.
When no track on the card fits the filter, the empty states say "No track
on this card fits these filters" and show [Open Library] alone
(`autodjview::emptyNoFit()`; frame `dj-empty`).

Its toast is "Auto DJ: {title}", with Undo. No track in the draws: "Found
no track with Auto DJ neighbours: try again". During a library update it
asks to wait.

### 9.4 "Auto DJ from here"

The **first** row of the Library's long-press sheet on a track the index
knows (a row in the maps; before the join, its path hash in DJPH: a few
small reads once a long press). Play, Play next and + Queue keep their
places, and Play stays the sheet's primary row. Albums, artists and
folders keep 3 rows. It replaces the queue with the track, switches Auto
DJ on, and starts a session seeded by it. Its toast: "Auto DJ from here:
{title}", with Undo.

**Undo** of either (the button's or this row's Play) puts the queue back
and, if that tap switched Auto DJ on, switches it off again. The queue
comes back in the play state it had: one that was paused stays paused
(its entry cued at 0:00, Auto DJ's cue mark kept), one that was stopped
stays stopped, and nothing that was silent starts
(`PlaybackController::undo()`, for every Play's and Clear's Undo). The
device check of 2026-10-14 found a Library Play's Undo resuming a queue
paused on Auto DJ's cued pick, and Auto DJ picking on: a rule older than
Auto DJ (the same `undo()` on `main` at `41d6721`), fixed since. On
2026-10-15 (14.2), with every start logged, no Undo started sound: a
Library Play's over a paused queue, over one paused on the cued pick
(no pick after), over a stopped one and over a playing one (the old
entry played from 0:00); "Auto DJ from here" over paused, stopped and
playing queues (Auto DJ off again when the tap had switched it on); the
empty Queue's Auto DJ button; a Clear's (the console's `qx`, `qu`).

### 9.5 The Queue

- A row Auto DJ added carries a small dim **DJ mark** at its right edge;
  a failed track's amber "!" takes the place instead.
- The marks are the keys of the last 64 entries Auto DJ added (RAM only).
  A library update carries them by position (6.4); a reboot loses them.
- Auto DJ's appends don't flash the Queue badge and don't set the
  Library's "added" mark.
- Auto DJ's append keeps the listener's undo: the quiet append adds the
  pick to that edit's snapshot too (not to a Play's or a Clear's, whose
  undo brings back the queue from before). When it can't (the queue or
  the snapshot at the 5,000 cap, or no memory to grow the snapshot), the
  undo goes, and the Undo toast with it.

### 9.6 Toasts

| When | Text | Kind |
|---|---|---|
| 3 picks in a row failed to play | Auto DJ paused: tracks failed to play | amber note, at once |
| A finished queue got a cued pick | Auto DJ: the next track is ready, press play | toast |
| A session's first pick was random: its track has no row (Stay on seed) | This track has no Auto DJ neighbours yet: the first pick becomes the seed | toast |
| ... (Follow the vibe) | This track has no Auto DJ neighbours yet: the next track is random | toast |
| Picks by id: no usable index (once a switch-on, or a boot) | No Auto DJ index: picks are random | toast |
| A mood filter that 1 to 50 tracks fit (3.14) | Only 37 tracks fit: they will repeat | amber note |
| ... changed while Auto DJ's pick up next fails it (D5) | Plays once, then the filters apply | toast |
| ... a pick with no close match inside it (a jump) | No close match: any track that fits | toast |

- One a pass: the most pressing, in the table's order. With no filter
  the last three never come, and the toasts are as before.
- **Two lines:** a toast too wide for one line ("Auto DJ from here: {title}",
  the no-neighbours toasts) splits at its ": ", the part before it in
  Small over the rest. Each line goes by the rows it inks with the
  firmware's fonts and CJK store (`uitext::toastLines()`), an empty row
  between them, both inside the card (rows 2-33, or all 36 when they need
  them; two 17-row CJK lines take its top and bottom rows, and the card
  keeps only its sides and corners then). On 2026-10-14 the Japanese and
  Chinese ones overlapped: the store's 16-17 px glyphs are taller than the
  Small font's Latin, and the fixed rows put the first line into the
  second and over the card's top edge. `test_ui_strings` places every
  two-line toast of the ten languages so. Most Japanese and Chinese ones
  and some Latin ones now take the 36 rows (a clear row above, between
  and below doesn't fit in the old card's 30).
- **On the device (2026-10-15, 14.2)** the 16 two-line toasts shot (en,
  de, ru, ja, zh) had their lines apart: at least one clear row between
  them (one in half of them, two or three in the rest), and 2 above and
  below ru's "й". Every line was inside the card. In three, the first
  line's faint top row (the glyphs' anti-aliased edge) sits in the row
  right under the card's outline, with no clear row between: ja and zh
  "This track has no Auto DJ neighbours yet: the next track is random",
  and zh "Auto DJ on: it follows the next track you play". zh's "no
  neighbours" also has its second line's bottom row just above the
  outline. At 1x it reads as text touching the outline: left for the
  user to judge.
- **While a toast with Undo is up**, these wait until it goes (the failure
  pause doesn't wait). An Undo tap drops them (but "Only {n} tracks fit",
  the filter's: after the Undo's own toast), and Auto DJ switched off
  drops them all.
- "No Auto DJ index" is said once per switch-on or boot, not once a
  session: every Play is a session.
- **Under a mood filter** (3.14) a jump, a repeat or a re-pin says none of
  the first five: a jump's anchors had lists, a repeat is for want of
  tracks that fit, and the pin stays or moves onto the playing track. A
  pick by id with the index joined (a jump's track with no row, or the
  only one that fits) isn't for want of an index, so it says no "No Auto
  DJ index".
- **The filter's own three** (`autodjview::NewsWatch`; MOOD-FILTERS.md
  4.5), only while Auto DJ is on:
  - **Only {n} tracks fit:** when a change settles (the editor closed on
    it, a `Dq`), at a switch on, and at the first appended pick after a
    boot or after a Play that switched Auto DJ on. Then not again until
    the filter changes, Auto DJ is switched off, or more than 50 fit (or
    none) and then 50 or fewer again; not once a session. Its count is
    the one at the moment it shows.
  - **No close match:** at a pick that jumped (a repeat after a jump
    keeps its code), once a session and again after a change settles.
  - **Plays once:** when a change settles with Auto DJ's pick up next
    outside the new filter. That pick stays and plays (D5).
  - Each is raised only when nothing above it is said in the same pass.
    Otherwise "Only {n} tracks fit" waits for the next appended pick and
    "No close match" for the next pick that jumps; "Plays once" goes, its
    moment past. So on a card with no index and 37 tracks inside the
    filter, the first pick says "No Auto DJ index" and the next "Only 37
    tracks fit".
  - A change settled behind the library update's fence says nothing (its
    count is from before the update): "Only {n} tracks fit" comes at a
    pick after it.
- **While the mood filters' editor is up** (Albums' or Auto DJ's), all of
  Auto DJ's toasts wait until it closes, the failure pause too. Auto DJ's
  editor closed on a change drops the filter's own that waited: the
  change says its own, from its count.
- What waited and the next news make one toast (`autodjview::NewsWait`).
  "Only {n} tracks fit" or "No close match" outranked there was already
  raised, so it isn't lost: it waits for that toast to go, then shows,
  "Only {n} tracks fit" first.
- Only a pick that was appended speaks; a refused one is tried again
  quietly.

---

## 10. The console: `D`

Type `D`, the rest, then Enter (the whole line is one command; `d` is the
dance screen). Every answer is a `[dj]` line. None of them touches the
output or the volume; scripts drive them in silent test mode (`z`).
`lib/core/AutoDjCommand` parses (READ); `src/app/AutoDj` carries out.

| Command | Does |
|---|---|
| `D` | Status: the settings and "now:" (the trigger's reason, 4.1; "owed" when the update step waits); the mood filter (what it lets through, how many tracks fit and the count's time, or why it filters nothing; its jumps, re-pins, repeats and picks that found none fits since the boot); the index (or why there is none: generation, selection, rows, k, head, floor, the gate's level, BROKEN); the join (ready with its coverage, time, slices, peak; or its phase; or none while off); the session (seed, the anchors' rows, the pin and whether the mood filter moved it, the key anchor, cooled rows / artists / ids, damaged rows, DJ marks, the queue's epoch); the counters (below); picks and breaks per mode; the classes; the last pick's reads and time; memory |
| `D0` / `D1` | Auto DJ off / on (saved, as the UI). Off gives the join back; on joins |
| `Dm0` / `Dm1` | Anchor: Follow the vibe / Stay on seed (saved) |
| `Df55` … `Df75` | Match strictness, 55-75 by 5 (saved); other values are refused |
| `Dt0` / `Dt1` | BPM continuity off / on (saved) |
| `Dk0` / `Dk1` | Harmonic mixing off / on (saved) |
| `Dq<hex>` / `Dq0` | The mood filter (3.14): its `q`, 1-4 hex digits, as `gm<hex>` takes them (saved); `Dq0` none. A value that isn't a filter (`Dq800`, `Dq30`, `Dq600`) is refused, and so is a bare `Dq`. The tracks inside it are counted at once, so a filter nothing fits holds from that pass; behind the library update's fence it is saved and the count waits for the fence's end. A pick already queued outside it: "it plays once". The session is kept |
| `Ds<hex>` | The next session's seed, 1-8 hex digits (a reproducible session) |
| `Dr` | A new session now (on or off), no seed track |
| `Dp` | One pick appended now, whatever the trigger says (cued at a finished queue); its line, then the anchors and the pin. Refused behind the fence |
| `Dn<k>` | k dry-run picks (1-2,000) on a copy of the session, each treated as played (the next follows it); a read ahead first when one is due, as the loop does. The live session is untouched. A summary: picks, dry, loosened, random, by id, repeats, artists, breaks, drawn statuses, reads, the time's p50 / p90 / max, the classes; under a mood filter one line more, its jumps, re-pins, repeats and picks that found none fits (with no filter the lines are as before) |
| `Dj` | The join again from its start (its time and memory). Refused while Auto DJ is off |
| `Dc` | The file closed and opened again, as a remount that found another index (a new session, and 6.6's tries from the first). No root read yet: the root read again, from the first try, the session kept whatever it finds. The last read of it failed: the same, but while Auto DJ is on another index, one now or none now is a new session (6.5). A read on a card silent past its init, with a card job's step under way, restarts the player, as any remount then (6.6) |
| `Db<n>` | The bench: n slot reads of random rows (1-5,000), the raw read timed apart from its check; p50 / p90 / max, a histogram, the check's mean and max |
| `Dh` / `Dh<id>` | "Auto DJ from here" on the current entry's track, or on library track `<id>`. Refused behind the fence |
| `De` | The empty states' Auto DJ button. Refused behind the fence |
| `Dx` | Stops a running `Dn` or `Db`, with its summary so far |
| `D?` | The list. A line that isn't a `D` command prints it too |

- **Long commands** (`Dn`, `Db`) take one step a loop pass, never while a
  list moves or the card is held. `Dj`, `Dc`, a closing file, the library
  update's fence, the join given back and Auto DJ off stop a running `Dn`
  with its summary so far (its maps go). `Db` stops if the file closes.
- **The counters** (since boot; a session doesn't reset them): picks (none),
  reads at a pick 0 / 1 / 2, reads ahead, anchors left out, dry, loosened,
  random (by id), drawn tempo and key, damaged, read failures, broken,
  quick skips, picks taken out, holds, the failure pause, refused, undos
  taken, waited for a refill.
- **The pick line:** `[dj] pick: <path> (row R, level L, class C:
  cooldown c, rank r; N candidates, G gated, I in its class; R reads; T
  ms, reads T' ms)`, then tags: `[dry: the 50 closest]`, `[loosened: …]`,
  `[random]`, `[by track id: no index]` (`[by track id]` under a mood
  filter with the index joined), `[tempo drawn]`, `[key drawn]`,
  `[tempo break]`, `[key break]`, then a mood filter's (3.14) `[re-pinned:
  …]`, `[jump: nothing listed fits the filter]`, `[jump: all listed that
  fits played lately]`, `[jump: a track with no row, by id]`, `[a repeat:
  …]`, then `[cued: …]`, `[REFUSED …]`, `[the listener's undo went with
  it]`. No track: `[dj] pick: no track (why)`, "no track fits the mood
  filter" among the whys (`Dp`'s: the loop holds instead).
- **Other lines:** a session's start with its seed; the index opened (or
  why not); "joined" with its coverage, time, slices, reads, bytes kept
  and at the peak, where its blocks went, `begin()`'s time and PSRAM's
  lowest free meanwhile (6.2); the join given back; a quick
  skip; a pick taken out; a pick that failed; a read ahead that failed; a
  slot found damaged at a pick ("read at the pick, row R: DAMAGED …",
  3.13); a broken slot (once a file); the library rebuilt with the marks
  carried.

---

## 11. Code, tests and tools

### 11.1 Files

| File | What |
|---|---|
| `lib/core/AutoDjEngine.h/.cpp` | The session and the pick (section 3); `Stats`, the console's counters |
| `lib/core/AutoDjTiers.h/.cpp` | The tempo windows, the Camelot codes, the statuses, the ranks, D3's odds, the gate's level |
| `lib/core/AutoDjRng.h` | PCG32, `below(n)` by Lemire's method |
| `lib/core/AutoDjJoin.h/.cpp` | The join, its `Maps`, the folders' states, the placement (smaller free blocks first, the spacers, the margin), a track's path hash |
| `lib/core/AutoDjTrigger.h/.cpp` | `decide()`, its waits and holds, the failure pause |
| `lib/core/AutoDjDriver.h/.cpp` | The loop pass: sessions, steering, the trigger, the pick, the read ahead, the DJ marks across a read-back, the dry run |
| `lib/core/AutoDjCommand.h/.cpp` | The `D` command's parser |
| `lib/core/MoodFilter.h/.cpp` | The mood filter: `q`, the words, the predicate (docs/MOOD-FILTERS.md); `AutoDjJoin.h`'s `MoodView` over them |
| `lib/core/AutoDjView.h/.cpp` | What the UI shows and says |
| `lib/core/AutoDjOpen.h` | What a failed open of the index comes to: the card's fault tried again (6.6) |
| `lib/core/CardAutoDj.h/.cpp` | The MPDJ v2 reader (`Table`, `slot()`, `rowOf()`) and a writer for the host tests |
| `lib/core/QueueModel`, `PlaybackController`, `QueueSaver`, `LibraryUpdate`, `NvsLayout`, `QueueView`, `CardRoot`, `CardContract` | The hooks: the quiet append and the play epoch; `addFromAutoDj()`, `endedAtEnd()`, the cue, `naturalEnds()`; `queue.txt`'s snapshot writes; `autoDjOwed` and the give-back; schema 5; the marks' ring and the badge rule; the root's index entry; `read.mpdj=2` |
| `src/app/AutoDj.h/.cpp` | The glue: the open file, the join's slices, remounts, the fence, the settings, the console, the UI's view and news |
| `src/app/AutoDjStore.h/.cpp`, `AutoDjContract.h` | NVS; the majors this build reads |
| `src/main.cpp` | `stepAutoDj()` after the card's guard and before the queue's save and the card's step; the refill wait; the give-back hook; the fence hooks; `UpdateEnv`; `MainUiHost`'s Auto DJ calls |
| `src/ui/*` | The "..." row, Output > Auto DJ, the empty states, the long-press row, the DJ mark, the toasts (two lines by their ink: `lib/core/TextInk`, `uitext::toastLines()`) |

### 11.2 The Python reference and the fixtures

- **`tools/autodj_ref.py`** is the pick rule in plain Python (standard
  library; `tools/mpdj.py` reads the golden indexes). Every rule is the
  engine's, in the same order, drawing from the same PCG32 stream (but
  a row's second failed read in a row, IoTwice, which no session here reaches).
  - `python tools/autodj_ref.py` writes `test/fixtures/autodj/`;
  - `python tools/autodj_ref.py --check` exits 1 when a file there
    differs.
- **`test/fixtures/autodj/sessions.json`** (CC0, synthetic only):
  - PCG32 outputs and `below(n)` draws;
  - the gate's level for each strictness and floor;
  - 8 tables: 4 goldens of `test/fixtures/card/golden`, 4 synthetic (one
    for dry ties, one that starves, `islands` for the mood filter's
    jumps);
  - 39 sessions: 1,960 picks and 1,653 reads ahead, with listener tracks,
    quick skips, Anchor changes, damaged, flaky, failed and broken slots,
    the index going and coming, reopens and renumbers, every pick's
    expected fields and the counters at the end. The first 26 (1,151
    picks, 956 reads ahead) are from before the mood filter, and
    `tools/test_autodj_ref.py` holds them by hash; the other 13 run under
    one (3.14; docs/MOOD-FILTERS.md 9.8), the last with rows that several
    tracks share (`trackToRow` many to one). 915,293 B.
  - Its README has the format.
- **`tools/test_autodj_ref.py`** tests the reference itself (the stream,
  the tiers against a port of mStream's waterfall, each rule, the fixture
  up to date and covering every rule).

CI's "Auto DJ fixtures" step runs the `--check` and both Python test
files.

### 11.3 Host tests (`pio test -e native`)

| Suite | Covers |
|---|---|
| `test_autodj_tiers` | PCG32 against the reference outputs, `below(n)`, the windows, the Camelot codes, the rank tables cell by cell against the waterfall, D3's odds, the gate's levels, the filter word |
| `test_autodj_engine` | Every fixture session replayed twice (runs made at the read ahead, then at the pick), every pick, read and counter exact; and each rule on its own: the gate, dry, loosening, the ring and the quick skip, the pin, the key anchor, the cooldowns, the read budget, the cache, damaged and broken slots (the rows a pick's reads damaged, named once), picks by id, no allocation in a pick; under a mood filter (3.14) the runs, the jumps and the scan, repeats oldest first, picks by id, the re-pin in its read budget and a moved pin's quick skip, the button, no allocation in a jump |
| `test_autodj_join` | The goldens and the hardening files, written files, strides and CRCs, every slice boundary with a fence or a changed index, no memory, the library limit, a FAT model, memory at scale; the folders' hash against `trackPathHash()` (every spelling of the root, paths of 509-513 B, a folder too long, depths 31-35); the placement's order, and in a model of ESP-IDF's TLSF heap, pinned to the device's logged numbers: free never under the margin (30 heap shapes and margins, about 970 sizes of heap or free block), the rejoin wherever the largest free block is in its size class (six classes, three library sizes), the room above the spacers (five lists of blocks, free blocks up to 1.3 MB), another task's block at every moment of `begin()`, the give-back leaving Auto DJ off's largest block, and a library update (the real `LibraryIndex` and `LibraryBuilder` at 19,412 tracks, main.cpp's order of frees and blocks) with Auto DJ off and on |
| `test_autodj_trigger` | Every condition, the first that fails, the finished queue, the holds (a mood filter no track fits among them), `owed` only while joining, the failure pause |
| `test_autodj_driver` | Sessions, steering, the cue, the marks across a read-back, refused and removed picks, the holds, the failure pause, the index arriving late, the seed before the join, the button's lookups, the refill wait, the dry run, the `D` parser; a quick skip's reason (before the join too); the Undo of "Auto DJ from here" and the button from a playing, paused, stopped, cued and empty queue, Auto DJ on and off before the tap; a slot whose CRC fails on the read ahead and again at the pick, named by that pick's pass once; a mood filter: the picks and the button inside it (the button with none drawing as before), the hold when no track fits (nothing owed), `Dq`'s parse, a filter changed keeping the session and the pick up next, `Dn`'s tallies under it |
| `test_autodj_open` | What a failed open comes to: the tries, their waits, a remount's new count; over the real reader, a read that fails is always the card's fault and a bad file never is |
| `test_ui_autodj` | The row's state, the Output line and its fallback, the dim rows, the strictness steps, the status lines, the empty-state button, the toasts' rules; under a mood filter the sheet's detail, Smart filters' value, the status lines and their amber, no second button when nothing fits, and the filter's three toasts (when each is said and said again, the order, one outranked waiting or going, no index with 37 fitting); what waits on the Ui's side (`NewsWait`: an Undo toast, the editor, one a merged toast outranked) |
| `test_card_autodj` | The MPDJ v2 slots, header and selection |
| `test_queue` | The quiet append (the listener's undo kept, its rank in the snapshot, at the cap), the play epoch, the snapshot writes under an hour of appends |
| `test_playback` | The pick's gapless word, refused behind the fence, Repeat All, the holds, the cue, every Play and Clear bumping the epoch, natural ends; the Undo of a Play, Shuffle all's Play and a Clear from every play state (playing, paused, paused by the timer, cued by Auto DJ, stopped, finished, waiting for the headphones), never starting what was silent |
| `test_library_update` | `autoDjOwed` holding the fence; the memory check with the join given back |
| `test_nvs_layout`, `test_ui_queue`, `test_ui_library`, `test_ui_strings`, `test_card_contract` | Schema 5 (`moods`, every `q`); the badge and the "added" mark; the texts' room in ten languages (a dim value row's second line too: the value where it can't sit beside its title), and every two-line toast's rows (CJK, Hangul and accented names too) never touching; `read.mpdj=2` |

### 11.4 The device test card

A card the firmware accepts with an AutoDJ index, without mStream:

1. **The tree:** `python tools/synthcard.py --out DIR` writes about 19,500
   silent MP3, FLAC and Opus stubs under `DIR/music` (made-up names) and
   `synthcard-files.jsonl`. `python tools/synthcard.py --out DIR --copy-to
   E:\ --yes` copies it to an empty FAT32 card (without `--yes` it only
   says what it would copy).
2. **Full-length files** for the bench and the gapless checks (a 320 kbps
   MP3 and a FLAC) go under `/music` before the next step: the first boot
   then lists them, and `--synthetic` gives them rows. (Added later, the
   walk finds them after the boot's build and a library update follows.)
3. **The transfer:** `python tools/synthtransfer.py --add-to E:\
   --synthetic` (or `--mpdj FILE`, an index built for this tree; `--dry-run`
   first). It writes only `/.mstream`: a tags file with no records (the
   library build is unchanged), the index, and the root last. It refuses
   the system drive, any file system but FAT32/FAT16, and a `/.mstream`
   that isn't empty (`--replace` commits a new generation over one it
   wrote; `--replace-foreign` over another program's). It never formats.
   `--synthetic` makes clustered vectors by genre and artist, BPM and keys
   from priors (about 5 % of rows without each), and 1 % of tracks without
   a row.
4. **The check:** `python tools/synthtransfer.py --check E:\` reads the
   card back as the boot does and says how many tracks map to a row.
5. **The first boot** after a new root rebuilds the library before the UI
   (about 10 s at 19,410 tracks).

`tools/test_synthtransfer.py` checks the lists against `tools/mpdj.py`'s
rule, reads the files back with the firmware's own readers built on the
host (with g++), and every refusal.

---

## 12. Known limits

- **The synthetic card's stubs are 1 s long,** shorter than the decoder's
  read-ahead: every pick is a late trigger. The 2-hour soak still joined
  every track (0 underruns), but gapless timing needs full-length files.
- **The console's `gs`** blocked the loop for 12 s on 2026-10-13. Auto DJ
  made no pick meanwhile; the 1-second stubs ran the queue dry and it got a
  cued pick. Console behaviour, outside Auto DJ.
- **A library update's safe point** needs at least 20 s left of the heard
  track (about 24 s at 20,000 tracks), which 1-second stubs never have.
  With Auto DJ appending stubs, a `gb` waited until playback paused. Real
  tracks give it.
- **A library rebuild fragments PSRAM, with Auto DJ off too** (`main`'s
  own: in `test_autodj_join`'s model the largest free block goes from
  753,652 B at the boot to 360,436 B after one update; on the device,
  MEASURED 2026-10-15, from 819,188 B to 409,588 B). A later update
  whose new track table outgrows the kept one needs about 660 KB in one
  block and waits for the next boot either way. Not Auto DJ's (7.4);
  not fixed here.
- **A card swapped while the player runs isn't walked.** A remount
  reopens the records and the index (6.5), but only a boot walks the
  card. So music files added on a PC to a card that goes back in while
  the player runs aren't in the library until the next boot. On
  2026-10-16 a card with 2 new files and a new index went back in while
  running: the new index joined at once (19,238 of 19,412 tracks mapped),
  and the 2 files came at the next boot (its walk, then a library update
  and a rejoin: 19,240 of 19,414). The firmware's, not Auto DJ's; not
  fixed here.
- **The two-line toasts in Japanese and Chinese** touch the card's
  outline in three cases (9.6): left for the user to judge.
- **Stay on seed narrows over long sessions:** one slot lists 169 rows.
  Loosening walks them before repeating; after that it repeats.
- **The synthetic card's keys are independent of the sound,** so its
  break shares are pessimistic. On it (MEASURED 2026-10-13, both matches
  on), Follow the vibe broke tempo or key on 0 / 0 / 1 % of picks at 55 /
  65 / 75 %, Stay on seed on 26 % at 55 % and 53 % at 65 %.

---

## 13. Deferred

| What | Why later |
|---|---|
| **Two-hop in Stay on seed** (M7; docs/AUTODJ-INDEX.md 11.5) | The user's D5: build it if the `D` counters' breaks per mode on a real card show the need. Up to 2 reads a pick, an engine option and a counter |
| **mStream's exact DJ** (docs/AUTODJ-INDEX.md Appendix A, minor 1) | Planned, provisional: its parts that depend on the real vectors wait for one aggregate run on the server |
| **Tempo, key and the artist cooldown for tracks without a row** | A per-track BPM/key block in `library.idx` (about 80 KB at 20k), or a tag read per pick |
| **"More like this"**, a tight (head-only) setting | A Library page of their own; the head alone measured worse for variety |
| **DJ marks across a reboot** | A `queue.txt` v3 |
| **A Now Playing glyph** | No room in the shuffle and repeat group |
| **Strictness above 75 %** | Stopped at 75 % (docs/AUTODJ-INDEX.md 11.4); 80 % would change the engine's, NVS's and the page's limits together |
| **A repo tool to damage a slot** (check 8) | The checks' helpers (2026-10-13, 2026-10-16) stayed outside the repo |
| **A native review of the translations** | As for the rest of the catalogue (docs/I18N.md) |

---

## 14. Device checks

On the synthetic test card (11.4), the release-check docs' way, with
their rules: silent test mode (`z`) after every boot before anything
plays; no Bluetooth or power commands (`f`, `B`, `c`, `P`); after each `D`
command, the console's loop-stack line must not say LOW; nowhere a Guru,
a `task_wdt`, an `[E]` line (but for the SD driver's "Card Failed" and "Check
status failed" at a card pull), `[heap] FAILED` or a LOW stack line.

**The first check ran on 2026-10-13** on the M4 build (`09f91a1`, console
only, a dev env that read the index; the release build reads it since
M5). The card: the synthetic tree, two full-length bench files, and the
research's 19,410-row index. Its five problems (the pick's CPU, Stay on
seed repeating at 75 %, the join's memory deferring a library update, the
first pick after a skip timing the decoder, `queue.txt` not keeping up)
were fixed in `7c89673`, and went on the device with the UI in the
second check (`a7ee54b`, 2026-10-14: 14.1). Its findings were fixed in
`314c624`, which the third check ran, with the card moves and the bad
index files (2026-10-15 and 16: 14.2).

The next release runs every check below on the release build. "Before" is
2026-10-13's result; 2026-10-14's is in 14.1, and 2026-10-15's and 16's
in 14.2.

| # | Check | Pass | Before (2026-10-13) |
|---|---|---|---|
| 1 | **Boot.** Boot with Auto DJ off, then `D1`, then `D0` | Off: "joined when Auto DJ is switched on", no "joined" line, `D` "0 B held", the engine 19,016 B. `D1`: `D` during the join names "hashing the library's folders" first; "joined" with where its blocks went (6.2: "... at the top of the largest free one, ..." or "all in smaller free blocks"), about 90 reads (87 on the synthetic card), the peak 275,952 B and 10 B a folder of the card's (300,722 B at the synthetic card's 2,477 folders); its "PSRAM free at least N B meanwhile" at least 65,536 B (2026-10-15: 170,416 B at a boot's join), `[stats]` pmin= never down to 10-14 KB (2026-10-14's), no `[heap] FAILED`; its time, idle and with the MP3 and the screen dark, against 2026-10-15's (1.48 s in 110 passes, 3.0 s: 7.3); a list scrolled meanwhile stays at 30 fps. `D0`: "the join given back … 155284 B". `Dj` while off is refused. The index opened in at most 20 ms. Browsable time level with 0.10.0's on the same card | **Partial.** Opened in 5.5-6.5 ms; joined in 2.08 s, 112 slices, 155 KB kept, 276 KB at the peak; scrolling at 30 fps (the join grew by the scroll's time). The browsable comparison needed another flash: not done. The join ran with Auto DJ off too (since changed) |
| 2 | **Bench.** `Db200` with the 320 kbps MP3, then the FLAC | p50 at most about 3 ms; max at most 14 ms (MP3) and 29 ms (FLAC); 0 failed | **Partial.** p50 1.2-1.4 ms, 0 failed, FLAC max 17.0 / 17.3 ms; MP3 max 13.2 ms in one run, 20.7 ms in the other. A run during the boot's card walk (p50 11.9 ms, max 88 ms) excluded as contended |
| 3 | **Pick CPU.** `Dn200` in both modes at 55 / 65 / 75 %, matching off then on, at 240 MHz; at 160 MHz Follow the vibe 55 % off, 75 % off, 55 % on and Stay on seed 55 % off. A live pick with 2026-10-13's seed and queue (`Ds717feda5`, then `qp0`: the same row as then), at both speeds. Live loosening: `Dm1 Df75`, then "Auto DJ from here" (`Dh`) on the track of row 693 (Northern Feather, which plays during the `Dn` runs: its slot has 3 rows above 75 %), 2 minutes of stubs. Not with the live pick's seed: its pin, row 3392, has 57 rows above 75 % and never loosens (2026-10-14: 0 of 140) | p90 at most 2 ms at 240 MHz, 3 ms at 160 MHz, every run (7.2 has 2026-10-14's); reads at a pick 200/0/0; `D`'s picks unchanged by the runs; the live pick at most about 2 ms plus its reads' time (then 6.38 ms, reads 3.25 ms). Stay on seed 75 %: no class 16-23, at least 150 loosened, artists far above 3; the live run's picks carry `[loosened …]` (2026-10-14: 106 of 115), `D`'s loosened rises, no class 16-23. Follow the vibe's repeats, artists, classes and breaks as before (0 loosened) | **Fail.** Follow the vibe p90 4.8-9.0 ms at 240 MHz, 6.3-12.0 ms at 160 MHz. Stay on seed passed (0.6-1.7 ms at 240 MHz; 1.6-1.9 ms at 160 MHz). Reads 200/0/0. Stay on seed at 75 % repeated 3 rows: "repeats 197, artists 3" |
| 4 | **Albums and skips.** Queue an album, Auto DJ on, skip 5 tracks quickly. Then the quick-skip line's forms (3.2), each `n` in a track's first 30 s: a bench file (no row); a track that started with Auto DJ on and joined; after `Dr` while a track with a row plays (joined); between `D1` and "joined" (Auto DJ off before), a track with a row. (`D0`, a track, `D1`, then `n` after "joined" gives "no longer steers": the playing track steers once the maps arrive) | No pass with more than one read ahead; no pick over 2 reads; the skipped tracks out of the anchors (`D`); quick skips +5; no pick until the album's last track. The four lines: "it has no row in the index: it never steered", "no longer steers", "it wasn't among the anchors: nothing changes", "the index isn't joined yet: it never steered" | **Pass** (run 2): one anchor left, quick skips +5, the pick at the last track read 1 slot. The line said "no longer steers" for a track with no row (since fixed) |
| 5 | **Soak.** Auto DJ on, the stubs, 2 hours, the queue reaching its 5,000 cap | No reset, no underrun; a word at every start; no row again within 50 picks; the cap pushing out the oldest; PSRAM and internal lows flat once the queue stops growing; the loop stack's low-water mark. **`queue.txt` keeps up:** `q`'s write count rises by at least 8 every 5 minutes (a write at least every ~35 s), most from a snapshot; a write's dip at most about 20 KB; after a pause, written within about 6 s, and a reboot restores at the paused entry. Live picks p90 at most 2 ms | **Pass, except two.** 7,520 picks, no reset, 0 underruns, 7,521 gapless joins with 0 cuts, all class 0, the closest repeat 51 apart, the cap at 79.5 min. **`queue.txt` failed:** no write for the last 59.5 min. PSRAM's low fell 446 → 400 KB with the queue's growth, then flat 54 min (partial). Live picks p90 8.4 ms (as check 3) |
| 6 | **Fence.** `gb` during a session with the MP3 playing, then `n` right after "joined", three times; `gb` during the boot's join | The freed bytes equal the join's; `D` says the fence; the session and the marks kept; the last entry still gets its pick; the rejoin's time with the MP3 and the screen dark against 2026-10-15's 5.9-6.2 s (7.3); each rejoin "joined", no `[heap] FAILED`, no "the join: NO PSRAM"; its "PSRAM free at least" at least 65,536 B (2026-10-15: 177,388 B) and its blocks mostly in the rebuild's holes ("77640 B at the top of the largest free one, 2 of 3 kept and 7 transient blocks in smaller ones"), "PSRAM's largest free block now" about 80 KB under Auto DJ off's after a rebuild, not 175 KB (2026-10-14: 180,212 B; 2026-10-15: 73,728 B under; 2026-11-02: 81,920 B under): `D0` and `D` ("the join given back", then the free and the largest block), `gb` with Auto DJ off and `D` again measure off's (also `main`'s own fragmentation: 7.4); the pick after `n` lands after the new track has 500 ms buffered, 0 reads, the usual time; "waited for a refill" +1 a skip. During the boot's join: aborted, one join after, no crash | **Pass,** with two findings since fixed: the rejoin took 7.2 s with the MP3 (4.6 s paused), and the first pick after it took 108.52 ms (the decoder's refill) |
| 7 | **Card.** (a) Pull and reinsert mid-session, paused. (b) The index replaced on a PC (`synthtransfer.py --replace`, a new generation), reinserted while running. (c) A first boot with Auto DJ on of a card with new files the records don't have | (a) "the same index: the file reopened, the maps and the session kept"; picks go on, at most 2 reads; damaged 0. (b) The new generation opened "after a remount", a new session ("another index on the card"), joined again, picks go on. (c) No "the update step waits for the next boot (the track table …)"; if the give-back line comes, the largest block grows to about its "up to N B" (the join's bytes at that block's top; free bytes next to them join it too: 9,576 B more on 2026-10-16); after the build "the join again" and "joined" | **Not run** (the card had to move). In 2026-10-13's first boot, the 155 KB join was resident when a library update deferred to the next boot for its track table's block: the give-back answers that |
| 8 | **Bad files.** (a) One slot's bytes changed so its CRC fails. (b) One slot rewritten with a bad list under a good CRC. (c) No index: the file renamed; and a card with no transfer data | (a) "its CRC failed (read again later)", then "DAMAGED …" for each row that `D` counts as damaged (the read ahead's line, or "read at the pick, row R: DAMAGED …", 3.13); picks go on; `D` damaged ≥ 1; the file stays in use. (b) "… BROKEN, picks at random …" once; then `[random]` picks; `D`'s index line says BROKEN; a remount clears it. (c) "ABSENT (the file the root names isn't on the card …)", picks `[by track id: no index]`, the page "Can't read the Auto DJ index", the settings dim, the empty states show Shuffle all, `De` plays nothing; with no transfer data, "No Auto DJ index on this card", and the toast once | **Not run.** (a) and (b) need a tool to change a slot (13) |
| 9 | **A finished queue.** An album ends with Auto DJ off; `D1`. Then the sleep timer's End of queue | A pick cued, paused at 0:00; nothing starts; Play starts it and the next pick follows; the toast. While the timer runs, no pick and `D` names it; after its stop, a pick cued | **Pass** (3 cued picks, none started by itself) |
| 10 | **UI.** Screenshots (`X`) in all ten languages | The "..." sheet in each state (On, Off, Waiting, Paused); Output's row line, with the Anchor and the state-only fallback; the Auto DJ page live, dim (Match strictness's value shown in every language: beside its title, or under it in de, es, it and pt: 9.2), and each status; the Queue's marks; both empty states with the Auto DJ button and with Shuffle all; the long-press sheet with "Auto DJ from here" first; the toasts, waiting behind an Undo toast, the two-line ones (ja and zh most: "Auto DJ from here", the empty states' button, no neighbours; ru's no neighbours) with their lines apart, a clear row between them, and inside the card (most ja and zh ones, and some Latin ones such as "Auto DJ from here: Burning Memory", in the card's 36 rows, y 36-71: their look accepted or not; two 17-row CJK lines, zh "正在等待耳机" over a title with 青 or 机, without the card's top and bottom lines but with its sides and corners); Undo switching Auto DJ off; a Library Play's Undo over a queue paused on Auto DJ's cued pick leaving it paused (`[ui] undo: done`, `[queue] now at … (paused)`, no pick after), over a stopped one, stopped, over a playing one, the old entry playing from 0:00; "Auto DJ from here" over a paused queue, then Undo: paused, and Auto DJ off again if the tap switched it on. The user's real-finger taps | **Not run** (no UI then) |
| 11 | **Memory at 20k.** The running lowest PSRAM over 2 minutes of the MP3, Auto DJ off, then on and joined | On is about 155 KB lower than off (the join), read from psram= or `D` (pmin= keeps the join's moment, now at least 64 KB: 7.4); the engine's 19 KB is held either way | **Couldn't tell:** the build joined at boot either way (pmin 547 KB idle and with an MP3, 459 KB with a FLAC, in both). Resident 177,814 B, against the design's 180 KB |
| 12 | **The Auto DJ button with Auto DJ off.** `D0`, `qx`, `De`; again in Stay on seed | At most 8 lookups (1 expected) in at most about 50 ms; playback at once; "joined" 1.5-2.5 s later; the first pick after it (a `D` before it shows "owed"). Stay on seed: `D`'s pin is the seed's row after the join | New since 2026-10-13 |
| 13 | **Settings across a reboot.** Change each, reboot | Each comes back "(saved)"; the boot starts its own session | **Pass** |
| 14a | **A card that fails the open.** Paused, the walk over (with one pending, a pull takes the guard's restart instead: 6.6), the card pulled, `Dc` (FatFs answers FR_NOT_READY). A `Dc` that opens with no driver line means the card is still on the bus: check the slot | "the card failed opening it (FatFs's answer 3 …): again in 2 s, 1 of 3"; `D` says "1 of 3"; the page says Reading the Auto DJ index…; about 44 s later (42 s of waits and three failed inits) "ABSENT (the card failed every try at it: io)" and Can't read the Auto DJ index. The card back, and the first read at least 30 s after ABSENT, a play: "(after a remount)", the join (read sooner: Absent until the next remount or boot, 6.6) | 14.4 |
| 14b | **A root the card fails** (6.5, METADATA.md 3.2.2). A temporary build that fails every read under `/.mstream`, on a spare card holding a transfer's library, never the music card | Boot: "[lib] the card's root: a read of the card failed: read 4 times, and each failed", "library.idx built from a transfer: load", "[card] the card's root couldn't be read"; `library.idx` unchanged; no walk; `gs` says a restart asks again; `@status` says `cid=?`; the Library row shows its sources, a tap on Rescan its note. `D` says "the card failed reading its root: tried again soon, N of 3" (N rising to 3) and the page says Reading the Auto DJ index…; about 42 s on (46.5 s with reads that get no answer) `D` says "ABSENT (the card failed every read of its root: io)" and the page says Can't read the Auto DJ index; no more root reads while it stays so. The fault off and a reboot: "matches the card", the same library, the index open | 14.4 |
| 14c | **A root the card fails, the jobs running** (6.6). The same build, each failed read also marking the card failed as a read with no answer does (`sd_diskio.cpp`), on a spare card whose `library.idx` has no transfer in it | Boot: "library.idx built with no transfer: load"; `D` says "ABSENT (the card failed every read of its root: io)" from the start, the page Can't read the Auto DJ index; `gs` says "T: not read"; the walk runs and ends; no restart, and no more root reads | 14.4 |
| 6b | **The rejoin at once.** `gb` on a fresh boot, stopped, then with the MP3 and with the FLAC playing; `D0`, `D1` right after a rebuild; `Dc` right after a join | "joined" every time (6.2's words), no `[heap] FAILED`, no "the join: NO PSRAM", the page live; "PSRAM free at least" at least 65,536 B; no underrun | **New since 2026-11-02:** 14.3 |
| 14 | **The user's own card** (no transfer data) | No index: the toast once, the page's status, Shuffle all on the empty states; Auto DJ adds random library tracks | New since 2026-10-13 |

After the checks, the device and the card are put back as the release
docs do (the firmware, the card, and the NVS keys the checks saved).

### 14.1 The second check (2026-10-14)

On the user's Core2, the synthetic card (11.4) with the bench files, the
Auto DJ build of `a7ee54b` (`7c89673`'s fixes and the UI); checks 1-7 of
that session's list, then the UI in all ten languages (276 screenshots)
and its flows, and a read-only verification of both logs. Silent test
mode before anything played; no Guru, `task_wdt`, `[E]`, `[heap] FAILED`
or LOW line; no pick over 1 read; nothing played by itself (every start
0.1-0.8 s after a command that plays). Not run: checks 2 (the bench), 7
and 8 (the card moves) and 14, the UI's no-index screens, and the Paused
state (3 failed picks: nothing causes it on this card).

| # | Result |
|---|---|
| 1 | **Partial.** Off: "joined when Auto DJ is switched on" (opened in 6.0 ms), no "joined", "0 B held"; `D1`: 87 reads, 155,284 B kept, 275,952 B at the peak, "at the top of the largest free one" every time; `D0` gave the 155,284 B back (PSRAM free exactly as before); `Dj` refused while off; 30.2 fps while scrolling; the load back to normal after it. **Fail on time:** 1.97-2.00 s idle (164 passes), 3.77-3.93 s with the MP3 (7.3). **Finding:** every join took PSRAM's free to about 10 KB for a moment, so pmin= stayed at 10-14 KB until a reboot (7.4) |
| 3 | **Pass.** p90 0.38-1.70 ms at 240 MHz, 0.52-2.31 ms at 160 MHz (7.2); reads 200/0/0; Follow the vibe's tallies the host's; `D`'s picks unchanged; the live pick 1.47 / 1.71 ms less its reads. Stay on seed 75 %: 188 loosened, classes 0-15 only, "repeats 105-118, artists 51-53" (197 and 3 before). Live loosening with row 693: 106 of 115 picks loosened, 52 artists, 84 rows. The check's text named row 3392, whose slot never loosens (corrected above) |
| 4 | **Partial.** "it has no row …" (a bench file) and "no longer steers" as expected. The third form wasn't reachable by the steps then written (`D0`, `D1`, `n`): the playing track steers once the maps arrive, so it said "no longer steers"; `Dr` while a track played gave "it wasn't among the anchors …" (the steps corrected above). **Finding:** `n` between `D1` and "joined" on a track with a row said "it has no row in the index": fixed, it now says "the index isn't joined yet" (3.2) |
| 5 | **Pass** (30 minutes, not 2 hours: the `queue.txt` part). `q`'s writes +9 to +10 every 5 minutes (59 of 60 from a snapshot), the generation moving on, the queue at 5,000, refused 0, 0 underruns; a write's dip 20 KB (read from psram=, pmin= being stuck by check 1's finding); after the pause, written within 6 s with its resume point, and a reboot restored the paused entry. Live picks p90 1.58 ms (7.2) |
| 6 | **Partial.** Three `gb`s with the MP3: the fence gave back the 155,284 B, "0 DJ marks carried, the session kept; the join again", the pick after `n` 0.35 s later, after its 500 ms buffered (250-255 ms), 0 reads, 0.64-0.70 ms, "waited for a refill" +1 each. **Fail on time:** the rejoin 6.6-6.9 s (7.3). **Finding:** after each update PSRAM's largest free block was 180,212 B and the join kept at 0x3f94becc (622,580 B and 0x3fbd69bc before) |
| 9 | **Seen in check 12's runs:** the Auto DJ button's 1-second seed stub ended before the join; the first pick came cued, paused, and nothing started |
| 10 | **Partial.** Screens and flows as listed pass in all ten languages: the "..." sheet (On, Off, Waiting), Output's line and its fallback, the page (live, each status), the Queue's marks, the empty states with the button, the long-press sheet, the toasts waiting behind an Undo, Undo switching Auto DJ off. **Findings:** in ja and zh the two-line toasts' first line ran into the second and over the card's top edge (ru's "й" touched the line above): fixed (9.6). A Library Play's Undo over a queue paused on Auto DJ's cued pick resumed it, and Auto DJ picked on (silent test mode: nothing heard): fixed (9.4) |
| 11 | **Pass.** On and joined 155,660 B lower than off (7.4); the engine's 19,016 B either way |
| 12 | **Pass.** 1 lookup in 9.1-10.0 ms, playback at once, "joined" 2.3-2.4 s after the button (the join 2.24-2.28 s), the first pick after it ("owed" before); Stay on seed: `D`'s pin the seed's row after the join |
| 13 | **Pass,** in passing: after the soak's reboots, "[dj] settings: Auto DJ on, Follow the vibe, strictness 55 %, … (saved)" |

### 14.2 The third check (2026-10-15 and 16)

On the user's Core2, the synthetic card (11.4) with the bench files, the
build of `314c624` (the second check's fixes). On 2026-10-15 a re-check
of those fixes, then a read-only verification of its log; on 2026-10-16
the card moves (check 7) and the bad index files (check 8), with the UI's
no-index screens in all ten languages. Silent test mode before anything
played; nothing started by itself (on 2026-10-15 every start came
0.1-0.9 s after a command that plays); no Guru, `task_wdt`,
`[heap] FAILED` or LOW line; 0 underruns. The only `[E]` lines were the
SD driver's "Card Failed" at each card pull; the boots between the card
moves were power-ons. Not run: checks 2, 3, 5 and 11-14. Not reached:
zh "正在等待耳机" (it needs the headphones as the output), the UI's Clear
(the console's `qx` and `qu` instead: the same `clearQueue()` and
`undo()`).

| # | Result |
|---|---|
| 1 | **Pass** (2026-10-15). `D1` idle, the screen dark: "joined" in 1.48 s, 110 passes; with the MP3, 3.0 s (7.3). `D` during the join named "hashing the library's folders", then DJRF, then DJPH. 87 reads, 155,284 B kept, 300,722 B at the peak (2,477 folders). At the boot and at each `D1` before a library update: "296526 B at the top of the largest free one, 0 of 3 kept and 1 transient blocks in smaller ones", PSRAM free at least 170,416 B (167,792 B with the MP3); pmin= 166K, never 10-14K. `D0` gave the join back: the largest free block 655,348 B joined, 819,188 B off. The time is over the estimates (1.1-1.3 s, 2.1-2.4 s) and under 2026-10-14's (2.0 s, 3.8 s) |
| 4 | **Pass** (2026-10-15). `n` 0.1 s after `D1`, on a track with a row: "the index isn't joined yet: it never steered"; after "joined": "no longer steers" |
| 6 | **Pass** (2026-10-15). Two `gb`s with the MP3, Auto DJ on: the fence gave back 155,284 B; "77640 B at the top of the largest free one, 2 of 3 kept and 7 transient blocks in smaller ones", PSRAM free at least 177,388 B. The largest free block: 335,860 B joined, 409,588 B given back, and 409,588 B after a `gb` with Auto DJ off (7.4). The pick after `n`: 0 reads, after its 500 ms buffered (at 252 ms), "waited for a refill" +1 each. The rejoin took 5.9-6.2 s (6.0-6.3 s from "the join again"): over the estimate (4.5-5 s), under 2026-10-14's (6.6-6.9 s). The first `D1` on the heap that the `gb` with Auto DJ off had rebuilt: PSRAM free at least 86,868 B, the session's lowest (pmin= 84K) |
| 7 | **Pass** (2026-10-16). (a) The card pulled while paused and put back: noticed at the next card read, after Play: "the card answered again (the same card)", "the card was mounted again (the same index): the file reopened, the maps and the session kept". One read ahead failed ("the card?"); the next two picks read 2 slots each (the cache cleared with the reopen), then 0; damaged 0, no underrun. (b) The index replaced on a PC (generation 2, 2 music files added) and the card put back while running: "index /.mstream/autodj-00000002.bin (after a remount) ... opened in 2.8 ms", a new session ("another index on the card"), joined in 1,594 ms (19,238 of 19,412 tracks; PSRAM free at least 145,156 B). The 1-second stubs ran out during the join, so the pick came cued and paused, as designed (4.3). The remount didn't walk the card: the 2 new files waited for the next boot (12). (c) That boot: the library built from the records for the new commit (10.3 s), joined in 1.47 s; the walk found 2 added; the update step's memory check gave the join back (154,264 B: the largest free block 540,660 → 704,500 B, 9,576 B past the line's "up to 694,924 B"); the update ran live (built 12.4 s, saved 2.5 s); joined again in 4.74 s, 19,240 of 19,414, "all in smaller free blocks" |
| 8 | **Pass** (2026-10-16). (a) An album's slots changed so that their CRCs fail, then the album played: "read ahead, row R: its CRC failed (read again later)" for four rows. Two failed again when a pick read them: `D` said damaged 2, but no line named them (the read ahead's "DAMAGED" line covers its own reads only). **Fixed since:** "read at the pick, row R: DAMAGED …" (3.13). The file stayed in use; the first pick was random (every anchor damaged or unread), then normal picks. That random pick's toast said "This track has no Auto DJ neighbours yet: the next track is random", as for any random first pick (9.6). (b) One slot rewritten with a bad list under a good CRC: "BROKEN, picks at random until a remount or the next boot", once; then `[random]` picks; `D`'s index line ends "BROKEN (a bad list under a good CRC): random picks until a remount". (c) The index file renamed: "ABSENT (the file the root names didn't open: io)"; the page's status "Can't read the Auto DJ index" over "picks are random", the settings dim; the empty Queue shows Shuffle all; `De` played nothing; picks `[by track id: no index]`; the toast "No Auto DJ index: picks are random" at the first pick, and again after `D0`, `D1`. No `/.mstream` at all: "index: none (no transfer data on the card)"; the page in all ten languages with "No Auto DJ index on this card" over "picks are random", no text cut. **Finding:** in de, es, it and pt the dim Match strictness row didn't show its value. **Fixed since** (9.2) |
| 9 | **Seen** (2026-10-15 and 16). `D1` at a finished queue, and the join after a card swap, cued a pick paused at 0:00; nothing started |
| 10 | **Partial** (2026-10-15 and 16). The two-line toasts (en, de, ru, ja, zh, 16 shots): the lines apart and inside the card in all of them. In three Japanese and Chinese ones a line touches the card's outline (9.6): left for the user to judge, as is en's taller card under the tab bar. Undo (9.4): no Undo started sound, from paused, stopped and playing queues: a Library Play's, the Auto DJ button's, "Auto DJ from here"'s, and a Clear's (the console's). The no-index screens in all ten languages (check 8). Not reached: zh "正在等待耳机" over a title with 青 or 机 |

### 14.3 The rejoin (2026-11-02)

On the user's Core2, the spare card (the synthetic card of 11.4: 19,412
tracks, 19,410 rows), silent test mode, Auto DJ on. Before: `e7623e0`.
After: `221f075`. No Guru, `task_wdt` or LOW line, 0 underruns.

| Run | Before | After |
|---|---|---|
| A boot's join | "296526 B at the top of the largest free one, 0 of 3 kept and 1 transient blocks in smaller ones", free at least 167,644 B | "300622 B at the top of the largest free one, 0 of 3 kept and 0 transient blocks in smaller ones", free at least 167,624 B, the largest free block 655,348 B as before (5 boots) |
| `gb` on a fresh boot, stopped | "[heap] FAILED: 77640 B", largest block 73,716 B, free 245,884 B of 816,024 B; "the join: NO PSRAM"; the page "Not enough memory for the Auto DJ index" | "joined" in 1,515 ms, 112 passes: "77640 B at the top of the largest free one, 2 of 3 kept and 7 transient blocks in smaller ones", in 11.4 ms; free at least 176,248 B; the largest free block 335,860 B, 81,920 B under the 417,780 B; no `[heap] FAILED`; the page "19,410 of 19,412 tracks have neighbours", "BPM 95 % · key 97 %" |
| `gb` with the 320 kbps MP3 playing, twice in a session | Not run | "joined" in 6,124 and 7,456 ms (172 and 195 passes; 2026-10-15: 5.9-6.2 s), the same blocks at the top, free at least 174,136 B, the largest free block 335,860 B and 221,172 B; no `[heap] FAILED` |
| `gb` with the FLAC playing | Not run | "joined" in 5,838 ms, the same blocks at the top, free at least 149,148 B, the largest free block 155,636 B; no `[heap] FAILED` |
| `D0`, `D1` right after a rebuild | The same failure, twice, the largest block still 417,780 B | "joined" in 1,524-1,527 ms, the same blocks at the top, free at least 176,248 B, the largest free block 335,860 B |
| `D0`, `D1` about 15 s after it | "joined", the largest block by then 303,092 B, where only a class's lowest 4 B fail | "joined" (1.5-3.6 s), free at least 178,028 B |
| `Dc` right after a join | Not run | "joined" (1.5-3.6 s), free at least 77,284 B: the lowest of any join, over the margin |

Host: `test_autodj_join` fails without the fix (the rejoin NO PSRAM at
19,412 tracks and a largest free block of 303,104 B), and with each part of
it taken out in turn (the spacer's size, the fall back, the spacer's and the
blocks' rounding in the margin, the second spacer's test, `probe()`'s).

**The mood block (2026-11-06).** The same Core2 and spare card, now
19,413 tracks (3 added by hand), T with mood tails for 17,954 of them
(docs/MOOD-FILTERS.md 9.6, Run C), so `library.idx` v7 holds a 38,826 B
block of mood words. The Auto DJ index is the one measured above (19,410
rows, restamped with `--mpdj`). One script, run on a control firmware
whose builder never sets the word (16 B smaller) and on the mood
firmware (`af72b50` with the mood filters' S0-S2): a load boot, `z`,
`D0`, `D`, `D1`, `D`, `gw`, `gb` stopped, `Dc`, `Dh` the 320 kbps bench
MP3 and `gb`, `Dh` the bench FLAC and `gb`, `Dh` an Opus. PSRAM's
largest free block, joined unless said (MEASURED):

| Step | Control | Moods | |
|---|---|---|---|
| Load boot, Auto DJ off | 819,188 | 786,420 | -32,768 |
| Load boot, joined | 655,348 | 622,580 | -32,768 |
| Update stopped | 327,668 | 294,900 | -32,768 |
| `Dc` | 327,668 | 294,900 | -32,768 |
| Update during the 320 kbps MP3 | 327,668 | 294,900 | -32,768 |
| Update during the FLAC | 294,900 | 262,132 | -32,768 |

The mood block costs exactly one 32 KB class, 32,768 B, at each step.
No update underran or printed `[heap] FAILED`. The updates' builds took
20.6 / 22.1 s stopped (control / moods), 42.2 / 45.0 s with the MP3 and
28.7 / 30.2 s with the FLAC. The lowest free in the stopped update's
join was 180,876 / 175,032 B. After the FLAC the Opus (2 s) opened its
128,023 B of buffers and played, 101 packets, 0 skipped, on both. The
script without `D0` and `D1` read 294,900 B on the mood firmware too.
Against docs/MOOD-FILTERS.md 9.5's rule: inside the modelled -106,496 /
-40,960 / +49,152 B, every joined figure of the script over 245,764 B,
no redesign.

Heap figures are compared only within one scripted sequence. The
control's own are not the 2026-11-02 figures above (327,668 B, not
335,860 B, after a stopped update; 294,900 B, not 155,636 B, with the
FLAC): a largest free block depends on the steps before it (7.4). In
the script above `library.idx`'s save ended before the join.

The save after the rejoin, A/B'd (MEASURED 2026-11-06): two firmwares
built from S2's tip (`3461c41`), a control whose builder never sets the
word and the mood firmware, each after a boot that rebuilt the library
with or without words, then a reset (a load boot), `z`, `D`, `gb` with
nothing playing and no `gw` first (not deferred), `D`, `Dc`, `D`.
PSRAM's largest free block, control / moods: 655,348 / 622,580 B joined
at the load boot, 327,668 / 294,900 B joined after the update with the
save not yet done, 204,788 / 180,212 B after the save and `Dc`. The
save landed after the rejoin: in the control it ended after the `D`
that follows `gb`; with the block, between the join and that `D`. So the
180,212 B is the save's layout, not the block, whose share on this path
is -24,576 B; 7.4's rebuilt heap records 180,212 B for this layout on
2026-10-14, before any mood work. The control is under 245,764 B too,
so docs/MOOD-FILTERS.md 9.5's rule forces no redesign; a FLAC still
fits (155,636 B), and so do an Opus's 65,536 and 62,464 B blocks. The
layout is the update's own, worth its own look. On the card without
tails (docs/MOOD-FILTERS.md 9.6, Run B) an update after `gw` read
221,172 B at the join while its walk still ran, 335,860 B after it.

Host: in `test_autodj_join`'s model heap with the real block, the boot's
largest free block goes from 753,652 B to 720,884 B (-32,768 B); after
the update, joined, it is 360,436 B in both, pinned in the test.

### 14.4 The card's root (2026-11-02)

On the user's Core2, the 32 GB spare card, never the music card, with the build of
`e7623e0` for 14a and a temporary build (a fault mode set from the console, not in
the tree) of `b2f47ca`, with `e7623e0`'s console from the last runs on, for 14b and 14c. An independent read
of the logs against each row found nothing that holds up a merge. Nothing here
ran after the look and the breaker (METADATA.md 3.8), which owe their own check.

| # | Result |
|---|---|
| 14a | **Pass** for the open, its tries and the remount, three runs; the join that follows, on a boot's fresh heap. The card off the bus: FatFs's answer 3, "1 of 3", ABSENT 43.9 s later, the next read 58.5 s after it: "(after a remount)", joined in 1.85 s. After an update step the join failed instead with `[heap] FAILED` and NO PSRAM, in the first run's update too, before any pull: the rejoin's spacer, nothing of the card's (PR 36). In one run a walk's retry reached the card first: the guard's one restart, the queue kept. A first `Dc` opened with no driver line: the card was still answering (hence the row's note). Not shown: the page's Can't read on a physical pull (a dark screen), a paused player (all three ran stopped), and a status on a card certainly out with the panel lit |
| 14b | **Pass,** with opens failing and with the directory sector getting no answer. The boot's lines; `library.idx` b48fa114 and `tags.bin` c27ddcb1 unchanged, no card writes; no walk; `cid=?`; the Library row, Rescan's note; Reading, then Can't read, 41.9 s on (46.5 s with no answer); no answers stuck at 52 for the last 45.9 s. The fault off and a reboot: "matches the card", 19,412 tracks, the index open and joined. Not shown: `gs` to its T line after ABSENT under no answer, and the spare's T lists no records, so a transfer's library by identity only |
| 14c | **Pass,** after `e7623e0`. Its first run failed: `gs` read the root with the jobs running, printed "no transfer data" for a read fault and restarted the player; since then, under these jobs, `gs` and `gt` don't read it ("T: not read"). ABSENT from the start, the walk (about 21 s), compaction and update step ran to their ends, no restart in 153 s, no answers stuck at 16 and 4. A temporary override took a transfer's library for one with no identity for most of it; the real state ran about 20 s. Not shown: the page's Can't read in a session whose jobs run |
