# Beat tracker: evaluation harness, baseline and rework

The dancing crab follows `BeatTracker` (lib/core; design in
[MASCOT-POC.md](MASCOT-POC.md#beat-tracker-beattracker)). Until October
2026 it was tuned on click tracks and spot-checked on a few songs. This
document describes a harness that scores the firmware's own tracker on the
user's 77-track library, the baseline it measured (tracker as of 2742f3d),
and the [rework](#the-rework-october-2026) made against it. Further
changes to the tracker are judged against the rework's numbers.

In short: the baseline tracker was exact on click tracks and good on
French house, but across the 69 tracks with a beat it was locked on the
beat for well under half of each track, and its confidence told a right
grid from a wrong one no better than chance. The rework changed what the
confidence measures (how the grid's phase dominates the others, and
whether its beats are hit), let the mid band count in full, fixed a bias
that had every noise "acquired" at 185 BPM, and, after a review, locks only
on two confident beats in a row and dances only while locked. On the 69
beat tracks the beat F-measure went from 0.40 to 0.49 (0.48 to 0.56 on a
monitored third of the tracks), the wrong locked beats from 16 to 13.5 %
and the false-lock episodes from 199 to 141, with the click tracks
unchanged. Opened mid-song and across gapless joins it is not yet as clean
as the baseline on every count (a little more false share mid-song, more
episodes at the joins), and four tracks got worse where the off-beat is as
strong as the beat. What is left is in the music the tracker's two bands
can't resolve: off-beats as strong as the beat, syncopated kicks, 4:3
metres, and sparse beats under sustained sound.

## Method

```
 corpus audio ──► decode (devmad.exe: the device's libmad path; libsndfile for FLAC)
                  ──► the firmware's gapless trim ──► mono (L + R) >> 1, as AudioTap mixes it
                  ──► PCM cache (scratch folder)
 runner (host g++, lib/core's BeatTracker + HopFrontEnd + ClickGen, unchanged)
   reset(track frame) ─► process() one hop at a time ─► every grid beat as it is reached,
   bpm / estimate / clarity / confidence / lock every 8 hops, framesToLock(), CPU
 score.py ─► beats.json (shifted to the same timeline) ─► score.json + report.md
```

### The pieces (`tools/beat_eval/`)

- **`runner.cpp`**: compiled with the host g++ (`-O2`, static on Windows)
  from the firmware's own `BeatTracker.cpp`, `HopFrontEnd.cpp` and
  `ClickGen.cpp`, unchanged.
  - **Feeding:** it does what `DanceMode` does. `reset()` to the track
    frame, no prior (the firmware sets one only from the console, or the
    computer's in host mode), then
    `process()` on mono int16. It feeds one 512-frame hop at a time, so every
    state the grid passes through is seen. The chunk size doesn't change the
    result (`test_block_size_does_not_matter`).
  - **Grid beats:** each beat is written when the audio reaches it, at the
    time the grid predicted for it. The PLL sets that time at the previous
    beat's close, about 0.8 beat ahead. The state written with it (lock,
    confidence, BPM) is the tracker's at that hop. A pending beat that a
    re-acquire replaces before its time is never shown, so it isn't written.
  - **`--via-hops`:** feeds the same audio through a separate `HopFrontEnd`
    and `feedHop()`, the USB visualizer's path. The output is identical to
    `process()`'s on all 275 cases.
  - **Other modes:** `--synth` builds synthetic cases (`click:BPM[off]:S`,
    `silence:S`, `noise:DBFS:S`, `ambient:DBFS:S[:SEED]`, the native
    tests' low-passed swelling noise, and
    `drums:BPM:S:PHASE:HAT_DB[:BASS_DB[:GHOST_DB[:FIRST]]]`, the native
    tests' drum pattern from their `Signals.h`, joined with commas).
    `--bench` and `--time-only` are the CPU measures. `--hops-out` dumps
    the front end's energies.
  - **The tracker's factors and the dancer:** each `T` line also carries
    `BeatTracker::factors()` (dominance, hit rate, pulse, and the PLL's RMS
    correction), so a run shows why the confidence was what it was, and
    the dance weight the figure would be shown (`dance::danceWeight` while
    locked, as DanceMode does).
- **`beat_eval.py`**: the driver, with the commands `build`, `decode`, `run`,
  `score`, `compare` and `all`.
  - **Decoding:** MP3s go through the corpus's `maddec/devmad.exe`, a PC
    build of ESP8266Audio's libmad behind the device's input code. FLACs go
    through libsndfile, as the corpus scripts did. The decoded sample counts
    equal beats.json's for every file.
  - **The device's timeline:** devmad keeps the Info frame and both delays.
    The firmware's gapless trim (GAPLESS.md 4.2-4.3) never decodes the header
    frame and skips `delay + 529` for a LAME tag. So the cache drops those
    2,257 frames from the 35 tagged MP3s and none from the two untagged Air
    MP3s or the FLACs. The reference beats are shifted by the same amount.
  - **Speed:** the cache is about 1.7 GB in the work folder, and decoding
    takes about 20 s. A full run (275 cases) takes about 15 s, and scoring
    about 10 s.
- **`score.py`**: the scorer, below. **`diagnose.py`** explains one track.
  **`test_score.py`** checks the scorer against made-up tracker output with
  known answers.

```
python tools/beat_eval/beat_eval.py --corpus <groundtruth> --work <scratch> all --name mychange
python tools/beat_eval/beat_eval.py ... compare --name mychange --against baseline
python tools/beat_eval/diagnose.py --corpus <groundtruth> --work <scratch> --name mychange 61 16
python -m unittest discover -s tools/beat_eval -p "test_*.py"
```

(`BEAT_EVAL_CORPUS` and `BEAT_EVAL_WORK` replace the two paths. Python needs
numpy, and soundfile for the FLACs; the corpus's own venv has both. Nothing
is written into the repository, and the corpus audio, which is the user's
music, stays out of it.)

### Suites

| Suite | What | Cases |
|---|---|---|
| corpus | every track from its start to its end, as the Dance tab sees a track it was open for | 77 |
| mid | from 60 s into the track, for 60 s: the Dance tab opened mid-song (a fresh reset) | 74 |
| joins | the last 30 s of a track, then the first 60 s of the next on the album, with no reset: a gapless join keeps the epoch, so the firmware doesn't restart the tracker. These are real tempo changes | 71 |
| clicks | the six built-in click tracks (exact beats), three with noise (−30/−20 dBFS), three tempo changes, intro and outro of silence or noise, an 8 s break with a phase jump, and noise or silence only (must not lock) | 17 |
| drums (run with the clicks) | the native tests' heavy off-beat drum patterns: a kick on every beat, a bass note and a ghost kick 4-6 dB under it at 0.5, 0.66 or 0.75 of the beat, a hat on top, at 96, 124 and 150 BPM. The report says where each locks: on the kick, on the lead-in (the notes before the next kick), elsewhere, or never | 36 |

`--prior ref` runs everything with the reference tempo as a metadata prior
(`setPrior`), as mStream's BPM would give it.

### The reference, and why it is used carefully

`beats.json` was made by librosa (onset strength on a mel spectrogram, a
tempo-family search, then constant-tempo grids fitted to librosa's beats).
It is not human truth:

- **The octave is a convention.** Every tempo is folded into [72, 150) BPM.
  So each tempo metric has an octave-tolerant twin, and the tracker's beats
  are matched at its own metrical level: at double tempo the reference's
  half-beats count as beats.
- **The phase and the metre can be wrong.** 43 of the 77 tracks carry at
  least one **reference flag** in the report (35 of the 69 beat tracks):
  - local tempo spread over 5 % (24);
  - plain librosa picked a 4:3 or 3:2 metre (15);
  - the corpus's own beat/off-beat decision was ambiguous (12);
  - another metre scored within 15 % of the chosen one (8);
  - more onset energy on the half-beat than on the beat (8);
  - attacks more than 10 ms from the beats (7);
  - low pulse clarity (7);
  - no constant grid (3).

  The scorer adds two **tracker flags**, shown next to them: the tracker
  locked confidently at a 4:3, 3:4 or 3:2 relation (Music Makers, Waxin,
  Deadcruiser), or on the off-beat (Kelly Watch the Stars, Nightvision),
  for most of its beats. For Waxin and Deadcruiser, the reference's own
  runner-up tempo was the tracker's (it scored 0.92-0.93 of the winner).
  For Music Makers, plain librosa said 138, as the tracker does. The
  summary has a row for the beat tracks with a **clean reference**: no
  reference flag (the tracker's flags don't count, so it is the same 35
  tracks for every run compared).
- **Tags:** none of the 77 files carries a usable BPM tag. The Kavinsky FLACs
  have `BPM=0`; the others have no field. So mStream's BPM can't cross-check
  the octave here. The harness reads TBPM, BPM or TEMPO when a file has one
  and flags a disagreement. The five published tempi in beats.json (One More
  Time, HBFS, Digital Love, Nightcall, Stronger) all agree with the
  reference.
- **Exact cases:** the click tracks are the sanity check. Their beats are
  ClickGen's, to the frame.

### Metrics

Per locked tracker beat inside the reference's span (its first to last beat,
± half a beat, split where the reference has a gap), at the local reference
tempo:

| Class | Meaning |
|---|---|
| on | within ±70 ms of a reference beat, at the reference tempo or an octave of it |
| offbeat | at the reference tempo or half of it, but on the half-beat (±70 ms) |
| off | octave-correct tempo, neither (in practice: a sixteenth-note phase, or drift) |
| wrong tempo | not within 4 % of 1×, 2× or ½× the reference (4:3, 3:2, ... are named) |
| no ref | locked where the reference has no beat (intros, outros, gaps) |

- **Beat F-measure (±70 ms):** the locked beats against all the reference
  beats, matched one-to-one. Unlocked time costs recall, so F is about
  "dancing on the beat" over the whole track. **F oct.** is the best of the
  reference at 1×, 2× and the two ½× phases.
- **Tempo:**
  - **exact**: the median locked BPM is within 4 % of the reference;
  - **oct.**: within 4 % of ½×, 1× or 2×.
  Both are taken per track, and also as shares of locked time in score.json.
- **Lock times:**
  - **lock** is `framesToLock()`, the first lock since the reset;
  - **on-beat lock** is the first of four consecutive locked beats that are
    all on. It is given from the track start and from the reference's first
    beat, and ≤10 s is the share of tracks that manage it within 10 s;
  - **never on** counts the tracks with no such run.
- **Phase error:** over the locked, octave-correct beats, the signed
  distance to the nearest reference beat (half-beats included at double
  tempo). The median and p95 of its magnitude are given, and the **bias** is
  its mean over the beats within ±70 ms (positive: the tracker is late).
- **False locks:**
  - **false beats** is the share of locked beats in the span that aren't on;
  - a **false episode** is a run of locked beats under half of which are on.
- **Intros and outros:** an intro is a first beat more than 2 s in, and the
  report counts the tracks locked more than 1 s before it. An outro is a last
  beat more than 2 s before the end, and the report gives how long the lock
  holds after it.
- **Breaks:** the reference's weak-pulse regions of 4 s or more. The report
  gives the time from a break's end to the next on beat.
- **Joins and tempo changes:** the time from the next track's first beat (or
  the change) to an on-beat lock, and the not-on beats in the 10 s after the
  change.
- **Tuning and monitored splits:** every third track by index (26 of 77,
  21 of them beat tracks) is the monitored split, the rest the tuning
  split; the summary has a row for each. Thresholds are chosen on the
  tuning rows, but the monitored ones are in every report, so they check
  for fitting rather than test independently.
- **Dancing (span):** the share of the span the figure is drawn dancing,
  its dance weight over DanceRate's 0.5 (only while locked).
- **`compare`** prints each summary row of two runs side by side, the drum
  patterns, the on-beat lock time paired track by track (median change,
  how many earlier and later), and every track whose F moved by more than
  0.1, with where its locked beats went.
- **By genre and album:** the genre comes from the album (the files' genre
  tags are missing or vague):

  | Album | Genre |
  |---|---|
  | Air | downtempo |
  | Selected Ambient Works 85-92 | ambient techno |
  | Discovery | french house |
  | Emancipator | downtempo |
  | Graduation | hip-hop |
  | OutRun | synthwave |

- **CPU:** the boot bench's case (click120, 60 s) and the first 60 s of five
  real tracks are each timed in 2048-frame chunks, serially, best of 5. That
  gives host µs per second of audio. The device measures 13.3 ms/s at
  240 MHz (`[dance] tracker bench`), about 84 times the host figure, so the
  ~2× budget is about 300 µs/s on this laptop. The device bench still has
  the last word.

### Known limits of the harness

- **Not bit-exact with the device.** x86 `float` arithmetic is IEEE single
  precision, like the ESP32's FPU, but libm's `log`/`exp` may differ from
  newlib's in the last ulp. The click tracks lock at the same times the
  device logged: click90/120/128/140/174/120off lock at 2.81/2.61/2.93/2.67/
  2.50/2.80 s here, against 2.83/2.62/2.93/2.67/2.51/2.81 s in MASCOT-POC.md's
  round 1. So the difference is below anything measured.
- **What it times.** The beats are those the tracker predicts as the audio
  reaches them. The dancer shows them ~115-175 ms later (output latency),
  only while the tracker is locked, with a weight from its confidence (0
  at 0.12, 1 at 0.5): the report's "dancing (span)".
- **The joins are a model.** They splice two cached PCM files. They don't
  replay the firmware's gapless join, though the samples are the same once
  trimmed.

## Baseline (2742f3d, no prior)

| | tracks | F (±70 ms) | F oct. | tempo exact | tempo oct. | locked (span) | lock, med | on-beat lock after 1st beat, med | on-beat ≤10 s | never on | phase med / p95 | bias | false beats | false episodes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **beat tracks (good+ok)** | 69 | **0.40** | 0.42 | 74 % | 86 % | 42 % | 4.4 s | 9.0 s | 41 % | 16 | 10.5 / 218 ms | +8.9 ms | 16 % | 199 |
| good | 36 | 0.50 | 0.52 | 86 % | 89 % | 48 % | 3.5 s | 13.9 s | 42 % | 5 | 9.7 / 165 ms | +9.2 ms | 12 % | 96 |
| ok | 33 | 0.29 | 0.31 | 61 % | 82 % | 34 % | 5.3 s | 8.3 s | 39 % | 11 | 12.0 / 237 ms | +8.3 ms | 23 % | 103 |
| hard | 6 | 0.04 | 0.04 | 17 % | 33 % | 3 % | 30.2 s | 30.1 s | 17 % | 4 | 13.3 / 37 ms | +5.2 ms | 27 % | 8 |
| no-clear-beat | 2 | 0.01 | 0.01 | 50 % | 50 % | 0 % | 160.9 s | - | 0 % | 2 | 27.7 / 50 ms | −10.1 ms | 0 % | 0 |
| all 77 | 77 | 0.36 | 0.38 | 69 % | 81 % | 38 % | 4.5 s | 9.0 s | 38 % | 22 | 10.5 / 218 ms | +8.9 ms | 16 % | 207 |
| beat tracks, clean reference | 35 | 0.49 | 0.50 | 83 % | 91 % | 48 % | 5.6 s | 6.8 s | 49 % | 3 | 9.3 / 171 ms | +9.4 ms | 12 % | 59 |
| mid-track start (60 s in) | 67 | 0.39 | 0.39 | 67 % | 76 % | 39 % | 7.7 s | 9.1 s | 36 % | 25 | 10.3 / 165 ms | +9.4 ms | 14 % | 40 |
| gapless joins | 71 | 0.36 | 0.37 | 72 % | 77 % | 35 % | 7.7 s | 9.9 s | 31 % | 27 | 10.1 / 230 ms | +6.4 ms | 18 % | 69 |

On the 69 beat tracks:

- **Precision and recall.** Precision is 0.69 and recall 0.34. While locked,
  most beats are right; the tracker just isn't locked for most of the music.
- **Locked beats** (13,184 in the reference spans):

  | Class | Beats | Share |
  |---|---|---|
  | on | 11,074 | 84 % |
  | off-beat | 714 | 5 % |
  | off | 952 | 7 % |
  | wrong tempo | 444 | 3 % |

  Of the wrong-tempo beats, 235 are at 4:3, 83 at 3:2, 66 at 3:4 and 26 at
  2:3. Another 655 locked beats fall where the reference has no beat.
- **Intros.** 6 of the 22 tracks whose first beat is more than 2 s in lock
  more than 1 s before it, 42 s in all.
- **Outros.** The lock holds a median 0.3 s past the last beat (68 tracks).
- **Breaks.** There are 46 weak-pulse regions inside beat spans. The median
  recovery after one is 7.8 s, and 19 of them are never followed by an on
  beat.
- **Joins.** 37 of the 71 relock on the next track within its first 60 s, a
  median 7.2 s after its first beat. Eight joins carry 4-12 wrong beats into
  the next track's first 10 s.

**With the reference tempo as a prior** (`--prior ref`), F is the same
(0.40). Tempo exact rises to 83 %, but octave-tolerant tempo falls to 83 %,
because the prior forces a level that the audio supports less well. False
beats fall to 13 %, and the tracks never locked on the beat rise from 16 to
21. MASCOT-POC.md found the same: a prior helps the octave, not the phase,
and makes the tracker refuse more often.

**CPU (host):** 152-158 µs per second of audio, median (a few % from run to run). In one run: click120 152,
One More Time 157, HBFS 160, Alligator 158, Stronger 158 and Testarossa
157 µs/s: real music costs the same as clicks.

### Click tracks and synthetic cases

| Case | Lock | BPM (error) | F | Phase med / p95 | Bias | Beats not on |
|---|---|---|---|---|---|---|
| click90 | 2.81 s | 90.00 (0.00 %) | 0.97 | 2.6 / 3.5 ms | −2.7 ms | 0 |
| click120 | 2.61 s | 119.99 (−0.01 %) | 0.97 | 2.7 / 5.7 ms | −2.9 ms | 0 |
| click128 | 2.93 s | 128.00 (0.00 %) | 0.97 | 2.6 / 3.5 ms | −2.7 ms | 0 |
| click140 | 2.67 s | 140.00 (0.00 %) | 0.97 | 2.9 / 4.9 ms | −2.9 ms | 0 |
| click174 | 2.50 s | 174.01 (0.00 %) | 0.98 | 2.8 / 3.5 ms | −2.8 ms | 0 |
| click120off | 2.80 s | 119.99 (−0.01 %) | 0.97 | 3.1 / 5.7 ms | −2.9 ms | 0 |
| click120, noise −30 dBFS | 2.61 s | 120.00 | 0.97 | 2.4 / 5.7 ms | −2.6 ms | 0 |
| click96, noise −20 dBFS | 3.26 s | 95.99 | 0.97 | 3.5 / 8.8 ms | −3.0 ms | 0 |
| click150off, noise −20 dBFS | 2.64 s | 149.99 | 0.98 | 2.4 / 5.6 ms | −2.4 ms | 0 |
| 10 s silence, 120, 10 s silence | 12.11 s (2.1 s after the first beat) | 119.98 | 0.96 | 2.3 / 5.7 ms | −2.7 ms | 3 after the end |
| 10 s noise, 110, 10 s noise | 12.31 s | 110.00 | 0.95 | 1.9 / 6.5 ms | −2.6 ms | 5 after the end |
| noise only, silence only | never | - | - | - | - | 0 |

| Tempo change | Relock after the change | Wrong beats in the 10 s after |
|---|---|---|
| 120 → 135 | 4.4 s | 2 |
| 128 → 96 | 6.9 s | 3 |
| 140 → 128 | 4.7 s | 3 |
| 124, an 8 s break, 124 half a beat later | 5.8 s | 0 |

The clicks still pass MASCOT-POC.md's targets: lock within 4 s, median
error under 10 ms, p95 under 25 ms, and a relock on the new tempo within
8 s. F stays below 1 only because the beats before the lock count against
recall.

### By album (good + ok tracks)

| Album | Tracks | F | F oct. | Tempo exact / oct. | Locked | On-beat lock after the 1st beat, med | Never on | Phase med / p95 | False beats |
|---|---|---|---|---|---|---|---|---|---|
| Air: Moon Safari | 9 | 0.25 | 0.30 | 67 / 78 % | 28 % | 66.2 s | 3 | 9.8 / 271 ms | 25 % |
| Aphex Twin: SAW 85-92 | 12 | 0.35 | 0.35 | 83 / 92 % | 34 % | 15.6 s | 3 | 14.2 / 239 ms | 21 % |
| Daft Punk: Discovery | 14 | **0.77** | 0.80 | 93 / 100 % | 79 % | 3.0 s | 1 | 5.9 / 27 ms | 5 % |
| Emancipator: Mountain of Memory | 13 | 0.15 | 0.16 | 54 / 85 % | 15 % | 19.6 s | 3 | 16.3 / 176 ms | 32 % |
| Kanye West: Graduation | 8 | 0.14 | 0.14 | 62 / 75 % | 28 % | 7.1 s | 3 | **142.5** / 199 ms | **66 %** |
| Kavinsky: OutRun | 13 | 0.57 | 0.58 | 77 / 77 % | 52 % | 7.8 s | 3 | 14.1 / 38 ms | 5 % |

By genre the same split shows:

| Genre | F |
|---|---|
| french house | 0.77 |
| synthwave | 0.57 |
| ambient techno | 0.35 |
| downtempo (Air + Emancipator) | 0.19 |
| hip-hop | 0.14 |

The full per-track table is in the run's `report.md`.

## The worst 15 tracks of the baseline, and why

These are ranked by F, with the no-clear-beat tracks left out. The evidence
column comes from `diagnose.py`:

- **Folds:** the tracker's own low and mid band onsets (rebuilt from its
  front end's energies) and a full-band and a >2 kHz spectral flux, each
  folded on the reference's beat in sixteenths. 0 is the beat, 8 the
  off-beat, and the numbers are peak heights against a mean of 1.
- **ACF:** the tracker's onset autocorrelation at the reference's lag and its
  relatives.
- **Run figures:** the tracker's median tempo estimate, clarity and lock
  episodes from the run.

"Clarity" is the tracker's `tempoClarity()`, which must reach 0.15 to
acquire.

| # | Track | Class | What happened | Evidence | Hypothesis |
|---|---|---|---|---|---|
| 5 | Air, Remember | ok | never locks | Low band folds on the beat (2.2), clarity median 0.08, the estimate wanders to 78 (2:3), onset ACF at the beat 0.16 vs 0.27 at half | Soft drums under sustained keys and bass. Over a 3 s memory the onset signal reads as noise, so clarity stays under the 0.15 gate. The beat is there over 2 min, not in any 3 s |
| 7 | Air, Ce matin la | ok | never locks | Clarity 0.07, estimate 68 (2:3), low band equal on the beat and the off-beat (2.3 / 2.1) | As Remember, plus an eighth-note bass line. **Reference doubtful**: local tempo spread 33 % (played live), and plain librosa said 138 (4:3) |
| 8 | Air, New Star in the Sky | hard | never locks | Onset ACF ≈ 0 at every metrical candidate; the reference is librosa's tracker beats (no grid), with attacks 41 ms off | No steady pulse in the tracker's bands; not locking is right. **Reference weak** (hard, untagged MP3, no grid) |
| 38 | Emancipator, Pollo Sneeps | ok | never locks | Folds at every sixteenth (0/4/8/12) in all bands, ACF at the beat 0.06, estimate 131 (unrelated), clarity 0.06 | Dense sixteenth-note percussion makes the onset periodic at the sixteenth. The beat level is too weak in a 3 s window to win; first beat 12 s in, weak pulse 70-82 and 104-140 s |
| 41 | Emancipator, Blue Dream | hard | never locks | **Estimate right (128.5)**, clarity 0.10, low band folds weakly on the beat (1.7) | The gate: the tempo is found but never clear enough. Two thirds of the track is weak pulse (reference clarity 0.28), so not locking is defensible |
| 52 | Kanye, Champion | hard | never locks | Low band peaks on the reference's **off-beat** (2.4 vs 1.5), mid at 7/16, full band flat; estimate 136 (4:3), clarity 0.04 | A swung, sparse sample-based beat. **Reference phase doubtful** (flat full-band fold, low clarity 0.27) |
| 56 | Kanye, Can't Tell Me Nothing | good | never locks | Full band and >2 kHz fold on the beat (1.6/1.7), the tracker's low band hardly (1.3), ACF at the beat 0.03 | The beat is in claps and hats above the tracker's bands (its mid band ends at 2.76 kHz, the decimated rate's Nyquist, with the box average already rolling off). The kick is a long 808 whose slow attack barely rises in log energy. A front-end blind spot |
| 58 | Kanye, Drunk and Hot Girls | ok | never locks | Low/mid fold on the beat, full band peaks on the off-beat; estimate 158 (unrelated), clarity 0.09 | A lilting, triple-feel groove; the metre is ambiguous for both. **Reference flags**: half-beat energy, 110 (4:3) scored 0.94 of 83, plain librosa said 110 |
| 76 | Kavinsky, Endless | good | never locks | **Estimate right (117.5)**, clarity 0.05, mid band folds on the beat (2.2), low band weakly (1.6) | The gate again: a soft kick under pads, the beat carried by the mid band. The tempo is known, but the clarity normalisation (score / lag-0 energy over 3 s) never trusts it |
| 57 | Kanye, Barry Bonds | hard | 2 beats at double tempo | Low band peaks a sixteenth (~45 ms) **before** the reference beat (3.0), full band on the off-beat; ACF strongest at half tempo; estimate wanders (105) | **Reference phase doubtful** (attacks +26 ms, beat/off-beat ambiguous, low clarity). Rap-style laid-back drums; the tracker finds no stable metre |
| 45 | Emancipator, Labyrinth | ok | 14 beats, mostly at double tempo, off the grid | Low/mid onsets peak 3/16 before the reference beat; full band on the beat and the off-beat equally (1.8 / 1.8); estimate 147 (3:2) | A syncopated kick (on the "a" before the beat) leads a low-band-weighted phase astray, and the metre reads as 3:2. **Reference** beat/off-beat ambiguous |
| 16 | Aphex Twin, Heliosphan | ok | locked at half tempo for 87 beats, 1/4-3/8 beat late | Low band peaks at sixteenths 4 and 6 (a breakbeat kick); hats on every sixteenth (full band 0/4/8/12); confidence 1.0 | The tracker follows the loudest, syncopated kick; the reference follows the hats' accent. The half tempo is fine, the phase isn't. Likely the tracker's fault (the low band dominates the onset); tape drift makes it harder |
| 61 | Kanye, The Glory | ok | locked at the right tempo for 241 beats, 198 of them a quarter beat (170 ms) late | Low band peaks hard at sixteenth 4 (4.3); the full band is almost flat (1.2 at the reference beat); confidence 1.0 | **Most likely the reference**: fitted to librosa beats on a mix where the full-band onset barely pulses (plain librosa said 124, local spread 15 %). The kick lands where the tracker says. Needs a listen |
| 0 | Air, La femme d'argent | ok | 7 beats, never on | Estimate 156 (2× the reference, which was itself halved from 159.7), clarity 0.07; low band peaks on every eighth | An eighth-note bass groove with no accented beat; the tracker sees no clear metre. **Reference**: beat/off-beat ambiguous, 107 (4:3) scored 0.91 |
| 28 | Daft Punk, Nightvision | ok | locked at 60 BPM (half) from 5 s to the end, every beat on the reference's off-beat | Low band has equal peaks on the beat and the off-beat (6.0 / 5.7); clarity 0.69, confidence 1.0 | **Reference/phase ambiguity**: the pulse sounds every half reference beat, so which of the two is "the beat" is a coin toss. The tracker's beats land on real pulses (F oct. 0.38). The dancer folds 60 BPM up to 120, so it would look about right |

### What the worst tracks and the rest show

1. **The acquisition gate.** 16 of the 69 beat tracks never get four on-beat
   locked beats in a row, and 6 never lock at all. On most of them the
   tracker's own onsets fold cleanly on the reference beat over the whole
   track, and twice (Endless, Blue Dream) its tempo estimate is right. But
   clarity, the harmonic ACF score over lag-0 energy with a 3 s memory,
   stays at 0.04-0.10, under the 0.15 gate. A sparse beat under sustained
   material reads as noise in 3 s. This costs more recall than anything
   else.
2. **Beat vs off-beat in the low band.** Under 150 Hz, an off-beat bass line
   is as strong as the kick. In the folds:

   | Track | Low band, beat / off-beat |
   |---|---|
   | Kelly Watch the Stars | 2.9 / 3.0 |
   | Ageispolis | 2.6 / 3.2 |
   | Chiefin | 2.9 / 3.2 |
   | Aerodynamic | 3.4 / 3.0 |

   The mid band and the full band favour the beat in all four. The 3 s phase
   fold then picks the off-beat about half the time. Kelly Watch the Stars
   sits on the off-beat for 232 confident beats.
3. **Sixteenth-note phases.** Syncopated kicks (hip-hop, breakbeats) pull the
   grid a quarter or three quarters of a beat away. This accounts for the
   952 off beats and Graduation's 142 ms median phase error.

   | Track | Where its locked beats fall (eighths of the reference beat) |
   |---|---|
   | The Glory | 2 |
   | I Wonder | 6, and 2 |
   | Good Morning | 2 |
   | Iron Ox | 6 |
   | Delphium | 6 |
   | Heliosphan | 2-3 |

   Each re-acquire can choose a different phase: Iron Ox goes 0 → 6 → 6,
   Chiefin 6 → 6 → 4. Some of these are the reference's fault (The Glory, Good
   Morning: the full band hardly pulses), but not all.
4. **4:3 and 3:2 metres.** 444 locked beats are at the wrong tempo, mostly
   on swung or shuffled downtempo:

   | Track | Tracked | Reference |
   |---|---|---|
   | We Are The Music Makers | 137 | 103 |
   | Deadcruiser | 88 | 117 |
   | Waxin | 131 | 87 |
   | Prélude | 144 | 108 |

   For Waxin and Deadcruiser the reference's own runner-up was the tracker's
   tempo, and for Music Makers plain librosa agreed with the tracker.
5. **Flapping locks.** There are 199 false episodes over the beat tracks. I
   Wonder, for one, alternates between 95.6 and 191 BPM episodes: after a
   loss, re-acquiring can pick the other octave and another phase.
6. **Recovery.** After breaks the tracker recovers in a median 7.8 s, and 19
   of 46 breaks are never followed by an on beat. Across gapless joins, it
   relocks in a median 7.2 s, and only on 37 of 71. The long-lag memory
   that rejects 4:3 also holds on to the old tempo.
7. **Timing.** When on, the median |error| is 8 ms, with the tracker 9 ms
   late on music and 3 ms early on clicks. The reference's own calibration
   is ±10 ms. The dancer is aimed 15 ms early, so this isn't where the work
   is.

The tracker's real problems were recall (it locked too seldom) and the
phase choice (beat vs off-beat vs sixteenth). Timing and tempo precision
were fine once it was on the beat. A change is judged on F and on-beat lock
first, with false beats and false episodes held at or below the baseline,
the clicks unchanged, and the host CPU within about 2× of ~155 µs/s
(compare runs made on the same machine).

## The rework (October 2026)

### What the baseline's traces showed

Before changing anything, the baseline's own `T` and `B` lines were split
by what the grid was doing (`lockstate.py`, `unlocked.py`, `factors.py` in
the scratch folder; the numbers are over the 69 beat tracks):

- The grid was **acquired but unlocked for 31 % of the span**, not
  acquired for 28 %, and locked for 42 %.
- Of the 10,432 unlocked grid beats inside the reference spans, **49 %
  were on the beat**. On some tracks nearly all of them: Talisman (263
  unlocked beats, 100 % on), Le voyage de Penelope (100 %), Dodo (592,
  85 %), Grand Canyon (85 %), Tha (566, 75 %), Alligator (83 %). On others
  nearly none: Music Makers (711 beats, 87 % at 4:3), Forged (96 % on a
  sixteenth), Good Morning (89 %), The Glory (85 %), Ageispolis (94 % on
  the off-beat).
- **None of the confidence's three factors told the two apart.** Among
  the unlocked beats, salience ≥ 0.1 kept 68 % of the on-beat ones and
  60 % of the wrong ones; the tempo-clarity factor was 0.93 for both; the
  jitter factor 0.55 against 0.43. The lock threshold wasn't the problem,
  the measure was.

So two candidate measures were computed from the same histogram and the
PLL's bookkeeping, and read off the traces per grid beat:

| Measure | On-beat grid beats, median | Wrong grid beats, median | ≥ threshold keeps on / wrong |
|---|---|---|---|
| **dominance**: the beat's three sixteenths over the strongest of the quarter, half and three-quarter phases | 2.9 | 1.27 | ≥ 1.5: 84 % / 36 %; ≥ 2.0: 72 % / 19 % |
| **hit rate**: recent PLL beats with an onset in their window | 0.97 | 0.69 | ≥ 0.7: 81 % / 49 % |
| dominance ≥ 1.5 and hit rate ≥ 0.7 | | | 71 % / 17 % |
| the baseline's lock (for comparison) | | | 66 % / 31 % |

An offline sweep of lock rules over the baseline's grids (`sweep.py`: a
rule applied per grid beat, no hysteresis, scored as beat F and false
share) ranked a product of dominance, hit rate and a three-sixteenth pulse
above every threshold on the old confidence, on the tuning and the monitored
splits alike (the sweep printed both, and the traces above pool all 69
tracks: the monitored split was in view throughout).

