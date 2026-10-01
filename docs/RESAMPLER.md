# Sample-rate converter

The ring between the decoder and the outputs (`PcmRing`) is to hold 44.1 kHz
only, with every track at another rate converted on its way in. This
document records what that converter is, how it was chosen, what has been
built and measured, and the plan for the rest.

**Status: built, wired into the firmware, host-tested and checked on the
device (section 6b): correct, but too slow.** Every track now reaches the
ring at 44.1 kHz, on both outputs. On the Core2 the pitch, the level, the
frame counts, positions and starts are exact, with 0 underruns in 40
minutes. But a 48 kHz track costs 22 % of a core at 240 MHz and 32 % at
160, two to three times the estimate. At 160 MHz a 48 kHz tone alone
takes list scrolling from 29 to 9 fps. Steps 1-4 of section 7 are done.
Step 4's gate fails, so step 5 is next, and it is two jobs: the kernel and
the per-frame path.

- Built: `lib/core/RateConverter` (the converter: routes, the C kernel, the
  ring-full contract), `lib/core/ResamplerTables` (the Q15 tables, generated
  by `tools/gen_resampler_tables.py`) and `test/test_rate_converter`
  (28 host tests).
- Wired in (step 3): `lib/core/RingFeed` (the decode side of the ring: the
  converter, the stage, the ring-full rule, the tail; 13 host tests in
  `test/test_ring_feed`, through the real `PcmRing`), which
  `src/audio/RingOutput` wraps; `Core2AudioBackend` (resets, ring-frame
  counters, tones through the converter, the refusals, the benches);
  `lib/core/ToneTrack` (the built-in tracks' paths, `tone:1000@48000`;
  tested in test_tone_gen); the unlisted test tracks in `TrackCatalog`; the
  console's `R`, `Rt` and `Rb`, and `b<n>`'s decode-plus-convert line.
  Section 9b lists where the build differs from the plan.

Where the numbers come from:

- **Host tests** (`pio test -e native`): every quality figure in this
  document comes from the committed tables and the real converter, measured
  by `test_rate_converter`, which also asserts them (with a margin of 2-3 dB).
- **Static instruction counts**: `RateConverter.cpp` compiled with the
  firmware's own compiler and flags (xtensa-esp32-elf-g++ 14.2.0, `-Os`,
  `-mfix-esp32-psram-cache-issue`), its inner loops counted in the
  disassembly.
- **The device: section 6b** (2026-10-01). The CPU figures in section 3
  ("The kernel and the CPU") are the static estimates made before it; the
  device measured two to three times more.
- **The firmware build** (`pio run -e core2`, all its guards passing): the
  app grew by about 20 KB (2.12 to 2.14 MB, 36 % of a 6 MB slot); all of the
  converter's code is in flash (no IRAM: iram_diet unchanged) and its tables
  in flash data.

## 1. Decisions

1. **Algorithm: our own polyphase FIRs for exact rational ratios**, with
   Kaiser-windowed sinc tables. Each polyphase row has 48 taps (K = 48) of
   Q15 coefficients, `const` in flash, and only half of each table is stored,
   because row L - p is row p reversed. Every row sums to exactly 32768, so
   the DC gain is exactly 1. The rounding of each row is optimised to keep the
   error out of the audio band. The output is rounded and saturated, never
   wrapped. speexdsp is not used: at 48 kHz its quality 4 is 3.2 dB down at
   20 kHz and costs 6x the multiplies, and its DC gain isn't exactly 1
   (section 3).
2. **Rates: 8, 11.025, 12, 16, 22.05, 24, 32, 48, 88.2 and 96 kHz** go to
   44.1 kHz, and 44.1 kHz passes through untouched (bit-exact, no cost):
   - 48 kHz: 147/160 (table D147).
   - 96 kHz: a halfband to 48 kHz, then 147/160. 88.2 kHz: a halfband.
   - 22.05 and 11.025 kHz: x2 and x4, straight to 44.1 kHz.
   - 8, 12, 16, 24 and 32 kHz: x6, x4, x3, x2 and x3/2 up to 48 kHz, then
     147/160.

   All the upsamplers are rows of one 12-row table (U12): every 6th, 4th,
   3rd or 2nd row. 88.2 and 96 kHz need the 240 MHz CPU setting and are
   refused at 160 MHz; until the device check (point 5) they are **off at
   any speed** (`RateConverter::kHiResOn`, a build with
   `-DMSTREAM_HIRES_RATES=1` turns them on). Anything else (176.4/192 kHz,
   odd rates) is refused on both outputs with a reason.
3. **Place: in `RingOutput`, on the decode task, for both outputs.** The ring
   has one rate, so an output switch mid-track can't play at the wrong speed,
   and Bluetooth's `BtSink` stays as it is. The generator's "ring full: return
   false, retry the sample" rule holds: a source frame is taken only when
   everything it can produce fits in the stage. The host tests check that over
   long runs with random refusals: the output is bit-identical to an unrefused
   run, with the exact frame count.
4. **Everything after `RingOutput` counts 44.1 kHz ring frames**: positions,
   durations, the taps and the beat tracker. Only the decoders' own seeks
   use the source rate. Output frame n sits exactly at source time n/44100
   (measured: within 0.001 µs at every rate), so `positionMs()` and resume
   starts stay exact.
5. **Kernel: portable C**, the specification on the host and the device:
   two int32 sums per output, one per contiguous half of the row, planar
   histories. By instruction count it costs about 8-10 % of a core at 240 MHz
   for a 48 kHz track. A MAC16 assembly kernel (about 3 %) is written only if
   the device bench says the C kernel hurts something the listener would
   notice (section 7, step 4). 88.2/96 kHz is turned on only once a 24/96
   FLAC has played 10 minutes on the speaker without an underrun: until
   then the build refuses them (`MSTREAM_HIRES_RATES` is 0); the benches
   (`Rb`, `b<n>`) convert them anyway.

## 2. What happened before the converter (read from the code at c9f3c7d)

- `RingOutput::SetRate()` refuses anything but 44100 Hz when `only44k` is set
  (`src/audio/RingOutput.h:50-57`). The backend sets it from the output at
  track start only (`out_->reset(output_ == Output::Bluetooth)`,
  `Core2AudioBackend.cpp:514`), so a 48 kHz track on Bluetooth fails with
  "48000 Hz can't play over Bluetooth yet" and the player skips it.
- `BtSink::onData()` (`BtSink.cpp:1833`) never reads `AudioShared::rate`, and
  ESP32-A2DP's SBC source runs at a fixed 44.1 kHz. A 48 kHz track started on
  the speaker and switched to Bluetooth mid-track would therefore play
  8.8 % slow, about 1.5 semitones flat. `positionMs()` would run 8.8 % slow
  too, because it divides ring frames by 48000. This is read from the code
  (`setOutput()` only moves the ring's consumer); it hasn't been tried.
- On the speaker, M5.Speaker converts a 48 kHz buffer itself, by linear
  interpolation between neighbouring samples (`Speaker_Class.inl`, its
  mixing loop). Modelled on the host, that is 1.3 dB down at 10 kHz and
  5.3 dB down at 20 kHz. A 10 kHz tone comes out with THD+N of -22 dB, and
  a 20 kHz tone puts a -11 dB alias at 16.1 kHz.
- The dancer's `BeatTracker` always assumes 44.1 kHz (`BeatTracker::Config{}`,
  `DanceMode.cpp:26`). A 48 kHz track on the speaker reaches it as 48 kHz
  audio counted as 44.1 kHz, so its tempo reads 8.8 % high.
- A 24-bit/96 kHz FLAC underran on the speaker: decoding it takes 37 % of a
  core, plus M5.Speaker's own resampling (POC-RESULTS.md, "What we
  learned").

Converting everything to 44.1 kHz in `RingOutput` fixes all four.

## 3. The algorithm

### Candidates

- **(a) Our own polyphase FIR.** For a ratio L/M (147/160 at 48 kHz), the
  prototype low-pass is a Kaiser-windowed sinc sampled at L x the source
  rate and split into L rows of K taps. Each output takes one row, chosen by
  a phase that advances by M per output and by -L per input. Everything is
  integer, so the count is exact: 48000 in, 44100 out, every second, with
  no drift.
- **(b) speexdsp's resampler** (BSD-3-Clause; Tangara uses it, according to
  the earlier format research). Its quality levels 0-10 set the filter
  length, cutoff, Kaiser window and table oversampling. For 48 to 44.1 kHz
  its default build doesn't use a full table (147 rows): it uses a short
  oversampled table with cubic interpolation, which costs 4 multiplies per
  tap. `RESAMPLE_HUGEMEM` gives it the full table, but it builds that table
  at init from `sin()` in double precision, which is software floating
  point on the ESP32.
  **Measured as a model, not the vendored code.** Downloading speexdsp
  needed the user's OK, which couldn't be had. The model re-implements its
  quality map, window betas, table layout, cubic interpolation and
  fixed-point rounding. Before anyone vendors it, these numbers must be
  re-checked against the real `resample.c`.
