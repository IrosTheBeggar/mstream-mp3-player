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
              |  GainRamp  VolumeMath  StreamRestart  HeadsetKeys             |
              |  Declicker  DeclickReader  hal/*                              |
              |  AudioTap  TapReader  BeatTracker  ClickGen  DancePose        |
              |  CrabPose  CrabArt (generated)  DanceSkin                     |
              |  LibraryIndex  LibrarySynth  TextFold  TouchGesture           |
              |  KineticScroll  ScrollGovernor  VScrollMap  RefillPacer       |
              |  ByteStream  QueueModel  QueueText  TrackCatalog              |
              |  TouchCalibration  TouchRecognizer  ButtonGesture             |
              |  ButtonPolicy  InputEvent                                     |
              |  NavModel  FrameClock  ListLayout  BitSet  TextFit            |
              |  TabBarModel  TrackProgress                                   |
              +------------------------------+--------------------------------+
                                             |
              +------------------------------+--------------------------------+
  src/        |  audio/  Core2AudioBackend (decode task), RingOutput,         |  Arduino-ESP32 3.x
  (Core2)     |          BtSink (ESP32-A2DP source), SpeakerSink (M5.Speaker) |  (pioarduino),
              |  storage/LocalStorage   app/SerialConsole   app/Library        |  M5Unified/M5GFX,
              |  app/QueueStore   ui/Input   ui/CalibrationScreen             |
              |  ui/Ui: TabBar, ListView (ui/ListScroller), Overlays, pages,  |
              |         Fonts (VLW DejaVu), Icons, Gfx   ui/BootScreen        |
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
- **Requests are generations.** `play()`/`stop()` post a new generation to
  `TransportSync`; the decode task's progress reports for anything older are
  dropped, so a stale "ended" can't skip the track that was just requested.
- **Bluetooth is 44.1 kHz only.** ESP-IDF's SBC source takes nothing else, so
  other rates fail on Bluetooth (the player skips them) until a resampler lands.
  The speaker takes any rate: `RingOutput` keeps the rate in an `int` because
  ESP8266Audio's base class stores it in a `uint16_t`.

## Tasks and cores

| Task | Core | Priority | Notes |
|---|---|---|---|
| Bluetooth controller + host (Bluedroid) | 0 | high | ~70 KB internal RAM, claimed at boot |
| A2DP data callback | 0 (Bluedroid's BTC task, BTC_TASK) | high | 128 frames at a time, several per ~30 ms tick; applies the volume ramp; never blocks or logs. ESP-IDF 5.5's A2DP source has no media task of its own: this is the task that also runs the GAP and AVRCP callbacks, which queue their events to BtAppT (below). If BtAppT's queue (20 entries) is full, each such event blocks BTC_TASK, and the audio, for up to 10 ms |
| ESP32-A2DP app task (BtAppT) | 0 | 15 | connection, stream and AVRCP handlers (`PlayerA2dp`); 6 KB stack; blocks 10 s at stack-up; must keep its queue drained (no long work in a handler) |
| decode | 1 | 2 | 16 KB stack in internal RAM (flash reads can't use a PSRAM stack); after a track start, once 500 ms are buffered, it sleeps after each pass so it refills at most 1.5x realtime (`RefillPacer`, on by default: it halved the UI's stall at every start) |
| speaker pump | 1 | 3 | three 1024-frame buffers, release-callback handshake |
| M5.Speaker | 1 | 2 | mixes/resamples to 44.1 kHz mono |
| Arduino loop (UI, console, input) | 1 | 1 | the input layer every pass (touch panel over I2C, the buttons); the UI (the one task that draws): what changed, and list frames at up to 30 fps on deadlines, each piece under its own short bus hold; on the Dance tab, the beat tracker and ~30 dancer frames/s; sleeps 1-5 ms every pass (less while a list frame is due) |

## Bluetooth

`BtSink` connects to the first headphones whose name contains the configured
name (`BT_SINK_NAME` build flag, or the console's `c<name>`, saved in NVS).
Without a name it only accepts a device practically touching the Core2;
signal strength alone once picked a TV in the next room. It remembers the
device it connected to: after a boot or a drop it tries it for ~30 s, then
scans again, and all along it stays connectable (never discoverable) so the
headphones can reconnect by themselves; other devices are refused. Scanning
that finds nothing for a minute goes back to trying the remembered device, and
a failed connection (also a first pairing) always leads to a retry or a new
scan, never to a dead end (`ReconnectPlanner`, host-tested against a model of
the library's reconnect logic). On connect the output switches to Bluetooth; a
real disconnect pauses playback.

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
  does nothing). Next and previous skip while playing; paused or stopped
  they only select the next or previous track, which the screen shows and a
  later play starts from its beginning. The Core2's own buttons and the
  console keep toggling and skip-and-play.
- **On-device checks** the host tests can't cover (the glue in `BtSink.cpp`:
  event and address filters, what the library does between our hooks): boot
  with the headphones in their case, then take them out (they reconnect by
  themselves); switch them off for 2 minutes and back on (paged again after
  the minute of scanning); fresh NVS with pairing mode left mid-connect (a
  new scan follows, no reboot needed); `f` then reboot (scans); while
  playing, caps arriving late must log `came up after playback started:
  dipping`, the music must go silent for about a second and then fade
  back in over ~2 s with `control=headphones` in the stats (or `no answer`
  and the software level), never jumping up; their buttons must then change
  the volume shown; pause from the headphones then play from them within
  3 s; while paused, next on the headphones shows the next track and stays
  paused, and their play then starts it.
- **Diagnostics.** Once per connection: the SBC configuration, the delay
  report, the headphones' AVRCP features and notifications, and how long a
  stream took to start. The `s` stats add a `[stats] bt` line: volume and who
  applies it, gain, headroom, stream state, longest gap between data
  callbacks, dropped events, BtAppT stack left.

## Dancing crab (proof of concept)

Each output copies what it plays into its own `AudioTap` (PSRAM, written only
by that output's task: the Bluetooth data callback before its gain stage,
the speaker pump after its read). The loop task feeds that to a
`BeatTracker` and draws the dancer for the frame being heard (the tap's
clock minus the output's latency). The dancer is a skin (`DanceSkin`): a
pixel-art crab by default (`CrabPose` maps the beat phase to layer frames
and offsets; its art, `CrabArt`, is generated by `tools/crab_art.py` from
`tools/art/crab.json` into flash, and `DanceView` blits it at 3x into an
RGB565 sprite in PSRAM), or the first stick figure (`DancePose`, an 8-bit
sprite). Console `m` or a tap on the dancer swaps them. Design, commands and
results: [MASCOT-POC.md](MASCOT-POC.md).

## Storage

`LocalStorage` mounts the SD card (shared SPI bus with the LCD, 25 MHz) if one
is present, otherwise the ~11.9 MB LittleFS partition. It walks `/music` for
the library (up to 8 folders deep, no cap, hidden names skipped) through the
VFS's `readdir`, which names each entry and says whether it's a folder, rather
than Arduino's `File::openNextFile()`, which opens every entry and so searches
its directory again for each file (the UI spike measured that walk at ~5.7 ms a
file). The player's own files live in `/.player` on the same volume:
`library.idx` (the index's cache) and `queue.txt`.

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
  stops. The edits that touch what plays go through it: Play starts the new
  queue; removing the current entry plays the next one that stayed (paused: it
  is cued; none left after it: stop); Clear stops; undo returns to the entry
  that was current if the one playing isn't in the restored queue. Play next,
  + Queue and Clear up next change nothing that plays. `HeadsetKeys` works
  unchanged on top (`cueNext()`/`cuePrev()` move the current entry).
- **Persistence** (`app/QueueStore`, `QueueText`): the queue is saved as paths,
  one a line (`queue.txt`, header `mstream-queue 1 <entries> <current>
  <generation>`), so a rebuilt library, whose ids differ, finds its tracks
  again; paths that are gone are dropped, and if the current one is among them
  the next one that stayed is current. The file is rewritten 2 s after the
  last edit, 32 lines a loop pass (a 10,000-track queue is ~700 KB and never
  holds the loop), into `queue.tmp`, then renamed over `queue.txt`. The
  position goes to NVS (at most once a second), tagged with the file's
  generation, so a track change doesn't rewrite the file and a position is
  never paired with an older file. After a restart the queue is where it
  was, stopped. `g0` carries the queue across the rebuild the same way, in a
  PSRAM buffer: the track that plays keeps playing if it's still there. A
  rebuild that leaves no library (out of PSRAM, or a card that went away)
  isn't taken as the queue changing: what survives stays in memory,
  playback stops if its track is gone, and `queue.txt` isn't rewritten.
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

One input layer (`ui/Input`) reads the touch panel and M5Unified's BtnA/B/C;
every screen gets **events** from it and nothing else reads the hardware
(the input lab, a measuring tool, is the one exception: while it is open the
layer is suspended).

- **Touch correction** (`TouchCalibration`, host-tested). The user's Core2
  reads x too far right, more so further right: ~0 at x 60-150, ~+20 px at
  x 190, +35-45 px from x 240, and it stops at 319 (thumb and index finger
  alike: the sensor). Every touch point goes through a monotonic
  piecewise-linear table per axis (x knots every 40 px, y every 60; y is the
  identity for now) before anything hit tests it. The default x table is
  the least-squares fit (smoothed, slopes kept between 0.25 and 4) of the
  input lab's 72 target-practice taps; the host test fits the same logs and
  checks the default still matches. A reading clamped at 319 can't say how
  far out the finger was: the table puts it at ~282, where the fingers that
  read 319 were aimed on average. So **a control at the right edge must have
  a hit area that reaches the screen's edge and is at least ~40 px wide**;
  events also flag a clamped reading (`InputEvent::atRightEdge()`,
  `inRightEdgeZone(left)`). The table is saved in NVS (namespace `input`,
  key `cal`), checked by a checksum when loaded; without it, the default.
- **Calibration screen** (`ui/CalibrationScreen`, console `a`): 5-9
  crosshairs with distinct x and y each; a tap on each (raw reading, where
  the finger landed) is a sample, one too far from the cross is asked
  again. The same fit makes a new table, shown with the error before and
  after; Save stores it, then a check page shows where each tap now lands.
  It is made in PSRAM on first use, owns the screen while up (like the spike
  screens), and opens from the Output tab ("Touch calibration") or the console.
- **The glass** (`TouchRecognizer`, host-tested): Down, Tap (within 12 px
  and 500 ms; its position is where the finger landed), LongPress (500 ms,
  while still down), Release, DragStart / DragMove / DragEnd, and Fling.
  The release velocity (over the last 60 ms) is **capped at 2,000 px/s**,
  and `KineticScroll` caps flings at the same speed: faster, nearly every
  frame of the hardware scroll moves more than its 84-line step and becomes
  a full redraw. A touch that lands on the button strip (raw y >= 240) is
  the buttons' and makes no glass events.
- **The buttons** (`ButtonGesture`, `ButtonPolicy`, host-tested): Click,
  Hold at **500 ms** (the user's clicks lasted 17-143 ms, holds 509-2383 ms),
  Repeat every 200 ms for A and C, HoldEnd. The same on every screen: A
  click previous / hold volume -5 % (repeating); B click play/pause / hold
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
  `ar0`; NVS keys `haptics`, `railtick`). `Haptics` plays patterns from a
  FreeRTOS timer, so a tick lasts its length whatever the loop does.

The layer costs ~660 B of internal RAM (an 8-event queue, the tables, the
recognisers); the calibration screen is in PSRAM.

## UI

The tab bar design (the design review's winner, with its grafts and the
user's measurements), built as a framework the screens sit on:
`ui/Ui` owns the display, `ui/TabBar`, `ui/ListView` and `ui/Overlays` are
its widgets, and five first pages exercise them (the real screens come
next).

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
  in PSRAM (~340 KB: six 320x42 row sprites, a 320x56 strip, a 320x168 panel
  for sheets and dialogs, the rail); the fonts' glyph tables too.
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
  Bluetooth state (cyan connected, amber connecting, red lost). The Library
  icon is a record half out of its sleeve, unlike Now Playing's bars. Only
  the cells whose state changed are drawn (a 54x36 push, ~1 ms).
- **Overlays**: the **toast** (one line, with Undo for 4 s after a queue
  edit, else 1.8 s) sits at the **top of the content area** (y 36-71, over
  the page header: the usability walk found the spec's bottom toast right
  above BtnC); the pages' drawing leaves those rows alone while it's up
  (`gfx::setCover`), and the header is redrawn when it goes. The **volume
  HUD** covers the tab bar for 1.5 s after an A/C hold or the headphones'
  volume keys, with or without absolute volume (20 blocks and the value; a
  change within 2 s of connecting is the link-up cap, not shown; a B hold
  says where the output went and whether it paused); a tap on it still switches the tab. A **sheet**
  (from the bottom, never above y 72) and a **dialog** (modal, the tab bar
  still works) freeze the page under them; one that opens while a finger is
  on the page (the headphones' drop dialog) ends that touch for the page (a
  Cancel), so its lift can't act or draw under it. All are opaque and drawn
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
  then sliding still scrubs. A tap there only stops the list: the spec's
  tap is the jump grid (not built yet), and a tap meant for a row's right
  end that reads at the panel's clamp no longer jumps the list. A press
  that only touches the list (a tap) doesn't hide it.
- **Text** (`ui/Fonts`, `TextFit`, host-tested): DejaVu Sans 16 and 13 and
  DejaVu Sans Bold 16 and 22 as anti-aliased VLW fonts in flash (~170 KB;
  `tools/vlw_font.py`), which cover ASCII, Latin-1, the common Latin
  Extended-A letters and the typographic punctuation. A character without a
  glyph is folded by `TextFold` (e.g. "Ł" to "L"); a name too wide is cut
  with "…". efont is out of the build (the font probe keeps it behind
  `UI_SPIKE_EFONT`). Licence: `LICENSES/DejaVu-Fonts.txt`.
- **Icons**: 1-bit bitmaps in flash (`tools/ui_icons.py` -> `IconData.cpp`).
- **The pages** for now: Now Playing (title, 40 px artist and album bands
  that open them in the Library at the playing track or its album,
  progress, transport, a "..." sheet); the Library (artists A-Z, an artist,
  an album's or an artist's tracks, with Play / Play next / + Queue and Undo
  toasts; two levels down an "Artists" pill in the header is the root crumb);
  the Queue (opened on the playing track, and there again with no row open
  when the queue changed while you were away; a row's bar Play / Play next /
  Remove; a tap on the title goes to the playing track, the top, the end in
  turn; selection mode with Remove in the header, away from the touch
  buttons); Dance (the dancer's
  box, the beat and the dancer's name around it; no tap zone at the bottom,
  which sat over BtnC); Output (Bluetooth or the speaker, the volume, the
  touch calibration, Forget with a confirmation). A Bluetooth drop while
  playing pauses and opens a dialog ("Use speaker" / OK) that closes itself
  when they come back.
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
  overlays and the loop task's unused stack. `ui0`-`ui4` tap a tab, `uib` goes
  back. The screenshot (`X`) reads the list band back through the scroll
  offset, as before. For tests without a hand on the device there is a
  **scripted finger** (`Input::simulate`, replacing the panel until it
  lifts; a real touch cancels it): `uit<x>,<y>` a tap, `uih<x>,<y>` a long
  press, `uis<x0>,<y0>,<x1>,<y1>,<ms>` a swipe that lifts at once (a fling
  when fast), `uid...` the same resting 150 ms before the lift (a drag).
  Every list motion logs `[ui] scroll: <ms>, <frames> (<fps>), draw mean/max,
  ring min, underruns +n, governor` when it settles; every touch logs
  `[touch] down/tap/long press/fling x,y (raw x,y)` (with `scripted` for
  the scripted finger's), and every move of the queue's current entry
  `[queue] now at n of N (state)`.

The UI costs ~200 B of static internal RAM (the font slots' pointer, the
drawing helpers' state, the host adapter); the boot log's `[ui] internal RAM
... before the UI, ... after` line has the rest.

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
2. **Browsing screens** on the UI framework (above): the Library's albums
   and folders segments with the A-Z jump grid, covers (92 px, from mStream's
   thumbnails), the full Now Playing, Queue and Output screens (pairing, the
   volume sheet), the first-boot coach cards; A-click restarting a track
   after 3 s, a double buzz for inert buttons; resume within a track after
   power-off.
3. **AutoDJ:** mStream precomputes a similar-tracks table (top-K neighbours per
   synced track, from its 1280-d embeddings) that the player walks with
   mStream's session-centroid scoring plus its BPM/key/artist filters.
4. **Server discovery without mDNS** (it doesn't work in Docker installs), then
   the device-code pairing flow.
5. A resampler for 48 kHz on Bluetooth, the RCA/3.5 mm module
   (`cfg.external_speaker.module_rca`), SD card verification, power management.