### What changed, step by step

Each step was a hypothesis, a change to the tracker, and a full harness
run (about 25 s), judged on the tuning split with the monitored one in
view. The clicks were watched on every run.

1. **Confidence = dominance × hits × pulse** (each scaled 0..1), the lock
   at 0.4 / 0.2. F 0.40 → 0.46, false beats 16 → 12 %, false episodes 199
   → 129; but the first lock came a median 16 s in (the hit rate started
   at 0.5 and rose slowly) and the tempo changes carried six wrong beats.
2. **Seed the hit rate from the acquisition window, let a miss count
   faster than a hit (0.35 against 0.2).** The clicks were back to their
   baseline lock times to the hundredth of a second; F 0.46, lock median
   8 s.
3. **A phase shift** (removed in the review: see below): a quarter, half
   or three-quarter phase that gathers 1.3× the beat's onsets for four
   beats becomes the grid. It never fired:
   on the tracks locked a sixteenth off (I Wonder, The Glory, Heliosphan)
   the tracker's own bands genuinely peak where it locked, and on the
   beat/off-beat ambiguous ones (Kelly Watch the Stars, Digital Love,
   Aerodynamic) the two phases are within a few percent. At 1.2 it fires
   on some, and was kept for the on-beat lock time it bought (tuning-split
   median 11.3 → 9.6 s, on the beat within 10 s 42 → 45 %); F is unchanged
   by it.
