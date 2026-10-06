# Now Playing's seek bar

The user asked for "the progress bar": Now Playing's progress line as a
seek bar, touch to seek, on the exact-seek work already in the tree
([SEEK.md](SEEK.md)). This document is its design: the gesture, the hit
area, what is drawn, what a seek does in each player state, the races
with the decode task and with gapless joins, the edge cases, the files
and functions, the host tests, the log lines and the device check.

**Status: designed (2026-10-02, at 439148d), built on feature/seek-bar**
(section 12's commits 1-3, host-tested; the firmware builds with every
guard), **and checked on the device** (section 16: section 11's scripted
steps on 68c342b found no firmware defect). Still to do: 11.6's
slow-motion video, 11.10 (Waiting, once headphones are paired again),
11.12 (the sleep timer), and the user's checks by hand, 11.13-11.15.

**The layout moved (2026-10-06, feature/np-menus: [QUEUE-MODES.md](QUEUE-MODES.md)).**
Now Playing's artist and album rows lost their taps (a tap anywhere on the
cover or the text opens a navigation menu now) and shrank to 23 px each,
so the band moved up 24 px. The bar's touch is y 138-167, whether or not a
play waits; the line y 148-151; off above y 106 (onto the artist row) and
back from 114; the readout row y 113-145, over the album row and the
cover's lowest rows while a finger scrubs. Sections 0-4, 7 and 11 have the
new rows. Sections 9, 10 and 12-16 are the bar as it was built and checked
on 2026-10-02, with the old ones (the band y 170-191, off above 130).

Three designs were written for it: one for the touch (the gesture and
its feedback), one for the player and the engine (the semantics and the
races), and one for the smallest safe change (the scope and the tests).
They were read against the code. This one takes the first's gesture and
feedback, the second's semantics and race analysis, and the third's
scope and tests. Section 15 says what was dropped, and which of their
claims the code didn't bear out.

Where the facts come from:

- **The code** at 439148d. Below, `PC` is `lib/core/PlaybackController`,
  `C2AB` is `src/audio/Core2AudioBackend` and `NPP` is
  `src/ui/NowPlayingPage.cpp`; references are `file:line`.
- **SEEK.md**: the start plans, how exact each is (6.5), and what a
  start costs on the device (17).
- **The user's measurements of the input** (the input lab):
  - the hold stays 500 ms;
  - a tap tick is 33 ms at level 235, a recognised hold a double tick (2
    x 33 ms, 80 ms apart); 15 ms can't be felt and 25 ms is too light;
  - the panel read x up to +35-45 px too far right on the right half; the
    calibration corrects that, to within a few px, more at the edges;
  - controls at the right edge need hit areas that reach the edge;
  - y >= 240 is the button strip;
  - while an MP3 decodes the UI loop runs at 16-25 fps, and a track
    start stalls it for a moment while the decoder refills.

## 0. In short

- **A tap on the line goes to the second under the finger.** A tap on
  the knob (within 4 px of where it plays) does nothing.
- **A sideways drag scrubs**, and you feel a tick as it starts.
  - A drag that starts within 16 px of the knob moves the knob with the
    finger, without a jump. One that starts anywhere else brings the
    knob to the finger.
  - A large readout ("2:31 +1:21") shows in the row above the line, on
    the side away from the finger.
  - The music plays on where it was. **The seek happens once, at the
    lift.**
- **Three ways out without a seek:**
  - lift with the knob back on the marker that shows where it plays (it
    snaps there with a tick);
  - slide off the bar (above y 106, or onto the strip), then lift;
  - a sheet or a dialog takes the touch.
- **The ends:**
  - the far right is the track's length less 6 s (the tail rule turns a
    start in the last 5 s into 0:00);
  - the far left is 0:00;
  - readings clamped at either screen edge reach them.
- **One new player call, `PlaybackController::seek(key, ms,
  durationMs)`.**
  - In every state it sets a start point, by the rules `qs` already
    follows; 0:00 is prev's restart.
  - It does nothing at all if the entry changed under the finger: it
    checks after taking any gapless join the backend heard.
  - Paused stays paused, and the next play and the next boot start
    there.
- **While the backend hasn't taken a start up, Now Playing shows where
  it was asked to start** (`pendingStart()`). **While the backend knows
  no length, it shows the length the player was told** (`lengthHint()`).
  - So a seek never flashes the old second or a dotted line.
  - The same holds for the tab bar and the Dance tab.
- **The bar is inert** (no knob; taps and drags do nothing new) when:
  - the length is unknown (the dotted line);
  - the track failed;
  - the track is under 10 s.

## 1. What the tree has

**The band** (`NPP:35`; the layout comment is `src/ui/Pages.h:42-71`).

- The progress band is y 146-167 (y 170-191 until 2026-10-06).
  - Above it, y 136-145 is background, and above that the album row, y
    113-135, x 112 to the edge: part of the navigation area, whose tap
    opens Now Playing's navigation menu.
  - Below it is the transport, y 168-239: five 64 x 72 zones (drawn as
    one 56-row strip, y 176-231).
- `drawProgress()` (`NPP:175-255`) draws, in band rows (0 is screen y
  146):
  - a 4 px rounded DIV line at rows 2-5, x 12-307 (`x0 = 12, w = 296`);
  - the fill to `min(pos, len) * 296 / len`, in the accent while playing,
    else DIM;
  - a dotted line instead when the length is 0 (`NPP:187`);
  - the elapsed time and the length in Font::Small, centred on row 14;
  - between them "Paused, 4 of 16 · SPYDRONE" and the sleep timer's moon.
- Small's text fills its whole line height with the background
  (`src/ui/Fonts.h`), so anything drawn in rows ~7-21 before the text is
  cut.
- `update()` redraws the band when any of these changes (`NPP:468-471`):
  the second, the length, the state, the entry, the queue's size, the
  output or the sleep text.

**Touches** (`NPP:498-647`).

- `zoneAt()` returns None for the band.
- While play waits, every touch at x >= 112 and y >= 90 that isn't on the
  waiting buttons is None, the band included (`NPP:505-510`). So a new
  zone must be tested before that branch.
- A Down highlights a zone. A Tap, DragStart, Release or Cancel clears it,
  and only a Tap acts (`NPP:581-594`).
- A swipe up from the strip is ignored (`NPP:575`).
- The `Zone` enum (`Pages.h:87`) keeps the transport last
  (`z >= Volume`).

**The recogniser** (`lib/core/TouchRecognizer.h/.cpp`).

- Slop 12 px (Chebyshev), hold 500 ms.
- Where each event says the finger is:
  - **Tap:** where it landed.
  - **DragStart:** the point now, with (dx, dy) from the landing.
  - **DragEnd:** the last point. That is always the last DragMove's
    point, because a lift brings no new sample.
- A Fling after a DragEnd reaches nobody: the DragEnd ended the page's
  touch (`src/ui/Ui.cpp:1300`).
- `noHold()` keeps a resting finger draggable: no LongPress, and a lift is
  still a Tap. The volume slider (`Ui.cpp:1126`) and the A-Z rail use it.
- A glass touch that slides onto the strip stays a glass touch, and its y
  goes on past 239.
- `InputEvent::edges` flags a reading clamped at the panel's edge.
  - The user's panel saturates. With its calibration (the lab fit,
    `lib/core/TouchCalibration.cpp:19-20`), raw 0 corrects to x 25.6 and
    raw 319 to x 281.3.
  - **No real finger reads below ~26 or above ~281.** Only the flags
    reach the screen's ends.
  - The scripted finger sets the flags at x <= 0 and x >= 319
    (`src/ui/Input.cpp:331-332`).

**Touches that end early.**

- When a modal opens, the page gets a Cancel, and the rest of that touch
  goes nowhere (`Ui::endPageTouch()`, `Ui.cpp:533-542`).
- `Input::cancelTouch()` ends a touch with a Cancel. Nothing more comes
  of it until it lifts.
- When a toast goes away, all of Now Playing is repainted, because the
  page has no header (`Ui.cpp:465-470`). Toasts sit at y 36-71.
- The touch that wakes the screen (from Dim or Off) is swallowed through
  its lift (WakeLatch). A finger resting on the glass as the screen dims
  is taken with a Cancel.

**The seek API** (`PC.h:205-225`, `PC.cpp:242-297`).

- `setStartPoint(ms, durationMs, anchor)` acts by the state:
  - Playing: `startCurrent()` now (held: Waiting).
  - Paused or Waiting, holding a track: the track is let go (one
    `stop()`) and cued.
  - Stopped: the start point waits.
  - **`ms` 0 only clears a start point and starts nothing**
    (`PC.cpp:244-247`).
- `restart()` (private, `PC.cpp:161-179`) is prev's: the entry from 0:00,
  in the same state.
- Every public action begins with `Act`, whose constructor runs
  `syncHeard()`. A gapless join the backend reports heard moves the entry
  first (`PC.h:293-300`, `PC.cpp:539-577`). `setStartPoint()` has its own
  `Act`.
- `startNow()` (`PC.cpp:21-47`):
  - clears the start point;
  - sets `playedFromMs_`;
  - starts the word on what follows again, with a new token.
- `resumePoint()` (`PC.cpp:290-297`):
  - returns a waiting start point first, else a paused track's position;
  - reads `audio_.positionMs()` even while a start is pending.

**The backend** (`C2AB`).

- `request()` replaces the one request under a lock and posts a new
  generation; the decode task always starts the newest
  (`C2AB.cpp:261-271`).
- `positionKnown()` is false (the Pending phase) from the request until
  `start()` reports Decoding (`C2AB.cpp:377, 800`).
- During Pending:
  - `positionMs()` is the old run's second until `engine_->begin()`
    restarts the book (`C2AB.cpp:766`);
  - `durationMs()` is 0 from `knownDurationMs_ = 0` (`C2AB.cpp:742`)
    until the plan stores the length (`C2AB.cpp:1040`);
  - `prepare()` blocks on SD reads in between, so the loop draws in that
    window.
  - So **a seek drawn from today's snapshot would snap back to the old
    second and flash a dotted line for 40-150 ms.**
- `start()` discards the ring (`C2AB.cpp:724`). A start is heard at once,
  faded in over 64 frames (the DeclickReader). First audio comes in
  (SEEK.md 17):
  - 74-91 ms for a `qs` plan;
  - 101-149 ms by the run's index;
  - 35 ms from the top.
- A stop keeps the run's index ("A stop doesn't clear the slots", SEEK.md
  4.3). So a seek back into what played goes by it, even after paused
  seeks: exact in a run that started exactly (from the top, by CBR or an
  exact anchor), and on the TOC's timeline in one a TOC start began
  (SEEK.md 6.5).

**The tail rule** (`lib/core/TrackSeek.h:49-57`,
`TrackSeek.cpp:144-148`). A start in the last `kTailMs` (5 s), or at or
past the end, starts at 0:00. **So a drop at the line's end would restart
the track.**

**The snapshot** (`src/main.cpp:455-475`).

- A waiting start point shows its second and its length.
- The loop runs in this order:
  1. the input is dispatched (`handleInput`, `main.cpp:2502`);
  2. `player.update()` (`main.cpp:2522`);
  3. `queueStore.loop()`, which saves the resume point (`main.cpp:2528`);
  4. the UI, which takes the snapshot (`main.cpp:2611`, `Ui.cpp:931`).
