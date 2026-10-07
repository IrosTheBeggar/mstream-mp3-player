# Shuffle and repeat, and Now Playing's two menus

The user asked for three changes to Now Playing:

- the artist and album rows lose their taps, and get smaller, so the
  transport below has room;
- a tap anywhere on the cover and the text opens a menu: Go to artist, Go
  to album, Go to folder;
- the "..." menu loses its Go to rows and gains Shuffle and Repeat.

That gives two menus, one for navigation and one for playback. Shuffle
and repeat are new modes of the player: there was no shuffle mode (only
"Shuffle all", a one-shot), and repeat was always on with no UI.

**Status: designed (2026-10-06, at 5e26eba, on feature/np-menus) and
built** (section 14: host-tested, both images build with every guard);
**checked on the device once** (section 10, at 065ec1f). It found two
faults, fixed since (section 14's last entry: Shuffle all's Undo left
shuffle on; Repeat One's "1" didn't read), and two of section 10's
expectations that were wrong; the fixes wait for their own device steps
(section 10, steps 1, 9a and 14). What Now Playing looks like and how it takes touches is the spec
in [ARCHITECTURE.md](ARCHITECTURE.md) ("Now Playing", under UI): the
layout, the hit areas, the two menus and their texts, the indicator, the
haptics. This document is the rest:

- the player's side: what shuffle and repeat do to the queue, the gapless
  joins and the sleep timer, and what is saved;
- the code, Now Playing's included;
- the host tests, the log lines, the device check and the build order.

Two designs were written for it, one for the layout and the touch and one
for the player. They were read against the code. This one takes the
first's layout, touch and sheets and the second's model of the queue, and
settles what they left open (section 13).

**What the user was told, and what this keeps:**

- Shuffle is a toggle. On: what is up next is shuffled, and the track
  that plays plays on. Off: the queue's own order comes back. It outlives
  a restart, and the Queue tab shows the order that plays.
- Repeat goes Off, All, One. All goes back to the first entry after the
  last, gaplessly. One plays the entry again at its natural end, and next
  and prev still move. Against the sleep timer, the timer wins: it
  pauses.
- Both rows show their state, as the Sleep timer row does, and a tap
  changes it in place.
- A small indicator on Now Playing shows when either is on.
- The navigation menu opens on a tap anywhere on the cover, the title,
  the artist or the album. The seek bar and the waiting panel's buttons
  keep their taps.

**What this design decided** (the user's defaults didn't say):

- **Repeat's default is Off.** Until now the queue always wrapped
  (`setRepeat()` was never called). With All as the default, the
  indicator would show on every unit from its first boot. The user
  confirmed it after the device check.
- **Off at the queue's end stops on the last entry**, as `setRepeat(false)`
  always has (host-tested); Play then plays that track again.
- **Shuffle all turns shuffle on** and plays the library from a random
  track. As a one-shot it left the menu saying "Shuffle: Off" over a
  shuffled queue, and Off could never bring the library's order back.
  It is one edit with the mode in it, so its toast's Undo puts back the
  queue and the mode it found (section 2.6).
- **While shuffled, a container's Play starts on a random track**; Play
  on a track starts on that track. The rest is shuffled after it.
- **Play next and + Queue are never shuffled in.** The listener put them
  where they are, and the toast's View finds them there.
- **Repeat All while shuffled replays the same order**, never a new one:
  the first entry is offered for the join while the last one plays.
- **With One, the skips wrap** at the queue's ends (as All's do); only a
  natural end repeats. A track that fails moves on.
- **While a play waits, only the cover opens the navigation menu.** The
  title strip holds the wait's status then, and the rows the buttons.

Where the facts come from: the code at 5e26eba (`PC` is
`lib/core/PlaybackController`, `QM` `lib/core/QueueModel`, `NPP`
`src/ui/NowPlayingPage.cpp`; references are `file:line`), and the user's
measurements of the input: the hold is 500 ms; a tap tick is 33 ms at
level 235 and a hold a double tick; controls at the right edge need hit
areas that reach it; y >= 240 is the button strip; the calibrated panel
is right to within a few px.

## 1. What the tree has

- **Repeat.** `bool repeat_ = true` (PC.h:422), never changed by the
  firmware (GAPLESS.md 3.1). Every wrap is a `QueueModel::step/peek(delta,
  repeat_)`: `advance()` (PC.cpp:128), `prev()` (:165), `cue()` (:215),
  `pauseAtBoundary()` (:394), `refreshOffer()` (:565), `syncHeard()`
  (:630, :644). Repeat is part of the offer's `Signature` (PC.h:362).
  `setRepeat()` is a bare setter (not an action; test_gapless_player:448
  notes it).
- **The queue.** Entries {track, key}, 8 B, in PSRAM, and an undo snapshot
  the same size, both growing by doubling (QM.h:15). Edits are by
  position, keys never change, the undo is one level (QM.cpp:246).
  `assign()` is the restore path (QM.cpp:263).
- **The self-join exists.** A queue of one on repeat joins its entry to
  itself: `refreshOffer()` never reuses the heard token, so each loop
  gets a new one (PC.cpp:577-584). Host-tested at both levels:
  test_a_queue_of_one_on_repeat_loops_as_one_stream
  (test_gapless_player:458) and test_the_same_track_again
  (test_gapless:369). The engine closes the decoder at Ending before it
  probes the next file (GaplessEngine.h:21-30), and the backend starts it
  through the same `file_` object (GAPLESS.md 3.2): never two handles on
  one file.
- **Persistence.** `queue.txt`, `mstream-queue 1 <entries> <current>
  <generation>` then a path a line (QueueText.h:12). The parser is
  strict: nothing after the generation (QueueText.cpp:46), the line count
  must match (:159). The line buffer is `kMaxPath + 2` (:97), the entry
  buffer `kMaxPath + 1` (:26). NVS `queue` holds gen, pos and resume
  (QueueStore.cpp). `QueueSaver` re-pairs a resume point at its entry's
  new line after an edit that only moved it. Schema 1, `migrate()` empty
  (NvsSchema.cpp:25); no key has been added since the schema began.
- **Shuffle all** (Ui.cpp:209): a PSRAM copy of the library's ids,
  `queueview::shuffle()` (xorshift32 Fisher-Yates, multiply-shift bound,
  QueueView.cpp:129) seeded with `esp_random()`, then `playNow(.., 0)`.
  Its only caller; Now Playing's and the Queue's empty states call it.
- **The sleep timer.** main.cpp works out "the last of the queue" and
  "the last of the album" from `cur + 1` twice, and they must agree:
  `sleepEndsAtCurrent()` (main.cpp:1906) and `stepSleep()` (:2034).
  `EntryStart` keys on the queue key (SleepTimer.cpp:345).
- **Track changes** are keyed on `currentKey()`: main's `[queue] now at`
  line and `danceMode.onTrackChanged()` (main.cpp:2569), and the lengths
  learned (:2547).
