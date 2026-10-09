# Dance sprites: drawn dancers for the Dance tab

The Dance tab's two dancers are code. The crab is pixel art posed by
`CrabPose`, the stick figure is lines placed by `DancePose`, and each has
one dance and one idle loop. A new dancer, or a new move, means new C++.
The user asked for a dance engine with a sprite system instead: characters
made of drawn frames, as many moves as a character needs, moves used only
within a BPM range or tied to genres, moves for when there is no beat, and
a way for other people to make characters.

This document is its design. A character is a sheet of drawn frames and an
INI manifest. A tool on the laptop compiles them into a small binary pack
(MPDN), which the firmware carries built in or reads from the SD card. The
engine times the frames to the tracked beat and adds the motion: offsets
it eases between, hops, sways and travel. A choreographer picks the moves.
The Dance tab gives the dancer a stage 320 px wide, and the first drawn
set is a new stick figure that travels between three spots.

**Status: designed (2026-10-07, at 2e1b5c5), with the user's decisions
(section 12). Steps 1 and 2 are built (cbfed62: tools/dancepack.py, its
tests and the test dancer; a5a6fdd: the C++ reader and blitter); the rest
is designed.** Section 11 is the build order. The contributor guide,
docs/DANCERS.md, comes with step 11. Appendices A-C freeze the contract
for step 1: the MPDN v1 bytes, the shared integer algorithms and
dancepack.py's interface. Appendix D freezes step 2's: the C++ reader and
blitter, the differential reader vectors and the ASan env. Appendix E
freezes step 3's: the character interface, DrawnCharacter, the budget and
the Choreographer. Where they and the sections above differ, the
appendices win.

Where the facts come from:

- **The code** at 2e1b5c5 (0.7.0). References name the function; a few
  `file:line` references point at the lines this design relies on or
  changes.
- **MASCOT-POC.md** ("The crab on the Core2"): the crab's and the stick
  figure's frame rate, draw and push times, and the 10-minute soak.
- **BEAT-TRACKER-EVAL.md**: the first lock on music (median 9.7 s), the
  locked beats that aren't on the beat (13.5 %), how long a lock outlives
  the music (median 1.1 s), and I Wonder's octave flips (95.7 and 191
  BPM).
- **ENERGY.md** items 8 (the Dance tab's frame rate, `DanceRate`) and 9
  (the outputs' taps written only while the Dance tab is shown).
- **The user's measurements of the input**: the hold is 500 ms; a tap tick
  is 33 ms and a recognised hold a double tick; the panel reads x up to
  +45 px too far right on the right half.
- **Costs not measured** are worked out from the crab's: 0.47 µs a pixel
  to push while idle, 0.8 µs while an MP3 decodes.
- **"(estimate)"** marks a number or a behaviour inferred, not read in the
  code or measured.

## 0. In short

- **Two classes of character behind one interface.** Drawn characters
  (frames plus engine-applied motion) are the standard. The crab and the
  old stick figure (now `stick-classic`) stay as code characters. The
  interface doesn't draw; each class has a renderer in src/ui.
- **Moves.** Beat moves last 1, 2, 4 or 8 move beats. No-BPM moves play
  when nothing is heard (`quiet`) or when no beat is locked (`listening`).
- **A BPM range is the move's danced pace.** The device can't know a
  song's tempo, so the engine dances each move at half, the same or double
  the tracked beat to land in its range (reachable: 36-180). A running
  move whose pace leaves its range is cut at its next whole move beat.
  `song_bpm` is reserved for when tracks carry a BPM.
- **Genre is a filter**, dormant until tracks carry genres. No fallback
  relaxes the pace or genre filters; when nothing is eligible, an
  engine-made bounce plays.
- **The format:** a PNG sheet (up to 15 colours) and `dancer.ini`,
  compiled by `tools/dancepack.py` into an MPDN v1 pack. Built-ins live in
  flash. Card packs live in `/dancers/*.dnc` and ship in the same release
  as the stick set.
- **The Dance tab:** a 320×164 stage over a 40 px rail with no tap
  targets. A tap goes to the next dancer, a flick to the previous or next,
  and a hold opens the Dancers list. The stage has three spots for travel.
- **The first drawn set** is a new stick figure. The crab stays the
  first-boot dancer, pixel for pixel, and the choice is remembered in NVS.
- **Rendering** costs no internal RAM and no IRAM, and pushes in holds of
  at most 40 rows and 8,000 px.

## 1. Characters

### 1.1 One interface, two classes

Both classes implement one portable interface in lib/core. **The interface
doesn't draw.** Drawing happens in src/ui, through one renderer per class.
That keeps the classic figure's anti-aliased `drawWideLine` path out of
lib/core.

**One `Choreographer` owns the dance weight, the idle clock and the
dancer's spot.** No character holds any of them, so a switch keeps all
three by construction; only a switch to a character without travel moves
puts the spot back at the centre (E.2.4).

```cpp
// lib/core/DanceCharacter.h (portable)
namespace dance {
enum class MoveKind : uint8_t { Beat, Idle };
enum class Origin : uint8_t { BuiltIn, Card };
enum : uint8_t { kWhenQuiet = 1, kWhenListening = 2 };   // Idle moves; 3 = any
enum : uint8_t { kMirror = 1, kFreeze = 2, kPlaceStart = 4, kPlaceEnd = 8, kHeavy = 16, kSnap = 32 };
enum : uint8_t { kNeedsSongBpm = 1 };                    // known to v1, unsupported: move skipped
enum class StartAt : uint8_t { Any, Centre, Side, Left, Right };  // mirrored: Left<->Right
struct MoveLabels {                // what the picker reads, the same for both classes
  const char* name;
  MoveKind kind;
  uint8_t beats;                   // Beat: 1, 2, 4, 8 MOVE beats
  uint16_t idleMs;                 // Idle: 500..12000; 0 = continuous (code characters only)
  uint8_t paceLo, paceHi;          // danced pace 36..200; 0,0 = any
  uint8_t weight10;                // 1..100 = 0.1..10.0 (default 10)
  uint8_t when, flags, repeat;     // repeat 0 (auto) ..8, repeat*beats <= 16
  uint8_t fromTag, toTag;          // pose keys ("stand", "low", "left"...), 0 = none;
                                   // the pack also carries each one's mirror twin (A.12)
  uint8_t needs;                   // feature bits; any bit this engine lacks -> move skipped
  int8_t travel; StartAt startAt;  // spots -2..+2, negated when mirrored (section 8)
  uint8_t genreCount; const uint32_t* genreKeys;  // folded keys, never FNV("")
};
struct CharacterInfo { const char* id; const char* name; const char* author;
                       const char* licence; const char* url; Origin origin;
                       bool inCycle; uint8_t ignoredMoves; };
class Character {
 public:
  virtual ~Character() = default;
  virtual const CharacterInfo& info() const = 0;
  virtual int moveCount() const = 0;
  virtual const MoveLabels& move(int i) const = 0;
};
}
```

This is the sketch. Appendix E.1 gives the exact header: the record bits
come from DanceRules.h, MoveLabels gains the twins' ids (`fromTagM`,
`toTagM`), and Character gains `startSpotMask()`, the spots a move can
start from (section 8's fit needs the art).

### 1.2 Drawn characters

`DrawnCharacter` (lib/core) is a zero-copy view over a parsed pack: a
const array in flash, or bytes in the PSRAM arena (section 5.6). Its
`frameAt(move, pos16, mirrored, spot, e16)` returns B.4's placement (the
frame, its drawn orientation, the root, the box, the shadow and the
character rect), and it is host-tested. `DrawnRenderer` (src/ui) blits
that result.

### 1.3 The crab

`CrabCharacter` stays procedural and declares two moves:

- `shuffle`: a 2-beat beat move, any pace, flagged `freeze`, from and to
  `stand`;
- `idle`: an idle move, continuous, for both no-beat states.

`CrabRenderer` maps the cue: phi = frac(u) and odd = floor(u) & 1, then
`crab::blend(idlePose(idleSeconds), dancePose(phi, odd), w)`, drawn with
`forEachLayer`/`blit` through a `fillRect` adapter. CrabPose, CrabArt and
`crab::Crab` stay byte for byte, so test_crab_pose's 48 hashes and
test_dance_rate still hold. Odd now means the beat inside the move, not
the tracker's beat index, so it no longer flips on a re-acquire. The crab
has no travel moves, so it always dances at the centre spot.

### 1.4 The classic stick figure

Today's stick figure becomes `stick-classic`, with `bounce` (2 beats,
flagged `freeze`) and `sway` (idle, continuous). `ClassicRenderer` draws
`dance::blend(idlePose, dancePose)` with `drawWideLine` into the RGB565
stage. DancePose.h/.cpp stay byte for byte, because `danceWeight`,
`danceStep`, `TempoFold`, test_dance_pose and beat_eval's explicit source
list all need them. The drawn set of section 9 takes the id `stick`.

### 1.5 The cast

`DanceCast` (lib/core/DanceCast.h) replaces `DanceSkin`:

- **Order:** `crab`, `stick`, `stick-classic`, then card packs sorted by
  file name. `stick-classic` is in the Dancers list but not in the tap
  cycle (`inCycle` false).
- **Identity:** `info().id` replaces `skinName()`, so `[dance] on (crab)`
  and `skin=` keep their shape.
- **Duplicate ids:** of two card packs with the same id, the first by file
  name wins, and the other is greyed out as "same id as robo.dnc".
- **Overrides:** only a **drawn** built-in (today just `stick`) can be
  overridden. `crab` and `stick-classic` can't, so the first-boot default
  always exists.
  - A card pack with that id replaces the built-in **only after its full
    load succeeds**. Until then, and if the load fails, the built-in
    dances and stays in the cast as the fallback.
  - The row is labelled "(card)". That is how contributors iterate on the
    reference stick.
- **Allocation:** cast entries, Character objects and roster rows are
  `psramNew`'d (src/app/Psram.h). Dance code uses no std containers that
  grow (section 6.8).

## 2. Move rules

All limits are constants in `lib/core/DanceRules.h`. The compiler and the
device enforce the same values.

### 2.1 Lengths and timing

| | Beat moves | No-BPM (idle) moves |
|---|---|---|
| Length | 1, 2, 4 or 8 **move beats**. At octave k a move lasts D = beats·2^−k dance beats. | 500-12,000 ms in all |
| Timing unit | a tick, 1/12 of a move beat; a step is at least 3 ticks (¼ beat); the steps sum to 12·beats | 10 ms; a step is 200-5,000 ms |
| Steps | at most 32 | at most 32 |
| Why | At the fastest pace (180) a ¼ beat is 83 ms: 2.5 renders at 30 fps and 2.0 at 24. Twelve ticks a beat also allow triplet feels (1/3 = 4 ticks). | Idle frames render at 10 fps (100 ms), so a 200 ms step always shows. A 100 ms step could be skipped by one missed deadline. |

A 1/6-beat step would fall below the safe quarter beat; the 1/12 tick with
a 3-tick floor keeps both triplets and safety.

**Other bounds:**

- `bounce` and `sway`: 0-16 native px.
- `repeat`: 0-8, with repeat·beats ≤ 16.
- `weight`: 0.1-10.
- `travel`: −2..+2 spots.
- `row = n` must split evenly on the 1/12 grid. Otherwise the move is
  skipped (a failure under `--strict`): `5 frames can't share 1 beat
  evenly on the 1/12 grid: give timing =`.

### 2.2 How frames map to beat phase

- **Position.** u is the position in move beats. It is accumulated from
  the tracker's beat deltas (section 3.5), never from frame counts, so it
  holds at 10, 24 and 30 fps.
- **Which step shows.** Step i shows while `start_i ≤ 12u < start_{i+1}`.
  Step 0 starts at u = 0, the hit.
- **Must-show rule.** A step that starts on a whole beat is never skipped
  by a render less than ¼ beat late. The first render at or after whole
  move beat b draws u = b exactly when it comes at most ⅛ beat late;
  later than that, a step of at least ¼ beat still covers b on that
  render. A render ¼ beat or more late (a stall of 83 ms or more at pace
  180, 125 ms at 120) can miss a 3-tick step outright (E.2.7).
- **Hit warning.** A step that straddles a whole beat gets a compiler
  warning ("beat 2 lands inside step 2 (B4): no step starts on it; fine
  for a hold, otherwise set timing = so one does"). It isn't dropped,
  because holds are legitimate.

### 2.3 Offsets

- **Keys.** Each step may carry a key {x, y} in native px. The engine
  eases from that key to the next step's key with the step's ease: `hold`,
  `linear`, `in` (quadratic), `out` or `inout` (smoothstep). Eases are
  computed in Q16 fixed point, so Python and C++ agree to the pixel
  (section 10).
- **Loops.** The last step eases into the first key, so runs loop cleanly.
  The compiler warns when a beat move's first key isn't (0,0), because the
  dancer would jump when a different move follows.
- **Hops.** An `out` key followed by an `in` key traces an exact parabola,
  so two drawings already hop.
- **Sugar.** `bounce = n` adds an engine hop that lands on every move
  beat, on the crab's curve: 0.12 beat on the ground, then a parabola.
  `sway = n` adds a side-to-side motion with its extremes on the beats.
- **Rounding.** All motion is scaled to screen px and rounded half up
  (`crab::rnd`). `snap = yes` rounds to the native grid instead.

### 2.4 What a BPM range is compared against: the danced pace

**The device can't know a song's tempo.** `TempoFold` (DancePose.h) folds
87 and 174 BPM to the same T = 87, and a 60 BPM ballad tracked at 120
folds to 120. A stored BPM covers only 0.9-2.7 % of tracks. So a range is
defined as the move's **danced pace**: how many of the move's own beats go
by in a minute.

- **T** is the folded dance tempo (72-176), passed through a Schmitt
  latch. T is set at lock, and moves only when the live folded tempo is
  more than 3 % away from it.
- **The pace at octave k** is P = T·2^k.
  - k = −1 is half time: each move beat lasts 2 dance beats. It is allowed
    only for moves of 1-4 beats, so no single play is longer than 8 dance
    beats (6.7 s at T = 72).
  - k = +1 is double time, allowed only while 2T ≤ 180, for a running
    move too (section 2.5).
  - So the reachable paces are 36-180.
- **The test:**
  - An **unranged** move always uses k = 0, so half time never leaks into
    unranged moves.
  - A **ranged** move [lo, hi] is eligible at the first k with
    `T·2^k ∈ [lo, hi]`, trying, in order: the k of the last *ranged* move,
    if it fits; then 0; then −1; then +1. So consecutive ranged moves
    don't flip octave.
- **What this means for authors:** "90-130" means "this move looks right
  with its beats 90-130 a minute apart". A 170-180 move fires on an 87 BPM
  hip-hop track danced at double time exactly as on a 174 BPM drum & bass
  track, because the tracker can't tell the two apart.
- **What can't be expressed yet:** "only on slow songs" and "only on real
  174 BPM drum & bass" need the track's BPM. The key `song_bpm = lo-hi` is
  reserved: a move using it sets `needs` bit 0, and v1 skips it with
  `song_bpm needs track BPM data: skipped in this version`.

**Allowed values.** lo and hi are integers from 36 to 200, with lo ≤ hi. A
range that no reachable pace (36-180) meets drops the move: `range 185-200
can't be reached: the fastest pace is 180`.

### 2.5 Edges and running moves

- Eligibility is checked when a move or a run starts, and again whenever T
  moves (a re-acquire at a new tempo, section 3.5).
- If a running move's pace T·2^k leaves [0.97·lo, 1.03·hi], or a
  double-time move's 2T passes 180, it holds at its next whole move beat
  and the picker chooses again at the next whole dance beat (the same
  moment, except at k = +1, where the hold lasts up to ½ beat: E.2.4's
  CUT). The 3 % never relaxes the 180 cap of
  section 2.4, so no move dances faster than 180 (section 2.1's quarter
  beat of 83 ms). The cut move is exempt from no-repeat, so it can carry
  on at the new octave if one fits.
- So "never used outside its range" means "never danced more than 3 %
  outside it for longer than one move beat". The contributor guide says
  so.

### 2.6 What "no BPM" covers

Idle moves carry `when = quiet | listening | any`. The default is `any`.

- **`quiet`**: nothing is heard.
  - Paused, stopped, starved, or "no track".
  - `Pk0`: taps off, so the reader starves and `heard` is invalid
    (estimate, from TapReader's 60 ms rule).
  - Host mode with no epoch or a stale clock (`!heard.valid`).
- **`listening`**: audio is heard but there is no locked beat.
  - `fresh_` after a skip, an output switch or dance-on.
  - The epoch window.
  - No grid: the first ≥ 1.5 s, ambient music, or a grid dropped after 12
    weak beats while the audio continues.
  - An unlocked grid.
- **During the fade.** Idle frames also show whenever no run is showing:
  through the ~0.6 s fade-in after a fresh lock, and at w ≤ 0.5, once a
  run's play in progress has played out (3.2's show latch).
- **Missing idles.** If a pack has no idle move for the current state, the
  other state's idles are used. If it has none at all, an engine
  **auto-idle** plays: the rest frame with an engine sway of ±1 native px
  over 4 s.

### 2.7 Genre: a filter

- **Tags.** A move may carry up to 8 free-text tags
  (`genre = disco, funk`).
- **Folding.** The compiler and the device fold tags the same way
  (Appendix B.8 gives the exact order).
  - textfold's Full folding, lowercased; then `&`, `+` and a lone `n`
    between words become "and"; then only letters and digits are kept.
    The key is FNV-1a 32.
  - A tag that folds to empty (a script Full folding can't spell; see
    `sameName()`'s note in TextFold.h) skips the move in the compiler (a
    failure under `--strict`) and drops it on the device.
  - A track genre that folds to empty is ignored, so it can never match
    everything.
- **Matching.** A tagged move is eligible only when ANY of the track's up
  to 16 genres matches one of its tags. A match multiplies its weight
  by 3.
- **Today** no track's genre is known, so tagged moves are dormant.
  Untagged moves are always eligible.
- **Later (L1).** Aliases (dnb, d&b, drum n bass → drumandbass; rnb →
  randb) and families (electronic ⊃ house, techno, synthwave and so on)
  arrive with the genre provider. They are a table only, so the format
  doesn't change.

### 2.8 When the filters leave nothing

The pace and genre filters are **never** relaxed. Falling back to the
nearest range would break "never outside its range", and dropping the
genre filter would break "tied to genres". Only the soft rules give way,
in this order:

1. Phrase alignment and `place`.
2. Continuity (from/to) and recency, which are already preferences.
3. No-repeat.
4. If nothing is eligible, an engine-made **auto-bounce** plays: the
   pack's `rest` frame with a 1-beat hop of 4 native px.

So a pack needs only a rest frame to play at every pace and in every
state, and a tagged move never plays on a track that doesn't match.

### 2.9 Validation

**One move is dropped, never the whole pack, when:**

- its beats aren't 1, 2, 4 or 8, its ticks don't sum to 12·beats, or a
  step is under 3 ticks;
- an idle total or idle step is out of range;
- a frame reference is out of range, or the frame failed its own checks;
- lo > hi, a bound is outside 36-200, or the range is unreachable;
- an offset is outside ±48 screen px, or bounce, sway, repeat or travel is
  out of bounds;
- the ease is unknown, a string is overlong or not UTF-8, there are more
  than 8 tags or 32 steps, or a tag folds to empty;
- a `needs` bit this engine lacks is set;
- its budget is red (section 6.7) and it lacks `heavy = yes`. The device
  recomputes the budget itself at load, so it never trusts the compiler.

**The whole pack is rejected** for failures in the header, a chunk
(including a duplicated chunk tag), the geometry or the rest frame.

**The freeze move** is the move flagged `freeze`; otherwise the first
unranged 2-beat beat move; otherwise the first unranged beat move;
otherwise auto-bounce.

**How drops are reported:**

- **Compiler:** one status line per move, e.g. `dancer.ini:41: move
  "spin": steps last 45 ticks (3 3/4 beats); a 4-beat move needs 48
  (skipped)`. `--strict`, used for built-ins, turns a skip into a
  failure. Appendix C.6 lists every message.
- **Device:** logged once per load, e.g. `[dance] robo: move "spin"
  ignored: 45 of 48 ticks`.
- **Dancers page:** the row reads "9 moves (1 ignored)" once the pack has
  been loaded in this session.
- **Console:** `Dl` lists the moves.

## 3. Choreography

`lib/core/DanceChoreo.{h,cpp}`, host-tested.

### 3.1 Input and output

`DanceMode::render()` builds one `BeatInput` per frame, and the
Choreographer returns one `Cue`:

```
BeatInput {heard, grid, locked, confidence, trackerBeats (double), rawBpm,
           foldScale, restarts, acquisitions, dt, freeze}
Cue       {beatMove, u, beatPos16, k, beatMirror, idleMove, idleMs,
           idleMirror, idleSeconds, weight, energy, showDance, flash, spot,
           rootX, rootY, state}
```

`beatPos16` is B.4's pos16 for the beat move, floor(u·786432), carried
so a freeze at a step's start is exact (E.1). The only tracker change is a
new `BeatTracker::acquisitions()` counter, incremented in
`BeatTracker::acquire()`, the one function that acquires a grid.

**Determinism.** Identical `BeatInput` streams (and calls) give identical
cues on one build. The host and the device agree too, except where the
float weight lands within rounding of a threshold: the ESP32 fuses float
multiply-adds and its `expf` isn't glibc's (E.3). Two device runs differ
from each other anyway, because their inputs depend on frame times,
latency and epochs.

### 3.2 States

| State | Entered when | Behaviour |
|---|---|---|
| Quiet / Listening | section 2.6 | Idle moves at the current spot, picked by weight for the state, in runs of 1-3 loops with no immediate repeat. |
| Entering | w > 0.5 and a locked grid | Keeps idling (or holding a held step: a lost lock or a pause that ended while w > 0.5) until the next whole dance beat (≤ 1 beat, ≤ 833 ms), anchors the phrase there (origin A) unless the phrase still holds, then picks. The dancer comes in on the beat. If the weight is falling through 0.5 there (its target at 0.5 or below), it waits for the next beat instead, so a lock fading as it comes in never flashes the dancer in for a frame. |
| Dancing | | Picks a new run each time the current run ends, while w > 0.5. At w ≤ 0.5 (a locked grid with a weak beat) the play in progress shows to its end, then idle frames; the run plays out unseen and no run follows it (Entering brings the dancer back). |
| Leaving | lock lost, grid and audio still valid | Plays on to the next whole move beat and holds that step. Idle frames take over at w ≤ 0.5. |
| Freeze | `!heard.valid` (pause, starve) | Holds the step, the offset and the travel position at once. Quiet idles take over at w ≤ 0.5 (~0.3 s). |

**Which frames show.** Beat-move frames show while a run exists (Dancing,
Leaving, Freeze holding a beat step, or Entering holding one) **and** its
**show latch** is on; otherwise idle frames show. A run starts only with
w > 0.5, so it starts shown. The latch then changes only on whole move
beats: off where a play starts with w ≤ 0.5, and on again at any whole
move beat with w > 0.5. So beat and idle frames each last at least a move
beat however w wobbles about 0.5 (drawn frames can't blend as the crab
does), and a play once shown, a travel above all, is seen to its end:
the dancer never snaps from a walk back to its start spot, and since no
run starts while idle frames show, no travel plays unseen and moves the
idle dancer a spot. Without a lock the run itself ends at w ≤ 0.5 (U8),
so beat frames at w ≤ 0.5 only show on a locked grid, where DanceRate
runs at the full rate anyway (E.2.7).

- During Entering (w > 0.5, idle frames, ≤ 1 dance beat while the weight
  rises), DanceRate and
  beat_eval still count "dancing" by w > 0.5, so their metrics don't
  change.
- The beat light flashes on dance beats as today. During Entering it leads
  the dancer by at most one beat, which shows the beat has been found.

### 3.3 Timing and the phrase

- A run starts exactly where the previous one ended, at a whole dance
  beat, even if its first frame comes ~0.1 beat after it (that frame is
  drawn at u = 0 by the must-show rule of 2.2). Any overshoot carries
  into the next run.
- There are no bars. The **pseudo-phrase** position is p = (danceBeats −
  A) mod 8.
- A move lasting D dance beats may start only where p mod min(D, 4) = 0.
  If no candidate fits, alignment is relaxed (section 2.8).
- `place = start` restricts a move to p = 0.
- `place = end` (fills) allows D = 1 at p ∈ {6, 7}, or D = 2 at p = 6.
- A downbeat estimator, later, only has to move A.

### 3.4 Picking a run

A pick takes under 50 µs (estimate).

1. **Candidates:** beat moves that are valid, pass the pace test
   (section 2.4) and the genre filter, are aligned, and pass `place` and
   the travel rules (section 8).
2. **No repeat:** the last run's move is excluded in both orientations,
   unless that run was a single play of a mirrorable move; then its mirror
   twin is allowed at ×2, which pairs single plays left and right. If the
   set is empty, the last move may repeat, but only after alignment and
   `place` have given way (2.8's order; E.2.5's ladder).
3. **Continuity:** candidates whose `from` equals the previous `to` keep
   full weight; the rest get ×0.15. Mirroring swaps `left` and `right` in
   tags. If nothing matches, all candidates count equally and the dancer
   cuts directly. This is a preference, never a gate, so authors can't
   deadlock the picker.
4. **Recency:** a move used in the last 3 runs gets ×0.35.
5. **Draw:** weight × genre boost × travel bias (section 8), with
   xorshift32. The seed is FNV(track path) ^ restarts ^ acquisitions,
   re-seeded on every track change, gapless joins included.
6. **Run length:** `repeat` if set, rounded up so that repeat·D is a whole
   number of dance beats. Otherwise enough plays to fill 4 dance beats: a
   1-beat move ×4 at k = 0 and ×8 at k = +1; a 2-beat move ×2 at k = 0; a
   4- or 8-beat move ×1. A mirrorable move alternates M, M′, M, M′ within
   the run, the first orientation random. A travelling move plays once per
   run.

### 3.5 Position and discontinuities

**Position.** Each frame, `u += Δ(trackerBeats)·M`, where **M = S·2^k is
recomputed at every acquisition** (S is the current fold scale). The
move's pace therefore stays T·2^k whatever the tracker's octave. Within a
run u never goes below 0: a backward PLL nudge or a re-phase that would
take it under the run's start holds it at 0, because `frameAt` needs a
position ≥ 0 (Appendix B.4).

| Event | Detected by | Handling |
|---|---|---|
| PLL nudge (≤ 0.07 beat) | same generation | passes through |
| Re-acquire or jump | a new `acquisitions()`, or Δ < −0.1 or > 1.5 tracker beats (or S changing: the row below) | (1) Recompute M from the new S. (2) Compare in **dance beats**: the beat index restarts arbitrarily, so the new grid's dance-beat origin is chosen among the 1/S whole-tracker-beat shifts (when S < 1) as the one nearest the predicted dance position. (3) Within ¼ dance beat: the move keeps its whole-beat count and takes the new phase. Otherwise it holds its step until the new grid's next dance beat, re-anchors A there, and picks afresh. (4) If T moved, re-test the running move's pace (section 2.5). |
| Fold change (S ×2 or ×½) | the TempoFold scale changes: on a re-acquire at a new tempo, or by the PLL's drift alone, when a re-acquire kept the old scale near 72 or 176 and the ±4 % clamp carries the folded tempo across (5 such flips on the 77 cached tracks) | Handled as the row above, acquisition or not: the frame re-phases among the 1/S shifts, so the dance beats stay on the tracker's beats (a halved S is always within ¼; a doubled one may hold and re-enter). A nudge there would leave the clock's whole beats (S_old − S_new)·trackerBeats off, until the next acquisition. With M recomputed, an octave flip (I Wonder, 95.7 ↔ 191) leaves T and the pace unchanged, and a 4:3 or 3:2 re-acquire triggers the pace re-test. |
| Lost lock | `!locked`, heard valid | Leaving: the run is cut to its next whole move beat. If the lock returns before the run ends, it ends there and the next pick follows as usual; if it returns while a held step still shows (w > 0.5), a fresh pick comes at the next whole dance beat, the phrase continuing. Otherwise the dancer goes through Entering (E.2.3). |
| Grid dropped, audio valid | grid invalid | Leaving, then Listening idles (the "no grid" case). |
| Pause / resume | heard invalid / valid | Freeze. On resume the same grid applies at once, so Entering starts on the next beat and the phrase continues from A. |
| Track change with a restart (skip, seek, start, output switch, host epoch) | a tracker restart: `BeatInput.restarts` changes | The Choreographer's track reset (E.2.4): history, latch, phrase and beat move are cleared, and it reseeds. The idle clock and the spot carry on, so nothing jumps. |
| Gapless join | `DanceMode::onTrackChanged()` with no restart (a join keeps the epoch, so the lock carries through it) | `onTrack(pathHash, genres)`, which DanceMode calls on every track change: the new seed and the track's genres apply, and `Dg` is cleared. Phrase, latch and current move are kept. |

### 3.6 Weight and energy

- **The weight is unchanged:** `danceWeight(confidence)` while there is a
  locked beat, otherwise 0, eased with τ = 0.4 s. DanceRate (fps), the
  beat light (dance phi < 0.12 while dancing), test_dance_rate and
  beat_eval's `IDLE_WEIGHT` 0.5 keep their meaning.
- **Offsets, bounce and sway** scale by the energy e = 0.6 +
  0.4·clamp((w − 0.5)/0.5), so a fresh lock (w 0.65) hops at 72 %.
  Travel doesn't: it always completes its full distance. Idle moves are
  drawn at e = 1 (Appendix B.4). Beat frames the show latch keeps at
  w ≤ 0.5 (3.2) are drawn at e = 0.6. A held step (Leaving, Freeze) keeps the
  energy it had, so its offset holds too (E.2.4).
- **Palette:** the 32-step fade between the idle and dance palettes comes
  for free through the LUT (section 6.2).

### 3.7 Switching characters

- A switch takes effect at once while idle. While dancing it waits for the
  next whole dance beat (≤ 1 beat), and the new character starts a fresh
  pick there, at the current spot; a character without travel moves (the
  crab, stick-classic) starts at the centre instead (1.3, E.2.4).
- A card pack still loading takes over at the first dance beat after its
  load finishes. Waiting for the next move boundary could take up to 8
  beats after a tap.

### 3.8 Freeze

- `k<n>` (0-15) keeps its meaning: n/8 of a two-beat cycle at w = 1. It
  plays the character's freeze move (section 2.9) at u = n/8 move beats,
  taken mod L for a move of L beats. For the crab this is exactly
  `Crab::frozen`, so ENERGY's k0 measurement and the k0-k15 sheet stay
  valid. (A 1-beat freeze move would wrap at k8: the freeze chain prefers
  2-beat moves.)
- `Df` freezes, like `k`, are cleared on entering host mode, alongside
  `frozen_` in `DanceMode::setHost()`.

### 3.9 The console

It lands in step 4, on step 3's Choreographer and its hooks (E.2.8). `D`
is free today.

- `D` lists the cast; `Dc<id>` picks a character by id. Until the Dancers
  page (step 9), that is the only way to reach `stick-classic`.
- `Dl` lists the moves, with their labels, and whether and at which k each
  is eligible now.
- `Dm<name>` forces the next run.
- `Df<move>.<step>[m]` freezes a move at the start of a step, `m` meaning
  mirrored; `Df` alone unfreezes.
- `Dg<genre>` injects a genre for the current track (cleared on a track
  change).
- `Dv` toggles `[move]` lines.
- `Ds<spot>` moves the dancer to a spot (−1, 0, +1) for screenshots.

`[flash]` still counts tracker beats, so `usb_viz --flash` is unaffected.

## 4. Authoring format

### 4.1 What an artist does

1. `python dancepack.py new robo` creates `robo/`. It holds `dancer.ini`
   with three stub moves, a transparent `sheet.png` of the right size, and
   `guide.png`: an underlay showing the grid, the feet cross, the ground
   line, the toast band and the stage's top.
2. Draw one row per move in any editor that exports PNG with transparency:
   Aseprite, LibreSprite, Piskel, Pixelorama or GIMP. Editors without
   alpha, such as MS Paint, work with `key = #ff00ff` (section 4.2).
3. `python dancepack.py preview robo --watch` opens a browser page that
   redraws on every save (section 10.1).
4. `python dancepack.py build robo` writes `robo.dnc`. Copy it to
   `/dancers/` on the card.
5. On the player, hold the stage and pick "Robo". The choice is kept
   across boots.

### 4.2 The sheet

- One PNG of equal cells, with no padding, trimming or rotation. Cells are
  named like spreadsheet cells: `B4` is column B, row 4. The sheet is at
  most 1024×1024, with at most 256 cells used.
- **Accepted PNGs:** colour types 0, 2, 3, 4 and 6, at every bit depth
  PNG allows for them: 1-8 for indexed, 1-16 for grey, 8 or 16 for grey
  with alpha, RGB and RGBA (16 reduced to 8; Appendix C.4).
  - **tRNS** is honoured for types 0, 2 and 3.
  - Interlaced (Adam7) files are rejected with "save without interlacing".
- **Colour key.** `key = #rrggbb` makes that colour transparent. With no
  alpha, no tRNS and no key, the build fails with `every cell is fully
  opaque: export with transparency, or set key = #ff00ff (the top-left
  pixel of A1)`, naming the likely background. Otherwise every bounding
  box would be the whole cell and the budget red.
- **Export recipes**, in the guide:
  - Aseprite / LibreSprite: Export Sprite Sheet, By Rows, no trim.
  - Piskel: PNG with columns.
  - Pixelorama: Rows.
  - Paint: a magenta background and `key = #ff00ff`.
- Identical cells, and cells that are mirror images of each other about
  the anchor column, are stored once. A mirror bit in the step record
  handles the second (A.14).
- **`row = n`** takes row n's cells from A up to its last non-empty cell.
  Empty cells between drawn ones stay as blank frames, so deliberately
  blank frames are possible. Anything else uses an explicit `frames =`
  list.

### 4.3 Colours

- At most 15 opaque colours. Index 0 is transparent.
- Alpha ≥ 128 counts as opaque and < 128 as transparent. Any partial alpha
  gets a warning with its pixel count and cells.
- The engine's own colours (background 0x0862, ground 0x4208, shadow, beat
  light) are never in the pack, so the artist owns all 15 slots.
- **Order:** `palette = #rrggbb …` fixes the index order; otherwise it is
  the order of first appearance.
- **Merges and warnings:** colours that become the same RGB565 value are
  merged, with a note. A colour too close to the background gets a "will
  vanish" warning.
- **Too many colours:** an error that lists the 15 most used colours by
  pixel count and sums up the rest, with the cells where the rare ones
  sit (usually stray pixels, or an export with smoothing on).
- **Anti-aliasing** is allowed only as opaque colours pre-blended against
  #080C10 (0x0862).
- **Idle palette:** derived automatically, each colour taken 50 % toward
  its luma and then ×0.7 (estimate, to tune by eye against the crab's).
  `idle_palette = …` overrides it, which pre-blended ramps may need.

### 4.4 Scale, size and anchor

- `scale` is 1-4. A cell times its scale must be at most 160×160
  screen px, for example 80×80 native at 2×.
- `anchor` is the ground point in native px. The default is the bottom
  centre. The feet's last drawn row is anchor y − 1; the compiler warns
  when the rest frame's lowest row isn't, and names the anchor that would
  stand it on the ground.
- The cell must fit the stage: anchor y × scale ≤ 153, and at most 10
  screen px drawn below the anchor.
- Offsets move the whole drawing, feet included. The compiler warns when
  the feet drift more than 1 native px within a move that stays in place:
  it compares the feet of every step that stands on the ground (drawn
  down to anchor y, keyed at y 0) at its key, and where an ease glides it
  to the next key on the ground (Appendix C.6). To sway the body with the
  feet planted, draw the sway in the frames.
- The compiler also warns when a standing frame reaches above stage row 36
  (the toast band).
- `faces = none | right | left` (default none) says which way the art
  faces. Art facing right is mirrored to travel left (section 8).

### 4.5 The manifest (`dancer.ini`)

It is read with configparser, with `interpolation=None`, `strict=True`
(duplicate sections or keys are errors), `comment_prefixes=('#',';')` for
whole-line comments, and `inline_comment_prefixes=(';',)`.

- **Comments:** a `;` after whitespace starts an inline comment. `#` is a
  comment only at the start of a line, so `palette = #ff0000 #00ff00`
  reads correctly. A `#` after a value, with whitespace before and after
  it (`name = Robo  # my robot`), is part of the value, and the compiler
  warns. A line starting with `#` is a comment even when indented, so a
  colour list can't wrap onto a line of its own: keep `palette` and
  `idle_palette` on one line (the compiler stops at a wrapped one). A BOM
  and CRLF are accepted. Keys are case-insensitive, with `=` or `:`.
- **`bpm` is required on every move:** a range, `any`, or `none` (a no-BPM
  move). A typo can't silently turn a range into "any", because a move
  without `bpm` is skipped with an error line (a failure under
  `--strict`).
- **Unknown keys:** an **error** ("did you mean bpm?") when the key is an
  adjacent transposition of a known key (`bmp` → `bpm`) or close to one
  (difflib's ratio ≥ 0.75: `timming` → `timing`); in a move it skips the
  move. A weaker match (0.6-0.75: `name` against `frames` in a move) is a
  warning that names the key it may mean and ignores the line, as is any
  other unknown key, so newer manifests still build. Keys starting with
  `x-` are ignored silently. A section header that nearly reads `[move
  <name>]` (`[moves wave]`, `[move:wave]`) skips that move, and one that
  nearly reads `[dancer]` is an error.

```ini
[dancer]
name    = Stick
author  = IrosTheBeggar
license = GPL-3.0-or-later      ; SPDX id, required
sheet   = sheet.png
cell    = 64x76                 ; one cell, in sheet pixels
scale   = 2                     ; 128x152 on screen
anchor  = 32,72                 ; ground point (default: bottom centre)
rest    = A1                    ; thumbnail and fallback frame
shadow  = 40                    ; contact shadow width, screen px (0 = none)
format  = 1

[move bounce]
row    = 1                      ; row 1, evenly timed (8 frames over 2 beats)
beats  = 2
bpm    = any
bounce = 5                      ; engine hop landing on every beat (native px)
freeze = yes                    ; what k0-k15 shows

[move point]
frames = A4 B4 C4 B4
beats  = 4
timing = 1/2 1/2 1 2            ; beats per frame (fractions or decimals on the 1/12 grid)
offset = 0,0 2,-2 4,-3 2,-2     ; native px at each frame's start; glides to the next
ease   = inout                  ; one, or a list
bpm    = 90-130                 ; danced pace; the engine uses half or double time when that fits
mirror = yes
place  = start
; weight = 0.3   from = stand   to = stand   repeat = 2

[move grapevine]
row    = 7
beats  = 4
bpm    = 90-140
mirror = yes
travel = 1                      ; ends one spot to the right (left when mirrored)

[move head-nod]
row    = 9
beats  = 1
bpm    = 70-100
genre  = hip hop                ; only on hip-hop tracks (dormant until tracks carry genres)

[move sway]
frames = A12 B12 C12 D12 E12 D12 C12 B12  ; the hip sways in the drawing: the feet stay planted
bpm    = none                   ; a no-BPM move: plays when there is no beat to follow
timing = .5s .5s .5s .5s .5s .5s .5s .5s
when   = any                    ; quiet | listening | any
```

The other move keys are `weight`, `from`, `to`, `repeat`, `place` (`start`
or `end`), `freeze`, `heavy`, `snap`, `sway`, `travel_ease` and `start_at`
(section 8), `go` (which steps travel, section 8), `genre`, `when` and the
reserved `song_bpm`; the other `[dancer]` keys are `key`, `palette`,
`idle_palette`, `faces`, `url` and `version`.

### 4.6 Timing, credits, text and versions

- **Timing values.** Beat moves take fractions (`1/4`, `1/3`, `3/4`) or
  decimals. Decimals within 1/48 beat of the 1/12 grid are snapped, with a
  warning. No-BPM moves take `s` or `ms` values. The compiler echoes each
  move as a table (step, cell, duration, offset, ease), so misaligned
  lists are easy to see. A list of the wrong length skips the move (a
  failure under `--strict`).
- **Licence and credits.** `license` and `author` are required; `url` and
  `version` are optional. All of them show in the Dancers page.
- **Text the device can show.** The name and the author must be valid
  UTF-8 with no control characters. The compiler warns about characters
  outside the device fonts' coverage: dancepack.py embeds the covered
  code points, and `dancepack.py --check` verifies them against the VLW
  arrays in src/ui/VlwFonts.cpp (its four fonts cover the same 256 code
  points), so no `vlw_font.py` change is needed. On the device a label
  goes through TextFit, which replaces a glyph the font lacks with its
  TextFold look-alike, or "?" (TextFit.h), and every name is ellipsized
  by the usual fit rules.
- **Versioning.** `format = 1`. A manifest declaring a higher format than
  the tool knows is an error ("update dancepack.py"). In the binary,
  `minReader` covers changes that break meaning, and per-move `needs` bits
  let an older engine skip one move instead of the whole pack.

## 5. The pack on the device: MPDN v1

### 5.1 Layout

**One binary layout** serves built-ins (const arrays read in place) and
card packs. The magic is `MPDN`, after MPTH (the thumbnail cache), MPLI
(the library index) and feature/metadata's MPTG. It is little-endian, with
4-byte-aligned chunks. The header is 32 B:

| Bytes | Field |
|---|---|
| 0-3 | magic `MPDN` |
| 4-5 / 6-7 | `format` = 1 / `minReader` = 1 |
| 8-11 | total size (equal to the file size; card packs ≤ 131,072) |
| 12-15 | CRC-32 (zlib) of bytes 16..end |
| 16-17 / 18-19 | chunk count ≤ 16 / flags |
| 20-31 | reserved, zero |
| 32… | directory: `{tag[4], u32 off, u32 size}` per chunk |

### 5.2 Chunks

The compiler puts the directory, META, GEOM, PALS and THMB in the first
2 KB. Unknown chunks are skipped. A tag that appears twice rejects the
pack. Appendix A gives every chunk byte for byte; this is the summary.

- **META:** id ≤ 23 chars `[a-z0-9-]`, name ≤ 24, author ≤ 40, licence
  ≤ 40, url ≤ 80, version ≤ 16 (each a u8 length, the UTF-8 bytes and a
  NUL, so the device can point at them), and a u8 **declared move
  count** (unverified until a load).
- **GEOM:** scale, faces (0 none, 1 right, 2 left), cellW, cellH,
  anchorX, anchorY, shadowW, frame count, rest frame.
- **PALS:** 16 × RGB888 for dancing, then 16 for idle.
- **THMB:** a 40×40 4-bit thumbnail (800 B) in the dance palette.
- **FTAB:** 12 B per frame: the opaque bounding box x0, y0, w, h (u8,
  native); a u32 data offset; a u16 length; a u16 reserved.
- **FDAT:** per bounding-box row: `skip` (u8, leading transparent px), `n`
  (u8), then n tokens `(len−1)<<4 | index` (1-16 px each; index 0 is an
  interior gap).
- **STRS:** a 4-byte header (the pose-key count), then the pose keys,
  move names and genre tags (u8 length + UTF-8 + NUL, ≤ 63 B).
- **MOVS:** a u8 count, a u8 record length (≥ 32, so records can grow; the
  reader takes the first 32 B) and two reserved bytes. Each record holds
  the `MoveLabels` fields plus bounce, sway, travelEase, the first genre
  and the genre count, and the first step and the step count.
- **STEP:** 8 B: u16 frame (bit 15 = mirrored), u16 duration (ticks, or
  10 ms units for idle moves), i8 dx, i8 dy, u8 ease, u8 flags (bit 0
  `go`, section 8).
- **Reserved tags:** `STAG` (a backdrop or props) and `SPOT` (custom spot
  sets), skipped today.

### 5.3 Built-ins

`dancepack.py builtin` writes `lib/core/packs/StickPack.cpp`. It holds
`extern const uint8_t kStickPack[]` and `kStickPackFrameHash[]` (FNV-1a
per frame, for the tests), with each frame echoed as a text grid in
comments. The file carries the pack's licence as its SPDX tag and a
"Generated … Do not edit" header, as CrabArt.cpp and VlwFonts.cpp do. It
is parsed in place: no PSRAM, no card reads.

### 5.4 Validation

One parser, `dance::Pack::open(ptr, size, avail, Report*)`, serves flash
and card. `size` is the file's length and `avail` the number of bytes
actually present; it equals size for a full load. open() needs the whole
file: with avail < size it rejects the pack as `short read (<avail> of
<size> bytes)` before any other check (A.15's first row). All checks run
at load, never per frame:

- the magic, `minReader` ≤ 1, the size equal to the file length, and the
  CRC (skipped only by a test-only option, section 10.3);
- each chunk in bounds, aligned and not overlapping another, with no
  duplicate tags, and counts within the caps (256 frames, 64 moves, 32
  steps per move, 16 chunks);
- the geometry limits of section 4.4;
- each bounding box inside its cell;
- **a dry-run decode of every frame**: each row's skip + Σlen ≤ w, and the
  frame's bytes consumed exactly (about 1 ms per 7-8 KB of FDAT at 240
  MHz, estimate: every byte comes through an inlined region view, D.1);
- every rule in section 2, the device's own budget (section 6.7) included,
  with move-level failures dropping only that move.

The blitter still clips to the canvas, as defence in depth.

### 5.5 Card packs: where, and the boot scan

**Where.** `/dancers/*.dnc` at the card's root, outside `/music`, so the
library's signature is untouched. Names starting with `.`, and macOS `._`
files, are skipped. 8.3 names are recommended. With no card, LittleFS
`/dancers` works for development only, since releases never ship LittleFS.

**The boot scan** runs in `DanceMode::begin()`, after the library and
before anything plays:

- a readdir of at most 32 entries, keeping at most 16;
- for each pack, one open, one 2 KB read and a close (~5 ms each,
  estimate, from the 4.9 ms `.565` read);
- `Pack::scan(buf, avail = bytes read, size = file size)` runs these
  checks in this order, the first failure naming the row's reason (A.15's
  reasons, plus "header unreadable"):
  1. A.15's header rows without the CRC: `too short` (avail < 32), the
     magic, minReader, the total size against `size` (the file's) and
     the 131,072 cap, then the chunk count;
  2. the directory within avail (32 + 12·count ≤ avail), else "header
     unreadable";
  3. A.3, every entry, against the total size;
  4. the nine required tags (`no <tag> chunk`);
  5. META, GEOM, PALS and THMB within avail: off ≤ avail, then size ≤
     avail − off (in that order, so nothing wraps in 32-bit arithmetic),
     else "header unreadable";
  6. META (A.5), its strings read as at load (A.1);
  7. GEOM (A.6), every field in table order, the rest frame below the
     frame count; whether its box is empty needs FTAB, so open() checks
     it;
  8. PALS = 96 B and THMB = 800 B (`PALS: <n> bytes, not 96`, `THMB: <n>
     bytes, not 800`).

  The CRC, FTAB, the frames, STRS, MOVS, STEP and every move check run
  only in open(), at load;
- then a roster entry is `psramAlloc`'d: id, name, author, licence, file
  size, declared move count, the 800 B thumbnail, and a status.

### 5.6 The arena

- After the scan, **one double-ended PSRAM arena** is allocated, sized to
  the two largest packs that pass the scan (at most 256 KB; typically about
  60 KB). It is never freed.
- The resident pack sits at one end and a new pack loads at the other. Any
  two packs fit, because a + b ≤ L1 + L2. So the old dancer keeps dancing
  while a new one loads, and no large allocation ever happens after boot.
- **Placement.** The arena and the stage are allocated in
  `DanceMode::begin()`, which `setup()` runs after the audio backend and
  the library, so after the decoders' pinned low blocks (estimate), as for
  today's dancer sprite. What is checked is the decoder state's addresses,
  not the arena's (section 10.5). If the 10k-library FLAC bench shows
  per-track decoder state above 0x3FA00000, the arena is capped at the
  largest pack plus 4 KB. A load that doesn't fit beside the resident card
  dancer then swaps to the built-in `stick` for the ~1 s load.

### 5.7 Loading a pack

- **At boot:** if NVS names a card pack, it loads synchronously (≤ 128 KB
  at 1.2 MB/s ≈ 110 ms, with nothing playing).
- **When chosen later:** it loads on the loop task, with no new task stack
  (QueueSaver's pattern).
  - It reads one 4 KB slice per dancing frame, or two per idle frame, and
    only while `Core2AudioBackend::bufferedMsNow()` is at least 500 ms or
    nothing plays.
  - **Each slice opens the file, seeks, reads and closes**, so no FatFs
    handle is held between frames: `max_files` is 5 (`SD.begin()`'s
    default), shared with the track, the queue writer, opus.idx and the
    thumbnails.
  - A slice costs about 5.4 ms idle or 7 ms during an MP3, the open
    included (estimate). 128 KB takes about 1.1 s while dancing.
  - After the last slice comes `Pack::open()`: the CRC and every check of
    5.4 but the budget, B.11's start spots included (D.2), in one call on
    the loop task. That is about 5 ms for a typical 30 KB pack and about
    25 ms at 240 MHz (40 ms at 160) for one near 128 KB, the start-spot
    hook's few ms for a 64-move pack included (it reads each move's steps
    once, with `Pack::steps`'s one record decode; estimate): at worst one
    dropped frame of the old dancer, once per load.
  - Then the budget simulation runs in slices of simulated frames:
    `BudgetPass::run(kBudgetSliceFrames)`, 256 frames a dancing frame,
    about 1-2 ms at 240 MHz (estimate: ~1,200-1,600 cycles a frame), and
    it drops a red move with `Pack::dropMove` (D.2). A move is never one
    frame's work: it is up to 24 cases and 2,908 frames (an 8-beat travel
    move with mirror), ~15-20 ms on the ESP32. The test dancer's 13 moves
    are ~4,070 frames, 22 slices (under a second); the largest pack the
    format allows (62 such moves, 180,296 frames) takes 744 slices, about
    25 s at 30 fps, while the old dancer dances on (an idle frame, at
    10 fps, has room for a bigger slice). 10.5 times it. Then the swap
    happens at the next dance beat. The rail shows "Loading Robo…"
    meanwhile.
- **Interruptions:**
  - Leaving the Dance tab or the screen going dark pauses the load
    (DanceMode draws nothing). It resumes from the same offset on return,
    and the final CRC catches a card changed meanwhile.
  - Choosing another dancer mid-load aborts the load and starts the new
    one, while the resident dancer keeps dancing.

### 5.8 Failures

- A short read, a bad CRC, a failed validation or a pulled card aborts the
  load.
- It is logged once (`[dance] ignored /dancers/robo.dnc: bad checksum`)
  and the pack is marked bad for the session.
- The Dancers page shows the pack greyed out with the reason, and the
  current dancer stays. For an override, the built-in stays.
- With no PSRAM for the arena, card packs are listed as "no memory".
- A pulled card can stall a read until FATFS's 10 s timeout: the same
  exposure QueueSaver's loop-task writes already have.

**Not done on the device:** no PNG or JSON decoding. pngle needs about
43 KB of PSRAM, makes small internal mallocs, adds flash and widens the
attack surface.

### 5.9 Persistence

- NVS namespace `dance`, key `char`, holds the id string (≤ 24 B).
- It is a schema bump with a no-op migrate step and a new test_nvs_layout
  row, per NvsLayout.h's rules (`kCurrent` is 2 on main today). **The
  number is taken at merge time:** if feature/metadata or another branch
  lands its bump first, this one becomes the next number. That avoids a
  clash like LibraryIndex's `kVersion` 3/4 → 5.
- The key is written 5 s after the last switch, so cycling through
  characters costs one write.
- A card id missing at boot falls back to the default without overwriting
  the stored id, so the choice comes back with the card.

## 6. Rendering

### 6.1 The stage

**The stage** is one `M5Canvas`, 320×164 RGB565 in PSRAM: **104,960 B**,
68,960 B more than today's 120×150 RGB565 crab sprite. It is created once
in `DanceMode::begin()` and never recreated, which removes today's
per-skin delete-and-recreate and its "no PSRAM for the other format" path.

**If the stage can't be allocated** (only at boot, with ~2.6 MB of PSRAM
free, so unlikely), `ready()` is false, as today. DancePage's "No PSRAM
for / the dancer" (`DancePage::repaint()`) is drawn centred on the stage
rect from `DanceStage` instead of at `kBoxX`. There is no smaller fallback
stage.

Its geometry is a portable struct in `lib/core/DanceStage.h`,
`{x, y, w, h, groundY, spotX[3]}` (`StageGeom`, there from step 3: E.1):

- **Final:** `{0, 36, 320, 164, 153, {64, 160, 256}}`.
- **Legacy, for step 5's parity check (section 11):**
  `{100, 44, 120, 150, 145, {60, 60, 60}}`.

### 6.2 The blitter

`lib/core/DanceBlit.h`, portable:

- It writes byte-swapped RGB565 from a 16-entry LUT into
  `{uint16_t* px, w, h, stride}`, taken from `getBuffer()`.
- For each native row, the tokens decode into an index row of at most
  160 B on the stack, read backwards when mirrored. Non-zero runs then
  fill `scale` rows of run·scale px.
- There is no M5GFX per-call overhead and no IRAM. The same code renders
  into a test buffer on the host, so frame hashes are exact.
- The LUT is recomputed only when the 32-step palette fade moves.
- The crab and the classic figure draw through their own renderers
  (section 1).

### 6.3 Each frame

1. Compute the character's rect from the frame's bounding box at root +
   offset, plus the shadow.
2. The dirty set is the rect now united with last frame's. If the union is
   more than 1.3× the sum of the two (a switch across spots, a fast
   travel), they stay two rects.
3. Clear the dirty set to 0x0862.
4. Redraw the scenery clipped to it: the ground line at stage rows
   153-154, x 8-311 (608 px).
5. Draw the shadow: centred under the root, its width shadowW minus 2 px
   per px of hop, over rows 152-154.
6. Blit the character.

### 6.4 Offsets and travel

- Root x = spotX + travel(u) + round(scale·e·(eased offset + sway)), with
  the offset and sway negated when mirrored.
- travel(u) eases from the start spot to the end spot over the `go` steps
  (the whole move by default), with `travelEase`.
- Root y is computed the same way, from the ground, with the bounce's hop
  in place of the sway. Appendix B.4 is the exact integer form.
- A mirrored frame places x′ = cellW − 1 − x, with the anchor at cellW − 1
  − ax.
- The root is clamped so the bounding box stays on the stage.

### 6.5 Pushes

- Every dirty rect is pushed with `setClipRect` + `pushSprite`, in holds
  of **at most min(40 rows, 8,000 px)** per LcdLock: 25 rows at full
  width.
- **Bottom band first.** Holds go bottom-up, so the feet and the hit reach
  the glass first. The frame's aim (half the push time) then errs early on
  the head rather than late on the feet.
- A 128×152 rect becomes 4 holds of ≤ 5,120 px: 2.4 ms each idle, 4.1 ms
  with an MP3. The largest legal hold, 8,000 px, takes 3.8 / 6.4 ms,
  inside the 6 / 10 ms gate.
- Overlay clipping through `DanceView::setVisibleRows()` is unchanged.
  Nothing is pushed while the screen is dark.

**No whole-stage pushes in normal operation:**

- **A page repaint** (tab entry, a wake, or any overlay closing: the
  headerless Dance page repaints all of itself, through `Ui::uncover()`
  and `Page::repaintHeader()`'s default) has already cleared to the
  background. `enterClean()` adds only the ground line, the character rect
  and the beat light. **`enterClean()` also discards any pending returned
  rows.**
- **Returned rows** are a safety net for a cover that shrinks with no
  repaint after it. They are pushed on the next frame only if no `enter()`
  came in between, banded as above. Today every close path repaints, so
  this path should never run (estimate). A `[stage]` counter shows if it
  does.
- **A character switch:** the normal union of the old and new rects, or
  two rects.
- **Fallback only (`invalidate()`):** the full 52,480 px in 7 holds of
  ≤ 25 rows: 24.7 ms idle or 42 ms with an MP3 in all, which drops one
  frame. Each hold stays within the gate.

### 6.6 Budget

Measured where marked (MASCOT-POC.md), otherwise worked out from 0.47
µs/px idle and 0.8 µs/px with an MP3:

| Case | Dirty px | Draw idle / MP3 (ms) | Push idle / MP3 (ms) | Verdict |
|---|---|---|---|---|
| Crab on the stage | ~11,800 (meas.) | 3.6 / 6.1 (meas.) | 5.8 / 9.8 (meas.) + ≤ 0.2 banding | 30 fps at 240 MHz |
| Drawn stick, 2× | ≤ 13,000 typical; 16,000 cap | ~2.5 / ~4.5 (estimate; clearing PSRAM dominates) | 6.1 / 10.4; 7.5 / 12.8 at the cap | ~17 ms of 33 with an MP3; ≥ 23 fps at 160 MHz (estimate) |
| Drawn stick travelling | the union ~3 px wider per frame at 120 BPM (estimate) | as above | + ≤ 0.3 | as above |
| Classic stick, now RGB565 | ~10,800 | ≥ 8.3 / ≥ 15.2 (estimate) | 5.1 / 8.6 | ~25 fps with an MP3 (estimate), so out of the tap cycle |
| Card-load slice | — | + 5.4 / + 7 per frame | — | fits in 33 ms; at 160 MHz a load may cost a few fps for ~1 s |
| Card-pack validation (one-shot, 5.7) | — | open(): ~5 for a 30 KB pack, ~25 (~40 at 160 MHz) near 128 KB; then the budget in slices of 256 simulated frames, ~1-2 a frame (estimates) | — | once per load: near 128 KB, one dropped frame of the old dancer; the slices for under a second on a typical pack, ~25 s on the largest |

### 6.7 The per-move budget: one integer simulation

- `lib/core/DanceBudget.{h,cpp}` loops each move at a fixed pace and
  energy 1 through `frameAt` and the dirty-rect rules. It runs at 24 and
  30 fps, at both ends and the middle of the move's reachable pace range,
  and reports the median and peak dirty px.
- It works on bounding boxes only (no pixels), in Q16 fixed point.
- dancepack.py mirrors it, and **shared vectors**
  (`test/support/dance/BudgetVectors.h`: move, fps, pace, orientation,
  spot → median, peak, plus a frame-by-frame trace; Appendix C.8) are
  pinned in both test suites.
- It is not a Choreographer: no picking, no state machine.
- **Lights:**
  - **green:** peak ≤ 16,000;
  - **amber:** peak ≤ 20,000 ("may run 25-29 fps with an MP3");
  - **red:** above that. The move is dropped unless it is flagged `heavy`.
- **Where it runs:**
  - The compiler shows it to authors.
  - **The device re-runs it at load for card packs**, so a drop never
    trusts the file. A hostile pack setting `heavy` only lowers fps, since
    every hold is still ≤ 8,000 px.
  - Built-ins skip it at run time, because test_stick_pack pins them
    green.
- More than 300 runs in a frame gives a warning.

### 6.8 Memory

- **Internal heap: 0 B new.** Every heap allocation by dance code goes
  through `psramNew`/`psramAlloc`, whatever its size, because with
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` = 4096 a plain `new` of under 4 KB
  would land in internal RAM. That covers Character objects, cast and
  roster entries, and the thumbnail scratch. Dance code uses no
  `std::vector` or `std::string`.
- **.bss:** the Choreographer (history, latch, RNG, cue: 424 B on the
  ESP32; ≤ 1 KB by a static_assert, E.1) and the cast pointers live in
  the global DanceMode, budgeted at ≤ 1 KB and measured.
- **IRAM: 0 B.** No `IRAM_ATTR`; 5,176 B remain.
- **PSRAM:** the stage (105 KB), plus the arena (0 B with no card packs,
  otherwise typically ~60 KB and at most 256 KB), plus the roster
  (≤ 14 KB), plus one 3,200 B thumbnail scratch, plus the budget pass
  while a card pack loads (`BudgetPass`, 792 B: its move's steps, cases
  and the case in progress; psramNew'd, never in DanceMode's .bss).
  Built-in packs need none.
- **Flash:** about +20 KB of code and about +30 KB for the stick pack
  (estimate). New rodata can shift the MP3 hot path's cache sets (10 KB
  once moved decode from 3.8× to 5.0× realtime), so cache_guard and the
  `b<n>` bench are acceptance checks for every step that adds rodata
  (section 11).

### 6.9 The beat light

It moves out of the toast band into the rail: a 12×12 square at screen
(8,206)-(19,217). **DanceView owns that square.** It pushes the square
only when its colour changes, clipped to the visible rows like the stage.
The page's 2 Hz strip pushes never cover it.

- **Colours:** faint grey with no beat, amber while listening, green when
  locked, and cyan 0x07FF (the crab's beat dot) while dancing with dance
  phi < 0.12 (today's rule).
- **Host mode:** DanceView hides the light, and the rail's text owns
  x 8-311, so test_dance_texts_fit is untouched.

## 7. The Dance tab

The tab today (DancePage.cpp's header comment): the dancer's 120×150 box
at x 100-219, y 44-193, a BPM panel on the left, the dancer's name on the
right, the track below at y 196-239 with a 2 px progress line. The new
layout is Layout A (decision 1).

### 7.1 The stage

- Screen x 0-319, y 36-199: 320×164 = 52,480 px, 2.9× today's box.
- The ground stays at screen rows 189-190. Both of today's skins draw
  their ground line at box row 145 (`DanceView::drawFigure()`: the stick's
  groundY 142 + 3; `DanceView::drawCrab()`: `crab::kOriginY`). The stick's
  feet stand at 142, but its line is drawn at 145.
- Three spots, at screen x 64, 160 and 256 (section 8). The crab's box
  lands on stage (100,8), which is screen (100,44): the same crab pixels
  as today.
- The toast (y 36-71) covers stage rows 0-35. The stick's standing head
  sits below that band; only hops and raised arms reach into it while a
  toast is up.

### 7.2 The rail

The rail is y 200-239, 40 px. DancePage draws it through the 320×56 strip,
at most every 500 ms (`kPanelMs`), and only on a change. **It has no tap
targets.** DancePage.cpp:8-10 records why: the usability walk removed a
bottom tap zone because it sat right above BtnC. A touch on the rail does
nothing outside host mode.

- **Tempo chip, x 0-79:**
  - the beat light (section 6.9);
  - the BPM in Bold 16 (`Font::Bold`) at x 26, top y 204 ("128" or "--"),
    followed by a Small "bpm";
  - a 48×4 confidence bar at (26,229), green at ≥ 60 %, amber below.
- **Track, x 84-311** (228 px, a new `kDanceRailTrackW`): the title in
  Bold 16 at y 204 and the artist in Small at y 223, ellipsized; "Nothing
  playing" when nothing plays.
  - For 2.5 s after a switch it shows the dancer's name and position
    ("Robo · 3/5") in Bold, and "by Ana · hold for the list", "Loading…"
    or "2 moves ignored" in Small.
  - The name goes in the rail, not a toast: a toast would cover the stage
    and repaint the page on every switch.
- **Dropped:** the 2 px progress line. The tab bar's Now Playing hairline
  already shows progress.
- **Tested:** the new rooms and strings go in UiText.h, with fit tests in
  test_ui_library.

### 7.3 Switching

- A tap anywhere on the stage goes to the next character in the cycle
  (`Input::tapTick()`). A stage-wide target is immune to the touch skew.
- A horizontal flick (> 400 px/s) goes to the previous or next character.
- A 500 ms hold (`Input::holdTick()`, the double tick) opens the Dancers
  page.
- Re-tapping the Dance tab while on its root could also open the list, if
  the hold proves hard to discover (decision 11). DancePage doesn't
  override `Page::home()` today, so that tap is free.
- Console: `m` (unchanged) and `Dc<id>`.

### 7.4 The Dancers page

`PageKind::Dancers`, on the Dance tab's stack: a ListView with 42 px rows.

- **Thumbnails.** Each row's 40×40 thumbnail goes through
  `ListView::thumb(Row&, const uint16_t*)`, which takes big-endian RGB565
  and copies it into the row at once (`pushImage`).
  - So the page expands a pack's 4-bit THMB through its dance palette into
    the one 3,200 B PSRAM scratch just before each row is drawn.
  - Procedural characters are rendered once at boot into their own
    3,200 B: the crab at 1× (40×50 native) cropped to 40×40 around its
    body (estimate), and the classic figure at ⅓ scale (estimate).
- Each row shows the name, "by author · 9 moves · licence", and a card
  mark; or it is greyed out with the reason it was ignored. The move count
  comes from META until the pack is loaded.
- A tap selects the character and pops the page.

### 7.5 Keeping the beat across the page

Today a push would restart the tracker. `Ui::push()` calls
`page_->leave()` (Ui.cpp:249-255), and `DancePage::leave()` calls
`setActive(false)` (DancePage.cpp:46). On return, `setActive(true)` →
`follow()` sets `fresh_` and restarts the tracker, and the first lock on
music takes a median 9.7 s. The fix:

- `Page::leave(LeaveWhy)`, with `Push` (from `Ui::push()`), `Back` (from
  `back()` and `toRoot()`) and `Away` (from everything else: `showTab()`,
  `tapTab()`, `showLibrary()`, `suspend()` and the other tab changes). The
  default implementation calls today's `leave()`.
- `DancePage::leave(Push)` calls a new `DanceMode::setDrawing(false)`: the
  tracker, the taps and the Choreographer keep running and nothing is
  pushed. `leave(Away)` calls `setActive(false)` as today.
- `DancersPage::leave(Away)` calls `setActive(false)`. `leave(Back)` does
  nothing.
- `DancePage::enter()` calls `setDrawing(true)` when DanceMode is already
  active, so `setActive()`'s early return keeps the tracker. Then
  `enterClean()` runs.
- The hardware scroller is ended (`Ui::ensureScroller()`) before DanceView
  pushes again.
- The screen going dark over the Dancers page still turns DanceMode off,
  and `Ui::redrawAll()` turns it back on only under the Dance page; going
  back after a wake starts a fresh lock, as any wake on another page does.

**Host mode over the page.** `Ui::showDance()` will pop the Dance tab's
stack to its root before it shows the tab: `toRoot()` when the Dance tab
is showing at a depth > 1, and the same pop on the Dance tab's stack when
another tab is showing (each tab keeps its own stack, so `showTab(Dance)`
would otherwise bring back the Dancers page). The computer's visualizer
never starts under the list.

### 7.6 Overlays and the screen

- **Overlays.** `Ui::applyCover()` is unchanged. The toast clips the
  stage's top, sheets clip from below, and the rail sits under sheets as
  today's strip does.
- **Screen.** It keeps the normal timeout (decision 13). While it is dark
  nothing draws and DanceMode is off. On a wake the page repaints and
  `enterClean()` runs.

### 7.7 USB host mode

- A Down on the stage switches the dancer (`DanceStage::inStage()`
  replaces `DanceView::inBox()`).
- A Down on the rail, a tab, any button or PWR ends host mode, so
  `kVizHint` ("Tap a button or a tab to stop") stays true.
- The page doesn't claim holds in host mode, so a hold lifts as a slow tap
  and switches.
- The rail hides the chip and the beat light, and shows `kVizTitle` and
  `kVizHint` across x 8-311 (`kDanceBottomW`, 304). test_dance_texts_fit
  is untouched.
- USB-VISUALIZER.md: `uit160,120` still switches; the "ends" check moves
  from `uit40,120` to `uit160,220`.

### 7.8 Screenshots and `describe`

- `x` shoots a 160×164 window centred on the dancer's current spot,
  clamped to the screen (at the centre spot: screen 80-239 × 36-199). The
  readback is checked against the stage canvas at (0,36). It takes about
  6 s over the UART (estimate).
- `X` shoots the whole screen, checked against the same canvas.
- `describe` keeps its shape as `Dance: <id>, …`.

## 8. The spatial system

**The stage model.**

- A 1-D floor seen from the front, with **3 spots** at stage x 64, 160 and
  256 (anchor x) on ground row 153, 96 px apart.
- The edge spots just hold a 128 px cell: x 0-127 and 192-319. Five spots
  60 px apart would put a 128 px character half off the stage at the
  edges.
- Travel is counted in spots, never in px, so packs survive later stage
  changes.
- Start positions are **meanings**, not indices:
  `start_at = any | centre | side | left | right`, with left and right
  swapped when mirrored. A later stage with 5 spots still knows what
  "side" means.

**In format 1:**

- per move:
  - `travel` (i8, −2..+2 spots, negated when mirrored);
  - `travel_ease` (default `inout`);
  - `start_at`;
- per step: a `go` flag (the root moves only during these steps; by
  default the whole move);
- per pack: `faces` in GEOM (none, right or left: art facing right is
  mirrored to travel left);
- the chunk tags `STAG` and `SPOT`, reserved and skipped.
- Art is always drawn in place, treadmill style. The engine moves the
  root.
- There is no lane field. A depth axis would arrive as a `needs` bit or a
  new chunk, which format 1 already allows.

**Built now** (step 8, part of this release):

- **Three spots.** The Choreographer owns the dancer's spot, which
  persists across moves, idles, character switches and track changes (a
  switch to a character without travel moves resets it to the centre).
- **A travel move is eligible when:**
  - its destination spot exists;
  - the move's widest frame, with its offsets, fits on the stage there;
  - D ≥ 2 (Appendix B.7's eligible_k never picks an octave that breaks
    it);
  - it starts at p ∈ {0, 4};
  - its `start_at` matches.
- **Travel bias:** moves heading toward the centre get ×2. After a track
  restart, the first run gives them ×4.
- Idles play at the current spot. There is no "walk home", which would
  look like skating without walk frames. A travel is seen to its end once
  it shows, and none starts while idle frames show (3.2's show latch), so
  the idle dancer never jumps between spots.
- `stageX(from, to, p16, ease)` (B.4 step 8, at travel progress p16) is
  in lib/core from step 3 and tested. Two-rect dirty pushes are live
  (section 6.3).
- Non-travel moves at an edge spot are clamped (section 6.4). The
  compiler's coverage report flags moves whose widest frame doesn't fit at
  an edge spot: a travel move is then eligible only from the spots where
  it fits at both ends, and any other move is pushed inwards there
  (Appendix B.11).
- The crab and `stick-classic` have no travel moves and stay at the
  centre.

**Later (L2):**

- a walk-home idle (it needs walk frames, so the pose key `walk` is
  reserved);
- floor ticks marking the spots;
- duets: two actors on spots −1 and +1, mirrored, in one dirty-rect list;
- custom `SPOT` sets;
- a depth axis.

## 9. The stick figure's first set

### 9.1 How the frames are made

- `tools/stick_frames/runner.cpp` is built with g++ against lib/core, as
  beat_eval's runner is. It prints joints for:
  - the `bounce` keyframes, straight from `dance::dancePose(phi, odd)`
    with the hop removed (the hop becomes `bounce`);
  - new poses written as whole-degree joint angles plus hip height in
    `tools/art/dancers/stick/poses.ini`, with DancePose's bone constants
    and `dance::knee()` for the IK.
- So the kinematics exist once, in C++; nothing re-implements them in
  Python.
- `tools/stick_frames.py` (standard library) rasterises those joints
  crisply into an indexed `raster.png`:
  - capsules of radius 1.0 native px, with no anti-aliasing;
  - a head ring of radius 5.5 native with a 2 px wall.
- It then composites `retouch.png` over the raster to give `sheet.png`.
  `retouch.png` is an indexed overlay, transparent except for hand edits,
  with index 15 meaning "erase to transparent".
- **The source of truth is poses.ini plus retouch.png.** `sheet.png` is
  never edited by hand. Regenerating keeps the retouches, and `--check`
  compares the composite.
- `dancer.ini` is written by hand, as the annotated reference.
- The float-based `--check` runs locally only, as
  gen_resampler_tables.py's does.

### 9.2 The look

- A 64×76 native cell at 2× (128×152 screen), anchor (32,72), shadow 40.
- The bones are today's ÷ 2 × ~1.05 (estimate, tuned so the standing
  head's top sits at screen y ≥ 72, below the toast band). That is today's
  height (~115 px against ~110). The extra room is used sideways, by
  travel and wider arms.
- Palette:
  - 1: the body, #F0F0F8 (0xF79F, today's ink);
  - 2: far limbs, #A8ACC0, a depth cue for crossed limbs;
  - 3: the Dance accent, #F472B6 (0xF396), for the shoes and a headband.
- The idle palette is derived, and gives today's grey look.
- The new poses fix the known weaknesses (MASCOT-POC.md, "Screenshots"
  under the device results): the legs look the same at phi ¼ and ¾, and at
  phi 7/8 the raised arm brushes the head.
- Crisp 2× (decision 6): a 1× anti-aliased cell (104×128) would be no
  bigger on the screen than today's figure, and a poor template to edit by
  hand.
- The jumping-jack's arms stop at about 45° (estimate), so its peak stays
  under the 16,000 px green cap.

### 9.3 The moves

**Beat moves** ("bpm" is the danced pace; "any" means no range):

| Move | Beats | bpm | Steps (cells) | Mirror | Notes |
|---|---|---|---|---|---|
| bounce | 2 | any | 8 (8) | — | today's dance; `freeze`; bounce 5 |
| pulse | 1 | any | 4 (4) | — | squash, rise, air, stretch (dancePose at phi 0, 1/6, 1/2, 5/6); shares frames with bounce |
| step-touch | 2 | any | 4 (3) | yes | sway 3; from/to stand; in place |
| clap | 1 | 70-150 | 3 (3) | — | timing 1/4 1/2 1/4 |
| twist | 2 | 96-150 | 6 (4) | yes | |
| jumping-jack | 2 | 100-160 | 4 (3) | — | bounce 6 |
| knee-up | 2 | 110-180 | 6 (4) | yes | double time on ~90 BPM tracks |
| arm-wave | 4 | 60-100 | 8 (6) | yes | half time on fast tracks |
| point | 4 | 90-130 | 4 (3) | yes | the disco point, exactly as in the section 4.5 example; `place = start` |
| head-nod | 1 | 70-100 | 3 (3) | — | `genre = hip hop`: dormant; shows the field, testable with `Dg` |
| pose | 4 | any | 2 (2) | yes | `place = start`, weight 0.3, timing 1 3 |
| grapevine | 4 | 90-140 | 8 (6) | yes | `travel = 1`; `go` on steps 1-6 |
| chasse | 2 | 100-150 | 4 (3) | yes | `travel = 1`; a quick side gallop |

**No-BPM moves:**

| Move | When | Length | Steps (cells) | Notes |
|---|---|---|---|---|
| sway | any | 4.0 s | 8 (5) | today's idle: idlePose's hip sway drawn in the frames (0.5 s each), so the feet stay planted and there are no offsets; exactly as in the section 4.5 example |
| breathe | quiet | 3.0 s | 3 (3) | |
| look-around | listening | 3.0 s | 4 (3) | mirror |
| tap-foot | listening | 1.6 s | 4 (2) | 0.4 s each |
| wait | quiet | 6.0 s | 3 (3) | hands on hips, weight 0.5 |

**Totals.** About 80 cells, and about 60 frames after mirror and duplicate
removal. The pack is roughly 28-32 KB, at ~460 B per RLE frame (estimate).

**Coverage.** At least three unranged moves (`bounce`, `pulse`,
`step-touch`) are eligible at every pace, so auto-bounce never fires for
the stick. There are idles for both states, and two travel moves.

## 10. Tools and tests

### 10.1 Tools

All are standard-library Python 3.8+. Each `--check` exits 1 with
`stale: <path>`.

- **`tools/dancepack.py`**: one file a contributor can download alone. It
  includes its own PNG reader (with tRNS) and writer.
  - `new`: the template and the guide.
  - `check`: per-move status lines, the budget light (the DanceBudget
    mirror), and a coverage report ("tracked beat → dance tempo T → this
    move danced at pace P, octave k", plus which moves fit at the edge
    spots).
  - `build -o x.dnc`.
  - `info x.dnc`.
  - `builtin --out …`: the .cpp plus the frame-hash table. `--strict` and
    the licence allowlist apply.
  - `preview [--watch]`: a self-contained HTML page. The browser decodes
    the PNG and shows RGB565-quantised colours, each move looping at a
    pace slider with a click, travel across the three spots, the quiet and
    listening idles, the budget light, and every message with its ini
    line. `--watch` serves it through http.server and rebuilds on save.
  - `--check`: built-in staleness.
  - The preview has **no move picker**: frames, timing and the budget are
    exact; motion in the page's own JavaScript is approximate. A picker
    there would be a third implementation of the Choreographer.
- **`tools/dance_preview/runner.cpp`** (for maintainers; g++ against
  lib/core):
  - exact contact sheets per move from the real blitter and `frameAt`,
    which produce `docs/img/stick-moves.png`;
  - `--replay <audio>`, which drives the real BeatTracker from PCM as
    beat_eval does, synthesises a valid heard clock at a fixed fps, runs
    the real Choreographer, and draws a per-beat strip of move names,
    octaves and spots. It doesn't parse logs, which lack the heard clock
    and the acquisition counter.
- **`tools/stick_frames.py`** and its `runner.cpp`: the stick generator
  (section 9.1), run locally only.
- **`tools/test_dancepack.py`** (unittest):
  - every rejection, with its message and line;
  - the PNG colour types, bit depths, tRNS, `key` and the all-opaque
    error;
  - the ini rules: `bmp` → the error "did you mean bpm?"; a move without
    `bpm` → skipped; `license = X ; comment` reads as X;
    `palette = #ff0000 #00ff00` reads both colours; duplicate keys → an
    error;
  - empty-folding tags, uneven `row =` splits, and the bounds on bounce,
    sway, repeat and travel;
  - a byte-identical rebuild;
  - the shared eligibility cases (T, lo, hi, previous k → k) and the
    shared budget vectors, both also pinned in C++.

### 10.2 CI

The firmware workflow (`.github/workflows/firmware.yml`) gains three steps
before the host tests, right after `actions/setup-python`:
`python tools/crab_art.py --check`, `python tools/dancepack.py --check`,
and `python -m unittest discover -s tools -p "test_dancepack.py"`. All
are integer-deterministic and need no pip install.

### 10.3 Host tests

- **`test_dance_pack`:**
  - parses the fixture pack (`test/support/dance/TestPack.h`, compiled
    from `tools/art/dancers/test/`, which uses every field; Appendix
    C.10), included by relative path as test_ogg_opus includes
    `../support/OggWriter.h`;
  - one hand-built bad pack per rejection reason, duplicate chunk tags
    included, with test_dancepack.py's list of patches and exact reasons
    (A.3's and A.9's bounds where 32-bit sums would wrap, a token-less
    row with skip > w, string offsets at and far past STRS's end, the
    accepted token sequences and reserved bytes); the CRC; unknown
    chunks skipped; a `needs` bit skipping a move; open() with avail <
    size → `short read (<avail> of <size> bytes)`;
  - each limit on both sides of it with well-formed values: a string at
    its limit and one byte past it (a mutator that patches a length byte
    moves the NUL, so it never makes one), and 64 moves, 32 steps and 8
    tags accepted, one more of each not;
  - **scan mode** (5.5's order): META, GEOM, PALS or THMB pointing past
    `avail` → "header unreadable"; a THMB of 799 bytes → `THMB: 799
    bytes, not 800`; a missing GEOM → `no GEOM chunk`; a bad CRC still
    scans (only open() checks it);
  - **fuzzing that reaches the validators:**
    - every truncation, and 10,000 seeded single-byte flips, with **the
      size and the CRC recomputed after each mutation**;
    - a structure-aware mutator that edits directory offsets and sizes,
      FTAB boxes, offsets and lengths, FDAT token bytes, MOVS step indices
      and counts, STEP frame indices, and STRS lengths;
  - **the differential reader vectors**
    (`test/support/dance/ReaderVectors.h`, generated by dancepack.py,
    Appendix D.3): about 3,000 mutated packs, each with what
    dancepack.py's reader says of it, compared reason for reason and drop
    for drop;
  - **every pack above, the hand-built bad packs included, is read
    through region views**: each byte open() and scan() read comes
    through a view of the region it belongs to (the header, the
    directory, or one chunk), which counts a read outside it as a fault
    instead of reading (`Pack::regionFaults()`, D.1), and the tests
    assert none. So a reader that reads a string's length byte before
    checking its offset (A.1, A.11) fails the `bad name` rows even where
    the byte it reads is still inside the pack. Every pack is an
    exact-size heap copy, so ASan catches a read past its end. A
    test-only `kSkipCrc` option on `Pack::open` lets the structure-aware
    fuzz skip the CRC.
  - It also runs in a CI-only `native_asan` env on Linux g++ (D.5).
    `-fsanitize=address,undefined` in its build_flags reaches the compile
    and the link: SCons's ParseFlags puts any `-fsanitize…` flag in both.
    MinGW on the laptop has no ASan; a Docker command stands in.
- **`test_dance_blit`:** frame hashes against the fixture's table;
  mirroring; scales 1-4; edge clipping with a canary border; the byte
  swap; index 0 left untouched.
- Appendix D.4 makes both lists concrete.
- **`test_dance_choreo`:**
  - **pace:** eligibility at k ∈ {previous ranged, 0, −1, +1}; unranged
    moves always at k = 0; the 180 cap, for a running double-time move
    too (a 170-180 move danced at +1 from T10 880 is cut by a re-acquire
    at T10 907: pace 181.4 is inside its 3 % but past 180); half time
    only for ≤ 4-beat moves; the 3 % latch;
  - **an I Wonder trace** (95.7 → 191 → 95.7 re-acquires while locked): M
    recomputed, the dance-beat origin chosen among the shifts, and **the
    pace never more than 3 % outside the running move's range**;
  - **a 4:3 re-acquire trace:** the running move cut at its next whole
    move beat;
  - **states:** quiet (pause, `Pk0`) and listening (fresh, no grid, grid
    dropped while audio continues, unlocked); the idle fallback to the
    other state, then auto-idle;
  - **genre:** dormant, the match boost, and the ladder: never a
    non-matching tagged move, auto-bounce last;
  - **picking:** no-repeat, runs (repeat·D whole), mirror alternation, the
    from/to preference, alignment, `place = start` and `end` (D = 1 at
    6/7, D = 2 at 6);
  - **travel:** the destination exists, it fits, D ≥ 2, p ∈ {0, 4}, the
    centre bias, the spot kept through idles, travel at e < 1 still
    completing;
  - **determinism:** the same cues for the same seeded inputs; a gapless
    join (new seed and genres, phrase kept);
  - **discontinuities:** a re-acquire with re-phase and with hold;
    Leaving; Freeze on pause, and resume continuing the phrase; a track
    reset; Entering on the beat, with idle frames during Entering;
  - **timing:** every hit shown on the first render at or after its beat,
    at 24 and 30 fps, **across paces 36-180**;
  - **weight:** a change ≤ 1/12 per frame at 30 fps (≤ 0.1 at 24), and
    above 0.95 after 2 s; the energy mapping;
  - **switching** at the next dance beat; the freeze fallback chain.
- **`test_drawn_character`:** step lookup at exact tick edges (integer);
  Q16 ease values at 0, ¼, ½, ¾ and 1; mirror symmetry; rounding half up;
  the bounds holding every pixel; `stageX`.
- **`test_dance_budget`:** the shared vectors; the peak never under any
  single frame's own box.
- Appendix E.4 makes these three lists concrete.
- **`test_stick_pack`:**
  - no dropped moves; ≤ 64 frames; ≤ 40 KB;
  - feet on the anchor row in ground frames; the palette limits;
  - **every move's DanceBudget peak ≤ 16,000 px** at 24 and 30 fps across
    its reachable paces (36-180), travel moves included, from every start
    spot.
- **`test_dance_stage`:**
  - union versus split (the 1.3× rule);
  - the hold plan (≤ 40 rows and ≤ 8,000 px, bottom-up);
  - `enterClean` discarding returned rows;
  - the spots on the stage;
  - the stage's hit zone under the lab's skews;
  - no hit zone on the rail outside host mode.
- **`test_character_set`** replaces `test_skins`, moved out of
  test_crab_pose.cpp (`test_skins()`, :488): the default crab, the order,
  ids, the wrap, card entries appended, duplicate ids, overrides limited
  to drawn built-ins and applied only after a load.
- **Extended suites:**
  - `test_beat_tracker`: `acquisitions()` counts every new grid,
    re-acquires included (E.4);
  - `test_nvs_layout`: the new schema row;
  - `test_ui_library`: the rail strings;
  - `test_dance_rate`: a Pacer run driven by the Choreographer;
  - a `Page::leave(LeaveWhy)` routing test, if Ui's NavModel side proves
    host-testable (estimate).
- **Unchanged:** test_crab_pose's hashes, test_dance_pose, beat_eval and
  tools/test_usb_viz.py.

### 10.4 Log lines

- New lines never change `[dance]`.
- Dirty-area stats go on a separate
  `[stage] dirty=avg/max holds=n returned=n` line, printed with `s`.
- `[move]` lines appear only with `Dv`.
- So usb_viz's regexes are untouched.

### 10.5 Device checks

They run in silent mode (`z`) with the crab, stick and stick-classic.
Tracks: click120, HBFS (MP3), Testarossa (24/96 FLAC), and paused. Clocks:
240 MHz and `Pc160`.

- **fps** (from `[dance]`):
  - crab and stick: median ≥ 29 and p10 ≥ 27 at 240 MHz with an MP3;
    median ≥ 23 at 160. (A p10 gate of 29.5 would fail today's crab, whose
    p10 with an MP3 is 29.2.)
  - **stick-classic:** recorded, not gated (~25 fps with an MP3 expected).
- **draw/push:** the crab within ±10 % of 0.7.0; the drawn stick's draw
  ≤ 6.1 ms with an MP3 (the crab's); dirty px average and max, travelling
  included.
- **bus holds:** the dance pushes' SpiHoldStats maximum ≤ 6 ms idle and
  ≤ 10 ms with an MP3, including an `invalidate()` forced from the
  console.
- **soak:** 10 minutes of MP3 per character with 0 underruns, a ring
  minimum ≥ 1,400 ms, and internal RAM minimum ≥ 45 KB.
- **memory:**
  - the **decoder state addresses** logged by the existing `O`/`b` paths
    stay below 0x3FA00000;
  - the `b<n>` bench, run with a 10k library, a 256 KB arena (two 128 KB
    test packs) and 24/96 FLAC, compared with 0.7.0;
  - `[heap]` PSRAM free; IRAM unchanged; DRAM within +1 KB; cache_guard
    passing; the flash delta recorded.
- **card:**
  - loads during MP3 and 24/96 FLAC with 0 underruns, and fps ≥ 25 while
    loading;
  - `Pack::open()`'s time on a typical (~30 KB) and a ~128 KB pack, the
    budget's per slice (`kBudgetSliceFrames`) and for the worst move (an
    8-beat travel move with mirror: 24 cases, 2,908 frames), the whole
    pass on the largest pack, and `blit`'s per frame for the stick and a
    dense card dancer, at 240 MHz and `Pc160`: they replace 5.4's, 5.7's
    and 6.6's estimates (steps 2 and 3 counted instructions from the
    disassembly; nothing was timed on the device);
  - a load during a queue save and during a thumbnails job (FatFs
    handles);
  - a load paused by leaving the tab, and resumed;
  - a card pulled mid-load falls back;
  - a set of corrupt packs, a THMB pointing past 2 KB among them, rejected
    cleanly;
  - duplicate ids; an override that fails keeps the built-in;
  - the NVS restore and its fallback.
- **beat sync:** click120 still locks in ~2.6 s, with a ~2.7 ms median
  error.
- **screenshots:** the `x`/`Df` readbacks show 0 px different. For the
  crab's k0-k15 sheet, see steps 5 and 6 in section 11.
- **overlays:** no stale pixels after a toast, a sheet or a dialog; the
  `[stage] returned=` counter stays 0.
- **gestures,** with the console's scripted finger (`uit<x>,<y>` a tap,
  `uih<x>,<y>` a long press, `uis<x0>,<y0>,<x1>,<y1>,<ms>` a swipe), with
  the default calibration and under the lab's skews:
  - a flick goes to the previous or next dancer;
  - a hold opens the Dancers page, with the double tick;
  - a stage tap at x 300 still lands on the stage;
  - a rail tap does nothing.
- **The Dancers page keeps the lock:** on a locked track, hold, pick a
  dancer and go back. There must be no `[dance] tracker reset` or
  `[dance] off` line, the lock is kept, and the new dancer dances within
  one beat.
- **host mode:**
  - `uit160,120` switches; `uit160,220` and the buttons end it;
  - starting the visualizer with the Dancers page up pops to the Dance
    root (and with the Dancers page left under another tab);
  - a `Df` freeze is cleared on entry;
  - the beat light is hidden.
- **energy:** re-measure the Dance tab's USB mA idle and dancing (ENERGY
  item 8, now with banded pushes), and with the Dancers page up, where the
  taps and the tracker stay on (ENERGY item 9 assumed they go off when the
  page leaves).

### 10.6 Docs

- `docs/DANCE-SPRITES.md`: this design and the MPDN v1 spec.
- `docs/DANCERS.md`: the contributor guide, with:
  - a 15-minute walkthrough;
  - a timing picture;
  - **pace and octaves**, including what can't be expressed yet;
  - the running-move tolerance;
  - the no-BPM states;
  - genres (a filter, dormant today);
  - travel and spots;
  - the budget light;
  - **the system caps and why** (arena RAM, card load time, 4-bit pixels);
  - a glossary of every compiler message;
  - licences, and putting a pack on the card.
- `tools/art/dancers/README.md` (start here), `template/`, and
  `stick/README.md` (poses.ini plus retouch.png).
- Updates: MASCOT-POC, ARCHITECTURE (the lib/core diagram,
  `Page::leave(LeaveWhy)`), README (the tools, "make a dancer"),
  USB-VISUALIZER (the touch checks), ENERGY items 8 and 9 (k0, the taps
  over the Dancers page) and THIRD-PARTY-NOTICES (the stick art,
  GPL-3.0-or-later).

## 11. Build order

Commits follow the house series ("Dance sprites: …"). Each acceptance list
names the hooks it keeps: k0-k15, `x`, `[dance] on (crab)`, the
visualizer's touches. Every step that adds rodata to the image (4, 7 and
8) also needs cache_guard passing and the `b<n>` bench within noise of
0.7.0. Step 3 links nothing new but the tracker's counter (`lib_archive`
leaves its code out until step 4 calls it), so its check is E.6's build.

| # | Commit | Contents | Accepted when |
|---|---|---|---|
| 0 | Dance sprites: the design | this document, with the MPDN v1 spec | the user's decisions recorded (section 12) |
| 1 | Dance sprites 1: the compiler | dancepack.py (PNG with tRNS and key, the INI rules, validation, the MPDN writer, build, check, info, builtin, `--check`, `--strict`, the budget mirror and the coverage report), test_dancepack.py, the shared eligibility, fold and budget vectors (test/support/dance/), the test dancer (tools/art/dancers/test/) → the TestPack fixture (test/support/dance/TestPack.h), the CI steps; Appendices A-C are its contract | unittest green on 3.10 and 3.12; byte-identical output on Windows and in CI |
| 2 | 2: the reader and blitter | DanceRules.h (the limits, B.7's reach), DancePack (open with its report and the late-check hook, scan mode, region views), DanceBlit, Crc32, Fnv1a.h, GenreFold (B.8: a tag folding to nothing drops a move); dancepack.py's `scan_pack`, `read_pack` options and `reader_digest`, and the generated ReaderVectors.h; test_dance_pack (the fixture, the hand-built packs, scan mode, the truncations, the flips, the structure-aware fuzz, the vectors), test_dance_blit; the `native_asan` env and its CI step; Appendix D is its contract | the fuzz and every reader vector clean under ASan and UBSan (CI; the Docker command of D.5 on the laptop); every reader vector agrees with step 2's lists; the hashes equal the fixture's table; `pio test -e native`, the unittest suite and `--check` green; `pio run -e core2` builds with IRAM and DRAM unchanged |
| 3 | 3: the choreographer | DanceCharacter.h, DanceChoreo (the pace rules, M recomputed, spots), DrawnCharacter, DanceBudget with D.2's split (the start spots in open()'s late-check hook, every pack; the budget after open(), in slices of simulated frames through `Pack::dropMove`, card packs), a minimal DanceStage.h (the geometry and rects), `acquisitions()`; test_dance_choreo, test_drawn_character, test_dance_budget (all three under native_asan too); test_dance_pack also on ReaderVectors' full lists; Appendix E is its contract | all suites green; the shared vectors agree with Python, ReaderVectors' full lists included; beat_eval builds and scores identically (E.5: the runner's outputs equal HEAD's, byte for byte, its timing aside); E.6 |
| 4 | 4: characters, the cast and the console | CrabCharacter, ClassicStick (its id still `stick`), DanceCast replacing DanceSkin, DanceMode on the Choreographer with today's view, `onTrack` for gapless joins, **the `D…` console** (D, Dc, Dl, Dm, Df, Dv, Dg, Ds), Df cleared by host mode; test_character_set | on the device: the crab's k0-k15 sheet pixel-identical to 0.7.0; fps, logs and `usb_viz --measure` unchanged |
| 5 | 5: the stage | DanceStage.h's hold plan and hit zone (its geometry and rects came with step 3), the DanceView rewrite (one RGB565 canvas, the renderers, direct blit, holds ≤ 40 rows and ≤ 8,000 px bottom-up, `enterClean`, returned rows as a safety net) **at the legacy rect, with the beat dot still in the box**; test_dance_stage | **the crab's k0-k15 sheet pixel-identical to 0.7.0 over the whole 120×150 window**; draw/push ±10 %; holds ≤ 6/10 ms; the classic figure measured in RGB565; overlays; soak |
| 6 | 6: the Dance tab layout | the final stage geometry, the rail with no tap targets, the beat light in the rail (hidden in host mode), tap and flick, the host-mode rule, `x`/`X`, the "No PSRAM" message on the stage, the UiText rooms and tests | **the crab layer identical to step 5**, masking the old dot's square (box x 102-117, y 2-17, which `DanceView::drawDot()` clears around the dot at (110,10)) and the ground line's new pixels (screen x 100-103 and 216-219 on rows 189-190); host checks; fit tests; fps. Meanwhile a hold isn't claimed yet, so it lifts as a tap |
| 7 | 7: the stick figure set | the runner, stick_frames.py (raster + retouch), the art, StickPack.cpp, test_stick_pack, the contact sheet; the classic figure becomes `stick-classic` (reachable with `Dc`) | the fps gates; draw ≤ 6.1 ms with an MP3; budget ≤ 16K; soak; the flash delta; the user's visual review of each move through `Dm`/`Df` |
| 8 | 8: travel across three spots | the spots in DanceStage, `stageX`, travel eligibility and bias, two-rect pushes exercised, `x` following the spot, the coverage report's edge-fit column | grapevine and chasse travel on the device; dirty px and holds within the gates while travelling; no stale pixels at the edges; the crab unaffected |
| 9 | 9: choosing dancers | the NVS schema bump (its number taken at merge), the Dancers page with thumbnails, `Page::leave(LeaveWhy)` + `setDrawing`, `showDance()` popping the Dance tab to its root, hold → page, the optional tab re-tap (decision 11) | the choice survives a reboot; test_nvs_layout; a hold gives the double tick; **hold → pick → back keeps the lock**; host mode pops the page; ENERGY item 9 re-measured |
| 10 | 10: dancers from the card | the boot scan (with `avail`), the double-ended arena, the sliced loader (a reopen per slice, pause and resume), the device's own budget, failures, duplicates, overrides after load, greyed rows | 0 underruns with MP3 and 24/96 FLAC; loads during a queue save and a thumbnails job; a card pull; the corrupt set; the NVS card restore; the decoder addresses and the 10k-library bench |
| 11 | Dance sprites: the contributor kit and docs | `new`, `preview --watch`, the template and guide, DANCERS.md, the doc updates | the template builds and previews; then a dry run in which the user builds a dancer alone and loads it from the card (it doesn't block other steps) |
| 12 | Dance sprites: review fixes | | |

**A contract change scheduled by step 3's review** (before step 7's
visual review of the stick set, with its own vectors): B.3's sway is
negated with the orientation (B.4 step 6), so a mirrorable move with sway
whose plays alternate jumps by up to 2·sway·scale px at each play
boundary (the test dancer's step-touch: 12 px). Leave the sway unnegated
(it is symmetric: mirroring only shifts its phase) or flip its parity
when mirrored, in B.3/B.4, dancepack.py's frame_at and the generated
vectors together.

**Why this order:**

- The compiler lands before the blitter tests that need its hash table.
- The console lands with the Choreographer (step 4), so steps 5-8 can use
  `Df`, `Dm`, `Dg` and `Dc`.
- Stage parity with 0.7.0 is proven at the legacy rect (step 5), where it
  can hold exactly. After the layout change (step 6) only the crab layer
  is compared.
- The 128 px stick arrives only after the layout; it wouldn't fit the
  120 px box.

**Scope.** Steps 0-12 deliver the system plus the stick figure set,
including travel (step 8, so the wider stage is used) and card dancers
(step 10, so other people's characters reach a device without a firmware
rebuild).

**Later:**

- L1: the genre provider (an MPTG lookup, or a library.idx v7 byte shared
  with AutoDJ's U15), plus the alias and family table.
- L2: walk-home, floor ticks, duets, custom spot sets.
- L3: the web studio.
- L4: an Aseprite/LibreSprite JSON importer.
- L5: converting the crab to a pack, with a deliberate re-baseline of its
  hashes.
- L6: a downbeat estimator, which only moves A.
- L7: Layout B as an immersive toggle.
- L8: pushing a pack over USB.
- L9: energy-aware move choice.
- L10: `song_bpm`, once tracks carry a BPM.

## 12. Decisions

**Before this design:**

- **The standard character is drawn frames plus engine-applied motion**
  (beat-timed frames, per-frame offsets the engine eases between). Not
  frames alone, nor C++ per character: two drawings and two keys already
  hop (section 2.3), and contributors draw rather than code.
- **Code-driven characters (the crab, stick-classic) stay as a second
  class behind the same interface.** Not converted to packs now (L5): the
  crab's hashes, its k0-k15 sheet and the classic figure's lines stay as
  they are.

**The user's decisions (2026-10-07):**

1. **Layout A:** a 320×164 stage at y 36-199 and a 40 px two-line rail at
   y 200-239 with no tap targets. Not B, an immersive 320×204 stage with
   the tempo and "Title · Artist" composited into the sprite (130,560 B;
   text redrawn whenever the dirty rect crosses it, ~1-3 ms a line); nor
   C, a 320×176 stage (y 36-211) over a 28 px one-line rail, the ground at
   row 201 (12 more rows, but the crab off its pixels and a new host-mode
   text). A keeps the crab pixel-identical, the host texts and their test,
   and two lines of track info.
2. **The 2 px progress line is dropped.** Not kept: the tab bar's Now
   Playing hairline already shows progress.
3. **A BPM range is the move's danced pace, the engine choosing an octave
   per move; `song_bpm` is reserved (its `needs` bit) for when tracks
   carry BPM.** Not the folded tempo taken literally (ranges only 72-176,
   the same ambiguity): the device can't tell 87 from 174 BPM, and stored
   BPM covers under 3 % of tracks.
4. **Genre is a filter:** tagged moves dormant until tracks carry genres,
   never relaxed. Not a preference (tagged moves anywhere, winning on a
   match): "tied to genres" means a filter.
5. **Move lengths are 1, 2, 4 or 8 beats.** Not any whole number to 8:
   powers of two keep the phrase grid.
6. **The stick figure is crisp 2× pixel art at about today's height.** Not
   1× anti-aliased: about 5× the flash, no bigger on the screen, hard to
   edit by hand.
7. **`stick-classic` is in the Dancers list only.** Not in the tap cycle,
   nor retired: it is likely to miss 27 fps with an MP3 (~25 expected).
8. **The first-boot dancer is the crab; the choice is remembered in NVS.**
   Not the new stick: the crab is today's default, and no card pack can
   override it.
9. **The manifest is INI (`dancer.ini`).** Not JSON: no brackets or commas
   to get wrong, and configparser is in the standard library.
10. **Contributors use `dancepack.py` (standard-library Python) now, and a
    web studio later (L3).** Not the device compiling PNG folders: a PNG
    decoder, a second validator and more flash.
11. **Tap = next, flick = previous or next, hold = the Dancers list;**
    re-tapping the Dance tab can open the list later if the hold proves
    hard to discover. Not a rail button above BtnC (the usability walk
    removed exactly that, DancePage.cpp:8-10), nor tap and the list only.
12. **In USB host mode a stage tap switches the dancer.** Not ending the
    session: the rail, a tab and the buttons still do, so `kVizHint` stays
    true.
13. **The screen keeps its normal timeout.** Not a setting that keeps it
    lit while locked on the Dance tab: it costs power.
14. **Built-ins are limited to the GPL-3-compatible allowlist**
    (GPL-3.0-or-later, GPL-2.0-or-later, CC0-1.0, CC-BY-4.0, CC-BY-SA-4.0,
    MIT, Apache-2.0); **card packs may declare any licence, shown in the
    picker.** Not card packs held to the same list: only built-ins are
    compiled into the GPL-3 firmware.
15. **A running move whose pace leaves its range is cut at its next whole
    move beat.** Not finishing it (up to 8 dance beats outside), nor
    cutting at once (a visible jump): after a 4:3 or 3:2 re-acquire (~3 %
    of locked beats), "never outside its range" then holds within 3 % and
    one move beat.
16. **The system caps stay as planned:** 64 moves, 256 frames, 32 steps
    per move, 128 KB per pack, 15 colours, 16 card packs listed. Not 128
    moves (no cost beyond MOVS's size): the arena holds two packs in
    ≤ 256 KB, a 128 KB pack loads in ~1.1 s while dancing, pixels are
    4-bit, and DANCERS.md explains the caps.
17. **Card dancers ship in the same release as the stick set.** Not later:
    without them only firmware rebuilds can add characters.
18. **The stick travels between three spots in this release (step 8), at
    today's height.** Not a taller ~140 px stick (its head under the toast
    for 1.8-4 s whenever one shows; ~+50 % dirty px, ~19K, amber; ~+5 ms
    push with an MP3, so 25-29 fps), nor Layout C's lower ground (+12
    rows, the crab off its pixels).

## 13. Risks

1. **Frame rate and SD-bus regressions,** worst at 160 MHz with an MP3.
   Built-in moves are capped at 16K dirty px by a host test, and card
   moves by the device's own budget at load. Every hold is ≤ 40 rows and
   ≤ 8,000 px (≤ 6.4 ms with an MP3), bottom-up, and normal operation
   never pushes the whole stage. Step 5 proves parity before any layout
   change, and every step has fps, hold and underrun gates.
2. **PSRAM placement slowing the decoders.** Only two large blocks are
   added, the stage and the arena, both allocated at boot and sized to
   need. The check is on the decoder state's addresses and the 10k-library
   FLAC bench, not on the arena's. A capped arena is the fallback;
   built-in packs live in flash.
3. **Hostile or corrupt card packs.** One parser serves flash and card,
   with a scan mode bounded by the bytes read, a dry-run decode, caps, the
   CRC and size limits. A structure-aware fuzz with the CRC recomputed
   reaches every validator, with ASan in CI. A failure means one log line,
   a greyed-out row, and the current dancer (or the overridden built-in)
   staying.
4. **Tracker errors made visible by strong hits.** 13.5 % of locked beats
   are off the beat, and a lock lingers 1.1 s after the music stops. The
   dancer dances only while locked, scales its energy at low weight,
   leaves on a lost lock and freezes on pause.
5. **Grid jumps** (re-acquires, octave flips). M is recomputed per
   acquisition, the phase is compared in dance beats with the origin
   chosen among the shifts, and the pace is re-tested. I Wonder and 4:3
   traces are pinned in test_dance_choreo.
6. **The BPM rule misunderstood.** It is called "danced pace" everywhere.
   The coverage report and the preview show tracked → T → pace, the guide
   says what can't be expressed yet, and `song_bpm` is reserved.
7. **The Dancers page silently restarting the tracker** (`Ui::push()` →
   `leave()` → `setActive(false)`). Contained by `Page::leave(LeaveWhy)`
   and `setDrawing`, with a device check that no reset line appears.
8. **Choreography that reads as random or monotonous.** Runs, mirror
   alternation, phrase alignment, `place`, recency, the from/to preference
   and the travel bias; the PCM replay runner checks it on real tracks.
9. **Broken hooks and tests.** Kept: CrabPose, CrabArt, DancePose,
   danceWeight, DanceRate, k0-k15, the `[dance]`/`[beat]`/`[flash]`
   formats and the ids. Migrated in named steps: test_skins, `x`, the
   visualizer's "ends" touch, and the k0-k15 parity mask in step 6.
10. **Loop-task card reads.** 4 KB slices with a reopen per slice, only
    with ≥ 500 ms buffered, paused off the tab, aborted on the first
    error. The 10 s FATFS stall is the exposure QueueSaver already has.
11. **Tool drift.** The budget and eligibility rules are pinned by shared
    vectors in both languages. Exact pixels come only from the C++ runner,
    and the preview's motion is labelled approximate.
12. **Float non-determinism in the stick generator.** Its output is
    committed, retouches live in an overlay, and its check runs locally.
    CI checks only the integer compiler.
13. **Flash, cache-layout and licence creep.** Built-in art is capped at
    about 300 KB. cache_guard and the `b<n>` bench gate every rodata step,
    and the flash_guard delta is recorded. `--strict` enforces the licence
    allowlist.
14. **Internal RAM and IRAM creep.** All dance allocations go through
    `psramNew`, .bss is budgeted at ≤ 1 KB, there is no `IRAM_ATTR`, and
    both are measured in steps 4-10.
15. **Scope creep** (genre, song BPM, duets). Reserved fields, `needs`
    bits and null providers: none of it needs a format change.
16. **NVS schema clashes across branches.** The number is taken at merge
    time, with a no-op migrate.
17. **Contributors put off by Python.** One file, a template with a guide,
    a watch preview, card loading in the same release, and a web studio
    (L3) next.

## Appendix A. MPDN v1, byte for byte

Appendices A-C are the contract for step 1. Three parties work from them
alone and must agree byte for byte: the compiler (tools/dancepack.py),
its unittest suite, and the C++ reader and engine of steps 2-3. Every
rule is integer-exact. Where they and the sections above differ, the
appendices win.

### A.1 Conventions

- **Little-endian.** Fields are unsigned unless marked `i8` (two's
  complement).
- **Offsets.** The header's and the directory's count from the pack's
  first byte; FTAB's data offsets from FDAT's first byte; string offsets
  from STRS's first byte.
- **A string** is a u8 length n, then n bytes of UTF-8, then a NUL: n + 2
  bytes in all. The NUL isn't counted in n, and no byte inside is 0, so
  the device can point at the bytes as a C string. **Valid** means strict
  UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF: what
  Python's `bytes.decode("utf-8")` accepts) with no control character
  (U+0000-U+001F, U+007F-U+009F).
- **Reading a string** at p inside a region that ends at e (a chunk's
  end) checks, in this order: p + 2 ≤ e (so the length byte n at p, and
  at least the NUL's place, lie inside); then n ≤ e − p − 2; then the
  byte at p + 1 + n is 0; then none of the n bytes is 0; then the text
  is valid, and within its field's limit. No byte is read before the check
  that puts it inside the region, so a 32-bit reader neither reads past
  the chunk nor forms a sum that wraps. The string ends at p + 2 + n,
  where the next one starts (it can equal e). A.5 and A.11 read every
  string this way.
- **Padding.** Every chunk starts at the first multiple of 4 at or after
  the previous chunk's end, and the file ends at the first multiple of 4
  at or after the last chunk's end. Padding bytes are 0. A chunk's size
  in the directory never includes its padding.
- **Determinism.** The bytes are a pure function of the dancer folder's
  dancer.ini and sheet pixels, and of the folder's name (the id): no
  time, no path, no locale, no set or hash order. Two PNG files with the
  same pixels give the same pack.

### A.2 Header (32 B)

| Offset | Type | Field | The compiler writes | The reader requires |
|---|---|---|---|---|
| 0 | 4 B | magic | `MPDN` | exactly that |
| 4 | u16 | format | 1 | nothing (informational) |
| 6 | u16 | minReader | 1 | ≤ 1 |
| 8 | u32 | total size | the file length, a multiple of 4 | equal to the file length; ≤ 131,072 |
| 12 | u32 | CRC-32 | over bytes 16 .. size − 1 | a match (skipped only by the test-only `kSkipCrc`) |
| 16 | u16 | chunk count | 9 | 1-16, and size ≥ 32 + 12·count |
| 18 | u16 | flags | 0 | nothing (ignored) |
| 20 | 12 B | reserved | 0 | nothing (ignored) |

The CRC is zlib's CRC-32: the reflected polynomial 0xEDB88320, initial
value 0xFFFFFFFF, final XOR 0xFFFFFFFF, as Python's
`zlib.crc32(data) & 0xFFFFFFFF`. It covers the chunk count, the flags,
the reserved bytes, the directory, every chunk and all padding.

### A.3 Directory

At byte 32: chunk-count entries of 12 B, `tag` (4 bytes, ASCII),
`u32 off`, `u32 size`. The reader checks every entry, known or not:

- off is a multiple of 4;
- off ≥ 32 + 12·count, then off ≤ total size, then size ≤ total size −
  off, checked in that order: a 32-bit reader never forms off + size
  before it knows that sum can't wrap (an off near 2^32 fails the second
  test, not the third);
- no tag appears twice;
- no two chunks of non-zero size overlap ([off, off + size) intersecting;
  the bounds above keep each sum ≤ 131,072). A zero-size chunk overlaps
  nothing.

Then all nine tags below must be present; unknown tags are skipped.
`STAG` and `SPOT` are reserved and skipped today.

### A.4 What the compiler writes, and where

- **Order:** META, GEOM, PALS, THMB, FTAB, STRS, MOVS, STEP, FDAT; the
  directory lists them in the same order. With 9 entries the directory
  ends at byte 140, where META starts.
- **The first 2 KB** then always holds the directory, META, GEOM, PALS and
  THMB: at most 140 + 236 + 12 + 96 + 800 = 1,284 B.
- A chunk with nothing in it (STEP with no moves) has size 0 and sits at
  the current offset.

### A.5 META

The fields in this order, with nothing between them:

| Field | Type | Limit |
|---|---|---|
| id | string | 1-23 B, `[a-z0-9][a-z0-9-]*`: the dancer folder's name (C.1) |
| name | string | 1-24 B |
| author | string | 1-40 B |
| licence | string | 1-40 B (an SPDX id or expression; any text for card packs) |
| url | string | 0-80 B |
| version | string | 0-16 B |
| declared move count | u8 | the MOVS count |

The reader reads the fields in order, each string as A.1 says, from the
chunk's first byte with the chunk's end as e. Each must lie inside the
chunk and be a valid string within its limit, and the id must match its
pattern; the count is the byte where the version ends, and must lie
inside the chunk too.
Bytes after the count are ignored, so a later format can append fields.
The count isn't checked against MOVS.

### A.6 GEOM (12 B)

| Offset | Type | Field | Values |
|---|---|---|---|
| 0 | u8 | scale | 1-4 |
| 1 | u8 | faces | 0 none, 1 right, 2 left |
| 2 | u8 | cellW | native px, 1 .. 160 // scale (cellW·scale ≤ 160) |
| 3 | u8 | cellH | native px, 1 .. 160 // scale (cellH·scale ≤ 160) |
| 4 | u8 | anchorX | the ground point's column, 0 .. cellW − 1 |
| 5 | u8 | anchorY | the ground row, 1 .. cellH; the feet's last drawn row is anchorY − 1 |
| 6 | u8 | shadowW | screen px, 0-160 (0 = none) |
| 7 | u8 | reserved | 0 |
| 8 | u16 | frame count | 1-256, equal to FTAB's records |
| 10 | u16 | rest frame | bits 0-14 the frame, bit 15 mirrored; the compiler always writes 0 (A.14) |

The reader requires size ≥ 12 (more is ignored), every value in range
(byte 7, reserved, is ignored), anchorY·scale ≤ 153 (the cell's top on
the stage) and
(cellH − anchorY)·scale ≤ 10 (the cell's rows at and under the ground
row, which is stage row 153 of 0-163), and a rest frame below the frame
count whose box isn't empty. Any failure rejects the pack.

### A.7 PALS (96 B)

16 RGB888 entries for dancing (entry i at byte 3i: r, g, b), then 16 for
idle (at 48 + 3i). Entry 0 of each half is index 0, transparent, and is
written 0, 0, 0; so is every entry past the last used index. The reader
requires size = 96 and checks no values.

### A.8 THMB (800 B)

A 40×40 picture of the rest frame, 4 bits a pixel: 40 rows of 20 bytes,
two pixels a byte, the high nibble the left pixel (CrabArt's packing).
Values are indices into PALS's dance half; 0 is the row's background.

It is made from the rest frame's cell grid as stored (unmirrored), within
its box (x0, y0, w, h), with integers only:

- **max(w, h) ≤ 40:** m = 40 // max(w, h); each native pixel becomes an
  m×m block; tw = w·m, th = h·m.
- **max(w, h) > 40:** s = (max(w, h) + 39) // 40; tw = (w + s − 1) // s,
  th = (h + s − 1) // s. Thumbnail pixel (i, j) covers the box pixels
  [i·s, min(i·s + s, w)) × [j·s, min(j·s + s, h)) and takes the most
  frequent non-zero index among them (a tie goes to the lower index), or
  0 when they are all transparent.
- It is placed with its top-left at ((40 − tw) // 2, (40 − th) // 2);
  every other pixel is 0.

The reader requires size = 800 and checks no values.

### A.9 FTAB (12 B a frame)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | x0: the box's left column in the cell |
| 1 | u8 | y0: its top row |
| 2 | u8 | w |
| 3 | u8 | h |
| 4 | u32 | off: the frame's data, from FDAT's start |
| 8 | u16 | len: its bytes |
| 10 | u16 | reserved, 0 (ignored) |

The box is the tightest one around the cell's opaque pixels. A blank
cell (every pixel transparent) has x0 = y0 = w = h = 0, off 0, len 0.

The reader requires size = 12 × the frame count, and for every frame:
w = 0 exactly when h = 0 and exactly when len = 0; x0 + w ≤ cellW and
y0 + h ≤ cellH; off ≤ FDAT's size, then len ≤ FDAT's size − off (in that
order, so a 32-bit u32 + u16 sum never wraps); and the dry-run decode of
A.10. The box needn't be tight: only the compiler's is.

### A.10 FDAT

The frames' data back to back in frame order, with no padding between
frames. A frame is h rows, from y0 down. A row is `u8 skip`, `u8 n`, then
n tokens, each a byte `(run − 1) << 4 | index`: run is 1-16 px, and index
0 is a transparent gap.

**The compiler's encoding** (the canonical one, so the bytes are fixed):

- skip is the row's leading transparent pixels inside the box. An
  all-transparent row inside the box is `0, 0`.
- The tokens cover the row from skip to its last opaque pixel; the
  trailing transparent pixels aren't encoded.
- Each maximal run of one index (gaps included) is split from its left
  into tokens of 16 px and a remainder: 37 px is 16, 16, 5.

**The reader's dry-run decode:** for every row, the two bytes and the n
tokens lie inside the frame's len, and skip + the sum of the runs ≤ w,
a row with no tokens included (its skip ≤ w); after the last row exactly
len bytes have been read. Any other token sequence that meets those
rules is accepted: a leading or trailing gap token, a run split into
shorter tokens, a skip of w on a blank row.

**Drawing** (for the blitter and the hashes): decode a row into w
indices, 0 for anything skip, a gap or past the last token covers. Box
column i lands on cell column x0 + i, or cellW − 1 − (x0 + i) when the
frame is drawn mirrored.

### A.11 STRS

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | P: how many pose keys the chunk starts with (0-255) |
| 1 | 3 B | reserved, 0 |
| 4 | strings | back to back, no padding between them |

**What the compiler writes, in this order:**

1. **The pose keys.** For each kept move in manifest order, its `from`,
   its `to`, the mirror twin of `from` and the mirror twin of `to`, each
   added when it is set and not yet present. Pose key id i (1..P) is the
   i-th string; id 0 means none.
2. **The move names**, in MOVS order.
3. **The genre runs.** For each move with tags, in MOVS order, its tags as
   consecutive strings in manifest order. A run equal to an earlier one
   (the same tags in the same order) isn't written again: the move points
   at the earlier run.

**The mirror twin** of a pose key: split it at `-`, swap each part that is
exactly `left` or `right`, and join it again. `lean-left` → `lean-right`;
`stand` and `leftish` stay as they are. A key that is its own twin keeps
its id. A twin can be longer than its key (`left` → `right`): the
compiler skips a move whose `from` or `to` has a twin over 23 B (C.6), so
every twin it writes is a valid pose key.

**References:** move names and genre runs by u16 byte offset (≥ 4, the
string's length byte); pose keys by u8 id. Pose keys and move names are
1-23 B of `[a-z0-9][a-z0-9-]*`; genre tags are valid strings of 1-63 B.

**The reader** reads every string as A.1 says, with STRS's size as e
(offsets from STRS's first byte). It requires size ≥ 4 and P strings
starting at 4, each starting where the previous one ends, each in
bounds and a valid pose key (else the pack is rejected). A referenced
offset o is good when, in this order: o ≥ 4; then o + 2 ≤ size (so the
length byte n at o, and at least the NUL's place, lie inside); then n ≤
size − o − 2; then byte o + 1 + n is 0; then no byte inside is 0, and
the string is valid with its kind's limit. Otherwise the move that
refers to it is dropped. A genre run's next tag has as its o the
previous tag's end, which can equal size (then it fails o + 2 ≤ size).
Strings no move refers to aren't checked.

### A.12 MOVS

A 4-byte head: `u8 count` (0-64), `u8 recordLen` (≥ 32; the compiler
writes 32), 2 reserved bytes (0). Then count records of recordLen bytes
each, in manifest order of the kept moves; the reader takes the first
32 B of each and skips the rest, so records can grow. The reader checks
the head in this order, rejecting the pack at the first failure: size ≥
4 (`MOVS: too short`); count ≤ 64 (`MOVS: <n> moves`); recordLen ≥ 32
(`MOVS: record length <n>`); then size ≥ 4 + count·recordLen (`MOVS:
too short`). So a count of 65 with one record reads `MOVS: 65 moves`.

The record (32 B):

| Offset | Type | Field | Values | MoveLabels |
|---|---|---|---|---|
| 0 | u16 | name | STRS offset | `name` (points at the STRS bytes) |
| 2 | u8 | kind | 0 beat, 1 idle (no-BPM) | `kind` |
| 3 | u8 | beats | beat: 1, 2, 4, 8; idle: 0 | `beats` |
| 4 | u8 | paceLo | beat: 36-200, or 0 with paceHi 0 for any pace; idle: 0 | `paceLo` |
| 5 | u8 | paceHi | likewise | `paceHi` |
| 6 | u8 | weight10 | 1-100 (default 10) | `weight10` |
| 7 | u8 | when | idle: 1 quiet, 2 listening, 3 any; beat: 0 | `when` |
| 8 | u8 | flags | bit 0 mirror, 1 freeze, 2 placeStart, 3 placeEnd, 4 heavy, 5 snap; bits 6-7 0 (ignored) | `flags` |
| 9 | u8 | repeat | beat: 0-8, repeat·beats ≤ 16; idle: 0 | `repeat` |
| 10 | u8 | needs | bit 0 song_bpm; the others 0 | `needs` |
| 11 | i8 | travel | beat: −2..2 spots; idle: 0 | `travel` |
| 12 | u8 | startAt | 0 any, 1 centre, 2 side, 3 left, 4 right; 0 without travel | `startAt` |
| 13 | u8 | travelEase | an ease code (A.13); default 4 (inout) with travel, 0 without | — |
| 14 | u8 | bounce | beat: 0-16 native px; idle: 0 | — |
| 15 | u8 | sway | beat: 0-16 native px; idle: 0 | — |
| 16 | u8 | fromKey | pose key id, 0 none | `fromTag` |
| 17 | u8 | toKey | pose key id, 0 none | `toTag` |
| 18 | u8 | fromKeyM | the twin of fromKey, for the mirrored orientation | (mirrored `fromTag`) |
| 19 | u8 | toKeyM | the twin of toKey | (mirrored `toTag`) |
| 20 | u16 | genreFirst | STRS offset of the run's first tag; 0 when there are none | `genreKeys` (folded at load, B.8) |
| 22 | u8 | genreCount | 0-8 | `genreCount` |
| 23 | u8 | stepCount | 1-32 | — |
| 24 | u16 | stepFirst | the first step's index in STEP | — |
| 26 | u8 | songLo | with needs bit 0: 1-255; otherwise 0 | — |
| 27 | u8 | songHi | likewise, ≥ songLo | — |
| 28 | u32 | reserved | 0 | — |

`MoveLabels.idleMs` isn't stored: it is 10 × the sum of the steps'
durations. The compiler writes each move's steps consecutively, in MOVS
order, so stepFirst is the running total; the reader allows any
in-bounds stepFirst.

### A.13 STEP (8 B a step)

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | frame: bits 0-14 the frame index, bit 15 drawn mirrored |
| 2 | u16 | dur: beat moves in ticks (1/12 move beat, ≥ 3); idle moves in 10 ms units (20-500) |
| 4 | i8 | dx: the step's key, native px, x right |
| 5 | i8 | dy: native px, y down |
| 6 | u8 | ease: 0 hold, 1 linear, 2 in, 3 out, 4 inout |
| 7 | u8 | flags: bit 0 `go`; the other bits 0 (ignored) |

The keys are stored as written; the engine negates x for the mirrored
orientation. A move with no `go` flag travels during every step. The
reader requires STEP's size to be a multiple of 8 (else the pack is
rejected).

### A.14 Frames: what is stored, and in what order

- A cell is compared as a whole grid: cellH rows of cellW palette indices.
- The cells are taken in this order: the rest cell, then for each kept
  move in manifest order, each step's cell in step order.
- A cell equal to a stored frame reuses it (bit 15 clear). Otherwise a
  cell whose mirror image about the anchor column (row y's pixel x moves
  to 2·anchorX − x; a cell with an opaque pixel that would land outside
  it has no mirror image) equals a stored frame reuses that frame with
  bit 15 set. Otherwise it becomes the next frame. A symmetric cell
  equals itself first, so it never needs the bit.
  The mirror is about the anchor column, not the cell's centre
  (grid[y][cellW − 1 − x]), because B.4 draws a mirrored frame about the
  anchor; the centre would draw a merged cell 1 native px off whenever
  cellW ≠ 2·anchorX + 1, as for the test dancer and the stick.
- So the rest cell is frame 0, unmirrored, and GEOM's rest frame is 0.
- A cell used only by a skipped move isn't stored. All blank cells are
  one frame.

### A.15 The reader's checks

One reader serves the device (`dance::Pack::open`, step 2) and
`dancepack.py info` (`read_pack`, C.9). Both report the same short
reasons. The device logs a rejection as `[dance] ignored
/dancers/robo.dnc: <reason>` and a dropped move as `[dance] robo: move
"spin" ignored: <reason>`.

**The pack is rejected** at the first of these, in this order:

| Check | Reason |
|---|---|
| `Pack::open` only: avail < size, fewer bytes present than the file's length (5.4; read_pack always has them all) | `short read (<avail> of <size> bytes)` |
| fewer than 32 bytes | `too short` |
| magic | `not an MPDN pack` |
| minReader > 1 | `needs a newer player` |
| total size ≠ the file length | `size <n> isn't the file's <m>` |
| total size > 131,072 | `too big (<n> bytes)` |
| the CRC | `bad checksum` |
| chunk count 0 or > 16, or the directory past the end | `bad chunk count <n>` |
| A.3, per entry | `chunk <tag>: misaligned`, `chunk <tag>: out of bounds`, `chunk <tag> twice`, `chunks <a> and <b> overlap` |
| a required chunk missing | `no <tag> chunk` |
| META (A.5) | `META: bad <field>` |
| GEOM (A.6) | `GEOM: too short`, `GEOM: bad <field>` |
| PALS, THMB sizes | `PALS: <n> bytes, not 96`, `THMB: <n> bytes, not 800` |
| FTAB size | `FTAB: <n> bytes for <f> frames` |
| a frame's box, data bounds or decode (A.9, A.10) | `frame <i>: bad box`, `frame <i>: data out of bounds`, `frame <i>: bad data` |
| the rest frame | `the rest frame is empty` |
| STRS head and pose keys | `STRS: too short`, `STRS: bad pose key <i>` |
| MOVS head, in A.12's order | `MOVS: too short` (size < 4), `MOVS: <n> moves`, `MOVS: record length <n>`, `MOVS: too short` (the records past the end) |
| STEP size | `STEP: <n> bytes` |

**How a reason reads**, so both readers print the same text:

- `<field>` is the field's name as in A.5 and A.6 (`id`, `cellW`...);
  META's last byte is `count`. GEOM's fields are checked in table order
  (scale, faces, cellW, cellH, anchorX, anchorY, shadowW, frame count,
  rest frame), and anchorY's two stage limits (anchorY·scale ≤ 153,
  (cellH − anchorY)·scale ≤ 10) report `bad anchorY`.
- An entry is misaligned when off isn't a multiple of 4, and out of
  bounds when it fails one of A.3's three bounds.
- The overlap reason names the earlier directory entry (the first one
  the later overlaps), then the later.
  With several required tags missing, the reason names the first in A.4's
  order.
- A tag prints its bytes 0x20-0x7E as themselves and any other as `?`.
- Numbers are plain decimal, with no commas. `frame <i>` counts frames
  from 0, as FTAB does; `STRS: bad pose key <i>` names the key's id, from
  1, as A.11 counts them; `step <i>` counts steps from 1. `needs 0x<hh>`
  prints uppercase hex digits: `needs 0xA0`.
- Every reserved byte or bit is ignored: the header's flags and bytes
  20-31, GEOM byte 7, FTAB bytes 10-11, STRS bytes 1-3, MOVS head bytes
  2-3, record bytes 28-31, the record's flag bits 6-7 and STEP's flag bits
  1-7. The rest frame's check reads its bits 0-14.

**A move is dropped** (the others keep working) at the first of these,
in this order:

| Check | Reason |
|---|---|
| the name's string (A.11) | `bad name` |
| kind > 1 | `bad kind` |
| stepCount 0 or > 32, or stepFirst + stepCount past STEP's end | `bad steps` |
| a step's frame ≥ the frame count | `frame <f> out of range` |
| beat: beats not 1, 2, 4 or 8 | `<n> beats` |
| beat: a step under 3 ticks | `step <i>: <d> ticks` (i from 1) |
| beat: the sum ≠ 12·beats | `<sum> of <need> ticks` |
| idle: a step outside 20-500 | `step <i>: <ms> ms` |
| idle: the sum outside 50-1,200 | `<ms> ms in all` |
| idle: beats, a pace, repeat, bounce, sway, travel, genreCount, freeze or place set (genreFirst isn't read) | `beat fields on a no-BPM move` |
| beat: when ≠ 0; idle: when not 1-3 | `bad when` |
| a pace bound outside 36-200, lo > hi, or one of them 0 | `pace <lo>-<hi>` |
| a ranged move (lo, hi not both 0) whose range no allowed octave reaches (B.7) | `range <lo>-<hi> unreachable` |
| weight10 0 or > 100 | `weight <n>` |
| repeat > 8 or repeat·beats > 16 | `repeat <n>` |
| bounce or sway > 16 | `bounce <n>`, `sway <n>` |
| travel outside −2..2, or travel with beats < 2 | `travel <n>` |
| startAt > 4, or an ease code > 4 (travelEase or a step's) | `bad start_at`, `bad ease` |
| both place bits | `bad place` |
| a step's abs(dx)·scale or abs(dy)·scale > 48 | `offset <dx>,<dy>` |
| a pose key id > P | `bad pose key` |
| genreCount > 8, a bad tag string, or a tag folding to nothing (B.8) | `bad genre` |
| needs bit 0 | `song_bpm needs track BPM data: skipped in this version` |
| any other needs bit | `needs 0x<hh>` |
| a travel move with no start spot in any orientation it plays in: unmirrored, and mirrored only with the mirror flag (B.11) | `travels from no spot` |
| card packs only: the budget is red (B.6) and heavy isn't set | `budget red (<peak> px)` |

A pack whose every move is dropped still loads: it plays auto-bounce and
auto-idle (section 2.8). So does a pack with no moves.

### A.16 The frame hash

`kXxxFrameHash[i]` is FNV-1a 32 (B.10) over frame i's cell grid as
stored: cellH rows of cellW bytes, each byte the palette index (0
transparent), row-major, unmirrored. test_dance_blit renders the frame
at scale 1, unmirrored, at the cell's origin into a cleared cellW×cellH
buffer through a LUT with LUT[i] = i, and hashes the low bytes.

### A.17 Worked example: a one-move pack

The folder `dot/` holds `sheet.png`, 4×4, transparent except #ff0000 at
x 1-2, y 2-3 (any PNG form with those pixels), and this dancer.ini:

```ini
[dancer]
name    = Dot
author  = Ana
license = CC0-1.0
cell    = 4x4

[move hop]
frames = A1
beats  = 1
bpm    = any
bounce = 2
```

The defaults fill the rest: scale 1, anchor 2,4 (the bottom centre),
rest A1, shadow 0, faces none, weight 1, ease hold. The pack is 1,156
bytes:

| Chunk | off | size | Bytes |
|---|---|---|---|
| META | 140 | 29 | `03 64 6f 74 00 03 44 6f 74 00 03 41 6e 61 00 07 43 43 30 2d 31 2e 30 00 00 00 00 00 01` |
| GEOM | 172 | 12 | `01 00 04 04 02 04 00 00 01 00 00 00` |
| PALS | 184 | 96 | entry 1 `ff 00 00`; idle entry 1 `74 1b 1b` (B.9); the rest 0 |
| THMB | 280 | 800 | `11` × 800 (the 2×2 box at m = 20 fills the square) |
| FTAB | 1080 | 12 | `01 02 02 02 00 00 00 00 06 00 00 00` |
| STRS | 1092 | 9 | `00 00 00 00 03 68 6f 70 00` |
| MOVS | 1104 | 36 | `01 20 00 00`, then `04 00 00 01 00 00 0a 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00` |
| STEP | 1140 | 8 | `00 00 0c 00 00 00 00 00` |
| FDAT | 1148 | 6 | `00 01 11 00 01 11` |

The header and directory (bytes 0-139); the CRC is 0x5BF1E285:

```
0000: 4d 50 44 4e 01 00 01 00 84 04 00 00 85 e2 f1 5b
0010: 09 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0020: 4d 45 54 41 8c 00 00 00 1d 00 00 00 47 45 4f 4d
0030: ac 00 00 00 0c 00 00 00 50 41 4c 53 b8 00 00 00
0040: 60 00 00 00 54 48 4d 42 18 01 00 00 20 03 00 00
0050: 46 54 41 42 38 04 00 00 0c 00 00 00 53 54 52 53
0060: 44 04 00 00 09 00 00 00 4d 4f 56 53 50 04 00 00
0070: 24 00 00 00 53 54 45 50 74 04 00 00 08 00 00 00
0080: 46 44 41 54 7c 04 00 00 06 00 00 00
```

META's 29 bytes are padded to 32, STRS's 9 to 12 and FDAT's 6 to 8, so
the file ends at 1,156. Frame 0's hash is 0x18857B21. The move's budget
(B.6) is green: median 4 px and peak 6 px in each of its six cases (24
and 30 fps at paces 72, 124 and 176).

## Appendix B. The shared algorithms, integer-exact

dancepack.py and the C++ engine (DrawnCharacter, DanceBudget, the
Choreographer's pace test, GenreFold) compute these the same way, bit for
bit. The vectors of C.8 pin them in both test suites.

### B.0 Integer rules

- **Q16:** 65536 is 1.0.
- **`a // b`** (b > 0) is floor division, as in Python. C++'s `/`
  truncates toward zero, so wherever a can be negative the C++ uses a
  floor helper; `>> k` on an `int64_t` is floor division by 2^k (an
  arithmetic shift: what GCC and Clang do, and guaranteed from C++20).
- **`a mod b`** (b > 0) is a − b·(a // b), so 0 ≤ a mod b < b, as Python's
  `%`; C++'s `%` takes the sign of a, so where a can be negative the C++
  uses a floor helper too. (B.4's divisions of pos16 need neither, since
  pos16 ≥ 0. Its rhu calls in steps 7 and 8 still take negative values,
  an offset up or left, the hop or a leftward travel, and need the floor:
  `(a + 2^(k−1)) >> k` on `int64_t`. Truncating gives −3 instead of B.4's
  −4 at tick 3.)
- **`rhu(a, b)`** = (2a + b) // (2b), b > 0: round half up, crab::rnd's
  rule. For b = 2^k it is (a + 2^(k−1)) >> k.
- Every intermediate below fits in a signed 64-bit integer (the largest
  is about 2^48); the C++ computes them in `int64_t`.

### B.1 Timing

Each error below skips the move (C.6 gives its message); the API's
parse functions raise it as a DanceError (C.9).

**Beat moves.** Each `timing` value is one token:

- Its form is an integer (`2`), a decimal (`0.5`, `.25`) or a fraction
  (`3/4`): the regex `^([0-9]+|[0-9]*\.[0-9]+|[0-9]+/[0-9]+)$`, with C.3's
  limit on digits (each integer part at most 9 digits after its leading
  zeros, at most 9 digits after the point; more is the error `timing
  "<value>" has a number of more than 9 digits`). Anything else (a unit,
  a sign, an exponent) is an error, and so is a zero denominator.
- v is its exact value (Python's `fractions.Fraction(text)`), and t =
  12·v ticks. n = floor(t + 1/2).
- t = n: exact. Otherwise, |t − n| ≤ 1/4 tick (1/48 beat): snapped to n,
  with a warning. Otherwise an error.
- n < 3 is then the step-too-short error.

| Value | Ticks |
|---|---|
| `1/4`, `.25` | 3 |
| `1/3` | 4 |
| `3/4` | 9 |
| `2` | 24 |
| `0.33` | 4, snapped (t = 3.96) |
| `0.26` | 3, snapped (t = 3.12) |
| `0.3` | error (t = 3.6) |
| `1/5` | error (t = 2.4) |
| `0.125` | error (t = 1.5, half a tick from the grid) |

**The even split.** A beat move with no `timing` (given by `row =` or by
`frames =`) shares 12·beats ticks among its n frames: an error when
12·beats isn't a multiple of n, otherwise 12·beats / n each (then the
3-tick floor applies). 8 frames on 2 beats: 3 each. 3 frames on 4 beats:
16 each. 5 frames on 1 beat: an error.

**Idle moves.** Each value matches `^([0-9]+|[0-9]*\.[0-9]+)(s|ms)$`,
the unit in any case (`2S` too, as C.3's words), with the same limit on
digits as a beat value. Its
milliseconds (exact: v·1000 for `s`, v for `ms`) must be a whole multiple
of 10; the step lasts ms / 10 units. Each step is 20-500 units
(200-5,000 ms) and the sum 50-1,200 (500-12,000 ms). `2s` is 200, `1.5s`
is 150, `400ms` is 40; `0.405s` and a bare `2` are errors. Idle moves
have no even split: `timing` is required.

**Beat sums.** A beat move's ticks must sum to 12·beats.

### B.2 Eases (Q16)

f is the step's elapsed fraction, 0 ≤ f ≤ 65536; each ease returns
0-65536, and 65536 at f = 65536:

| Code | Name | e(f) |
|---|---|---|
| 0 | hold | 0 for f < 65536, and 65536 at f = 65536 |
| 1 | linear | f |
| 2 | in | (f·f) >> 16 |
| 3 | out | 65536 − (((65536 − f)·(65536 − f)) >> 16) |
| 4 | inout | (f·f·(196608 − 2f)) >> 32 (smoothstep, 3f² − 2f³) |

| f | 0 | 16384 | 32768 | 49152 | 65536 |
|---|---|---|---|---|---|
| hold | 0 | 0 | 0 | 0 | 65536 |
| linear | 0 | 16384 | 32768 | 49152 | 65536 |
| in | 0 | 4096 | 16384 | 36864 | 65536 |
| out | 0 | 28672 | 49152 | 61440 | 65536 |
| inout | 0 | 10240 | 32768 | 55296 | 65536 |

A step's f is always below 65536 (B.4 step 2), so a held key holds until
the next step starts. Only travel reaches p = 65536 (B.4 step 8): a held
travel stays at its start spot, then lands on its end spot when the last
`go` step ends, or at the frame that crosses the play's end.

### B.3 Bounce and sway

φ is the phase within the current move beat (Q16, 0 ≤ φ < 65536), and b
counts the move beats since the run started, not since the play started;
B.4 derives both from the unwrapped pos16.

**The hop** (the crab's curve: 0.12 beat on the ground, then a parabola
that lands on the next beat):

- hop(φ) = 0 when φ < 7864 (0.12·65536 = 7864.32, floored);
- otherwise a = ((φ − 7864)·65536) // 57672 and hop(φ) = (4·a·(65536 −
  a)) >> 16. It peaks at 65536 at φ = 36700 (0.56 beat), as the crab's.
- `bounce = n` adds −n·hop(φ) to the key's y (Q16 native px, up).

**The sway** (extremes on the beats):

- sway(b, φ) = s·(65536 − 2·inout(φ)), with s = −1 when b is even and +1
  when b is odd. So it is −1 on even beats, +1 on odd beats, and a
  smoothstep between: continuous across beats, and across plays while the
  orientation stays. B.4 step 6 negates it with the orientation, so in a
  mirrorable move whose plays alternate (3.4 step 6) the sway reverses at
  each play boundary: the root jumps by up to 2·sway·scale px on that
  beat (the test dancer's step-touch: 12 px, its key's change included).
  Section 11 schedules the fix as a contract change.
- `sway = n` adds n·sway(b, φ) to the key's x (Q16 native px).

| φ | 0 | 7863 | 7864 | 7865 | 7866 | 16384 | 32768 | 36700 | 49152 | 65535 |
|---|---|---|---|---|---|---|---|---|---|---|
| hop | 0 | 0 | 0 | 3 | 7 | 33003 | 64317 | 65536 | 53317 | 7 |

sway(0, 0) = −65536, sway(0, 32768) = 0, sway(0, 65535) = 65534,
sway(1, 0) = 65536. So a 1-beat move with sway swings on through its
plays: at scale 2 with sway 2 from the centre spot, rootX is 156, 160,
164, 160, 156 at ticks 0, 6, 12, 18 and 24 (tick 12, the second play's
start, has b = 1, the other extreme from tick 0).

### B.4 frameAt

**Inputs:**

- the pack's scale, anchorX, anchorY and shadowW, and the frames' boxes
  (FTAB);
- a move: its steps with durations d_0..d_(n−1), the starts s_i = d_0 +
  … + d_(i−1), and L = the sum;
- **pos16 ≥ 0**, the position since the run started, in Q16 step units
  (Q16 ticks for beat moves, Q16 10 ms units for idle moves), never
  wrapped. The Choreographer derives it as floor(u·12·65536) from u in
  move beats, or as (ms·65536) // 10 from the milliseconds an idle run has
  played, and never passes a negative one: u is held at 0 within a run
  (section 3.5). A negative pos16 is a caller's error (dancepack.py raises
  ValueError);
- **mirrored**, the move's orientation (0 or 1);
- **spot**, the start spot (−1, 0 or +1), at stage x spotX[spot + 1] of
  `{64, 160, 256}`;
- **e16**, the energy in Q16 (section 3.6: 39322-65536). The budget passes
  65536, and idle moves are always drawn at 65536.

**Steps** (stage coordinates: x 0-319, y 0-163, groundY 153):

1. **Wrap.** loop = pos16 // (L·65536); q = pos16 − loop·L·65536.
2. **The step.** i is the last step with s_i·65536 ≤ q, and f =
   (q − s_i·65536) // d_i, so 0 ≤ f < 65536.
3. **The frame.** frame = step i's frame index; drawnMirror = (step i's
   bit 15) XOR mirrored.
4. **The key.** j = (i + 1) mod n: the last step eases into the first
   key. E = ease_i(f). kx = dx_i·65536 + (dx_j − dx_i)·E, and ky likewise
   from dy.
5. **Bounce and sway** (beat moves; both 0 for idle moves). b = pos16 //
   786432 and φ = (pos16 mod 786432) // 12 (786432 = 12·65536). by =
   −bounce·hop(φ); sx = sway·sway(b, φ).
6. **Orientation.** sgn = −1 when mirrored, else +1. x16 = sgn·(kx + sx);
   y16 = ky + by.
7. **To screen px.** X = rhu(scale·e16·x16, 2^32) and Y = rhu(scale·e16·
   y16, 2^32). With `snap`: X = scale·rhu(e16·x16, 2^32), Y likewise.
8. **Travel** (beat moves with travel ≠ 0; otherwise 0). t = travel·sgn;
   startX = spotX[spot + 1]; endX = spotX[spot + t + 1]. G is the steps
   flagged `go`, or every step when none is; Gt = the sum of their
   durations. When loop ≥ 1, p = 65536 (done). Otherwise g = the sum of
   d_k·65536 over the steps k in G before i, plus q − s_i·65536 when i is
   in G; p = g // Gt (floored, not rounded). travelPx =
   rhu((endX − startX)·ease_travelEase(p), 65536). The energy doesn't
   scale it. With every ease p = 65536 means the travel is complete, `hold`
   included (B.2).
9. **The root.** rootX = startX + travelPx + X; rootY = 153 + Y.
10. **The box.** For a frame box (x0, y0, w, h) with w > 0: left =
    rootX + (x0 − anchorX)·scale when drawnMirror is 0, or rootX +
    (anchorX − x0 − w + 1)·scale when it is 1 (the anchor column mirrors
    to cellW − 1 − anchorX); top = rootY + (y0 − anchorY)·scale; right =
    left + w·scale; bottom = top + h·scale.
11. **The clamp.** cx = −left when left < 0, 320 − right when right >
    320, else 0; cy = −top when top < 0, 164 − bottom when bottom > 164,
    else 0. Add cx to rootX, left and right, and cy to rootY, top and
    bottom. A blank frame has no box and isn't clamped.
12. **The shadow and the character rect** (B.5).

**Output:** frame, drawnMirror, rootX, rootY, the box (or none), the
shadow (or none) and the character rect.

**Example.** Scale 2, cell 8×8, anchor 4,8, shadow 10; one frame, box
(2, 2, 4, 6). A beat move of 2 beats, bounce 2, sway 1, travel 1,
travelEase inout; step 1: frame 0, 12 ticks, key 0,0, ease out; step 2:
frame 0 with bit 15, 12 ticks, key 3,−2, ease in, `go`. Rects are
x0,y0-x1,y1, half-open. With travelEase hold instead, rootX at ticks 0,
12, 23, 24 and 30 (unmirrored, from spot −1) is 62, 72, 63, 158 and 165:
the root lands at tick 24.

| Tick (pos16 / 65536) | Unmirrored from spot −1: frame/mirror, root, box, shadow | Mirrored from spot +1 |
|---|---|---|
| 0 | 0/0, 62,153, 58,141-66,153, 57,152-67,155 | 0/1, 258,153, 256,141-264,153, 253,152-263,155 |
| 3 | 0/0, 65,149, 61,137-69,149, 64,152-66,155 | 0/1, 255,149, 253,137-261,149, 254,152-256,155 |
| 6 | 0/0, 69,146, 65,134-73,146, none | 0/1, 252,146, 250,134-258,146, none |
| 12 | 0/1, 72,149, 70,137-78,149, 71,152-73,155 | 0/0, 248,149, 244,137-252,149, 247,152-249,155 |
| 15 | 0/1, 86,147, 84,135-92,147, none | 0/0, 234,147, 230,135-238,147, none |
| 18 | 0/1, 117,146, 115,134-123,146, none | 0/0, 204,146, 200,134-208,146, none |
| 24 | 0/0, 158,153, 154,141-162,153, 153,152-163,155 | 0/1, 162,153, 160,141-168,153, 157,152-167,155 |

At tick 3, for instance: f = 16384, out gives 28672, so kx = 86016 and
ky = −57344; φ = 16384, hop 33003, so by = −66006; inout(16384) = 10240,
so sx = −45056. X = rhu(2·65536·40960, 2^32) = 1 and Y =
rhu(2·65536·(−123350), 2^32) = −4. At tick 24 the play is over: the
travel is complete (96 px) and the step wraps to the first. At e16 =
47186 (0.72) and tick 15, unmirrored: root 84,149, box 82,137-90,149,
shadow 83,152-85,155.

**The clamp, for example:** scale 2, cell 64×76, anchor 32,72, shadow
40, one frame with box (0, 10, 64, 62), a 1-beat move of one step with
key −8,0, at tick 0 from spot −1. Unmirrored, X = −16 puts the box's left
at −16, so it is pushed 16 px right: root 64,153, box 0,29-128,153,
shadow 44,152-84,155. Mirrored, the key is +16 px: root 80,153, box
18,29-146,153, no clamp.

### B.5 Rects, the shadow and the dirty rule

- **A rect** is half-open, [x0, x1) × [y0, y1) in stage px. It is empty
  when x1 ≤ x0 or y1 ≤ y0; its area is (x1 − x0)·(y1 − y0), or 0 when
  empty. The union of two rects is their bounding rect; an empty one adds
  nothing.
- **The shadow.** hop = max(0, −Y), with Y from B.4 step 7 (before the
  clamp). sw = shadowW − 2·hop; none when sw ≤ 0. Otherwise x0 = rootX −
  sw // 2 (rootX after the clamp), x1 = x0 + sw, rows [groundY − 1,
  groundY + 2) = [152, 155), clipped to x 0-320; none when that leaves it
  empty. This is CrabPose's `shadowRect()` (2 px narrower per px of hop);
  at the legacy stage's groundY 145 the rows are the crab's 144-146.
- **The character rect** is the union of the box and the shadow.
- **The dirty rule.** A is the last frame's character rect and B this
  frame's; U = union(A, B). When 10·area(U) > 13·(area(A) + area(B)),
  they stay two rects and the dirty px are area(A) + area(B). Otherwise
  it is the one rect U, and the dirty px are area(U). A frame with no
  previous one dirties area(B).
- **Example** (B.4's move, unmirrored): from tick 6 to tick 15, A and B
  are 96 px each and U is 351 px, more than 1.3 × 192: two rects, 192 px.
  From tick 0 to tick 24, U is 1,484 px against 140 + 140: 280 px.

### B.6 The per-move budget

**The cases.** For each move:

- **fps** 24 and 30, with the frame periods 42 and 33 ms
  (`dancerate::periodMs`). Frame n is at n·period ms, so pos16(n) =
  (n·period·pace·65536) // 5000 for a beat move (pace P BPM is P/5000
  ticks a ms) and (n·period·65536) // 10 for an idle move.
- **paces** (beat moves): with (rlo, rhi) = reach(lo, hi, beats, travel)
  (B.7), the paces rlo, (rlo + rhi) // 2 and rhi, ascending, a repeat
  dropped. Idle moves have the one pace 0.
- **orientations:** unmirrored, then mirrored when the move has the
  mirror flag.
- **spots:** the centre (0) for a move without travel; for a travel move
  each start spot of that orientation (B.11), ascending.
- The cases run in this nested order: fps, pace, orientation, spot.

**One case.** N is the first n ≥ 1 with pos16(n) ≥ L·65536: one play,
plus the frame that crosses its end (which wraps to the first step, with
any travel complete). Frames n = 0..N go through frameAt at e16 = 65536,
and the dirty px d_n of n = 1..N (B.5) make N values. The case median is
the lower median: the element (N − 1) // 2, from 0, of the values sorted
ascending. The case peak is the largest.

**The move.** Its peak is the largest case peak, and its median the
largest case median. The light: **green** when the peak ≤ 16,000,
**amber** when ≤ 20,000, **red** above. The budget reads boxes only,
never pixels.

**Examples.** A.17's move: six cases, each median 4 and peak 6: green.
B.4's move (unranged, travel 1, no mirror flag, so the paces 72, 124 and
176 and the start spots −1 and 0) gives 12 cases. At 30 fps, pace 124,
spot −1: N = 30, median 160, peak 266. At 24 fps, pace 124: median 176,
peak 304. The move: median 192 (24 fps, pace 176), peak 304: green.

### B.7 Pace: eligibility, tolerance and reach

**T10** is the dance tempo T (section 2.4) in tenths of a BPM, an
integer 720-1760. The device takes floor(T·10 + 0.5) of its latched T.
At octave k the pace is T·2^k; in integers, x(k) = T10·2^(k+1), and the
pace is x(k)/20.

**eligible_k(T10, lo, hi, prevK, beats, travel) → k or none** (prevK: the
k of the last ranged move, or none; travel: the move's, 0 for none):

- An unranged move (lo = hi = 0) gets 0.
- Otherwise try, in order, prevK (when there is one), then 0, −1 and +1,
  each once. k fits when 20·lo ≤ x(k) ≤ 20·hi, and k = −1 needs beats ≤ 4,
  and k = +1 needs T10 ≤ 900 (2T ≤ 180) and, for a travel move, beats ≥ 4
  (section 8's D = beats·2^−k ≥ 2). The first k that fits is the answer;
  none when nothing fits.
- So for every valid move its octaves are reach's (below), and the
  Choreographer needs no travel test of its own: a travel move has at least 2 beats
  (A.15), so k = 0 and k = −1 always give D ≥ 2. The coverage report and
  its warning (C.5) use it the same way.

**pace_holds(T10, k, lo, hi)**, the running move's tolerance (section
2.5): always true for an unranged move. Otherwise 1940·lo ≤ 100·x(k) ≤
2060·hi, both bounds inclusive, and k = +1 needs T10 ≤ 900, as in
eligible_k: the 3 % never takes a pace past 180. For a 100-100 range at
k = 0: T10 970 and 1030 hold (97 and 103 exactly), 969 and 1031 don't.
For a 170-180 range at k = +1: T10 900 holds (180), 901 and 927 don't
(180.2 and 185.4, inside 1.03·180 but past 180).

**reach(lo, hi, beats, travel) → (rlo, rhi) or none:**

- The octaves allowed: 0, unless the move travels and beats < 2; −1, for
  a ranged move with beats ≤ 4; +1, for a ranged move without travel or
  with beats ≥ 4 (D ≥ 2).
- Each octave covers paces: −1 → 36-88, 0 → 72-176, +1 → 144-180. Their
  union is one interval [a, b] (36 or 72 to 176 or 180).
- An unranged move reaches [a, b] (72-176). A ranged one reaches
  [max(lo, a), min(hi, b)], or none when that is empty: the error names b
  as the fastest pace when lo > b, and a as the slowest otherwise.
- reach is none when no octave is allowed, which happens only for an
  unranged travel move under 2 beats. The reader never asks reach about
  one: A.15's range row applies to ranged moves only, and its travel row
  drops the move as `travel <n>` (the compiler skips it earlier, `a
  travel move needs at least 2 beats`).

| T10 | Range | prevK | Beats | Travel | k |
|---|---|---|---|---|---|
| 1200 | 90-130 | none | 4 | 0 | 0 |
| 870 | 150-180 | none | 1 | 0 | +1 |
| 870 | 150-180 | none | 8 | 0 | +1 |
| 1740 | 36-60 | none | 4 | 0 | none |
| 1200 | 36-200 | +1 | 2 | 0 | 0 (double time needs T10 ≤ 900) |
| 1200 | 36-200 | −1 | 2 | 0 | −1 (the previous k fits first) |
| 950 | 170-180 | none | 2 | 0 | none |
| 900 | 170-180 | none | 2 | 0 | +1 |
| 1760 | any | +1 | 2 | 0 | 0 |
| 1200 | 60-100 | none | 4 | 0 | −1 |
| 1200 | 60-100 | none | 8 | 0 | none (half time needs ≤ 4 beats) |
| 800 | 72-180 | +1 | 2 | 1 | 0 (at +1 D would be 1) |
| 720 | 100-150 | none | 2 | 1 | none (only +1 fits, and D would be 1) |
| 720 | 100-150 | none | 4 | 1 | +1 (D = 2) |

| Range | Beats | Travel | reach |
|---|---|---|---|
| any | any | 0 | 72-176 |
| any | 1 | 1 | none: no octave is allowed |
| 40-80 | 1 | 1 | 40-80 (half time only) |
| 90-130 | 4 | 0 | 90-130 |
| 36-60 | 4 | 0 | 36-60 |
| 36-60 | 8 | 0 | none: the slowest pace is 72 |
| 185-200 | 2 | 0 | none: the fastest pace is 180 |
| 178-200 | 2 | 1 | none: the fastest pace is 176 |
| 100-200 | 4 | 1 | 100-180 |
| 40-200 | 8 | 0 | 72-180 |

### B.8 Genre folding

**fold_genre(tag) → an ASCII key:**

1. **Full folding.** Each code point below 0x80 stays; any other becomes
   its textfold Full replacement, exactly `textfold::replacement(cp,
   Mode::Full)`: `punctuation()`, else `latin1Multi()`, else `symbols()`,
   else `kLatin1` (when its entry isn't `\0`), else `kLatinExtA`, else
   `?`.
2. Lowercase A-Z.
3. Replace every `&` and every `+` with ` and `.
4. Split into words: the maximal runs of `[a-z0-9]`. Everything else
   separates words and is dropped.
5. A word that is exactly `n`, with a word before it and a word after it,
   becomes `and`.
6. The key is the words joined with nothing between them. An empty key
   means the tag **folds to nothing**.

The genre key is FNV-1a 32 (B.10) of the key's bytes. A tag that folds to
nothing has no key, so FNV("") (0x811C9DC5) is never a key.

**Where the tables come from.** dancepack.py embeds `TEXTFOLD`, a dict
from code point to replacement for every code point whose Full
replacement isn't `?` (267 entries at 2e1b5c5; U+00BF `¿` isn't one, as
it folds to `?`). `dancepack.py --check`
rebuilds it from lib/core/TextFold.cpp's source (C.2) and reports any
difference, so the two can't drift. The C++ GenreFold (step 2, D.1) calls
`textfold::replacement()` itself and folds as a stream, with no buffer
limit.

| Tag | Key | FNV-1a 32 |
|---|---|---|
| `Hip Hop`, `hip-hop`, `HIP_HOP` | hiphop | 0x5DE39FA5 |
| `Drum & Bass`, `drum n bass`, `Drum ’n’ Bass` | drumandbass | 0xE6318A55 |
| `Drum&n&Bass` (step 3's spaces split the words, so step 5 sees a lone `n`) | drumandandandbass | 0xEBEFBAA3 |
| `R&B`, `r+b` | randb | 0x1BDE7F5C |
| `d&b` | dandb | 0xDD173CB2 |
| `rock'n'roll` | rockandroll | 0x4666B1EE |
| `n` | n | 0xEB0C3431 |
| `a n` | an | 0x4124F2E6 |
| `n n n` | nandn | 0x66E2517C |
| `&` | and | 0x0F29C2A6 |
| `C++` | candand | 0xF49E7244 |
| `Électro` | electro | 0xEED54F8F |
| `Straße` | strasse | 0x0581A214 |
| `Æther` | aether | 0x886A2654 |
| `Œuvre` | oeuvre | 0xA67EB213 |
| `Łódź` | lodz | 0xD5FC07C6 |
| `Ñandú` | nandu | 0x61E2499D |
| `½ step` | 12step | 0x8E61F028 |
| `©` | c | 0xE60C2C52 |
| `Synth–Pop` | synthpop | 0x87CDEC94 |
| `K-Pop` | kpop | 0x9076A259 |
| `90s` | 90s | 0xA0CA74AD |
| `Música Popular Brasileira` | musicapopularbrasileira | 0xC9B7E57E |
| `AZ Zydeco` (both ends of step 2's A-Z) | azzydeco | 0x5B6EF8E2 |
| `日本`, two spaces, the empty string | (folds to nothing) | — |

### B.9 Colours

- **RGB565:** `(r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)`, as M5GFX's
  `color565`. The background #080C10 is 0x0862.
- **Merging:** sheet colours with the same RGB565 value are one palette
  entry. The first in scan order (C.4) keeps its RGB888 value.
- **The idle palette**, derived: Y = (77r + 150g + 29b + 128) >> 8; each
  channel c becomes c1 = (c + Y + 1) >> 1 (half way to its luma, rounded
  half up), then (7·c1 + 5) // 10 (×0.7, rounded half up). Index 0 stays
  0, 0, 0. #ff0000 → #741b1b, #f0f0f8 → #a9a9ac, #a8acc0 → #787980,
  #f472b6 → #8e6178, #ffffff → #b3b3b3.
- **Too close to the background:** every 8-bit channel within 15 of
  #080C10's (|r − 8| < 16, |g − 12| < 16, |b − 16| < 16). It is tested on
  every dance colour and every idle colour, derived or given: #101418
  vanishes, #181c20 doesn't.

### B.10 FNV-1a 32, and the frame hash

h starts at 0x811C9DC5; for each byte, h = (h XOR byte) × 0x01000193 mod
2^32 (QueueSaver.cpp's). FNV("a") = 0xE40C292C. The frame hash is FNV-1a
32 over the stored cell grid (A.16).

### B.11 Extent, fit and start spots

**extent(move, mirrored) → (lo, hi)**, screen px from the root x: a
bound on the columns the move can draw at any e16 ≤ 65536, before the
clamp. For each step i whose frame box isn't empty, with m = (bit 15)
XOR mirrored, sgn as in B.4 and j = (i + 1) mod n, the next step (the
last step eases into the first key, as in B.4):

- L = (x0 − anchorX)·scale when m is 0, (anchorX − x0 − w + 1)·scale when
  1; R = L + w·scale;
- a = sgn·dx_i·scale and c = sgn·dx_j·scale (the keys it eases between);
- lo_i = L + min(a, c, 0) − sway·scale and hi_i = R + max(a, c, 0) +
  sway·scale.

extent = (the smallest lo_i, the largest hi_i); a move whose boxes are
all empty fits anywhere. It is a true bound: every ease stays between its
two keys, an energy below 1 only pulls the offset toward 0 (hence the 0
in min and max), and rounding a value that lies between two integers
keeps it between them.

**fits(spot):** spotX + lo ≥ 0 and spotX + hi ≤ 320.

**Start spots** of a travel move in an orientation: each s of −1, 0, +1,
ascending, such that s + t is a spot (t = travel·sgn), `start_at` allows
s (any: all; centre: 0; side: −1 and +1; left: −1, or +1 when mirrored;
right: +1, or −1 when mirrored), and the move fits at s and at s + t
(so it fits all the way, the root moving monotonically between them).

A travel move with no start spot in any orientation it plays in
(unmirrored, and mirrored only with the mirror flag, as B.6's cases) can
never play: the compiler skips it, and the reader drops it. Mirrored
spots don't reflect unmirrored ones (the mirror is about the anchor
column, so the mirrored extent is (scale − hi, scale − lo)), so a move
without the mirror flag that would have a mirrored spot but no
unmirrored one is still dropped. A move without travel is
always eligible; where it doesn't fit, B.4's clamp pushes it inwards, and
the coverage report says so.

B.4's move, unmirrored: step 1 gives L = −4, R = 4, keys 0 and 6, so −6
and 12; step 2 (drawn mirrored) gives L = −2, R = 6, so −4 and 14. The
extent is (−6, 14), and the start spots are −1 and 0.

Where the wrap matters: scale 2, cell 16×8, anchor 8,8, frames with boxes
(7, 0, 2, 8) and (0, 0, 16, 8); step 1 frame 0 keyed −10,0, step 2 frame
1 keyed 0,0, both linear. Step 2's c is step 1's key, −20, so its lo is
−16 − 20 = −36: the extent is (−36, 16), and the drawn box does reach
36 px left of the root as step 2 eases back into the first key.

## Appendix C. dancepack.py's contract

### C.1 Files, paths and the registry

| Path | What |
|---|---|
| `tools/dancepack.py` | the compiler: standard library only, Python 3.8+, one file a contributor can download alone (`--check` and `builtin` need the repository) |
| `tools/test_dancepack.py` | its unittest suite |
| `tools/art/dancers/test/` | the test dancer (C.10): `dancer.ini` (hand-written), `sheet.txt` (the art as text, its source) and `sheet.png` (committed, rendered from it) |
| `test/support/dance/TestPack.h` | generated: the test dancer's pack, the fixture of steps 2-3 |
| `test/support/dance/EligibilityVectors.h` | generated: B.7's cases |
| `test/support/dance/FoldVectors.h` | generated: B.8's cases |
| `test/support/dance/BudgetVectors.h` | generated: the test pack's budget cases and traces (B.6) |
| `lib/core/packs/StickPack.cpp` | generated from `tools/art/dancers/stick/`: added to the registry in step 7 |

- **The registry** is a list in dancepack.py, `BUILTINS = [(source
  folder, output file)]`, both relative to the repository root with `/`.
  In step 1 it holds one entry, `("tools/art/dancers/test",
  "test/support/dance/TestPack.h")`. The three vector headers are
  generated from dancepack.py's own tables and from the test dancer.
  `--check` and `builtin` cover exactly the registry and the vectors,
  and a registry folder's `sheet.txt` when it has one. `builtin` renders
  sheet.txt to an RGB sheet.png with the colours the file lists, and
  `--check` compares the pixels, not the bytes, so zlib versions don't
  matter; the format is in `_render_sheet_txt`'s docstring.
- **The id** of a pack is its dancer folder's name: the last part of DIR
  once resolved, so `robo`, `robo/` and `./robo` agree. DIR is resolved
  lexically, as `os.path.abspath`, so on a file system that ignores case
  the name keeps the case it was given. It must be 1-23 of `a-z`, `0-9`
  and `-`, starting with a letter or digit.
- **Python 3.8+:** no syntax or library call newer than 3.8 unless
  guarded (no `list[int]` evaluated at run time, no `match`, no
  `str.removeprefix`). Determinism: never iterate a set or a dict built
  from one into output; sort.
- Test suites in test/ include the headers by relative path
  (`#include "../support/dance/TestPack.h"`), as test_ogg_opus includes
  OggWriter.h: PlatformIO compiles only a suite's own folder.

### C.2 The command line

```
python tools/dancepack.py check DIR [--strict]
python tools/dancepack.py build DIR [-o FILE] [--strict]
python tools/dancepack.py info FILE
python tools/dancepack.py builtin [DIR --out FILE]
python tools/dancepack.py --check
```

- **check** reads and validates DIR, prints the report of C.5 (with the
  echo tables and the coverage report), and writes nothing.
- **build** prints the same report without the echo tables and the
  coverage report, then writes the pack to FILE (default: `<id>.dnc` in
  the current directory), creating FILE's folder when it doesn't exist
  (so `-o E:/dancers/robo.dnc` works on a fresh card), and prints `wrote
  <FILE>`. A failure writes nothing. A FILE that can't be written (a
  folder, say) prints `<FILE>: error: can't write it (<reason>)`, the
  operating system's reason.
- **info** reads FILE with the reader of A.15 and prints it. The first
  line is `<FILE>: MPDN v<format> (reader <minReader>), <size> bytes, CRC
  ok`; then the id, name, author and licence, the geometry, the chunks
  (tag, offset, size), and a line per move (its status line's move and
  light, C.5, with its step count), or `ignored: <reason>`. An invalid
  pack prints `<FILE>: error: <reason>`; a folder `<FILE>: error: a
  folder, not a .dnc pack`.
- **builtin** with no arguments regenerates every registry output and
  vector header. It writes only the files whose text changed, printing
  `wrote <path>` for each. With DIR and `--out FILE` it compiles one pack
  to FILE, which must end in `.cpp` or `.h` and whose stem must be a C++
  identifier (it names the symbols, C.8). `--strict` and the licence
  allowlist always apply. Without arguments it needs the repository (C.1);
  with DIR and `--out` it works anywhere.
- **--check** regenerates the same outputs in memory and compares each
  with the file on disk, read as text with universal newlines (so a CRLF
  checkout under `core.autocrlf`, as on the laptop, still passes). Each
  file that differs or is missing prints `stale: <path> (run python
  tools/dancepack.py builtin)`. It then checks the embedded tables:
  - `TEXTFOLD` against lib/core/TextFold.cpp: in the bodies of
    `punctuation()`, `latin1Multi()` and `symbols()`, each group of `case
    0x…:` labels maps to the next `return "…";`; the string literals of
    `kLatin1` and `kLatinExtA` are concatenated, with `\0`, `\"` and `\\`
    decoded; the four are combined in `replacement()`'s order (B.8), and
    entries equal to `?` are left out.
  - `FONT_COVERAGE` (sorted code point ranges) against
    src/ui/VlwFonts.cpp: every `const uint8_t kVlw<Name>[<n>] = { … };`
    array's bytes, whose glyph count is the big-endian u32 at byte 0 and
    whose glyph i has its code point in the big-endian u32 at 24 + 28·i;
    the coverage is the intersection over the arrays (at 2e1b5c5 all four
    hold the same 256 code points).
  - A difference prints `stale: tools/dancepack.py (TEXTFOLD differs from
    lib/core/TextFold.cpp at U+00E9)`, or the same for `FONT_COVERAGE`
    and src/ui/VlwFonts.cpp, naming the lowest code point that differs.
  - It prints nothing when everything matches.
- **Outside the repository** (dancepack.py downloaded alone, so there is
  no lib/core beside tools/), `--check` and `builtin` without arguments
  are a usage error: `--check and builtin work only in the
  mstream-mp3-player repository; to check a dancer use: dancepack.py
  check DIR`. They would otherwise report the repository's files as
  stale.
- **Exit codes:** 0 on success. 1 for a pack-level error, a `--strict`
  failure, an invalid pack (`info`), a stale file, a missing DIR, FILE
  or dancer.ini, or an output that can't be written. 2 for a usage
  error: argparse's own, `builtin` with only one of DIR and `--out`, an
  `--out` name that can't name symbols, or `--check` or `builtin` outside
  the repository.
- **Output** goes to stdout, except argparse's usage errors (stderr).
  Paths print with `/`.

### C.3 Reading dancer.ini

**The parser.**

- The bytes are decoded as `utf-8-sig`, so a BOM is accepted. A file
  that isn't UTF-8 is a pack error at the line of the first bad byte.
  CRLF and lone CR become LF before parsing.
- `configparser.ConfigParser(interpolation=None, strict=True,
  comment_prefixes=('#', ';'), inline_comment_prefixes=(';',),
  default_section='')`, with its default lowercasing of keys and both `=`
  and `:`. `default_section=''` can't name a section, so `[DEFAULT]` is
  an unknown section like any other instead of lending its keys to every
  section.
- **A section header** is a line starting with `[` (after any indent): its
  name runs to the line's last `]`, and text after that is ignored. So
  `[move hop]]` names the move `hop]`, an invalid name. This is
  configparser's rule from Python 3.10 (3.8 and 3.9 stopped at the first
  `]`), and dancepack.py sets it (`SECTCRE = \[(?P<header>.+)\]`) so every
  version reads a header alike.
- **A line starting with `#` or `;`** (after any indent) is a comment,
  even inside a value's continuation lines, so a value can't continue on
  one. An indented comment made only of `#rrggbb` tokens inside
  `palette` or `idle_palette` is an error: those colours would be lost.
- **A `#` inside a value** is part of it. When one stands alone (at the
  value's start or after whitespace, and followed by whitespace or the
  line's end) the compiler warns, since it reads like a comment (`name =
  Robo  # my robot`); `Robo #2` and `#ff0000` are quiet.
- Lines count from 1 in the decoded text. A key's line is the line where
  it starts; a move's line is its section header's.
- Sections are `[dancer]`, exactly once, and `[move <name>]`: the word
  before the first whitespace, lowercased, is `dancer` or `move`, and a
  move's name is the rest, stripped. A section that is neither is a near
  miss when, checked in this order, its first word, lowercased, is
  `move` followed by a character that isn't a letter (`move:wave`,
  `move-wave`, `move-1`: the name is what follows that character in the
  header, stripped), or else matches `dancer` or `move` by the rule for
  unknown keys below (a typo such as `dancr`, `moves` or `mvoe`, or
  `dancer` followed by more words: a typo of `move` is named by the
  header's next word). The separator rule comes first, so a one-letter
  name such as `[move-1]`, whose first word also scores 0.8 against
  `move`, skips move `1`. A near miss of `dancer` is a pack error; of
  `move`, it skips that move (a pack error when it has no name:
  `[move2]`, `[moves]`). Any other section is a warning and is ignored,
  so later formats can add sections.
- Lists split on whitespace (continuation lines included), except
  `genre`, which splits on commas, each tag stripped.
- Words (`any`, `none`, the eases, `quiet`, `start`...) match in any
  case. Booleans are configparser's `yes`/`no`, `true`/`false`, `on`/`off`
  and `1`/`0`. Cell names match in any case (`b4` is B4), and so do the
  hex digits of `#rrggbb`.
- Integers are `[0-9]+`, or `[+-]?[0-9]+` for `travel` and the parts of
  `offset`. Ranges are `lo-hi`, spaces allowed around the `-`. **Long
  numbers:** any number of leading zeros, then at most 9 digits; a
  decimal (`weight`, timing) has at most 9 digits after the point. A
  longer number is outside the grammar, so it reads as that key's syntax
  error (`scale must be 1, 2, 3 or 4`) or, in `timing`, as `timing
  "<value>" has a number of more than 9 digits`. So the tool never
  converts a long digit string (Python 3.10.7, 3.9.14 and 3.8.14 on
  refuse one over 4,300 digits, older releases don't), and every version
  agrees.
- **Unknown keys.** A key starting with `x-` is ignored silently.
  Otherwise, for an unknown key, K is the known key of its section that
  swapping two adjacent characters gives, else the one with the highest
  `difflib.SequenceMatcher(None, key, K).ratio()` of at least 0.6, a tie
  going to the earlier key in the tables below. With a swap or a ratio of
  at least 0.75 it is an error, "did you mean K?": in `[dancer]` a pack
  error, in a move a skip. With a ratio from 0.6 up to 0.75 it is a
  warning that ends `(did you mean K?)`, and the key is ignored (`name`
  in a move scores 0.6 against `frames`, `speed` in `[dancer]` 0.6
  against `sheet`). Any other unknown key is a warning.

**`[dancer]`**, in the order used to break ties:

| Key | Syntax | Default | Rules |
|---|---|---|---|
| `name` | text | required | 1-24 B |
| `author` | text | required | 1-40 B |
| `license` | text | required | 1-40 B; on the allowlist under `--strict` |
| `sheet` | a relative path with `/`, inside the folder (no `..`) | `sheet.png` | must exist, each name matched exactly as written (case included) on every file system; no name may end in `.` or a space |
| `cell` | `WxH` | required | W, H ≥ 1; W·scale and H·scale ≤ 160 |
| `scale` | 1-4 | 1 | |
| `anchor` | `x,y` | cellW // 2, cellH | 0 ≤ x < W, 1 ≤ y ≤ H, y·scale ≤ 153, (H − y)·scale ≤ 10 |
| `rest` | a cell | A1 | on the sheet, not blank |
| `shadow` | 0-160 | 0 | screen px |
| `format` | integer | 1 | above 1: "update dancepack.py"; below 1: an error |
| `key` | `#rrggbb` | none | that colour becomes transparent |
| `palette` | `#rrggbb` list | first appearance | ≤ 15, no two with the same RGB565 |
| `idle_palette` | `#rrggbb` list | derived (B.9) | one per palette entry |
| `faces` | `none`, `right`, `left` | `none` | |
| `url` | text | empty | 0-80 B |
| `version` | text | empty | 0-16 B |

Text values must hold no control character; their limits are in UTF-8
bytes.

**`[move <name>]`.** The name is 1-23 of `a-z`, `0-9` and `-`, starting
with a letter or digit. `bpm = none` makes a no-BPM (idle) move; a range
or `any` makes a beat move. In the order used to break ties (C.6 gives
the order of the checks):

| Key | Moves | Syntax | Default | Stored as (A.12, A.13) |
|---|---|---|---|---|
| `row` | both | n ≥ 1 | one of `row` and `frames` is required | the steps' cells: A n, B n … up to the row's last non-blank cell |
| `frames` | both | cell names, 1-32 of them | | the steps' cells |
| `beats` | beat, required | 1, 2, 4, 8 | | beats |
| `bpm` | both, required | `lo-hi` (36-200), `any`, `none` | | kind, paceLo, paceHi |
| `timing` | beat: optional; idle: required | B.1 | the even split | dur |
| `offset` | both | `x,y` per step, no space inside | `0,0` each | dx, dy |
| `ease` | both | one name for every step, or one per step | `hold` | ease |
| `mirror` | both | boolean | no | flags bit 0 |
| `place` | beat | `start`, `end` | neither | flags bit 2 or 3 |
| `weight` | both | 0.1-10 in steps of 0.1 | 1 | weight10 |
| `from`, `to` | both | a pose key (1-23 of `a-z0-9-`) whose mirror twin (A.11) is at most 23 B too | none | fromKey, toKey and their twins |
| `repeat` | beat | 0-8, repeat·beats ≤ 16 | 0 | repeat |
| `freeze` | beat | boolean; one move at most | no | flags bit 1 |
| `heavy` | both | boolean | no | flags bit 4 |
| `snap` | both | boolean | no | flags bit 5 |
| `sway` | beat | 0-16 | 0 | sway |
| `bounce` | beat | 0-16 | 0 | bounce |
| `travel` | beat | −2..2, with beats ≥ 2 | 0 | travel |
| `travel_ease` | beat, with travel | an ease | `inout` | travelEase (0 without travel) |
| `start_at` | beat, with travel | `any`, `centre`, `side`, `left`, `right` | `any` | startAt |
| `go` | beat, with travel | step numbers from 1 and ranges `a-b`, by spaces or commas | none: every step travels | step flags bit 0 |
| `genre` | beat | 1-8 tags, by commas, 1-63 B each | none | a genre run |
| `when` | idle | `quiet`, `listening`, `any` | `any` | when |
| `song_bpm` | beat | `lo-hi`, 1-255 | none | needs bit 0, songLo, songHi |

A key outside its "Moves" column skips the move.

### C.4 Reading the sheet

- **PNG.** The signature, then chunks whose lengths and CRCs are
  checked. IHDR comes first. A critical chunk other than IHDR, PLTE, IDAT
  and IEND is an error; ancillary chunks other than tRNS are ignored (no
  gamma or colour profile: the stored samples are the colours).
  Compression and filter methods are 0. The IDATs are concatenated,
  inflated with zlib and unfiltered row by row (filter types 0-4, Paeth
  as the PNG specification).
- **Accepted:** colour type 0 at 1, 2, 4, 8 or 16 bits; 2 at 8 or 16; 3
  at 1, 2, 4 or 8 (PLTE required); 4 at 8 or 16; 6 at 8 or 16. Interlace
  method 1 is "save without interlacing".
- **To 8-bit RGBA:** a 16-bit sample keeps its high byte; a grey sample
  of d < 8 bits becomes v·255 // (2^d − 1); an indexed pixel takes its
  PLTE colour. Alpha: types 4 and 6 their alpha sample (high byte); type
  3 its tRNS entry (255 past the end of tRNS); types 0 and 2 alpha 0 when
  the raw sample or samples, at the file's own bit depth, equal tRNS's
  value, else 255.
- **Opaque** means alpha ≥ 128. Then a pixel whose 8-bit RGB equals
  `key` is transparent. Only opaque pixels have a colour.
- **Size:** at most 1024×1024, a whole number of cells. Cell names are
  spreadsheet columns (A-Z, then AA, AB…) and rows from 1, in any case
  and with any leading zeros on the row; a name of more than 3 letters
  or a row of more than 4 digits (after its zeros) is no cell, as none
  fits 1024 px.
- **Inflating:** the IDATs are inflated to at most one byte more than the
  image needs (height × (the row's bytes + 1)), so a small file holding
  gigabytes of zeros fails at once with `more than <m> bytes of pixels`.
- **Used cells** are the rest cell and every cell that any move's `row`
  or `frames` names (skipped moves too, near-miss headers of C.3
  included; a `row` or a `frames` token that names no cell of the sheet
  adds nothing), at most 256. So a move's cells count alike however it
  is skipped. **Scan order** is
  the used cells in sheet order (row 1 from A, then row 2...), each cell
  row-major. Colours, partial alpha and "every cell is fully opaque" look
  at the used cells only.
- **The palette.** Opaque colours merge by RGB565 (B.9), the first in
  scan order keeping its RGB888; more than 15 after merging is an error.
  Without `palette =`, indices 1..n follow first appearance in scan
  order. With it, the listed colours are indices 1..n, each opaque pixel
  must match one of them by RGB565, and the listed RGB888 is stored.
- Blank cells (no opaque pixel) are allowed in `row` and `frames`; the
  rest cell can't be blank.

### C.5 What it prints

**Order.**

1. As dancer.ini is read: warnings about unknown keys and sections and
   about a lone `#` in a value, the skip lines of near-miss move headers,
   and the errors a reading finds (a near miss of `[dancer]`, a wrapped
   palette).
2. The `[dancer]` keys, the sheet and the palette: errors, warnings and
   notes. A pack-level error stops here (exit 1).
3. Each move in manifest order: its skip line; or its status line, then
   (`check` only) its echo table, then its warnings and notes.
4. Frame warnings, in frame order (A.14).
5. Pack-level errors found after the moves (frames, moves, pose keys,
   size).
6. The coverage report (`check` only), then the coverage warnings (no
   moves; tempo gaps), which both commands print.
7. With `--strict`: the licence error and the skipped-moves error.
8. The summary line; `build` then prints `wrote <FILE>`.

**Shapes.**

- A location is `dancer.ini:<line>: `, `<sheet>: ` (the `sheet` value as
  written), `<DIR>: `, `<FILE>: `, or nothing.
- After the location: `error: `, `warning: ` or `note: `. A skip line and
  a status line carry no such word.
- A move is named `move "<name>"`.
- Counts of px and bytes of 1,000 or more carry commas (Python's
  `f"{n:,}"`, which ignores the locale). Other numbers are plain.
- Text echoed from dancer.ini or from a path prints each control
  character (U+0000-U+001F and U+007F-U+009F) as `\xNN`, two lowercase
  hex digits, so a downloaded dancer can't send escape sequences to the
  terminal: `unknown section [\x1b]0;x\x07] ignored`.
- **beats_text(t)** for t ticks: w = t // 12 and the remainder r/12
  reduced to p/q. `1 beat` when t = 12, `<w> beats` when r = 0, `<p>/<q>
  beat` when w = 0, `<w> <p>/<q> beats` otherwise: `1/4 beat`, `2 beats`,
  `3 3/4 beats`.

**A skip line:** `dancer.ini:<line>: move "<name>": <message> (skipped)`.

**A status line:**

- beat: `dancer.ini:<line>: move "<name>": <beats_text(12·beats)>, pace
  <lo>-<hi>; <light>` (`pace any` when unranged);
- idle: `dancer.ini:<line>: move "<name>": no BPM, when <when>, <ms> ms;
  <light>`;
- `<light>` is `green: median <m> px, peak <p> of 16,000 px`, `amber:
  median <m> px, peak <p> of 20,000 px` (each light's limit, B.6), or, for
  a red move kept by `heavy`, `red (heavy): median <m> px, peak <p> px,
  over 20,000`. A.17's move reads `dancer.ini:7: move "hop": 1 beat, pace
  any; green: median 4 px, peak 6 of 16,000 px`.

**The echo table** (`check`): indented 4 spaces, a header `step cell
ticks offset ease` (`ms` for idle moves) and a row per step: its number
from 1, the cell as written (then ` (blank)` for a blank cell), the
duration, `x,y`, the ease, and `go` when flagged. Its spacing is free.

**The coverage report** (`check`), here with a few of the test dancer's
rows (C.10):

```
coverage: pace at dance tempo T (h = half time, d = double time, . = not eligible)
  T              72   80   90  100  110  120  130  140  150  160  176
  bounce         72   80   90  100  110  120  130  140  150  160  176
  slow          36h  40h  45h  50h  55h  60h    .    .    .    .    .
  nod*           72   80   90  100    .    .    .  70h  75h  80h  88h
  * genre-tagged: dormant until tracks carry genres
idles: quiet: sway, breathe; listening: sway, look
edges:
  walk: travels from -1; mirrored from +1
  heavy: clamped at both edge spots
```

(The label column is as wide as the whole report's: the test dancer's
longest label is `step-touch`.)

- The columns are T = 72, 80, 90 … 160, 176, each right-aligned in 5
  characters; the labels are left-aligned, indented 2, in a column 2
  wider than the longest label (`T` included). A cell is the
  pace at the k of `eligible_k(10·T, lo, hi, none, beats, travel)`:
  `<pace>` at 0, `<pace>h` at −1, `<pace>d` at +1, `.` for none. So a
  2-beat travel move never shows a `d` (B.7).
- The rows are the kept beat moves without a needs bit, in MOVS order. A
  genre-tagged move's label ends with `*`, and the footnote line follows
  when one does.
- `idles:` names, per state, the idle moves for it in MOVS order, joined
  with `, `. A state with none reads `(the listening idles)` or `(the
  quiet idles)` when the other has some, and `auto-idle` when neither
  has.
- `edges:` has a line per kept beat move with something to say, in MOVS
  order: a move
  without travel that doesn't fit at an edge spot in an orientation it
  plays (B.11) is `clamped at spot -1`, `clamped at spot +1` or `clamped
  at both edge spots`; a travel move is `travels from <spots>`, with `;
  mirrored from <spots>` when it mirrors (spots `-1`, `0`, `+1`, joined
  with ` and `). With no line, it reads `edges: every move fits at both
  edge spots`.
- **The coverage warning:** over the integer tempos T = 72..176, with
  the kept beat moves that have neither genre tags nor a needs bit, each
  run of tempos where none is eligible (eligible_k with the move's
  travel) gives `warning: no beat move at dance tempo <a>-<b>:
  auto-bounce plays there` (`<a>` alone for one tempo).

**The summary:** `<id>: <n> move[s] (<s> of <w> skipped), <f> frame[s],
<bytes> bytes`, e.g. `dot: 1 move (0 of 1 skipped), 1 frame, 1,156
bytes`. `<n>`, `<f>` and `<bytes>` count what the pack holds (`<n>` is
MOVS's count), and `<w>` the moves dancer.ini writes, near-miss headers
included: five moves with one skipped read `4 moves (1 of 5 skipped)`.

### C.6 The message catalogue

Every message the compiler prints. `<…>` are filled in; "at" says which
line a location names (a key's own line when it is present, else the
section's). Hex digits print lowercase in `#rrggbb` and uppercase after
`0x`, as `0x%04X`: `#fe0000`, `0xF800`.

**Pack errors** (exit 1; nothing is written):

| Message | At |
|---|---|
| `<DIR>: error: no dancer.ini` | — |
| `<DIR>: error: the folder name "<name>" can't be an id: use 1-23 of a-z, 0-9 and -, starting with a letter or digit` | — |
| `dancer.ini: error: can't read it (<reason>)` | — (the operating system's reason) |
| `dancer.ini:<line>: error: not UTF-8: save it as UTF-8` | the first bad byte |
| `dancer.ini:<line>: error: duplicate section [<section>]` | the second |
| `dancer.ini:<line>: error: duplicate key "<key>" in [<section>]` | the second |
| `dancer.ini:<line>: error: "<text>" is outside any section` | it |
| `dancer.ini:<line>: error: can't read "<text>": not key = value` | it |
| `dancer.ini:<line>: error: a move needs a name: [move <name>]` | the section |
| `dancer.ini:<line>: error: duplicate move "<name>"` | the second |
| `dancer.ini:<line>: error: unknown section [<section>]: did you mean [dancer]?` | the section (a near miss, C.3) |
| `dancer.ini:<line>: error: unknown section [<section>]: did you mean [move <name>]?` | the section (a near miss of `move` with no name) |
| `dancer.ini:<line>: error: an indented line starting with # is a comment, not more colours: keep <key> on one line` | that line (`palette` or `idle_palette`) |
| `dancer.ini: error: no [dancer] section` | — |
| `dancer.ini:<line>: error: [dancer] needs <key> =` | `[dancer]` |
| `dancer.ini:<line>: error: unknown key "<key>" in [dancer]: did you mean <known>?` | the key |
| `dancer.ini:<line>: error: format <n> is newer than this dancepack.py knows (1): update dancepack.py` | `format` |
| `dancer.ini:<line>: error: format must be 1` | `format` |
| `dancer.ini:<line>: error: <key> is empty` | the key |
| `dancer.ini:<line>: error: <key> is <n> bytes: at most <max>` | the key |
| `dancer.ini:<line>: error: <key> has a control character` | the key |
| `dancer.ini:<line>: error: sheet must be a path inside the dancer's folder`, then `: use / between folders` when the value holds a `\` | `sheet` |
| `dancer.ini:<line>: error: sheet: "<name>" ends in a dot or a space, which Windows drops: rename it` | `sheet` |
| `dancer.ini:<line>: error: no sheet at <sheet>`, then ` (the file is <path>: names are case-sensitive)` when the names differ from a file's only in case | `sheet` |
| `dancer.ini:<line>: error: cell must be WxH, like 64x76` | `cell` |
| `dancer.ini:<line>: error: scale must be 1, 2, 3 or 4` | `scale` |
| `dancer.ini:<line>: error: cell <w>x<h> at scale <s> is <W>x<H> on screen: at most 160x160` | `cell` |
| `dancer.ini:<line>: error: anchor must be x,y` | `anchor` |
| `dancer.ini:<line>: error: anchor <x>,<y> is outside the <w>x<h> cell` | `anchor` |
| `dancer.ini:<line>: error: anchor y <y> at scale <s> puts the cell's top <n> px above the ground: at most 153` | `anchor` |
| `dancer.ini:<line>: error: the cell reaches <n> screen px below the ground: at most 10` | `anchor` |
| `dancer.ini:<line>: error: shadow must be 0-160 (screen px)` | `shadow` |
| `dancer.ini:<line>: error: faces must be none, right or left` | `faces` |
| `dancer.ini:<line>: error: key must be #rrggbb` | `key` |
| `dancer.ini:<line>: error: <key>: "<token>" isn't #rrggbb` | `palette` or `idle_palette` |
| `dancer.ini:<line>: error: palette lists #<a> and #<b>, both RGB565 0x<hhhh>` | `palette` |
| `dancer.ini:<line>: error: palette has <n> colours: at most 15` | `palette` |
| `dancer.ini:<line>: error: idle_palette has <n> colour[s]; the palette has <m>` | `idle_palette` |
| `<sheet>: error: can't read it (<reason>)` | — |
| `<sheet>: error: not a PNG` | — |
| `<sheet>: error: damaged PNG (<detail>)`: `bad CRC in <chunk>`, `truncated`, `no IHDR`, `no IDAT`, `no PLTE`, `bad PLTE` (its length isn't 1-256 entries of 3 bytes, or a pixel indexes past it), `bad IHDR`, `bad zlib data`, `bad filter <n>`, `unknown critical chunk <chunk>`, `<n> bytes of pixels, not <m>` (too few), `more than <m> bytes of pixels` (too many: C.4 stops inflating) | — |
| `<sheet>: error: colour type <t> at <d> bits isn't valid PNG` | — |
| `<sheet>: error: save without interlacing` | — |
| `<sheet>: error: <w>x<h>: at most 1024x1024` | — |
| `<sheet>: error: <w>x<h> isn't a whole number of <cw>x<ch> cells` | — |
| `dancer.ini:<line>: error: rest <cell> isn't a cell of the <c>x<r> sheet` | `rest` |
| `dancer.ini:<line>: error: the rest frame <cell> is empty` | `rest` |
| `dancer.ini: error: <n> cells used: at most 256` | — |
| `<sheet>: error: every cell is fully opaque: export with transparency, or set key = #<rrggbb> (the top-left pixel of <cell>)` | — (`<cell>` is the first used cell in sheet order) |
| `<sheet>: error: <n> colours: at most 15. By pixel count: <list>` | — |
| `<sheet>: error: colour #<rrggbb> (in <cells>) isn't in palette =` | — |
| `error: <n> frames: at most 256` | — |
| `error: <n> moves: at most 64` | — |
| `error: <n> pose keys: at most 255` | — |
| `error: the pack is <n> bytes: at most 131,072` | — |
| `error: internal: the pack doesn't read back (<reason>): please report it` | — (the compiler's own pack fails A.15: a bug, never an exception) |
| `dancer.ini:<line>: error: license "<licence>" isn't on the built-in allowlist: GPL-3.0-or-later, GPL-2.0-or-later, CC0-1.0, CC-BY-4.0, CC-BY-SA-4.0, MIT, Apache-2.0` | `license`; `--strict` only |
| `error: --strict: <n> move[s] skipped` | `--strict` only |
| `<FILE>: error: <reason>` | `info`: A.15's reasons, `no such file`, `a folder, not a .dnc pack` or `can't read it (<reason>)` |
| `<FILE>: error: can't write it (<reason>)` | `build`'s or `builtin`'s output |

`<list>` is the 15 most used colours by pixel count, most first (a tie
in scan order), as `#<rrggbb> <n> px`, joined with `, `; a colour under
16 px adds ` in <cells>`. Then `, and <k> more colour[s] of at most <m>
px each, in <cells>` sums up the rest (no `each` when k is 1; `<m>` the
largest of their counts, `<cells>` where they all sit), and the line
ends ` (smoothing or anti-aliasing in the export adds colours: export at
100 % with no smoothing)`. With this error no merge note prints.
`<cells>` is at most 8 cell names, in sheet order, joined with `, `
(then ` …` when there are more).

**Move skips**, `dancer.ini:<line>: move "<name>": <message> (skipped)`.
A move's checks run in this order and the first failure is its only
line:

| Message | At |
|---|---|
| `unknown section [<section>]: did you mean [move <name>]?` | the section (a near miss, C.3; printed as dancer.ini is read) |
| `its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit` | the section |
| `unknown key "<key>": did you mean <known>?` | the key |
| `needs bpm = (a range like 90-130, any, or none)` | the section |
| `bpm must be lo-hi, any or none` | `bpm` |
| `<key> applies to beat moves only` | the key |
| `<key> applies to no-BPM moves only` | the key |
| `<key> needs travel =` (`travel_ease`, `start_at`, `go`, when `travel` is absent or reads 0) | the key |
| `give row = or frames =` | the section |
| `give row = or frames =, not both` | `row` |
| `row <n> isn't a row of the <c>x<r> sheet` | `row` |
| `row <n> is empty` | `row` |
| `frames: "<token>" isn't a cell of the <c>x<r> sheet`, then `: separate cells with spaces` when the token holds a comma | `frames` |
| `<n> frames: at most 32 steps` | `row` or `frames` |
| `needs beats = 1, 2, 4 or 8` | the section |
| `beats must be 1, 2, 4 or 8` | `beats` |
| `a no-BPM move needs timing = (s or ms)` | the section |
| `timing has <n> value[s] for <m> step[s]: give one per step` | `timing` |
| `timing "<value>" has a number of more than 9 digits` | `timing` |
| `timing "<value>" isn't a number of beats, like 1/4, 0.5 or 2` | `timing` |
| `timing <value> is off the 1/12-beat grid by more than 1/48 beat` | `timing` |
| `timing "<value>" needs s or ms, like 2s or 400ms` | `timing` |
| `timing <value> isn't a whole number of 10 ms` | `timing` |
| `<n> frames can't share <beats_text> evenly on the 1/12 grid: give timing =` | `row` or `frames` |
| `step <i> lasts <t> ticks: at least 3 (1/4 beat)` | `timing`, else `row` or `frames` |
| `steps last <t> ticks (<beats_text(t)>); a <b>-beat move needs <12·b>` | `timing` |
| `step <i> lasts <ms> ms: 200-5,000 ms` | `timing` |
| `the steps last <ms> ms in all: 500-12,000 ms` | `timing` |
| `offset has <n> value[s] for <m> step[s]: give one per step` | `offset` |
| `offset "<token>" isn't x,y` | `offset` |
| `offset <x>,<y> moves <n> screen px: at most 48` | `offset` |
| `ease "<word>" isn't hold, linear, in, out or inout` | `ease` or `travel_ease` |
| `ease has <n> value[s] for <m> step[s]: give one, or one per step` | `ease` |
| `<key> must be yes or no` | the key |
| `place must be start or end` | `place` |
| `weight must be 0.1-10 in steps of 0.1` | `weight` |
| `<key> "<value>": pose keys are 1-23 of a-z, 0-9 and -` | `from` or `to` |
| `<key> "<value>": its mirror twin "<twin>" is <n> bytes: at most 23` | `from` or `to` |
| `repeat must be 0-8` | `repeat` |
| `repeat <r> x <b> beats is <r·b> beats: at most 16` | `repeat` |
| `freeze is already set on move "<first>"` | `freeze` |
| `sway must be 0-16 (native px)` | `sway` |
| `bounce must be 0-16 (native px)` | `bounce` |
| `travel must be -2 to 2 (spots)` | `travel` |
| `a travel move needs at least 2 beats` | `travel` |
| `start_at must be any, centre, side, left or right`, then `: did you mean centre?` for `center` | `start_at` |
| `go: "<token>" isn't a step (1-<m>) or a range of them` | `go` |
| `empty genre tag` | `genre` |
| `more than 8 genre tags (<n>)` | `genre` |
| `genre tag "<tag>" is <n> bytes: at most 63` | `genre` |
| `genre tag "<tag>" has a control character` | `genre` |
| `genre tag "<tag>" folds to nothing: use letters or digits Full folding can spell` | `genre` |
| `when must be quiet, listening or any` | `when` |
| `song_bpm must be lo-hi, 1-255` | `song_bpm` |
| `bpm <lo>-<hi>: each bound must be 36-200` | `bpm` |
| `bpm <lo>-<hi>: lo is above hi` | `bpm` |
| `range <lo>-<hi> can't be reached: the fastest pace is <b>` | `bpm` |
| `range <lo>-<hi> can't be reached: the slowest pace is <a>` | `bpm` |
| `can't travel from any spot: too wide there, or start_at rules every spot out` | `travel` |
| `budget red: peak <p> px is over 20,000; set heavy = yes to keep it` | the section |

**Warnings** (the build goes on):

| Message | At |
|---|---|
| `dancer.ini:<line>: warning: unknown key "<key>" in [<section>] ignored`, then ` (did you mean <known>?)` for a weak match (C.3) | the key |
| `dancer.ini:<line>: warning: unknown section [<section>] ignored` | the section |
| `dancer.ini:<line>: warning: <key>: "<text>" is part of the value; a comment after a value starts with ;` | the key (`<text>` runs from the lone `#` to its line's end, C.3) |
| `dancer.ini:<line>: warning: <key>: <n> character[s] the device fonts lack will show as a look-alike or "?": <U+XXXX, …>` | `name`, `author`, `license`, `url` or `version`; code points ascending, at least 4 hex digits |
| `<sheet>: warning: <n> px with partial alpha in <cells>: alpha 128-254 counts as opaque, 1-127 as transparent` | — |
| `<sheet>: warning: colour <i> #<rrggbb> is close to the background #080c10: it will vanish` | — |
| `dancer.ini:<line>: warning: the rest frame <cell>'s feet end on row <r>, <d> native px above the ground row (anchor y <y>): <fix>` (or `below`) | `anchor`, else `[dancer]` |
| `<sheet>: warning: the drawing in <cell> runs on into <next>: is cell = <w>x<h> right?` | — |
| `dancer.ini:<line>: warning: idle colour <i> #<rrggbb> is close to the background #080c10: it will vanish while idle` | `idle_palette`, else `[dancer]` |
| `dancer.ini:<line>: warning: palette colour #<rrggbb> isn't used` | `palette` |
| `dancer.ini:<line>: warning: move "<name>": timing <value> snapped to <beats_text(n)>` | `timing` |
| `dancer.ini:<line>: warning: move "<name>": beat <b> lands inside step <i> (<cell>): no step starts on it; fine for a hold, otherwise set timing = so one does` | `timing`, else the section |
| `dancer.ini:<line>: warning: move "<name>": the first key is <x>,<y>, not 0,0: the dancer jumps when another move follows` | `offset` |
| `dancer.ini:<line>: warning: move "<name>": the feet drift <d> native px across its standing frames` | the section |
| `dancer.ini:<line>: warning: move "<name>": travels <left or right> while the art faces <right or left>` | `travel` |
| `dancer.ini:<line>: warning: move "<name>": song_bpm needs track BPM data: skipped in this version` | `song_bpm` |
| `dancer.ini:<line>: warning: move "<name>": budget red (peak <p> px), kept because heavy = yes` | `heavy` |
| `<sheet>: warning: frame <cell>: standing, it reaches stage row <r>, under the toast (rows 0-35)` | — |
| `<sheet>: warning: frame <cell>: <n> runs, over 300: slow to draw` | — |
| `warning: no moves: only the engine's auto-bounce and auto-idle will play` | — |
| `warning: no beat move at dance tempo <a>-<b>: auto-bounce plays there` | — |

**Notes:**

| Message | At |
|---|---|
| `<sheet>: note: #<later> merges with #<first> (both RGB565 0x<hhhh>)` | — |
| `dancer.ini:<line>: note: move "<name>": step <i> (<cell>) is blank: the dancer disappears for <length>` | `row` or `frames` |
| `dancer.ini:<line>: note: move "<name>": genre tags keep it dormant until tracks carry genres` | `genre` |

**The triggers that need a definition:**

- **The hit:** in a beat move, a step that starts before tick 12·(b − 1)
  and ends after it, for b = 2..beats; `<i>` is that step's number from
  1 and `<cell>` its cell as written.
- **A blank step:** a step whose cell has no opaque pixel; `<length>` is
  beats_text(its ticks) for a beat move and `<ms> ms` for an idle move.
  A note, not a warning: blank frames can be deliberate (4.2).
- **The first key** warning: a beat move whose first step's key isn't
  0,0.
- **The feet drift:** in a move without travel, for each standing step
  (its cell's box bottom, y0 + h, equals anchorY, and its key's dy is
  0: a step its key lifts isn't on the ground), take the leftmost and
  rightmost opaque columns xa and xb of the box's bottom row in the
  step's cell as drawn on the sheet, and c = xa + xb + 2·dx. When the
  step's ease isn't `hold` and the next step's key (j = (i + 1) mod n)
  has another dx and dy 0, the ease glides the same feet to that key by
  the step's end, so xa + xb + 2·dx_j counts too. When max(c) − min(c) >
  2 (more than 1 native px), d = (max(c) − min(c) + 1) // 2. So feet
  drawn against an offset sway still drift: offsets move the whole
  drawing (4.4).
- **The rest frame's feet:** the rest cell's box bottom y0 + h isn't
  anchorY. `<r>` = y0 + h − 1 and `<d>` = |anchorY − (y0 + h)|. `<fix>`
  is `set anchor = <x>,<y0 + h> to stand it on the ground` (x the
  anchor's) when that anchor obeys C.3's rules, else `draw it <d> native
  px lower` (above) or `higher` (below).
- **Runs on:** a used, non-blank cell runs on when, on some row, its
  last column and the next cell's first column (the cell to its right on
  the sheet) are both opaque. It warns when at least half of the used
  non-blank cells run on, naming the first in sheet order: drawings cut
  in two by a cell width that is too small.
- **Faces:** `faces = right` with travel < 0, or `faces = left` with
  travel > 0 (the unmirrored direction).
- **Standing frame under the toast:** a standing frame whose top, 153 +
  (y0 − anchorY)·scale, is below 36. `<cell>` is the frame's first cell
  in A.14's order.
- **Runs:** a frame's tokens (A.10) with a non-zero index.
- **The font warning:** a code point outside `FONT_COVERAGE` (C.2).
- The status line's light and the red skip come from B.6; `song_bpm`
  moves keep their needs bit and are written (the device skips them), so
  they aren't skips.

### C.7 `--strict` and the allowlist

- `--strict` (on `check` and `build`; always on for `builtin` and
  `--check`) fails the build when any move is skipped, or when `license`
  isn't on the allowlist, exactly (case-sensitive): GPL-3.0-or-later,
  GPL-2.0-or-later, CC0-1.0, CC-BY-4.0, CC-BY-SA-4.0, MIT, Apache-2.0
  (decision 14).
- Warnings and notes never fail it. A `song_bpm` move isn't a skip (C.6),
  and neither is a red move kept by `heavy`.
- So a built-in can't hold a deliberately skipped move: the skips are
  tested on throwaway dancers that test_dancepack.py writes to a
  temporary folder, and the device-side drops on bytes test_dance_pack
  patches into a copy of the fixture (with the size and CRC recomputed).

### C.8 Generated files

All are UTF-8 with LF line endings and end with a newline. Numbers are
decimal, except hashes, written `0x%08Xu`, and pack bytes, written
`0x%02X`, 16 a line. Each starts:

```
// SPDX-License-Identifier: <licence>
// Copyright (C) <holder>
// Generated by tools/dancepack.py from <source>. Do not edit:
// change the source and run `python tools/dancepack.py builtin`.
```

For a pack, `<licence>` and `<holder>` are the pack's licence and author
and `<source>` the dancer folder (`tools/art/dancers/test`). For the
vector headers they are `GPL-3.0-or-later`, `2026 IrosTheBeggar`, and
`its own tables` (Eligibility, Fold) or `tools/art/dancers/test`
(Budget).

**A pack as a header** (`.h`, the fixture), the symbols named `k<stem>`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

// (comments: the dancer, then each frame: its cells, its box and its
// pixels as text, '.' for 0 and 1-9, a-f for the index)
namespace dance {
alignas(4) static const uint8_t kTestPack[<size>] = { … };
static const size_t kTestPackSize = <size>;
static const uint32_t kTestPackFrameHash[<frames>] = { … };  // A.16
static const int kTestPackFrameCount = <frames>;
}  // namespace dance
```

**A pack as a `.cpp`** (StickPack.cpp, step 7): the same comments, then
in `namespace dance` the `extern const` declarations of the four symbols
followed by their definitions (`alignas(4) const uint8_t kStickPack[…]`),
so they have external linkage without a header.

**The vector headers** (`#pragma once`, `<cstdint>`, `namespace
dancevec`), each array with a `static const int k…Count`:

```cpp
// EligibilityVectors.h: B.7's eligible_k and pace_holds on grids
constexpr int8_t kNoK = 127;  // prevK: no previous ranged move; k: not eligible
struct Eligibility { uint16_t t10; uint8_t lo, hi; int8_t prevK; uint8_t beats; int8_t travel; int8_t k; };
static const Eligibility kEligibility[] = { … };
struct PaceHolds { uint16_t t10; int8_t k; uint8_t lo, hi; uint8_t holds; };  // holds: 1 or 0
static const PaceHolds kPaceHolds[] = { … };

// FoldVectors.h: B.8's fold_genre and key
struct Fold { const char* tag; const char* key; uint32_t hash; };  // key "" and hash 0: folds to nothing
static const Fold kFold[] = { … };

// BudgetVectors.h: B.6 on every move of kTestPack (move = its MOVS index)
struct BudgetCase { uint8_t move, fps, pace, mirrored; int8_t spot; uint16_t frames; uint32_t median, peak; };
static const BudgetCase kBudgetCases[] = { … };   // every case, in B.6's order
struct MoveBudget { uint8_t move; uint32_t median, peak; uint8_t light; };  // 0 green, 1 amber, 2 red
static const MoveBudget kMoveBudgets[] = { … };
struct TraceCase { uint8_t move, fps, pace, mirrored; int8_t spot; uint16_t first, count; };
static const TraceCase kTraceCases[] = { … };     // one per move
struct TraceFrame { uint16_t n; uint32_t pos16; uint16_t frame;  // bit 15: drawn mirrored
                    int16_t rootX, rootY, x0, y0, x1, y1;        // the character rect; 0s when empty
                    uint32_t dirty; };                           // 0 for n = 0
static const TraceFrame kTrace[] = { … };         // frames 0..N of each trace case
```

- **The eligibility grid**, in nested order: T10 in 720, 800, 870, 880,
  900, 901, 957, 1200, 1600, 1740, 1760; (lo, hi) in (0, 0), (36, 60),
  (36, 200), (60, 100), (88, 88), (90, 130), (100, 160), (110, 180),
  (144, 180), (150, 200), (170, 180), (177, 200); prevK in none, −1, 0,
  +1; beats in 1, 2, 4, 8; travel in 0, 1 (1 only with 2 beats or more,
  as a travel move has). 3,696 cases.
- **The pace-holds grid**, in nested order: T10 in 720, 800, 870, 880,
  900, 901, 957, 969, 970, 1030, 1031, 1200, 1600, 1740, 1760; k in −1, 0,
  +1; (lo, hi) in the eligibility grid's ranges, then (100, 100). 585
  cases.
- **The fold cases** are B.8's table, one row per tag, in its order (32
  tags; the last three are `日本`, two spaces and the empty string). A
  tag's bytes of 0x80 and above are written as three-digit octal escapes
  (`\303\211`), `"` and `\` are escaped, and every other byte is written
  as itself.
- **The trace case** of a move is its first case (B.6's order) at 30 fps
  and pace (rlo + rhi) // 2, or at 30 fps for an idle move.
- Each count is the array's name plus `Count`: `kEligibilityCount`,
  `kPaceHoldsCount`, `kFoldCount`, `kBudgetCasesCount`,
  `kMoveBudgetsCount`, `kTraceCasesCount` and `kTraceCount`. A
  BudgetCase's `frames` is B.6's N, the number of dirty values. "Every
  move" is every MOVS record, so `future`, which a v1 reader drops for
  its needs bit, has its cases and trace too.

### C.9 The Python API

test_dancepack.py imports dancepack and calls these directly. Names and
argument orders are fixed; the record types are `collections.namedtuple`.

```python
class DanceError(ValueError)   # args[0]: a C.6 message, without location or "(skipped)",
                               # or an A.15 reason (read_pack)

Geom(scale, faces, cell_w, cell_h, anchor_x, anchor_y, shadow_w, frame_count, rest_frame)
Box(x0, y0, w, h)
Step(frame, mirrored, dur, dx, dy, ease, go)          # mirrored, go: bool; ease: 0-4
Move(name, kind, beats, pace_lo, pace_hi, weight10, when, flags, repeat, needs,
     travel, start_at, travel_ease, bounce, sway, from_key, to_key, from_key_m,
     to_key_m, genres, song_lo, song_hi, steps)       # keys: str or None; genres, steps: tuples
Pack(size, crc, id, name, author, licence, url, version, declared_moves, geom,
     palette, idle_palette, thumb, boxes, grids, pose_keys, moves, ignored)
     # palettes: 16 (r, g, b) each; thumb: 800 bytes; grids: per frame, cell_h rows of
     # cell_w-byte bytes; ignored: tuple of (move index, A.15 reason)
     # moves holds every MOVS record, decoded as far as it goes (a bad
     # name is None, steps past STEP's end are ()), so a move index indexes moves and
     # the dropped ones are those ignored names
Pose(frame, mirrored, root_x, root_y, box, shadow, rect)   # rects (x0, y0, x1, y1) or None
Case(fps, pace, mirrored, spot, frames, median, peak)
Budget(cases, median, peak, light)                     # light: "green", "amber" or "red"
Result(ok, data, pack, messages, report, skipped, budgets)
     # data: the pack's bytes or None; messages: the diagnostic lines of C.5 in order
     # (no echo tables, no coverage report); report: every line check prints;
     # skipped: names; budgets: {name: Budget} for the kept moves

FORMAT, MIN_READER, MAX_PACK          # 1, 1, 131072
STAGE_W, STAGE_H, GROUND_Y, SPOTS     # 320, 164, 153, (64, 160, 256)
EASES                                 # ("hold", "linear", "in", "out", "inout")
LICENCE_ALLOWLIST, TEXTFOLD, FONT_COVERAGE, BUILTINS, ROOT

fnv1a32(data) -> int                  crc32(data) -> int
rgb565(r, g, b) -> int                idle_colour((r, g, b)) -> (r, g, b)
near_background((r, g, b)) -> bool
textfold_full(text) -> str            fold_genre(tag) -> str
genre_key(tag) -> int or None         pose_twin(key) -> str
parse_beat_time(text) -> (ticks, snapped)          # raises DanceError
parse_idle_time(text) -> units                     # 10 ms units; raises DanceError
even_split(frames, beats) -> [ticks, ...]          # raises DanceError
beats_text(ticks) -> str
ease_q16(ease, f) -> int              hop_q16(phi) -> int
sway_q16(beat, phi) -> int
eligible_k(t10, lo, hi, prev_k, beats, travel=0) -> int or None   # prev_k: -1, 0, 1 or None
pace_holds(t10, k, lo, hi) -> bool
reach(lo, hi, beats, travel=0) -> (rlo, rhi) or None
frame_at(geom, boxes, move, pos16, mirrored, spot, e16=65536) -> Pose   # pos16 < 0: ValueError
dirty_px(prev_rect, rect) -> int      # prev_rect None: area(rect)
extent(geom, boxes, move, mirrored) -> (lo, hi) or None
fits(geom, boxes, move, mirrored, spot) -> bool
start_spots(geom, boxes, move, mirrored) -> [spot, ...]
budget(geom, boxes, move) -> Budget
trace(geom, boxes, move, fps, pace, mirrored, spot) -> [(n, pos16, Pose, dirty or None), ...]
encode_frame(grid, box) -> bytes      # A.10's canonical rows
thumbnail(grid, box) -> bytes         # A.8, 800 bytes
frame_hash(grid) -> int               # A.16
read_png(data) -> (width, height, rows)   # rows[y][x] = (r, g, b, a), 8-bit (C.4); raises DanceError
compile_dir(path, strict=False) -> Result
read_pack(data, check_crc=True) -> Pack   # A.15 as for a card pack (budget included);
                                          # raises DanceError(reason)
parse_textfold_cpp(text) -> {code point: replacement}
parse_vlw_coverage(text) -> [(first, last), ...]
generated_files() -> [(path, text), ...]  # every registry output and vector header
main(argv=None) -> int                # the command line; usage errors raise SystemExit(2)
```

`spot` is −1, 0 or +1, `mirrored` a bool, `pos16` and `e16` as in B.4,
and `grid` a cell grid as in Pack.grids. `budget`, `trace` and
`frame_at` read only the fields B.4-B.6 use, so a test can build the
records by hand. Printed lines escape control characters (C.5); a
DanceError's message doesn't.

### C.10 The test dancer

`tools/art/dancers/test/` (id `test`) uses every manifest key, and its
pack becomes `test/support/dance/TestPack.h`. It compiles under
`--strict` with no skipped move. The art is free; the geometry, the
cells' roles and the moves are fixed, so tests can name them.

**`[dancer]`:** `name = Tëst` (non-ASCII, inside the fonts' coverage),
`author = IrosTheBeggar`, `license = GPL-3.0-or-later`, `url` and
`version` set, `sheet = sheet.png`, `cell = 24x32`, `scale = 4`, `anchor
= 12,30`, `rest = A1`, `shadow = 35` (odd, so B.5's sw // 2 rounds), `format = 1`, `key = #ff00ff`,
`palette` (4 colours) and `idle_palette` (4) given, `faces = right`, and
an `x-note` key.

**`sheet.png`:** colour type 2 (RGB, 8-bit, no alpha), magenta #ff00ff
wherever transparent, 4 columns by 11 rows of cells (96×352).

| Cells | Role |
|---|---|
| A1, B1, C1, D1 | stand (the rest), squash, a pixel copy of A1, air |
| A2, B2 | a lean, and its exact mirror image drawn |
| A3-C3 | the point |
| A4, B4, C4 | a blank B4 between two drawn cells |
| A5-C5 | the fast steps |
| A6-D6 | the walk, drawn in place |
| A7, B7 | the nod |
| A8 | the whole cell filled: the heavy block |
| A9, B9 | sway, drawn in place: A9 is a pixel copy of A1, B9 A1 with a mark on the head |
| A10-C10 | breathe |
| A11, B11 | look |

**The moves**, in this manifest (and MOVS) order:

| Move | Keys | What it exercises |
|---|---|---|
| `bounce` | `row = 1`, `beats = 2`, `bpm = any`, `bounce = 3`, `freeze = yes` | row, the even split, bounce, freeze, a duplicate cell |
| `pulse` | `frames = A1 B1 D1 B1`, `beats = 1`, `bpm = any`, `timing = 1/4 0.25 1/4 0.26`, `offset = 0,0 0,-1 0,-2 0,-1`, `ease = out`, `snap = yes`, `sway = 1` | fractions, decimals, a snap warning, one ease for all, snap, a 1-beat sway (B.3's b since the run started) |
| `step-touch` | `frames = A2 B2 A2 B2`, `beats = 2`, `bpm = any`, `mirror = yes`, `sway = 2`, `from = stand`, `to = stand`, `offset = 0,0 -1,0 0,0 1,0`, `ease = linear out in hold` | a mirror-image cell, sway, an ease list |
| `point` | `frames = A3 B3 C3 B3`, `beats = 4`, `timing = 1/2 1/2 1 2`, `offset = 0,0 2,-2 4,-3 2,-2`, `ease = inout`, `bpm = 90-130`, `mirror = yes`, `place = start`, `weight = 0.3`, `repeat = 2`, `from = stand`, `to = left` | section 4.5's example; a pose key with a twin |
| `slow` | `row = 4`, `beats = 4`, `bpm = 36-60`, `weight = 2.5` | a blank frame, half time only |
| `fast` | `frames = A5 B5 C5`, `beats = 1`, `timing = 1/4 1/2 1/4`, `bpm = 150-180`, `place = end`, `from = low`, `to = stand` | double time, place end |
| `walk` | `row = 6`, `beats = 4`, `bpm = 90-140`, `mirror = yes`, `travel = 1`, `travel_ease = linear`, `start_at = side`, `go = 2-3` | travel: from −1, mirrored from +1 |
| `nod` | `frames = A7 B7 A7`, `beats = 1`, `timing = 1/3 1/3 1/3`, `bpm = 70-100`, `genre = hip hop, R&B, drum n bass` | triplets, genre tags |
| `future` | `frames = A1 B1`, `beats = 2`, `bpm = any`, `song_bpm = 160-180` | the needs bit, kept |
| `big` | `frames = A8 A8`, `beats = 2`, `bpm = 100-160`, `offset = -4,0 4,0` | an amber light |
| `heavy` | `frames = A8 A8`, `beats = 2`, `bpm = any`, `offset = -12,0 12,0`, `heavy = yes` | a red light, kept |
| `sway` | `frames = A9 B9`, `bpm = none`, `timing = 2s 2s`, `offset = -2,0 2,0`, `ease = inout`, `when = any` | an idle with offsets and an ease; its feet slide (the drift warning) |
| `breathe` | `frames = A10 B10 C10`, `bpm = none`, `timing = 1.1s 1.1s 1.1s`, `when = quiet` | a quiet idle whose 3.3 s play ends exactly on a 30 fps frame (B.6's N) |
| `look` | `frames = A11 B11 A11`, `bpm = none`, `timing = 500ms 1s 1.5s`, `when = listening`, `mirror = yes` | a listening idle, ms, a mirrored idle |

- **Lights:** `big` amber and `heavy` red (both A8 moved 4 or 12 native
  px either side: 128×128 and 192×128 screen px at the switch); every
  other move green. The art is drawn to keep it so.
- **Pose keys:** stand, left, right (the twin), low: P = 4.
- So the fixture holds: identical cells (C1 = A1, and A1 reused), a
  mirror-image cell (B2), the blank frame (B4), every ease, each `when`,
  travel with `go` and `start_at`, three genre tags, a needs bit, a heavy
  red move, an odd shadow, a 1-beat sway, an idle play ending on a frame,
  and the expected warnings and notes: the snap, `song_bpm`, the kept red
  move, the dormant genre note, the first key of `big` and `heavy`, the
  hits (`point`'s beat 4 inside step 4, B3; `slow`'s beats 2, 3 and 4
  inside steps 1-3, A4, B4 and C4: they follow from the fixed timings, a
  2-beat last step and three 16-tick steps), `slow`'s blank step 2 (B4),
  and `sway`'s feet drift (4 native px: its offsets slide feet drawn in
  place).
- The art keeps every other warning away: standing frames leave row 0
  empty, the rest frame stands on the ground row, art is drawn in place
  (except `point`'s B3 and C3, whose feet are drawn against their
  offsets; the drift rule ignores them anyway, as their keys lift
  them), and the lean (A2) is drawn 1 px off the ground, since as a
  standing frame step-touch's ±1 offsets and glides on its mirror image
  would drift the feet 2 px. A7 and A9 are pixel copies of A1. The
  compiled fixture: 14 moves, 25 frames, 5,380 bytes, with only B2 stored
  as a mirrored reference.

### C.11 test_dancepack.py, and CI

**The suite** writes only to temporary folders (`tempfile`), builds its
throwaway sheets with its own small PNG encoder (zlib and struct), and
covers at least:

- every message of C.6, each with its line, including the doc's own
  quotes (`did you mean bpm?`, `5 frames can't share 1 beat evenly on the
  1/12 grid: give timing =`, `range 185-200 can't be reached: the
  fastest pace is 180`, `every cell is fully opaque: export with
  transparency, or set key = #ff00ff (the top-left pixel of A1)`,
  `song_bpm needs track BPM data: skipped in this version`, `save without
  interlacing`, `update dancepack.py`);
- the PNG forms of C.4 (every colour type and bit depth, tRNS, `key`,
  16-bit, interlacing, damage), and the INI rules (`license = X ;
  comment` reads as X; `palette = #ff0000 #00ff00` reads both; duplicate
  keys; a BOM and CRLF; `x-` keys);
- A.17's 1,156 bytes exactly, and a round trip through `read_pack`;
- A.15's rejections and drops on patched copies, with their exact
  reasons, including the bounds a 32-bit reader could get wrong (a
  directory off past the total size; an FTAB off near 2^32; string
  offsets at and far past STRS's end), a token-less row whose skip is
  past w, the MOVS head's order, a travel move whose only start spot is
  in an orientation it doesn't play (on the test dancer's pack), and the
  accepted token sequences and reserved bytes; step 2's test_dance_pack
  takes the same list, through 10.3's bounds-asserting accessor;
- every worked example of Appendix B;
- determinism: two builds equal; the same dancer from another path, with
  CRLF line endings, or with its PNG re-encoded in another colour type,
  gives the same bytes;
- `--strict` (a skip, a licence), the exit codes 0, 1 and 2 through
  `main(argv)`, and `--check` passing on the repository (which also
  checks TEXTFOLD and FONT_COVERAGE);
- the test dancer: compiled under `--strict`, its moves, lights and pose
  keys as in C.10.

**CI** (`.github/workflows/firmware.yml`), three steps right after
`actions/setup-python`, before PlatformIO is installed:

```yaml
      - name: Crab art up to date
        run: python tools/crab_art.py --check
      - name: Dance packs up to date
        run: python tools/dancepack.py --check
      - name: Dance pack compiler tests
        run: python -m unittest discover -s tools -p "test_dancepack.py"
```

Step 1 is accepted when the suite passes on Python 3.10 (the laptop's
3.10.7) and 3.12 (CI), and `--check` passes in CI on the files generated
on Windows.

## Appendix D. The C++ reader and blitter (step 2)

Appendix D is step 2's contract, as Appendices A-C are step 1's. Three
parties build step 2 from it in parallel and meet only through it:

- the **C++ implementer**: lib/core, platformio.ini and the CI step;
- the **Python agent**: dancepack.py's additions and the vectors they
  generate;
- a **test writer**, who writes test_dance_pack and test_dance_blit from
  Appendices A-D alone, without seeing the C++.

Where D and the sections above differ, D wins. It adds to C.1, C.8 and
C.9 (D.3) and changes nothing else in A-C.

### D.1 Files and the public API

| File | Written by | What |
|---|---|---|
| `lib/core/DanceRules.h` | C++ | the format's limits, the record bits and B.7's `reach` |
| `lib/core/Crc32.h`, `.cpp` | C++ | zlib's CRC-32 (A.2) |
| `lib/core/Fnv1a.h` | C++ | FNV-1a 32 (B.10), header-only |
| `lib/core/GenreFold.h`, `.cpp` | C++ | B.8's fold and genre key |
| `lib/core/DancePack.h`, `.cpp` | C++ | the reader: `Pack::open`, `Pack::scan`, the reasons, the accessors |
| `lib/core/DanceBlit.h`, `.cpp` | C++ | the row and grid decoders, and 6.2's RGB565 blitter |
| `platformio.ini`, `.github/workflows/firmware.yml` | C++ | the `native_asan` env and its CI step (D.5) |
| `tools/dancepack.py`, `tools/test_dancepack.py` | Python | `read_pack`'s options, `scan_pack`, `reader_digest` and the vector generator (D.3) |
| `test/support/dance/ReaderVectors.h` | Python (generated) | the differential reader vectors (D.3) |
| `test/support/dance/PackEdit.h` | tests | test-only helpers (D.4) |
| `test/test_dance_pack/test_dance_pack.cpp` | tests | D.4 |
| `test/test_dance_blit/test_dance_blit.cpp` | tests | D.4 |

**Rules for every new lib/core file:**

- Portable C++17 (gnu++17), with no Arduino or M5 header. The SPDX
  `GPL-3.0-or-later` and `Copyright (C) 2026 IrosTheBeggar` lines, then a
  header comment that explains the file, as CrabPose.h and LibraryIndex.h
  do. `namespace dance`.
- No heap, no std containers or strings, no exceptions, no floats, no
  mutable statics and no `IRAM_ATTR`. Tables are `constexpr` (rodata, in
  flash). Objects stay small: a `Pack` may sit in internal RAM.
- Every multi-byte field is read bytewise, little-endian, never through a
  cast pointer: a pack can start at any address, and UBSan checks
  alignment.
- Integers follow B.0, and every bound is checked in the order A.1, A.3,
  A.9, A.11 and 5.5 give, so no 32-bit sum wraps. Sums of step durations
  are `int32_t` or wider.
- Nothing in src/ changes, and nothing calls the new code yet.
  `lib_archive = yes` keeps it out of the image (IRAM and DRAM
  unchanged), but every file still compiles for the ESP32 (`pio run -e
  core2`) and into every native test program.

**Names:**

- DancePose.h already declares `dance::Box`, `dance::Step`, `dance::Pose`
  and `dance::Point`, and it stays byte for byte (1.4). So the pack's
  types are `PackGeom`, `FrameBox`, `PackFrame`, `StepRecord` and
  `MoveRecord`, and no new name in `dance` may be one of those four.
- The record bits keep section 1.1's names and are declared once, in
  DanceRules.h. Step 3's DanceCharacter.h includes it instead of
  declaring them again.
- `ogg::crc32` (OggPage.h) is Ogg's CRC, another polynomial, and stays.
  So do the file-local FNV helpers of LibraryIndex, TouchCalibration and
  QueueSaver: step 2 doesn't refactor them.

#### DanceRules.h

```cpp
namespace dance {
// MPDN v1 (Appendix A)
constexpr uint16_t kReaderVersion = 1;  // A.2: minReader <= this
constexpr uint32_t kMaxPackBytes = 131072;
constexpr int kMaxChunks = 16, kMaxFrames = 256, kMaxMoves = 64, kMaxSteps = 32, kMaxGenreTags = 8;
constexpr int kPalsBytes = 96, kThumbBytes = 800, kThumbSide = 40;
// geometry (A.6), in screen px
constexpr int kMaxScale = 4, kMaxCellPx = 160, kMaxAnchorPx = 153, kMaxBelowGroundPx = 10;
constexpr int kMaxShadowPx = 160, kMaxOffsetPx = 48;
// strings (A.5, A.11), in UTF-8 bytes
constexpr int kMaxIdBytes = 23, kMaxNameBytes = 24, kMaxAuthorBytes = 40, kMaxLicenceBytes = 40;
constexpr int kMaxUrlBytes = 80, kMaxVersionBytes = 16, kMaxKeyBytes = 23, kMaxTagBytes = 63;
// moves (section 2, A.12, A.13)
constexpr int kTicksPerBeat = 12, kMinStepTicks = 3;
constexpr int kMinIdleStep = 20, kMaxIdleStep = 500;     // 10 ms units
constexpr int kMinIdleTotal = 50, kMaxIdleTotal = 1200;  // 10 ms units
constexpr int kMinPace = 36, kMaxPace = 200;
constexpr int kMaxWeight10 = 100, kMaxRepeat = 8, kMaxRepeatBeats = 16;
constexpr int kMaxBounce = 16, kMaxSway = 16, kMaxTravel = 2, kEaseCount = 5, kStartAtCount = 5;
enum : uint8_t { kMirror = 1, kFreeze = 2, kPlaceStart = 4, kPlaceEnd = 8, kHeavy = 16, kSnap = 32 };
enum : uint8_t { kWhenQuiet = 1, kWhenListening = 2 };  // 3 = any
enum : uint8_t { kNeedsSongBpm = 1 };
constexpr uint16_t kFrameMirrored = 0x8000;  // STEP's frame word and GEOM's rest frame: bit 15
constexpr uint8_t kStepGo = 1;               // STEP's flags: bit 0

// B.7's reach: the paces a move can be danced at, [*rlo, *rhi]; false for
// none. `travels` is travel != 0 whatever its value, as dancepack.py reads it.
inline bool reach(int lo, int hi, int beats, bool travels, int* rlo, int* rhi);
}  // namespace dance
```

#### Crc32.h, Fnv1a.h and GenreFold.h

```cpp
namespace dance {
// zlib's CRC-32 (A.2) of n bytes. `prev` continues an earlier result, as
// zlib.crc32's second argument does: crc32(b, nb, crc32(a, na)) is the CRC
// of a then b, and 0 starts. "123456789" gives 0xCBF43926. A 1 KB table
// made at compile time.
uint32_t crc32(const uint8_t* p, size_t n, uint32_t prev = 0);

// FNV-1a 32 (B.10) of n bytes, continued from h: fnv1a32("a", 1) is
// 0xE40C292C, and n = 0 gives h.
constexpr uint32_t kFnvBasis = 0x811C9DC5u;
inline uint32_t fnv1a32(const void* data, size_t n, uint32_t h = kFnvBasis);

// B.8's folded key of a genre tag (UTF-8, NUL-terminated), written into
// `out` NUL-terminated and cut to cap - 1 bytes. Returns the whole key's
// length: 0 when the tag folds to nothing.
size_t foldGenre(const char* tag, char* out, size_t cap);
// B.8's genre key: FNV-1a 32 of the folded key, computed as a stream (no
// buffer, so no limit on the tag's length). False, *key untouched, when the
// tag folds to nothing.
bool genreKey(const char* tag, uint32_t* key);
}  // namespace dance
```

- GenreFold's step 1 decodes with `textfold::decode` and takes
  `textfold::replacement(cp, textfold::Mode::Full)` for each code point
  from 0x80 up; ASCII stays. A pack's tags are valid UTF-8 (A.1). A
  malformed byte elsewhere (a track's genre, later) folds as textfold
  decodes it.
- B.8's step 5 needs one word of look-ahead: a word that is exactly `n`
  waits for the next word (then it is `and`, when a word came before it)
  or for the tag's end (then it stays `n`).
- It reproduces test/support/dance/FoldVectors.h, key and hash.

#### The reasons (DancePack.h)

`Why` names every reason of A.15 in A.15's order, plus `HeaderUnreadable`
(scan only, 5.5) and step 3's two rows. A `Reason` is a `Why` and its
arguments, and `formatReason` writes A.15's exact text:

- numbers in plain decimal, unsigned, except `Travel`'s and `Offset`'s,
  which are signed;
- a tag's bytes 0x20-0x7E as themselves, any other as `?`;
- `Needs` as two uppercase hex digits.

| `Why` | Text | Arguments |
|---|---|---|
| `None` | (empty) | |
| `ShortRead` | `short read (<a> of <b> bytes)` | a avail, b size |
| `TooShort` | `too short` | |
| `NotMpdn` | `not an MPDN pack` | |
| `NeedsNewerPlayer` | `needs a newer player` | |
| `SizeMismatch` | `size <a> isn't the file's <b>` | a the header's total, b the file's size |
| `TooBig` | `too big (<a> bytes)` | a the total |
| `BadChecksum` | `bad checksum` | |
| `BadChunkCount` | `bad chunk count <a>` | |
| `HeaderUnreadable` | `header unreadable` | scan only |
| `ChunkMisaligned` | `chunk <tag>: misaligned` | |
| `ChunkOutOfBounds` | `chunk <tag>: out of bounds` | |
| `ChunkTwice` | `chunk <tag> twice` | |
| `ChunksOverlap` | `chunks <tag> and <tag2> overlap` | tag the earlier entry's, tag2 the later's |
| `NoChunk` | `no <tag> chunk` | |
| `MetaBad` | `META: bad <field>` | a: 0 id, 1 name, 2 author, 3 licence, 4 url, 5 version, 6 count |
| `GeomTooShort` | `GEOM: too short` | |
| `GeomBad` | `GEOM: bad <field>` | a: 0 scale, 1 faces, 2 cellW, 3 cellH, 4 anchorX, 5 anchorY, 6 shadowW, 7 frame count, 8 rest frame |
| `PalsSize` | `PALS: <a> bytes, not 96` | |
| `ThmbSize` | `THMB: <a> bytes, not 800` | |
| `FtabSize` | `FTAB: <a> bytes for <b> frames` | |
| `FrameBadBox` | `frame <a>: bad box` | a from 0 |
| `FrameDataOutOfBounds` | `frame <a>: data out of bounds` | |
| `FrameBadData` | `frame <a>: bad data` | |
| `RestFrameEmpty` | `the rest frame is empty` | |
| `StrsTooShort` | `STRS: too short` | |
| `StrsBadPoseKey` | `STRS: bad pose key <a>` | a from 1 |
| `MovsTooShort` | `MOVS: too short` | both of A.12's rows |
| `MovsCount` | `MOVS: <a> moves` | |
| `MovsRecordLength` | `MOVS: record length <a>` | |
| `StepSize` | `STEP: <a> bytes` | |
| `BadName` | `bad name` | |
| `BadKind` | `bad kind` | |
| `BadSteps` | `bad steps` | |
| `FrameOutOfRange` | `frame <a> out of range` | a the step's frame (bits 0-14) |
| `Beats` | `<a> beats` | |
| `StepTicks` | `step <a>: <b> ticks` | a from 1 |
| `TickSum` | `<a> of <b> ticks` | a the sum, b 12·beats |
| `StepMs` | `step <a>: <b> ms` | b 10·dur |
| `IdleTotal` | `<a> ms in all` | a 10·the sum |
| `BeatFields` | `beat fields on a no-BPM move` | |
| `BadWhen` | `bad when` | |
| `Pace` | `pace <a>-<b>` | |
| `RangeUnreachable` | `range <a>-<b> unreachable` | |
| `Weight` | `weight <a>` | |
| `Repeat` | `repeat <a>` | |
| `Bounce` | `bounce <a>` | |
| `Sway` | `sway <a>` | |
| `Travel` | `travel <a>` | a signed |
| `BadStartAt` | `bad start_at` | |
| `BadEase` | `bad ease` | |
| `BadPlace` | `bad place` | |
| `Offset` | `offset <a>,<b>` | a, b signed: the step's dx, dy |
| `BadPoseKey` | `bad pose key` | |
| `BadGenre` | `bad genre` | |
| `SongBpm` | `song_bpm needs track BPM data: skipped in this version` | |
| `Needs` | `needs 0x<a>` | |
| `TravelsFromNoSpot` | `travels from no spot` | step 3 (D.2) |
| `BudgetRed` | `budget red (<a> px)` | step 3 (D.2): a the peak |

The enum's numeric values matter only inside the C++: the vectors carry
texts.

#### The reader (DancePack.h)

```cpp
namespace dance {
class Pack;

enum class Why : uint8_t {
  None,
  // the pack is rejected (A.15's first table, in its order)
  ShortRead, TooShort, NotMpdn, NeedsNewerPlayer, SizeMismatch, TooBig, BadChecksum,
  BadChunkCount, HeaderUnreadable, ChunkMisaligned, ChunkOutOfBounds, ChunkTwice,
  ChunksOverlap, NoChunk, MetaBad, GeomTooShort, GeomBad, PalsSize, ThmbSize, FtabSize,
  FrameBadBox, FrameDataOutOfBounds, FrameBadData, RestFrameEmpty, StrsTooShort,
  StrsBadPoseKey, MovsTooShort, MovsCount, MovsRecordLength, StepSize,
  // a move is dropped (A.15's second table, in its order)
  BadName, BadKind, BadSteps, FrameOutOfRange, Beats, StepTicks, TickSum, StepMs, IdleTotal,
  BeatFields, BadWhen, Pace, RangeUnreachable, Weight, Repeat, Bounce, Sway, Travel,
  BadStartAt, BadEase, BadPlace, Offset, BadPoseKey, BadGenre, SongBpm, Needs,
  TravelsFromNoSpot, BudgetRed,  // step 3's (D.2)
};

struct Reason {
  Why why = Why::None;
  uint8_t tag[4] = {0, 0, 0, 0};   // a chunk's tag as stored; ChunksOverlap: the earlier entry's
  uint8_t tag2[4] = {0, 0, 0, 0};  // ChunksOverlap: the later entry's
  uint32_t a = 0, b = 0;           // the table's numbers or field; Travel, Offset: int32_t bits
};
constexpr size_t kReasonChars = 64;  // the longest text and its NUL fit
// A.15's text, NUL-terminated and cut to cap - 1 bytes (nothing written
// when cap is 0). Returns the whole text's length, as snprintf does.
size_t formatReason(const Reason& r, char* out, size_t cap);

// A.1's "valid": strict UTF-8 (no overlong form, no surrogate, nothing past
// U+10FFFF, no cut sequence), no NUL and no control character
// (U+0001-U+001F, U+007F-U+009F). n = 0 is valid.
bool validText(const char* s, size_t n);

using DropFn = void (*)(void* ctx, int move, const Reason& why);
struct Report {
  Reason rejected;          // Why::None when open() or scan() succeeded
  uint8_t dropped = 0;      // the moves open() dropped, then those dropMove() dropped
  DropFn onDrop = nullptr;  // optional: once per dropped move: open()'s in MOVS order, then each dropMove()'s
  void* ctx = nullptr;      // onDrop's
};

// D.2: a move check run inside open(). True keeps the move; false, with
// *why set, drops it. Step 3's is B.11's start spots (TravelsFromNoSpot);
// the budget (BudgetRed) runs after open() and drops through dropMove.
using LateCheck = bool (*)(void* ctx, const Pack& pack, int move, Reason* why);
enum : uint8_t { kSkipCrc = 1 };  // tests only: open() doesn't compare the CRC
struct OpenOptions {
  uint8_t flags = 0;         // kSkipCrc
  LateCheck late = nullptr;  // nullptr in step 2
  void* lateCtx = nullptr;
};

struct PackGeom {  // A.6; restFrame as stored, bit 15 included
  uint8_t scale, faces, cellW, cellH, anchorX, anchorY, shadowW;
  uint16_t frameCount, restFrame;
};
struct FrameBox { uint8_t x0, y0, w, h; };                               // A.9
struct PackFrame { FrameBox box; const uint8_t* data; uint16_t len; };  // data: its FDAT bytes
struct StepRecord {  // A.13; frame is bits 0-14
  uint16_t frame;
  bool mirrored;
  uint16_t dur;
  int8_t dx, dy;
  uint8_t ease;
  bool go;
};
struct MoveRecord {  // A.12's first 32 bytes, as stored
  uint16_t name;
  uint8_t kind, beats, paceLo, paceHi, weight10, when, flags, repeat, needs;
  int8_t travel;
  uint8_t startAt, travelEase, bounce, sway, fromKey, toKey, fromKeyM, toKeyM;
  uint16_t genreFirst;
  uint8_t genreCount, stepCount;
  uint16_t stepFirst;
  uint8_t songLo, songHi;
};

class Pack {
 public:
  enum class State : uint8_t { None, Scanned, Open };

  // 5.4: the whole file. data may be null only when avail is 0.
  bool open(const uint8_t* data, uint32_t size, uint32_t avail, Report* report = nullptr,
            const OpenOptions* options = nullptr);
  // 5.5: the boot scan of a file of `size` bytes whose first `avail` were
  // read into buf. buf may be null only when avail is 0.
  bool scan(const uint8_t* buf, uint32_t avail, uint32_t size, Report* report = nullptr);
  void clear();  // State::None, regionFaults() 0

  State state() const;
  uint32_t size() const;          // the file's size; 0 in State::None
  uint16_t regionFaults() const;  // reads outside their view in the last open() or scan()

  // State Scanned or Open
  const char* id() const;
  const char* name() const;
  const char* author() const;
  const char* licence() const;
  const char* url() const;
  const char* version() const;
  uint8_t declaredMoves() const;
  const PackGeom& geom() const;
  const uint8_t* palette() const;      // PALS's dance half: 16 RGB888 entries, 48 B
  const uint8_t* idlePalette() const;  // its idle half, 48 B
  const uint8_t* thumb() const;        // THMB, 800 B (A.8)

  // State Open
  int frameCount() const;
  FrameBox box(int frame) const;
  PackFrame frame(int frame) const;
  int poseKeyCount() const;           // P
  const char* poseKey(int id) const;  // id 1..P
  int moveCount() const;              // every MOVS record, dropped ones included
  int keptMoves() const;
  bool moveKept(int move) const;
  MoveRecord moveRecord(int move) const;
  const char* moveName(int move) const;
  StepRecord step(int move, int k) const;
  // Step 3's review: steps 0..n-1 at once, n = min(stepCount, cap), each as
  // step() reads it, with one record decode; *record (optional) the record.
  int steps(int move, StepRecord* out, int cap, MoveRecord* record = nullptr) const;
  const char* genreTag(int move, int g) const;

  // D.2: drops a kept move after open() (step 3's budget, in slices).
  bool dropMove(int move, const Reason& why, Report* report = nullptr);
};
}  // namespace dance
```

The signature extends 5.4's `open(ptr, size, avail, Report*)` with an
optional fifth argument.

**open():**

1. It clears the Pack. With avail < size it rejects the pack as `short
   read (<avail> of <size> bytes)` without reading a byte. With avail ≥
   size it reads only data[0, size): size is the file's length.
2. It runs A.15's pack rows in order, exactly as read_pack does (C.9),
   skipping the CRC row under kSkipCrc. The CRC reads [16, size)
   directly; every other byte comes through a region view (below).
3. The pack is then open (State Open). Each MOVS record, in order, goes
   through A.15's move rows down to `needs 0x<hh>`. A move that passes
   them all goes to `options->late` when there is one (D.2). The first
   failing row drops the move: `report->dropped` goes up by 1, then
   `report->onDrop(ctx, move, reason)` is called. The other moves are
   kept.
4. It returns true. A rejection leaves State None, sets
   `report->rejected` (with `dropped` 0) and returns false.

**scan()** clears the Pack, cuts avail to size, and runs 5.5's rows 1-8
in their order, the first failure naming its reason. It never reads past
avail. On success the state is Scanned: META's strings, the declared
count, the geometry, the palettes and the thumbnail work, and the other
accessors return 0 or null.

**Region views** (10.3's bounds-asserting accessor):

- Every byte open() and scan() read, except the CRC's span, comes through
  a view of one region:
  - the header [0, 32);
  - the directory [32, 32 + 12·count), once the count has passed its
    row;
  - one of the nine required chunks [off, off + size), once its entry
    has passed A.3 (and in scan, for META, GEOM, PALS and THMB, 5.5's
    row 5).
- Unknown chunks get no view: they are never read. Strings are read in
  their chunk's view (META's, STRS's), frames in FDAT's, records in
  MOVS's and STEP's.
- A view's read at an index outside it returns 0 and adds 1 to the
  Pack's fault count instead of reading. A reader that follows A.1, A.3,
  A.9, A.11 and 5.5 never makes one, so `regionFaults()` is always 0, and
  the tests assert it after every open() and scan(), rejected ones
  included.
- The views stay in the firmware: one compare per byte, at load only,
  and a mistake reads 0 instead of past the pack. Their reads take a
  32-bit index (every index the reader forms stays below 2^20) and are
  forced inline (`__attribute__((always_inline))`): out of line, the
  ESP32's -Os made every FDAT byte of the dry run a windowed call, about
  35 cycles a byte.

**The report.** `report` may be null. open() and scan() write only
`rejected` and `dropped`, and call `onDrop` with `ctx`; so does
dropMove() (below), after open()'s calls. A dropped move's
`Reason` lives only during its `onDrop` call, so the device logs
`[dance] robo: move "spin" ignored: <reason>` there, through a
`kReasonChars` stack buffer. Nothing is kept per move but its kept bit.

**The accessors:**

- Out of their state, or for an index out of range, they return 0, null,
  false or a zeroed struct (`geom()` a zeroed PackGeom). None reads
  outside the pack's bytes, whatever its arguments.
- Strings point into the caller's bytes (zero copy) and end in their NUL
  (A.1), so they live as long as those bytes. `url()` and `version()` are
  `""` when empty.
- `poseKey(id)`: ids 1..P, as A.11 counts them; null for 0 and past P.
- `moveCount()` counts every MOVS record. `moveKept(i)` is false exactly
  for the dropped ones, and `keptMoves()` counts the others.
- `moveRecord(i)`: record i as stored, for any i < moveCount(), kept or
  not.
- `moveName(i)`: the name when it passes `bad name`'s check (A.11's,
  then 1-23 B of `[a-z0-9][a-z0-9-]*`), else null; for any record.
- `step(move, k)`: step k when k < stepCount and stepFirst + stepCount
  fit STEP (always, for a kept move), else zeroed. It decodes the whole
  record each call (28 bounds-checked bytes), so a move's steps read one
  by one cost 33 record decodes.
- `steps(move, out, cap, record)` (added by step 3's review): the first
  min(stepCount, cap) steps as `step()` gives each, after one record
  decode; 0 out of state or range (the record then zeroed). B.4's readers
  (DrawnCharacter's readMoveSteps) use it: ~3.5k instead of ~14k
  instructions for a 32-step move on the ESP32 (estimate).
- `genreTag(move, g)`: the run's tag g (g < genreCount) when it and every
  tag before it pass A.11's check with 1-63 B, else null. Its key is
  `genreKey(tag)` (GenreFold.h).
- `frame(i)`: the box, the frame's FDAT bytes and their length (0 for a
  blank frame, whose data is never read).

**dropMove(move, why, report)** drops a kept move after open(): step 3's
budget, run in slices of frames (5.7, D.2). Unless the pack is open and the
move kept, it returns false and changes nothing. Otherwise the move's
kept bit clears, keptMoves() goes down by 1, and with a report `dropped`
goes up by 1 and `onDrop(ctx, move, why)` is called. Its record stays
readable through the accessors, as any dropped move's.

**Size.** A Pack is a view, not a copy: copying one is cheap and shares
the bytes. `sizeof(Pack)` ≤ 192 B and `sizeof(Report)` ≤ 48 B on every
target, held by static_asserts in DancePack.cpp.

#### DanceBlit.h

```cpp
namespace dance {
struct BlitCanvas {  // getBuffer()'s pixels on the device; any buffer on the host
  uint16_t* px;
  int16_t w, h;
  int32_t stride;    // px from one row to the next, >= w
};
constexpr int kBlitRowMax = 160;  // a box row's indices, on the stack (cellW <= 160)

// B.9's rgb565 with its two bytes swapped: what an M5GFX RGB565 sprite
// holds. lutEntry(0xFF, 0, 0) is 0x00F8, so the canvas's bytes read F8 00.
constexpr uint16_t lutEntry(uint8_t r, uint8_t g, uint8_t b);
// lut[i] = lutEntry of RGB888 entry i of `rgb` (a PALS half, 48 B); lut[0] = 0.
void makeLut(const uint8_t* rgb, uint16_t lut[16]);

// A.10, one row starting at p, into row[0, w). Returns where the next row starts.
const uint8_t* decodeRow(const uint8_t* p, const uint8_t* end, int w, uint8_t* row,
                         bool* ok = nullptr);
// The frame's cell grid: cellH rows of cellW indices, row-major, unmirrored.
bool decodeGrid(const PackFrame& f, int cellW, int cellH, uint8_t* grid);
// Draws f's box with its top-left at (left, top), in canvas px.
void blit(const BlitCanvas& c, const PackFrame& f, int left, int top, int scale, bool mirrored,
          const uint16_t lut[16]);
}  // namespace dance
```

- **decodeRow** reads `skip` and `n` at p, then n tokens, and writes w
  indices: 0 wherever the skip, a gap token or the end past the last
  token covers. It never reads at or past `end` and never writes past
  row[w − 1]. A row whose bytes run out, or whose skip or runs pass w,
  is cut there, clears `*ok` and returns end (a good row leaves `*ok` as
  it is; ok may be null). A w outside 1..kBlitRowMax writes nothing and
  clears `*ok`.
- **decodeGrid** zeroes cellW·cellH bytes, then decodes the h rows into
  rows y0.. and columns x0.. (A.10's drawing, unmirrored). It returns
  true when every row kept A.10's rules and exactly len bytes were read:
  what open()'s dry run checks. A blank frame (w = 0) gives a zero grid,
  true when its len is 0. A box that doesn't fit (x0 + w past cellW,
  y0 + h past cellH, or w above 160) gives a zero grid and false.
- **blit** draws box pixel (i, y) as the rectangle [left + i′·scale,
  left + (i′ + 1)·scale) × [top + y·scale, top + (y + 1)·scale), where
  i′ = i, or w − 1 − i when mirrored. Index 0 isn't drawn; index k writes
  lut[k] as it is (the LUT holds canvas values: lutEntry's in the
  firmware, k itself in A.16's hash test).
- It writes only pixels inside [0, c.w) × [0, c.h), whatever left and
  top: the canvas clips, as defence in depth after open()'s checks
  (5.4).
- It decodes one box row at a time with decodeRow into a stack array of
  kBlitRowMax bytes, its only buffer, reverses it in place when the frame
  is drawn mirrored, walks it forwards, and fills each run of one index
  as one clipped rectangle of run·scale × scale px. The clamps are 64-bit
  (left and top may be anything); each clamped bound lies in [0, c.w] or
  [0, c.h], so the fills run on int bounds through a pointer, with no
  64-bit arithmetic per pixel. No M5GFX call, no heap, no IRAM.
- It draws nothing when scale isn't 1-4, w is 0 or above kBlitRowMax, or
  c.px is null. It trusts f's bytes no further than decodeRow does: safe
  on any bytes, exact on the bytes open() accepted.
- The caller places the box (B.4 step 10). Drawn mirrored at the cell's
  mirrored column, left = (cellW − x0 − w)·scale from the cell's origin,
  box column i lands on cell column cellW − 1 − (x0 + i): A.10's rule.

### D.2 Step 2 and step 3: the move checks

A.15's move rows need these algorithms:

| Row | Needs | Built in |
|---|---|---|
| `travels from no spot` | B.11's start spots (extent and fit, over the boxes and the steps) | step 3 |
| `budget red (<peak> px)` | B.6's budget (B.4's frameAt, B.5's dirty rule) | step 3 |
| `range <lo>-<hi> unreachable` | B.7's reach | step 2, DanceRules.h |
| `bad genre`, for a tag folding to nothing | B.8's fold | step 2, GenreFold |
| every other row | the record, its steps and its strings | step 2, DancePack |

- **Nothing above them moves.** The two deferred rows are A.15's last,
  so a move that fails an earlier row gives the same reason in both
  steps.
- **The hook.** `OpenOptions::late` runs inside open() for each move
  that passed every other row, in MOVS order, with the Pack open: every
  accessor works, and moveKept(move) reads false until the hook returns.
  It isn't called for a move an earlier row dropped. A drop it asks for
  is reported like any other. It must be cheap: open() is one call on the
  loop task (5.7).
- **dropMove.** The budget isn't cheap (B.6: up to 24 cases and 2,908
  simulated frames a move, for an 8-beat travel move with mirror; at the
  ~3,300 instructions a frame step 3 first wrote, 40-70 ms for that one
  move on the ESP32, and ~15-20 ms after its review cut a frame to
  ~1,200-1,600 cycles; estimates). So it runs after open(), in slices of
  frames before the swap (`BudgetPass::run`, E.1), and drops a red move
  with `Pack::dropMove` (D.1), which reports it after open()'s drops.
- **Step 2's reader** passes no hook. A move that fails only those two
  rows is kept and not reported. Nothing plays packs yet, so nothing can
  tell. ReaderVectors carries both answers: step 2's drop lists, and the
  full lists that read_pack gives (D.3).
- **Step 3** adds the hook, `bool startSpots(void* ctx, const Pack& pack,
  int move, Reason* why)` in DanceBudget.h, for `travels from no spot`
  (B.11, every pack), and a per-move budget, `bool budgetRed(const Pack&
  pack, int move, Reason* why)`, which the loader runs on each kept move,
  in slices of frames (BudgetPass), for card packs only, passing a red one
  to dropMove (A.15;
  test_stick_pack pins the built-ins green). test_dance_pack then reads
  every vector twice: without either against step 2's lists, and with
  the hook and the budget pass, as a card pack, against the full lists,
  the budget's drops arriving after open()'s. (Step 2's test plays both
  from the full lists already: a stub hook, then dropMove.)
- In the fixture the two lists agree: `heavy` is kept by `heavy = yes`,
  and `walk` has its spots. The vectors' anchors and D.4's hook test make
  them differ.

### D.3 Differential reader vectors

dancepack.py reads about 3,000 mutated packs with read_pack or scan_pack
and writes what they say into `test/support/dance/ReaderVectors.h`. The
C++ reader must say the same of each: the same rejection, word for word,
the same drops in the same order, and the same digest of what it read.

**The Python API gains** (C.9 otherwise unchanged):

```python
read_pack(data, check_crc=True, late_checks=True, avail=None) -> Pack
    # avail: the bytes present (Pack::open's avail). Below len(data): DanceError("short read
    #   (<avail> of <len(data)> bytes)") before any other check, A.15's first row.
    # late_checks=False: A.15's last two move rows don't run (D.2), as step 2's C++ reader.
Scan(size, crc, id, name, author, licence, url, version, declared_moves, geom, palette,
     idle_palette, thumb)          # a namedtuple; the fields as in Pack
scan_pack(prefix, size) -> Scan    # 5.5's boot scan of a file of `size` bytes whose first
                                   # len(prefix) were read (a longer prefix is cut to size);
                                   # raises DanceError(reason), "header unreadable" included
reader_digest(info) -> int         # the digest below: of a Scan, or of a Pack over the moves
                                   # its `ignored` doesn't name
READER_H                           # "test/support/dance/ReaderVectors.h"
```

- `generated_files()`, `builtin` and `--check` cover ReaderVectors.h:
  C.1's list of generated headers gains it.
- scan_pack runs 5.5's rows exactly as 5.5 lists them, with A.15's
  reasons. It shares nothing with the C++ but this text.
- test_dancepack.py tests the additions:
  - scan_pack's rows in 5.5's order: each reason; a THMB of 799 bytes; a
    missing GEOM; a bad CRC that still scans; a THMB past avail beating
    a bad META;
  - read_pack's avail, and late_checks=False, under which
    test_card_budget_and_spots's two drops vanish;
  - reader_digest of A.17's pack, pinned;
  - ReadPackTest.test_limits: the anchors' limits on both sides (below),
    through read_pack, and test_order's pairs of adjacent rows;
  - two generations of ReaderVectors.h equal, and the coverage below.

**The header.** C.8's head (licence GPL-3.0-or-later, holder 2026
IrosTheBeggar, source tools/art/dancers/test); numbers in decimal, hashes
`0x%08Xu`, bytes `0x%02X` 16 a line; strings escaped as C.8's fold cases
are, and every `?` written `\?`, so no trigraph warning can fire.

```cpp
// ReaderVectors.h: Appendix D.3's differential reader vectors.
#pragma once
#include <cstdint>

namespace dancevec {

alignas(4) static const uint8_t kDotPack[1156] = { … };  // A.17's pack, byte for byte
static const uint32_t kDotPackSize = 1156;

constexpr uint8_t kBaseTest = 0, kBaseDot = 1;           // dance::kTestPack (TestPack.h), kDotPack
constexpr uint8_t kOpen = 0, kOpenShort = 1, kScan = 2;  // the entry
constexpr uint8_t kFixSize = 1, kFixCrc = 2;             // applied after the patches
constexpr int16_t kAccepted = -1;

static const uint8_t kReaderBytes[] = { … };
struct ReaderPatch { uint32_t off; uint16_t len; uint32_t at; };  // kReaderBytes[at, at + len) at off
static const ReaderPatch kReaderPatches[] = { … };
static const char* const kReaderReasons[] = { … };    // each reason's text once, in first-use order
struct ReaderDrop { uint8_t move; uint16_t reason; };  // reason: a kReaderReasons index
static const ReaderDrop kReaderDrops[] = { … };        // the drop lists back to back; equal lists shared
struct ReaderCase {
  uint8_t base, entry, fix, patchCount;
  uint32_t firstPatch;
  uint32_t length;                    // the mutated bytes' length: the file's size
  uint32_t avail;                     // kOpen: length; kOpenShort: less; kScan: the bytes read
  int16_t reason;                     // the rejection, a kReaderReasons index; or kAccepted
  uint16_t dropCount, fullDropCount;
  uint32_t firstDrop, firstFullDrop;  // step 2's drops; every A.15 row's (step 3)
  uint32_t digest;                    // accepted: the digest below; 0 when rejected
};
static const ReaderCase kReaderCases[] = { … };
static const int kReaderBytesCount = …, kReaderPatchesCount = …, kReaderReasonsCount = …,
                 kReaderDropsCount = …, kReaderCasesCount = …;

}  // namespace dancevec
```

**A case's bytes**, which the test builds as dancepack.py built them:

1. The base's bytes, cut or zero-extended to `length`.
2. The case's patches, in order. Each lies inside [0, length).
3. With kFixSize and length ≥ 12: length, as a u32, at byte 8. Then,
   with kFixCrc and length ≥ 16: the CRC of bytes [16, length), at byte
   12.
4. The entry:
   - kOpen: `open(copy, length, length)`, the copy holding all `length`
     bytes;
   - kOpenShort: `open(copy, length, avail)`, the copy holding only the
     first `avail`;
   - kScan: `scan(copy, avail, length)`, the copy holding the first
     `avail`.

   Every copy is an exact-size heap block.

The generator may make a case's bytes any way (patching fields,
reassembling chunks, growing the file) and stores them as the runs of
bytes that differ from step 1's. **What it records** is what Python says
of those bytes, b:

- kOpen: `read_pack(b, late_checks=False)` (the reason, or the step-2
  drops and the digest) and `read_pack(b)` (the full drops);
- kOpenShort: `read_pack(b, avail=avail)`;
- kScan: `scan_pack(b[:avail], length)`.

A rejected case has no drops. The two read_pack calls always agree on a
rejection, and the generator asserts it.

**The digest** is FNV-1a 32 over these bytes, in this order, with numbers
little-endian. A kScan case stops after THMB.

| Part | Bytes |
|---|---|
| META | for id, name, author, licence, url and version: u8 n, then its n bytes; then the declared move count (u8) |
| GEOM | scale, faces, cellW, cellH, anchorX, anchorY, shadowW (u8 each); the frame count and the rest frame as stored (u16 each) |
| PALS, THMB | their 96 bytes, then their 800 |
| frames | per frame in order: x0, y0, w, h (u8 each), then its A.16 hash (u32) |
| pose keys | P (u8); per key 1..P: u8 n, then its n bytes |
| moves | the MOVS count (u8); then per move that the case's step-2 list doesn't name, in MOVS order: its index (u8); its name (u8 n, n bytes); kind, beats, paceLo, paceHi, weight10, when, flags, repeat, needs, travel (its byte), startAt, travelEase, bounce, sway (u8 each); fromKey, toKey, fromKeyM and toKeyM, each as its pose key (u8 n, n bytes; n = 0 for id 0); genreCount (u8), then each tag's genre key (u32); songLo, songHi (u8 each); stepCount (u8), then per step its frame word as stored (u16, bit 15 the mirror), dur (u16), dx, dy (their bytes), ease (u8) and go (u8, 0 or 1) |

- A move outside step 2's list passed every row before `travels from no
  spot`, so its name, keys and tags are good and the digest is defined.
- The C++ tests digest the moves that the case's step-2 list doesn't
  name, not the ones `moveKept()` reports, so the same digest holds in
  step 3, with the hook.

**The cases**, in this order (the counts are approximate):

1. **Anchors** (about 270): at least one case per reason, from a table in
   dancepack.py taken mostly from test_dancepack.py's ReadPackTest
   lists; the generator checks each gives its reason. They include each
   base unmodified (kOpen, and kScan with avail = min(2,048, length)), a
   `short read`, a `header unreadable`, and a move each for `travels from
   no spot` and `budget red` in a full list. They also hold each limit on
   both sides of it with well-formed values, since the structure-aware
   edits below patch a length byte in place (which moves the string's
   NUL) and so never make a good string one byte too long: each META
   string at its limit and one byte past it; pose keys and move names of
   23 and 24 bytes, and with a leading `-`; genre tags of 63 and 64 bytes;
   64 moves, 32 steps, 8 tags, and one past each; three records of 40
   bytes; a tag past STRS offset 255 and a frame past FDAT offset 65,535;
   a frame 160 px wide; anchorY·scale of 153 and 154, and 10 px below the
   ground; idle steps of 190 and 200 ms and totals of 490 and 500; each
   beat field alone on a no-BPM move; and pairs of adjacent A.15 rows
   that both fail, where the first one's reason must show.
2. **Truncations** (about 340): per base, every length 0-40 and 136-144,
   each chunk's off − 1, off, off + 1, end − 1, end and end + 1, and 64
   seeded lengths; kFixSize | kFixCrc.
3. **Byte flips** (about 800, both bases): a seeded offset XORed with
   1-255; the size fixed unless the flip is in bytes 8-11, and the CRC
   unless it is in 12-15.
4. **Structure-aware edits** (about 1,200, mostly on the dot pack, where
   read_pack is fast): one to three edits a case. Each sets a field from
   a pool of edge values: 0, 1, the field's limit and one past it, 255
   or 0xFFFF, the stored value's neighbours, another chunk's offset or
   size, 0xFFFFFFF0. The fields:
   - the header's minReader, format and flags, and the chunk count;
   - directory entries: tags (another required tag, an unknown one, a
     duplicate), offsets and sizes;
   - META's length bytes and string bytes, including the UTF-8 edges: a
     lone continuation byte, an overlong form (C0 80, C1 BF, E0 80 80,
     F0 80 80 80), a surrogate (ED A0 80), past U+10FFFF (F4 90 80 80,
     F5), a sequence cut by the string's end, a good second byte then a
     bad third or fourth (E2 82 41, F0 9F 98 41), a C1 control (C2 80)
     and DEL; and valid 2-, 3- and 4-byte forms;
   - GEOM's fields, and PALS's and THMB's sizes;
   - FTAB's boxes, offsets and lengths; FDAT's row heads and token bytes;
   - STRS's P, its length bytes and string bytes (as META's);
   - MOVS's count and record length, and every record field, the name,
     stepFirst, stepCount, genreFirst, genreCount and the keys most;
   - STEP's frame words, durations, keys, eases and flags.

   Both fixes, except about 3 % with the CRC left stale (`bad checksum`,
   or an earlier row).
5. **Short reads** (about 40): kOpenShort, avail seeded below length.
6. **Scans** (about 400): kScan of each base and of header-side edits
   (the header, the directory, META, GEOM, PALS and THMB), with avail
   from 0, 31, 32, the directory's end and one less, each of META's,
   GEOM's, PALS's and THMB's ends and one less, 1,284, 2,048 and length.

**Coverage, determinism and cost:**

- The generator fails, with an internal error that makes `builtin` and
  `--check` exit 1, unless every `Why` but `None` is some case's
  rejection or step-2 drop (`TravelsFromNoSpot` and `BudgetRed`: a full
  drop), `MetaBad` and `GeomBad` occur with each of their fields,
  `MetaBad` also by length for each string (a well-formed string longer
  than A.5's limit), and at least 500 kOpen and 100 kScan cases are
  accepted.
- Its randomness is xorshift32 (x ^= x << 13; x ^= x >> 17; x ^= x << 5,
  all mod 2^32), seeded 0x4D50444E, with below(n) = next() mod n. It
  uses no `random` module and puts no set or dict order into the output,
  so the file is the same on Windows and in CI.
- It adds at most 10 s to `builtin` and `--check` on the laptop.
  read_pack may memoize B.6's budget and B.11's start spots by their
  inputs (the geometry, the boxes, the move), which changes no result.
  (Note, step 2: it also skips B.6's simulation when a bound from the
  boxes and keys, 2.6 × the largest box-and-shadow rect, keeps the peak
  at 20,000 px or less: dancepack.py's `_peak_bound`, tested against the
  budget. The C++ needn't copy it.)
- The header stays under 400 KB.

### D.4 The host tests

Both suites follow the house form: one `.cpp` per `test/test_<name>/`
folder, `setUp` and `tearDown`, and `int main` with `UNITY_BEGIN`,
`RUN_TEST` and `UNITY_END`, as test_crab_pose. They include what they
need of TestPack.h, FoldVectors.h, ReaderVectors.h and PackEdit.h as
`../support/dance/…`.

**PackEdit.h** (test/support/dance/, written by the test writer; it is
test-only, so std::vector is fine):

- `HeapCopy`: an exact-size `new uint8_t[n]` copy, freed by its
  destructor, so ASan reports any read past the bytes given (and LSan
  any leak). Every pack and scan buffer either suite hands the reader is
  one.
- Little-endian writes of 1, 2 or 4 bytes; `fixSize` and `fixCrc`
  (`dance::crc32`); and `patch`, which writes and then fixes the CRC, as
  test_dancepack.py's does.
- `assemble(chunks)`, test_dancepack.py's assemble_mpdn; `dotPack()`,
  A.17 built from its chunk table; and `dotWith(...)`, its a17_with:
  chunks replaced in A.4's order, extra tags after them, sorted by tag.
- `Xorshift32`, D.3's generator.
- A collector for `Report::onDrop`: up to 64 (move, text) pairs.
- `readerCase(i)`: D.3's steps 1-3.

**test_dance_pack:**

1. **The pieces:**
   - crc32 of "123456789" (0xCBF43926), its continuation, A.17's
     0x5BF1E285 and kTestPack's stored CRC; fnv1a32 of "" and "a", and
     its continuation;
   - FoldVectors in full, through foldGenre (the key) and genreKey (the
     hash; a tag folding to nothing gives 0 and false), plus a key cut
     by a small cap and a 1,000-byte tag;
   - validText on D.3's UTF-8 edges and on control characters; B.7's
     reach table;
   - formatReason for every `Why` with sample arguments: a tag with bytes
     outside 0x20-0x7E, `needs 0xA0`, `travel -3`, `offset 0,-49`, and
     `short read (4294967295 of 4294967295 bytes)`; cut at caps 0, 1
     and 5;
   - D.1's size bounds.
2. **The fixture.** kTestPack opens with no fault:
   - id `test`, name `Tëst`, the author, licence, url and version that
     tools/art/dancers/test/dancer.ini gives, 14 declared moves;
   - GEOM: scale 4, faces 1 (right), 24×32, anchor 12,30, shadow 35, 25
     frames, rest 0; the palettes; the pose keys stand, left, right and
     low;
   - the 14 moves by name, in C.10's order, with the fields C.10's table
     fixes. For example `point`: 4 beats, 90-130, weight10 3, repeat 2,
     mirror and placeStart, keys 1 → 2 and mirrored 1 → 3; `walk`:
     travel 1, travelEase 1, startAt 2, `go` on steps 2-3; `nod`'s three
     genre keys are FoldVectors' for `hip hop`, `R&B` and `drum n bass`;
     `future`: needs 1, song 160-180;
   - one drop: move 8, `song_bpm needs track BPM data: skipped in this
     version`;
   - every frame's decodeGrid hash equals kTestPackFrameHash.

   dotPack() equals kDotPack byte for byte, opens, and its frame hashes
   to 0x18857B21.
3. **Hand-built packs**: test_dancepack.py's ReadPackTest lists,
   transcribed, each with its exact reason:
   - test_rejections and test_order (with pairs of adjacent A.15 rows
     that both fail, the first one's reason showing);
   - test_accepted, with kSkipCrc on a zeroed CRC, and the unknown and
     zero-size chunks;
   - test_accepted_token_sequences, with their grids;
   - test_drops and test_drops_rebuilt;
   - test_limits: D.3's limit anchors (each limit on both sides of it,
     with well-formed values; each count at its cap and one past it), and
     what the accessors read at the caps: 64 moves kept with moveKept(63)
     true and moveKept(64) false, step(0, 31), genreTag(0, 7), a 63-byte
     tag, three 40-byte records' fields, names and steps, a tag at STRS
     offset 260 and a frame at FDAT offset 65,540;
   - test_strings_read_inside_strs, with STRS at the file's end.

   The needs bit: `future`, and `needs 0x02` and `needs 0xA0` on the dot.
4. **The hook.** On the fixture, a stub LateCheck drops move 2 as
   `travels from no spot` and move 9 as `budget red (12345 px)`:
   - it is called once for each move but 8 (already dropped), in MOVS
     order, with the Pack open, the move's record and steps readable and
     moveKept(move) false;
   - the drops arrive as 2, 8 and 9 with their texts; dropped is 3 and
     keptMoves 11.

   Without the hook, test_dancepack.py's two step-3 cases keep their
   moves. dropMove (D.2) on the fixture: false for a dropped move, a
   move out of range and a pack not open; for kept moves 9 and 10 their
   drops arrive after open()'s, with dropped and keptMoves following; and
   it clears the kept mask's top bit, move 63 of 64.
5. **Short reads.** open() with avail < size, from a copy of only
   `avail` bytes (0 included, with a null pointer): `short read (<avail>
   of <size> bytes)`.
6. **Scan mode**, 5.5's rows in their order:
   - avail < 32 → `too short`, then the header's rows;
   - the directory past avail → `header unreadable`;
   - each of META, GEOM, PALS and THMB past avail (its off past avail,
     or only its end) → `header unreadable`, before a bad META (row 5
     comes before row 6);
   - `THMB: 799 bytes, not 800`, and `no GEOM chunk`;
   - a bad CRC, a bad FTAB or a bad MOVS still scans;
   - the fixture's first 2,048 bytes give the strings, geometry,
     palettes and thumbnail that open() gives;
   - an avail above size is cut to size.
7. **Truncations**: every length 0..size of both bases, the size and the
   CRC fixed where those fields fit. A pack is accepted exactly when its
   length reaches the last chunk's end (the largest off + size in the
   directory).
8. **Flips**: 10,000 single-byte flips, xorshift32 seeded 0x464C4950,
   alternating the bases: offset 16 + below(size − 16), XOR 1 +
   below(255), the CRC recomputed.
9. **The structure-aware mutator**: 10,000 cases, seeded 0x4D555441,
   each with one to three of D.3's edits in place (the length
   unchanged), opened with kSkipCrc.
10. **ReaderVectors in full**: every case, compared as D.3 says: the
    reason's text; the drop list through onDrop, in order, with dropped
    and moveKept agreeing; the digest. Each opened case again against
    the full lists (D.2): a stub hook drops `travels from no spot` inside
    open(), then dropMove drops `budget red` after it, those drops
    arriving last. Step 3 replaces the stubs with DanceBudget's.
11. **After every open() and scan()** of items 7-10: no crash and no
    sanitizer report, and regionFaults() 0.
    - A rejection formats a non-empty text shorter than kReasonChars and
      leaves State None, with every accessor null or 0.
    - An acceptance gives onDrop's calls = dropped = moveCount −
      keptMoves; decodeGrid true for every frame; for a seeded quarter
      of the frames, one blit at a seeded scale, orientation and
      position into a canvas with a canary border, the canary intact;
      poseKey(1..P) non-null, and poseKey(0) and poseKey(P + 1) null;
      every kept move with a name, step frames below the frame count,
      and tags whose genreKey is true; a dropped move's moveName null
      exactly when its drop is `bad name` (otherwise 1-23 bytes); and
      every accessor called with out-of-range arguments too.
12. **Under native_asan**: `#if defined(DANCE_ASAN) &&
    !defined(__SANITIZE_ADDRESS__)` then `#error` at the file's top, so
    a flag that didn't reach the compiler can't pass silently (D.5).
13. **Added in integration**: both bases and every fourth vector opened
    and scanned 1-3 bytes into a block (a pack at any address: a field
    read through a cast pointer trips UBSan); one Pack and Report reused
    across opens, rejections and scans, with and without a report, and
    no hook call for a rejected pack; and malformed bytes in a tag
    folding as textfold decodes them (U+FFFD, then `?`).

**test_dance_blit:**

1. **Hashes** (A.16): every frame of kTestPack, and the dot's, drawn at
   scale 1, unmirrored, with left = x0 and top = y0, into a cleared
   cellW×cellH canvas with lut[i] = i. FNV-1a 32 of the low bytes equals
   kTestPackFrameHash[i] (0x18857B21 for the dot), and so does FNV-1a 32
   of decodeGrid's grid.
2. **The reference.** The checks below compare the canvas with a
   reference computed pixel by pixel from decodeGrid's grid and D.1's
   placement (box column i at i′ = i or w − 1 − i, each pixel
   scale×scale).
3. **Mirroring**: drawn mirrored at left = cellW − x0 − w, the canvas is
   the grid flipped, grid[y][cellW − 1 − x] (A.10).
4. **Scales 1-4**, both orientations, every frame: equal to the
   reference, with nothing written outside the box's w·scale × h·scale
   rect.
5. **Clipping with a canary border**: a canvas of about 40×50 px inside
   a buffer with a border of at least 4 px on every side, filled with
   0xA5A5 (the stride is the width plus both borders). Every frame,
   scale and orientation is drawn at lefts and tops from fully off one
   side to fully off the other: −w·scale − 1, −w·scale + 1, about half
   off, −1, 0, 1, the far edge and one past it. The border stays intact,
   and the canvas equals the reference's window. The buffer is an
   exact-size heap block.
6. **The byte swap**: lutEntry(0xFF, 0, 0) is 0x00F8 and the canvas's
   bytes read F8 00; lutEntry(0x08, 0x0C, 0x10) is 0x6208 (the
   background, 0x0862); makeLut of kTestPack's palette gives lutEntry per
   entry, and lut[0] = 0.
7. **Index 0**: over a canvas filled with a pattern, only the non-zero
   pixels change.
8. **Nothing drawn** for scale 0 and 5, a blank frame, a box wider than
   160, a null px, a canvas of width or height 0, and a stride below the
   width (in an exact-size block, and before canary pixels). A stride
   above the width leaves the columns past it untouched.
9. **decodeRow** on test_dancepack.py's accepted token sequences (a
   leading gap, a trailing gap, a run split in two, skip = w with no
   tokens) and on broken rows (skip > w, runs past w, n past the end).
   It never reads past an exact-size buffer or writes past row[w − 1] (a
   canary after the row), and clears ok for the broken rows only. Every
   one of the w indices is written, a broken row's too (its indices up
   to the cut, then 0), and with no bytes at all; and a row of
   kBlitRowMax (160) px is filled exactly, one more run breaking it.
   **The widest frame**: D.3's 160-px anchor pack decodes and draws to
   dancepack.py's frame hash, and a 160×2 frame of ten indices, drawn
   both ways round at scales 1 and 2 and clipped, equals the reference.
10. **Garbage**: hand-built PackFrames over seeded random bytes, in
    exact-size copies, 160 px wide among the widths. blit writes only
    inside the box's rect and the canvas.

Each suite should run in under 30 s under ASan.

### D.5 native_asan and CI

**The env**, in platformio.ini after `[env:native]`:

```ini
; ---------------------------------------------------------------------------
; The dance pack reader and blitter under AddressSanitizer and UBSan
; (docs/DANCE-SPRITES.md, Appendix D.5). Linux g++, CI only: MinGW has no
; ASan, so on Windows run D.5's Docker command instead. Never a default env.
; pio test -e native_asan -f test_dance_pack -f test_dance_blit
; ---------------------------------------------------------------------------
[env:native_asan]
extends = env:native
test_filter =
  test_dance_pack
  test_dance_blit
build_flags =
  ${env:native.build_flags}
  -g
  -fno-omit-frame-pointer
  -fsanitize=address,undefined
  -fsanitize=float-cast-overflow
  -fno-sanitize-recover=all
  -DDANCE_ASAN=1
```

(Step 3's review added `-fsanitize=float-cast-overflow`: GCC's
`-fsanitize=undefined` leaves it out, and it is the one undefined
behaviour the Choreographer's doubles could reach. GCC folds `(int64_t)
floor(x)` into an unchecked builtin at -O1, so those conversions are kept
in range by the code itself, E.2.3, rather than left to the sanitizer.)

- **Compile and link.** PlatformIO parses build_flags with SCons's
  ParseFlags, which puts any flag starting with `-fsanitize` in both
  CCFLAGS and LINKFLAGS (SCons 4.11.1, the tool-scons of PlatformIO
  6.2.0: SCons/Environment.py, beside `-fopenmp` and `-pthread`). So
  build_flags alone reach the link, and no extra script is needed.
  `-fno-sanitize-recover=all` is a compile flag, and makes every UBSan
  finding abort.
- **No silent pass.** A flag missing at the link fails the link (the
  `__asan_*` symbols are undefined). One missing at the compile would
  build an uninstrumented test, so test_dance_pack's `#error` stops it:
  native_asan defines DANCE_ASAN, and GCC defines `__SANITIZE_ADDRESS__`
  only under `-fsanitize=address`.
- **Never a default.** `default_envs` stays `core2`, and `pio test -e
  native` (CI's host tests and the laptop's) runs both suites without
  sanitizers. LeakSanitizer stays on: the tests free what they allocate.
- **Not on the laptop.** MinGW has no ASan, and the env would download
  Unity into .pio/libdeps/native_asan. This command stands in for it,
  from Git Bash, with the local rust:bookworm image's g++ 12 and no
  network:

```sh
MSYS_NO_PATHCONV=1 docker run --rm -v "$(cygpath -w "$PWD")":/src:ro rust:bookworm sh -c 'cp -r /src /w && cd /w && for t in test_dance_pack test_dance_blit; do g++ -std=gnu++17 -g -O1 -fsanitize=address,undefined -fsanitize=float-cast-overflow -fno-sanitize-recover=all -DDANCE_ASAN=1 -Ilib/core -Isrc -I.pio/libdeps/native/Unity/src test/$t/$t.cpp lib/core/*.cpp .pio/libdeps/native/Unity/src/unity.c -o /tmp/$t && /tmp/$t || exit 1; done && echo ASAN-ALL-OK'
```

(Note, step 2's integration: the laptop's WSL2 kernel has
`vm.mmap_rnd_bits` 32, and GCC 12's ASan then sometimes hangs before
main, printing `AddressSanitizer:DEADLYSIGNAL` without end (about one run
in three). That is the kernel, not the tests: rerun, or retry a run that
hangs before its first test line, as long as no ASan or UBSan report
shows.)

**The CI step**, in .github/workflows/firmware.yml right after `Host
tests`:

```yaml
      # The dance pack reader and blitter under ASan and UBSan
      # (docs/DANCE-SPRITES.md, Appendix D.5): Linux g++ only.
      - name: Host tests under ASan
        run: pio test -e native_asan -f test_dance_pack -f test_dance_blit
```

The libdeps cache's key follows platformio.ini, so the new env's Unity is
cached with the rest. Step 2 is accepted as section 11's row says.

## Appendix E. The choreographer, drawn characters and the budget (step 3)

Appendix E is step 3's contract, as D is step 2's. Two parties build step
3 from it in parallel and meet only through it:

- the **C++ implementer**: the lib/core files of E.1,
  `BeatTracker::acquisitions()`, and native_asan's suite list (E.4);
- a **test writer**, who writes test/support/dance/BeatScript.h and the
  suites of E.4 from Appendices A-E alone, without seeing the C++.

Python doesn't change. dancepack.py stays the reference for Appendix B,
and the headers steps 1 and 2 generated (TestPack.h, EligibilityVectors.h,
FoldVectors.h, BudgetVectors.h, ReaderVectors.h) are the vectors E.4 pins.
The Choreographer has no Python mirror: E.2 defines it and E.4's tests pin
it. Where E and the sections above differ, E wins. Of A-D it changes
nothing but three additions from step 3's review: D.1's `Pack::steps`,
D.5's `-fsanitize=float-cast-overflow`, and B.3's note on the sway at a
mirror flip (section 11 schedules its fix). It implements D.2's two
deferred rows.

### E.1 Files and the public API

| File | What |
|---|---|
| `lib/core/DanceStage.h` | new: the stage's geometry and rects (B.4's stage, B.5); step 5 adds to it |
| `lib/core/DanceRules.h` | gains B.7's `eligibleK` and `paceHolds` |
| `lib/core/DanceCharacter.h` | new, header-only: section 1.1's interface, made exact |
| `lib/core/DrawnCharacter.h`, `.cpp` | new: B.2-B.4 and B.11 over a Pack, and the Character view of an opened pack |
| `lib/core/DanceBudget.h`, `.cpp` | new: B.6, D.2's start-spot hook, the budget verdict and the pass in slices of frames |
| `lib/core/DanceChoreo.h`, `.cpp` | new: BeatInput, Cue and the Choreographer (E.2) |
| `lib/core/BeatTracker.h`, `.cpp` | `acquisitions()`, nothing else |
| `lib/core/DancePack.h`, `.cpp` | `Pack::steps` (step 3's review: a move's steps after one record decode, D.1), nothing else |
| `test/support/dance/BeatScript.h` | new, tests: a scripted BeatInput source and a hand-built Character (E.4) |
| `test/test_dance_choreo/`, `test/test_drawn_character/`, `test/test_dance_budget/` | new suites (E.4) |
| `test/test_dance_pack/test_dance_pack.cpp` | D.4 item 10 on the real hook and budget pass (E.4) |
| `test/test_beat_tracker/test_beat_tracker.cpp` | one test added (E.4) |
| `platformio.ini`, `.github/workflows/firmware.yml` | native_asan's suite list and, from step 3's review, `-fsanitize=float-cast-overflow`; nothing else (E.4) |

**Rules.** D.1's rules for a new lib/core file hold, with one exception:
DanceChoreo.cpp may use `float` and `double`, exactly where E.2 says
(E.3 lists them). Everything of Appendix B is integer: `int64_t` wherever
B.0 says, with floor helpers for values that can be negative. No heap, no
std containers or strings, no exceptions, no mutable statics, no
`IRAM_ATTR`, nothing allocated per frame. Nothing in src/ changes and
nothing calls the new code, so `lib_archive` keeps it out of the image:
`pio run -e core2` builds with IRAM unchanged, and flash and DRAM within a
few bytes (the tracker's new counter, inside DanceMode, and its
increment). DancePose.h/.cpp, CrabPose, CrabArt, DanceRate and
DanceSkin.h stay byte for byte (step 4 replaces DanceSkin).

**Names.** D.1's list stands: no new `dance::Box`, `Step`, `Pose` or
`Point`. The vector headers' namespace `dancevec` names `Eligibility`,
`PaceHolds`, `kNoK`, `Fold`, `BudgetCase`, `MoveBudget`, `TraceCase` and
`TraceFrame`; `dance` uses none of them, so a test can open both
namespaces.

#### DanceStage.h

```cpp
namespace dance {
// The dance stage (section 6.1): where it sits on the screen, its ground row and its
// three spots' anchor x, all in px. Stage coordinates run from the stage's top-left.
struct StageGeom {
  int16_t x, y, w, h;   // the stage on the screen
  int16_t groundY;      // the ground row; standing feet's last row is groundY - 1
  int16_t spotX[3];     // spots -1, 0, +1
};
constexpr StageGeom kStage = {0, 36, 320, 164, 153, {64, 160, 256}};       // final: B.4's stage
constexpr StageGeom kLegacyStage = {100, 44, 120, 150, 145, {60, 60, 60}};  // step 5's parity rect
inline int spotX(int spot);  // kStage.spotX[spot + 1], spot clamped to -1..1

// B.5's rect: half-open [x0, x1) x [y0, y1) in stage px, empty when x1 <= x0 or
// y1 <= y0. {0, 0, 0, 0} is how "none" is written.
struct StageRect { int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
inline bool rectEmpty(const StageRect& r);
inline int32_t rectArea(const StageRect& r);  // 0 when empty
// The bounding rect; an empty one adds nothing; two empty ones give {0, 0, 0, 0}.
inline StageRect rectUnion(const StageRect& a, const StageRect& b);
// B.5's dirty rule: the px a frame dirties, A the last frame's character rect (null:
// no last frame, which dirties area(now)) and B this frame's. U = union(A, B); when
// 10 * area(U) > 13 * (area(A) + area(B)) (in 64 bits) they stay two rects,
// area(A) + area(B); otherwise area(U). As dancepack.py's dirty_px.
inline uint32_t dirtyPx(const StageRect* last, const StageRect& now);
}  // namespace dance
```

- B.4, B.5 and B.11 use `kStage` only (its w, h, groundY and spotX).
  `kLegacyStage` is 6.1's rect for step 5's parity check; nothing in step
  3 reads it.
- Step 5 adds the hold plan, `inStage()` and DanceView's needs to this
  header, beside these declarations and without changing them. So 6.1's
  `DanceStage.h` exists from step 3, minimal.

#### DanceRules.h, B.7 added

```cpp
namespace dance {
constexpr int8_t kNoOctave = 127;  // eligibleK: not eligible; prevK: no previous ranged move
// B.7's eligible_k: the octave (-1, 0, +1) a move is danced at for dance tempo t10
// (tenths of a BPM), or kNoOctave. prevK: the k of the last ranged run, or kNoOctave
// (any value outside -1..1 counts as none). travel: nonzero means the move travels.
inline int8_t eligibleK(int t10, int lo, int hi, int8_t prevK, int beats, int travel);
// B.7's pace_holds: the running move's tolerance (2.5): its pace within 0.97*lo ..
// 1.03*hi, and k = +1 only while t10 <= 900. True for an unranged move; false for a
// ranged move with k outside -1..1.
inline bool paceHolds(int t10, int8_t k, int lo, int hi);
}  // namespace dance
```

Both are integer-exact, as dancepack.py's `eligible_k` and `pace_holds`
(B.7: x(k) = t10·2^(k+1); the order prevK, 0, −1, +1, each tried once).
EligibilityVectors.h's `kNoK` has the same value, 127.

#### DanceCharacter.h

```cpp
#include "DanceRules.h"   // the record bits: kMirror ... kSnap, kWhenQuiet, kWhenListening, kNeedsSongBpm
namespace dance {
enum class MoveKind : uint8_t { Beat, Idle };                     // A.12's kind 0, 1
enum class Origin : uint8_t { BuiltIn, Card };
enum class StartAt : uint8_t { Any, Centre, Side, Left, Right };  // A.12's startAt 0-4; mirrored: Left <-> Right
constexpr int8_t kNoMove = -1, kAutoBounce = -2, kAutoIdle = -3;  // move indices that aren't a character's

struct MoveLabels {           // what the picker reads: the same for both classes
  const char* name;
  MoveKind kind;
  uint8_t beats;              // Beat: 1, 2, 4 or 8 move beats; Idle: 0
  uint16_t idleMs;            // Idle: 500..12000, 10 x its steps' sum; 0 = continuous (code characters only); Beat: 0
  uint8_t paceLo, paceHi;     // the danced pace, 36..200; 0, 0 = any (Idle: 0, 0)
  uint8_t weight10;           // 1..100 = 0.1..10.0
  uint8_t when;               // Idle: kWhenQuiet | kWhenListening (3 = any); Beat: 0
  uint8_t flags;              // kMirror, kFreeze, kPlaceStart, kPlaceEnd, kHeavy, kSnap
  uint8_t repeat;             // 0 (auto) .. 8, repeat * beats <= 16
  uint8_t fromTag, toTag;     // pose key ids, 0 = none
  uint8_t fromTagM, toTagM;   // their mirror twins' ids (A.11, A.12), for the mirrored orientation
  uint8_t needs;              // feature bits: any bit set and the picker never uses the move
  int8_t travel;              // -2..+2 spots, negated when mirrored
  StartAt startAt;
  uint8_t genreCount;         // 0..8
  const uint32_t* genreKeys;  // genreCount B.8 keys (genreKey()), never FNV(""); null when 0
};
struct CharacterInfo {
  const char* id; const char* name; const char* author; const char* licence; const char* url;
  Origin origin; bool inCycle; uint8_t ignoredMoves;
};
// B.11's start_at as spots, bit s + 1 for spot s: Any 0b111, Centre 0b010, Side 0b101,
// Left 0b001 (0b100 mirrored), Right 0b100 (0b001 mirrored); 0 for a value above Right.
inline uint8_t startAtMask(StartAt a, bool mirrored);

class Character {  // the interface doesn't draw: each class has a renderer in src/ui
 public:
  virtual ~Character() = default;
  virtual const CharacterInfo& info() const = 0;
  virtual int moveCount() const = 0;                // 0..64
  virtual const MoveLabels& move(int i) const = 0;  // 0 <= i < moveCount()
  // The spots move i can start from in an orientation, bit s + 1 for spot s: 0b111 for an
  // idle move and for a move without travel. This default knows no art: for a travel
  // move, every s whose s + travel * sgn is a spot (sgn -1 when mirrored) and that
  // startAtMask allows. DrawnCharacter overrides it with B.11's, which adds the fit.
  virtual uint8_t startSpotMask(int i, bool mirrored) const;
};
// 2.9's freeze move: the first beat move flagged kFreeze; else the first unranged 2-beat
// beat move; else the first unranged beat move; else kAutoBounce. Moves with a needs
// bit are skipped. (Unranged: paceLo = paceHi = 0.)
inline int freezeMove(const Character& c);
// Whether any beat move without a needs bit has travel != 0.
inline bool hasTravel(const Character& c);
}  // namespace dance
```

- Section 1.1's sketch gains `fromTagM`/`toTagM` (the picker's continuity
  needs the twins: 3.4 step 3, "mirroring swaps left and right"),
  `startSpotMask` (travel eligibility needs the fit, which only the art
  knows), the engine move indices and the two helpers. The record bits
  stay in DanceRules.h (D.1).
- Step 4's procedural characters implement it as they are: the crab has
  `shuffle` (2 beats, any pace, kFreeze, from and to `stand`, its own pose
  key id 1, twins equal) and `idle` (idleMs 0, when 3); `stick-classic`
  has `bounce` and `sway` alike. Their renderers read E.2's Cue (u,
  idleSeconds, weight) and ignore what they don't need.

#### DrawnCharacter.h

```cpp
#include "DanceCharacter.h"
#include "DancePack.h"
#include "DanceStage.h"
namespace dance {
// B.2: ease code 0-4 at f (0..65536). A code above 4 acts as hold.
int32_t ease16(int ease, int32_t f);
// B.3: the hop at phase phi (0..65535) of a move beat; the sway at move beat b (>= 0,
// counted since the run started) and phase phi.
int32_t hop16(int32_t phi);
int32_t sway16(int64_t b, int32_t phi);
// B.4 step 8 (section 8's stageX): the stage x of a travel from spot `from` to spot `to`
// (each clamped to -1..1) at progress p16 (0..65536) through `ease`:
// spotX(from) + rhu((spotX(to) - spotX(from)) * ease16(ease, p16), 65536).
int stageX(int from, int to, int32_t p16, int ease);

struct Placement {     // B.4's output
  uint16_t frame;      // the shown step's frame index (bits 0-14)
  bool mirrored;       // drawn mirrored: the step's bit 15 XOR the orientation
  int16_t rootX, rootY;
  StageRect box;       // the frame's box after the clamp; {0, 0, 0, 0} for a blank frame
  StageRect shadow;    // {0, 0, 0, 0} for none
  StageRect rect;      // the character rect: union(box, shadow)
};
// B.4 for MOVS record `move` of an open pack, kept or not.
Placement frameAt(const Pack& pack, int move, int64_t pos16, bool mirrored, int spot,
                  int32_t e16 = 65536);
// B.11. extent: false (lo, hi untouched) when every box is empty.
bool extent(const Pack& pack, int move, bool mirrored, int* lo, int* hi);
bool fits(const Pack& pack, int move, bool mirrored, int spot);
uint8_t startSpotMask(const Pack& pack, int move, bool mirrored);  // bit s + 1 per start spot

class DrawnCharacter final : public Character {
 public:
  // The view of an open pack (State::Open): its kept moves at this moment, in MOVS
  // order. False, and an empty view, otherwise. The pack's bytes must outlive the view.
  bool init(const Pack& pack, Origin origin, bool inCycle);
  const CharacterInfo& info() const override;
  int moveCount() const override;
  const MoveLabels& move(int i) const override;
  uint8_t startSpotMask(int i, bool mirrored) const override;  // B.11's
  int packMove(int i) const;   // move i's MOVS index; -1 out of range
  const Pack& pack() const;
  // B.4 for move i, or for the engine moves kAutoBounce and kAutoIdle (below).
  Placement frameAt(int i, int64_t pos16, bool mirrored, int spot, int32_t e16 = 65536) const;
};
}  // namespace dance
```

**The free functions are dancepack.py's, bit for bit:** `frameAt` is
`frame_at` (its Pose's frame, mirrored, root_x, root_y, box, shadow and
rect, None written {0, 0, 0, 0}), `extent` is `extent` (false for None),
`fits` is `fits`, `startSpotMask` is `start_spots` (bit s + 1 for each
listed spot, so 0b111 for a move without travel), and `ease16`, `hop16`
and `sway16` are `ease_q16`, `hop_q16` and `sway_q16`. They read only
what B.4-B.11 use: the geometry, the boxes and the record's steps.

- **Any record.** They work on every MOVS record whose steps read (1-32
  steps inside STEP, durations summing above 0), kept or not: BudgetVectors
  carries `future` (move 8), which the reader drops. On any other record
  frameAt gives frame 0, unmirrored, root (spotX(spot), 153) and no box or
  shadow; extent gives false; startSpotMask 0b111. A step's frame index at
  or past the frame count reads as a blank box (Pack::box's zeros).
- **Where dancepack.py raises, the C++ clamps:** pos16 < 0 counts as 0;
  spot is clamped to −1..1, and so is a travel's destination spot +
  travel·sgn; a travel whose `go` steps sum to 0 (only on records A.15
  drops) counts as done (p = 65536). e16 is used as given. The
  Choreographer never asks for any of these.
- frameAt reads the record's steps once per call, with `Pack::steps`:
  one record decode and up to 32 step decodes, ~15-20 µs on the ESP32
  (estimate). (Note, step 3's implementation: the header also has
  frameAt's two halves, `MoveSteps`, `readMoveSteps` and `placeSteps`, and
  `startSpotMask(pack, MoveSteps, mirrored)`, so the budget reads a move
  once, not once a case or a frame. Its review made readMoveSteps also
  precompute B.4 step 8's sums (anyGo, the travelling steps' total and
  each step's travelling start) and placeSteps find the step by binary
  search and divide in 32 bits whenever the values fit, as they always do
  for a record A.15 keeps: no O(steps) loop and no libgcc 64-bit division
  a frame, ~1,200-1,600 cycles instead of ~3,300 instructions (estimate),
  with the same results bit for bit: 1.25M queries against dancepack.py
  and every BudgetVectors row agree.)

**DrawnCharacter:**

- `init` keeps a copy of the Pack (a view: cheap, sharing the bytes). The
  moves are the ones kept at that moment: a `dropMove` after `init`, on
  any copy, isn't seen, so the loader builds the view after the budget
  pass. Move i is MOVS record `packMove(i)`.
- **The labels** of move i, from its record: `name` = moveName; kind;
  beats (0 for an idle move); `idleMs` = 10 × its durations' sum for an
  idle move, 0 for a beat move; paceLo, paceHi, weight10, when, flags,
  repeat, needs (0 for every kept move) and travel as stored; `fromTag`,
  `toTag`, `fromTagM`, `toTagM` = fromKey, toKey, fromKeyM, toKeyM;
  `startAt` = StartAt(startAt); genreCount, and `genreKeys` pointing at
  the view's own copy of `genreKey(genreTag(m, g))` for each tag.
- **The info:** id, name, author, licence and url from the Pack (url `""`
  when empty), origin and inCycle as given, `ignoredMoves` = moveCount() −
  keptMoves() at init.
- `startSpotMask(i, o)` = `startSpotMask(pack, packMove(i), o)`, which the
  view may compute once at init. `frameAt(i, ...)` = `frameAt(pack,
  packMove(i), ...)`; an i that is neither a move nor an engine move gives
  the unreadable record's placement.
- **The engine moves** (2.6's auto-idle, 2.8's auto-bounce) are B.4 on two
  synthetic records, with r = GEOM's rest frame (bits 0-14 the frame, bit
  15 the step's mirror bit; the compiler always writes 0):
  - **auto-bounce:** a beat move of 1 beat, unranged, flags 0, bounce 4,
    sway 0, travel 0, one step {frame r, dur 12, key 0,0, ease hold, no
    `go`};
  - **auto-idle:** an idle move, flags 0, two steps {r, 200, key −1,0,
    inout} and {r, 200, key +1,0, inout}: the rest frame swaying ±1
    native px over 4 s.
- `sizeof(DrawnCharacter)` ≤ 6,144 B on every target, a static_assert in
  DrawnCharacter.cpp. Step 4 `psramNew`s it.
- (Note, step 3's integration: a copy, by the copy constructor or
  assignment, is another view of the same bytes whose labels' genreKeys
  point at its own keys, so it outlives the original.)

#### DanceBudget.h

```cpp
#include "DrawnCharacter.h"
namespace dance {
constexpr uint32_t kGreenPx = 16000, kAmberPx = 20000;  // B.6's lights
constexpr int kBudgetMaxCases = 24;    // 2 fps x 3 paces x 2 orientations x 2 start spots
constexpr int kBudgetMaxFrames = 364;  // B.6's largest N: a 12 s idle move at 30 fps
constexpr uint32_t kBudgetSliceFrames = 256;  // the loader's slice a frame (5.7)
enum class BudgetLight : uint8_t { Green, Amber, Red };  // 0, 1, 2, as kMoveBudgets' light
BudgetLight budgetLight(uint32_t peak);  // green <= 16,000 < amber <= 20,000 < red

// B.6's cases of MOVS record `move`, in B.6's order (fps, pace, orientation, spot).
struct BudgetCaseSpec { uint8_t fps; uint8_t pace; bool mirrored; int8_t spot; };  // pace 0: idle
int budgetCaseSpecs(const Pack& pack, int move, BudgetCaseSpec out[kBudgetMaxCases]);

// One case frame by frame: n = 0..N, as dancepack.py's trace().
struct BudgetFrame { uint16_t n; uint32_t pos16; Placement place; uint32_t dirty; };  // dirty 0 at n = 0
// One case over steps the caller keeps (BudgetRun's and BudgetPass's engine).
class BudgetCursor {
 public:
  void begin(const MoveSteps& m, const BudgetCaseSpec& spec);         // from frame 0
  bool next(const Pack& pack, const MoveSteps& m, BudgetFrame* out);  // false after frame N
};
class BudgetRun {  // a BudgetCursor with its own MoveSteps
 public:
  BudgetRun(const Pack& pack, int move, const BudgetCaseSpec& spec);
  bool next(BudgetFrame* out);  // false after frame N
};

struct BudgetCaseResult { uint16_t frames; uint32_t median, peak; };  // frames: N
// scratch: kBudgetMaxFrames values, for the median; null: median 0.
BudgetCaseResult budgetCase(const Pack& pack, int move, const BudgetCaseSpec& spec, uint32_t* scratch);
struct BudgetResult { uint32_t median, peak; BudgetLight light; uint8_t cases; };
BudgetResult budget(const Pack& pack, int move, uint32_t* scratch);

// D.2, A.15's last two move rows.
// OpenOptions::late: false, *why = TravelsFromNoSpot, for a beat move with travel != 0
// whose startSpotMask is 0 unmirrored and, when it has kMirror, mirrored too. ctx unused.
bool startSpots(void* ctx, const Pack& pack, int move, Reason* why);
// The verdict: true (drop it), *why = BudgetRed with a = the peak, when the move hasn't
// kHeavy and its B.6 light is red. A heavy move returns false without a simulation.
bool budgetRed(const Pack& pack, int move, Reason* why);

// 5.7's budget pass for a card pack, resumable frame by frame: it keeps the move it is
// on, its cases, the case in progress and the running peak (792 B on the ESP32: the
// loader psramNews it).
class BudgetPass {
 public:
  void begin(Pack* pack, Report* report = nullptr);  // from MOVS record 0
  // Up to maxFrames simulated frames (0 counts as 1) of the next kept move's cases; on
  // the call that ends its last case, budgetRed's verdict, a red move dropped through
  // pack->dropMove (which reports it). A heavy move takes no frames. True while it did
  // something; false, doing nothing, once no kept move is left to check.
  bool run(uint32_t maxFrames);
  bool step();  // run() to the end of one kept move
  bool done() const;
};
}  // namespace dance
```

- **As dancepack.py.** `budgetCaseSpecs` is `_case_specs` (paces from
  `reach` with travel ≠ 0: rlo, (rlo + rhi) // 2, rhi, ascending, a
  repeat dropped; idle moves the one pace 0; orientations unmirrored, then
  mirrored with kMirror; spots 0, or for a travel move each start spot of
  that orientation, ascending). `BudgetRun` is `trace()`: the frame period
  is `(1000 + fps / 2) / fps` ms (dancerate::periodMs: 42 at 24 fps, 33
  at 30), pos16(n) is B.6's (kept as a quotient and remainder, advanced
  by a 32-bit addition and division a frame at 24 and 30 fps), frames go
  through `frameAt` at e16 = 65536, and the run stops after the first n
  ≥ 1 with pos16 ≥ L·65536.
  `budgetCase` is a case's N, lower median and peak; `budget` is the
  move's largest case median and peak, its light, and its case count.
  A beat move with pace 0, or a record that doesn't read, has no frames.
- **The scratch.** A case's median needs its N values: `kBudgetMaxFrames`
  = 364 holds every move A.15 keeps (the longest is a 12,000 ms idle move:
  N = 364 at 33 ms; a beat move's largest is 203, 8 beats at 72 or 4 at
  36, at 30 fps). On a record outside A.15's limits values past 364 aren't
  stored and the median is unspecified. `budgetRed` and `BudgetPass` need
  only the peak, so they take no scratch and keep nothing per frame: the
  device's verdict costs only the pass itself (792 B, in PSRAM).
- **The cases** are at most 24: a travel move can start from at most two
  spots in an orientation (s + travel must be a spot), any other move
  from 0 only. An 8-beat travel move with mirror has all 24, 2,908
  simulated frames at the paces 72, 124 and 176.
- **The split** (D.2): the loader opens a card pack with `OpenOptions{0,
  &startSpots, nullptr}` (built-in packs too: the hook reads each move's
  steps once), then calls `BudgetPass::run(kBudgetSliceFrames)` once a
  frame (or run() in a loop under a ~2 ms time budget) until it returns
  false, then builds the DrawnCharacter. Built-in packs skip the pass (test_stick_pack
  pins them green). The drops arrive as D.2 says: open()'s in MOVS order,
  then the pass's in MOVS order.
- `run()` and `step()` skip moves that aren't kept without counting
  them: each `step()` that returns true checked exactly one kept move, and
  `run()` gives a move's verdict on the call that ends its last case, so
  the drops, their order and their reasons are `step()`'s and
  `budgetRed`'s whatever the slice. A pack that isn't open (cleared
  mid-pass too), or a null pack, is done at once. (Note, step 3's review:
  the first design ran one whole move a call, up to ~70 ms on the ESP32
  for the worst move.)

#### DanceChoreo.h

```cpp
#include "DanceCharacter.h"
#include "DanceStage.h"
namespace dance {
struct BeatInput {        // section 3.1: one a frame, from DanceMode::render()
  bool heard;             // audio is heard now
  bool grid;              // the tracker's grid applies to what is heard (meaningful with heard)
  bool locked;            // BeatTracker::locked()
  float confidence;       // BeatTracker::confidence(), 0..1
  double trackerBeats;    // the tracker's beat count at the moment shown (with grid)
  float rawBpm;           // BeatTracker::bpm(): 0 when none
  double foldScale;       // S: TempoFold::apply(rawBpm), a power of two (1/2, 1 or 2)
  uint32_t restarts;      // tracker restarts so far (DanceMode's resets_)
  uint32_t acquisitions;  // BeatTracker::acquisitions()
  float dt;               // seconds since the last frame; 0 on the first
  int8_t freeze;          // the console's k<n>: -1 none, 0..15
};
enum class DanceState : uint8_t { Quiet, Listening, Entering, Dancing, Leaving, Freeze };
struct Cue {              // section 3.1, plus beatPos16
  int8_t beatMove;        // the run's move, kAutoBounce, or kNoMove (none since a reset or switch)
  double u;               // move beats since its run started, as drawn
  uint32_t beatPos16;     // B.4's pos16 for beatMove: floor(u * 786432), exact under a freeze
  int8_t k;               // the run's octave, -1..+1
  bool beatMirror;        // this play's orientation
  int8_t idleMove;        // the idle run's move, kAutoIdle, or kNoMove (no character)
  uint32_t idleMs;        // ms since the idle run started (B.4: pos16 = idleMs * 65536 // 10)
  bool idleMirror;
  float idleSeconds;      // the idle clock, 0..2000 s (code characters' continuous idles)
  float weight;           // w: the dance weight
  uint32_t energy;        // e16: 39322..65536 (3.6), for B.4's e16
  bool showDance;         // beat frames show; otherwise idle frames
  bool flash;             // the beat light's cyan
  int8_t spot;            // the current spot, -1..+1: the running move's start spot
  int16_t rootX, rootY;   // spotX(spot), kStage.groundY: the spot's anchor on the stage
  DanceState state;
};
enum class RunStatus : uint8_t { None, Running, Held };
struct RunInfo {          // the current beat run (status None: the last one's fields)
  int8_t move;            // a move, kAutoBounce, or kNoMove
  int8_t k;
  uint8_t plays;
  bool firstMirror;
  int8_t spot;            // its start spot
  int8_t phrase;          // p at its start, 0..7
  uint8_t level;          // the ladder level that picked it: 0-2, 3 auto-bounce, 4 forced (Dm)
  bool cut;               // its pace failed (2.5)
  RunStatus status;
  int64_t start, end;     // dance-clock beats: where it started, where it ends (E)
  double uEnd;            // move beats where its content ends (plays * beats, or a cut point)
};

class Choreographer {
 public:
  Choreographer();                     // as after reset(0), with no character
  void reset(uint32_t pathHash);       // dance on (E.2.8): everything back but the character
  void onTrack(uint32_t pathHash, const uint32_t* genreKeys, int count);  // every track change
  void setCharacter(const Character* c);                   // 3.7; null: none
  void freeze(int move, uint32_t pos16, bool mirrored);    // Df; kNoMove unfreezes
  void forceNext(int move);            // Dm: the next beat run
  void setSpot(int spot);              // Ds
  void addGenre(uint32_t key);         // Dg: a genre key for this track
  const Cue& update(const BeatInput& in);                  // once a frame
  const Cue& cue() const;              // the last update's
  const Character* character() const;  // the current one, not a pending switch
  const RunInfo& run() const;
  int t10() const;                     // the latched T in tenths, 0 = none yet
  int8_t prevK() const;                // kNoOctave = none
  double danceClock() const;           // E.2's clock
  int8_t eligibleNow(int move) const;  // Dl: the k move would get now, or kNoOctave
};
}  // namespace dance
```

- `sizeof(Choreographer)` ≤ 1,024 B on every target, a static_assert in
  DanceChoreo.cpp (6.8's .bss budget; about 450 B expected). It holds
  pointers to the characters, never copies them: a character must
  outlive its use, a pending switch included.
- **What step 4 feeds it** (DanceMode::render(), names as today):

| Field | Source |
|---|---|
| heard | `heard.valid` (TapReader::Audible); host mode: `heard.valid && hostEpochOn_` |
| grid | today's `beat`: heard and `g.valid && !fresh_` and the epoch matching (host mode: `hostEpochOn_`) |
| locked, confidence, rawBpm | `tracker_.locked()`, `.confidence()`, `.bpm()` |
| trackerBeats | today's `beats`: `g.beatIndex + g.beatsAt(...)`; 0 without grid |
| foldScale | `fold_.apply(bpm)` on frames with grid, as today; `fold_.scale()` otherwise |
| restarts, acquisitions | `resets_`, `tracker_.acquisitions()` |
| dt, freeze | today's `dt`, `frozen_` |

  `onTrack`'s pathHash is `fnv1a32` of the track's path (`""` when there
  is none), and its keys are `genreKey` of the track's genres (none
  today).

#### BeatTracker::acquisitions()

```cpp
// Grids acquired since construction: +1 each time acquire() sets a new grid (the first
// after a reset, a re-acquire, one after a dropped grid), never on its early return.
// reset(), begin() and setSampleRate() leave it; it wraps at 2^32. The Choreographer
// compares it for change only (E.2).
uint32_t acquisitions() const { return acquisitions_; }
```

`++acquisitions_` goes in `BeatTracker::acquire()` beside `acquired_ =
true`, after the `return false`; the member is `uint32_t acquisitions_ =
0`. Nothing else in the tracker changes, so beat_eval's outputs don't
(E.5).

### E.2 The Choreographer's behaviour

Section 3 says what the dancer does; this is the same behaviour as code,
complete enough that two implementations agree cue for cue (E.3). The
names below are for this text; an implementation may name and pack its
state as it likes, as long as every observable (the Cue, `run()`,
`t10()`, `prevK()`, `danceClock()`, `eligibleNow()`) is what E.2 gives.

#### E.2.1 Terms

- **The dance clock**, `clock` (double), counts dance beats. On each frame
  with a grid it advances by Δ(trackerBeats)·S (U4). Its whole values are
  the dance beats; the beat light, the phrase and every run boundary use
  them. Runs start and end on whole values, held as `int64_t` (`start`,
  `end`, A, `enterAt`, `switchAt`).
- **A run's position.** For a running run, uRaw = max(0, min((clock −
  start)·2^k, uEnd)) move beats (0 or above whatever uEnd is), where 2^k
  is 2, 1 or 0.5 (exact). S changes only on a frame that re-phases (U4:
  an acquisition, a jump, or S itself changing), so between re-phases
  this is 3.5's u += Δ(trackerBeats)·M with M = S·2^k recomputed at each,
  and a run start carries the overshoot past its whole beat. uEnd is plays·beats unless a cut, a
  hold or a switch lowered it.
- **D2** = beats·2^(1−k), a play's length in half dance beats (D = D2/2):
  beats at k = +1, 2·beats at 0, 4·beats at −1.
- **mod8(x)** = x − 8·floor(x / 8), 0..7, on int64.
- **e16(w):** c = (w − 0.5f)·2.0f in float, clamped to [0, 1]; then
  e16 = 39322 + (int32) floor(26214.0·(double) c + 0.5). So w ≤ 0.5 gives
  39322, 0.65f gives 47186 (3.6's 72 %), 0.75f gives 52429, and 1 gives
  65536.
- **The RNG:** xorshift32 (x ^= x << 13; x ^= x >> 17; x ^= x << 5, mod
  2^32), `below(n)` = next() mod n, as D.3's. `seed(v)` sets the state to
  v, or to 0x4D50444E when v is 0. Section 3.4's seed is pathHash ^
  restarts ^ acquisitions.
- **Orientation** o is false (unmirrored, sgn +1) or true (mirrored, sgn
  −1). A move's key in o: from(o) = o ? fromTagM : fromTag, to(o) likewise.
- Comparisons with the weight are in float: `w > 0.5f`, `w <= 0.5f`.

#### E.2.2 State

| State | Type | Constructed / reset(pathHash) | Notes |
|---|---|---|---|
| w | float | 0 | the dance weight |
| idleSec | float | 0 | the idle clock, wrapped at 2,000 s (a multiple of the crab's 1,000 s and stick-classic's 400 s wraps) |
| clock, haveClock | double, bool | 0, false | the dance clock, and whether it is set |
| lastTB, lastS, lastTl, sinceGrid, acqSeen | double ×4, uint32 | 0 (lastS 1) | at the last frame with a grid: trackerBeats, S, rawBpm·S, seconds since, acquisitions |
| T, t10 | double, int | 0, 0 | the latch (0: none) |
| phraseValid, A | bool, int64 | false, 0 | the pseudo-phrase's origin |
| entering, enterAt | bool, int64 | false, 0 | Entering's beat |
| run | RunInfo, plus `leaving`, `prevU` and `lastU` (double), `heldEnergy`, `show` (bool) | status None, move kNoMove, show false | prevU: the last frame's uRaw; lastU: the u it drew; show: E.2.7's latch |
| spot | int8 | 0 | |
| lastMove, lastPlays, lastFirstMirror | int8, uint8, bool | kNoMove, 0, false | the last run's |
| recent[3] | int8 | kNoMove ×3 | the last three runs' moves, newest first |
| prevTo, cutExempt, prevK | uint8, int8, int8 | 0, kNoMove, kNoOctave | |
| firstRunBias | bool | false | set by a track reset |
| idle | move, mirrored, runMs (uint32), cls, ms (double), lastIdle | move kNoMove, lastIdle kNoMove | the idle run; runMs 0 never ends |
| rng | uint32 | seed(pathHash) | |
| character, pending, switchAt | pointers, int64 | kept by reset | |
| genres | 16 track keys + 4 injected | none | |
| pathHash, restartsSeen, started, lastAcqIn | | pathHash, 0, false, 0 | |
| lastShowDance, lastEnergy | bool, uint32 | false, 39322 | the last update's, before any freeze |
| Df | move, pos16, mirrored | off | the console's freeze |
| forced | int8 | kNoMove | Dm |

#### E.2.3 update(in)

Each call runs these steps in this order, then returns the cue.

**U1. dt.** dt = in.dt when it is above 0, else 0 (a NaN too); then at
most 1,000 s (+inf and huge values too). The cap keeps every state
finite, and lets one subtraction keep the idle clock in 0..2000 s (from
just under 2,000, a 1,000 s step rounds to at most 3,000). DanceMode's dt
is an int32 of µs, at most 2,147 s, and a frame gap past 1,000 s draws
the same either way.

**U2. Restarts.** When `!started` or in.restarts ≠ restartsSeen: TRACK
RESET (E.2.4), restartsSeen = in.restarts, seed(pathHash ^ in.restarts ^
in.acquisitions), started = true. lastAcqIn = in.acquisitions on every
call. (3.5's "track change with a restart": the Choreographer sees the
restart in its input, so identical inputs give identical cues.)

**U3. Weight and the idle clock.** First the inputs outside their
contract, so that none can poison the state or reach an undefined
conversion: usable = |in.trackerBeats| < 2^40 and 0 ≤ in.rawBpm ≤ 1000
(false for a NaN or an infinity); S = in.foldScale when it lies in
[1/64, 64], else 1 (0, a negative, a NaN or an infinity: "none"); c =
in.confidence clamped to [0, 1], a NaN to 0 (danceWeight's own clamps
pass a NaN through, and one NaN weight would stop the dancer for good).
Then heard = in.heard; gridOk = heard && in.grid && usable; lockedOk =
gridOk && in.locked; target = lockedOk ? danceWeight(c) : 0; `w +=
(target − w) * (1.0f − std::exp(−dt / 0.4f))`, in float, exactly as
Dancer::update and Crab::update do. idleSec += dt; when idleSec ≥ 2000,
idleSec −= 2000.

**U4. The dance clock.** sinceGrid += dt; far = false.

- With gridOk, tb = in.trackerBeats:
  - `!haveClock`: clock = tb·S, haveClock = true.
  - otherwise, when in.acquisitions = acqSeen, S = lastS and −0.1 ≤ tb −
    lastTB ≤ 1.5 (a PLL nudge passes through): clock += (tb − lastTB)·S.
  - otherwise (3.5's re-acquire, jump or fold change): REPHASE, below;
    entering = false. S can change with no acquisition: after a
    re-acquire that kept the old scale near 72 or 176, the PLL's ±4 %
    drift can carry the folded tempo across, and TempoFold refolds. A
    nudge there would add Δ·S_new to a clock counted at S_old, leaving its
    whole beats (S_old − S_new)·tb off the tracker's until the next
    acquisition.
  - When the clock comes out at 2^52 dance beats or more in size (never in
    practice: some 10^13 frames): clock = tb·S, far = true, entering =
    false. So every int64 made from the clock is in range.
  - Then lastTB = tb, lastS = S, lastTl = in.rawBpm·S, acqSeen =
    in.acquisitions, sinceGrid = 0.
- Heard without a grid (no grid, a fresh tracker, a dropped grid, or a
  grid U3 found unusable): haveClock = false, phraseValid = false,
  entering = false.
- Not heard (paused, starved, no epoch): nothing; the clock waits, so a
  resume continues it.
- When far: phraseValid = false.

REPHASE chooses the new grid's dance-beat origin among the 1/S
whole-tracker-beat shifts, the one nearest the predicted dance position:

```
predicted = clock + sinceGrid * lastTl / 60        // sinceGrid includes this frame's dt
x = tb * S
n = S < 1 ? llround(1 / S) : 1
best = none
for j in 0 .. n - 1:
    xj = x + j * S
    cj = xj + floor(predicted - xj + 0.5)          // the value of xj's phase nearest predicted
    if best is none or |cj - predicted| < |best - predicted|: best = cj   // a tie keeps the lower j
delta = best - predicted
clock = best
far = |delta| > 0.25
```

Within ¼ dance beat the running move keeps its whole-beat count and takes
the new phase (its uRaw moves by delta·2^k, and is held at 0 if that
would take it below its start). Farther, U6 holds it and Entering (U9)
re-anchors A at the new grid's next dance beat.

**U5. The latch (2.4).** tMoved = false. With lockedOk and in.rawBpm > 0:
Tl = in.rawBpm·S; when t10 = 0 or |Tl − T| > 0.03·T: tMoved = (t10 ≠ 0),
T = Tl, t10 = clamp(floor(Tl·10 + 0.5), 720, 1760). (A 3 % Schmitt latch
on the live folded tempo, set at the first locked frame.)

**U6. The running run's guards.** When run.status = Running:

- without gridOk, or when far: HOLD at once (E.2.4): Freeze when not
  heard, Leaving when the grid dropped, the re-anchor of a far jump;
- otherwise, with uNow = max(0, (clock − start)·2^k):
  - when tMoved, the move is ranged and `!paceHolds(t10, run.k, lo, hi)`:
    CUT(uNow) and run.cut = true (2.5: the move holds at its next whole
    move beat and the picker chooses again);
  - when `!in.locked` and `!run.leaving`: CUT(uNow) and run.leaving = true
    (Leaving plays on to the next whole move beat).

**U7. The run's end.** When run.status = Running, gridOk and clock ≥
run.end, with E = run.end:

- a pending switch: END the run, then SWITCH to the pending character
  (E.2.4);
- otherwise without lockedOk: HOLD(run.uEnd) (Leaving holds that step);
- otherwise: END the run.

Then, when the run is None, lockedOk, w > 0.5 and there is a character:
PICK at start = E (E.2.5). With w ≤ 0.5 no run starts (the play that just
ended showed to its end, E.2.7): idle frames show, and Entering (U9)
brings the dancer back, the phrase continuing. So no run, and no travel,
plays unseen. If clock is already at or past the new run's end (a
stall longer than the run), HOLD(run.uEnd): Entering replaces it at the
next whole beat. At most one pick a frame here.

**U8. Abandoning.** A held run with w ≤ 0.5, or a running one without
lockedOk and with w ≤ 0.5, is ENDed: idle frames take over. Then, when a
switch is pending and the run isn't Running, SWITCH now.

**U9. Entering.** When run.status ≠ Running, lockedOk, w > 0.5 and there
is a character:

- if not entering: entering = true, enterAt = floor(clock) + 1 (the next
  whole dance beat, ≤ 1 beat away);
- when clock ≥ enterAt and target > 0.5: END a held run; if
  `!phraseValid`, A = enterAt and phraseValid = true; PICK at start =
  enterAt; entering = false;
- when clock ≥ enterAt and target ≤ 0.5 (w falling through 0.5: a pick
  would show for a frame or two, then the latch would hold it a move
  beat): enterAt = floor(clock) + 1, the next beat.

Otherwise entering = false. So the dancer comes in on the beat; the phrase
continues from A when nothing invalidated it (a pause, a lost lock with
the same grid) and is re-anchored otherwise (a reset, a dropped grid, a
far re-acquire).

**U10. The idle run** (E.2.6).

**U11. The cue** (E.2.7). Then lastShowDance and lastEnergy = the cue's
showDance and energy before any freeze.

#### E.2.4 Operations

**TRACK RESET** (3.5: history, latch, phrase and beat move cleared): END
the run if any (its travel rule may move the spot); lastMove = kNoMove,
lastPlays = 0, recent = kNoMove ×3, prevTo = 0, cutExempt = kNoMove, prevK
= kNoOctave; T = 0, t10 = 0; phraseValid = false; haveClock = false,
entering = false, sinceGrid = 0; the beat cue fields (E.2.7) to kNoMove,
u 0, beatPos16 0, k 0, beatMirror false; firstRunBias = true. The weight,
the idle clock, the idle run, the spot, the character, a pending switch,
the genres, Df and forced stay.

**HOLD(u)**: run.uEnd = min(run.uEnd, u), run.status = Held, run.heldEnergy
= lastEnergy. "At once" means u = run.lastU, the u the last frame drew
for this run (E.2.7, before any freeze). A held run shows that step at
uEnd with the energy it had: 3.2's "holds the step, the offset and the
travel position at once".

**CUT(uNow)**: c = ceil(uNow); when c < run.uEnd: run.uEnd = c, run.end =
min(run.end, ceil(start + c·2^−k)). The move plays on to its next whole
move beat c and holds there until the next whole dance beat (at k = +1
that is up to ½ beat later; at k ≤ 0 none), where the run ends. ceil(uNow)
of a whole uNow is uNow: a cut on the beat holds at once.

**END(run)**, with u its position now (Running: uRaw; Held: uEnd):

- a travel run: when u ≥ beats/2, spot = clamp(run.spot + travel·sgn(o),
  −1, 1), o its (only) play's orientation. A completed travel lands, and
  one abandoned past half way counts as arrived;
- prevTo = to(o_last) when u ≥ plays·beats (the run completed; o_last the
  last play's orientation, E.2.7), else 0 (unknown: a wildcard);
- cutExempt = run.cut ? run.move : kNoMove;
- run.status = None (its other fields stay, for `run()` and the cue).

**SWITCH(c)** (3.7): END the run if any; character = c, pending = none;
lastMove = kNoMove, lastPlays = 0, recent = kNoMove ×3, prevTo = 0,
cutExempt = kNoMove (prevK stays: it is about the tempo); forced =
kNoMove; Df off; when c isn't null and `!hasTravel(c)`, spot = 0; the beat
cue fields to kNoMove, u 0, beatPos16 0, k 0, beatMirror false; entering
= false; idle.move = kNoMove and lastIdle = kNoMove, so the next idle step
(U10: this update's, or the next one's for a switch made by setCharacter)
picks the new character's idle. SWITCH itself makes no draw. So a
character without travel moves, the crab included, always dances at the
centre (1.3), and a travelling one keeps the current spot (8).

#### E.2.5 PICK(start)

```
if !phraseValid: A = start; phraseValid = true
p = mod8(start - A)
if forced != kNoMove: take it (below) and stop
for each move i, in index order, its k_i = eligibleK(t10, lo, hi, prevK, beats, travel)
    (with t10 = 0: kNoOctave for a ranged move, 0 for an unranged one)
hard(i)  = kind Beat and needs = 0 and k_i != kNoOctave and genreOk(i) and allowed(i) is not empty
soft(i)  = aligned(i) and placeOk(i) and (travel = 0 or p in {0, 4})
for level in 0, 1, 2:
    entries = moves i, in index order, with hard(i), (level >= 1 or soft(i)), and
              (level >= 2 or the no-repeat rule admits i)
    stop at the first level with an entry
no entry: auto-bounce (level 3)
```

- **genreOk(i):** an untagged move (genreCount 0); or a tagged one with
  any of its keys among the track's keys and the injected ones (2.7: a
  filter, never relaxed).
- **allowed(i)**, its orientations: unmirrored, and mirrored when it has
  kMirror; for a travel move only those o whose `startSpotMask(i, o)` has
  the current spot (section 8: the destination exists, it fits there and
  on arrival, start_at allows the spot; D ≥ 2 comes from eligibleK). A
  twin entry (below) allows only its own orientation.
- **aligned(i):** D2 < 2, or (2p) mod min(D2, 8) = 0 (3.3: p mod min(D,
  4) = 0).
- **placeOk(i):** kPlaceStart: p = 0; kPlaceEnd: (D2 = 2 and p ∈ {6, 7})
  or (D2 = 4 and p = 6); neither flag: true.
- **The no-repeat rule** (3.4 step 2), with L = lastMove: a move other than
  L is admitted; so is L when L = cutExempt (2.5's cut move carries on, in
  either orientation); otherwise, when lastPlays = 1 and L has kMirror, L
  is admitted as a **twin entry** in orientation !lastFirstMirror only
  (when that orientation is allowed); otherwise L is left out. At level 2
  every hard move is a plain entry, L included, in any allowed
  orientation.
- **The ladder** is 2.8's: phrase alignment, place and travel's p ∈ {0,
  4} give way first (level 1), then no-repeat (level 2). Continuity and
  recency are weights and never empty a level. The pace, genre and travel
  feasibility never give way.

**The weights** are integers. For each entry e of move i:

```
W(e) = weight10 * G * B * X * C * R
G = 3 when i is tagged (then it matched), else 1                      // 2.7's boost
B = 1; when travel != 0 and some o in allowed(e) has |spot + travel*sgn(o)| < |spot|:
    B = firstRunBias ? 4 : 2                                          // section 8: toward the centre
X = 2 for a twin entry, else 1                                        // 3.4 step 2
match(e) = prevTo = 0, or some o in allowed(e) has from(o) = 0 or from(o) = prevTo
C = 100 when match(e) or no entry matches, else 15                    // 3.4 step 3
R = 35 when i is in recent and e isn't a twin entry, else 100         // 3.4 step 4
```

The largest W is 100·3·4·2·100·100 = 24,000,000, so the sum of 64 fits in
uint32. A key of 0 is a wildcard: a move with no `from` follows anything,
and after a run whose end key is unknown everything matches.

**The draws**, in this order, and no others:

1. r = below(ΣW); the entry is the first, in index order, with r < W(e)
   after subtracting the W of those before it. One draw, also when there
   is one entry.
2. Its orientation: O = allowed(e); when some entry matched, prevTo ≠ 0
   and match(e), O keeps only the o with from(o) ∈ {0, prevTo}. With two
   left, o = (below(2) = 1) (mirrored on 1); with one, that one and no
   draw.

**The run**, with the chosen move i, o and k = k_i:

- plays: 1 for a travel move (3.4 step 6); for repeat > 0, the least n ≥
  repeat with n·D2 even (repeat·D whole); otherwise max(1, 8 / D2), integer
  division (fill 4 dance beats: a 1-beat move 4 plays at k = 0 and 8 at
  +1, a 2-beat move 2, a 4- or 8-beat move 1);
- run = {move i, k, plays, firstMirror o, spot = the current spot, phrase
  p, level, cut false, leaving false, Running, start, end = start +
  plays·D2/2, uEnd = plays·beats, prevU = −1, lastU = 0};
- lastMove = i, lastPlays = plays, lastFirstMirror = o; recent = {i,
  recent[0], recent[1]}; prevK = k when i is ranged; cutExempt = kNoMove;
  firstRunBias = false.

**Auto-bounce** (level 3, 2.8): run of kAutoBounce, k 0, 1 beat, plays 4
(4 dance beats), unmirrored, no draws; lastMove = kAutoBounce, lastPlays =
4, recent shifted, prevK kept, cutExempt = kNoMove, firstRunBias = false.

**A forced move** (Dm): forced = kNoMove first. When the value is a beat
move of the character (needs ignored): k = its eligibleK, or 0 when that
is kNoOctave; O = its unmirrored and (with kMirror) mirrored orientations
whose startSpotMask has the spot, or just unmirrored when none has; one
below(2) draw when O has two; plays and the history as above; level 4. The
pace, genre, alignment, place and no-repeat aren't checked. Any other
value: the normal pick.

#### E.2.6 Idle runs

U10, with cls = heard ? Listening : Quiet (2.6):

- no character: idle.move = kNoMove, nothing else;
- idle.move = kNoMove, or idle.cls ≠ cls: IDLE-PICK(cls), idle.ms = 0;
- otherwise idle.ms += dt·1000 (double); when idle.runMs > 0 and idle.ms ≥
  idle.runMs: idle.ms −= idle.runMs, then 0 when that is still ≥
  idle.runMs (a frame gap longer than the run: one re-pick, not one a
  frame), lastIdle = idle.move, IDLE-PICK(cls); when idle.runMs = 0 (a
  continuous idle) and idle.ms ≥ 2^31, idle.ms −= 2^31, so idleMs stays a
  uint32 (code characters read idleSeconds; 2^31 ms is 24.9 days).

The idle run advances while beat frames show too, so it is ready when they
stop. A class change re-picks at once, so the quiet idles start with the
pause, and show once w ≤ 0.5.

**IDLE-PICK(cls):**

```
bit = cls = Quiet ? kWhenQuiet : kWhenListening
cands = idle moves (kind Idle, needs 0) with when & bit, in index order
if none: cands = every idle move (kind Idle, needs 0)        // the other state's (2.6)
if none: idle = {kAutoIdle, unmirrored, runMs 4000}; no draws  // auto-idle, re-picked every 4 s
else:
    if cands has two or more: leave out lastIdle             // no immediate repeat
    r = below(sum of weight10); walk as the beat draw
    loops = idleMs > 0 ? 1 + below(3) : 1                    // a draw only for a timed idle
    mirrored = kMirror ? below(2) = 1 : false                // a draw only with kMirror
    idle = {move, mirrored, runMs = loops * idleMs, cls}     // runMs 0: continuous, never ends
```

#### E.2.7 The cue

**The run's u** (Running): uRaw = max(0, min((clock − start)·2^k, uEnd)).
**The must-show rule** (2.2): with b = floor(uRaw), when b > run.prevU and
uRaw − b ≤ 0.125, the cue's u is b exactly; otherwise uRaw. Then
run.prevU = uRaw and run.lastU = the cue's u. So the first render at or
after each whole move beat draws that beat's step from its start whenever
it comes at most ⅛ move beat late:
for frames on time, always at 30 fps (a frame is under 0.1 move beat at
pace 180) and at 24 fps up to pace 178 (0.126 at 180). Past ⅛ and under
¼, a step of at least 3 ticks (¼ beat) still covers the beat, so the
hit's step shows on that render; a render ¼ beat or more late (a stall
of 83 ms or more at pace 180, 125 ms at 120) can skip a 3-tick step. A
run's first frame has prevU = −1, so its step 0 is drawn from u = 0 when
the run started at most ⅛ beat before it (3.3's "u ≈ 0.1" becomes u =
0). Held: u = uEnd, no snap. None:
the beat fields keep their last values (a code character's dance half
holds its last pose while it fades, as Dancer and Crab do).

**The fields** (U11), before any freeze:

| Field | Value |
|---|---|
| beatMove, k | run.move, run.k while a run exists |
| u, beatPos16 | as above; beatPos16 = (uint32) floor(u·786432) |
| beatMirror | o_j: with j = min(floor(u / beats), plays − 1) (beats 1 for auto-bounce), firstMirror XOR (j odd) for a move with kMirror, else false |
| idleMove, idleMirror | idle.move, idle.mirrored |
| idleMs | (uint32) floor(idle.ms) |
| idleSeconds, weight | idleSec, w |
| energy | run held: run.heldEnergy; otherwise e16(w) |
| showDance | run.status ≠ None and run.show (the latch, below) |
| flash | gridOk and w > 0.5f and clock − floor(clock) < 0.12 (today's light: dance phi under 0.12 while dancing) |
| spot, rootX, rootY | spot, spotX(spot), 153 |
| state | below |

**The show latch** (3.2). run.show is set to w > 0.5 when the run starts
(always true: U7 and U9 pick only above 0.5) and again on its first
running frame (prevU = −1). On a later running frame, before prevU moves
on, with j = floor(uRaw / beats) (beats 1 for auto-bounce): when j >
floor(prevU / beats) and j < plays (a new play starts), show = w > 0.5;
otherwise, when floor(uRaw) > prevU (a whole move beat) and w > 0.5, show
= true. A held run keeps it. So beat frames go off only where a play
starts and come on only at a whole move beat: each kind shows at least a
move beat, less a frame, however w wobbles about 0.5, and a travel (one
play) once shown is seen to its end. Beat frames at w ≤ 0.5 need a
running run under a locked grid (U8 ends the others), where DanceRate
already runs at the full rate. (Note, step 3's review: showDance was
`w > 0.5` alone, which flickered 1-4 frames of a dance pose on real music
3.8 times a minute, snapped a walk back to its start spot when w dipped,
and let runs, travels included, play unseen. Replayed over the 77 cached
tracks at 30 fps with the test dancer, the latch and U7's and U9's gates
take the toggles from 1,282 to 1,058, the shortest dance shown from
0.03 s to 0.43 s and the shortest idle gap to 0.36 s; started from spot
−1, the idle dancer's 96 px jumps between spots go from 20 to none. The
cost: idle frames with w > 0.5 while the dancer comes back in, at most a
move beat (2.0 % of the locked dancing time, up from 1.1 %; the longest
1.9 dance beats, a half-time move).)

**state:** not heard: Freeze when a run exists (Running cannot be: U6
held it), else Quiet; heard: Dancing when the run is Running and
lockedOk; Leaving when a run exists without lockedOk; Entering when
lockedOk and w > 0.5 (waiting for its beat, a held step showing when
there is one); Listening otherwise (no grid, an unlocked grid, or a fade
with w ≤ 0.5). A running run under a locked grid with w ≤ 0.5 is
Dancing: its play in progress shows to its end, then idle frames until a
whole move beat with w > 0.5; when it ends no run follows (U7) and the
state is Listening.

**Freezes** override the shown cue only; the state machine runs on
underneath, and the cue the state machine keeps (lastShowDance,
lastEnergy) is the unfrozen one.

- **Df** (when set; it wins over k): a beat move (kind Beat, or
  kAutoBounce): beatMove = it, beatPos16 = pos16, u = pos16 / 786432.0, k
  0, beatMirror = mirrored, showDance true, weight 1.0f, energy 65536,
  flash false. An idle move (kind Idle, or kAutoIdle): idleMove = it,
  idleMs = (pos16·10) >> 16 (pos16 in Q16 10-ms units, so step s's start
  s·65536 gives 10·s), idleMirror = mirrored, showDance false, weight 0,
  energy 39322, flash false.
- **k<n>** (in.freeze 0..15; other values count as −1; 3.8): f =
  freezeMove(character), or kAutoBounce with no character; L = its beats
  (1 for kAutoBounce); beatMove = f, beatPos16 = (n·98304) mod
  (L·786432), u = beatPos16 / 786432.0, k 0, beatMirror false, showDance
  true, weight 1.0f, energy 65536, flash = (n mod 8 = 0). For the crab's
  2-beat `shuffle` that is u = n/8, so phi = frac(u) and odd = floor(u)
  & 1 give `Crab::frozen((n % 8)/8, n >= 8)` exactly.

#### E.2.8 The calls

- **Before the first update**, cue() is {beatMove kNoMove, u 0,
  beatPos16 0, k 0, unmirrored, idleMove kNoMove, idleMs 0, unmirrored,
  idleSeconds 0, weight 0, energy 39322, showDance false, flash false,
  spot 0, root (160, 153), Quiet}, and run() is {kNoMove, k 0, plays 0,
  unmirrored, spot 0, phrase 0, level 0, not cut, None, start 0, end 0,
  uEnd 0}.
- **Choreographer()** and **reset(pathHash)** (dance on): every state of
  E.2.2, cue() and run() to their first values, except the character: a
  pending switch becomes
  the character at once (no draw), and reset makes no draw at all.
  rng = seed(pathHash); `started` is false, so the next update reseeds
  with the inputs (U2). Today's `setActive(true)` resets the dancers;
  step 4 calls reset there.
- **onTrack(pathHash, keys, count)** (3.5's gapless join, and every other
  track change): pathHash is kept; the track keys become the first
  min(count, 16) of keys (null with count 0: none); the injected keys are
  cleared; seed(pathHash ^ restartsSeen ^ lastAcqIn). The phrase, the
  latch, the run and the history stay.
- **setCharacter(c):** c = the current character: a pending switch is
  dropped, nothing else. Otherwise SWITCH(c) at once when there is no
  character, the run isn't Running, or the last update's showDance was
  false (3.7: "at once while idle"). Otherwise the switch waits for the
  next whole dance beat: pending = c, switchAt = floor(clock) + 1, run.end
  = min(run.end, switchAt), run.uEnd = min(run.uEnd, max(0, (switchAt −
  start)·2^k)) (0 when backward nudges left the clock more than a beat
  behind the run's start). U7 then ends the run there, switches and picks afresh with
  the new character at the same spot (or the centre, SWITCH's rule). A
  second call while pending replaces the pending character.
- **freeze(move, pos16, mirrored):** sets Df when move is a move index of
  the current character, kAutoBounce or kAutoIdle; any other value
  (kNoMove included) unfreezes. A switch unfreezes.
- **forceNext(move):** forced = move; the next PICK takes it (E.2.5). A
  switch clears it.
- **setSpot(s):** s clamped to −1..1; END the run if any, then spot = s
  (overriding END's travel rule); entering = false. Dancing resumes
  through Entering at the next beat. (Note, step 3's integration: cue()'s
  spot, rootX and rootY change at once, for Ds's screenshots; its other
  fields wait for the next update.)
- **addGenre(key):** appended to the injected keys while there are fewer
  than 4; otherwise ignored. Cleared by onTrack.
- **eligibleNow(i):** kNoOctave for an i out of range, an idle move, a
  move with a needs bit, or a tagged move no key matches; with t10 = 0,
  0 for an unranged move and kNoOctave for a ranged one; otherwise
  eligibleK(t10, lo, hi, prevK, beats, travel). (Dl's "at which k each is
  eligible now"; alignment, place and travel feasibility aren't in it.)

#### E.2.9 Section 3, where it lives

| Section 3's rule | E.2 |
|---|---|
| 3.2 Quiet / Listening idles, runs of 1-3 loops, no immediate repeat, fallbacks, auto-idle | E.2.6 |
| 3.2 Entering: w > 0.5 and a locked grid, the next whole dance beat, A | U9 |
| 3.2 Dancing: a new run each time one ends | U7 |
| 3.2 Leaving: on to the next whole move beat, that step held, idles at w ≤ 0.5 | U6 (cut), U7 (hold), U8 |
| 3.2 Freeze: hold at once, quiet idles at w ≤ 0.5 | U6, U8, E.2.6 |
| 3.2 which frames show (the show latch); the beat light | E.2.7, U7, U9 |
| 3.3 runs start where the last ended, overshoot carried; p; alignment; place | U7, U9, E.2.5 |
| 3.4 the pick | E.2.5 |
| 3.5 PLL nudge | U4 (−0.1 ≤ Δ ≤ 1.5, same acquisition, same S) |
| 3.5 re-acquire, jump or fold change: M recomputed, the shift, ¼ beat, hold and re-anchor, re-test | U4 REPHASE, U6, U9, U5 |
| 3.5 lost lock; grid dropped; pause and resume | U6, U7, U8, U9 |
| 3.5 track change with a restart | U2, TRACK RESET |
| 3.5 gapless join | onTrack |
| 2.4 the latch, eligible_k with prevK; 2.5 the tolerance and the cut | U5, E.2.5, U6, CUT |
| 2.2 the must-show rule | E.2.7 |
| 3.6 the weight and the energy | U3, E.2.1's e16, HOLD |
| 3.7 switching | setCharacter, SWITCH, U7 |
| 3.8 k<n>, Df | E.2.7 |
| 3.9's hooks: Dm, Ds, Dg, Dl | E.2.8 |
| section 8: travel eligibility, the bias, the spot | E.2.5, END, SWITCH |

### E.3 Determinism

- **One build, one input.** The same sequence of calls (reset, onTrack,
  setCharacter, freeze, forceNext, setSpot, addGenre and update, with the
  same arguments and the same characters' labels) gives the same cues,
  every field equal, floats and doubles compared with `==`. Nothing else
  is an input: no clock, no address, no global. test_dance_choreo runs
  every script twice and compares.
- **Integers, shared with Python, on every target:** B's functions
  (eligibleK, paceHolds, reach, ease16, hop16, sway16, stageX, frameAt,
  extent, fits, startSpotMask, dirtyPx, the budget) equal dancepack.py's
  bit for bit.
- **Integers inside the Choreographer:** t10, the phrase, every run
  boundary (start, end, A, enterAt, switchAt), plays, the weights and the
  draws, beatPos16, idleMs and e16.
- **Doubles:** the clock, trackerBeats·S, lastS, lastTl, sinceGrid, T,
  uRaw and u, uEnd and the idle run's ms. U1 and U3 bound every input
  (E.2.3), so they stay finite and every conversion of one to an integer
  (start, end, enterAt, switchAt, beatPos16, idleMs) is in range. They use
  +, −, ×, ÷, floor, ceil, llround, fabs and comparisons only, which IEEE
  754 binary64 rounds exactly: the x86-64 host
  (SSE2) and the ESP32 (GCC's soft-float) agree bit for bit, given equal
  inputs. lib/core is never built with flags that change that (`-mfma`,
  `-march=native`, `-ffast-math`).
- **Floats:** w (Dancer's formula: `std::exp` on a float), idleSec, dt and
  confidence (inputs), and e16's c. The ESP32's FPU fuses a float multiply
  and add (GCC's default `-ffp-contract=fast` emits `madd.s`), and
  newlib's `expf` isn't glibc's, so w can differ from the host's in its
  last bits.
- So section 3.1's claim narrows to this: a host replay of a device's
  BeatInputs gives the device's cues, except on frames where w lies within
  float rounding of 0.5 (or of an e16 step); after such a frame the two
  may part. Two device runs differ anyway (3.1). The tests pin host
  behaviour; the replay runner (10.1) is a tool, not a proof.

### E.4 The host tests

All three new suites follow D.4's house form (one `.cpp` per
`test/test_<name>/`, `setUp`/`tearDown`, `UNITY_BEGIN`/`RUN_TEST`/
`UNITY_END`, D.4 item 12's `#if defined(DANCE_ASAN) &&
!defined(__SANITIZE_ADDRESS__)` `#error` at the top), include the vector
headers and PackEdit.h by relative path, and run in under 30 s each
under ASan. Packs handed to the reader are PackEdit's exact-size
`HeapCopy`s.

#### BeatScript.h (test/support/dance/, written by the test writer)

Test-only, so std::vector and floats are fine. It holds:

- **`Script`**, one BeatInput a frame at a fixed rate, from a scripted
  tracker:

  ```cpp
  namespace beatscript {
  class Script {
   public:
    explicit Script(int fps);   // dt = dancerate::periodMs(fps) / 1000.0f: 33 ms at 30, 42 at 24
    // From the next frame on:
    Script& heard(bool on);     // the track's time stands still while not heard (a pause)
    Script& grid(bool on);
    Script& locked(bool on);
    Script& confidence(float c);
    Script& acquire(double bpm, double beatAt, int32_t index = 0);  // a new grid: beat `index` at
                                // track time beatAt (s); acquisitions + 1; grid on
    Script& nudge(double beats); // the grid moved by `beats` tracker beats, same acquisition
    Script& drift(double bpm);  // (review) the tempo drifts to bpm, same acquisition: no jump
    Script& restart();          // restarts + 1, grid off, the TempoFold reset (DanceMode::restart)
    Script& freezeK(int n);     // BeatInput.freeze, -1 off
    dance::BeatInput frame();   // the next frame's input (dt 0 on the first)
    std::vector<dance::BeatInput> seconds(double s);
    double trackTime() const;   // tau
    double danceBeats() const;  // the script's own trackerBeats * S at tau
  };
  }  // namespace beatscript
  ```

  A frame's trackerBeats is index + (τ − beatAt)·bpm/60, τ the track time,
  which advances by dt only while heard; rawBpm is (float) bpm; foldScale
  is a `dance::TempoFold`'s apply(rawBpm), kept by the script and applied
  on frames with a grid, as DanceMode keeps `fold_`; grid = heard and the
  grid on; trackerBeats is 0 without a grid. A first `acquire` starts
  locked with confidence 0.8 unless set otherwise.
- **`ScriptCharacter`**, a hand-built Character: `add(MoveLabels)`
  returning the index; helpers `beatMove(name, beats, lo, hi, weight10 =
  10)` and `idleMove(name, ms, when, weight10 = 10)` filling every other
  label with its default (0, null, StartAt::Any); and `setSpots(i,
  unmirrored, mirrored)` overriding startSpotMask for a move (otherwise
  the default). Its info: id `script`, name `Script`, origin BuiltIn,
  inCycle true.
- A helper opening kTestPack (or patched copies) into a DrawnCharacter,
  as a card pack would be (E.1's split, with BudgetPass run to the end).

#### test_dance_choreo

Two conventions hold through the list (Note, step 3's implementation: the
items below that use them were wrong without them):

- **The true u** of a running run is (danceClock() − run().start)·2^k,
  the uNow that CUT takes (E.2.4). The cue's u isn't it on the first frame
  after a whole move beat, where the must-show rule draws that beat
  exactly (E.2.7), so a cut is checked against ceil of the true u.
- **A test that counts picks over many restarts gives each restart its
  own track:** onTrack(fnv1a32 of a distinct path, e.g. `track<k>`) and
  then restart(). With one pathHash the seed pathHash ^ restarts ^
  acquisitions takes only a few values ((k + 1) ^ k when the acquire comes
  a frame after the restart, the same 0x4D50444E every time when both
  share a frame), so the shares below wouldn't hold.

1. **B.7 in C++:** every kEligibility row through eligibleK (prevK and k
   127 as kNoOctave) and every kPaceHolds row through paceHolds; B.7's two
   tables; paceHolds with k outside −1..1.
2. **The latch:** at S = 1, T latched 100.0 on the first locked frame
   (t10 1000); then, each from T = 100: rawBpm 103.0f doesn't move it,
   103.5f does (t10 1035), 97.0f doesn't, 96.9f does (t10 969); an
   unlocked grid never moves it; t10 is 0 before the first lock and after
   a restart.
3. **Pace in the pick** (ScriptCharacter): an unranged move always at k
   0; a ranged move at eligibleK's k, consecutive ranged moves keeping
   prevK; the 180 cap for a running move (10.3: a 170-180 1-beat move
   danced at +1 from t10 880, then a re-acquire at 90.7 BPM within ¼ beat:
   t10 907, the run cut, `run().uEnd` = ceil of its true u then, the cue's u
   held there until `run().end`, never past); half time never for an
   8-beat move (a lone 8-beat 36-60 move at T 120 gives auto-bounce).
4. **An I Wonder trace** (30 fps, the fixture's DrawnCharacter and,
   separately, a ScriptCharacter with one 2-beat move 90-100): locked at
   95.7 BPM; at 20 s a re-acquire at 191.4 whose beat 0 falls on a
   midpoint between the 95.7 beats (so REPHASE's j = 1 is the shift that
   matches: j = 0 would be ½ beat off); at 40 s back to 95.7 on a true
   beat; to 60 s. Every frame: t10() = 957; no run held or cut;
   danceClock() advancing by dt·(rawBpm·S)/60 within 1e-6 at every frame,
   the flips included; and for every frame of a ranged run, paceHolds(957,
   k, lo, hi) (the pace never more than 3 % outside the running move's
   range). Then once more with the flip back on a midpoint: there S = 1
   has one shift and delta = ½, so that frame the run is Held, and the
   next pick comes at floor(clock) + 1 with `run().phrase` 0.
5. **A 4:3 trace** (ScriptCharacter: A 2 beats 100-130, B 2 beats
   140-170): at 120 BPM only A dances (level 2 repeats); mid-play, one or
   two frames after a whole dance beat, a re-acquire at 160 BPM whose beat
   0 is on that beat (|delta| ≤ ¼): that frame t10 becomes 1600,
   `run().cut` is true and `run().uEnd` is ceil of the true u (A's next
   whole move beat), `run().end` the dance beat where u reaches it; the
   cue's u never passes it; the next run is B at k 0, starting exactly at
   that end.
6. **States:** quiet (`heard(false)`: Freeze while a run exists, Quiet
   after w ≤ 0.5; a quiet idle shows); listening (no grid; a fresh grid
   not yet locked; an unlocked grid; a grid dropped while heard: Leaving,
   then Listening); the idle fallback (a character with only a quiet idle
   uses it while listening) and auto-idle (no idles: kAutoIdle, runs of
   4,000 ms); idle runs of 1-3 loops (idleMs advancing by dt·1000, a new
   pick after loops·idleMs, never the same move twice in a row when the
   class has two).
7. **Genre** (ScriptCharacter): tagged moves never picked with no track
   genres (dormant) and never when the keys don't match; a matching track
   key via onTrack or addGenre makes them eligible; the ×3: over the
   first picks after 2,000 restarts, each on its own track (no history
   then), with one matched
   tagged and two untagged 1-beat moves of equal weight, the tagged one's
   share is within 0.05 of 3/5; the ladder: a character whose only moves
   are unmatched tagged ones plays auto-bounce (`run().level` 3).
8. **Picking:** never the same move in two consecutive runs while another
   hard move exists, except a single play of a mirrorable move followed
   by its twin (the other orientation, at ×2) and the cut move; plays per
   E.2.5 (a 1-beat move 4 plays at k 0 and 8 at +1, `repeat = 3` on a
   1-beat move at +1 gives 4, a travel move 1), every run a whole number
   of dance beats; beatMirror alternating play by play in a mirrorable
   run; the from/to preference (after a forced run of X, `to` = k1, that
   completes, the pick between Y, `from` = k1, and Z, `from` = k2, equal
   1-beat moves: Y's share within 0.02 of 100/115 (Note, step 3's
   review: the seeds are fixed, so the share is exact; 0.05 let
   continuity 10 or 20 pass) over 2,000 restarts,
   each on its own track; a
   key of 0 matching anything);
   alignment (at level 0, `run().phrase` mod min(D, 4) = 0); `place =
   start` only at p 0; `place = end` only at D = 1 with p 6 or 7, or D = 2
   with p 6; `run().level` 1 when only alignment blocked, 2 when only
   no-repeat did.
9. **The draw, exactly:** the seed is pathHash ^ restarts ^ acquisitions
   of the first update after a reset (0x4D50444E for 0); a
   ScriptCharacter with two unranged 1-beat moves of weight10 10 and 30,
   no keys, no history: the first pick is move 0 exactly when
   `Xorshift32(seed).below(10·100·100 + 30·100·100) < 10·100·100`; with
   kMirror on the chosen move, the next draw decides its orientation.
10. **Travel** (the fixture: `walk` travels +1, start_at side, from −1
    unmirrored and +1 mirrored): never picked from spot 0; from −1 (setSpot)
    picked only at p ∈ {0, 4} on level 0 and only unmirrored; when its run
    ends, cue.spot is 0 and stays through idles. The centre bias, from
    spot −1 (setSpot before each lock), with T (2 beats, unranged, travel
    +1) and P (2 beats, unranged) of equal weight: with only T and P, the
    first pick after each of 2,000 restarts, each on its own track (p 0),
    is T with a share within 0.02 of 4/5 (×4: 0.05 let ×5 pass); with a
    third move F (1
    beat, unranged) forced first (4 plays, using up the first-run bias)
    and setSpot(−1) in its fourth dance beat, the next pick (p 4, F left
    out by no-repeat), again over 2,000 restarts on their own tracks, is
    T within 0.02 of 2/3 (×2). D ≥ 2: a 2-beat travel move never runs at
    k = +1.
11. **Determinism:** every script above run twice from a new
    Choreographer gives equal cues; a different pathHash gives a
    different sequence of runs; a gapless join (onTrack mid-run) keeps the
    run, the phrase count and t10, takes the new genres, clears Dg's, and
    changes the later picks.
12. **Discontinuities:** a nudge of −0.05 beat passes through (no hold);
    at 120 BPM a nudge of −0.2 is a jump within ¼ (the clock ends 0.2·S
    behind the prediction, the run
    goes on, its u held at 0 if that crosses its start); a jump of 2 whole
    tracker beats at S = 1 changes nothing (delta 0); a re-acquire ½ beat
    off holds, then re-anchors (`run().phrase` 0 at the next whole beat);
    Leaving (locked(false) mid-run: the run cut at ceil of its true u;
    idles once w ≤
    0.5; and with the lock back after two frames, dancing goes on with a
    new pick at `run().end`); Freeze on pause (that frame the cue's u and
    energy equal the last frame's, state Freeze) and resume (Entering,
    the next pick at the next whole beat, `run().phrase` continuing the
    count from before the pause); a track reset (restart(): t10 0, prevK
    none, run None and the beat fields kNoMove that frame; the spot of a
    run without travel unchanged, w and idleSeconds going on without a
    jump); Entering (from Listening: idle frames until the
    first frame with danceClock() ≥ floor(clock at Entering's start) + 1,
    then `run().start` equal to that beat and phrase 0).
13. **Timing,** at 24 and 30 fps, at every whole pace P from 36 to 180: a
    ScriptCharacter with one 1-beat move of range P-P, at T = P for 80-160
    (k 0), T = 2P for 40-79 (k −1), T = P/2 for 161-180 (k +1), and T = 2P
    for 36-39 reached by acquiring at 100 BPM first, so the TempoFold keeps
    S = 1 (its 72-176 band) and the latch moves (Note, step 3's
    implementation: 80 BPM first, as this said, leaves T at 80 for P = 39,
    since 78 is within the latch's 3 % of 80); for every whole move beat b
    a running run crosses (its true u, (danceClock() − run().start)·2^k,
    going from below b to at least b), that frame's beatPos16 lies in
    [12b·65536, (12b + 3)·65536): the hit's 3-tick step shows on the first
    render at or after its beat; and u = b exactly whenever the true u − b
    ≤ ⅛.
14. **Weight and energy:** at 30 fps the weight changes by at most 1/12 a
    frame (by at most 0.1 at 24 fps); locked at confidence ≥ 0.5 it is
    above 0.95 after 2 s; a lost lock takes it under 0.5 within 0.3 s;
    e16 at w ≤ 0.5, 0.65f, 0.75f and 1 is 39322, 47186, 52429 and 65536;
    the beat light only with w > 0.5 and the clock's phase under 0.12.
15. **Switching:** at once while idle (idle frames from the new character
    on the next update); while dancing, at the next whole dance beat (the
    old run's last frame, then the new character's first run starting at
    floor(clock at the call) + 1), with w, idleSeconds and (for a
    travelling character) the spot unchanged; a switch to a travel-less
    character sets the spot to 0.
16. **Freezes:** k0-k15 on the fixture (bounce, 2 beats): beatPos16 =
    n·98304, u = n/8, showDance, weight 1, energy 65536, flash exactly at
    k0 and k8; on a character whose freeze move has 1 beat, k9 gives u =
    1/8; the freeze chain (kFreeze, then unranged 2-beat, then unranged,
    then kAutoBounce); Df on a beat move (exact pos16, mirrored) and on an
    idle move (idleMs = 10·s for step start s); Df over k; freeze(kNoMove)
    and a switch unfreeze.
17. **The console hooks:** forceNext (taken at the next pick, `run().level`
    4, even when not eligible, k 0 then); setSpot (the run ended, the cue's
    spot at once); addGenre (up to 4, cleared by onTrack); eligibleNow
    against eligibleK.
18. **Size:** `sizeof(Choreographer)` ≤ 1,024.
19. **Added in integration** (rules no test above pinned; each test fails
    when its rule is broken in the code): U7's stall (a forward jump of
    1.4 tracker beats past the next run's end: that run held at its end at
    once, then picked again at floor(clock) + 1, the phrase continuing);
    U8's pending switch made on a pause's frame, and a running run without
    a lock ended as soon as w ≤ 0.5, before its cut point; U1's negative
    and NaN dt counting as 0; the idle clock's wrap at 2,000 s; setSpot's
    cue at once (E.2.8); the cut move exempt from no-repeat (level ≤ 1 at
    its new octave) and a run not cut getting no exemption; the second
    draw's orientation keeping only a matching `from`; prevTo 0 after an
    unfinished run (1/2, not 100/115); SWITCH keeping prevK; REPHASE
    setting entering = false (a far re-acquire during Entering picks at
    the new grid's next beat), its ¼-beat line (±0.24 runs on, ±0.26
    holds) and its tie keeping the lower j; onTrack's seed, exactly; the
    must-show rule's b > prevU (a run held at u 0 then draws its true u);
    the travel bias only toward the centre (+2 from −1: 1/2); and TRACK
    RESET clearing the phrase when the new grid applies on its frame.
20. **Added in review** (each fails when its rule is broken: a mutation
    run killed every listed rule's mutant): U4's fold change with no
    acquisition (a re-acquire to 175.9 BPM, then a drift to 176.3 with
    the same acquisitions; and 72.2 to 71.8): clock − S·trackerBeats a
    multiple of min(1, S) on every frame; the show latch (the fixture's
    walk shown to its end as w falls under 0.5, the drawn root never
    moving more than 48 px a frame; no run starting at w ≤ 0.5, the spot
    unchanged; with the confidence wobbling around 0.31, beat and idle
    frames each lasting at least 14 frames at 30 fps; Entering not
    picking with w > 0.5 and the target ≤ 0.5); hostile inputs (one frame
    of a NaN, infinite or huge confidence, dt, trackerBeats, foldScale or
    rawBpm in a dance, then dancing again, with or without a restart; a
    continuous idle for 58 days in 1,000 s frames, its ms below 2^31; a
    timed idle after a 1e30 s gap re-picked once); a switch with the
    clock more than a beat behind the run's start (uEnd 0, u never
    negative); E.2.6's idle draws exactly (the entry, the loops of a
    timed idle, the orientation of a kMirror one, and a continuous
    kMirror idle re-picked at each class change); E.2.5's draws over two
    picks (the entry draw with one entry, the orientation draw only with
    two orientations); prevTo from the last play's orientation (4 and 3
    plays, within 0.03 of 100/115); a switch clearing forceNext, at once
    and pending; U9's w > 0.5 at a steady 0.579 (confidence 0.33), and
    never at 0.461; the idle classes over 300 seeds; the centre bias of a
    mirrored travel from +1 (within 0.03 of 4/5); the must-show ⅛ (a
    frame 0.2 late keeps its true u, one 0.125 late draws the beat);
    END's half-way boundary (the lock lost at u 0.85-0.92 of a 2-beat
    travel, cut to 1.0: arrived); `place = end` at p 6 and at p 7 for D
    1, and at p 6 for D 2. The Twin also writes E.2.3's weight again
    (equal to the cue's with `==`) and E.2.7's latch, and checks that
    every new run starts at w > 0.5 and the clock stays under 2^52.

#### test_drawn_character

1. **B.2 and B.3:** ease16's table (B.2) at f 0, 16384, 32768, 49152 and
   65536 for every ease; hop16's table (B.3); sway16(0, 0) = −65536,
   sway16(0, 32768) = 0, sway16(0, 65535) = 65534, sway16(1, 0) = 65536.
2. **B.4's worked example** on a pack built with PackEdit (scale 2, cell
   8×8, anchor 4,8, shadow 10, one frame with box (2, 2, 4, 6), the
   2-beat move of B.4): every cell of its table, both orientations; the
   hold-travel variant's rootX 62, 72, 63, 158 and 165; tick 3's Y of −4
   (B.0's floor); e16 = 47186 at tick 15; and the clamp example, both
   orientations. B.3's 1-beat sway: rootX 156, 160, 164, 160, 156 at ticks
   0, 6, 12, 18 and 24.
3. **Step lookup** at exact tick edges: pos16 = s_i·65536 − 1 shows step
   i − 1 and s_i·65536 step i, for every step of every fixture move; the
   wrap at L·65536 back to step 0.
4. **Mirror symmetry:** for every frame, B.11's mirrored offsets: L′ =
   scale − R and R′ = scale − L (the anchor column mirrors to cellW − 1 −
   anchorX).
5. **The bounds hold every pixel:** for every kept move of the fixture,
   both orientations, spots −1, 0 and +1, e16 39322 and 65536, and pos16
   every 7 ticks over two plays: the frame drawn with `blit` at the box's
   top-left into a 320×164 canvas inside a canary border writes only
   inside `box`, `box` lies on the stage, and for a move without travel at
   spot 0, box.x0 − 160 ≥ lo and box.x1 − 160 ≤ hi of `extent`.
6. **stageX:** its ends (p 0 gives the start spot, 65536 the end, for
   every ease), a leftward travel's rounding (rhu with floor), and the
   clamp of a spot past ±1.
7. **B.11:** extent (−6, 14) and start spots −1 and 0 for B.4's move; the
   wrap example's (−36, 16); the fixture's `walk`: 0b001 unmirrored, 0b100
   mirrored; every other fixture move 0b111; fits: `heavy` (extents
   (−96, 96) and (−92, 100)) only at spot 0, `big` ((−64, 64) and
   (−60, 68)) everywhere but mirrored at +1.
8. **The view of the fixture:** `init` false on a pack not open; 13 moves
   (MOVS 0-7 and 9-13: `future` is dropped), packMove 0-7 then 9-13; the
   labels C.10 fixes (names, kinds, beats, paces, weight10, when, flags,
   repeat, travel, startAt; `point`'s keys 1 → 2 and mirrored 1 → 3;
   `fast`'s 4 → 1; idleMs 4,000, 3,300 and 3,000 for sway, breathe and
   look; `nod`'s three genre keys FoldVectors' hashes); the info (id
   `test`, name `Tëst`, author, licence, url, origin and inCycle as
   given, ignoredMoves 1); `freezeMove` 0; `hasTravel` true;
   DrawnCharacter::frameAt equal to the free frameAt through packMove.
9. **The engine moves:** auto-bounce at pos16 0 is the rest frame at
   (spotX, 153) and its hop is B.3's at 4 native px; auto-idle's rootX is
   spotX − scale at 0 ms (pos16 0), spotX + scale at 2,000 ms (pos16
   200·65536), and back at 4,000 ms.
10. **BudgetVectors' traces:** every kTrace frame of every kTraceCases
    case, its pos16 through the free frameAt (all 14 records, `future`
    included) and, for the 13 kept, through DrawnCharacter::frameAt: frame
    with bit 15 for the drawn mirror, rootX, rootY and the rect.
11. **Added in integration:** copies of the view (E.1's note), its genre
    keys still read after the original is freed.
12. **Added in review:** a travel move with no `go` flag travels during
    every step (rootX 64, 88, 112, 136 and 160 at ticks 0-24, linear,
    from −1; the same with every step flagged; with `go` on the second
    step only, 64, 64, 64, 112, 160); a 160 px shadow clipped at spots −1
    and +1 (x 0-144 and 176-320), and a 120-row frame keyed 40 px up
    pushed down to the stage's top with its root (rootY 120); views of 20
    and 64 kept moves; `point`'s fromKeyM patched to 2 (fromTagM 2); GEOM's
    rest frame with bit 15 (the engine moves drawn mirrored); and
    readMoveSteps equal to step-by-step reads, its starts and travelling
    sums included.

#### test_dance_budget

1. **B.5:** dirtyPx's examples (192 px from tick 6 to 15, 280 from 0 to
   24 on B.4's move), a null last rect, empty rects, and the 1.3 boundary
   (10·U = 13·(A + B) gives the union).
2. **kBudgetCases:** for each move of kTestPack, budgetCaseSpecs gives
   exactly its rows, in order; budgetCase gives each row's frames, median
   and peak.
3. **kMoveBudgets:** budget() gives each move's median, peak and light
   (`big` amber, `heavy` red, the rest green), and with a null scratch the
   same peak and light, median 0.
4. **kTrace through BudgetRun:** n, pos16, frame and mirror, root, rect
   and dirty, frame for frame.
5. **The worked examples:** A.17's dot (six cases, median 4, peak 6,
   green); B.6's (B.4's move: 12 cases; 30 fps, pace 124, spot −1: N 30,
   median 160, peak 266; 24 fps, pace 124: median 176, peak 304; the move
   median 192, peak 304, green).
6. **The peak is never under a frame's own rect:** in every case, the
   peak ≥ rectArea of every frame's rect for n ≥ 1.
7. **The verdicts:** startSpots and budgetRed keep every move of the
   fixture (`heavy` is heavy, `big` amber); with `heavy`'s kHeavy cleared
   (a patched copy), budgetRed drops it as `budget red (24576 px)`; with
   `walk`'s kMirror cleared, start_at centre and every step's dx 10
   (test_dancepack.py's case: extents (−24, 68) and (−64, 28), start spots
   none unmirrored and 0 mirrored), startSpots drops it as `travels from
   no spot`.
8. **BudgetPass:** on the fixture as a card pack, no drop and 13 true
   steps; on a copy with two red moves (`heavy` with kHeavy cleared, and
   `big` keyed −12,0 and 12,0 as `heavy` is: 24,576 px each), their drops
   arrive after open()'s in MOVS order, one per step(), with dropped and
   keptMoves following;
   false at once on a pack with no kept move, a pack not open and a null
   pack.
9. **Added in review:** budgetCaseSpecs, every case's N, median and peak,
   and the move's budget for an odd rlo + rhi (90-131: paces 90, 110 and
   131), lo = hi (100-100: two cases) and B.4's move with kMirror (24
   cases: kBudgetMaxCases), all dancepack.py's for the same bytes; the
   longest case, a 12,000 ms idle (N 286 and 364, median 320, peak 640,
   the median over all 364 values); and BudgetPass::run in slices (run(1)
   and run(0) one frame a call, a move's drop on the call after its last
   frame; slices of 2, 7, 64, 256 and 100,000 dropping what step()
   drops; a pack cleared mid-pass done at once).

#### test_dance_pack and test_beat_tracker

- **test_dance_pack**, D.4 item 10 again with the real code: each kOpen
  case opened with `OpenOptions{0, &dance::startSpots, nullptr}` (through
  a counting wrapper: called exactly for step 2's kept moves, in order)
  gives the full list's drops but `budget red`, in order; a BudgetPass run
  to its end then adds the `budget red` drops, in MOVS order; dropped =
  fullDropCount, and the digest is unchanged. Every other item stays,
  item 4's stub hook included. (Step 3's review: the accessor check run
  on every accepted pack also compares `Pack::steps` with `step()` on
  every record, at caps 34 and 3, with a null out, and out of range.)
- **test_beat_tracker** gains `test_acquisitions_count_new_grids`: 0 after
  begin(); unchanged by reset() and by setSampleRate(); on
  test_tempo_change_relocks' 120 → 135 track fed a hop at a time, it never
  falls, rises by at most 1 a hop, is at least 1 when the tracker first
  locks, and rises again after the change (the grid at 135 BPM); and every
  hop where it rises has a valid grid. (Note, step 3's implementation: on
  this track the tracker unlocks at 15.1 s and re-acquires at 15.5 s, so
  that rise isn't made while locked; the unchanged tracker re-acquires
  after an unlock on all three of test_tempo_change_relocks' tracks.
  Step 3's review: the feedHop half prints how many new grids came while
  locked rather than asserting one did; that is the tracker's tuning, not
  the counter's.) The other tests don't change.

#### native_asan and CI

- `platformio.ini`'s `[env:native_asan]` `test_filter` gains
  test_dance_choreo, test_drawn_character and test_dance_budget, and its
  comment line names all five; the CI step becomes `pio test -e
  native_asan -f test_dance_pack -f test_dance_blit -f test_dance_choreo
  -f test_drawn_character -f test_dance_budget`. Step 3's review adds
  `-fsanitize=float-cast-overflow` to its build_flags (D.5). Nothing else
  in either file changes.
- On the laptop, this command stands in (D.5's, with what step 2's
  integration learnt: copy only what the build needs, and run with ASLR
  off so GCC 12's ASan doesn't hang):

```sh
MSYS_NO_PATHCONV=1 docker run --rm --security-opt seccomp=unconfined -v "$(cygpath -w "$PWD")":/src:ro rust:bookworm sh -c 'mkdir /w && cp -r /src/lib /src/src /src/test /w/ && mkdir -p /w/.pio/libdeps/native && cp -r /src/.pio/libdeps/native/Unity /w/.pio/libdeps/native/ && cd /w && for t in test_dance_pack test_dance_blit test_dance_choreo test_drawn_character test_dance_budget; do g++ -std=gnu++17 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fsanitize=float-cast-overflow -fno-sanitize-recover=all -DDANCE_ASAN=1 -Ilib/core -Isrc -I.pio/libdeps/native/Unity/src test/$t/$t.cpp lib/core/*.cpp .pio/libdeps/native/Unity/src/unity.c -o /tmp/$t && setarch x86_64 -R /tmp/$t || exit 1; done && echo ASAN-ALL-OK'
```

(Note, step 3's review: GCC folds `(int64_t) floor(x)` into an unchecked
builtin at -O1, so the review also ran this command once with -O0 in
place of -O1; both print ASAN-ALL-OK.)

### E.5 beat_eval builds and scores identically

The ground-truth corpus (beats.json) is gone, so `beat_eval.py run` can't
run. What it would score is the runner's output, so step 3 compares that
directly: the runner built from the commit step 3 starts from (HEAD while
step 3 is uncommitted: a5a6fdd) and the one built from the working tree,
run over the same inputs, must print the same bytes, the one timing field
aside. Scores of equal outputs are equal.

- **The base sources** are `git archive HEAD lib/core tools/beat_eval
  test/test_beat_tracker`, unpacked into the scratch folder: the files
  `git show HEAD:<path>` gives, every header the runner includes among
  them.
- **The builds** are beat_eval.py's `cmd_build` command on Windows, once
  per root. Both must succeed, and the new log may hold no warning the
  base log lacks (their paths aside).
- **The cases** are what `beat_eval.py run` would run, from the cached
  inputs (read only): its corpus suite (every `pcm/NN.s16`), the same with
  `--via-hops`, its mid suite (`--start 60 --seconds 60` on each file of
  at least 120 s), its joins suite (every `joins/*.s16`, `--lead-in 0`)
  and its clicks suite (CLICK_CASES and DRUM_CASES). With the cache as it
  stands that is 352 cases: 77, 77, 74, 71 and 53. The list is read from
  beat_eval.py itself, imported with a stub numpy (numpy is used only to
  decode). `--bench` and `--time-only` print only timings and are left
  out.
- **The comparison:** every run exits 0, and each pair of outputs is equal
  byte for byte once the `end` line's ` ns_per_audio_s=<n>` (the timed
  second pass) is deleted.
- **test_beat_tracker:** `pio test -e native -f test_beat_tracker`
  passes, and `git diff HEAD -- test/test_beat_tracker` adds E.4's test
  and changes no other line.

From Git Bash at the worktree's root (S is this session's scratch
`step3` folder; W the cached beat_eval work folder):

```sh
S=C:/Users/paul/AppData/Local/Temp/claude/C--Users-paul-Documents-code-mstream-mp3-player--claude-worktrees-strange-roentgen-d6cb19/b66d988e-630b-443d-b111-58861852acd0/scratchpad/step3
W=C:/Users/paul/AppData/Local/Temp/claude/C--Users-paul-Documents-code-mstream-mp3-player--claude-worktrees-funny-almeida-627da3/d55d7b19-2e5c-414d-9df1-f7ee14d47a08/scratchpad/beat_eval
rm -rf "$S/base" "$S/beat_eval" && mkdir -p "$S/base" "$S/beat_eval/base" "$S/beat_eval/new"
git archive HEAD lib/core tools/beat_eval test/test_beat_tracker | tar -x -C "$S/base"
for side in base new; do
  root=.; [ $side = base ] && root="$S/base"
  g++ -static -std=gnu++17 -O2 -Wall -Wextra -I "$root/lib/core" -I "$root/test/test_beat_tracker" \
    "$root/tools/beat_eval/runner.cpp" "$root/lib/core/BeatTracker.cpp" "$root/lib/core/HopFrontEnd.cpp" \
    "$root/lib/core/ClickGen.cpp" "$root/lib/core/DancePose.cpp" -o "$S/beat_eval/runner_$side.exe" \
    2> "$S/beat_eval/build_$side.txt" || echo "BUILD FAILED: $side"
done
python - "$W" <<'EOF' | tr -d '\r' > "$S/beat_eval/cases.txt"
import os, sys, types
sys.modules["numpy"] = types.ModuleType("numpy")   # beat_eval.py needs numpy only to decode
sys.path.insert(0, "tools/beat_eval")
import beat_eval as be
w = sys.argv[1]
for f in sorted(os.listdir(f"{w}/pcm")):
    if f.endswith(".s16"):
        p, t = f"{w}/pcm/{f}", f[:-4]
        print(f"corpus_{t}", "--pcm", p)
        print(f"hops_{t}", "--pcm", p, "--via-hops")
        if os.path.getsize(p) // 2 >= (be.MID_START_S + be.MID_SECONDS) * be.RATE:
            print(f"mid_{t}", "--pcm", p, "--start", str(be.MID_START_S), "--seconds", str(be.MID_SECONDS))
for f in sorted(os.listdir(f"{w}/joins")):
    if f.endswith(".s16"):
        print(f"join_{f[:-4]}", "--pcm", f"{w}/joins/{f}", "--lead-in", "0")
for name, spec, extra, _ in be.CLICK_CASES + be.DRUM_CASES:
    print(f"click_{name}", "--synth", spec, *extra)
EOF
while read -r name args; do
  for side in base new; do
    "$S/beat_eval/runner_$side.exe" $args > "$S/beat_eval/$side/$name.txt" || echo "RUN FAILED: $side $name"
  done
done < "$S/beat_eval/cases.txt"
n=0; bad=0
for f in "$S/beat_eval/base/"*.txt; do
  n=$((n + 1))
  cmp -s <(sed -E 's/ ns_per_audio_s=[0-9]+$//' "$f") \
         <(sed -E 's/ ns_per_audio_s=[0-9]+$//' "$S/beat_eval/new/${f##*/}") || { bad=$((bad + 1)); echo "DIFFERS: ${f##*/}"; }
done
echo "$n cases, $bad different"
```

It passes with no `FAILED` line and `352 cases, 0 different`. (Note,
step 3's implementation: the `tr -d '\r'` is needed on Windows, where
Python prints CRLF, and the `\r` left on each line's last argument made
every corpus and hops run fail on both sides, 154 of the 352, which an
empty output then compared equal.)

### E.6 Step 3 is accepted when

- every native suite passes (`pio test -e native` from Git Bash, Unity
  from .pio/libdeps/native, no download), the three new ones included;
- E.4's Docker command prints ASAN-ALL-OK for the five dance suites;
- the shared vectors agree with Python: EligibilityVectors
  (test_dance_choreo), BudgetVectors' cases, budgets and traces
  (test_dance_budget, test_drawn_character), and ReaderVectors in full, its
  full lists
  included, through the real hook and pass (test_dance_pack);
  `python -m unittest discover -s tools -p "test_dancepack.py"` and
  `python tools/dancepack.py --check` pass unchanged;
- E.5 finds every case equal;
- `pio run -e core2` (PowerShell, MSYSTEM removed) builds, with IRAM
  unchanged and flash and DRAM within a few bytes of step 2's (the
  tracker's counter and its increment: nothing else links).
