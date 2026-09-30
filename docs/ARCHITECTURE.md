# Architecture

A portable music player on the M5Stack Core2 (original ESP32, 16 MB flash,
8 MB PSRAM). It plays MP3 and FLAC from local storage to Bluetooth headphones
or the built-in speaker, and will keep its library in sync with an
[mStream](https://mstream.io) server over WiFi.

The guiding rule is unchanged from the first scaffold: **portable logic knows
nothing about hardware.** Anything that can be tested on the laptop lives in
`lib/core/` and is covered by `pio test -e native`; `src/` holds the Core2 side.

## Layers

```
              +---------------------------------------------------------------+
  lib/core/   |  PlaybackController   PcmRing   TransportSync   ToneGen       |  portable C++17,
  (portable)  |  BtControl (StreamControl, AbsVolumePolicy)  ReconnectPlanner  |  host-tested
              |  GainRamp  VolumeMath  StreamRestart  HeadsetKeys  SinkSearch |
              |  Declicker  DeclickReader  hal/*                              |
              |  AudioTap  TapReader  BeatTracker  ClickGen  DancePose        |
              |  CrabPose  CrabArt (generated)  DanceSkin  DanceRate          |
              |  LibraryIndex  LibrarySynth  TextFold  TouchGesture           |
              |  KineticScroll  ScrollGovernor  VScrollMap  RefillPacer       |
              |  ByteStream  QueueModel  QueueText  TrackCatalog              |
              |  TouchCalibration  TouchCheck  TouchRecognizer  ButtonGesture |
              |  ButtonPolicy  InputEvent                                     |
              |  NavModel  FrameClock  ListLayout  BitSet  TextFit            |
              |  TabBarModel  TrackProgress  JumpIndex                        |
              |  ThumbCache  ThumbScaler  JpegInfo                            |
              |  OutputModel  PlayGate  QueueView  PowerWindow                |
              |  ScreenPower (and WakeLatch)  AmpGate                         |
              |  SleepTimer  FadeStage  IdlePolicy  QueueSaver                |
              |  PowerChoices                                                 |
              +------------------------------+--------------------------------+
                                             |
              +------------------------------+--------------------------------+
  src/        |  audio/  Core2AudioBackend (decode task), RingOutput,         |  Arduino-ESP32 3.x
  (Core2)     |          BtSink (ESP32-A2DP source), SpeakerSink (M5.Speaker) |  (pioarduino),
              |  storage/LocalStorage   app/SerialConsole   app/Library        |  M5Unified/M5GFX,
              |  app/QueueStore   ui/Input   ui/CalibrationScreen             |
              |  app/PowerProbe + app/PowerLab (power measurement, console P) |
              |  app/ScreenControl (the screen policy: backlight, sleep)      |
              |  app/BoardPower (IMU suspended, EXTEN off at boot)            |
              |  app/IdlePower (the idle power-off: setting, note, power off) |
              |  app/PowerSettings (CPU speed, Bluetooth power: NVS, restart) |
              |  ui/Ui: TabBar, ListView (ui/ListScroller), Overlays, pages,  |
              |         Fonts (VLW DejaVu), Icons, Gfx, Thumbs (covers),      |
              |         EmptyState                                            |
              |  ui/BootScreen                                                |
              |  main.cpp: input events, Bluetooth events, UiHost             |  ESP8266Audio
              +---------------------------------------------------------------+
```

## Audio pipeline

```
 source:  /music on LittleFS (SD card later: same fs::FS code)  |  built-in tone: tracks
            └ AudioFileSourceFS (+ID3 for MP3)                   |    └ ToneGen
                └ AudioGeneratorMP3 (libmad) | AudioGeneratorFLAC (libFLAC)
 decode task (core 1, prio 2, 16 KB internal stack) ─► RingOutput ─► PcmRing (PSRAM, 64k frames ≈ 1.5 s)
                                                                        ├─► BtSink: ESP32-A2DP data callback (Bluedroid's BTC task, 44.1 kHz)
                                                                        └─► SpeakerSink: pump task ─► M5.Speaker.playRaw (44.1 kHz out, mono)
```

ESP8266Audio's ID3 reader walks the whole ID3v2 tag a byte per read. An
MP3 with an embedded picture (two Moon Safari tracks on the test card have
351 KB tags) then took 4 s of CPU on the decode task, above the loop: the
track started 4 s late and the UI froze as long (seen in the stage-2 device
soak). A tag over 16 KB is now skipped (the file handed to the decoder
from its first frame, `[audio] ID3 tag of N KB (a picture?): skipped`); its
title and artist aren't needed (the library has them). Both tracks now
start in 23-30 ms.

The rules that keep it deadlock- and glitch-free:

- **One ring, two consumers, one active at a time.** `PcmRing::setConsumer()`
  hands the right to read to one output; the other reads nothing. Switching
  output is a consumer handover: the decoder never touches M5.Speaker, and
  neither `M5.Speaker.end()` nor ESP32-A2DP's `end()` is ever called.
- **The Bluetooth callback never blocks or logs.** It reads the ring only
  while Bluetooth is the consumer (through its `DeclickReader`), takes what the
  ring has, pads with silence, and only *tries* the ring's mutex (taken for
  real only by `setConsumer()` and `discardAll()`). Its gain stage
  (`GainRamp`) takes new targets through atomic words.
- **Pause happens in the outputs.** They fade the next 64 frames (~1.5 ms)
  out, then play silence without reading, so pause is near-instant and the
  buffered audio waits for resume, which fades back in.
- **Nothing starts, stops or jumps with a click.** Each output reads the ring
  through a `DeclickReader`: audio fades in after any gap; when it breaks off
  (underrun, output switch) the last frame is held and ramped to 0 over 64
  frames; a skip (`discardAll()` bumps the ring's epoch) becomes a 64-frame
  crossfade from that held frame into the new track. At full level it is
  bit-exact. The speaker always queues that fade, because M5.Speaker drops
  straight to 0 when its queue runs dry. Test tones have 5 ms attack/release.
  On Bluetooth the fader runs before the gain stage; both only ever multiply
  by at most 1, so together they never add level. A new or resumed stream
  fades in from silence; the data callback detects that itself
  (`StreamRestart`, host-tested against ESP-IDF's callback sequences):
  ESP-IDF's flush, the first callback, a wait of 1 s or more, or a START of
  ours or a new link followed by a wait of 100 ms or more. A shorter stall
  without a START is only a late callback, and the audio goes on.
- **Track changes don't wait for the outputs.** A skip calls `discardAll()`;
  a natural end drains the ring first (`finished()` = end of file *and* ring
  empty), so a new track's sample rate never plays into the old track's tail.
- **The decoder never blocks inside an output.** `RingOutput::ConsumeSample`
  returns false when the ring is full or the pass's budget (1024 frames) is
  spent; the generator keeps that sample and retries it on its next `loop()`.
  The decode task re-checks requests and yields between passes.
- **A track can start part of the way in** (`play()`'s `startMs`: the
  resume point, below under Library and queue; `lib/core/TrackSeek`,
  host-tested in test_track_seek). Where it lands first: in the last 5 s,
  at the end or past it (a file that got shorter), it starts at 0:00
  (`trackseek::startMs()`; an unknown length starts where asked). An MP3
  with LAME's "Info" header (its CBR marker) whose first frames agree on
  their bitrate starts at a byte by that bitrate (its TOC's 256ths of the
  bytes put a 4 min file up to ~0.3 s off on the device); else at a byte
  from its Xing TOC (100 points), else its VBRI TOC, else its average
  bitrate (the header's length over its bytes). One without a
  header: the first frame's bitrate (a plain CBR file, exact) when its
  first frames (the 4 KB read) agree on it and the length `play()` was
  handed (`durationHintMs`: the resume point's, as the backend had it)
  agrees with the length that gives, within 3%; else the average bitrate
  by that handed length (a VBR file whose Xing frame was stripped, a
  silent 32 kbit/s start); with no length handed, a VBR file can't be
  placed and starts at 0:00. From that byte the decoder is
  handed the first frame whose next frame's header follows (a clean
  start: libmad would resync, maybe on a false sync first), without the ID3
  reader (the library has the tags; a big tag is skipped either way). No
  such frame in the 4 KB read, or one with 5 s or less of audio after it
  at its bitrate (a file shorter than its header says): the seek failed,
  and it starts at 0:00 (never a start that ends at once: the player would
  move on to the next entry). A FLAC's rate and length come from its
  STREAMINFO, past an ID3v2 tag in front of "fLaC" if a tagger put one
  there (libFLAC skips it too). A
  FLAC seeks through libFLAC (`FLAC__stream_decoder_seek_absolute()`: its
  SEEKTABLE, else a bisection on frame headers), reached through a
  subclass (`SeekableFlac`: the decoder is AudioGeneratorFLAC's protected
  member) that also sets the stream's format, which the generator would
  otherwise learn only from a frame of its own (the first frame, handed
  over from the target sample, would be read as 8-bit); a seek that fails
  opens the file again from the top. A built-in track only counts from
  there (the same sound, what is left of its length). `positionMs()` is
  the start plus what was played, so Now Playing is right from the first
  frame; the read-rate length estimate adds the start to what it
  estimates is left. The ring was emptied as for any start, so nothing
  from before the start plays, and the DeclickReader fades it in; a rate
  Bluetooth can't take is still refused. Accuracy: a CBR MP3 to the frame,
  a FLAC to the sample, a VBR MP3 with a TOC within about 1% of its length,
  one without by its average bitrate; the time shown is the time asked for.
  Measured on the device (ENERGY.md, "Device run: batch 3 follow-ups"):
  FLAC 0 ms; CBR MP3 30-50 ms behind (it lands on the next clean frame, and
  libmad drops the first one, which lacks its bit reservoir); a LAME VBR
  MP3 by its TOC -0.29 to +0.35 s of a 3:44 track (0.15%). Each start logs
  `[audio] MP3: starting 1:23 in, of 5:20 (Xing TOC): byte ...` (or `CBR,
  Info header`, `the first frame's bitrate`, ...) or `[audio] FLAC:
  starting 1:23 in (libFLAC's seek, N ms)` (75-112 ms, with or without a
  SEEKTABLE).
- **Requests are generations.** `play()`/`stop()` post a new generation to
  `TransportSync`; the decode task's progress reports for anything older are
  dropped, so a stale "ended" can't skip the track that was just requested.
  Until the decode task takes a play up (`Pending`), `positionMs()` may
  still count the track before: `positionKnown()` says so, and the
  player's prev doesn't read the position then but where that play asked
  to start (a quick second prev goes to the entry before, not to the
  start of the one just asked for; one right after a resume point's start
  at 2:30 restarts it).
- **Bluetooth is 44.1 kHz only.** ESP-IDF's SBC source takes nothing else, so
  other rates fail on Bluetooth (the player skips them) until a resampler lands.
  The speaker takes any rate: `RingOutput` keeps the rate in an `int` because
  ESP8266Audio's base class stores it in a `uint16_t`.
- **The sleep timer's fade is one more stage, after the gain** (`FadeStage`,
  in `AudioShared`, host-tested in test_output_chain; docs/ENERGY.md
  section 3): on Bluetooth after `gain_.process`, on the speaker after the
  tap write and before `playRaw`. One level for both outputs (an output
  switch during a fade carries it), an atomic Q15 target from the loop
  (never above 1.0) and an atomic Q30 level, moved only by the output that
  is the ring's consumer: down at most full scale in 1024 frames, up at
  GainRamp's slow rate (~20 dB/s). `restore()` puts it back to 1.0 at once,
  only while the outputs play silence (a confirmed pause). At 1.0 it is
  bit-exact. It is our gain only: no AVRCP absolute volume is ever sent
  for it.
- **The speaker amp is on only while the speaker is used** (`AmpGate`,
  host-tested in test_output_chain; docs/ENERGY.md item 5). The pump
  switches the NS4168 (AXP192 GPIO2) and M5.Speaker's I2S off
  (`M5.Speaker.end()`: the enable first, then the I2S) 2 s after it last
  queued audio, once every queued buffer is back: paused, stopped, the
  queue's end, or the output moved to Bluetooth. Before the next buffer
  it switches the I2S on (`begin()`; the pump owns M5.Speaker's enable
  callback, so the amp stays off while the port is set up and its pins
  reconfigured), raises the amp's enable 5 ms later on the running
  clock, lets it clock 20 ms of zeros into the amp, then the audio, which
  the DeclickReader fades in (always after such a gap). It saves ~5.3 USB mA (measured), and with the amp off
  no stray buffer can reach the speaker while Bluetooth plays. Each
  switch logs `[speaker] amp on (audio to play)` / `[speaker] amp off: I2S
  stopped, AXP192 GPIO2 low (quiet for 2 s)`.

## Tasks and cores

| Task | Core | Priority | Notes |
|---|---|---|---|
| Bluetooth controller + host (Bluedroid) | 0 | high | ~70 KB internal RAM, claimed at boot |
| A2DP data callback | 0 (Bluedroid's BTC task, BTC_TASK) | high | 128 frames at a time, several per ~30 ms tick; applies the volume ramp; never blocks or logs. ESP-IDF 5.5's A2DP source has no media task of its own: this is the task that also runs the GAP and AVRCP callbacks, which queue their events to BtAppT (below). If BtAppT's queue (20 entries) is full, each such event blocks BTC_TASK, and the audio, for up to 10 ms |
| ESP32-A2DP app task (BtAppT) | 0 | 15 | connection, stream and AVRCP handlers (`PlayerA2dp`); 6 KB stack; blocks 10 s at stack-up; must keep its queue drained (no long work in a handler) |
| decode | 1 | 2 | 16 KB stack in internal RAM (flash reads can't use a PSRAM stack); after a track start, once 500 ms are buffered, it sleeps after each pass so it refills at most 1.5x realtime (`RefillPacer`, on by default: it halved the UI's stall at every start) |
| speaker pump | 1 | 3 | three 1024-frame buffers, release-callback handshake; switches the amp and I2S (M5.Speaker end/begin) off 2 s after it last queued audio and on again before the next buffer (`AmpGate`) |
| M5.Speaker | 1 | 2 | mixes/resamples to 44.1 kHz mono; runs only while the amp is on |
| cover thumbnails (`thumbs`, ui/Thumbs) | 1 | 1, or 0 while a list moves | only while there are covers to make: made for the first, gone after 3 s without one; 6 KB internal stack while it lives (2.3 KB used at most on the device); reads the card in 4 KB pieces; level with the loop while nothing moves (at 0 it shared what was left with the idle task: 2-2.5x slower), below it the moment a list moves, always below the decoder (below) |
| Arduino loop (UI, console, input) | 1 | 1 | the input layer every pass (touch panel over I2C, the buttons); the UI (the one task that draws): what changed, and list frames at up to 30 fps on deadlines, each piece under its own short bus hold; on the Dance tab, the beat tracker and the dancer's frames (10/s idle; dancing 30/s at 240 MHz, 24/s below: `DanceRate`); sleeps 1-5 ms every pass (less while a list frame is due), 20 ms while the screen is off (`Ui::idleMs`; with no UI, main's own 20 ms) |

## Bluetooth

`BtSink` connects to the headphones the listener paired on the Pair screen
(Output > Pair new headphones: the audio devices nearby, with their kind
and signal, and only the one tapped is connected to). It never picks a
device by itself (`SinkSearch`, lib/core, host-tested): with none
remembered, a release build doesn't scan at all, at the boot or on any
ask (a play waiting, Connect, a B hold, console `o` or `Pr1`, the Pair
screen closing), and stays quiet, connectable only; a device a scan finds
is taken only by the name a developer build was given (`BT_SINK_NAME`,
changed with the console's `c<name>`), never by signal strength alone
(that once picked a TV in the next room), but for one scan after the
console's `Bs`. The library's stack-up, which scans whenever nothing is
remembered, is kept from it then: while BtAppT is in that handler
`has_last_connection()` answers yes, and the page of the zero address it
then asks for isn't made. It remembers the
device it connected to, and all along it stays connectable (never
discoverable) so the headphones can reconnect by themselves; other devices
are refused. How it looks for them while no link is up is
`ReconnectPlanner`'s (host-tested with a model of the glue and the library;
[ENERGY.md](ENERGY.md) item 1: the old cycle, pages then a minute of inquiry
forever, cost +35.5 mA for as long as they were away):

- **Burst**: on a drop, at boot and on a listener's ask (a play waiting for
  them, Connect or a tap on the card, a B hold, console `o`), 3 pages: one
  at once, the next once the last has had its answer (refused, or the 5.12 s
  page timeout) and 10 s after it began. A page already on its way is the
  burst's first try, never paged over.
- **Back-off**: then one page at 30 s, 1, 2 and 5 min, then every 5 min.
  Never an inquiry while headphones are remembered (it can't find them
  unless they are in pairing mode); a scan found running is stopped.
- **Resting**: after 15 min without an answer, or at once after the burst
  while nobody is around (the screen off and nothing playing or waiting,
  and not lost: a drop while listening pauses the player itself, and the
  listener may still wear them, so that gets the whole back-off;
  `BtSink::setQuiet()`, fed with the screen policy's Off), no pages and
  no scans: connectable only, so headphones
  that are switched on or taken out of their case come back by themselves.
  A listener's ask starts a burst again.
- **Scan by name**: only with none remembered and a name to look for
  (`sinksearch::mayScan()`: a developer build's `BT_SINK_NAME`, not after
  Forget; or `Bs`), for 2 min after the boot or an ask, then resting. A
  release build never gets here: with nothing remembered, `start()` goes
  straight to Resting. A device it finds and fails to connect to is
  remembered by then: a burst pages it.

The library's own auto-reconnect is kept disarmed outside a pairing (its
"retries exhausted: start discovery" branch must never run): every
background page is the planner's, carried out on BtAppT from its
heartbeat and the loop's 250 ms ticks. A failed connection (also a first
pairing) never ends in a dead end: a burst, then the back-off, and the
listener's Connect at any time. On connect the output switches to
Bluetooth; a real disconnect pauses playback.