- So a page's `onEvent()` sees the last pass's snapshot. A seek made
  there is in this pass's snapshot, and saved in this pass.

**The resume point.** `QueueSaver::stepResume()` saves `resumePoint()`
when it moved 250 ms or more, **or its anchor changed**. It clears it
when there is none (`lib/core/QueueSaver.cpp:158-191`). A paused seek
replaces an anchored point with an unanchored one, so it is saved at
once.

**How the UI reaches the player.**

- The UI calls the player directly for its edits
  (`src/ui/UiHost.h:15-19`; the Queue page calls `ui_.player().remove()`).
- The transport keys go through `UiHost` because of PlayGate and the
  pocket rule (a B click that would start playing out loud).
- A seek never starts or raises sound, so it needs neither.

## 2. The gesture

### 2.1 A tap: go there

- A Tap in the bar's zone seeks to `msAt(x)` (2.4), where x is where the
  finger landed. The user asked for touch to seek, and every phone
  player jumps on a tap.
- The exception is a tap within `kStayPx` of where it plays (2.5): a tap
  on the knob is not a seek.
- A tap that seeks ticks once the player has taken it, like any tap that
  acts.

A tap may seek although the band is thin, because:

- a stray tap on Next or Prev, just below, already can't be undone, and
  it is worse (it loses the track, not only the place);
- the zone is 30 px tall;
- the detent swallows taps on the knob;
- the drag with its readout is the precise way.

If the device run finds stray seeks, drag-only is a change of one
branch (section 13).

### 2.2 A drag: scrub, then one seek at the lift

- A DragStart in the bar's touch becomes a scrub only if it is sideways:
  `|dx| >= |dy|`, from the landing. Otherwise the bar lets the touch go,
  and nothing happens, then or at the lift.
- The scrub begins with a tick. The thin bar took the drag, so an
  accidental grab is felt at once and can still be left (2.6).
- **The music plays on where it was.** Each move only moves the knob and
  the readout. A seek on every frame would cost, each time:
  - a new request: 74-150 ms to first audio;
  - a ring discard and a fade-in;
  - SD reads competing with the LCD on the shared SPI bus;
  - on Bluetooth, the headphones' ~175 ms delay on top;
  - a UI stall at every start.
- **The lift (DragEnd) seeks once**, to the target the readout shows.
  Not when the knob is in the detent, and not when the finger is off the
  bar.

### 2.3 The grab

By the DragStart the finger has already moved 13 px or more. Where it
landed decides how the knob follows.

- **From the knob.** The Down landed within `kGrabPx` (16 px) of the
  knob, and its reading wasn't clamped.
  - The knob stays where it is at the DragStart, then moves by the
    finger's movement from there: `vx = x + (the knob's x at the
    DragStart − x at the DragStart)`. The knob's x at the DragStart is
    where it plays then, not where it was at the Down: `noHold()` lets a
    finger rest on the knob, and the pressed knob follows the music
    meanwhile. Anchored at the Down, a 1.5 s rest on a 1:00 track pulled
    the knob 13 px back at the first frame, and the lift sought behind
    where it played.
  - There is no jump on the first frame.
  - Only differences count, so the calibration's few px don't matter.
  - A nudge of a few seconds needs no aim.
- **Anywhere else.** The knob comes to the finger: `vx = x`.

### 2.4 x to time

- The line runs from `kLineX` (12) to `kLineX + kLineW − 1` (307):
  `drawProgress()`'s geometry.
- `msAt(x, edges, L)` gives:
  - **0** for a reading clamped at the left edge (`kEdgeLeft`), or
    x <= 12;
  - **`seekLimitMs(L)`** for one clamped at the right edge (`kEdgeRight`),
    or x >= 308;
  - otherwise `(x − 12) · L / 296` (in 64 bits), **floored to a whole
    second**, and at most `seekLimitMs(L)`.
- **Whole seconds.** The readout and the times show m:ss, and the seek
  asks for exactly that second. The backend shows the time asked:
  - exactly, for CBR, FLAC, built-in tracks and the run's index of an
    exact run;
  - for a LAME VBR start by its TOC, within 0.74 s (p95) of the time
    asked, which is still what it shows (SEEK.md 6.5); later seeks into
    that run, by its index, keep the same error.
  - So what the finger read is what Now Playing shows after the lift.
- **The reach.**
  - `trackseek::seekLimitMs(L) = L > 6000 ? (L − 6000) / 1000 · 1000 : 0`,
    from `kSeekGuardMs = kTailMs + 1000`.
  - The extra second covers a length the bar has from an estimate or a
    saved start point.
  - Past the reach the knob stops. While scrubbing, the rest of the line
    is drawn dotted, so the stop explains itself.
- **The length is frozen at the Down.**
  - The mapping and the seek use the length the bar showed then. A
    length estimate that settles mid-drag can't move the knob under a
    still finger.
  - The same length goes to the player, as the backend's hint.
- **The edges.**
  - On the user's calibrated panel, x never reads below ~26 (4.6 % of the
    line) or above ~281 (91 %). Without the edge flags, neither end could
    be reached.
  - With them, a drag to the right jumps from about 91 % to the reach as
    the reading clamps. The knob shows where it landed.
  - Both hit areas reach the screen's edges.
- **When it can seek.**
  - `seekable(L)` is `L >= kMinLengthMs` (10 s). A 10 s track has 4 s to
    seek in.
  - The page also needs a track (`s.current >= 0`) that hasn't failed
    (`!s.failed`).

### 2.5 The stay detent

- During a scrub, a marker shows where it plays: the snapshot's position,
  which moves while playing.
- **Staying** is `|xOf(target) − xOf(live)| <= kStayPx` (4 px, in line
  px). While staying:
  - the knob snaps onto the marker;
  - the readout says "no change";
  - a lift seeks nothing. Nothing is buffered again, and a paused track
    keeps its exact resume anchor.

  It is the "never mind" that can be found by feel.
- When it ticks:
  - a finger that moves into the detent ticks, like a new letter on the
    A-Z rail;
  - a knob grab that starts in it doesn't;
  - the marker moving onto a still knob doesn't.
- The same rule decides a tap: a tap whose target is within 4 px of the
  live position seeks nothing.
- How wide 4 px is:
  - on a 4 min track, about 3 s;
  - on a 60 min mix, about 48 s. Fine scrubbing is in section 13.

### 2.6 Off the bar: cancel

While scrubbing, the finger's y decides:

| Where the finger is | Then |
|---|---|
| y 114-231 | Scrubbing: x maps as in 2.3. A thumb coming from below may dip into the transport; it presses nothing there, because the touch is the bar's. |
| y < 106 (onto the artist row, the title or the cover) | **Off.** The knob goes back to where it plays, the readout says "Release to cancel" in amber, and a tick. |
| y >= 240 (onto the button strip: a glass touch keeps reporting there) | Off, the same way. |
| Back in y 114-231 | Scrubbing again, with a tick. The knob comes back by the grab's mapping. The 8 px from 106 to 114 (and from 232 to 240) stop it flapping. |
| A lift while off | No seek, no tick. |

### 2.7 Every end of a touch on the bar

| End | Seek? | Tick | Log (section 8) |
|---|---|---|---|
| Tap, outside the detent | yes, where it landed | the tap tick, once the player took it | `seek ... (tap)` |
| Tap in the detent | no | none | `no seek (back where it plays)` |
| DragEnd while scrubbing, outside the detent | yes, the readout's target | none (the audio's jump confirms it, as for the volume slider's drag) | `seek ... (drag ...)` |
| DragEnd in the detent | no | none | `no seek (back where it plays)` |
| DragEnd while off | no | none | `no seek (slid off the bar)` |
| A drag that wasn't sideways | no | none | none |
| Cancel (a sheet or dialog opened; the headphones-lost dialog; a finger taken as the screen dimmed) | no | none | `no seek (cancelled)` |
| The entry changed under the finger (a gapless join heard, a natural end, a skip from the headphones or the console, an edit) | no | none | `no seek (the track changed under the finger)` |
| `leave()` or `enter()` (the page went, or the display was taken) | no | none | none |

### 2.8 The thresholds

| Name | Value | Where | What |
|---|---|---|---|
| `kLineX`, `kLineW` | 12, 296 | SeekBar | the line, x 12-307 (`drawProgress()`'s; a `static_assert` ties them) |
| `kSeekReachPx` | 8 | NowPlayingPage | how far above the band the zone starts: y 138, whether or not a play waits |
| `kMinLengthMs` | 10,000 ms | SeekBar | shorter tracks: inert |
| `trackseek::kSeekGuardMs` | 6,000 ms | TrackSeek.h | `kTailMs` + 1 s. The reach is the length less this, floored to a second |
| `kGrabPx` | 16 px | SeekBar | a Down this near the knob grabs it (no jump) |
| `kStayPx` | 4 px | SeekBar | the detent around where it plays |
| `kOffAboveY` / `kBackAboveY` | 106 / 114 | SeekBar | off above the first; back from the second down (`static_assert`s in the page tie them to the readout row: 113 - 7 and 113 + 1) |
| `kOffBelowY` / `kBackBelowY` | 240 / 232 | SeekBar | off on the strip; back above the second |
| `kReadoutLeftX` / `kReadoutRightX` / `kReadoutStartX` | 184 / 136 / 160 | SeekBar | the knob right of 184: the readout goes left; left of 136: right; in between, it stays where it was (at the scrub's start: left from 160) |
| `uitext::kSeekReadoutW`, `kSeekReadoutGap` | 160, 8 | UiText.h | the readout group's width at most (a mix's "999:59 no change" is 159 px), and the gap between its two texts |
| `kReadoutY`, `kReadoutH` | 113, 33 | NowPlayingPage | the readout row: y 113-145, the full width (over the album row and the cover's lowest rows) |
| the slop, the hold | 12 px, none | TouchRecognizer, `noHold()` | the slop as everywhere; no LongPress on the bar |

## 3. The hit area

```
y  36-137  the navigation area (x 0-319)      a tap opens the navigation menu (Go to
                                              artist, album, folder); while play waits,
                                              the cover only, and Play on speaker and
                                              Cancel take x 112-319, y 90-137
y 138-167  THE BAR (x 0-319)                  Down: the pressed look; Tap: a seek;
                                              a sideways drag: a scrub. Waiting or not