- **Also looked at and dropped:**
  - **esp-dsp** is already in the Arduino framework (Apache-2.0). Its
    multi-rate FIR has only an ANSI-C version on the ESP32. Its resampler is
    mono, block-based and tracks the ratio in a float, which is made for
    clock drift, not exact counts. Its MAC16 dot product
    (`dsps_dotprod_s16_ae32`, disassembled from the framework's library)
    shows the speed the hardware can reach. But it stores the shifted 40-bit
    result into 16 bits without saturating, so a hot sample would wrap.
  - **libopus's SILK resampler** comes inside ESP8266Audio, but it does only
    8/12/16/24/48 kHz, never 44.1 kHz.
  - **Linear interpolation** is what M5.Speaker does: see section 2.

### How it is measured

`test_rate_converter` measures the real converter. Inputs are int16, as the
decoders hand them over; outputs are int16 at 44.1 kHz.

- **Passband**: tones from 50 Hz to the passband's edge (20 kHz for 48 kHz
  and above, 0.875 of the source's Nyquist below), and the analytic |H(f)|
  of each whole prototype (an FFT of all its rows, interleaved by their
  offsets).
- **THD+N**: a -1 dBFS sine at 1 kHz and at 10 kHz (or the passband's edge,
  when that is lower), placed exactly on an FFT bin at 44.1 kHz (16384
  points, rectangular window). Everything in 20 Hz-20 kHz except the tone's
  own bin counts, relative to the tone. The same analysis of a 16-bit sine
  generated directly at 44.1 kHz gives -97.5 dB: the floor nothing can beat.
- **Spurs and aliases**: the largest component in 20 Hz-20 kHz other than the
  tone, with a Blackman-Harris window, relative to the input tone. A tone on a
  bin leaks nothing past 3 bins with that window, so the spurs are clean.
- **DC and full scale**: constant inputs (32767, -32768, 12345, -1 and 1)
  come out exactly, at every rate. A full-scale square wave (Gibbs overshoot
  past full scale) and the worst possible input for the largest row
  saturate and never flip sign.
- **Time alignment**: the phase of a 100 Hz and a 1 kHz tone against
  n/44100.
- **Bit-exactness**: every route against an independent reference written in
  the test (direct convolution at each output instant, in 64 bits, with
  `floor` rounding), for lengths around every stage's delay, at moderate and
  at full-scale noise.
- **Exact counts and the retry rule**: section 5.

### Results

| source | route | THD+N 1 kHz | THD+N high tone | worst spur | at the passband's edge |
|---|---|---|---|---|---|
| 48 kHz | 147/160 | -85.7 dB | -85.6 dB (10 kHz) | -97.0 dB | -0.0002 dB (20 kHz) |
| 96 kHz | halfband, 147/160 | -85.5 | -85.4 (10 kHz) | -97.2 | -0.0001 (20 kHz) |
| 88.2 kHz | halfband | -95.8 | -96.1 (10 kHz) | -110.6 | -0.0013 (20 kHz) |
| 32 kHz | x3/2, 147/160 | -84.3 | -80.7 (10 kHz) | -82.6 | -0.0007 (14 kHz) |
| 24 kHz | x2, 147/160 | -85.5 | -84.6 (10 kHz) | -93.2 | -0.0001 (10.5 kHz) |
| 22.05 kHz | x2 | -94.6 | -84.3 (9.65 kHz) | -84.6 | -0.0001 (9.65 kHz) |
| 16 kHz | x3, 147/160 | -82.8 | -85.3 (7 kHz) | -86.9 | -0.0008 (7 kHz) |
| 12 kHz | x4, 147/160 | -84.4 | -81.5 (5.25 kHz) | -84.0 | -0.0001 (5.25 kHz) |
| 11.025 kHz | x4 | -94.8 | -83.3 (4.82 kHz) | -83.6 | -0.0002 (4.82 kHz) |
| 8 kHz | x6, 147/160 | -83.2 | -82.5 (3.5 kHz) | -87.6 | -0.0004 (3.5 kHz) |

The analytic responses of the committed tables (Q15):

| table | passband | stopband |
|---|---|---|
| D147 (48 kHz) | ±0.0002 dB, 20 Hz-20 kHz | -88.4 dB at 25.95-36 kHz; at most -85.9 dB anywhere above, up to 3.5 MHz |
| U12 (upsampling) | ±0.0004 dB, to 0.875 of the source's Nyquist | at most -78.7 dB from 0.5575 of the source rate |
| HB96 | ±0.0008 dB, to 20 kHz | at most -81.2 dB from 28 kHz |
| HB88 | ±0.0014 dB, to 20 kHz | at most -76.1 dB from 24.1 kHz |

Content the output can't hold, and what it leaves in 20 Hz-20 kHz (a
-1 dBFS tone):

| source | tone | largest spur |
|---|---|---|
| 48 kHz | 20 kHz / 22 kHz | -93.6 / -87.9 dB |
| 48 kHz | 23 kHz / 23.9 kHz | -37.2 dB at 19.1 kHz / -18.7 dB at 20.0 kHz |
| 96 kHz | 24.1 kHz | -25.3 dB at 20.0 kHz |
| 96 kHz | 30 / 40 / 47 kHz | -90.4 / -88.2 / -91.9 dB |
| 88.2 kHz | 24.1 / 30 / 40 kHz | -76.7 / -82.4 / -98.5 dB |

Notes:

- **The one thing D147 gives up** is source content at 22.05-25.95 kHz: in
  a 48 kHz file that is 22.05-24 kHz, in a 96 kHz file 22.05-25.95 kHz
  (the halfband passes it). It lands at 18.15-22.05 kHz, as little as
  -19 dB down at the band's bottom edge. Those land above what adults hear,
  and lossy encoders low-pass at 16-20 kHz, so music has next to nothing
  there; a hi-res file's ultrasonic noise is far below full scale. A strict
  design (stopband from 24.1 kHz) gets those spurs to -85 dB, but needs
  K = 64: a third more CPU and a third larger table. It's a one-constant
  change in the generator if it is ever wanted.
- **The Q15 stopband is about -86 dB**, not the -91 dB the unquantised
  design reaches (the earlier draft of this document quoted the
  unquantised figure). Each row's rounding error is the same error that
  sets THD+N: it scales with the signal, so it isn't a noise floor, and the
  tone tests above already include it.
- K was chosen by measurement on the prototypes: K = 32 gives -65 dB of
  stopband and -77 dB THD+N, K = 40 -79.5 and -84 dB, K = 48 -86 dB THD+N.
  K = 56 adds nothing in Q15.
- **The upsampling cascades** (8-32 kHz) cost 1-5 dB of THD+N against a
  direct 441-row table (-84.5 to -85.7 dB measured on the prototype),
  because two stages add their rounding errors. -80.7 dB (32 kHz, 10 kHz) is
  the worst, level with libmad's own ~81 dB. The direct table was dropped
  for the cache (section 8).
- The 22.05 and 11.025 kHz routes reach -94.6 dB at 1 kHz: their 2 and 4
  phases' rounding errors barely vary, so they make less noise.
- Against speexdsp (the model, 48 kHz): its quality 4 costs 296 multiplies
  per output and channel (ours: 48), is -0.08 / -3.18 dB at 19 / 20 kHz,
  and measures THD+N -82.2 / -84.5 dB with spurs around -83 dB. Quality 8
  matches our passband at 712 multiplies. Its rows don't sum exactly: a
  constant comes out up to 4-6 LSB off, as a faint tone at the phase
  pattern's rate. Its full-table build (`RESAMPLE_HUGEMEM`, 72 multiplies)
  needs a 21 KB table built in soft-float double at init and keeps the
  3 dB loss at 20 kHz.

### The tables

`tools/gen_resampler_tables.py` (Python, standard library only) writes
`lib/core/ResamplerTables.cpp`; `ResamplerTables.h` declares them.

| table | for | design | stored | bytes |
|---|---|---|---|---|
| D147 | 48 kHz, and the second stage of 96 and 8-32 kHz | 147 rows x 48, passband 20 kHz, stopband from 25.95 kHz, A 93.4 dB | rows 0-73 | 7,104 |
| U12 | every upsampler | 12 rows x 48, passband 0.875 of the source's Nyquist, stopband from 0.5575 of its rate, A 90.7 dB | rows 0-6 | 672 |
| HB96 | 96 kHz | 71-tap halfband, passband 20 kHz, stopband from 28 kHz, A 92.9 dB | 18 side taps | 36 |
| HB88 | 88.2 kHz | 123-tap halfband, passband 20 kHz, stopband from 24.1 kHz, A 90.1 dB | 31 side taps | 62 |

- **Polyphase rows.** Element j of row p multiplies the history's x[j],
  oldest first, at 23 - j + p/L source samples from the output instant. The
  tap at exactly 24 samples is zero, so row L - p is row p reversed, and the
  kernel reads the stored row backwards for p > L/2.
