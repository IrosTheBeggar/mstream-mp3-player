# Exact resume and seeks

A track that starts part of the way in should start where it was asked to:

- **A resume point** (after a pause and a restart, the power-off, the CPU
  speed's restart) should pick up on the very sample it paused at.
- **A seek** (the console's `qs`, a future scrubber) should land as close
  to the time asked as the file allows. The time shown should be the time
  that plays.

Today neither holds for MP3s:

- A VBR MP3 lands up to 2.4 s from the time asked, and the time shown is
  the time asked anyway.
- A CBR MP3 lands 26-52 ms late.
- A start 5-10 s before the end of a VBR file falls back to 0:00 one time
  in five.

This document is the design that fixes all three. It covers what happens
today, the measurements it rests on, the mechanism, how it fits gapless
playback and the queue saver, and the host and device test plans.

**Status: design, not built.** Section 12 is the build order.

Where the facts come from:

- **The code**, read at 2742f3d: `lib/core/TrackSeek`, `TrackProgress`,
  `LameTag`, `TrimFeed`, `QueueSaver`, `NvsLayout`, `PlaybackController`,
  `src/audio/Core2AudioBackend`, `PinnedMp3`, `GuardedSource`, and
  ESP8266Audio's `AudioGeneratorMP3` and libmad in
  `.pio/libdeps/core2/ESP8266Audio`.
- **Measurements on the PC (2026-10-02)**, on the PC's copy of the card's
  library (the `groundtruth` set: 37 MP3s, 40 FLACs), with a host build of
  the tree's own libmad. Section 2 gives the method and the numbers. The
  scripts are temporary, in the session's scratchpad (`seek/`). Section 12
  proposes keeping the libmad harness as a tool.

## 1. What happens today

### 1.1 Saving a resume point

1. At a pause, `PlaybackController::resumePoint()` returns the backend's
   `positionMs()` and `durationMs()`. That is the heard track's start (ms)
   plus `(readPos − its first ring frame) / 44.1`.
2. `QueueSaver::stepResume()` saves it within a pass as a `QueueResume`
   holding the generation, the queue line, the path's FNV-1a hash, the
   position in ms and the length in ms.
3. `QueueStore::saveResume()` writes it to NVS (`queue`/`resume`) as
   `NvsLayout`'s version-1 blob (24 bytes).
4. It is saved again only if the position moves by 250 ms or more
   (`kResumeSlackMs`). It is cleared as soon as playback moves on.

At boot, `QueueStore::restore()` checks the blob against the restored file
(`resumeApplies()`: the generation, the line and the path hash). If it
applies, it becomes the player's start point (`setStartPoint(ms, length)`).
The next play then asks the backend for `play(path, length, ms)`.

The same `play(path, length, ms)` is used by:

- `stopKeepingPlace()`;
- a start point set while paused (the held track is let go);
- the console's `qs<sec>`.

So every start part of the way in goes through one path, and only the
millisecond survives.

### 1.2 Starting part of the way in

`Core2AudioBackend::prepare()` reads 4 KB after the ID3 tags.
`mp3StartByte()` then works as follows:

1. **The length** (`trackseek::mp3LengthMs()`, or the header's already
   known length) and **where a start lands** (`trackseek::startMs()`: the
   last 5 s, or at or past the end, give 0:00).
2. **A byte** (`trackseek::mp3SeekByte()`), the first method that applies:
   - an Info header whose frames agree on their bitrate: the bitrate (from
     the first audio frame when LAME's tag is trusted);
   - else the Xing TOC, with straight lines between its 100 points;
   - else the VBRI TOC;
   - else the average bitrate (the header's length over its bytes);
   - else, with no header, the first frame's bitrate, or the hint's
     average.

   With a trusted LAME tag the time is first moved onto the decoded stream
   (`lametag::untrimmedMs()`: `+ (delay + 529) / rate`).
3. **A clean frame at or after that byte** (`mp3FrameAt()`: a header
   followed by another of the same version and rate).
4. **The tail rule's second check**: the bytes left from that frame, at
   that frame's bitrate (`mp3MsLeft()`). 5 s or less means 0:00.
5. The decoder is handed that frame. `TrimFeed` skips only the generator's
   lead (one `{0,0}`) and holds the end padding (GAPLESS.md section 4.6).
   `positionMs()` counts from the time asked.

libmad drops the first frame it is handed: its `main_data_begin` points
into frames it never saw (`MAD_ERROR_BADDATAPTR`, section 2.4). So
playback really starts one frame (26 ms) after the frame found, which is
already up to one frame after the byte. The time shown is the time asked.

**FLAC** works differently. `STREAMINFO` gives the length and the rate.
libFLAC seeks to the sample `ms × rate / 1000`. That is exact to the
millisecond, because only the ms was saved.

**A built-in track** just counts from there.

### 1.3 The tail rule

The tail rule is applied twice:

- **By the length:** `trackseek::startMs()` sends a start in the last 5 s
  to 0:00. The length is exact for a LAME file (frames × spf − delay −
  padding).
- **By the landed frame's bitrate:** `mp3MsLeft()` is the second check. It
  exists for a file shorter than its header says. On a VBR file it is
  wrong in both directions, because one frame's bitrate says nothing about
  the average from there to the end.

## 2. Measured for this design

### 2.1 The material

- **The files:** the 37 MP3s of the card, as copied to the PC.
  - 27 are LAME VBR with a Xing TOC: Aphex Twin's *SAW 85-92* (13, LAME
    3.98) and Daft Punk's *Discovery* (14, LAME 3.97b).
  - 8 are LAME 3.92 CBR 192 with an Info header (Air's *Moon Safari*).
  - 2 are CBR 192 with no header (Air 09 and 10).
- **The truth:** every frame walked from the first audio frame: its byte,
  bitrate, length and `main_data_begin`. In every file the walked frame
  count equals the Xing frame count.
- **The firmware's seek:** `mp3SeekByte()` (its TOC and CBR branches) and
  `mp3FrameAt()`, replicated line for line.
- **The device's decoder on the PC:** `seekmad`, built from
  `groundtruth/maddec`. It is the tree's libmad (`diff -r` identical to
  `.pio/libdeps/core2/ESP8266Audio/src/libmad`), with `-DFPM_64BIT` as on
  the device, behind a verbatim copy of `AudioGeneratorMP3::Input()`: the
  0x600-byte buffer, the 0xFFE sync search, and `lastReadPos` not moved by
  the resync shift.

### 2.2 VBR seeks by the TOC

A start was asked every second from 0:10 to 10 s before the end of each of
the 27 VBR files: 7,617 starts in all. The error is the trimmed time of
the first sample libmad really outputs, minus the time asked. It includes
the frame libmad drops (section 2.4).

| Mapping | mean | \|err\| p50 | p95 | max |
|---|---|---|---|---|
| A: the firmware (point *i* at *i* % of the time, its byte at `toc[i]/256`) | −360 ms | 396 ms | 1,317 ms | 2,423 ms |
| B: A with the truncation undone (`(toc[i] + 0.5)/256`) | +327 | 385 | 1,323 | 2,595 |
| **C: LAME's TOC inverted (section 6.3)** | **+25** | **242** | **735** | **1,617** |
| D: C without the +0.5 | −662 | 583 | 1,535 | 2,365 |

| Album | A: mean, p50, p95, max | C: mean, p50, p95, max |
|---|---|---|
| *SAW 85-92* (13 files, 4,233 starts) | −437, 472, 1,411, 2,423 ms | +33, 277, 724, 1,086 ms |
| *Discovery* (14 files, 3,384 starts) | −264, 318, 1,139, 2,286 ms | +14, 209, 750, 1,617 ms |
| *One More Time* alone (301 starts) | −515, 496, 1,215, 1,499 ms | +61, 218, 534, 652 ms |

- The ~1.4 s the device saw on *One More Time* (`qs`) is this error. Its
  maximum here is 1,499 ms.
- C is better than A on 26 of the 27 files. The exception is
  *Nightvision*: p95 460 ms against 267 ms. Its stored TOC doesn't match
  its frames (next section).
- The half measures, B and D, are no better than A. Only the whole model
  helps.

### 2.3 Why: how LAME writes its TOC

This is LAME's `VbrTag.c` as recalled; the model below reproduced the
stored tables, and the source should be read again when this is built.

1. Every encoded frame adds its bitrate (kbps) to a running sum.
2. Every `want` frames, the sum goes into a 400-slot "bag".
3. When the bag is full, every other slot is kept and `want` doubles.
4. At the end, `TOC[i] = (int)(256 × bag[⌊i/100 × pos⌋] / sum)`, where
   `pos` is the number of slots in use.

So TOC point *i* is the byte share after `(⌊i·pos/100⌋ + 1)·want` frames,
not at *i* % of the time. That is up to `want` frames late, where `want`
is 8-64 frames in these files: up to 1.7 s. The share is also truncated
to 1/256, which is up to 1/256 of the file early: about 1.2 s in a 5-minute
256 kbit/s file. The firmware reads the points as *i* % of the time at
exactly `toc[i]/256`, so it carries both errors.

The model was checked by running it on the walked frames' bitrates and
comparing its TOC with the stored one:

- **LAME 3.98:** all 13 files match on all 100 points.
- **LAME 3.97b:**
  - 6 of 14 files match on all 100 points;
  - 7 match on 97-99 points, the others off by one 256th;
  - *Nightvision* matches on 53 points, the others off by 1-2.

  All 14 *Discovery* files have a Xing byte count 380-404 bytes larger
  than their stream. They were edited after encoding, so their TOC was
  computed on a slightly different stream.

The integer index `i·pos/100` gives the same points as LAME's float one
in all 27 files. A sum of frame bytes instead of kbps gives the same
match.

### 2.4 A cold start loses a frame; a preroll of two frames and 1 KB brings it back exactly

Layer III frames borrow main data from the frames before them (the bit
reservoir: `main_data_begin`, up to 511 bytes for MPEG-1 and 255 for
MPEG-2/2.5).

- **What libmad does** (`layer3.c`, `mad_layer_III()`): a frame whose
  reservoir reaches further back than the bytes libmad holds is not
  decoded (`MAD_ERROR_BADDATAPTR`). The tail of its main data is still
  loaded as reservoir for the frame after it.
- **What the generator does:** `AudioGeneratorMP3::loop()` retries with
  the next frame, and the failed frame outputs nothing at all, not even
  silence.
- **Bit-exact output from frame *k*** needs three things:
  - frame *k*'s reservoir;
  - the IMDCT overlap from frame *k* − 1, so frame *k* − 1 must decode;
  - the synthesis filterbank's history, which frame *k* − 1's 1,152
    samples give.

The test with `seekmad`, on all 37 MP3s:

1. Decode from the first audio frame to the end.
2. Draw 200 random targets per run: frame *k* and sample *n* in it
   (8,000 trials in all: 40 runs, so three of the files ran twice).
3. For each target, begin a fresh decoder (as `begin()` makes one) at the
   byte of frame *k* − *p*. Synthesize every frame, and drop the output
   until the cursor (section 2.5) says frame *k*, then *n* more samples.
4. Compare the next 4,608 samples with the decode from the top.

| Start the decoder at | bit-exact | frame *k* never decoded | mean preroll |
|---|---|---|---|
| frame *k* itself (*p* = 0) | 3 of 8,000 | 7,977 | 0 |
| *p* = 1 | 926 | 26 | 1 frame |
| *p* = 2, 3 or 4 | 7,977-7,978 | 22 | 2-4 frames |
| **the latest frame at least 2 frames and 1,024 bytes before frame *k*** | **8,000 of 8,000** | **0** | **2.03 frames** |
| at least 2 frames and 2,048 bytes | 8,000 of 8,000 | 0 | 3.3 frames |

- Frame *k* itself as the start is today's case. The frame it lands on is
  lost 99.7% of the time, which is the 26-52 ms a CBR seek was measured
  behind (ARCHITECTURE.md).
- The 22 failures at a fixed *p* = 2-4 are frames in near-silence at low
  bitrates, whose reservoir reaches over more frames.
- **The rule "2 frames and 1 KB" is also sufficient by construction.**
  - Frame *k* − 1's reservoir is at most 511 main-data bytes before it.
  - The smallest MPEG-1 frame (32 kbit/s) has 104 bytes, of which 64 are
    main data. 511 bytes then lie within 8 frames, 832 bytes, which with
    frame *k* − 1 is still under 1,024.
  - MPEG-2/2.5's reservoir is at most 255 bytes.

### 2.5 The cursor: which frame a sample comes from

From a subclass, the generator's protected state says exactly which frame
the sample it is offering comes from:

- the frame's file offset: `lastReadPos + (stream->this_frame − buff)`,
  which `ErrorToFlow()` also uses for its log;
- the sample's index in that frame: `(nsCount − 1) × 32 + (samplePtr − 1)`.
  `GetOneSample()` synthesizes 32 samples per `mad_synth_frame_onens()`
  call and has moved both counters on before the sample is offered.

Checked with `seekmad` on all 37 files, from the first audio frame to the
end: for all 412,324 frames, the cursor's offset equalled the header
walk's offset, and frame *j*'s first sample was output sample *j* × spf. No
frame was lost from the top in any file.

### 2.6 CBR frames are where arithmetic says

In the 10 CBR files, every frame *k* starts at `firstAudio + ⌊k·L⌋` or one
byte later. *L* is the frame length with its fraction: 144,000 × kbps /
rate for MPEG-1, 72,000 × kbps / rate for MPEG-2/2.5; 626.94 bytes here.
So `k = round((byte − firstAudio) / L)` holds for every frame (100,569
frames, no exception: 96,465 at `⌊k·L⌋`, 4,104 one byte later). A CBR file can therefore be started on the exact
frame for a time, and the time of a frame is known exactly.

### 2.7 The tail rule's false 0:00s

The test asked starts every 100 ms from 10 s down to 5.2 s before the end
of each of the 27 VBR files: 1,323 starts, all of which should start.

- 258 of them (20 %), in 23 of the 27 files, went to 0:00 instead.
  `mp3MsLeft()` read the bytes left at the landed frame's bitrate, and
  that frame was a 256 or 320 kbit/s one in a stretch whose average is
  lower.
- *Crescendolls* asked 6.6 s before its end, as on the device: the TOC
  lands on a 256 kbit/s frame. `mp3MsLeft()` says 3,861 ms are left; the
  truth is 6,513 ms. So it starts at 0:00.

## 3. Decisions

1. **A resume point saves bytes, not only a millisecond.** While it
   decodes, the backend knows the byte of the frames it decodes and their
   place on the trimmed timeline. A pause's resume point gets an anchor,
   which is everything a later start needs to begin exactly there:
   - the byte to hand the decoder (a preroll frame);
   - the frame the paused sample is in, and the samples into it;
   - the sample on the trimmed timeline;
   - what checks that it is still the same file.

   It goes into NVS as version 2 of the resume blob (section 5.2). At the
   start the anchor is checked against the file. If it fails, the start is
   by the millisecond, as today.
2. **One mechanism for every MP3 start part of the way in: a start plan.**
   A plan has a preroll byte, a landing frame's byte, the samples to skip
   in and after that frame, and the start's place on the timeline (exact
   or not).
   - The decoder is handed the preroll byte.
   - `TrimFeed` drops everything until the cursor says the landing frame,
     then the skip (section 4.2).
   - The plan comes from the first source that can give one: a checked
     anchor, the run's index, CBR arithmetic, LAME's TOC inverted, the
     Xing or VBRI TOC, the average bitrate (section 4.5).
   - Every plan has a preroll of at least 2 frames and 1 KB, clamped at
     the first audio frame. So the landing frame is decoded and is
     bit-exact (section 2.4).
3. **The run index.** While a track plays, the decode task records every
   4th frame: its byte, its first sample on the timeline and a hash of its
   first 32 bytes. These go into a 24 KB PSRAM ring per track, two rings
   in all (the heard track and one decoded ahead).
   - It is the anchors' source.
   - It makes a seek back into what this run has decoded exact (the last
     3.6 min at 44.1 kHz).
   - It is per run only: no cache per path, nothing on the card
     (section 6.6).
4. **LAME's TOC inverted** (section 6.3) for a Xing VBR file with a
   trusted LAME tag that says "LAME".
   - Measured on 27 files: p95 1,317 → 735 ms, mean −360 → +25 ms.
   - Files with another encoder keep today's straight lines.
5. **CBR seeks become exact** (section 6.2).
6. **The tail rule goes by the exact length** (frames × spf − delay −
   padding; FLAC's total samples), never by a frame's bitrate.
   - A file shorter than its header's byte count has its length scaled by
     the share it holds (section 7).
   - `mp3MsLeft()` goes.
7. **FLAC's resume point saves its sample**, so libFLAC's seek is exact to
   the sample instead of to the millisecond.
8. **What a start shows.**
   - Exact plans (an anchor, the index of an exact run, CBR, FLAC) show
     the true time.
   - A TOC start shows the time asked, as today. Its error (p95 0.7 s with
     the model) stays in the time shown until the next exact start.
     Nothing in the file can correct it.
   - A resume of such a run is exact in the audio and continues the time
     as it was shown.
9. **Anchors only with gapless trimming on** (`G1` and `Gt1`, the defaults
   after every boot). With `G0` or `Gt0`, `resumeAnchor()` gives none and
   a start uses no anchor.
10. **Not built** (section 6.6):
    - a seek index cached per path on the card;
    - a header walk of the whole file;
    - a corrected time after a TOC landing;
    - iTunes' `iTunSMPB`;
    - changes to VBRI.

## 4. The mechanism

### 4.1 The cursor (`src/audio/PinnedMp3`)

`PinnedMp3` gains one accessor, on the generator's protected state:

```cpp
// The frame the sample offered next comes from: its file offset, the
// sample's index in it, and the frame's bytes (in the generator's buffer:
// a hash of its first 32 bytes is taken from there). False before the first
// frame is decoded (the generator's lead {0,0}).
bool cursor(uint32_t* frameByte, uint32_t* sampleInFrame, const uint8_t** frame) const;
```

- It is valid while `1 ≤ nsCount ≤ nsCountMax`. `begin()` sets 9,999,
  `DecodeNextFrame()` sets 0, and the first `GetOneSample()` of a frame
  makes it 1.
- Every sample the generator hands `ConsumeSample()` has been through
  `GetOneSample()`. That includes the one re-offered at the top of
  `loop()`, whose state hasn't changed. So the cursor describes exactly
  the sample being offered.
- At the end of a pass, when `loop()` returned because a sample was
  refused, the cursor describes the refused sample: the next one to go in.
- The fallback generator (`makeMp3()` without the arena) must have the
  cursor too. Both become one subclass, `PinnedMp3`, with the arena
  optional. Without the arena it allocates the frame and synth state
  itself, as ESP8266Audio did. The cursor only reads members, so it costs
  nothing when unused.
- **A known limit, from the library.** `Input()`'s resync shift (junk
  before a header in a refill) moves the buffer but not `lastReadPos`.
  After junk, the cursor is off by the junk's length until the next
  refill. Plans only hand the decoder frame starts, and the files have no
  junk between frames (2.5: 0 of 412,324). So this only shows up in a
  damaged file, and there it shows as a landing that misses (4.2), never
  as a wrong exact claim.

The cursor's interface is portable (lib/core `FrameCursor`), so `TrimFeed`
and the host tests can use it:

```cpp
class FrameCursor {
public:
  virtual bool at(uint32_t* frameByte, uint32_t* sampleInFrame) const = 0;
protected:
  ~FrameCursor() = default;
};
```

### 4.2 The landing: `TrimFeed::armAt()`

```cpp
// A start by a plan: every frame is dropped until the cursor says the frame
// at `landByte` (`landLength` bytes long, `spf` samples), then `skip` more,
// then as arm(0, hold).
void armAt(const FrameCursor* cursor, uint32_t landByte, uint32_t landLength, uint32_t spf,
           uint32_t skip, uint32_t hold);
enum class Landing : uint8_t { None, Waiting, Exact, NextFrame, Elsewhere };
Landing landing() const;
uint32_t lateBy() const;  // NextFrame: samples later than planned (the start moves by that)
uint32_t kept() const;    // samples accepted since the landing (into the feed or the hold)
```

**The landing phase.** For every offered sample, `consumeTrimmed()` asks
the cursor:

| The cursor says | What happens |
|---|---|
| no frame yet (the lead), or a frame before `landByte` | dropped (`return true`), counted as skipped |
| `landByte` | landed: from this sample on, the count skip (`skip`), then the hold, as `arm()` |
| `landByte + landLength`, the next frame (the landing frame was lost: damaged data, a reservoir the preroll didn't cover) | landed one frame late: `skip ≥ spf` keeps `skip − spf` (still exact); else `lateBy = spf − skip`, skip 0 |
| anything else past `landByte` (a resync elsewhere) | landed where it is, skip 0; the plan is now inexact (logged) |

- Once landed, nothing changes for the rest of the track. The cursor is
  not asked again, and `TrimFeed` is as cheap as after `arm()`.
- The landing phase lasts the preroll: 2-3 frames, rarely up to ~12
  (section 2.4). It costs one virtual call per sample during those frames,
  once per start.
- **The generator's first word.** ESP8266Audio says its rate after its
  first decoded frame, which is now a preroll frame. Nothing is held then
  (everything is dropped), so the first word goes straight to the feed, as
  the fixed rule says (GAPLESS.md 4.4). The end hold starts at the
  landing.
- **The lead** is dropped by the landing phase (the cursor says no frame
  yet), so a plan's `skip` is only the samples in and after the landing
  frame.
- **`kept()`** counts every sample accepted after the landing (or after
  the start skip for `arm()`), and also in the inactive fast path:
  `if (!active_) { if (feed_.consume(s)) { ++kept_; return true; } return false; }`.
  That is one increment per sample in internal RAM (about 0.05 % of a
  core at 240 MHz). The run index needs it (4.3).
- `TrimFeed` gains about 24 B (the cursor pointer, the landing byte,
  length, spf, state, `kept_`, `lateBy_`). `RingOutput` stays well under
  its 4 KB `static_assert`.

### 4.3 The run index (lib/core `SeekIndex`)

A **run** is one decoder's pass through one file: from its start (top,
plan or anchor) to its end, a request or a cut.

- Each run has a header:
  - the kind (MP3 or FLAC);
  - the path hash and the file size;
  - the rate and spf;
  - the first audio byte;
  - the base: the trimmed sample of the run's first kept sample, int64;
  - whether the base is exact;
  - the plan that started it.
- An MP3 run also keeps a ring of entries:

  ```cpp
  struct Entry {
    uint32_t byte;  // a frame's file offset
    int32_t t0;     // its first sample on the run's timeline (source rate, trimmed; < 0 inside the start trim)
    uint32_t hash;  // FNV-1a of its first 32 bytes (the header and side information)
  };
  ```

  The ring holds 2,048 entries of 12 B (24 KB of PSRAM). It overwrites
  the oldest, so it holds the last 8,192 frames: 3.6 min at 44.1 or
  22.05 kHz, 9.8 min at 8 kHz. Two rings, allocated once at `begin()`, are
  the two slots: the **heard** run and the **decoding-ahead** run.

**Recording**, on the decode task:

1. After each `produceDecoded()` pass that ends on a refused sample, and
   once `TrimFeed` has landed, the cursor gives the frame's byte, the
   sample's index in it and the frame's bytes.
2. The next sample's place on the timeline is `base + trim.kept()`.
3. So `t0 = base + kept − sampleInFrame`. An entry is written if this
   frame is at least 4 frames after the last entry's (`t0 ≥ last.t0 +
   4·spf` and `byte > last.byte`), or if it is the run's first. The hash
   is taken over the frame's first 32 bytes, from the generator's buffer.
4. A pass hands over at most 1,024 samples (`kChunkFrames`), so every
   1,152-sample MPEG-1 frame holds at least one pass's end. Entries are therefore exactly every 4th frame, or the
   frame after when a pass skipped one (MPEG-2's 576-sample frames). The
   lookups below don't need them regular.
5. Writing an entry takes the index's mutex. Reading the last entry, which
   only the decode task writes, doesn't.

**Lookups**, from any task, under the mutex:

- **`plan(t)`**, a start at `t` inside what the run has decoded:
  - the landing is the entry with the largest `t0 ≤ t`, skip `t − t0`;
  - the preroll is the latest entry at least 2 frames and 1,024 bytes
    before it;
  - if there is none, use the run's own plan's preroll when it lies
    before the landing, or the first audio byte when the run started from
    the top;
  - if neither applies (the oldest entries were overwritten), there is no
    plan (the next source, 4.5).
- **`anchor(t)`**: the same, and its hash and the run's header make the
  anchor (4.4).
- `t` past the last entry's frame plus 4 frames isn't covered: the
  decoder is always ahead of the reader, so a pause never asks that.

**The slots and gapless playback** (section 8):

- A request (play, stop, the benches) runs the lookup for its own path
  first, then resets both slots. The new run records into one.
- A join's run records into the other.
- `takeAdvance()` swaps the roles under the index's mutex, in the same
  call that rebases the book.
- A cut clears the decoding-ahead slot.
- A stop doesn't clear the slots. A track let go while paused (a `qs`
  while paused) keeps its index until the next play claims a slot, so
  that play can still use it.

**FLAC runs** have the header only: libFLAC seeks by sample itself. A
built-in track has no run (it counts exactly from its ms).

### 4.4 The resume anchor (lib/core `ResumeAnchor`)

```cpp
struct ResumeAnchor {
  enum class Kind : uint8_t { None, Mp3, Flac };
  Kind kind = Kind::None;
  bool exact = false;       // `sample` is the file's own time (else the time a TOC start showed)
  uint32_t rate = 0;        // the file's rate: the unit of `sample`
  uint64_t sample = 0;      // where it picks up, on the trimmed timeline (FLAC: the absolute sample)
  uint32_t fileSize = 0;
  // MP3:
  uint32_t prerollByte = 0; // the decoder is handed this frame's start
  uint32_t frameByte = 0;   // the landing frame's start
  uint32_t skip = 0;        // samples from the landing frame's first one to `sample`
  uint32_t frameHash = 0;   // FNV-1a of the landing frame's first 32 bytes
  // FLAC: frameHash holds STREAMINFO's total samples (low 32 bits); the byte fields are 0.
};
```

**`IAudioBackend` gains:**

```cpp
// Where a start begins: `ms` in (0: the top), `hintMs` its length as known
// elsewhere, and an anchor (Kind::None: none).
struct StartAt { uint32_t ms = 0; uint32_t hintMs = 0; ResumeAnchor anchor; };
virtual bool play(const std::string& path, const StartAt& at) { return play(path, at.hintMs, at.ms); }
// The heard track's anchor at the read position (a paused track's resume
// point). False: none (a built-in track, G0/Gt0, nothing held).
virtual bool resumeAnchor(ResumeAnchor* out) const { (void)out; return false; }
```

The default bodies keep the host fakes compiling unchanged.
`Core2AudioBackend` implements both, and its old `play()` becomes
`play(path, {ms, hint})`.

### 4.5 Planning a start: the order

`prepare()` asks `trackseek::plan()` (portable) with:

- the probe buffer and its offset;
- the file size and the first audio byte;
- the LAME info and the target as a sample on the trimmed timeline:
  `t = ms × rate / 1000`, or the anchor's sample;
- the hint;
- the anchor, and a view of the index slot when its run is this file's
  (path hash and size).

The first source that gives a plan wins:

| # | Source | When | The plan | Exact |
|---|---|---|---|---|
| 1 | the anchor | it passes its checks (5.4) | its bytes and skip | its own flag |
| 2 | the run index | its run is this file, `t` is inside it | 4.3 | the run's flag |
| 3 | CBR arithmetic | an Info header, or no header, and the first frames agree on their bitrate | 6.2 | yes |
| 4 | LAME's TOC inverted | Xing (VBR) with frames, bytes and a TOC; LAME extension trusted, encoder "LAME"; the TOC doesn't decrease | 6.3 | no |
| 5 | the Xing TOC (straight lines) | other encoders' Xing TOC | today's byte, then the chain walk (6.4) | no |
| 6 | VBRI | a VBRI TOC | today's byte, then the chain walk | no |
| 7 | the average bitrate, or the hint's | as today | today's byte, then the chain walk | no |
| — | from 0:00 | the tail rule (7); a VBR file with nothing to place it by; no frame found | — | — |

`[audio] MP3: starting 1:23.456 in (the run's index; exact): byte 2343590,
frame 2345678 + 517 samples` replaces today's line, with the source and
whether it is exact. The reads stay as today: the 4 KB header probe, then
one 4 KB read at the estimate for the chain walk (an anchor: two small
reads instead, 5.4; the index and CBR need the walk's read only to check
the frames).

## 5. Resume, end to end

### 5.1 Taking the anchor at a pause

`Core2AudioBackend::resumeAnchor()`, on the loop task, reads the heard
slot, the book and `readPos()`:

1. The ring frames played: `min(readPos, B) − heardStart`, as
   `positionMs()` counts them (held at B while a join waits).
2. The sample: `T = base + ⌊ringFrames × rate / 44100⌋`. At 44.1 kHz,
   ring frames are source frames, so it is exact. At other rates it is
   within one source sample: ring frame *n* sits at exactly *n* / 44100 s
   of the source (RESAMPLER.md section 5).
3. MP3: `index.anchor(T)`. FLAC: `{Flac, sample = T, rate, size, total}`.
   An anchor is given only while the generation matches the book's (a
   request under way gives none) and with `G1` and `Gt1`.

`PlaybackController::resumePoint(ms, length, anchor*)` returns the start
point's anchor while a start point waits. For a paused held track it
returns `audio_.resumeAnchor()`. `stopKeepingPlace()` keeps the backend's
anchor with the start point it sets. The start point owns the anchor:
both go together (next, another entry, an edit that changes the entry, a
`qs` that sets a new second without one).

### 5.2 Saving it: the resume blob, version 2

`QueueResume` gains the anchor. `QueueSaver::Transport` gains it too, from
`resumePoint()`.

**The save rule.** As today, with one addition: a resume point is also
saved again when its anchor's sample changed, however little.

- While paused, the read position moves only during the pause's own fade:
  the outputs read 64 more frames, 1.5 ms. So this adds at most one NVS
  write per pause, when the first save came in the same pass as the
  `pause()`.
- The saved point is then the sample the in-RAM resume would continue
  from (the frame after the fade).

**The blob** (`NvsLayout`, little-endian, 64 bytes):

| Offset | Field |
|---|---|
| 0 | version: 2 |
| 1 | the anchor's kind: 0 none, 1 MP3, 2 FLAC |
| 2 | flags: bit 0 exact |
| 3 | 0 |
| 4-23 | generation, entry, path hash, position (ms), length (ms): version 1's five words |
| 24 | file size |
| 28 | rate |
| 32 | sample (u64) |
| 40 | preroll byte |
| 44 | frame byte |
| 48 | skip |
| 52 | frame hash (FLAC: total samples, low 32 bits) |
| 56-63 | 0 (reserved) |

- `kResumeVersion` becomes 2 and `kResumeMaxBytes` 64.
- Versions 1 (24 bytes) and 0 (beta.1's 20 bytes) are still read, with no
  anchor. Any other size or version is not read, as today.
- **The schema stays 1.** NvsLayout's rule allows a blob that carries its
  own version byte to gain a version without a bump, as long as every
  older version is still read. The schema text (`NvsLayout.h`,
  ARCHITECTURE.md "NVS: the rules") then says schema 1's resume blob may
  be version 0, 1 or 2. No key is added, renamed or reused.
- **A downgrade** to a firmware that reads only version 1 (v0.5.0) finds
  no resume point, once. The same happened going from v0.5.0 back to
  beta.1.
- **Rejected: a second key for the anchor.** It would keep the downgrade's
  millisecond, but two writes per pause aren't atomic, and the pairing of
  the two blobs would need checks of its own.

### 5.3 At boot and at the play

1. `QueueStore::restore()` reads version 2. `resumeApplies()` is unchanged
   (generation, line, path hash).
2. `player_.setStartPoint(ms, length, &anchor)`. Now Playing shows the
   second, as today. `remap()` (a library rebuild) carries the anchor with
   the start point.
3. The play: `audio_.play(path, StartAt{ms, hint, anchor})`.
4. `prepare()`: the anchor is checked (5.4) and becomes plan 1 (4.5).
   `beginPrepared()` seeks the file to the preroll byte (`GuardedSource`
   re-arms its guard at the seek) and arms
   `TrimFeed::armAt(cursor, frameByte, landLength, spf, skip, hold)`.
5. The start's `landedMs` is `⌊sample × 1000 / rate⌋`, for `positionMs()`
   and the book. The run's base is `sample` exactly, plus `lateBy` if the
   landing came a frame late.
6. The run records its index as any run does. So a pause later in this
   run anchors exactly again: a chain of resumes never drifts.

### 5.4 Checking the anchor, and the fallback

An anchor is used only if all of these hold:

- the file's size equals the anchor's;
- at `frameByte`: a Layer III header at the anchor's rate, whose first 32
  bytes hash to `frameHash`, followed right after its length by a header
  of the same version and rate;
- at `prerollByte`: a header of the same version and rate,
  `prerollByte < frameByte`, `frameByte − prerollByte` under 64 KB, and at
  or after the first audio byte (a preroll never decodes the Info frame);
- `trackseek::startMs(sample in ms, length)` isn't 0. The tail rule
  applies to a resume as to any start: one saved in the last 5 s starts
  at 0:00, as today.

These need two small reads: 4 bytes at the preroll, and up to 1,441 + 4
bytes at the frame. The 4 KB probe for the header stays as it is.

**Any failure** logs `[audio] MP3: the resume anchor isn't this file's
(the size: 8737445 → 8737060): by its second` (or "the frame at 2345678",
"the preroll", "in its last 5 s"). The start then goes by the
millisecond through sources 2-7. Typical failures:

- a re-tag that changed the tag's size (every offset moves);
- a replaced file;
- a card from another unit with the same path.

**What it can't tell apart:** a different file of the same size with the
same 32 bytes at that offset. A re-tag that keeps the size keeps the audio
at the same offsets, so the anchor is still right there.

A FLAC anchor is checked by its size, its STREAMINFO rate and total
samples. libFLAC's seek then goes to `sample`. A seek that fails opens
the file again from the top, as today.

### 5.5 How exact

- **MP3, any bitrate mode:** the first sample after the start is the
  paused sample, bit for bit. The device's decoder, on the PC, gave the
  same output as the decode from the top in 8,000 of 8,000 trials
  (section 2.4).
- **FLAC:** exact to the sample (libFLAC's own seek).
- **What is heard:** the `DeclickReader` fades the start in over 64 frames,
  as at every start.
- **The time shown:** the sample in ms, rounded down: under 1 ms.

## 6. Other seeks (`qs`, a scrubber)

### 6.1 Inside the run: the index

A start at `t` that the run's index covers (4.3) is exact. With `qs` that
means a second earlier in what has played, or a second already decoded
ahead. With a scrubber it is any position the run has passed in its last
3.6 min.

### 6.2 CBR: arithmetic

For an Info file, or one with no header, whose first frames agree on their
bitrate:

- `L` = the frame length with its fraction (2.6). `D = t + trimSkip` is the
  sample in the decoded stream (`trimSkip` = delay + 529 with a trusted
  LAME tag, else 0). `k = ⌊D / spf⌋` is the landing frame, `skip = D −
  k·spf`.
- The landing byte is the header found within ±2 bytes of `firstAudio +
  ⌊k·L⌋` such that `round((byte − firstAudio)/L) = k` and the next header
  follows (same version and rate).
- The preroll is the same for frame `k − max(2, ⌈1024/L⌉)`, clamped at
  the first audio frame.
- The plan is exact. If the header isn't there (the file isn't the CBR its
  first frames say), the plan falls through to the next source.

This replaces today's CBR branch: no more 26-52 ms behind.

### 6.3 VBR: LAME's TOC inverted

For a Xing header with frames, bytes and a TOC, a trusted LAME extension
whose encoder string starts with "LAME", and a TOC that doesn't decrease:

```
N         = the Xing frame count (audio frames)
want      = 2^m, the smallest with ⌊N / 2^m⌋ < 400        (LAME's bag after N frames)
pos       = ⌊N / want⌋
F_i       = (⌊i · pos / 100⌋ + 1) · want,  i = 1..99      (frames before TOC point i's byte)
audio     = the Xing byte count − the header frame's length
B_i       = (toc[i] + 0.5) / 256 · audio                  (the truncation undone)
points    = (0, 0), (F_i, B_i)..., (N, audio); a point whose F doesn't grow replaces the one before
x         = (t + delay + 529) / spf                       (frames into the decoded stream)
estimate  = firstAudio + the straight line through the points at x
```

- **The chain walk.** Read 4 KB from `estimate − 2,560` (clamped at the
  first audio byte). Find the first clean frame (a header followed by
  another), and walk headers from there.
  - The landing is the first frame at or after the estimate.
  - The preroll is the latest frame in the chain at least 2 frames and
    1,024 bytes before it.
  - If there isn't one, take the chain's first frame. The landing frame is
    then still decoded unless it was lost, and the landing phase's
    one-frame-late rule covers that.
  - 2.5 KB back covers two frames at 320 kbit/s (2,088 bytes).
- `skip = 0`. The plan is inexact; the time shown is `t`.
- Measured: p50 242 ms, p95 735 ms, max 1.6 s, against 396 ms, 1,317 ms
  and 2.4 s today (2.2).
- `want`'s closed form matches LAME's loop (2.3: *One More Time* 12,284
  frames gives want 32, pos 383; *Nightvision* 4,001 gives 16, 250).

**For other encoders.** FFmpeg's encoder ("Lavf"/"Lavc") is believed to
fill its TOC the same way. That wasn't checked: the card has no such file.
Until a file is, they keep today's straight lines (source 5).

### 6.4 Everything else

The Xing TOC of other encoders, VBRI, and the average bitrate (by the
header or the hint) keep today's byte. Then comes the chain walk of 6.3, so
the landing frame is decoded (a preroll before it), instead of being the
one libmad drops.

### 6.5 What the time shown means

| Start | The time shown | Error |
|---|---|---|
| a resume by its anchor | the paused sample | 0 (an inexact run's: its shown time continues) |
| inside the run, by its index | the time asked | 0 in an exact run |
| CBR | the time asked | 0 |
| FLAC | the time asked | 0 |
| VBR, LAME's TOC inverted | the time asked | p95 0.74 s, max 1.6 s (2.2) |
| VBR, other TOCs, the average | the time asked | as today |

An exact run is one that started from the top, by an exact plan, or by an
exact anchor. Its index and anchors are exact. A run that started by a TOC
carries the TOC's error in its timeline. Its resume points are exact in
the audio (the same sample plays), and the time shown continues as it
was.

### 6.6 Not built, and why

- **A seek index per path, cached on the card.** 5-25 KB per track played,
  written as tracks end, with an invalidation of its own. It would make
  seeks into a track played before exact. It means writes during
  playback, which the queue saver avoids on purpose, and its value is
  mostly a scrubber's, which doesn't exist yet.
- **A header walk of the whole file** (an exact index of any track before a
  seek). It reads almost every sector: a 9 MB file is about 7 s of SD time
  at ~1.2 MB/s, competing with the decoder's reads and the battery, for
  each track seeked.
- **Correcting the time shown after a TOC start.** Nothing in the file says
  where the landing really is, except a decode from the top.
- **iTunes' `iTunSMPB`**, a TOC for VBRI (its entries already count frames
  exactly), and Lavf/Lavc's TOC model (6.3): later, each with a file to
  check it on.

## 7. The tail rule

`trackseek::startMs(t, length)` stays the one rule: a start in the last
`kTailMs` (5 s), or at or past the end, starts at 0:00. Only the length
changes:

- **An MP3 with Xing or VBRI frames:** frames × spf, less LAME's delay
  and padding when the tag is trusted (`lametag::lengthMs()`, as today).
- **Truncated: a file shorter than its header says.** When
  `fileSize − firstAudio < audioBytes − 4 KB` (the Xing or VBRI byte
  count), the length is scaled by `(fileSize − firstAudio) / audioBytes`.
  The 4 KB slack covers the *Discovery* files, whose byte count runs
  380-404 bytes over their stream (2.3).
- **CBR without a header:** the audio bytes at its bitrate (exact).
- **VBR without a header:** the hint, as today.
- **FLAC:** STREAMINFO's total samples.

`mp3StartByte()`'s second check (`mp3MsLeft()` at the landed frame's
bitrate) goes, and `mp3MsLeft()` with it (its only caller). After the
plan, the landing only has to be a frame of the chain.

The truncated-file case it guarded is now the scaled length. A plan in a
file that ends before its header says still finds no chain there, and
starts at 0:00.

- **The effect** on the 1,323 starts of 2.7: none goes to 0:00, because
  each is at least 5.2 s before the exact end.
- A TOC landing can be up to ~0.7 s (p95) off the time asked. A start
  asked 5.2 s before the end then begins at least ~4.5 s before it. That
  is not a start that ends at once, which is all the rule must prevent
  (the player would move on).
- `progress::mp3HeaderDurationMs()` (Now Playing's length) should scale a
  truncated file's length the same way, so the bar and the rule agree.
  This is a one-line change in the same commit.

## 8. How it fits the rest

**The trimmed timeline** (GAPLESS.md 4.6).

- Every sample here is on it: 0 is the first kept sample. Anchors and the
  index store it, and plans take it.
- Bytes are the file's own. A plan's skip is counted in the decoded
  stream from the landing frame, so it doesn't depend on the trim.
- The LAME tag gives the delay that maps `t` onto the decoded stream
  (`D = t + delay + 529`) for CBR arithmetic and the TOC.
- A preroll never starts before the first audio frame. The Info frame is
  never decoded, as with `G1` today.
- A resume point inside the start trim (`t` under one frame) lands on
  frame 0 or 1, with the first audio frame as preroll. That is exactly a
  decode from the top.

**`TrimFeed`** (GAPLESS.md 4.4).

- The landing phase sits in front of the count skip and the end hold.
- The end hold is armed for every plan start, as for every seek start
  today. A resumed track joins its next one gaplessly, padding cut.
- The first-word rule (fixed in "Gapless: device fixes") holds: the
  generator's rate comes with the first preroll frame, while nothing is
  held. A host test pins it (10).

**`LameTag`**: unchanged. `GaplessEngine`'s `Tracks::probe()` for a join
is a start from the top: no plan.

**`GuardedSource`**: re-armed at every seek (it is today). Plans and the
cursor are in file offsets; `getPos()` and `getSize()` pass through. The
tail rule keeps every landing far from the guard bytes.

**Decode-ahead and the join** (GAPLESS.md 3).

- The joined track's run records into the second slot, from the top: an
  exact run.
- Until the advance, the heard track's slot stays the heard one. A pause
  in N's last 1.5 s, with N+1 already decoded, anchors in N.
- At the advance the slots swap. A pause in N+1's first second anchors in
  N+1, whose run began at the join, so it is exact.
- A cut clears the second slot. The track joined after the cut records
  afresh.
- A track that was decoded ahead and is then resumed after a restart is
  just a file with an anchor. When it ends, its next track joins as usual.
- A short joined track that ends before it is heard waits for the word
  (GAPLESS.md 3.2), so two slots are always enough.

**Requests.**

- A request's own lookup (the index of the run that played this file)
  happens in `prepare()`, before the slots reset. So `qs` back into what
  has played is exact, and so is a `qs` while paused.
- The benches (`b<n>`) don't record: `openDecoder(..., trimmed = false)`
  never lands a plan.

**`PlaybackController`.**

- `setStartPoint(ms, length, const ResumeAnchor* = nullptr)`. A start
  point without an anchor (the console's `qs`) is as today.
- `startPoint()` and `resumePoint()` return the anchor.
- `startNow()` passes it to `play(path, StartAt{...})` and clears it with
  the start point.
- `stopKeepingPlace()` takes the backend's anchor.
- `prevRule()` and `playedFromMs_` are unchanged: they use the ms.

**`QueueSaver`/`QueueStore`.**

- The anchor rides in `Transport` and `QueueResume`.
- The re-save rule (5.2): `flushNow()` (the power-off) saves the newest.
- `restore()` and `remap()` carry it.
- `q` prints it: `[queue] resume point saved: 1:23.456 into 5 (generation
  12): MP3 frame at 2345678 + 517 samples, preroll 2343590, exact`.

**The converter** (RESAMPLER.md section 5).

- Samples are at the file's rate. Ring frames convert to them as in 5.1;
  the passthrough is exact. A 48 kHz MP3 or FLAC resumes to within one
  source sample (22 µs).
- No 48 kHz file is on the card, so this is host-tested only (10).

**Unchanged:**

- the fades (a start fades in), the sleep timer, `FadeStage`;
- the dancer (a start is a new epoch);
- `trackSeq()`;
- the frozen length at a file's end (counted from the start's ms, as
  today);
- `TransportSync`'s generations;
- HostLink.

## 9. Memory, CPU, flash

**PSRAM:**

- the two index slots: 2 × 24 KB = 48 KB, allocated once in `begin()`
  after the MP3 arena;
- nothing per track.

**Internal RAM:**

- `TrimFeed`, in `RingOutput`: +24 B.
- The run headers, in the backend (a global): about 2 × 80 B.
- `ResumeAnchor`: about 40 B each in `PlaybackController` (the start point),
  `QueueSaver` (`resume_`, `transport_`) and the `StartAt` passed by
  reference.

In all about 400 B of `.bss`. The `[heap] playing` figure must not move by
more than that; the ≥ 50 KB requirement over Bluetooth stands.

**CPU:**

| What | Cost |
|---|---|
| Every pass | a cursor read (four loads) and a compare |
| Every 4th frame | a 12 B PSRAM write and an FNV over 32 bytes (~100 cycles) |
| Every sample | the `kept_` increment (~0.05 % of a core) |
| Every plan start | the preroll's 2-3 frames decoded (~10-20 ms at 5x realtime) and the landing phase's cursor calls; plus, for an anchor, two small file reads |

`IRAM_ATTR` isn't used anywhere.

**NVS:**

- the resume blob grows from 24 to 64 bytes;
- it is written at the same moments, plus at most one write per pause
  (5.2);
- nothing is written while playing.

**Flash** (code): the cursor, `SeekIndex`, the plan sources and the LAME
model add perhaps 3-4 KB. `flash_guard` sees it.

## 10. Host tests (`pio test -e native`)

All synthetic: no real audio decoding in the native env. The PC tool
(section 12) checks the real libmad.

**The test helpers** (in `test/`, not lib/core):

- **A synthetic MP3 stream writer.** It writes frame headers at chosen
  bitrates, with padding as LAME pads, and side information with a chosen
  `main_data_begin`. The payload is pseudo-random bytes. It can add an
  Info or Xing header frame with frames, bytes, a TOC and a LAME
  extension (the delay and the padding).
- **A TOC writer by LAME's bag** (2.3), for the Xing frames above.
- **A model decoder.** It walks the synthetic stream like
  `AudioGeneratorMP3::Input()` plus libmad's reservoir rule: a frame is
  dropped when its `main_data_begin` exceeds the bytes held, and the
  preload goes as in `mad_layer_III()`. It emits spf samples per decoded
  frame, each a hash of (frame, index), through any `AudioOutput`-like
  sink. It implements `FrameCursor`, including the library's `lastReadPos`
  quirk after junk.

**test_track_seek** (new cases):

- LAME's TOC inverted:
  - the bag's `want` and `pos` against a loop over N frames, for N from 1
    to 200,000 (the closed form);
  - on synthetic VBR streams (quiet, loud and mixed stretches), the
    estimate's error against the true frame: under 1/256 of the stream at
    every 1 % point, and better than the straight lines on every stream;
  - a decreasing TOC, a missing flag, a non-LAME encoder: the straight
    lines.
- CBR arithmetic at 32, 44.1 and 48 kHz and MPEG-2/2.5, with LAME's
  padding pattern: the landing frame and skip exact for every millisecond
  of a 30 s stream; a stream that turns VBR after 4 KB falls through.
- The chain walk:
  - the landing is the first frame at or after the estimate;
  - the preroll is at least 2 frames and 1 KB back;
  - it is clamped at the first audio frame;
  - a false sync inside the audio data (0xFFE in the payload) is skipped.
- The tail rule:
  - starts 5.2-10 s before the end of synthetic VBR streams whose last
    frames are at 320 kbit/s start (today's *Crescendolls*);
  - 4.9 s before goes to 0:00;
  - a truncated file's length is scaled, and a start in its missing part
    goes to 0:00;
  - the *Discovery* case (a byte count up to 404 over) is not scaled.
- Today's cases stay, with the plan's bytes in place of the byte.

**test_trim_feed** (new cases):

- The landing phase:
  - the lead and the preroll are dropped;
  - the landing frame's sample `skip` is the first kept sample;
  - `skip ≥ spf` works;
  - a lost landing frame lands on the next (`lateBy`, or the skip less
    spf);
  - a resync elsewhere lands inexact;
  - a cursor that has no frame yet drops.
- The first word comes during the landing phase: the end hold stays on,
  and an anchored start's end drops the padding (the "Gapless: device
  fixes" bug, through the new path).
- `kept()` counts in the active and the inactive paths.
- After the landing, `active()` is as after `arm()`.

**test_seek_index** (new):

- `note()`:
  - one entry per 4 frames;
  - none before the landing;
  - a pass that skipped a frame;
  - the ring overwrites the oldest;
  - `t0` < 0 inside the start trim.
- `plan(t)` and `anchor(t)` for every `t` in a covered run:
  - the landing entry and skip;
  - a preroll at least 2 frames and 1 KB back;
  - the run's own plan, or the first audio byte, for the first entries;
  - none outside the ring.
- Two slots: a join records into the other, the advance swaps, a cut
  clears, a request resets after its lookup.
- An inexact run's flag reaches its anchors.

**End to end** (`test_seek_index`, with the model decoder over synthetic
CBR and VBR streams at 44.1, 48 and 22.05 kHz, with and without LAME
tags):

1. Play from the top through `TrimFeed` and the index.
2. "Pause" at 1,000 random samples and take the anchor.
3. Start a fresh model decoder from the anchor's plan.
4. Check: the first kept sample is the paused one, and the next 10,000
   match the top decode.

Also:

- the same after a start by an anchor (a chain of five resumes);
- after a TOC start (the audio continues exactly, and the shown base is
  kept);
- a pause in N's tail while N+1 is decoded ahead (through `GaplessEngine`
  and the `HostBackend` of test_gapless_player).

**test_nvs_layout** (new cases):

- version 2 written and read back, every field;
- version 1 (24 B) and version 0 (20 B) still read, with kind None;
- a version-2 blob of the wrong size, version 3 and 63 or 65 bytes are not
  read;
- the schema number stays 1.

**test_queue** (new cases):

- the anchor saved with the resume point;
- saved again once when only its sample moves (the fade), not again after;
- cleared with the resume point;
- `flushNow()` writes the newest;
- `resumeApplies()` unchanged.

**test_playback** (new cases):

- a start point with an anchor reaches `play(StartAt)`, once;
- next, another entry and an edit drop it;
- `qs` (no anchor) clears an older anchor;
- `stopKeepingPlace()` keeps the backend's;
- `resumePoint()` returns the held track's anchor while paused, and the
  start point's while one waits;
- the fakes without `resumeAnchor()` behave as today.

## 11. Device test plan

**The setup**, as in GAPLESS.md 11.7:

- silent mode `z` on the speaker; nothing goes to the headphones, and
  Bluetooth isn't needed;
- the user's queue noted first and restored at the end;
- COM3 through the serial daemon.

**The probe, `Sp` (temporary, like `Gp` and `Rp`; removed after the
run).**

- It turns the speaker's tap on (`setTapsOn(true)`).
- After the next start, it waits for the start's first real frame in the
  tap. The tap's segment says it, with its position.
- It dumps 8,192 tap frames from the 64th after it (past the fade-in) over
  the console, base64 (`Base64Text`, as the screenshots go): 16 KB of mono
  int16, about 22 KB of text.
- It logs the start's line (the plan, its source, exact or not), the
  landing (`lateBy`) and `positionMs()` at that frame.

**The reference, on the PC.**

- `devmad` (`groundtruth/maddec`) is the device's own decoder chain. Use
  it with the gapless trim applied: drop the Info frame's 1,152 samples,
  then the delay + 529.
- Or ffmpeg's gapless decode: `imageio-ffmpeg` 7.1 is in
  `groundtruth/venv`. The groundtruth notes show `devmad` matches it within
  0.4-0.6 LSB RMS after the 2,257-sample offset.
- FLAC: ffmpeg or libFLAC.
- Mix to mono as the tap does, `(L + R) / 2` in integers.
- **Without a PC decoder**, the device's own decode from 0:00 is the
  reference: the same file played from the top in silent mode, with `Sp`
  dumping the 8,192 frames at the same sample.

**The alignment.** This is RESAMPLER.md 6b's tap method, against a
decode instead of a tone:

1. Find the dump in the reference within ±3 s of the sample asked: the
   exact match, else the cross-correlation peak.
2. The lag is the landing error in samples.
3. After alignment, the residual is the difference.

**Pass criteria:**

| Start | Lag | Residual |
|---|---|---|
| MP3 at 44.1 kHz, anchor or exact plan, against `devmad` | 0 samples | 0 LSB (the same libmad, FPM_64BIT on both, a bit-exact passthrough) |
| the same, against ffmpeg | 0 samples | ≤ 1 LSB RMS |
| FLAC | 0 samples | 0 |
| any start | — | `positionMs()` at the first real frame equals the sample asked, in ms (±1) |

**The cases:**

1. **Resume, the user's path.** On a VBR MP3 (*One More Time*,
   *Crescendolls*), a CBR MP3 (Air's *La femme d'argent*) and a FLAC (a
   *Graduation* track), at ten points each:
   - random points;
   - 0.1-0.5 s after the track's start;
   - inside the first 0.2 s after a `qs` start (the run's own plan);
   - 6-10 s before the end;
   - in a track's last 1.4 s with the next one decoded ahead (`G`: the
     word taken);
   - 0.2 s after a gapless advance.

   For each point:
   1. Pause (space). `[queue] resume point saved` shows the anchor.
   2. Reset the board through the serial daemon, as in earlier runs. A
      reset saves nothing, so the pause must come first.
   3. `q` shows the start point and its anchor. Play (space) with `Sp`
      armed.
   4. Expect lag 0 and the residuals above, and `[audio] MP3: starting ...
      (its resume anchor; exact)`.
2. **The fallback.** The same resume after the anchor is corrupted. A
   temporary `Sa!` flips the saved hash, since the card can't be edited
   in the Core2.
   - Expected: `the resume anchor isn't this file's (the frame at ...): by
     its second`.
   - The start then goes as `qs` would.
3. **`qs` on VBR.** *One More Time* at 20 points. The lags should follow
   the offline model's errors for those points (2.2, C), within one frame;
   p95 under 0.75 s.
4. **`qs` on CBR.** Air at 10 points: lag 0. Today's 26-52 ms is gone.
5. **`qs` back into the run.** Play *One More Time* from the top for 90 s,
   then `qs30` and `qs85`. Both: `(the run's index; exact)`, lag 0.
6. **The tail rule.**
   - *Crescendolls*: `qs` at 6.6, 6.0 and 5.3 s before the end starts
     there (today 0:00).
   - At 4.9 s before, it goes to 0:00 with the "last 5 s" line.
7. **A resumed track's join.** A resume 20 s before the end of *Discovery*
   1. The join to 2 has 0 inserted frames and no silence, as GAPLESS.md
   11.7 measured. That uses `Gp`, if it is rebuilt for the run; otherwise
   the `G` counters and the `[gapless]` lines.
8. **Cost.**
   - The start's `[audio] refill` line: first audio within ~20 ms of a
     plain `qs` start.
   - `s`: the decode stack's minimum free, unchanged ±100 B.
   - `[heap] playing`: internal free unchanged ±0.5 KB.
   - The boot `[heap]` lines: PSRAM 48 KB lower.
   - `b0`: unchanged, since the bench doesn't record.
9. **NVS.** Ten pauses and resumes: one or two `[queue] resume point
   saved` lines per pause, none while playing.

**Not on this device:**

- a 48 kHz MP3 (none on the card; host-tested);
- a file with junk between frames;
- Lavf and VBRI files;
- Bluetooth (unchanged by construction; only silence may go to the
  headphones).

## 12. Build order

One commit each, host tests with each, committed locally:

1. **The tail rule** (TrackSeek's length rule, the truncated-file scale,
   `mp3MsLeft()` removed; TrackProgress's length scaled the same way).
   This is small and independent, and fixes *Crescendolls*.
2. **The cursor, start plans and the landing** (`FrameCursor`, `PinnedMp3`
   with the optional arena, `TrimFeed::armAt()`, `trackseek::plan()` with
   sources 3 and 5-7 and the chain walk, the backend's `prepare()`). CBR
   seeks become exact and no landing frame is lost.
3. **The run index and the anchors** (`SeekIndex`, `ResumeAnchor`, the two
   slots in the backend, `IAudioBackend::StartAt` and `resumeAnchor()`,
   `PlaybackController`, `QueueSaver`, `NvsLayout` version 2,
   `QueueStore`). Exact resume, and seeks back into the run.
4. **LAME's TOC inverted** (source 4).
5. **The device run** (section 11) with the temporary `Sp` (and `Sa!`),
   removed after it. Then the docs (section 14).

**A PC tool.** The libmad harness behind section 2 is worth keeping under
`tools/seek_check/`:

- `seekmad.c` against the tree's libmad;
- the TOC and tail-rule scripts.

Rerun it whenever ESP8266Audio is updated: the cursor reads its protected
members, and the reservoir rule is libmad's. Whether to commit it is the
user's call.

## 13. Risks and open questions

- **The cursor reads ESP8266Audio's protected members** (`lastReadPos`,
  `buff`, `stream`, `samplePtr`, `nsCount`, `nsCountMax`). An update that
  renames them breaks the build, which is visible. An update that changes
  their meaning breaks exactness silently, so the PC tool (12) is the
  check.
- **LAME's TOC model is from memory and reproduction, not from reading
  `VbrTag.c`.** It reproduces 13 LAME 3.98 files exactly, and 3.97b files
  within 1-2 256ths. Read the source when building it.
- **An edited file whose TOC was computed on another stream** can seek
  worse with the model: *Nightvision*'s p95 is 460 ms against 267 ms with
  straight lines. Such a file can't be recognized cheaply. All 14
  *Discovery* files are edited, and 13 still seek better.
- **The resync quirk** (4.1): in a damaged file, a landing can miss and
  become inexact. The landing phase handles it, and it is logged.
- **The 12.4 h limit.** `t0` is int32 at the file's rate (13.5 h at
  44.1 kHz, 12.4 h at 48 kHz). The blob's sample is u64. A longer file's
  index stops past that; its anchors fall back to the millisecond.
- **A frame lost in the middle of a file** (bad data). The output-based
  timeline then runs one frame behind the file's frame count from there
  on, as `positionMs()` does today. Anchors stay exact, because they
  record what was output.
- **A downgrade loses the resume point once** (5.2).
- **Not measured on the device yet:** everything in section 11. The
  exactness claim rests on the host build of the same libmad (2.4, 2.5).
  The device run confirms the firmware's wiring and that the ESP32's
  libmad behaves the same.

## 14. The other docs, when built

- **ARCHITECTURE.md, "Audio pipeline":** the paragraph on starting part of
  the way in (plans, sources, accuracy; the CBR "30-50 ms behind" becomes
  exact; the VBR figure becomes 2.2's).
- **ARCHITECTURE.md, "Library and queue":** the resume point's anchor.
- **ARCHITECTURE.md, "NVS: the rules":** the resume blob's versions 0-2
  under schema 1.
- **GAPLESS.md:**
  - 4.4: `TrimFeed`'s landing phase;
  - 4.6: seeks on the trimmed timeline, exact for anchors and CBR;
  - 6: PSRAM, the index slots;
  - 12: the seek bullet points here.
- **RESAMPLER.md, section 5** ("Positions and durations"): a resume to the
  sample.
- **Header comments:**
  - `TrackSeek.h` (the plan, the sources, the tail rule);
  - `TrimFeed.h`, `IAudioBackend.h`, `QueueSaver.h`, `NvsLayout.h`;
  - `PinnedMp3.h`, `Core2AudioBackend.h`.
- **The README's lib/core list:** `SeekIndex`, `ResumeAnchor`,
  `FrameCursor`.

## 15. Sources

1. libmad, `layer3.c` (`mad_layer_III()`: `main_data_begin`,
   `MAD_ERROR_BADDATAPTR`, the reservoir preload) and `stream.h`
   (`MAD_BUFFER_MDLEN`), in this tree's
   `.pio/libdeps/core2/ESP8266Audio/src/libmad`.
2. ESP8266Audio, `AudioGeneratorMP3.cpp` (`Input()`, `DecodeNextFrame()`,
   `GetOneSample()`, `loop()`, `ErrorToFlow()`'s byte offset), in the same
   tree.
3. LAME, `libmp3lame/VbrTag.c` (`AddVbrFrame()`, `Xing_seek_table()`: the
   bag and the TOC). Recalled, and reproduced on the files of 2.3; to be
   read when building section 6.3.
4. The Xing header's TOC as specified (point *i* is the byte at *i* % of
   the time, in 256ths of the bytes): GAPLESS.md section 14's [1].
5. The measurements of section 2: the `groundtruth` copy of the card
   (`groundtruth/tracklist.txt`), `groundtruth/maddec` (the device's
   libmad on the PC; `groundtruth/summary.md`, "MP3 priming"), and this
   design's scripts (`seek/`: `toc_error.py`, `toc_variants*.py`,
   `cbr_preroll.py`, `tail_rule.py`, `seekmad.c`, `run_seekmad.py`,
   `cursor_check.py`).
