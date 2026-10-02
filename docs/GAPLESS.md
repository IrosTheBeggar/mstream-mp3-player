# Gapless playback

When a track ends by itself, the next one should start on the very next
sample: no gap, no fade, no click. Albums whose tracks run into each other
(Daft Punk's *Discovery*, live albums, DJ mixes, classical movements) then
play as one piece. This document is the design and how it was built:
what happened before, the mechanism, the MP3 trimming rules with their
sources, what happens when the listener changes something while the next
track is already decoded, the portable pieces and the firmware wiring, and
the host and device test plans.

**Status: built, host-tested, not yet run on the device** (the commit
"Gapless playback", after the design at 5d7d6ba and its review; section 2a
lists the review's amendments and what became of each). Sections 1 and 2
are the design as it was reviewed; from section 3 on the text describes
what was built. `G0` on the console turns all of it off (section 9): that
is the v0.5.0 behaviour, for the A/B and as a safety valve.

Where the facts come from:

- **The code**, by file and function. Line numbers drift, so they're given
  only where they help.
- **Primary sources for the MP3 rules**, cited where they are used
  (section 4) and listed in section 14. The LAME tag's layout and its CRC
  were also checked against 13 real LAME 3.99r files on the development
  PC (section 4.1).
- **Nothing here is measured on the device yet.** Section 11 says what to
  measure there and what would change the design.

## 1. What happened before (v0.5.0)

Here is what happened at a natural end, step by step (and still does
with `G0`):

1. The decoder reaches the end of the file. `produceDecoded()` sees
   `loop()` return false and sets `sourceDone_`. `finishSource()` pushes
   the converter's tail (`RingFeed::finish()`) and closes the decoder.
2. `decodeTask()` reports `Phase::Draining`, clears `ringSteady_` and
   `expectingAudio`, and polls every 10 ms until `ring_->size() == 0`.
   Then it reports `Phase::Ended`.
3. On the loop task, `PlaybackController::update()` sees `finished()`.
   It calls `advance()`, which steps the queue (`QueueModel::step(+1,
   repeat_)`) and then `startCurrent()` and `audio_.play()`.
4. `play()` posts a new generation (`TransportSync::post()`) and wakes the
   decode task.
5. On the decode task, `start()` closes the decoder and calls
   `discardAll()` on the ring (which is already empty), which bumps its
   epoch. It resets the converter, opens the file and fills the ring from
   empty. The refill is paced by `RefillPacer`; the UI stalls for
   0.6-0.8 s.
6. In each output, the `DeclickReader` sees the ring run dry at the end of
   the track. It fades the last frame to 0 over 64 frames (`Declicker`)
   and plays zeros. When the new track's frames arrive, it fades them in.
   The epoch bump makes that a crossfade.

So between two tracks there is the following:

- a fade-out of the held frame (1.5 ms);
- silence while all of these happen: the ring drains, the loop polls (up
  to one pass), the request is handed over, the decode task wakes, the
  file opens (MP3: 23-30 ms; FLAC: libFLAC's init), the first frames
  arrive, and the speaker waits to top up a 1024-frame buffer
  (`kTopUpWaitMs`);
- a fade-in (1.5 ms);
- for an MP3, the encoder's delay and padding (about 1,100 samples at the
  start and 500-1,500 at the end; section 4), plus the Info frame decoded
  as 1,152 samples of silence (section 4.3).

Before the converter, a change of rate forced the drain. Now the ring is
always 44.1 kHz (RESAMPLER.md), so the drain only exists because nothing
knows what comes next.

## 2. Decisions

1. **Decode ahead, one track deep.** When the decoder reaches track N's
   end of file, it opens the next track at once and keeps writing into
   the same ring, with no `discardAll()`. At that moment the ring holds
   up to 1.49 s of N. The next track is the queue's real next entry,
   which only `PlaybackController` knows (section 3.1). There is one
   decoder at a time, as today: N's generator is closed before N+1's
   opens. There is at most one pending boundary. If N+1 reaches its own
   end before N's boundary is heard (a track shorter than the ring), the
   decoder waits (as built: for the word about N+1, while the ring holds
   more than 250 ms; section 3.2).
2. **The boundary is heard, not decoded.** The ring frame where N+1
   begins is recorded (the boundary, B). The current entry, Now Playing,
   the progress, the durations, the resume point, the sleep timer and the
   dancer's track all switch when the consumer's read position
   (`PcmRing::readPos()`, moved by `BtSink`'s callback or `SpeakerSink`'s
   pump) passes B. That is the clock `positionMs()` already uses. Until
   then everything describes N, and N's position stops at its exact end
   (section 3.5).
3. **No fade at a join.** The ring never runs dry and its epoch never
   changes, so the `DeclickReader` sees one continuous stream and passes
   it through bit for bit (`Declicker`'s steady state). Starts, skips,
   seeks and pauses keep their fades as today.
4. **The converter runs on across a same-rate join.** There is no reset
   and no `finish()`: N+1's first source frame follows N's last in the
   same filter history, exactly as if the two files were one. Only a rate
   change at a join flushes N's tail (`finish()`) and resets the
   converter. That leaves a discontinuity of at most the filter's length
   (about 24 ring frames, 0.5 ms), saturated and never louder. Joins at
   44.1 kHz, the passthrough, have no filter at all.
5. **Trimming at the source rate, before the converter** (section 4), in a
   new portable stage, `TrimFeed`, in front of `RingFeed`. MP3s with a
   LAME/Xing Info tag lose the encoder delay plus the decoder delay at the
   start, and the padding minus the decoder delay at the end. FLAC and
   the built-in tracks are taken as they are. MP3s without the tag aren't
   trimmed, and the gap that remains is documented (section 4.5).
6. **Changes are cut out of the ring, never played through.** Section 5
   lists the changes that can come while N+1 is already decoded: an edit
   to what comes next, Shuffle all, a sleep-timer end, `G0`, and so on.
   The decode task cuts the ring back to the join with a new producer-side
   operation, `PcmRing::cutBack()`. That operation never makes a consumer
   miss a read. The decode task also restores the converter as it was at
   the join (`RingFeed::rewind()`) and decodes the right track. N's tail is
   never touched. If the consumer has already passed the join, nothing is
   cut: the join has been heard, and the change applies to what plays now,
   as any edit does.
7. **Requests stay generations.** A skip, prev, another entry, Play, stop
   or a start point posts a new generation and calls `discardAll()`, as
   today. That drops the decoded-ahead track and its boundary with
   everything else. A join is not a request: it posts no generation, and
   `TransportSync`'s rules are unchanged (section 3.6).
8. **A runtime switch** (`G0`/`G1`, RAM only, on by default; a build flag
   for the default) and a separate trimming switch (`Gt0`/`Gt1`) for the
   A/B (section 9).

## 2a. The review's amendments, and what was built

The design was reviewed before it was built. Each amendment, and what
became of it:

1. **The advance against a cut at the boundary.** Applied.
   `GaplessJoin::takeAdvance()` fires only strictly past B (`readPos - B >
   0`: a frame of the next track read); every transition of the boundary
   is under the book's lock; the decode task puts the boundary in
   `Cutting` before it raises the fence, and `takeAdvance()` ignores a
   boundary in `Cutting`; on Done the boundary goes before anything more is
   written, and a new one is recorded before its track's first frame.
   Host-tested with the reader at J = B exactly during a cut.
2. **A wakeable decode task.** Applied: every rest on the producing paths
   (the ring-full waits, the pacing, the drain's poll, the word's wait, a
   cut's retry) is `ulTaskNotifyTake()`, so `setNext()`'s notify ends it.
   The race window is restated in section 5.1; `G` shows each cut's time
   from its word.
3. **An advance is checked against what would happen now.** Applied, and
   it replaces the design's separate handling of a removed or a moved key
   (section 5.2): "pause after this track" or the timer ending here: the
   boundary's pause; what `advance()` would start isn't the joined entry
   any more: that is started; else the joined entry becomes current.
4. **A track count for the advance.** Applied: `Core2AudioBackend::
   trackSeq()` (every start taken up, every advance) feeds the sleep timer's
   `EntryStart` and the Queue's learned lengths; `EntryStart` compares the
   count with the one of the pass before the entry changed, so a count that
   moves in the same pass (an advance) counts. `startOffsetMs()`,
   `durationKnown()`, `description()` and `note()` are the heard track's.
   `trackTitle()`/`trackArtist()` had no callers and went with the ID3
   reader (8).
5. **The book cleared at every request; generations checked; no joins for
   a forced rate.** Applied: `GaplessEngine::begin()`/`idle()` restart the
   book for every request kind (play, stop, the benches); the position
   clamp and the advance check the boundary's generation; a play at a
   forced rate (the console's `Rf`) is never joined.
6. **Token to entry, and re-keying.** Applied in a simpler form: the
   player keeps its last four offers (token, key, track). The same track
   still next keeps its token when its key is gone (a library rebuild's
   fresh keys, one of two duplicates removed), so nothing is cut; at the
   advance the entry is matched by key, or by the track when the key is
   gone. Host-tested (a rebuild and a duplicate removed while decoded
   ahead).
7. **A resumable state machine; a cut from Draining.** Applied: the decode
   side of a join is `GaplessEngine` (lib/core), one step per call, the
   generation checked between steps; a cut runs from any phase while a
   boundary waits, Draining included (host-tested).
8. **The lead skipped on every start; the ID3 reader removed.** Applied:
   1 frame for every MP3 open and every FLAC seek, 0 for a FLAC from the
   top, except with `G0`; the ID3v2 tags (one after another too) are
   skipped to the first frame and the Info frame after them.
9. **The tables' copy never freed during a chain.** Applied: the firmware's
   hook ignores "not wanted" from the first join until the next request's
   start, so a mark's saved rows stay valid and `rewind()` is a plain copy.
10. **B from the converter's counts.** Applied: `B = J + tailFrames()`,
   `RateConverter::tailFrames()` being `ringFrames(taken) - produced` (the
   carried frames included; the held frames before a rate is known).
11. **A rate change in the middle with frames held.** Applied as the
   alternative: the held frames go into the feed at the old format first,
   then the change, and the end trim is off for the rest of that track.
12. **Failures after a join.** Applied: a joined track that fails after
   giving frames ends early (its note shown once it is heard); one that
   gives none is cut back out and the track before ends as before.
13. **A late word only with 250 ms left; the flags back.** Applied
   (`GaplessEngine::kWaitMinMs`); a late join reports `Decoding` again and
   sets `expectingAudio`.
14. **`held()` not a reason to withhold the word.** Applied: only Stopped
   and cued withhold it; an advance taken while held starts the entry as
   `advance()` would (Waiting).
15. **Depth one as "one unheard boundary", with a two-entry book.**
   Rejected: the player names the track after a joined one only once it
   has heard the join (the word carries the token of the track it
   follows), so the decoder can never be two boundaries ahead and a second
   entry could never fill. A track shorter than the ring waits at its end
   for the word while the ring holds more than 250 ms; under that it ends
   as before (host-tested both ways). A loop stall during such a short
   track can still cost that one join (section 12).
16. **The tag's CRC range per mode, and CRC-protected header frames.**
   Applied: the CRC covers every byte of the frame before it (190 for
   MPEG-1 stereo with all four Xing fields, 175 for MPEG-1 mono and
   MPEG-2/2.5 stereo, 167 for MPEG-2/2.5 mono); a protected header frame
   is accepted, its side information 2 bytes later. CRC-16/ARC and the
   190-byte range were confirmed on 13 real LAME 3.99r files.
17. **The word worked out only when its inputs change.** Applied: a
   signature of the queue's versions, repeat, "pause after", gapless, the
   gate and the heard token.
18. **FLAC with a big embedded picture at a join.** Applied: the open
   walks the metadata block headers and logs their size (over 256 KB at
   any open; over 1 MB at a join as an underrun risk); added to the
   device plan.
19. **More tests.** Applied on the host: a Crossed Play next (the inserted
   track plays next), a Crossed End of track (the pause comes at once), an
   advance taken late (`EntryStart` and the learned length still work), a
   re-key with a boundary waiting, and a two-thread stress test of the cut.
   On the device: the console's `Gx` runs the same stress test (lib/core
   `RingCutStress`) with the reader on core 0; and the build's
   disassembly was checked: GCC puts `memw` between the reading mark's
   store and the fence's load in `PcmRing::read()`, and between the
   fence's store and the mark's load in `cutBack()`. The impulse
   calibration aligns by cross-correlating a broadband burst (section
   11.1).
20. **Optional simplifications.** (a) Cutting the padding out of the ring
   at the passthrough instead of holding it: not taken for now (one
   mechanism, measured first; it stays the fallback if the hold's cost
   shows). (b) Flushing the hold at an early end: taken. (c) DanceMode's
   click truth turned off at a track change instead of re-armed at the
   join: taken. (d) The heard record under a lock: taken (the book's).

## 3. The mechanism, end to end

```
 loop task                                   decode task (GaplessEngine)          consumer (BtSink / SpeakerSink)
 ─────────                                   ───────────────────────────          ──────────────────────────────
 PlaybackController::update()                produceDecoded(): N decoding          reads N from the ring
   syncHeard(): takeAdvance()?                 ...
   refreshOffer(): the entry after             N's EOF ──► Ending:
   N ──setNext({after N, token, path})──►        TrimFeed::end(), commit, J = write index
                                                 freeze N's exact length
                                                 take(gen, N's token): Next
                                                   probe N+1's rate
                                                   same rate: mark(), B = J + tail
                                                   another rate: finish(), mark(),
                                                     restartStream(), B = write index
                                                   start N+1 (same file_ object)
                                                   joined({gen, after, token, J, B})
                                               produceDecoded(): N+1 decoding     ... still reading N
                                               (each pass: cutCheck(): the word
                                                for N changed? cutBack(J))
                                                                                   readPos passes B
   takeAdvance(): readPos > B ◄───────────── (GaplessJoin, its own lock)
   checked against what advance() would do:
   current := the joined entry, no play():
   Now Playing switches
   refreshOffer(): {after N+1, ...}
```

### 3.1 The word: the player says what comes next

`PlaybackController` (lib/core) is the only place that knows the queue's
real next entry. It works it out the way `advance()` would, without
moving anything (`QueueModel::peek(+1, repeat_)`), and hands it to the
backend as a word (`IAudioBackend::setNext()`):

```
Next { after, token, path, hintMs }
```

- **`after`** names the track the word is about: 0 for the track the last
  `play()` started, else the token of the joined track the player has
  heard (`heardToken_`). The decode task takes a word only for the track
  it is at the end of, so a word about the track before is never taken
  for the track after it.
- **`token`** names the entry offered: unique, never 0. Token 0 means
  nothing follows: the track ends as before gapless playback.
- **Only while the backend holds the current entry's track** (Playing,
  Paused, or Waiting to resume it). Stopped or cued, nothing is sent; the
  next `play()` comes first, and the word after a `play()` always has a
  new token (a token taken before that play is never taken again).
- **Nothing follows** when gapless is off (`setGapless(false)`, the
  console's `G0`), "pause after this track" is set (`pauseAfter_`), the
  `NextGate` says the sleep timer ends at the current entry (section 5.4),
  the queue ends without repeat, or the next id has no path. `held()` is
  no reason (the review's amendment 14): nothing is heard without an
  output reading, and a drop pauses the player anyway.
- **The same track still next keeps its token** (no cut), whether its
  entry stays or its key is gone and the same track took its place (a
  library rebuild's fresh keys, one of two duplicates removed). Never the
  heard token, though: the backend took it already and would answer
  "nothing follows", so a queue of one on repeat (the same entry after
  itself) gets a new token every time round. Anything else next gets a
  new token. The player keeps its last four offers
  (token, key, track) for the advance.
- **Worked out only when something it depends on changed**: a signature of
  the queue's position and content versions, repeat, "pause after", the
  gapless switch, the gate and the heard token. It is checked at the end
  of every `update()` and of every public action (an `Act` guard that also
  takes the heard advance first, section 3.5), and sent only when the word
  changed. Shuffle, repeat, the queue edits and the sleep timer need no
  hooks of their own: anything that changes the next entry changes the
  word.

There is no shuffle mode today. "Shuffle all" (`Ui::shuffleAll()`)
replaces the queue (`playNow()`), which is a new generation, and repeat
has no UI (`setRepeat()` is never called; it is on). If either is added
later, it only has to change what `peek()` returns.

### 3.2 Decode-ahead at the end of a file: `GaplessEngine`

The decode task's side is a portable, resumable state machine
(`lib/core/GaplessEngine`). `Core2AudioBackend::decodeTask()` drives it
through `GaplessEngine::Tracks` (probe, start, close: ESP8266Audio and the
files on the device; synthetic tracks in the host tests). When the
decoder or a built-in track reaches its end (`sourceEnded()`), each pass
is one `step()`, and the decode task looks at the request generation
between any two:

1. **Ending: the commit.** `TrimFeed::end()` (the padding dropped; at an
   early end the held frames go in instead), the stage into the ring
   (waiting for room), the decoder closed. J (`cutAt`) is the ring's write
   index. The track's exact length is frozen in the book: its start plus
   `(J + tailFrames() - its first ring frame) / 44.1`, which is where the
   next stream begins whether it joins or not.
2. **Ending: the word** (`GaplessJoin::take()`, for this track's token):
   - **Next**: the file is probed (`Tracks::probe()`: an MP3's first frame
     header, a FLAC's STREAMINFO, a built-in track's name) for its rate.
     If the converter is configured at that rate (`RingFeed::continues()`),
     a continuous join right away: the feed is marked (`RingFeed::mark()`)
     and B = J + `tailFrames()` (`RateConverter::tailFrames()`: the
     stream's `ringFrames(taken) - produced`; 0 at the passthrough). At
     another rate, or a rate the file doesn't say: Flushing first.
   - **Nothing**, or a probe that failed: Flushing, then Draining.
   - **NoWord**: the player hasn't spoken about this track yet (it speaks
     of a joined track only once it has heard it begin, section 3.5). It
     waits, 5 ms at a time and woken by `setNext()`, while the ring holds
     more than `kWaitMinMs` (250 ms); after that, as Nothing.
3. **Flushing**: `finish()` pushes the converter's tail, and the feed is
   marked with it in. For a join: `restartStream()` (a new stream at the
   same ring position, `made()` running on) and B = the write index. Else
   Draining.
4. **The join**: `Tracks::start()` begins the decoder with its trim armed,
   the boundary {generation, after, token, J, B} goes into the book before
   the track's first frame, and the engine is Producing again. There is no
   `discardAll()`, no new generation and no `startTiming()`; the decode
   task's per-track counters (`producedFrames_`, `srcPos0_`, `srcPos_`,
   `busyUs_`, `described_`, `knownDurationMs_`, `startMs_`) start again for
   the joined track, while the heard track's values are the book's
   (section 3.5).
5. **Draining**: until the ring is empty, then `Ended`. A late word for
   this track (+ Queue onto the last entry, the sleep timer turned off in
   the last second) is still taken while the ring holds 250 ms or more: a
   join after the tail. At 44.1 kHz that is seamless (the passthrough has
   no history); at a converting rate it is a fresh filter (section 12).
   `Decoding` is reported again and `expectingAudio` set.

The engine keeps two marks (`RingFeed::Mark`, ~2 KB each, in PSRAM): one
for the boundary that waits to be heard, one for the decoding track's own
end, since a short joined track can end before its join is heard.

Timing: the open happens with up to 1.49 s of N still in the ring, while
the decoder would otherwise only wait for room. An MP3 opens in 23-30 ms
(ARCHITECTURE.md; less now that no ID3 tag is parsed), and a FLAC's
libFLAC init is of the same order unless it has a big embedded picture
(section 12). After the open, the ring is still nearly full, so there is
no refill from empty: no `RefillPacer` window (`fullMs_` is already set)
and none of the old 0.6-0.8 s UI stall at a natural end.

### 3.3 Built-in tracks

A tone or click track ends when `ToneGen`/`ClickGen` returns 0 frames
(`produceTone()`), and the same end of source runs (section 3.2). The
built-in tracks are made sample-exact at their own rate, so no trimming
applies; the probe of a `tone:` word is its name (`ToneTrack::parse()`). A file to a tone, or a tone to a file, at different rates takes
the rate-change join. The test tones' own 5 ms attack and release make
their joins click-free by content.

### 3.4 The boundary book (lib/core `GaplessJoin`)

The state the loop and the decode task share fits in one small portable
class, `GaplessJoin`, with its own mutex (never the outputs': they only
read the ring). It holds:

- **the word**: generation, after, token, path, length hint (the loop
  writes it; the decode task takes it), and the last token taken, so a
  word is taken once;
- **at most one boundary**: generation, after, token, J (`cutAt`: where a
  cut goes back to), B (`heardAt`: where the listener's track changes), and
  its state: `Pending` (cuttable), `Cutting` (the decode task is cutting
  it), `Committed` (a cut came too late);
- **the heard track**: its first ring frame, where it started (ms: a
  resume point's landing, 0 for a joined track), and its exact length once
  its file has ended; the joined track's exact length waits with the
  boundary if its file ends before it is heard.

| Call | Task | What it does |
|---|---|---|
| `setOffer()` | loop | `setNext()`: replaces the word (the backend then wakes the decode task, `xTaskNotifyGive`) |
| `restart()` | decode | every request (play, stop, the benches): the boundary, the heard record and older generations' words go (a newer one stays: the loop may post a second request and its word while the decode task still starts the first) |
| `take(gen, after)` | decode | NoWord, Nothing, or Next (taken once) |
| `freeze(gen, ms)` | decode | the decoding track's file ended at exactly that length |
| `joined(b)` | decode | records the boundary (Pending) |
| `cutCheck(gen)` | decode | a boundary of this generation waits; it is cuttable; the word for the track before it now names another track or nothing |
| `beginCut()`, `cutDone()`, `cutCrossed()` | decode | Pending to Cutting; the boundary removed; Cutting to Committed |
| `takeAdvance(gen, readPos)` | loop | strictly past B, not Cutting: the heard record becomes the joined track's (start B, 0 ms, its length if frozen), the boundary goes, its token is returned, once |
| `positionMs(gen, readPos)` | any | the heard track's, held at B while a boundary of this generation waits |
| `startMs()`, `frozenLength()` | any | the heard track's start and exact length |
| `status()` | any | the console's `G` |

### 3.5 Positions, durations and what switches when

The backend keeps the **heard** track's numbers (the book's) apart from
the **decoding** track's (its atomics). While no boundary waits and the
heard track's file hasn't ended they are the same track, as before.

- `positionMs()` is `GaplessJoin::positionMs()`: the heard track's start
  (ms) plus `(min(readPos, B) − its first frame) / 44.1`. The `min` holds
  N at its exact end for the moment between the outputs passing B and the
  loop taking the advance (one loop pass, or longer if the loop stalls).
- `durationMs()` is the heard track's: its frozen exact length once its
  file has ended, else its known length (the MP3 header, trimmed by its
  LAME tag; STREAMINFO), else `TrackProgress`'s estimate from the decoding
  counters, which belong to the heard track while it is the one decoding.
  A benefit: every track's length becomes exact at its end of file, so the
  sleep timer's last-10-s fade is placed exactly.
- `startOffsetMs()` and `durationKnown()` are the heard track's too (the
  book's start, and its frozen length or the decoding track's known one).
- `description()` stays the heard track's: at a join the decoding track's
  text is kept aside (`heardDescription_`) until the advance. `note()` is
  the heard track's; a joined track that ends early keeps its reason
  aside until it is heard.
- `takeAdvance()` (strictly past B, not while a cut is under way) rebases
  the heard record to B and 0 ms and makes the joined track's frozen
  length the heard one if its file has ended too. It happens in the call
  that tells the player, on the loop task, so the queue entry and the
  backend's position change together, in the same `player.update()`.
  It also counts the advance in `trackSeq()` (section 3.5's followers).

On the player's side (`PlaybackController::syncHeard()`), each token the
backend reports is checked against what `update()` would do at N's
natural end right now (the review's amendment 3):

- "pause after this track" set, or the `NextGate` says the timer ends at
  N: `pauseAtBoundary()` (the backend stops, the next entry is cued at
  0:00, the pause is the timer's). This is the too-late End of track: at
  most the cut's latency plus the pause's 1.5 ms fade of N+1 is heard.
- what `advance()` would start (`peek(+1, repeat_)` from N) is the token's
  entry (by its key, or by its track when its key is gone): it becomes
  current with no `play()` (`queue_.setCurrent()`); the state stays.
- otherwise (an edit that came too late to cut N+1 out: Play next, a
  remove, repeat changed): `advance()`, a request that starts what follows
  now; while paused (a pause's fade read past B), the entry after N is
  cued instead, so nothing starts by itself.
- in every case the heard token becomes the track the next word is about,
  `failuresInARow_` goes to 0 (N played through), and the start point and
  `playedFromMs_` are cleared.

`syncHeard()` runs at the top of `update()` (before the natural-end and
failure checks) and of every public action (an `Act` guard). A next
pressed 20 ms into N+1 therefore skips N+1, not N; a prev there goes to N,
which is what `prevRule()` would say.

Who follows:

- **Now Playing, the Queue's mark, `[queue] now at ...`, `danceMode.
  onTrackChanged()`** read the queue's current entry, so they switch at
  the advance (main.cpp's loop, after `player.update()`).
- **The sleep timer's `EntryStart`** goes by `trackSeq()`: the backend
  counts every start it takes up and every advance. `EntryStart` compares
  the count with the one of the pass before the entry changed, so an
  advance counts as the entry's start however late the loop takes it (a
  library rebuild, a screenshot), and the timer gets N+1's length at once.
- **The Queue's learned lengths** (main.cpp) use an `EntryStart` the same
  way.
- **`QueueSaver`/`QueueStore`** see the current position move: the
  position is saved within a second, and the resume point is removed
  (playback moved on), as after any advance.
- **The resume point** (`resumePoint()`, at a pause) is the heard track's
  position.
- **The dancer.** `AudioTap` positions are in the ring's epoch, and a
  join doesn't change the epoch. The beat tracker therefore keeps its
  lock through a join, which is what a segue wants: the beat runs on.
  `onTrackChanged()` clears the tempo prior at the advance, and turns a
  click track's truth off until the next epoch (a skip, a seek, a start
  re-arms it): the review's simpler option (20c), instead of re-arming it
  from the boundary's frame.
- **HostLink's epochs** are the computer's. Host mode pauses the player
  (`pauseByComputer()`), so no join can happen during a session.
  Nothing changes there.

`AudioTap`'s comment ("the track frame, the counter positionMs() is made
of") now reads "the frame in its epoch".

### 3.6 TransportSync's rules

`TransportSync` doesn't change, and its rules hold:

- **Only requests post generations.** A join is part of the request that
  started N, so the phase stays `Decoding` across it.
- **`finished()` and `failed()` still describe only the newest
  generation.** `Ended` comes only after the last track (one with no
  word) has drained. A failure of a joined track is never reported as
  `Failed` (section 5.3).
- **Stale reports are dropped, and so are stale joins.** Words and
  boundaries carry their generation. `take()`, `takeAdvance()` and the
  position clamp ignore every other generation, and the decode task's
  `begin()`/`idle()` restart the book for every request kind (play, stop,
  the benches), so a boundary recorded for an old generation is never
  taken.
- **`positionKnown()`** is unchanged (`Pending` only after a `play()`).
  Advances never pass through `Pending`.
- **`isPlaying()`** (`Pending`, `Decoding`, `Draining`, not paused) is
  true across a join without a blink. A late join from `Draining` reports
  `Decoding` again.

### 3.7 The outputs

- **Bluetooth.** `transportPlaying_` stays true (there is no `stop()`, no
  `play()`), so `bt_.update()` keeps the A2DP media stream started. Its
  3 s suspend never starts. `StreamRestart` sees no stall, and
  `expectingAudio` stays true.
- **The speaker.** The pump keeps sending full buffers (the ring never
  runs short), so `AmpGate` sees no quiet spell and the amp stays on.
- **Both.** `ringSteady_` is no longer cleared at the end of every file:
  only at the queue's real end, which is the `Draining` state. The
  scrolling lists therefore don't change behaviour at joins.
- **`FadeStage`** (the sleep timer's fade) is one level for the stream and
  carries across a join unchanged.

## 4. Trimming

### 4.1 The LAME/Xing "Info" tag

LAME writes a Xing header into the first frame of every MP3 it makes
("Xing" for VBR, "Info" for CBR). The frame has no audio. After the
header comes its LAME extension, which holds the encoder delay and the
padding, among other fields [1][2]. The parts that matter here:

- **Where it is.** The tag starts right after the frame header and its
  side information: 4 + 32 bytes (MPEG-1 stereo), 4 + 17 (MPEG-1 mono or
  MPEG-2/2.5 stereo), 4 + 9 (MPEG-2/2.5 mono), 2 bytes later in a frame
  with a CRC (the protection bit 0; LAME doesn't write one, but
  `lametag::parse()` accepts it).
- **The Xing part.** It holds "Xing"/"Info", then 4 bytes of flags (big
  endian), then, only if their flag is set:
  - the frame count (0x1), 4 bytes;
  - the byte count (0x2), 4 bytes;
  - the TOC (0x4), 100 bytes;
  - the quality (0x8), 4 bytes.
  The LAME extension starts right after the last field present. LAME
  always writes all four, so it sits at +120, but a parser must follow
  the flags.
- **The LAME extension** (36 bytes) [1][2]:
  - +0: the encoder's version string, 9 bytes ("LAME3.100");
  - +9: the revision and VBR method;
  - +10: the lowpass;
  - +11: the peak;
  - +15: the two ReplayGain fields;
  - +19: the encoding flags;
  - +20: the bitrate;
  - **+21: the encoder delay and the padding, 12 bits each, in 3 bytes
    `[xxxxxxxx][xxxxyyyy][yyyyyyyy]`: x is the delay, the samples added at
    the start; y is the padding, the zero samples added at the end** [1];
  - +24: the misc byte;
  - +25: the MP3 gain;
  - +26: the preset and surround;
  - +28: the music length;
  - +32: the music CRC;
  - +34: the tag's CRC-16 (CRC-16/ARC: the polynomial 0x8005 reflected,
    starting at 0) over every byte of the frame before it: 190 for MPEG-1
    stereo with all four Xing fields, 175 for MPEG-1 mono and MPEG-2/2.5
    stereo, 167 for MPEG-2/2.5 mono.
- **Checked on real files.** On 13 LAME 3.99r files from the development
  PC (CBR 128 kbit/s, joint stereo, the ID3v2 tag in front skipped), the
  header was "Xing" with all four flags, the extension at +120, the
  delay 576 and paddings 648-1,669 at +21, and the stored CRC equal to
  CRC-16/ARC over the first 190 bytes in every file. One of those headers
  is written out in test_lame_tag.
- **The frame count excludes the Info frame.** LAME writes the number of
  audio frames. Some other tools (mp3splt) counted the Info frame too [6],
  and a decoder that decodes the Info frame gets one frame more than the
  count says [7].

Which tags to trust: the same as FFmpeg's demuxer [4]. The version string
must start with "LAME", "Lavf" or "Lavc" (FFmpeg's own encoder writes the
same layout). Anything else, such as a Xing header from another encoder,
or no Xing header, gets no trimming. Sanity checks are ours: the frame
flag must be set, and `delay + padding < frames × spf`. The tag's CRC is
checked and logged, but a mismatch doesn't stop the trimming, because
FFmpeg doesn't check it either, and some taggers rewrite the frame.

### 4.2 The rules

All counts are samples per channel at the file's rate. spf is 1152 for
MPEG-1 Layer III and 576 for MPEG-2/2.5.

- **Decoder delay: 529 samples.** The ISO filterbank (the MDCT and the
  polyphase synthesis) delays the output by 528 samples [3]. Decoders
  that follow the standard's synthesis output one more, so the figure
  used everywhere for Layer III is 528 + 1 = 529:
  - FFmpeg: `start_skip_samples = start_pad + 528 + 1` [4];
  - Rockbox, whose MP3 codec is libmad, the same library ESP8266Audio
    uses: `mpeg_latency = { 0, 481, 529 }` for layers I-III [5];
  - the trimming recipe written for JLayer [7].
- **At the start, skip `delay + 529`** decoded samples, counted from the
  first audio frame's output (the Info frame not decoded) [4][5][7].
- **At the end, cut `padding − 529`** samples. FFmpeg's
  `first_discard_sample = frames × spf − padding + 529` [4] is the same
  thing counted from the start.
- **What is kept:** `frames × spf − delay − padding` samples, the
  encoder's input exactly [4][7]. That is the track's length.
- **If the padding is under 529**, the decoder's output ends before the
  encoder's input does. There is nothing to cut at the end, and the last
  `529 − padding` input samples were never decoded. Rockbox clamps that
  case to 0 [5], and so do we (`max(0, padding − 529)`). LAME's own
  padding is normally well over 529, so this only happens with other
  encoders.

These numbers are the standard's. Section 4.3 is what this decoder adds
on top, and section 11.1 checks the sum on the device before anything
relies on it.

### 4.3 What ESP8266Audio adds (read from `.pio/libdeps/core2/ESP8266Audio`)

- **A leading zero frame.** `AudioGenerator`'s constructor sets
  `lastSample = {0, 0}`. `AudioGeneratorMP3::loop()` first hands over
  `lastSample` ("try and push in the stored sample"), and a fresh
  generator is made for each track. So every MP3 starts with one {0,0}
  frame that isn't in the file. `AudioGeneratorFLAC::begin()` sets
  `channels = 0`, and `loop()` only hands over `lastSample` once
  `channels` is non-zero, so a FLAC from its start has no such frame.
  After `SeekableFlac::seekTo()` (which sets `channels`), one {0,0} goes
  first (RESAMPLER.md section 5 noted it). Call this the lead: 1 for every
  MP3 start and every FLAC seek, 0 for a FLAC from the top.
- **The Info frame is decoded as audio** if the decoder is handed it.
  v0.5.0 handed the decoder byte 0 (through the ID3 reader) or the end of
  a big ID3 tag, so the Info frame came out as 1,152 samples of exact
  silence: its side information is all zeros [7]. That is 26 ms of extra
  silence at every LAME-encoded track's start. **Now, when the probe finds
  a Xing/Info (or VBRI) frame, the decoder is handed the byte after it**
  (the probe knows its offset and length). The ID3 reader is gone: the
  ID3v2 tags (one after another too) are skipped to the first frame (the
  library has the title and artist; `trackTitle()`/`trackArtist()` had no
  callers). The same applies without a LAME extension: a header frame is
  never audio. With `G0` the header frame is decoded again, as in v0.5.0.
- **The last frame is lost without guard bytes.** libmad's
  `mad_header_decode()` refuses a frame unless `MAD_BUFFER_GUARD` (8)
  bytes follow it (`N + MAD_BUFFER_GUARD > end - this_frame` gives
  `MAD_ERROR_BUFLEN`; libmad/frame.c). At the end of a file,
  `AudioGeneratorMP3::Input()` reads nothing more, throws the leftover
  away ("something wicked") and stops. So a file that ends right after
  its last frame (no ID3v1 or APE tag behind it) loses that frame: up to
  1,152 samples. That is real audio cut from a gapless join, and the
  padding arithmetic assumes every frame is decoded. **The fix:** a small
  `AudioFileSource` decorator (`GuardedSource`, src/audio) returns 8 zero
  bytes once, at the end of the file. This is the usual libmad practice,
  and the constant exists for it. The decorator is for MP3 only, and it
  passes `getPos()`/`getSize()` through, so `TrackProgress` is unchanged.
  Section 11.1 checks it by counting.
- **libmad delay.** ESP8266Audio's libmad synthesizes 32 samples at a time
  (`mad_synth_frame_onens()`) but runs the standard filterbank. It is
  expected to give 529 like Rockbox's libmad [5]. The device's impulse
  file (section 11.1) measures it.

So, decoding from the first audio frame:

```
skip  = lead + delay + 529          (lead = 1 for an MP3)
hold  = max(0, padding − 529)       (cut at the end)
kept  = frames × spf − delay − padding
```

The lead is skipped on every start whatever the tag (the review's
amendment 8): 1 for every MP3 open and every FLAC seek, 0 for a FLAC from
the top. Without that, an MP3 with no LAME tag (or any MP3 with `Gt0`)
would put a stray zero frame into the stream at every same-rate join.
Only `G0` keeps it, as v0.5.0 did. (`lametag::trim()` works these out.)

### 4.4 Where trimming runs: `TrimFeed` (lib/core)

`RingOutput::ConsumeSample()` hands the sample to `TrimFeed` when the
track has a trim, and straight to `RingFeed` otherwise. A FLAC pays one
branch per frame.

- **The start: a count.** While `skip > 0`, the frame is taken and
  dropped (`return true`, so the generator moves on). Once it is done and
  nothing is held, `TrimFeed` is inactive and `RingOutput` pays one branch
  per frame.
- **The end: a hold of `hold` frames** in a FIFO in PSRAM. It is
  allocated once at `begin()`: 4,095 frames is 16 KB, the largest padding
  the 12-bit field can say. Once the FIFO is full, a new frame can only
  go in if the oldest goes into `RingFeed` first. If `RingFeed` refuses
  the oldest (ring full, budget spent), `TrimFeed` refuses the new frame
  and changes nothing, so the generator's "offer the same sample again"
  contract holds exactly as in `RingFeed`. At the end of the file
  (`TrimFeed::end()`, the engine's commit), whatever is held is dropped:
  that is the padding. At an early end (a decode error the generator gave
  up on, the file not read to its end) it goes into the feed instead: it
  is real audio (the review's 20b).
- **A rate or channel change in the middle**, with frames held (MP3's
  generator says a new rate once and ignores the answer): the change
  waits, the held frames go into the feed at the old format first, then
  the change, and the end trim is off for the rest of that track. The
  first `setRate()` of every track comes during its start skip, with
  nothing held, and passes straight through.
- **Why a hold and not a count to the end.** A count needs the absolute
  sample index, and after a seek start (a resume point) that index isn't
  known. The byte comes from a TOC, and libmad drops the frame after a
  seek that lacks its bit reservoir (ARCHITECTURE.md). Every resumed
  track would then join its next one with its padding still in. The
  hold works from wherever the decoder started, and it isn't fooled by a
  frame lost to bad data in the middle. It does need every frame at the
  end decoded, hence the guard bytes in section 4.3.
- **Cost.** Two PSRAM accesses per frame, only for MP3s with a LAME tag.
  It is estimated at 10-20 cycles per frame, under 0.5 % of a core at
  240 MHz, and measured in section 11.4. If that is too much, a track
  started from its top could use the count instead and keep the hold for
  seek starts.
- **Budget.** Skipped and held frames don't count against `RingFeed`'s
  pass budget, but they are bounded (at most 4,095 + 529 + 1 at a start,
  4,095 held). A pass decodes at most about 7 extra MP3 frames, once per
  track.

`TrimFeed` is about 64 B. It goes into `RingOutput`, which stays under
its 4 KB `static_assert`. `G` shows the decoding track's trim (the tag's
delay and padding, the skip and the hold, a CRC mismatch).

### 4.5 Files without a LAME tag

These are not trimmed: an MP3 with no Xing header, a Xing header from
another encoder, iTunes' `iTunSMPB` (not read), or a LAME tag that fails
its sanity checks. The gap that remains at such a join:

- the previous file's padding plus the decoder delay;
- then the next file's encoder delay plus the decoder delay.

The encoders' own sizes vary. With LAME-like sizes (delay 576, padding
about 1,000) it is roughly 576 + 529 + 1,000 − 529 + 529 ≈ 2,100 samples,
about 48 ms. It sounds like a short dropout in a segue. There is no fade
and no click, because the samples are codec silence, not a cut. Its
length depends on the files, and `[gapless]` logs "no LAME tag: not
trimmed" at the open (the generator's lead is still skipped, and a
Xing/VBRI header frame from another encoder still isn't decoded).

FLAC is sample-exact: libFLAC outputs STREAMINFO's total, and the
generator adds nothing from the top. The built-in tracks are made to the
frame.

### 4.6 Seeks and resume starts on the trimmed timeline

There is one timeline: the trimmed one, where 0:00 is the first kept
sample.

- **Lengths.** `progress::mp3HeaderDurationMs()` and
  `trackseek::mp3LengthMs()` return `kept / rate` when a trusted LAME tag
  says delay and padding (`lametag::lengthMs()`). Without one, they return
  `frames × spf / rate`, as before. Now Playing, the Queue,
  `trackseek::startMs()`'s last-5-s rule and the resume point's saved
  length all use it.
- **The byte for a start at T.** With a trusted LAME tag the time is moved
  onto the decoded stream first: `T + (delay + 529) / rate`
  (`lametag::untrimmedMs()`), and a CBR Info file's byte is counted from
  the first audio frame (the Info frame isn't decoded). A TOC or the
  average bitrate maps that untrimmed time too. This removes a fixed
  +25 ms bias from MP3 seeks, which were measured 30-50 ms behind
  (ARCHITECTURE.md). The rest is the frame libmad drops for its bit
  reservoir, as before. Without a trusted tag, nothing changes (and with
  `G0` a CBR Info file's seek is one frame off: the Info frame is decoded
  again there).
- **The trim after a seek start.** Only the lead is skipped (the decoder
  warm-up was played and faded in before too), and the end hold applies,
  so a resumed track joins its next gaplessly.
- **A FLAC seek**: the lead (1) is skipped. That is one sample
  (22.7 µs), which makes positions after a FLAC seek exact to the sample.
- **`positionMs()` after any start** is the start plus the kept frames,
  as now.

## 5. Changes while the next track is already decoded

Most edits change the word (section 3.1). That is noticed at the end of
the action (or of the next `update()`), and the decode task is woken to
act on it.

### 5.1 The cut

**`PcmRing::cutBack(J)`** (producer only) moves the write index back to
J if the consumer hasn't read past J. It never makes a consumer's read
fail. Taking `lock_` for real, as `discardAll()` does, was rejected: a
consumer only ever tries the lock (`try_lock`), and a Bluetooth callback
that finds it held plays silence for that callback. In the middle of N's
tail that would be a 3 ms dip with fades, and a mutex holder preempted
on core 1 can make it longer. A skip can afford that; an edit to the
next entry can't. So the consumer and the producer use a fence and a
"reading" flag (a Dekker pair, sequentially consistent):

```
consumer read() (under its try_lock, as now):
  reading_ = true                          // seq_cst store
  fenced   = fenced_                        // seq_cst load, after the store
  w        = writeIdx_                      // acquire
  if fenced and r <= fenceIdx_ < w: w = fenceIdx_   // never read past a fence
  copy [r, r + n), readIdx_ = r + n         // as now
  reading_ = false                          // release

producer cutBack(J) -> Done | Pending | Crossed:
  if not fenced_: fenceIdx_ = J; fenced_ = true      // seq_cst store
  if reading_: return Pending              // seq_cst load: a read that may have
                                           // missed the fence; the fence stays,
                                           // try again next pass (1 ms)
  r = readIdx_
  if (int32_t)(J - r) < 0: fenced_ = false; return Crossed
  writeIdx_ = J                            // release
  fenced_ = false                          // release, after writeIdx_
  return Done
```

Why this is safe:

- **Every read either saw the fence, or it is the one the producer waits
  for.** The consumer stores `reading_` and then loads `fenced_`. The
  producer stores `fenced_` and then loads `reading_`. With sequential
  consistency, at least one of them sees the other's store.
- **A read that saw the fence** never copies past J.
- **A read that didn't** has `reading_` set where the producer sees it,
  so the producer waits for it to end. Then it reads `readIdx_`: past J
  means the consumer crossed (Crossed); at or before J, no read can
  cross from then on.
- **After the cut,** a read that loads `fenced_ == false` happens after
  `writeIdx_ = J` (release/acquire through `fenced_`), so it sees J or
  newer frames. A read that still sees the fence clamps to J anyway.
- **The consumer never waits and never fails.** A read is only shortened
  if it reaches J while the fence is up. That is the consumer arriving at
  the join while it is being cut, the "too late" race, and a gap is
  acceptable there.

`readPos()`, `size()` and `space()` keep their meaning. `discardAll()`
clears the fence under its lock. The cost on the consumer's side is two
stores and one load per read. The new fields (`fenced_`, `fenceIdx_`,
`reading_`) add 12 B to the `PcmRing` object, which is in internal RAM.

**On the ESP32.** GCC's code for the build was checked: in
`PcmRing::read()` the store of `reading_` is followed by `memw` before the
load of `fenced_`, and in `cutBack()` the store of `fenced_` by `memw`
before the load of `reading_` (Xtensa's `memw` waits for every access
before it). The pair is also run on the two cores by the console's `Gx`
(lib/core `RingCutStress`, the same harness as the host's two-thread
test): the reader on core 0, as the Bluetooth callback, the producer on
core 1.

**Then the decode task** finishes the cut (`GaplessEngine::stepCut()`):
before it raises the fence it moves the boundary to `Cutting` under the
book's lock (`beginCut()`); a boundary the loop has already taken can't be
cut, and one in `Cutting` is never taken.

- **Done:**
  - N+1's decoder is closed, the feed rewound to the mark
    (`RingFeed::rewind()`: the converter and the feed's scalars as they
    were at J, nothing staged or held; a plain copy, since the tables'
    copy is never freed during a chain of joins), the trim disarmed, and
    the boundary removed (`cutDone()`) before anything more is written.
  - The engine is back at N's end with the word as it is now (section 3.2,
    step 2): a new next entry, or none (`finish()`, `Draining`).
- **Pending:** the fence stays and the cut is tried again 1 ms later.
  Nothing is decoded meanwhile.
- **Crossed:** the boundary becomes `Committed` and is never cut. The
  listener is about to hear N+1's start (the outputs are in [J, B] or
  past it). The advance comes past B, and the player then sorts the
  change out (section 3.5).

**The race windows.** `setNext()` wakes the decode task
(`xTaskNotifyGive()`, and every rest on its producing paths is
`ulTaskNotifyTake()`, the review's amendment 2), so an edit reaches it
within a pass: about 3-5 ms if it is decoding, sooner if it is waiting
for room. If the edit lands while it is opening N+1 (the join itself), it
waits for the open: 30-100 ms or more (a FLAC's init, a big picture). A
cut is too late only if the outputs pass J within that time. `G` shows
each cut's time from its word (the last and the largest), so the device
run measures it. A re-decode after a cut has the time the outputs still
need to reach J: usually more than a second, at least an open. An edit in
the last ~30-100 ms before the join can leave a short gap between N's end
and the new next track: N's tail is whole, then the new track starts
(a converting rate: a fresh filter). That is the v0.5.0 behaviour at that
one join.

### 5.2 The cases

When this table says the consumer is before J, N plays out whole and
nothing of the old N+1 is heard. When it says the consumer passed J, the
change came after the join: the advance is heard first and the change
applies to the track that now plays.

| Change while N+1 is decoded ahead | What happens |
|---|---|
| Next, prev, `play(pos)` (another entry), Play (`playNow`), Shuffle all, `setStartPoint` while playing, a restart by prev, stop, Clear | a request, as before: a new generation; the decode task's `start()` calls `discardAll()` (the epoch bumps, so the outputs crossfade), resets the converter and restarts the book (the boundary and the old word go). `syncHeard()` ran first, so the action is on the entry the listener hears (section 3.5) |
| Play next / `insertNext`, `moveNext`, + Queue onto the last entry, `remove` of the next entry, Clear up next, an undo that changes the next entry | the word changes. Outputs before J: cut, then the new next is joined. Passed J: the advance is heard, and since what `advance()` would start isn't N+1 any more, it is started (a request): Play next's track plays next. Host-tested (test_gapless_player) |
| `remove` of the current entry (N) | `currentMoved()`: a request, as before |
| A library rebuild (`queueReplaced(true)`: fresh keys, same tracks), or one of two adjacent duplicates removed | the same track stays next: the word keeps its token, no cut; the advance finds the entry by its track (section 3.1) |
| Repeat changed (no UI today) | at the queue's last entry a word appears or goes; a cut if needed |
| Gapless turned off (`G0`) | nothing follows any more, and the engine is off: cut, and N ends as in v0.5.0. Trimming stays as it was for tracks already open (`Gt` applies at the next open) |
| The sleep timer's end chosen or its kind changed (End of track, album, queue) | section 5.4 |
| "Pause after this track" (`setPauseAfterTrack(true)`) | nothing follows: cut, N drains, `Ended`, `pauseAtBoundary()` as before |
| Pause | nothing to cut: decoding ahead while paused is harmless, because nothing reads. Resume plays the join gaplessly |
| The output switched (speaker ⇄ Bluetooth) | a consumer handover (`setConsumer()`); the read index continues, the boundary stays valid |
| A `Hold` appears (headphones gone) | the drop pauses the player (`BtSession`); the word stays (nothing is heard without an output reading). An advance taken while held starts the entry as `advance()` would: a wait |
| The USB visualizer starts | `pauseByComputer()`: a pause, as above |

### 5.3 Failures

- **The next track can't be opened** (missing file, not an MP3 or FLAC,
  a rate the converter refuses, an unknown `tone:`, a decoder that won't
  begin): the word was taken, so it isn't tried again; the decode task
  logs `[gapless] can't decode ahead ...; the track before ends as
  before` and does what it does with nothing following: `finish()`,
  `Draining`, `Ended`. The player's `advance()` then calls `play()` on
  that entry, which fails as before (`Failed`, the note "Skipped ...",
  the Queue's mark, the next one). The file is opened twice, which costs
  milliseconds; a transient error gets its second chance, and the failure
  path is the one that exists and is tested. N ends cleanly; the decode
  task never waits on the failed track. Host-tested at both ends (the
  engine, and the player skipping it).
- **A joined track that gives no audio** (it ends before its first frame
  reaches the ring: an MP3 libmad can't decode, a rate refused at once):
  the engine cuts it back out (`EmptyAhead`, then Done; the outputs can't
  have read past J, nothing was written after it) and N ends as before;
  `play()` on that entry then fails or ends as before.
- **A joined track that fails after giving audio** (a rate refused in
  the middle, a decode error libmad gives up on): an early end (the
  review's amendment 12). What the trim holds goes in (real audio), its
  reason (`[gapless] ... ended early: ...`) becomes `note()` once it is
  heard, and the next join follows. `Failed` is reported only for a
  request's own track that fails before its first frame, as before, so
  `failed()` never describes N while N is heard.
- **An underrun at the join** (an SD stall during the open) is counted
  and faded like any underrun (`expectingAudio` stays true).

### 5.4 The sleep timer

- **The predicate.** `PlaybackController::NextGate` (like `Hold`) asks
  whether the timer ends at the current entry. main.cpp implements it
  (`SleepGate`) with the pure `SleepTimer::endsAt(choice, lastOfAlbum,
  lastOfQueue)`, which `atBoundaryTrack()` now uses too:
  - End of track: always;
  - End of album: `albumEndsBetween(current, next)` or the last entry;
  - End of queue: the last entry.

  Nothing follows whenever it says yes. N+1 is then never decoded ahead
  of the track the timer ends at, and that stays true right after an
  advance: in the same `update()` that makes A9 (an album's last track)
  current, the next word is worked out with A9 as the boundary track. It
  doesn't wait for `stepSleep()` to set `pauseAfter_` in the next pass
  (host-tested: the next album's first track is never even probed).
- **The pause.** With nothing following, N drains, `Ended`, `pauseAtBoundary()`:
  exactly v0.5.0's End of track. The ring empties with N's last frame,
  nothing of N+1 is decoded, and the next entry is cued at 0:00.
- **Chosen late** (N+1 already decoded): the gate changes the word, so
  the cut happens. If the cut is Done, it pauses exactly at the boundary.
  If it is Crossed (the choice landed within a cut's latency of the
  outputs passing J), the advance is checked against "pause after this
  track" (the review's amendment 3): the player pauses at once, the next
  entry cued at 0:00, and at most the cut's latency plus the pause's
  1.5 ms fade of N+1 is heard. Host-tested both ways.
- **The fade** is computed from the heard track's position and length.
  N's length is exact once its file has ended (section 3.5). The fade
  over the last 10 s ends at the boundary, and `FadeStage` is one level
  for the stream, so a join changes nothing about it.
- **Timed choices** don't involve the boundary. They fade and pause by
  the clock, wherever the stream is.

## 6. Memory, CPU and the stack

- **Decoders.** There is still one at a time. N's generator is
  `stop()`ped (libFLAC's ~100 KB PSRAM and 2.5 KB internal freed, as
  `closeDecoder()` always did) at N's commit, before N+1's file is even
  probed. The peak is the old per-track peak.
- **Internal RAM** (estimates; `[heap] playing` measures it):
  - `GaplessJoin` in `Core2AudioBackend` (a global, .bss): about 100 B
    plus the word's path string (heap, internal, up to
    `TrackCatalog::kMaxPath`); the backend's `Prepared` (the track opened
    for a join, with its LAME info) about 150 B more.
  - `GaplessEngine` (`new` at `begin()`): about 120 B, plus the word it
    took (another path string).
  - `PcmRing`: +12 B.
  - `TrimFeed` in `RingOutput`: about 64 B. `static_assert(sizeof(
    RingOutput) < 4096)` still holds (about 3.3 KB).
  - The tables' 7.6 KB copy stays while a chain of joins goes from a
    converted track to 44.1 kHz ones (the review's amendment 9); it is
    freed at the next request's start, as before.
  - There is no `IRAM_ATTR` anywhere.
- **PSRAM**, allocated once at `begin()`:
  - two feed marks (`RingFeed::Mark`: a `RateConverter` image and a few
    scalars), about 2 KB each;
  - the `TrimFeed` hold: 16 KB.
- **The decode stack (16 KB).** The join's path, `decodeTask →
  GaplessEngine::step → Tracks::probe/start → prepare/beginPrepared →
  AudioGenerator*::begin`, is no deeper than the request's `decodeTask →
  start → prepare/beginPrepared → begin`. The deepest frames stay
  libFLAC's and libmad's inside `loop()`, which doesn't run during an
  open. The MP3 probe buffer stays in PSRAM. The device run watches
  `decodeStackFree` (`s`) across joins of each kind.
- **CPU:**
  - The open moves from a moment when the ring is empty and the UI waits
    to a moment when the ring is full and the decoder would otherwise
    sleep.
  - The refill from empty disappears at natural ends, which saves about
    0.7 s of flat-out decoding per track.
  - The hold costs two PSRAM accesses per frame for LAME MP3s (estimated
    under 0.5 % of a core; measured in section 11.4).
  - A mark is a ~2 KB copy once per join.
  - The consumer's read gained the reading mark and the fence check
    (a few `memw` per read of 128-1024 frames).

## 7. Hearing safety

- **Nothing starts by itself beyond the normal auto-advance.** A word
  exists only while the player holds a track that plays or is paused,
  and only for the entry `advance()` would start. A word decodes into
  the ring; only the outputs reading make anything heard.
- **A paused player never advances.** Paused outputs don't read, so
  `readPos()` can't pass B. One edge: a pause within 64 frames (1.5 ms)
  of B. The pause's own 64-frame fade-out reads across B, so the player
  ends paused at N+1's 0:00 (if N+1 is still what comes next; otherwise
  the next entry is cued). That is what was heard, nothing plays, and a
  resume continues N+1. Host-tested (a pause 10 frames before B).
- **Never louder.**
  - A same-rate join is the converter's normal running.
  - A rate-change join is a `finish()` (the tail decays) followed by a
    fresh filter.
  - A cut restores a state the converter was really in.
  - The trimming only removes samples.
  - The fades and gains downstream are untouched (each at most 1).
- **The sleep timer** pauses at the boundary with nothing of N+1 decoded
  (section 5.4). Its fade is unchanged.
- **Bluetooth.** The headphones' level isn't touched, no AVRCP command is
  sent, and the stream doesn't restart, so there is no burst.

## 8. The pieces

### Portable, host-tested (lib/core)

| Piece | What |
|---|---|
| `PcmRing` | `cutBack(index)` → Done/Pending/Crossed (section 5.1); `writePos()`; the fence in `read()` |
| `RateConverter` | `tailFrames()`: what `finishPush()` still owes (B's offset at a continuous join) |
| `RingFeed` | `Mark`, `mark()` (nothing staged or held: the converter and the scalars saved), `rewind()` (a plain copy back), `continues(hz)`, `tailFrames()`, `restartStream()` (after `finish()`: a new stream, `made()` running on) |
| `LameTag` (new) | `lametag::parse()`: the first frame's Xing/Info or VBRI header and LAME extension (frames, spf, rate, delay, padding, the CRC and its range, the header frame's offset and length); `trim()` (skip, hold), `keptSamples()`, `lengthMs()`, `untrimmedMs()`, `crc16()` |
| `TrimFeed` (new) | the start skip and the end hold in front of `RingFeed` (section 4.4) |
| `GaplessJoin` (new) | the word, the boundary, the heard record (section 3.4) |
| `GaplessEngine` (new) | the decode task's state machine from a source's end: the commit, the word, the join, the tail, the drain, the late word, the cut (sections 3.2, 5.1) |
| `RingCutStress` (new) | the cut against a reader on another task, step by step (the host's two-thread test, the device's `Gx`) |
| `QueueModel` | `peek(delta, wrap)` |
| `PlaybackController` | `setGapless()`, `NextGate`, `refreshOffer()`, `syncHeard()`, `gaplessStats()` (sections 3.1, 3.5) |
| `IAudioBackend` | `setNext(const Next&)` and `takeAdvance(uint32_t*)`, with default bodies (the test fakes compile unchanged) |
| `SleepTimer` | `endsAt(choice, lastOfAlbum, lastOfQueue)`; `EntryStart` counts a start in the pass of the entry's change |
| `TrackProgress`, `TrackSeek` | the trimmed lengths, the delay in the seek byte (section 4.6) |

### The firmware (src)

| Where | What |
|---|---|
| `audio/Core2AudioBackend` | the decode task driving `GaplessEngine` (and reporting its phases), `GaplessEngine::Tracks` (probe, start, close, the `[gapless]` log), `prepare()`/`beginPrepared()` (the file's rate, length, LAME tag and trim, the header frame skipped, `GuardedSource`, the ID3 tags skipped, a FLAC's metadata size), the heard record through `GaplessJoin` (`positionMs()`, `durationMs()`, `startOffsetMs()`, `durationKnown()`, `description()`, `note()`), `setNext()`/`takeAdvance()`, `trackSeq()`, the PSRAM at `begin()`, the tables' copy kept during a chain, `setGapless()`/`setGaplessTrim()`, `printGapless()` |
| `audio/GuardedSource` (new) | 8 zero bytes at the end of the file (section 4.3) |
| `audio/RingOutput` | `TrimFeed` in front of `RingFeed` |
| `main.cpp` | the `NextGate` (`SleepGate`), `trackSeq()` for the sleep timer's and the learned lengths' `EntryStart`, the console's `G` and `Gx` |
| `app/SerialConsole` | `Pending::Gapless` on `G` |
| `app/DanceMode` | a click track's truth off at a track change (until the next epoch) |

The log, one line per event:

- `[gapless] decoding ahead: /music/.../06 - Digital Love.mp3 (44100 Hz, the same rate: one stream) with 1472 ms of the track before left; opened in 27 ms` (or `another rate: after the tail`, `late: after the tail`)
- `[gapless] trim: LAME3.100 delay 576, padding 1308: skipping 1106, holding 779` (or `no header: no LAME tag: not trimmed`)
- `[gapless] heard: the joined track plays (taken 3.0 ms after its first frame was read)`
- `[gapless] cut: what comes next changed: the track decoded ahead taken back out, 1210 ms before the join (2400 us after the word)` (or `too late to cut: ...`)
- `[gapless] can't decode ahead /music/.../07 - x.flac; the track before ends as before`
- `[gapless] /music/... ended early: <why>`, `[gapless] ... gave no audio: taken back out`

## 9. The console switch: `G`

`G` was free (SerialConsole.cpp's key list). It takes an argument up to
Enter, as `T` and `R` do.

- **`G`**: the status: on or off, trimming on or off; the word (the track
  it is about, the token, the path, taken or not); the boundary (its
  token, B and J, ms from the reader to it, cuttable, being cut or too
  late); the heard track's exact length once its file has ended; the
  counters since boot (joins continuous, after the tail, late; advances
  heard; cuts, too late, retried; failed opens, empty tracks; the last and
  the largest cut's time from its word); the decoding track's trim (the
  tag's delay and padding, the skip and the hold, a CRC mismatch, or "no
  LAME tag"); and the player's side (words sent, joins taken as the next
  entry, started again, paused at the boundary).
- **`G0`**: gapless off. That is v0.5.0 at the next end: nothing named
  (the player's `setGapless(false)`), the engine off (a track decoded
  ahead is cut at once), and from the next open no trimming, no skipping
  of the header frame, no lead skip and no guard bytes. RAM only, for the
  A/B and as the safety valve.
- **`G1`**: on (the default; `-DMSTREAM_GAPLESS=0` builds it off by
  default, and both the player and the backend start from the backend's
  setting).
- **`Gt0` / `Gt1`**: trimming by the LAME tag off or on, from the next
  open, with decode-ahead (and the header-frame and lead skips) left as
  they are. This measures the trimming's share of a join.
- **`Gx<n>`**: the cut's stress test on the two cores (section 5.1),
  `n` tracks (20,000 if not said), on a test ring of its own; it stops
  the player first (keeping its place, as `Rt` does) and logs
  `[gapless] Gx: PASSED ...` with its counts.
- **`Gp<sec>`** (the join probe of section 11): not built yet. It is
  temporary, for the device run only, and goes after it like
  RESAMPLER.md's `Rp`.

## 10. Host tests (`pio test -e native`)

Everything here runs with synthetic tracks: arrays of noise frames at a
given rate, with the trim they arm (a lead, a delay, a padding), and
failures at the probe, at the start, before the first frame or in the
middle. The outputs are a reader that takes random amounts (0-1,500
frames, nothing a third of the time), so the ring is often full and
frames are refused. "Exact" below means frame for frame against the
converter alone (`RateConverter` on the concatenated or separate
streams).

**test_pcm_ring** (7 new)

- `cutBack()`: the frames after the index taken out, the next write lands
  there; refused (Crossed) once the reader is past it, nothing changed;
  to the read index and to the write index; across the 2^32 wrap;
- a read under way (the reading mark set by a probe): Pending, the fence
  up, a read stops at the fence, then Done;
- `discardAll()` takes a pending fence down;
- two threads: tracks tagged (track, index), part of the next one
  written past each end and cut at random: every track starts at its
  first frame and runs on without a gap or a repeat, in order, and no
  frame of a cut track is ever read (both outcomes exercised, hundreds of
  Done and tens of Crossed); and the same through `RingCutStress` (the
  `Gx` harness).

**test_ring_feed** (6 new)

- a same-rate join is the concatenated stream converted in one go, at
  every rate, and B (the write index plus `tailFrames()`) is
  `ceil(len(A) × num / den)` from the stream's start;
- a rate-change join is A converted alone, then B alone, each exact;
- a cut and `rewind()` leave no trace: A, then Y, bit for bit as if X had
  never been fed, at every route, three cut points each, with the ring
  full in between;
- `mark()` refuses while frames are staged or held; a cut that comes too
  late (Crossed) leaves A then X;
- a cut after a rate-change join goes back to A's end with its tail in;
- a rewind across a table copy made in between gives the same bits.

**test_trim_feed** (new, 9)

- exactly the kept frames come out, bit for bit, for seven skip/hold
  cases (a hold as long as the rest of the track, a skip longer than the
  track) and three reader seeds;
- the trim is at the source rate (48 and 22.05 kHz, against the
  converter);
- a zero skip and hold is a passthrough (inactive);
- a seek start skips only the lead and holds the end;
- an early end flushes the hold;
- a rate change in the middle releases the hold first (the same output as
  the feed alone given the same change), said again while it waits it
  still waits, and with nothing held it passes straight through;
- the hold is clamped to its buffer.

**test_lame_tag** (new, 10)

- MPEG-1 stereo with all four Xing fields; MPEG-1 mono, MPEG-2 and
  MPEG-2.5 (the CRC over 190, 175 or 167 bytes); every subset of the
  Xing flags (the extension moves; without the frame count not trusted);
- the 12-bit delay and padding at their edges (0, 4,095, single bits);
- "LAME", "Lavf", "Lavc" trusted; another string, no extension, VBRI, no
  header, no frame: no trim (a header frame still reported);
- a wrong CRC reported and trusted anyway; the sanity checks; an
  extension cut off by the buffer;
- a CRC-protected header frame;
- a real LAME 3.99r header, byte for byte (delay 576, padding 701, CRC
  0x1856 over 190 bytes);
- the trim rules (skip, hold, the padding-under-529 clamp, a seek start,
  `Gt0`, an untrusted tag), kept, the length, the untrimmed time;
- `TrackProgress`/`TrackSeek` on the trimmed timeline: the header's
  length, and a CBR Info file's seek byte (+25 ms, after the Info frame),
  unchanged without the extension.

**test_gapless** (new, 24): `GaplessEngine` and `GaplessJoin` with a
stand-in for the player that names the next track once each join is
heard.

- Sample-exact joins: three tracks at one rate are one stream (44.1, 48,
  22.05, 32 and 8 kHz, with trims); rate changes are streams back to back
  (48 → 44.1 → 22.05 → 22.05); a rate the probe can't say resets, MP3's
  call order (the rate after the 2nd frame) continues; the same track
  three times; a track shorter than the ring (the word waited for: still
  one stream) and one under 250 ms (it ends as before).
- The heard boundary: no advance at B exactly, the first read past it
  gives it once, the position held at A's exact end until it is taken,
  then from 0; A's length frozen at its end; another generation's never
  comes; near the 2^32 wrap; at a converting rate B is the converter's
  frame for B's first source frame, after J.
- Changes: before A's end (just taken, no cut); after the join with the
  reader before J (cut, A then Y exact, nothing of X, at 44.1 and 48 kHz
  with trims); the reader one frame past J (too late: X stays, its advance
  comes); nothing follows or gapless off (cut, A ends, Ended); two changes
  in a row; a cut while the joined track drains; the reader at J = B
  during a cut (no advance, Done, and the advance that comes is Y's); a
  pending cut (the reading mark set) retried, nothing taken meanwhile.
- A late word while draining with 250 ms or more (a join after the tail),
  and one too close to the end (not taken).
- Failures: a probe that fails, a start that fails at the same rate and
  after a tail (A whole, Ended, no advance); a joined track with no frames
  (cut back out); an early end in a joined track (its hold flushed, the
  next join follows).
- Generations: a word for another request never taken; a new request
  drops the boundary; a taken word is "nothing" from then on; no advance
  while cutting, and after a too-late cut there is.

**test_gapless_player** (new, 15): the real `PlaybackController` and
`QueueModel` over a backend made of the same pieces (`HostBackend`).

- An album plays as one stream with one `play()`; each advance taken when
  the reader passes the join, and the entry changes in that same pass.
- Play next while decoded ahead (cut: A, Y, B, C); Play next too late to
  cut at 48 kHz (a little of B's start, then Y plays next, then B again);
  remove the next entry, move another before it, Clear up next, an undo
  (each cut and joined again, exact); repeat turned off on the last entry.
- The sleep timer: End of track never decodes the next track (nothing
  probed; paused at the next entry, cued, the timer's pause); chosen late
  (cut, the exact pause) and too late (paused at once, at most a read of
  B heard, nothing more after); End of album decided in the advance's own
  update (the next album's first track never probed).
- A next just after the reader passed the join skips the joined track; a
  paused player never advances and resumes gaplessly; gapless turned off
  ends tracks as before (requests); a library rebuild's new keys and a
  removed duplicate keep the join (no cut); a next track that can't be
  opened is skipped as before; an advance taken 1.13 s late still counts
  as the entry's start (`EntryStart` with the track count); the word goes
  only when it changes, never while stopped.

**test_playback** (7 new): the word is what `advance()` would start (the
end without repeat, a queue of one, "pause after", the gate, gapless off,
stopped); tokens (kept for the same track, new after an edit or a play);
an advance moves the entry without a `play()`; a stale advance starts
what comes next; actions take the advance first; an advance with "pause
after" pauses at once; a failure after an advance is the new entry's.

**test_queue** (1 new): `peek()` at both ends, with and without wrap, a
queue of one, empty.

**test_sleep_timer** (2 new): `endsAt()` for each choice; `EntryStart`
after a late gapless advance (and not without the count moving).

## 11. Device test plan (silent mode `z`, the speaker)

The setup:

- **Silent mode.** Everything runs in silent mode `z`, on the speaker,
  so nothing is heard (RESAMPLER.md section 6). Bluetooth is item 6, with
  silence tracks only.
- **The probe** (not built yet: it comes with the device run, and goes
  after it). A temporary `Gp<sec>` probe, like RESAMPLER.md's `Rp`. It
  turns the speaker's tap on (`setTapsOn(true)`) and, at each advance,
  reads the tap ±100 ms around the boundary's frame in the ring's epoch
  (B less the epoch's start: a `heardEpochFrame()` the probe adds to the
  backend). It logs one `[gapless probe]` line per join with:
  - **frames inserted**: how many frames between N's last real frame and
    N+1's first. With gapless on, the tap's segment must run on (0
    inserted). With `G0`, the tap holds only real frames, so the gap is
    taken from the pump's write timestamps: the time between N's last
    write and N+1's first, less the duration of N's last write, plus the
    fade frames the `DeclickReader` sent (`total − read`);
  - **the silence run across the boundary**: the run of frames with both
    channels within ±8 (about −72 dBFS) that contains B;
  - **a click figure**: the largest second difference
    `|x[n] − 2x[n−1] + x[n−2]|` within ±64 frames of B, over the 99.9th
    percentile of the same in the 2 s before. Around 1 means nothing
    stands out; a step at the join shows as 5-10 or more;
  - **for the sine files**: the zero-crossing intervals across the join
    (interpolated, as `Rp` did), and the deviation of the one that
    straddles B from the median, in samples;
  - **the heard timing**: when the controller took the advance, against
    the tap's write timestamp of the buffer that held frame B (expected
    0 to one loop pass), and `outputLatencyUs()`, the time the listener
    hears it after that (speaker ~115 ms).

### 11.1 Calibration with made-up files (first: the rest relies on it)

`tools/gapless_files.py` (to be written for the run, on the PC, with
`lame` 3.100 and `flac`) writes `/music/zz gapless test/`:

1. **A burst.** 3 s of silence with a 2,048-sample burst of broadband
   noise starting at frame 10,000, encoded as LAME CBR 128 and as LAME
   V2. The probe cross-correlates the tap's kept timeline with the
   original burst (the review's amendment 19: an MP3's lowpassed impulse
   can put its peak a sample off, a correlation's peak can't): the lag
   must be 0. If it isn't, the constant in `skip` is wrong by that much:
   the lead, the Info frame, or libmad's 529. Fix the constant before
   anything else (`Gt0` keeps the joins meanwhile). Played on its own
   (`Rf`), the converter's `taken` (`R`, after its end) is the kept count:
   it must be exactly 132,300 frames, and `G` shows the tag's delay and
   padding and the trim.
2. **The last frame.** A file of k × 1152 samples with no ID3v1 tag, so
   it ends right after its last frame. Its kept count must be exact; a
   lost last frame (no guard bytes) shows as 1,152 short, which would
   confirm section 4.3's reading.
3. **A sine split across files.** A continuous 441 Hz sine at −12 dBFS,
   cut at frames 100,003, 177,780 and 301,236 (not frame-aligned) into
   four files, made in four sets:
   - LAME CBR 320;
   - LAME V0;
   - FLAC 16/44.1;
   - FLAC and LAME at 48 kHz (the converting route; there is no 48 kHz
     file on the card today).

   Each set is played as an album (`ql`, `qp<n>`). Expected at each
   join:
   - FLAC: the second-difference figure as elsewhere in the sine (≈ 1),
     and the straddling crossing interval within ±0.02 sample;
   - MP3: within ±0.5 sample. Lossy edges add some noise, but a k-sample
     misalignment shows as k samples.
   - With `Gt0`, the MP3 joins show the encoder silence (about 2,100
     frames); with `G0`, v0.5.0's gap.
4. **The untagged gap.** The same sine set encoded with `lame -t` (no
   Xing/Info tag): it must play with the gap section 4.5 predicts, and no
   click.

### 11.2 Real album joins (the user's library)

1. `ql` lists the albums. Pick the albums whose tracks run into each
   other: Daft Punk's *Discovery* if it's on the card, live albums, mixes
   or classical. `l` shows the files, MP3 or FLAC, so the candidates can
   be picked for both.
2. **The A/B.** For each candidate:
   1. `G0`, then `qp<n>`.
   2. For each track, `qs<len−10>`: a start point 10 s before its end,
      which a playing track takes at once. This makes it a seek start, so
      the end hold after a seek is checked too.
   3. `Gp` logs each join.
   4. Then the same with `G1`.

   Record per join: frames inserted, the silence run, the click figure,
   the open time and the ring left at the open (`[gapless] decoding
   ahead`), and the trim line.
3. **Expected.**
   - With `G1`: 0 frames inserted, the silence run no longer than the
     music's own (compare with the same files decoded on the PC by
     `ffmpeg`, which trims by the same rules [4]), and the click figure
     near 1.
   - With `G0`: v0.5.0's gap, which this measures for the first time:
     tens of ms plus the 1.5 ms fades and the encoder silence.
4. **Whole albums, start to finish.** For at least one MP3 album and one
   FLAC album, played through with `G1`: 0 underruns (`s`). The ring
   stays above the pacing level at each join, because it never refills
   from empty. `[audio] refill` is logged only once, at the album's start.
   The decode load shows no spike at joins.

### 11.3 Changes while decoded ahead

Each is done in the last 1.5 s of a track, with `Gp` on, logging which
path was taken:

- `qn<album>` (Play next) and `qr<pos>` (remove the next entry): `cut`,
  N's tail contiguous in the tap (0 inserted before J), then the new
  next. Nothing of the old next is in the tap.
- `n` (next) and `p` (prev): a request. The epoch bumps and the outputs
  crossfade, as before.
- `G0`: a cut, then a v0.5.0 end.
- `Tt` (End of track) at 1 s before the end: the cut. The tap's last real
  frame is N's last, the player is paused at the next entry's 0:00
  (`[sleep]` lines), and no frame of N+1 has been read.
- `Tt` well before the end: no decode-ahead at all (`G`'s word says nothing follows).
- The timing race, on purpose: `Tt` and `qr` at about 20 ms before the
  join, repeated. Count Done, Crossed and Pending, and check that a
  Crossed always leaves the timer acting on the new track and the
  controller on the right entry.
- A missing file as the next entry (renamed on the card): `[gapless]
  can't decode ahead`, N plays out whole, then the usual "Skipped ...".
- Pause in the last second, resume after 10 s: the join is gapless.
- **The cut's latency:** `G` after each change shows the last cut's time
  from its word and the largest; repeat the changes while the decoder is
  mid-open (a FLAC next) for the worst case (section 5.1).
- **The fence on the two cores:** `Gx` (20,000 tracks, then `Gx200000`),
  silent or not (it has a ring of its own): `PASSED`, with hundreds of
  cuts and some too late, errors 0, cut tracks heard 0.
- **A FLAC with a big embedded picture** (1-3 MB) as the next track:
  `[audio] FLAC: N KB of metadata` and the ring's level at the join
  (`s`'s `buf=` just after the open): it must not run dry (the review's
  amendment 18).

### 11.4 Cost and limits

- `s` across joins of every kind:
  - FLAC → MP3 → FLAC;
  - 48 → 44.1 (with the tables' copy freed and made again);
  - a tone → a file;
  - a cut followed by a FLAC.

  Check the decode stack free (v0.5.0's margin, no drop), the internal
  heap minimum (`[heap] playing`, no drop beyond the bytes in section 6),
  and the largest free block.
- The hold's cost: `b<n>` on a LAME MP3 with `Gt1` and `Gt0`. The
  difference must be under 0.5 % of a core at 240 MHz.
- At 160 MHz: one album with `G1`, 0 underruns.

### 11.5 The heard moment

- On every join, the advance must come 0 to 20 ms after the tap's write
  of frame B (one loop pass), never before it.
- Now Playing's title must change in the same frame as the position goes
  to 0:00. Check this with screenshots (`X`) taken around a join at 30 fps
  of the UI: the title and 0:00 together, never the new title with N's
  time, or N's title at 0:00.
- The listener hears the change `outputLatencyUs()` later. This is
  reported, not corrected (section 12).

### 11.6 Bluetooth (needs the user, or silence only)

Run with a queue of `tone:silence@48000`, then `tone:silence`, then
`tone:silence@48000`. Each is an hour long, so `qs3590` on each puts it
10 s from its natural end.

- The A2DP stream doesn't suspend or restart across the joins: no
  `[bt]` stream lines, and `btFramesPerSec` stays about 44,100.
- 0 underruns.
- The rate-change joins logged as resets.

## 12. Risks and open questions

- **The 529 and the lead are from sources and from reading the code, not
  from this decoder's output.** ESP8266Audio's libmad is a modified fork.
  Section 11.1's impulse file decides the constant; until it passes, `G1`
  can still decode ahead with `Gt0`.
- **The last-frame fix (`GuardedSource`) changes every MP3's end, also
  with `G0`.** It adds up to 26 ms of real audio that was lost before.
  `G0` turns it off too, to keep the A/B honest.
- **The heard moment is the consumer's read, not the ear.** The UI
  switches about 115 ms (speaker) or about 175 ms (Bluetooth: the
  headphones' report plus 25 ms) before the listener hears N+1. That is
  the same lead `positionMs()` has always had. Holding the advance back
  by `outputLatencyUs()` would make it exact for the ear. It is left
  out, because positions would then disagree with the dancer's taps and
  with everything else that reads `positionMs()`.
- **The fence protocol is new concurrency in the hottest path.** The
  host's two-thread tests, the build's disassembly (`memw` between each
  side's store and load) and the device's `Gx` are the gates; a
  microbench of `read()` before and after is still to do (the consumer
  read gained a few `memw`).
- **Files without a LAME tag** keep about 50 ms of encoder silence at a
  join (section 4.5). iTunes' `iTunSMPB` could be added later; it is an
  ID3 comment, and the backend reads no ID3 frames at all now.
- **Rate-change joins** have a discontinuity of up to 0.5 ms (a fresh
  filter). Albums rarely change rate mid-album.
- **The late-word join** (taken in `Draining`, after the tail was
  flushed) has the same 0.5 ms discontinuity at converting routes.
  Deferring `finish()` until the ring is nearly dry would remove it, at
  the cost of one more state. Not planned.
- **A skip during a pending join re-decodes N+1 from its file.** It
  could instead jump the read index to B (the frames are there),
  crossfaded. That is an optimisation for later.
- **A track shorter than about 250 ms** can't be joined gaplessly to its
  next one: the player names what follows a track only once it has heard
  it begin, and the decoder can't wait for that word with less than
  250 ms in the ring (section 3.2; the review's amendment 15 was rejected
  for this reason). A loop pass that comes late (a library rebuild, a
  screenshot) during a track shorter than the ring can cost that one join
  the same way. Such tracks are rare, and v0.5.0's gap is all that
  happens.
- **A FLAC with a big embedded picture** is read through at its open
  (libFLAC skips metadata it doesn't keep through the read callback). At
  a join that must finish within the ~1.4 s of N left in the ring; on an
  SPI card a multi-MB picture may not. Logged (section 3.2), measured in
  section 11.3; if it bites, such a file could be joined after the ring
  has refilled, or its picture block seeked over.
- **`G0`'s seeks in a CBR Info file are one frame off**: the seek byte is
  counted from the first audio frame (the Info frame not decoded), while
  `G0` decodes the Info frame again. `G0` is for the A/B of ends, not of
  seeks.

## 13. The other docs

Updated with the build:

- **ARCHITECTURE.md, "Audio pipeline":** the diagram (`GuardedSource`,
  `TrimFeed`), the ID3 paragraph (the reader is gone), "a natural end
  drains the ring first" became the join, the converter's reset "at every
  request, never at a join", the trimmed timeline for starts part of the
  way in, and the Tasks table's decode row.
- **ARCHITECTURE.md**, the other sections: "Library and queue" (the word
  and the advance), "Sleep timer" (the `NextGate`), "USB visualizer" (no
  change in host mode).
- **RESAMPLER.md, section 5:** resets (same-rate joins don't reset; the
  mark and the rewind) and positions and durations (the heard record).
- **ENERGY.md:** the refill at natural ends is gone; the hold's cost.
- **Header comments:** `AudioTap.h` ("the frame in its epoch"),
  `PcmRing.h` (`cutBack()`, the fence), `IAudioBackend.h` (`setNext()`,
  `takeAdvance()`), `Core2AudioBackend.h`, `RingOutput.h`.

## 14. Sources

1. Gabriel Bouvigne, *Mp3 Info Tag revision 1 Specifications* (the LAME
   tag): http://gabriel.mp3-tech.org/mp3infotag.html, and its rev 0
   text for LAME 3.100: https://linux.m2osw.com/mp3-info-tag-specifications-rev0-lame-3100
   (the field order, the 12-bit delay and padding in 3 bytes).
2. LAME's own writer, `libmp3lame/VbrTag.c` in LAME 3.100's sources
   (https://lame.sourceforge.io/). It was not read for this design. Only
   the delay and padding (+21) are used. The later offsets (the music
   length, the CRCs) are rev 1's [1], and they feed only a logged CRC
   check, so an error there can't change a trim. When `LameTag` is
   written, check them against `VbrTag.c` and a real file.
3. Mark Taylor, *LAME Technical FAQ*: https://lame.sourceforge.io/tech-FAQ.txt
   ("All decoders I have tested introduce a delay of 528 samples").
4. FFmpeg, `libavformat/mp3dec.c`, `mp3_parse_info_tag()`:
   https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/mp3dec.c
   (`start_skip_samples = start_pad + 528 + 1`, `first_discard_sample =
   -end_pad + 528 + 1 + frames * spf`, the "LAME"/"Lavf"/"Lavc" check, the
   Xing frame skipped as non-audio, the duration `frames × spf − start_pad
   − end_pad`).
5. Rockbox, `lib/rbcodec/codecs/mpa.c` (libmad-based):
   https://github.com/Rockbox/rockbox/blob/master/lib/rbcodec/codecs/mpa.c
   (`mpeg_latency[3] = { 0, 481, 529 }`, `start_skip = lead_trim +
   latency`, `stop_skip = tail_trim − latency` clamped at 0, the decoder
   started at `first_frame_offset` past the Xing/LAME frame); and the
   tracker task FS#11891, https://www.rockbox.org/tracker/task/11891.
6. mp3splt bug #206, *Issues with Xing/Lame/Info MPEG frame*:
   https://sourceforge.net/p/mp3splt/bugs/206/ (LAME's frame count is
   the audio frames, not the Info frame; mpg123 expects that).
7. JLayer issue #15, *Xing/Info header frame is decoded as audio, and
   LAME gapless delay/padding are never trimmed*:
   https://github.com/umjammer/jlayer/issues/15 (the Info frame decodes to
   1,152 samples of exact silence; kept = frames × 1152 − delay − padding;
   drop delay + 529 at the front, stop padding − 529 short of the end).
8. libmad, `MAD_BUFFER_GUARD` and `mad_header_decode()` in this tree's
   ESP8266Audio copy (`src/libmad/frame.c`, `stream.h`): a frame is decoded
   only with 8 bytes after it.