4. **Mid band at weight 1.0** (the numpy prototype of the tempo stage had
   predicted it): F 0.46 → 0.48, on-beat lock 10.7 → 6.8 s, false beats
   15 → 13 %. A 6 s autocorrelation memory, which the same prototype
   favoured, gained nothing on F and slowed the tempo-change relock to
   7-9 s, so it stays at 3 s.
5. **Noise.** The new confidence let ambient and white noise lock for a
   few seconds on some seeds, where the old jitter factor had refused.
   Three honest rejectors were tried and each cost real music: a window
   share per beat (F → 0.41), the jitter factor back (F → 0.39-0.43), no
   seeding of the histogram at acquisition (F 0.44, locks 15 s late). The
   traces then showed every noise acquired at exactly 1.86 s and 185 BPM:
   the onset mean removed before correlating is a leaky mean started from
   zero, so for its first three seconds the centred onsets are positive on
   average, their autocorrelation positive at every lag, and the comb
   rewards the shortest lags with the best preference weight. **True
   means until the leaky ones have settled** fixed it: white noise never
   acquires a grid now (12 seeds), ambient noise never locks, and the
   clicks and the music are unchanged.
6. **Lock at 0.35 / 0.12.** F 0.49 → 0.50 at the same false share, fewer
   false episodes than at 0.35 / 0.18 (the deeper hysteresis holds the
   good locks through a weak bar).