- **Rounding.** The ideal row, scaled so it sums to 32768, is rounded to the
  nearest integer, and the residue goes onto the largest taps, one LSB at a
  time. Then the generator tries every +1/-1 pair of taps (the sum is kept)
  and keeps a pair if it lowers the error's energy in the passband. That
  gains about 4 dB of THD+N (-81.4 to -85.7 dB at 48 kHz). More isn't
  available: the audio band is 83 % of the source's band, so there is little
  room to push the error out of it.
- **Halfbands.** The centre tap is 16384, the taps at even distances are 0,
  and one side's taps sum to exactly 8192. Their rounding is optimised for
  the stopband (the passband's error mirrors it), which took HB88 from about
  -75 to -76.1 dB.
- **The generator refuses** a table whose half-row sums of |c| could overflow
  the kernel's sums (below).
- **Tested by their properties, not their bytes**: the test checks each
  row's sum, each half's bound and each filter's response. The optimiser
  compares floating-point energies, so a last-bit difference in another
  platform's `sin` or Bessel series can flip a tap; regenerating on another
  machine may change a few LSBs, which is fine as long as the tests pass.
  `--check` compares the bytes, for a local run on one machine only; CI
  doesn't run it.

### Coefficient format

| K = 48 at 48 kHz | THD+N 1 kHz | THD+N 10 kHz | half table | inner loop |
|---|---|---|---|---|
| Q15, nearest rounding | -81.4 dB | -82.6 dB | 7.1 KB | 3.6 instructions / multiply (C) |
| **Q15, optimised rounding** | **-85.7 dB** | **-85.6 dB** | **7.1 KB** | same |
| float | -94.5 dB | -94.7 dB | 14.1 KB | 3.5 (C) |

**Why Q15 and not float**, despite float's 9 dB:

- **The cache.** The ESP32's flash and PSRAM share one 32 KB cache per core
  with the decoder's code and data, and a 48 kHz track reads its whole
  table every 3.3 ms. In Q15, half the table is 7.1 KB; in float it would be
  14.1 KB. A table small enough can also be moved into internal RAM if the
  bench shows misses.
- **Bit-exact tests.** The integer kernel gives the same bits on the laptop
  and on the ESP32, so the host tests check the device's exact output. With
  float, GCC fuses multiply-adds on the ESP32 but not on the laptop.
- **The MAC16 path**, if it is ever needed: the ESP32's MAC16 unit multiplies
  16-bit values into a 40-bit accumulator, about one multiply per
  instruction.
- **-85.6 dB is below the rest of the chain.** libmad with `FPM_64BIT` is
  about 81 dB SNR (platformio.ini). Neither SBC nor the speaker comes close.

### Overflow and saturation

A whole row's sum of |coefficients| is up to 2.34, so a single int32 sum
could reach 2.34 x 32768 x 32768 = 2.5e9 on an adversarial full-scale input,
past int32's 2.1e9 (and signed overflow is undefined in C++). The kernel
therefore keeps two int32 sums, one per contiguous half of the row (taps
0-23 and 24-47). Each half's sum of |c| is at most 42,280 in D147 and under
65,536 in every table, so each sum stays below 65,536 x 32,768 < 2^31: at
most 1.39e9 in D147. A halfband keeps its left side plus the centre in one
sum and its right side in the other, each also under 65,536 x 32,768. The
two sums are added in 64 bits once per output, rounded half up, shifted by
15 and saturated to int16. The generator refuses a table that breaks the
bound, and a host test proves it for every committed row.

Measured: the worst input for D147's largest row (every tap's sign at full
scale: an exact sum of 2.5e9) comes out at exactly +32767, or -32768 with
the signs flipped. A full-scale square wave saturates on its Gibbs
overshoot at every rate and never shows the wrong sign. The converter
counts clamped samples (`clamped()`).

### Exact unity DC gain

Each row's Q15 taps sum to exactly 32768 (a halfband's centre and sides
too). A constant input therefore comes out as exactly the same constant, at
every phase: checked for 32767, -32768, 12345, -1 and 1 at every rate. A
converter whose rows don't sum exactly (speex: up to 6 LSB) turns a constant
into a faint tone at the phase pattern's rate.

### Time alignment and delay

Each stage's prototype is centred, so its delay is half its length: 24 input
samples for a 48-tap polyphase stage, 35 or 61 for the halfbands. Each stage
swallows that many inputs before its first output, and at the end of a file
`finishPush()` pushes zeros through the whole route, so output frame n sits
at source time n x 44100 / rate exactly. Measured against the ideal: within
0.001 µs at every rate. The only latency this adds is the time to first
audio (0.5 ms at 48 kHz, 3.5 ms at 8 kHz; section 4). The outputs' latency
doesn't change, because they read the ring as before.

### The kernel and the CPU (estimates, made before the device bench)

**Measured since, in section 6b: 22 % of a core at 240 MHz for 48 kHz, not
8-10 %.** The loop runs at 7-8 cycles per multiply, not 3.6-4.6, and the
per-frame path around it costs about 390 cycles per source frame, not 20.
The estimate below is kept as it was made.

The C kernel (`RateConverter.cpp`):

- one int32 sum per half-row, 24 taps, unrolled by 8 with constant offsets,
  and marked always-inline;
- planar int16 histories, one per channel, each written twice so a
  48-sample window is contiguous; the kernel runs once per channel (once in
  mono, copied);
- rows past the middle read backwards (`c[-k]`) with the same loop shape.

Compiled with the firmware's compiler and flags at the build's own `-Os`,
each 8-tap body is 16 `l16ui` loads, 8 `mula.aa.ll`, two pointer `addi`s, the
branch, and a `wsr.acclo`/`rsr.acclo` pair: 29 instructions, 3.6 per
multiply, forward and reversed alike. GCC keeps each half's sum in MAC16's
accumulator. Interleaved stereo histories or even/odd partial sums would
make it swap the accumulator at every tap (6.0 per multiply, measured on the
prototypes). `-O2` gives the same loop, so no pragma is needed. The code is
4.3 KB of flash at `-Os`.

Per second of audio, with 3.6-4.6 cycles per multiply (the upper figure
allows one load-use stall per tap), about 60 cycles per output frame (row
selection, the 64-bit add, rounding and clamping, stores) and 20 per input
frame (the history writes, each followed by the PSRAM workaround's `memw`),
all with cache hits:

| source | multiplies / output / channel | per second (stereo) | 240 MHz | 160 MHz |
|---|---|---|---|---|
| 48 kHz, 22.05 kHz, 11.025 kHz | 48 | 19-23 M cycles | 8-10 % | 12-14 % |
| 88.2 kHz | 63 | 25-31 M | 10-13 % | refused |
| 96 kHz | 48 + 1.09 x 37 = 88 | 34-42 M | 14-17 % | refused |
| 8-32 kHz | 48 + 1.09 x 48 = 100 | 38-47 M | 16-20 % | 24-29 % |

Mono halves the kernel's part. Next to the measured decoding (MP3 23-24 % at
240 MHz and 32-34 % at 160; FLAC 22-23 %; a 24/96 FLAC 37 % at 240):

- A 48 kHz MP3 needs 31-34 % at 240 MHz, 44-48 % at 160.
- A 32 kHz MP3 decodes in about 2/3 of the time, so decode plus convert comes
  out about level with a 48 kHz track (46-51 % at 160 MHz); lower rates
  decode faster still, and voice files are usually mono.
- A 24/96 FLAC needs about 51-54 % at 240 MHz. It underran at 37 % plus
  M5.Speaker's own 96 kHz conversion, which goes away.

These figures leave out cache misses, which is the main unknown (section 8).
A MAC16 kernel would take the 48 kHz figure to about 3 %, and 96 kHz to
about 5 %.

The halfbands are not folded (adding the two samples a symmetric pair
multiplies first): the sum of two int16s needs 17 bits, which rules out
MAC16's 16 x 16 multiply, and in C it saves no instructions.

### Flash and RAM

- **Tables in flash**: 7,874 bytes in all (D147 7,104, U12 672, HB96 36,
  HB88 62). A 6 MB slot is 35 % used.
- **Code**: 4.3 KB at `-Os`.
- **RAM**: `sizeof(RateConverter)` is 1,544 bytes on the ESP32: the
  histories (stage 1: two channels x 2 x 123 samples, enough for any first
  stage; stage 2: two channels x 2 x 48), 8 held frames, 8 frames waiting
  to come out, the stages' state and the counters. All of it is in the
  object, which lives inside `RingOutput`: no allocation per track, nothing
  on the decode task's stack, no IRAM.

## 4. Which rates

