# Opus on the Core2: the M0 bench gate, M1's reader, M2's playback, M3's seeks, M4's faster starts

Status: **M0's gate ran and passed where it had to; M1 (the reader,
complete) and M2 (playback from the library, the G5 and G6 fixes, the
notices) are built; M2's device checks ran and passed (section 8.11), and
their four follow-ups are built: the table copy's PSRAM block is the
default, G6 is 30 ms, `pass_max` is the heard track's, the resync after a
damaged page is made in steps; M3 (section 9) is built: seeks and resume
points land on the exact sample by the reader's plan, the resume anchor
is saved and taken (the NVS blob's kind 3), and the seek bar is live on
Opus; its device check ran and passed but for the latency it recorded
(9.6's S5: 270-460 ms to first audio on the 128k-class files) and the
damaged-file step (F2dmg); M4 (section 10) is built for both: the open
cache (a seek on the playing track opens with one read; `/.player/opus.idx`
persists it), the tail scan's 16 KB window checked in memory, no header
read at a probe's guess, Q kept in hand and the decode after a page's
last slice as a step of its own (the seek preroll stays M3's 200 ms: its
review's re-measure over 500 starts a file, 10.7, where M4's 160 ms had
one seed's 50 behind it); its review's findings are fixed (10.7); its
device check is the next session's (10.5).** M0 was
the gate build the research report asked for
before anything else of Opus is built: the playback path end to end,
playable and benchable by path from the console, with the logging the nine
gates need and a card set to run them on. The gate's device numbers are in
section 4: the speed (G1), the stack (G4), the lengths (G9) and the soak
(G2, G3) passed; the internal-RAM floor (G5) and the longest pass (G6) did
not, and M2 fixes both (section 8); the MP3 bench's slowdown (G7) turned out
not to be Opus's. M1 (section 7) completed the portable reader: a chained
file's exact length, the gap plan and the gap fill after a damaged page,
the plan for a start part of the way in (the bisection and the preroll)
and the resume anchor, each host-tested and checked on the real files; its
review's findings are fixed (section 7.5). M2 (section 8) made Opus a
format of the player: the library lists `.opus`, so it plays from the
queue, over Bluetooth, and joins the next track gaplessly; the generator
is complete (the concealment of malformed packets, the refusal of frames
under 10 ms); a decode pass ends on time; the decode stack is back at
16 KB; the notices, the licence file and the README's Opus section with
its patent note are in; its review's findings are fixed (section 8.10);
its device checks passed (8.11: G5 at 56 KB steady with the table copy in
PSRAM, G6 at 10.3-26.5 ms (the stereo CELT files 16.6-26.5) under the
30 ms the gate asks now, the lengths,
the joins, the refusals, the library, the 20 min soak), and the four
follow-ups they asked for are built (8.11). M3 (section 9) wired the plan
M1 made into the firmware: a `qs`, the seek bar's tap or drag and the boot's
resume point start an Opus track on the exact sample asked (the research's
6.5: a bisection by the pages' granule positions, a 200 ms preroll for a
seek and 600 ms for a resume point), a pause's anchor is the FLAC model's
(`ResumeAnchor::Kind::Opus`, the NVS blob's kind 3, which an unknown kind
now survives with its position), and `IAudioBackend::seekable(path)` no
longer refuses `.opus`: the seek bar shows its knob on an Opus track as on
an MP3; its device check passed (the M3 results in the scratchpad's
`opus/gate/results`), with S5's latency and F2dmg's step as the open
items M4 (section 10) takes up: a 128k seek is expected at ~165-215 ms
after the request (M3 measured 287-453) by section 10's model. Still to
come: M4's device check (10.5) and the soak. Section 2
is how the gate was run (two parts, one needing nobody and one needing the
headphones linked), section 3 the card set it ran on.

**Release conditions:** the patent position (the research's section 4.3:
libopus under the IETF royalty-free grants, and the Vectis pool's
assertions against makers of hardware that decodes Opus) was accepted by
the user on 2026-10-04, with the README's note (section 8.7); the release
notes of the first release that carries Opus must carry the same sentence.
Still open: M4's device check (10.5: the latency of every start again,
the open cache's lines, F2dmg's step), the next session's.

The design is the research report's (the scratchpad's `opus/RESEARCH.md`),
in short: our own Ogg Opus reader in `lib/core` (`OggPage`, `OggOpus`:
host-tested, 22 tests, and checked on 55 real files against ffmpeg's libopus
decoder by `tools/opus_check`) drives the libopus that ESP8266Audio bundles
(v1.5.1 sources, fixed point, decoder only, VAR_ARRAYS: compiled by every
build already, linked now). ESP8266Audio's own `AudioGeneratorOpus` is never
included or linked (it overruns its 1 KB packet buffer on ordinary tagged
files, drops 60-120 ms packets, applies half the pre-skip and no end trim).

## 1. What M0 built

| Piece | Where | What |
|---|---|---|
| The generator | `src/audio/OpusGenerator.{h,cpp}` | An `AudioGenerator`: the reader's packets split into frames, one `opus_decode()` call per frame (at most 2,880 samples: a 120 ms packet is six calls, bit for bit the whole packet's output), a refused frame concealed for its duration, the Timeline's pre-skip and EOS trim (the generator trims everything itself: `TrimFeed` is armed `{0,0}` and never sees a lead sample), mono expanded L = R, a family-1 mapping table applied (L = decoded `map[0]`, R = decoded `map[1]`: the channels swapped, or one of them on both sides), `OPUS_SET_GAIN` from the header, phase inversion off on the speaker. The frame goes to the output whole through `ConsumeSamples()`; a pass also ends on its own after a frame's worth of decoded samples (2,880, kept or dropped), 10 decode calls or 16 malformed packets, so a pre-skip or a run of junk packets can't hold the decode task |
| The block path | `lib/core/RingFeed::writeBudgeted()`, `TrimFeed::consumeBlock()`, `RingOutput::ConsumeSamples()` | The converter's block path for a generator's frame, within the pass's two budgets (source frames, ring frames); the same bits as a frame at a time, also mixed with it (host-tested at 48, 44.1, 8 and 96 kHz, mono too). The bench's `CountingOutput` keeps the per-frame path |
| The arena | `lib/core/DecoderArena` | Per-claim layouts: layout 0 libmad's 25,056 B, layout 1 the Opus state (`opus_decoder_get_size(2)`) + the frame's 11,520 B of PCM; the block, allocated at boot in the PSRAM's fast lower half, is sized to the larger. One decoder at a time, as before (`PinnedMp3` unchanged) |
| The backend | `src/audio/Core2AudioBackend` | `.opus` prepared by the generator's open (the headers and the tail scan: the rate for a join's continuity and the exact length before any frame; a refusal's sentence is the track's note), `codec_` "Opus", the early-end hook from the generator (a file cut short, another stream after ours, an EOS page promising more than it held), `kDecodeStack` 16,384 -> 20,480, the longest pass per track (`s`: `pass_max`), the bench's Opus lines, the `O` placement knob |
| The console | `src/main.cpp`, `src/app/SerialConsole` | `b</path>` benches a file by its path (the library doesn't list `/bench/opus/`); `Rf</path>` already took any path and now plays an `.opus` (silent mode `z` only, as before); `O` / `Ol` / `Oi` / `Oh` the placement knob |
| The card set | `tools/opus_check/cardset.py` | Builds `/bench/opus/` (section 3) |

Unchanged: the MP3 and FLAC paths (their generators, trims, run index,
anchors; `b<n>` benches them exactly as 0.6.0 did, for G7), `LibraryIndex`
(`.opus` stays "Other": an Opus file plays by path only), `TrackSeek`,
`SEEK.md`, `ResumeAnchor`, `NvsLayout`, the UI.

### The decode loop, as built

One pass of the decode task (`produceDecoded()`, budget 1,024 source frames
and 1,024 ring frames) calls `OpusGenerator::loop()`, which: hands the
frame in hand over (`ConsumeSamples()`: `RingFeed::writeBudgeted()`), and
if the output took all of it decodes the next frame, until the output
refuses (the ring full, or the budget spent: the rest of that frame is
offered again next pass), the pass's own caps are met, or the track ends.
The caps (`kPassSamples` 2,880 decoded samples, kept or dropped;
`kPassCalls` 10; `kPassMalformed` 16) are there because the output's
budget only bounds what reaches it: inside a 3,840-sample pre-skip four
frames are decoded and thrown away whole, and a page of 255 empty packets
is 255 reader steps with nothing to decode; without the caps one pass ran
all of them (6 decode calls, ~40 ms, for that pre-skip; a crafted file of
a million empty packets in one call). A frame is one `opus_decode()` of at
most 20 ms of CELT/hybrid or 60 ms of SILK; the timeline
(`oggopus::Timeline`) says which of its samples are audio (`Keep{skip,
take}`), and after a packet's last frame `packetDone()` puts the count at
the page's granule. The end is the EOS trim reached (`tl.finished()`) or
the reader's End (`Eos` natural; `Truncated` or `Chained` early; `Eos`
with the trim not reached early too: the page promised more than it held).

The reader's own bounds, so a crafted or badly damaged file costs seconds
at most, not minutes of SD reads inside one call on the decode task: after
a damaged or lost page the next good page of ours is looked for within
1 MB and 2 MB of reads (past that the track ends as truncated; a header
with the right version and serial but a wrong CRC claiming a 65 KB page
would otherwise cost a page's read per 282 bytes of file), the first audio
page within 128 KB and 512 KB of reads of the headers, and each tail-scan
window's walk may read four times the window. A candidate header that lies
inside the scan's chunk is judged there (its version, serial and, when the
page is wholly in the chunk, its CRC) with no read of its own. A granule
position over 2^40 (726 years at 48 kHz) is refused as the first audio
page's and counts as none anywhere else (the timeline's sums would
overflow), and a one-page (EOS) file whose granule is under the pre-skip
is refused as RFC 7845 section 4.5 says, with "damaged Opus start".

### Memory

| What | Where | Size |
|---|---|---|
| Page buffer, packet buffer, frame scratch | PSRAM, at the first `.opus`, kept | 65,307 + 61,440 + 1,276 = 128,023 B |
| Decoder state + frame PCM | The pinned block (layout 1) | `opus_decoder_get_size(2)` (26,520 B on Xtensa, aligned 26,528) + 11,520 = 38,048 B; the block grows from 25,056 to 38,048 B |
| The generator object (reader ~500 B, timeline, frame table) | Internal RAM, once | ~1 KB |
| Decode-task stack | Internal RAM | 20,480 B (+4,096) in M0's gate build, to measure; back to 16,384 in M2 (section 8.2) |
| The converter's table copy (M2, `Ot1`: the default since 8.11) | A PSRAM block pinned next to the arena's | 7,776 B (internal RAM with `Ot0`; section 8.2) |

### The build

