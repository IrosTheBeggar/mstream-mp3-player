# Auto DJ

Auto DJ keeps the queue going. When the queue's last track starts, it adds
one more, picked on the device from the card's AutoDJ index. No network,
no server. This document is the engine as built: what it does and doesn't,
the pick rule step by step, when it adds a track, sessions, the index on
the device, what a pick costs, the settings, the UI, the console's `D`
command, the Python reference, the tests, the device test card, what is
deferred, and the device checks.

**Status (2026-10-14):** built for 0.11.0, host-tested. The first device
check ran on 2026-10-13 on a synthetic 19,412-track card, on the build
before the fixes (`09f91a1`, console only). Its five problems were fixed
in `7c89673`; the UI came in `e5efd22`. Neither of those two commits has
been on the device yet: section 14 lists what the next release must run.

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
- **No usable index:** random picks from the library, with a 50-track
  cooldown.
- **Cost:** at most 2 slot reads a pick, usually 0 (a read ahead between
  picks). The pick's CPU was 5-9 ms in Follow the vibe on the device
  (MEASURED, before the optimisation); about 1-1.7 ms after it
  (ESTIMATED, to be measured).
- **Memory:** the engine's 19 KB of PSRAM from boot; the join's 155 KB at
  19,410 tracks, only while Auto DJ is on.
- **The UI:** a 4th row in Now Playing's "..." menu; Output > Auto DJ; an
  Auto DJ button on the empty Queue and Now Playing; "Auto DJ from here" in
  the Library's long-press sheet; a small DJ mark on the Queue rows it
  added.

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
| Filter by rating, genre, year, folder, length or keyword | The card has none of them for Auto DJ (DJRF holds BPM, key and an artist key only) |
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

The console's line for a quick skip says which case it was: "no longer
steers", "it has no row in the index: it never steered", or "it wasn't
among the anchors: nothing changes" (for example a track that started
before Auto DJ was switched on).

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
Auto DJ never stops and never goes silent. Counted: "dry".

### 3.8 Loosen before repeating

The user's decision of 2026-10-14. When **every row of the pool** (the
gate's, or dry's 50) is in the 50-track cooldown and **a candidate outside
it exists**, the pool becomes the **50 candidates outside the cooldown
with the highest level sum**, ties by row: the dry rule over the fresh
candidates, below the gate if need be. Counted: "loosened".

Only when every candidate is cooled does the pool stay as it was, and the
pick repeats a track. Why: on the device check, a Stay on seed seed with 3
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
said on the screen (9.6).

### 3.13 Damaged and broken slots