| source | route | tables | time to first frame | multiplies / output / channel | notes |
|---|---|---|---|---|---|
| 44.1 kHz | passthrough | none | 0 | 0 | bit-exact, as today |
| 48 kHz | 147/160 | D147 | 0.50 ms | 48 | the must |
| 96 kHz | halfband (71 taps) to 48 kHz, then 147/160 | HB96 + D147 | 0.86 ms | 88 | 240 MHz only |
| 88.2 kHz | halfband (123 taps), /2 | HB88 | 0.69 ms | 63 | 240 MHz only |
| 32 kHz | x3/2 to 48 kHz, then 147/160 | U12 (every 4th row) + D147 | 1.25 ms | 100 | |
| 24 kHz | x2 to 48 kHz, then 147/160 | U12 (every 6th) + D147 | 1.5 ms | 100 | |
| 22.05 kHz | x2 | U12 (every 6th) | 1.09 ms | 48 | |
| 16 kHz | x3 to 48 kHz, then 147/160 | U12 (every 4th) + D147 | 2.0 ms | 100 | |
| 12 kHz | x4 to 48 kHz, then 147/160 | U12 (every 3rd) + D147 | 2.5 ms | 100 | |
| 11.025 kHz | x4 | U12 (every 3rd) | 2.2 ms | 48 | |
| 8 kHz | x6 to 48 kHz, then 147/160 | U12 (every 2nd) + D147 | 3.5 ms | 100 | |

`RateConverter::plan(rate, cpuMhz)` gives the route, the exact ratio of ring
frames to source frames (`num/den`: 147/160, 147/320, 441/80, ...) and, when
refused, the reason.

**Upsampling** (all rates below 44.1 kHz) protects source content up to
0.875 of the source's Nyquist: 14 kHz for 32 kHz, 9.65 kHz for 22.05 kHz,
3.5 kHz for 8 kHz. The cutoff sits at 0.995 of Nyquist so no Q15 tap
reaches 1.0. Content in the top 12.5 % below Nyquist is in the transition
band: its images, just above Nyquist, are down only 7-50 dB. Every
converter has this region, and low-rate MP3s are low-passed below it.
speexdsp's quality 4 is no better inside the band: -5.4 dB at 15 kHz for
32 kHz.

**Downsampling above 48 kHz:**

- **96 kHz**: halfband to 48 kHz, then D147. Content at 28-48 kHz (hi-res
  dither noise, ultrasonics) leaves -88 dB or less; content at
  22.05-25.95 kHz aliases as in a 48 kHz file (section 3). A direct 147/320
  design matches it but needs 96 taps and a 28 KB table.
- **88.2 kHz**: one halfband with 123 taps. Content at 20-24.1 kHz folds
  back to 20-24.1 kHz, above the audio band; content at 24.1-44.1 kHz is
  down at least 76 dB (-89 dB unquantised: rounding the small outer taps to
  Q15 costs the difference). THD+N is -96 dB: a single filter, so Q15 rounding adds no
  error that changes from output to output.
- **176.4/192 kHz**: refused. Decoding alone would take most of a core.

**The 160 MHz rule.** At 160 MHz a 24/96 FLAC would need an estimated
77-81 % of core 1 before the UI, the speaker task and the pump. 88.2 and
96 kHz are therefore refused below 240 MHz, with the reason "needs the
240 MHz CPU speed". Two things about it:

- **The clock to check is the setting, not the clock right now.** The
  console's `Pc` changes the clock live, and after a `Pc80` quiet spell
  `PowerLab::loop` raises it again only once `audioBusy()`, which can race
  `start()` on the decode task (`PowerLab.cpp:437-441`). So `main.cpp`
  hands the backend `PowerSettings::cpuBootMhz()` once at boot
  (`Core2AudioBackend::setCpuMhz()`), and every start passes that to
  `plan()`, never `getCpuFrequencyMhz()`. It is the speed set at boot, not
  `cpuSaved()`: a `Pcb` change is saved for the next boot without a
  restart, and until then the CPU still runs on the boot speed's PLL.
- **It is provisional** until check 5 of section 6 has measured a 24/96
  FLAC at 160 MHz.

Rates up to 48 kHz play at both speeds.

**Mono** (voice MP3s): the left channel is converted and copied to both,
which costs half. Both channels' histories are always written (the left
into both), so a stream that switches between mono and stereo mid-file goes
on with no reset.

**Refused, on both outputs**, as a failed track (the player skips it, and
`Ui::noteFailures()` says why: "Skipped ...: 37.8 kHz isn't supported" or
"Skipped ...: needs the 240 MHz CPU speed", measured in test_ui_library;
a failure for any other reason stays "can't play it"):

- any rate not in the table above: "[audio] 37800 Hz isn't supported (8-48
  kHz, 88.2 and 96 kHz)";
- 88.2/96 kHz while they are off (the build's default until the device
  check): "[audio] 96000 Hz is off in this build (...)"; on screen "96 kHz
  isn't supported";
- 88.2/96 kHz below 240 MHz (once on): "[audio] 96000 Hz needs the 240 MHz
  CPU speed".

## 5. Where it sits, and its real-time rules

### In `RingOutput`, for both outputs

Bluetooth-only conversion was considered and rejected. It would leave the
ring at two rates. An output switch mid-track would then need the ring
re-converted or refused, so the 8.8 % bug would come back in another form.
The speaker would keep M5.Speaker's linear interpolation, and the dancer
would keep getting the wrong rate. `BtSink` runs next to SBC and Bluedroid
on core 0 with no slack; the decode task on core 1 has its own budget and
already yields between passes.

### The converter's contract

`RateConverter` takes one source frame per `push()` and writes 0 to
`maxOut()` ring frames:

- `maxOut()` is 1 at 44.1 kHz and above, 2 at 22.05, 24 and 32 kHz, 3 at
  16 kHz, 4 at 11.025 and 12 kHz, and 6 at 8 kHz. While frames held before the rate (below) are still
  waiting to come out at 44.1 kHz, it is that many more: never more than
  `kMaxOut` = 9.
- A frame is taken whole (history and all) or not at all. A caller with less
  room than `maxOut()` doesn't push; it keeps the frame and offers it again.
- `process()` does the same for a block: it takes frames while the room
  left holds `maxOut()`, and returns how many it took.
- `finishPush()` pushes the tail, one zero frame at a time, under the same
  rule, until `finished()`. It stops at exactly ceil(taken x num / den)
  frames, counted from the source frames taken, and drops any excess from
  the last push. A cascade's tail goes through both stages.
- Tested: the output doesn't depend on how the input is split up (one frame
  at a time, random blocks, random room, refusals), and a push never writes
  more than `maxOut()`.

### The retry rule, the stage and the budget

```
ConsumeSample(sample):
  if rejected or budget == 0 or ringBudget == 0: return false
  if stage room < conv.maxOut(): commit()             // push staged frames into the ring
  if stage room < conv.maxOut(): return false         // ring full: the generator keeps the sample
  staged += conv.push(sample, stage + staged)         // history in, 0..maxOut frames out
  budget -= 1                                         // source frames: bounds the decode work per pass
  ringBudget -= frames made                           // ring frames: bounds the conversion per pass
  return true
```

Nothing is lost or duplicated, and the generator's retry of `lastSample`
stays correct.

- **The budget** caps both sides at `kChunkFrames` = 1024: source frames, so
  a pass's decode work is as today, and ring frames, so a low rate's pass
  converts no more than a 44.1 kHz pass's worth. (Source frames alone let an
  8 kHz pass make 5,645 ring frames through x6 and 147/160: about 10 times
  the conversion of a 48 kHz pass, an estimated 17-21 ms at 240 MHz above
  the UI loop, every pass.) The last frame taken may add up to `maxOut() - 1`
  more. At 44.1 kHz and above the source frames run out first.
- **Room before a pass.** `produceDecoded()`'s check becomes ring frames: the
  stage plus `plan.ringFrames(2048)`, which is 11,290 frames at 8 kHz, 17 %
  of the ring.
- **End of file.** When the decoder is done, `RingOutput::finish()` runs
  `finishPush()` under the same rule, so it resumes after a full ring.
- **Tones.** `produceTone()` hands its chunk to `RingOutput` as source frames
  at the tone's rate, through `process()` (`RingFeed::write()`), so the
  built-in test tracks exercise the converter. Its room check is in ring frames (the stage plus
  `ringFrames(n)`), and it keeps an offset into `chunk_` for the frames not
  yet taken, the way the generator keeps `lastSample`. Without that,
  `ToneGen`/`ClickGen` would already have moved past frames the ring didn't
  take, losing audio and breaking the exact length and the resume
  arithmetic. At 44.1 kHz it is a copy.

**Measured on the host** (`test_long_runs_through_a_full_ring_are_exact`):
RingOutput's 256-frame stage and 1024-frame budget, a ring taking a random
0-1500 frames per pass and nothing at all a third of the time, and the
generator's hold-and-retry. 60 s at 48 kHz and 10 s (plus 7 frames) at every
other rate came out bit-identical to an unrefused run, with exactly
ceil(N x num / den) frames. `test_exact_counts_for_any_length` checks every
length from 0 to 130 frames and random ones up to 12,000 at every rate, and
`test_every_route_matches_an_independent_reference` checks the bits too.
600 s at 48 kHz is 26,460,000 frames exactly (`plan(48000).ringFrames`).
Nothing drifts, because the phase is an integer.

### The real call order: frames before the rate