Measured on this host from `firmware.map` (`[env:core2]`; `core2-dio` is
within 64 B of flash), the baseline at 0a15e64 (the reader committed, not
linked: byte for byte 731da0a's image) against the review-fixed M0 build,
the input sections of the map summed per object (so a merged string pool
lands on whichever object the linker credited: the per-object figures are
within a few hundred bytes):

| | Before | After | Delta |
|---|---|---|---|
| IRAM (`.iram0.vectors` + `.iram0.text`) | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` + `.dram0.bss`) | 56,040 B | 56,152 B | +112 B (`.bss`: the generator's pointer, the knob, the longest pass, the refusal string) |
| Flash, the image (`Flash:` line) | 2,228,411 B | 2,321,063 B | **+92,652 B** (the first M0 build was 2,320,119 B; the review's bounds and caps are the 944 B on top) |
| ... `.flash.text` | 1,509,976 B | 1,577,720 B | +67,744 B |
| ... `.flash.rodata` | 568,212 B | 593,120 B | +24,908 B |
| libopus: the `libESP8266Audio.a` objects newly in the map (63, named by source: `celt_decoder.c.o`, `dec_API.c.o`, `kiss_fft.c.o`, ...; no `AudioGeneratorOpus`) | none | 54,903 B text + 20,767 B rodata = **75,670 B**; `.data`/`.bss` 0 | |
| Ours: `OpusGenerator` 5,035 B; `OggOpus` 4,718 B; `OggPage` 2,483 B (its 1 KB CRC table included); the backend 1,585 B; the console (`main`) 1,276 B; `RingFeed`, `SerialConsole`, `DecoderArena` 663 B | | **~15.8 KB** | |

Against the research: libopus 75,670 B here against the 74,957 B of its
standalone `--gc-sections` link (the rodata exactly its 20,767 B; the text
713 B over), and 0 IRAM/DRAM as it said, so its +75 KB holds; our code
15.8 KB against its 5-8 KB estimate for the reader and generator (the
reader 7.2 KB, the generator with its logging 5.0 KB, the rest the
backend's and the console's wiring). The app is 2.27 MB of the 6 MB slot.
`iram_diet` (51 of 51 objects moved), `flash_guard` and the version check
pass on `core2` and `core2-dio` (`core2-dio` is 64 B larger, as before).

## 2. How to run the gate (the console; silent mode `z` first)

One device session runs it on the HEAD of `feature/opus` (the code of
29ff5f0; this plan changes no code), flashed with `pio run -e core2 -t
upload` from PowerShell with `MSYSTEM` removed, the card set of section 3
on the card, and the serial daemon holding COM3. The session's runbook,
`opus\gate\run_gate.py` in the same scratchpad as the card set, drives the
console through the daemon (it appends to `dev_cmd.txt` and reads
`dev.log`; it never opens COM3), parses the log into section 4's table
(`results.md`, with the detail tables under it) and stops on a Guru, a
panic, an `abort()`, a stack overflow, an unexpected reboot or a play not
in silent mode. `--dry-run` runs it against a simulated Core2 on a fake
log (a pass, a slow image, a crash in the fuzz bench, headphones that
never link, a play that leaves silent mode) and checks its verdicts and
its safety rules; `--report` merges the two parts' runs into one table.
What it sends is below, gate by gate, so a session without it can type
the same.

- **Silent mode after every boot.** `z` (a key, no Enter) as soon as the
  `[console]` line is up, before anything plays, and its answer `[test]
  silent mode: speaker muted, bluetooth won't take over` waited for. `Rf`
  is refused without it. Silent mode keeps the ring on the speaker at
  volume 0: Bluetooth, even linked, reads nothing of the ring and sends
  zeros (`BtSink::onData`), and a link that comes up answers `[test]
  silent mode: staying on the speaker`. Every `[stats]` line of a play must
  say `out=speaker(silent test mode)`.
- **By path.** The angle brackets in this document are notation: the
  console takes `b/bench/opus/ms128k.opus` and `Rf/bench/opus/ms128k.opus`
  (`b<n>`'s argument is a path when it starts with `/`; `R`'s `f` is
  followed by the path). `b` benches a file into nothing (no sound,
  whatever the mode); `Rf` plays one on its own, with nothing after it.
  The fuzz file is never played: bench only.
- **Never `f`.** It forgets the headphones and restarts. Every line the
  runbook sends starts with `b`, `R`, `O` or `P`, every key is `z`, `s`
  or `d`; a reset is the daemon's `!reset` (an RTS pulse), written raw.

The bench prints, in this order:

```
[opus] open: 2 ch, pre-skip 312, gain 0, input 44100 Hz, g0 0, tags 155 B in 1 pages, length 209920 ms (exact, from the last page), 9 reads / 24576 B in 12 ms
[opus] decoding: /bench/opus/ms128k.opus, 209920 ms long; the state pinned at 0x3f808e80, PSRAM, its lower 2 MB
[opus] bench, decode only: not at its end (stopped: the bench's 20 s, or a stop or skip); ~1003 packets, ~1003 decode calls (0 concealed), 0 malformed, 0 gaps; kept ~963000 samples at 48 kHz, SHORT of the exact length by ~9113000 (10076160); pages ...; timeline corrections 0 (slip 0); longest decode call N us, longest loop() N us; decode stack free since boot N B
[opus] state: pinned at 0x3f808e80, PSRAM, its lower 2 MB
[bench] /bench/opus/ms128k.opus: 20.0 s of 48000 Hz audio in 6.xx s = 3.xx realtime (3x.x% of a core), decode stack free N
[opus] open: ... (the second open, for decode + convert)
[opus] bench, decode + convert: ...
[bench] /bench/opus/ms128k.opus: decode + convert to 44100 Hz (147/160) in 7.xx s = 3x.x% of a core at 240 MHz: the converter x.x% in the decoder's company
```

(The 20 s bench stops the track before its end by design: its `[opus]
bench` lines say so and show `kept ... SHORT of the exact length`; the
`[opus] end:` line of a whole play (`Rf`) is where `exactly the length` and
gate G9 are read. The bench's `longest loop()` is one of its 4,096-frame
bursts, longer than a playing pass, and `longest decode call` is one
frame: neither is G6's figure, which is `s`'s `pass_max`, the whole pass.
Every `decode stack free` figure, in the `[bench]` and `[opus]` lines and
`s`'s `stack_free=`, is `uxTaskGetStackHighWaterMark()` of the one decode
task made at boot: the lowest free since boot, not that file's.)

### 2.1 Two parts

The gates split by what they need. Part a needs nobody; part b needs the
listener's headphones linked. Each starts from a fresh boot (`!reset`),
and the order inside each is set by the figures that are lowest-since-boot
(the decode stack, `min=`) and by G7's need for a boot with no `.opus`
opened yet. The times are the runbook's on its simulated Core2, whose
benches take as long as the research's estimates and whose plays are real
time.

| Part | Needs | Runs, in order | Time |
|---|---|---|---|
| a | nobody; the headphones in their case, so nothing links (the benches as RESAMPLER.md section 10d ran them) | a boot, `z`; `O` (the knob's state); G7: One More Time, Stronger, One More Time, Stronger; G1: `ms128k` and `ms96k` at `Ol`, `Oi` and `Oh`, `Ol` back; the other 12 benches (`f2p5` `f10` `f60` `f120` `cbr510k` `mono64k` `hyb32k` `silk16k` `pic` `ytdl` `ms192k` `ms128k_2`); the 5 edge benches and `Rf` on `/bench/opus-edge/vorbis.opus` (refused at the open, so nothing decodes); every real file but `long128k` played to its end, `R` after each (G9, G6, G4: 18 plays, 28.4 min of audio); `s` (G4's real-file figure); the fuzz bench, last (G4's fuzz figure); `Pcb160`, a boot, `z`, the 128k and 96k benches (G8); `Pcb0`, a boot, `z` | ~36 min |
| b | the headphones linked: out of their case near the Core2, the phone's Bluetooth off | a boot, `z`, the link (`[bt] connected`, the boot's pages; `Pr1` pages them again, twice, before it gives up); `d` (the Dance tab, `[dance] on` checked with `s`) and `long128k.opus` played to its end with `Ps1` every 15 s to keep the screen (and so the dancer) lit: G2 over its first 10 min, G5 from its `[heap] playing` line (the first play of the boot) and `ram=`/`min=`, its G9; the album's 4 parts one after another, the Dance tab up, for 30 min (G3); `d` off, `Pcb160`, a boot, `z`, the link, 10 min of `long128k.opus` without the Dance tab, `Rx` (G8); `Pcb0`, a boot, `z` | ~55 min |

**Bluetooth in M0.** M0 can't play an Opus file over Bluetooth: `Rf`
needs silent mode, and silent mode keeps the ring on the speaker. Part b
is the closest M0 gets to the research's "over Bluetooth": the headphones
linked (the Bluetooth stack's RAM held, its radio up, AVRCP), the stream on
the muted speaker, the headphones sent nothing but zeros. What it leaves
out is the A2DP stream's own cost (SBC on core 0, its buffers): G2's and
G5's figures over a real stream wait for M2 (section 6). The runbook
records whether the link held for each play (headphones that switch
themselves off when idle drop it; it pages them again between plays).

### 2.2 Gate by gate

| # | Measure | Commands | Reads from |
|---|---|---|---|
| G1 | x realtime, decode only and decode + convert, 128k and 96k, three placements | `b</bench/opus/ms128k.opus>`, `b</bench/opus/ms96k.opus>`; then `Oi` and both again, `Oh` and both again, `Ol` back | the two `[bench]` lines (decode + convert is given as % of a core: x = 100 / %); `[opus] state:` says where the state really was (a placement with no room falls back to the block, logged: `[opus] no N B block of internal RAM for the state ...: the pinned block instead`) |
| G2 | `load=` playing 128k 10 min, the Dance tab up | part b: `d`, `Rf</bench/opus/long128k.opus>` (14 min: the real tracks are 3:30 and `Rf` plays one file with nothing after it), `s` every 30 s, `Ps1` every 15 s | `[stats] ... load=x%` over the first 10 min (`pos=` up to 600 s): the median and the max. `load=` is the track's decode and convert time over the audio made since its start |
| G3 | the 30 min soak with the gapless album on repeat and an Opus/MP3 join | needs the library index and the queue (M2): not runnable in M0 as written. What runs, part b: `Rf` on `/bench/opus/album/01.opus` ... `04.opus` one after another (each to its end, a stop and a start between them), the Dance tab up, for 30 min | `[stats] underruns=` (counted since boot: the rise over the 30 min), `buf=` after each part's ring first reaches 1,000 ms and up to its `[opus] end:` line (the drain after it is on purpose), and no `[E]` line |
| G4 | decode-stack free | The figure is the lowest free since boot (one decode task, made at boot), so the order matters: from a fresh boot, every real file benched and played first (the reading after the last of them is the real files' figure), then `b</bench/opus/fuzz.opus>` last (its reading is the fuzz figure; loud noise if ever played, so bench only). A real file after the fuzz one shows the fuzz file's figure: restart first | pass: >= 3,072 B after the real files, >= 1,536 B after `fuzz.opus`. The final `kDecodeStack` is the most used (20,480 - the lowest free) + 3 KB, rounded up to 1 KB |
| G5 | `[heap] playing` | part b, the headphones linked: the first play after the boot (`long128k.opus`) logs `[heap] playing` once, 2 s in; `s`'s `ram=` while playing, `min=` (`heap_caps_get_minimum_free_size()`, the lowest since boot) at the end. Part b has no `Oi` bench (G1's internal-RAM placement holds the 38 KB state in internal RAM and would lower the minimum for the rest of the boot) and no fuzz file | steady: the median `ram=` playing; the session min: the lowest `min=` of part b's 240 MHz boot |
| G6 | the longest pass | part a: every real file played to its end (`f120.opus` included), part b's plays too | `pass_max=Nus` (<= 20,000 at M0; <= 30,000 since 8.11), the track's longest pass: the whole pass, `loop()` and the converter's `commit()`. The bench's `longest decode call` bounds one frame and its `longest loop()` is a 4,096-frame burst: neither is this figure |
| G7 | MP3 and FLAC on this image | From a fresh boot, before any `.opus` is opened (an Opus open keeps 128 KB of PSRAM buffers for good, which moves libFLAC's mallocs: RESAMPLER.md section 10d saw layout alone move FLAC 4.7x -> 4.3x): `b</music/Daft Punk/Discovery/01 - One More Time.mp3>` and `b</music/Kanye West/Graduation/03 Stronger.flac>` by path (the `b27` / `b2` tracks of ENERGY.md section 10 and RESAMPLER.md section 10d, named as the earlier sessions' logs have them), twice each. No queue command is needed (and `qp`/`q+` would replace the listener's queue) | the `[bench]` lines (and `[bench] libmad's state: pinned at ...`), against 4.9x / 4.7x |
| G8 | 160 MHz | `Pcb160` and a restart (`Pc160` at runtime is refused from 240 MHz: it would retune the PLL the Bluetooth radio runs from; `Pc` alone shows the clock), `z`; part a: `b</bench/opus/ms128k.opus>` and `b</bench/opus/ms96k.opus>`; part b: `Rf</bench/opus/long128k.opus>` for 10 min without the Dance tab, then `Rx`; `Pcb0` and a restart after each | `[bench]` (`at 160 MHz` in its second line), `[stats] underruns=` |
| G9 | the length | `Rf</bench/opus/<file>>` to the end, then `R`: every real file (part a) and `long128k` and the album's parts again (part b) | `[opus] end: ... kept N samples at 48 kHz, exactly the length (N)` and `R`'s `source frames taken` = N, `ring frames made` = ceil(N x 147/160) with `0 still in the filter` (the decode task publishes the converter's counts after the end's flush pushed its tail, so the figure is whole). The lengths: section 5's |

Also checked on the way, outside the nine: the boot's `[audio] MP3 decoder
state: 25056 B pinned at 0x3f8..., PSRAM, its lower 2 MB (the block is
38048 B: the Opus decoder's 38048 B layout shares it)` (an image without
Opus says 0 B there, and the runbook stops); `O`'s answer (`[opus] the
decoder's state goes to the pinned block (PSRAM, its lower 2 MB) from the
next open; ...`); and the edge files' refusals, each a sentence that
becomes the track's note: `[audio] /bench/opus-edge/vorbis.opus: Ogg Vorbis
isn't supported (only Opus)`, `surround Opus (6 channels) isn't
supported`, `an Ogg file with several streams (video?) isn't supported`;
the chained and the truncated file open (the chained one's length unknown
in M0, the truncated one 20,993 ms from its last complete page).

## 3. The card set

`tools/opus_check/cardset.py` built it (46.3 MB, 20 files) from the
research's files, mStream's own ffmpeg (N-126217, the binary mStream
runs) with mStream's exact transcode arguments (`src/api/transcode.js`:
`-vn -f opus -acodec libopus -ab <rate> pipe:1`), and the research's
continuous PCM of its source track (16-bit 44.1 kHz, 209.92 s). The built
set is at

```
<scratchpad>\opus\cardset\bench\opus\
```

(a session's temp directory, not the repo): copy the `bench\` folder above
it to the card's root while it exists (it holds `opus\` and `opus-edge\`,
below: 27 files, 25 of them `.opus`, 49.4 MB in all), or rebuild `opus\`
with, from the repo root,

```
python tools/opus_check/cardset.py --out <DIR> --research <scratchpad>\opus ^
  --ffmpeg "<mStream>\bin\ffmpeg\ffmpeg.exe" ^
  --flac "<a FLAC>" ^
  --flac "<another FLAC>" ^
  --ytdl "<a yt-dlp .opus download>"
```

where `<scratchpad>\opus` is the research scratchpad above (its
`fit\media\` holds `killers.pcm` and the frame-size, mono, hybrid and SILK
files, `integration\files\` the 60/120 ms and picture files) and the
FLACs and the yt-dlp file are the research's own inputs (`opus\value\
sources.txt`, `opus\value\ytdl\list.txt`). Rebuilt that way on
2026-10-02 the set came out the same byte for byte, but for the Ogg
serial numbers ffmpeg draws at random for each transcode (and the page
CRCs that follow from them), so results tie to the files whichever copy
ran. `README.txt` in the set lists every file. In short:

| File | What |
|---|---|
| `ms96k.opus`, `ms128k.opus`, `ms192k.opus` | mStream transcodes of the research's source track (3:30) |
| `ms128k_2.opus` | mStream transcode of its second track |
| `long128k.opus` | the source track four times over (839.7 s) as one mStream transcode at 128k: the 10 min plays of G2 and G8 (`Rf` plays one file with nothing after it, and the real tracks are 3:30) |
| `ytdl.opus` | a real yt-dlp download (YouTube's Opus stream-copied) |
| `f2p5.opus`, `f10.opus`, `f60.opus`, `f120.opus` | 2.5 and 10 ms frames; 60 and 120 ms packets |
| `cbr510k.opus` | 510k CBR, 60 s (1,276 B packets) |
| `mono64k.opus`, `hyb32k.opus`, `silk16k.opus` | mono; hybrid; SILK (VoIP) |
| `pic.opus` | a 1.9 MB picture tag |
| `fuzz.opus` | 2,134 random packets in every TOC configuration x stereo x code, ~4 % malformed, in real pages with CRCs, an EOS trim: bench only |
| `album/01.opus` .. `04.opus` | the source track cut into 4 contiguous parts on sample boundaries, each an mStream transcode at 128k: the gapless album for G3 (M2) |

Every file was checked on the host with `tools/opus_check/opus_check.py
run` before it went in (section 5).

Next to it, `/bench/opus-edge/` holds five of the research's own files
(`opus\integration\files\`), copied by hand as they are, two renamed to
`.opus` so the player opens them as Opus (3.1 MB, its own `README.txt`).
They are benched only, for the refusals and the odd files' opens; the
reader's verdict on each is from `tools/opus_check`'s runner on the host:

| File | Source | The open |
|---|---|---|
| `vorbis.opus` | `vorbis.ogg`, renamed | refused: "Ogg Vorbis isn't supported (only Opus)" (also played once by `Rf`: refused at the open, its sentence the note) |
| `surround51.opus` | `surround51.opus` | refused: "surround Opus (6 channels) isn't supported" |
| `muxed.opus` | `muxed.ogv` (Opus and a video stream), renamed | refused: "an Ogg file with several streams (video?) isn't supported" |
| `chained.opus` | `chained.opus` (two Opus streams, one after the other) | opens; its length unknown in M0 (a chained file's length is M1's); the first stream plays |
| `trunc.opus` | `trunc.opus` (cut at 300,000 B, no EOS page) | opens; 20,993 ms from its last complete page; it ends early |

## 4. Results on the device

The gate session ran the two parts on the image of b6415d3 (the review-fixed
M0 build) on 2026-10-04, through the runbook; its `results.md` has the
benches, the plays, the boots and the edge files under this table. The pass
marks and the actions on a fail are the research's (section 8, M0); the
measures are M0's versions of its gates (section 2), all at 240 MHz but G8.
A verdict is PASS or FAIL; MARGINAL for G1 between 2.0x and 2.5x (the
research's middle band; a G2 fail takes G1's actions); RECORDED for G8, a
policy rather than a pass; INCOMPLETE when a gate's run fell short.

| # | Measure | Pass | Fail -> | Result | Verdict |
|---|---|---|---|---|---|
| G1 | `b` decode + convert, mStream 128k and 96k, the state pinned low; decode only and the internal and high placements recorded | >= 2.5x realtime (<= 40 % of a core) | 2.0-2.5x: go on only with the research's section 3.4 planned before release, and the user's OK. < 2.0x: NOT NOW (or 3.4 first, then the gate again) | 128k: 3.3x decode only, 35.2 % of a core decode + convert = **2.84x**; 96k: 3.6x, 33.2 % = 3.01x. Internal RAM 2.87x / 3.04x (no better: the state's reads are a small share); the upper PSRAM half 2.25x / 2.35x (keep the state pinned low) | **PASS** |
| G2 | `load=` playing `long128k.opus` with the Dance tab, 10 min (part b: the headphones linked, the ring on the muted speaker) | median <= 55 %, max <= 65 % (MP3 today 39-43 %; the UI-starving case is ~64 %) | as G1 | median 44.9 %, max 44.9 % over the first 599 s (the A2DP stream's own cost waits for M2) | **PASS** |
| G3 | 30 min with the Dance tab: the album's 4 parts by `Rf`, one after another (part b); the gapless joins, the album on repeat by the queue and the Opus<->MP3 join wait for M2 | 0 underruns; the ring never under 1,000 ms; no Guru, panic or `[E]` | investigate; no M2 | 33 plays, 30.5 min: 0 underruns, the ring never under 1,111 ms, no error line | **PASS** |
| G4 | decode-task stack free at `kDecodeStack` 20,480 (`b`'s lines, `s`'s `stack_free`) | >= 3,072 B on every real file; >= 1,536 B on the fuzz file | raise the stack, or the research's section 3.5 (the pseudostack) | real files 8,392 B free at the lowest; the fuzz file 7,480 B (18 malformed packets dropped, no crash): the most used is **13,000 B** of 20,480 | **PASS** |
| G5 | internal RAM playing with the 48 kHz table copy (`[heap] playing`, `ram=`, `min=`; part b: the headphones linked, the ring on the muted speaker) | steady >= 50 KB (POC-RESULTS.md); session min >= 42 KB | ask the user: accept a lower floor, or section 3.5 with the scratch in the pinned PSRAM block (and G1 again) | steady **44 KB** (`ram=` playing), session min 44 KB; `[heap] playing` free 45 KB, largest block 39 KB | **FAIL** (M2: `kDecodeStack` back to 16,384, which G4 supports with 3,384 B to spare; then the table copy's placement if still short) |
| G6 | the longest decode pass (`s`'s `pass_max`), every file played | <= 20 ms on every file, the 120 ms one included | fix the split or the budget | 22-79 ms: 128k 33.9, 96k 29.0, 192k 45.9, ytdl 37.9, 10 ms frames 22.3, 60 ms 30.9, 120 ms 31.4, 510k and mono **79.3**, hybrid and SILK 23.1, the album's parts 30-41; `pass_max` may also not reset per track (the mono file's 79.3 is the 510k file's figure) | **FAIL** (M2: a pass ends on elapsed time, <= 15 ms, not frames alone; `pass_max` per track) |
| G7 | MP3 and FLAC `b` on this image (One More Time, Stronger, twice each), from a boot, before any `.opus` | within 3 % of 0.6.0's 4.9x / 4.7x | investigate the layout before M2 (75 KB more code can move the cache layout: RESAMPLER.md section 10d) | MP3 3.8x, 3.7x (-23.5 %); FLAC 4.7x, 4.7x (0 %) | **FAIL, not Opus's**: v0.6.0's own image gives the same MP3 figure (a separate investigation owns it) |
| G8 | 160 MHz: `b` on 128k and 96k (part a), then 10 min of `long128k.opus` without the Dance tab (part b) | recorded: 0 underruns -> Opus allowed at 160 MHz, documented "UI slow" as a 48 kHz MP3; any underrun -> Opus refused at 160 MHz with a note, as 88.2/96 kHz | a policy, not a stop | 128k 2.4x decode only, 49.7 % decode + convert (2.01x); 96k 2.5x, 47.1 % (2.12x); 10 min playing: 0 underruns, load median 58.6 %, max 60.1 % | **RECORDED: Opus allowed at 160 MHz**, "UI slow" |
| G9 | the lengths on the device (`[opus] end:`, `R`), every file played to its end | every track's kept samples = its exact length; ring frames = ceil(N x 147/160), 0 left in the filter | a bug: fix before M2 | 18 files in part a and 34 plays in part b: every one exact | **PASS** |

G1 and G8 by placement and clock (x realtime decode only; decode +
convert in % of a core, and as x):

| | `ms128k.opus` | `ms96k.opus` |
|---|---|---|
| `Ol`: the pinned block (PSRAM, its lower 2 MB) | 3.3x; 35.2 % (2.84x) | 3.6x; 33.2 % (3.01x) |
| `Oi`: internal RAM | 3.4x; 34.8 % (2.87x) | 3.6x; 32.9 % (3.04x) |
| `Oh`: PSRAM above 0x3FA00000 | 2.6x; 44.5 % (2.25x) | 2.7x; 42.6 % (2.35x) |
| `Ol` at 160 MHz (G8) | 2.4x; 49.7 % (2.01x) | 2.5x; 47.1 % (2.12x) |

The other files, decode + convert at 240 MHz: 192k 2.63x, 10 ms frames
2.24x, 60 and 120 ms packets 2.80x, 510k CBR 1.89x, mono 4.35x, hybrid
2.40x, SILK 7.35x, the picture file 2.88x, yt-dlp 2.82x; **2.5 ms frames
1.00x and 2,616 underruns in a play** (refused with a note, M2: frames
under 10 ms). G4's final `kDecodeStack` (the most used + 3 KB, rounded up
to 1 KB): **16,384**, M0's value before the gate build raised it.

## 5. Checked on the host

The card set through `tools/opus_check/opus_check.py run` (the reader and
the bundled libopus built for the host; every packet decoded frame by frame
as the generator does and whole by a second decoder; ffmpeg's libopus
decoder as the reference):

| File | Trimmed length (samples) | = ffmpeg's | = the tail scan's | Per-frame = whole-packet | SNR to ffmpeg (dB) |
|---|---|---|---|---|---|
| `ms96k`, `ms128k`, `ms192k` | 10,076,160 | yes | yes | bit for bit | 62-66 |
| `ms128k_2` | 9,881,144 | yes | yes | bit for bit | 47 |
| `long128k` | 40,304,640 | yes | yes | bit for bit | 64 |
| `ytdl` | 9,964,488 | yes | yes | bit for bit | 42 |
| `f2p5` (24,003 frames), `f10`, `cbr510k`, `hyb32k`, `mono64k`, `silk16k` | 2,880,000 | yes | yes | bit for bit | 59-68 (SILK: identical) |
| `f60` (3 frames a packet), `f120` (6), `pic` | 1,440,000 | yes | yes | bit for bit | 47-50 |
| `album/01`-`04` | 2,519,040 each | yes | yes | bit for bit | 62-67 |
| `fuzz` | 3,949,628 | no: ffmpeg gives up after 178,608 | yes | bit for bit | (garbage) |

The SNRs are fixed point against ffmpeg's float decode (a few LSB on CELT;
the SILK file is identical). `pic.opus` opened with 23,815 B of reads for
its 1,862 KB of tags in 30 pages. `fuzz.opus` decodes all 6,184 of its
frames (the largest packet 52,579 B, 86 malformed packets dropped, the EOS
trim reached: kept 3,949,628 = the tail scan's length); ffmpeg stops at
its first undecodable packet, by design.

## 6. What M0 left, and where it went

- The library doesn't index `.opus` (`LibraryIndex` v3, the badge, the
  empty-state text): **M2 (section 8.4)**. Opus plays from the queue now, so
  G3's queue-driven soak and the Opus<->MP3 join are M2's device checks.
- Seeks and resume points in the firmware: **M3 (section 9)**, on the plan
  M1 made (section 7). (Until it, a start asked part of the way in was from
  0:00, logged, and the seek bar showed no knob on an Opus track.)
- Gap fill after a damaged page: M1 (section 7). Concealment of malformed
  packets for their duration: **M2 (section 8.1)**.
- `Rf` is silent-mode only, so nothing Opus has played over Bluetooth yet:
  G2's Bluetooth load, G3 and G5 as written are **M2's device checks
  (section 8.9)**.
- THIRD-PARTY-NOTICES.md, `LICENSES/BSD-3-Clause-libopus.txt`, the README's
  format list and patent note: **M2 (section 8.7)**.
- The 160 MHz policy from G8: allowed, "UI slow" (**M2 documents it**,
  section 8.3).
- G5 and G6 (section 4): **M2 (section 8.2)**, checked on the device next.

## 7. M1: the reader, complete

M1 finished `lib/core/OggOpus` (its class comment is the design): what
the research's section 8 lists for it, each host-tested in test_ogg_opus
(30 tests at M1, 38 with its review's fixes, 25 in M0) on synthetic files
and checked on the research's 43 real files by `tools/opus_check`
(section 7.3). No device work: the firmware builds (section 7.4), the
plan's wiring into the backend is M3's.

### 7.1 What it built

| Piece | What |
|---|---|
| Continuity and serials | As M0 had them (a damaged or lost page drops the packet it cut, a sequence gap is a gap, pages of other serials are stepped over, a BOS page after ours ends the link), with the link's end now noted (`Reader::linkEnd()`) |
| A chained file's exact length | When the tail windows (the last 8 KB, then 73 KB) hold no page of ours (another link's pages fill the tail, or a junk tail longer than the window), a bisection by serial number finds where our link ends (a page of ours starts at `lo`, none at or after `hi`: a BOS page there, junk, or the file's end; the links are one after another, RFC 3533), then the window before it is walked as the tail is: ~10 probes of a 16 KB chunk for a 50 MB file. The research's `chained.opus` (two whole streams, the second filling the tail) now opens with its first link's length, 1,440,000 samples, exact (M0 said "unknown"), and plays it to its EOS page |
| The gap plan | The first packet after a damaged or lost page says where it starts on the absolute timeline (`Packet::startK`: its page's granule less the TOC samples of the packets completing on the page from it on; its own from its assembled bytes, so a packet that began on the gap page and completes later is placed right), and `Timeline::gap()` is the fill owed before it. A gap the reader can't size (a page without a granule, a malformed packet on it) is put right at the page's end by the granule, as M0 did everything. On the EOS page the trim is unknown after a gap: its packets are kept to its granule and the fill is shorter by the same count, so the track's length holds either way. A gap over `kMaxGapSamples` (10 s) ends the track |
| The gap fill | `OpusGenerator` makes the fill before decoding the packet: libopus's concealment for up to two frames of the last frame's size (its PLC fades the last frame out), then silence, a frame's worth a `loop()` step (the pass caps count it), through the same keep and expansion as a decoded frame (`made()`); over 10 s ends the track with its own note. The log's `[opus] end:` line counts the gaps filled and their samples |
| The start plan | `Reader::planStart(target, preroll)`: G = target + g0 + preSkip, P = G - preroll; an interpolated bisection by page headers (our serial, a believable granule; the tail scan's last granule stands in for the upper bound until a probe gives one; a plain halving every third probe) to the last page Q with granule <= P, then forward by headers to the last such page. The first packet completing after Q starts at granule(Q), so reading starts after Q (at Q itself with its own packets stepped over when a packet continues out of it: `ogg::Header::continues`, `StartPlan::skipPage`); `Timeline::wanted()` skips packets ending at or before P undecoded, the one holding P and all after it are decoded, the samples before G dropped. A target inside the first page or the first preroll, at or past the known end, starts from the top with the pre-skip (RFC 7845 section 4.6) and drops up to G. `planStartMs()` applies the tail rule first (`trackseek::startMs()`, now inline in TrackSeek.h so the host tool links the reader alone). Bounded: 48 probes, 64 header steps a probe, 1 MB of reads a plan (past them the plan starts earlier than it need: still exact). `startAt(plan)` positions the reader, `Timeline::start(plan)` the timeline; the generator then resets the decoder (`OPUS_RESET_STATE`), M3 |
| The prerolls | `kSeekPrerollMs` 200, `kResumePrerollMs` 600 (the research's 6.5; section 7.3 measures them) |
| The anchor | `ResumeAnchor::Kind::Opus` (3), the FLAC model's: `oggopus::makeAnchor(sample, fileSize, lengthSamples)` (exact, 48 kHz, frameHash the exact length's low 32 bits, the byte fields 0) and `checkAnchor()` (the kind, the size, the length, which a length not known refuses, the tail rule). `NvsLayout::decodeResume()` still refuses kinds above FLAC: M3's, with the unknown-kind rule |
| The seek bar | Now Playing shows a knob and takes touches whenever the length is known, the track hasn't failed and it is 10 s or longer, and an Opus open gives an exact length, so until M3 a touch would restart the track from 0:00. `IAudioBackend::seekable(path)` joins those three: false for an `.opus` path, and the player asks it of its current entry's path (`PlaybackController::seekable()`), so a track joined gaplessly (no `play()` of its own), one cued, or the entry restored at a boot each get their own answer, never the last request's (M1 had `play()` set a flag from the request's path, and M2's review found a joined track inheriting the track before's; section 8.10); `AppState::seekable`, NowPlayingPage's rule, SEEK-BAR.md. M3 turns it back on |

### 7.2 Host tests (test_ogg_opus, 30 at M1; 38 with section 7.5's)

Added or changed: the gap fill in the harness (`play()` fills `gap()`,
skips by `wanted()`, counts what was decoded before the first kept
sample); a damaged page and a lost page now keep the exact length with
no correction and no jump, and the first packet after says 48,000
(test_damaged_and_lost_pages); a lost middle page with packets spanning
it fills the two it took (test_continuation_rules); the gap plan's edges:
a malformed packet on the page after the gap (put right at its end, as
before), the page before the EOS page lost (the length holds, the end is
reached), 12 s of damaged pages (ended) and 9 s (filled)
(test_gap_plan_edges); a chained file whose second link is 120 KB (the
bisection: the length, the link's end, a plan that never probes into the
second link, the cut-before-EOS case) (test_chained_length); a 200 KB junk
tail (test_long_junk_tail); the start plan at every packet boundary and
the sample on either side of it on a 60 s VBR-ish file (8,997 plans:
every one lands on its sample; the samples decoded before it are the
preroll and under a packet more; probes mean 4.64 max 8, reads mean 17.8
max 32, 61 KB a plan with the play's page read at M1; 19.7 and 34 reads,
75 KB, once Q is read whole as well, section 7.5), a file whose every page
continues a packet (the skip-page path), a target in the first 80 ms,
the resume preroll, the tail rule through `planStartMs()`, a length not
known (test_start_plan); the anchor's round trip and each field's
corruption refused (test_anchor); `ogg::Header::continues`
(test_page_header).

### 7.3 Checked on the host (tools/opus_check)

`opus_check.py run` on the research's 43 files (the card set, the edge
files and the integration files): 0 failures. Every length equals
ffmpeg's and the tail scan's, every split is bit-exact, as in M0
(section 5); new: `chained.opus` gives its first link's 1,440,000 samples
(ffmpeg plays both links, 2,880,000: noted, not a failure), and the fuzz
file's "ffmpeg stopped at 178,608" is noted as what it is. `opus_check.py
corrupt` on `ms128k.opus` at byte 2,000,000: one page lost, 48,000 samples
filled, 0 lost of the length, no correction.

`opus_check.py seek` (new: 50 random starts a file at each preroll,
decoded for the 4,096 samples after the target against the decode from
the top): **every one of the 2,150 starts landed on its sample.** CELT
files (mStream's, yt-dlp's, the album, the frame-size and 510k files):
with the 600 ms resume preroll bit-exact on all 50 starts of every file
(the two synthetic `exact441` transcodes: 49 of 50, the other within
4 LSB, and 10 of 50, the rest within 1 LSB); with the 200 ms seek preroll
54-68 dB SNR at worst and 73-181 dB on average, but album part 4's 37 dB
(a quiet passage: the error is 40 LSB at most). SILK and hybrid files
(the research's 6.5 measured CELT) never converge bit for bit: 41-46 dB
at worst with either preroll. The rules the check applies: landed
exactly; worst SNR >= 35 dB; a CELT-only file's resumes within 4 LSB.
Cost per start: probes mean 2.0-5.5 (max 16 on the 510k files, whose
64 KB pages take sixteen 4 KB chunks to cross), reads mean 5-25 (the
510k files 44-53), 6-106 KB (the 510k files 246-289 KB); the research's
prototype measured 3.3-3.7 probes and 29-38 KB a seek on the mStream
files, in line with these (4.3-5.1 probes, 47-65 KB with the play's own
page read).

### 7.4 The build

Measured as section 1's table was (the input sections of `firmware.map`
summed per object), against the merged seek-bar baseline b44a26c:

| | b44a26c | M1 | Delta |
|---|---|---|---|
| IRAM (`.iram0.vectors` + `.iram0.text`) | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` 24,328 + `.dram0.bss` 31,832) | 56,160 B | 56,160 B | **0** (the backend's `seekable_` byte fits the padding; the generator's fill state is on the heap with it) |
| `.flash.text` | 1,581,420 B | 1,583,168 B | +1,748 B (the reader's probes, plan and gap plan; the generator's fill) |
| `.flash.rodata` | 593,804 B | 593,948 B | +144 B (the log texts) |
| Image (core2, `Flash:` line) | 2,325,447 B | 2,327,339 B | +1,892 B |
| Image (core2-dio) | 2,325,495 B | 2,327,403 B | +1,908 B |

`iram_diet` (51 of 51 objects moved), `flash_guard` (38 % of the slot) and
the version check pass on both; the host suite is 1,071 tests, all green.

With the review's fixes (section 7.5), against M1 the same way:

| | M1 | Review fixes | Delta |
|---|---|---|---|
| IRAM (`.iram0.vectors` + `.iram0.text`) | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` 24,328 + `.dram0.bss` 31,832) | 56,160 B | 56,160 B | **0** (the generator's `passYield_` byte fits the padding) |
| `.flash.text` | 1,583,168 B | 1,583,892 B | +724 B (Q's check, the probes' CRC reads, the chunked tail walk, the per-call share) |
| `.flash.rodata` | 593,948 B | 593,948 B | 0 (no new text) |
| Image (core2, `Flash:` line) | 2,327,339 B | 2,328,063 B | +724 B |
| Image (core2-dio) | 2,327,403 B | 2,328,127 B | +724 B |

`iram_diet` (51 of 51), `flash_guard` (38 % of the slot) and the version
check pass on both as before; the host suite is 1,079 tests, all green.

### 7.5 The review's findings, fixed

Two review passes over M1 (an RFC lens and a robustness lens) found the
same handful of holes from different sides; each is fixed in the reader
with a host test that failed before it (test_ogg_opus, 38 tests now):

| Finding | What was wrong | The fix | Test |
|---|---|---|---|
| A page lost right after Q | `startAt()` reset the sequence check, so a page cut out right after Q was never seen as a gap: the first packet handed over was labelled granule(Q) when it started a page later, and the landing was late by the lost page (its intact audio skipped as preroll) or the count short by it | `Probe` and `StartPlan` carry Q's sequence number; `startAt()` seeds the check with it when reading starts after Q (Q itself seeds it when it is read). The gap is then sized as any other | test_lost_page_after_plan |
| Headers believed without their CRC | The probes read headers only, so a damaged granule field (its page's CRC wrong) could become Q and land a seek 0.5-2.5 s off; a BOS flag set by damage on a page of ours made every later plan treat the file as chained there (one seek then read 655 KB) | Q is read whole and checked (its CRC, its granule as the header said) before it is believed: the page found before it stands in when it fails, the first audio page past that (earlier, still exact; one page read more a plan, 14 KB on the 60 s file). A probe whose granule is under the page before it (granules never go down) is read whole: damage is stepped over, a real page (a later link under our serial) ends the range. A BOS page is read whole before it is a link, in the probes and the tail walk alike | test_damaged_probe_headers |
| The walk outside the plan's budget | The forward walk after the bisection wasn't counted against the 1 MB plan budget: 64 steps with a 147 KB probe budget each, ~10 MB on a crafted file of pages separated by junk | Each probe, the walk's included, gets what is left of the plan's budget, up to its own; the walk stops when it is spent (an earlier start, still exact) | test_plan_walk_budget |
| A start at or past the known end | The plan said "from the top" but kept from G past the end: the whole track decoded, nothing kept, the end reached as if natural (a 60 min file: ~21 min of silence). `checkAnchor()` compared the tail rule in ms truncated to 32 bits, which let a corrupted sample 2^32 ms in through | The plan is a real top (target 0, keepFrom = g0 + pre-skip), a target past kMaxGranule the same; the anchor's tail rule is in samples | test_start_plan, test_anchor |
| The tail walk's budget on tiny pages | `walkTail()` read a 290-byte header per page against a budget of four times the window: pages of 39-54 bytes (one tiny packet each, as a muxer flushing every packet writes them) ran it out mid-window and the "exact" length came out up to 1.3 s short | The walk parses headers from the chunk of the file in the page buffer (`PageReader::readChunk()`, `headerInChunk()`): a read per 16 KB of pages, whatever their size | test_tiny_pages |
| A second link under our serial | RFC 3533 forbids it, but joined ffmpeg `-fflags +bitexact` outputs do it: the tail walk skipped only the BOS page itself and counted the second link's pages as ours, so the length was the second link's while play stopped at the first | The walk ends at a (checked) BOS page: everything after it is a later link's. A second link longer than the tail windows, or a probe landing inside one, still can't be told apart by serial: unsupported, as the class comment says (play still ends at its BOS) | test_same_serial_chain |
| One `next()` call unbounded | A run of pages that yield no packet (empty pages, another stream's, the pages of one endless packet) was all read inside one call: 100,000 empty pages, 100,002 reads; the generator's pass caps never saw it. The first-granule search in `open()` and the OpusTags skip (65,536 pages) the same | `next()` returns `Pending` after 64 pages or 128 KB of reads that yielded nothing, and the next call carries on; the generator ends its pass on it (`passYield_`). `open()` gives up on `Pending` past 128 KB of file or 512 KB of reads; the tags skip is capped at 4,096 pages (a 256 MB comment header; 1.2 MB of reads at most) | test_next_bounded |
| A seek past a long damaged stretch | A seek to the first good page after more than 10 s of destroyed pages ended the track: the plan's Q was before the stretch, the gap was filled although none of it would be kept, and the fill counted against the 10 s rule | The Timeline steps over the part of a gap before the first kept sample (never heard: no fill made for it) and only what would be heard is left for the 10 s rule. A seek to 32.5 s past pages 20-31 destroyed plays exactly; a target inside the stretch keeps its fill from the target on; from the top the stretch still ends the track | test_seek_past_damage |
| g0 untested in the plan | Every start-plan test had g0 = 0, so a plan that dropped g0 from the target or from its k passed all 30 tests and landed every seek on a cropped-start file wrong | A boundary sweep on a file with g0 = 123,457, an odd pre-skip of 3,841 and ~1 s pages that end inside a packet, every plan played to the EOS trim, then one whose every page continues a packet; both mutations fail it (checked by building the suite against them) | test_start_plan_cropped |

Not a finding after all: a probe already reported a link's end for any
BOS page it walked past, not only the first header it found (the
finding's second half); the fix above adds the CRC check to it.

## 8. M2: firmware playback

M2 made Opus a format of the player, on M0's path and M1's reader: the
library lists `.opus`, so a track plays from the queue and the Library,
over Bluetooth as on the speaker, and joins the next track through the
gapless engine; the generator is complete; the two gates M0 failed are
fixed in the code (G5 and G6, section 8.2) and wait for the device
(section 8.9); the notices, the licence file and the README's Opus section
with its patent note are in. No device work in M2: the firmware builds
(section 8.8), and section 8.9 is the device session's list.

### 8.1 The generator, complete

| Piece | What |
|---|---|
| Mono, the mapping table, the gain, phase inversion | As M0 built them (section 1): a 1-channel decoder expanded L = R in place, family 1's table applied (the channels swapped, or one on both sides), `OPUS_SET_GAIN` from the header, `OPUS_SET_PHASE_INVERSION_DISABLED` on the speaker (decided at the open, by the output then) |
| The gap fill | As M1 built it (section 7.1): concealment for two frames, then silence, a frame's worth a step; a gap over 10 s ends the track |
| The early-end hook | As M0 built it: the generator says whether the track ended before its audio did (a file cut short, another stream after ours, an EOS page promising more than it held, a gap too long to fill), the backend's file-position test is never used for Opus, and the note shows once the track is heard |
| Malformed packets concealed | A packet whose TOC reads but whose framing doesn't (a code-1 packet of an odd length, a code-2 first-frame size past the packet's end: libopus would refuse it too) is concealed for its TOC's duration, as a frame libopus refuses is (two PLC frames of its TOC's size, then zeros), in one fill with any gap before it, so the count runs on as the file says. A packet with no TOC to size it by (0 bytes, a code-3 count of 0, over 120 ms) is dropped, as M0 did with every malformed packet, and the page's granule puts the count right. The one case that needed a rule of its own: a malformed packet concealed for more than the file counted for it (the card set's fuzz file counts its malformed packets as 0 samples; a damaged TOC byte does the same) runs the count ahead, and the page's granule puts it back: the next packet's first samples, up to where the count was, are dropped instead of played twice (`Timeline::packetDone()`'s drop mark), so nothing is heard twice and the length stays exact (the fuzz file's 16 such packets: 16 corrections of -240, the kept count exactly the tail scan's 4,093,508). Host-tested: test_malformed_packets_concealed; the runner mirrors it (section 8.6) |
| Frames under 10 ms refused | At the open, by the first audio page's TOCs (`Reader::Open::ShortFrames`, `oggopus::kMinFrameSamples`): "Opus with 2.5 ms frames isn't supported (10 ms or longer only: too slow to decode here)" (or 5 ms), the track's note. The M0 gate measured 2.5 ms frames at 1.0x realtime with 2,616 underruns in a play (the research's counts: 57-88 M instructions a second against 35 M for 20 ms frames); mStream, ffmpeg and yt-dlp only ever write 20 ms. A file whose first page holds 20 ms frames and a later page shorter ones isn't the open's to refuse: it plays, slowly, and the pass's time rule (8.2) keeps the UI alive. The card set's `f2p5.opus` is the device's check; its fuzz file now starts with a page of four 20 ms packets (cardset.py, regenerated), so it still benches: 2,140 packets, 4,093,508 samples by its tail scan |
| Surround refused | As M0 had it: "surround Opus (6 channels) isn't supported" (the bundled libopus has no multistream decoder); family 255 and 2 uncoupled streams the same, with their own sentences |
| The reader's slice | The audio pages are read in 8 KB slices (`Reader::setReadSlice()`, 8.2); the headers, the tail scan and the plans read whole pages as before |
| The pass's time | `kPassBudgetUs` 15 ms (8.2); the longest step and the longest decode call in the `[opus] end:` line, the longest pass in `s`'s `pass_max` |
| Packets before a plan's preroll | Skipped by their TOCs, undecoded (`Timeline::wanted()`, the generator's `skipped` count): M3's step, wired now so `begin()` on a plan is one line there |

### 8.2 The G5 and G6 fixes

**G5, internal RAM (measured 44 KB steady with the headphones linked,
against the 50 KB floor).** Two changes, one certain and one to measure:

1. **`kDecodeStack` back to 16,384.** M0's gate build raised it to 20,480
   to measure libopus's stack (its scratch is on the stack: `VAR_ARRAYS`).
   G4 measured the most the task ever used at **13,000 B** (the fuzz
   file: random packets in every configuration, 18 malformed, no crash)
   and **12,088 B** on real files (8,392 B free of 20,480 at the lowest
   over 18 files and 34 plays). The research's rule for the final size is
   the measured maximum + 3 KB, rounded up to 1 KB: 16,000 rounds to
   16,384, which is what 0.6.0 had. The margin: **3,384 B on hostile
   input, 4,296 B on real files**, over the 3 KB rule in both; MP3 and FLAC
   use ~3 KB in all and never came near it. So Opus costs no stack over
   MP3's budget after all, and the 4 KB go back to the floor: the static
   estimate is 44 + 4 = **48 KB** steady, still under 50 KB by about 2 KB.
2. **The converter's table copy in pinned PSRAM (`Ot1`): a knob at M2,
   off; the default since the device check (8.11).** Every Opus track is
   48 kHz and so holds the converter's 7,776 B table copy for as long as
   it plays (RESAMPLER.md section 10: read from flash instead, the tables
   cost +13.5 points of a core through cache conflicts with the decoder,
   so flash is no fallback), in internal RAM until M2. M2 gave the copy a
   third place: a 7,776 B block allocated at boot right after the decoder
   arena's, so it lands in the PSRAM's fast lower half as the arena does
   (the boot log: `[audio] the filter tables' pinned PSRAM block (Ot1, the
   default; Ot0: internal RAM): 7776 B at 0x3f8..., PSRAM, its lower
   2 MB`). The next converted track's copy goes there (`[rate] the filter
   tables copied into the pinned PSRAM block (Ot1) (7776 B)`), and the
   internal RAM stays free: 48 + 7.8 = **~56 KB**, over the floor. The copy
   is made at a stream's first rate and kept through a chain of joins as
   before; the knob applies from the next copy, and a copy in the other
   place is dropped at the next request's start (`[rate] the Ot knob moved:
   the filter tables' copy freed`), never inside a chain. Why it was a
   knob first, off: the cost is in the cache, and the device had to say
   what it is. The ESP32's 32 KB cache serves the flash the decoder runs
   from and the PSRAM alike, so a PSRAM copy conflicts with libopus's code
   and tables the way the flash tables did, and a PSRAM miss (40 MHz QSPI)
   costs more than a flash miss (80 MHz QIO); the one measurement that
   bore on it, the decoder's state in the lower half decoding as fast as
   in internal RAM (G1: 2.84x against 2.87x), was of a 26 KB state read
   and written in long runs, not of a 7 KB table read 96 times per output
   sample from a row that changes every sample. The flash case's +13.5
   points would have taken mStream 128k from 35 % of a core to ~49 %, and
   G2's 44.9 % load with the Dance tab to ~58 %. The device check (8.11)
   measured the cost at **+2.8 points** (the converter 8.3-8.5 % of a core
   in the decoder's company against 5.6 %: the copy stays cached, a fifth
   of the flash figure; decode + convert 2.72x against 2.97x) and the RAM
   at **56 KB steady, 55 KB at the least**, against 48 KB and 47 KB with
   the internal-RAM copy, over 10 minutes each with the headphones linked
   and the Dance tab up. So the default flipped: the copy goes to the PSRAM
   block, every converted track's (a 48 kHz MP3's too), `Ot0` is the
   internal-RAM copy for an A/B, and G5 passes with 6 KB to spare.