`BtSink` wraps `PlayerA2dp`, a subclass of ESP32-A2DP's `BluetoothA2DPSource`
(in `BtSink.cpp`) that makes it behave like a phone. All its state lives on
the library's app task (BtAppT); the loop task only posts work to it (also
forgetting the remembered headphones). The decisions are made in `BtControl`
(lib/core), which ties the media stream and the volume together and is
host-tested with both; `PlayerA2dp` feeds it the stack's events and carries
out what it returns.

- **Volume** (`AbsVolumePolicy`, host-tested). The library's volume path is
  off (it scaled the PCM with truncation and echoed every headphone volume
  change back). Headphones that report volume changes get the Bluetooth volume
  as AVRCP absolute volume; once they accept it (or change it themselves) the
  Core2's gain stays at a fixed -2 dB of headroom, and their buttons change
  the volume shown without it being sent back. Others get a software volume
  (dB-linear over 40 dB). Every link starts in software mode at the safe level
  and at most 60 %. The gain stage (`GainRamp`) falls in ~23 ms, rises at
  most ~20 dB/s, and fades in over ~46 ms when a stream (re)starts. The
  speaker and Bluetooth keep separate volumes.
  Hearing safety: what the listener hears is the headphones' own level (unknown
  until they report it) times our gain, and SET_ABSOLUTE_VOLUME changes their
  level at once. So a command the listener didn't ask for only reaches them
  while nothing from us is heard. Before anything has played on a link, the
  first SET_ABSOLUTE_VOLUME (the probe) goes out while no media flows, a new
  stream waits until it is answered (or ~1.5 s for AVRCP to show up, up to
  3 s once it is connected; 2 s for the answer, then 1 s more in case they
  apply it late), and the first stream fades in from silence straight to the
  handed-over level. Headphones whose AVRCP comes up after playback started
  (Powerbeats Pro: ~7 s after the link), or comes back after dropping on a
  link that has played, get a late probe: the volume is capped at 60 % as on
  a new link, our gain fades to silence (~23 ms), and the command goes out
  800 ms later, once that silence has passed ESP-IDF's frame queue and the
  headphones' own buffer (or once nothing has streamed for 800 ms: at once
  after a longer pause). The stream keeps running, silent; an answer within
  1 s hands over and our gain rises from silence at the gain stage's slow
  rate (~2.4 s to the headroom), no answer brings back the software level
  the same way. The listener's volume presses during a probe are sent and
  restart its timeout, but a probe ends at the latest twice its timeout
  after its first command. Only an answer counts: an ACCEPT for more than
  the command it answers asked for (a stale one, or a level of their own),
  or an echo louder than what we now ask for, gets ours again instead, if
  that brings them down, and not again in reply to the answer to that
  repeat: headphones that can't set our level (they round up, or have a
  floor) would otherwise trade commands and answers with us for the rest
  of the link. A notification of theirs at or below ours hands over at
  their level, which is sent back once so that a command of ours still on
  its way can't land after it and raise them (the same when their report
  ends a probe after a stream they started themselves has played), and a
  new stream waits until that is answered (its ACCEPT or echo, at most 2 s):
  what was on its way lands before the stream, not ~1 s into it. During a
  late probe's dip the silence waits for it too. A notification names no
  command; only an ACCEPT shows that one of ours has landed. So a
  notification near it only counts as its echo once every older command of
  ours has an ACCEPT (paired by order), and none left our short history of
  commands without one; one near an older command with no ACCEPT answers
  nothing either. Before that, a key of theirs near either (headphones with
  fine steps) looks the same: it answers nothing and uses nothing up, so
  their ACCEPTs still pair with our commands in order, and the volume shown
  doesn't follow it either (echo or key, the command that waits lands after
  it, and they end at that). Headphones that only notify wait the 2 s out.
  After an
  unanswered probe their level is unknown, and any command could be the
  first they apply, at once, from their own level (even a volume step down
  could raise them), so the listener's volume is no longer sent at all: our
  gain alone follows it. A later ACCEPT or echo of a volume we sent (the
  probe, or ours again after an answer above what a command asked for)
  then also hands over: before anything has played with a lift, and the
  listener's volume sent if it changed (a new stream waiting for its
  answer, as above); after, they end at the last value sent (the volume
  shown comes down to it if the listener rose since, and a step down since
  is sent now), and our gain rises slowly from the software level to where
  they are, but only once that step down is answered: until it lands they
  are at what they confirmed, louder than the listener chose. Headphones
  that confirm late are slow, so both waits last as long as they took to
  confirm (2 to 6 s). Their own change first means the listener sets their
  level there: if a louder command of ours may still be on its way, their
  level goes back once (the volume shown instead, if lower), and the link
  stays in software mode.
  Apart from the listener's own volume
  step and the fade back after a stream restarts, our gain only rises at
  that slow rate once audio has played. (That includes the narrow race of a
  stream the headphones start themselves during the probe: our data
  callback runs before STARTED reaches BtAppT, so the handover still looks
  pre-audio, and the gain stage, finding the stream's restart used up,
  ramps to the handover level instead of lifting.) Residual risk, from
  latency: a probe the headphones apply after its timeout and grace, once
  audio plays in software mode, raises their level to what we sent for
  volume p while it plays: heard as their level for p times our gain at
  that moment (the software gain for the volume shown, which the listener
  may have raised since), never above what absolute volume gives for p.
  The same bound holds for a command still on its way when a stream, or a
  rise of our gain, held for an answer goes ahead after its wait without
  one (headphones slower than that), or after an ACCEPT paired with it by
  order that was an older command's (more than one of ours left the
  history unanswered, so the pairing runs late).
  And in absolute mode, a report of theirs that overtakes a command of ours
  still on its way can leave the volume shown above their level until they
  report again; a step down from it then sends the level shown. A notification
  counts as an echo of ours only if it matches a command they haven't
  confirmed yet (within 4/127, or what their ACCEPT said), so their own
  steps reach the UI, except one near a command of ours on its way while a
  new stream waits for an answer (if the command that waits is then dropped,
  the volume shown stays at it until they report again). Volume steps travel as steps to BtAppT, so
  quick presses are never lost. The headroom (-2 dB) can be changed from the
  console (`h<n>`, not saved) to find where loud masters start to distort.
- **Media stream** (`StreamControl`, host-tested against the event orderings
  ESP-IDF produces). Started (CHECK_SRC_RDY, then START) as soon as Bluetooth
  is the output and the player plays, retried after 1, 3, then every 10 s;
  suspended 3 s after playback stops, or at once after a pause from the
  headphones' key: ESP-IDF's AVRCP target can't report the play status, so
  they pick PLAY or PAUSE for their button from the stream. One media command
  at a time, and one unanswered for 3 s is given up on, as is a START that was
  acknowledged (ESP-IDF's stop_tx acks any pending command with SUCCESS) but
  never reported STARTED. The data callback fades in after a START or a new
  link; a late callback alone (congestion, a flash write) doesn't restart
  anything. A stream the headphones suspend themselves pauses the
  player and isn't restarted until it plays again; one they start themselves
  (ESP-IDF suspends it at once) is not ours and doesn't pause anything. The
  library's own media handling (heartbeat starts, its connecting-state
  guesses) is bypassed.
- **Buttons.** The AVRCP target is on: play, pause/stop, next, previous and
  volume keys become `BtSink::Event`s, queued to the loop. What the transport
  keys do is `HeadsetKeys` (host-tested): headphone input never starts music
  that wasn't playing, since in-ear detection and a bud being adjusted send
  keys too. Play and pause are commands, never toggles. Play only resumes
  paused playback (stopped, e.g. after a boot and an automatic reconnect, it
  does nothing), and never a pause the sleep timer made
  (`PlaybackController::pausedByTimer()`: in-ear detection sends Play
  when a sleeper turns over; the Core2's own play resumes it). Only a key
  that acted counts as input for the idle power-off (`HeadsetKeys::
  isInput()`: not an ignored Play or Pause, nor a cue after the timer's
  pause). Next and previous skip while playing; paused or stopped
  they only select the next or previous track, which the screen shows and a
  later play starts from its beginning. Previous past a track's first 3 s
  restarts it, as every prev does (see Transport): playing, from 0:00;
  paused, at 0:00 and still paused. The Core2's own buttons and the
  console keep toggling and skip-and-play.
- **On-device checks** the host tests can't cover (the glue in `BtSink.cpp`:
  event and address filters, what the library does between our hooks): boot
  with the headphones in their case, then take them out (they reconnect by
  themselves); switch them off for 2 minutes and back on (they page back by
  themselves; if not, the back-off's next page finds them: at most 30 s, 1,
  2 or 5 min after the last, and not at all once resting: then Play,
  Connect or a B hold); fresh NVS with pairing mode left mid-connect (the
  device found is remembered by then: a burst pages it, no reboot needed); `f` then reboot (a build with
  `BT_SINK_NAME` scans by name; a release build logs "no headphones paired ... not scanning" and the radio
  meter stays at 0); `Bf` (the next boot as a fresh unit) and `Bn` / `Bn0` (this session): the card reads
  "Bluetooth headphones / No headphones paired", a B hold and a play show "No headphones paired: Output > Pair new headphones"
  and stay on the speaker, nothing scans, and the following boot pages the stored headphones as before; while
  playing, caps arriving late must log `came up after playback started:
  dipping`, the music must go silent for about a second and then fade
  back in over ~2 s with `control=headphones` in the stats (or `no answer`
  and the software level), never jumping up; their buttons must then change
  the volume shown; pause from the headphones then play from them within
  3 s; while paused, next on the headphones shows the next track and stays
  paused, and their play then starts it.