y 168-239  transport (64 x 72 zones)          never the bar's
```

- **Why 8 px above the band.**
  - The line is drawn at the band's top (screen y 148-151), so a finger
    aimed at it lands on both sides of y 146. Without the extra rows, a
    seek aimed a few px high would open the navigation menu.
  - The boundary at y 138 sits about halfway between the album's text
    (centred at y 124) and the line (y 150). A tap aimed at either is
    about as unlikely to land on the other.
  - Touch y reads true on this panel; only x was skewed.
- **Nothing below the band.** A sloppy tap on prev, play or next must
  never seek. The line is 17 px above the transport's touch (y 168)
  anyway, 25 above its drawn strip (y 176).
- **While play waits** the zone starts at y 138 too: Play on speaker and
  Cancel are drawn y 94-129 and take y 90-137, so nothing is taken from
  them. `zoneAt()` tests the bar right after the transport, before the
  waiting buttons' branch.
- **The full width.** x 0-11 maps to 0:00 and x 308-319 to the reach.
  The clamped readings (2.4) reach both ends on a panel that can't report
  them.
- **`noHold()`** on every Down the bar takes. A finger that rests, then
  slides, still scrubs; one that rests, then lifts, is a Tap (a seek),
  never a long press.
- A swipe up from the strip still does nothing here (`NPP:575`).

## 4. What is drawn

### 4.1 The band (y 146-167)

Rows are the band's (row 0 is screen y 146). Everything stays inside the
band. The knob is drawn last, after the text, whose background fill would
cut it.

| Look | When | The line | Fill | Knob | Marker | Text row |
|---|---|---|---|---|---|---|
| **Inert** | not seekable (2.4): the dotted line, a failed track, under 10 s, nothing queued | as today | as today | none | none | as today |
| **Rest** | seekable, no finger on it | as today: 4 px DIV, rows 2-5 | as today: the accent while playing, else DIM | r 3 at (12 + fill, row 4), in the fill's colour | none | as today |
| **Pressed** | a Down on the bar, before it is a tap or a scrub | 6 px DIV, rows 1-6 (radius 3) | the accent, even paused (it is the active control), to the live position | r 4 TXT with an r 2 accent dot, at the live x | none | as today |
| **Scrubbing** | a sideways drag, on the bar | 6 px DIV, rows 1-6, to the reach; past it, FAINT dots (3 x 2 px every 6 px, rows 3-4) to x 307 | the accent, to the knob | r 4 TXT with an r 2 accent dot, at the target's x (at the marker while staying) | 2 x 9 px DIM, rows 0-8, at the live x; hidden while staying | blank (BG) |
| **Off** | slid off (2.6) | 6 px DIV, rows 1-6 | DIM, to the live position | r 4 DIM, at the live x | none | blank |

The fill and every x go through `SeekBar::xOf()`, which is today's
arithmetic: `min(pos, len) · 296 / len`.

### 4.2 The readout row (y 113-145, x 0-319)

It is up only while Scrubbing or Off.

- It is the 33 rows above the band, the full width: over the album row
  (y 113-135) and the cover's lowest 24 rows (y 113-136, its frame's last
  included), which come back at the lift. Legibility needs its
  background, so it isn't drawn over the artwork.
- A finger on the bar covers a 60-80 px patch around the contact, and a
  fingertip coming from below reaches 25-40 px above it. A bubble over
  the knob would be under the finger, so the readout goes to the side
  away from the knob.

| State | Text | Where |
|---|---|---|
| Scrubbing | The target ("2:31") in Font::Title, TXT; 8 px; then the change from the live position ("+1:21", "-0:45") in Font::Small, DIM. The minus is an ASCII "-": none of the four fonts has U+2212 (`tools/vlw_font.py`'s ranges), and one it lacks is measured as a space (4 px in Sans 13) but drawn folded to "-" (5 px), so every backward change was cut to "-0:…" | Left-aligned at x 12 when the knob is right of x 184; right-aligned to x 308 when it is left of x 136; in between, where it was. At the scrub's start: left if the knob is at x 160 or right of it |
| Staying | The live second ("1:10") in Title, TXT; then "no change" in Small, DIM (`uitext::kSeekStay`) | as above |
| Off | "Release to cancel" in Font::Bold, AMBER (`uitext::kSeekCancel`) | centred |

- Title is centred on the row's row 16 (screen y 129) and Small on row 19
  (y 132), so that their baselines roughly agree. Check it on the
  device's screenshot.
- The change is in whole seconds: `target/1000 − live/1000`.
- The times are m:ss, as the band's: a mix of 100 min or more reads
  "100:00" (84 px in Title, as any three-digit minute: the figures are
  tabular).
- The readout group is at most 160 px wide (`uitext::kSeekReadoutW`):
  "999:59 no change" is 84 + 8 + 67 = 159 px. (150 cut "no change" from
  100 min on.) From x 12, or to x 308, the widest group still ends 12 px
  short of where the knob sends it across (x 184 and 136); a
  `static_assert` in the page holds that.
- It is drawn again whenever what it shows changes: off, its side,
  staying, the target's second or the live second
  (`SeekBar::Readout`, compared field by field). A hash of those let two
  different readouts pass for one (a flip of side with a 31 s step, or
  "no change" and a sweep to 0:07), and the row kept the old text.

**Entering** the scrub look:

- while a play waits, clears the artist row (x 112-319, y 90-112): the
  waiting buttons' upper halves (y 94-112) would stick out above the
  readout;
- draws the readout row.

**Leaving** it (any end):

- fills the readout row with BG;
- pushes the cover's rows y 113-136 back from its sprite (no re-render,
  no thumbnail lookup; with no sprite, the placeholder's CARD);
- draws the middle again (`drawMiddle()`: the artist and album rows, or
  the waiting buttons);
- draws the band in its rest look.

**The cover is never pushed over the readout.** `drawCover()` renders
into its sprite as always, but while the readout is up it pushes only the
rows above it (y 39-112), so a thumbnail that arrives mid-scrub can't cut
it; the lift pushes the rest.

**Whenever `drawMiddle()` draws while the scrub look is up**, the
readout row is drawn again right after it, in the same pass (and, while
a play waits, the artist row cleared). That happens on a repaint after a
toast went away, and when the waiting panel's "try 2 of 3" changes. So
neither can leave the album row or a button over the readout.

### 4.3 Haptics

- Every one is the user's tap tick (33 ms at 235), through `ui_.tick()`,
  so the haptics setting rules them.
- Never per pixel: nothing on scroll frames.

| Moment | Feel |
|---|---|
| A Down on the bar | nothing (a Down only highlights) |
| A tap that seeks | one tick, once the player took it (Started or Waits) |
| A tap in the detent; a tap on an inert bar | nothing (nothing acted) |
| The drag becomes a scrub | one tick |
| The finger moves into the detent | one tick (not at a knob grab's start, and not when the marker comes to the knob) |
| Off, and back | one tick each |
| The reach or 0:00 | nothing (the knob visibly stops) |
| The lift (a seek, staying, or off) | nothing |

Off and back are single ticks:

- the double tick means "a hold recognised";
- the inert buzz's 20 ms pulses are close to what the user measured as
  too light.

### 4.4 After a seek: what Now Playing shows

The page holds nothing back. The snapshot (5.3) shows, from the pass of
the seek on:

- **Playing:**
  - until the backend has taken the start up: the target
    (`pendingStart()`) and its length (`lengthHint()`);
  - then the backend's position, which counts from the landed ms (the
    time asked).
- **Paused, Waiting, Stopped:** the start point's second and length, as
  today.
- **After a seek to 0:00 while paused:** 0:00 and `lengthHint()`, not
  "--:--". The bar stays seekable.

The tab bar's hairline and the Dance tab's line read the same snapshot
(`Ui.cpp:904-905`), so they follow too.

### 4.5 Frames and their cost

- `onEvent()` only changes the model (SeekBar) and plays the touch's
  ticks. `update()` draws.
  - It compares, with what it drew last (the `Drawn` struct, like
    everything else on the page): the bar's look, the knob's x, the
    marker's x, and the readout's text and side.
- **Scrub frames** are drawn only when `frameDue`.
  - They are: the look entering Scrubbing or Off, and any move of the
    knob, the marker or the readout.
  - `frameDue` is the 30 fps deadline; the ScrollGovernor backs it off
    when the audio ring runs low.
  - `update()` returns true for a scrub frame.
  - `animating()` is true while Scrubbing or Off, so the loop keeps the
    frame cadence.
  - While an MP3 decodes, the loop runs at 16-25 fps: in practice, a
    frame each pass.
- **Drawn at once:** the Pressed look on a Down, and the end of a scrub
  (the middle and the band put back), as the other zones' presses are.
- **Costs, as estimated.** A push is ~0.43 µs per pixel, and a row of
  anti-aliased text takes ~8 ms while an MP3 decodes (UI-SPIKE.md).
  - A scrub frame where only the knob or the marker moved: the whole
    band, 320 x 22 = 7,040 px, ~3.0 ms on the bus, plus fills and circles
    (no text). About 3.5 ms in all.
  - When the readout's text changes (the second under the finger; while
    playing, also the change, once a second): the row, 320 x 33 = 10,560
    px, ~4.5 ms, plus a Title string and a Small one, ~4-8 ms.
  - The worst frame is ~16 ms of drawing, against a 40-60 ms pass.
  - Every push is 33 rows or fewer, under the 40-line bus hold, so the SD
    card never waits long.
- **Costs, as measured** (section 16, 25 scrubs). The `[ui] scrub:`
  line's draw times are wall time: the decode task, above the loop on
  the same core, counts in them whenever it runs during a frame.
  - Nothing decoding (paused): a mean of 9.3-9.6 ms a frame, at most
    10.9 ms. That is the estimate: nearly every frame of a scripted drag
    draws the readout's new second.
  - A built-in click track playing: a mean of 7.8 ms, at most 13.4 ms.
  - A FLAC playing: a mean of 14.3 ms, at most 33.2 ms.
  - An MP3 playing, its ring steady: means of 19.3-22.4 ms, at most
    38.6 ms.
  - An MP3 in the refill after a seek (flat out to 500 ms, then paced at
    1.5x realtime until the ring is full, ~2.5 s in): means of 26-37 ms,
    at most 58.6 ms. The line's ring minimum reads `n/a (not playing)`
    for a scrub that ends before the ring is steady again (1000 ms): it
    only counts a steady ring.
  - So with an MP3 a frame takes 2-4 times the estimate, and the worst
    one about a pass. The cadence held all the same: 10 s across an MP3
    (11.7) drew 269 frames at 27.8 fps, the ring never below 1416 ms.
    No scrub had an underrun, and the governor stayed normal.
- **A scrub counts as a motion** in the UI's frame log.
  - `Ui::trackMotion()` takes any animating page, not only one with a list
    attached.
  - A page without a list logs the line as `[ui] scrub: ...` instead of
    `[ui] scroll: ...`: the frames, the fps, the draw time's mean and
    maximum, the ring's low point, new underruns and the governor.
  - The device run reads the underruns from it.

## 5. The player: `PlaybackController::seek()`

### 5.1 The API

In `lib/core/TrackSeek.h`, beside the tail rule:

```cpp
// A seek (Now Playing's seek bar) never asks for the tail rule's last 5 s,
// where a start goes to 0:00: it stops this far before the end (a second
// more covers a length known from an estimate or a saved start point).
constexpr uint32_t kSeekGuardMs = kTailMs + 1000;
// The furthest a seek goes in a track `durationMs` long, a whole second (0:
// only its start).
inline uint32_t seekLimitMs(uint32_t durationMs) {
  return durationMs > kSeekGuardMs ? (durationMs - kSeekGuardMs) / 1000 * 1000 : 0;
}
```

In `PlaybackController.h`, under "starting part of the way in":

```cpp
  // Now Playing's seek bar: what seek() did.
  enum class Seek : uint8_t {
    Started,  // playing: it starts there now (the ring cut, faded in, as a skip)
    Waits,    // paused, waiting, stopped or held: the next play (or the release) starts there
    Moved,    // the current entry isn't `key` any more (a join heard, an end, a skip): nothing done
    NoPlace,  // no track, no length, or the backend's track failed: nothing done
  };
  // The current entry from `ms` in (at most trackseek::seekLimitMs()), a
  // track `durationMs` long (the bar's: the backend's hint), if it is still
  // the entry `key` (the one the finger landed on). The heard join is taken
  // first, so one heard since is Moved, never a seek of the next track. ms
  // 0: prev's restart (playing: from 0:00 again; paused or waiting: the
  // held track let go, cued at 0:00; stopped: a start point dropped).
  // Otherwise as setStartPoint() with no anchor. Never plays from a pause
  // and never clears the timer's or the computer's marks; the queue, its
  // undo and "pause after this track" stay as they are.
  Seek seek(uint32_t key, uint32_t ms, uint32_t durationMs);
  // A play the backend hasn't taken up yet (IAudioBackend::positionKnown()
  // false, while the player holds the entry's track): where it asked to
  // start. The backend's position may still be the track before's.
  bool pendingStart(uint32_t* ms) const;
  // The current entry's length as the player was last told it (a seek's, a
  // play's hint, the held track's at a restart); 0: none. Now Playing shows
  // it while the backend knows none.
  uint32_t lengthHint() const;
  // Where the current entry is and how long it is, as Now Playing shows
  // them (the snapshot's, 5.3): a start point's; a pending start's, with
  // lengthHint() alone; else the backend's, with lengthHint() while it
  // knows none, unless the track failed.
  void shownTime(uint32_t* positionMs, uint32_t* durationMs) const;