ESP8266Audio doesn't always say the rate before the first frame:

- **MP3**: `loop()` first retries `lastSample`, which is the constructor's
  {0,0} (`AudioGenerator.h:32`). Then `GetOneSample()` checks
  `pcm.samplerate` before `mad_synth_frame_onens` has set it
  (`AudioGeneratorMP3.cpp:206-232`), so the first decoded sample goes over
  too, and `SetRate()` arrives with the second.
- **FLAC** says the rate before its first frame (it doesn't retry
  `lastSample` while `channels` is 0), says 0 before it has read its header,
  and a seek sets the same rate again and then hands over a {0,0}.

So: until the first `setRate()`, up to `kMaxPending` = 8 frames are held.
The first rate is a configure, never a mid-stream change: the held frames
are replayed through the route. Every converting route's delay (24 frames or
more) is longer than the hold, so they only fill the history; at 44.1 kHz
they come out ahead of the next frame. The output is identical to setting
the rate first (tested at every rate, through the full ring as well). A
stream that never says its rate is taken as 44.1 kHz once the hold is full,
as ESP8266Audio's outputs assume. Rate 0 is ignored.

### Resets

- **`RingFeed::reset()` resets the converter** (zeroed histories, phases,
  delays, counters, no rate). It runs right after
  `trackStart_ = ring_->discardAll()` in `start()` (`Core2AudioBackend.cpp`),
  for every kind of request, and again after a bench. Today `out_->reset()` sits at
  line 514, after the `tone:` branch and after `Kind::Stop` and
  `Kind::Bench` have returned. Once tones go through `RingOutput`, a tone
  after a music track would otherwise start from the previous track's
  history: up to 24 frames of old audio. That covers every track change,
  skip, stop, and seek or resume start. A host test checks that a reset
  leaves nothing behind: the output after it is bit-identical to a fresh
  converter's.
- **The same rate again changes nothing**: the FLAC seek's `SetRate()` and
  the reopen from the top after a failed seek.
- **Another rate mid-stream** (MP3 allows it; it's rare) restarts the
  filters: from there on, the same as a fresh converter at the new rate. It
  may click, and only that (saturating, never louder).
- **`SetChannels()` never resets**: MP3's `begin()` says 2 and
  `GetOneSample()` says 1 two frames into every mono file. A reset there
  would clip the start of every mono track.

The bench (section 6) uses `RingOutput`'s converter (`start()` has stopped
playback anyway), never one on the decode task's 16 KB stack, which also
hosts libFLAC.

### Internal RAM

The converter's histories are read 96 times per output, so `RingOutput`
(about 2.6 KB with its stage and the converter) must stay in internal RAM.
The framework's sdkconfig has `CONFIG_SPIRAM_USE_MALLOC=y` with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`: a `new RingOutput` of 4 KB or
more would go to PSRAM without a word. So `RingOutput` has
`static_assert(sizeof(RingOutput) < 4096)` (`src/audio/RingOutput.h`), and
the firmware build checks it; `test_ring_feed` bounds `sizeof(RingFeed)`
too.

### Positions and durations: what counts what

| counter | today | with the converter |
|---|---|---|
| ring frames, `positionMs()` (`readPos() - trackStart_`) | source frames at the source rate | **44.1 kHz ring frames**, divided by 44100 |
| `AudioShared::rate`, `sampleRate()` | the source rate | **always 44100** (kept as a field; the speaker's `playRaw` rate and the dancer read it) |
| `producedFrames_` (durations, decode load, `expectingAudio`, pacing) | source frames (`budgetLeft()`) | **ring frames** made this pass (a `RingOutput` counter) |
| `progress::estimateDurationMs()` | frames at `shared_.rate` | ring frames at 44100 (bytes per ring frame is exact) |
| `noteRingFill` / `noteStartProgress` / `RefillPacer` rate | `out_->rate()` | 44100 |
| MP3 resume byte, FLAC seek sample | source rate (STREAMINFO) | unchanged: the decoder's own units |
| a built-in tone's length at a resume start | 44100 x seconds | the tone's own rate x seconds, as source frames |
| `description()` | "FLAC, 96000 Hz" | unchanged; the start logs "[audio] 96000 Hz -> 44100 Hz (halfband /2, then 147/160)" |
| `AudioShared::rate` | the source rate, published before a track's first frame | gone: `AudioShared::kRingRate` (44100), a constant; `SpeakerSink` plays at it, `sampleRate()` returns it |

Because output frame n sits exactly at source time n/44100, `positionMs()`
is exact from the first frame. A resume start lands exactly where the
decoder put it.

A side benefit: the ring's 65,536 frames hold 1.49 s at every rate, where a
96 kHz track holds only 0.68 s today.

### The taps and the beat tracker

Both outputs' `AudioTap` copy ring frames, now always at 44.1 kHz. That
matches what `BeatTracker::Config{}` assumes, and `TapReader`'s clock
(`setSampleRate(sampleRate())`). The dancer's click-track truth is in
44.1 kHz track frames; the click tracks stay at 44.1 kHz. Nothing in the
dancer changes, and the 8.8 % tempo error on 48 kHz tracks is gone.

### The speaker (M5.Speaker)

`SpeakerSink` already sets `cfg.sample_rate = 44100` (`SpeakerSink.cpp:71`),
and its `playRaw` rate becomes 44100 always. M5.Speaker's mixer
interpolates linearly for every output sample whatever the input rate.
Its rate is computed from the I2S divider, so it is a hair off 44100 even
for a 44.1 kHz input. The work per output stays the same. It reads 1 input
frame per output instead of 1.09 (48 kHz) or 2.18 (96 kHz), so it does a
little less, and none of its aliasing at 48 kHz remains.

A side observation, unchanged by this design: since M5.Speaker's rate isn't
exactly 44100, its interpolation fraction drifts slowly. At the worst
fraction, a two-tap average is 3 dB down at 11 kHz, and that is true of
44.1 kHz tracks today. It is a possible follow-up: tell M5.Speaker the
I2S's real rate, or bypass its resampler.

### Bluetooth

`BtSink` doesn't change. Its data callback already assumes 44.1 kHz, and
that is now true for every track. The `only44k` argument of
`RingOutput::reset()` is gone, and with it the "can't play over
Bluetooth yet" failure.

### Hearing safety and UX

- **Never louder.** Each row's DC gain is exactly 1. Overshoot near full
  scale (Gibbs, at most +7.4 dB for the worst adversarial input) is
  saturated after rounding, and the accumulators are proved not to
  overflow. The fades, gain and sleep fade downstream are untouched, still
  "at most 1".
- **No burst at a start, seek or skip.** The history is zeroed at every
  reset, right after the ring's `discardAll()`, for every request kind. The
  converter's first output is the filter's response to the source starting
  from silence, and the `DeclickReader` fades it in as today.
- **Nothing plays by itself.** The converter outputs only in response to
  frames handed to it, plus its tail at the end of a file. That tail is the
  real audio's last half-millisecond (3.5 ms at 8 kHz), which decays to
  zero. Silence in gives exact zeros out, at every rate, tail included (a
  host test).

## 6. Test hooks on the device

**Built-in tracks at other rates**, unlisted like `tone:silence`
(`TrackCatalog::find()` knows them):

- `tone:1000@48000`, `tone:1000@96000`, `tone:1000@88200`, `tone:1000@32000`,
  `tone:1000@22050`, `tone:1000@8000`: a 1 kHz tone at -18 dBFS, 30 s, made
  by `ToneGen` at that rate and converted like a file. **Silent mode `z`
  only** (below).
- `tone:silence@48000`, `tone:silence@96000`, `tone:silence@22050`: an hour
  of zeros at that rate. The output runs at its full rate, nothing is
  heard. These are the only ones for Bluetooth.

**Console `R`** (the key is free):

- `R`: status and help. It shows the current track's route, the source
  frames taken and the ring frames produced (their ratio must be exactly
  `num/den`: 160:147 at 48 kHz), and how many samples were clamped since
  the start.
- `Rt<n>`: play test track n **in a queue of its own** (one entry, repeat
  off), or with a "stop, don't advance" flag on the request. Never the way
  `Pz` does it: `PowerLab::playSilence()` uses `playNext` and then
  `play(current + 1)`, so the listener's queue follows it, and
  `PlaybackController::update()` calls `advance()` when a track fails or
  ends (`PlaybackController.cpp:269-283`). A test track that failed (a
  converter bug, a `plan()` refusal) or ended would go on to real music at
  the listener's volume. While Bluetooth is the output, `Rt` refuses every
  track that isn't silence and says why: headphones may be on someone's
  ears.
- `Rb`: the converter bench. On the decode task (a `Kind::RateBench`
  request, like `b`), playback stopped, it converts 10 s of a fixed stereo
  signal at each supported rate with `RingOutput`'s own converter. For each
  rate it prints cycles per second of audio (`esp_cpu_get_cycle_count()`)
  and the percentage of a core at the current clock. Run it at 240 and at
  160 MHz: cache misses cost a different number of cycles at each speed.
- `b<n>` (existing): for a file at another rate, it also prints decode plus
  convert. Together with `Rb`'s convert-only figure, that shows how much
  the two get in each other's way in the cache, the main unknown.

**Device checks, all silent.** Silent mode `z` can't be turned off without a
restart (`main.cpp:194`) and forbids Bluetooth (`main.cpp:186/566/580`), so
the speaker checks run in `z` mode, and a Bluetooth check runs without it:
that one needs the user awake, or a queue holding nothing but
`tone:silence@48000`.

1. `Rb` gives the real cost at 240 and 160 MHz.
2. `tone:silence@48000` on the speaker, cut to 60 s: `R` shows exactly
   160:147 source to ring frames, the track ends at 60 s of ring time and
   60 s of `millis()` (within 0.1 %), and 0 underruns. A converter that
   produced the wrong number of frames (a passthrough by mistake) fails
   here; `positionMs()` and `btFramesPerSec` alone are "right" by
   construction and can't show it.
3. A silent pitch check in `z` mode: `tone:1000@48000` (and `@8000`,
   `@96000`) shows 2,000 zero crossings per 44,100 ring frames, counted from
   the ring or the tap.
4. **Deferred until the user is awake, or with the queue holding only
   `tone:silence@48000`**: that track with the output switched to Bluetooth
   mid-track: `R`'s ratio still 160:147, `btFramesPerSec` about 44100, the
   position in real time, 0 underruns. This is the switch bug's check.
5. `tone:silence@96000` at 240 MHz for 10 min on the speaker: 0 underruns,
   the decode load from the stats line. Then the same with a real 24/96
   FLAC, still silent, logging the SD card's read time apart from decode
   plus convert: at 576 KB/s the FLAC needs about 48 % of the measured
   1.2 MB/s SD throughput, and about 72 % while the ring refills, so an
   underrun may be I/O, not CPU. This is the gate for 88.2/96 kHz. Then, at
   160 MHz, a 24/96 FLAC's real load, to confirm or lift the refusal.
6. A resume start part of the way into a 48 kHz FLAC lands at the second
   asked for (the "[audio] FLAC: starting ..." line), and the position then
   counts on from there.

## 6b. Checked on the device

The run was overnight on 2026-10-01, on the Core2 v1.3 on COM3, with this
tree's firmware (v0.5.0-dev+c9f3c7d-dirty; the final image is ELF
e440d1ec). Everything ran on the speaker in silent mode `z`; the
headphones were asleep (below).

