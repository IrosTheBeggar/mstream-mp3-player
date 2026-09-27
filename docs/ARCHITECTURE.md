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
              |  TabBarModel  TrackProgress  JumpIndex                        |
              |  ThumbCache  ThumbScaler  JpegInfo                            |
              |  OutputModel  PlayGate  QueueView                             |
              +------------------------------+--------------------------------+
                                             |
              +------------------------------+--------------------------------+
  src/        |  audio/  Core2AudioBackend (decode task), RingOutput,         |  Arduino-ESP32 3.x
  (Core2)     |          BtSink (ESP32-A2DP source), SpeakerSink (M5.Speaker) |  (pioarduino),
              |  storage/LocalStorage   app/SerialConsole   app/Library        |  M5Unified/M5GFX,
              |  app/QueueStore   ui/Input   ui/CalibrationScreen             |
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
| cover thumbnails (`thumbs`, ui/Thumbs) | 1 | 1, or 0 while a list moves | only while there are covers to make: made for the first, gone after 3 s without one; 6 KB internal stack while it lives (2.3 KB used at most on the device); reads the card in 4 KB pieces; level with the loop while nothing moves (at 0 it shared what was left with the idle task: 2-2.5x slower), below it the moment a list moves, always below the decoder (below) |
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
- **The Output screen** (`OutputModel`, host-tested for its decisions;
  `BtSink` carries them out on BtAppT, the loop only posts asks, retried
  in order when BtAppT's queue is full). `BtSink::link()` publishes what
  the link is doing: Off, Paging (the try and of how many: `connect_to()`
  is overridden to count the library's retries and ours), Scanning,
  Linked, PairScan, Pairing, and whether a device is remembered. The
  listener's asks: **connect()** pages the remembered headphones at once
  (then the library's retries; with none remembered, a scan by name);
  **disconnect()** lets go (or stops a page) and stops trying: no paging,
  no scanning, not connectable, and the headphones coming back by
  themselves are refused, until the next connect(); **startPairScan()**
  runs inquiry rounds back to back and lists every audio device (class of
  device: the rendering service or the Audio/Video major class; the name
  from the result or its EIR) into a `BtScanList` in PSRAM, under a
  spinlock, connecting to none (the library would connect to the first
  that matches), while the background cycle is held off; **pairWith()**
  lets go of the link that is up first, then pages the picked device with
  the usual tries: it becomes the remembered one (NVS) only once linked,
  and the sink name becomes its name (so a later scan by name finds it);
  a pairing that fails puts the old address back in RAM (NVS still has
  it) and stops there; a Cancel while the old link is still being let go
  ends the pairing the same way. The Pair list leaves out the headphones
  linked now (multipoint sets stay discoverable; "pairing" with them only
  let them go), and picking them anyway just makes them the output.
  Forget from the screen forgets and disconnects, no restart, and **for
  good**: a saved flag (NVS `bt_forgot`) stops every scan by name (the
  boot's, the background cycle's, a B hold's), so the forgotten headphones
  can't come back by their name; the next pairing clears it (the
  console's `f` still restarts and scans by name). `BtSession` (main.cpp's)
  holds what the listener asked: **the audio stays on its output until the
  headphones they asked for are linked** (the card, Connect, the B hold
  only connect; the Connected event moves the audio, as it always did,
  and when it was asked for, a toast "Now playing on SPYDRONE" and two
  ticks say so), a connection whose tries run out (the link goes on to
  Scanning or Off) is Failed, and so is a connect with none remembered (a
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
- **Play while the headphones aren't connected** (`PlayGate`, host-tested
  with the player, `BtSession` and `ButtonPolicy` in `test_play_gate`). The
  bug it fixes: the headphones dropped overnight while idle, the output
  stayed Bluetooth with the background cycle scanning, and play went to
  "Playing" at 0:00, "connecting...", for good (the decoder filled the
  ring, the stream stayed suspended; no explanation, timeout or way out).
  Now play from anywhere (the button, B, the Library's Play, the Queue's
  Play now, a resume, a skip) while Bluetooth is the output and the link
  is down makes the player **wait** (`PlayState::Waiting`: nothing starts,
  the position holds) and `PlayGate` (fed every loop pass after
  `BtSession`) **connects at once**: `BtSession::connect()` and
  `BtSink::connect()`, the paging burst ("try 1 of 3"), not the background
  cycle's next round. A background page still on its way (within the
  page timeout, `kPageMs`) isn't paged over: it counts as try 1 of a full
  burst (the retries restarted), so a wait that begins at the background
  burst's last try doesn't fail with it. While the Pair screen has the
  radio (its scan, or a pairing) only the session is asked (a pairing's
  own session is kept): the scan isn't stopped for a B click. The "lost"
  mark goes (the tab bar and the card say Connecting). The link comes up:
  the wait is released and plays on them, "Now playing on SPYDRONE"
  (said once: the Connected event's own toast is skipped while the gate
  waits). The tries run out (`BtSession::failed()`) or 20 s pass
  (`kBackstopMs`, the listener's number: the burst itself takes 20-30 s, a
  try at once and then one each ~10 s heartbeat, so this usually ends the
  wait during the third try): the wait ends **paused**, and a notice says
  "Couldn't reach SPYDRONE. Are they on, out of the case, and not
  connected to your phone?" with Try again (a new wait and burst) and
  Play on speaker. The session's ask is **withdrawn** as failed
  (`BtSession::withdraw(true)`: no disconnect), so the card ("Couldn't
  connect") and the tab bar turn red with the notice while the tries left
  and the background cycle go on quietly; a link they bring later closes
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
  test mode) ends a wait paused. Headphones dropping while idle stay
  quiet (no dialog), but the Output tab shows them lost and Now Playing's
  output line says "SPYDRONE (not connected)". The name shown is the
  headphones' own (read at each link), or before any link the name
  looked for.
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
  stops. The edits that touch what plays go through it: Play starts the new
  queue; removing the current entry plays the next one that stayed (paused: it
  is cued; none left after it: stop); Clear stops; undo returns to the entry
  that was current if the one playing isn't in the restored queue. Play next,
  + Queue and Clear up next change nothing that plays. `HeadsetKeys` works
  unchanged on top (`cueNext()`/`cuePrev()` move the current entry). A
  `Hold` (main.cpp's: Bluetooth is the output and the headphones aren't
  connected) turns every play into **Waiting**, a state of its own (not
  Playing): a new track is only selected (the backend drops what it had), a
  resume leaves the paused track where it is, skips stay waiting on the new
  track, play/pause cancels the wait (Paused); `release()` plays what waits,
  `cancelWait()` ends it paused (`PlayGate` decides which: see Bluetooth).
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
  `ar0`, and the haptics also on the Output tab; NVS keys `haptics`,
  `railtick`). `Haptics` plays patterns from a
  FreeRTOS timer, so a tick lasts its length whatever the loop does.

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
  Bluetooth state (cyan connected, amber connecting, red lost or a
  connection that failed); the EQ bars lie flat in amber while a play
  waits for the headphones. The Library
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
  (from the bottom, never above y 72) and a **dialog** (modal, the tab bar
  still works; optionally an icon, a live status line and a red primary
  button) freeze the page under them; one that opens while a finger is
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
  choice, not the way on: neither is the accent). The
  artist opens the Library at that artist, the album at the album (one Back
  from its artist), each **scrolled to the playing item and tinted**; "..."
  is a sheet of Go to artist, Go to album and Show in folders (each with
  its name, dim, on the right), the last opening the chain of folders down
  to the track's, one Back apart, the playing file tinted. The volume
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
  for SPYDRONE... [Cancel] (also a connect with none remembered),
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
  headphones** (the **Pair** page: "Searching", the audio devices found,
  their kind and 4 signal bars, in the order found so no row moves under a
  finger; a tap pairs, after a confirmation when it replaces the
  remembered pair, and goes back to the card), **Haptics** on/off (the
  input layer's saved setting), **Touch calibration**, **About** (battery,
  storage, the library, the headphones, memory, the version, and "Show the
  tips again"). The tab bar's Output icon is amber while a connection the
  listener asked for is on its way.
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
  live line: "Trying to reconnect: try 2 of 3"), Use speaker (paused: B
  plays) or OK; it closes itself when they're back, and whatever it was
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
  **`uiF<c/s/p/l/n/w>`** shows a faked state for screenshots of what a test
  can't safely cause (display only: the radio and the card are left
  alone): the Bluetooth card connecting, searching or pairing, the
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
  when fast), `uid...` the same resting 150 ms before the lift (a drag).
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
   A-click restarting a track after 3 s, a double buzz for inert buttons;
   resume within a track after power-off.
3. **AutoDJ:** mStream precomputes a similar-tracks table (top-K neighbours per
   synced track, from its 1280-d embeddings) that the player walks with
   mStream's session-centroid scoring plus its BPM/key/artist filters.
4. **Server discovery without mDNS** (it doesn't work in Docker installs), then
   the device-code pairing flow.
5. A resampler for 48 kHz on Bluetooth, the RCA/3.5 mm module
   (`cfg.external_speaker.module_rca`), SD card verification, power management.