```

Private:

- `void placeStart(uint32_t ms, uint32_t durationMs, const ResumeAnchor* anchor);`
  is today's `setStartPoint()` body, from the `ms == 0` check on.
  `setStartPoint()` becomes `Act act(*this); placeStart(ms, durationMs,
  anchor);`.
- `void noteLength(uint32_t ms)`: `if (ms > 0 && hasTrack()) { lengthKey_
  = queue_.currentKey(); lengthMs_ = ms; }`.
- `uint32_t lengthKey_ = QueueModel::kNone, lengthMs_ = 0;`
  - `lengthHint()` gives `lengthMs_` only while `lengthKey_ ==
    queue_.currentKey()`, so any change of entry drops it with no
    bookkeeping.
  - An entry never changes its track, so if the same key comes round
    again, the length is still right.

```cpp
PlaybackController::Seek PlaybackController::seek(uint32_t key, uint32_t ms, uint32_t durationMs) {
  Act act(*this);  // the heard join first: the entry may not be the one the finger was on
  if (!hasTrack()) return Seek::NoPlace;
  if (queue_.currentKey() != key) return Seek::Moved;
  const bool holding = state_ != PlayState::Stopped && !cued_;
  if (durationMs == 0 || (holding && audio_.failed())) return Seek::NoPlace;
  failuresInARow_ = 0;  // a listener's action, as next and prev
  noteLength(durationMs);
  ms = std::min(ms, trackseek::seekLimitMs(durationMs));
  if (ms == 0) {
    restart();
  } else {
    placeStart(ms, durationMs, nullptr);  // (no nested Act between the check and the start)
  }
  return state_ == PlayState::Playing ? Seek::Started : Seek::Waits;
}
```

- **No nested `Act`** comes between the key check and the start. An inner
  `Act`'s `syncHeard()` could take a join after the check. Then
  `setStartPoint()` would stamp the next entry's key (`PC.cpp:261`) and
  seek the next track to this one's second. That is why `placeStart()` is
  split out.
- `pendingStart()` follows `prevAction()`'s rule (`PC.cpp:142-145`):
  `if (!hasTrack() || state_ == PlayState::Stopped || cued_ ||
  audio_.positionKnown()) return false; *ms = playedFromMs_; return
  true;`.
- `noteLength()` is called by:
  - `seek()`;
  - `startNow()`, with its hint when it is above 0 (a resume point's
    length, or a built-in track's);
  - `restart()`, at its top, with `audio_.durationMs()` when the backend
    holds the track and has taken its start up (`positionKnown()`). Prev's
    restart while paused then keeps "0:00 / 4:05" instead of "0:00 /
    --:--", and the bar stays seekable. Not while a start is pending: the
    backend's length may still be the track before's, and a seek to 0:00
    then (an entry with a hint, right after a skip) has noted the bar's.
- `static_assert(trackseek::kSeekGuardMs > trackseek::kTailMs)` goes in
  `PC.cpp`.
- **`resumePoint()` while a start is pending** (a pause within ~150 ms of
  a seek): after the state check, `if (!audio_.positionKnown()) { *ms =
  playedFromMs_; *durationMs = lengthHint(); if (anchor) *anchor =
  ResumeAnchor{}; return *ms > 0; }`.
  - That is how `stopKeepingPlace()` already reads it (`PC.cpp:229-235`).
  - Today it reads the old second for a pass, which is saved and then
    corrected.
- `qs` stays as it is (`setStartPoint()`): the console's test of the
  start point.

### 5.2 What a seek does in each state

"Saved" is what `QueueStore::loop()` writes for the next boot
(`resumePoint()` → `QueueSaver::stepResume()`), in the same pass as the
touch.

| State at the lift | `seek(key, ms > 0, L)` | `seek(key, 0, L)` | Now Playing then shows | Saved for the next boot |
|---|---|---|---|---|
| Playing | Started: `placeStart()` → `startCurrent()` → `startNow()`: `play(path, {ms, hint L})`. One request: the ring discarded (heard at once), a plan, first audio in 74-150 ms, faded in. The word on what follows goes again with a new token | Started: prev's restart from 0:00 (35 ms to first audio), faded in | the target at once (`pendingStart()`), then counting from it | nothing while playing (`QueueSaver`); the next pause saves the new run's second with its anchor |
| Playing, held (Bluetooth, the headphones not connected) | Waits: Waiting, cued, the start point kept (`PC.cpp:10-16`) | Waits: Waiting, cued at 0:00 | the start point / 0:00 | the start point / cleared |
| Paused, holding its track | Waits: the track let go (one `stop()`, which the paused run's index survives), cued, the start point (no anchor); still Paused | Waits: let go, cued at 0:00, still Paused | the start point / 0:00 of `lengthHint()` | the start point, at once (its anchor changed) / cleared |
| Paused, cued (a start point waits; after a seek to 0:00; after a cue) | Waits: the start point replaced (no second stop) | Waits: dropped | the same | replaced / cleared |
| Waiting, holding a paused track | Waits: let go, cued, the start point; still Waiting. `release()` starts there, and so does Play on speaker | Waits: cued at 0:00 | the start point / 0:00 | the start point / cleared |
| Waiting, cued | Waits: the start point set; `release()` starts there | Waits: dropped | the same | the same |
| Stopped, with a start point (a boot's) or a remembered length | Waits: replaced; the next play starts there | Waits: dropped | the same | saved / cleared |
| Stopped otherwise; cued on a new entry; the length unknown | not reachable (the bar is inert). If called anyway: NoPlace | | | |
| The backend's track failed | NoPlace | | | |
| The entry isn't `key` | Moved: nothing at all | | | |

What a seek never does (none of `placeStart()`, `restart()` and
`startNow()` touches these):

- **play from a pause, or clear `pausedByTimer_` or `pausedByComputer_`.**
  `setPlaying()` only runs for a start from Playing, where both are
  already clear. So there is no new hearing-safety path, and no
  unattended (pocket) rule is needed.
- **change the entry or its key.** So these stay:
  - the queue and its undo;
  - the Dance tab's tempo prior (`main.cpp:2571`);
  - the Queue's learned lengths.
- **touch the sleep timer**: its "pause after this track", its count
  (`pauseAfter_`, `timerStops_`), and the NextGate.

### 5.3 The snapshot (`src/main.cpp`, `MainUiHost::snapshot()`)

The snapshot's position and length are the player's
(`player.shownTime(&s.positionMs, &s.durationMs)`), so the rule is
host-tested with the fake backend's late starts:

```cpp
void PlaybackController::shownTime(uint32_t* positionMs, uint32_t* durationMs) const {
  *positionMs = 0;
  *durationMs = 0;
  if (!hasTrack()) return;
  uint32_t ms = 0, length = 0;
  if (startPoint(&ms, &length)) {  // (as before the bar)
    *positionMs = ms;
    *durationMs = length;
    return;
  }
  if (pendingStart(&ms)) {  // never the backend's length here
    *positionMs = ms;
    *durationMs = lengthHint();
    return;
  }
  *positionMs = audio_.positionMs();
  *durationMs = audio_.durationMs();
  if (*durationMs == 0 && !audio_.failed()) *durationMs = lengthHint();
}
```

- A seek's pending start shows its target and the bar's length, so a seek
  never shows the old second or a dotted line.
- A skip's pending start shows 0:00 at once, and **no length** unless
  the entry has a hint (a built-in track's, a resume point's, one told
  before). Until the decode task takes the request up (`start()`, which
  clears `knownDurationMs_` and the frozen length), the backend's length
  is still the track before's. The first version of the snapshot kept it
  (`if (hint) s.durationMs = hint;`), and a finger landing on the bar in
  that pass made the new entry seekable at the old length: the seek went
  by it, and `noteLength()` kept it as the entry's `lengthHint()`. Now the
  bar is inert for that moment, as it already was through `prepare()`
  (where the backend's length is 0).

## 6. The races

The decode task runs on core 1 at priority 2, above the loop. The
outputs read the ring on their own tasks.

| | What happens | Handled by |
|---|---|---|
| R1 | A stale position and length while Pending: until the book restarts, the position is the old run's second, and until `start()` the length is the old track's (frozen or known). That includes a produce pass already under way | `pendingStart()` and `lengthHint()` alone in the snapshot (`shownTime()`, 5.3): a skip with no hint shows no length, so the bar is inert, never seekable at the old track's length |
| R2 | A length of 0 while `prepare()` reads the card (`C2AB.cpp:742` to `1040`). A header-less file has none for about 1 s more | `lengthHint()` in the snapshot |
| R3 | A join heard between the page's look at the snapshot and the action. `takeAdvance()` depends on the output's `readPos`, which moves on another task, and the page's snapshot is a pass old anyway | the key check inside `seek()`'s `Act`, after `syncHeard()`, with no nested `Act` (5.1) |
| R4 | A boundary pending in the book at the request (the next track decoded ahead) | It goes with the request's book restart and is never taken (`GaplessJoin.cpp:18, 52-63`). `startNow()` resets the offer. The next track is offered again with a new token, and decoded ahead again near the new end. A few ms of the next track may be heard in the µs between `syncHeard()` and `play()`, as with Next pressed at that moment |
| R5 | Seeks faster than `start()` (several taps in a row) | The backend keeps only the newest request (`C2AB.cpp:261-271, 649-653`). A start overtaken in `prepare()` costs its own SD reads and never makes audio. No rate limit is needed |
| R6 | A pause racing a seek's start | the backend's count of pauses keeps it paused (`C2AB.cpp:768-774`) |
| R7 | A pause in the Pending window | `resumePoint()` gives the pending start, without an anchor (5.1) |
| R8 | The track changes while the finger is down (a join, an end, a skip, an edit) | `update()` compares `s.currentKey` with the key the Down took. If they differ: the scrub ends, the page calls `ui_.input().cancelTouch(nowMs)` so nothing more comes of the touch, and it logs the drop. A lift before `update()` sees it is R3's case: Moved |
| R9 | A queue of one on repeat joins itself (the same key) | Not a drop: the same file and the same second. The seek applies to the new run |
| R10 | The sleep timer reads `audio.durationMs()` directly (`main.cpp:2047`), so during R2's window `knownLength` is false for a pass | Harmless: one pass without the Armed→Fading check, and a track fade's level only holds |

## 7. Edge cases

| Case | Behaviour |
|---|---|
| A failed track (`s.failed`, "Can't play this track") | Inert. While playing it is never seen, because `update()` skips it first. It stays only when stopped after every track failed, or paused on a start that then failed, with a length of 0. `seek()` returns NoPlace too |
| Unknown length (the dotted line) | Inert. A header-less VBR file gets an estimate after about a second of frames. An entry never held (stopped after a boot with no resume point; cued by next while paused) has no bar until it plays |
| Under 10 s | Inert |
| Built-in tracks (tones, click tracks) | Seekable. Their length is the catalog's hint; they count from the start ms (`C2AB.cpp:861-875`), exactly, at no SD cost. A click track's grid starts again, and the Dance tab's tracker resets on the new epoch |
| Waiting (PlayGate) | Seekable (5.2), from y 138 (as when not waiting). During a scrub the readout row covers the waiting buttons' lower halves and the artist row is cleared, and `drawMiddle()` puts them back. The spinner keeps turning in its own zone |
| The sleep timer's End of track | "Pause after this track" survives a seek: it still pauses at the end of the seeked track. A seek into the last 10 s starts the track fade at once (the reach lands at about −16 dB). A seek back out of a running fade keeps the faded level: the fade only falls by itself (`SleepTimer.cpp:169-176`), as after a skip, and the fade toast's +10 min and Turn off raise it. This is unchanged on purpose (SEEK.md 8 lists the sleep timer as unchanged) and pinned by a test. The timed choices count by the clock, so seeks don't affect them |
| Paused by the timer | A paused seek keeps the timer's mark: headphone Play still won't resume it |
| A gapless join | A seek lands at most at the reach (6 s before the end), outside the ~1.4 s decode-ahead window. A join heard during a drag ends the drag (R8); one heard at the lift gives Moved (R3) |
| A second seek within the first's 150 ms | The newer request replaces the older one (R5), and `pendingStart()` follows the newer |
| LAME VBR, by its TOC | It lands within 0.74 s (p95; 0.47 s on the device's 20 points), showing the time asked. Later seeks back into what played go by the run's index and keep that start's timeline, error and all (`the run's index; the time asked`); in a run from the top they are exact |
| A paused seek and the resume anchor | The exact paused-sample anchor goes. The next play (and a boot) starts by the second: exact for CBR, FLAC, built-in tracks, and inside the paused run's index (which the stop keeps) when that run was exact; by the TOC for a LAME VBR file after a reboot |
| The screen dim or off | The first touch only wakes it, swallowed through its lift, so a scrub can't begin from Dim or Off. A moving finger keeps the screen lit. One held still for 15 s stops counting; if the screen dims under it, the touch ends with a Cancel: no seek |
| A toast going away mid-scrub | The page repaints. The scrub's look is part of what `update()` draws, so the readout and the band come back as they were (4.2) |
| A sheet or dialog opening mid-scrub | Cancel (`endPageTouch()`): no seek. When the modal closes, the repaint shows the rest look |
| A second finger | Only the first touch point counts |
| The headphones drop mid-scrub | The pause comes from main.cpp. The lost dialog's opening cancels the touch; without a dialog, the lift seeks in the Paused state (it waits) |
| Long tracks (30 min and more) | 6 s or more per px, and the detent is ±4 px. Fine scrubbing is in section 13 |
| `uiF` faked states | Display only: a seek acts on the real player |
| The console's tests that borrow the backend (Rt, b<n>) | The snapshot shows the borrowed track's numbers on the stopped entry, as today. A seek there only sets a start point. Console only |

