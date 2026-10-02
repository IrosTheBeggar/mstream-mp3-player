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
whether its beats are hit), let the mid band count in full, and fixed a
bias that had every noise "acquired" at 185 BPM. On the 69 beat tracks the
beat F-measure went from 0.40 to 0.50 (0.48 to 0.57 on the 21 tracks held
out of the tuning), with the share of wrong locked beats and the click
tracks unchanged. What is left is in the music the tracker's two bands
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
    `process()`'s on all 77 tracks and the 17 synthetic cases.
  - **Other modes:** `--synth` builds ClickGen cases (`click:BPM[off]:S`,
    `silence:S`, `noise:DBFS:S`, `ambient:DBFS:S[:SEED]`, the native
    tests' low-passed swelling noise, joined with commas). `--bench` and
    `--time-only` are the CPU measures. `--hops-out` dumps the front end's
    energies.
  - **The tracker's factors:** each `T` line also carries
    `BeatTracker::factors()` (dominance, hit rate, pulse, and the PLL's RMS
    correction), so a run shows why the confidence was what it was.
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
    takes about 20 s. A full run (239 cases) takes about 10 s, and scoring
    about 8 s.
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

  The scorer adds two flags of its own: the tracker locked confidently at a
  4:3, 3:4 or 3:2 relation (Music Makers, Waxin, Deadcruiser), or on the
  off-beat (Kelly Watch the Stars, Nightvision), for most of its beats. For
  Waxin and Deadcruiser, the reference's own runner-up tempo was the
  tracker's (it scored 0.92-0.93 of the winner). For Music Makers, plain
  librosa said 138, as the tracker does. The summary has a row for the
  beat tracks with a **clean reference**.
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
- **Dev and held out:** every third track by index (26 of 77, 21 of them
  beat tracks) is held out of any tuning; the summary has a row for each
  half. A change is judged on the dev rows and then checked on the held-out
  ones.
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
  reaches them. The dancer shows them ~115-175 ms later (output latency), and
  it is driven by confidence (a weight from 0.3 to 0.7), not by `locked()`.
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
| beat tracks, clean reference | 34 | 0.50 | 0.50 | 82 % | 91 % | 48 % | 5.5 s | 6.6 s | 50 % | 3 | 8.7 / 139 ms | +9.4 ms | 9 % | 52 |
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
above every threshold on the old confidence, on the dev and the held-out
tracks alike.

### What changed, step by step

Each step was a hypothesis, a change to the tracker, and a full harness
run (about 25 s), judged on the dev tracks and checked on the held-out
ones. The clicks were watched on every run.

1. **Confidence = dominance × hits × pulse** (each scaled 0..1), the lock
   at 0.4 / 0.2. F 0.40 → 0.46, false beats 16 → 12 %, false episodes 199
   → 129; but the first lock came a median 16 s in (the hit rate started
   at 0.5 and rose slowly) and the tempo changes carried six wrong beats.
2. **Seed the hit rate from the acquisition window, let a miss count
   faster than a hit (0.35 against 0.2).** The clicks were back to their
   baseline lock times to the hundredth of a second; F 0.46, lock median
   8 s.
3. **A phase shift**: a quarter, half or three-quarter phase that gathers
   1.3× the beat's onsets for four beats becomes the grid. It never fired:
   on the tracks locked a sixteenth off (I Wonder, The Glory, Heliosphan)
   the tracker's own bands genuinely peak where it locked, and on the
   beat/off-beat ambiguous ones (Kelly Watch the Stars, Digital Love,
   Aerodynamic) the two phases are within a few percent. At 1.2 it fires
   on some, and is kept for the on-beat lock time it buys (dev median
   11.3 → 9.6 s, on the beat within 10 s 42 → 45 %); F is unchanged by it.
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

### Results

The 69 beat tracks, baseline against the rework (`final`; the full tables
are the two runs' `report.md`):

| | baseline | rework | |
|---|---|---|---|
| Beat F (±70 ms) | 0.40 | **0.50** | |
| F, octave-tolerant | 0.42 | 0.51 | |
| Precision / recall | 0.69 / 0.34 | 0.70 / 0.44 | the gain is recall at the same precision |
| Tempo exact / octave-tolerant (per track) | 74 % / 86 % | 74 % / 84 % | |
| Locked, share of the reference span | 42 % | 52 % | |
| First lock, median | 4.4 s | 7.6 s | the first lock is a real one more often |
| On-beat lock after the first beat, median | 9.0 s | **6.5 s** | |
| On the beat within 10 s | 41 % | 45 % | |
| Never on the beat | 16 | 15 | |
| Phase error, median / p95 | 10.5 / 218 ms | 11.2 / 166 ms | |
| Bias | +8.9 ms | +9.9 ms | |
| Locked beats that aren't on the beat | 16 % | 15 % | |
| False-lock episodes | 199 | 186 | |
| Intro false locks (tracks / seconds) | 6 of 22 / 42 s | 5 of 22 / 37 s | |
| Lock held past the last beat, median | 0.3 s | 1.3 s | the hit rate takes a few beats to fall |
| Break recovery, median / never | 7.8 s / 19 of 46 | 3.8 s / 18 of 46 | |

- **Locked beats:** 15,027 on (82 %), 526 off-beat (3 %), 1,550 off (8 %),
  608 at the wrong tempo (3 %), against 11,074 / 714 / 952 / 444 before:
  5,000 more beats locked, and the extra ones are 79 % on.
- **Dev and held out:**

  | | F | locked | on-beat ≤ 10 s | never on | false beats | episodes |
  |---|---|---|---|---|---|---|
  | dev (48 beat tracks), baseline → rework | 0.37 → 0.46 | 40 → 50 % | 40 → 40 % | 12 → 11 | 18 → 17 % | 134 → 114 |
  | **held out** (21 beat tracks), baseline → rework | 0.48 → **0.57** | 45 → 57 % | 43 → 57 % | 4 → 4 | 12 → 10 % | 65 → 72 |
  | mid-track start, held out | 0.47 → 0.55 | 43 → 52 % | 38 → 48 % | 6 → 6 | 6 → 6 % | 11 → 9 |

  The held-out gain is as large as the dev one, so the changes aren't
  fitted to the tracks they were tuned on.
- **By class and album:**

  | | F before | F after |
  |---|---|---|
  | good (36) | 0.50 | 0.62 |
  | ok (33) | 0.29 | 0.36 |
  | hard (6) | 0.04 | 0.17 |
  | clean reference (34) | 0.50 | 0.65 |
  | Daft Punk | 0.77 | 0.77 |
  | Kavinsky | 0.57 | 0.71 |
  | Aphex Twin | 0.35 | 0.43 |
  | Air | 0.25 | 0.37 |
  | Emancipator | 0.15 | 0.30 |
  | Kanye | 0.14 | 0.22 (phase median still 104 ms: the sixteenth-note kicks) |

- **Other suites:**

  | Suite | F before → after | on-beat lock, median | never on |
  |---|---|---|---|
  | Opened 60 s into the song | 0.39 → 0.47 | 9.1 → 6.5 s | 25 → 23 of 67 |
  | Gapless joins (no reset) | 0.36 → 0.42 | 9.9 → 13.3 s | 27 → 19 of 71 |

  At the joins, 45 of 71 relock on the next track within its first 60 s
  (37 before); the median relock is 7.6 s (7.2 s) because the harder joins
  now count, and eight joins still carry 4-12 wrong beats into the next
  track.
- **With the reference tempo as a prior:** F 0.48, tempo exact 87 %, false
  beats 12 %, never on 14. As before, the prior helps the octave and the
  false share, and costs a little recall.
- **Click tracks:** lock times unchanged to the hundredth of a second
  (2.50-3.26 s), tempo error ≤ 0.01 %, phase median 2.7-3.6 ms, p95 ≤ 8.5
  ms, bias about −3.2 ms (−2.8 before: the mid band's rise comes a hop
  earlier on a click). The tempo changes relock in the same 4.4-6.9 s but
  carry 5-6 wrong beats in the 10 s after the change instead of 2-3: the
  hit rate takes three misses to fall. Noise and silence never lock, and
  white noise never acquires.
- **The USB visualizer's path:** `--via-hops` gives output identical to
  `process()` on all 77 tracks and the 17 synthetic cases; the front end
  and protocol are untouched.
- **CPU (host, serial, same machine, back to back, best of 7):** the
  baseline 156-178 µs per second of audio over the bench and five tracks,
  the rework 161-181 µs/s: within the run-to-run noise. The rework adds a
  16-bin decay per hop and a few sums per beat; its memory is the same
  (the allocation test's 12 KB bound holds, nothing new in internal RAM).
  The device bench (`[dance] tracker bench`) still has the last word.

### What is left

The worst 15 after the rework are, by F: Nightvision, Pollo Sneeps, Blue
Dream, Good Morning, Champion, Can't Tell Me Nothing, Barry Bonds,
Prélude, New Star in the Sky, Ce matin la, La femme d'argent, The Glory,
Delphium, Drunk and Hot Girls, Remember. Nine of them never lock at all
(no steady pulse in the tracker's bands, or a beat too sparse for a 3 s
memory), and the rest lock on a pulse the reference doesn't call the beat:

1. **Beat vs off-beat.** Nightvision now never locks (it sat on the
   off-beat before): with equal pulses on both, no phase dominates. Kelly
   Watch the Stars, Ageispolis and Digital Love still spend stretches on
   the off-beat where the low band favours it. The mid band at full weight
   helped (off-beat beats fell from 714 to 526); the next step would be a
   third band above the decimated rate (hats and snares, which favour the
   beat in every fold measured), which changes the hop protocol.
2. **Sixteenth-note phases.** The Glory, I Wonder, Good Morning,
   Heliosphan: the tracker's bands peak a sixteenth off the reference beat,
   and the dominance measure agrees with the tracker. These need the same
   third band, or the reference needs a listen.
3. **4:3 and 3:2 metres.** Music Makers (137 for 93 s), Waxin, Deadcruiser,
   Prélude: now locked with confidence where the baseline flapped. The
   reference's own runner-up is the tracker's tempo on three of them.
4. **Sparse beats under sustained sound** (Remember, Endless, Blue Dream):
   the right tempo estimate, clarity under the 0.15 acquisition gate. A
   longer memory for sparse material, switched in when the onset signal
   is quiet, is the obvious next experiment.
5. **The lead-in.** On the synthetic heavy off-beat cases the tracker now
   locks on a lead-in a sixteenth or a swung eighth before the kick in 11
   of 36 cases (124 and 150 BPM, both notes within 4-6 dB of the kick with
   the hat on top), where before it sat on the kick by a thin margin. The
   test accepts exactly that lead-in and nothing else; MASCOT-POC.md has
   the detail.
