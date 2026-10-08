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
(section 12). Nothing is built.** Section 11 is the build order. The
contributor guide, docs/DANCERS.md, comes with step 11.

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
three by construction.

```cpp
// lib/core/DanceCharacter.h (portable)
namespace dance {
enum class MoveKind : uint8_t { Beat, Idle };
enum class Origin : uint8_t { BuiltIn, Card };
enum : uint8_t { kWhenQuiet = 1, kWhenListening = 2 };   // Idle moves; 3 = any
enum : uint8_t { kMirror = 1, kFreeze = 2, kPlaceStart = 4, kPlaceEnd = 8, kHeavy = 16 };
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
  uint8_t fromTag, toTag;          // pose keys ("stand", "low", "left"...), 0 = none
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

### 1.2 Drawn characters

`DrawnCharacter` (lib/core) is a zero-copy view over a parsed pack: a
const array in flash, or bytes in the PSRAM arena (section 5.6). Its
`frameAt(cue)` returns `{frame, mirrored, dx, dy, shadowW}`, and it is
host-tested. `DrawnRenderer` (src/ui) blits that result.

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
- `row = n` must split evenly on the 1/12 grid. Otherwise it is an error:
  `5 frames can't share 1 beat evenly on the 1/12 grid: give timing =`.

### 2.2 How frames map to beat phase

- **Position.** u is the position in move beats. It is accumulated from
  the tracker's beat deltas (section 3.5), never from frame counts, so it
  holds at 10, 24 and 30 fps.
- **Which step shows.** Step i shows while `start_i ≤ 12u < start_{i+1}`.
  Step 0 starts at u = 0, the hit.
- **Must-show rule.** A step that starts on a whole beat is never skipped.
  If the last render was before beat b, and the current u is past that
  step but within ⅛ beat of b, the step is shown anyway.
- **Hit warning.** A step that straddles a whole beat gets a compiler
  warning ("the hit on beat 2 falls inside B4"). It isn't dropped, because
  holds are legitimate.

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
  - k = +1 is double time, allowed only while 2T ≤ 180.
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
- If a running move's pace T·2^k leaves [0.97·lo, 1.03·hi], it holds at
  its next whole move beat and the picker chooses again. The cut move is
  exempt from no-repeat, so it can carry on at the new octave if one fits.
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
- **During the fade.** Idle frames also show whenever w ≤ 0.5, including
  the ~0.6 s fade-in after a fresh lock.
- **Missing idles.** If a pack has no idle move for the current state, the
  other state's idles are used. If it has none at all, an engine
  **auto-idle** plays: the rest frame with an engine sway of ±1 native px
  over 4 s.

### 2.7 Genre: a filter

- **Tags.** A move may carry up to 8 free-text tags
  (`genre = disco, funk`).