## 8. Logs and `ui`

Each touch on the bar logs one line when it ends, from the page. The
player logs nothing new; the backend's own start lines follow, as for
`qs`.

```
[ui] now playing: seek 1:10 -> 2:31 of 4:05 (tap): plays from there
[ui] now playing: seek 1:10 -> 2:31 of 4:05 (drag from the knob, held 240 ms): paused, the next play starts there
[ui] now playing: seek 1:10 -> 0:00 of 4:05 (drag, held 80 ms): waiting, it starts there when they connect
[ui] now playing: seek 1:10 -> 3:59 of 4:05 (tap): stopped, the next play starts there
[ui] now playing: no seek (back where it plays)
[ui] now playing: no seek (slid off the bar)
[ui] now playing: no seek (cancelled)
[ui] now playing: no seek (the track changed under the finger)
[ui] now playing: no seek (nothing to seek in)
```

- The fields:
  - `from`: the live position at the lift;
  - `to`: the target;
  - `of`: the length frozen at the Down.
- How the touch went:
  - `tap`;
  - `drag from the knob` (2.3's grab from the knob);
  - `drag`.
- `held N ms`: how long the final target's second had been shown before
  the lift. It is the measure for a lift guard (section 13).
- What happens next, from the result:
  - Started: `plays from there`;
  - Waits, by the state: `paused, the next play starts there`, `waiting,
    it starts there when they connect`, or `stopped, the next play starts
    there`.
- After it come the usual start lines:
  - `[audio] MP3: starting 2:31.000 in, of 4:05 (CBR; exact): byte ...`,
    or the FLAC, built-in or TOC line;
  - `[audio] refill: first audio in the ring N ms after the request ...`.
- Paused, the saver's line follows: `[queue] resume point saved: ...
  anchor: none`, or, for 0:00, `[queue] resume point cleared (playback
  moved on)`.
- `ui` (`describe()`) adds the bar to Now Playing's line, from the
  snapshot the page last drew with (so a `ui` read in a seek's own loop
  pass still shows the second before it: 11.6), one of:
  - `; the bar: inert`
  - `; the bar: rest, the knob at x 159`
  - `; the bar: pressed`
  - `; the bar: scrubbing to 2:31 (drag from the knob; readout left; 1:10 plays)`
  - `; the bar: staying`
  - `; the bar: off`

## 9. Files and functions

1. **`lib/core/TrackSeek.h`**: `kSeekGuardMs` and `seekLimitMs()` (5.1),
   in the tail rule's paragraph.
2. **`lib/core/PlaybackController.h/.cpp`**:
   - public: `Seek`, `seek()`, `pendingStart()`, `lengthHint()`,
     `shownTime()` (the snapshot's position and length, 5.3);
   - private: `placeStart()` (split from `setStartPoint()`),
     `noteLength()`, `lengthKey_`, `lengthMs_`;
   - `noteLength()` calls in `startNow()` and `restart()` (not while a
     start is pending);
   - `resumePoint()`'s pending case;
   - the `static_assert`;
   - a class-comment paragraph after the start point's, "A seek (Now
     Playing's seek bar) ...", in the style of the prev paragraph: what
     it is, 0:00, the key, the guard, and what it never touches.
3. **`lib/core/SeekBar.h/.cpp`** (new). It is portable and host-tested,
   with no drawing, no clock and no IRAM_ATTR. Its header comment, in the
   house style, covers: what the bar is, the tap and the drag, the grab,
   whole seconds, the edges and the reach, the detent, off, and one seek
   per touch.

   ```cpp
   class SeekBar {
   public:
     static constexpr int kLineX = 12, kLineW = 296;
     static constexpr uint32_t kMinLengthMs = 10000;
     static constexpr int kGrabPx = 16, kStayPx = 4;
     static constexpr int kOffAboveY = 130, kBackAboveY = 138, kOffBelowY = 240, kBackBelowY = 232;
     static constexpr int kReadoutLeftX = 184, kReadoutRightX = 136, kReadoutStartX = 160;
     enum class Phase : uint8_t { Idle, Pressed, Scrubbing, Off };
     // How a touch on the bar ended (None: it hasn't). Let: a drag that
     // wasn't sideways, let go.
     enum class End : uint8_t { None, Seek, Stay, Off, Cancel, Let };
     struct Out {
       End end = End::None;
       bool tick = false;  // the scrub began, the detent entered, off or back
       bool tap = false;   // the Seek was a tap's (its tick waits for the player)
       uint32_t ms = 0;    // Seek: the target
     };
     // What the readout shows, field by field (4.2): the page draws it
     // again whenever it changes.
     struct Readout {
       bool off, staying, left;
       uint32_t targetS, liveS;
       bool operator==(const Readout& o) const;
       bool operator!=(const Readout& o) const;
     };

     static bool seekable(uint32_t durationMs) { return durationMs >= kMinLengthMs; }
     static int xOf(uint32_t ms, uint32_t durationMs);                 // 0..kLineW (0: no length)
     static uint32_t msAt(int x, uint8_t edges, uint32_t durationMs);  // whole s, 0..seekLimitMs()
     // m:ss ("100:00" past 99 min), and "+1:21" / "-0:45" (an ASCII minus)
     static void timeText(uint32_t ms, char* buf, size_t size);
     static void changeText(uint32_t targetMs, uint32_t liveMs, char* buf, size_t size);

     // A Down in the bar's zone, on entry `key` playing at `liveMs` of
     // `durationMs` (frozen for the touch). False (inert, Idle): not seekable.
     bool down(const InputEvent& e, uint32_t key, uint32_t liveMs, uint32_t durationMs);
     // The touch's other events (Tap, DragStart, DragMove, DragEnd, Release, Cancel).
     Out onEvent(const InputEvent& e, uint32_t liveMs);
     // Every pass while active: the marker follows where it plays (staying
     // may change; no tick).
     void live(uint32_t liveMs);
     void cancel();

     Phase phase() const;
     bool active() const;     // not Idle
     bool scrubbing() const;  // Scrubbing or Off
     uint32_t key() const;
     uint32_t lengthMs() const;
     uint32_t targetMs() const;
     uint32_t liveMs() const;
     bool staying() const;
     bool knobGrab() const;
     bool readoutLeft() const;
     Readout readout() const;  // while scrubbing or off
     int knobX() const;    // screen x: the target's (the marker's while staying or off)
     int markerX() const;  // screen x: where it plays
     uint32_t heldMs(uint32_t nowMs) const;  // since the target's second last changed
   };
   ```

   What `onEvent()` does, by event:
   - **Tap** (while Pressed): the target from the landing x; ends Stay,
     or Seek (a tap).
   - **DragStart** (while Pressed):
     - not sideways: ends Let;
     - else: the grab (2.3), Scrubbing, the readout's side by the knob
       (left if x >= 160), and a tick.
   - **DragMove and DragEnd** (while Scrubbing or Off):
     - off or back by y (2.6), with a tick each;
     - on the bar: `vx` → `msAt()` → staying, with a tick when a move
       enters the detent;
     - the readout's side by `kReadoutLeftX` and `kReadoutRightX`.
   - **DragEnd**, after that update: ends Off, Stay or Seek.
   - **Release, Cancel:** ends Cancel.
   - Anything while Idle: nothing.
4. **`lib/core/UiText.h`**: `kSeekCancel` "Release to cancel",
   `kSeekStay` "no change", and the readout's `kSeekReadoutW` (160) and
   `kSeekReadoutGap` (8), which test_ui_library measures.
5. **`src/main.cpp`**: the snapshot (5.3: `player.shownTime()`), and
   nothing else.
   - No new console command: the scripted finger drives the bar, and `qs`
     stays the start point's test.
   - No UiHost change: the UI reaches the player directly.
6. **`src/ui/Pages.h`**:
   - The layout comment: `162-191 the seek bar's touch (170 while play
     waits)` and `137-169 its readout while a finger scrubs`, plus a
     paragraph on the tap, the drag, the lift, the ends, and when it is
     inert.
   - `Zone`: `{ None = -1, Cover, Artist, Album, WaitSpeaker, WaitCancel,
     Bar, Volume, ... }`. `Bar` comes before `Volume`; it isn't called
     `Seek`, which `PlaybackController` has.
   - `void leave() override;` and `bool animating() const override {
     return bar_.scrubbing(); }`.
   - `drawProgress()` takes its look (4.1) from `bar_` and the snapshot.
   - New: `void drawReadout();`; `void endScrub();` (the middle, the left
     column, the band); `void seekTo(const SeekBar::Out& o, uint32_t
     nowMs);`; `bool seekable() const;`.
   - `Drawn` gains: `uint8_t bar` (the look), `int16_t knobX`, `int16_t
     markerX`, `SeekBar::Readout readout` (what the readout shows, its
     fields: a hash let two readouts collide) and `bool scrubUp`.
   - A member `SeekBar bar_;`.
7. **`src/ui/NowPlayingPage.cpp`**:
   - the header comment: the seek bar;
   - `constexpr int kSeekReachPx = 8; constexpr int kReadoutY = 137,
     kReadoutH = 33;`, with `static_assert(SeekBar::kLineX +
     SeekBar::kLineW == kW - 12)` and `static_assert(kReadoutY == kCoverY
     + kCoverPx + 1)`;
   - `enter()` and `leave()`: `bar_.cancel()`;
   - `zoneAt()`, right after the transport: `if (e.y >= kProgressY -
     (ui_.state().play == PlayState::Waiting ? 0 : kSeekReachPx)) return
     Bar;`;
   - `onEvent()`, after the strip and empty-state checks:
     - a touch the bar has goes to it first: `bar_.onEvent(e,
       s.positionMs)`; a tick on `o.tick`; `seekTo()` on End::Seek; the
       log for the other ends; `pressed_ = None` once the bar is Idle;
     - a Down on `Bar`: if `seekable()`, `bar_.down(e, s.currentKey,
       s.positionMs, s.durationMs)`. Taken: `pressed_ = Bar` and
       `ui_.input().noHold()`. Otherwise `pressed_ = None` (inert, no
       tick);
   - `seekTo()`: `ui_.player().seek(bar_.key(), o.ms, bar_.lengthMs())`;
     the tick for a tap on Started or Waits; the log line (section 8);
   - `update()`:
     - R8's abort first, before the empty-state branch;
     - `bar_.live(s.positionMs)` while the bar is active;
     - the look and its redraw rules (4.2, 4.5);
     - after any `drawMiddle()`, the readout again while the scrub look
       is up;
     - the return value;
   - `drawProgress()`: the looks (4.1), the knob last;
   - `describe()`: the bar (section 8).
8. **`src/ui/Ui.cpp`**: `trackMotion()` takes any animating page, and
   says `scrub` for a page without a list (4.5).
9. **Docs, when it is built:**
   - README's Now Playing bullet: "the progress line: tap it, or drag
     along it and lift, to move in the track (never into its last 6 s;
     slide off it, or back onto where it plays, to leave it; paused, it
     stays paused, and play, or the next boot, starts there)";
   - README's resume paragraph: "a pause, or a seek while paused, saves
     ...";
   - README's lib/core list: `SeekBar`;
   - ARCHITECTURE.md's Now Playing paragraph: drop "not built yet";
   - this document: its status, and an "On the device" section.

Cost: flash ~2-3 KB; static RAM ~50 B (the page's `SeekBar` and `Drawn`
fields, the player's 8 B); no IRAM.

## 10. Host tests (`pio test -e native`, from Git Bash)

**test_seek_bar** (new; `SeekBar`, `InputEvent`, `TouchRecognizer`,
`TrackSeek`). L is 245,000 ms (4:05) unless said otherwise.

1. `test_ms_at_maps_the_line`:
   - x 12, 0 and −5 give 0;
   - x 40 gives 23,000; 160 gives 122,000; 250 gives 196,000; 300 gives
     238,000;
   - 308 and 319 give 239,000 (the reach);
   - every result for x −10..330 is a whole second, and none is smaller
     than the one before.
2. `test_ms_at_takes_the_clamped_edges`: (200, kEdgeLeft) gives 0;
   (282, kEdgeRight) gives 239,000.
3. `test_no_seek_lands_in_the_tail`: for L of 10,000, 10,999, 60,000,
   245,000, 3,600,000 and 36,000,000, and every x:
   `trackseek::startMs(msAt(x), L) == msAt(x)`, and `msAt(x) <= L −
   6,000`.
4. `test_seekable`: 0 and 9,999 are not seekable; 10,000 is. `down()` on
   a length that isn't seekable returns false, and the next Tap ends
   nothing.
5. `test_x_of_is_the_drawing`:
   - `xOf(0) = 0`, `xOf(L) = 296`, `xOf(L + 5,000) = 296`, `xOf(x, 0) = 0`;
   - `|xOf(msAt(x)) − (x − 12)| <= max(1, px per second)`, up to the
     reach.
6. `test_a_tap_seeks_where_it_landed`: live 60,000 (the knob at x 84). A
   Tap at 160 ends Seek (a tap) at 122,000, and nothing is active after.
7. `test_a_tap_on_the_knob_seeks_nothing`, live 60,000:
   - Taps at 81 and at 89 end Stay;
   - a Tap at 80 ends Seek at 56,000;
   - a Tap at 90 ends Seek at 64,000.
8. `test_a_drag_seeks_once_at_the_lift`, live 60,000, a far grab:
   - Down at 200, DragStart at 214, DragMoves to 250: no end, a tick only
     at the DragStart, the target 196,000;
   - DragEnd at 250: Seek at 196,000, not a tap;
   - a Fling after it: nothing.
9. `test_a_knob_grab_doesnt_jump`, live 60,000:
   - Down at 90, DragStart at 103: staying, the knob at x 84;
   - DragMove to 153 (`vx` 134): the target 100,000;
   - DragEnd: Seek at 100,000.
10. `test_a_vertical_drag_is_let_go`: a DragStart with dx 3, dy −13 ends
    Let; a DragEnd after it ends nothing.
11. `test_the_detent_snaps_and_ticks`:
    - entering it by a move ticks once;
    - staying at the DragStart of a knob grab doesn't tick;
    - `live()` moving the marker onto the knob makes it staying, without
      a tick;
    - a DragEnd there ends Stay.
12. `test_off_and_back`:
    - y 129 is off (a tick), 135 still off, 138 back (a tick);
    - 240 is off, 235 still off, 231 back;
    - a DragEnd while off ends Off;
    - while off, the knob is at the marker.
13. `test_the_readout_changes_side_with_hysteresis`: the knob at 150 at
    the start: right; moved to 190: left; back to 150: still left; at
    130: right.
14. `test_the_length_is_frozen_for_the_touch`: after `down()`, the
    mapping uses its length, whatever `live()` says.
15. `test_a_cancel_or_release_seeks_nothing`.
16. `test_with_the_recognizer`: a real `TouchRecognizer`, with `noHold()`
    and `down()` on its Down, and its events fed to `onEvent()`:
    - a finger resting 700 ms at x 100, then sliding to 200: no
      LongPress, and one Seek at `msAt(200)`;
    - resting 900 ms, then lifting: a Tap, and a Seek;
    - a flick from 50 to 250: a DragEnd, then a Fling, and one Seek.

Added by the review: the grab after a rest, every threshold on both
sides (a test on one side passes a threshold moved the other way), and
what the readout shows:

17. `test_a_knob_grab_after_a_rest_doesnt_jump_back`: a 1:00 track at
    20 s, a Down on the knob (x 110), 1.5 s of rest (the pressed knob at
    x 118), a DragStart at 123: staying, the knob still at 118; 10 px on,
    23,000 (forward); the lift seeks there.
18. `test_off_and_back_at_their_rows`: y 130 still on, 129 off; 137 still
    off, 138 back; 239 still on, 240 off; 232 still off, 231 back.
19. `test_the_grab_reaches_16_px`: Downs 16 px either side of the knob
    grab it, 17 px don't.
20. `test_the_readout_sides_at_their_edges`: the knob at 184 keeps the
    readout right, 185 sends it left; 136 keeps it left, 135 sends it
    right; at the start, 159 is right (160 left: test 13).
21. `test_the_readout_changes_with_what_it_shows`: the two pairs a hash
    once collided on (a 51:00 mix, x 184 to 187: 29:38 right, then 30:09
    left; 4:05 at 1:40: staying, then a sweep to 0:07) give different
    `readout()`s, and off differs from both.
22. `test_the_readout_texts`: "0:00", "2:31", "100:00"; "+1:21", "-0:45"
    (ASCII), "+0:00".

**test_playback** (the Rig; `FakeAudioBackend` with `asyncStarts` and
`anchorsOn`; `TestHold`):

1. `test_a_seek_while_playing_starts_there_now`:
   - Started; `playCount` 2, `lastStartMs` 90,000, `lastHintMs` 245,000;
     Playing; no start point waits;
   - with `asyncStarts`, `pendingStart()` gives 90,000 until `take()`,
     and `prevAction()` is Restart.
2. `test_a_seek_to_0_is_prevs_restart_in_every_state`:
   - playing: a play from 0;
   - paused: still Paused, one stop, no start point, `resumePoint()`
     false; then play starts at 0;
   - stopped with a start point: it is dropped, and nothing plays;
   - and the contrast: `setStartPoint(0, L)` while playing starts
     nothing.
3. `test_a_seek_while_paused_lets_the_track_go_and_is_the_resume_point`,
   with `anchorsOn` and a held MP3 anchor:
   - before: `resumePoint()` gives 30,000 with the anchor;
   - after `seek(key, 120000, 245000)`: Waits, Paused, one stop;
     `resumePoint()` gives 120,000 and 245,000, with no anchor;
   - a second paused seek doesn't stop again;
   - play starts at 120,000 with hint 245,000, and no anchored play.
4. `test_a_seek_while_waiting_starts_there_when_they_connect`:
   - with TestHold, play: Waiting;
   - the seek: Waits, a start point at 60,000;
   - `release()` starts there;
   - also `cancelWait()`, then play.
5. `test_a_seek_while_stopped_waits`.
6. `test_a_seek_for_an_entry_that_moved_does_nothing`:
   - a heard join (the next word's token pushed to `advances`), then
     `seek(key0, ...)`: Moved; `currentIndex` 1, `playCount` unchanged,
     no start point;
   - after `next()`: the same;
   - an empty queue: NoPlace.
7. `test_a_seek_never_asks_the_tail`: 243,000 and 300,000 on 245,000 ask
   for 239,000; any ms on 5,000 restarts (0).
8. `test_a_seek_on_a_failed_or_unknown_length_track_does_nothing`:
   NoPlace, no play, no stop.
9. `test_a_seek_keeps_the_timers_marks_and_pause_after`: `pausedByTimer()`
   and `pauseAfterTrack()` stay; `timerStops()` is unchanged.
10. `test_a_seek_resets_the_failures_in_a_row`.
11. `test_the_length_hint_follows_its_entry`: after a seek on entry 0,
    `lengthHint()` is 245,000; after `next()`, 0; back on entry 0 (prev),
    245,000 again.
12. `test_prev_restart_while_paused_keeps_the_length`: `duration`
    245,000, paused at 30 s, `prev()`: `lengthHint()` is 245,000.
13. `test_the_resume_point_while_a_start_is_pending`: with `asyncStarts`,
    a seek to 90,000, then `togglePlayPause()` before `take()`:
    `resumePoint()` gives 90,000, with no anchor.
14. `test_the_shown_time_never_lends_the_track_befores_length`
    (`shownTime()`, 5.3): playing 300,000 at 120 s, `next()` before
    `take()`: 0 and no length (the backend still says 300,000); taken up:
    its 180,000; a seek's pending start: 90,000 and the bar's 180,000;
    a backend that knows no length: 180,000; failed: none.
15. `test_a_seek_to_0_while_a_start_is_pending_keeps_the_bars_length`:
    entry 1 told 180,000, on to entry 2 (300,000), prev back to entry 1
    (pending): it shows 180,000, and a seek to 0:00 then keeps
    `lengthHint()` at 180,000 (`restart()` doesn't note the backend's).

**test_gapless_player** (`World`, the real engine; its backend starts
every play from 0, which is enough for the words and the joins):

1. `test_a_seek_right_after_a_join_is_dropped`, modelled on
   `test_next_right_after_a_join_skips_the_joined_track`: Moved, `plays`
   unchanged, the joined track plays on.
2. `test_a_seek_while_the_next_is_decoded_ahead_takes_it_back_out`:
   - the old token is never taken;
   - a new token is offered after the request;
   - the album still ends as one stream after it.
3. `test_end_of_track_still_names_nothing_after_a_seek`: with the gate
   up, the word after the seek is token 0.

**test_sleep_timer**:
`test_a_position_that_jumps_back_in_a_track_fade_keeps_its_level` (the
decision in section 7).

**test_ui_library**:

- `fits()` for `kSeekCancel` (Bold, in 296 px);
- the readout group for lengths up to 999:59 (Title from
  `SeekBar::timeText()`, 8 px, Small "no change" and the widest changes
  from `SeekBar::changeText()`, both ways) within `kSeekReadoutW`
  (160 px), the figures all as wide; every character one the font has
  (`Vlw::hasAll()`: a missing one is measured as a space and drawn
  folded), and "-0:45" whole.

**test_queue**: nothing new. `test_resume_anchor_saved_with_the_point`
already saves a point whose anchor went, and a clear.

## 11. Device check

**The build.** From PowerShell, with MSYSTEM removed: `Remove-Item
Env:MSYSTEM -ErrorAction SilentlyContinue; pio run -e core2`.

- The first build in a worktree is a full one.
- `iram_diet` and the flash and version guards must pass; if one fails,
  read its message.

**The setup.**

- COM3 through the serial daemon.
- Silent mode `z` on the speaker.
- Now Playing up (`ui0`).
- Note the user's queue first, and restore it afterwards.
- The console takes every byte outside a command's argument as a key,
  at once: a script must send nothing but commands. A stray `-f` steps
  the volume down, then forgets the headphones (`f`), which then have to
  be paired again on the Output tab.

**The scripted finger** works in screen pixels; the line is at y ~150
(it was ~174 when these steps were first run: section 16's lines).
For a track of length L:

- the second at x is `floor((x − 12) · L / 296 / 1000)`;
- the knob for a position p is at `12 + p · 296 / L`.

The examples are for a 4:05 CBR MP3.

1. **The look.** `X` (the whole screen): the knob (r 3) at rest. A dotted
   line (an entry never held) has none.
2. **Taps, paused (deterministic).** Pause (`uit160,204`), then `qs60`
   (`[queue] start point: 60 s into the current entry (paused)`).
   - `uit85,150`: `no seek (back where it plays)`, and no tick.
   - `uit90,150`: `seek 1:00 -> 1:04 of 4:05 (tap): paused, the next play
     starts there`, then `[queue] resume point saved: ... anchor: none`.
     Still Paused.
3. **Drags, paused.** `qs60` again before each.
   - `uid200,150,250,150,600`: exactly one line, `seek 1:00 -> 3:16 of
     4:05 (drag, held N ms): paused, ...`, and none during the move.
   - `uid84,150,184,150,600`: `(drag from the knob, ...)`. It lands short
     of x 184's 2:22 by the DragStart's slop: about 2:08-2:12.
   - `uid200,150,84,150,600`: `no seek (back where it plays)`.
   - `uid100,150,110,90,300` (upward): no line at all.
   - `uid100,150,250,60,400`: `no seek (slid off the bar)`; so does
     `uid100,150,250,100,400` (y 100: onto the artist row), while
     `uid100,150,250,120,400` (y 120, the readout's rows) still seeks.
   - `uid100,150,250,262,400` (onto the strip): the same.
4. **The ends.**
   - `uit319,150`: `-> 3:59` (L − 6 s).
   - `uit0,150`: `-> 0:00`, then `[queue] resume point cleared (playback
     moved on)`. `ui` shows the length kept (`0 / 245000 ms`, not a
     dotted line), and `uit160,150` still seeks.
   - With the measured panel (`uk1`): `uit290,150` (raw 319, flagged)
     gives `-> 3:59`; `uit20,150` (raw 0, flagged) gives `-> 0:00`. Then
     `uk0`.
5. **The resume.** Paused after a seek to 2:02, reset through the serial
   daemon.
   - At the boot, `q` shows the start point at 2:02, and Now Playing
     shows 2:02 / 4:05.
   - Play logs `[audio] MP3: starting 2:02.000 in, of 4:05 (CBR;
     exact)`.
6. **Playing, CBR.** Play, then `uit160,150`:
   - `seek a -> 2:02 of 4:05 (tap): plays from there`;
   - `[audio] MP3: starting 2:02.000 in ... (CBR; exact)`;
   - `[audio] refill: first audio in the ring N ms`, with N 74-150;
   - `s`: pos ~122 s, underruns unchanged.
   - The time and the knob go straight to 2:02: never back to the old
     second, never dotted. Watch it, and film it with a phone's
     slow-motion video for the record.
   - The console can't show this. `ui` describes the snapshot the page
     last drew with, and the console reads every byte waiting after the
     touch is handled and before `Ui::loop()` takes the next snapshot. So
     a `ui` read in the seek's own loop pass (sent with the tap, or
     arriving while the start stalls the loop) shows the old second; one
     sent alone after the seek's log line shows the target. Neither
     sees the frames in between.
7. **A long scrub while an MP3 plays.** `uid20,150,300,150,10000`. The
   `[ui] scrub:` line gives the fps, the draw maximum, the ring's
   minimum and underruns (expect +0). `[heap] playing` is unchanged.
8. **LAME VBR, FLAC, a built-in track.**
   - *One More Time*: `(LAME's TOC inverted; the time asked)`. A seek
     back into what played goes by the run's index, as exact as the
     run: `(the run's index; the time asked)` after that TOC start, and
     `(the run's index; exact)` after a seek to 0:00.
   - A FLAC: `[audio] FLAC: starting 2:02.000 in (libFLAC's seek to
     sample ...)`.
   - `qb`, a click track: `uit319,150` gives `-> 0:54 of 1:00` and
     `[audio] built-in track: 0:54 asked: counting from there`. It joins
     the next track 6 s later (`G`: joins continuous).
   - On the Dance tab after a seek: `[dance] tracker reset`, with the
     tempo prior kept.
9. **The zone.** `uit160,139` seeks. `uit160,136` logs `[ui] now playing:
   the navigation menu` (close it 400 ms later: `uit160,60`. Every sheet
   ignores a touch within 300 ms of its opening, `[ui] sheet: a touch
   right after it opened, ignored`, and the menu would stay up over
   step 10's taps).
10. **Waiting.** Only if the headphones are certainly off: nothing but
    silence may go to them. Otherwise skip it; the host tests cover
    Waiting.
    - Set the output to Bluetooth, turn `z` off (the hold needs it off),
      and play: Waiting.
    - `uit160,150`: `... (tap): waiting, it starts there when they
      connect`.
    - `uit300,130` logs `cancel the wait for the headphones`: Cancel's
      area, not the bar's (which starts at y 138). The state is Paused,
      and `q` shows the start point.
    - Turn `z` back on, set the speaker as the output, and play:
      `[audio] MP3: starting 2:02.000 in`.
11. **A join under the finger.** `qb`, play the first click track,
    `qs50`, then at once `uid30,150,200,150,15000`.
    - The join comes at ~10 s, during the drag: `no seek (the track
      changed under the finger)`.
    - No `[audio]` start follows the lift.
12. **The sleep timer.** `Tt`, then a seek back to 1:00: the moon still
    says "track", and it pauses at the end (`[sleep]` lines). A seek into
    the last 10 s starts the fade.
13. **The screen** (by hand: the scripted finger acts in the dark). Let
    the screen dim, then touch the bar: the touch only wakes it.
14. **By hand (the user):**
    - a tap and a drag on the right half, and at both ends;
    - a grab from the knob (no jump), and a grab elsewhere (the knob
      comes to the finger);
    - the readout legible on both halves, with the finger coming from
      below;
    - the ticks felt: the grab, off and back, the detent;
    - sloppy taps aimed at the album's lower half, and at the line;
    - whether stray taps seek (section 13's drag-only switch);
    - the `held N ms` values over ~20 drags on a 4 min track and on a
      long mix (the data for a lift guard).
15. **Bluetooth** (the user, with silence only): seeks while connected
    to the headphones give no dropout beyond the fade-in, and no AVRCP
    effects.

## 12. Build order

One commit each on feature/seek-bar, with the host tests in each:

1. **"Seek bar: PlaybackController::seek(), and the snapshot while a
   start is pending"**:
   - TrackSeek.h's guard;
   - the player (5.1), and `resumePoint()`'s pending case;
   - main.cpp's snapshot;
   - tests: test_playback, test_gapless_player, test_sleep_timer.
2. **"Seek bar: SeekBar, the mapping and the touch"**: lib/core/SeekBar,
   tested by test_seek_bar.
3. **"Seek bar: Now Playing's progress line seeks"**:
   - the page, Pages.h, UiText, and Ui.cpp's motion log;
   - test_ui_library;
   - the README, and ARCHITECTURE.md's "not built yet" dropped.
4. **The device run** (section 11), with its results appended as
   section 16, "On the device".

## 13. Not built (later, each with what would bring it)

- **Drag-only**: a tap shows a hint instead of seeking. It is one branch
  in `SeekBar::onEvent()`'s Tap. Build it if the device run or the user
  finds stray seeks.
- **A lift guard**: when the finger rolled 4 px or less in the last
  ~60 ms, commit the target shown before the roll. The `held N ms` log
  measures whether the FT6336U's lift moves the last point.
- **Live audio scrubbing**, or a seek per move (2.2).
- **Fine scrubbing**: slower as the finger moves away vertically. The off
  zones use the vertical for now; it matters for long mixes.
- **An Undo after a seek** (a toast). The detent and sliding off cover
  the accidental drags, before the lift.
- **Pre-decoding a paused seek** (a "start paused" flag in `StartAt`).
  It would give an instant resume, and an exact anchor for a VBR file
  after a reboot. It needs the pause count and the outputs on a fresh
  paused ring checked on the device.
- **An anchor for a paused seek** from the paused run's index
  (`SeekIndex::find`), so that a reboot resumes it exactly.
- **SEEK.md 6.6's seek index per path.** Now that the bar exists, first
  measure how often its seeks miss the run's index.
- **The bar elsewhere**: on the tab bar's hairline, on the Dance tab, or
  as the headphones' FF/REW keys; and a drag to the very end meaning
  next.
- **Seeking an entry whose length isn't known yet** (stopped after a
  boot with no resume point).
- **Pushing only the band's line rows during a scrub**, which would save
  about 1.5 ms a frame.

## 14. Risks

- **Stray taps seek.** The band is thin, between the album band and the
  transport, and a stray tap loses the place (not the track, as a stray
  Next would). The detent swallows taps on the knob. Drag-only is the
  fallback (section 13).
- **The album loses 8 rows.** Its hit area is now y 130-161 (32 px),
  while its drawn band stays 40 px; y 162-169 seek.
- **The edges on the user's panel.**
  - A drag to the right jumps from ~91 % to the reach when the reading
    clamps.
  - The first ~4.6 % on the left is reached only through the flag.
  - It never restarts or skips, and the knob shows where it landed.
- **What can't be reached:** the last 6 s of every track; and tracks
  under 10 s can't be seeked at all.
- **A paused seek drops the exact anchor.** After a reboot, a LAME VBR
  file resumes by its TOC (0.74 s p95). CBR, FLAC, built-in tracks and
  seeks inside the paused run stay exact.
- **A header-less VBR file's length estimate** may be off by more than
  the 1 s margin. A seek to the reach can then still hit the tail rule
  and start at 0:00 (the backend logs it).
- **Lift jitter** of a few px moves a drag's second: ~1-3 s on a 4 min
  track, more on long mixes. It is unmeasured (section 13).
- **Long tracks:** 12 s per px for an hour, and there the detent is
  ±48 s.
- **A seek back out of an End-of-track fade** keeps the faded level
  until +10 min or Turn off, as a skip does.
- **`lengthHint()` changes what prev's restart while paused shows**:
  0:00 / the length, not --:--. Intended, and tested.
- **Every seek while playing is a full start**:
  - 74-150 ms to first audio, and a fade-in;
  - a brief UI stall while the ring refills;
  - the run's index starts again, so only the latest run is exact to
    seek back into.
- **The bus.** Each scrub frame competes with the SD card. `frameDue`
  and the ScrollGovernor govern it, and the `[ui] scrub:` line measures
  it.
- **The readout's side** assumes the finger comes from below. A thumb
  at an angle may still cover it.
- **Bluetooth** can only be checked by the user, with silence.

## 15. The three designs, judged

**Taken:**

- **From the touch design:**
  - the drag gesture: sideways only, the two grabs, the seek at the lift;
  - the stay detent and its marker;
  - sliding off to cancel, with hysteresis;
  - the readout row, on the side away from the finger;
  - the looks and the haptics;
  - the abort with `cancelTouch()` when the key changes;
  - the log line per touch.
- **From the engine design:**
  - `seek(key, ...)` and its result;
  - the key check inside `Act`, with `placeStart()` split out;
  - 0:00 as prev's restart;
  - the 6 s guard, in the player too;
  - `pendingStart()` and `lengthHint()` in the snapshot;
  - the `resumePoint()` hardening;
  - the table by state, and the races.
- **From the minimal design:**
  - the portable class for the arithmetic and the touch;
  - the edge flags, for both ends;
  - the zone from y 162 (y 170 while play waits), tested before the
    waiting branch;
  - the knob drawn last;
  - the host tests, including the one with the real recogniser;
  - no UiHost change and no console command.

**Changed or dropped, and why:**

- **Drag-only (the touch design): dropped.**
  - The user asked for touch to seek.
  - A stray tap does the kind of harm a stray Next already does, and
    less of it.
  - The detent covers taps on the knob.
  - It stays as the fallback (section 13).
- **The touch design's grab rows at y 158-169, where a tap opens the
  album: changed.** The line is drawn at the band's top, so a tap aimed
  at it often lands above y 170, and those rows would open the album.
  The zone from y 162 splits the two aims evenly.
- **The touch design's left end at x < 12: wrong for this panel.** On
  the user's calibrated panel it can't be reached (raw 0 corrects to
  x 25.6). The left edge flag maps to 0:00. The engine design's "x below
  16 snaps to 0:00" has the same gap.
- **The engine design's "a Tap clamped at the right edge is ignored":
  rejected.** The clamp is how the right end is reached on this panel.
- **The touch design's hold until `AppState::positionKnown` (with a 2 s
  timeout), and the minimal design's 500 ms landing hold: replaced** by
  the snapshot's pending start and length hint. These end exactly when
  the backend has the start, outlast a slow `prepare()`, and serve the
  tab bar and the Dance tab too. So the minimal design's "no main.cpp
  change" doesn't hold; the change is about ten lines.
- **The minimal design's length remembered in the page: moved** into the
  player (`lengthHint()`), so that every view has one source.
- **The minimal design's drag down onto the strip still seeking, and its
  dead zone measured from where the knob was at the landing: replaced**
  by off on the strip, and the live marker's detent.
- **The engine design's "a target within 1 s isn't sent": replaced** by
  the 4 px detent, which is what the user sees.
- **The engine design's "the saver sees a paused seek one pass later":
  wrong.** It is the same pass, because `handleInput()` runs before
  `queueStore.loop()`.
- **The touch design's lift guard (60 ms / 4 px): deferred.** It is
  unmeasured, and the DragEnd's point is the last DragMove's anyway;
  `held N ms` measures it.
- **The touch design's pushes of only the changed columns: dropped** for
  whole-row pushes: about 1.5 ms more a frame, for one code path.
- **The touch design's hint toast for a tap: not needed**, since a tap
  seeks.

## 16. On the device (2026-10-02)

Section 11's scripted steps on the Core2 (COM3, through the serial
daemon), with 68c342b's build (`v0.5.0-26-g68c342b`, ELF 606c6cdf),
silent mode (`z`) on the speaker throughout. The tracks: Air's *Moon
Safari* (CBR MP3s of 7:09, 4:58 and 4:28), Daft Punk's *One More Time*
(LAME VBR, 5:20), Kavinsky's *OutRun* (FLACs of 1:54 and 3:27) and the
built-in click tracks (`qb`); section 11's x values were worked out again
for each length. Every line matched sections 2-8: **no firmware
defect**. The user's queue (27 tracks, at 9) and its start point (0:55)
were put back.

