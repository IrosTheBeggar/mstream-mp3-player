# Proof-of-concept results

Measured on an M5Stack Core2 v1.3 (ESP32-D0WDQ6-V3 rev 3.1, AXP192, BMI270)
running on USB power, with Beats Powerbeats Pro as the Bluetooth headphones,
September 2026. Firmware at the commits that close M1–M3 plus the M4 docs.

## Verdict

The concept works on this hardware. MP3 and FLAC decode comfortably faster than
realtime on the original ESP32, stream to Bluetooth headphones without
dropouts, and leave enough internal RAM with the Bluetooth stack connected.
The binding constraint is not CPU or RAM but **IRAM**, which will need attention
before WiFi sync is added.

## Numbers

**Decoding** (`b<n>` console bench: decode as fast as possible, output
discarded, 20 s of audio or the whole file):

| File | Speed | Share of one core |
|---|---|---|
| MP3, real music, 244 kbps, 44.1 kHz | 4.9× realtime | 21% |
| MP3, test tone, 128 kbps | 5.1–5.5× | 18–20% |
| FLAC, real music, 16-bit/44.1 kHz (~985 kbps) | 4.1× | 24% |
| FLAC, test tone, 16-bit/44.1 kHz | 9.4× | 11% |
| FLAC, 24-bit/96 kHz | 2.7× | 37% |

During playback the whole producing path (decode plus ring writes) runs at
about 30% of a core for MP3 and 24% for FLAC. The decode task never used more
than ~2.7 KB of its 16 KB stack for either codec.

**Bluetooth streaming** (ESP32-A2DP source to Powerbeats Pro):

- 10-minute soak of test tones: **0 underruns**, pull rate 43,130–44,921
  frames/s (average 44,097), no disconnects or resets.
