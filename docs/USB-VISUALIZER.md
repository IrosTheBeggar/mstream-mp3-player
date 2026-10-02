# USB visualizer

While mstream-terminal-player plays music on a computer, it sends beat data
down the USB cable and the Core2's dancer dances to it. The computer does
the decoding and the playing; the Core2 shows the crab.

Status: **the firmware side and the reference sender are in** (October
2026): the protocol, host mode on the Dance tab, host tests for every
portable piece, and `tools/usb_viz.py` with its own tests ([Host
results](#host-results-october-2026)), and [checked on a
Core2](#checked-on-the-device-october-2026): the handshake, the tracker at
both rates, the ways in and out, with one bug fixed there. The rest of the
[device tests](#device-test-plan) is still to run. This file is the
protocol spec, exact enough to write the Rust side from later, and the
record of the work. The feel test
came first: a PC script stepped the crab through its 16 frozen poses (the
console's `k<n>`) in time with tapped beats, and the verdict was "It's
fun, let's build the firmware demo".

The demo's scope:

- The firmware drives the **existing Dance tab**. A full-screen takeover,
  60 fps, a spectrum strip and so on come later (polish).
- A Python reference sender in `tools/` proves the protocol now. The Rust
  sender in the terminal player comes later, after its `claude/device-flash`
  branch merges (that branch owns the serial port code).
- Nothing on the Core2 plays by itself, at any point. The sender plays
  audio only when the command line asks it to.

## At a glance

```
 computer (terminal player, or tools/usb_viz.py)          Core2
 ───────────────────────────────────────────────          ─────────────────────────────────────
 decoded audio ─► front end (BeatTracker step 1) ─► @h ──► BeatTracker::feedHop ─► tempo, PLL,
                  low/mid energy per 512 frames                                     confidence, grid
 play position − output delay − offset ──────────► @c ──► HostClock (min filter, slew) ─┐
 track change / seek / crossfade ─────────────────► @e ──► tracker reset, rate, prior    │
                                                                                         ▼
                                              DanceMode::render: grid.beatsAt(heard frame) ─► crab
```

The computer runs only step 1 of the Core2's own `lib/core/BeatTracker`:
the 8x box average, the DC blocker, the two 150 Hz biquads and the low and
mid band energy per hop of 512 frames. It sends those two numbers per hop.
The Core2 feeds them into its existing, device-validated tracker (tempo,
phase, PLL, confidence) through a new public `feedHop()`. Separately, about
10 times a second, the computer says which frame of the track the listener
hears now; the Core2 fits a smooth clock to those and asks the grid for the
beat at that frame, in place of `TapReader::audibleAt`.

Why this split:

- **One tracker.** The tempo, PLL and confidence code stays the one that
  was tuned and measured on the device (MASCOT-POC.md: lock in 2.5-2.9 s,
  median error 2.4-2.8 ms on click tracks). A full Rust port would be two
  trackers to keep in step. (The October 2026 rework of the tracker's
  confidence, lock and onset start, and its review, BEAT-TRACKER-EVAL.md,
  are on the Core2's side of this split: the front end, the `@h` line and
  protocol 1 are unchanged, and `feedHop()` still gives what `process()`
  gives, bit for bit, on all 275 harness cases: the 77 library tracks, the
  mid-song and join suites, and the 53 synthetic cases. On the device,
  `usb_viz.py --measure` with the reworked tracker locks the click tracks
  at the same times as before, 2.50-2.81 s, with a median error of
  2.5-3.6 ms: BEAT-TRACKER-EVAL.md, "Checked on the device". The tap's
  lead-in silence added then is on the Core2's own tap, not in host mode.)
- **Little to port.** Step 1 is about 80 lines with no tables.
- **Little to send.** Two floats 86 times a second, about 3.5 KB/s with
  the rest, a third of 115200 baud. Raw decimated audio would be 11 KB/s
  of binary.
- **No clock sync.** Hops are stamped with their frame, not with a time, so
  USB jitter can't move a beat. Only the heard clock depends on arrival
  times, and it is filtered for that.

## The link

- The Core2's USB serial port: CH9102 (USB ID 1A86:55D4), or a CP2104
  (10C4:EA60) on older units. 115200 baud, 8N1, no flow control. The baud
  rate doesn't change: it is the console's, and 3.5 KB/s fits.
- One owner. Windows opens a COM port for one process only, so while the
  sender holds it, `pio monitor` and the console tools can't. The sender
  shows the Core2's log lines itself (`--quiet` hides them).
- **Open it with DTR and RTS low, and never touch them after.** The board's
  auto-reset circuit pulls EN or GPIO0 when the two lines differ. pyserial:
  set `dtr = False` and `rts = False` on an unopened `serial.Serial()`, then
  `open()` (the repo's `shot.py` and serial daemon do this and don't reset
  the board). Rust: `serialport::new(port, 115200).dtr_on_open(false)`,
  then `write_request_to_send(false)` at once. Whether that open leaves RTS
  high for a moment on Windows is unverified; Linux raises DTR briefly on
  open whatever is asked. So the protocol must survive a reboot on open,
  and it does: see [A reboot on open](#a-reboot-on-open).
- Only open a port the user picked, or one with a Core2 bridge ID, and only
  when the user turned the feature on. Other ESP32 boards use the same
  bridges, and opening one can reset it.

## Framing

The link carries ASCII lines both ways. The computer's lines and the
Core2's protocol replies start with `@`; the Core2's ordinary log lines
never do (checked: no log line in `src/` or `lib/core` starts with `@`).

### Why `@`, and the console's rules

The serial console (`src/app/SerialConsole`) treats **every byte** as a
single-key command: `f` forgets the paired headphones and restarts, space
plays or pauses, `+` and `-` change the volume, `b` starts a decode bench,
and so on. An unprefixed line such as `beat 12.5` would fire some of them.
`@` is unused (today it falls to `default: break`).

The rules, in the order the console applies them to each byte:

1. **Inside a host line, every byte belongs to the line.** Nothing in it is
   ever a console key, whatever it is, until the line ends.
2. **`@` always starts a host line**, wherever it comes:
   - outside a line: a new line starts;
   - inside a host line: the partial line is dropped (counted as bad, no
     reply) and a new one starts at this `@`. So a sender that died
     mid-line can't swallow the next session's `@hello`;
   - while a console command waits for its argument (`i`, `k`, `B`, ...
     typed without Enter): that command is **abandoned**, logged as
     `> i: abandoned (a computer's line began)`, and the host line starts.
     The one exception: inside an `R` argument that already has text, `@`
     is text (`Rttone:1000@48000`, a tone at a rate). No other console
     argument may contain `@` (a headphones name for `c<name>` can't).
3. **A line ends at `\n` or `\r`.** Either ends it, so `\r\n` gives one
   line and then a lone `\n`, which the console ignores. A line that is
   just `@` is a syntax error.
4. **Reading that starts partway through a line drops the tail (Sync).**
   The rules above need the `@`. If the Core2 starts reading mid-line (its
   Serial starting while a computer sends: a boot; or bytes lost when the
   receive buffer overflowed), the line's tail would reach the keys: a
   `@hello`'s `viz` ends in `i` and `z`, a session id can hold an `f`, an
   `@h` line has `-`, `.` and digits after `h`. So the console starts in
   Sync, and goes back to it when the buffer is found nearly full (the IDF
   driver drops bytes only once it is full; the console is its only
   reader):
   - `@` starts a line as ever;
   - any other byte outside a line is held. If another byte follows it
     before 20 ms of quiet, it was a tail: it and the rest up to the
     terminator are dropped. If the input goes quiet for 20 ms after it, it
     was a keypress, and the console gets it then;
   - 20 ms of nothing, outside a line or a tail, ends Sync (no line can be
     under way: its bytes come back to back). Right after Serial starts,
     `SerialConsole::begin()` waits up to 20 ms for that, before the banner,
     so a quiet boot is out of Sync at once;
   - logged once when Sync ends: `[console] dropped 37 bytes of a
     computer's line already under way ...`. A line under way when the
     overflow was found ends as a bad line. A key sent with its Enter in
     one write during Sync goes with the tail: send it again.

   Host tests: every suffix of the sender's lines into a fresh reader, in
   one read and a byte at a time, gives no console byte and no key; a
   scratch replay of a 5 s `--dry-run` stream (17,257 bytes) from every
   offset gives none either (it was 15,092 offsets that ran commands
   before Sync).
4. **Bytes allowed in a line:** printable ASCII, 0x20-0x7E. Any other byte
   (a control character, a tab, 0x80 and up) makes the line bad; it is
   still consumed up to its end, then answered `@err 1 -`.
5. **At most 255 bytes from the `@` to the last byte before the
   terminator.** Byte 256 makes the line overlong: the rest is consumed up
   to the terminator (never as console keys), then answered `@err 6 -`.
6. **There is no timeout inside a line.** A line only ends at its
   terminator (or at the next `@`), so a slow writer can't have its tail
   read as console keys.
7. The console **never echoes** a host line (no `> ...` lines). Errors are
   counted and answered; the log names the verb at most, never the fields
   (later lines may carry a Wi-Fi password).

What the computer must do in return: **send only `@` lines.** No bare
bytes, no newline on its own, no keep-alive characters. A newline sent on
its own would end a half-typed console command and run it.

### Fields

```
@<verb>[ <field>]...<terminator>
```

- **Verb:** `[a-z][a-z0-9]*`, optionally dotted for a namespace
  (`wifi.set`), at most 16 bytes. Lowercase only.
- **Fields** are separated by one or more spaces (senders use exactly
  one); leading and trailing spaces are ignored. A field is 1 or more bytes
  of 0x21-0x7E. At most 8 fields after the verb.
- **Extra trailing fields are ignored**, in both directions. This is how a
  protocol version grows: a new optional field goes at the end, and an old
  receiver doesn't see it. Changing or removing a field is a new protocol
  version.
- **Unsigned integers** (`u32`): decimal digits only, no sign, at most
  4294967295.
- **Signed integers** (`i32`): an optional `-`, then digits,
  -2147483648..2147483647.
- **Numbers with a fraction** (`f32`): `-?[0-9]+(\.[0-9]*)?([eE][-+]?[0-9]+)?`,
  for example `0`, `0.0291153`, `1.49884555e-15`. No `nan`, `inf`, hex or a
  leading `.`; the value must be finite as a 32-bit float. The Core2 parses
  them with `strtof` after checking the syntax.
- **Tokens** (session ids, versions, feature lists): 0x21-0x7E with no
  spaces, as each message says.
- **Strings** (titles, names, later SSIDs and passwords): base64url
  (RFC 4648 section 5, `A-Z a-z 0-9 - _`), no padding, UTF-8 inside. That
  keeps spaces and `@` out of every field.

## Sessions

### Messages

Computer to Core2:

| Line | Fields | When |
|---|---|---|
| `@hello <proto> <session> <features>` | `proto`: u32, the highest protocol version the sender speaks (this spec: **1**). `session`: token, 1-16 of `[0-9A-Za-z]`, new and random for each connection. `features`: a comma list of `[a-z]+`; protocol 1 knows `viz` | to start; again every 1 s until answered; again after any sign of a reboot |
| `@bye` | none | when the sender stops (a clean exit, the user turned it off, the port is wanted for flashing) |

Core2 to computer:

| Line | Fields | Meaning |
|---|---|---|
| `@ok <proto> <session> <fw> <caps>` | `proto`: the version chosen, the lower of the sender's and the Core2's highest. `session`: echoed. `fw`: the firmware version (`version::player()`, e.g. `v0.5.0-beta.1`, no spaces). `caps`: a comma list of what it supports: `viz,log` in this firmware | the session is up |
| `@err <code> <verb> [<detail>]` | `code`: u32 (table below). `verb`: the offending line's verb, or `-` when it had none or was cut off. `detail`: one token, only where the table says | the line was refused and had no effect |
| `@bye <why>` | `ok` (answering the computer's `@bye`), `user` (ended on the Core2: a touch or a button), `timeout` (nothing for 3 s), `dance` (the Dance tab went away) | the session is over |

Device replies end in `\n` (a log line made with `println` ends in
`\r\n`): the computer strips a trailing `\r`. Every reply is written with a
single `printf`, so it isn't split by another log line, though a log line
from another task can still land in the middle of one in a bad case: the
computer ignores a line it can't parse and retries what it was waiting for.

### Versions

The Core2 speaks protocol versions `min..max` (this firmware: `1..1`).
`@hello` carries the sender's highest. The Core2 answers with `min(sender,
max)` if that is at least `min`, else `@err 2 hello <min>-<max>` (`@err 2
hello 1-1`), and the sender speaks one of those or gives up and says why
("the Core2's firmware is too old/new for this player: update it").
Optional additions within a version go at the end of a line (old receivers
ignore them) or as new verbs listed in `caps` (the sender sends a verb only
if the Core2 listed it).

### Session rules

- `@hello` with `viz` among its features starts **host mode** (see
  [Host mode on the Core2](#host-mode-on-the-core2)), or answers `@err 4
  hello <detail>` when it can't now (below). Features the Core2 doesn't
  know are ignored; if none is known: `@err 7 hello`.
- `@hello` during a session with **another session id** is the sender
  starting over (it restarted, or saw a reboot): the Core2 keeps host mode
  and the screen, forgets the epoch, and answers `@ok`. The same id again
  (a retry that crossed the `@ok`) is answered `@ok` again and changes
  nothing.
- After `@ok`, the sender sends `@e` before any `@h` or `@c`. Data lines
  before a session get `@err 3 <verb>`; data lines in a session before the
  first `@e` get `@err 9 <verb>`.
- **Any valid line keeps the session alive.** After **3 s** with no valid
  line, the Core2 ends the session (`@bye timeout`). The sender keeps `@c`
  going at 10 Hz while it is paused for exactly this.
- **The user wins.** When a touch or a button ends host mode on the Core2,
  it sends `@bye user` and then **declines**: `@hello` and data lines get
  `@err 8 <verb>` until 3 s pass with no line at all from the computer.
  The sender, on `@bye user` or `@err 8`, stops sending and doesn't try
  again until its user asks (the terminal player shows "Stopped on the
  Core2"). A sender that keeps streaming stays declined; one that stopped
  and is started again later gets in.
- `@bye` from the computer ends the session at once; the Core2 answers
  `@bye ok` (useful before flashing: the port is free when it arrives).
  `@bye` outside a session is ignored.
- **Quiet while the Core2 boots.** The sender writes nothing from the ESP32
  ROM's lines (`ets `, `rst:`) until the firmware's banner
  (`mstream-mp3-player `), and listens ~300 ms after opening the port
  before its first `@hello`. See [A reboot on open](#a-reboot-on-open).

## The visualizer's messages

All in a session with `viz`.

| Line | Fields | Rate |
|---|---|---|
| `@e <epoch> <rate> <prior>` | `epoch`: u32, the new epoch's number. `rate`: u32 Hz, **44100 or 48000** (anything else: `@err 5 e`). `prior`: f32 BPM, **0** for none or **30-300** | at the start of each epoch, before its first `@h` or `@c`; again with the same epoch and rate to change only the prior |
| `@h <epoch> <hop> <low> <mid>` | `epoch`: u32. `hop`: u32, the hop number in the epoch (frames `hop × 512` to `hop × 512 + 511`). `low`, `mid`: f32, the hop's low and mid band energy, 0 to 1e4 (negative, larger, or not finite: `@err 5 h`) | one per 512 frames of audio as it is decoded: 86.13/s at 44.1 kHz, 93.75/s at 48 kHz; bursts of up to 16 lines are fine |
| `@c <epoch> <heard> <playing>` | `epoch`: u32. `heard`: i32, the epoch frame the listener hears at the moment the line is written (may be negative, see below). `playing`: `1` the audio is advancing, `0` it isn't (paused, stopped, buffering) | **10 Hz** (5-20 Hz allowed), paused or not; at least once a second |
| `@log <level>` | u32: `0` off, `1` a `[beat]` line per tracker beat, `2` also a `[flash]` line per beat drawn (for tests) | when wanted; back to 0 when the session ends |

Reserved for later, with these shapes so the Rust side can plan for them.
This firmware answers `@err 7` and doesn't list them in `caps`:

| Line | Meaning |
|---|---|
| `@s <epoch> <heard> <bands>` | a spectrum for the strip: `bands` is 32 hex digits, 16 bands of one byte each, log-spaced 60 Hz to 16 kHz, 0 = -60 dBFS and 255 = 0 dBFS, for the frame `heard`; at most 30 Hz. Cap `spectrum` |
| `@t <epoch> <title> <artist>` | the track's title and artist (base64url) for the Dance tab's bottom line. Cap `title` |

### Epochs

An epoch is one unbroken run of one source's frames. Frame 0 of an epoch is
the first frame of the audio the sender's front end processes in it; hop
numbers and heard frames count from there. Every frame of the run is in
the front end once, in order. **Anything that breaks that starts a new
epoch:**

| Event | Epoch | Notes |
|---|---|---|
| A new track (a skip, the queue moving on, gapless or a hard cut) | new | frame 0 = the new track's first frame |
| A seek, however small | new | frame 0 = the frame sought to, so `heard` counts from the seek target |
| A crossfade | new, when the sender's tap switches to the incoming track | frame 0 = the incoming track's first frame. The terminal player's tap follows the incoming track as the crossfade starts, so the old track's last beats aren't danced (the crab idles into the new lock) |
| Pause and resume | **same** | the frames carry on where they stopped, so the tracker keeps its lock and the crab dances again at once. `@c` says `playing 0` meanwhile. If the sender can't guarantee the frames carry on (it drops what it had buffered), it starts a new epoch on resume instead |
| The sender's own feed dropped audio | same, with a gap in the hop numbers; or new | see [Hops](#hops) |
| 12 hours in one epoch (a live stream) | new | keeps every frame count far from 2^31 |

- Epoch numbers are chosen by the sender; it adds 1 each time. The Core2
  only compares them for equality: any `@e` with a number other than the
  current one starts a new epoch.
- On a new epoch the Core2 resets the tracker, sets its rate and prior,
  restarts the heard clock, and the crab idles until the new lock (as on a
  track change today, about 3 s).
- `@h` and `@c` lines for any epoch but the current one are dropped
  silently (counted as `stale`). They shouldn't happen: everything goes
  down one ordered stream.
- `@e` for the current epoch with the same rate changes only the prior (a
  BPM that arrived late from the server). With another rate: `@err 5 e`.
  The rate can't change within an epoch.

### Hops

- Hop `n` covers epoch frames `512n` to `512n + 511`. The hop size is 512
  frames at both rates in protocol 1 (it is `BeatTracker::Config::hop`).
- The first `@h` after `@e` may have any number (normally 0). The tracker
  resets with its origin at frame `512 × hop`.
- Then each `@h` must be the previous number plus 1:
  - **the next number:** fed to the tracker;
  - **the same or lower:** a duplicate, dropped (counted);
  - **higher: a gap.** The tracker resets with its origin at the new hop
    and starts over (counted as a gap; the `[dance] tracker reset (gap in
    the computer's hops)` line is logged at most once a second). A gap
    means lines were lost (the Core2's receive buffer overflowed, or the
    sender's own feed dropped audio): both are bugs to find, and the
    counters in the `[dance]` line make them visible. Filling a short gap
    with the last hop's energies would keep the lock through a lost line;
    that can come later if the counters show gaps in real use.
- If the sender's own feed loses audio, it either skips the hop numbers
  that audio would have had (the Core2 resets; the epoch's frame numbers
  stay right, so the heard clock is undisturbed) or starts a new epoch.
  It resets its front end's filters either way.
- Hops should run ahead of what is heard (they come from decoded audio,
  which is ahead by the output's buffer). They don't have to: the grid
  extrapolates, so hops up to about a beat behind still give the right
  beat. They never have to be sent at an even pace; bursts are normal (the
  terminal player's tap delivers 2048 frames at a time: 4 hops).

### The front end

The sender computes, per epoch, from the audio its listener hears, before
volume (full scale), with all state zero at the start of the epoch or after
its own gap:

1. **Mono:** `m[i]` = the mean of the channels, in full-scale units (an
   int16 sample / 32768, or the f32 sample as decoded).
2. **Box average 8x:** `x[k] = (m[8k] + ... + m[8k+7]) / 8`. Sample rate
   `fd = rate / 8` (5512.5 or 6000 Hz).
3. **DC blocker:** `d[k] = x[k] - x[k-1] + p · d[k-1]`, with
   `p = 1 - 2π · 5 · 8 / rate` (the code's approximation of a 5 Hz pole).
4. **Low band:** two biquads in series, each an RBJ-cookbook low-pass at
   `f0 = 150 Hz` on `fd`, Q = 0.5412 then 1.3066 (fourth-order
   Butterworth). Coefficients worked out in double and stored as float:

   ```
   w0 = 2π · 150 / fd,  α = sin(w0) / (2Q),  c = cos(w0),  a0 = 1 + α
   b0 = (1 - c) / 2 / a0,  b1 = (1 - c) / a0,  b2 = b0,  a1 = -2c / a0,  a2 = (1 - α) / a0
   ```

   Each runs as transposed direct form II, exactly:
   `y = b0·x + z1;  z1 = b1·x - a1·y + z2;  z2 = b2·x - a2·y`.
   `lo[k] = biquad2(biquad1(d[k]))`.
5. **Mid band:** `mi[k] = d[k] - lo[k]` (everything above, up to `fd / 2`).
6. **Energy per hop:** a hop is 64 decimated samples (512 frames).
   `low = Σ lo[k]²` and `mid = Σ mi[k]²` over the hop's 64 samples.

Reference values (from the C++ code, float32):

| | 44100 Hz | 48000 Hz |
|---|---|---|
| p | 0.994300961 | 0.99476403 |
| biquad 1: b0, b1, b2 | 0.00629974995, 0.0125994999, 0.00629974995 | 0.00537849916, 0.0107569983, 0.00537849916 |
| biquad 1: a1, a2 | -1.70313001, 0.728329003 | -1.72593498, 0.747448981 |
| biquad 2: b0, b1, b2 | 0.00684436876, 0.0136887375, 0.00684436876 | 0.00580813596, 0.0116162719, 0.00580813596 |
| biquad 2: a1, a2 | -1.85036695, 0.877744496 | -1.86380351, 0.887036026 |
| `tone:click120`, hops 0-3 (low, mid) | 0.672406, 1.09918; 0.0291153, 0.00515993; 0.00766506, 5.37276e-05; 0.0036862, 2.56443e-05 | 0.712341, 1.19113; 0.0495953, 0.0126538; 0.00879115, 5.67129e-05; 0.00452538, 3.15944e-05 |
| hop 43 (beat 1 is at frame 22050, in hop 43) | 0.261903, 0.43584 | |

The golden file `test/test_hop_feed/hop_golden.h` has every hop of two 4 s
click tracks (`click120` at 44.1 and at 48 kHz) for a port to check itself
against. `test_hop_feed` writes it (`HOP_GOLDEN_OUT=test/test_hop_feed/hop_golden.h
pio test -e native -f test_hop_feed`, only after a deliberate change to
the front end or ClickGen) and otherwise checks the front end against it
(relative 1e-5). Easy to read without a C++ compiler:

- one comment line per track: `// track 0: name=click120 rate=44100 bpm=120
  offset=0 frames=176400 mono_fnv1a=0x38cd684d hops=344 beats=8
  dc_pole=... lp1=b0,b1,b2,a1,a2 lp2=...`;
- then `kBeats<i>[]` (ClickGen's beat frames) and `kHops<i>[][2]`, one hop
  per line: `{low, mid},  // <hop>`, each a float printed `%.9g` (exact);
- `mono_fnv1a` is FNV-1a (32-bit) over the track's mono int16 samples, each
  little-endian: a ClickGen port can check its samples before its front end
  (ClickGen's `llround` is round-half-away-from-zero, not Python's `round()`;
  its shape is float math, so a port in double may differ by an LSB here
  and there, which the 1e-5 tolerance on the energies absorbs).

**Precision.** Bit-exact agreement isn't needed and isn't possible (the
ESP32 may fuse multiply-adds; Python works in double). The tracker takes
the log of each energy plus a floor of 6.4e-4 (-50 dBFS over a hop), so
send at least 6 significant digits (`%.6g`; Rust's `{}` for f32 prints the
shortest exact form, which is better). Energies under 1e-9 may be sent as
`0`. A host test holds the tracker to the same lock and phase with 1e-4
relative noise on every energy.

`tools/usb_viz.py` does better than it needs to: its front end rounds every
operation to float32 in the C++ order (`struct`), which matches the golden
file bit for bit, and it sends `%.9g` of each float32, tiny ones included
(not `0`), which `strtof` reads back to the same bits. Fed its lines, the
firmware's own `HostLine`, `HostLink` and `BeatTracker` give the tracker that
`process()` gives on the same audio, bit for bit ([Host
results](#host-results-october-2026)). Sending `0` under 1e-9 instead broke
that after a few hundred hops on two tracks of six (with the same lock and
errors).

### Sample rate

The tracker's tempo search, filters and smoothing are built for one rate
(`BeatTracker::Config::sampleRate`, tables sized in `begin()`). The Core2
sets it from each `@e`: 44100 or 48000, the rates the tracker is tested at
(48 kHz is validated by new host tests before the demo ships; the device
tap path only ever saw 44.1 kHz, since the rate converter brings every
track there). Changing the rate rebuilds the tracker's tables (about 9 KB
in PSRAM, freed and allocated again); the same rate again costs nothing.

A track at another rate: the sender brings it to one of the two before its
front end. For 88.2 or 96 kHz, averaging pairs of frames and halving every
frame count (hops and `heard`) is enough: the tracker only looks below
2.75 kHz. Other rates (22.05, 32 kHz): resample, or send no hops for that
epoch (the crab idles).

### The BPM prior

The third field of `@e`: a tempo the sender knows from metadata (mStream's
`tracks.bpm`, tagged or from its Essentia pass), or 0. The Core2 passes it
to `BeatTracker::setPrior()`, as the console's `t<bpm>` does: the tempo
search is weighted strongly towards it and its octave (a log-Gaussian a
quarter of an octave wide), which picks the octave (174 or 87) but can't
invent beats the audio doesn't have. It doesn't make the lock sooner (the
tracker waits 1.5 s of audio and two PLL beats regardless). A whole-number
BPM is fine. The prior belongs to the epoch: a new epoch without one
clears it.

### The heard clock

**What the sender sends.** `heard` is the epoch frame whose sound reaches
the listener at the moment the `@c` line is written to the port:

```
heard = (frames played into the output so far in this epoch)
        - (output latency + user offset) × rate
```

- The output latency is the time from the audio API taking a frame to the
  speaker playing it: tens of ms on wired output, 150-300 ms on Bluetooth
  headphones on the computer. rodio doesn't report it, so the terminal
  player needs a user setting (an offset in ms). So does the Python sender:
  its `--play` (Windows' `winsound`) reports no timing at all, so it anchors
  the clock when the call that starts the sound returns and takes the whole
  output delay from `--offset-ms`.
  A positive offset makes the crab later, as the console's `y<ms>` does.
- Right after a seek or a track change, `heard` is negative until the new
  epoch's audio reaches the ear. Send it as it is.
- Send it from a smooth clock: anchor the play position when playback
  starts (or resumes, or the epoch starts) and count from there at the
  rate; don't send each raw `sink.get_pos()` if it moves in steps. Correct
  the anchor slowly if the position drifts from it by more than 20 ms.
- `playing 0` while paused, stopped or starved; `heard` then stays where
  the sound stopped.

**What the Core2 does with it** (`HostClock`, lib/core). Each `@c` of the
current epoch is stamped on arrival with `esp_timer` microseconds. The
delay from the sender's write to the stamp is always positive and varies:
USB and the UART (2-5 ms), plus up to a loop pass (5-40 ms) before the
console reads it. So the clock follows the **least delayed** samples:

1. **Window.** The samples of the last 2 s (at most 32).
2. **Leading edge.** `E(t) = max over the window of heard_i + rate · (t - t_i)`:
   each sample says "at least this far by now", and the least delayed one
   says the most. `E` trails the true heard frame by the smallest delay in
   the window (a few ms, constant enough to fold into the offset).
3. **The clock** `C(t) = A + rate · (1 + s) · (t - tA)`. At each sample,
   with `e = E(t) - C(t)`:
   - no clock yet, or `|e|` over **100 ms**: **snap** (`A = E`, `s = 0`;
     counted). That happens at the first sample of an epoch, on `playing`
     going back to 1, and after a real jump (which a correct sender
     doesn't make);
   - otherwise **slew**: `A = C(t)`, `s = e / (rate · 0.5 s)` clamped to
     **±5 %**. The error shrinks by a fifth per 100 ms; the crab's tempo
     changes by at most 5 % while it does, which nobody sees, and its
     phase never jumps.
   - `s` applies for at most 200 ms after the sample; with no new sample
     the clock runs on at the plain rate.
4. **Drift.** The computer's audio clock and the Core2's crystal differ by
   up to about 100 ppm (0.2 ms over the window). Old samples leave the
   window, so `E` follows the drift and the slew takes it up.
5. **Precision.** Frames are kept as a double (a float alone loses
   sub-frame precision after 2^24 frames, 6 minutes; a double keeps it past
   2^31, 12 hours at 48 kHz), and times are `uint32_t` microseconds
   compared by signed difference (it wraps after 71 minutes).
6. **Valid** only while `playing` is 1 and the newest sample is under
   **1.5 s** old. `playing 0` makes it invalid at once and empties the
   window; the next `playing 1` snaps. Invalid means the dancer fades to
   its idle sway, as it does today when the Core2's own audio pauses.

`DanceMode::render` asks for `C(now + lead - offset)`, where `lead` is the
same as today (the frame's drawing time, half its push, and 15 ms early)
and `offset` is the console's `y<ms>` (0 unless set before the session).
The beat position is `grid.beatsAt(frame, frac)`, exactly as with
`TapReader::audibleAt`; the grid and the clock are in the same epoch
frames, so a negative `heard` works (`beatsAt` takes signed differences).

## Errors

`@err <code> <verb> [<detail>]`. At most 4 per second; more are only
counted (a sender stuck in a loop can't flood the link back). An error
never changes state.

| Code | Name | When | The sender |
|---|---|---|---|
| 1 | syntax | wrong field count or format, a byte outside 0x20-0x7E, a line that is only `@` | a bug: log it, don't retry the line |
| 2 | version | `@hello`'s protocol is below the Core2's lowest. Detail: `<min>-<max>` | speaks a version in range, or tells the user to update one side |
| 3 | nosession | `@e`, `@h`, `@c`, `@log` with no session (the Core2 rebooted, or timed out) | sends `@hello` (a new session) |
| 4 | busy | `@hello` can't start host mode now. Detail: `ui` (the start-up screen; the UI starts ~3 s after boot), `screen` (the touch calibration or a spike screen is up), `pairing` (a Bluetooth pairing is under way), `dance` (no PSRAM for the dancer: permanent) | retries `@hello` every 1 s; gives up on `dance`, or after 20 s, and says why |
| 5 | range | a field out of range: rate not 44100/48000, prior not 0 or 30-300, an energy negative, over 1e4 or not finite, `@log` over 2, a rate change within an epoch | a bug |
| 6 | long | over 255 bytes (the verb is `-`: it may be cut off) | a bug |
| 7 | verb | a verb this firmware doesn't know, or `@hello` with no feature it knows | stops sending that verb (it should have checked `caps`) |
| 8 | declined | the user ended the session on the Core2 and lines keep coming | stops; waits for its own user |
| 9 | noepoch | `@h` or `@c` in a session before any `@e` | sends `@e` for the current epoch |

## Host mode on the Core2

**Enter**, on `@hello ... viz` (never on USB power alone: the AXP192's
VBUS bits look the same for a phone charger as for a computer):

1. Refuse with `@err 4` if it can't: the UI hasn't started, another screen
   owns the display, a Bluetooth pairing is under way, or the dancer has no
   PSRAM.
2. **Pause the player**: Playing becomes Paused, a play waiting for the
   headphones is cancelled (Paused). Stopped and Paused stay as they are.
   A test track the console's `Rt`/`Rf` started on its own is stopped.
   Nothing ever resumes by itself afterwards.
3. **Quiet the headphones' background search**: `setQuiet(true)` for the
   session (one more term in the loop's existing `setQuiet` expression).
   The Bluetooth controller stays up and linked headphones stay linked.
   Quiet doesn't cut a burst short: the boot's three pages to the
   remembered headphones (about 30 s) finish, and then the search rests
   instead of backing off (`[bt] reconnect: resting (nobody around (the
   screen off and nothing playing, or the computer's visualizer))`).
4. **Wake the screen** if it was off, close a toast, show the **Dance
   tab**, and keep the screen lit for the session (the loop's `keepLit`).
   The idle power-off counts the session as busy (it never acts on USB
   power anyway).
5. **Unfreeze the dancer** (a feel test with `k<n>` may have left it
   frozen), stop the output taps (the tracker is fed from the computer
   now), and wait for `@e`.
6. Answer `@ok` and log one line:
   `[viz] on: dancing to the computer (protocol 1, session 7f3a); the player paused; the headphones' search quiet`.

**While on:**

- The Dance tab's left panel shows the tracker as always (BPM, locked or
  listening, confidence). The bottom line, which shows the Core2's track,
  shows `Dancing to your computer` (Bold) over `Tap a button or a tab to
  stop` (Small) instead, with no progress line.
- A tap on the dancer still switches it (crab, stick figure).
- The 5 s `[dance]` line carries the session instead of the output
  latency: `| viz epoch=2 44100Hz prior=0 hops=4310 gaps=0 dup=0 stale=0
  bad=0 errs=0 | clock ok age=43ms snaps=1 slew=+0.4% spread=6.0ms
  offset=+0ms |`. `spread` is how far the window's samples sit behind its
  leading edge (p95): the delivery jitter. Nothing is logged per line. A
  tracker reset is logged much as today (`[dance] tracker reset (computer:
  epoch 2, its first hop) at hop 0, 44100 Hz`): an epoch's first hop
  always, a gap's at most once a second; `locked` and `lost the beat` as
  today, `locked` with the epoch: `[dance] locked: 120.00 BPM, 2.60 s after
  the reset (computer: epoch 2)`.

**Exit**, on the first of:

| Trigger | Why logged | Sent |
|---|---|---|
| `@bye` | the computer said bye | `@bye ok` |
| no valid line for 3 s | nothing from the computer for 3 s | `@bye timeout` (likely nobody listening) |
| USB unplugged (the screen code's once-a-second VBUS read) | USB unplugged | nothing (no link) |
| a touch outside the dancer's box, a button (A, B, C) or the PWR key | a touch / a button | `@bye user`, then declines (see sessions) |
| the headphones' play key | the headphones' play key | `@bye user`; the key then acts as a headphone play does after the sleep timer's pause: what host mode paused stays paused (in-ear detection sends play as a bud goes back in; the Core2's play button resumes it), and a pause the listener made before the session resumes |
| the Dance tab went away some other way (defensive) | the Dance tab closed | `@bye dance` |

The touch or press that ends host mode does nothing else: the rest of that
gesture is dropped, as a wake touch is. On exit the player **stays
paused**, the tab stays on Dance (the crab goes back to the Core2's own,
paused, audio and idles), the output taps and the tracker's 44.1 kHz come
back, `@log` goes back to off, and one line is logged:
`[viz] off (a touch): 61.2 s, 2 epochs, 5263 hops, 0 gaps, 0 bad lines; the player stays paused`.

### A reboot on open

If opening the port resets the board (Linux always may; Windows shouldn't
with DTR and RTS low), the sender sees the ESP32 ROM's lines (`ets ...`,
`rst:0x1 (POWERON_RESET)...`) and then the banner `mstream-mp3-player
<version> (commit ...)`. The sender doesn't need to recognise any of that:

1. It listens for 300 ms after opening the port (a reset on open shows
   its ROM lines within that), then sends `@hello` every 1 s until an
   `@ok` with its session id comes back. Before the UI starts (~3-4 s
   after the banner) the answer is `@err 4 hello ui`.
2. **Nothing is written from a ROM line (`ets `, `rst:`) to the banner**,
   in any state: a line the Core2's Serial starts to read halfway would be
   a tail (the firmware drops those, rule 4 above, but a sender shouldn't
   make them). The banner prints after `SerialConsole::begin()`, so a line
   sent after it is read whole. No banner within 5 s of a ROM line (not
   this firmware, or a boot loop): it asks anyway.
3. In a session, `@err 3` (no session) means the Core2 rebooted (or timed
   out): back to step 1 with a **new session id**, then `@e` for the
   current epoch with its current hop numbering (the first hop after it
   restarts the tracker; the heard clock snaps).
4. Seeing the banner or a ROM line in a session is the same signal, just
   sooner.

A reboot loses the Core2's silent test mode (`z`) and anything typed at
its console; it comes back with the queue stopped, as after any boot, so
nothing plays.

## Sharing the framing with pairing and Wi-Fi

The terminal player's plan (PLAN.md Phase 13, branch `claude/device-flash`)
lists "Wi-Fi and pairing over the same port" as left to do. That work and
this one share the port, so they share this framing and one port owner in
the terminal player (its `src/device/`). What is fixed now:

- **One framing:** `@` lines as above, both ways, the same limits (255
  bytes, printable ASCII, base64url strings), the same `@hello`/`@ok`/
  `@err`/`@bye` session, the same rule that the sender never sends a bare
  byte.
- **Features pick the side effects.** `@hello <proto> <session> <features>`:
  `viz` starts host mode (pause, Dance tab, lit screen); a later `setup`
  starts a session that changes nothing on screen or in playback by
  itself. One sender can ask for both (`viz,setup`). A session's features
  are fixed at `@hello`; to change them, send a new `@hello` (a new session
  id; host mode stays up if `viz` is still asked for).
- **Reserved verbs.** One-letter verbs are the visualizer's high-rate
  stream (`e h c s t`), plus `log`. Reserved for Phase 13 and answered
  `@err 7` until then: the namespaces `wifi.*` (`wifi.set <ssid> <pass>`,
  `wifi.scan`, `wifi.status`), `pair.*` (`pair.set <url> <key>`,
  `pair.code`, `pair.status`, `pair.clear`: the mStream binding, which
  `--server` feeds), `sync.*`, and the verbs `info`, `ping`, `pong`.
- **Requests that need an answer** carry a request id (u32) as their first
  field, and the Core2 answers `@re <id> <code> [fields]` (`0` for done),
  so a slow Wi-Fi join can't be mistaken for the answer to something else.
  The visualizer's lines need no answers and carry no ids.
- **Secrets** (a passphrase, an API key) are never echoed, logged or put in
  an `@err`; the console already never echoes host lines, and `@err` names
  only the verb.
- **Long values:** a server URL as base64url can pass 255 bytes with a key
  beside it. Phase 13 sends the URL and the key in separate requests
  (`pair.url`, then `pair.key`) rather than raising the limit, which is a
  fixed buffer in internal RAM.

## Bandwidth and the receive buffer

| Line | Typical bytes | Rate | Bytes/s |
|---|---|---|---|
| `@h 12 4310 0.0291152764 0.00515992753` | 38 | 86.13 (93.75 at 48 kHz) | 3270 (3560) |
| `@c 12 1904512 1` | 16 | 10 | 160 |
| `@e 12 44100 0` | 14 | per track | - |
| **Total** | | | **about 3.5-3.7 KB/s**, a third of 115200 baud (11.5 KB/s) |

Arduino-ESP32 (core 3.3) gives `Serial` a 256-byte receive buffer by
default. At 3.7 KB/s that is 70 ms; a loop pass that draws the Dance tab's
panels and prints the 5 s `[dance]` line (about 250 bytes, which blocks the
loop ~10 ms: `Serial` has no transmit buffer, so a print waits until all
but the last 128 bytes are on the wire) can come close. The plan raises it to **1024 bytes**
(`Serial.setRxBufferSize(1024)` before `M5.begin(cfg)`; it can't change
after `begin()`): 270 ms of the stream, for 768 more bytes of internal RAM
(the UART driver's ring buffer is internal). On the device 1 KB lost
nothing: 0 gaps in over 6 minutes of sessions, a 21 s screenshot dump
included. If gaps show up without injected faults, 2048 is the fallback. There is no cheap overflow
counter (`onReceiveError` would start the UART's event task, a few KB of
stack), so lost lines show up as hop gaps and bad lines, which are counted.

## A session, line by line

```
→ @hello 1 7f3a viz
← @err 4 hello ui                       (just booted: the UI starts in ~3 s)
→ @hello 1 7f3a viz
← @ok 1 7f3a v0.5.0-beta.1 viz,log
→ @e 1 44100 120
→ @c 1 -6615 1                          (150 ms of output latency: frame 0 not heard yet)
→ @h 1 0 0.672406 1.09918
→ @h 1 1 0.0291153 0.00515993
→ @h 1 2 0.00766506 5.37276e-05
→ @h 1 3 0.0036862 2.56443e-05
→ @c 1 -2205 1
   ...                                  (86 @h and 10 @c a second)
→ @c 1 1323000 0                        (paused at 30 s; @c goes on at 10 Hz)
   ...
→ @c 1 1323000 1                        (resumed: same epoch, the hops carry on)
→ @e 2 48000 0                          (the next track, at 48 kHz)
→ @h 2 0 ...
   ...
→ @bye
← @bye ok
```

## Implementation

As built (October 2026), with where it differs from the plan it started
as. The order of work was the plan's: the front end and the tracker's two
methods, the console's router, the session and the clock, the src wiring;
then the sender, then the device tests (one fix: `HostLink`'s signed
time differences).

### lib/core (portable, host-tested)

**`HopFrontEnd`** (new, `HopFrontEnd.h/.cpp`): step 1 moved out of
`BeatTracker`, with no change in behaviour.

```cpp
// The beat tracker's onset front end (BeatTracker step 1; docs/USB-VISUALIZER.md
// "The front end"): mono in, the low and mid band energy of each hop out.
class HopFrontEnd {
public:
  void begin(uint32_t sampleRate, uint32_t hop, uint32_t decimation, float lowpassHz);
  void reset();  // the filters and the hop in progress
  // Calls onHop(low, mid) for each hop completed.
  template <typename F> void process(const int16_t* mono, uint32_t frames, F&& onHop);
};
```

- `process()` is a header template, so `BeatTracker::process()` compiles to
  the same loop as before. The `Biquad` struct and the decimation, DC and
  hop state moved into it. Checked bit for bit against the tracker before
  the move (HEAD's `BeatTracker.cpp` and the new one built side by side on
  the host, `-O2`: BPM, confidence, lock, grid and frames fed identical at
  41,766 checkpoints over click tracks at both rates and three block
  sizes). The boot bench (13.3 ms per second of audio) is still to be
  read on the device.
- `BeatTracker` keeps a `HopFrontEnd fe_` and calls it from `process()`;
  `reset()` resets it.

**`BeatTracker`**, two public additions:

```cpp
// Hop energies computed elsewhere (a HopFrontEnd on the computer), instead
// of process(): the next hop since reset(), standing for frame origin +
// hops * hop. Don't mix with process() between resets.
void feedHop(float low, float mid);
// Rebuilds the tables for another rate (the computer's 48 kHz) through the
// begin() allocator; keeps the prior. False: out of memory (it can't track
// until a later call succeeds). Nothing to do at the rate it has.
bool setSampleRate(uint32_t rate);
```

`feedHop()` calls the private `onHop()` and adds `hop` to `fed_`, so
`framesSinceReset()` and `framesToLock()` mean the same on both paths.
`setSampleRate()` keeps the `alloc` hook (a new member) and runs
`freeBuffers()` + `begin()`, then `reset(0)`; at the rate it has it does
nothing at all.

**`HostLine`** (new): the byte router for the console.

```cpp
// The computer's lines on the serial console (docs/USB-VISUALIZER.md
// "Framing"): '@' to '\n' or '\r', at most kMaxLine bytes, never a console key.
class HostLine {
public:
  static constexpr size_t kMaxLine = 255;
  enum class Byte : uint8_t { Console, Start, Restart, Taken, Line, Bad, Long };
  // Every byte the console reads. `atIsText`: a console argument is being
  // typed that may hold '@' (an R argument with text).
  Byte push(char c, bool atIsText);
  char* text();  // after Line: the line from its '@', 0-terminated (writable)
};
// Splits a line in place into its verb and up to 8 fields (more: `more`,
// ignored); false: a syntax error.
bool splitHostLine(char* line, HostFields* out);
bool parseHostU32(const char*, uint32_t*);  // and parseHostI32, parseHostF32 (the grammar above)
```

`Start` tells the console to abandon a waiting command; `Restart` is an
`@` inside a line (the partial line dropped, a new one started), counted
as bad with no reply. The buffer is 256 bytes in the console object
(internal RAM, `.bss`).

**`HostLink`** (new): the session state machine, clock-free (times are
passed in), in the style of `IdlePolicy` (an `Out` struct per call).

```cpp
class HostLink {
public:
  enum class Event : uint8_t { None, Enter, Restart, Exit, Epoch, Prior, Hop, Clock, Log };
  enum class Why : uint8_t { Bye, Timeout, Unplugged, Touch, Button, HeadsetKey, DanceGone };
  enum class Busy : uint8_t { None, Ui, Screen, Pairing, Dance };  // why @hello is refused (the first that applies)
  struct Out {
    Event event = Event::None;
    Why why = Why::Bye;              // Exit
    uint32_t epoch = 0, rate = 0;    // Epoch, Prior, Hop, Clock
    float prior = 0.0f;              // Epoch, Prior
    uint32_t hop = 0;                // Hop
    bool restart = false;            // Hop: reset the tracker at this hop first (first, or a gap)
    bool gap = false;                // Hop: ... because of a gap
    float low = 0.0f, mid = 0.0f;    // Hop
    int32_t heard = 0;               // Clock
    bool playing = false;            // Clock
    uint8_t level = 0;               // Log
    char reply[96] = "";             // a line to send back ('@...', no '\n'), or ""
  };
  void begin(const char* fw);
  Out line(char* text, uint32_t nowMs, Busy busy);  // a complete host line
  Out bad(HostLine::Byte kind, uint32_t nowMs);     // Bad, Long, or Restart (no reply)
  Out poll(uint32_t nowMs, bool usbPower);          // the 3 s timeout, unplugging, a decline's end
  Out end(Why why, uint32_t nowMs);                 // the Core2's side: Touch, Button, HeadsetKey, DanceGone, Unplugged
  bool active() const;
  bool declined() const;
  const HostStats& stats() const;  // lines, epochs, hops, gaps, dup, stale, clocks, bad, errors, suppressed
};
```

It owns: the session id, the protocol chosen, the epoch, its rate and
prior, the expected hop, the 3 s timeout, the declined state after a user
exit, the log level and the `@err` rate limit. `@ok` takes the firmware
version from `begin()` (spaces become `_`) and the caps `viz,log`. The
order a data line is checked in: syntax (`@err 1`), the verb (`@err 7`),
the decline (`@err 8`), the session (`@err 3`), the fields (`@err 1`), the
ranges (`@err 5`), the epoch (`@err 9`), then stale lines are dropped.
`@hello`: its fields (`@err 1`), the decline, the version (`@err 2`), the
features (`@err 7`), then, outside a session, busy (`@err 4`).
`Prior` is the plan's "same epoch, another prior" made its own event.

**`HostClock`** (new): the heard clock above.

```cpp
class HostClock {
public:
  void start(uint32_t rate);  // a new epoch or session: forgets everything
  void sample(uint32_t nowUs, int32_t heard, bool playing);
  struct Heard { bool valid = false; int32_t frame = 0; float frac = 0.0f; };
  // The frame heard `aheadUs` after `nowUs`, if the clock is valid at `nowUs`
  // (so the LCD's lead never makes it stale early).
  Heard at(uint32_t nowUs, int32_t aheadUs = 0) const;
  // For the [dance] line: snaps, the slew now, the window's spread (p95, ms), the newest sample's age.
  Stats stats(uint32_t nowUs) const;
};
```

About 300 bytes (32 samples), inside `DanceMode`.

**`PlaybackController::pauseByComputer()`** (new): Playing pauses, Waiting
cancels the wait, and either pause is marked the computer's
(`pausedByComputer()`, cleared by any play, as `pausedByTimer()` is);
anything else does nothing, mark included. It never starts playback,
which `togglePlayPause()` would from Paused or Stopped. `HeadsetKeys` takes
`pausedNotByListener()` (the timer's or the computer's): the headphones'
play doesn't resume such a pause, and a cue after it isn't idle input.

**`UiText`**: `kVizTitle = "Dancing to your computer"` (Bold) and
`kVizHint = "Tap a button or a tab to stop"` (Small), each in the bottom
line's room (`kW - 16` = 304 px), measured by `test_ui_library`.

### src

- **`SerialConsole`**: a `HostLine` member; each byte goes through `push()`
  first, and only `Byte::Console` bytes reach the existing switch. `Start`
  abandons a waiting command (`> i: abandoned (a computer's line began)`).
  `Line`, `Bad`, `Long` and `Restart` go to a new action, `hostLine(char*
  line, HostLine::Byte kind)` (`line` only for `Line`). `poll()` still
  returns true for host bytes (they count as someone at the console). The
  header comment and the boot `[console]` line have the `@` entry. Sync
  (rule 4 of the framing): `push()` takes the read's time; `Held` and
  `Dropped` bytes go nowhere; after the reads, `quiet()` may end Sync and
  hand over a held key; a poll that finds the buffer within 128 B of full
  calls `resync()` (and abandons a waiting command); `begin()`, called
  from `setup()` right after `M5.begin()`, waits up to 20 ms for quiet.
  `kRxBuffer` (1024) is the size `setup()` gives Serial.
- **`app/UsbViz`** (new, `src/app/UsbViz.h/.cpp`): the glue. Owns the
  `HostLink`; `onLine()` (from the console) stamps `esp_timer_get_time()`
  and carries out each `Out`: `Enter` (`danceMode.setHost(true)`, then
  main.cpp's `enter` hook) and `Exit` (`setHost(false)`), `Restart` →
  `hostForget()`, `Epoch` → `hostEpoch()`, `Prior` → `hostPrior()`, `Hop`
  → `hostHop()`, `Clock` → `hostClock()`, `Log` → `setHostLog()`, and
  writes `reply` with one `Serial.printf`. `loop(now, usbPower)` runs
  `poll()` and the defensive Dance-tab check. `userEnded(why)` is called by
  `main.cpp`'s input handling. `active()` for the loop's expressions. Three
  hooks from `main.cpp`: `busy` (why an `@hello` can't start now), `enter`
  (pause, stop a test track, wake, show the Dance tab) and `danceGone`
  (the dancer stopped other than by the screen going dark: the dancer
  stops while the panel sleeps and comes back with the wake, which happens
  later in the same loop pass than the check). A refused `@hello` is logged
  once per reason (`[viz] a computer asked to drive the dancer: not now
  (ui)`).
- **`DanceMode`**:
  - `setHost(bool)`: on, the taps go off (`syncTaps()` adds `&& !host_`),
    the freeze is cleared, the click-track truth is off, the tracker waits
    for an epoch; off, the tracker goes back to the output's rate
    (`setSampleRate(audio_.sampleRate())`), `follow()` starts the tap path
    afresh, the log level goes back.
  - `hostEpoch(epoch, rate, prior)`: `setSampleRate`, `setPrior`, the
    clock's `start()`, fresh until the first hop.
  - `hostHop(hop, low, mid, restart, gap)`: on `restart`, `tracker_.reset(hop *
    512)` and `fold_.reset()`, logged as `[dance] tracker reset (computer:
    epoch 2, its first hop) at hop 0, 44100 Hz`: an epoch's first hop
    always (`--measure` learns the epoch from it), `a gap in its hops` at
    most once a second (`; 3 more not logged` when it held some back);
    then `feedHop()`, timed into `trackerUs_`; the `locked` (with
    `(computer: epoch N)`) / `lost the beat` lines and the `[beat]` log as
    in `feed()`.
  - `hostClock(nowUs, heard, playing)` → `clock_.sample()`.
  - `render()`: in host mode, `clock_.at(nowUs + lead - offset)` in place
    of `reader_.audibleAt()`; `beat = heard.valid && g.valid && !fresh_`.
    The rest is unchanged.
  - `loop()`: no `reader_.poll()` in host mode.
  - `printStats()`: the host fields in place of `latency=...`; rates from
    the epoch's rate, not `audio_.sampleRate()`.
  - `[beat]` lines in host mode add the epoch, the next beat's frame in it
    (to a tenth: the grid is sub-frame) and the hop it was predicted at:
    `[beat] #12 next at 6.000s (epoch 2, frame 264600.4, hop 509) bpm=120.00 conf=0.83 locked`.
    `@log 2` adds a `[flash] #<beat> heard=<frame> aim=<frame>` line from
    `render()` when the drawn beat number changes: `heard` is the clock's
    frame now, `aim` the frame the dancer was drawn for (now + the lead).
- **`Ui` / `DancePage`**: `Ui::showDance()` (closes a sheet or dialog, a
  toast (its Undo dropped), the volume HUD and the first-boot tips (shown
  again at the next boot), then `showTab(Dance)` rather than toggling;
  false when the UI isn't started or is suspended). `DancePage` draws the
  viz texts in the bottom room while `dance().host()`, with no progress
  line, and redraws that room when it changes (at most half a second
  later: its panel pace).
- **`main.cpp`**:
  - `Serial.setRxBufferSize(1024)` before `M5.begin(cfg)`.
  - The console's `hostLine` action → `usbViz.onLine()`.
  - `usbViz.loop(now)` every pass, after `console.poll()`.
  - `setQuiet(... || usbViz.active())`; `keepLit = ... || usbViz.active()`;
    `stepIdle`'s `in.busy = screenTaken() || usbViz.active()`.
  - `handleInput()`: while `usbViz.active()`, the first event of a touch
    (`Down`, or a strip swipe's `DragStart`) outside `DanceView::inBox()`,
    or any button event, calls `usbViz.userEnded()` with a haptic tick and
    is swallowed with the rest of its gesture (the touch until the next one
    lands, a held button until its `HoldEnd`); a tap inside the box still
    reaches the page. The PWR key's short press counts as a button (it only
    wakes the screen or counts as input otherwise).
  - `handleBluetooth()`: the headphones' play key ends the session first,
    then acts as a headphone play: it resumes nothing host mode paused
    (`pausedNotByListener()`).
  - Entering: `player.pauseByComputer()`, `audio.stop()` if the player is Stopped
    and the backend plays (an `Rt`/`Rf` track), `screen.wake("the
    computer's visualizer")`, `userInterface->showDance()`,
    `danceMode.setHost(true)`.

Internal RAM: the RX buffer +768 B, and from the ELF: `usbViz` 208 B,
`console` +280 B (the line buffer and its Sync state: 848 B now),
`danceMode` +~370 B (`HostClock` and the host fields): about 1.6 KB in
all. Still to check on the device with
the boot's `diag::logHeap("dance")` and the `[dance]` line's `ram=`. No
`IRAM_ATTR`; `pio run -e core2` passes `iram_diet`, `flash_guard` (the app
2.16 MB, 36 % of its slot) and the version guard.

### The reference sender: `tools/usb_viz.py`

Python 3 with pyserial, nothing else (`winsound`, in the standard library on
Windows, only for `--play`; ffmpeg only for files that aren't WAV). Tests:
`tools/test_usb_viz.py`, `python -m unittest discover -s tools -p
"test_usb_viz.py"` (52 tests, about 5 s; no port, no sound).

```
python tools/usb_viz.py --port COM3 --click 120 [--seconds 60]   # silent: nothing is heard
python tools/usb_viz.py --port COM3 --wav a.wav --wav b.wav       # silent; an epoch per file
python tools/usb_viz.py --port COM3 --file song.flac --play       # plays on this computer too
python tools/usb_viz.py --port COM3 --click 120off --measure --expect "lock<=4,bpm<=0.5,med<=10,p95<=25"
python tools/usb_viz.py --dry-run --fast --click 120 --seconds 2  # the lines on stdout, a fake Core2 answering
python tools/usb_viz.py --selftest                                 # offline: no port
python tools/usb_viz.py --score run.jsonl --expect "..."           # score an earlier run's --log
```

`--port auto` takes the one port with a Core2's USB bridge (CH9102 or
CP2104) and refuses if there are none or several. Exit status: 0 done (or
Ctrl-C), 1 an error or a failed `--expect`, 2 the command line, 3 stopped on
the Core2 (`@bye user`, `@err 8`, `@bye dance`).

Parts:

- **`ClickGen`**: a port of `lib/core/ClickGen` (`--click 120`, `120off`,
  `click120off`), every operation rounded to float32 as the C++ does, so
  its samples match the golden file's FNV-1a exactly; the beat frames
  (`llround` is round-half-away, not Python's `round()`) are the truth.
- **`HopFrontEnd`**: the front end above, every operation rounded to
  float32 in the C++ order (Python's `struct`): bit-exact on all 719 hops of
  the golden file at both rates. About 4 % of a core at 44.1 kHz. Mono is
  the firmware's mix, `(left + right) >> 1`.
- **Sources** (an epoch each, clicks first, then files, in order):
  `--click` at `--rate 44100|48000` (`--seconds`, default 60); `--wav` (or
  `--file`): a 16- or 24-bit PCM WAV at 44.1 or 48 kHz through the `wave`
  module, anything else (MP3, FLAC, other rates) decoded by ffmpeg to 16-bit
  stereo at 48 kHz if the source rate is a multiple of 48 kHz, else 44.1.
  ffmpeg comes from PATH, an importable `imageio-ffmpeg`, or the repo's
  `.venv-tools` (as `tools/make_test_audio.py` sets it up); with none the
  sender says so and names what works without it. `--beats FILE.json` (a
  list of beat times in seconds, or `{"beats": [...]}`) is the truth for the
  `--wav` at the same position. `--prior` goes in every `@e`.
- **The port** (`SerialPort`): `dtr` and `rts` set False on an unopened
  `serial.Serial()`, then `open()`, and never touched again (a test checks
  the order). A reader thread splits lines (a trailing `\r` stripped),
  stamps their arrival and hands them over; the Core2's log lines are shown
  as `core2| ...` (`--quiet` hides them). It reads what is waiting, or one
  byte: pyserial's `read(n)` with a timeout returns only when n bytes came
  or the timeout ran out (Windows: `ReadTotalTimeoutConstant`, no interval
  timeout), so the first version's `read(4096)` ended every read on its
  50 ms timeout and stamped lines up to 50 ms late, which `--flash` scores
  as a median ~25 ms behind and a ~45 ms spread. A line is stamped by when
  its first byte came (the read's return less its bytes' time on the wire,
  10 bits each at 115200 baud): when the Core2 printed it, or when the line
  before it was out. A test fakes Windows' batching in virtual time and
  checks each stamp to 0.5 ms (it fails by 46 ms with the old read).
- **The guard** (`encode_line()`): every byte written goes through it. It
  refuses (raises) anything that doesn't start with `@`, holds a byte
  outside 0x20-0x7E, or passes 255 bytes, and adds the `\n` itself, so the
  sender can't send a console key or a bare newline.
- **The session** (`Sender`): 300 ms of listening after the port opens,
  then `@hello 1 <8 hex digits> viz` every 1 s until
  an `@ok` with its session id (20 s, then it gives up and says why; `@err
  4 ... ui` is said once and retried; `@err 4 ... dance`, `@err 2` and `@err
  7 hello` give up at once). Nothing at all is written from a ROM line
  (`ets `, `rst:`) until the banner, in any state (5 s without one: it
  asks anyway). After `@ok`: `@log` with `--measure`, then `@e`.
  A new session (new id) on `@err 3`, `@bye timeout`, or a ROM line or the
  banner (`ets `, `rst:`, `mstream-mp3-player `) while streaming; then `@e`
  for the same epoch, and the hops of audio played while there was no
  session are skipped (the first hop sent restarts the tracker), not sent
  in a burst. `@err 9` sends `@e` again. `@bye user`, `@err 8` or `@bye
  dance`: it stops sending at once (no `@bye`) and exits 3 with a message.
  Other `@err`s are counted and shown as a bug in the sender. `@bye` on the
  way out (the end, an error, Ctrl-C), waiting up to 1 s for `@bye ok`.
- **The virtual player** (`Player`): the play position is a clock anchored
  when an epoch starts or playback resumes, counting at the rate.
  `heard = position - offset` (`--offset-ms`, the output's delay; positive
  makes the crab later). The hops are released in bursts of `--batch`
  frames (2048, as the terminal player's tap) once they are within
  `--lead-ms` (150) of the position; `@c` at `--clock-hz` (10), with
  `playing 0` and `heard` held while paused, and a last `@c ... 0` at the
  end. The next source starts the moment the last ends (a new epoch).
- **`--play`** (Windows): writes each source to a temporary WAV at
  `--gain-db` (default -12 dB), prints a warning to turn the volume down,
  and plays it with `winsound.PlaySound(..., SND_ASYNC)`; the clock is
  anchored when that call returns, and the sound is stopped on the way out.
  `winsound` can't pause or seek, so `--pause-at` and `--seek-at` are
  refused with `--play`.
- **Faults** (each tests one rule; times are seconds since playback
  started): `--pause-at S:LEN` (the same epoch: `@c ... 0`, then the hops
  carry on), `--seek-at S[:TO]` (a new epoch from TO, default 10 s on),
  `--gap-at S` (0.5 s of audio lost before the front end: its hop numbers
  skipped, the filters reset), `--drop P` (leave out that share of the
  `@h` lines, as a lost line would), `--jitter-ms N` (hold each data line
  back 0-N ms, order kept; `--seed`).
- **`--dry-run`**: the lines go to stdout (only the lines: the sender's own
  messages and the `--measure` table go to stderr), and `FakeCore2`, a
  small stand-in for `HostLink`, answers (`@ok`, `@bye ok`, `@err 3`/`9`)
  and checks each line's grammar and the hop numbering. `--fast` runs it in
  virtual time. The tests script the same fake (lost and busy hellos, a
  reboot mid-stream, a user exit, unprompted `@err`s).
- **`--log FILE`**: the run as JSON lines: every line sent (`tx`) and
  received (`rx`, with the sender's epoch and heard frame at its arrival),
  and each epoch's truth (`epoch`: rate, source, start frame, BPM, period,
  the beat frames in epoch frames). `--score FILE` reads it back.
- **`--measure`**: sends `@log 1` (`--flash`: `2`) and scores per epoch:
  the first `[dance] locked: ... s after the reset` (the lock time); every
  `[beat] ... locked` line's next-beat frame against the nearest true beat,
  folded into half a true period as `errorAgainst()` does (median, p95 and
  mean of the error); the BPM (median of those lines) against the truth,
  octave-folded; with `--flash`, each `[flash]` line's heard frame against
  the sender's own at its arrival (median, p5-p95 spread); the resets; and
  the last `[dance]` line's counters (gaps, bad lines). The epoch is the one
  the line names (`[beat] ... (epoch N, ...)`, `[dance] locked: ...
  (computer: epoch N)`), else the one in the Core2's last `tracker reset
  (computer: epoch N, ...)` line, else this computer's at the line's
  arrival. (The first firmware's lines named none and rate-limited every
  reset line to one a second, so a seek within a second of a gap credited
  the new epoch's lock and beats to the old one; a test keeps the case.) It prints a
  table; `--expect` (keys `lock`, `bpm`, `med`, `p95`, `spread`) turns it
  into the exit status, and an epoch with no lock fails `lock`.
- **`--selftest`**: the coefficients, the click's beat frames and samples,
  and every hop against `test/test_hop_feed/hop_golden.h` (the spec's
  tolerance, relative 1e-5 or 1e-12 absolute, and it reports how many are
  bit-exact: all of them); every energy through its text back to the same
  float32; the guard against 11 lines it must refuse.

Not built from the plan: `--epoch-every` (track changes at alternating
rates; two `--click`s at the two rates, or a `--seek-at`, do much of it),
`--burst`, `--fuzz` and `--reset-on-open` (which would pulse RTS on
purpose). `sounddevice` for `--play` became `winsound`, which gives no DAC
times: hence the `--offset-ms`.

### Host tests (`pio test -e native`, all in and passing)

- **`test_hop_feed`** (new):
  - the same click tracks through `process()` and through `HopFrontEnd` +
    `feedHop()`: identical (bit for bit) BPM, confidence, lock and grid
    after every hop;
  - the energies through `%.9g` and `strtof`: identical results; through
    `%.6g`: the same lock time ±1 hop and phase errors within 0.5 ms;
  - 1e-4 relative noise on every energy: lock and phase within the
    targets;
  - **48 kHz**: the click-train table of MASCOT-POC.md (90-174 BPM, the
    off-beat phase, noise at -30 dBFS) at 48 kHz, to the same targets (lock
    ≤ 4 s, median < 10 ms, p95 < 25 ms); `Signals.h` takes a rate;
  - `setSampleRate()`: the tables follow (a 48 kHz track tracked after a
    44.1 kHz one), the prior is kept, nothing is allocated at the same
    rate, everything is freed through the hook (the allocator test's
    counter);
  - writes `hop_golden.h` when `HOP_GOLDEN_OUT` is set; otherwise checks
    the checked-in file within 1e-5 (float results differ by a ulp between
    compilers' `exp` and `sin`, so no hash).
- **`test_host_line`** (new): the framing rules one by one: `@` starts a
  line; `\n` and `\r` end it; `\r\n`; 255 bytes pass and 256 are `Long`
  with the rest swallowed; control and high bytes make `Bad`; `@` mid-line
  restarts; an `@` with an empty `R` argument or another command's
  argument is `Start`, with text in an `R` argument it is `Console`. A fuzz
  of 10,000 random streams (lines and console bytes mixed, random
  splits): no byte between an `@` and its terminator ever comes out as
  `Console`. Sync: every suffix of the sender's lines (hex session ids,
  floats, `\r\n`) into a fresh reader, in one read and a byte at a time
  with `quiet()` between, gives no `Console` byte and no key, and every
  whole line after the cut is read; a lone key followed by quiet comes
  through, a key with its Enter in one read doesn't; a tail or line
  paused past 20 ms still runs to its terminator; `resync()` makes the
  line under way `Bad` and drops what follows the loss; a fuzz of 2,000
  cut streams in random reads with gaps between lines. The tokenizer and the number parsers: overflow, signs, `nan`,
  `inf`, `0x10`, `1e`, `.5`, empty.
- **`test_host_link`** (new): the session table above, line by line:
  `@ok`'s exact text; versions (`@err 2 hello 1-1`); each `busy` detail;
  features; another session id (`Restart`), the same id again; codes 3
  and 9; a new epoch, the same epoch with a prior, the same epoch at
  another rate; rates and priors out of range; the first hop, the next, a
  duplicate, a gap (`restart` at the new hop); stale epochs; `@log`; `@bye`
  → `Exit` + `@bye ok`; the 3 s timeout → `@bye timeout`; USB off; a user
  exit → `@bye user`, then `@err 8` until 3 s of quiet, then `@ok` again;
  the `@err` rate limit (4 per second); unknown verbs; extra fields
  ignored; lines stamped a few ms after the time `poll()` is given (as
  the loop does it), which neither time out nor end a decline (the
  device found that one).
- **`test_host_clock`** (new), arrivals simulated against a known heard
  frame: no delay: exact; a constant delay: behind by exactly that;
  2-32 ms delays (uniform) with a 100 ms outlier every 2 s: within 2.5 ms
  of (truth - the least delay in the window) at p95 after 2 s (the plan
  said 2 ms; measured 1.2-2.4 over six seeds), the spread against the
  truth under 8 ms, the slope never more than 5 % off the rate, no snap
  after the first; a snap at the start, on `playing` back to 1 and past
  100 ms, a slew (no snap) for a 50 ms step either way; 200 ppm of drift
  followed within 2 ms; `playing 0` invalid at once; 1.5 s without a
  sample invalid; the microsecond counter wrapping; negative frames;
  frames past 2^24 and near 2^31 keep their fraction.
- **`test_playback`**: `pauseByComputer()` from each state (never starts
  playback), its mark (set only by a pause it made, cleared by any play
  and by a stop; the timer's mark kept).
- **`test_headset_keys`**: after the computer's pause, headphone play
  (three times) resumes nothing and isn't input, a cue isn't input; the
  Core2's play resumes it, and then their own pause and play work.
- **`test_ui_library`**: `kVizTitle` and `kVizHint` fit.

### Docs (with the code)

- This file: the host results and the device results (below).
- `docs/ARCHITECTURE.md`: a "USB visualizer" section after "Dancing crab":
  the split, the protocol in a paragraph, host mode, links here.
- `README.md`: the console table gets a row: `@...` lines are the
  computer's (USB visualizer, this file); an `@` abandons a half-typed
  command; typing one by hand gets an `@err` and does nothing else.
- `docs/MASCOT-POC.md`: a pointer from "Not done".
- With the sender: `README.md` a "USB visualizer" paragraph after the
  dancing crab (what it does, three commands, that nothing plays unless
  `--play`), the `@...` row names the sender, and the layout's `tools/`
  list has it; `ARCHITECTURE.md` a "reference sender" bullet.

### Order of work

1. Done: `HopFrontEnd`, `feedHop()`, `setSampleRate()`, `test_hop_feed`
   (no change in behaviour; the boot bench still to read on the device).
2. Done: `HostLine` in the console. This alone makes any `@` line
   harmless.
3. Done: `HostLink`, `HostClock`, `PlaybackController::pauseByComputer()`, their tests.
4. Done: `UsbViz`, `DanceMode`, `Ui`/`DancePage`, `main.cpp`; the build's
   guards (`iram_diet`, `flash_guard`, version) pass.
5. Done: the golden file (`test_hop_feed` writes it) and `tools/usb_viz.py`
   with `tools/test_usb_viz.py`.
6. Done in part: the device tests ([Checked on the
   device](#checked-on-the-device-october-2026)); the rest of the plan
   below is still to run.

## Host results (October 2026)

All from `pio test -e native` (MinGW g++ 11.2 on Windows).

- **Hops give the same tracker as audio, bit for bit** (`test_hop_feed`):
  the click-train table (90, 120, 128, 140, 174 BPM, on the beat and 0.37
  of a beat late, white noise at -30 dBFS, 20 s) at 44.1 and 48 kHz,
  compared after every hop: BPM, confidence, the tempo estimate and its
  clarity, lock, lock time, frames fed and the whole grid identical. Also
  at 777, 128 and 2048 frames a call, with priors. Through text with
  `%.9g` (a float's round trip): identical too.
- **Through `%.6g`** (the protocol's least): the lock within one hop of the
  exact path's and every scored beat's phase error within 0.5 ms of it,
  over the whole table at both rates. Through `%.6g`, scored as the tap
  path's tests score (from 4 s, every beat):

  | | lock (s) | median (ms) | p95 (ms) | locked |
  |---|---|---|---|---|
  | 44.1 kHz, 10 tracks | 2.51-3.07 | 1.9-2.6 | 3.0-5.8 | every beat |
  | 48 kHz, 10 tracks | 2.50-3.07 | 1.3-2.9 | 2.2-5.0 | every beat |
  | 48 kHz, 174 BPM with prior 87 | 3.05 (at 87.01 BPM) | 2.6 | 5.0 | every beat |

  The targets (lock <= 4 s, median < 10 ms, p95 < 25 ms) hold with room;
  the 44.1 kHz numbers are the tap path's round 1 (MASCOT-POC.md: lock
  2.5-2.9 s, median 2.4-2.8 ms). So the tracker works at 48 kHz, which the
  device never fed it (its rate converter brings every track to 44.1).
  A prior of 87 or 174 picks that octave at 48 kHz as at 44.1.
- **1e-4 relative noise on every energy**: the same lock times and errors
  to the hundredth (90, 128, 174 BPM off-beat, both rates).
- **`setSampleRate()`**: at 48 kHz after 44.1 the tracker matches one begun
  at 48 kHz, hop for hop, bit for bit; the prior stays; the same rate
  allocates nothing; back at 44.1 the audio path meets its targets; every
  block is freed through the hook; out of memory it returns false and
  tracks nothing.
- **The console's router** (`test_host_line`): each framing rule, and
  10,000 random streams of console keys and host lines (good, bad,
  overlong, cut off by an `@`, ended by `\n`, `\r` or `\r\n`): no byte
  between an `@` and its terminator ever came out as a console key.
  Reading that starts mid-line (Sync): every suffix of the sender's lines,
  and 2,000 cut streams in random reads, gave no console key. A scratch
  replay of a real 5 s `--dry-run` stream (17,257 bytes) into a fresh
  reader from every offset, in one read and in 64-byte reads, gave 0
  console keys and read every whole line; before Sync, a review's replay
  of the same kind found 15,092 of 17,170 offsets running commands
  (`h<1 0 ...>` set the headroom, `c<1 52368 1>` saved a headphones name,
  ` ` played, a hex `f` would forget the headphones and restart).
- **The session** (`test_host_link`): 20 tests, line by line through every
  rule in [Sessions](#sessions), [The visualizer's messages](#the-visualizers-messages)
  and [Errors](#errors).
- **The heard clock** (`test_host_clock`): exact with no delay, behind by
  exactly a constant delay; with 2-32 ms of delivery jitter and a 100 ms
  outlier every 2 s, 2.7-3.6 ms behind the computer's frame (median) with
  a p5-p95 spread of 1.9-5.7 ms, tracking its leading edge to 1.2-2.4 ms
  (p95), its rate never more than 4.3 % off, and no snap after the first;
  200 ppm of drift followed within 0.5 ms.

### The sender

From `python -m unittest discover -s tools -p "test_usb_viz.py"` (52
tests, Python 3.10) and `--selftest`, and one check outside the repo:

- **The port is bit-exact.** Against `hop_golden.h`: the coefficients
  equal as float32, the click's beat frames equal and its samples' FNV-1a
  equal (`0x38cd684d`, `0x13b3a89d`), and all 344 + 375 hops equal as
  float32 (worst relative difference 4e-9: the golden file's `%.9g`
  printing). In double instead of float32 the worst was 5.5e-5, over the
  spec's 1e-5, which is why every operation is rounded. The hops are also
  the same whatever the chunk sizes, and after a reset.
- **Its lines through the firmware's own code.** A scratch harness (not in
  the repo) built `lib/core`'s `HostLine`, `HostLink`, `BeatTracker`,
  `HopFrontEnd` and `ClickGen` with g++ on the PC, fed the bytes of
  `--dry-run --fast` through them as `UsbViz` and `DanceMode` do, and
  compared the tracker with one fed the same click through `process()`:
  | | |
  |---|---|
  | `click90`, `click120off`, `click174` at 44.1 kHz; `click128`, `click140off`, `click174` at 48 kHz, 20 s each | every byte inside a host line (0 console bytes), every line accepted (0 `@err`, 0 bad, 0 gaps), two replies (`@ok`, `@bye ok`); the tracker identical to `process()`'s after every hop (BPM, lock, grid), lock 2.50-2.92 s |
  | the same, firmware-format `[beat]` and `[dance] locked` lines from that tracker scored by `--score` (60 s) | `click120off` 44.1 kHz: lock 2.80 s, 119.99 BPM, median 3.0 ms, p95 5.7 ms; `click174` 48 kHz: lock 2.50 s, 174.00 BPM, median 2.3 ms, p95 3.3 ms: `--expect "lock<=4,bpm<=0.5,med<=10,p95<=25"` passes |
  | `--gap-at 8`, `--seek-at 8:2`, `--pause-at 5:3` | 1 gap; a second epoch; the same epoch, no gap: 0 errors each, the tracker locked again on the beat |
  | `--drop 0.002` (60 s), `0.005` (60 s), `0.01` (20 s) | 9, 29 and 23 gaps: locked for most of the run; unlocked at the end; first locked at 9 s and unlocked at the end. Each gap resets the tracker, which takes ~2.6 s to lock again, so a lost line every second or two keeps the crab idle (see [Open questions](#open-questions)) |
  | `--measure --flash` | its `@log 2` accepted |
- **The session, in virtual time against `FakeCore2`**: `@hello` first and
  `@e` before any `@h` or `@c`; the hop numbers 0, 1, 2, ... with no gaps
  and their energies the golden file's; `@c` every 100 ms (±6 ms, the
  loop's tick) with `heard` = played - `--offset-ms`; the hops never more
  than the lead ahead nor a batch behind, at most 16 in a burst; hellos a
  second apart with one session id while lines are lost or `@err 4 hello
  ui` comes back (said once); giving up after the time limit; a reboot
  mid-stream (ROM lines, banner, two hellos lost): a new session id, `@e`
  for the same epoch, the hops of the 2 s without a session skipped; `@err
  3` and `@bye timeout` start over; `@err 9` sends `@e` again; `@bye user`
  and `@err 8` stop it with nothing more written (exit 3); `@err 2`, `@err
  7 hello`, `@err 4 hello dance` give up; a pause keeps the epoch and holds
  `heard`; a seek is a new epoch from hop 0; two sources in turn, at the
  second's exact start; `@bye` after Ctrl-C, answered; every byte written
  inside an `@` line of at most 255 bytes, in every run. The port opens
  with `port`, `baudrate`, `timeout`, `dtr = False` and `rts = False` set
  before `open()`, and neither touched after. WAV reading (16-bit, 24-bit,
  mono, another rate refused), a FLAC through ffmpeg (sample-exact), the
  message with no ffmpeg, and the scoring (lock, phase folding, octaves,
  flashes, the `[dance]` counters, `--expect`).
- **After review**: the first `@hello` 300 ms after the start; a reset
  before the first answer, and one mid-stream, with the banner 1.5 s after
  the ROM lines: nothing written in between, the hello at the banner; a
  ROM line and no banner: the hello 5 s later. Lines stamped on arrival
  through a fake with Windows' read batching, in virtual time: each within
  0.5 ms of its first byte on the wire (46 ms late with the old
  `read(4096)`). Scoring by the epoch a line names: a gap's reset, then a
  seek's epoch with no reset line of its own: its lock and beats go to it
  (with the old unnamed lines they went to the epoch before).

## Checked on the device (October 2026)

A Core2 v1.3 on COM3 (CH9102), Windows 10, Python 3.10, firmware
`v0.5.0-dev+249d8de-dirty` (this branch). Everything silent: the sender
without `--play`, the Core2 in its silent test mode (`z`), its queue
stopped at entry 8 of 27. The fault actions came from a scratch wrapper
around `tools/usb_viz.py` (not in the repo) that, at a given second,
writes raw bytes past the line guard, pulses RTS, raises the sender's
Ctrl-C, or asks for a screenshot (a bare `X`).

**A bug the host tests couldn't see, fixed.** On the first run every
session ended in the loop pass it began: `[viz] on`, then at once `[viz]
off (nothing from the computer for 3 s): 0.0 s, 0 epochs, 0 hops` and `@bye
timeout`, about 4,000 sessions in 3 minutes and not one hop fed. The loop
reads `millis()` once a pass, before `console.poll()`; `UsbViz::onLine()`
stamps the pass's lines with `millis()` again, a millisecond or more later;
`HostLink::poll(now)` then took `now - lastValidMs_` unsigned: ~49 days.
The same subtraction would have ended every decline at once. `HostLink` now
measures those two gaps as a signed difference, a later stamp counting as 0
(`since()`; the `@err` rate limit's window keeps its unsigned one: it only
ever sees line times), and `test_host_link` has
`test_lines_stamped_after_the_polls_now` (it fails without the fix). Every
check below is after it.

**1. The handshake.** `@ok 1 <session> v0.5.0-dev+249d8de-dirty viz,log`
35 ms after the first `@hello`, read whole: the tab switched to Dance and
`[viz] on: dancing to the computer (protocol 1, session ...); nothing was
playing; the headphones' search quiet`. Opening the port with DTR and RTS
low never reset the board: no ROM line and no banner in 13 sender runs and
the monitor's 8 reopenings, whether the last owner closed the port or was
killed.

**2. The tracker, fed from the computer** (`--measure`: the sender scores
the `[beat]` lines' next-beat frames against the click track's beats, as
the design specifies; 60 s a track, one session per rate, an epoch per
track):

| Track | Lock | BPM (error) | Beats scored | Median / p95 | Mean | Tap path, round 1 (MASCOT-POC.md) |
|---|---|---|---|---|---|---|
| click90, 44.1 kHz | 2.81 s | 90.00 (0.00 %) | 86 | 2.6 / 3.6 ms | −2.7 ms | 2.83 s, 2.6 / 3.5 ms |
| click120, 44.1 kHz | 2.61 s | 119.99 (0.01 %) | 115 | 2.4 / 5.7 ms | −2.9 ms | 2.62 s, 2.4 / 5.7 ms |
| click174, 44.1 kHz | 2.50 s | 174.01 (0.01 %) | 167 | 2.8 / 3.5 ms | −2.8 ms | 2.51 s, 2.8 / 3.5 ms |
| click120off, 48 kHz | 2.81 s | 120.00 (0.00 %) | 115 | 1.9 / 3.5 ms | −2.0 ms | (at 44.1: 2.81 s, 2.4 / 5.7 ms) |
| click174, 48 kHz | 2.50 s | 174.00 (0.00 %) | 167 | 2.3 / 3.3 ms | −2.3 ms | |

The targets (lock ≤ 4 s, BPM within 0.5 %, median < 10 ms, p95 < 25 ms)
hold with room, and at 44.1 kHz the numbers are the tap path's own to
0.02 s (lock) and 0.1 ms (error): the same tracker on the same energies,
as the host tests said. The 48 kHz numbers are the host results'
(`click120off` 2.81 s; `click174` 2.50 s, 2.3 / 3.3 ms). Shorter runs
agree: `click128` 2.93 s, 2.6 / 3.5 ms (MASCOT-POC: 2.93 s, 2.6 / 3.5);
`click120` 2.61 s, 2.4 / 5.7 ms twice; `click174` at 48 kHz again after the
last flash, 2.50 s, 2.3 / 3.3 ms. The two long sessions, 180 s with 15,501
hops and 120 s with 11,250: **0 gaps, 0 duplicates, 0 bad lines, 0 `@err`**
with the 1 KB receive buffer; 30 fps while dancing (10 when idle, before
each lock); `ram=81K min=75K` from start to end, as before the session.

The heard clock (`--flash`, `click120`, 40 s), the `[flash]` lines' heard
frame against the sender's own at their arrival: **median −6.9 ms, p5-p95
spread 4.0 ms** (expected: a few ms behind, a spread under 10 ms). The
`[dance]` line's `spread`, how far the window's samples sit behind its
leading edge (p95), read 21-23 ms: that is how unevenly Windows delivers
the `@c` lines, and the leading edge filters it out (one snap per session,
the first; the slew within ±0.5 %).

Screenshots of the Dance tab while the computer drives it (`X`: the whole
screen read back from the LCD, 0 of 18,000 px differing from the dancer's
sprite). `click128`, 8 s in: the left panel `128` BPM, `locked`,
confidence 100 %; the crab mid-step with its beat sparks and the beat dot
lit; "Dancing to your computer" over "Tap a button or a tab to stop" at
the bottom. `click174` at 48 kHz, 7 s in: `174`, locked, the crab with
both claws up. A whole-screen dump takes ~21 s at 115200 baud and holds
the loop to ~10.7 fps while it prints; the session took it without a gap
or a lost lock.

**3. Robustness.**

| What | Seen |
|---|---|
| The sender's Ctrl-C (its `KeyboardInterrupt` path, raised 10 s in) | `@bye`, `@bye ok`, `[viz] off (the computer said bye): 10.0 s, 1 epochs, 872 hops, 0 gaps, 0 bad lines; the player stays paused`. Then the screen's own timeout again: dim 19 s later, off 10 s after that. Still stopped |
| The sender killed (`Stop-Process -Force`, 12 s in), a monitor reopening the port at once | no reset (no banner); the crab idle 1.5 s after the last `@c` (the clock stale); `@bye timeout` and `[viz] off (nothing from the computer for 3 s): 14.4 s, 1 epochs, 992 hops, 0 gaps, 0 bad lines; ...` 2.8 s after the kill; stopped |
| Eleven bad `@` lines mid-session, 0.6 s apart: an unknown verb made of console keys (`@lLq`), `nan`/`inf`, 400 bytes of `l`, control bytes, bytes over 0x7E, a 20-digit epoch, `@l` + 0.4 s + `Lq` (one line, slowly), `@c 1 abc 1\r\n`, `@@@@lq`, `@ `, `@h\r` | eleven `@err` replies (`1` nine times, `6`, `7 lq`); `bad=13 errs=11` in the `[dance]` line (the `@`s that cut a line short count as bad, unanswered). **No console command ran**: no `> ` line, no queue listing, no stats, no partition table. 0 gaps; the crab danced on, locked (2.80 s, 3.0 / 5.9 ms) |
| The board reset by an RTS pulse 8 s into a session | the sender saw the ROM lines (`ets ...`, `rst:0x1 ...`), said "booting: quiet until its banner" and wrote nothing until the banner (its first `@hello` 1.8 s after the ROM line), got `@err 4 hello ui` once, then `@ok` 5 s after the reset with a new session, `@e` for the same epoch and the hops from where the play clock was (hop 1125): relocked 3.05 s later. One `[console] dropped 2 bytes of a computer's line already under way`: the tail of an `@c` the sender wrote during the pulse, before it could see the ROM line (Sync at work); no `> ` line |
| Two taps on the dancer (the console's scripted finger, `uit160,120`) | `[dance] skin: stick`, then `skin: crab`; the session went on |
| Button B (`uit160,260`, on the strip) | `[button] B click: ends the computer's visualizer`, `@bye user`, the sender stopped (exit 3). Run again at once: `@err 8 hello`, exit 3. Again 4 s later: `@ok` |
| A touch beside the dancer (`uit40,120`) | `[touch] down 40,120 ...: ends the computer's visualizer`, `@bye user`, exit 3 |
| The screen | woken and kept lit for whole sessions (180 s and 120 s with no `[screen]` line); `off -> bright (an event for the listener: the computer's visualizer)` when a session began with the screen off, `dim -> bright` when dim |
| The headphones' search | a boot's burst of three pages to the remembered headphones carries on into a session, then rests (`[bt] reconnect: resting (nobody around ...)`), as `setQuiet` does with the screen off. The entry line said "search resting" and the rest line "the screen off, nothing playing", both wrong for that case: now "search quiet" and "(the screen off and nothing playing, or the computer's visualizer)" |

**4. The Core2's own state**, recorded before the first run and after the
last: the queue 27 tracks at entry 8, 19 up next, stopped, no resume
point; the output the speaker; the headphones remembered (SPYDRONE), the
link resting; the touch calibration, haptics and rail ticks as they were;
the sleep timer off; the idle power-off at 20 min; the dancer the crab.
Nothing played at any point (every `[stats]` line `stopped`, `buf=0ms`,
`bt=0fps`). Then a normal boot (an RTS reset), which ends the silent test
mode: `[stats] track=8/27 stopped ... out=speaker`, and the boot bench
`[dance] tracker bench: 12.78 ms per second of audio`: moving the front end
out of `BeatTracker` cost nothing (before: ~13.3 ms).

**Not checked on the device:** unplugging the cable, the PWR key, a
finger on the glass (the scripted finger takes the same input path), the
headphones' play key, `--play` and the 240 fps video, the fault runs of
the plan's step 3 (`--jitter-ms`, `--drop`, `--gap-at`, `--seek-at`,
`--pause-at`; epochs at both rates were covered by the multi-track runs),
`--burst`, `--fuzz` and `--reset-on-open` (not built; the bad lines and the
RTS pulse above stand in), the 30 min soak, and the tap path's click
tracks after a session (only the boot bench).

## Device test plan

Everything runs **silent**: the sender plays nothing without `--play`, and
host mode pauses the Core2's own player. Before the first run, `z` in
`pio monitor` as well (silent test mode), then close the monitor: the
sender needs the port. `python tools/usb_viz.py --selftest` first, and
`--log run-<n>.jsonl` on every run, so `--score` can read it again later.
The fault times are seconds since playback started.

Ground truth comes from the sender: a click track's beats are known to the
frame (`ClickGen`), so the `[beat]` lines (the grid's prediction of each
next beat, scored as it stood, as the tap path's `clicks.py` did) give the
phase error, and the `[dance] locked` line the lock time. Every run prints
a table: per epoch the lock time, the BPM (and its error, octave-folded),
the median and p95 phase error from the lock on, the resets, gaps, bad
lines and the clock's snaps and spread.

**1. Entry and exit.** Play a track on the Core2 (silent), then start the
sender: the `[viz] on` line, the player paused, the Dance tab, the bottom
texts, the screen lit past its timeout. End it each way and check the log
line, the reply, that the player is still paused, and the tab: Ctrl-C
(`@bye` → `@bye ok`); killing the sender (off after 3 s, `@bye timeout`);
unplugging the cable; a tap beside the dancer and each button (`@bye user`,
the sender stops and says so; started again at once it is declined, after
3 s it gets in); a tap on the dancer (switches it, stays on). With the
remembered earbuds playing before the session: take one out and put it
back during the session: `@bye user`, and the player stays paused
(`[bt] headphones: play (ignored: paused, paused for the computer's
visualizer)`); the Core2's play button resumes it.

**2. Tracker accuracy.** `--measure` over `click90`, `click120`,
`click128`, `click140`, `click174`, `click120off`, each 60 s, at 44.1 and
48 kHz; `click174` again with `--prior 87` and `--prior 174`. Targets, as
for the tap path: lock ≤ 4 s, BPM within 0.5 % (octave-folded), median
< 10 ms, p95 < 25 ms. Expected: the tap path's own numbers (MASCOT-POC.md,
round 1: lock 2.5-2.9 s, median 2.4-2.8 ms, p95 3.5-5.7 ms) within a hop,
since it is the same tracker fed the same energies.

**3. Faults.** One run each, `click120off`:

| Run | Expect |
|---|---|
| `--jitter-ms 30` | tracker numbers unchanged (hops carry frames); the clock's spread ~30 ms, no snaps after the first |
| `--drop 0.002` | gaps counted (about 10 a minute), a reset and a new lock (~3 s) after each, never locked off the beat. (At 0.01 a gap comes every ~1.2 s and the tracker never holds a lock: the host run above.) |
| `--gap-at 20` | one gap, one reset, relocked by ~23 s |
| `--seek-at 20:40` | a new epoch, relocked ~3 s later, on the beat |
| `--pause-at 20:5` | the crab idles within ~0.5 s; on resume it dances again at once, no reset, no new lock |
| `--click 120off --rate 44100` and again at 48000, as two runs; or `--click 120 --click 140off` (two epochs) | a reset and a lock per epoch, the rate followed. (`--epoch-every`, alternating rates in one run, isn't built) |
| `--burst 1` (not built yet) | no gaps with the 1 KB buffer (and the largest burst that passes, by raising S) |
| `--fuzz 30` (not built yet: the guard refuses such lines, so it needs a raw path of its own) | `@err` replies (at most 4 a second), `bad` counted, and **no console command ran**: no `> ` line in the log, nothing plays, the headphones' pairing is intact, no restart |
| `--reset-on-open` (not built yet; meanwhile the reset button while the sender runs, in a session and while it is still helloing, and the Core2 powered on while the sender waits) | the ROM lines, the sender's "booting: quiet until its banner" and nothing sent until the banner; then `@err 4 hello ui`, `@ok` and dancing, within 8 s; no `> ` line and no `[console] dropped` line in the boot log (a `dropped` line means a tail was caught: nothing ran, but the sender wrote during the boot) |

**4. The heard clock.** `--measure` with `@log 2` (`--flash`): the
`[flash]` lines' heard frame against the sender's own clock at arrival.
Expect a median of a few ms behind (the least inbound delay plus the
outbound one) and a p5-p95 spread under 10 ms: the jitter the eye could
see. Then by eye, with `--play` on a click track and the Core2 next to the
computer's speaker: film both at 240 fps and compare the beat dot with
the click (about 4 ms a frame), on wired output and on the computer's
Bluetooth headphones; note the `--offset-ms` that lines them up for each.

**5. Soak.** 30 minutes of WAVs (`--wav` several; there is no loop, so
name them again): no restart,
`ram`/`min` steady against the first minute, 0 gaps, 0 bad, 30 fps while
dancing (24 at 160 MHz).

**6. The tap path afterwards.** After a session, play on the Core2 again:
the tracker back at 44.1 kHz, the taps on, a click track's numbers as
before.

## Notes for the terminal player (later)

- Start after `claude/device-flash` merges: a `src/device/link.rs` beside
  `ports.rs`, reusing `candidates()`/`pick()`, one owner of the port that
  lets go of it for `device flash`.
- A **lossless** feed for the front end: the visualizer's `AudioTap` drops
  batches by design and has no frame counter. A second never-blocking
  output from `Tapped::next` (mono, already box-averaged 8x: about 5.5 k
  samples/s into a ring), stamped with a running frame count and the
  epoch, bumped on a track change, a seek (`try_seek` already clears the
  tap) and the crossfade's switch. Native builds only; no C libraries.
- The heard clock from a smooth position anchor, the output latency as a
  setting (ms), the BPM prior from the API's `bpm`.
- As `tools/usb_viz.py` does: nothing written from a ROM line to the
  banner, ~300 ms of listening after opening the port, and the Core2's
  lines stamped as they arrive (serialport-rs: read what `bytes_to_read()`
  says is there, not a fixed size against a timeout).
- Off by default; on by a CLI flag or a setting; a status line ("Core2:
  dancing", "Stopped on the Core2", "Core2 firmware too old").

## Open questions

- Ending host mode: any touch outside the dancer (planned), or any touch
  at all (then the dancer can't be switched while the computer drives it)?
- On exit: stay on the Dance tab (planned) or go back to the tab before?
- A computer paused for a long time keeps the screen lit for the whole
  session (planned). Let it dim after, say, 10 minutes of `playing 0`?
- The receive buffer: 1 KB (planned) or 2 KB from the start?
- Pause keeps the epoch (planned), which needs the terminal player's feed
  to carry on frame-exactly across a pause; otherwise it starts a new
  epoch on resume and the crab relocks (~3 s).
- Whether serialport-rs's open on Windows raises RTS for a moment (a
  reset). The protocol copes either way; it changes how long the first
  connection takes.
- A lost `@h` line resets the tracker (a gap), and it takes ~2.6 s to lock
  again, so a loss rate of one line in a few hundred keeps the crab idle
  most of the time (host run: `--drop 0.01`). If the device tests show real
  gaps, fill a gap of one or two hops with the last hop's energies instead
  of resetting (the plan's "can come later" in [Hops](#hops)).
- The sender's `--play` has no output timing (`winsound`), so its heard
  clock is the call's return less `--offset-ms`. Good enough for the demo;
  a sender that wants it measured needs an audio API with stream times
  (`sounddevice`, an extra dependency) or the terminal player's own.