- **Sheets.** `Ui::route()` closes the sheet on every row tap, then calls
  the owner (Ui.cpp:1180-1188). `Sheet::onEvent()` clears `pressed_` on a
  Tap without drawing (Overlays.cpp:362-366). `Sheet::setDetail()` draws
  a changed detail; Ui keeps the "..." sheet's Sleep timer row following
  the timer each pass (Ui.cpp:959). `VolumeSheet` ignores a touch that
  starts within 300 ms of its opening (`kSettleMs`, Overlays.cpp:610).
  The rows' geometry is `lib/core/SheetLayout.h`: `top(n) = 240 - (36 +
  40 n + 4)`, so 3 rows start at y 80. Pages don't update under a modal
  (Ui.cpp:1010).
- **Now Playing.** The constants are NPP:39-65; `kStripH` is 56
  (src/ui/Gfx.h:34), so nothing taller than 56 rows is drawn in one push.
  The seek bar's off and back rows are absolute (`SeekBar.h:60`: 130 and
  138), tested at those rows (test_seek_bar:287-301, 344-356; its line at
  `kY = 174`).

## 2. Shuffle

### 2.1 The model: the queue is reordered, and remembers its own order

Shuffle **reorders the entries themselves**; each entry carries a
**rank**, its place in the queue's own order.

- A play-order view over an unchanged queue was the other way. Every
  position the code passes around would then need mapping: the Queue
  tab's rows and selection, remove, moveNext, the saver's lines, the
  resume point's line, `step()`/`peek()` and the gapless word.
- Reordering keeps positions **play positions** everywhere, so none of
  that changes, and "the Queue tab shows the order that plays" is true by
  construction. GAPLESS.md 3.1 said a shuffle "only has to change what
  `peek()` returns"; here not even `peek()` changes.

`Entry` becomes {track, key, rank}: 12 B. While shuffled, ranks are
distinct and give the own order (gaps are fine). While not, they mean
nothing (the own order is the positions) and nothing keeps them.

Memory: +4 B an entry, and the same in the undo snapshot. A 10,000-entry
queue goes from 160 KB to 240 KB of PSRAM with its snapshot (480 KB at
worst after doubling). A rank array kept only while shuffled would save
that, but would double every memmove path. Not worth it.

### 2.2 On (`QueueModel::setShuffled(true)`)

1. Every entry's rank is its position (O(n)).
2. The entries after the current one (`current + 1 .. n - 1`) are
   shuffled (section 2.7). The current entry and everything before it
   stay as they are: what played stays as it played.
3. `shuffled_` is set, the undo is dropped (`undoEdit_ = None`, its
   memory kept for the next snapshot), and `changed()` bumps both
   versions.

Nothing up next (an empty queue, or the current entry the last): nothing
moves, but the versions still bump, so the saver writes the mode.

### 2.3 Off (`QueueModel::setShuffled(false)`)

1. `std::sort` of the entries by (rank, key), in place. Introsort
   allocates nothing; `std::stable_sort` asks the heap for a buffer, so
   not that. Keys are unique, so the order is total (two equal ranks only
   come from a hand-edited file).
2. `current_ = positionOf(the current key)`.
3. `shuffled_` cleared, the undo dropped, `changed()`.

On, then straight off, is the identity. Tracks that played while shuffled
but rank after the current one come up next again: that is the own order
coming back, which is what the user was told.

Cost: about 130,000 comparisons for 10,000 entries in PSRAM, tens of ms
on the loop task, once per tap. main logs the ms (section 6).

### 2.4 Edits while shuffled

Positions are play positions, as the Queue tab shows them.

| Edit | Where it goes in the play order | Its rank | After Off |
|---|---|---|---|
| Play (`replace`, start s) | s's track first (current, position 0); every other track shuffled after it, those before s in the list too | the given order, 0 .. n-1 | the given order, s's track current |
| Play with `kAnyStart` | all shuffled, the first current | the given order | the given order |
| Play next (`insertNext`) | right after the current entry, in the given order | right after the current entry's rank: the ranks above it go up by n | still right after the current entry |
| + Queue (`append`) | at the end, in the given order | after the highest rank | at the end |
| An add to an empty queue | as Play from its first: the first current, the rest shuffled | the given order | the given order |
| Remove | the others keep their places; a removed current entry gives way to the next in play order, as ever | the others' unchanged (gaps) | what is left, in its own order |
| Play next in edit mode (`moveNext`) | right after the current entry, in play order | the moved ones, in that order, right after the current entry's rank; the ranks above it go up by their count | right after the current entry |
| Clear up next | the play order's up next goes | unchanged | what is left, in its own order |
| Clear | empty; shuffle stays on | - | - |
| Undo | the snapshot, ranks and all | as snapshotted | consistent |

- **Not unshuffled: the empty queue.** Adding to an empty queue makes the
  first new entry current (QM's rule), so it is a Play from the first,
  minus the start; it is laid out as one.
- **Ranks can only grow** between two Plays (a Play renumbers 0 .. n-1;
  so does Off, in effect). An edit that would take the highest rank past
  0xFFFFFFFF is refused, as out of memory is (false, the queue
  unchanged). That takes four billion adds without a Play: never, but
  checked, so it can't wrap silently.
- The shifts in `insertNext()` and `moveNext()` are one O(n) pass, as
  their memmove already is. `append()` scans for the highest rank: O(n).

### 2.5 Play, a container's Play, Shuffle all

- `QueueModel::kAnyStart` (= `kNone`) for `replace()`'s start: shuffled,
  a random first; not shuffled, the first. (Not shuffled it must be 0,
  not clamped: the clamp would give the last.)
  `PlaybackController::kAnyStart` is the same.
- **The Library** (LibraryPage.cpp:688): `start >= 0 ? start : kAnyStart`.
  A tapped track plays first; a container's Play (an artist, an album, a
  folder, "Play all N") starts on a random track while shuffled, on the
  first while not.
- **Shuffle all** (`Ui::shuffleAll()`): `playNow(the library A-Z, n,
  kAnyStart, /*shuffle*/ true)`, a Play that turns shuffle on as part of
  the edit (`QueueModel::replace(.., shuffled)`; 2.6). No PSRAM copy any
  more (`replace()` copies the ids) and no `queueview::shuffle()`. Off
  afterwards gives the library A-Z, from the track playing. The toast
  stays "Shuffling 77 tracks" with Undo, and the Undo puts back the queue
  and the mode it found. Out of memory, neither changes.
  - *As first built* it called `host_.setShuffle(true)` and then
    `playNow()`: the toggle, then the Play. The Play's snapshot was taken
    after the toggle, so its Undo brought the old queue back with shuffle
    still on: the device check undid a Shuffle all from the empty state and
    got `q`'s `shuffle on` over 0 tracks, the next album Play shuffled.

### 2.6 Undo

A toggle is **not an edit and not undoable**: it drops the undo (keeping
its memory). An undo across a toggle would bring back an order from the
other mode, with ranks that mean nothing in it. Toggling again is the
undo of a toggle, and Off restores the own order exactly.

**A snapshot carries the mode it was taken in** (`undoShuffled_`), and
`undo()` puts it back with the entries. For every edit but one that is
the mode the queue is in now (a toggle drops the undo), so nothing
changes for them. The one is a Play that sets the mode:
`replace(tracks, n, start, shuffled)` takes the snapshot first, in the
mode the queue was in, then sets the mode and lays the new queue out in
it, as one edit. Shuffle all is its caller (shuffled true): its Undo
gives back the queue as it was, in its own order when shuffle was off,
and shuffle off. `replace()` without the mode keeps the queue's.
`undoShuffled()` reads what an undo would put back (`q`'s line says
`undo: play now (and shuffle off)` when it differs), and both undo log
lines add `(shuffle off again)` when it did (section 6).

What else Shuffle all changed stays as any Play's Undo leaves it
(PlaybackController's rule, unchanged): the track that was current is
current again from 0:00, in the player's state at the Undo (Shuffle all
started playing, so it plays unless paused since, even if it was paused
before), a start point it had is gone, and the Queue's "added" marks stay
cleared. Repeat isn't touched by Shuffle all.

An Undo toast still up after a toggle would answer "Nothing to undo"
(Ui.cpp:1280). Ui hides it instead (section 4.5). So too after the
console's `qu` (it used the undo); its undo of a Shuffle all changes the
mode back, which is no toggle, and the log says which took the undo.

### 2.7 The generator

- A third hook on QueueModel, beside alloc and free:
  `using RandomFn = uint32_t (*)();`. The firmware passes `esp_random`
  (fresh hardware entropy for each shuffle; main.cpp:80 becomes
  `static QueueModel queue(psramAlloc, psramFree, esp_random);`).
  nullptr: a fixed xorshift32 sequence from 0x2545F491, so host tests
  repeat.
- One draw seeds a shuffle; a random first (`kAnyStart`) is one more draw,
  bounded by multiply-shift.
- The loop is today's `queueview::shuffle()`, moved to
  `lib/core/Shuffle.h` as a template (`shuffle::permute(T* a, uint32_t n,
  uint32_t seed)`: xorshift32 from the seed, 0 taken as 1; Fisher-Yates
  from the end; j = (x · (i + 1)) >> 32). QueueModel permutes `Entry`s
  with it. `queueview::shuffle()` loses its only caller (2.5) and goes;
  its test moves with the loop (same seed, same order).
- Fair enough: a uniform permutation, the bound's bias at most n / 2^32
  (2e-6 at 10,000). Only 2^32 orders are reachable from a 32-bit seed;
  that doesn't matter for listening. No spreading of an artist's tracks.

### 2.8 What the player does

- `PlaybackController::setShuffle(bool on)` is an action (`Act`) around
  `queue_.setShuffled(on)`. The current entry is the same entry (its
  key), in the same state: nothing starts, stops or is cued, and there is
  no `currentMoved()`. A start point (the resume point after a boot) and
  the length hint belong to the key, so they stay, and so does the last
  failure.
- The `Act` sends the word on what follows at once. If the next entry
  changed, it gets a new token: inside the decode-ahead window (the last
  ~1.4 s) that is any edit's cut, and past the join the heard advance
  starts what now comes next (GAPLESS.md 3.5's table, "Play next /
  insertNext..."). If the next entry is the same (Off, where the own
  order has the same next), the token stays and nothing is cut.

### 2.9 Saved: `queue.txt` version 2

```
mstream-queue 2 <entries> <current> <generation>
<rank> <path>
...
```

- **Version 2 means shuffled**, and is written only then. Not shuffled,
  the file is version 1, byte for byte as today: a unit that never
  shuffles never changes format, and a downgrade still reads it. An
  empty shuffled queue is the header alone (`mstream-queue 2 0 -1 7`).
- **A line** is the rank in decimal, one space, the path. An id the
  catalog doesn't know writes `17 ` (no path), so the line count stays
  the entry count and is dropped at the read like v1's empty line.
- **The read** takes both versions (`Header::shuffled`). A v2 line that
  doesn't start `<digits> `, or whose rank is over 4294967295, means the
  file isn't whole (as a bad header): the queue is left alone. The ranks
  of the lines that survive go into a second `MemorySink` (4 B each),
  then `assign(ids, n, current, /*shuffled*/ true, ranks)`. Dropped
  tracks leave gaps in the ranks, which is fine.
- **The buffers grow by 11** (`4294967295 `): `LineReader::line_`
  (QueueText.cpp:97) and `writeEntry()`'s (:26). Otherwise a 256-byte
  path's line overflows, and its entry is dropped.
- **The writer** picks the version from `q.shuffled()` at the header. A
  toggle between `begin()` and the header bumps `contentVersion()`, so
  the step returns Changed and the write starts again, as for any edit.
- **No migration**: a version 1 file is unshuffled, its ranks the
  positions.
- **A downgrade while shuffled** (to a firmware from before this): it logs
  `[queue] /.player/queue.txt: not a whole queue file, ignored`, starts
  with the whole library, and writes a version 1 file over it. The queue
  is lost once; nothing else is.
- **`QueueSaver` is unchanged.** A toggle is a content change: the file
  is written 2 s later, or at `flushNow()` before a power-off. The NVS
  position pairs by generation as before. Off while paused moves the
  current entry to another line: the resume point is saved again at the
  new line once the file holds it (the moved-only rule).
- **A library rebuild** (`QueueStore::remap()`) goes through the same
  text, so the order, the ranks and the mode survive it. Its two paths
  that clear the queue keep the mode: `assign(nullptr, 0, -1,
  queue_.shuffled())`.
- **Why in the file and not NVS:** the mode is atomic with the ranks it
  needs, so a shuffled mode with a rankless file, or the reverse, can't
  happen. With no card there is no library to shuffle anyway.

## 3. Repeat

### 3.1 The modes and the default

- `enum class PlaybackController::Repeat : uint8_t { Off, All, One };`.
  The menu cycles Off, All, One, Off.
- **The saved default is Off** (a unit with nothing saved). This changes
  what an updated unit does at the end of its queue: it stops there, where
  it used to wrap.
- **PlaybackController's own default stays All**: the host tests that
  assume wrapping keep their meaning. main sets the saved mode at boot,
  before the queue is restored and before the first play (3.7).

### 3.2 Two successors: a natural end and a skip

The rule: **a natural end follows the mode; a skip wraps unless the mode
is Off.**

| Where | Kind | Off | All | One |
|---|---|---|---|---|
| `checkEnd()`: finished, `advance(true)` (PC.cpp:422) | end | the next; at the last: stop, on it | the next, wrapping | this entry again |
| `syncHeard()`: expected, and the paused step (:630, :644) | end | the next | the next, wrapping | this entry |
| `refreshOffer()`: the word (:565) | end | the next; at the last, nothing | the next, wrapping | itself (the self-join) |
| `pauseAtBoundary()` (:394) | end | the next cued; at the last: stop | the next cued (the first after the last) | this entry cued at 0:00 |
| `next()`: `advance(false)` (:120) | skip | the next; at the last: stop | wrap | wrap |
| `checkEnd()`: failed, `advance(false)` (:412) | skip | as next() | wrap | wrap: a failure moves on |
| `prev()` (:165) | skip | at the first: the first again | to the last | to the last |
| `cue()` (:215) | skip | stays at the ends | wrap | wrap |

In PC:

- `bool wraps() const { return repeat_ != Repeat::Off; }`
- `uint32_t endNext() const`: One, `queue_.peek(0, false)` (the current
  position); otherwise `queue_.peek(+1, wraps())`.
- `bool stepAtEnd()`: One, true without moving (and `++repeats_`);
  otherwise `queue_.step(+1, wraps())`.
- `advance(bool atEnd)`: `atEnd ? stepAtEnd() : queue_.step(+1,
  wraps())`, then `startCurrent()`, or `stop()` when it couldn't move.
- `prev()` and `cue()`: `queue_.step(delta, wraps())`.
- `pauseAtBoundary()`: One, no step (`audio_.stop()`, Paused, cued,
  marked the timer's); otherwise as now with `step(+1, wraps())`.
- `refreshOffer()`: `pos = endNext()`; `syncHeard()`: `expected =
  endNext()`, and its paused branch `stepAtEnd()`. In `syncHeard()`'s
  adopt branch, an adoption of the current position itself (One) counts
  `++repeats_` too.
- `Signature::repeat` becomes the enum.
- `setRepeat(Repeat)` becomes an action (`Act`), so the word changes at
  once, not at the next `update()`. `setRepeat(bool)` goes (the tests'
  `false` is Off, `true` All).

### 3.3 All

The wrap is a join like any: the first entry's word goes as soon as the
last entry is current. Prev at the first entry goes to the last (at 3 s or
less; past 3 s it restarts the track, `prevRule()` unchanged).

### 3.4 One: the gapless self-join

At the natural end the word is this same entry; each loop gets a new
token, by the existing "never the heard token" rule. It is safe:

1. The player already does this for a queue of one on repeat, tested at
   both levels (section 1).
2. The engine closes the decoder before it probes the next file, and the
   backend starts it through the same `file_` object, its seek index in
   the other slot: no file is opened twice.
3. In `syncHeard()`, `expected` is the current position and the key
   matches: `setCurrent()` to the same position bumps nothing
   (QM.cpp:218), so no position version, no saver write. `heardToken_`
   moves, so the next word has a new token.

What doesn't happen, rightly: the key doesn't change, so main's `[queue]
now at` line and `danceMode.onTrackChanged()` don't fire (the same song:
its tempo prior is worth keeping); `EntryStart` stays started; the length
is learned once. main logs each loop from `repeats()` instead (section 6).

- Gapless off (`G0`): One plays the track again from 0:00, a start like
  any (the ring cut, the fade-in).
- A track that fails moves on (a skip), so a failing track is never tried
  again in place, and "every track failed in a row" still stops it
  (PC.cpp:417).
- A file that "finishes" with no frames and no failure would replay back
  to back. A queue of one on repeat has the same exposure today, and the
  backend fails a track refused before its first frame
  (Core2AudioBackend.cpp:1518-1524). Not guarded; section 12.

### 3.5 The sleep timer wins

- **End of track with One.** "Pause after this track" is set, and the gate
  (`NextGate::endsHere()`) keeps the self-join from being decoded ahead
  (no word). At the natural end `pauseAtBoundary()` cues **this** entry
  at 0:00: paused, the timer's, `timerStops() + 1`. A later Play starts
  the track from the top, which is what One would have done.
- **End of album and End of queue with One.** Nothing after this track
  would ever play, so this track is the boundary; otherwise the timer
  would never end. Both of main's "last" computations take it so, through
  one pure helper they share (they must agree):
  `static bool SleepTimer::lastOfQueue(int32_t current, uint32_t size,
  bool repeatOne)`: `current >= 0 && (repeatOne || current + 1 >= size)`.
  The album's end is that or `albumEndsBetween()`, as now. `canExtend()`
  follows (+10 min acts on such a track).
- **All and Off**: as now (ENERGY.md section 3): End of queue ends at the
  queue's last entry; with All it doesn't wrap first.
- **With shuffle**, End of album reads the next entry in play order: a
  shuffled library pauses at the end of most tracks, a shuffled album at
  the queue's end. That is the rule as it stands (the order that plays is
  the Queue tab's), written up in ENERGY.md and the README.
- The timed choices: as now.

### 3.6 Saved: NVS `queue`/`repeat`, schema 2

- A u8: 0 Off, 1 All, 2 One. Written at once on a change (a tap, the
  console: flash wear is no concern at that rate). Read with `isKey()`
  first (QueueStore's pattern: a missing key's read logs an `[E]` line).
  Absent: Off. Another value: Off, and logged.
- **Schema 2.** A new key changes the layout the schema names (the key
  list is per schema), so `kCurrent` goes to 2, with a `migrate(1, 2)`
  step that does nothing: an absent key already reads as Off. The key
  list becomes "schema 2: schema 1's, and `queue`/`repeat`".
- A downgrade to a firmware from before this: `[nvs] schema 2, newer than
  this firmware's 1 (a downgrade): left as it is`; it never reads the
  key, and its queue wraps, as it always did.