Three temporary console hooks were built in for the run and removed after
it; none of them is in the tree:

- `Rp<sec>`: a probe of the output's tap (frequency from interpolated zero
  crossings, peak, RMS, tap frames per second);
- `Rt<n>s<sec>`: a test track started that far in;
- `Rk`: a microbench of the kernel's loop.

**In short:**

- **Correct.** Pitch, level, frame counts, positions and starts are
  exact. There were 0 underruns in 40 minutes and no burst at any start.
- **Too slow.** A 48 kHz track costs 22 % of a core at 240 MHz and 32 % at
  160, against an estimate of 8-10 % and 12-14 %. The passthrough path
  for 44.1 kHz tracks costs 3-5 % (the old per-sample path was never
  measured). At 160 MHz a 48 kHz tone takes list scrolling from 29 fps
  to 9.
- **Step 4's gate fails**, so step 5 follows, and it is two jobs (item 1
  below).

### 1. The bench (`Rb`)

Each line is 10 s of stereo noise through `RingOutput`'s own converter on
the decode task, with the output dropped. "% of a core" is at the clock
the bench ran at.

| Source | Route | 240 MHz | 160 MHz |
|---|---|---|---|
| 44.1 kHz | passthrough | 7.7 M cycles/s, 3.2 % | 7.8 M, 4.9 % |
| 48 kHz | 147/160 | 53.3 M, **22.2 %** | 51.3 M, **32.1 %** |
| 96 kHz (off) | halfband, then 147/160 | 107.6 M, 44.8 % | 106.3 M, 66.5 % |
| 88.2 kHz (off) | halfband | 74.2 M, 30.9 % | 75.0 M, 46.8 % |
| 32 kHz | x3/2, then 147/160 | 94.1 M, 39.2 % | 92.9 M, 58.0 % |
| 24 kHz | x2, then 147/160 | 126.9 M, 52.9 % | 89.9 M, 56.2 % |
| 22.05 kHz | x2 | 74.4 M, 31.0 % | 41.9 M, 26.2 % |
| 16 kHz | x3, then 147/160 | 89.1 M, 37.1 % | 88.0 M, 55.0 % |
| 12 kHz | x4, then 147/160 | 105.7 M, 44.0 % | 86.6 M, 54.1 % |
| 11.025 kHz | x4 | 55.2 M, 23.0 % | 39.2 M, 24.5 % |
| 8 kHz | x6, then 147/160 | 98.6 M, 41.1 % | 85.5 M, 53.4 % |

- Two runs at each speed agreed to within 0.3 M cycles.
- 48, 32, 16, 88.2 and 96 kHz take the same cycles at both clocks. The
  x2, x4 and x6 routes (8, 11.025, 12, 22.05 and 24 kHz) take 13-37 M
  more cycles at 240 MHz than at 160. That points at a cost in time rather
  than in cycles: flash-cache misses or wait states, which depend on where
  the linker puts the code and the U12 table. This isn't explained yet.
- The low rates cost more than 48 kHz because they run two stages; their
  decoding costs less in proportion (section 3).

**Where 48 kHz's 53 M cycles go.** The temporary `Rk` ran a copy of the
kernel's loop (the same source, the same code from GCC) for 44,100 stereo
outputs of 96 multiplies each:

| The table in | 240 MHz | 160 MHz |
|---|---|---|
| flash (as built) | 34.5 M cycles, 8.15 per multiply | 34.9 M, 8.23 |
| flash, one row only (no walk through the table) | 33.1 M, 7.81 | 33.4 M, 7.88 |
| internal RAM | 30.4 M, 7.19 | 30.7 M, 7.26 |
| PSRAM | 30.6 M, 7.23 | 30.8 M, 7.28 |

- **The table's memory hardly matters**: internal RAM is only 12 % faster
  than flash. Section 8's fallback, copying the table into internal RAM,
  isn't worth 7 KB of heap.
- **The loop runs at 7-8 cycles per multiply**, about 2 cycles per
  instruction (3.6 instructions per multiply). The estimate assumed
  roughly 1: each `l16ui` feeds the next `mula.aa.ll`, and every 8 taps
  the accumulator is written and read back.
- **The other ~19 M cycles (about 390 per source frame) are the per-frame
  path**: `process()`, `push()`, `runRoute()` and `pushPoly()` for every
  source frame, `maxOut()` each time, 64-bit counters, and the history
  writes, each followed by the PSRAM workaround's `memw`. The passthrough
  alone costs 7.7 M cycles (175 per frame), on every 44.1 kHz track.

So **step 5 is two jobs**:

- the MAC16 kernel, which loads as it multiplies (esp-dsp's dot product
  runs at about one multiply per cycle): about 34 M cycles towards 5-10 M;
- a block path: one call per block that writes the history and computes
  every output in it, instead of four calls per frame: about 19 M towards
  a few M.

Both of those are estimates again; `Rb` measures them.

### 2. Pitch and level (section 6, check 3)

Each tone was a 1 kHz tone at -18 dBFS, played with `Rt`. `Rp` started
3 s into the track and read the speaker's tap (the 44.1 kHz ring frames
that played) for 8 s, about 8,000 crossings.

| Source | Measured | Peak | RMS |
|---|---|---|---|
| 8 kHz | 999.9999 Hz | -18.00 dBFS | -21.01 dBFS |
| 11.025 kHz | 999.9999 Hz | -18.00 | -21.01 |
| 12 kHz | 999.9998 Hz | -18.00 | -21.01 |
| 16 kHz | 1000.0001 Hz | -18.00 | -21.01 |
| 22.05 kHz | 999.9999 Hz | -18.00 | -21.01 |
| 24 kHz | 999.9999 Hz | -18.00 | -21.01 |
| 32 kHz | 1000.0002 Hz | -18.00 | -21.01 |
| 44.1 kHz (passthrough) | 1000.0005 Hz | -18.00 | -21.01 |
| 48 kHz | 1000.0005 Hz | -18.00 | -21.01 |

- The old path would have played the 48 kHz tone at 1,088.4 Hz over
  Bluetooth. The level is unchanged at every rate (peak 4,125-4,126).
- 88.2 and 96 kHz were refused ("is off in this build"), as built. They
  weren't measured: the gate stays closed, and no build with it open was
  made. 37.8 kHz was refused with "isn't supported".
- **Probe pitfalls** (the probe's, not the converter's):
  - A probe that straddles a track switch reads 999.88 Hz: the phase
    jumps between the two tracks.
  - Probes started before a 32 or 8 kHz tone lost tap frames. The UI loop
    was starved for over 0.37 s during the tone's start refill.