- MP3 and FLAC excerpts: 0 underruns, buffer steady at ~1.45 s of 1.49 s.
- 60-minute soak looping the library with a skip every 5 minutes and a pause
  every 7: see [the soak section](#60-minute-soak).

**Memory** (internal RAM free; PSRAM has ~3.5 MB free throughout):

| Stage | Free | Notes |
|---|---|---|
| Boot | 199 KB | |
| Bluetooth stack started | 102 KB | the stack claims ~70 KB at boot |
| Bluetooth connected, idle | 86–90 KB | |
| FLAC to Bluetooth | 75–78 KB | libFLAC's buffers (~240 KB) land in PSRAM |
| MP3 to Bluetooth | 66 KB (lowest 61 KB) | |

The target was at least 50 KB with Bluetooth, decoding and the UI running.

**Robustness:**

- 50 rapid commands (skip, back, pause, jump, output switch): 0 crashes, every
  command acknowledged, internal RAM 66 KB → 65 KB (no leak).
- Tracks that can't play are skipped: FLAC before it was supported, 48 kHz MP3
  and 24-bit/96 kHz FLAC on Bluetooth. A playlist where everything fails stops
  after one pass (unit-tested).

**By ear** (checked by the user): clean tones on the speaker and on the
headphones; the left-only tone plays in the left ear only. Music "sounds pretty
good", but with **mild distortion in louder sections, in both the MP3 and the
FLAC**. FLAC decodes losslessly, so the cause is most likely after decoding, in
the Bluetooth path; this is under investigation.

**Build:** app 1.62 MB of the 4 MB slot; IRAM 124 KB of 128 KB (7 KB free).

## 60-minute soak

Bluetooth playback of the whole library on loop (tones, test MP3/FLAC files,
real MP3 and FLAC excerpts, plus the 48 kHz and 96 kHz files that Bluetooth
must refuse), with a skip every 5 minutes and a short pause every 7:

- **0 new underruns** and 0 resets over 120 track changes; the Bluetooth pull
  rate stayed at 43,002–45,056 frames/s (average 44,060); every 48 kHz and
  96 kHz track (16 of each) was refused and skipped.
- **It found a memory leak.** Internal RAM fell from 65 KB to **6 KB** free and
  PSRAM from 3.6 MB to 1.1 MB: minutes from a crash. Cause: when a FLAC file
  plays to its end, ESP8266Audio's `AudioGeneratorFLAC` marks itself stopped
  but only its `stop()` deletes the libFLAC decoder, and `closeDecoder()`
  skipped `stop()` for a decoder that wasn't running. Each natural FLAC ending
  leaked ~100 KB of PSRAM and ~2.5 KB of internal RAM. Fixed by always calling
  `stop()`; eight natural FLAC endings in a row afterwards left memory flat.
  The short tests couldn't have caught this: only a long run shows it.

## The loud-passage distortion (review, September 2026)

The user heard mild distortion in loud sections of both the MP3 and the FLAC. A
code review with offline simulation (the ESP-IDF SBC encoder built on a PC and
fed our exact stream) found **nothing in our own chain that clips at the
default 30% volume**, and no single proven cause. What it did establish:

- **Our volume design double-attenuates.** ESP32-A2DP scales the PCM (−27.5 dB
  at 30%) and also sends absolute volume to the headphones when the volume
  changes while connected. That pushes the user to turn the headphones up, and
  the headphones' volume buttons then silently move the software gain toward
  0 dB (up to a 20 dB jump, no ramp) while our screen still says 30%.
- **SBC at bitpool 53** (the prebuilt stack's fixed setting, same as stock
  Android) loses the top octave on bright, dense material: simulated SNR is
  28.6 dB on the FLAC but 56 dB on the MP3, which has little high-frequency
  content. So SBC can't explain the MP3.
- **The FLAC master is itself clipped:** 205 full-scale samples in 30 s, true
  peak +1.1 dBTP. The player passes it through bit-exactly.
- **Both tracks are bass-heavy** (55–68% of the FLAC's energy is below
  120 Hz). The headphones' own DSP or drivers at high volume are a plausible
  cause the player can't fix.
- **libmad is built in low precision** (FPM_DEFAULT: 54.8 dB SNR vs 80.9 dB
  with `-DFPM_64BIT`), but end to end that is worth only about 1 dB.

Next: fix the volume design (fixed −2 dB headroom, headphone volume via AVRCP
only) and run the A/B listening tests to separate SBC, the master and the
headphones.

### After the fixes

- **First listen (software volume only):** "sounds a lot better", but far too
  quiet even with the headphones at max. The Powerbeats bring up AVRCP after
  the first stream has started, so the volume never reached them: the Core2
  applied −30 dB and their rocker only moved their own amp. No volume
  notifications or keys reached the Core2 in that state.
- **Second listen (late handover, e944373):** AVRCP came up 3.7 s after the
  link, 1.1 s into playback. The Core2 dipped to silence, the headphones
  accepted absolute volume 38/127 (30%) 1.0 s later, and our gain rose from
  silence to −2 dB in about 2 s. The rocker then moved the volume in 6–7%
  steps (up to 74%, 94/127), their pause key worked, and there were 0 underruns
  in 16 minutes. The user was happy with it. The top of the volume range on
  loud material hasn't been checked specifically; `h<n>` can raise the
  headroom by ear if it distorts there.

## What we learned

- **IRAM is the tight resource.** The prebuilt Arduino-ESP32 libraries pin
  ~11 KB of libc in IRAM for the rev-1 PSRAM workaround, and Bluetooth takes
  ~33 KB. The M1 build overflowed by 1.5 KB until `lib_archive = yes` and
  `tools/iram_diet.py`. WiFi's IRAM needs won't fit in the 7 KB left:
  rebuilding the framework with pioarduino's `custom_sdkconfig`
  (`CONFIG_SPIRAM_CACHE_WORKAROUND=n`, FreeRTOS/heap functions in flash) is
  the likely next step.
- **pioarduino on Windows** must be run from PowerShell with `MSYSTEM` unset,
  needs PlatformIO Core ≥ 6.2.0, and links every library object unless
  `lib_archive = yes` is set.
- **Picking headphones by signal strength is unsafe.** A TV in the next room
  crossed a −70 dBm threshold and got paired. The PoC then paired by name
  (and, with no name, only below −55 dBm). Today the Core2 never picks
  headphones by itself: the listener pairs them on Output > Pair new
  headphones, a release build never scans on its own, and only a developer
  build's `BT_SINK_NAME` (or the console's one-scan `Bs`) is ever looked
  for (`SinkSearch`, [ARCHITECTURE.md](ARCHITECTURE.md#bluetooth)).
- **ESP32-A2DP defaults** needed changing: auto-reconnect is off by default,
  and when on it retries 1,000 times (hours) before scanning again.
- **The stream can start late after a reconnect.** After an auto-reconnect the
  Powerbeats pulled no audio until playback was restarted. The review traced
  this to ESP32-A2DP starting the media stream only on its 10-second heartbeat,
  plus our own pause on the earlier disconnect (not ear detection, as first
  guessed). Fix: start the stream on connect (as Espressif's a2dp_source
  example does) and handle AVRCP so the headphones' buttons work.
- **ESP8266Audio quirks:** ID3 tags in UTF-16 arrive truncated (library
  metadata will come from mStream instead), and its FLAC decoder reports a rate
  of 0 before reading the header.
- **24-bit/96 kHz FLAC underruns on the speaker.** Decode (37%) plus the
  speaker's resampling on the same core is too much. Bluetooth can't take it
  anyway, so hi-res files should be converted to 16-bit/44.1 kHz during sync.

## Not verified yet

- SD card: coded, but no card was available.
- Battery drain: every run was on USB power. The tooling to measure it is
  in (the console's `P`, [ARCHITECTURE.md](ARCHITECTURE.md#power-measurement)).
- WiFi, and WiFi alongside Bluetooth.
- The RCA/3.5 mm module (M5Unified supports it with one config bit).

## Next steps

See the roadmap in [ARCHITECTURE.md](ARCHITECTURE.md): library index and sync
(with hi-res conversion), browsing UI, AutoDJ from a precomputed similarity
table, server discovery without mDNS, and headphone controls.

Audio fixes from the review, smallest first. 1–6 are done (1682ddc, 6a7817b,
e944373); 7 is open:

1. A fresh FLAC generator per track: the reused one can replay up to ~100–190 ms
   of freed memory at the start of a FLAC that follows an interrupted one.
2. Cap the Bluetooth software gain at −2 dB, then move headphone volume to
   AVRCP absolute volume with a fixed software headroom and a ramp.
3. `-DFPM_64BIT` for libmad; speaker volume cap 200 → 181 (M5's mono sum clips
   above that).
4. Start the Bluetooth stream on connect, re-arm auto-reconnect after a
   connect, and handle AVRCP play/pause/skip/volume from the headphones.
5. De-click pause, skip and output switches with short fades.
6. Log the negotiated SBC settings, the live volume and gaps between Bluetooth
   callbacks, so listening tests can be interpreted.
7. A 48 → 44.1 kHz resampler for Bluetooth (speexdsp or polyphase), and
   shrinking the decode stack from 16 KB to 8 KB.

Known follow-up (volume): the absolute-mode UI race. The volume shown
follows the headphones' reports, so one that overtakes a command of ours
still on its way leaves the UI above their level until they report again,
and a step down from there sends more than they have (documented as a
residual in `AbsVolumePolicy.h`; the random-events test counts these steps).