- `nvslayout::kRepeatKey = "repeat"` and `uint8_t repeatFrom(bool have,
  uint8_t stored)` (absent or unknown: 0), pure and host-tested.

### 3.7 Boot, waiting, empty

- **Boot**: `player.setRepeat(queueStore.loadRepeat())` after
  `nvsschema::check()`, before `queueStore.restore()` (main.cpp:2417)
  and the first play. Shuffle comes back with the file. The resume point
  belongs to the entry's key and line, so it is untouched (2.9).
- **Waiting**: the waiting entry stays current through either toggle (the
  same key), and `release()` starts it. Repeat applies at its end.
- **Empty or stopped**: both only set the mode (an empty shuffled queue's
  file still says so), and the menu shows it anywhere.

## 4. Now Playing: the code

The layout, the hit areas, the menus' texts and the haptics are
ARCHITECTURE.md's; this is how NPP and Ui get there.

### 4.1 The constants (NPP:39-65)

```cpp
constexpr int kCoverX = 12, kCoverY = 40, kCoverPx = 96;   // unchanged
constexpr int kTextX = 120, kColumnX = 112;               // unchanged
constexpr int kTitleY = 38, kTitleH = 52;                 // unchanged
constexpr int kArtistY = 90, kArtistH = 23;
constexpr int kAlbumY = 113, kAlbumH = 23;
constexpr int kProgressY = 146, kProgressH = 22;
constexpr int kTransportY = 168, kTransportH = 72;        // the touch
constexpr int kTransportDrawY = 176, kTransportDrawH = 56;  // the strip
constexpr int kZoneW = 64;
constexpr int kSeekReachPx = 8;                           // whether or not a play waits
constexpr int kReadoutY = 113, kReadoutH = 33;
```

static_asserts, beside the existing two on the line and the readout's
width: `kArtistY == kTitleY + kTitleH`; `kAlbumY == kArtistY + kArtistH`;
`kAlbumY + kAlbumH == kCoverY + kCoverPx` (the album row ends on the
cover's last row); `kReadoutY == kAlbumY`; `kReadoutY + kReadoutH ==
kProgressY`; `kProgressY + kProgressH == kTransportY`; `kTransportY +
kTransportH == kH`; `kTransportDrawH == gfx::kStripH`;
`kTransportDrawY - kTransportY == kTransportY + kTransportH -
(kTransportDrawY + kTransportDrawH)` (8 px each side);
`SeekBar::kBackAboveY == kReadoutY + 1`; `SeekBar::kOffAboveY ==
kReadoutY - 7`. The last two are today's relations (138 = 137 + 1, 130 =
137 - 7) and replace NPP:61's.

`lib/core/SeekBar.h:60`: `kOffAboveY = 106, kBackAboveY = 114`
(`kOffBelowY`, `kBackBelowY` stay 240 and 232). Off stays 42 px above
the line's top, as today (172 - 130 = 148 - 106). Its comment: "Past the
album band" becomes "onto the artist row or above".

### 4.2 Drawing

- **`repaint()`** fills what no piece covers. Today's fills stay (rows
  36-37; row 38 left of x 112; beside the frame, x 0-10 and 109-111, rows
  39-136); the one under the cover becomes x 0-111, y 137-145; and three
  are new: x 112-319, y 136-145 (the old album band's lower rows), and the
  transport's margins, y 168-175 and 232-239. Without them a chevron and
  old album pixels stay on screen. The nav-area fills (36-37, 38, beside
  the frame, and x 112-319 row 136) use `navBg()` (below), the rest BG.
- **`drawArtistAlbum()`**: one strip push of 208 x 46 at (112, 90): filled
  `navBg()`; the artist (Body, SOFT) at local y 11, x 8, 190 px wide; the
  album (Body, DIM, "(loose tracks)" when "") at local y 34. No chevrons,
  no per-row press. The "Open the Library" branch goes: with nothing
  queued the empty state is drawn instead.
- **`drawTitle()`**: on `navBg()`. While a play waits: the title on one
  line (Bold 16, TXT, local y 9, 190 px), "Waiting for SPYDRONE…"
  (Small, AMBER, local y 26, x `kWaitTextX`, `kWaitTextW`), and the
  status line (Small, DIM, local y 43): today's two texts, moved here
  from `drawWaiting()`.
- **`drawWaiting()`**: the buttons only, one 208 x 46 push at (112, 90):
  `fillRoundRect(kWaitSpeakerX, 4, kWaitSpeakerW, 36, 8)` and the same for
  Cancel (y 94-129), the labels (Body) centred at local y 22 (y 112).
- **`drawTransport()`**: the strip is 56 rows, pushed at (0, 176); local
  `cy = 28`. Volume: icon at `cy - 9`, "60%" at `cy + 14`. Prev, next at
  `cy`. Play: `drawPlayButton()` with r 25 (the spinner's ring, r 11,
  fits as it is). "...": `kMore` at `cy - 9` (always, so it doesn't jump
  when the indicator comes or goes), the indicator at `cy + 14`. Pressed:
  `fillCircle(cx, cy, 24, BTN_HI)`. `drawPlayZone()`: 64 x 56 at
  (128, 176).
- **The indicator** (`drawModes(c, cx, cy)`): the glyphs that apply, in
  order shuffle then repeat, centred as a group (4 px between two), in
  `accent::NowPlaying`, drawn as bitmaps (transparent over the pressed
  circle). Three new icons, 11 rows, 1 px strokes, in
  `tools/ui_icons.py` (then `IconData.cpp` regenerated, `Icons.h`
  declares them): `kShuffleSmall` (two crossing arrows, 14 x 11),
  `kRepeatSmall` (the loop, 14 x 11), `kRepeatOneSmall` (the same loop,
  pixel for pixel, and a bold "1" beside it, 20 x 11):

  ```
  shuffle_small     repeat_small      repeat_one_small
  ...........#..    .........#....    .........#..........
  ...........##.    .........##...    .........##.....##..
  ####.....#####    .###########..    .###########...###..
  ....#...#..##.    .#.......##...    .#.......##.....##..
  .....#.#...#..    .#.......#..#.    .#.......#..#...##..
  ......#.......    .#..........#.    .#..........#...##..
  .....#.#...#..    .#..#.......#.    .#..#.......#...##..
  ....#...#..##.    ...##.......#.    ...##.......#...##..
  ####.....#####    ..###########.    ..###########...##..
  ...........##.    ...##.........    ...##..........####.
  ...........#..    ....#.........    ....#...............
  ```

  kShuffle (20 x 16, Shuffle all's) is too heavy under the 18 x 4 dots.
  The "1" is a digit of its own, 2 px clear of the loop: a 2 px stem, 9
  rows (y 214-222, the loop's body and a row either side), a flag and a
  foot; the glyph's last column is blank, as the loop's first is, so the
  group centres on the dots. **The bounds**: every glyph 11 rows (y
  213-223, under the dots at y 193-196); the widest group, shuffle and
  One, 14 + 4 + 20 = 38 px at x 269-306, which is the pressed circle's
  width (r 24 at y 204) at the indicator's middle row, well inside the
  zone's x 256-319. `ui_icons.py` asserts all three: the 11 rows, the
  loop the same as `repeat_small`'s, the 38 px.
  - *As first built* the "1" was inside the loop, 1 px wide and 5 tall
    between the arrowheads (`.#.....#.##...` ...). On the device it
    didn't read (the check's 3x zoom, step 1). The fallback this section
    named (a Small "1" after the loop, 8 px more) became this drawn one:
    a bitmap like its siblings, bolder than the font's, and 6 px more.
- **`Drawn`** gains `bool shuffle` and `uint8_t repeat` (0xFF: none yet);
  `update()` draws the transport again when either changed (the
  condition at NPP:656).
- **`update()`**: the title strip is drawn whenever the middle is (a new
  track, the wait coming or going, `waitSig()` changing), since it now
  holds the wait's status.