- **Test tracks aren't paced.** `produceTone()` only yields
  `vTaskDelay(1)`, so a 32 or 8 kHz tone's refill takes most of core 1.
  Files are paced (`RefillPacer`).
- The tap's clock read 44,081-44,146 frames per second: M5.Speaker's I2S
  rate is about 0.05 % fast (section 5, "The speaker").

### 3. 40 minutes of 48 kHz (section 6, check 2, extended)

The track was `tone:silence@48000` on the speaker, in silent mode. The
headphones (SPYDRONE) didn't answer, so the soak wasn't over Bluetooth.

**30.3 minutes at 240 MHz:**

- 0 underruns. The ring held 1,450-1,485 ms (at least 1,450 every 5 s).
- The decode load was 25.2-25.7 %: the tone generator plus the
  conversion. The internal heap's minimum was 73 K.
- The position against the host's clock: 1.00001.
- `R` at the end: 87,472,128 source frames taken, 80,364,996 ring frames
  made. ceil(taken x 147/160) less made = 22, the filter's delay, still in
  it. 0 samples clamped.

**10 minutes at 160 MHz:** 0 underruns, the ring at least 1,445 ms, a
decode load of 36.1 %, and the position at 0.99998 of the host's clock.

### 4. Scroll stress (`w1`)

Each run was 25 s with a tone playing in silent mode, on the 10,000-track
synthetic library (`g10000`, artists). The build's defaults applied:
hardware scroll, paced refill, a 30 fps cap and flicks at 2,000 px/s.

| Tone | 240 MHz: fps p10 / p50 / min | Frame max | Decode p50 / max | 160 MHz: fps p10 / p50 / min | Frame max | Decode p50 / max |
|---|---|---|---|---|---|---|
| 44.1 kHz | 28.0 / 29.9 / 25.7 | 65 ms | 5 / 5 % | 23.7 / 29.3 / 22.7 | 82 ms | 7 / 7 % |
| 48 kHz | 17.9 / 22.4 / 16.8 | 96 ms | 26 / 29 % | **7.6 / 8.7 / 6.8** | 157 ms | 37 / 44 % |

- All four runs had 0 underruns, with the ring at least 1,439 ms.
- For comparison, ENERGY.md step 6a measured a 44.1 kHz MP3 alone at
  12.9 fps at 240 MHz and 6.7 at 160 (decode 42 % and 55 %).
- A 48 kHz MP3 would add the conversion to that: about 64 % of core 1 at
  240 MHz and about 87 % at 160. These are sums of loads measured apart,
  not measured together; there is no 48 kHz file on the card.
- Step 4's gate (decode plus convert under about 45 % at 160 MHz) fails by
  a wide margin.

### 5. The output switch (section 6, check 4): not run

The headphones were asleep. They didn't answer 3 pages at each boot or a
further burst (`Pr1`). The check waits for the user.

- **Checked:** the ring holds 44.1 kHz from every rate (item 2 measures
  the ring's frames), and BtSink reads 44.1 kHz. So the bug's cause is
  gone by construction.
- **Not checked:** Bluetooth's side, on the device.

### 6. A start part of the way in (section 6, check 6, on a tone)

The temporary `Rt0s20` asked the backend for `tone:1000@48000` from 20 s:
`play(path, 0, 20000)`, the call a resume start makes.

- It logged "built-in track: 0:20 asked: counting from there".
- The position read 21.021 s one second later and 25.572 s at 5.6 s.
- The track ended at exactly 30.000 s: 480,000 source frames, 441,000
  ring frames, nothing left in the filter.
- The first 50 ms peaked at the tone's own level: no burst.
- **Not checked:**
  - The queue's `qs` path itself: it would need a test track in the
    listener's queue.
  - A 48 kHz file's resume (an MP3 byte or a FLAC sample, in source
    units): there is no 48 kHz file on the card.

### 7. The dancer

The Dance tab, with `tone:1000@48000` playing:

- the tracker reset once at the start and stayed unlocked (a steady tone
  has no beat);
- no further resets, 0 lost;
- 2.5-6 % tracker load, the same as with the 44.1 kHz tone (4-5 %).

The tracker's input is the 44.1 kHz ring at every rate. The tempo itself
couldn't be judged: the click tracks are 44.1 kHz only (`ToneTrack`), and
no 48 kHz click track was made for the run.

### Fixed during the run

- **`R` after a refused built-in track** said "no track at a known rate".
  `Core2AudioBackend::failRate()` now publishes the rate first, so `R`
  says "37800 Hz: refused (...)".

## 7. The plan

**Step 1: the tables. Done.**

- `tools/gen_resampler_tables.py` writes `lib/core/ResamplerTables.cpp`:
  D147, U12, HB96 and HB88, each with its design in a comment.
- The test checks the committed tables by their properties (section 3).

**Step 2: the converter. Done.**

- `lib/core/RateConverter.h/.cpp`: `plan(rate, cpuMhz)`, `reset()`,
  `setRate()`, `setMono()`, `maxOut()`, `push()`, `process()`,
  `finishPush()`/`finished()`, the counters (`taken()`, `produced()`,
  `clamped()`). Pure: no allocation, no Arduino.