- **11.2-11.4, paused, on the 7:09 track** (`qs60`: the knob at x 53):
  - a tap at x 55: `no seek (back where it plays)`; at x 70: `seek 1:00
    -> 1:24 of 7:09 (tap): paused, the next play starts there`, then
    `resume point saved: 1:24 into 1; anchor: none`;
  - drags, each with one line at the lift and none during the move: x
    200 to 250, `-> 5:45 (drag, held 144 ms)`; from the knob, x 53 to
    153, `-> 3:05 (drag from the knob, ...)`, short of x 153's 3:24 by
    the DragStart's slop; x 200 back to 53, `no seek (back where it
    plays)`; upward, no line; above y 130 and onto the strip, `no seek
    (slid off the bar)`;
  - a hold (`uih`) on the bar ends as a tap, never a long press;
  - x 319 gives `-> 7:03` (L − 6 s); x 0 gives `-> 0:00` and `resume
    point cleared (playback moved on)`, `ui` then shows `0 / 429897 ms`
    with the knob at x 12, and x 160 still seeks (`-> 3:34`). With `uk1`,
    x 290 gives 7:03 and x 20 gives 0:00. On the 1:54 FLAC, x 319 gives
    1:48.
- **11.5, the resume.** Paused at 2:07 after a seek, then reset: `resume
  point: 2:07 into 1 (stopped: play starts there)`, Now Playing at
  127000 / 429897 ms. A tap while stopped: `-> 2:51 ...: stopped, the
  next play starts there`; then play: `MP3: starting 2:51.000 in, of
  7:09 (CBR; exact)`. The 1:54 FLAC paused at 1:12 and reset: `FLAC:
  starting 1:12.000 in`. A paused seek to 0:00, then a reset, leaves an
  entry never held: `0 / 0 ms; the bar: inert`, the dotted line (section
  7).
- **11.6-11.8, playing.** Every seek logged `plays from there` and its
  start line:
  - `(CBR; exact)` on *Moon Safari*. Five drags 0.8 s apart gave five
    seeks and five starts, with no underrun;
  - `(LAME's TOC inverted; the time asked)` on *One More Time*; back into
    that run, `(the run's index; the time asked)`; back into a run from
    0:00, `(the run's index; exact)` (7 and 11.8 said exact for both;
    they now say which);
  - `libFLAC's seek to sample N` (78-140 ms) on the FLACs;
  - on a click track, x 319 gives the length less 6 s (`0:24 of 0:30`,
    `0:54 of 1:00`) and `built-in track: 0:54 asked: counting from
    there`; it joined the next track 6 s later (`G`: joins continuous).
  - First audio in the ring after a seek: CBR 80-85 ms, the TOC 74-75 ms,
    the run's index 117-133 ms, a FLAC 99-180 ms, a click track 3-14 ms.
- **11.7 and the frames:** 4.5's measured costs; 25 scrubs, no underrun.
- **11.9, the zone** (x 160): y 163 seeks and y 191 is still the bar;
  y 161 opens the album; y 192 and 195 are play/pause.
- **11.11, a join under the finger:** `qs50` on a click track, then a
  15 s drag. The join came ~10 s in: `no seek (the track changed under the
  finger)`, and no start after the lift.
- **The console's `ui` right after a seek** shows the old second when it
  is read in the seek's own loop pass (11.6 now says why). So Now
  Playing's frames in the pending window are still the slow-motion
  video's to check.
- **Not run:** 11.10 (Waiting) and 11.12 (the sleep timer); the user's
  11.13-11.15. The `held N ms` values (113-162 ms) are the scripted
  finger's, not data for a lift guard.
- **Not a firmware matter:** a stray `-f` from the test script, 51 s in,
  made the Core2 forget the remembered headphones (`[bt] forgot the
  remembered headphones`, and the restart that follows it). They have to
  be paired again (Output > Pair new headphones); 11's setup now warns
  of it.
