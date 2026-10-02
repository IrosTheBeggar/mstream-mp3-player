# Gapless playback

When a track ends by itself, the next one should start on the very next
sample: no gap, no fade, no click. Albums whose tracks run into each other
(Daft Punk's *Discovery*, live albums, DJ mixes, classical movements) then
play as one piece. This document is the design: what happens today, the
mechanism, the MP3 trimming rules with their sources, what happens when
the listener changes something while the next track is already decoded,
the portable pieces and the firmware wiring, and the host and device test
plans.

**Status: design only, nothing built.** Read from the code at 6e2bbe8
(v0.5.0 merged, NEXT_RELEASE 0.6.0). `G0` on the console turns all of it
off (section 9): that is the v0.5.0 behaviour, for the A/B and as a safety
valve.

Where the facts come from:

- **The code**, by file and function. Line numbers drift, so they're given
  only where they help.
- **Primary sources for the MP3 rules**, cited where they are used
  (section 4) and listed in section 14.
- **Nothing here is measured yet.** Section 11 says what to measure on the
  device and what would change the design.

## 1. What happens today

Here is what happens at a natural end, step by step:

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
   decoder waits.
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

## 3. The mechanism, end to end

```
 loop task                                   decode task                          consumer (BtSink / SpeakerSink)
 ─────────                                   ───────────                          ──────────────────────────────
 PlaybackController::update()                produceDecoded(): N decoding          reads N from the ring
   syncHeard(): takeAdvance()?                 ...
   refreshOffer(): the entry after             N's EOF ──► atSourceEnd():
   the current one ──setNext(offer)──►           commit the stage, J = write index
                                                 mark() the converter (PSRAM)
                                                 offer for this generation,
                                                 not taken, no boundary pending?
                                                   open N+1 (same file_ object)
                                                   same rate: carry on
                                                   another rate: finish(), reset
                                                   boundary {gen, token, J, B}
                                               produceDecoded(): N+1 decoding     ... still reading N
                                               (each pass: offer still the
                                                same token? else cutBack(J))
                                                                                   readPos passes B
   takeAdvance(): readPos >= B ◄──────────── (boundary book, under lock_)
   current := the offered entry,
   no play(): Now Playing switches
   refreshOffer(): the entry after N+1
```

### 3.1 The offer: the controller says what comes next

`PlaybackController` (lib/core) is the only place that knows the queue's
real next entry. It works it out the same way `advance()` would, without
moving anything. Every `update()`, and at the end of every public action,
`refreshOffer()` works out the offer:

- **There is no offer** when any of these is true:
  - gapless is off (`setGapless(false)`, the console's `G0`);
  - the backend doesn't hold the current entry's track (`Stopped`, or
    `cued_`: nothing decodes);
  - the next start would wait for the output (`held()`: Bluetooth is the
    output and the headphones aren't connected, so `advance()` would turn
    the play into a wait; `PlayGate` decides it, as today);
  - "pause after this track" is set (`pauseAfter_`), or the new
    `NextGate` hook says the sleep timer ends at the current entry
    (section 5.4);
  - the queue ends after the current entry without repeat
    (`QueueModel::peek(+1, repeat_)`, new, returns `kNone`).
- **Otherwise the offer is that entry.** That includes the same entry
  again in a one-entry queue with repeat, which loops it gaplessly. The
  offer carries the entry's path (`TrackCatalog::path()`), its length
  hint (`durationHintMs()`), its queue key and a fresh token.

The offer is sent to the backend (`IAudioBackend::setNext()`, new) only
when it changes. It changes when the next track changes (its path) or
when it appears or goes away. A key change alone, such as a library
rebuild's `assign()` (fresh keys, same tracks), only re-keys it, with no
cut. `play()` and `stop()` clear the backend's offer synchronously, on the
loop task, and the controller then sends its offer again. Shuffle,
repeat, the queue edits and the sleep timer therefore need no hooks of
their own: anything that changes the next entry changes the offer.

There is no shuffle mode today. "Shuffle all" (`Ui::shuffleAll()`)
replaces the queue (`playNow()`), which is a new generation, and repeat
has no UI (`setRepeat()` is never called; it is on). If either is added
later, it only has to change what `peek()` returns.

### 3.2 Decode-ahead at the end of a file (decode task)

`produceDecoded()` and `produceTone()` reach the end of their source
(`sourceDone_`). Instead of going straight to `finishSource()`, they call
`atSourceEnd()`, which does the following:

1. **Commit everything.** It converts the held block, pushes the stage
   into the ring (waiting for room, as `finish()` does) and drops the
   `TrimFeed`'s end hold (N's padding; section 4.4). J is now the ring's
   write index, the frame after N's last committed frame.
2. **Mark.** `RingFeed::mark()` saves the converter (about 1.9 KB) and the
   feed's scalars into a PSRAM buffer allocated once at `begin()`. The
   passthrough has no converter state, so a 44.1 kHz mark is a few words.
3. **Take the offer** (under `lock_`) if all of these hold:
   - gapless is on;
   - the offer's generation is this generation's (`setNext()` stamps it
     with `sync_.generation()`, so an offer made before the newest
     `play()` is never taken);
   - its token hasn't been taken before (an offer is taken once);
   - no boundary is pending.
4. **Probe N+1's rate before any of its frames** with what `openDecoder()`
   already reads:
   - an MP3's first frame header after its tags (`progress::parseMp3Frame`);
   - a FLAC's STREAMINFO (`trackseek::flacStreamInfo`);
   - a built-in track's rate (`ToneTrack::parse`).

   If `RateConverter::plan()` refuses the rate, that is a failed open
   (section 5.3).
5. **Choose the join.**
   - **Same rate and same route** (almost every album): nothing changes in
     the feed. N+1's frames continue N's stream through the same converter
     state. B is computed: `B = streamStart + ceil(taken × num / den)`.
     Here `taken` is the converter's source frames since the stream began
     (`RateConverter::taken()`), and `streamStart` is the ring index of
     that stream's first frame. That is exactly the frame count
     `finishPush()` would have stopped at, so ring frame B is N+1's first
     source sample on the 44.1 kHz grid (to within one ring frame, 22.7 µs,
     at a converting route). For the passthrough, B = J.
   - **Another rate:** `finish()` pushes N's tail (waiting for room), so
     N ends with exactly ceil(N's frames × num / den) frames. Then the
     converter is reset (a new stream: `streamStart` = the write index) and
     N+1's frames come in on `Hold` until its decoder says its rate, as at
     any start. B is the write index after the tail.
6. **Open N+1**, from its start and from the same `file_` object
   (`closeDecoder()` has already run for N). This is `openDecoder()` as
   at any start, with the trimming armed (section 4). There is no
   `discardAll()`, no `start()`, no new generation and no
   `startTiming()`. The decode task's per-track counters
   (`producedFrames_`, `srcPos0_`, `srcPos_`, `srcSize_`, `busyUs_`,
   `described_`) restart for N+1, the decoding track. The heard track's
   values are kept apart (section 3.5).
7. **Record the boundary** in the boundary book (section 3.4) under
   `lock_`. Then go on producing. The phase stays `Decoding`.

With no offer, nothing changes from today: `finish()`, `closeDecoder()`,
`Draining`, `Ended`. In `Draining` the decode task looks for an offer each
time it polls the ring. A late one, such as + Queue onto the last entry
or the sleep timer turned off in the last second, is taken while the ring
still has N in it. N's tail was already flushed, so that join resets the
converter (a converting route only).

Timing: the open happens with up to 1.49 s of N still in the ring, while
the decoder would otherwise only wait for room. An MP3 opens in 23-30 ms
(the ID3 skip, ARCHITECTURE.md), and a FLAC's libFLAC init is of the same
order. After the open, the ring is still nearly full, so there is no
refill from empty. There is no `RefillPacer` window (`fullMs_` is already
set) and none of today's 0.6-0.8 s UI stall at a natural end.

### 3.3 Built-in tracks

A tone or click track ends when `ToneGen`/`ClickGen` returns 0 frames
(`produceTone()`, `sourceDone_`), and the same `atSourceEnd()` runs. The
built-in tracks are made sample-exact at their own rate, so no trimming
applies. A file to a tone, or a tone to a file, at different rates takes
the rate-change join. The test tones' own 5 ms attack and release make
their joins click-free by content.

### 3.4 The boundary book (lib/core `GaplessJoin`)

The shared state between the loop and the decode task fits in one small
portable class, `GaplessJoin`. It is guarded by the backend's `lock_`,
never by the consumer, and both tasks may wait on it briefly. It holds:

- **the offer slot**: path, length hint, token, generation (loop writes;
  decode takes);
- **the last token taken**, so an offer is never taken twice;
- **at most one pending boundary**: generation, token, J (where a cut
  goes back to), B (where the listener's track changes), and N+1's known
  length if the file says it;
- **N's frozen record**, from its end of file: its ring start, its start
  offset (`startMs_`) and its exact length,
  `(B − start) / 44.1 + startMs`;
- **the join's state**: `Pending` (cuttable), `Cutting` (fence set, the
  cut not finished yet), `Committed` (the consumer passed J: too late to
  cut).

The calls on it:

| Call | Task | What it does |
|---|---|---|
| `setNext(offer)` | loop | replaces the slot; wakes the decode task (`xTaskNotifyGive`) so a cut happens at once, not after its 10 ms sleep on a full ring |
| `clear()` | loop | from `play()`/`stop()`: slot and boundary dropped (the decode task's `discardAll()` drops their frames) |
| `takeOffer(gen)` | decode | step 3 of section 3.2 |
| `joined(...)` | decode | records the boundary |
| `wantsCut(gen)` | decode | a pending boundary whose token isn't the slot's any more (changed, withdrawn, gapless off) |
| `takeAdvance(readPos, gen)` | loop | `readPos − B` as a signed 32-bit difference (the ring's counters wrap) ≥ 0, and the boundary's generation is the newest: pops it and returns its token |
| `positionLimit()` | loop | B while a boundary is pending, so `positionMs()` never runs past N's end |

### 3.5 Positions, durations and what switches when

The backend keeps the **heard** track's numbers apart from the
**decoding** track's. While no boundary is pending they are the same
track, as today.

- `positionMs()` is `heardStartMs + (min(readPos, B) − heardStart) / 44.1`.
  The `min` holds N at its exact end for the moment between the consumer
  passing B and the loop taking the advance (at most one loop pass).
- `durationMs()` is the heard track's: the frozen exact length once its
  file has ended, else its known length (header, STREAMINFO), else
  `TrackProgress`'s estimate from the decoding counters, which belong to
  the heard track while it is the one decoding. A benefit: every track's
  length becomes exact at its end of file, so the sleep timer's
  last-10-s fade is placed exactly.
- `description()` (and `note()`) switch at the advance.
- `takeAdvance()` rebases `heardStart` to B and `heardStartMs` to 0, and
  makes N+1's length the heard one. All of this happens in the call that
  tells the controller, on the loop task. So the queue entry and the
  backend's position change together, in the same `player.update()`.
  Nothing on the loop can see the new entry with the old position, or the
  other way round.

On the controller's side (`PlaybackController::syncHeard()`, new), a
token that matches the offer means:

- the entry with the offer's key becomes current (`queue_.setCurrent()`,
  by `positionOf(key)`), with no `play()`;
- `failuresInARow_` goes to 0 (N played through), the start point and
  `playedFromMs_` are cleared, `cued_` is false, and the state is
  unchanged;
- if the key is gone (an edit after the consumer passed J; section 5),
  the controller does `advance()` from N: a normal start of what follows
  now.

`syncHeard()` runs at the top of `update()`, before its "not Playing:
return", and at the top of every public action (next, prev,
`togglePlayPause()`, `cue*`, the edits, `setStartPoint`,
`stopKeepingPlace`). A next pressed 20 ms into N+1 therefore skips N+1,
not N. A prev there goes to N, which is what `prevRule()` would say.

Who follows:

- **Now Playing, the Queue's mark, `[queue] now at ...`, `danceMode.
  onTrackChanged()`** read the queue's current entry, so they switch at
  the advance (main.cpp's loop, after `player.update()`).
- **The sleep timer's `EntryStart`** sees the key change with the position
  under 1 s. That is "started", so the timer gets N+1's length at once,
  never N's end.
- **The Queue's learned lengths** (main.cpp: `started = pos < 1000` at the
  key change) work the same way.
- **`QueueSaver`/`QueueStore`** see the current position move: the
  position is saved within a second, and the resume point is removed
  (playback moved on), as after an advance today.
- **The resume point** (`resumePoint()`, at a pause) is the heard track's
  position.
- **The dancer.** `AudioTap` positions are in the ring's epoch, and a
  join doesn't change the epoch. The beat tracker therefore keeps its
  lock through a join, which is what a segue wants: the beat runs on.
  `onTrackChanged()` still clears the tempo prior at the advance. For the
  click-track truth, `DanceMode` takes the boundary's frame in the epoch
  (`B − epochStart`, new: `Core2AudioBackend::heardEpochFrame()`) and
  re-arms its truth from the new path with that offset.
- **HostLink's epochs** are the computer's. Host mode pauses the player
  (`pauseByComputer()`), so no join can happen during a session.
  Nothing changes there.

`AudioTap`'s comment ("the track frame, the counter positionMs() is made
of") becomes "the frame in its epoch; `positionMs()` subtracts the heard
track's start in it".

### 3.6 TransportSync's rules

`TransportSync` doesn't change, and its rules hold:

- **Only requests post generations.** A join is part of the request that
  started N, so the phase stays `Decoding` across it.
- **`finished()` and `failed()` still describe only the newest
  generation.** `Ended` comes only after the last track (one with no
  offer) has drained. A failure of the decoded-ahead track is never
  reported while N is still heard (section 5.3). It surfaces only when
  that track becomes the heard one, or as today through `play()`.
- **Stale reports are dropped, and so are stale joins.** Offers and
  boundaries carry their generation. `takeOffer()` and `takeAdvance()`
  ignore every other generation, and `play()`/`stop()` clear both on the
  loop task before posting. A boundary the decode task records for an old
  generation after that (it was mid-pass) is never taken.
- **`positionKnown()`** is unchanged (`Pending` only after a `play()`).
  Advances never pass through `Pending`.
- **`isPlaying()`** (`Pending`, `Decoding`, `Draining`, not paused) is
  true across a join without a blink.

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
  MPEG-2/2.5 stereo), 4 + 9 (MPEG-2/2.5 mono). This is what
  `TrackProgress.cpp`/`TrackSeek.cpp` already compute as
  `i + 4 + f.sideInfo`.
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
  - +34: the tag's CRC-16, over the frame's first 190 bytes.
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
- **The Info frame is decoded as audio** if the decoder is handed it. The
  backend hands the decoder byte 0 (through the ID3 reader) or the end of
  a big ID3 tag, so today the Info frame comes out as 1,152 samples of
  exact silence: its side information is all zeros [7]. That is 26 ms of
  extra silence at every LAME-encoded track's start. **From now on, when
  the probe finds a Xing/Info (or VBRI) frame, the decoder is handed the
  byte after it** (the probe knows its offset and length), without the ID3
  reader. The library has the title and artist: `trackTitle()` and
  `trackArtist()` have no reader outside the backend (grep), and a big
  tag is already skipped that way. The same applies without a LAME
  extension: a header frame is never audio.
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

### 4.4 Where trimming runs: `TrimFeed` (lib/core)

`RingOutput::ConsumeSample()` hands the sample to `TrimFeed` when the
track has a trim, and straight to `RingFeed` otherwise. A FLAC pays one
branch per frame.

- **The start: a count.** While `skip > 0`, the frame is taken and
  dropped (`return true`, so the generator moves on).
- **The end: a hold of `hold` frames** in a FIFO in PSRAM. It is
  allocated once at `begin()`: 4,095 frames is 16 KB, the largest padding
  the 12-bit field can say. Once the FIFO is full, a new frame can only
  go in if the oldest goes into `RingFeed` first. If `RingFeed` refuses
  the oldest (ring full, budget spent), `TrimFeed` refuses the new frame
  and changes nothing, so the generator's "offer the same sample again"
  contract holds exactly as in `RingFeed`. At the end of the file
  (`atSourceEnd()`), whatever is held is dropped: that is the padding.
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

`TrimFeed` holds a pointer and four counters (about 24 B). It goes into
`RingOutput`, which stays under its 4 KB `static_assert`.

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
trimmed" at the open.

FLAC is sample-exact: libFLAC outputs STREAMINFO's total, and the
generator adds nothing from the top. The built-in tracks are made to the
frame.

### 4.6 Seeks and resume starts on the trimmed timeline

There is one timeline: the trimmed one, where 0:00 is the first kept
sample.

- **Lengths.** `progress::mp3HeaderDurationMs()` and
  `trackseek::mp3LengthMs()` return `kept / rate` when a LAME tag says
  delay and padding. Without one, they return `frames × spf / rate`, as
  today. Now Playing, the Queue, `trackseek::startMs()`'s last-5-s rule
  and the resume point's saved length all use it.
- **The byte for a start at T.** The untrimmed sample is
  `T × rate + delay + 529`, so the frame index is that over spf. For a
  CBR Info file the byte is computed from that frame index. A TOC or the
  average bitrate maps the untrimmed time (T plus 25 ms or so) instead
  of T. This removes a fixed +25 ms bias from MP3 seeks, which were
  measured 30-50 ms behind (ARCHITECTURE.md). The rest is the frame
  libmad drops for its bit reservoir, as before.
- **The trim after a seek start.** Only the lead is skipped (the decoder
  warm-up was played and faded in before too), and the end hold applies,
  so a resumed track joins its next gaplessly.
- **A FLAC seek**: the lead (1) is skipped. That is one sample
  (22.7 µs), which makes positions after a FLAC seek exact to the sample.
- **`positionMs()` after any start** is the start plus the kept frames,
  as now.

## 5. Changes while the next track is already decoded

Most edits change the offer (section 3.1). That is noticed in the same
loop pass, and the decode task acts on it at once.

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

**Then the decode task** finishes the cut:

- **Done:**
  - `RingFeed::rewind()`: the converter and feed scalars come back from
    the mark. Staged and held frames are dropped. The table pointers are
    re-pointed through the current plan, and the internal-RAM copy is
    wanted again if the route converts, because N+1 at 44.1 kHz may have
    freed it (`TableCopy`, RESAMPLER.md section 10c).
  - N+1's decoder is closed and the boundary dropped.
  - The decode task is back at "N at its end of file" and goes to step 3
    of section 3.2 with the offer as it is now: a new next entry, or
    none, which means `finish()` → `Draining`.
- **Pending:** the fence stays and the cut is tried again on the next
  pass. Nothing is decoded meanwhile.
- **Crossed:** the boundary becomes `Committed` and is never cut. The
  listener is about to hear N+1's start (the consumer is in [J, B) or
  past it). The advance comes at B, and the controller then treats the
  change as an edit to what plays (sections 3.5 and 5.2).

**The race windows.** An edit reaches the decode task within about 1 ms
(it is woken). A cut is too late only if the consumer passes J in that
millisecond. A re-decode after a cut has the time the consumer still
needs to reach J. That is usually more than a second, and at least an
open (23-30 ms for an MP3, more for a FLAC's init). An edit in the last
~30-100 ms before the join can leave a short gap between N's end and the
new next track: N's tail is whole, then the new track starts as any start
does (faded in after the gap). That is the v0.5.0 behaviour at that one
join.

### 5.2 The cases

When this table says the consumer is before J, N plays out whole and
nothing of the old N+1 is heard. When it says the consumer passed J, the
change came after the join: the advance is heard first and the change
applies to the track that now plays.

| Change while N+1 is decoded ahead | What happens |
|---|---|
| Next, prev, `play(pos)` (another entry), Play (`playNow`), Shuffle all, `setStartPoint` while playing, a restart by prev, stop, Clear | a request, as today: `play()` or `stop()` clears the offer and the boundary, the decode task's `start()` calls `discardAll()` (the epoch bumps, so the outputs crossfade) and resets the converter. `syncHeard()` ran first, so the action is on the entry the listener hears (section 3.5) |
| Play next / `insertNext`, `moveNext`, + Queue onto the last entry, `remove` of the next entry, Clear up next, undo that changes the next entry | the offer changes. Consumer before J: cut, then the new next is decoded. Passed J: the advance is heard, then a removed key makes the controller `advance()` from N (a start of the new next, crossfaded), and a moved one is simply found where it went |
| `remove` of the current entry (N) | `currentMoved()`: a request, as today |
| A library rebuild (`queueReplaced(true)`: fresh keys, same tracks) | the offer is re-keyed, no cut (section 3.1) |
| Repeat changed (no UI today) | at the queue's last entry the offer appears or goes; a cut if needed |
| Gapless turned off (`G0`) | the offer goes: cut, and N ends as in v0.5.0. Trimming stays as it was for tracks already open (`Gt` applies at the next open) |
| The sleep timer's end chosen or its kind changed (End of track, album, queue) | section 5.4 |
| "Pause after this track" (`setPauseAfterTrack(true)`) | the offer goes: cut, N drains, `Ended`, `pauseAtBoundary()` as today |
| Pause | nothing to cut: decoding ahead while paused is harmless, because nothing reads. Resume plays the join gaplessly |
| The output switched (speaker ⇄ Bluetooth) | a consumer handover (`setConsumer()`); the read index continues, the boundary stays valid |
| A `Hold` appears (headphones gone) | the drop pauses the player (`BtSession`), so the offer goes with the pause. If the controller is still Playing, `held()` makes the offer go: cut |
| The USB visualizer starts | `pauseByComputer()`: a pause, as above |

### 5.3 Failures

- **The next track can't be opened** (missing file, not an MP3 or FLAC,
  a rate the converter refuses, an unknown `tone:`, a decoder that won't
  begin): the decode task marks the offer's token as failed and logs
  `[gapless] can't decode ahead ...: <why>; N ends as before`. Then it
  does what it does with no offer: `finish()`, `Draining`, `Ended`. The
  controller's `advance()` then calls `play()` on that entry, which fails
  as today (`Failed`, the note "Skipped ...", the Queue's mark, the next
  one). The file is opened twice, which costs milliseconds. A transient
  error gets its second chance, and the failure path is the one that
  exists and is tested. N ends cleanly. The decode task never waits on
  the failed track.
- **The decoded-ahead track fails while N is still heard.** The decoder
  starts N+1 fine and then fails, for example with a mid-stream rate
  change to a rate that is refused (`f.rejected()`; rare). The decode
  task tries the cut first. Done means N ends cleanly and the failure
  comes again through `play()`. Crossed means the decode task stops
  producing, waits until the boundary has been taken
  (`GaplessJoin::pending()` false), then reports `Failed` for the
  generation. `failed()` then describes N+1, the heard entry, never N.
- **A decode error in the middle of N+1** (a corrupt file) ends it early,
  as libmad's errors do today. It is a short track, and the next join
  follows.
- **An underrun at the join** (an SD stall during the open) is counted
  and faded like any underrun (`expectingAudio` stays true).

### 5.4 The sleep timer

- **The predicate.** `NextGate` (new, like `PlaybackController::Hold`)
  asks whether the timer ends at the current entry. main.cpp implements
  it with a pure `SleepTimer::endsAt(choice, lastOfAlbum, lastOfQueue)`,
  taken from `atBoundaryTrack()`:
  - End of track: always;
  - End of album: `albumEndsBetween(current, next)` or the last entry;
  - End of queue: the last entry.

  The offer is withheld whenever it says yes. N+1 is then never decoded
  ahead of the track the timer ends at, and that stays true right after
  an advance. In the same `update()` that makes A9 (an album's last
  track) current, the next offer is computed with A9 as the boundary
  track. It doesn't wait for `stepSleep()` to set `pauseAfter_` in the
  next pass.
- **The pause.** With no offer, N drains, `Ended`, `pauseAtBoundary()`:
  exactly today's End of track. The ring empties with N's last frame,
  nothing of N+1 is decoded, and the next entry is cued at 0:00.
- **Chosen late** (N+1 already decoded): the gate changes the offer, so
  the cut happens. If the cut is Done, it pauses exactly at the boundary.
  If it is Crossed (the choice landed within a millisecond of the
  consumer passing J), N+1 is the heard track, and End of track applies
  to it ("a skip during the countdown: End of track then applies to the
  new track", SleepTimer.h). That is the one way N+1 is heard, and it is
  the same as choosing a moment after the track changed.
- **The fade** is computed from the heard track's position and length.
  N's length is exact once its file has ended (section 3.5). The fade
  over the last 10 s ends at the boundary, and `FadeStage` is one level
  for the stream, so a join changes nothing about it.
- **Timed choices** don't involve the boundary. They fade and pause by
  the clock, wherever the stream is.

## 6. Memory, CPU and the stack

- **Decoders.** There is still one at a time. N's generator is
  `stop()`ped (libFLAC's ~100 KB PSRAM and 2.5 KB internal freed, as
  `closeDecoder()` does today) before N+1's is made. The peak is today's
  per-track peak.
- **Internal RAM** (all estimates; `[heap] playing` measures it):
  - `GaplessJoin`: about 64 B plus the offer's path string. It sits in
    `Core2AudioBackend` (a global, .bss). The `std::string` path is
    allocated internally (under 4 KB), up to `TrackCatalog::kMaxPath`.
  - `PcmRing`: +12 B.
  - `TrimFeed` in `RingOutput`: +24 B. `static_assert(sizeof(RingOutput)
    < 4096)` still holds (3,192 B today).
  - There is no `IRAM_ATTR` anywhere.
- **PSRAM:**
  - the converter's mark: one `RateConverter` image, about 1.9 KB
    (`sizeof`, asserted);
  - the `TrimFeed` hold: 16 KB;
  - both allocated once at `begin()`.
- **The decode stack (16 KB).** The join's path, `decodeTask →
  produceDecoded → atSourceEnd → openDecoder → AudioGenerator*::begin`,
  is no deeper than today's `decodeTask → start → openDecoder → begin`.
  The deepest frames stay libFLAC's and libmad's inside `loop()`, which
  doesn't run during an open. The probes stay in PSRAM. The device run
  watches `decodeStackFree` (`s`) across joins of each kind.
- **CPU:**
  - The open moves from a moment when the ring is empty and the UI waits
    to a moment when the ring is full and the decoder would otherwise
    sleep.
  - The refill from empty disappears at natural ends, which saves about
    0.7 s of flat-out decoding per track.
  - The hold costs under 0.5 % of a core for LAME MP3s (estimated;
    measured in section 11.4).
  - A mark is a 1.9 KB copy once per join, and only at a converting
    route.

## 7. Hearing safety

- **Nothing starts by itself beyond the normal auto-advance.** Offers
  exist only while the controller holds a track that plays or is paused,
  and only for the entry `advance()` would start. An offer decodes into
  the ring; only the consumer reading makes anything heard.
- **A paused player never advances.** Paused outputs don't read, so
  `readPos()` can't pass B. One edge: a pause within 64 frames (1.5 ms)
  of B. The pause's own 64-frame fade-out reads across B, so the player
  ends paused at N+1's 0:00. That is what was heard, nothing plays, and
  a resume continues N+1.
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
| `RingFeed` | `mark(buffer)` (stage committed, converter and scalars saved), `rewind(buffer)` (restore, drop staged/held, re-point tables); `boundary()` = `streamStart + plan.ringFrames(taken)`; `endStream()` = `finish()` then a converter reset that keeps `made()` running (a rate-change join) |
| `RateConverter` | copy-out/copy-in of its state (`save()`/`restore()`), with `restore()` re-pointing the polyphase rows to the tables in use now (`useTables()` may have moved them) and wanting the copy if the route converts |
| `LameTag` (new) | `lametag::parse(buf, n, &Info)`: the first frame's Xing/Info header and LAME extension (frames, spf, rate, delay, padding, CRC ok, the header frame's offset and length) per section 4.1 |
| `GaplessTrim` (new, in LameTag) | `skip`/`hold`/`kept` per section 4.2-4.3, the lead per decoder and start kind; the trimmed length; the untrimmed time for a seek |
| `TrimFeed` (new) | the start skip and the end hold in front of `RingFeed` (section 4.4) |
| `GaplessJoin` (new) | the offer slot, the boundary book, the heard record, the join's states (section 3.4) |
| `QueueModel` | `peek(delta, wrap)` |
| `PlaybackController` | `setGapless()`, `NextGate`, `refreshOffer()`, `syncHeard()` (section 3.1, 3.5) |
| `IAudioBackend` | `setNext(const Next*)` and `takeAdvance(uint32_t* token)`, with default bodies (the five fakes in test/ compile unchanged) |
| `SleepTimer` | `endsAt(choice, lastOfAlbum, lastOfQueue)` |
| `TrackProgress`, `TrackSeek` | the trimmed lengths, the delay in the seek byte (section 4.6) |

### The firmware (src)

| Where | What |
|---|---|
| `audio/Core2AudioBackend` | `atSourceEnd()`, the join (section 3.2), the cut each pass (`wantsCut()`), the heard and decoding records, `positionMs()`/`durationMs()`/`description()` from the heard one, `setNext()`/`takeAdvance()`, `heardEpochFrame()`, the mark and hold buffers at `begin()`, the `[gapless]` log lines, counters for `G` |
| `audio/Core2AudioBackend::openDecoder()` | the LAME tag from the probe it reads already; the decoder handed the byte after a Xing/Info/VBRI frame; `GuardedSource` for MP3; the trim armed in `RingOutput` |
| `audio/GuardedSource` (new) | 8 zero bytes at the end of the file (section 4.3) |
| `audio/RingOutput` | `TrimFeed` in front of `RingFeed` |
| `main.cpp` | the `NextGate` (the sleep timer), the console `G`, `DanceMode`'s truth re-armed at the advance |
| `app/SerialConsole` | `Pending::Gapless` on `G` |
| `app/DanceMode` | the click truth re-armed from `heardEpochFrame()` at a gapless advance; no tracker reset there |

The log, one line per event:

- `[gapless] decoding ahead: 06 - Digital Love.mp3 (MP3, 44100 Hz, same rate: continuous) with 1,472 ms of 05 left; opened in 27 ms`
- `[gapless] trim: LAME delay 576, padding 1,308: skipping 1,106, holding 779` (or `no LAME tag: not trimmed`)
- `[gapless] heard: 05 -> 06 at ring frame N (taken 3 ms after the consumer passed it)`
- `[gapless] cut: the next entry changed: 1.21 s of 06 dropped, 05 ends at frame N` (or `too late to cut: 06 already heard`)
- `[gapless] can't decode ahead 07 - x.flac: <why>; 06 ends as before`

## 9. The console switch: `G`

`G` is free (SerialConsole.cpp's key list). It takes an argument up to
Enter, as `T` and `R` do.

- **`G`**: the status. It shows on or off, trimming on or off, the offer
  (the entry, its path), the pending boundary (J, B, ms of ring to it,
  the join's kind and state), and the counters since boot: joins
  continuous, joins with a reset, cuts, too-late cuts, failed opens. It
  also shows the current track's trim (delay, padding, skip, hold, CRC)
  or "no LAME tag".
- **`G0`**: gapless off. That is v0.5.0 at the next end: no offers (a
  pending decode-ahead is cut at once), and from the next open no
  trimming, no skipping of the header frame and no guard bytes. It is
  RAM only, for the A/B and as the safety valve.
- **`G1`**: on (the default; `-DMSTREAM_GAPLESS=0` builds it off by
  default).
- **`Gt0` / `Gt1`**: trimming and the header-frame skip off or on with
  decode-ahead left as it is. This measures the trimming's share of a
  join.
- **`Gp<sec>`** (temporary, for the device run only, removed after it
  like RESAMPLER.md's `Rp`): the join probe of section 11.

## 10. Host tests (`pio test -e native`)

Everything here runs with synthetic decoders. A **fake source** is an
array of frames at a given rate. It can be given a fake lead, a fake
delay and padding (junk values), a failure at open or after k frames,
and the "ring full" refusals of the existing `test_ring_feed` harness.
A **fake consumer** reads random chunks (0-1,500 frames, nothing a third
of the time), as `test_long_runs_through_a_full_ring_are_exact` does.

**test_pcm_ring**

- `cutBack()`: basic (write 100, read 10, cut to 50, size 40, the next
  write lands at 50);
- refused when the reader is past the cut;
- equal to the read index;
- near the 2^32 wrap;
- `discardAll()` clears a fence;
- a read clamped by a fence;
- a stress test with two `std::thread`s. Frames are tagged with
  (sequence, track). The consumer must never see a frame of a cut track
  after a Done, never a short read except at a fence, and never a
  negative size.

**test_ring_feed**

- `mark()`, feed X, `rewind()`, feed Y gives the same bits as feeding Y
  right after `mark()`. This is checked at every route, at random points,
  with random refusals.
- A tables move (`useTables()` to a copy) between mark and rewind changes
  nothing.
- A same-rate join is bit-identical to the concatenated stream converted
  in one go, at every rate, and `boundary()` is `ceil(taken × num / den)`.
- A rate-change join gives N converted alone followed by N+1 converted
  alone, with N's exact frame count.

**test_lame_tag** (new)

- Synthetic first frames:
  - MPEG-1 stereo and mono, MPEG-2 and 2.5 LSF;
  - every subset of the Xing flags, so the LAME extension moves;
  - "Xing" and "Info";
  - "LAME", "Lavf" and "Lavc" accepted; another string, a Xing header
    without the extension, and a VBRI header give no trim.
- The delay and padding are read from `[xxxxxxxx][xxxxyyyy][yyyyyyyy]`,
  including 0 and 4,095.
- The CRC is computed over 190 bytes.
- The sanity rejections.
- The header frame's length is reported.
- The trim maths: skip, hold (and the `padding < 529` clamp), kept, the
  trimmed length in ms, the untrimmed time for a seek. There is one
  worked example from a real LAME 3.100 file's header bytes, written out
  by hand in the test.

**test_trim_feed** (new)

- Through `RingFeed` and the real `PcmRing` with random refusals: exactly
  `kept` frames come out, bit-identical to the source's middle.
- A refusal never loses or repeats a frame (the hold's retry).
- A start after a seek skips only the lead.
- The hold is dropped at the end.
- A hold of 0 and a skip of 0 are pure passthrough.

**test_gapless** (new; `GaplessJoin` plus a host model of the decode task
built from the same pieces)

- Sample-exact joins: the consumer's output equals the concatenation of
  the trimmed tracks, frame for frame, at:
  - 44.1 → 44.1;
  - 48 → 48 (equal to converting the concatenated 48 kHz stream);
  - 48 → 44.1 and 44.1 → 22.05 (rate-change joins: the tail, then a
    fresh filter);
  - a tone between two files;
  - a one-entry repeat (the same track twice);
  - a track shorter than the ring (the depth-1 wait), with no frame lost
    or added.
- Boundary-heard timing:
  - `takeAdvance()` is false until `readPos ≥ B` and true at the first
    read past it, once;
  - `positionMs()` runs to N's exact length and stays there until the
    advance, then reads from 0;
  - `durationMs()` is N's exact length after its end of file;
  - wrap-safe near 2^32.
- Each edit case of section 5.2, at three moments:
  1. before N's end of file (no cut: the new offer is simply taken);
  2. after the join with the consumer before J (a cut: the output is N
     then the new next, sample-exact, with nothing of the old N+1 in it);
  3. with the consumer past J (Crossed: N+1 heard; the advance taken;
     the controller then acts on it).
- `G0` during a pending join: the cut, then `Draining`, `Ended`.
- A late offer during `Draining` is taken (a reset join).
- Failures: an open failure gives N whole, then `Ended`, and no `Failed`
  for N. A failure after the join gives a cut (Done) or a `Failed` only
  after the advance (Crossed).
- Generations: an offer stamped with an old generation is never taken; a
  boundary recorded after a newer `play()` is never taken; `play()` and
  `stop()` clear both.

**test_playback** (the fake backend gains `setNext`/`takeAdvance`)

- The offer is the entry `advance()` would start:
  - none at the end without repeat;
  - the same entry in a one-entry queue with repeat;
  - none while Stopped, cued, `held()`, with `pauseAfter_` set, when
    `NextGate` says so, or with gapless off.
- It is re-sent after every edit that changes the next entry (`insertNext`,
  `append` at the end, `remove` of the next, `moveNext`, `clearUpNext`,
  `undo`) and not after edits that don't. A re-key doesn't count as a
  change.
- The advance moves the current entry without a `play()` and clears the
  start point; a removed key leads to `advance()`; a moved key is found.
- `syncHeard()` runs before `next()`, `prev()` and `togglePlayPause()`: a
  next right after an advance skips the new entry.
- `failed()` after an advance is the new entry's: the note, the skip.

**test_sleep_timer**

- `endsAt()` for each choice.
- End of track: no offer on the boundary track, and the pause at
  `Ended`, with `timerStops()` counted.
- End of album: offers inside the album, none on its last track, decided
  in the same update as the advance.
- Chosen late: the offer withdrawn (the cut happens in the backend).
- `EntryStart` is started at once on an advance (the key changes with the
  position under 1 s).
- The fade is placed by the exact length.

**test_queue**

- `peek()` at both ends, with and without wrap, and on an empty queue.

**test_track_seek**

- With a LAME tag, the lengths are trimmed and the CBR Info byte includes
  `delay + 529`.
- Without one, they are unchanged (the existing tests pass as they are).

## 11. Device test plan (silent mode `z`, the speaker)

The setup:

- **Silent mode.** Everything runs in silent mode `z`, on the speaker,
  so nothing is heard (RESAMPLER.md section 6). Bluetooth is item 6, with
  silence tracks only.
- **The probe.** A temporary `Gp<sec>` probe is built in and removed
  after the run, like RESAMPLER.md's `Rp`. It turns the speaker's tap on
  (`setTapsOn(true)`) and, at each advance, reads the tap ±100 ms around
  the boundary's epoch frame (`heardEpochFrame()`). It logs one
  `[gapless probe]` line per join with:
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

`tools/gapless_files.py` (new, on the PC, with `lame` 3.100 and `flac`)
writes `/music/zz gapless test/`:

1. **An impulse.** A single full-scale sample at frame 10,000 of 3 s of
   silence. It is encoded as LAME CBR 128 and as LAME V2. The probe finds
   the impulse's peak in the kept timeline. It must be at frame 10,000,
   ±1 for the MP3's smearing (it is a lowpassed pulse; its peak). If it
   isn't, the constant in `skip` is wrong by that much: the lead, the
   Info frame, or libmad's 529. Fix the constant before anything else.
   The kept length must be exactly 132,300 frames. The trim stage counts
   them, and `G` shows it.
2. **The last frame.** A file of k × 1152 samples with no ID3v1 tag, so
   it ends right after its last frame. Its kept count must be exact.
   Without `GuardedSource` (a `Gt`-style switch for the run only) it
   should come out 1 frame short, which confirms section 4.3.
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
     frames); with `G0`, the gap of today.
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
   - With `G0`: today's gap, which this measures for the first time:
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
  crossfade, as today.
- `G0`: a cut, then a v0.5.0 end.
- `Tt` (End of track) at 1 s before the end: the cut. The tap's last real
  frame is N's last, the player is paused at the next entry's 0:00
  (`[sleep]` lines), and no frame of N+1 has been read.
- `Tt` well before the end: no decode-ahead at all (`G` shows no offer).
- The timing race, on purpose: `Tt` and `qr` at about 20 ms before the
  join, repeated. Count Done, Crossed and Pending, and check that a
  Crossed always leaves the timer acting on the new track and the
  controller on the right entry.
- A missing file as the next entry (renamed on the card): `[gapless]
  can't decode ahead`, N plays out whole, then today's "Skipped ...".
- Pause in the last second, resume after 10 s: the join is gapless.

### 11.4 Cost and limits

- `s` across joins of every kind:
  - FLAC → MP3 → FLAC;
  - 48 → 44.1 (with the tables' copy freed and made again);
  - a tone → a file;
  - a cut followed by a FLAC.

  Check the decode stack free (today's margin, no drop), the internal
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
  stress test (section 10) and an `Rb`-style microbench of `read()` before
  and after are the gates.
- **Files without a LAME tag** keep about 50 ms of encoder silence at a
  join (section 4.5). iTunes' `iTunSMPB` could be added later; it is an
  ID3 comment that the backend doesn't read today.
- **Rate-change joins** have a discontinuity of up to 0.5 ms (a fresh
  filter). Albums rarely change rate mid-album.
- **The late-offer join** (taken in `Draining`, after the tail was
  flushed) has the same 0.5 ms discontinuity at converting routes.
  Deferring `finish()` until the ring is nearly dry would remove it, at
  the cost of one more state. Not planned.
- **A skip during a pending join re-decodes N+1 from its file.** It
  could instead jump the read index to B (the frames are there),
  crossfaded. That is an optimisation for later.
- **A track shorter than about 30-100 ms** (the open time) can't be
  joined gaplessly to its next one (the depth-1 wait). Such tracks are
  rare, and a gap is all that happens.

## 13. Docs to update when it is built

- **ARCHITECTURE.md, "Audio pipeline":**
  - "a natural end drains the ring first" becomes the join;
  - the `discardAll()` and converter-reset rule ("at every request")
    gets "and never at a join";
  - the start-part-of-the-way-in paragraph gets the trimmed timeline;
  - the Tasks table's decode row gets the open at the join.
- **ARCHITECTURE.md**, the other sections:
  - "Library and queue" gets the offer and the advance;
  - "Sleep timer" gets the `NextGate`;
  - "USB visualizer" gets a sentence (no change in host mode).
- **RESAMPLER.md, section 5:** "Resets" (same-rate joins don't reset; the
  mark and rewind) and "Positions and durations" (the heard record).
- **ENERGY.md:** the refill at natural ends is gone; the hold's cost.
- **Header comments:** `AudioTap.h` ("the frame in its epoch"),
  `PcmRing.h` (`cutBack()`, the fence), `IAudioBackend.h` (`setNext()`,
  `takeAdvance()`).

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