**G6, the longest pass (measured 22-79 ms against 20 ms).** The passes
were long for two reasons the gate's figures show once read together: the
number of decode calls a pass made (M0 ended a pass on 2,880 decoded
samples, three 20 ms frames, ~20 ms of decoding by themselves), and, the
larger part, **the page reads**: the reader read each Ogg page whole, one
SD read, and an mStream page is a second of audio (16 KB at 128k, 24 KB at
192k, 64 KB at 510k: ~8, 12 and 32 ms of SD time at the card's ~2 MB/s),
inside the pass that turned the page. That is why the figure rose with
the bitrate (33.9, 45.9, 79.3 ms) and why the mono file, 8 KB pages and
4 ms calls, showed the 510k file's 79.3 ms: `pass_max` was read by the
runbook right after the request, while the backend still held the track
before's figure (it was reset at the decoder's begin, on the decode task).
Three changes:

1. **A pass ends on time.** `OpusGenerator::loop()` times every step
   (`decodeNext()`: a page's slice read, a decode call or a fill) and takes
   the next one only while the pass's time so far plus what the last step
   took stays within `kPassBudgetUs`, 15 ms, so a pass is ~15 ms plus the
   output's conversion of its last frame unless a single step is longer
   than that by itself. **The first step of every pass runs whatever the
   last step took** (the rule is `lib/core/PassClock`, host-tested:
   test_pass_clock). A step longer than the budget by itself (an SD
   stall; a page turn at 160 MHz near 15 ms; at M2 the resync after a
   damaged page, which read 32-118 KB in one `next()` call, the scan and
   the page it found, until item 4 made it steps) ends only the pass it is in, and the
   next pass goes on, one step a pass while that lasts. M2's first cut
   asked the rule before the first step too, so one such step stopped
   every pass after it before it stepped, for good (the last step's time
   is only replaced by a step): the ring drained, underruns climbed and
   the track never ended; the review caught it (8.10). The caps
   M0 had (2,880 samples, 10 calls, 16 malformed packets, the reader's
   Pending) stay; the time rule is the one that bites.
2. **The audio pages are read in 8 KB slices.** `ogg::PageReader::read()`
   takes a slice: the header's read, then the body at most a slice a
   call, `Read::Partial` between (the CRC checked once the page is whole,
   as ever; any other use of the buffer in between starts the page over,
   and nothing is read twice). `Reader::next()` says Pending after each
   slice, which ends the generator's pass with nothing in hand, and the
   next pass reads on. The generator sets `kPageSlice` 8,192 (~4-5 ms of
   SD time), so a 64 KB page is eight steps over eight passes instead of
   one 32 ms read; the headers, the tail scan and the plans (synchronous
   by design, ~10 reads) read whole pages as before. The cost: two or
   three reads per mStream page instead of one, ~1 ms a second of audio.
   Host-tested with 64 KB pages (test_sliced_page_reads: the same packets,
   kept count and bytes read as whole reads, seven Pendings a page, a
   damaged page bridged the same way, a restart between slices reading
   on), and the real files by the runner (8.6), which reads in the same
   8 KB slices by default (`--slice N`; 0 reads whole pages).
3. **`pass_max` per heard track** (rewritten after the device check,
   8.11). M2 reset the backend's longest pass at every request's start and
   at each decoder's begin, a join's included. Two things were wrong with
   that: a join's begin reset it while the track before was still heard
   (the ring holds ~1.5 s), so for that stretch `s` showed the next
   track's figure under the heard track's name; and `s`'s figures were a
   snapshot refreshed once a second, so a reading within a second of a
   request still showed the track before's (the check read `mono64k.opus`
   0.6 s after its `Rf` and saw `cbr510k.opus`'s 26,545 us). Now the
   backend keeps two figures: the decoding track's (`maxPassUs_`, reset at
   each begin) and the heard track's (`heardMaxPassUs_`), which `s` shows:
   the same while the track decoding is the one heard, frozen while the
   next decodes ahead, and the joined track's own once its join is heard
   (`takeAdvance()`: what it made decoding ahead, then its passes on; the
   decoder is at most one track ahead, GaplessJoin's one boundary at a
   time, so the decoding figure then is that track's); a request starts
   both over, and the figure is read live, not from the snapshot. The
   heard figure has two writers, so it is raise-only on both sides (a
   compare-exchange max), and `takeAdvance()` raises it to the decoding
   figure once more after setting it: a pass that ends between its load
   and its store isn't put back under (9.7). Every
   codec, and nothing else of MP3 or FLAC touched.
4. **The scan after a damaged page is steps** (8.11). M2 read the resync
   in one `next()` call: the scan for the next good page of ours (4 KB
   chunks, `ogg::PageReader::find()`) and, when that page reached past its
   chunk, the page itself, whole. The device measured the step at
   **55.5 ms** on `ms128k_dmg.opus` (a 16 KB scan and a 16 KB page) and
   **138.9 ms** on `cbr510k_dmg.opus` (54 KB and a 64 KB page), over the
   budget by itself, the one such step left in a play. `PageReader::
   beginScan()` and `findStep()` now make the same scan one read a call: a
   chunk, scanned in memory as before, or, for the page found, a slice of
   it as the audio pages are read; `Reader::next()` says Pending between
   them and the generator's pass ends there as it does between a page's
   slices; the bounds (1 MB of file and 2 MB of reads in a play, 128 KB and
   512 KB at the open) are the scan's as before, and `find()` is the steps
   run to the end. Host-tested: test_page_reader (eight steps from inside
   a 15 KB page to the EOS page with a 4 KB slice, none over a slice and a
   header; five with no slice; a one-chunk budget; a damaged candidate
   stepped over), test_resync_in_steps (the 64 KB pages of
   test_sliced_page_reads with one damaged: the same track and the same
   bytes as whole reads, no call over 8 KB and a header where one call read
   128 KB before, 45 Pendings where the clean file has 28; the 200 KB of
   zeros of test_resync_bounded, fifty chunks each a call; a restart
   mid-scan drops the scan); three older tests' by-hand `next()` loops
   step through the Pendings (`nextPacket()`). The runner reports
   `maxCallBytes` (the most one call read) and `opus_check.py corrupt`
   prints it.

What a pass looks like now, at 240 MHz on mStream 128k: a slice read
(4-5 ms) and a 7 ms decode call in one step, then the time rule stops
the pass (12 + 12 > 15), ~14 ms with the conversion; or two decode calls
(14 ms) when no page turns. At 160 MHz a call is 10.7 ms, so a pass is
one call, ~12 ms, and the audio made per pass halves (the rest between
passes is 1 ms: ~1.6x realtime, enough). The 510k file: 9.3 ms calls, a
slice and a call ~14 ms. MP3 and FLAC are untouched: their generators keep
their own loops and the backend's pass budget (`kChunkFrames`), and the
slice and the time rule live in the Opus generator and the Ogg reader.

### 8.3 The 160 MHz policy

G8 recorded 0 underruns over 10 minutes at 160 MHz (load median 58.6 %,
max 60.1 %; the bench 2.01x decode + convert), so **Opus is allowed at
160 MHz**, documented "UI slow" like a 48 kHz MP3 (README, "Opus"): no
refusal, no check in `RateConverter` (48 kHz needs no `kHiResMinMhz`), no
note on the Output tab beyond the one the CPU speed setting has.

### 8.4 The library and the UI

- `LibraryIndex::Format::Opus`: `.opus` is a track (case-insensitive, as
  the others); `.ogg` and `.oga` stay other files, counted in their folder
  (the decision: index `.opus` only; an Ogg file of another codec would
  only fail at its open). The header comment names the formats.
- The cache's `kVersion` 2 to 3: the signature hashes only the paths, so a
  v2 cache built from the same card would keep `.opus` files as "other".
  A v2 file loads as `Load::Outdated` (new: a good file of an older
  version, not Corrupt), the library's note says "the cache is an older
  version's: rebuilt once", and one rebuild follows; a version from the
  future is Corrupt. test_library_index: `.opus`/`.OPUS` tracks, `.ogg`/
  `.oga` other, the v2 file Outdated. Since the merge with the file-name
  rules (ARCHITECTURE.md, "Names") the version is 5, and 2, 3 and 4 are
  all Outdated; those rules read an `.opus` name as the other formats'
  (test_library_index has the shapes in the three formats together).
- `LibraryPage`: the Folders view's badge is `OPUS` beside `MP3` and
  `FLAC`; the empty state says "MP3, FLAC or Opus, then tap Try again."
- Nothing else names a format: the queue, Now Playing, the resume point
  and the sleep timer are format-agnostic (section 6.12 of the research).
  The artist, album, number and title come from the path, as for MP3 and
  FLAC (no tag reader exists: the OpusTags packet is skipped by its page
  headers; the cover is the folder's `cover.jpg`).

### 8.5 Playback, Bluetooth, the joins, the seek bar

- **From the queue and the Library:** `Core2AudioBackend::prepare()`'s
  Opus branch (M0's) opens the track (the headers, the tail scan), and
  `beginPrepared()` begins the generator; nothing in the play path knows
  the format past that. `Rf` (silent mode only) and `b` by path stay for
  the card set, which the library doesn't list (`/bench/opus/`).
- **A refusal on the screen:** a file the open refuses (surround, frames
  under 10 ms, another Ogg codec) fails the track with the reader's
  sentence in the log and `note()` (`[audio] /music/...: surround Opus (6
  channels) isn't supported`), and its few words on Now Playing's toast:
  `IAudioBackend::failureNote()` (`Core2AudioBackend` keeps them from
  `OpusGenerator::refusalNote()`, `oggopus::Reader::refusalNote()`: under
  40 characters, "surround Opus isn't supported", "Opus with 2.5 ms frames
  isn't supported", "Ogg Vorbis isn't supported") goes with the player's
  failure record (`PlaybackController::Failure::note`) to `Ui::noteFailures`,
  which shows it where "can't play it" went (a refused sample rate still
  says the rate first). Host-tested: test_ogg_opus's refusals, test_playback.
- **Bluetooth:** nothing to build: the ring holds 44.1 kHz whatever the
  track, and an Opus track is converted into it like a 48 kHz MP3 (the
  147/160 block path). The headphones take what they took before. The
  device check plays the album over the link (8.9).
- **Gapless joins** (GAPLESS.md section 4.7): the generator trims itself
  (`TrimFeed` armed `{0,0}`), the open says 48,000 before any frame, so
  Opus to Opus is a continuous join (`RingFeed::continues(48000)`: one
  stream, the first track's EOS trim meeting the next's pre-skip sample
  for sample) and Opus to a 44.1 kHz MP3 or FLAC a rate change (the tail,
  then a new stream), exactly as a 48 kHz MP3 joins today; the early end
  is the generator's hook. The host test for the shape:
  `test_self_trimming_48k_tracks_join_as_opus_does` (test_gapless). The
  sample-exact count on the device is 8.9's.
- **Duration and position:** the length is exact from the open (the tail
  scan), so Now Playing has it before the first frame (`durationKnown()`
  true, no estimate); the position counts ring frames as for every track.
- **The seek bar, until M3: inert for Opus** (section 9 turned it on; this
  is how M2 left it, and what 8.11 checked. The cleaner of the two
  options then: a bar that restarts the track from 0:00 on a touch would
  look like a seek that landed wrong). `IAudioBackend::seekable(path)` was false
  for an `.opus` path, and the UI asks `PlaybackController::seekable()`,
  the backend's answer for the current entry's path, so Now Playing draws
  no knob and takes no touch on an Opus track however it became current:
  started by a `play()`, joined gaplessly after an MP3 (no `play()` of its
  own: the review's case, where M1's flag set by `play()` left the MP3's
  knob on it), cued by a paused next, or restored at a boot (SEEK-BAR.md).
  An MP3 or FLAC joined after an Opus track keeps its knob the same way.
  A start asked part of the way in (`qs`,
  a resume point saved by a pause) plays from 0:00 with the log line
  `[audio] Opus: m:ss asked: seeks aren't built yet (M3; the seek bar is
  off for Opus): from 0:00`, and Now Playing counts from 0:00. M3 turns
  the bar on with the plan (section 7.1).

### 8.6 Host tests and the runner

- test_ogg_opus, 43 tests (42 at M2, 38 at M1's review): the resync after
  a damaged page made one read a call (test_resync_in_steps, and the page
  level in test_page_reader: 8.2's item 4, from 8.11); the sliced read at
  the page
  level (test_page_reader: Partial a slice, the chunk and another offset
  starting the page over, a slice as big as the rest finishing it, the
  CRC judged whole, Short at the slice past the end) and the track level
  (test_sliced_page_reads); frames under 10 ms refused in code-0 and code-3
  packets, 10 ms SILK, hybrid and CELT accepted, a later page's short frames
  not the open's (test_short_frames_refused); malformed packets concealed,
  with a gap before them, dropped without a TOC, and the count put back
  without a repeat (test_malformed_packets_concealed); every refusal's
  screen form (`refusalNote()`, under 40 characters: test_refusals,
  test_short_frames_refused); the probes' floor stepping over a page whose
  granule is damaged low, for eight damaged pages and a plan every half
  second, Q the same page as on the clean file (test_probe_floor_steps_
  over_damage: the review found the floor untested, and the test fails
  with it disabled). The harness's `play()` mirrors the generator's
  concealment.
- test_pass_clock, 6 tests: the pass rule's edges (8.2): the first step
  of a pass always, exactly the budget fits, a step over the budget ends
  its pass only and the next pass steps again (the review's 7, 9, 16.5 ms
  case run to the end: no pass takes nothing), steps each over the budget
  go one a pass, a new track forgets the last step.
- test_playback: the backend's note goes with the failure record and the
  next failure without one clears it; `seekable()` is the current entry's
  (an entry waiting after a boot, a track joined without a `play()`, the
  entry after a skip, no entry).
- test_library_index: test_opus_files_are_tracks (8.4).
- test_gapless: test_self_trimming_48k_tracks_join_as_opus_does (8.5).
- The suite: 1,093 tests at M2, all green; 1,104 after dev's merge and the
  follow-ups' test_resync_in_steps (8.11), all green.
- `tools/opus_check`: the runner conceals malformed packets as the
  generator does (the whole-packet decoder gets the same PLC calls, so the
  split check stays in step), reports `dropped` beside `malformed`,
  knows `shortFrames`, and reads the audio pages in the generator's 8 KB
  slices by default (`--slice N`, 0 whole: the review found it reading
  whole pages while this document said it covered the sliced path).
  `opus_check.py run` on the 43 files of section 7.3:
  **0 failures**; `f2p5.opus` is now refused ("Opus with 2.5 ms frames
  isn't supported"), the regenerated fuzz file decodes all 2,140 packets
  (16 malformed concealed, 72 dropped, 6,724 frames; ffmpeg stops at
  154,608 by design), every other line as at M1.

### 8.7 Notices, the licence file, the README

- `LICENSES/BSD-3-Clause-libopus.txt`: `src/libopus/COPYING` verbatim (the
  Xiph/IETF variant: "Internet Society, IETF or IETF Trust" in the third
  clause; the IETF IPR links after the disclaimer).
- `THIRD-PARTY-NOTICES.md`: a libopus entry under ESP8266Audio (the
  v1.5.1 sources at `lib/opus` ab4e8359, `config.h` claiming 1.5.2, the
  decoder subset, fixed point, `VAR_ARRAYS`; the licence; COPYING's
  copyright line and the file-level holders in the linked code: Skype
  Limited, Koen Vos, Mark Borgerding, Timothy B. Terriberry, CSIRO,
  Xiph.Org Foundation, Gregory Maxwell, Jean-Marc Valin, Erik de Castro
  Lopo, Parrot (celt/cpu_support.h; the review found "Google Inc." named
  there instead: that notice is include/opus_projection.h's, which no
  linked object includes); the three IETF IPR statements, and Skype's that
  Microsoft's supersedes; the 63 linked objects from the map), "Opus" out
  of the not-in-the-binary list and `AudioGeneratorOpus` kept in it, the
  "linked and listed" line naming the three decoders and the file source
  (the ID3 source isn't in either map since e1984a8: the review found it
  still listed as linked; it is in the not-in-the-binary list now).
- `README.md`: the intro and status name Opus; the music line lists
  `.opus`; an "Opus" section (what plays, the cost and the 160 MHz note,
  gapless, the names from the path, what is refused and how it says so,
  the seek bar until M3) ending with the research's patent sentence
  (section 4.3): the IETF grants, and "Separately, members of the Vectis
  Opus patent pool (Dolby, Fraunhofer, NTT) assert patents against makers
  of hardware that decodes Opus. If you sell devices with this firmware
  installed, that may concern you."; the licence paragraph names libopus;
  the console's `O` row has `Ot1`/`Ot0`.
- `docs/ARCHITECTURE.md` and `docs/GAPLESS.md` (section 4.7) say what
  Opus is to the player.
- **The patent position:** accepted by the user on 2026-10-04 (the top of
  this document); the release notes still have to carry the sentence.

### 8.8 The build

Measured as section 1's table was, against the M2 baseline the plan named,
7a42429 (M1, the reader complete), and against 0e722df (M1's review fixes,
the tip M2 started from):

| | 7a42429 | 0e722df | M2 | Delta from 7a42429 |
|---|---|---|---|---|
| IRAM (`.iram0.vectors` + `.iram0.text`) | 125,895 B | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` 24,328 + `.dram0.bss`) | 56,160 B | 56,160 B | 56,168 B | **+8 B** (`.bss`: the tables' pinned block pointer and the Ot knob) |
| `.flash.text` | 1,583,168 B | 1,583,892 B | 1,585,216 B | +2,048 B (+1,324 from 0e722df: the backend +581, the console +492, the generator +415, the reader +472 (OggOpus +308, OggPage +164), the library +52, LibraryPage and Library +85) |
| `.flash.rodata` | 593,948 B | 593,948 B | 594,652 B | +704 B (the log and refusal texts, the console's help) |
| Image (core2, `Flash:` line) | 2,327,339 B | 2,328,063 B | 2,330,091 B | +2,752 B |
| Image (core2-dio) | 2,327,403 B | 2,328,127 B | 2,330,139 B | +2,736 B |

Internal RAM at runtime: -4,096 B for the decode task's stack; the tables'
pinned block is PSRAM (+7,776 B of it, allocated at boot). `iram_diet` (51
of 51 objects moved), `flash_guard` (38 % of the slot) and the version
check pass on `core2` and `core2-dio`; no `IRAM_ATTR` anywhere.

The review fixes (8.10), against the M2 figures above: IRAM **0**; DRAM
56,168 -> 56,264 B (**+96 B** of `.bss`: the player's failure record
holds the 48-byte note, and the backend two note strings); `.flash.text`
1,585,216 -> 1,586,112 B (+896 B: the backend +334, the reader +238, the
generator +157, PlaybackController +121, Ui +12); `.flash.rodata` 594,652
-> 595,012 B (+360 B: the reader's short refusal texts +324, the rest the
backend's and the player's); the image 2,330,091 -> 2,331,347 B (core2,
+1,256 B) and 2,330,139 -> 2,331,411 B (core2-dio, +1,272 B). The three
guards pass on both; no `IRAM_ATTR`.

### 8.9 The device checks M2 needed (run on 2026-10-06, the results in 8.11; silent mode `z` first)

The runbook of section 2 applies (the daemon holds COM3; `z` after every
boot; never `f`). The card set of section 3 with its fuzz file
regenerated and its two damaged copies added (8.1 and 8.10:
`ms128k_dmg.opus`, `cbr510k_dmg.opus`, one page's CRC wrong in the middle
of each; the scratchpad's copy is regenerated, the card's has to be copied
again); the research's `vorbis.ogg` and `surround51.opus` under
`/bench/opus-edge/`. For the library checks, a folder of `.opus` under
`/music` (the album's four parts as `/music/<artist>/<album>/01 - <title>
(part 1).opus` ... and an MP3 or two beside them), the index rebuilt at the
boot (`[index]` says "the cache is an older version's: rebuilt once" the
first time).

**Hearing safety.** Every row but two runs in silent mode (`z`), as the
gate did: the headphones, linked or not, get zeros. The two rows marked
**attended only** (the Opus-to-Opus joins over the real stream, and G2/G3
over the real stream) are the only ones that play audible sound, and they
run only with the user present, the headphones out of anyone's ears, and
the volume set low by the user first; the daemon never runs them on its
own, and a session without the user skips them and says so (the joins'
counts, the kept samples, the underruns and the load can all be read in
silent mode too: the silent rows cover them; what the attended rows add
is the A2DP stream's own cost, SBC on core 0 and its buffers, which silent
mode leaves out). The runbook's safety stop on a play out of silent mode
(section 2) stays armed for every other row.

| # | Check | Commands | Proof in the log |
|---|---|---|---|
| G5 | internal RAM with the stack at 16 KB, the headphones linked | part b's boot and link; `Rf</bench/opus/long128k.opus>`, `s` every 30 s for 10 min | `[heap] playing ... free N` and `ram=` steady **>= 48K** (the estimate; 50K is the floor), `min=` >= 42K; `stack_free=` >= 3,072 on every real file (the margin at 16,384) and >= 1,536 after `b</bench/opus/fuzz.opus>`, benched last |
| G5 (the knob) | the table copy in PSRAM: its RAM and its cost | `Ot1`, then `b</bench/opus/ms128k.opus>` twice, `Ot0`, the same twice; then `Ot1` and the 10 min play again, `s` every 30 s | `[rate] the filter tables copied into the pinned PSRAM block (Ot1)`; the second `[bench]` line's "the converter x.x % in the decoder's company" with `Ot1` against `Ot0` (M0: 5.2 %; the flash figure would be ~+13.5 points): the cost; `ram=` with `Ot1` >= 50K. The decision on the default: 8.11 (flipped: `Ot1` is the default) |
| G6 | the longest pass, every file | `Rf` on every real file of the card set to its end (`f120.opus` and `cbr510k.opus` included), `s` at the end of each | `pass_max=` **<= 20,000 us** on every file (30,000 since 8.11: measured 10.3-26.5 ms, SILK 10.3, mono 15.5, the stereo CELT files 16.6-26.5); the `[opus] end:` line's `longest step N us` (a slice and a call: ~12-14 ms) and `longest decode call` (~7 ms; 9.3 at 510k); `pass_max` after `Rf` on `mono64k.opus` following `cbr510k.opus` is the mono file's own (15.5 ms measured), not 79 ms |
| G6, a damaged page | the pass rule's edge: a step longer than the budget | `Rf</bench/opus/ms128k_dmg.opus>` and `Rf</bench/opus/cbr510k_dmg.opus>`, each to its end, `s` after each | each ends on its own: `[opus] end: the EOS trim; ... 1 gaps (1 filled: N samples ...); kept N samples at 48 kHz, exactly the length`, `bad 1, resyncs 1` (the host runner on these copies: `gaps=1`, 16 KB and 54 KB of resync reads, the length exact); its `longest step` **reported beside the 15,000 us budget** (at M2 the resync's scan plus the next page read whole, tens of KB in one `next()` call: measured 55.5 and 138.9 ms; since 8.11 a 4 KB chunk or an 8 KB slice a step, so within the budget); `underruns=` +0 (the ring holds ~1.5 s; M2's first cut stalled here for good: the track never ended, 8.10) |
| G6 at 160 MHz | the same on 128k | `Pcb160`, a boot, `z`, `Rf</bench/opus/ms128k.opus>`, `Rf</bench/opus/ms128k_dmg.opus>`, `s` after each; `Pcb0`, a boot | `pass_max=` <= 20,000 us (30,000 since 8.11: measured 23.4 ms) on the clean file (a call is 10.7 ms: one call a pass); the damaged file to its end with `underruns=` +0 |
| Joins | Opus to Opus sample-exact, through the queue | silent mode (`z`), the headphones in their case: `ql`, `qp<n>` on the Opus album, `G` after each join, `R` at the end | `[gapless] decoding ahead: ... (48000 Hz, the same rate: one stream)` for each of the 3 joins; `[opus] end: ... kept N samples at 48 kHz, exactly the length`; `G`: `joins 3 continuous, 0 after the tail`, 0 cuts; the heard length `exactly 52480 ms` per part (2,519,040 samples); `underruns=` +0 over the album |
| Joins over Bluetooth | **attended only** (the user present, the headphones not worn, their volume set low by the user first): the same album over the real A2DP stream | the headphones linked, not silent: `ql`, `qp<n>` on the Opus album, `G` after each join | as the row above, with `[stats] ... out=bt(connected)`; the headphones' own join heard as one (the user's ear, afterwards, at their volume). Skipped, and said so, when the user isn't there |
| Joins | Opus to MP3 and back | silent mode: a queue of an `.opus`, a 44.1 kHz MP3, an `.opus` (`qp` on a folder holding both, or `q+`) | `[gapless] decoding ahead: ... (44100 Hz, another rate: after the tail)` then `(48000 Hz, another rate: after the tail)`; the MP3's `[gapless] trim:` line as ever; 0 underruns, no `[E]` |
| Refusals | a Vorbis `.ogg` and a 5.1 file | `Rf</bench/opus-edge/vorbis.opus>`, `Rf</bench/opus-edge/surround51.opus>`, `Rf</bench/opus/f2p5.opus>`; then the same three from the library (copied under `/music`, `i<n>` on each, Now Playing up); an `.ogg` under `/music` after a rebuild (`g0`) | `[audio] /bench/...: Ogg Vorbis isn't supported (only Opus)`, `surround Opus (6 channels) isn't supported`, `Opus with 2.5 ms frames isn't supported (10 ms or longer only: too slow to decode here)` in the log; from the library, the toast `Skipped <title>: Ogg Vorbis isn't supported` / `surround Opus isn't supported` / `Opus with 2.5 ms frames isn't supported` (the screen's short form, 8.5) and the player skipping on; the `.ogg` counted as "other" in its folder (the Folders view), not listed |
| The library | `.opus` listed, the badge, the cache | a boot with the card's new folder; `l`; the Folders view on the screen (`X`) | `[index]` the rebuild note once, then "from the cache" at the next boot; `l` lists the `.opus` tracks; the OPUS badge in the Folders view; `i<n>` on one plays it |
| The seek bar | inert for Opus, however the track became current | an Opus track playing, Now Playing up; `qs60` on it; a pause, a reboot; then a queue of an MP3 followed by an `.opus` (gapless on): the join heard, Now Playing up; the reverse queue, the MP3 joining | no knob on the bar (a tap does nothing) on the Opus track started, after the reboot (the restored entry) and once it is heard after the MP3's join (no `play()` of its own); the knob back on the MP3 joined after an Opus track; `[audio] Opus: 1:00 asked: seeks aren't built yet (M3; the seek bar is off for Opus): from 0:00`, Now Playing counting from 0:00 |
| G2, G3 in silent mode | the load and the 30 min soak | part b's boot: `z`, the headphones linked (zeros to them), `d`, the album on repeat (`qp<n>` with repeat on), 30 min, `s` every 30 s | `load=` median <= 55 %, max <= 65 %; `underruns=` +0; `buf=` never under 1,000 ms after a part's fill; no Guru, panic or `[E]` |
| G2, G3 as written | **attended only** (the user present, the headphones not worn, their volume set low by the user first): the same over the real Bluetooth stream | the headphones linked, not silent: `d`, the album on repeat, 30 min, `s` every 30 s | as the row above with `out=bt(connected)`: the A2DP stream's own cost on top. Skipped, and said so, when the user isn't there |
| G7 (not Opus's) | untouched: a separate investigation owns the MP3 slowdown | `b` on One More Time from a fresh boot | recorded only |

### 8.10 The review's findings, fixed

The review of M2 (three lenses: the engine, the UI and the documents, the
tests) found these; each was first checked against the code.

- **The pass rule could stall a track for good** (high; two lenses found
  it). `loop()` asked the time rule before every step, the first of a pass
  included, and the last step's time is only ever replaced by a step: once
  one step took over 15 ms (the resync after a damaged page was tens of
  KB of reads in one `next()` call, 32-118 KB on the card set's two
  damaged copies, 8.11; the review's host model stalled after
  one 15.5 ms step), every later pass broke before its first step with
  `loop()` still saying it had more, so the ring drained into underruns,
  the track never ended, and `b` on such a file never returned. Fixed: the
  rule moved to `lib/core/PassClock` and the first step of every pass runs
  (8.2); test_pass_clock has the edges (the review's 7, 9, 16.5 ms case to
  the end; exactly the budget; steps each over the budget one a pass). The
  bench's loops need no extra exit: every pass now takes a step, and the
  reader's own bounds (1 MB and 2 MB of resync reads) end a damaged file.
  The card set gains two damaged copies, and 8.9 plays them to their end
  (G6: the review found no damaged file on the device's list).
- **The seek bar's knob went by the last `play()`** (medium; two lenses).
  `seekable()` was a flag `play()` set from the request's path, so a track
  joined gaplessly inherited the track before's answer (an Opus track
  after an MP3 showed a knob whose drag restarted it from 0:00; an MP3
  after an Opus track lost its knob), and the entry restored at a boot had
  the default. Fixed: `IAudioBackend::seekable(path)` answers for a path,
  `PlaybackController::seekable()` asks it of the current entry, and the
  UI reads that (8.5; test_playback).
- **The refusal note never reached the screen** (medium). The README
  promised each refusal "a note on Now Playing"; the sentence only went to
  the log and the console's `R`, and the toast said "can't play it".
  Fixed: `IAudioBackend::failureNote()` carries the reader's short form
  (`refusalNote()`, under 40 characters) through the player's failure
  record to the toast (8.5); the README's wording now says which text
  shows where.
- **8.9 asked for sound unattended** (medium; two lenses): two rows had
  the album played over the real Bluetooth stream, out of silent mode,
  with nobody attending. Fixed: every row runs in silent mode, and the two
  real-stream rows are marked attended only, with the user present, the
  headphones not worn and the volume set low by the user first (the
  paragraph at the top of 8.9).
- **The probes' floor had no test that depended on it** (low): disabling
  the floor check left test_ogg_opus green. Fixed:
  test_probe_floor_steps_over_damage (8.6), which fails with the check
  disabled (Q falls two pages).
- **The runner read whole pages** (low) while 8.2 said it covered the
  sliced path. Fixed: `--slice N`, 8,192 by default (8.6).
- **The notices** (low, two findings): "Google Inc." credited to
  celt/cpu_support.h (that file is Xiph.Org 2010 and Parrot 2013; Google's
  notice is include/opus_projection.h's, not linked), and
  AudioFileSourceID3 listed as linked (in neither map since e1984a8).
  Fixed in THIRD-PARTY-NOTICES.md (8.7).
- **Not changed, with the reason.** The header output gain to
  `OPUS_SET_GAIN` unclamped (low): libopus applies it in fixed point and
  saturates (`celt_exp2` caps the multiplier, the samples are clamped to
  the 16-bit range), so the loudest a file can get is full scale, which
  any file reaches with gain 0 and full-scale content; a clamp would lower
  no ceiling, and would mis-play files whose positive gain is meant (a
  quiet master normalised up by its header gain, which RFC 7845 says the
  decoder applies). The gain stays as the file says; the `[opus] open`
  line logs it.

### 8.11 M2's device checks: the results, and the four follow-ups

The device session ran 8.9's list on the image of 68c6926 on 2026-10-06
through section 2's runbook, in two parts (part a: the plays, the joins,
the refusals, the library, the seek bar, the benches, G6 at 160 MHz; part
b: the headphones linked for G5, the 20 min soak and the Dance tab), every
row in silent mode (`z`); the two attended-only rows were skipped, as 8.9
says they are without the user. The results:

| # | Check | Result | Verdict |
|---|---|---|---|
| G5, `Ot0` | internal RAM with the table copy in internal RAM: the headphones linked, the Dance tab, `long128k.opus` for 10 min, `s` every 30 s | steady `ram=` **48K** (the median of 140 readings), `min=` 47K; `[heap] playing` free 48K, largest block 43K: as 8.2 estimated, 2K under the 50K floor | MARGINAL |
| G5, `Ot1` | the same with the copy in the pinned PSRAM block | steady **56K**, `min=` 55K; `[heap] playing` free 56K, largest block 49K | **PASS** |
| The copy's cost | `b` on `ms128k.opus`, three times with each | the converter 5.6, 5.6, 5.6 % of a core in the decoder's company with `Ot0`, 8.3, 8.5, 8.5 % with `Ot1`: **+2.8 points** (the copy stays cached; the flash figure was +13.5); decode + convert 2.97x against 2.72x, decode only 3.5-3.6x either way | flip the default: follow-up 1 |
| G5, the stack | `kDecodeStack` 16,384, every real file and the fuzz file | real files 4,288 B free at the lowest (12,096 B used: 1,216 B over the 3 KB rule); the fuzz file 3,680 B free (12,704 B used: 2,144 B over the 1.5 KB rule), 5 malformed concealed and 13 dropped, no crash | **PASS** |
| G6 | the longest pass: 17 clean files to their end at 240 MHz, `s` at each end | every one exact; `pass_max` **10.3-26.5 ms** (SILK 10.3, mono 15.5, the stereo CELT files 16.6-26.5): 128k 19.5, 96k 16.6, 192k 21.4, 128k_2 19.3, yt-dlp 19.3, 10 ms frames 18.8, 60 ms 18.4, 120 ms 18.5, hybrid 17.2, SILK 10.3, the picture file 19.0, 510k CBR **26.5**, the album's parts 22.4, 18.1, 18.2, 19.9; the longest step 24.7 ms and the longest decode call 10.5 ms (both 510k); over 20 ms: 192k, 510k, album/01 (and `mono64k`'s first reading, 26,545: the 510k file's, carried over: follow-up 3; its own, the gate log's `[stats]` lines after its `Rf`, 15,474) | FAIL at 20 ms; **PASS at 30 ms**: follow-up 2 |
| G6, a damaged page | `ms128k_dmg.opus`, `cbr510k_dmg.opus` to their end | each ends on its own, exactly the length, 1 gap filled (48,000 and 41,280 samples), bad 1, resyncs 1 (16,364 and 54,342 B), underruns 0; the longest step **55.5 ms** and **138.9 ms** (the resync's scan and the page after it in one `next()` call), `pass_max` 57.3 and 140.7 ms | the track ends, no underrun: as 8.9 asked; the step over the budget: follow-up 4 |
| G6 at 160 MHz | `ms128k.opus`, `ms128k_dmg.opus` | the clean file exact, `pass_max` **23.4 ms** (the longest step 21.2 ms, a call 11.5 ms), underruns 0, load median 57.2 % max 64.7 %; the damaged file exact, `pass_max` 71.4 ms (the resync step 68.9 ms), underruns 0 | FAIL at 20 ms; **PASS at 30 ms** |
| G9 | the lengths | 26 ends of 24 files in part a and 22 of 4 in part b: every one exact, `R` ceil(N x 147/160) made with 0 in the filter | **PASS** |
| Joins | Opus to Opus through the queue | 3 joins `(48000 Hz, the same rate: one stream)`; the parts kept 2,519,040 each and were heard 52,480 ms each; `G` +3 continuous, 0 cuts; underruns 0 | **PASS** |
| Joins | Opus to MP3 and back | `(44100 Hz, another rate: after the tail)`, then `(48000 Hz, another rate: after the tail)`; the MP3's `[gapless] trim: Lavc63.8. delay 576, padding 1224: skipping 1106, holding 695`, heard 30,000 ms by its LAME tag; the Opus part after it exact | **PASS** |
| Refusals | by path and from the library | each sentence in the log (Vorbis, surround, 2.5 ms frames, the muxed file); from the library the toast's short form and the player skipping on, for all three | **PASS** |
| The library | `ql`, `l`, the cache, the Folders view | the two test albums listed (9 and 4 tracks; the `.ogg` counted as "1 other", not listed), the index from its cache at every boot, the OPUS badge in the Folders view (two screenshots) | **PASS** |
| The seek bar | inert for Opus however the track became current | started, joined after the MP3, restored at a boot: `the bar: inert`; the MP3 joined after Opus: its knob; `qs30` on Opus: `0:30 asked ... from 0:00` | **PASS** |
| G2, G3 in silent mode | the four parts on repeat through the queue, the headphones linked, the Dance tab, 20 min with `Ot1` | load median 46.8 %, max 50.3 %; underruns 0; the ring never under 1,362 ms after a fill; 22 joins, 22 continuous; 22 parts ended, 22 exact; 0 error lines; the Dance tab confirmed on (280 `[dance] on` lines) | **PASS** |
| MP3 and FLAC `b` | One More Time, Stronger, twice each, before any `.opus` | MP3 **4.8x** (M0's image: 3.8x; against G7's 4.9x: -2 %), FLAC 4.7x (0 %) | PASS: the slowdown M0's image showed was its layout's, not Opus's |
| The queue | the user's queue put back after each part | 27 tracks at entry 10, 0:02 in, restored at every boot | **PASS** |

**The four follow-ups**, built the same day (this commit; their device
check is the next session's):

1. **The table copy's PSRAM block is the default (`Ot1`).** Measured
   above: internal RAM 56K steady with it against 48K without (the floor
   50K), at +2.8 points of a core for the converter. `Ot0` stays as the
   console's A/B (the internal-RAM copy). `tablesPinned` starts true; the
   boot log says `(Ot1, the default; Ot0: internal RAM)`; the README's
   console row and Opus section, ARCHITECTURE.md, RESAMPLER.md and
   ENERGY.md say where the copy lives now. It applies to every converted
   track (a 48 kHz MP3's copy goes there too). The details: 8.2, G5's
   item 2.
2. **G6 is <= 30 ms for the longest pass, the same as FLAC.** The
   decision: the gate's 20 ms was the research's figure for the UI loop's
   turn on the shared core, and the UI already lives with FLAC's passes,
   which on this build reach **27-36 ms at a track's start** (the device
   session's reading over its boots; the gate log's first `[stats]` line
   after a boot, the restored FLAC entry paused 2.1 s in, says
   `pass_max=27236us`), so a 30 ms Opus pass adds nothing to the UI's
   worst wait. The evidence for Opus: **10.3-26.5 ms** on the clean files
   at 240 MHz (the table above: SILK 10.3, mono 15.5, the stereo CELT
   files 16.6-26.5; the 510k CBR file's 26.5 ms is an 8 KB
   slice read and a 10.5 ms call with the conversion), **23.4 ms** at
   160 MHz on mStream 128k; 0 underruns everywhere, the ring never under
   931 ms (album/04's fill) on the clean files. `kPassBudgetUs` stays
   15 ms (the rule that keeps a pass to one or two steps); what changed is
   the mark the gate asks of `pass_max`. Section 2.2's and 8.9's rows carry
   the note; the generator's class comment says 30.
3. **`pass_max` is the heard track's** (8.2, item 3): the backend keeps
   the decoding track's and the heard track's longest pass, `s` shows the
   heard track's, read live. Every codec.
4. **The resync after a damaged page is steps** (8.2, item 4): a 4 KB
   chunk of the scan, or a slice of the page it finds, a `next()` call
   each, so no step of a damaged file's play is longer than a slice read;
   the device's 55.5 and 138.9 ms steps become 2-5 ms ones, within the
   budget. To measure: the next device session plays the two damaged
   copies again and reads the `[opus] end:` line's `longest step`, and the
   runner's `maxCallBytes` on them (8,474 B at most: a slice and a header).

The build after the four, against the worktree's last build of 68c6926
(the two `firmware.map`s' output sections; 8.10's image of 2,331,347 B was
before dev's merge, b198d65, which is +3,676 B of its own):

| | 68c6926 | The follow-ups | Delta |
|---|---|---|---|
| IRAM (`.iram0.vectors` 1,028 + `.iram0.text` 124,867) | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` 24,328 + `.dram0.bss`) | 56,272 B | 56,280 B | **+8 B** (`.bss`: the heard track's two atomics in the backend object; `tablesPinned`'s byte moved from `.bss` to `.data` with its initializer, inside the same 24,328) |
| `.flash.text` | 1,588,896 B | 1,589,156 B | +260 B (the backend +123: the heard figure; the scan's steps, `OggPage` +100 and `OggOpus` +49; the generator -27) |
| `.flash.rodata` | 595,904 B | 596,000 B | +96 B (the console's texts) |
| Image (core2, `Flash:` line) | 2,335,023 B | 2,335,379 B | **+356 B** (37.1 % of the slot) |
| Image (core2-dio) | | 2,335,443 B | (core2 + 64 B, as ever) |

`iram_diet` (51 of 51 objects moved, the MP3 hot set pinned: 9 code and 3
data lines), `cache_guard` (the hot lines at most 2 in a set, bench and
playback; the pin present) and `flash_guard` (38 % of the slot; QIO at
80 MHz, DIO at 40) pass on `core2` and `core2-dio`; no `IRAM_ATTR`; the
host suite is 1,104 tests, all green (test_ogg_opus 43).

## 9. M3: seeks and resume points

M3 wired M1's plan (section 7.1) into the firmware. A start part of the
way in on an Opus track (`qs`, the seek bar's tap or drag, the boot's
resume point, a paused seek's start point) now lands on the exact sample
asked, as a FLAC's does, by the reader's bisection on the pages' granule
positions and a preroll; a pause's anchor is the FLAC model's and goes
through NVS as kind 3; and the seek bar is live on Opus. No device work in
M3: the firmware builds (9.4), the host tests and the runner cover the
logic (9.2, 9.3), and 9.6 is the device session's list.

### 9.1 What it built

| Piece | What |
|---|---|
| The plan (`Core2AudioBackend::planOpus()`, `src/audio/Core2AudioBackend.cpp`) | In `prepare()`'s Opus branch, after the open (the headers and the tail scan: the exact length the plan and the anchor's check need). Its sources, the first that gives one: **1. the resume anchor**, checked against the file as opened (`oggopus::checkAnchor()`: the kind at 48 kHz and exact, the file's size, the exact length, the tail rule; gapless trimming on, as every anchor), planned by its sample with the **600 ms resume preroll** (`kResumePrerollSamples`: the decoded PCM then matches a play from the top, 7.3); a failure logs `[audio] Opus: the resume anchor isn't this file's (the size: N -> N / the length: N -> N samples / in its last 5 s / an MP3's / gapless trimming off): by its second` and falls through; **2. the millisecond** with the **200 ms seek preroll**, the tail rule first (`planStartMs()`: the last 5 s and past the end log `[audio] Opus: m:ss.mmm asked, of m:ss: in its last 5 s or past its end: from 0:00`); a length not known (no last page found: a junk tail beyond the scan's windows) starts from 0:00 with its own line, as a FLAC without STREAMINFO does (a plan would land where asked and, past the end, end at once: the player would move on). The plan lands exactly either way: `[audio] Opus: starting 1:00.000 in, of 3:29 (its resume anchor / the time asked; exact): the page at byte N (granule N[, its own packets stepped over]) / from the first audio page (the target is inside its first second), decoding from N ms before (the 200 ms preroll), N probes, N reads / N KB in N ms`. The plan goes into `Prepared` (`opusPlanned`, `opusPlan`: ~56 B) with `landedMs`, `startSample` and `startExact` true, so `positionMs()`, the book and the run index count from the target as for an MP3's plan |
| The start (`OpusGenerator::setStartPlan()`, `planStartMs()`, `planStart()`) | `beginPrepared()` hands the plan to the generator (or null: from the top) before `begin()`, which puts the reader at the plan's page (`Reader::startAt()`: Q's end, or Q itself with its own packets stepped over when a packet continues out of it; a scan in progress is dropped with the position) and the timeline by the plan (`Timeline::start(plan)`): the packets ending before the preroll are skipped by their TOCs undecoded (the end line's `skipped`, M2's wiring), the preroll's frames are decoded and dropped (`decodedBefore`: the preroll and under a packet more), and the first sample kept is the target. The decoder is initialised afresh at every `begin()` (`opus_decoder_init()`: the reset such a start needs, Rockbox's 2019 overflow came from a missing one). The first `next()` after `startAt()` may say Pending (a slice of the plan's page, or a step of a scan past a damaged one: 8.2's items 2 and 4): `decodeNext()` ends the pass on it and the next pass asks again, as from the top. The generator records the first kept sample's trimmed index (`landedSample()`) and the end line says it: `[opus] end: ...; kept N samples at 48 kHz, exactly the length from the start on (N); the start: sample N asked, N landed (exact); ...` (a play from the top keeps "exactly the length (N)" as before: gate G9's text is unchanged) |
| The resume anchor (`lib/core/SeekIndex`, `ResumeAnchor`, `NvsLayout`) | `SeekIndex::Kind::Opus`: a header-only run, as a FLAC's (`beginRun()` now runs for Opus too: the rate 48,000, `totalSamples` the exact length, the base the start's sample), from which `anchorAt()` makes the pause's anchor (`resumeAnchor()`): `Kind::Opus`, exact, 48 kHz, the sample, the size, the length's low 32 bits, the byte fields 0, what `oggopus::makeAnchor()` makes and `checkAnchor()` takes. `G`'s line says `run index: heard Opus run from sample N (exact), 0 entries`. `NvsLayout::decodeResume()` takes kind 3; **a kind it doesn't know (4 and up: a later format's) now keeps the five words and drops the anchor**, so the point resumes by its second instead of being lost (0.6.0 refused the whole blob on a kind above FLAC: a downgrade to it from a pause saved on an Opus track loses the resume point once, SEEK.md 5.2's pattern). The blob stays version 2, 64 B; the schema stays 1; `QueueStore` is unchanged. `trackseek::checkAnchor()` (the MP3 check) refuses an Opus anchor by its kind, as it does a FLAC's: the Opus check is the reader's, which has the exact length |
| The seek bar | `Core2AudioBackend::seekable()` is gone: `IAudioBackend::seekable(path)` is the base's true for every format, so Now Playing draws the knob and takes touches on an Opus track however it became current (started, joined, cued, restored), `PlaybackController::seek()` places the start as for an MP3 (whole seconds, never into the last 6 s), and the player's `seekable()` by the entry's path stays as M2's review left it (test_playback's fake keeps one unseekable path for the shape). SEEK-BAR.md's inert list no longer names Opus; SEEK.md's exactness lists (5.5, 6.5, 6.7) gain it |
| The heard-track model and the latency | A seek is a request: `start()` resets `maxPassUs_`, `heardToken_` and `heardMaxPassUs_` and bumps `startSeq_`, so `s`'s `pass_max` is the seek's own from its first pass and the time to first audio is the same line as an MP3's or a FLAC's: `[audio] refill: first audio in the ring N ms after the request, ...` (`noteStartProgress()`). Expected: the plan's reads (the runner's 50 random starts a file, 9.3: 2-8 probes and ~20-140 KB on the card set's 128k files, ~80 KB the mean, at ~2 MB/s ~10-70 ms; the 510k file's 64 KB pages ~320 KB the mean and 450 KB at most, ~160-230 ms) plus the preroll's decode (ten 20 ms frames at ~7 ms, two a pass with a 1 ms rest: ~40-80 ms), ~100-170 ms on a 128k file against an MP3's 74-149 ms (SEEK.md 17), ~250-300 ms on the 510k file; 9.6 measures it |
| Not built, by the research's section 12 | A per-page run index for Opus (the bisection is exact and costs ~20-140 KB a seek on a 128k file, up to 450 KB on 64 KB pages, 9.3: `SeekIndex` holds the header only); a resume anchor with gapless trimming off (as for MP3 and FLAC: the start goes by its second, still exact for Opus); the `R` command's counts after a seek are the converter's as before (`source frames taken` = kept = the length less the start) |

### 9.2 Host tests

- test_ogg_opus, 44 tests: test_plan_through_slices (new): the 64 KB
  pages of test_sliced_page_reads' file in 8 KB slices, a plan every
  7,777 samples, each the same plan as with whole reads (the plan reads
  whole pages whatever the slice) and each landing on its sample through
  the Pendings (the first `next()` after `startAt()` is one: a slice of
  the plan's page), the kept count the length less the target, no call
  over a slice and a header; the second audio page damaged: a plan past it
  lands exactly with nothing filled (the probes take the damaged page's
  intact header for Q, its CRC fails when read whole, the page before it
  stands in and the resync's steps follow), one inside it fills the gap
  from the target on (35,688 samples: 96,000 less the target's 60,312),
  and, on the 60 s file of 1 s pages with its page 30 damaged, `startAt()`
  in the middle of the resync after it (a `next()` that said Pending
  mid-scan) to a plan at 10 s drops the scan with the position: a kept
  scan would run on from inside the damaged page and hand over page 31
  with the timeline at 10 s, a 22 s gap that ends the track; the start
  lands exactly, the gap is filled when the play reaches it, the damage is
  found out twice and bridged once (the M3 review: the block first planned
  past the damage, where a kept scan finds the same page, so it passed
  with the scan kept; 9.7).
- test_nvs_layout, 8 tests: test_resume_blob_opus_anchor (kind 3's round
  trip byte for byte: the sample, the size, 48,000, the length's low
  bits; 0.6.0's rule, modelled, refuses the blob: the downgrade pattern),
  test_resume_blob_unknown_kind_keeps_the_position (kinds 4 and 255 keep
  the words with no anchor; 1-3 read whole).
- test_seek_index: test_an_opus_run_anchors_by_its_sample (a header-only
  run's anchor at a second of ring frames: `Kind::Opus`, 48,000,
  base + 48,000, the length's low bits, exact; equal to `makeAnchor()`'s and
  taken by `checkAnchor()`, the size refused; a length not known gives
  one the check refuses; `find()` finds nothing in it; `heardRun()` says
  Opus).
- test_track_seek: an Opus anchor is `Kind` to the MP3 check.
- The suite: 1,108 tests, all green (1,104 at the follow-ups).

### 9.3 Checked on the host: the runner's `plan` command

`tools/opus_check/runner.cpp` gained `plan <file> <ms> [--sample S]
[--preroll MS] [--window N] [out.raw]` and `opus_check.py plan`: one start
planned and decoded as the firmware does it (the tail rule by the ms, or
an anchor made and checked by `--sample` with the resume preroll, falling
back to the ms when the check refuses, as the backend's "by its second"),
the plan printed as the device's `[audio] Opus: starting` line names it
(the page byte and granule, where decoding and keeping begin, the probes
and reads: a function of the file alone, so the device's figures and the
host's must agree for the same file and second), the window after the
target (4,096 samples; 0: the rest) compared with the in-process decode
from the top and with ffmpeg's decode of the whole file at the same offset
(a damaged file against the in-process decode alone: ffmpeg drops the lost
page and its timeline runs short from there), and the kept samples written
for a PC comparison. On the card set (section 3), every start landed on
its sample:

| File, the second asked | The plan | Landed | Against the decode from the top (4,096 samples) | Against ffmpeg |
|---|---|---|---|---|
| `ms128k` 60 s | byte 975,987 (granule 2,832,000), 7 probes, 25 reads / 82 KB | exact | max 51, 63.6 dB | max 1,377, 45.1 dB (a loud passage: fixed point against float) |
| `ms128k` 60 s by an anchor (`--sample 2880000`, the 600 ms preroll) | the same page, 4 probes, 18 reads / 65 KB | exact | **bit-exact** | 45.1 dB |
| `ms128k` 1 s | from the first audio page (inside its first second), 0 probes | exact | bit-exact | max 3, 69.8 dB |
| `ms128k` 203 s (6.9 s before the end) | byte 3,303,866, 7 probes, 23 reads / 74 KB | exact | max 35, 63.1 dB | max 34, 65.6 dB |
| `ms128k` 206 s (in the last 5 s) | the tail rule: from 0:00, target 0 | 0 | bit-exact | 71.4 dB |
| `mono64k` 30 s | 2 probes, 6 reads / 13 KB | exact | max 10, 64.2 dB | max 7, 66.7 dB |
| `silk16k` 20 s | 2 probes, 6 reads / 13 KB | exact | bit-exact | bit-exact |
| `hyb32k` 45 s | 2 probes, 6 reads / 13 KB | exact | max 49, 50.0 dB | max 49, 50.0 dB |
| `f60` 12 s, `f120` 17 s | 5 probes, 19 reads / 65-66 KB | exact | max 4-10, 66-77 dB | max 5-10, 66-72 dB |
| `cbr510k` 33 s | 7 probes, 58 reads / 251 KB (64 KB pages) | exact | max 5, 64.8 dB | max 5, 65.3 dB |
| `ytdl` 100 s | 5 probes, 19 reads / 66 KB | exact | max 4, 66.3 dB | max 3, 68.4 dB |
| `album/03` 25 s | 6 reads / 32 KB of play | exact | max 25, 65.3 dB | max 21, 66.4 dB |
| `ms128k_dmg` 150 s (past the damaged page), `cbr510k_dmg` 50 s | the plans as on the clean files | exact, 0 filled | max 31 / 19, 62-64 dB | (ffmpeg's timeline is a second short from the damage) |
| `ms128k_dmg` 40 s (before the damage) | | exact | 63.7 dB | 65.1 dB |
| `ms128k` by an anchor in its last 5 s (`--sample 10000000`) | the anchor refused (`tail`), by the ms: from 0:00 | 0 | bit-exact | |
| `mono64k` 40 s, the whole rest (`--window 0`: 960,000 samples kept, the EOS trim reached) | 2 probes, 6 reads / 13 KB | exact | max 14, 87.6 dB over the whole rest | max 1,899, 65.3 dB |

The resume preroll's bit-exactness and the seek preroll's 50-77 dB are
the research's figures (7.3) again; `opus_check.py seek` (50 random
starts a file at each preroll) stands as M1 left it. Its figures are the
plan's cost per file, what 9.1's latency estimate and `planOpus()`'s
comment use (the 200 ms preroll, 50 starts, seed 1): `ms128k` 4.3 probes
a start (8 at most) and 82 KB (121 KB at most); `album/01` 4.7 (8) and
83 KB (137 KB); `ytdl` 4.4 (10) and 78 KB (138 KB); `mono64k`, 8 KB
pages, 2.2 (4) and 19 KB (35 KB); `cbr510k`, 64 KB pages, 6.7 (16) and
323 KB (452 KB). A start's cost goes with where the target falls between
the probes' pages, so the table's figures above are one start each, not
the file's mean.

### 9.4 The build

Measured as section 1's table was (the output sections of `firmware.map`),
against the follow-ups' build (8.11, the same tree before M3):

| | The follow-ups | M3 | Delta |
|---|---|---|---|
| IRAM (`.iram0.vectors` 1,028 + `.iram0.text` 124,867) | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` 24,328 + `.dram0.bss`) | 56,280 B | 56,352 B | **+72 B** (`.bss`: `Prepared`'s plan, ~56 B, and its flag, in the backend object) |
| `.flash.text` | 1,589,156 B | 1,592,528 B | +3,372 B (the reader's plan, anchor and `startAt()` code linked at last, +1,626: it was in `OggOpus.cpp` since M1 but unreferenced, so `--gc-sections` dropped it; the backend's `planOpus()` +898; the generator +575; `SeekIndex` +15) |
| `.flash.rodata` | 596,000 B | 596,672 B | +672 B (the log lines: the backend +460, the reader +242) |
| Image (core2, `Flash:` line) | 2,335,379 B | 2,339,423 B | **+4,044 B** (37.2 % of the slot) |
| Image (core2-dio) | 2,335,443 B | 2,339,487 B | (core2 + 64 B, as ever) |

`iram_diet` (51 of 51 objects moved, the MP3 hot set pinned: 9 code and 3
data lines), `cache_guard` (the hot lines at most 2 in a set, bench and
playback; the pin present) and `flash_guard` (38 % of the slot; QIO at
80 MHz, DIO at 40) pass on `core2` and `core2-dio`; no `IRAM_ATTR`. The
generator's plan (~60 B) lives in the generator object on the heap; the
runtime cost of a seek is the plan's reads and the preroll's decode (9.1),
nothing kept.

### 9.5 How exact, in one place

- **Where it lands:** the sample asked, always (`exact` in the log; the
  end line's `the start: sample N asked, N landed (exact)`): the pages'
  granule positions place every packet, so unlike an MP3's TOC start
  there is no estimate to be off by. The tail rule and a target at or past
  the known end start at 0:00, as any start.
- **What the first samples are:** after the 200 ms seek preroll, within
  50-77 dB of a play from the top on CELT files (a quiet passage less:
  album part 4's 37 dB at M1; over 500 random starts a file the worst
  start of an mStream 128k transcode is 35-37 dB and a few quiet starts
  score 26-29 at any preroll: 10.7), 41-50 dB on SILK and hybrid; after the
  600 ms resume preroll, bit-exact on CELT files (SILK and hybrid never
  converge bit for bit). The `DeclickReader` fades the start in over 64
  frames, as at every start, and the converter restarts, so the ring's
  frames aren't bit-identical to uninterrupted playback (a 48 kHz MP3's
  aren't either: SEEK.md 5.5).
- **The time shown:** the target in ms, rounded down (`landedMs`).

### 9.6 The device checks M3 needs (the next session; silent mode `z` first)

The runbook of section 2 applies (the daemon holds COM3; `z` after every
boot; never `f`; `Rf` plays one file with nothing after it, in silent mode
only; the card set of section 3 with the two damaged copies). A seek is
`qs<seconds>` on the playing entry (the player's `seek` path: the tail
rule, whole seconds), so the Opus files to seek in must be in the queue:
the m2card's four-part album under `/music` (52.48 s each: `ql`,
`qp<n>`) for the CBR/VBR mStream shape, and, for the other
kinds, a folder under `/music` holding copies of `mono64k.opus`,
`hyb32k.opus`, `silk16k.opus`, `f60.opus`, `f120.opus`, `cbr510k.opus`,
`ytdl.opus` and `ms128k_dmg.opus` (the index rebuilt at the boot). Where
a row needs the plan's figures for the same file and second, run
`opus_check.py --work <DIR> plan <file> <ms>` on the PC's copy (9.3)
before or after: the page byte, granule, probes and reads must agree.

| # | Check | Commands | Proof in the log |
|---|---|---|---|
| S1 | `qs` on each kind of file: CBR (`cbr510k`), VBR (the album's parts, `ytdl`), mono (`mono64k`), hybrid (`hyb32k`), SILK (`silk16k`), 60 and 120 ms packets (`f60`, `f120`), the damaged one (`ms128k_dmg`: a seek past the damaged page, 150 s, and one before it, 40 s) | each file playing from the queue (`i<n>` or `qp`), Now Playing up; `qs30` (and `qs150`, `qs40` on the damaged copy); the track to its end, `R` | `[audio] Opus: starting 0:30.000 in, of m:ss (the time asked; exact): the page at byte N (granule N), decoding from 200 ms before (the 200 ms preroll), N probes, N reads / N KB in N ms`, the byte, granule, probes and reads **equal to `opus_check.py plan` on the PC's copy** for the same second; then `[opus] end: the EOS trim; ... kept N samples at 48 kHz, exactly the length from the start on (N); the start: sample 1440000 asked, 1440000 landed (exact)` (kept = the exact length - 30 x 48,000); `R`'s `source frames taken` = kept; the damaged copy's 150 s seek: `0 gaps`, `0 filled`, exact; its 40 s seek: `1 gaps (1 filled: 48000 samples)`, exact; no `[E]`, `underruns=` +0 |
| S2 | Seeks near the start and the end | the album's part 1 playing: `qs1` (inside the first second: the plan from the first audio page), `qs2`, `qs46` (6 s before its 52.48 s end: lands), `qs48` (in the last 5 s: the player's bar never asks it, `qs` does) | `qs1`: `from the first audio page (the target is inside its first second), decoding from 1006 ms before` and the end line's `sample 48000 asked, 48000 landed (exact)`, kept = length - 48,000; `qs46`: `the page at byte N (granule N)`, `sample 2208000 asked, 2208000 landed (exact)`, kept 311,040 (2,519,040 - 2,208,000); `qs48`: `[audio] Opus: 0:48.000 asked, of 0:52: in its last 5 s or past its end: from 0:00`, the whole part kept |
| S3 | The resume point after a raw `!reset` | a part playing about 30 s into the album's part 2 (each part is 52.48 s, so there is no 60 s point in it, and a pause in its last 5 s saves an anchor the tail rule refuses, `in its last 5 s`, with the start at 0:00; 30 s has room either way), pause (the pause's save: `[queue] resume point saved: 0:30 into 2; anchor: Opus sample N`), `!reset` written raw, `z`, play | at the boot `[queue] resume point: 0:30 into 2 (stopped: play starts there); anchor: Opus sample N` (the same N); at the play `[audio] Opus: starting 0:30.xxx in, of 0:52 (its resume anchor; exact): ..., decoding from 600 ms before (the 600 ms preroll), ...` (the 600: the anchor was taken), and the end line's `sample N asked, N landed (exact)`; the run index's line on `G`: `run index: heard Opus run from sample N (exact), 0 entries`. Then the fallback: change the file (copy another part over it under the same name) and reset again: `[audio] Opus: the resume anchor isn't this file's (the size: N -> N): by its second` (the size, checked first, is what refuses here: the four parts are one length, 2,519,040 samples, so the length check can't be; a file of another length would say `the length: N -> N samples`), then `(the time asked; exact)` with the 200 ms preroll |
| S4 | The seek bar's taps and drags on Opus | an Opus track playing, Now Playing up (`X` for a screenshot): a tap at a third of the bar, a drag to two thirds and lift, a drag back onto the marker and lift (no seek), a drag off the bar (cancel); then the same on a track joined gaplessly after an MP3 (no `play()` of its own) and on the entry restored at a boot | the knob drawn (`the bar: rest` in `ui`'s line, not `inert`); each tap and lift logs the player's seek (SEEK-BAR.md section 8: `[seek] ...`) followed by `[audio] Opus: starting m:ss.000 in ... (the time asked; exact)`; the detent and the cancel log no seek; the time shown after the seek is the second asked |
| S5 | Latency | S1's and S3's starts | `[audio] refill: first audio in the ring N ms after the request, 500 ms buffered at N ms, ...` per start: expected ~100-170 ms on the 128k files (the plan's ~20-140 KB of reads and the preroll's decode, 9.1; an MP3's seek 74-149, a FLAC's 99-180: SEEK.md 17, SEEK-BAR.md 16); record each file's; the 510k file's plan (58 reads / 251 KB on the host at 33 s; 323 KB the mean over 50 starts, 9.3) is the slow one, ~250-300 ms expected |
| S6 | `s` after a seek | `s` within a second of each `qs` | `pass_max=` the new start's own (the request reset it: 8.11's follow-up 3), <= 30,000 us (G6), the preroll's passes included |
| S7 | A seek inside a gap (the damaged copy) | `ms128k_dmg` playing: `qs40` (before the damage: the control, S1's 40 s seek), then `qs104`, inside the damaged page itself (the page at byte 1,713,764 holds the file's samples 4,992,000-5,039,999, 103.99-104.99 s trimmed: `opus_check.py plan ms128k_dmg.opus 104000 --window 0` on the PC's copy says `k=4944000 target=4992000 ... filled=47688`; a `qs41` would land 63 s before the damage and start inside no gap) | both exact; the second's plan line, `the page at byte 1696309 (granule 4944000)`, 7 probes, then no `[E]`; the end line's `1 gaps (1 filled: 47688 samples)` for the start inside the gap (the fill from the target on: 5,040,000 less the target's 4,992,312 absolute; the 40 s control's is `48000`), `kept` the length less 4,992,000 and `sample 4992000 asked, 4992000 landed (exact)` |

**How to compare a landing with a PC decode.** The device dumps no PCM
(SEEK.md 17 used a temporary probe for that, removed afterwards), so the
comparison is in three parts, each already in the tree: (1) **the plan:**
the device's `[audio] Opus: starting` line and `opus_check.py plan`'s
`plan:` line for the same file and second must name the same page byte,
granule, `decodeFrom`/`keepFrom` (the preroll in ms), probes and reads,
the plan being a function of the file alone (the card's copy is the PC's
byte for byte: `sha256sum` both if in doubt); (2) **the landing:** the end
line's `sample N asked, N landed (exact)` and `kept = the exact length -
N` (the `R` command's `source frames taken` the same), which the runner
shows as `landed=N target=N keptSamples=...` with `--window 0`; (3) **the
samples:** what the device decodes after the landing is what the runner
decodes (the same libopus, fixed point, frame by frame, from the same
plan: 8.6's split check holds on every file), and `opus_check.py plan`
compares that with ffmpeg's decode from the top at the same offset (9.3's
table: 45-72 dB on CELT, bit-exact on SILK). A listening check is the
user's, at their volume, afterwards: a seek should sound like the same
second of the PC's decode, with the start's 64-frame fade.

### 9.7 The review's findings, fixed

The review of M3 and the four follow-ups (the documents, the engine, the
tests) found these; each was first checked against the code or the data.

- **The heard track's longest pass could lose a pass at a join** (low;
  three findings). `takeAdvance()` set `heardMaxPassUs_` by a load of
  `maxPassUs_` then a store, and `produceDecoded()` stored it too when the
  decoding track was the heard one: a pass of the joined track that ended
  between the loop's load and its store went into the heard figure on the
  decode task (it saw the token) and the loop's store then put the smaller
  value back over it. The comment's claim that sequential consistency
  alone lost no pass held for the token against the figure, not for the
  two stores to the same figure. Rare (a few instructions a join), and
  only `s`'s `pass_max` for that track afterwards, G6's figure. Fixed:
  the figure is raise-only on both sides (`raiseTo()`, a compare-exchange
  max), and `takeAdvance()` sets it to the joined track's figure, then
  raises it to `maxPassUs_` once more, so a pass that ended in the window
  is restored. Not host-testable (the two tasks are the firmware's); the
  comments at both sites say the order and why. The build: +60 B of
  `.flash.text` (core2 2,339,483 B, core2-dio 2,339,547 B), DRAM
  unchanged at 56,352 B; the guards pass as in 9.4; the host suite stays
  1,108 tests, all green.
- **The mid-scan `startAt()` test tested nothing** (medium).
  test_plan_through_slices planned past the damaged page after a `next()`
  that said Pending mid-scan; there a kept scan finds the same next page
  as a fresh start does, so a `startAt()` that kept `resync_` passed all
  44 tests (the review's mutation, repeated here before the fix). Fixed:
  the block plans back to 10 s on the 60 s file with its page 30 damaged,
  where a kept scan hands over page 31 with the timeline at 10 s and the
  22 s gap ends the track; with the mutation the block fails on
  `gapEnded`, and the real code lands exactly, fills the gap when the play
  reaches it, finds the damage out twice and bridges it once (9.2).
- **Two device checks couldn't be run as written** (medium; four
  findings). S3 paused 60 s into a 52.48 s part; S7's `qs41` lay 63 s
  before the damaged page (which holds 103.99-104.99 s), so no start was
  ever made inside a gap. Fixed in 9.6: S3 pauses at 30 s and its fallback
  names the size alone (the parts are one length); S7 seeks to 104 s and
  expects `1 filled: 47688 samples`, the runner's figure for that start.
- **The G6 evidence had three ranges** (low; two findings): 19-26.5,
  19.3-26.5 and 16.6-26.5 ms for one measurement whose own table listed
  SILK at 10.3 and mono64k's own at 15.5 (the gate log's `[stats]` lines
  after its `Rf`). Fixed: 10.3-26.5 ms (SILK 10.3, mono 15.5, the stereo
  CELT files 16.6-26.5) in the status, 8.9, 8.11 and the decision. 8.2's
  and 8.10's "40-120 KB" for M2's one-read resync is the measured
  32-118 KB (a 16 KB scan and a 16 KB page; 54 KB and a 64 KB page).
- **The plan's cost was understated** (low): `planOpus()`'s and
  `OpusGenerator.h`'s "3-5 headers, 30-60 KB" against the runner's 4-8
  probes and 65-140 KB on an mStream 128k file (9.3's per-file means over
  50 starts, added there), and 9.1's "13-84 KB" left the 510k file's
  251-452 KB out. Fixed in the comments, 9.1, the "not built" row and S5
  (~100-170 ms expected on 128k, ~250-300 ms at 510k).
- **`opus_check.py plan` printed the frames written, not compared** (low):
  4,488 (whole frames) for the 4,096 window. Fixed: `compare()` returns
  the frames it covered and that is printed. **Its usage line put
  `--work` after the subcommand** (low), which argparse gave to the file
  and the ms; every subcommand takes `--work` too now (`SUPPRESS`, so the
  top-level value stands when it isn't given), and both orders work.
- **The docs' format lists** (low; three findings): SEEK-BAR.md 2.4 and
  its section 7 row name Opus among the exact formats, with an Opus row
  beside LAME VBR's; SEEK.md 4.3's run kinds are MP3, FLAC or Opus, and
  FLAC and Opus runs have the header only. **The card set's f120 note**
  (low) still said G6 was 20 ms: cardset.py says 30 (8.11; 20 at M0); the
  card's README.txt takes it at its next build (the scratchpad copy's line
  was changed by hand).

## 10. M4: faster starts

The user asked for "the faster Opus start" after M3's device check (9.6,
S5): first audio came 270-460 ms after the request on the 128k-class
files (`cbr510k` 793 ms), against MP3's 74-149 and FLAC's 99-180. M4
takes the reads out of the start, host-side only: the firmware builds
(10.6), the host tests and the runner cover the logic (10.3, 10.4), and
10.5 is the device session's list. The plans are unchanged (the same page
for every one of the device's seconds, checked against M3's reader on the
host: 10.2), so the landings stay exact; what changed is how many reads
it takes to get there and how much is decoded before the first kept
sample.

### 10.1 Where the time went (the model, from M3's gate log)

Each start in the log is four lines: the `[opus] open:` line's reads, KB
and ms, the `[audio] Opus: starting` line's probes, reads, KB and ms, and
the `[audio] refill: first audio in the ring N ms after the request`
line. A seek (`qs`, the seek bar) is a new request, so the backend opens
the file again for it: every seek in the log is preceded by a second
`[opus] open:` of the same file, a second or two after the first.

| Phase | `part 1` 30 s (128k VBR, 16 KB pages) | `cbr510k` 30 s (64 KB pages) | `mono64k` 30 s (8 KB pages) |
|---|---|---|---|
| The request to the open (the console's line, the decode task's wake, the file's FAT open) | ~20 ms | ~20 | ~20 |
| The open: the headers | 4 reads / 17 KB: the BOS page, the OpusTags page's header read twice (once for the "another BOS page?" check), the first audio page in three 8 KB slices | 4 / 65 KB (the 64 KB page in 9 slices) | 4 / 9 KB |
| The open: the tail scan | 9 reads / 67 KB: the 8 KB window misses the last page's header (the last page of a 16 KB-page file is longer than 8 KB about half the time), the 73 KB window in 16 KB chunks, then the last page read whole for its CRC | 13 / 81 KB | 4 / 8 KB |
| The open, in all | **13 reads / 84 KB / 84 ms** | **17 / 146 KB / 138 ms** | **8 / 17 KB / 24 ms** |
| The plan | **7 probes, 21 reads / 63 KB / 127 ms**: every probe a 290-byte header read at the guess (never a page's start: wasted) then ~2.5 chunks of 4 KB, the walk's header reads, Q whole (2 reads, 16 KB) | **5 probes, 73 reads / 318 KB / 383 ms** (Q 64 KB whole, the 4 KB chunks across 64 KB pages) | **2 probes, 6 reads / 12 KB / 30 ms** |
| After the plan to the first audio | ~125 ms: Q (when a packet continues out of it) or the page after it read again in 8 KB slices (a pass each), the preroll's ten 20 ms frames at ~7-8 ms (three a pass), the first kept frame | ~250 ms (Q's 64 KB again in 8 slices, ten frames at ~12 ms) | ~75 ms |
| **First audio after the request** | **353 ms** (the model's 356) | **793 ms** (791) | **151 ms** (149) |

The card's cost, fitted to these figures (`tools/opus_check`'s model since
M4, `opus_check.py seek`'s `ms` columns): **~3 ms a read and ~0.65 ms a
KB** (21 reads / 63 KB in 127 ms, 73 / 318 in 383, 6 / 12 in 30; the open's
sequential 16 KB chunks a little cheaper). A 290-byte header read costs
what a 4 KB chunk does less 3 ms, so the probes' wasted header reads were
~20 ms of a 128k plan, and the second open ~84 ms of a 128k seek that the
first open had already paid for.

### 10.2 What it built

| Piece | What |
|---|---|
| **The open cache** (`lib/core/OpusOpenCache`, `oggopus::OpenRecord`, `Reader::openFrom()` and `record()`; the backend's `findOpusRecord()`, `putOpusRecord()`, `loadOpusCache()`, `saveOpusCache()`) | Everything an open and its tail scan read from a file (the head, the serial number and the BOS page's CRC field, the first audio page's offset, g0, the first granule page, the last page's offset and granule, the link's end, the tags' size: 88 B) kept per path in a 48-entry LRU in PSRAM (48 x 104 B = 4,992 B), keyed by the path's hash (FNV-1a 64) and the file's size. The next open of the same path at the same size is **one read**: the BOS page's header, whose serial number and CRC field must be the record's (`openFrom()`: re-encoded, a file has a new random serial and a new head; retagged with the same padding it keeps both and the record stays right, the audio pages not having moved; a refused record is followed by the full open, which replaces it). So a seek on the playing track (the backend's second open), a track played before and the boot's resume point open with no tail scan. Persisted on the card as `/.player/opus.idx` (the library's cache rules: "MPOC", a version, the entry size, the count, the entries oldest first, Ogg's CRC-32 over all of it; another version's blob is Outdated, one that doesn't sum Corrupt, and either way the cache starts empty and fills again), loaded at the backend's `begin()` (the loop task, before any play: `[opus] the open cache: N of 48 entries (4992 B of PSRAM) loaded / no cache yet / ... /.player/opus.idx in N ms`) and saved from the loop task 3 s after its last change (`[opus] the open cache saved to /.player/opus.idx: N entries, N B, N ms (saves N, failed N)`; written aside as `opus.idx.tmp` then swapped in, as the library's cache is; never on the decode task's start path; a write that fails leaves the cache dirty and is tried again 10 s later, the queue saver's wait). Nothing in it is believed on its own: the check at the open is what makes a stale or wrong entry harmless. The open line says which it was: `[opus] open: ... length N ms (exact, from the last page), from the cache's record, checked in 1 reads / 290 B in N ms` |
| **The open** (`Reader::open(bool withTail)`) | The firmware opens with `open(true)`: the tail scan runs between the OpusTags pages and the first audio page, so that page is still in hand when the open returns and the first pass (or a `restart()`) reads nothing for it (`reset()` keeps the page in hand when it is the one the walk starts at). The headers and the first audio page are read whole whatever the slice (two reads for a 16 KB page, not three; two for a 64 KB page, not nine). The OpusTags page's header is read once (M3 read it twice). **The tail scan's windows:** the file's last 16 KB as one chunk, walked by page headers in memory, the last page checked there too (it ends at the file's end, so it lies in the chunk whole: `ogg::PageReader::pageInChunk()`, no read for the check); then the last 64 KB as one chunk the same way (a 64 KB page's); then M3's 73 KB window in 16 KB chunks with the page read whole (a junk tail of up to 8 KB after a page of the largest size), then the bisection for a chained file as before. A walk over junk scans the chunk in hand before reading on (`findHeaderInChunk()`: a tail of crafted headers cost a chunk's read per junk byte), and a scan whose chunk ends at the file's end no longer re-reads its last 3 bytes. An mStream 128k file's open: **5 reads / ~33 KB** (the BOS page, the tags page's header, the 16 KB window, the first audio page whole), ~30 ms by the model, against M3's 13 / 84 KB / 84 ms |
| **The plan** (`Reader::planStart()`, `probeOurs(atPage)`, `checkProbe()`) | A probe's guess is never a page's start, so no header is read there: the 4 KB chunk scan starts at the guess (M3 read 290 bytes at every guess first). A step along the file (the walk after the bisection) takes the next header from the chunk in hand when a probe's scan left it there, else one small read as before. The bisection's upper point is the last page itself (its offset and granule from the tail scan, exact) instead of the file's end. Q, read whole by its check, **stays in hand**: a start at Q (a packet continues out of it: `skipPage`, every start on the 510k file and about four in five on a file whose packets span pages) reads nothing until the page after; a start at the page after Q reads that page. On the device's own seconds (the runner's `plan` command, the host; M3's reader built from the commit before for the comparison): `album/01` 30 s 21 reads / 63 KB -> **15 / 65 KB**, `ms128k` 60 s 25 / 81 -> **10 / 48**, `ms128k_dmg` 40 s 25 / 78 -> **11 / 52**, `f60` 12 s 19 / 65 -> **9 / 43**, `ytdl` 30 s 23 / 74 -> **16 / 72**, `cbr510k` 30 s 73 / 318 -> **37 / 193**, `mono64k` 30 s 6 / 12 -> 8 / 28 (one start: the new upper point moved its guesses), **the same page for every one of the 17 seconds** (the plan is a function of the file: the device's `starting` line and `opus_check.py plan`'s must still agree, and they name the same bytes as M3's). Over 50 random starts a file (`opus_check.py seek`, the play's own page reads included): `ms128k` 21.2 reads / 79 KB -> **15.7 / 73**, `ytdl` 19.4 / 72 -> 15.4 / 71, `cbr510k` 60.2 / 299 -> 46.8 / 258, `mono64k` 8.8 / 17 -> 7.4 / 19 |
| **The preroll** (`oggopus::kSeekPrerollMs`: **200 ms**, M3's) | M4 first cut it to 160 ms, measured on the research's 54 files (the card set, `fit/media`, `decoders-licences/vec`) with one seed's 50 random starts a file (the 4,096 samples after the target against the decode from the top, `opus_check.py seek --prerolls`): nothing under 35 dB, CELT 44 dB at worst. The review ran the same runner with other seeds and found 128k files at 32-33 dB; the re-measure over ten seeds (500 starts a file: 10.7's table) puts the mStream 128k transcodes at 30-32 dB at their worst start with 160 ms against 35-37 with 200 (38-42 with 240), so 200 ms stays: the two frames (~16 ms of decoding) that 160 would have saved bought 3-6 dB where the bar is. The resume preroll stays 600 ms (bit-exact decoded PCM on CELT: a resume happens once a boot and its reason holds). One pre-existing figure noted on the way: on `cbr510k_dmg` one resume in 50 lands within 10 LSB, not 4 (its preroll crosses the damaged page: the concealment's state differs from the top decode's); M3's reader gives the same |
| **The step after a page** (F2dmg: `Reader::advance()`) | With a slice set, the call that reads a page's last slice and checks its CRC says Pending once more; the next call hands the first packet, so the decode is a step of its own. M3's device check measured the longest step at 19.5-28.7 ms on every file, damaged or not (`cbr510k` clean 26.5, `ms128k` clean 17.7-20.2): the last 8 KB slice (~5 ms), the CRC over the whole page (~3 ms for 64 KB) and the first decode call (8-12 ms) in one step. Now the longest step is the longest of the two: a decode call (12.2 ms at most on the 510k files) or a slice with its CRC (~8 ms), under the 15 ms budget. The cost is one Pending (one short pass, ~1 ms) a page: a second of audio |
| **The runner** (`tools/opus_check`) | `opus_check.py seek` takes `--prerolls 80,120,160,200` (the default pair is the reader's seek preroll and 600) and prints each file's modelled device time (`ms` and `max`: 3 ms a read and 0.65 ms a KB). The runner's `plan` and `seek` default to `kSeekPrerollMs`; since the review, `seek --seeds K` runs K seeds from `--seed` on and reports the worst, the maxima and the means over all of them (10.7: one seed's 50 starts miss a file's worst start by 5-10 dB) |

### 10.3 Host tests

- test_ogg_opus, 45 tests (44 at M3). New: test_open_with_tail_and_page_in_hand
  (`open(true)` on the 60 s file is five reads and the first `next()`
  reads nothing, a restart in hand reads nothing, the length and the last
  page are `open()` then `scanTail()`'s; the 64 KB-page file's tail is two
  reads with the page checked in memory; on the spanning file a start at
  Q reads the page after Q alone, two reads, and four once Q has left the
  buffer; on the sliced 60 s file the call that makes a page whole says
  Pending and the next hands its first packet with no read). Changed:
  test_sliced_page_reads (the first audio page in hand after the open:
  three pages read after it, eight Pendings each; the restart block
  rewritten around the page in hand), test_resync_in_steps (the Pendings
  recounted: 40), test_start_plan (the preroll from the constant; the
  from-the-top count computed: 178 at 200 ms, 172 at 160), and every
  test that scans the tail after the open (the page in hand goes back to
  the walk's start: `dropPage()`). The M4 bugs they caught on the way: a
  page in hand dropped by the tail scan left the walk past it (the first
  page never handed over) and its sequence number in the continuity
  check (a false gap); a scan's 3-byte re-read at the file's end; a junk
  walk's chunk read per junk byte (the bounded tail test's 512 KB).
- test_opus_open_cache, 5 tests (new): the keys (path and size; another
  size evicts the path's old entry; the same key replaces; forget and
  clear; no block: every find a miss), the LRU (a hit is the most recently
  used; the oldest goes), the blob's round trip (every field, the recency
  preserved, read in 7-byte pieces, an empty cache), the corrupt blob (a
  flipped byte anywhere, a cut-off file at five lengths, a wrong magic,
  another version or entry size: Outdated, a count over the capacity, an
  entry with 0 channels; each leaves the cache empty, never half loaded;
  no block: NoMemory) and the record through the reader (a file opened
  from its record with one read knows what the full open knew, plans the
  same start and keeps the same samples, reads the first audio page at
  its first `next()`; another file of the same size is refused with that
  one read, by its serial and by its head's CRC alike; a record with the
  wrong size, an offset past the end or an empty stream's length is
  refused with no read; a file whose length isn't known gives no record,
  before the tail scan and after one that finds no last page (the last
  two pages damaged); through the cache end to end); since the review,
  the round trip's blob marked dirty again (a write that failed) saves
  the same blob once more.
- Since the review (10.7): test_ogg_opus's refusal test takes an empty
  stream (the EOS granule exactly the pre-skip) as no audio, by its first
  page or by the tail scan, and plays the one-sample stream after it;
  test_playback's `test_an_end_at_0_00_is_a_failure` and
  test_gapless_player's `test_an_empty_entry_is_a_failure_under_repeat`
  (the real engine, the self-join of an empty track) are the player's
  guard.
- The suite: 1,173 tests, all green (1,115 at M4's commit, 1,171 after dev's merge brought its 56, 1,173 with the review's two; 1,108 at M3; test_audio_tap's threaded writer-and-reader test failed once in a run that overlapped the firmware build, every core busy, and passes on its own: not M4's).

### 10.4 The expected latency, after (the model)

The same phases as 10.1, with M4's reads at the model's 3 ms a read and
0.65 ms a KB, the 200 ms preroll's ten frames (M3's, kept: 10.7), and Q
in hand:

| Start | The open | The plan | After the plan | **First audio, expected** | M3 measured |
|---|---|---|---|---|---|
| A seek on the playing 128k track (`qs`, the seek bar: the second open hits the cache) | 1 read: ~4 ms | 10-17 reads / 48-77 KB: ~60-90 ms | the page after Q in slices ~17 ms (0 when Q itself is the start), ten frames ~75 ms, the first kept frame ~8 | **~165-215 ms** | 287-453 |
| The same on `cbr510k` | ~4 | 37 reads / 193 KB: ~240 ms | Q in hand (0), ten frames at ~12 ms ~120 | **~380 ms** | 793 |
| The same on `mono64k` | ~4 | 7-8 reads / 20-28 KB: ~40 ms | ~17 + ~56 + ~8 | **~120 ms** | 151 |
| A play from the top of a 128k track, its first open ever | 5 reads / 33 KB: ~36 ms | - | the first page in hand (0), the pre-skip's frame ~8 | **~65 ms** | 146-222 |
| A play from the top of a track played before (the cache, persisted) | ~4 | - | the first page in slices ~17, a frame ~8 | **~50 ms** | |
| The resume point at a boot (the 600 ms preroll; the cache persisted) | ~4 | ~60-90 | thirty frames ~240 | **~330 ms** | 462 |
| The first open of `cbr510k` | the 16 KB window misses (its last page is 64 KB), the 64 KB window: 2 reads / 81 KB plus the headers' 64 KB page: ~140 ms | | | | 138 |

The request's own ~20 ms (the console or the touch to the decode task's
wake, the FAT open) is in every row. The 128k-class seek lands at the
target's ~200 ms, just over FLAC's band (99-180), the open from the
cache, the plan's reads and the preroll's ten frames sharing what is
left (M4's 160 ms preroll would have taken ~16 ms off: 10.7 says why it
didn't stay); the resume at a boot is
what its 600 ms preroll costs (~240 ms of decoding), as the research
chose.

### 10.5 The device checks M4 needs (the next session; silent mode `z` first)

The runbook of section 2 and the card set of 9.6 apply (the daemon holds
COM3; `z` after every boot; the m2card's four-part album under `/music`
and the folder of `mono64k`, `hyb32k`, `silk16k`, `f60`, `f120`,
`cbr510k`, `ytdl`, `ms128k_dmg`). M4 changes no landing, so S1-S4 and S7
of 9.6 must pass as before (the plan lines' bytes, granules and probe
counts equal to `opus_check.py plan`'s on the PC's copy for the same
second, which is built from this tree: the reads differ from M3's, the
pages don't; `(the 200 ms preroll)` in every seek's line, `(the 600 ms
preroll)` at the resume). What M4 adds:

| # | Check | Commands | Proof in the log |
|---|---|---|---|
| S5 | Latency, every start again: S1's and S3's starts of 9.6, the seek bar's taps and drags of S4, a play from the top (`i<n>`) of each kind | the same as 9.6 | `[audio] refill: first audio in the ring N ms after the request`: a `qs` on a 128k track (the album's parts, `ytdl`, `ms128k_dmg`) **~165-215 ms, the target's ~200** (10.4; M3 287-453), `cbr510k` ~380 (793), `mono64k` ~120 (151), `f60`/`f120` ~165-200 (273, 326); a play from the top of a 128k track ~65-100 ms (146-222); the resume at the boot ~330 (462). Record each file's, with its open and plan lines |
| S5open | The open's cost: the first open and the second | the first play of each file after the boot (`i<n>`), then `qs30` on it | the first: `[opus] open: ... N reads / N B in N ms` with **5 reads / ~33 KB (a 128k file; the first audio page's size plus ~17 KB), ~30 ms** (M3 13 / 84 KB / 84 ms); `cbr510k` 6-7 reads / ~150 KB (the 64 KB window after the 16 KB one, and its 64 KB first page); the second (the `qs`): `from the cache's record, checked in 1 reads / 290 B in N ms` (1-5 ms) |
| S5cache | The cache's file: the boot line, the save, a boot after it | the boot; 3 s after the first open `[opus] the open cache saved to /.player/opus.idx: N entries, N B, N ms`; `!reset`; the boot line again; a play of a track played before the reset | the first boot `[opus] the open cache: 0 of 48 entries (4992 B of PSRAM) no cache yet /.player/opus.idx`; after the reset `N of 48 entries ... loaded ... in N ms` (N the files opened before it); the play's open `from the cache's record` on its first open after the boot; the queue's resume point at the boot (S3 of 9.6) `from the cache's record` too, and its `[audio] refill` ~330 ms |
| S5plan | The plan's cost | S1's `qs30` lines | `N probes, N reads / N KB in N ms` with the reads and KB of 10.2's table for the same file and second (`album/01` 30 s: 7 probes, 15 reads / 65 KB; `ms128k_dmg` 40 s: 5 probes, 11 reads / 52 KB; `cbr510k` 30 s: 4 probes, 37 reads / 193 KB), the ms by the model (10.1: 3 ms a read and 0.65 ms a KB), and the same page byte and granule as M3's log (S1's table in the M3 results) |
| F2dmg | Follow-up 4 again: `Rf cbr510k_dmg` and `Rf ms128k_dmg` to their end, and `Rf cbr510k` | as 8.11 | the end line's `longest step` **<= 15,000 us** on all three (M3: 28,657, 19,543 and 26,530: the slice, the CRC and the first decode in one step), `longest decode call` as before (12,241 at most); `pass_max` <= 30,000 (G6); 1 gap, bad 1, resyncs 1, exact on the damaged copies |
| S6 | `s` within a second of each request, as 9.6 | | `pass_max` the new start's own, <= 30,000 us |
| G5 | The internal RAM with the cache | `s` while an Opus track plays with the headphones linked | `ram=` steady >= 50K: the cache's entries are PSRAM (`4992 B of PSRAM` in the boot line); the backend object grew by ~100 B of DRAM (10.6) |
| host | The samples after a landing, as 9.6's `host` row: each start's plan decoded on the host against ffmpeg's decode from the top | `opus_check.py plan` (built from this tree) per start | >= 35 dB on the 128k files but for a quiet start (10.7's measure at the 200 ms preroll: the mStream 128k transcodes 35-37 dB at their worst start over 500, and a few quiet starts score 26-29 at any preroll; SILK and hybrid 34-40); M3's rows gave 39-97 dB at the same preroll, so these should too |

**If a seek is still over ~215 ms on a 128k file:** the log's three lines
say where (the open's reads, the plan's reads and ms, and the rest);
the plan's ms against the model tells whether the card is slower than
M3's figures (the FAT's cluster walk on a large file: a probe into a big
file seeks far), and 10.4's per-phase figures say what to expect from
each.

### 10.6 The build

Measured as section 1's table was (the output sections of `firmware.map`),
against M3's build (9.7's figures, the same tree before M4):

| | M3 | M4 | Delta |
|---|---|---|---|
| IRAM (`.iram0.vectors` 1,028 + `.iram0.text` 124,867) | 125,895 B | 125,895 B | **0** |
| DRAM (`.dram0.data` 24,328 + `.dram0.bss` 32,120) | 56,352 B | 56,448 B | **+96 B** (`.bss`: the backend's cache object, the lock, the path and the counts; its entries are PSRAM) |
| `.flash.text` | 1,592,588 B | 1,597,980 B | +5,392 B (the cache and its blob, the record and the open from it, the tail windows and the chunk scans, the backend's load and save) |
| `.flash.rodata` | 596,672 B | 597,192 B | +520 B (the log lines) |
| Image (core2, `Flash:` line) | 2,339,483 B | 2,345,395 B | **+5,912 B** (37.3 % of the slot) |
| Image (core2-dio) | 2,339,547 B | 2,345,459 B | (core2 + 64 B, as ever) |

`iram_diet`, `cache_guard` and `flash_guard` pass on `core2` and
`core2-dio`; no `IRAM_ATTR`. The cache's entries (48 x 104 B = 4,992 B;
the review corrected both figures, 10.7, and two `static_assert`s in
OpusOpenCache.h now pin them) are one PSRAM block from the backend's
`begin()`, beside the run index's slots; the record a fresh open fills is
88 B on the decode task's stack.

### 10.7 The review's findings, fixed

The review of M4 (the documents, the engine, the tests, the privacy rule)
found these; each was first checked against the code or re-measured.

- **The 160 ms seek preroll rested on one seed** (medium). 10.2's
  measure ran `opus_check.py seek` with its default seed, 50 starts a
  file, and 160 ms was the smallest preroll with nothing under 35 dB. The
  review re-ran the same runner with seeds 2-5 and found `ms128k` at
  32.3 dB, `long128k` 33.0, `ytdl` 30.9 and `machine_128k` 21.5 at
  160 ms, where 200 ms gave 59.5, 40.5, 41.7 and 59.3: a file's worst
  start isn't in one seed's 50. Re-measured here over ten seeds (500
  starts a file, 54 files, 160 / 180 / 200 / 240 ms), the worst start of
  a file:

  | Files | 160 ms | 180 | 200 | 240 |
  |---|---|---|---|---|
  | mStream 128k transcodes (`ms128k`, `long128k`, `ytdl`, the research's `killers_128k`; its `machine_128k` after the semicolon) | 30-32 dB; 21.5 | 33-35; 26.2 | 35-37; 43.1 | 38-42; 31.0 |
  | mStream 192k transcodes | 36.5-37.0 | 39.1-39.5 | 40.5-40.7 | 44.2-45.5 |
  | ffmpeg's own libopus encodes at 128k (`fit/media`, `vec`), the 256k, 510k and CBR files, `mono64k`, `pic`, `f10`-`f120` | 45.5-56.6 | 50.3-60.7 | 56.2-61.2 | 60.1-66.8 |
  | mStream 64k and 96k transcodes | 22.0-25.3 | 24.0-26.3 | 25.3-29.3 | 29.2-47.4 |
  | SILK and hybrid (`silk16k`, `hyb32k`, the voice files, the VoIP vectors) | 30.6-38.6 | 31.6-38.8 | 34.2-40.2 | 33.0-42.5 |
  | the damaged copies, a start inside the gap | -5.8, 0.0 | the same | the same | the same |

  Two things the one-seed figure hid: a file's worst start over 500 is
  5-10 dB under its worst over 50, and a few starts score the same at
  every preroll (`killers_128k`'s seed 5 29.3 dB at all four, `ms128k_2`'s
  seed 10 26-28; `machine_128k`'s seed 10 is one such passage, 21.5 at
  160 and 31.0 at 240 with 43.1 at 200: a start's figure isn't monotonic
  in the preroll, the decoder's state differing by the packet it began
  at): a quiet window, where the codec's own noise is the floor, not the
  decoder's state. SILK and hybrid never converge bit for
  bit (7.3), and the 64k and 96k transcodes converge slowly (quiet
  passages at a low bitrate). So the project's 35 dB bar is met at the
  worst start of the mStream 128k transcodes by 200 ms (M3's figure,
  device-checked at M3) and not by 160, which gave up 3-6 dB there for
  two frames (~16 ms) of decoding; 240 would buy another 3-6 dB for
  another 16 ms, which the seek's 64-frame fade-in doesn't need. Fixed:
  `kSeekPrerollMs` is 200 again, with the table in its comment; 10.2,
  10.4 (~165-215 ms expected on a 128k seek), 10.5 (`(the 200 ms
  preroll)`, the `host` row's figures), the status and 9.5's note;
  SEEK.md 5.4, SEEK-BAR.md's Opus row, ARCHITECTURE.md, the README and
  the firmware's comments; test_start_plan's from-the-top count is 178
  (172 was 160's). `opus_check.py seek --seeds K` runs K seeds from
  `--seed` on and reports the worst, the maxima and the means over all of
  them, so one seed can't decide a constant again (the default stays one
  seed, the quick look; a figure to decide by is `--seeds 10`).
- **A failed cache write was never retried** (low). `OpusOpenCache::
  save()` cleans the cache as it makes the blob, under the lock, and the
  card write follows outside it: a temp file that wouldn't open (the SD
  mount's five handles taken by the track, the queue's write and the
  thumbnails), a short write or a failed rename only counted a failure,
  and the record waited for the next put. Fixed: `markDirty()`, which the
  backend calls on a failed write with the save due again in 10 s
  (`kOpusCacheRetryMs`, the queue saver's wait; `opusCacheDueMs_` is now
  the time the save is due rather than the time of the change); a put in
  between dirties it anyway. test_opus_open_cache's round trip marks the
  saved cache dirty again and saves the same blob once more.
- **An empty track spun under Repeat One** (low). An Opus file whose
  last granule is exactly its pre-skip is valid (RFC 7845 4.5 forbids
  only a granule under it) and opens with length 0; the reader's own test
  said so. Its play keeps nothing, the engine's self-join finds nothing
  ahead, cuts, drains and ends, `checkEnd()` sees a natural end and
  Repeat One (or All with nothing else to play) starts it again: the
  reviewer's run of the real engine gave 2,501 plays in 20,000 ticks, an
  SD open and a log line fifty times a second with Now Playing at 0:00.
  QUEUE-MODES.md had this down as a guard waiting for a file that does
  it; here is one. Fixed both ways: the reader refuses an empty stream as
  no audio ("no audio in it") when the end is known at the open (the
  first page the EOS page, or the tail scanned: `open(true)`, the
  firmware's), and `openFrom()` refuses a record of one; and the player
  takes a natural end with the position still at 0:00 as a failure, with
  the same note, which moves on even under One and stops a queue of
  nothing else (any format can hold such a track: an MP3 that is all
  encoder delay and padding, a FLAC of 0 samples). Tests: test_ogg_opus's
  refusal test (the one-page file by its first page, a two-page one by
  the tail scan, `open()` alone still opening it and playing nothing, one
  sample past the pre-skip playing), test_opus_open_cache's empty record,
  test_playback's `test_an_end_at_0_00_is_a_failure` (the fake's new
  `finish()` puts the position a second in, so the thirteen ends that set
  the flag alone still mean a track that played; test_sleep_timer's
  `endTrack()` likewise leaves a track of unknown length where its play
  got to, where it had put the position at the length, 0) and test_gapless_player's
  `test_an_empty_entry_is_a_failure_under_repeat` (the real engine: Off,
  One and All alone give one play and a stop; One inside a queue moves on
  to the next entry, which loops). Checked to fail without the guard.
- **The no-record test never ran its assertion** (medium). Its file (a
  200 KB junk tail, no EOS) is resolved by the tail scan's bisection, as
  test_long_junk_tail shows, so the `if (!scanTail())` around the
  assertion skipped it and nothing covered `record()`'s refusal of a
  length not known. Fixed: asserted on `open()` before any scan, and on a
  file the scan genuinely can't resolve (its last two pages damaged:
  neither checks, and nothing else of ours lies at the file's end for the
  walk to take), by `scanTail()` and by `open(true)` alike.
- **The cache's sizes were wrong in the docs** (medium): 112 B a slot and
  5,376 B for 48, ~72 B a record, where `sizeof` with the Core2's
  xtensa-esp32-elf-g++ and the host's g++ alike says 104 B (4,992 B) and
  88 B; the boot line prints `4992 B of PSRAM`, which 10.5's two rows
  would have failed on. Fixed in 10.2, 10.5, 10.6 and the two headers'
  comments; two `static_assert`s in OpusOpenCache.h pin the figures.
- **SEEK.md section 6 still sent the reader to a measurement to come**
  (low): "an Opus track's to be measured, OPUS.md 9.6". Fixed: M4's
  expectation and M3's measurement, as SEEK-BAR.md's row has them.
- **10.5 named the user's library folder** (low; privacy). Fixed: the
  four-part album under `/music`; and the same name in section 3's
  table, 8.9, 9.6, test_library_index's paths (a generic artist, album
  and titles now) and cardset.py's notes, from before the rule, is
  generic too (the research's own working file names stay: the
  scratchpad's, never committed).

The suite: 1,173 tests, all green (1,171 before the review's two:
test_playback 100, test_gapless_player 28). The build, measured as 10.6's table was: IRAM 125,895 B
(10.6's figure, unchanged); DRAM 56,480 B (+32 B over 10.6, dev's merge
of the Now Playing menus in between); `.flash.text` 1,603,312 B and
`.flash.rodata` 599,308 B (+5,332 and +2,116 over 10.6, the merge's
included); the image 2,352,843 B on `core2` (37.4 % of the slot; 10.6's
2,345,395 B plus the merge and these fixes: the guard in `checkEnd()`,
the reader's check, the cache's retry) and 2,352,907 B on `core2-dio`
(core2 + 64 B, as ever); `iram_diet`, `cache_guard`, `flash_guard` and
`version` pass on both; no `IRAM_ATTR`.