7. Tried and rejected: the linear onset term at 0.1 and 0.2 (F equal,
   locks later), a clarity factor (noise reaches the music's clarity), the
   mid-band-only dominance (no better than the combined one).

### Review fixes

A review of the rework (re-running the harness, rescoring the baseline with
the same scorer, and ablating its parts) found the false-lock side worse
than the results claimed, a crab that stayed idle through most of the new
locked time, and a few things the harness couldn't see. What was done about
each:

1. **More false locks on two of the three music suites.** The rework had
   cut false beats and episodes on whole tracks, but opened mid-track it
   went from 13.9 to 15.6 % false beats (40 → 50 episodes), and across the
   gapless joins from 69 to 99 false episodes, with 68 → 92 wrong beats in
   the 10 s after a join; the results reported only the whole tracks. Three
   remedies were run:
   - a faster fall of the hit rate (0.5 and 0.7 per miss instead of 0.35):
     fewer false beats, but F 0.48 and 0.45 and a later on-beat lock;
   - a "beat stopped" detector (two misses in a row with onsets elsewhere
     in the beat cut the confidence): no change at all, because a beat that
     moves (a tempo change, a join) keeps landing inside the ±0.2-beat
     window for a few beats, so those are hits;
   - **a lock dwell: the confidence must be at 0.35 or more for two PLL
     beats in a row.** The false episodes were mostly 5-16 beats long, a
     lock flapping on and off on a grid at the wrong phase; one good beat no
     longer starts a lock. Kept. A dwell of three met the baseline on every
     suite but more than doubled the time to the first lock (good tracks:
     3.8 → 13.6 s) and moved the click tracks' locks; applying it only to
     relocks helped half as much.
2. **The quarter-phase shift** (step 3) was ablated: F 0.495 without it
   against 0.496 with it, on the beat within 10 s 42 against 45 % (two
   tracks), but false beats 14.5 against 15.2 % and false episodes 175
   against 186. With the dwell its on-beat gain is gone (39 % either way).
   **Removed**, with its test.
3. **The dancer.** The rework moved the lock from 0.55 to 0.35 but the
   confidence-to-weight map stayed at 0.3-0.7, so most of the new locked
   time drew an idle crab: locked 42 → 52 % of the span, dancing (a weight
   over DanceRate's 0.5) only 41 → 42 %. **`dance::danceWeight` now maps
   0.12 → 0 and 0.5 → 1** (a fresh lock at 0.35 shows 0.65), and DanceMode
   dances only while the tracker is locked (an unlocked grid counts as no
   beat: the pose holds and fades). Dancing is now 46 % of the span against
   50 % locked. The runner writes the weight shown, so the report has a
   **dancing (span)** column; `test_a_fresh_lock_dances` and
   `test_low_confidence_blends_to_idle` hold the two scales together.
4. **Acquisition cost grew with the time since the reset.**
   `seedBeatStats()` stepped through every beat since `reset()` to reach
   the 3 s window, and scanned the whole window for each beat. A gapless
   album never resets, so on the host the acquiring hop went from 33 µs
   after an hour to 342 µs after 24 h (simulated, the tempo switching every
   20 s). It now starts at the first beat whose window reaches the
   acquisition window and reads only each window's hops: flat at 6-7 µs
   over 8 h, and the same beats, so the harness output is identical.
   `acquireReads()` counts the work and `test_acquisition_work_is_bounded`
   (40 simulated minutes) bounds it.
5. **The drum patterns with known beats** are in the harness now (36
   cases, below), and the heavy off-beat test is strict again, with the 11
   lead-in locks listed as known failures (a case that leaves or joins the
   list fails it).
6. **The clean-reference row** depended on the run: two of its flags came
   from the tracker's own output, so a track locked on its off-beat left the
   row. It is now picked from the reference's flags only (the same 35 beat
   tracks for every run); the tracker's flags are still shown per track.
7. **The held-out split** was in every tuning run's report and in the
   sweeps, so it is renamed the **monitored split**: a check on fitting, not
   an independent test. That needs tracks nobody has looked at.

### Results

The 69 beat tracks: the baseline, the rework as first committed (a7aba1a),
and after the review fixes, all scored by the same `score.py`:

| | baseline | rework | after review | |
|---|---|---|---|---|
| Beat F (±70 ms) | 0.40 | 0.50 | **0.49** | |
| F, octave-tolerant | 0.42 | 0.51 | 0.50 | |
| Precision / recall | 0.69 / 0.34 | 0.70 / 0.44 | 0.74 / 0.43 | the gain is recall, at a higher precision |
| Tempo exact / octave-tolerant (per track) | 74 / 86 % | 74 / 84 % | 72 / 84 % | |
| Locked, share of the span | 42 % | 52 % | 50 % | |
| Dancing (weight over 0.5), share of the span | 41 % | 42 % | **46 %** | each version's firmware rule, from the T lines |
| First lock, median | 4.4 s | 7.6 s | 9.7 s | fewer early locks, more of them right |
| On-beat lock after the first beat, median | 9.0 s | 6.5 s | 6.6 s | |
| On the beat within 10 s | 41 % | 45 % | 41 % | |
| Never on the beat | 16 | 15 | 16 | |
| Phase error, median / p95 | 10.5 / 218 ms | 11.2 / 166 ms | 10.8 / 154 ms | |
| Bias | +8.9 ms | +9.9 ms | +9.9 ms | |
| Locked beats that aren't on the beat | 16.0 % | 15.2 % | **13.5 %** | |
| False-lock episodes (their time) | 199 (1347 s) | 186 (1391 s) | **141 (1180 s)** | |
| Intro false locks (tracks / seconds) | 6 of 22 / 42 s | 5 / 37 s | 4 / 30 s | |
| Lock held past the last beat, median | 0.3 s | 1.3 s | 1.1 s | the hit rate takes a few beats to fall |
| Break recovery, median / never | 7.8 s / 19 of 46 | 3.8 s / 18 | 4.1 s / 18 | |

- **Locked beats:** 14,678 on, 360 off-beat, 1,380 off, 544 at the wrong
  tempo (11,074 / 714 / 952 / 444 at the baseline).
- **Paired lock times.** The pooled median hides which tracks moved. Track
  by track, the on-beat lock after the first beat is a median 0.0 s
  different from the baseline over the 51 tracks on the beat in both runs:
  17 earlier, 7 later (by more than 0.5 s), 2 on only now and 2 only at the
  baseline. Opened mid-track: a median 0.5 s earlier, 21 earlier, 6 later.
- **Other suites,** baseline → rework → after review:

  | Suite | F | on-beat lock, median | never on | false beats | false episodes (their time) |
  |---|---|---|---|---|---|
  | Whole tracks (69) | 0.40 → 0.50 → 0.49 | 9.0 → 6.5 → 6.6 s | 16 → 15 → 16 | 16.0 → 15.2 → **13.5 %** | 199 → 186 → **141** (1347 → 1391 → 1180 s) |
  | Opened 60 s in (67) | 0.39 → 0.47 → 0.46 | 9.1 → 6.5 → 7.7 s | 25 → 23 → 24 | 13.9 → 15.6 → **14.5 %** | 40 → 50 → **39** (202 → 284 → 250 s) |
  | Gapless joins (71) | 0.36 → 0.42 → 0.41 | 9.9 → 13.3 → 13.3 s | 27 → 19 → 21 | 17.5 → 18.2 → **16.3 %** | 69 → 99 → **81** (432 → 521 → 465 s) |

  **The criterion (false beats and episodes no worse than the baseline) is
  met on whole tracks, and not quite on the other two:** opened mid-track
  the false share is 0.6 points over the baseline (the episodes are
  under), and at the joins there are 12 more false episodes (the false
  share is under). Both trade against F and lock time: a hit-rate fall of
  0.55 per miss meets them everywhere, at F 0.46 and an on-beat lock median
  of 13.1 s. At the joins, 44 of 71 relock on the next track within its
  first 60 s (37 at the baseline), and 71 beats in the 10 s after a join
  are wrong (68 at the baseline, 92 in the rework); the click tracks'
  tempo changes still carry 5-6 wrong beats in the 10 s after them (2-3 at
  the baseline): see "What is left".
- **Monitored split** (every third track; see the review fixes):

  | | F | locked | on-beat ≤ 10 s | never on | false beats | episodes |
  |---|---|---|---|---|---|---|
  | tuning split (48), baseline → after review | 0.37 → 0.46 | 40 → 48 % | 40 → 38 % | 12 → 12 | 18 → 16 % | 134 → 87 |
  | monitored split (21) | 0.48 → 0.56 | 45 → 54 % | 43 → 48 % | 4 → 4 | 12 → 9 % | 65 → 54 |
  | mid-track, monitored (21) | 0.47 → 0.53 | 43 → 50 % | 38 → 43 % | 6 → 6 | 6 → 5 % | 11 → 6 |

  The F gain is about the same on both splits (+0.09 and +0.08), so the
  changes aren't fitted to the tuning tracks alone. But the on-beat lock
  within 10 s didn't improve on the tuning split, and the monitored split
  was looked at throughout, so this isn't an independent test.
- **Tracks that got worse** (F down by more than 0.1 against the baseline;
  the same four in the rework and after the review):

  | Track | Suite | F | Locked beats on / off-beat |
  |---|---|---|---|
  | Digital Love | whole | 0.81 → 0.56 | 345 / 0 → 223 / 74 |
  | Green Calx | whole | 0.51 → 0.38 | 280 / 109 → 181 / 75 |
  | Kelly Watch the Stars | from 60 s | 0.88 → 0.00 | 85 / 0 → 0 / 5 |
  | Tha | from 60 s | 0.48 → 0.16 | 42 / 0 → 12 / 0 |

  19 whole tracks and 20 mid-track starts gained more than 0.1 (Rampage,
  Talisman, Dodo and Grand Canyon by 0.5 or more). The losses share a
  mechanism, traced on Digital Love and Kelly: the grid is acquired on the
  beat, but there the off-beat gathers about as much onset energy as the
  beat in the tracker's two bands, so dominance sits at 0.8-1.3, the
  confidence under 0.1, and after 12 such beats the grid is dropped. The
  new acquisition folds the last 3 s and takes the strongest phase, which
  is the off-beat (dominance 1.2-1.8 there), and that grid locks. The
  baseline locked on the beat on salience alone and never re-acquired.
  Keeping a weak grid while its tempo still matches the estimate fixed
  Digital Love (0.61, no off-beat beats) but cost F overall (0.489 → 0.471;
  Aerodynamic 0.21 → 0.05), so it was not kept.
- **Drum patterns with known beats** (the host test's 36 heavy off-beat
  cases): on the kick in 16, on the lead-in in 11 (124 and 150 BPM at
  0.75, 150 BPM at 0.66), never locked in 9 (the straight off-beat as loud
  as the kick at 124 and 150 BPM, and 96 BPM at 0.75), and nowhere else.
  The rework as committed gives the same 16 / 11 / 9; the baseline locked on
  the kick in all but the four 150 BPM, 0.75 cases.
- **By class and album** (F, baseline → after review): good 0.50 → 0.62,
  ok 0.29 → 0.35, hard 0.04 → 0.16, clean reference (35 tracks, the
  reference's flags only) 0.49 → 0.63; Daft Punk 0.77 → 0.77 (Digital Love
  down, the rest up), Kavinsky 0.57 → 0.71, Aphex Twin 0.35 → 0.43, Air
  0.25 → 0.36, Emancipator 0.15 → 0.29, Kanye 0.14 → 0.21 (the phase
  median still about 100 ms: the sixteenth-note kicks).
- **With the reference tempo as a prior:** F 0.47 (baseline 0.40), tempo
  exact 84 % (83 %), false beats 9 % (13 %), false episodes 107 (142), but
  an on-beat lock median of 14.0 s (10.2 s); never on 16 (21).
- **Click tracks:** lock times as before to the hundredth of a second
  (2.50-3.26 s), except intro_outro_silence (12.11 → 12.61 s: the dwell)
  and the break's relock (5.8 s at the baseline, 6.3 s); tempo error ≤
  0.01 %, phase median 2.7-3.6 ms, p95 ≤ 8.5 ms, bias about −3.2 ms (−2.8
  at the baseline: the mid band's rise comes a hop earlier on a click).
  The tempo changes relock in the same 4.4-6.9 s. Noise and silence never
  lock, and white noise never acquires.
- **The USB visualizer's path:** `--via-hops` gives output identical to
  `process()` on all 275 cases (the 77 tracks, the mid and join suites and
  the 53 synthetic cases); the front end and protocol are untouched.
- **CPU (host).** In the default build the review's tracker measured 15-25 %
  slower than a7aba1a's, back to back (the bench and two tracks, best of 7;
  the laptop was busy that day, so every version read 210-310 µs/s). Built
  with the code alignment pinned (`-falign-functions=64 -falign-loops=64
  -falign-jumps=16`) all three versions are within the noise (220-264
  µs/s), and the per-hop path is unchanged (the fixes touch the
  acquisition and a per-beat counter), so the default build's gap is code
  layout. The memory is the same. The device bench (`[dance] tracker
  bench`) has the last word and hasn't been run.

### What is left

The worst 15 are, by F: Nightvision, Pollo Sneeps, Blue Dream, Good
Morning, Champion, Can't Tell Me Nothing, Barry Bonds, Prélude, New Star in
the Sky, Ce matin la, La femme d'argent, The Glory, Delphium, Drunk and Hot
Girls, Remember. Nine of them never lock at all (no steady pulse in the
tracker's bands, or a beat too sparse for a 3 s memory), and the rest lock
on a pulse the reference doesn't call the beat:

1. **Beat vs off-beat.** Where the off-beat gathers as much onset energy as
   the beat in the two bands, the new confidence refuses to lock, and a
   re-acquisition can land on the off-beat: the regressions above (Digital
   Love, Green Calx, and opened mid-track Kelly Watch the Stars and Tha),
   and Nightvision never locks. The mid band at full weight helped
   (off-beat beats 714 → 360). The next step would be a third band above
   the decimated rate (hats and snares, which favour the beat in every fold
   measured), which changes the hop protocol; a phase preference that
   survives the drop and re-acquire is cheaper, but the form tried cost
   more than it gave.
2. **Sixteenth-note phases.** The Glory, I Wonder, Good Morning,
   Heliosphan: the tracker's bands peak a sixteenth off the reference beat,
   and the dominance measure agrees with the tracker. These need the same
   third band, or the reference needs a listen. On the synthetic drums with
   known beats the tracker also prefers a lead-in (item 5), so the
   reference isn't simply wrong here.
3. **4:3 and 3:2 metres.** Music Makers (137 for 93 s), Waxin, Deadcruiser,
   Prélude: locked with confidence where the baseline flapped. The
   reference's own runner-up is the tracker's tempo on three of them.
4. **Sparse beats under sustained sound** (Remember, Endless, Blue Dream):
   the right tempo estimate, clarity under the 0.15 acquisition gate. A
   longer memory for sparse material, switched in when the onset signal is
   quiet, is the obvious next experiment.
5. **Lead-ins, and straight off-beats as loud as the kick.** On the
   synthetic heavy off-beat cases the tracker locks on a lead-in a
   sixteenth or a swung eighth before the kick in 11 of 36 cases (the
   baseline in 4), and doesn't lock at all in 9 (the baseline in none).
   The host test lists the 11 as known failures.
6. **Letting go.** The lock holds about 1 s past the last beat (0.3 s at
   the baseline) and carries 5-6 wrong beats through a tempo change (2-3):
   the hit rate takes three misses to fall, and a moved beat keeps hitting
   the PLL window for a few beats. A faster fall costs F and lock time (the
   numbers above); a detector for a beat that has moved, rather than
   stopped, is not written.
7. **Not measured on the device:** the CPU, and how the dancer looks with
   the new weight map (a listen on Digital Love and a hip-hop track).