- **The Output screen** (`OutputModel`, host-tested for its decisions;
  `BtSink` carries them out on BtAppT, the loop only posts asks, retried
  in order when BtAppT's queue is full). `BtSink::link()` publishes what
  the link is doing: Off, Paging (a burst: the try and of how many;
  `connect_to()` is overridden to count every page, the planner's and a
  pairing's library retries), Scanning (by name, none remembered),
  Backoff, Resting, Linked, PairScan, Pairing, and whether a device is
  remembered (Off too with none remembered and nothing to scan for: the
  card's "No headphones paired" [Pair new headphones]; a tap on the card
  opens the Pair screen too). The listener's asks: **connect()** pages the remembered
  headphones at once (a full burst, then the back-off; with none
  remembered, a scan by name for 2 min if there is a name to look for,
  else nothing: main.cpp doesn't even ask then, `nothingToFind()`: the B
  hold is refused with the toast "No headphones paired: Output > Pair new
  headphones" (a toast rather than the Pair screen: B works on every tab
  and from a pocket, and the Pair screen's scan would start from a press
  that asked for none; the card's Connect or Try again, refused the same
  way, shows it too), and a play on Bluetooth doesn't wait
  (`PlayGate`'s NotPaired: paused at once, the same toast, the output
  left as it is));
  **disconnect()** lets go (or stops a page) and stops trying: no paging,
  no scanning, not connectable, and the headphones coming back by
  themselves are refused, until the next connect(); **startPairScan()**
  runs inquiry rounds back to back and lists every audio device (class of
  device: the rendering service or the Audio/Video major class; the name
  from the result or its EIR) into a `BtScanList` in PSRAM, under a
  spinlock, connecting to none (the library would connect to the first
  that matches), while the background search is held off (the Pair page
  stops it after 2 min, `PairSearch`, checked every UI pass so a dialog
  over the page doesn't keep it running, or when the screen goes off, and
  offers "Search again": **pausePairScan()**, which keeps the background
  search held off while the page is up, so the old headphones aren't
  paged while new ones are picked; closing the page without a pairing,
  **stopPairScan()**, starts a burst); **pairWith()**
  lets go of the link that is up first, then pages the picked device with
  the usual tries: it becomes the remembered one (NVS) only once linked,
  and the sink name becomes its name (the name shown until theirs is
  read; in a build with `BT_SINK_NAME`, a later scan by name finds it);
  a pairing that fails puts the old address back in RAM (NVS still has
  it) and stops there; a Cancel while the old link is still being let go
  ends the pairing the same way. The Pair list leaves out the headphones
  linked now (multipoint sets stay discoverable; "pairing" with them only
  let them go), and picking them anyway just makes them the output.
  Forget from the screen forgets and disconnects, no restart, and **for
  good**: a saved flag (NVS `bt_forgot`) stops every scan by name in a
  build with a name (the boot's, the search's with none remembered, a B
  hold's), so the forgotten headphones can't come back by their name; the
  next pairing clears it (the console's `f` still restarts and, in such a
  build, scans by name). A release build never scans by name anyway. `BtSession` (main.cpp's)
  holds what the listener asked: **the audio stays on its output until the
  headphones they asked for are linked** (the card, Connect, the B hold
  only connect; the Connected event moves the audio, as it always did,
  and when it was asked for, a toast "Now playing on SPYDRONE" and two
  ticks say so), a connection whose tries run out (the link goes on to
  Backoff, Resting, Scanning or Off) is Failed, and so is a connect with none remembered (a
  scan by name) after 30 s; a link let go on purpose (Disconnect, Forget,
  a new pairing) drops without the "lost" dialog (expected for 10 s at
  most while the link stays up). A pairing started while other headphones
  are linked only counts a new link (the old one is still up for a
  moment), and the Connected event and the link's phase, seen in either
  order, give the same answer (`BtSession::onConnected()`), so the new
  name is always kept. Every move off the headphones pauses first
  (Disconnect, Forget, the speaker card, pairing others); a cancelled
  connection leaves music already on the speaker playing, whether the
  Speaker row or a B hold cancels it. The
  card's codec line comes from the SBC configuration ("SBC 44.1 kHz") and
  the delay report. A pairing scan while headphones stream costs airtime:
  the Pair screen is the only thing that runs one.
- **Developer tests that leave the pairing alone** (the console's `B`):
  `Bs` arms auto-pair by signal for the next scan (RAM only, off at boot,
  logged; used up by the device it takes, a link, the scan's end, or
  anything that ends or replaces that scan: a Disconnect, the Pair
  screen, a pairing, the search resting) and starts that scan as a
  connect would (refused while
  headphones are remembered: they are only paged). The **fresh-unit test**
  makes `BtSink` behave as if nothing were remembered and the build had
  no `BT_SINK_NAME`, without reading, erasing or changing the stored
  address or the stack's bond: `PlayerA2dp` overrides the library's
  `read_address()` / `write_address()`, so the remembered address lives
  in RAM meanwhile (the library's `start()` finds none; a pairing made
  during the test is remembered in RAM only), and the sink name and
  Forget aren't saved. `Bf` saves a flag (NVS `bt_fresh`) and restarts:
  the next `begin()` uses it and clears it, so only that boot is the
  test. `Bn` is the same for the running session (RAM only; refused while
  linked, pairing or on the Pair screen), `Bn0` ends it and reads the
  stored address, name and Forget again. With nothing to find, the card's
  title is "Bluetooth headphones", not a name left in RAM or NVS
  (`BtSink::shownName()`), as on a fresh unit.
  **Checked on the device** (2026-09-30, a build with `BT_SINK_NAME`, the
  stored headphones asleep): a `Bf` boot logged "no headphones paired, and
  nothing to scan for: not scanning, connectable only", then no device
  found and no `[stats] bt` line (search resting, radio 0 %) for 6 min; the
  card read "Bluetooth headphones / No headphones paired" [Pair new
  headphones]; a B hold showed the toast and stayed on the speaker, as did
  console `o` (logged); a silent play played on the speaker, nothing
  scanned. Pair new headphones from the card scanned (30 s, until the
  screen went off and stopped it), listed nothing, and after it closed the search
  rested and the radio went back to 0. The next boot paged the stored
  headphones (the same address) with no inquiry; `Bn` behaved as `Bf`
  (the same card and toast), `Bn0` remembered them again; `Bs` with them
  remembered is refused. Not run: `Bs` scanning (it would take any device
  at -55 dBm or closer).
- **Play while the headphones aren't connected** (`PlayGate`, host-tested
  with the player, `BtSession` and `ButtonPolicy` in `test_play_gate`). The
  bug it fixes: the headphones dropped overnight while idle, the output
  stayed Bluetooth with the old background cycle scanning, and play went to
  "Playing" at 0:00, "connecting...", for good (the decoder filled the
  ring, the stream stayed suspended; no explanation, timeout or way out).
  Now play from anywhere (the button, B, the Library's Play, the Queue's
  Play now, a resume, a skip) while Bluetooth is the output and the link
  is down makes the player **wait** (`PlayState::Waiting`: nothing starts,
  the position holds) and `PlayGate` (fed every loop pass after
  `BtSession`) **connects at once**: `BtSession::connect()` and
  `BtSink::connect()`, the paging burst ("try 1 of 3"), not the back-off's
  next page, and from resting too. A page still on its way (within the
  page timeout, `ReconnectPlanner::kPageMs`, and not answered yet) isn't
  paged over: it counts as try 1 of a full burst, so a wait that begins at
  a burst's last try (or a back-off page) doesn't fail with it. While the Pair screen has the
  radio (its scan, or a pairing) only the session is asked (a pairing's
  own session is kept): the scan isn't stopped for a B click. The "lost"
  mark goes (the tab bar and the card say Connecting). The link comes up:
  the wait is released and plays on them, "Now playing on SPYDRONE"
  (said once: the Connected event's own toast is skipped while the gate
  waits). The tries run out (`BtSession::failed()`) or 20 s pass
  (`kBackstopMs`, the listener's number: the burst itself takes ~25 s, a
  try at once and then one 10 s after the last began, so this usually ends
  the wait during the third try): the wait ends **paused**, and a notice says
  "Couldn't reach SPYDRONE. Are they on, out of the case, and not
  connected to your phone?" with Try again (a new wait and burst) and
  Play on speaker. The session's ask is **withdrawn** as failed
  (`BtSession::withdraw(true)`: no disconnect), so the card ("Couldn't
  connect") and the tab bar turn red with the notice while the tries left
  and the back-off go on quietly; a link they bring later closes
  the notice and is no answer to anything: nothing plays, no "Now playing
  on". A tap on the play button (a spinner while waiting) or a B click
  cancels the wait (paused); a B hold, Disconnect or Cancel on the card
  end it paused too (every move to the speaker pauses first;
  `pauseIfPlaying()` counts a wait as playing). A wait that ends any way
  but a link withdraws its ask the same way (`withdraw(false)`: the radio
  left as it was), so a link after a cancel says nothing either. **Play on speaker** is the one way out that plays: the
  listener's explicit choice, the wait ended paused first, the speaker
  made the output as the Speaker row makes it (a connection on its way
  cancelled), then play, at the speaker's own volume
  (`PlayGate::playOnSpeaker()`). The output moving away by itself (silent
  test mode) ends a wait paused. With no headphones paired and nothing
  that scans for any (`BtSink::nothingToFind()`: a release build, the
  fresh-unit test) there is nothing to wait for: the wait ends paused at
  once (`Do::NotPaired`), nothing is paged or scanned, the output stays,
  and the toast says "No headphones paired: Output > Pair new
  headphones" (not while the Pair screen scans or pairs: then the play
  follows that). Headphones dropping while idle stay
  quiet (no dialog), but the Output tab shows them lost and Now Playing's
  output line says "SPYDRONE (not connected)". The name shown is the
  headphones' own (read at each link), or before any link the name
  looked for.
- **Diagnostics.** Once per connection: the SBC configuration, the delay
  report, the headphones' AVRCP features and notifications, and how long a
  stream took to start. The `s` stats add a `[stats] bt` line: volume and who
  applies it, gain, headroom, stream state, longest gap between data
  callbacks, dropped events, BtAppT stack left, the search (`search=burst`,
  `backoff`, `resting`, `scan`, `idle`) and `radio=N%/min`, the share of the
  last minute spent paging or scanning (`RadioMeter`, sampled on BtAppT's
  ticks: ~85-100% with the old cycle, ~0 resting). Unlinked and not the
  output, the line shows only while the radio looks or did in the last
  minute. Each page, scan and phase change logs a `[bt] reconnect:` line
  (`paging ..., try 2 of 3`, `backing off`, `resting (why)`).

## Sleep timer

The listener's "fell asleep with music on" (docs/ENERGY.md section 3;
`SleepTimer`, `FadeStage`, host-tested in test_sleep_timer). Choices, RAM
only: 15, 30, 45, 60 or 90 min, End of track, End of album (the next
entry on another album; with no album, another folder), End of queue.
Now Playing's "..." > Sleep timer opens its sheet (`SleepSheet`: the
minutes, the three ends, and while one runs +10 min and Turn off in red;
the running choice outlined). main.cpp's `stepSleep()` feeds the timer
every pass before the player and carries out what it says:

1. the fade, our gain only (the stage above): a timed choice over the 30 s
   after it expires, linear in dB to -40 dB then 0; the ends over the
   track's last 10 s when its length is known. It only falls by itself: a
   skip keeps it, and only +10 min, Turn off or another choice bring it
   back, at the slow rate. After a skip the length counts only once the
   backend has started the new track (`EntryStart`: until then it reports
   the old one's end, which would fade the new track at once);
2. the pause (`pauseByTimer()`, or the player's own at the boundary:
   `setPauseAfterTrack()`), never a stop, the output never moved; a pause
   during the fade, or an expiry while paused or waiting for the
   headphones, goes straight here;
3. once nothing has played for 250 ms: the fade back to 1.0 and the screen
   off (`ScreenControl::sleepTimerOff()`; the pocket guard for any wake);
4. 5 min later, unless something played: `releaseHeadphones()`, the drop
   expected (no dialog), the reconnect resting, still connectable, the
   output still Bluetooth (a later play pages them through PlayGate). A
   play in the moment between the release and the drop (the link still
   up) is paused when the drop comes (`BtSession::onDisconnected()`), so
   nothing "plays" without a link and the headphones never start music
   when they come back.

+10 min never leaves less time than before (`SleepTimer::canExtend()`):
End of album or End of queue before its last track can't say what is
left, so +10 min is dim and does nothing there.

During the fade any touch, strip press or PWR wake (swallowed as usual)
shows the toast "Sleep timer: fading" with +10 min and Turn off, the only
controls that act on it; they come before an open sheet's (the toast is
drawn over it) and take a tap to y 77. A screen lit when the fade starts
stays lit while it counts down to the pause (the screen's `holdLit`,
below; `SleepTimer::fadeCountingDown()`: a timed fade's 30 s, a track's
fade in the boundary track's last 10 s); the pause's screen off still
turns it off. A track's fade held after a skip (until the new track's
last 10 s, or the album's end: minutes) keeps its toast but doesn't hold
the screen, which times out as ever. Both raise the level, so neither
acts on a clamped edge reading or on the touch that attended a screen
woken from off (`SleepTimer::toastTap()`, `ScreenPower::
landedUnattended()`): a pocket's second contact lands on a lit toast;
the listener's next tap acts. After the timer's pause the headphones' Play is
ignored (HeadsetKeys); the Core2's play resumes. Console: `T` status,
`T<min>`, `Ts<sec>` (tests), `Tt`, `Ta`, `Tq`, `T+`, `T0`; each step logs a
`[sleep]` line. The idle power-off (below) follows the timer's pause.

## Idle power-off

The device turns itself off after N minutes idle (docs/ENERGY.md item 4;
`IdlePolicy`, host-tested in test_idle_policy): "Turn off when idle" on the
Output tab, 10 / **20** / 60 min / Never, saved (NVS "power"/"idle_after",
`app/IdlePower`). Idle means all of these: the player Stopped or Paused
(not Playing, not Waiting for the headphones); not on USB (the AXP192's
power status, ACIN or VBUS, read once a second by `ScreenControl`; not read
yet counts as USB); no Pair screen scan or pairing; no queue write under way
or edit waiting (`QueueStore::busy()`; not a write that failed and waits its
retry); no screen of its own (calibration, a spike tool). Anything that
blocks restarts the countdown when it goes, and so does any input: a touch
or strip press (a waking one too), PWR, a headphone key that acted (not a
Play ignored after the sleep timer's pause, a Pause while paused, a cue
after the timer's pause: `HeadsetKeys::isInput()`), a console byte. So it
counts from the pause, the sleep timer's too.

main.cpp's `stepIdle()` feeds it each pass after the player and carries it
out:

1. **The warning**, the last 30 s: the toast "Turning off in 30 s" (counting
   down) with **Keep on** (`Toast::showIdle`). Any input ends it, the
   button included; so does anything that blocks. It is drawn on a lit
   screen only: it doesn't light a dark one (it may be night). A screen
   lit when it appears stays lit, brightened, until it ends (the screen's
   `holdLit`, below), so it can't go dark partway through the countdown
   (checked on the device from bright and from dim; an off screen stays
   off: ENERGY.md, "Device run: batch 3 follow-ups").
   In practice it is seen mostly with Screen off after: Never: the
   shortest idle length (10 min) outlasts the longest screen timeout (5
   min), and what keeps the screen lit also blocks the countdown.
2. **At the end:** `QueueStore::flushNow()` (a piece-wise write under way
   finished, or the queue written again whole if it changed since it
   began; an edit inside its 2 s written at once; the position), the note
   for the next boot (NVS "power"/"off_idle": the length), and the
   headphones let go the sleep timer's way (`releaseHeadphones()`: the
   drop expected, resting, still connectable, the output unchanged).
3. **The power**, once they are unlinked (at most 3 s, so they see a clean
   disconnect; unlinked is `BtSink::linkUp()` false, the loop's link up
   from CONNECTED until DISCONNECTED: the library's `connected()` goes
   false as soon as the disconnect starts, the stack's DISCONNECTING,
   0.15-1.5 s before the link is gone): the power status read once more (USB plugged in meanwhile:
   it stays on), haptics stopped, `Serial.flush()`, `M5.Power.powerOff()`
   (the AXP192's power-off bit; M5Unified then deep-sleeps with no wake
   source in case it didn't take). Input or a blocker during step 3's wait
   cancels it: the note is cleared, the headphones stay let go (a play
   pages them).

PWR boots it again, stopped where it was, at the second it paused at (the
resume point) if it was paused (the queue and position from the
card and NVS). The next boot reads and removes the note and, once the UI is
up, shows "Turned off after 20 minutes idle" for 6 s. Console: `I` status
(`[power] idle: off after 20 min; counting, off in 1142 s`, or what it waits
for), `I<min>` / `Is<sec>` a test length until restart, `I0` the setting's
again; for bench tests on USB, `Iu1` / `Iu0` tell the policy "on battery"
(until restart; the last-moment read of the AXP192 is the real one, so on
USB it logs `USB power at the last moment: staying on` instead of turning
off) and `Ib<sec>` leaves the note for the next boot's toast. Its log lines are `[power] idle: ...`, `[power] off after idle
(...)`, `[power] off now (...)`.

## CPU speed and Bluetooth power

Two settings on the Output tab after "Turn off when idle" (docs/ENERGY.md
items 6 and 7; the choices, lines and checks are `PowerChoices`,
host-tested in test_power_choices; `app/PowerSettings` keeps them in NVS
"power" and applies them):

- **CPU speed**: **240** / 160 MHz (`powerchoice::kDefaultCpuMhz`: 240, as
  ENERGY.md step 6a decided; 160 halves list scrolling with an MP3), NVS
  "cpu_mhz" (160 or 240; absent or anything else: the default), the same
  value as the console's `Pcb`. `PowerSettings::applyBootClock()` sets it
  first thing in setup(), before Bluetooth: 240 <-> 160 retunes the PLL the
  radio runs from, so it can't change at runtime. The row's line says what
  each costs: 240 "Smoothest lists, dancing", 160 "Slower lists, saves a
  little" (UiText). A tap opens a dialog ("Restart at 160 MHz?", [Cancel]
  [Restart]; its body from `powerchoice::cpuDialogBody()`: to 160 "Saves
  a little battery; lists scroll at half speed while music plays. Music
  pauses and picks up at the same second.", to 240 "The speed changes at
  a restart. The music pauses and picks up at the same second."); Restart
  saves it, pauses,
  flushes the queue and its place (`QueueStore::flushNow()`), leaves a note
  (NVS "boot_cpu"), lets go of the headphones the idle power-off's way and
  asks the speaker's pump to switch the amp off (`requestAmp(Off)`, as
  `Pa0`: the AXP192 isn't reset by `esp_restart()`, so a live amp would pop
  as its clock pins are reconfigured); `stepCpuRestart()` then calls
  `esp_restart()` (under the LCD lock) once the headphones are unlinked
  (`BtSink::linkUp()` false: the disconnect done, as the idle power-off
  waits) and the amp is off, at most 3 s later
  (`powerchoice::cpuRestartDue(now, asked, ...)`: a restart asked during
  the loop pass is stamped after the pass's `now`, which counts as 0 ms
  waited, not a wrap). The pause before the flush saves the resume point,
  so after the boot the entry waits, stopped, at the same second (as both
  dialogs say). The
  toast "Restarting at 160 MHz..." stays up until then. The next boot
  shows "CPU speed: 160 MHz" for 6 s, stopped where it was: nothing plays
  by itself. While a pairing is under way the restart isn't offered ("Wait
  for the pairing to finish"; `CpuTap::WaitPairing`, and
  `MainUiHost::setCpuSpeed()` refuses it too). After a `Pcb` that differs
  from the clock the row reads "240 MHz until a restart", and a tap saves
  the running speed back without a restart.
- **Bluetooth power**: Low / **Normal** / High, the BR/EDR TX power
  levels 0..2 / 0..5 / 0..7 (-12..-6 / -12..+3 / -12..+9 dBm), NVS "bt_tx".
  A tap takes the next, saved and applied at once (`BtSink::setTxPower()`);
  at every stack start `PlayerA2dp::bt_start()` sets it right after the
  controller is enabled, before Bluedroid, so before any page, scan or page
  scan. A link that is up keeps its level: after a change while linked the
  row reads "From the next connection" until the link goes
  (`BtLinkLevel`). No reconnect is forced.

About's "CPU speed, Bluetooth power" row shows both as they run ("240 MHz;
Normal (-12..+3 dBm)"). Boot log: `[power] CPU 240 MHz from boot (the
default)`, `[power] bluetooth power: Normal (-12..+3 dBm)`, `[bt] tx power:
-12..+3 dBm (levels 0..5), from the stack's start`.

## Dancing crab (proof of concept)

Each output copies what it plays into its own `AudioTap` (PSRAM, written only
by that output's task: the Bluetooth data callback before its gain stage,
the speaker pump after its read). The taps are on only while the Dance tab
is up and tracking (`DanceMode` switches them; off from boot, off while the
screen is off): a tap switched back on starts a new segment, so the tracker
never splices audio across the time it didn't see. The loop task feeds that to a
`BeatTracker` and draws the dancer for the frame being heard (the tap's
clock minus the output's latency). The dancer is a skin (`DanceSkin`): a
pixel-art crab by default (`CrabPose` maps the beat phase to layer frames
and offsets; its art, `CrabArt`, is generated by `tools/crab_art.py` from
`tools/art/crab.json` into flash, and `DanceView` blits it at 3x into an
RGB565 sprite in PSRAM), or the first stick figure (`DancePose`, an 8-bit
sprite). Console `m` or a tap on the dancer swaps them. Design, commands and
results: [MASCOT-POC.md](MASCOT-POC.md).

**Frame rate** (`DanceRate` in lib/core, host-tested: test_dance_rate;
[ENERGY.md](ENERGY.md) item 8). The rate follows what the last frame
showed: **10 fps** while the dancer idles (no beat heard, as when paused,
stopped or starved, or no lock, with a dance weight of 0.5 or less);
dancing, **30 fps** at 240 MHz and a steady **24** below it (the clock
that runs, `getCpuFrequencyMhz()`, so the console's `Pc` counts too). A
fade in or out runs at the dancing rate until the weight passes 0.5, and a
locked beat that comes back is danced to from the next idle frame. Frames
are kept on deadlines (`dancerate::Pacer`), and a new rate starts over from
the last frame drawn, so there is no catch-up burst. With the screen off
the UI turns `DanceMode` off: no frames at all. Each change is logged
(`[dance] 10 fps (idle)`, `[dance] 24 fps (dancing at 160 MHz)`), and the
5 s `[dance]` line reads `fps=23.8/24 (dancing)`: measured / target.

## Storage

`LocalStorage` mounts the SD card (shared SPI bus with the LCD, 25 MHz) if one
is present, otherwise the ~11.9 MB LittleFS partition. It walks `/music` for
the library (up to 8 folders deep, no cap, hidden names skipped) through the
VFS's `readdir`, which names each entry and says whether it's a folder, rather
than Arduino's `File::openNextFile()`, which opens every entry and so searches
its directory again for each file (the UI spike measured that walk at ~5.7 ms a
file). The player's own files live in `/.player` on the same volume:
`library.idx` (the index's cache), `queue.txt`, and `thumbs/` (the album
covers' thumbnails, below).

## Library and queue

Everything the player knows about the music is one `LibraryIndex` in PSRAM,
the single store: a string arena, fixed-size records and sorted views
(~70-80 B a track, nothing in internal RAM). The queue, the player and later
the browsing UI hold its **track ids**, never strings.

- **Track ids** (`TrackCatalog`): `0 .. n-1` are the index's tracks, and
  `0x80000000 + k` the built-in tracks (the test tones and the click tracks,
  paths `tone:...`), which are there with or without a card. The catalog turns
  an id into a path to play or a title and artist to show, into the caller's
  buffer, when needed. An id it doesn't know (a library rebuilt under it) gives
  the path "", which the backend fails and the player skips.
- **Boot** (`app/Library`): `/music` is walked for its paths only, hashed into a
  signature (FNV-1a 64). If `/.player/library.idx` was saved for that
  signature, it's loaded as it is: blocks of exactly the saved size, no sorting,
  no build peak (`LibraryIndex::load()`, checked by a checksum; a stale, cut or
  damaged file is refused). Otherwise the walk is done again into a new index,
  which is saved (written aside, then renamed over the old one). Any file
  added, removed or renamed under `/music` rebuilds it. Console `g0` rebuilds
  it on request.
- **Folders and covers**: every file under `/music` that isn't audio is
  counted in its folder (the Folders view lists only folders and audio
  files, and says "14 audio files, 1 other"), and a folder's cover image is
  picked from them by name: `cover.jpg`, then `folder.jpg`, then
  `front.jpg`, then any other `.jpg`/`.jpeg` (`imageRank()`; the largest of
  several such is picked when the cover is made). An album's cover is its
  folder's, else its first track's folder's (a disc subfolder). A folder
  with no audio anywhere under it (artwork alone) is left out of the folder
  views. A folder's whole tree is one run of the tracks-by-folder view
  (`treeTracks()`: its own files A-Z, then each subfolder's tree), which is
  what a folder's Play plays. (The cache file's version went to 2 with the
  36-byte folder record: an old cache is rebuilt once.)
- **The queue** (`QueueModel`, host-tested): track ids in a PSRAM array (8 B an
  entry with its key), a current position, and one level of undo. Its edits
  are the design's Library and Queue actions: Play (replace the queue, start at
  a track), Play next (after the current entry), + Queue (append), remove a
  selection, move a selection after the current entry, Clear up next (keeps
  what plays and what played), Clear. Each entry has a **key** given when it
  joins and never reused, so the UI can keep a selection or a row across edits.
  Each edit saves a snapshot first; undo puts the queue back, keeping what
  plays current if it was in the queue then.
- **Transport** (`PlaybackController`) plays the queue's current entry through
  the catalog and keeps its rules: prev/next and a track's end wrap around the
  queue (`setRepeat(false)` stops at the end instead), a track that can't be
  played is skipped, and once every track in the queue has failed in a row it
  stops. **Prev restarts a track past its first 3 s** (`prevRule()`,
  host-tested in test_playback; every prev: the A click, Now Playing's, the
  console's `p`, the headphones'): more than 3 s in, the same entry from
  0:00 in the same state. Playing, it starts again (a start like any, faded
  in); paused, it stays paused at 0:00 (the backend lets the track go, Now
  Playing reads 0:00, no resume point is left to save), so nothing starts
  out loud from a paused prev; waiting for the headphones, it waits for
  the entry's 0:00. At 3 s or less the entry before, as ever (and it plays,
  as prev always did), and so while stopped, on a cued entry (at 0:00
  already) and on a track that failed. Until the backend has taken a start
  up (`IAudioBackend::positionKnown()`) the position is where that play
  asked to start: a quick second prev after a restart or a skip is at 0:00
  (the entry before), one right after a resume point's start at 2:30 at
  2:30 (a restart). A built-in track restarts like any. A restart is no
  skip and no edit: the queue, its undo and the sleep timer's "pause after
  this track" (and its count of boundary pauses) stay as they were. The
  log says `[player] prev: this track again from 0:00 (playing)` (or
  `waiting for the headphones`, or `nothing starts`); the headphones'
  `previous: this track from 0:00`. The edits that touch what plays go
  through it: Play starts the new queue; removing the current entry plays the next one that stayed (paused: it
  is cued; none left after it: stop); Clear stops; undo returns to the entry
  that was current if the one playing isn't in the restored queue. Play next,
  + Queue and Clear up next change nothing that plays. `HeadsetKeys` works
  unchanged on top (`cueNext()`/`cuePrev()` move the current entry). A
  `Hold` (main.cpp's: Bluetooth is the output and the headphones aren't
  connected) turns every play into **Waiting**, a state of its own (not
  Playing): a new track is only selected (the backend drops what it had), a
  resume leaves the paused track where it is, skips stay waiting on the new
  track, play/pause cancels the wait (Paused); `release()` plays what waits,
  `cancelWait()` ends it paused (`PlayGate` decides which: see Bluetooth). For the sleep
  timer: `setPauseAfterTrack()` (at the track's natural end the next entry
  is cued and it stays paused at 0:00; at the queue's end without repeat,
  the natural stop; `timerStops()` counts them) and `pauseByTimer()`
  (playing pauses, a wait ends paused); both mark the pause
  `pausedByTimer()` until any play.
- **Persistence** (`app/QueueStore` over `QueueSaver`, `QueueText`): the queue is saved as paths,
  one a line (`queue.txt`, header `mstream-queue 1 <entries> <current>
  <generation>`), so a rebuilt library, whose ids differ, finds its tracks
  again; paths that are gone are dropped, and if the current one is among them
  the next one that stayed is current. The file is rewritten 2 s after the
  last edit, 32 lines a loop pass (a 10,000-track queue is ~700 KB and never
  holds the loop), into `queue.tmp`, then renamed over `queue.txt`. The
  position goes to NVS (at most once a second), tagged with the file's
  generation, so a track change doesn't rewrite the file and a position is
  never paired with an older file. The **resume point** goes to NVS too
  ("queue"/"resume", one blob: generation, line, the path's FNV-1a hash,
  ms, the length then): written at every pause (the player's
  `resumePoint()`: a paused track's position, or a start point that
  waits), so at every orderly shutdown (the CPU speed's restart pauses
  first; the idle power-off comes only paused or stopped; the sleep
  timer's end is a pause), and removed as soon as playback moves on (a
  play, a skip, another entry, an edit that changes the current entry).
  Nothing is written while playing (flash wear), so a power cut while
  playing finds none and the entry starts at 0:00. Like the position it
  pairs with the file of its generation: saved once the file holds the
  queue as it is, and again (at its new line) after an edit that only
  moved the entry. At boot one saved for the restored file's current line,
  whose track still has that path, becomes the player's **start point**
  (`setStartPoint()`): stopped, nothing plays, Now Playing shows that second
  and the length saved with it, and the next play starts there (the fade-in
  as always). It belongs to that entry's key: next, another entry, or an
  edit that changes the current entry drops it; prev on it (the Core2's or
  the headphones') goes to 0:00 of the same entry, whatever the second, and
  starts nothing: stopped stays stopped, as a paused track's restart does
  (a second prev is the entry before, and plays, as from stopped). `g0`
  carries it across the rebuild. It applies
  after any boot with one saved: the CPU speed's restart, the idle
  power-off, the power key while paused. Console: `q` and `l` print it
  (`[queue] resume point saved: 1:23 into 5 (generation 12); start point
  waiting: none`);
  `qs<sec>` sets a start point on the current entry (playing: it starts
  there now), with the length as known (the held track's, else the
  catalog's), to check the seek without a restart; `qs0` clears it. A
  dropped start point stays dropped: an undo that brings its entry back
  doesn't bring the second back.
  After a restart the queue is where it
  was, stopped. `g0` carries the queue across the rebuild the same way, in a
  PSRAM buffer: the track that plays keeps playing if it's still there. A
  rebuild that leaves no library (out of PSRAM, or a card that went away)
  isn't taken as the queue changing: what survives stays in memory,
  playback stops if its track is gone, and `queue.txt` isn't rewritten.
  When and what to write is `QueueSaver`'s (lib/core, host-tested in
  test_queue: the timing, a failure keeping the last file); `QueueStore`
  gives it the card and NVS. `flushNow()` does it all synchronously, for
  the idle power-off: a write under way finished (or, if the queue changed
  since it began, dropped and written again whole), an edit not yet written
  written without its 2 s, then the position and the resume point. `queue.tmp` only replaces
  `queue.txt` complete, so a flush that fails leaves the last good file.
- **SD access while playing**: the index cache and the queue file are
  written and read in pieces of at most 4 KB (`storage/FileStream`): one
  `f_write` holds the FAT volume's lock for its whole length, and the decoder
  reads the playing track through that lock, so a g0 rebuild's cache save
  (blocks of hundreds of KB at 10,000 tracks) never makes it wait long.
  With no saved queue, it's the whole library (artist, album, track order)
  followed by the built-in tracks.
- **Internal RAM**: the old `std::vector<Track>` library and its playlist copy
  cost ~86 B of internal RAM per track per copy (13 KB for 77 tracks, and
  impossible at a few thousand). Now the library, the queue and its undo are
  in PSRAM; the boot log's `[lib] library + queue` line and `[heap] library`
  show what is left in internal RAM, and `[heap] playing` the figure while
  playing.

## Input

One input layer (`ui/Input`) reads the touch panel (the glass and the
button strip below it); every screen gets **events** from it and nothing
else reads the hardware
(the input lab, a measuring tool, is the one exception: while it is open the
layer is suspended).

- **Touch correction** (`TouchCalibration`, host-tested). Every touch
  point goes through a monotonic piecewise-linear table per axis (x knots
  every 40 px, y every 60) before anything hit tests it. **The default is
  no correction** (identity): panels differ, and a stranger's Core2 must
  not get a table fitted to one unit's panel. The one panel measured, the
  user's, reads x too far right, more so further right: ~0 at x 60-150,
  ~+20 px at x 190, +35-45 px from x 240, and it stops at 319 (thumb and
  index finger alike: the sensor); y reads true. Its table, the
  least-squares fit (smoothed, slopes kept between 0.25 and 4) of the input
  lab's 72 target-practice taps, is kept as `TouchCalibration::labFitX()`:
  the host tests fit the same logs and check it still matches, and use its
  inverse (`Axis::unmap()`) as a skewed panel. A reading clamped at 319
  can't say how far out the finger was (the lab table puts it at ~282). So
  **a control at the right edge must have a hit area that reaches the
  screen's edge and is at least ~40 px wide**, and **two controls side by
  side on the right half need their split well right of the left one's
  centre**: uncorrected, the lab's panel reads a tap on View's centre in
  the toast (x 214) at 242, and one on the speaker card's volume chip (242)
  at 269. `test_ui_library` audits those pairs (the toast's View / Undo,
  the sleep toast's +10 min / Turn off, the tabs, the speaker chip / radio)
  against the lab panel and a true one. (The jump grid's 44 px cells can't
  be: uncorrected, a right-half letter lands one cell right; that is what
  calibrating is for.) Events also flag a clamped reading
  (`InputEvent::atRightEdge()`, `inRightEdgeZone(left)`). A table is saved
  in NVS (namespace `input`, key `cal`), checked by a checksum when loaded;
  without it, no correction.
- **The touch check and calibration** (`ui/CalibrationScreen`, the rules
  in `TouchCheck`, host-tested; texts and rooms in `UiText`). One screen,
  made in PSRAM on first use, that owns the display while up (main.cpp
  suspends the UI and the dancer; the screen stays lit, and the idle
  power-off waits for it), drawn with the UI's fonts, colours and the
  Output accent. Every button is a **full-width 40 px row hit tested by y
  alone** (the panel reads y true), the glass's way out is a pill at the
  top **left** (x < 110: a panel that reads right carries taps away from
  it), and the strip's **A click is the way out on every page** (Skip, Not
  now, Cancel, Discard, Done; Undo right after a Save), with a red arrow
  and "A: Cancel" over the A dot. That is the one exception to
  `ButtonPolicy`'s "the same on every screen": main.cpp routes A's click
  to the screen only while it is up (A's hold and all of B and C stay the
  policy's). A touch that went down before a page or cross was drawn, or
  within 300 ms of it, is ignored (a bounce is never the next cross's
  sample), and after 60 s with no touch it closes as its way out (nothing
  saved, and the first-boot check not answered). It opens four ways:
  - **The first-boot check**: with no table saved and NVS `input`/`cal_ask`
    unset (`TouchCheck::due()`), before the UI starts (so before the tips).
    Three dots, one at a time, at x 50, 190 and 280 (where the lab's panel
    was 0, +20 and +40 px off), each tap leaving a mark; one more than 90 px
    off is asked again, once. The verdict (`TouchCheck::verdict()`):
    calibrate when a dot is more than 30 px off, two of three more than
    15, or a dot away from the edges read the clamp; it says which way
    ("Taps land about 40 px to the right of your finger"), with
    [Calibrate] [Not now]; else "Touch is accurate", a tap goes on. Only
    an answer stores `cal_ask` (`TouchCheck::answers()`): Skip, Not now,
    Calibrate, going on from "Touch is accurate" (A for any of them), or a
    calibration tapped through to its result, however it was opened. The
    60 s close doesn't (nor a Cancel on the crosses, or `aq`): a Core2
    switched on and put down for a minute asks again at its next boot,
    until someone answers. **The user's own unit
    has no saved table and no `cal_ask`, so it shows the check at its next
    boot**: that is the intended first device test, not a regression.
  - **Output > Touch calibration** (its line: "Not calibrated" /
    "Calibrated on this Core2"): a sheet with Calibrate, Test taps,
    and Remove calibration (red, while a table is saved; a dialog asks).
  - **The rescue**: while the start-up screen shows (the UI not started,
    glass events going nowhere), a finger held anywhere on the glass for
    2 s opens the crosses, and the UI's start waits while that finger is
    down. The screen's last line says so, and it stays at least 1.5 s after
    that line shows (setup() can outlast the screen's 3 s). It needs no accuracy, and a glass hold has no other meaning
    there (a B hold would switch the output at 500 ms). The finger goes on
    once the screen lights: the FT6336U likely takes its baseline at power
    up (still to check on the device).
  - The console: `a` (`a5`-`a9`), `ac`, `ab` (`ab0` forgets `cal_ask`).
    With the screen off, these wake it and the screen opens once the panel
    is awake: nothing may be drawn into a sleeping panel (below).

  The crosses (`TouchCheck::kCross`: 9, distinct x and y each; no cross's
  sample window reaches the Cancel pill, and none is drawn in the A
  hint's corner): a tap within 70 px (x) and 50 (y) of the cross, from the
  raw reading, is a sample (a tick and a green flash); a miss buzzes (the
  inert double buzz) and asks again (a retap counts after 150 ms, not the
  page's 300: only a bounce of the miss is ignored), and after two misses
  a third tap within 20 px of the last is taken, up to 120 px from the
  cross (two taps that agree are the panel's reading, not the finger's).
  The glass's two ways out are judged first: the header's Cancel, and the
  "A: Cancel" label over the A dot (x < 200, y >= 222, at least 22 px
  below every cross; on the other pages that band alone is the A label,
  as the A click). Then the fit (as above) and the result: "Now: up to 42
  px off, average 21" (the table in use) / "Calibrated: up to 12 px off,
  average 6" (2D distances), with [Save] [Try again] [Discard]. The new
  table's figures are leave-one-out (`TouchCheck::measureUnseen()`: each
  tap against a table fitted to the others, 2n+2 small fits): measured on
  the taps it was fitted to, 9 x knots follow 9 taps' finger scatter, and
  a panel that reads true was told to save a table worse than none (a
  simulation of a true panel with 5 px of scatter per axis: "better" in
  44% of runs). **A reading at the panel's clamp on a skewed panel is the
  exception**: on that axis it is judged on the table fitted to all the
  taps. Every finger past the clamp reads the same, so the other taps
  can't predict it: left out, the table's end was extrapolated from the
  taps inside, which the saved table never does (it puts the clamp where
  the taps that read it were aimed). On the lab's panel the crosses at x
  20 and 300 read 0 and 319, and they set the "up to" figure (up to 15-23
  px in the host simulation, 15-20 on the device, against 27-35 with no
  table): it undersold a good fit. With the rule it reads up to 8-14
  (average 5-8), and it is still never better than the table on its own
  taps. Skewed: on the taps inside the clamps, leave-one-out beats the
  table in use by 3 px on average. A panel that reads true reads the
  clamps too when an edge cross is tapped 20 px towards the bezel, and
  judged on the fit those two taps flattered its table into "better" (37
  of 588 simulated runs with both edge crosses so); its taps inside show
  nothing to gain, so there, as on a panel already corrected by its
  table, the figures stay plain leave-one-out (`test_touch_input` checks
  them against it). `TouchCheck::outcome()` judges the figure shown
  against the one with the table in use: Save first only when the new
  table is at least 3 px closer on average (the same simulation, before
  the clamp rule: 0.1% of runs on a true panel, 95% on the lab's); else
  Discard first, "already accurate" when the taps average within 8 px
  with the table in use, "no better" otherwise: a table fitted to finger
  scatter helps nothing. A fit whose rms is over 15 px on either axis is
  "the taps didn't agree", with Try again first. After Save the check page
  (a coral ring where the Core2 reads each tap, a grey dot where it would
  without the table; its key, a grey dot and "without calibration", in
  the A hint's band), where A undoes the Save (the table before is kept in
  RAM). Its two lines of instructions go at the first tap: the marks draw
  anywhere from the top of the screen (the header too) to the Done bar,
  never under or over the instructions, so a tap near the top shows where
  it landed like any other (they drew over the lines before, and a tap on
  the header showed nothing). After 16 taps the marks clear, the header is
  drawn again, and it goes on.
- **The scripted finger on a skewed panel** (console `uk1`, `uk2` with up
  to 4 px of deterministic jitter a touch, `uk0` off): the scripted finger
  (`uit`, `uih`...) then reads x as the lab's panel did (its table's
  inverse, clamped, the edge flags from that), and the table in use
  corrects it as a real touch's, so a run can take the check, the crosses,
  the fit, the Save and the check page end to end and see the fit recover
  roughly the lab table (`test_touch_input` does the same on the host).
- **Checked on the device** (2026-09-30, the user's Core2, scripted finger
  only: no hand, so the FT6336U's behaviour with a finger already down at
  power-up is still open):
  - Boot with nothing saved: "[input] touch: no correction (default), the
    touch check is due", and the check opened ~1.4 s after the console line.
  - The check with `uk1`: the dots read 2, 21 and 37 px off, "Taps land
    about 35 px to the right of your finger. Calibrate now?"; Calibrate
    opens the crosses. With `uk0` (an exact finger): "Touch is accurate",
    a tap goes on.
  - The crosses with `uk2`: progress "n of 9", a miss (a tap 140 px away)
    buzzes and shows "Missed: tap the cross itself", then a tap on the
    cross counts. Cancel by the top-left pill, by the strip's A (from
    Output's sheet, back to the same scrolled Output list), Discard, Try
    again, Save, the test taps page (rings, grey uncorrected dots), Done by
    its bar and by A, A's Undo right after a Save ("Undone", no correction
    again), and a deliberately scattered run: "The taps didn't agree".
  - Four fits of 9 jittered taps: "better", now up to 27-34 px off
    (average 15-16), the new table up to 15-20 (average 7-10) on taps it
    wasn't fitted to, 4-7 on its own. The leave-one-out took 393 ms on the
    loop task. One saved table: x 40->45.0, 80->78.5, 120->117.2,
    160->151.1, 200->181.7, 240->215.1, 280->254.5 (the lab's: 43.2, 78.5,
    122.7, 154.4, 181.9, 212.3, 253.1); the ends differ (0->17.8, 319->297.9
    against 25.6 and 281.3) because the crosses at x 20 and 300 read at the
    clamps. The unseen "up to" figure is set by those two edge crosses
    (left out, the edge is extrapolated), so it reads worse than the saved
    table does at the edges. (Fixed since: on a skewed panel, clamped
    readings are judged on the fit, above; rechecked below.)
  - Output: the row reads "Not calibrated" / "Calibrated on this Core2";
    the sheet shows Remove calibration only when calibrated; its dialog's
    Cancel keeps the table, Remove takes it off with a "Calibration
    removed" toast.
  - The rescue: a glass hold at boot opened the crosses at 2.0 s with the
    UI waiting; A left them and the UI started. A strip B hold in the same
    window switched the output (to Bluetooth) and did not open them; a B
    hold after start switched it back.
  - The screen stayed lit with the check up and untouched, which closed
    itself at 60 s; then the usual dim (20 s) and off (30 s).
  - The user then calibrated with a real finger: it worked well. Three
    follow-ups, rechecked on the device (scripted finger, the user's own
    table in use): `ab` left untouched closed at 60 s with "the touch
    check isn't answered", and `as` no longer said "answered"; opened
    again, Skip stored it. Four `uk2` runs of 9 crosses: "better", now up
    to 14-19 px off (average 10, the user's table on the lab's skew), the
    new table up to 9-13 (average 5-7) on taps it wasn't fitted to, 5-8
    on its own, the leave-one-out 435-486 ms (against 15-20 by plain
    leave-one-out in the run above). The test taps page lost its two lines at the first tap; rings and
    grey dots then showed near the top (one on the header, clipped at the
    screen's edge), and the 17th tap cleared the marks with the header
    drawn again. The "without calibration" key sits in the A band.
  - Two bugs found and fixed. (1) Opened while the screen was off (the
    console), the screen drew into the sleeping panel (Ui::suspend() lets
    the screen that takes over draw at once): its last piece, the A hint,
    came out shifted ~118 px, in the wrong colours. Now it wakes the screen
    and opens once the panel is awake. (2) A B click refused by the pocket
    rule while the calibration was up drew the UI's "Tap the screen first,
    then B plays" toast over it: `Ui::warn()` and `Ui::toast()` drew while
    the UI was suspended (`note()` already didn't). Both now log and skip.
  - Not wired, by design: B isn't Done on the check page (A and the Done
    bar are), and a B click while the calibration is up plays or pauses as
    ever.
- **The glass** (`TouchRecognizer`, host-tested): Down, Tap (within 12 px
  and 500 ms; its position is where the finger landed), LongPress (500 ms,
  while still down), Release, DragStart / DragMove / DragEnd, and Fling.
  The release velocity (over the last 60 ms) is **capped at 2,000 px/s**,
  and `KineticScroll` caps flings at the same speed: faster, nearly every
  frame of the hardware scroll moves more than its 84-line step and becomes
  a full redraw. A touch that lands on the button strip (raw y >= 240) is
  the buttons' and makes no glass events, unless it is a swipe up from the
  strip (below).
- **The button strip** (`StripButtons`, host-tested). The three buttons
  are made from the same touch point as the glass, not taken from
  M5Unified's BtnA/B/C: those press for any point in the strip that isn't
  "moving", and once one is down a sliding finger adds its neighbours. So
  a flick up the Queue that ended in the strip, where the panel lost the
  finger and found it again as a new touch, clicked B and C at once (a
  "random pause" and a skip, in the device log). Now **only a touch that
  went down in the strip presses a button**: the one under where it went
  down (raw x 0-106 A, 107-213 B, 214-319 C, M5Unified's split), however
  it drifts after. Moving more than 20 px from there cancels it (no click,
  no hold, the repeats stop: `ButtonGesture::cancel()`). **A swipe up from
  the strip scrolls**: if that move is upward (at least as much up as
  sideways, or onto the glass by then) the touch is handed to the glass
  recogniser as a drag (`TouchRecognizer::fromStrip()`): a DragStart at the
  hand-over point with no Down before it (so the list moves with the finger
  from there, no jump), then DragMove, DragEnd and Fling (capped) as any
  drag, each flagged `InputEvent::fromStrip`; never a Tap or a LongPress,
  and no tick. The Ui gives it to the page (nobody's under a modal: the
  sheets, dialogs and the volume slider only have presses), and only the
  lists follow it (`ListView`, the Library, Queue and Output pages; the
  Queue's edit bar and the A-Z rail are not pressed); Now Playing ignores
  it. A slide along the strip, or down it, stays ignored, and one that
  later turns up onto the glass becomes a swipe there; a press that has
  held (500 ms) did its hold and never scrolls. A touch that went
  down on the glass never presses one, wherever it goes; a strip touch
  starting soon after a touch that wasn't a press lifted (a glass touch,
  or a strip touch already ignored) is ignored as the panel finding that
  finger again: within 150 ms anywhere, and within 400 ms if that touch
  had moved (a swipe) and this one is within 60 px of where it was last
  seen. (The captured finger came back within ~145 ms; one found a little
  later, sliding less than the 20 px slop into C, would otherwise click B.
  A deliberate press elsewhere, or after another press, is not held up.)
  Such a touch presses nothing, but a swipe up from it still scrolls
  (flicking the list again and again from the strip is quicker than the
  windows); a swipe from the strip, once lifted, opens them like a glass
  touch.
  Only the panel's first touch point counts, as for the glass: a second
  finger pressing the strip does nothing, and when the first point
  becomes another finger without a lift (the first lifted, a second
  stayed on) the old touch lifts and the new one goes down there, bounce
  rules and all. Each rejected strip touch logs
  `[button] ignored: <button> at x,y (raw): <why>` once (a second finger
  too), and a swipe handed over `[button] B at x,y (raw): a swipe from the
  strip (n px): scrolling` (its fling logs `[touch] fling ... from the
  strip`). The user's
  measured presses (down at y 247-278, clicks 17-143 ms, holds
  509-2383 ms) all still count (the host test replays them).
- **The buttons** (`ButtonGesture`, `ButtonPolicy`, host-tested): Click,
  Hold at **500 ms** (the user's clicks lasted 17-143 ms, holds 509-2383 ms),
  Repeat every 200 ms for A and C, HoldEnd. The same on every screen (but
  A's click while the touch calibration is up: its way out, above): A
  click previous (past a track's first 3 s: the same track from 0:00,
  paused still paused) / hold volume -5 % (repeating); B click play/pause / hold
  switch the output, and switching **to the speaker always pauses first**;
  C click next / hold volume +5 % (repeating). Each hold leaves a
  `Feedback` (the new volume, or the new output and whether it paused) for
  the screens' HUD. The glass taps near the bottom edge never fired a
  button in the user's tests, so there is no dead band.
- **Haptics**: a tap tick (33 ms at level 235, 3.3 V) on every button click
  and a double tick (2 x 33 ms, 80 ms apart) the moment a button hold is
  recognised, both played by the input layer. On the glass the feedback is
  played by whatever acts on the touch (`tapTick()`, `holdTick()`), so it
  confirms only what did something: a tap on a control, a long press on a
  list row that has a hold (an artist, an album, a Queue row). A long press
  on anything else is no hold: its lift, where it pressed, is a tap (the Ui
  converts it). Nothing on scroll frames, and optionally a tick per new
  letter on the A-Z rail (`railTick()`). Both can be turned off (`ah0`,
  `ar0`, and the haptics also on the Output tab; NVS keys `haptics`,
  `railtick`). `Haptics` plays patterns from a
  FreeRTOS timer, so a tick lasts its length whatever the loop does.

- **The screen's wake** (`WakeLatch`, host-tested in test_ui_input with the
  recognisers and in test_screen_power; [ENERGY.md](ENERGY.md) item 2). A
  finger that lands while a touch doesn't act (dim, off, or lit by an
  event nobody has answered yet) only wakes it: every event is dropped
  (the path the input lab's suspension takes; the recognisers keep
  following the finger) until no finger has been on the panel for 400 ms
  (`kQuietMs`, StripButtons' swipe bounce window: the panel loses a finger
  and finds it again, and a finger found again after a shorter window
  would be a fresh press of B). So its lift is swallowed too (a button
  clicks on its lift, a tap and a fling are made there), and so is any
  other finger meanwhile: no tap, click, hold, volume repeat, swipe or
  tick. It is a hearing-safety rule: a press of B in a pocket, with the
  speaker as the output, can't start music. Each wake logs `[screen] wake
  by touch at x,y (raw)` (and the button, on the strip). A finger already
  resting on the panel as the screen dims is taken too (its glass touch
  ends with a Cancel), but isn't a wake. The scripted finger isn't a
  finger for it: it acts in the dark (and lights the screen).
- **The pocket rule** (`ScreenPower::unattended()`, `ButtonPolicy::
  Transport::startRefused()`). After any wake from Off (a touch, the PWR
  key, an event, or something that keeps it lit) nobody may be looking:
  until a touch lands on the glass (or the PWR key while lit, or the
  console), a B click that would start playing on the speaker does
  nothing but the inert buzz and a note, "Tap the screen first, then B
  plays". A pause, A and C, the volume, the B hold (a move to the speaker
  pauses first) and a play on the headphones act as ever. Going Off ends
  it. An event's wake from Off also leaves the first touch only answering
  it (swallowed, as a wake from Off would be), so a pocket's contact can't
  tap the dialog's "Use speaker".

The layer costs ~660 B of internal RAM (an 8-event queue, the tables, the
recognisers); the calibration screen is in PSRAM.

## UI

The tab bar design (the design review's winner, with its grafts and the
user's measurements), built as a framework the screens sit on:
`ui/Ui` owns the display, `ui/TabBar`, `ui/ListView` and `ui/Overlays` are
its widgets, `ui/Thumbs` makes the album covers, `ui/EmptyState` draws the
empty and error states, and the five tabs' pages sit on them, all as the
spec has them with the review's grafts: Now Playing, the Library, the
Queue, Dance and Output (with its Pair and About pages).

- **One owner, on the loop task.** The UI is the only thing that draws
  (while no other screen has taken the display: the touch calibration or a
  spike tool, which suspend it; it redraws everything when they let go; one
  at a time: the calibration closes any spike screen, and a spike command
  closes the calibration, so no two draw at once). It
  runs from `loop()`, not a task of its own: the player, the queue, the
  library index and the input layer all live on the loop task and aren't
  thread-safe, so a UI task would need a lock around each, and its own stack
  in internal RAM (6-8 KB of the ~48 KB left while playing). The loop runs
  below the decoder on core 1 (priority 1 vs 2), so the audio goes first,
  and it sleeps 1-5 ms every pass. The `Ui` object and all its sprites live
  in PSRAM (~365 KB: six 320x42 row sprites, a 320x56 strip, a 320x204 panel
  for sheets and dialogs (the content area: the 4-row sheet), the rail); the fonts' glyph tables too.
- **The screen's power** (`ScreenPower`, host-tested; `app/ScreenControl`
  on the device; [ENERGY.md](ENERGY.md) item 2: the screen was ~15 mA of
  the ~116 while streaming). Bright at the chosen brightness (Low 60,
  **Medium 100**, High 160, Max 255), Dim (backlight 30) for the last 10 s
  (from 7 s with 15 s), then Off (the backlight's DCDC3 off and the
  panel's sleep-in) after the chosen time without input: 15 s, **30 s**,
  1, 2 or 5 min, or Never; the same whether playing or not. Input is a
  touch that acts as it lands or moves (a finger resting still stops
  counting after 15 s, `FingerActivity`, so a pocket's pressure can't keep
  it lit), or the PWR key. A touch
  (glass or strip) or a PWR short press on a screen that isn't bright
  only wakes it (the input layer swallows the touch: "The screen's wake"
  above). A wake from Off with no input after it goes off again 10 s
  later, without the dim step (the pocket guard; not with Never). What
  needs the listener wakes it with the whole countdown: the headphones
  lost (the dialog), "Couldn't reach", a track that failed (their
  `Ui` calls, `UiHost::wakeScreen()`), USB plugged in or out (AXP192 reg
  0x00, read once a second). Headphone keys, track changes and a link
  coming up don't. It stays lit while the touch calibration or a spike
  tool has the display, a play waits for the headphones, or a pairing is
  under way (`BtSession::pairingUnderWay()`: not one whose failure the
  card still shows). While a toast with a countdown is up (the idle
  power-off's warning, the sleep timer's fade while it counts down to the
  pause: `holdLit`) a lit screen
  (bright or dim) goes bright and stays lit until it ends, then counts
  down from there; an off one stays off (it may be night). One woken
  during it is held from the wake, except that a touch's or PWR's wake
  from Off keeps its pocket guard until input follows (for the idle
  warning that wake is input, which ends the warning). `ScreenControl` alone switches the backlight, and the
  panel's sleep-in and sleep-out (under `LcdLock`, at least 120 ms apart;
  5 ms after a sleep-out before anything is drawn). Going
  off, the UI goes **dark** first (`Ui::setDark`): every `gfx` fill and
  push is dropped (and the list's and the dancer's own pushes), the loop
  keeps the snapshot, dialogs, toasts and their timers but draws nothing,
  a fling stops, no cover job starts, the dancer stops, and the Pair
  screen stops its scan. Waking, the UI draws everything (the list's
  scroll registers sent again, the tab bar, the page, whatever was open
  over it: a dialog opened in the dark is there) after the sleep-out and
  its 5 ms, with the backlight still off, then the backlight. Nothing is
  drawn into the sleeping panel: on the device, pixels written in
  sleep-in landed garbled in its memory (rows shifted, colours
  byte-swapped) and stayed so until drawn again. Each change logs
  `[screen] <from> -> <to> (<why>)`. The Bluetooth search rests at once
  after its burst while the screen is off and nothing plays or waits,
  unless the headphones dropped while listening (`BtSink::setQuiet()`). NVS namespace `screen`: `off_after`, `bright`.
- **Frames.** Only what changed is redrawn (each page keeps what it drew and
  compares the one `AppState` snapshot the host fills per pass). Animation
  (a moving list) is drawn on a **30 fps cap kept on deadlines**
  (`FrameClock`: each frame due a period after the previous one's deadline,
  so a late pass doesn't push the rest back; after a stall the cadence starts
  over, no catch-up burst), with `ScrollGovernor` as the safety net (fewer
  frames, or whole rows, when the decoder's buffer runs low). Everything is
  drawn into a PSRAM sprite and pushed in pieces of at most 40 lines, each
  under its own `LcdLock` (`ui/Gfx`).
- **Navigation** (`NavModel`, host-tested): five tabs (Now Playing, Library,
  Queue, Dance, Output), a stack of pages each. A tab tap switches (the tab
  keeps its stack); a tap on the current tab takes it to its root (the Queue:
  back to the playing track); "‹" in a header pops. Each stack entry keeps its
  page's scroll position and expanded row. Deeper than 8, the oldest page
  above the root goes.
- **The tab bar** (`TabBarModel`, host-tested for layout and redraws): 36 px
  at the top, in the panel's fixed area. Tabs 0-53, 54-107, 108-161, 162-215
  and **Output 216-319, to the screen's edge**: the user's panel reads the
  right side up to 45 px too far right and stops at 319, so a reading clamped
  there is the Output tab wherever it's corrected to. The **volume is part of
  the Output tab** ("60%" beside its icon; a separate chip in the corner took
  every tap meant for the Output tab), with the battery in the same cell. The
  active tab gets a plate, a text label and an underline **in its section's
  colour** (Now Playing coral, Library violet, Queue teal, Dance pink, Output
  blue; the page headers and primary buttons carry it too). Status: the Now
  Playing icon's EQ bars move 4 times a second while playing, with a progress
  hairline (dotted while the length isn't known); the Queue's up-next badge
  (flashing for 1.5 s when tracks are added); the Output icon's colour is the
  Bluetooth state (cyan connected, amber connecting, red lost or a
  connection that failed); the EQ bars lie flat in amber while a play
  waits for the headphones; a 7 x 7 moon in the Now Playing cell's corner
  while a sleep timer runs (amber while it fades). The Library
  icon is a record half out of its sleeve, unlike Now Playing's bars. Only
  the cells whose state changed are drawn (a 54x36 push, ~1 ms).
- **Texts that must fit**: the fixed texts with a fixed room (the coach
  cards, the empty states' buttons, the Queue's bar, the Output tab's
  buttons, status lines and hints, the two-line toast's name) are in
  `lib/core/UiText.h` next to their room, and `test_ui_library` measures
  each with the firmware's own DejaVu data (as it does the tab bar's), so
  a label cut to "Remov…" or "Tap ag…" on the device fails a host test.
- **Overlays**: the **toast** (with Undo for 4 s after a queue edit, else
  1.8 s) sits at the **top of the content area** (y 36-71, over the page
  header: the usability walk found the spec's bottom toast right above
  BtnC); the pages' drawing leaves those rows alone while it's up
  (`gfx::setCover`), and the header is redrawn when it goes. One line when
  it fits; "Plays next: <name>" whose name doesn't fit beside View and
  Undo (54 px pills, their hit areas 190-249 and 250 to the edge) takes
  two lines, "Plays next" small over the name, and the buttons become
  icons (Undo's arrow, View as the Queue tab's icon; hit areas 228-263 and
  264 to the edge), so the name has 216 px (Body, else Small): "Can'T
  Tell Me Nothing" and "Harder, Better, Faster, Stronger" both fit. It
  stays 36 px tall: a taller toast would reach the list's hardware-scrolled
  band, which carries whatever is in it along with the list. The header's
  ‹ (and its pill, when the toast has no buttons) still works under it: a
  touch there hides the toast and goes to the page. **Sheets** have a ✕
  pill in their title row, all rows alike but a main choice (the Library's
  Play, Bold in the accent) and one that can't be taken back (red). The
  **volume sheet**
  (Now Playing's volume button; mockup 04): from y 124, the output and its
  volume, − and + (to the edge), a slider to tap or drag in 5 % steps
  (nothing changes on touch-down, and a touch in its first 300 ms is
  ignored: a quick second tap on the chip that opened it would land on
  the slider); closes 3 s after the last change, on a tap above it or a
  tab. Its steps
  go through the host's `stepVolume()` as the buttons' do (relative to the
  last one asked for, since Bluetooth applies them on its own task: the
  volume read back lags a drag), so AbsVolumePolicy and BtControl see
  nothing new. The **jump grid** (a tap on the A-Z rail; mockup 08) covers
  the content: '#', A-Z in 7 x 4 cells of 44 x 41, the letters with no rows
  faint and inert, the list's letter outlined; a letter with more than 16
  rows opens its second level, the letter and its 26 two-letter starts
  ("Ka", "Ke", "Ki"... in the library's folding: "Kæthe" is "Ka"), with a
  way back (`JumpIndex`, host-tested: binary searches on the names, no
  index, no memory). In 5,000 artists any one is three taps and a short
  scroll away. The **volume
  HUD** covers the tab bar for 1.5 s after an A/C hold or the headphones'
  volume keys, with or without absolute volume (20 blocks and the value; a
  change within 2 s of connecting is the link-up cap, not shown; a B hold
  says where the output went and whether it paused); a tap on it still switches the tab. The
  volume sheet is **per output**: Now Playing's opens the active one's,
  the Output cards' chips the speaker's or the headphones' whichever
  plays (`UiHost::stepOutputVolume()`: the active one's as the buttons
  do, the headphones' as their own keys do, the speaker's directly). The
  **coach cards** (first boot, once: NVS `ui`/`coach`; About's "Show the
  tips again" and console `uic`): the three red buttons' clicks and holds,
  each over its button with an arrow down to its dot, then "tap the tab
  you're on again: back to its start"; a tap anywhere goes on, a tab tap
  ends them. A **sheet**
  (from the bottom, rows of 40 px: up to 3 in the list's band from y 72,
  the 4-row one from y 40, never onto the tab bar) and a **dialog** (modal, the tab bar
  still works; optionally an icon, a live status line and a red primary
  button) freeze the page under them; one that opens while a finger is
  on the page (the headphones' drop dialog) ends that touch for the page (a
  Cancel), so its lift can't act or draw under it. When a toast goes, the
  header row it covered is drawn again (`Ui::uncover()`); a page with no
  header (Now Playing, the Dance tab) draws all of itself for that, so every
  sheet up over it (the "..." sheet, the Sleep timer sheet, the volume
  sheet) is drawn again after it (found on the device: the fade toast's
  +10 min over the Sleep timer sheet left the sheet up and live but unseen,
  and a tap on "..." hit its Turn off). All are opaque and drawn
  through the list's scroll mapping, so they land right over a scrolled
  list. After an add (Play next, + Queue) the toast also has **View**: the
  Queue, scrolled to the first added entry (found by its queue key).
- **Lists** (`ui/ListView`, with `ListLayout`, host-tested): virtualised rows
  (a source gives a count and a row renderer; standard pieces for 1- and
  2-line rows, the number column, a disc, a 40x40 thumbnail slot, a
  chevron), 42 px items in the band y 72-239 on the LCD's **hardware
  vertical scroll** (`ListScroller`), inertia from `KineticScroll` with
  **flings capped at 2,000 px/s**, presses highlighted on the Down and acted
  on at the Tap (a drag never taps, nor does a press that stops a moving
  list), long press on rows that have a hold, a top action bar (a
  container's Play / Play next / + Queue) and **inline action rows** (a tap on
  a track opens its bar under it; `ListLayout` keeps the tapped row where it
  was and scrolls just enough to show the bar), and **selection mode**
  (checkboxes; `BitSet` in PSRAM). The right edge is an **A-Z rail** on long
  alphabetical lists (over 30 rows) or a thin scrollbar: both sit in the
  scrolled band, which moved them on every frame (the user saw the scroll
  bar jitter), so they are **hidden while the list moves** (their column
  cleared once, the rows pushed full width over it) and drawn again when it
  settles, or at once when a finger lands on the rail (x 280 to the edge).
  A drag on it scrubs (a haptic tick per new letter); the touch has no hold
  (`TouchRecognizer::noHold()`), so resting on it to read the letter and
  then sliding still scrubs. A tap on it opens the jump grid (above). A
  press that only touches the list (a tap) doesn't hide it. **A move is
  drawn in steps of at most 84 lines a frame** (the hardware scroll's
  one-hold step, `ListLayout::stepToward()`): only flings were capped, so a
  finger faster than ~2,500 px/s made a frame move more than that, a full
  redraw of 50-78 ms (1 motion in 5 on the device). Now the list trails such
  a finger by a frame or two (84 lines at 30 fps is 2,520 px/s) and catches
  up as it slows; flings (2,000 px/s) are never held back, and jumps (a
  scroll to a row, the rail, a page's first frame) still draw at once. The
  rail and the scrollbar stay hidden until it has caught up. **The next
  row is rendered ahead** (`ListView::renderAhead()`): the device showed
  that the remaining long frames (35-90 ms, in most motions while an MP3
  played) were not big steps but a single row's render (~5 ms) stretched
  to 20-39 ms by the audio decoder, which runs above the loop, in the
  middle of it. So between frames, while a list moves and the next frame
  is at least 15 ms off, the row just past the edge it moves toward is
  rendered into the spare slot (5 rows show at most, 6 slots); the frame
  that brings it on screen only pushes it. Drags and flings then drew in
  2-5 ms a frame on average, 10-25 ms at most; only very fast drags (over
  ~3,000 px/s, two new rows in one frame) still have one frame of 35-55
  ms. `ui` counts the rows rendered ahead. The band can
  be **shorter** (`Ui::setListBand()`: the Queue's selection mode, 72-197):
  the LCD's scroll area is defined again with a bottom fixed area
  (VSCRDEF's BFA), so a bar under the list (198-239) stays put while the
  list scrolls; the rail's pushes stop at the band's end. An **empty list**
  shows its source's empty state (a disc and icon, a title, a line or two,
  up to two buttons, drawn in strips through the scroll mapping): the
  Queue's "Your queue is empty" (Open Library, Shuffle all), the Library's
  "No music found" and, with no card, "No microSD card" (Try again). A
  row can have buttons of its own (the Output card's): the source gets
  where on the row it was tapped (`onTapAt()`) and where a finger presses
  (`Row::pressX`).
- **Text** (`ui/Fonts`, `TextFit`, host-tested): DejaVu Sans 16 and 13 and
  DejaVu Sans Bold 16 and 22 as anti-aliased VLW fonts in flash (~170 KB;
  `tools/vlw_font.py`), which cover ASCII, Latin-1, the common Latin
  Extended-A letters and the typographic punctuation. A character without a
  glyph is folded by `TextFold` (e.g. "Ł" to "L"); a name too wide is cut
  with "…". efont is out of the build (the font probe keeps it behind
  `UI_SPIKE_EFONT`). Licence: `LICENSES/DejaVu-Fonts.txt`.
- **Icons**: 1-bit bitmaps in flash (`tools/ui_icons.py` -> `IconData.cpp`).
- **Now Playing** (spec §6.1, mockups 01-04): the album's cover (96 x 96,
  its thumbnail, a note until it's made), the title (Bold 22 on up to two
  lines, else Bold 16 on up to three), the artist and the album as **40 px
  bands** right of the cover (the review's graft: a tap meant for one never
  opens the other; the cover is the album's too), the progress and times
  (between them "4 of 16 · SPYDRONE": where it plays, mockup 01's output
  line, when it fits; the headphones not connected, "SPYDRONE (not
  connected)" alone when both don't fit), and the transport row (the
  volume, prev, play/pause, next, "..."). While a play waits for the
  headphones (`PlayGate`), the play button is a spinner (a tap cancels the
  wait), "Waiting, 24 of 86" is the progress line, and the artist and
  album bands give way to "Waiting for SPYDRONE..." over "try 2 of 3" and
  two plain buttons, **Play on speaker** and **Cancel** (out loud is a
  choice, not the way on: neither is the accent). While a sleep timer runs,
  the progress line has a moon and "23 min" ("track", "45 s", "fading")
  after "4 of 16 · Speaker"; when both don't fit the output's name goes
  first ("4 of 16" and the moon), then the line (the moon alone), but the
  amber "SPYDRONE (not connected)" never does (the moon, or nothing of the
  timer, beside it: `uitext::sleepLineFit()`; see "Sleep timer" above). The
  artist opens the Library at that artist, the album at the album (one Back
  from its artist), each **scrolled to the playing item and tinted**; "..."
  is a sheet of Sleep timer (its state), Go to artist, Go to album and
  Show in folders (each with its name, dim, on the right; the Sleep timer
  row's follows the timer while the sheet is up; four rows of 40 px, the
  sheet rising into the header row from y 40: `SheetLayout.h`), the last
  opening the chain of folders down to the
  track's, one Back apart, the playing file tinted. The volume
  button opens the volume sheet. Only what changed is redrawn: the cover
  when the album changes or its thumbnail arrives, the text when the track
  does, the times once a second, the transport on a change.
- **The Library** (spec §6.2, mockups 07-15, with the grafts): the root's
  header is the segmented control **Artists | Albums | Folders** (the root
  PageRef's id is the segment; each keeps its own scroll; the Library opens
  on the last one). Artists A-Z (a disc with the initial, "1 album, 14
  tracks") > an artist (its Play / Play next / + Queue bar, "All tracks",
  its albums with covers) > an album's or all its tracks. **Albums**: every
  album A-Z with its 40 x 40 cover and artist. **Folders**: a folder's
  folders (amber icon, "1 folder, 13 files, 1 other"), then its audio files
  (a file icon, the name, an MP3/FLAC badge); the header's second line is
  its counts, whole ("14 audio files, 1 other"), and, when 60 px or more
  are left beside them, the folder's place cut from the left
  ("…/Kavinsky"; beside "‹ Library" there is no room, and "/Daft…" said
  nothing on the device); the root's bar is **"Play all N"** and
  + Queue instead of a Play that would quietly replace the queue with the
  whole card. Every container's list starts with its bar; a tapped track
  or file opens the **inline action row** under it (Play: its album or
  folder from it, the rest following; Play next and + Queue: it alone);
  **a long press on any row** gives the same three in a sheet. Deeper than
  one level the header's pill is **"‹ Library"**, the root crumb. The row
  holding what plays (its artist, album, track, the folders on its path) is
  tinted in every list. The rail and the jump grid are on the Artists and
  Albums lists and on folders of folders, once over 30.
- **The Queue** (spec §6.4, mockups 16-18, with the grafts): opened on the
  playing track (tinted, on top; there again with no row open when the
  queue changed while you were away); the header's summary "4 of 16 · 12
  up next · 49 min" (`QueueView`: the lengths are learned as tracks play
  into a `DurationBook`, 2 bytes a track in PSRAM, reset with the library;
  "49+ min" while some are unknown; "4 of 16 · 49 min" when the long one
  doesn't fit beside Edit: the position is kept first); a tap on the title goes to the playing track,
  the top, the end in turn; a row's bar **Play now / Play next / Remove**.
  **After an add in the Library** (Play next, + Queue) the next visit
  scrolls to the first added entry and highlights what was added
  (`AddedMark`: every entry's key only grows, so what came since the first
  add is every key from that one's; the visit clears it, a Play in the
  Library clears it, the toast's View does the same at once). **Selection
  mode** (Edit, or a long press: that row selected): ✕, "2 selected of
  16", **All**/None, and a bar under a shorter list band: **Remove N**
  (red, wide enough for "Remove 16"; a bigger count drops the icon, never
  the number), **Play next** (the selection after the playing entry, in queue
  order), **Clear...**: a sheet, "Clear up next" (keeps what plays and
  what played) or "Clear queue" (stops; a dialog with a red Clear asks
  first). Every edit has Undo on its toast. A track that failed keeps an
  amber "!" (`KeyRing`, the last 16 entries by key). Empty: "Your queue is
  empty", Open Library, Shuffle all (`queueview::shuffle()`: the whole
  library, a random seed, playing).
- **Output** (spec §6.6, mockups 19-21, with the grafts; a list, so it
  scrolls): the **Bluetooth card** (two rows; `OutputModel`'s view of the
  link and the session: No headphones paired [Pair new headphones], Not
  connected [Connect, Forget], Connecting... try 2 of 3 [Cancel], Looking
  for SPYDRONE... [Cancel] (the back-off, or a connect with none
  remembered), Not connected with "They'll reconnect / when switched on."
  beside [Connect] (resting: no spinner, not amber; red, with "Back in
  range? / Tap Connect.", if they dropped while the output: they rest
  then only after the whole back-off),
  Pairing... [Cancel], Connected "SBC 44.1 kHz, 175 ms" [Disconnect, "...",
  the volume chip], Couldn't connect [Try again, Forget] (a pairing that
  failed: [Try again, Connect], the headphones remembered before; none
  remembered: [Try again, Pair new]), Lost [Cancel]; a spinner while under
  way. Connected, Forget is not beside Disconnect and the volume: "..."
  opens a sheet (Disconnect, Pair new headphones, Forget in red, which
  asks in a dialog). Elsewhere **Forget takes a second tap within 3 s**,
  the button turning red, "Tap again"; a tap on the card makes it the
  output, connecting first), the **speaker card** (its own volume chip;
  "Here until SPYDRONE connects" while they're on their way, "Waits for
  SPYDRONE" when that doesn't fit; a tap moves the output, pausing
  first), the **line-out** row
  (the 3.5 mm / RCA module's place, not fitted yet), **Pair new
  headphones** (the **Pair** page: "Searching" (for 2 min, or until the
  screen goes off, then "Search again", a tap on it), the audio devices found,
  their kind and 4 signal bars, in the order found so no row moves under a
  finger; a tap pairs, after a confirmation when it replaces the
  remembered pair, and goes back to the card), **Haptics** on/off (the
  input layer's saved setting), **Screen off after** and **Brightness**
  (the screen policy's; the value in a pill, a tap takes the next choice,
  saved), **Turn off when idle** (the idle power-off's, the same way: 10 /
  20 / 60 min / Never, a power symbol), **CPU speed** (240 / 160 MHz, a
  chip, "Smoothest lists, dancing" / "Slower lists, saves a little"; a
  restart, asked first) and **Bluetooth power** (Low / Normal /
  High, signal bars), **Touch calibration**, **About** (battery,
  storage, the library, the headphones, the CPU speed and Bluetooth power,
  memory, the version, and "Show the tips again"). The tab bar's Output icon is amber while a connection the
  listener asked for is on its way, and while the link looks for them; the
  plain icon while the search rests (`tabbar::outputFor()`, host-tested).
- **States** (spec §7): no microSD card (and no music on the flash
  fallback): the Library, and the Queue and Now Playing while nothing is
  queued, show "No microSD card" and **Try again**, which looks for a card
  (`LocalStorage::probeCard()`, never inside an LCD hold) and restarts the
  player to use it (the backend, the library and the queue were set up on
  the flash); no automatic re-check (an SD init with no card could hold
  the loop). A card without music: "No music found", Try again walks
  /music again (the `g0` path). A track that can't be played: an amber
  note ("Skipped 07 - x: can't play it", from `PlaybackController`'s
  failure record) and its "!". The headphones lost while playing: paused,
  a long buzz (80 ms), and the dialog of mockup 23 ("SPYDRONE
  disconnected. Paused, so the speaker doesn't suddenly play out loud.";
  a name too long for the title goes into the body; the reconnecting as a
  live line: "Trying to reconnect: try 2 of 3", then "Looking for
  them...", and after the whole back-off "Stopped looking for them: Play
  tries again"), Use speaker (paused: B plays) or OK; from a dark screen
  its first touch only answers the wake (the pocket rule); it closes itself when they're back, and whatever it was
  over (the coach cards too) is drawn again. A play that waited for the
  headphones and failed: a buzz and the dialog "Couldn't reach SPYDRONE"
  (no icon: the title has its whole width; a name too long goes into the
  body), "Are they on, out of the case, and not connected to your
  phone?", Play on speaker (in Small: 4 px too wide for its button in
  Body, which the Dialog now does for any such label) and Try again; it
  closes itself when they connect after all or the speaker becomes the
  output. A Library Play or a Queue Play now that waits says so in its
  note ("Waiting for SPYDRONE: One More Time"). Now Playing with nothing
  queued: "Nothing playing", Open Library, Shuffle all. With nothing
  queued the A/B/C clicks are inert: a short double buzz instead of the
  tap tick (`ButtonPolicy::Transport::idle()`; the input layer plays the
  buttons' feedback once the policy has acted).
- **Dance** (the dancer's box, the beat and the dancer's name around it; no
  tap zone at the bottom, which sat over BtnC).
- **Album covers** (`ui/Thumbs`, with the host-tested `ThumbCache`,
  `ThumbScaler`, `JpegInfo`): a page draws a cover with `get()`: the
  thumbnail, or the placeholder and a request. Where they come from, first
  that works: the **PSRAM LRU** (64 small, 6 large: ~315 KB), the **card's
  copy** (`/.player/thumbs/<h>/<hash>.565`: both sizes of one cover, 21.6 KB,
  8.3 names in 16 subfolders since a FAT folder is searched entry by entry;
  what the next boot finds), then the **cover image**, decoded once into both
  sizes (40 x 40 for the lists, 96 x 96 for Now Playing: TJpgDec shrinks
  by 1/2-1/8 while decoding as far as the larger allows, then a box filter
  on a centred square) and saved as the card's copy. A **progressive JPEG**
  (TJpgDec reads baseline only; one of six covers on the test card) or a
  damaged one keeps the placeholder, is logged once, and gets a
  header-only file so the next boot doesn't try it again. Requests are
  served newest first (the rows on screen when a list stops come before the
  ones it passed; only the last 24 are kept). When one arrives, the Ui has
  the page redraw what shows that album: a row (`refreshRow`), or Now
  Playing's cover. **Why a worker task, not work in the loop's idle time:**
  a decode takes 125-300 ms, and TJpgDec can't stop part-way and carry on
  later (its entropy decoding runs through the file in one call), so the
  loop would stop answering the finger for that long. The worker runs on
  core 1 below the decoder (2), **level with the loop (1) while nothing
  moves** and at **priority 0 the moment a list moves** (a job is never
  started then, but one may be under way), so no frame waits for it. (At
  priority 0 throughout, as first built, it shared what the loop and the
  decoder left with the idle task: on the device, while an MP3 played, a
  200 x 200 cover took 0.6-1.5 s and Discovery's 650 x 565 one 2.6 s;
  level with the loop, 0.36-0.56 s and 1.1 s. Decoding at 1/4 or 1/2 for
  the 96 px size costs more than the spike's 1/8, which skips the IDCT.)
  Its card reads are 4 KB each (while it
  holds the SPI bus or the FAT lock, priority inheritance lifts it, ~3 ms);
  and **no new job starts while a list moves**. **Its RAM:** a 6 KB stack in
  internal RAM (a task that reads the card or the flash can't have its
  stack in PSRAM), only while it lives: it is made for the first job and
  ends after 3 s without one. It drives lgfx_tjpgd itself with its 3.9 KB
  work pool **in PSRAM** (M5GFX's `drawJpg()` mallocs it, in internal RAM:
  with Arduino's File buffers, the spike's 8.6 KB), and reads with POSIX
  calls (no stdio buffer); the JPEG (read whole, up to 2 MB), the scaler's
  sums (~170 KB while decoding) and the job are in PSRAM. `ui` prints the
  cache, the decodes (mean and max ms), failures, the worker's stack
  high-water mark and the lowest internal RAM free during its jobs; each
  decode logs `[thumb] <path>: WxH at 1/n, 40 + 96 px in N ms (read R,
  decode D, card copy W)`; console `uiT` decodes every cover again this
  session (the card's copies are rewritten), for those timings. A cover
  replaced under the same name keeps its old thumbnail until
  `/.player/thumbs` is deleted.
- **The length of a track** for the progress: the decoders don't expose it,
  so the backend reads it when the track opens (`TrackProgress`, host-tested):
  a FLAC file's from its STREAMINFO, a VBR MP3's from the Xing/Info or VBRI
  header in its first frame (after the ID3v2 tag; a 2 KB PSRAM read). Only
  a file without one (a plain constant-bitrate MP3) falls back to the
  estimate from how fast the decoder reads the file since its first audio,
  which is exact for those. (On the device the estimate alone read 7:37 for
  a 5:20 LAME VBR track 15 s in, and 3:30 for a 3:13 FLAC.)
- **Console** `ui`: each tab's stack with scroll positions, the list's state,
  frames and fps, the governor, the UI's bus holds (count, mean, max), the
  overlays, the covers (above) and the loop task's unused stack. `ui0`-`ui4`
  tap a tab, `uib` goes back, `uic` shows the coach cards, `uiT` decodes
  the covers again (their timings), `uiV` shows the volume HUD, and
  **`uiF<c/s/p/r/l/n/w>`** shows a faked state for screenshots of what a test
  can't safely cause (display only: the radio and the card are left
  alone): the Bluetooth card connecting, searching, pairing or resting, the
  headphones lost (with the dialog), no card (on Now Playing), or a play
  waiting for the headphones (Now Playing's panel and spinner); `uiF0`
  the real state. **`uil<n>`**: the Library browses a synthetic
  library of n tracks (the spike's `g<n>`: 6 artists and 15 albums per 100
  tracks), to see the lists, the rail and the jump grid at the scale of
  thousands (the card has 6 artists); look only (its ids aren't the
  player's: its actions are refused, and it has no covers); `uil0` the
  card's again (`g0` or a new `g<n>` re-points it). The screenshot (`X`) reads the list band back through the scroll
  offset, as before. For tests without a hand on the device there is a
  **scripted finger** (`Input::simulate`, replacing the panel until it
  lifts; a real touch cancels it): `uit<x>,<y>` a tap, `uih<x>,<y>` a long
  press, `uis<x0>,<y0>,<x1>,<y1>,<ms>` a swipe that lifts at once (a fling
  when fast), `uid...` the same resting 150 ms before the lift (a drag),
  `uip<x>,<ms>` a press on the button strip (y 260) for ms. A y from 240
  is the strip in all of them, through the same `StripButtons` as a
  finger: `uit160,260` clicks B, `uis160,200,160,264,80` is a swipe that
  ends there and presses nothing, and `uis160,265,160,100,120` is a swipe
  up from the strip that flings the list (`uid...` the same as a drag).
  Every list motion logs `[ui] scroll: <ms>, <frames> (<fps>), draw mean/max
  (n over 35 ms), ring min, underruns +n, governor` when it settles, and a
  frame over 50 ms logs `[ui] slow frame` with its move and its renders; every touch logs
  `[touch] down/tap/long press/fling x,y (raw x,y)` (with `scripted` for
  the scripted finger's), every move of the queue's current entry
  `[queue] now at n of N (state)`, every tab change (`[ui] tab: Queue`) and
  every action that plays something (`[ui] queue: play entry n (its row's
  bar)`, `[ui] now playing: next`, `[ui] library: play 14 tracks`, `[ui]
  shuffle all: 77 tracks`), every queue edit from the Queue (`[ui] queue:
  removed 2`, `... to play next`, `... clear up next`, `... cleared`) and
  every output change (`[output] ...`, `[ui] bluetooth: ...`). `ui` adds
  the Bluetooth link and session and the queue's marks.
- **The queue jump seen once (entry 24 to 2, during a screenshot)**: every
  path that sets the current entry was reviewed. The player moves it only on
  `play(n)` (the console's `i<n>`, the Queue's row-bar Play), prev/next and a
  track's end or failure (always ±1, logged by the audio on a failure),
  `playNow()` (a Library Play: the queue replaced, a toast), a removal of
  the current entry (a toast), undo (logged) and the queue's restore/remap
  (boot, `g0`). A jump to entry 2 with the queue's size unchanged is only
  `play(1)`: the console (it prints `> play 1`; nothing was sent) or **the
  Queue's row-bar Play, which had no toast and no log line**. The same log
  shows the Dance tab opening at 341.7 s, 2.5 s after the jump, with no
  console `d`: only a tap on the Dance tab does that, so fingers were on the
  glass then; and the Queue opened at its remembered top (it had been left
  at "@0px"), where entry 2 is the second row. So the likely cause is three
  touches (the Queue tab, row 2, its Play) that nothing logged at the time.
  Now touches, tab changes and that Play are all logged: a repeat will say.

The UI costs ~200 B of static internal RAM (the font slots' pointer, the
drawing helpers' state, the host adapter; the pages' text buffers, the
Pair screen's device list copy and About's texts are members, in PSRAM
with the Ui); the Queue and Output screens added ~160 B more (the
Bluetooth session and the link's published state, the pairing's name; the
scan list and the learned track lengths are in PSRAM). The boot log's
`[ui] internal RAM ... before the UI, ... after` line has the rest. The covers add ~340 KB of PSRAM (the
LRU and the worker's buffers) and, only while the worker lives, its 6 KB
stack.

## UI spike (browsing UI groundwork)

The browsing UI follows the tab bar design. Its riskiest parts are measured
first by tools that stay in the firmware as diagnostics
([UI-SPIKE.md](UI-SPIKE.md)): an input lab (button and glass-touch timing,
target practice near the bottom edge, haptic ticks), a scroll lab (a
virtualised list flick-scrolled while audio streams from the SD card, with
per-slice SPI bus hold times and the ring fill), a font probe and a thumbnail
probe. The pieces the real UI will keep are portable and host-tested:
`LibraryIndex` (the library as a PSRAM string arena, fixed records and
sorted views, built from the card's folders: now the single store, see above),
`TextFold`, `TouchGesture`, `KineticScroll`
and `ScrollGovernor` (lists back off the SPI bus when the decoder's buffer,
`Core2AudioBackend::bufferedMsNow()`, runs low), and round 2's `VScrollMap`
(the LCD's hardware vertical scroll, `ui/ListScroller` on the Core2: the
user found it "much smoother") and `RefillPacer`. On the Core2, `LcdLock`
times each LCD hold of the bus it shares with the SD card. Round 2's
interaction boost (the decoder lowered below the loop while a list moves)
measured worse and was removed. The scroll lab now runs the
recommended setup by default: the hardware scroll, a 30 fps cap kept on
the frames' deadlines, flings capped at 2,000 px/s, and the A-Z rail hidden
while the list moves (it sits in the scrolled band, and jittered).

## Power measurement

Nothing had been measured on battery (every POC run was on USB), so every
battery-life estimate was a guess. The console's `P` commands (with Enter;
`src/app/PowerLab`, `src/app/PowerProbe`, `lib/core/PowerWindow`) measure
what the Core2 draws and switch one consumer at a time. All of it is off
until a `P` command: then the loop reads the power chip 10 times a second.

**What is read.** The AXP192 through M5Unified's I2C, from the loop task:
reg 0x00 (which supply is there), 0x56-0x5F (ACIN and VBUS voltage, 1.7 mV;
ACIN current, 0.625 mA; VBUS current, 0.375 mA; the chip's temperature,
0.1 C), 0x78-0x7F (battery voltage, 1.1 mV; charge and discharge current,
0.5 mA, 13 bits; APS, the system rail, 1.4 mV). Three I2C transactions,
~0.7 ms per sample. M5Unified already switches every ADC on (reg 0x82 =
0xFF, 0x83 = 0x80) at 25 Hz; the probe checks and says so. Samples go into
5 s windows: mean, min and max. At 160 and 80 MHz the chip's ACIN current
now and then reads 0-15 mA for a single sample on USB: such a sample is
held until the next, and left out (counted as `glitches=N` on the line)
when that one is back up; a drop that lasts counts (`power::Window`,
host-tested). A window a console command starts is timed from that
command's `millis()`, a moment after the loop's: it isn't taken as 2^32 ms
old (`Pl`'s first line said "4294967.5 s: no samples").

`P` prints one line at the end of a 5 s window; `Pl` prints one every 5 s;
`Pw` appends every window to `/.player/power.csv` on the card; `Pm<name>`
marks a change (the window ends there, so the next is all after it). The
line's format (one line on the console; `x` stands for the numbers):

```
[power] 5.0 s n=50 in=x mA (min..max) x W (min..max) [<supplies>: ACIN x V x mA, VBUS x V x mA]
  bat=±x mA (min..max) ±x W x V aps=x V x C [cc=...] | bl=127 (DC3 2875 mV) screen=on cpu=240 MHz
  play=playing out=bt link=<phase> stream=started amp=off exten=off led=0 imu=suspended taps=off dance=hidden bg=<search> radio=N% loop=auto tx=-12..+3 dBm (Normal)
```

`<supplies>` is what reg 0x00 says is present (ACIN, VBUS, both, none);
`cc` appears while the coulomb counter runs; `tx=` is the Bluetooth power
setting's range, or `Pt`'s levels once it set them (`tx=+0..+3 dBm (Pt)`),
until the Bluetooth power row is next tapped (it replaces them).

- **in** is what comes in from USB. Which AXP192 pin USB-C reaches on this
  board isn't assumed: ACIN and VBUS are both read and added (the one
  without a supply reads ~0 mA; with the 5 V boost on, VBUS can show the
  bus's own ~5 V with no current).
- **bat** is + charging, − discharging. With USB in, the Core2 runs from
  USB, so the battery current is the charger's, not the device's.
- **Two ways to measure.** (1) On USB with the battery full (bat ~0: the
  charger has stopped; it restarts once the cell sags, which spoils long
  runs): `in` is the device's draw. (2) On battery alone, the real case:
  serial goes with the cable, so start `Pw` (and `Pq1`) first, unplug,
  and read the CSV off the card afterwards. `Pq1` starts the AXP192's
  coulomb counter (the battery's charge and discharge, integrated in the
  chip: best for overnight runs); P lines then show the net mAh and the
  average mA since.
- Average at least 60 s per condition and alternate A/B/A: the 25 Hz ADC
  against Bluetooth's TX bursts is noisy. The probe's own reads cost
  ~0.1 mA of CPU while it runs.

**At boot** (app/BoardPower, docs/ENERGY.md item 9) the BMI270 is
suspended and the 5 V boost is off (`cfg.output_power = false`); the green
LED is off (M5Unified's default). One line says so: `[power] boot: IMU
suspended, 5 V boost (EXTEN) off, green LED off`.

**The knobs** (each says what it was and what it is now, and is undone by
its opposite; only `Pcb` is saved):

| Command | What it switches | Notes |
|---|---|---|
| `Pb<0-255>` | backlight while the screen is bright (M5GFX: AXP192 DCDC3, 2.5-3.275 V) | the screen policy's Bright level until restart, `Pb0` the setting's (Medium, 100; M5GFX's own default was 127, 2.875 V); refused while dim or off; it still dims and goes off (Screen off after: Never holds it) |
| `Ps` / `Ps0` / `Ps1` | the screen policy: its state / Off now / on | Off is the policy's (backlight off, ILI9342C sleep-in, the UI draws nothing); a touch wakes it and does nothing else (`[screen] wake by touch at x,y (raw)`), and with no input after that it goes off again in 10 s (the pocket guard); off with nothing playing, the Bluetooth search rests after its burst (`setQuiet`) |
| `Pc<mhz>` | CPU clock now | only 160 ↔ 80 (same 320 MHz PLL); 240 ↔ 160/80 retunes the BBPLL the Bluetooth radio runs from, refused. 80 not while audio runs, and back to 160 by itself when it starts |
| `Pcb<mhz>` | CPU clock from boot (saved) | 160 or 240 (`Pcb0` the default, 240), set before Bluetooth starts; the Output tab's CPU speed is the same NVS value |
| `Pt<min>,<max>` | Bluetooth BR/EDR TX power levels 0-7 (−12..+9 dBm) | a test until restart, not saved (the Output tab's Bluetooth power is the setting, default 0,5: −12..+3 dBm; a change there wins); `Pt` alone reports the setting too; from the next page, scan or connection (with a link up the line says "applies from the next connection": the link keeps its level; `Pt` alone reads back the controller's setting, the new range at once, not the link's level) |
| `Pe0` / `Pe1` | the 5 V boost (EXTEN, M-Bus/Grove 5 V) | M5Unified's setExtOutput(); off from boot (`cfg.output_power = false`) |
| `Pg0` / `Pg1` | the green LED | off at boot |
| `Pi0` / `Pi1` | the BMI270 IMU suspended / on | suspended from boot (app/BoardPower); nothing reads it |
| `Pa0` / `Pa1` | the speaker amp (NS4168 enable, AXP192 GPIO2) and M5.Speaker's I2S | by itself it goes off 2 s after the speaker goes quiet; `Pa0` off as soon as it is quiet (no 2 s wait); `Pa1` on (zeros, silent) and held on, through playing and pausing, until `Pa0` (`amp=held`) |
| `Pd<ms>` | the loop's idle delay while nothing animates (1-100) | `Pd0` the UI's own (~5 ms); never while a list moves or the Dance tab is up |
| `Pk0` / `Pk1` | the dance beat tracker (and so the outputs' taps) | the taps are on only while the Dance tab is up and the tracker is on |
| `Pr0` / `Pr1` | the background Bluetooth search: rest now / a burst again | stays connectable; `link=resting`, `bg=resting`; a connect, the Pair screen or a play waiting for the headphones starts a burst |
| `Pz` | plays `tone:silence` next | an hour of zeros: the output runs at its full rate (SBC over Bluetooth), nothing is heard |

`tone:silence` is a built-in track (TrackCatalog) that isn't queued with
the others: only `Pz` plays it.

## Build notes

- **pioarduino** (Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5) instead of the official
  platform, which is stuck on Arduino 2.0.17. On Windows, build from PowerShell
  with `MSYSTEM` unset.
- **IRAM is the scarcest resource** on the original ESP32 (128 KB). Bluetooth
  plus M5Unified overflowed it with the prebuilt Arduino libraries. Two fixes:
  `lib_archive = yes` (pioarduino otherwise links every library object), and
  `tools/iram_diet.py`, which moves the libc functions that the rev-1 PSRAM
  workaround pins in IRAM back to flash (this rev-3 chip doesn't need it).
  About 7 KB of IRAM is left. Adding WiFi will need more: likely pioarduino's
  `custom_sdkconfig` to rebuild the framework without the workaround.

## Roadmap

1. **Sync over WiFi.** mStream exports a manifest and compact index files
   (tracks, albums, artists, strings) for the synced selection; the player
   mirrors files to the SD card under the server's paths, downloads with
   resumable requests, and swaps the index in atomically (the library index,
   its cache and the queue's remap by path are in place). First fill by card
   reader; WiFi for updates. WiFi and Bluetooth don't share the radio well, so
   sync is its own mode.
2. **UI follow-ups**: covers from mStream's thumbnails at sync (the
   device's own decode stays the fallback), track lengths from the sync's
   metadata (the Queue's minutes are learned as tracks play until then);
   a double buzz for inert buttons.
3. **AutoDJ:** mStream precomputes a similar-tracks table (top-K neighbours per
   synced track, from its 1280-d embeddings) that the player walks with
   mStream's session-centroid scoring plus its BPM/key/artist filters.
4. **Server discovery without mDNS** (it doesn't work in Docker installs), then
   the device-code pairing flow.
5. A resampler for 48 kHz on Bluetooth, the RCA/3.5 mm module
   (`cfg.external_speaker.module_rca`), SD card verification, power management.