- **Folding.** The compiler and the device fold tags the same way.
  - `&`, `+` and a lone `n` between words become "and". Then textfold's
    Full folding keeps letters and digits only, lowercased. The key is
    FNV-1a 32.
  - A tag that folds to empty (a script Full folding can't spell; see
    `sameName()`'s note in TextFold.h) is an error in the compiler and
    drops the move on the device.
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
  "spin": frames last 15 quarter beats; a 4-beat move needs 16 (skipped)`.
  `--strict`, used for built-ins, turns a skip into a failure.
- **Device:** logged once per load, e.g. `[dance] robo: move "spin"
  ignored: 15 of 16 quarters`.
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
Cue       {beatMove, u, k, beatMirror, idleMove, idleMs, idleMirror,
           idleSeconds, weight, energy, showDance, flash, spot, rootX,
           rootY, state}
```

The only tracker change is a new `BeatTracker::acquisitions()` counter,
incremented in `BeatTracker::acquire()`, the one function that acquires a
grid.

**Determinism.** Identical `BeatInput` streams give identical cues on the
host and on the device. Two device runs differ from each other, because
their inputs depend on frame times, latency and epochs.

### 3.2 States

| State | Entered when | Behaviour |
|---|---|---|
| Quiet / Listening | section 2.6 | Idle moves at the current spot, picked by weight for the state, in runs of 1-3 loops with no immediate repeat. |
| Entering | w > 0.5 and a locked grid | Keeps idling until the next whole dance beat (≤ 1 beat, ≤ 833 ms), anchors the phrase there (origin A), then picks. The dancer comes in on the beat. |
| Dancing | | Picks a new run each time the current run ends. |
| Leaving | lock lost, grid and audio still valid | Plays on to the next whole move beat and holds that step. Idle frames take over at w ≤ 0.5. |
| Freeze | `!heard.valid` (pause, starve) | Holds the step, the offset and the travel position at once. Quiet idles take over at w ≤ 0.5 (~0.3 s). |

**Which frames show.** Beat-move frames show when w > 0.5 **and** the
state is Dancing, Leaving, or Freeze holding a beat step. Otherwise idle
frames show.

- During Entering (w > 0.5, idle frames, ≤ 1 dance beat), DanceRate and
  beat_eval still count "dancing" by w > 0.5, so their metrics don't
  change.
- The beat light flashes on dance beats as today. During Entering it leads
  the dancer by at most one beat, which shows the beat has been found.

### 3.3 Timing and the phrase

- A run starts exactly where the previous one ended, at a whole dance
  beat, even if the first drawn frame shows u ≈ 0.1. Any overshoot carries
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
   set is empty, the last move may repeat.
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
move's pace therefore stays T·2^k whatever the tracker's octave.

| Event | Detected by | Handling |
|---|---|---|
| PLL nudge (≤ 0.07 beat) | same generation | passes through |
| Re-acquire or jump | a new `acquisitions()`, or Δ < −0.1 or > 1.5 tracker beats | (1) Recompute M from the new S. (2) Compare in **dance beats**: the beat index restarts arbitrarily, so the new grid's dance-beat origin is chosen among the 1/S whole-tracker-beat shifts (when S < 1) as the one nearest the predicted dance position. (3) Within ¼ dance beat: the move keeps its whole-beat count and takes the new phase. Otherwise it holds its step until the new grid's next dance beat, re-anchors A there, and picks afresh. (4) If T moved, re-test the running move's pace (section 2.5). |
| Fold change (S ×2 or ×½) | the TempoFold scale changes, only on a re-acquire at a new tempo (the PLL's ±4 % clamp keeps drift inside 72-176) | Covered by the row above. With M recomputed, an octave flip (I Wonder, 95.7 ↔ 191) leaves T and the pace unchanged, and a 4:3 or 3:2 re-acquire triggers the pace re-test. |
| Lost lock | `!locked`, heard valid | Leaving. If the lock returns while w > 0.5, the move continues from the next dance beat, re-phased. Otherwise the dancer goes through Entering. |
| Grid dropped, audio valid | grid invalid | Leaving, then Listening idles (the "no grid" case). |
| Pause / resume | heard invalid / valid | Freeze. On resume the same grid applies at once, so Entering starts on the next beat and the phrase continues from A. |
| Track change with a restart (skip, seek, start, output switch, host epoch) | a tracker restart | `reset(seed)`: history, latch, phrase and beat move are cleared. The idle clock and the spot carry on, so nothing jumps. |
| Gapless join | `DanceMode::onTrackChanged()` with no restart (a join keeps the epoch, so the lock carries through it) | `onTrack(seed, genres)`: the new seed and the track's genres apply, and `Dg` is cleared. Phrase, latch and current move are kept. |

### 3.6 Weight and energy

- **The weight is unchanged:** `danceWeight(confidence)` while there is a
  locked beat, otherwise 0, eased with τ = 0.4 s. DanceRate (fps), the
  beat light (dance phi < 0.12 while dancing), test_dance_rate and
  beat_eval's `IDLE_WEIGHT` 0.5 keep their meaning.
- **Offsets, bounce, sway and travel** scale by the energy e = 0.6 +
  0.4·clamp((w − 0.5)/0.5), so a fresh lock (w 0.65) hops at 72 %. Travel
  always completes its full distance; e scales only the hop.
- **Palette:** the 32-step fade between the idle and dance palettes comes
  for free through the LUT (section 6.2).

### 3.7 Switching characters

- A switch takes effect at once while idle. While dancing it waits for the
  next whole dance beat (≤ 1 beat), and the new character starts a fresh
  pick there, at the current spot.
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

It lands in step 4, with the Choreographer. `D` is free today.

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
- **Accepted PNGs:** colour types 0, 2, 3, 4 and 6; bit depths 1-8 for
  indexed and grey; 8 or 16 (16 reduced to 8) for RGB and RGBA.
  - **tRNS** is honoured for types 0, 2 and 3.
  - Interlaced (Adam7) files are rejected with "save without interlacing".
- **Colour key.** `key = #rrggbb` makes that colour transparent. With no
  alpha, no tRNS and no key, the build fails with `every cell is fully
  opaque: export with transparency or set key =`. Otherwise every bounding
  box would be the whole cell and the budget red.
- **Export recipes**, in the guide:
  - Aseprite / LibreSprite: Export Sprite Sheet, By Rows, no trim.
  - Piskel: PNG with columns.
  - Pixelorama: Rows.
  - Paint: a magenta background and `key = #ff00ff`.
- Identical cells, and cells that are mirror images of each other, are
  stored once. A mirror bit in the step record handles the second.
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
- **Too many colours:** an error that lists the colours by pixel count,
  and the cells where the rare ones sit (usually stray pixels).
- **Anti-aliasing** is allowed only as opaque colours pre-blended against
  #080C10 (0x0862).
- **Idle palette:** derived automatically, each colour taken 50 % toward
  its luma and then ×0.7 (estimate, to tune by eye against the crab's).
  `idle_palette = …` overrides it, which pre-blended ramps may need.

### 4.4 Scale, size and anchor

- `scale` is 1-4. A cell times its scale must be at most 160×160
  screen px, for example 80×80 native at 2×.
- `anchor` is the ground point in native px. The default is the bottom
  centre. The feet's last drawn row is anchor y − 1.
- The cell must fit the stage: anchor y × scale ≤ 153, and at most 10
  screen px drawn below the anchor.
- The compiler warns when the feet drift more than 1 native px within a
  move that stays in place, unless offsets account for it.
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
  reads correctly. A BOM and CRLF are accepted. Keys are case-insensitive,
  with `=` or `:`.
- **`bpm` is required on every move:** a range, `any`, or `none` (a no-BPM
  move). A typo can't silently turn a range into "any", because a move
  without `bpm` is an error.
- **Unknown keys:** an **error** when the key is an adjacent transposition
  of a known key (`bmp` → `bpm`), or when difflib's ratio is ≥ 0.6 ("did
  you mean bpm?"); `bmp` against `bpm` scores 0.667, which a 0.75 cutoff
  would miss. Any other unknown key is a warning, so newer manifests still
  build. Keys starting with `x-` are ignored silently.

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
frames = A12 B12
bpm    = none                   ; a no-BPM move: plays when there is no beat to follow
timing = 2s 2s
offset = -2,0 2,0
ease   = inout
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
  lists are easy to see. A list of the wrong length is an error.
- **Licence and credits.** `license` and `author` are required; `url` and
  `version` are optional. All of them show in the Dancers page.
- **Text the device can show.** The name and the author must be valid
  UTF-8 with no control characters. The compiler warns about characters
  outside the device fonts' coverage: dancepack.py embeds the covered
  ranges, generated by a new `vlw_font.py --ranges` flag. On the device
  M5GFX draws a missing glyph as a blank (the note in `Fonts::widthOf()`),
  and every name is ellipsized by the usual fit rules.
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
pack.

- **META:** id ≤ 23 chars `[a-z0-9-]`, name ≤ 24, author ≤ 40, licence
  ≤ 40, url ≤ 80, version ≤ 16 (each a u8 length + UTF-8), and a u8
  **declared move count** (unverified until a load).
- **GEOM:** scale, cellW, cellH, anchorX, anchorY, shadowW, rest frame,
  frame count, faces (0 none, 1 right, 2 left).
- **PALS:** 16 × RGB888 for dancing, then 16 for idle.
- **THMB:** a 40×40 4-bit thumbnail (800 B) in the dance palette.
- **FTAB:** 12 B per frame: the opaque bounding box x0, y0, w, h (u8,
  native); a u32 data offset; a u16 length; a u16 reserved.
- **FDAT:** per bounding-box row: `skip` (u8, leading transparent px), `n`
  (u8), then n tokens `(len−1)<<4 | index` (1-16 px each; index 0 is an
  interior gap).
- **STRS:** move names, pose keys and genre tags (u8 length + UTF-8,
  ≤ 63 B).
- **MOVS:** a u8 count, a u8 record length (≥ 32, so records can grow; the
  reader takes the first 32 B). Each record holds the `MoveLabels` fields
  plus bounce, sway, travelEase, the first genre and the genre count, and
  the first step and the step count.
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
and card. `avail` is the number of bytes actually present; it equals size
for a full load. All checks run at load, never per frame:

- the magic, `minReader` ≤ 1, the size equal to the file length, and the
  CRC (skipped only by a test-only option, section 10.3);
- each chunk in bounds, aligned and not overlapping another, with no
  duplicate tags, and counts within the caps (256 frames, 64 moves, 32
  steps per move, 16 chunks);
- the geometry limits of section 4.4;
- each bounding box inside its cell;
- **a dry-run decode of every frame**: each row's skip + Σlen ≤ w, and the
  frame's bytes consumed exactly (~1 ms for 60 frames, estimate);
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
- `Pack::scan(buf, avail = bytes read, size = file size)`:
  - the header checks;
  - META, GEOM, PALS and THMB must each satisfy off + size ≤ avail, or the
    row reads "header unreadable";
  - scan-time strings get the same UTF-8, control-character and length
    checks as at load;
- then a roster entry is `psramAlloc`'d: id, name, author, licence, file
  size, declared move count, the 800 B thumbnail, and a status.

### 5.6 The arena

- After the scan, **one double-ended PSRAM arena** is allocated, sized to
  the two largest header-valid packs (at most 256 KB; typically about
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
  - After the last slice come the CRC and the full validation. The budget
    simulation runs one move per frame, ≤ 3 ms each (estimate). Then the
    swap happens at the next dance beat. The rail shows "Loading Robo…"
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
`{x, y, w, h, groundY, spotX[3]}`:

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

- Root x = spotX + travel(u) + round(scale·e·(eased offset + bounce +
  sway)), with offsets negated when mirrored.
- travel(u) eases from the start spot to the end spot over the `go` steps
  (the whole move by default), with `travelEase`.
- Root y is computed the same way, from the ground.
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

### 6.7 The per-move budget: one integer simulation

- `lib/core/DanceBudget.{h,cpp}` loops each move at a fixed pace and
  energy 1 through `frameAt` and the dirty-rect rules. It runs at 24 and
  30 fps, at both ends and the middle of the move's reachable pace range,
  and reports the median and peak dirty px.
- It works on bounding boxes only (no pixels), in Q16 fixed point.
- dancepack.py mirrors it, and **shared vectors**
  (`test/fixtures/dance/budget_vectors.txt`: move, fps, pace → median,
  peak) are pinned in both test suites.
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
- **.bss:** the Choreographer (history, latch, RNG, cue: ~240 B, estimate)
  and the cast pointers live in the global DanceMode, budgeted at ≤ 1 KB
  and measured.
- **IRAM: 0 B.** No `IRAM_ATTR`; 5,176 B remain.
- **PSRAM:** the stage (105 KB), plus the arena (0 B with no card packs,
  otherwise typically ~60 KB and at most 256 KB), plus the roster
  (≤ 14 KB), plus one 3,200 B thumbnail scratch. Built-in packs need none.
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
  persists across moves, idles, character switches and track changes.
- **A travel move is eligible when:**
  - its destination spot exists;
  - the move's widest frame, with its offsets, fits on the stage there;
  - D ≥ 2;
  - it starts at p ∈ {0, 4};
  - its `start_at` matches.
- **Travel bias:** moves heading toward the centre get ×2. After a track
  restart, the first run gives them ×4.
- Idles play at the current spot. There is no "walk home", which would
  look like skating without walk frames.
- `stageX(from, to, u, ease)` is in lib/core and tested. Two-rect dirty
  pushes are live (section 6.3).
- Non-travel moves at an edge spot are clamped (section 6.4). The
  compiler's coverage report flags moves whose widest frame doesn't fit at
  an edge spot, because those are then eligible only at the centre.
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
| sway | any | 4.0 s | 2 (2), 2 keys | today's idle; offsets ±2, inout, exactly as in the section 4.5 example |
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
    `bpm` → an error; `license = X ; comment` reads as X;
    `palette = #ff0000 #00ff00` reads both colours; duplicate keys → an
    error;
  - empty-folding tags, uneven `row =` splits, and the bounds on bounce,
    sway, repeat and travel;
  - a byte-identical rebuild;
  - the shared eligibility cases (T, lo, hi, previous k → k) and the
    shared budget vectors, both also pinned in C++.

### 10.2 CI

The firmware workflow (`.github/workflows/firmware.yml`) gains three steps
before the host tests: `python tools/crab_art.py --check`,
`python tools/dancepack.py --check`, and `python -m unittest discover -s
tools -p "test_dancepack.py"`. All are integer-deterministic and need no
pip install.

### 10.3 Host tests

- **`test_dance_pack`:**
  - parses the fixture pack (`test/fixtures/dance/TestPack.cpp`, compiled
    from `tools/art/dancers/test/`, which uses every field);
  - one hand-built bad pack per rejection reason, duplicate chunk tags
    included; the CRC; unknown chunks skipped; a `needs` bit skipping a
    move;
  - **scan mode:** META, GEOM, PALS or THMB pointing past `avail` →
    "header unreadable";
  - **fuzzing that reaches the validators:**
    - every truncation, and 10,000 seeded single-byte flips, with **the
      size and the CRC recomputed after each mutation**;
    - a structure-aware mutator that edits directory offsets and sizes,
      FTAB boxes, offsets and lengths, FDAT token bytes, MOVS step indices
      and counts, STEP frame indices, and STRS lengths;
    - all through a bounds-asserting accessor, with a test-only `kSkipCrc`
      option on `Pack::open`.
  - It also runs in a CI-only `native_asan` env on Linux g++, which needs
    `-fsanitize=address,undefined` at compile and link (estimate). MinGW
    on the laptop has no ASan.
- **`test_dance_blit`:** frame hashes against the fixture's table;
  mirroring; scales 1-4; edge clipping with a canary border.
- **`test_dance_choreo`:**
  - **pace:** eligibility at k ∈ {previous ranged, 0, −1, +1}; unranged
    moves always at k = 0; the 180 cap; half time only for ≤ 4-beat moves;
    the 3 % latch;
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
  - **weight:** a change ≤ 1/12 per frame, and > 0.95 after 2 s; the
    energy mapping;
  - **switching** at the next dance beat; the freeze fallback chain.
- **`test_drawn_character`:** step lookup at exact tick edges (integer);
  Q16 ease values at 0, ¼, ½, ¾ and 1; mirror symmetry; rounding half up;
  the bounds holding every pixel; `stageX`.
- **`test_dance_budget`:** the shared vectors; the peak never under any
  single frame's own box.
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
  - `test_beat_tracker`: `acquisitions()` counts re-acquires made while
    locked;
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
visualizer's touches. Every step that adds rodata (3, 4, 7 and 8) also
needs cache_guard passing and the `b<n>` bench within noise of 0.7.0.

| # | Commit | Contents | Accepted when |
|---|---|---|---|
| 0 | Dance sprites: the design | this document, with the MPDN v1 spec | the user's decisions recorded (section 12) |
| 1 | Dance sprites 1: the compiler | dancepack.py (PNG with tRNS and key, the INI rules, validation, the MPDN writer, build, check, info, builtin, `--check`, `--strict`, the budget mirror and the coverage report), test_dancepack.py, the shared eligibility and budget vectors, the test dancer → the TestPack fixture, the CI steps | unittest green on 3.10 and 3.12; byte-identical output on Windows and in CI |
| 2 | 2: the reader and blitter | DancePack (with scan mode), DanceBlit, Crc32; test_dance_pack (with the structure-aware fuzz), test_dance_blit; the `native_asan` env | the fuzz clean under ASan; the hashes equal the fixture's table |
| 3 | 3: the choreographer | DanceCharacter.h, DanceChoreo (the pace rules, M recomputed, spots), DrawnCharacter, DanceBudget, GenreFold (folding only), `acquisitions()`; test_dance_choreo, test_drawn_character, test_dance_budget | all suites green; the shared vectors agree with Python; beat_eval builds and scores identically |
| 4 | 4: characters, the cast and the console | CrabCharacter, ClassicStick (its id still `stick`), DanceCast replacing DanceSkin, DanceMode on the Choreographer with today's view, `onTrack` for gapless joins, **the `D…` console** (D, Dc, Dl, Dm, Df, Dv, Dg, Ds), Df cleared by host mode; test_character_set | on the device: the crab's k0-k15 sheet pixel-identical to 0.7.0; fps, logs and `usb_viz --measure` unchanged |
| 5 | 5: the stage | DanceStage.h, the DanceView rewrite (one RGB565 canvas, the renderers, direct blit, holds ≤ 40 rows and ≤ 8,000 px bottom-up, `enterClean`, returned rows as a safety net) **at the legacy rect, with the beat dot still in the box**; test_dance_stage | **the crab's k0-k15 sheet pixel-identical to 0.7.0 over the whole 120×150 window**; draw/push ±10 %; holds ≤ 6/10 ms; the classic figure measured in RGB565; overlays; soak |
| 6 | 6: the Dance tab layout | the final stage geometry, the rail with no tap targets, the beat light in the rail (hidden in host mode), tap and flick, the host-mode rule, `x`/`X`, the "No PSRAM" message on the stage, the UiText rooms and tests | **the crab layer identical to step 5**, masking the old dot's square (box x 102-117, y 2-17, which `DanceView::drawDot()` clears around the dot at (110,10)) and the ground line's new pixels (screen x 100-103 and 216-219 on rows 189-190); host checks; fit tests; fps. Meanwhile a hold isn't claimed yet, so it lifts as a tap |
| 7 | 7: the stick figure set | the runner, stick_frames.py (raster + retouch), the art, StickPack.cpp, test_stick_pack, the contact sheet; the classic figure becomes `stick-classic` (reachable with `Dc`) | the fps gates; draw ≤ 6.1 ms with an MP3; budget ≤ 16K; soak; the flash delta; the user's visual review of each move through `Dm`/`Df` |
| 8 | 8: travel across three spots | the spots in DanceStage, `stageX`, travel eligibility and bias, two-rect pushes exercised, `x` following the spot, the coverage report's edge-fit column | grapevine and chasse travel on the device; dirty px and holds within the gates while travelling; no stale pixels at the edges; the crab unaffected |
| 9 | 9: choosing dancers | the NVS schema bump (its number taken at merge), the Dancers page with thumbnails, `Page::leave(LeaveWhy)` + `setDrawing`, `showDance()` popping the Dance tab to its root, hold → page, the optional tab re-tap (decision 11) | the choice survives a reboot; test_nvs_layout; a hold gives the double tick; **hold → pick → back keeps the lock**; host mode pops the page; ENERGY item 9 re-measured |
| 10 | 10: dancers from the card | the boot scan (with `avail`), the double-ended arena, the sliced loader (a reopen per slice, pause and resume), the device's own budget, failures, duplicates, overrides after load, greyed rows | 0 underruns with MP3 and 24/96 FLAC; loads during a queue save and a thumbnails job; a card pull; the corrupt set; the NVS card restore; the decoder addresses and the 10k-library bench |
| 11 | Dance sprites: the contributor kit and docs | `new`, `preview --watch`, the template and guide, DANCERS.md, the doc updates | the template builds and previews; then a dry run in which the user builds a dancer alone and loads it from the card (it doesn't block other steps) |
| 12 | Dance sprites: review fixes | | |

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