| What | Effect |
|---|---|
| A slot's CRC fails (S1) | Read once more, in this pick if the budget allows, else at its next read. Failing twice in a row it is **damaged**: into a ring of the last 8 damaged rows, never read again until the file is reopened. That anchor is left out; none left: a random pick |
| The read fails (`Why::Io`: the card pulled, a FIL a remount invalidated) | Skipped for this pick; never in the damaged ring |
| S2-S4 fail under a good CRC (**broken**: the writer's bug) | The file is treated as absent: every later pick is random, until a remount or the next boot. Logged once |

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

All hold: "due". The pick then goes through
`PlaybackController::addFromAutoDj()`: one append, no undo of its own
(the queue's quiet append), and the word on what follows goes to the
backend in the same action, so the pick joins gaplessly.

### 4.2 Holds: Waiting and Paused

Conditions 6-8 are holds. Auto DJ adds nothing; the "..." row reads
**Waiting** (Repeat One, the sleep timer) or **Paused** (failures), and
the Output page's status row says why. A hold never blocks a library
update (4.6).

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
| A read ahead that failed | The next after 2 s (a pick still reads what it needs) |
| The index joining | No pick: it waits for the join (condition 1, 5), and the library update waits for it (4.6) |

### 4.6 The library update's fence

`LibraryUpdate`'s safe point gets one input from Auto DJ,
`UpdateEnv::autoDjOwed`: a pick is due (the entry is last, conditions 1-3
and 6-8 hold) but the index is joining or reopening. No fence goes up
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
| A remount that finds another index (or one now, or none now) while on; `Dc` | New | None |
| Play next, + Queue, a jump in the Queue, Remove, a move, Clear up next, their undo | Kept | Played tracks join the ring as they start |
| Skip, previous, seek | Kept | A quick skip leaves the ring (3.2) |
| Anchor, strictness, BPM continuity, Harmonic mixing | Kept | From the next pick; Anchor drops the pin |
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
  1. **Hash:** each library track's path hash (FNV-1a 64), kept as its top
     32 bits and its id, 6 bytes a track, in runs of 4,096 (24 KB blocks).
  2. **Sort** each run.
  3. **STRS** checked.
  4. **DJRF** streamed with its CRC: each row's filter word (BPM rounded
     to a whole number, the Camelot code, the artist key's low 16 bits).
  5. **DJPH** streamed with its CRC, in hash order: each entry's track
     found by a cursor in every run, an equal prefix confirmed by the full
     64-bit hash. A row maps to its present file with the smallest path
     hash. A hash two library tracks share maps neither.
  6. The runs freed: ready.
- **Slices:** 250 hashes, one run's sort, or a 4 KB piece of DJRF or DJPH
  (100 confirms at most) a slice. A loop pass takes slices for up to 8 ms
  and at most one card read. None while a list moves or the card is held.
  The loop's idle wait is 2 ms while it works.
- **Its checks are the file's:** a section that fails its CRC or its
  order makes the file absent until a remount or the next boot.
- **The library under it:** every slice checks that the library index is
  still the one it began on; otherwise it aborts and frees everything, so
  it never reads a freed index.
- **Placement:** its blocks go to the top of PSRAM's largest free block
  (spacers hold the block's bottom while they are taken), the short-lived
  runs below the kept maps. What comes later is carved below them, so
  giving the join back leaves one block, as if it had never joined.

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

On every FatFs remount (the card's guard counts them) the glue reads the
root again into its own copy (never the library's) and compares the index
entry:

- **The same index:** the file reopened; the maps and the session kept; the
  damaged ring cleared. A join under way starts again.
- **Another one, one now, or none now:** closed, opened, and, while Auto
  DJ is on, joined again and a new session (`Dc` starts one on or off).
- No PSRAM for the root: the index closed until the next remount, its
  entry kept.

### 6.6 What makes the index unusable

| What | Effect | The Output page's status |
|---|---|---|
| No transfer data on the card, or the root names no index | Picks by id | No Auto DJ index on this card |
| An index of a major this build doesn't read | Picks by id | No Auto DJ index on this card |
| The file failed its checks (frame, header, the root's entry; a section in the join) | Absent until a remount or the next boot: picks by id | The Auto DJ index failed its checks |
| A broken slot (3.13) | Random picks over the index's rows until a remount or the next boot | The Auto DJ index failed its checks |
| The file didn't open, a read failed, no PSRAM for the root after a remount | Absent until a remount or the next boot: picks by id | Can't read the Auto DJ index |
| A library over 65,534 tracks | Not joined | Too many tracks for the Auto DJ index |
| No PSRAM for the join | Picks by id until the next library build, or until Auto DJ is switched off and on | Not enough memory for the Auto DJ index |
| A read failed during the join | Picks by id; joined again 10 s later | Reading the Auto DJ index… |
| The file older than the library | Rows whose files are gone map to no track; new files have no row | "{mapped} of {tracks} tracks have neighbours" |

The status's second line is "picks are random" under each reason but
"Reading" and the last.

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

| Setting | MEASURED 2026-10-13 (`09f91a1`) | ESTIMATED now (`7c89673`, not yet measured) |
|---|---|---|
| Follow the vibe 55 / 65 / 75 %, matching off, 240 MHz | 7.7 / 6.0 / 4.8 ms | 1.4 / 1.2 / 1.1 ms |
| Follow the vibe 55 / 65 / 75 %, matching on, 240 MHz | 9.0 / 6.5 / 5.2 ms | 1.65 ms at 55 % |
| Stay on seed 55 / 65 / 75 %, matching off / on, 240 MHz | 1.5 / 1.1 / 0.6 ms; 1.7 / 1.3 / 0.6 ms | about 0.35 ms |
| Follow the vibe 55 % off / 75 % off / 55 % on, 160 MHz | 10.1 / 6.3 / 12.0 ms | 1.9 / 1.45 / 2.3 ms |
| Stay on seed, 160 MHz: 55 % matching off / 65 % matching on | 1.9 / 1.6 ms | |

- The estimates come from a host benchmark of the same picks on the
  synthetic card's index (p90 55.4 → 14.9 µs for Follow the vibe 55 %
  with matching off) and a model of the PSRAM cache-line misses (2,138 →
  309 a pick there), against the device's measured times. Only the device
  can confirm them (section 14, check 3).
- Live picks before the optimisation (MEASURED 2026-10-13, the 2-hour
  soak, Follow the vibe 55 %, matching off): p50 7.9 ms, p90 8.4 ms, max
  23.0 ms, flat over the 2 hours.
- The targets: p90 at most 2 ms at 240 MHz, 3 ms at 160 MHz (section 14,
  check 3).

### 7.3 The join

| | |
|---|---|
| Reads (READ) | DJRF, DJPH and STRS once: 18 bytes a row, in 4 KB pieces (about 90 at 19,410 rows) |
| Time, 1,000-hash slices one a pass (MEASURED 2026-10-13) | 2.08-2.12 s idle (112 slices), 2.46 s at 160 MHz; after a fence, 4.6 s with playback paused and 7.2 s with a 320 kbps MP3 and the screen dark (20 ms of idle wait a pass); a scroll adds its own time |
| Time now, 8 ms of smaller slices a pass and a 2 ms wait (ESTIMATED) | At most about 1.5 s idle, about 2.5 s with an MP3 and the screen dark |

### 7.4 Memory

At 19,410 rows and 19,412 tracks, all in PSRAM:

| Item | Bytes | When |
|---|---|---|
| The engine: slot cache (7 × 512), runs, the pick's scratch (READ) | 19,016 (15,414 before `7c89673`, MEASURED) | From boot, on or off |
| The open index file (MEASURED) | 4,148 | While an index is open |
| The glue (MEASURED, the M4 build) | 2,968 | From boot |
| The join's maps and filters: 2 B a track + 2 B a row + 4 B a row (MEASURED) | 155,284 | While Auto DJ is on and joined |
| ... its largest block, the filters (MEASURED) | 77,640 | |
| The join's peak: the maps + runs at 6 B a track + a 4 KB buffer (MEASURED) | 275,952 | A second or two while it joins |
| `Dn`: a copy of the engine, 16 KB of tallies, 4 B a pick (READ) | about 35 KB + 4 B a pick | While a `Dn` runs |
| `Db`: 4 B a read and one slot buffer (READ) | | While a `Db` runs |

- Internal RAM: main.cpp's pointer (READ). The pick allocates nothing; its
  frames are about 420 bytes on the loop stack (READ, the engine's note).
- MEASURED 2026-10-13: the resident set was 177,814 B (with the old
  engine), against the design's 180 KB. The running player's lowest free
  PSRAM was 547 KB idle or with an MP3, 459 KB with a FLAC: the join's peak
  came after the UI's boot dip and never set it.
- Auto DJ on against off: the join's 155 KB (ESTIMATED; check 11).

---

## 8. The settings

| Setting | Where | Console | NVS key (u8) | Values | Default |
|---|---|---|---|---|---|
| Auto DJ | "..." row, Output > Auto DJ, the empty states' button, "Auto DJ from here" | `D0`, `D1` | `on` | 0 off, 1 on | off |
| Anchor | Output > Auto DJ | `Dm0`, `Dm1` | `mode` | 0 Follow the vibe, 1 Stay on seed | Follow the vibe |
| Match strictness | Output > Auto DJ | `Df55` … `Df75` | `floor` | 55, 60, 65, 70, 75 | 55 |
| BPM continuity | Output > Auto DJ | `Dt0`, `Dt1` | `match` bit 0 | | off (D2) |
| Harmonic mixing | Output > Auto DJ | `Dk0`, `Dk1` | `match` bit 1 | | off (D2) |

- **NVS:** the namespace `autodj`, from schema 4. Every change writes all
  four keys at once. Each is read with `isKey()` first: an absent key, or
  a value this firmware doesn't take (a later one's), reads as its
  default. `migrate(3, 4)` does nothing; an older firmware never reads the
  namespace. (`lib/core/NvsLayout`, `src/app/AutoDjStore`.)
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
pages, the sheets and `UiHost`'s Auto DJ calls. 51 messages, in all ten
languages (machine translations, not yet reviewed by native speakers;
the mobile app's words where its English matches).

### 9.1 Now Playing's "..." menu

A 4th row, **Auto DJ**, its state the detail: **On**, **Off**,
**Waiting** (Repeat One, the sleep timer's End of queue) or **Paused**
(failures). A tap switches it in place, the sheet staying, as Shuffle.
The 4-row sheet rises into the header row.

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
| BPM continuity | a switch; "within 8 BPM, or half/double tempo" | switches it |
| Harmonic mixing | a switch; "keys that mix well" | switches it |
| Status | two lines (below) | nothing |

- **Without a usable index** the four in between are dim, "needs an Auto
  DJ index"; a tap gives the amber note "Needs an Auto DJ index: a
  transfer to the card adds one". The switch and the status stay live.
- **The status row:**
  - joined: "{mapped} of {tracks} tracks have neighbours" over "BPM {n}% ·
    key {n}%" (the mapped tracks' coverage);
  - Auto DJ off: "Auto DJ index: {n} tracks" over "read when Auto DJ is
    switched on";
  - joining: "Reading the Auto DJ index…";
  - no index, or none usable: why (6.6) over "picks are random";
  - held: "Waiting: Repeat One is on", "Waiting: sleep timer set to End of
    queue" or "Paused: tracks failed to play", over the index's first line.
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
and, if that tap switched Auto DJ on, switches it off again.

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

- One a pass: the most pressing, in the table's order.
- **While a toast with Undo is up**, these wait until it goes (the failure
  pause doesn't wait). An Undo tap, or Auto DJ switched off, drops them.
- "No Auto DJ index" is said once per switch-on or boot, not once a
  session: every Play is a session.
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
| `D` | Status: the settings and "now:" (the trigger's reason, 4.1; "owed" when the update step waits); the index (or why there is none: generation, selection, rows, k, head, floor, the gate's level, BROKEN); the join (ready with its coverage, time, slices, peak; or its phase; or none while off); the session (seed, the anchors' rows, the pin, the key anchor, cooled rows / artists / ids, damaged rows, DJ marks, the queue's epoch); the counters (below); picks and breaks per mode; the classes; the last pick's reads and time; memory |
| `D0` / `D1` | Auto DJ off / on (saved, as the UI). Off gives the join back; on joins |
| `Dm0` / `Dm1` | Anchor: Follow the vibe / Stay on seed (saved) |
| `Df55` … `Df75` | Match strictness, 55-75 by 5 (saved); other values are refused |
| `Dt0` / `Dt1` | BPM continuity off / on (saved) |
| `Dk0` / `Dk1` | Harmonic mixing off / on (saved) |
| `Ds<hex>` | The next session's seed, 1-8 hex digits (a reproducible session) |
| `Dr` | A new session now (on or off), no seed track |
| `Dp` | One pick appended now, whatever the trigger says (cued at a finished queue); its line, then the anchors and the pin. Refused behind the fence |
| `Dn<k>` | k dry-run picks (1-2,000) on a copy of the session, each treated as played (the next follows it); a read ahead first when one is due, as the loop does. The live session is untouched. A summary: picks, dry, loosened, random, by id, repeats, artists, breaks, drawn statuses, reads, the time's p50 / p90 / max, the classes |
| `Dj` | The join again from its start (its time and memory). Refused while Auto DJ is off |
| `Dc` | The file closed and opened again, as a remount that found another index (a new session) |
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
  `[random]`, `[by track id: no index]`, `[tempo drawn]`, `[key drawn]`,
  `[tempo break]`, `[key break]`, `[cued: …]`, `[REFUSED …]`, `[the
  listener's undo went with it]`.
- **Other lines:** a session's start with its seed; the index opened (or
  why not); "joined" with its coverage, time, slices, reads, bytes kept
  and at the peak, and where its blocks went; the join given back; a quick
  skip; a pick taken out; a pick that failed; a read ahead that failed; a
  broken slot (once a file); the library rebuilt with the marks carried.

---

## 11. Code, tests and tools

### 11.1 Files

| File | What |
|---|---|
| `lib/core/AutoDjEngine.h/.cpp` | The session and the pick (section 3); `Stats`, the console's counters |
| `lib/core/AutoDjTiers.h/.cpp` | The tempo windows, the Camelot codes, the statuses, the ranks, D3's odds, the gate's level |
| `lib/core/AutoDjRng.h` | PCG32, `below(n)` by Lemire's method |
| `lib/core/AutoDjJoin.h/.cpp` | The join, its `Maps`, the placement's spacers, a track's path hash |
| `lib/core/AutoDjTrigger.h/.cpp` | `decide()`, its waits and holds, the failure pause |
| `lib/core/AutoDjDriver.h/.cpp` | The loop pass: sessions, steering, the trigger, the pick, the read ahead, the DJ marks across a read-back, the dry run |
| `lib/core/AutoDjCommand.h/.cpp` | The `D` command's parser |
| `lib/core/AutoDjView.h/.cpp` | What the UI shows and says |
| `lib/core/CardAutoDj.h/.cpp` | The MPDJ v2 reader (`Table`, `slot()`, `rowOf()`) and a writer for the host tests |
| `lib/core/QueueModel`, `PlaybackController`, `QueueSaver`, `LibraryUpdate`, `NvsLayout`, `QueueView`, `CardRoot`, `CardContract` | The hooks: the quiet append and the play epoch; `addFromAutoDj()`, `endedAtEnd()`, the cue, `naturalEnds()`; `queue.txt`'s snapshot writes; `autoDjOwed` and the give-back; schema 4; the marks' ring and the badge rule; the root's index entry; `read.mpdj=2` |
| `src/app/AutoDj.h/.cpp` | The glue: the open file, the join's slices, remounts, the fence, the settings, the console, the UI's view and news |
| `src/app/AutoDjStore.h/.cpp`, `AutoDjContract.h` | NVS; the majors this build reads |
| `src/main.cpp` | `stepAutoDj()` after the card's guard and before the queue's save and the card's step; the refill wait; the give-back hook; the fence hooks; `UpdateEnv`; `MainUiHost`'s Auto DJ calls |
| `src/ui/*` | The "..." row, Output > Auto DJ, the empty states, the long-press row, the DJ mark, the toasts |

### 11.2 The Python reference and the fixtures

- **`tools/autodj_ref.py`** is the pick rule in plain Python (standard
  library; `tools/mpdj.py` reads the golden indexes). Every rule is the
  engine's, in the same order, drawing from the same PCG32 stream.
  - `python tools/autodj_ref.py` writes `test/fixtures/autodj/`;
  - `python tools/autodj_ref.py --check` exits 1 when a file there
    differs.
- **`test/fixtures/autodj/sessions.json`** (CC0, synthetic only):
  - PCG32 outputs and `below(n)` draws;
  - the gate's level for each strictness and floor;
  - 7 tables: 4 goldens of `test/fixtures/card/golden`, 3 synthetic (one
    for dry ties, one that starves);
  - 26 sessions: 1,151 picks and 956 reads ahead, with listener tracks,
    quick skips, Anchor changes, damaged, flaky, failed and broken slots,
    the index going and coming, reopens and renumbers, every pick's
    expected fields and the counters at the end.
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
| `test_autodj_engine` | Every fixture session replayed twice (runs made at the read ahead, then at the pick), every pick, read and counter exact; and each rule on its own: the gate, dry, loosening, the ring and the quick skip, the pin, the key anchor, the cooldowns, the read budget, the cache, damaged and broken slots, picks by id, no allocation in a pick |
| `test_autodj_join` | The goldens and the hardening files, written files, strides and CRCs, every slice boundary with a fence or a changed index, no memory, the library limit, a FAT model, memory at scale, the placement in a model heap |
| `test_autodj_trigger` | Every condition, the first that fails, the finished queue, the holds, `owed` only while joining, the failure pause |
| `test_autodj_driver` | Sessions, steering, the cue, the marks across a read-back, refused and removed picks, the holds, the failure pause, the index arriving late, the seed before the join, the button's lookups, the refill wait, the dry run, the `D` parser |
| `test_ui_autodj` | The row's state, the Output line and its fallback, the dim rows, the strictness steps, the status lines, the empty-state button, the toasts' rules |
| `test_card_autodj` | The MPDJ v2 slots, header and selection |
| `test_queue` | The quiet append (the listener's undo kept, its rank in the snapshot, at the cap), the play epoch, the snapshot writes under an hour of appends |
| `test_playback` | The pick's gapless word, refused behind the fence, Repeat All, the holds, the cue, every Play and Clear bumping the epoch, natural ends |
| `test_library_update` | `autoDjOwed` holding the fence; the memory check with the join given back |
| `test_nvs_layout`, `test_ui_queue`, `test_ui_library`, `test_ui_strings`, `test_card_contract` | Schema 4; the badge and the "added" mark; the texts' room in ten languages; `read.mpdj=2` |

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
| **A repo tool to damage a slot** (check 8) | The first check's helper stayed outside the repo |
| **A native review of the translations** | As for the rest of the catalogue (docs/I18N.md) |

---

## 14. Device checks

On the synthetic test card (11.4), the release-check docs' way, with
their rules: silent test mode (`z`) after every boot before anything
plays; no Bluetooth or power commands (`f`, `B`, `c`, `P`); after each `D`
command, the console's loop-stack line must not say LOW; nowhere a Guru,
a `task_wdt`, an `[E]` line, `[heap] FAILED` or a LOW stack line.

**The first check ran on 2026-10-13** on the M4 build (`09f91a1`, console
only, a dev env that read the index; the release build reads it since
M5). The card: the synthetic tree, two full-length bench files, and the
research's 19,410-row index. Its five problems (the pick's CPU, Stay on
seed repeating at 75 %, the join's memory deferring a library update, the
first pick after a skip timing the decoder, `queue.txt` not keeping up)
were fixed in `7c89673`. Nothing since `09f91a1` has been on the device.

The next release runs every check below on the release build. "Before" is
2026-10-13's result.

| # | Check | Pass | Before (2026-10-13) |
|---|---|---|---|
| 1 | **Boot.** Boot with Auto DJ off, then `D1`, then `D0` | Off: "joined when Auto DJ is switched on", no "joined" line, `D` "0 B held", the engine 19,016 B. `D1`: "joined", "at the top of the largest free one", about 90 reads; at most about 1.5 s idle, 2.5 s with an MP3 and the screen dark; a list scrolled meanwhile stays at 30 fps. `D0`: "the join given back … 155284 B". `Dj` while off is refused. The index opened in at most 20 ms. Browsable time level with 0.10.0's on the same card | **Partial.** Opened in 5.5-6.5 ms; joined in 2.08 s, 112 slices, 155 KB kept, 276 KB at the peak; scrolling at 30 fps (the join grew by the scroll's time). The browsable comparison needed another flash: not done. The join ran with Auto DJ off too (since changed) |
| 2 | **Bench.** `Db200` with the 320 kbps MP3, then the FLAC | p50 at most about 3 ms; max at most 14 ms (MP3) and 29 ms (FLAC); 0 failed | **Partial.** p50 1.2-1.4 ms, 0 failed, FLAC max 17.0 / 17.3 ms; MP3 max 13.2 ms in one run, 20.7 ms in the other. A run during the boot's card walk (p50 11.9 ms, max 88 ms) excluded as contended |
| 3 | **Pick CPU.** `Dn200` in both modes at 55 / 65 / 75 %, matching off then on, at 240 MHz; at 160 MHz Follow the vibe 55 % off, 75 % off, 55 % on and Stay on seed 55 % off. A live pick with 2026-10-13's seed and queue (`Ds717feda5`, then `qp0`: the same row as then), at both speeds | **The re-measure after the optimisation.** p90 at most 2 ms at 240 MHz, 3 ms at 160 MHz, every run (estimates in 7.2); reads at a pick 200/0/0; `D`'s picks unchanged by the runs; the live pick at most about 2 ms plus its reads' time (then 6.38 ms, reads 3.25 ms). Stay on seed 75 %: no class 16-23, at least 150 loosened, artists far above 3; live picks carry `[loosened …]`. Follow the vibe's repeats, artists, classes and breaks as before (0 loosened) | **Fail.** Follow the vibe p90 4.8-9.0 ms at 240 MHz, 6.3-12.0 ms at 160 MHz. Stay on seed passed (0.6-1.7 ms at 240 MHz; 1.6-1.9 ms at 160 MHz). Reads 200/0/0. Stay on seed at 75 % repeated 3 rows: "repeats 197, artists 3" |
| 4 | **Albums and skips.** Queue an album, Auto DJ on, skip 5 tracks quickly. Then the quick-skip line's three forms: a bench file (no row), a track that started with Auto DJ on, a track that started before `D1` | No pass with more than one read ahead; no pick over 2 reads; the skipped tracks out of the anchors (`D`); quick skips +5; no pick until the album's last track. The three lines: "it has no row …", "no longer steers", "it wasn't among the anchors …" | **Pass** (run 2): one anchor left, quick skips +5, the pick at the last track read 1 slot. The line said "no longer steers" for a track with no row (since fixed) |
| 5 | **Soak.** Auto DJ on, the stubs, 2 hours, the queue reaching its 5,000 cap | No reset, no underrun; a word at every start; no row again within 50 picks; the cap pushing out the oldest; PSRAM and internal lows flat once the queue stops growing; the loop stack's low-water mark. **`queue.txt` keeps up:** `q`'s write count rises by at least 8 every 5 minutes (a write at least every ~35 s), most from a snapshot; a write's dip at most about 20 KB; after a pause, written within about 6 s, and a reboot restores at the paused entry. Live picks p90 at most 2 ms | **Pass, except two.** 7,520 picks, no reset, 0 underruns, 7,521 gapless joins with 0 cuts, all class 0, the closest repeat 51 apart, the cap at 79.5 min. **`queue.txt` failed:** no write for the last 59.5 min. PSRAM's low fell 446 → 400 KB with the queue's growth, then flat 54 min (partial). Live picks p90 8.4 ms (as check 3) |
| 6 | **Fence.** `gb` during a session with the MP3 playing, then `n` right after "joined", three times; `gb` during the boot's join | The freed bytes equal the join's; `D` says the fence; the session and the marks kept; the last entry still gets its pick; the rejoin at most about 2.5 s with the MP3; the pick after `n` lands after the new track has 500 ms buffered, 0 reads, the usual time; "waited for a refill" +1 a skip. During the boot's join: aborted, one join after, no crash | **Pass,** with two findings since fixed: the rejoin took 7.2 s with the MP3 (4.6 s paused), and the first pick after it took 108.52 ms (the decoder's refill) |
| 7 | **Card.** (a) Pull and reinsert mid-session, paused. (b) The index replaced on a PC (`synthtransfer.py --replace`, a new generation), reinserted while running. (c) A first boot with Auto DJ on of a card with new files the records don't have | (a) "the same index: the file reopened, the maps and the session kept"; picks go on, at most 2 reads; damaged 0. (b) The new generation opened "after a remount", a new session ("another index on the card"), joined again, picks go on. (c) No "the update step waits for the next boot (the track table …)"; if the give-back line comes, the largest block grows by about the join's bytes; after the build "the join again" and "joined" | **Not run** (the card had to move). In 2026-10-13's first boot, the 155 KB join was resident when a library update deferred to the next boot for its track table's block: the give-back answers that |
| 8 | **Bad files.** (a) One slot's bytes changed so its CRC fails. (b) One slot rewritten with a bad list under a good CRC. (c) No index: the file renamed; and a card with no transfer data | (a) "its CRC failed (read again later)", then "DAMAGED …"; picks go on; `D` damaged ≥ 1; the file stays in use. (b) "… BROKEN, picks at random …" once; then `[random]` picks; `D`'s index line says BROKEN; a remount clears it. (c) "ABSENT (the file the root names didn't open …)", picks `[by track id: no index]`, the page "Can't read the Auto DJ index", the settings dim, the empty states show Shuffle all, `De` plays nothing; with no transfer data, "No Auto DJ index on this card", and the toast once | **Not run.** (a) and (b) need a tool to change a slot (13) |
| 9 | **A finished queue.** An album ends with Auto DJ off; `D1`. Then the sleep timer's End of queue | A pick cued, paused at 0:00; nothing starts; Play starts it and the next pick follows; the toast. While the timer runs, no pick and `D` names it; after its stop, a pick cued | **Pass** (3 cued picks, none started by itself) |
| 10 | **UI.** Screenshots (`X`) in all ten languages | The "..." sheet in each state (On, Off, Waiting, Paused); Output's row line, with the Anchor and the state-only fallback; the Auto DJ page live, dim, and each status; the Queue's marks; both empty states with the Auto DJ button and with Shuffle all; the long-press sheet with "Auto DJ from here" first; the toasts, waiting behind an Undo toast; Undo switching Auto DJ off. The user's real-finger taps | **Not run** (no UI then) |
| 11 | **Memory at 20k.** The running lowest PSRAM over 2 minutes of the MP3, Auto DJ off, then on and joined | On is about 155 KB lower than off (the join); the engine's 19 KB is held either way | **Couldn't tell:** the build joined at boot either way (pmin 547 KB idle and with an MP3, 459 KB with a FLAC, in both). Resident 177,814 B, against the design's 180 KB |
| 12 | **The Auto DJ button with Auto DJ off.** `D0`, `qx`, `De`; again in Stay on seed | At most 8 lookups (1 expected) in at most about 50 ms; playback at once; "joined" 1.5-2.5 s later; the first pick after it (a `D` before it shows "owed"). Stay on seed: `D`'s pin is the seed's row after the join | New since 2026-10-13 |
| 13 | **Settings across a reboot.** Change each, reboot | Each comes back "(saved)"; the boot starts its own session | **Pass** |
| 14 | **The user's own card** (no transfer data) | No index: the toast once, the page's status, Shuffle all on the empty states; Auto DJ adds random library tracks | New since 2026-10-13 |

After the checks, the device and the card are put back as the release
docs do (the firmware, the card, and the NVS keys the checks saved).