- `test/test_rate_converter`, 28 tests:
  - the tables: rows sum to 32768, each half's bound, the responses;
  - plans and refusals; `maxOut()` per route; a refused rate takes nothing;
  - bit-exact passthrough; every route against an independent reference;
  - exact counts for any length; long runs through a full ring; output
    independent of chunking;
  - DC exact, silence to zeros, the worst input and full-scale squares
    saturating without a wrap;
  - reset; mono = stereo with equal channels; channels independent; a
    channel change never resets; frames before the rate (MP3's order); a
    stream that never says its rate; the same rate again; a rate change
    mid-stream;
  - the passband, THD+N and spurs, aliasing, time alignment, the size.

**Step 3: the backend (src). Done** (the list below is the plan as written;
section 9b says where the build differs).

- `RingOutput`: holds a `RateConverter`; the rule in section 5;
  `finish()`; counters of ring frames; `SetRate()` refuses through `plan()`
  with the stored CPU setting; the `only44k` argument goes;
  `static_assert(sizeof(RingOutput) < 4096)`.
- `Core2AudioBackend`:
  - `out_->reset()` right after `discardAll()` in `start()`, for every
    request kind;
  - every rate in section 4 goes to 44100;
  - `producedFrames_` counts ring frames, and the room check is in ring
    frames;
  - tones go through `RingOutput::process()` with an offset into `chunk_`,
    and the `tone:<what>@<rate>` paths are parsed;
  - the failure text says why ("isn't supported", "needs the 240 MHz CPU
    speed"), and the route is logged at track start.
- `TrackCatalog`: the unlisted rate tracks. `SerialConsole` and `main.cpp`:
  `R`, `Rt` (an isolated queue), `Rb`, plus `b`'s decode-plus-convert
  figure.
- The build guards (iram_diet, flash_guard, version) must pass: about
  12 KB of flash with the code, and no IRAM.

**Step 4: the device bench, then decide. Done (section 6b): the gate
fails.** At 160 MHz a 48 kHz track's conversion alone takes 32 % of a
core (37-44 % with the tone generator, under the scroll stress), and list
scrolling falls from 29 fps to 9. Check 4 (Bluetooth) and check 5
(hi-res) haven't run. As planned:

- Run checks 1-3 and 5-6 of section 6 (check 4 when the user is awake).
- **The gate is what the listener would notice, not a percentage.** On a
  48 kHz MP3 at 160 MHz, in 10 silent minutes: 0 underruns; the
  `[audio] refill` timings and the UI's stall at a skip within about 20 % of
  a 44.1 kHz MP3's; decode plus convert under about 45 % of the core. Core 1
  also runs the speaker pump, M5's mixer and the UI, and `RefillPacer` runs
  the fill flat out below 500 ms of ring. If that holds, the C kernel stays.

**Step 5 (only if step 4 fails): the MAC16 kernel.**

- A `.S` file in `src/audio/` (flash, not IRAM) using
  `mula.dd.ll/hh.ldinc`, with a 40-bit accumulator and saturation at the
  end. `ldinc` needs 32-bit alignment, so it keeps a second history copy one
  sample over and picks the copy where the window starts aligned; the
  reversed rows need `lddec` and swapped lanes. About 4 % of a core saved at
  48 kHz.
- Then, and only then, the `Rc` self-test: it converts a fixed
  pseudo-random input at each rate and prints a CRC-32, which
  `test_rate_converter` checks too, so the assembly matches the C kernel to
  the bit. For the C kernel alone (no undefined behaviour) it would only
  re-prove what the host tests prove.

**Step 6: hi-res.**

- With check 5 passing (a 24/96 FLAC with no underruns at 240 MHz), turn
  on 88.2/96 kHz, and settle the 160 MHz rule from its measurement.

**Docs, as each step lands:**

- ARCHITECTURE.md: "Audio pipeline" (the diagram, the "Bluetooth is 44.1 kHz
  only" bullet, positions in ring frames), "Tasks and cores" (the decode
  task's load), and the roadmap's item 5.
- POC-RESULTS.md, review item 7. This document's estimates, replaced with
  the device's.

## 8. Risks and open questions

- **CPU on the device: measured (section 6b), two to three times the
  estimate.** The C loop takes 7-8 cycles per multiply wherever its table
  is (copying the table into internal RAM gains 12 %, so that fallback is
  off). The per-frame path costs about 390 cycles per source frame. Step 5
  must cut both. Still open: why the x2, x4 and x6 routes take up to 40 %
  more cycles at 240 MHz than at 160.
- **Why the 441-row table went.** The first design had a direct 441-row
  upsampling table for 8/16/32 kHz (21.2 KB). That is larger than one 16 KB
  way of the 2-way 32 KB cache that the decoder's code and PSRAM data share
  on core 1, its rows are visited in stride-M order, and at worst it would
  miss about 3 lines per output (about 400 cycles each at DIO 80 MHz): up to
  about 15 % of a core. It was also too big to copy into internal RAM. U12
  is 672 bytes, and every route but the halfband-only and x2/x4 ones reads
  one hot table, D147. The cost: twice the multiplies for 8-32 kHz sources,
  whose decoding costs proportionally less, and 1-5 dB of THD+N.
- **A 24/96 FLAC at 240 MHz** may still underrun: 37 % decode plus an
  estimated 14-17 % conversion, next to the UI. The cause may be the SD
  card, not the CPU (check 5 logs it apart). Step 6 is gated on it.
- **22.05-25.95 kHz content** in 48 and 96 kHz sources aliases to
  18.15-22.05 kHz, as little as -19 dB down near 20 kHz for a full-scale
  tone. That is above adult hearing and outside what lossy encoders keep;
  a hi-res file's ultrasonic noise is far below full scale. K = 64 strict
  fixes it for a third more cost.
- **Upsampled low-rate sources**: content in the top 12.5 % below the
  source's Nyquist images at only 7-50 dB down (the transition band; low-rate
  MP3s are low-passed below it).
- **The 88.2 kHz halfband's stopband** is -76 dB in Q15 (-89 dB
  unquantised).
- **A mid-file rate change** restarts the filters: there may be a click,
  never louder.
- **The speexdsp figures are from a model** of its algorithm, not its code.
  The decision doesn't hinge on them: our filter's own measured figures
  meet every requirement at a fraction of the multiplies. But anyone who
  reconsiders vendoring it must re-measure against `resample.c` (and add
  its BSD-3 notice to THIRD-PARTY-NOTICES.md and LICENSES/).
- **No third-party code.** The tables come from our own generator. esp-dsp's
  assembly is reference reading only: a MAC16 kernel would be written fresh.

## 9. The design review's amendments

The design was reviewed against the code before the DSP core was built.
Every amendment was taken; two were taken with a change.

| amendment | taken | where |
|---|---|---|
| `Rt` must not queue its track ahead of the listener's music; the Bluetooth switch check waits for the user, or a queue of only silence | yes | section 6 (`Rt`, the device checks) |
| Reset the converter right after `discardAll()`, for every request kind | yes | section 5, "Resets"; step 3 |
| Say what happens to frames before the first `SetRate()`; test against the real call order | yes | the hold and replay in `RateConverter`; section 5; `test_frames_before_the_rate_are_kept` runs MP3's order, `test_the_same_rate_again_changes_nothing` FLAC's |
| Never reset on `SetChannels()`; always write both histories | yes | `setMono()`; `test_a_channel_change_never_resets`; the risk is gone from section 8 |
| Contiguous halves, one int32 sum each, planar histories, always-inline, no `-O2` pragma; re-estimate | yes | the kernel; checked on `RateConverter.cpp` itself with the firmware's compiler at `-Os`: 3.6 instructions per multiply; 8-10 % / 12-14 % at 48 kHz |
| Gate step 4 on what the listener would notice; MAC16 conditional; `Rc` only with it | yes | section 7, steps 4-5 |
| Drop the 441-row table; upsample through small pre-stages into 48 kHz, then D147 | yes, with a change | one 12-row table serves x2, x3, x4, x6 and x3/2 (and 22.05/11.025 kHz direct) as every 6th, 4th, 3rd or 2nd row: 672 bytes instead of five tables |
| Define a cascade's end exactly; test the streamed count without trimming | yes | `finishPush()`; every count test uses the converter's own tail |
| Device checks that can see a wrong ratio | yes | section 6: `R`'s 160:147, the zero-crossing pitch check, 60 s of ring time and `millis()` |
| The hi-res rule from the stored CPU setting; the clock does change live; log SD time; the longer cushion | yes | section 4 ("The 160 MHz rule"); check 5; section 5. `plan()` takes the MHz from its caller |
| Count the halfbands unfolded: 63 and about 88 multiplies | yes | sections 3-4 |
| Tones through `RingOutput`: room in ring frames, an offset into the chunk | yes | `process()` returns the frames taken; section 5 |
| Guard internal-RAM placement | yes | section 5; the host test bounds `sizeof(RateConverter)` (1,544 bytes on the ESP32) |
| Test the tables by their properties; no byte-equality against a fresh Python run in CI | yes, with a change | the tests check properties; `--check` stays for a local run on one machine, not in CI |

## 9b. Step 3 as built: where it differs from the plan

| plan | as built | why |
|---|---|---|
| `RingOutput` holds the converter and the rule | `lib/core/RingFeed` holds them, `RingOutput` is a thin `AudioOutput` around it | the rule, the stage and the tail are pure, so they are host-tested against the real `PcmRing` (test_ring_feed: bit-exact and exact counts at every rate through a full ring, MP3's call order, refusals, the 240 MHz rule, resets, blocks, mono, the budget, silence) |
| `plan()` gets PowerSettings' stored CPU setting | it gets `PowerSettings::cpuBootMhz()`, set once at boot | the stored value can differ from the running PLL until a restart (`Pcb` saves without one); the boot speed is what the CPU runs at, and `Pc80` doesn't change it |
| `AudioShared::rate` kept as a field, always 44100 | removed, `AudioShared::kRingRate` instead | one truth: nothing can publish another rate by mistake |
| `Rt` plays its track "in a queue of its own, or with a stop flag" | the player is stopped first (`PlaybackController::stopKeepingPlace()`), then the backend plays the path directly | `PlaybackController::update()` only advances while the player is Playing, so a test track that ends or fails is followed by nothing; the queue isn't touched. The listener's place isn't either: a paused or playing track's second becomes a start point, so it stays the saved resume point and the next play picks up there (`Rb` and `b<n>` stop the same way). While it plays, the Queue's learned lengths aren't noted (main.cpp now notes them only while the player plays) |
| `Rt` refuses non-silence on Bluetooth | that, and a tone only in silent mode `z` (section 6: "silent mode only") | the user may be asleep; silence plays anywhere |
| `Rt<n>` | also `Rt<tone:...>`: any built-in path, e.g. `tone:silence@37800` to see a refusal, `tone:silence@96000` at 160 MHz | the refusal paths can be tried without a file |
| `Rb` bench | 10 s of noise per rate through `RingFeed::write()` with its output dropped (`setDiscard()`), timed with `esp_cpu_get_cycle_count()` per block; 88.2/96 kHz are measured at 160 MHz too and while they are off (planned as at 240, with hi-res on) and marked "not played: it ..." with the reason | the 160 MHz rule is provisional until measured |
| `b<n>`: decode plus convert | a second pass over the same stretch through `RingOutput` (output dropped), only for a file at another rate; it prints both and the difference | the difference is the converter's cost in the decoder's company (cache misses included) |
| `Rc` | not built | only with a MAC16 kernel (step 5) |
| The failure text says why | `[audio] 37800 Hz isn't supported (8-48 kHz, 88.2 and 96 kHz)` in the log and `note()`; the toast too: "Skipped ...: 37.8 kHz isn't supported", or "...: needs the 240 MHz CPU speed" (`IAudioBackend::rateRefusal()` -> `PlaybackController::Failure::rate` -> `Ui::noteFailures()`; `uitext::kSkippedRate`/`kSkippedCpu`, measured in test_ui_library) | a refused rate isn't a broken file, and at 160 MHz a setting would play it |
| 88.2/96 kHz on at 240 MHz | off at any speed until the device check (section 6, step 5): `RateConverter::kHiResOn`, set by `-DMSTREAM_HIRES_RATES=1` | the plan's own gate: a 24/96 FLAC underran on the speaker before any conversion; the routes and their tests stay, and the benches still convert them |
| The budget counts source frames | it caps ring frames too (`RingFeed::setBudget()`; `write()`'s `maxMade` for the built-in tracks) | an 8 kHz pass would otherwise convert ~10x a 48 kHz pass's worth above the UI loop |

Not changed: the default queue. It is still the library followed by the
nine listed built-in tracks (README, "Using it"); the rate test tracks are
unlisted, like `tone:silence`, so they never join it.