- **`describe()`** (the console's `ui`) adds `; shuffle on, repeat one`.

### 4.3 The readout over the cover

The readout row (y 113-145) now covers the album row and the cover's
lowest 24 rows (113-136, its frame's last included).

- **Entering it** (`drawReadout()`, `!drawn_.scrubUp`): NPP:389's clear of
  "the album band's top rows" is empty now (`kReadoutY == kAlbumY`) and
  goes. While a play waits, x 112-319, y 90-112 is cleared instead: the
  buttons' upper halves (94-112) would stick out above the readout.
- **`endScrub()`**: fill x 0-319, y 113-145 with BG; push the cover's rows
  back from the sprite, which the readout never touched
  (`gfx::pushRows(*cover_, 11, 39, 98, 74, 98)`: y 113-136; no re-render,
  no thumbnail lookup; with no sprite, `drawCover()`'s CARD fill for
  those rows); then `drawMiddle()`.
- **The cover is never pushed over a scrub.** `update()` draws the
  readout (NPP:621) before `drawCover()` (NPP:632), and `thumbReady()`
  (NPP:130) can push a thumbnail that arrives mid-scrub. So
  `drawCover()` renders into `cover_` as always, but while
  `drawn_.scrubUp` pushes only its rows above the readout (y 39-112;
  sprite rows 0-73), and `endScrub()` pushes the rest.

### 4.4 Touch

- **Zones** (Pages.h:110): `Cover, Artist, Album` become one `Nav`: `None
  = -1, Nav, WaitSpeaker, WaitCancel, Bar, Volume, Prev, PlayPause, Next,
  More` (the transport still last, from `Volume`).
- **`zoneAt()`** (replacing NPP:682-703), in this order:
  1. `y >= kTransportY`: `More` on a clamped right-edge reading, else
     `Volume + clamp(x / 64, 0, 4)`.
  2. `y >= kProgressY - kSeekReachPx` (138): `Bar`. The waiting exception
     (NPP:692) goes: the waiting buttons end at y 137.
  3. `x < kColumnX`: `Nav` (the cover's column, waiting or not).
  4. Waiting: `y < kArtistY`: `None`; else `WaitCancel` on a clamped
     right-edge reading or `x - kColumnX >= kWaitCancelX - 3`, else
     `WaitSpeaker`.
  5. `Nav`.
- **The press look.** `uint16_t navBg() const`: ROW_SEL while `pressed_ ==
  Nav` and no play waits, else BG. `drawNav()`: the nav-area fills of
  `repaint()`, then `drawTitle()` and `drawMiddle()`; the cover isn't
  touched (no fill overlaps its frame). A Down on `Nav` calls it (not
  while waiting); the Tap, DragStart, Release or Cancel that ends the
  touch calls it again after `pressed_ = None`, before any sheet opens
  (the sheet leaves y 36-79 in view). A repaint mid-press keeps the look
  (`repaint()`, `drawTitle()` and `drawArtistAlbum()` all read
  `navBg()`).
- **`onEvent()`**, a Tap: `Nav` opens the navigation menu
  (`openNavMenu()`), `More` the playback menu (`openPlaybackMenu()`); the
  rest as now. The tick at NPP:847 stays the one tick.
- **`openNavMenu()`**: the playing track id is `navTrack_`. A built-in
  track: the toast "A built-in track isn't in the Library"; no index, not
  ready, or the id out of range: "The Library isn't ready yet"; a
  synthetic browse: today's toast (NPP:717). Each logs its own line
  (section 6), and no sheet. Otherwise the sheet: the title
  (`catalog().title()`), the rows `uitext::kGoTo`, the details: the
  artist or `kNoArtistFolder`, the album or `kLooseTracks`, and
  `navFolder_` (96 B, replacing `moreFolder_`): `folderPath()` into a
  256 B buffer, then `textfit::cutPathLeft()` (Small) to
  `sheet::detailRoom(width of "Go to folder")`; "" if `folderPath()`
  failed. `ask_ = Ask::Nav`.
- **`openPlaybackMenu()`**: the title `kPlaybackTitle`, the rows
  `kShuffleRow`, `kRepeatRow`, `kSleepRow`, the details from the state
  (`kOnOff[s.shuffle]`, `kRepeatModes[s.repeat]`, `s.sleepRow`); then
  `ui_.sheetFollows(0, SheetFollow::Shuffle)`, `(1, Repeat)`, `(2,
  Sleep)` and `ui_.sheetStays(0)`, `ui_.sheetStays(1)`. `ask_ =
  Ask::Playback`.
- **`onSheet(choice)`**: -1 (✕, outside, a modal closing it): `ask_ =
  None`. Nav: `ask_ = None`, then `goToLibrary(Go::Artist / Album /
  Folders, navTrack_)` for 0 / 1 / 2. Playback: 0, `host().setShuffle(!
  s.shuffle)`; 1, `host().setRepeat((s.repeat + 1) % 3)` (both stay up,
  `ask_` stays); 2, `ask_ = None` and `openSleepSheet()`.
- **`goToLibrary(Go, uint32_t track)`** (NPP:705): the track it's given,
  not the playing one, with the same checks (the index may have gone
  while the sheet was up; then the toast). The Folders log line becomes
  `go to the folder (3 deep)`.

### 4.5 The sheets: Sheet and Ui

- **`sheet::detailRoom(int labelW)`** in SheetLayout.h (with `kScreenW =
  320`): `kScreenW - 16 - (16 + labelW + 16)`, what `Sheet::render()`
  gives a detail (Overlays.cpp:311), which now calls it. The page cuts
  the folder to it, and the tests measure against it.
- **`Sheet::kSettleMs = 300`**, and `bool Sheet::setDetail()` returns
  whether it drew. **`Sheet::drawRow(int i)`**: render and push row i
  (the Tap path clears `pressed_` without drawing).
- **Ui** (Ui.h, Ui.cpp):
  - `enum class SheetFollow : uint8_t { None, Sleep, Shuffle, Repeat };`
    `void sheetFollows(int row, SheetFollow what);` replaces
    `sheetFollowsSleep(int)` (its one caller was NPP:894).
    `SheetFollow sheetFollow_[sheet::kMaxRows]` replaces
    `sheetSleepRow_`.
  - `void sheetStays(int row);`: a tap on that row doesn't close the
    sheet. `uint8_t sheetStays_` (a mask).
  - `openSheet()` clears both and notes `sheetOpenedMs_ = nowMs_`.
  - `followSheet(int pressed = -1)`: each followed row's detail from
    `state_` (`sleepRow`; `kOnOff[shuffle]`; `kRepeatModes[repeat]`) via
    `setDetail()`; the row `pressed` is drawn with `drawRow()` if
    `setDetail()` didn't draw it. `update()` calls it each pass, in place
    of Ui.cpp:959.
  - **The settle**: in `route()`, a Down while the sheet is up and `e.ms -
    sheetOpenedMs_ < Sheet::kSettleMs` sets `touch_ = TouchOn::None`: the
    whole touch goes nowhere. For every sheet. (The fade toast's buttons
    are routed before the sheet, Ui.cpp:1117, so they never settle.)
  - **The staying row** (`route()`, `TouchOn::Sheet`): `r >= 0` and in
    `sheetStays_`: `tick()`, `owner->onSheet(r)`,
    `host_.snapshot(state_)` (as `retryCard()` does), `followSheet(r)`.
    The sheet stays. Every other answer as now.
  - **The undo a toggle took**: `update()` keeps `lastShuffle_`. When
    `state_.shuffle` differs: if the toast offers Undo and
    `queue_.undoable()` is None, the toast goes (`hide()`, `uncover()`);
    and that pass doesn't flash the Queue badge (Ui.cpp:988: Off can
    grow "up next" without adding anything). Shuffle all's toast stays:
    its Replace is undoable, the mode with it (2.6).
    *As built after the review (review fixes 2)*: `lastShuffle_` became
    `queueview::UndoWatch undoWatch_` (QueueView.h, host-tested), which
    also hears the console's `qu`: main's `u` calls
    `Ui::queueUndone()` when it undid something. Each pass,
    `pass(state_.shuffle, toast up with Undo, queue_.undoable())` says
    whether the toast goes and why: `Undone` (a `qu` since the last
    pass) before `Toggle` (the mode changed since the last pass), and
    `modeChanged()` keeps the badge quiet as before. Without it, `qu`
    on Shuffle all's toast (the mode back off, the undo gone) read as a
    toggle and logged `a shuffle toggle took the undo`; and `qu` on any
    other edit's toast (no change of mode) left it up, its Undo then
    answering "Nothing to undo".
- **`UiHost`**: `virtual void setShuffle(bool on) = 0;` and `virtual void
  setRepeat(uint8_t mode) = 0;` (`PlaybackController::Repeat`'s value).
  `AppState`: `bool shuffle = false; uint8_t repeat = 0;`, filled by
  `snapshot()` from the player.
- **main's `MainUiHost`** implements them with the console's helpers
  (section 6): apply, save (repeat: NVS; shuffle: the queue file follows
  by itself), log.

### 4.6 The texts (`lib/core/UiText.h`)

```cpp
inline constexpr const char* kGoTo[3] = {"Go to artist", "Go to album", "Go to folder"};
inline constexpr const char* kNoArtistFolder = "(no artist folder)";
inline constexpr const char* kLooseTracks = "(loose tracks)";
inline constexpr const char* kBuiltinNotInLibrary = "A built-in track isn't in the Library";
inline constexpr const char* kLibraryNotReady = "The Library isn't ready yet";
inline constexpr const char* kPlaybackTitle = "Playback";
inline constexpr const char* kShuffleRow = "Shuffle";
inline constexpr const char* kRepeatRow = "Repeat";
inline constexpr const char* kOnOff[2] = {"Off", "On"};
inline constexpr const char* kRepeatModes[3] = {"Off", "All", "One"};
```

Measured with the firmware's VLW data: the labels (Body) 89, 98, 95, 56,
57 and 92 px, all in a row's 288; the detail rooms 183, 174, 177, 216,
215 and 180; the states (Small) at most 26 ("One"); "Playback" (Small)
59 in the title's 226; the toasts (Body) 264 and 209 in a one-line
toast's 284; "…/Daft Punk/Discovery" (Small) 150 in 177, the whole
"/music/Daft Punk/Discovery" 180 (so it is cut). The waiting panel's
comment (UiText.h:119) moves to "the title strip and the rows under it
(y 38-137)".

## 5. The APIs

```cpp
// lib/core/Shuffle.h (new)
namespace shuffle {
// Fisher-Yates over a[0..n), xorshift32 from `seed` (0 is taken as 1),
// j by multiply-shift: queueview::shuffle()'s loop, for any element type.
template <typename T> void permute(T* a, uint32_t n, uint32_t seed);
}

// QueueModel
using RandomFn = uint32_t (*)();
explicit QueueModel(AllocFn alloc = nullptr, FreeFn release = nullptr, RandomFn random = nullptr);
static constexpr uint32_t kAnyStart = kNone;  // replace(): shuffled, a random first; else the first
bool shuffled() const;
uint32_t rankAt(uint32_t pos) const;          // shuffled: the entry's rank; else pos
bool setShuffled(bool on);                    // false: it already was; allocates nothing; drops the undo
bool assign(const uint32_t* tracks, uint32_t n, int32_t current, bool shuffled = false,
            const uint32_t* ranks = nullptr);  // ranks nullptr: the positions
// Play in a mode, one edit: the snapshot (in the mode before), then the mode,
// then the queue laid out in it; undo() puts both back (2.6). n 0: a Clear in
// that mode. The three-argument replace() keeps the queue's mode.
bool replace(const uint32_t* tracks, uint32_t n, uint32_t start, bool shuffled);
bool undoShuffled() const;                    // the mode undo() would put back

// PlaybackController
enum class Repeat : uint8_t { Off, All, One };
static constexpr uint32_t kAnyStart = QueueModel::kAnyStart;  // playNow()
bool playNow(const uint32_t* tracks, uint32_t n, uint32_t start, bool shuffle);  // Shuffle all's
void setRepeat(Repeat r);   // an action: the word changes at once
Repeat repeat() const;
void setShuffle(bool on);   // an action: the same entry, in the same state
bool shuffle() const { return queue_.shuffled(); }
uint32_t repeats() const;   // Repeat One's loops, free-running
// private: wraps(), endNext(), stepAtEnd(), advance(bool atEnd); uint32_t repeats_

// queuetext: Header::shuffled; version 2 written iff q.shuffled(); read() takes 1 and 2.

// nvslayout
inline constexpr uint16_t kCurrent = 2;
inline constexpr const char* kRepeatKey = "repeat";   // namespace "queue"
uint8_t repeatFrom(bool have, uint8_t stored);        // absent or unknown: 0 (Off)

// QueueStore
PlaybackController::Repeat loadRepeat();   // setup, before restore(); logs
void saveRepeat(PlaybackController::Repeat r);

// SleepTimer
static bool lastOfQueue(int32_t current, uint32_t size, bool repeatOne);

// textfit
// `path` cut from the left by whole folders to fit `maxW` ("…/Daft
// Punk/Discovery"), into out: Ui::drawHeader()'s loop (Ui.cpp:1430),
// which then calls it. If even "…/<last>" is too wide, "/<last>" (the
// draw cuts its end), as the header does now.
void cutPathLeft(const Font& f, const char* path, int maxW, char* out, size_t size);

// SheetLayout.h
inline constexpr int kScreenW = 320;
constexpr int detailRoom(int labelW);

// ui::Sheet: kSettleMs; bool setDetail(int, const char*); void drawRow(int)
// ui::Ui: SheetFollow, sheetFollows(), sheetStays(); sheetFollowsSleep() goes
// ui::UiHost: setShuffle(bool), setRepeat(uint8_t); AppState: shuffle, repeat
```

## 6. Logs and the console

Now Playing (`[ui] now playing: ...`):

- `the navigation menu` (the sheet opened);
  `no navigation menu (a built-in track)` / `(the Library isn't ready)`
  / `(a synthetic library)`, each followed by the toast's own line;
- `go to the artist 12`, `go to the album 40` (as now), `go to the
  folder (3 deep)` (was `show in folders`);
- `the playback menu (shuffle off, repeat all, sleep timer Off)`;
- `shuffle on` / `shuffle off`, `repeat off` / `repeat all` /
  `repeat one` (at the tap; the player's lines follow).

`[ui] shuffle all: 77 tracks (shuffle on; was off)` (`was on` when it
already was). It was `[ui] shuffle all: 77 tracks`, then, as first built,
`(shuffle on)` after a `[player] shuffle on: ...` line from the toggle;
there is no toggle now (the mode is part of the Play, 2.6), so no
`[player]` line. Its Undo: `[ui] undo: done (shuffle off again)` (the
console's `qu`: `[queue] undo: done (shuffle off again)`); an undo that
leaves the mode as it is adds nothing.

An Undo toast taken away because its undo went (4.5): `[ui] the Undo
toast went: a shuffle toggle took the undo` (a toggle while it was up),
`[ui] the Undo toast went: undone from the console` (a `qu` that undid
something while it was up, Shuffle all's included: its change of mode
back is no toggle).

The player (main's helpers, shared by the host and the console):

- `[player] shuffle on: 37 up next shuffled; 3 of 40 plays on` (nothing up
  next: `[player] shuffle on: nothing up next to shuffle`);
- `[player] shuffle off: the queue's own order again, now 12 of 40 (28 up
  next), in 23 ms`;
- `[player] repeat: off (the queue stops after its last track)`, `all (the
  queue starts again after its last track)`, `one (this track again at its
  end; next and prev still move)`;
- `[queue] repeat one: 5 of 40 again (playing)`: main, each time
  `repeats()` moves;
- `[player] shuffle on: already` / `[player] repeat: all already` for a
  console command that changes nothing.

Boot and status:

- `[nvs] schema 1 -> 2: migrated` (once);
- `[queue] repeat: one (saved)`, `[queue] repeat: off (none saved)`,
  `[queue] repeat: off (an unknown 7 saved)`;
- `[queue] restored 40 of 40 tracks from ... at 3 of 40 (position from
  NVS), shuffled` (`, shuffled` only then);
- `q`: `[queue] 40 tracks, at 3, 37 up next; shuffle on, repeat all; undo:
  none; file generation ...`; after a Shuffle all that turned shuffle on,
  `undo: play now (and shuffle off)`;
- `G`: `..., paused at the boundary 0, repeat-one loops 3`;
- `ui`: Now Playing's line ends `; shuffle on, repeat one`.

The console, under `q` (letters free today; the console is
case-sensitive, and `G0`, `Rt` are precedents):

- `qS` toggles shuffle, `qS0` / `qS1` set it;
- `qR` steps repeat (Off, All, One), `qR0` / `qR1` / `qR2` set it;
- each through the same helper as the menu, so it is logged and saved,
  and the open menu follows it. The usage line (main.cpp:907) gains
  them.

## 7. Files and functions

- **lib/core/Shuffle.h** (new): `shuffle::permute()`.
- **lib/core/QueueModel.h/.cpp**: `Entry::rank`; `RandomFn` and its
  member; `shuffled_`; `kAnyStart`; `setShuffled()`, `rankAt()`,
  `assign(.., shuffled, ranks)`; `replace()`, `insertAt()`
  (`insertNext()`, `append()`), `moveNext()` keep ranks while shuffled
  (2.4); a private `draw(uint32_t bound)`; the header's comment (memory:
  12 B an entry; the shuffle rules).
- **lib/core/QueueView.h/.cpp**: `shuffle()` goes. (Review fixes 2:
  `UndoWatch`, 4.5.)
- **lib/core/QueueText.h/.cpp**: `Header::shuffled`; version 2 (2.9);
  the buffers +11; the comment.
- **lib/core/PlaybackController.h/.cpp**: `Repeat`, `setRepeat()` as an
  action, `repeat()`, `setShuffle()`, `shuffle()`, `repeats()`,
  `kAnyStart`; `wraps()`, `endNext()`, `stepAtEnd()`, `advance(bool)`;
  every call site of section 3.2; `Signature::repeat`; the class comment
  (PC.h:30-32: the wrap; :56-62: "pause after this track" with One).
- **lib/core/SleepTimer.h/.cpp**: `lastOfQueue()`.
- **lib/core/NvsLayout.h/.cpp**: `kCurrent = 2`, `kRepeatKey`,
  `repeatFrom()`; the comment (schema 2).
- **lib/core/SeekBar.h**: 106 / 114, the comment.
- **lib/core/SheetLayout.h**: `kScreenW`, `detailRoom()`; the comment (no
  4-row sheet any more; `kMaxRows` stays 4: the panel and test_ui_nav).
- **lib/core/TextFit.h/.cpp**: `cutPathLeft()`.
- **lib/core/UiText.h**: section 4.6.
- **src/app/NvsSchema.cpp**: `migrate()`'s comment: the step 1 to 2 is
  nothing.
- **src/app/QueueStore.h/.cpp**: `loadRepeat()`, `saveRepeat()`;
  `restore()`'s line; `printStatus()`; `remap()`'s clears keep the mode.
- **src/main.cpp**: `QueueModel queue(psramAlloc, psramFree,
  esp_random)`; setup's `setRepeat(loadRepeat())` before `restore()`;
  `applyShuffle(bool)` and `applyRepeat(Repeat)` (apply, save, log: the
  lines of section 6); `MainUiHost::setShuffle()`, `setRepeat()`,
  `snapshot()`'s two fields; `queueCommand()`'s `S` and `R` (and `u`'s
  `queueUndone()`, review fixes 2);
  `sleepEndsAtCurrent()` and `stepSleep()` through `lastOfQueue()`; the
  repeat-one line from `repeats()`; `G`'s count.
- **src/ui/UiHost.h**: section 4.5.
- **src/ui/Overlays.h/.cpp**: `Sheet::kSettleMs`, `setDetail()`'s bool,
  `drawRow()`, `render()` through `detailRoom()`; the comments (Overlays.h:
  38-44, 69-70, 159-160: no 4-row sheet now).
- **src/ui/Ui.h/.cpp**: `SheetFollow`, `sheetFollows()`, `sheetStays()`,
  `followSheet()`, the settle, the staying row, `lastShuffle_` (as
  built: `undoWatch_` and `queueUndone()`, 4.5);
  `shuffleAll()` (2.5); `drawHeader()` through `cutPathLeft()`; the
  comments that name the 4-row sheet (Ui.cpp:471, 552).
- **src/ui/Pages.h**: the layout comment (NPP's picture: the table of the
  ARCHITECTURE spec, the zones, the two menus, the indicator, the waiting
  panel); `Zone`; `Ask ask_`; `navTrack_`; `navFolder_`; `navBg()`,
  `drawNav()`, `drawModes()`, `openNavMenu()`, `openPlaybackMenu()`,
  `goToLibrary(Go, uint32_t)`.
- **src/ui/NowPlayingPage.cpp**: sections 4.1-4.4; the file's comment.
- **src/ui/LibraryPage.cpp:688**: `kAnyStart`.
- **tools/ui_icons.py**, **src/ui/IconData.cpp**, **src/ui/Icons.h**: the
  three icons.

The guards: iram_diet, cache_guard (the MP3 hot-set pin), flash_guard,
version. Nothing here is IRAM code, and nothing gets `IRAM_ATTR`.

## 8. Host tests (`pio test -e native`, from Git Bash)

**test_queue** (QueueModel):

- `test_shuffle_on_keeps_what_played_and_what_plays`: positions 0 .. cur
  and their keys unchanged; up next a permutation of the same entries;
  both versions bumped; `undoable()` None.
- `test_shuffle_off_brings_the_own_order_back`: on then off is the
  identity; on, two steps, off: the current entry at its own place, keys
  kept.
- `test_shuffle_with_nothing_up_next`: empty, 0 and 1 up next: the mode
  flips, the content version bumps, nothing moves.
- `test_shuffled_adds_keep_their_place`: insertNext and append while
  shuffled, then off: Play next's right after the current entry, + Queue's
  at the end, each in the given order.
- `test_shuffled_move_remove_and_clear_up_next`: moveNext, remove (the
  current one too), clearUpNext, each then off.
- `test_shuffled_play_puts_the_chosen_track_first`: start s, then off: s
  current at s, the given order; `kAnyStart` shuffled (a random first)
  and not (the first, not the last).
- `test_an_add_to_an_empty_shuffled_queue`.
- `test_undo_while_shuffled_restores_the_ranks`; `test_a_toggle_drops_the_undo`.
- `test_a_play_that_sets_the_mode_undoes_it_too` (after the device
  check): Shuffle all's `replace(.., true)` from an empty queue, off (the
  undo leaves it empty and off), over a queue mid-way (its own order, its
  current entry, ranks the positions, an add after it not shuffled in),
  already on (stays on, its ranks), the other way (`false` over a
  shuffled queue), a toggle after it (drops that undo), out of memory
  (neither changes), and `n` 0 (a Clear in that mode).
- `test_shuffle_is_repeatable_and_uniform`: the same hook sequence, the
  same order; 5 up next x 20,000 shuffles: each entry in each slot within
  ±3 % of 1/5; the current entry never moves.
- `test_a_toggle_allocates_nothing`: 10,000 entries, a counting alloc hook,
  and a counting global operator new (the hooks can't see a
  `std::stable_sort`'s buffer or a scratch `std::vector`; the test first
  checks that the count sees a stable sort's).
- `test_assign_with_ranks`; `test_a_rank_overflow_is_refused`.
- `test_permute_is_a_permutation_and_repeatable`: test_ui_queue's
  `test_shuffle_is_a_permutation_and_repeatable` moved, on
  `shuffle::permute()`, with its numbers (same seed, same order).

**test_queue** (QueueText, QueueSaver):

- `test_v1_unchanged_while_not_shuffled`: byte for byte.
- `test_v2_round_trip`: order, current, ranks, mode; off after the read
  gives the own order.
- `test_v2_dropped_tracks_leave_rank_gaps`.
- `test_v2_bad_lines_leave_the_queue_alone`: no rank, no space, a rank
  over 2^32 - 1.
- `test_v2_long_path_and_ten_digit_rank`: a 256-byte path survives.
- `test_v2_empty_shuffled_queue`.
- `test_a_toggle_rewrites_the_file` (2 s later) and
  `test_a_toggle_during_a_write_restarts_it`.
- `test_shuffle_alls_undo_writes_version_1_again`: version 2 after Shuffle
  all, version 1 (the old queue whole) after its Undo.
- `test_off_while_paused_pairs_the_resume_point_again`.

**test_playback**:

- `test_repeat_off_stops_at_the_end` (test_without_repeat_..., :517, on
  the enum); `test_repeat_all_wraps` (test_next_and_prev_wrap, :172).
- `test_repeat_one_plays_the_entry_again`: gapless off, `plays + 1`, the
  same key; `repeats()` counts it.
- `test_repeat_one_next_and_prev_move_and_wrap`.
- `test_repeat_one_moves_on_from_a_failure`; every track failed, with One,
  still stops.
- `test_an_end_at_0_00_is_a_failure`: a natural end with the position
  still at 0:00 (an empty track) is a failure, "no audio in it": One
  moves on; alone in the queue it stops.
- `test_repeat_one_with_pause_after_this_track`: the same entry cued at
  0:00, `pausedByTimer()`, `timerStops() + 1`.
- `test_a_start_point_on_a_cued_entry_keeps_its_told_length` (after the
  device check, step 14's note): `setStartPoint(60000, 0)` on that cued
  entry keeps the length told for it, so the bar stays live; an entry
  with none told keeps the catalog's hint. `test_a_start_point_keeps_its_length`
  follows: stopped, nothing held, the told length before the catalog's.
- `test_the_word_for_each_repeat_mode` (the block at :1375-1416): at the
  last entry Off nothing, All the first entry, One itself with a new
  token each loop.
- `test_set_repeat_is_an_action`: the word changes before any `update()`.
- `test_set_shuffle_changes_nothing_that_plays`: Playing, Paused, Waiting,
  with a start point: the state, the key, `plays`, the start point and the
  length hint unchanged; the word moves to the new next.
- `test_play_now_while_shuffled`: `kAnyStart`, and a start.
- `test_shuffle_all_and_its_undo` (after the device check):
  `playNow(.., kAnyStart, true)` over a queue that plays (the Undo: shuffle
  off, the old current entry playing, the word its own next), from an
  empty queue (empty, stopped, off) and already shuffled (stays on).
- The 21 `setRepeat(bool)` calls become the enum (`false` Off, `true`
  All).

**test_gapless_player**:

- `test_repeat_one_loops_inside_a_queue`: a, b, c; One on b gives b, b, b,
  then All gives c, as one stream; `adopted` counts them.
- `test_repeat_all_wraps_gaplessly`: the last into the first.
- `test_repeat_one_turned_off_while_decoded_ahead`: cut, and c follows.
- `test_shuffle_toggled_while_the_next_is_decoded_ahead`: before J a cut;
  after J the advance starts what now comes next.
- `test_end_of_track_with_repeat_one_never_decodes_itself_ahead`: the
  pause at the boundary, the same entry cued, nothing of it heard.

**test_sleep_timer**: `test_last_of_queue_with_repeat_one` (One: last of
the queue, so of the album; Off and All as before).

**test_nvs_layout**: `kCurrent` 2 (:35, :175); `repeatFrom()`: absent Off,
3 and 255 Off, 0-2 themselves; the key 15 characters or fewer.

**test_ui_nav**: `sheet::detailRoom()` against `Sheet::render()`'s
formula; `top(3) == 80`.

**test_ui_library**: the texts of 4.6 in their rooms (the labels in 288,
each state in its row's `detailRoom()`, the sleep states as now against
the Sleep timer row's, the toasts in 284, "Playback" in 226);
`cutPathLeft()` on the VLW Small font ("/music/Daft Punk/Discovery" to
177 is "…/Daft Punk/Discovery"; a path that fits stays whole; a last
folder too wide alone); the waiting title's one line (Bold 16, 190).

**test_seek_bar**: 129/130 become 105/106 and 137/138 become 113/114
(:287-301, :344-356), the line's row `kY` 150; the comments.

**test_ui_queue**: the shuffle test leaves (moved to test_queue). After
the review (review fixes 2), `UndoWatch`:
`test_undo_watch_a_toggle_takes_the_undo` (Play next's toast, then a
toggle: `Toggle`; a toggle with no Undo toast up: nothing),
`test_undo_watch_qu_of_shuffle_all_is_no_toggle` (the review's case:
from empty and off, Shuffle all's toast stays, then `qu`: `Undone`, not
`Toggle`), `test_undo_watch_qu_of_any_edit` (`qu` of Play next: `Undone`;
`qu` with no toast up is told once; an undo still there keeps its
toast).

## 9. Docs to update with the code

- **ARCHITECTURE.md**: Now Playing is done (this design). With the code:
  the queue bullet (12 B an entry, ranks, the shuffle rules) and the
  transport bullet (Off / All / One, the two successors) under "Library
  and queue"; persistence (`queue.txt` version 2, the downgrade); the
  NVS rules' schema text and key list (schema 2, `queue`/`repeat`); the
  console's `qS` and `qR`; the empty states' Shuffle all (the Queue
  bullet names `queueview::shuffle()`: shuffle on now); the overlays
  bullet's sheets ("the 4-row one from y 40" goes; every sheet's 300 ms
  settle comes in).
- **SEEK-BAR.md**: section 1 (the band y 146-167, the album row above);
  2.6 (off above 106, back from 114); 2.8 (`kOffAboveY`, `kBackAboveY`;
  the reach from y 138 with no waiting exception; `kReadoutY` 113);
  section 3's diagram (the navigation area above, the waiting buttons
  90-137); 4.1 (y 146-167); 4.2 (the readout row y 113-145 over the album
  row and the cover's lowest rows; entering and leaving as 4.3 here);
  section 11's coordinates (the line at y ~150); a dated note at the top
  that the layout moved.
- **GAPLESS.md**: 3.1's "There is no shuffle mode today" paragraph (now:
  what exists, and that it needed nothing of the engine); `peek(+1,
  repeat_)` becomes `endNext()` where it is quoted; section 3.5's table:
  the Repeat row (with its UI now, and One's self-join) and a row for a
  shuffle toggle.
- **ENERGY.md**: :703-704 and :1364 (the Sleep timer row is the
  "Playback" sheet's last, 3 rows from y 80), :1519 (no 4-row sheet), and
  section 3's boundaries (One: this track is the boundary for End of
  track, album and queue; with shuffle, End of album reads the play
  order).
- **README.md**: the Now Playing bullet (:316-324): tap the cover or the
  text for Go to artist / album / folder; "..." for Shuffle, Repeat and
  the Sleep timer; the indicator. The queue paragraph (:418-427): shuffle
  and repeat are kept across a restart; repeat is off by default (the
  queue stops at its end); Shuffle all turns shuffle on. The console
  table (:620): `qS`, `qR`.

## 10. Device check

Run once, at 065ec1f (2026-10-06). It found two faults, fixed since
(section 14's last entry): Shuffle all's Undo left shuffle on (step 9),
and Repeat One's "1" didn't read (step 1). It also found two of the
expectations below wrong, corrected here (steps 11 and 14). Steps 1, 9a
and 14 check the fixes. Every step but 11's listening, 14's slow scrub
and 17 is scripted, in silent mode.

**The build.** From PowerShell, with MSYSTEM removed: `Remove-Item
Env:MSYSTEM -ErrorAction SilentlyContinue; pio run -e core2`, then `pio
run -e core2-dio`. Every guard passes (iram_diet, cache_guard,
flash_guard, version). Never with the host tests running. Flash core2
(`-t upload --upload-port COM3`, the serial daemon stopped first).

**The setup.** COM3 through the serial daemon; silent mode `z`; Now
Playing up (`ui0`). First note the user's state: `q` (its line ends
`shuffle off, repeat off` on a fresh flash) and `l` (the queue, `*` at
the current entry); put both back at the end (`qS0` / `qR<n>`, and the
queue as SEEK-BAR.md section 11 says). A script sends nothing but
commands: a stray byte is a key, and never a `-` or an `f` (`f` forgets
the headphones). The reset is `!reset` written raw into the daemon's
command file.

**The scripted finger** is screen pixels and one touch at a time: a
`uit` lands for 60 ms, and a second `uit` sent before the first has
lifted replaces it. So send each tap after the line the one before
logs, and wait 400 ms after a sheet opens before tapping it (the 300 ms
settle swallows anything sooner: step 7 tests exactly that). The
places: the cover x 12-107, y 40-135; the title y 38-89, the rows y 101
and 124 (x 120-310); the line y 150; the transport y 204 (volume x 32,
prev 96, play 160, next 224, "..." 288); a 3-row sheet from y 80, its
✕ at (290, 98) and its rows centred at y 136, 176 and 216; a tap above
it (y 60) closes it. `ui` prints the overlays (`sheet open`, `sleep
timer sheet open`, `volume open (speaker)`, the toast's text) and Now
Playing's line (it ends `; shuffle on, repeat one`).

1. **The look.** `qS0`, `qR0`, then `X` (the whole screen) while
   playing: the rows without "›", the background y 136-145, the band at
   y 146-167, the transport drawn at y 176-231 with background above and
   below, the play disc (r 25) clear of the bezel, no indicator. `qS1`,
   `X`: the shuffle glyph under the dots, at (288, 218). `qR1`, `X`: both
   glyphs, 4 px apart. `qS0`, `qR2`, `X`: the loop and its bold "1"
   beside it, alone (20 x 11 at x 278-297, y 213-223: the "1" a 2 px
   stem, y 214-222, read at arm's length). `qS1` (still `qR2`), `X`: the
   widest pair, 38 px at x 269-306, inside the dots' zone and clear of
   the next button. `qS0`, `qR0`. Each command logs its `[player]` line and
   `q`'s line (`shuffle on, repeat all`, ...); `ui` agrees.
   - *Found at 065ec1f*: the first Repeat One glyph's "1" was 1 px wide
     and 5 tall inside the loop, between its arrowheads: unreadable (the
     3x zoom). Redrawn (section 4.2).
2. **The navigation area.** Each of `uit60,80` (the cover), `uit200,50`
   (the title), `uit200,101` (the artist), `uit200,124` (the album),
   `uit5,137` (the corner margin), `uit318,95` (the right edge) logs
   `[ui] now playing: the navigation menu` (and a tap tick), each closed
   by `uit160,60` 400 ms later (`ui`: `sheet no`). Once, `X` with it open:
   the track's title (Small, dim), Go to artist / Go to album / Go to
   folder, each detail dim on the right, the folder cut from the left
   ("…/Album"); then `uit290,98` (its ✕) closes it.
3. **Not the menu.** `uit200,140`: a seek line (`[ui] now playing: seek
   ... (tap)`), no menu. `uit32,170` (y 170, the transport's touch above
   its drawing): the volume sheet (`ui`: `volume open (speaker)`); close
   it with `uit160,60`. `uit288,236`: the playback menu; close it.
4. **The rows.** `uit60,80`, 400 ms, `uit160,136`: `[ui] now playing: go
   to the artist N`, the Library on that artist, the playing album tinted
   (`X`). `ui0`; `uit60,80`, 400 ms, `uit160,176`: `go to the album N`
   (one Back from its artist). `ui0`; `uit60,80`, 400 ms, `uit160,216`:
   `go to the folder (N deep)`, the Folders segment, the file tinted.
   `ui0`.
5. **The refusals.** `qb` (a built-in tone plays, silent), `uit60,80`:
   `no navigation menu (a built-in track)` then `[ui] toast: A built-in
   track isn't in the Library`, and no sheet; at once `uit200,50`: the
   toast goes (`ui`: `toast none`), no menu line (a toast takes y 36-71
   first). Put a library queue back (`qp<n>`). `uil100`, then
   `uit60,80`: `(a synthetic library)` and its toast; `uil0`. The "isn't
   ready" case needs a tap during `g0`'s rebuild; it can't be scripted
   while the rebuild holds the loop, so its text is only host-tested.
6. **The playback menu.** `uit288,204`: `[ui] now playing: the playback
   menu (shuffle off, repeat off, sleep timer Off)`; `X`: "Playback",
   then Shuffle "Off", Repeat "Off", Sleep timer "Off". 400 ms later
   `uit160,136`: `[ui] now playing: shuffle on`, then `[player] shuffle
   on: ...`; `ui`: `sheet open`; `X`: "On" on the row, no highlight left.
   `uit160,176` three times (each after the last's `[player]` line):
   `repeat all`, `repeat one`, `repeat off`, the row following. With the
   sheet up, `qR` on the console: `[player] repeat: all ...`, and `X`
   shows "All" on the row. `uit160,216`: the sheet closes and the Sleep
   timer sheet opens (`[ui] sleep timer sheet (Off)`); close it with
   `uit160,50` 400 ms later. `qS0`, `qR0`.
7. **The settle.** `uit288,204`, then 150 ms after its line `uit160,176`:
   `[ui] sheet: a touch right after it opened, ignored`, no `repeat`
   line, the sheet still up and still "Off". Close it. The same with
   `uit200,124` then, 150 ms later, `uit160,136`: the ignored line, no
   `go to the artist`.
8. **Shuffle.** On a queue of 10 or more, mid-queue: `l`, `qS1`
   (`[player] shuffle on: N up next shuffled; c of n plays on`), `l`: the
   lines up to `*` unchanged, the rest reordered; `ui2`, `X`: the Queue
   tab in that order; `ui0`. `qn<a>`, `q+<b>` (two short albums from
   `ql`: Play next, + Queue), `l`: album a's tracks right after `*` in
   their order, album b's at the end in theirs. `qS0`: `[player] shuffle
   off: the queue's own order again, now c of n (u up next), in N ms`;
   `l`: the own order, album a after the playing entry, album b at the
   end. While playing, `qS1` prints no `[queue] now at` line and `s`'s
   pos runs on.
9. **Shuffle all.** `qS0`, `qx` (the empty state), `X`, then
   `uit246,189` (its Shuffle all button: x 184-307, y 172-205): `[ui]
   shuffle all: N tracks (shuffle on; was off)` (no `[player] shuffle
   on` line: the mode is part of the Play), the toast "Shuffling N
   tracks" with Undo, the indicator on, `l`: `*` at 0 on a random track,
   the rest shuffled. `qS0`: `l` is the library in its own order from the
   playing track.
   - *Found at 065ec1f*: an Undo of it (from this empty state, shuffle
     off) logged `[ui] undo: done` and emptied the queue, but `q` said
     `shuffle on` over 0 tracks, and the next album played shuffled.
     Fixed (section 2.6); step 9a checks it.
   - **9a. Shuffle all's Undo.** `qS0`, `qx`, `uit246,189` (Shuffle
     all): `q`: `shuffle on, ...; undo: play now (and shuffle off)`.
     Within the toast's 4 s, `uit290,54` (its Undo, x 250 to the edge,
     y 36-71): `[ui] undo: done (shuffle off again)`, the toast
     "Undone"; `q`: `0 tracks, at 0, 0 up next; shuffle off`; Now
     Playing's empty state. Then `qp<n>` (an album): `l` the album in
     order, `*` at 0, nothing shuffled. The same with shuffle on before
     (`qx`, `qS1`, Shuffle all): `(shuffle on; was on)`, `q`'s `undo:
     play now` with no `(and ...)`, the Undo's line `[ui] undo: done`
     alone, `q`: `shuffle on` over 0 tracks (as it was). The console's
     `qu` instead of the toast (shuffle off before, within the toast's
     4 s) gives `[queue] undo: done (shuffle off again)`, then `[ui] the
     Undo toast went: undone from the console` (`ui`: `toast none`); no
     `a shuffle toggle took the undo` line (the review found it there:
     the mode put back read as a toggle). `qS0` at the end.
10. **Kept across a restart.** `qS1`, `qR2`, pause (`uit160,204`), `qs60`;
    reset. The boot: `[nvs] schema 2` (on the first boot of this
    firmware: `[nvs] schema 1 -> 2: migrated`), `[queue] repeat: one
    (saved)` (first boot: `off (none saved)`), `[queue] restored ...
    (position from NVS), shuffled`, `[queue] resume point: 1:00 into
    ...`; `X`: the shuffle glyph and Repeat One's loop and "1"; `q`: `shuffle on,
    repeat one`. Then `qS0`, `qR0`, reset: `[queue] repeat: off (saved)`,
    no `, shuffled`.
11. **Repeat at the end** (a short album: `qp<n>`; on its last track,
    `uit300,150` seeks to its last 6 s). Off: the track ends, stopped on
    it (`[queue]` / `ui`: the same entry, stopped). `qR1`, the same: the
    last into the first with no request (`G`: joins taken +1; `[queue]
    now at 1 of M`). `qR2`, the same: `[queue] repeat one: M of M again
    (playing)` at each end and no `now at` line; `G`'s `repeat-one loops`
    counts them; next (`uit224,204`) moves on. **Listen at the seam** on
    an MP3 with a LAME tag and on a FLAC (the user's ears: no gap, no
    click). `G0`: One starts the track again from 0:00, faded in: `[queue]
    repeat one: M of M again (playing)`, then only the `[audio] refill:
    first audio in the ring ...` line (a start at 0:00 logs no `[audio]
    ... starting` line; only a start part of the way in does); `G1`.
    `qR0`.
    - *Corrected after 065ec1f*: this step expected an `[audio] ...
      starting` line on the `G0` restart; the device logged the refill
      line alone, which is right.
12. **The sleep timer with One.** `qR2`, `Tt` near the end: `[sleep] this
    track is the last: pausing at its end`, the pause at its end, the
    same entry cued (`ui`: the same entry, 0 ms), the moon gone. Again
    with `Ta` and `Tq` on a track mid-album: the same pause at this
    track's end. `qR0`.
13. **The wait.** `uiFw`, `X`: the title on one line, "Waiting for ...…"
    (amber) and "try 2 of 3" in the title strip, Play on speaker and
    Cancel in the rows, the spinner in the bigger disc. `uit200,60`:
    nothing (the strip is inert while waiting); `uit60,80`: the
    navigation menu (close it). `uiF0`. With headphones paired and off (a
    real wait): `uit180,112` plays on the speaker, `uit290,112` cancels.
14. **The seek bar moved.** Paused, `qs60`: `uit90,150` seeks (`seek 1:00
    -> ...`); `uid100,150,250,100,400`: `no seek (slid off the bar)` (off
    above 106); `uid100,150,250,120,400`: a seek (y 120 is still on). By
    hand: a slow scrub shows the readout over the album row and the
    cover's bottom, and the cover comes back whole at the lift (once with
    a thumbnail arriving mid-scrub: a new album's first scrub).
    - *Found at 065ec1f, known* (the console's path only: no touch makes
      a start point without a length): run straight after step 12, `qs60`
      on the entry the sleep timer's end-of-track pause cued (Repeat One
      cues the track that just played, its length told by step 12's
      seek) left the bar inert, `ui`: `60000 / 0 ms ... the bar: inert`,
      until a play/pause. `setStartPoint(ms, 0)` took the catalog's length
      hint for a stopped or cued entry, 0 for a library track, over the
      length told for the entry, which the bar showed until then. Fixed
      (one branch in `PlaybackController::placeStart()`, the length the
      bar already shows): `lengthHint()` (the length told for this
      entry's key: a seek's, a start point's) before the catalog's. Check: `qR2`, a seek to a
      track's last 10 s, `Tt`, the pause at its end, `qR0`, `qs60`: `ui`:
      `60000 / <its length> ms ...; the bar: rest, the knob at x ...`;
      `uit90,150` seeks (`seek 1:00 -> ...`). An entry with no length told
      (one never played) has none to keep: `qs` gives it the catalog's
      hint, as before.
15. **The undo a toggle took.** An add from the Library with its toast:
    `ui1`, an album open (`X` to find its bar), Play next on its bar by
    the scripted finger; within the toast's 4 s, `qS1`: `[ui] the Undo
    toast went: a shuffle toggle took the undo` (`ui`: `toast none`).
    `qS0` after tracks played while shuffled: the Queue badge doesn't
    flash (`X` of the tab bar). Again with `qu` in place of `qS1`
    (review fixes 2): `[queue] undo: done`, `[ui] the Undo toast went:
    undone from the console` (`ui`: `toast none`; before, the toast
    stayed and its Undo said "Nothing to undo").
16. **The cost of Off.** A long queue: `q+<n>` of a 25-track album 400
    times (10,000 entries; `q` shows it), `qS1`, `qS0`: the off line's
    `in N ms` (expected tens of ms), and `ui`'s fps unharmed after. Clear
    it (`qx`) and put the user's queue back.
17. **The press look** (by hand): a finger resting on the cover or the
    text lights the area around the cover; a slide off and a lift does
    nothing; a long press opens the menu at the lift, with one tick.

## 11. Build order

Each commit host-tested; the firmware built (core2 and core2-dio) at 4
and 6.

1. `Shuffle.h`, QueueModel's shuffle and ranks, `kAnyStart`;
   `queueview::shuffle()` moved; test_queue, test_ui_queue.
2. QueueText version 2; test_queue (QueueText, QueueSaver).
3. PlaybackController: `Repeat`, the two successors, `setShuffle()`,
   `repeats()`; `SleepTimer::lastOfQueue()`; test_playback,
   test_gapless_player, test_sleep_timer.
4. NvsLayout schema 2 and `repeatFrom()`; QueueStore's load and save;
   main: the queue's hook, the boot, the helpers, the console, the logs,
   the sleep timer's two sites; test_nvs_layout.
5. Sheet and Ui: `detailRoom()`, the settle, the staying rows, the
   follows, the undo toast; `cutPathLeft()`; UiText; UiHost and AppState;
   test_ui_nav, test_ui_library.
6. Now Playing: the layout, the readout over the cover, the zones, the
   press look, the two menus, the indicator and its icons, the waiting
   panel; SeekBar's rows; LibraryPage's `kAnyStart`; Shuffle all;
   test_seek_bar.
7. The docs of section 9.

## 12. Risks, and what isn't built

- **The default is a behaviour change.** An updated unit stops at the end
  of its queue where it wrapped. Said in the README; the menu is one tap.
- **The readout over the cover** is new to the eye (its band cuts the
  cover's bottom 24 rows while a finger scrubs). Legibility needs its
  background, so drawing over the artwork isn't an option. Check 14.
- **The cover pushed over a scrub** (a thumbnail, a repaint) would cut the
  readout: 4.3's rule. The host tests can't see it; check 14.
- **The waiting title on one line** cuts a long title harder than the
  three it had. Check 13.
- **A shuffle toggle in the last ~1.4 s** cuts the track decoded ahead, or
  restarts the next one past the join (the ring cut): any edit's
  behaviour, now one tap away. Host-tested; heard as a skip at worst.
- **Off on a 10,000-entry queue** sorts 120 KB of PSRAM on the loop task;
  estimated, not measured: check 16.
- **Repeat One on a file that ends with no frames and no failure** would
  have replayed back to back, as would a queue of one on repeat. OPUS.md
  M4's review found a file that does it (an Opus file whose last granule
  is its pre-skip: valid, 0 samples; an MP3 that is all encoder delay and
  padding or a FLAC of 0 samples would too), so the guard is in: a
  natural end with the position still at 0:00 counts as a failure ("no
  audio in it"), which moves on even under One, and every entry failing
  in a row stops (`checkEnd()`; test_playback's
  `test_an_end_at_0_00_is_a_failure`, test_gapless_player's
  `test_an_empty_entry_is_a_failure_under_repeat`). The Opus reader also
  refuses such a file at its open.
- **A downgrade while shuffled** loses the queue once (2.9); schema 2 is
  left alone by older firmware (3.6).
- **End of album with shuffle** pauses at most tracks' ends: the rule as
  it stands, documented.
- **Not built:** a reshuffle at Repeat All's wrap (it would cut the first
  entry already decoded ahead); spreading an artist's tracks apart;
  shuffle or repeat on the tab bar or the Queue's header; bigger prev and
  next glyphs (`next_track()` at 22 x 18 is a follow-up if the user wants
  them); the shuffle and repeat states as a toast.

## 13. The two designs, reconciled

Both were checked against the code. Their facts held, including the
widths (re-measured from `src/ui/VlwFonts.cpp`: "Waiting for SPYDRONE…"
is 160 px in Small and "Waiting for the headphones…" 193, both in
`kWaitTextW` 196). What this design took, and what it changed:

- **Taken from the layout design**: the 23 px rows, the band 24 px
  higher, the 72 px transport drawn as a 56-row strip, the readout over
  the cover and its three repairs, the one `Nav` zone (the cover only
  while waiting), the waiting panel moved into the title strip and the
  rows, both menus as 3-row sheets, the "Playback" title, Sleep timer as
  the last row, the staying rows, the indicator under "...", the seek
  bar's rows 106 and 114.
- **Made firm**: its "optional" press look (the house rule: a Down
  highlights) and its "recommended" settle guard, for every sheet (the
  fade toast's buttons are routed first and never settle). "Sheet
  follows" is generalised, so the console's toggles show on an open menu.
  The navigation sheet remembers its track. The icons are drawn here,
  pixel by pixel.
- **Taken from the player design**: the reordered queue with ranks, Off as
  a sort, the edits' table, toggles not undoable, the generator hook,
  `queue.txt` version 2, the two successors, One as the self-join, the
  timer winning, NVS `queue`/`repeat` with schema 2, Off as the saved
  default with the engine's own default left at All, the APIs and logs.
- **Changed**: Shuffle all turns the mode on (the player design kept it a
  one-shot, which left "Shuffle: Off" over a shuffled queue and no way
  back to the library's order), so `queueview::shuffle()` goes rather than
  being shared. The undo toast is hidden by Ui on a toggle (the player
  design left it to the UI). The menu's row order is the layout design's
  (Shuffle, Repeat, Sleep timer), not the player design's README wording
  (Sleep timer first).

## 14. As built (2026-10-06)

Built on feature/np-menus in the order of section 11, one commit a step:
3f230d2 (QueueModel's shuffle and ranks, `lib/core/Shuffle.h`), a15a1a9
(`queue.txt` version 2), 8e3d1bd (the player's repeat and shuffle,
`SleepTimer::lastOfQueue()`), b7203cd (NVS schema 2, `QueueStore`, main's
helpers, the console, the logs), 0a89e5e (the sheets: the settle, the
staying and followed rows, the undo toast; `cutPathLeft()`, the texts,
`UiHost`), 03b9898 (Now Playing, the icons, the seek bar's rows, Shuffle
all, the Library's `kAnyStart`), 7d1c98a (these docs), 4e47b69 (a
review fix: `repeats()` counts Repeat One's loops only), then a second
review's fixes (`test_a_toggle_allocates_nothing` counts the global heap
too; comments and SEEK-BAR.md's lines that still had the old rows or
missed the settle). 1,088 host tests
pass (41 new); at 4e47b69 core2 and core2-dio build with every guard
(iram_diet: 51 of 51 objects moved, the hot set pinned; cache_guard ok;
flash_guard: 2.20 MB, 37 % of the slot). The device check ran at
065ec1f (section 10).

**After the device check** (the commit "Now Playing menus: Shuffle all's
Undo keeps the mode; a readable Repeat One"):

- **Shuffle all's Undo keeps the mode** (section 2.6): QueueModel's
  snapshot carries the mode (`undoShuffled_`) and `undo()` restores it;
  `replace(.., shuffled)` and `PlaybackController::playNow(.., shuffle)`
  set the mode inside the edit; `Ui::shuffleAll()` calls that instead of
  `host_.setShuffle(true)` then `playNow()`, and clears the "added" marks
  only when the Play took. `undoShuffled()` feeds `q`'s `undo: play now
  (and shuffle off)`; the toast's and `qu`'s undo lines add `(shuffle off
  again)`; Shuffle all's line says `(shuffle on; was off)`.
- **Repeat One's glyph**: the loop and a bold "1" beside it, 20 x 11
  (section 4.2); `ui_icons.py` checks the indicator's bounds; every other
  icon's bytes in `IconData.cpp` are unchanged.
- **A start point on a cued entry keeps its told length** (section 10,
  step 14's note): `placeStart()` falls back to `lengthHint()` before the
  catalog's hint.
- The doc's wrong expectations: step 11's `G0` restart (no `[audio] ...
  starting` line at 0:00) and step 14's note.

1,092 host tests pass (4 new: `test_a_play_that_sets_the_mode_undoes_it_too`,
`test_shuffle_alls_undo_writes_version_1_again`,
`test_shuffle_all_and_its_undo`,
`test_a_start_point_on_a_cued_entry_keeps_its_told_length`; and
`test_a_start_point_keeps_its_length` follows the told length). core2 and
core2-dio build with every guard (iram_diet: 51 of 51 objects moved, the
hot set pinned; cache_guard ok; flash_guard: 2.20 MB, 37 % of the slot;
core2's app 2,243,247 B), no warnings. Not yet run on the device: section
10's steps 1, 9a and 14 check it.

**The review after that** (the commit "Now Playing menus: review fixes
2"):

- **`qu` on Shuffle all's toast logged `a shuffle toggle took the
  undo`**, though no toggle happened: the undo put the mode back, and
  Ui read any change of mode with the undo gone as a toggle (the toast
  going was right; the line was not). Ui's `lastShuffle_` is now
  `queueview::UndoWatch` (QueueView.h, section 4.5), which main's `qu`
  tells through `Ui::queueUndone()`; the line says `undone from the
  console` then. `qu` on any other edit's toast now takes it away too
  (it stayed, its Undo answering "Nothing to undo"). Section 6 lists
  both lines; steps 9a and 15 check them.

1,095 host tests pass (3 new, test_ui_queue:
`test_undo_watch_a_toggle_takes_the_undo`,
`test_undo_watch_qu_of_shuffle_all_is_no_toggle`,
`test_undo_watch_qu_of_any_edit`). core2 and core2-dio build with every
guard (iram_diet: 51 of 51 objects moved, the hot set pinned;
cache_guard ok; flash_guard: 2.20 MB, 37 % of the slot; core2's app
2,243,399 B), no warnings. Not yet run on the device: step 9a's `qu`
variant and step 15's.

Where the build differs from sections 1-13, and why:

- **The settle covers the Sleep timer sheet too**, not only `Sheet`:
  ARCHITECTURE.md's spec says "every sheet", and the Sleep timer sheet
  opens from the playback menu's last row, so a quick second tap there
  would land on its pills (Turn off, while a timer runs). One
  `sheetOpenedMs_` (from `millis()` at the opening) serves both; the
  ignored touch logs `[ui] sheet: a touch right after it opened,
  ignored`.
- **`cutPathLeft()` always marks a cut.** The Folders header's loop,
  moved as it was, gave "/a/b/Album" (the root dropped, no "…") when
  "/a/b/Album" fit but "…/a/b/Album" didn't. A cut path now always
  starts with "…" ("…/b/Album"); a path that fits is whole, and "/<last>"
  is still the fallback. The header gets the same fix.
- **`repeats()`**: a Repeat One boundary pause (the sleep timer) is no
  loop (nothing played again), so it doesn't count; a loop taken while
  paused (a pause's fade read past the join, the entry cued again) does.
  A queue of one on All joins its entry to itself too, but that is no
  Repeat One loop: not counted, no `[queue] repeat one` line.
- **The off line's ms** are measured with `micros()` and rounded.
- **The scripted check** (section 10): the scripted finger runs one
  touch at a time, so the settle's double tap is two `uit`s about 150 ms
  apart, never in one write; and a tap on the transport at y 170 uses
  the volume zone (x 32), not next, so the check doesn't skip the track.
- **The "isn't ready" refusal** can't be reached by the scripted finger
  (`g0`'s rebuild holds the loop); its text is host-tested.
- `PlaybackController::kAnyStart` is declared next to `play()`; the
  console's `qR` refuses a mode past 2 with its usage line.
