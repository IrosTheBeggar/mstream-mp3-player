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
              |  Declicker  DeclickReader  Track  hal/*                       |
              |  AudioTap  TapReader  BeatTracker  ClickGen  DancePose        |
              |  CrabPose  CrabArt (generated)  DanceSkin                     |
              |  LibraryIndex  LibrarySynth  TextFold  TouchGesture           |
              |  KineticScroll  ScrollGovernor                                |
              +------------------------------+--------------------------------+
                                             |
              +------------------------------+--------------------------------+
  src/        |  audio/  Core2AudioBackend (decode task), RingOutput,         |  Arduino-ESP32 3.x
  (Core2)     |          BtSink (ESP32-A2DP source), SpeakerSink (M5.Speaker) |  (pioarduino),
              |  storage/LocalStorage   ui/DisplayView   app/SerialConsole    |  M5Unified/M5GFX,
              |  main.cpp: buttons, Bluetooth events, rendering               |  ESP8266Audio
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
| decode | 1 | 2 | 16 KB stack in internal RAM (flash reads can't use a PSRAM stack) |
| speaker pump | 1 | 3 | three 1024-frame buffers, release-callback handshake |
| M5.Speaker | 1 | 2 | mixes/resamples to 44.1 kHz mono |
| Arduino loop (UI, console, buttons) | 1 | 1 | redraws at 4 Hz; on the dance screen, the beat tracker and ~30 dancer frames/s (each push holds the SPI bus for that push only), `delay(5)` every pass |

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
is present, otherwise the ~11.9 MB LittleFS partition, and lists the `.mp3` and
`.flac` files under `/music` (recursive, sorted, capped at 200). The planned
library index and sync from mStream replace this scan (see Roadmap).

## UI spike (browsing UI groundwork)

The browsing UI follows the tab bar design. Its riskiest parts are measured
first by tools that stay in the firmware as diagnostics
([UI-SPIKE.md](UI-SPIKE.md)): an input lab (button and glass-touch timing,
target practice near the bottom edge, haptic ticks), a scroll lab (a
virtualised list flick-scrolled while audio streams from the SD card, with
per-slice SPI bus hold times and the ring fill), a font probe and a thumbnail
probe. The pieces the real UI will keep are portable and host-tested:
`LibraryIndex` (the library as a PSRAM string arena, fixed records and
sorted views, built from the card's folders; the future single store, which
the playlist doesn't use yet), `TextFold`, `TouchGesture`, `KineticScroll`
and `ScrollGovernor` (lists back off the SPI bus when the decoder's buffer,
`Core2AudioBackend::bufferedMsNow()`, runs low). On the Core2, `LcdLock`
times each LCD hold of the bus it shares with the SD card, and `Haptics`
plays vibration patterns from a FreeRTOS timer.

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

1. **Library index + sync over WiFi.** mStream exports a manifest and compact
   index files (tracks, albums, artists, strings) for the synced selection; the
   player mirrors files to the SD card under the server's paths, downloads with
   resumable requests, and swaps the index in atomically. First fill by card
   reader; WiFi for updates. WiFi and Bluetooth don't share the radio well, so
   sync is its own mode.
2. **Browsing UI:** artists, albums (92 px covers from mStream's thumbnails),
   playlists, queue, resume after power-off.
3. **AutoDJ:** mStream precomputes a similar-tracks table (top-K neighbours per
   synced track, from its 1280-d embeddings) that the player walks with
   mStream's session-centroid scoring plus its BPM/key/artist filters.
4. **Server discovery without mDNS** (it doesn't work in Docker installs), then
   the device-code pairing flow.
5. A resampler for 48 kHz on Bluetooth, the RCA/3.5 mm module
   (`cfg.external_speaker.module_rca`), SD card verification, power management.
