# Host status

The mStream terminal player (a Rust desktop app) shows each plugged-in
Core2 on its MP3 Player tab: whether its firmware is up to date, its
microSD card's size and free space, its library, its battery, and a way to
tell two identical boards apart; it sets the board's clock, for the
play log's dates; and it can bring up, on the board's own screen, the
question that formats its card. It asks the **running firmware**, over
the USB serial port, without resetting the board (the bootloader probe it
used before resets it, stops the music and can't see the card). Five
lines, on the USB visualizer's `@` lines ([USB-VISUALIZER.md](USB-VISUALIZER.md)):

| Line | Answer |
|---|---|
| `@status` | at once, one line: `@status fw=... elf=... card=... size=... free=... tracks=... music=... bat=... state=... cid=... clock=... bt=...` |
| `@count` | one count of the card's free clusters, on request: `@count 0`, `@count <percent>` about every tenth, `@count 100`, `@count done free=<bytes>` |
| `@identify <label>` | "This one" and the label on the board's screen for 5 s, one buzz: `@identify ok` |
| `@time [<unix-ms>]` | the board's clock: alone, `@time <unix-ms>` or `@time unset`; with the computer's time, the board's RTC set to it: `@time ok` |
| `@format` | the question "Format this card?" on the board's screen, nothing more: `@format ok`; then what the listener answered there, `@format no`, or `@format 0`, the board's restart, `@format <percent>` about every tenth and `@format done size=... free=... cluster=...` |

Status: **built** (October 2026), in the release after v0.8.0 (0.9.0):
the host tests below pass, and both firmware builds pass their guards. The
device checks are the [plan](#device-test-plan) at the end. The design is
the MP3 Player tab's set in the mStream repository
(`docs/designs/mp3-tab`, cards 01 section 5 and 05: alternate A).
`@status cid=` (which card, at which commit) is newer, in the release
after v0.11.0 (0.12.0): host-tested and built; its device check is step 9
of the plan. So are `@time` and `@status clock=` (the board's clock, for
the play log), in 0.12.0 too: host-tested and built; their device check
is step 10. And `@format` (the card formatted in the player, from its own
screen or at a computer's ask, over any card), in 0.12.0: host-tested and
built; its device check is step 11, **on a spare card only**. Its second
step is a 2 s hold on the board, and a computer may ask it over a card
the player reads too: the user's picks (October 2026) on the card
transfer's set in the mStream repository (`docs/designs/card-transfer`,
card 06 section 5: alternate B).

## The link

- The Core2's USB serial port at 115200 baud, 8N1, as the visualizer's
  ([USB-VISUALIZER.md, "The link"](USB-VISUALIZER.md#the-link)). **Open it
  with DTR and RTS held low** and never touch them after: the board isn't
  reset, the music plays on.
- The lines are the visualizer's framing ([USB-VISUALIZER.md,
  "Framing"](USB-VISUALIZER.md#framing)): from `@` to `\n` or `\r`,
  printable ASCII, at most 255 bytes, never a console key. The replies end
  in `\n` (strip a `\r`); each is written with one `printf`, and a log line
  from another task can still land between two of them: a reader skips the
  lines that don't start with `@`.
- **No session.** The five are answered with or without a visualizer
  session (`@hello`), and while the visualizer is declined (the listener
  ended it on the Core2): they drive nothing on the board's screen but the
  identify banner and the format's question, and change nothing but the
  clock (`@time`): a card is formatted only by the listener's two answers
  on the board, a tap and a 2 s hold (`@format`). They don't
  keep a session alive, and they don't count as the computer talking for a
  decline's quiet, so a player that asks for `@status` every second can
  still start the dancer once its own user asks.
- **Older firmware.** A verb a firmware doesn't know gets `@err 7 <verb>`:
  v0.6.0 to v0.8.0 answer `@status` with `@err 7 status`, which says "this
  firmware is older than 0.9.0". The player then sends the console's `L`
  for the version (on its own, not with an Enter in the same write: right
  after a boot the console's Sync drops a key sent with its Enter) and says
  the card needs a firmware update. A board that answers `@status` is new
  enough for `@count` and `@identify`. `@time` came in 0.12.0: an older
  board answers it `@err 7 time` (and its `@status` has no `clock=`), and
  that answer is how a sender tells, never the version; so with
  `@format`, also 0.12.0's (`@err 7 format`). No answer to
  either within about a second: not this firmware running (another
  firmware, or none booted); only a reset can say more. A key added to
  `@status` later is missing from an older firmware's line, and its
  absence is how a reader tells, never the version: `cid=` and `clock=`
  came in 0.12.0.

## @status

```
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87 state=playing cid=5eedc0de0a1b2c3d.0000002a clock=ok bt=Alex%27s%20headphones
```

One line, answered in the loop pass that reads the question, **never a
read of the card**: every field is something the firmware holds already.
The fields come in this order, each `key=value`, one space between them; a
value never holds a space. `?` is unknown, `-` is none. **A reader must
ignore keys it doesn't know**, and take a key that is missing for an older
firmware's line: a field added later goes in after `state=` and the ones
added before it, and before `bt=`, which stays last. When the line runs
long, `bt=` gives way first, down to 36 bytes; past that the keys after
`state=` do, the last first ([the budget](#the-budget)).

| Key | Values | From |
|---|---|---|
| `fw` | the version as the app description holds it, which `L` prints and the bootloader probe reads: `v0.9.0`, `v0.9.0-3-gabc1234-dirty`, `v0.9.0-dev+abc1234` | `version::appDesc()` (31 bytes at most). Bytes outside 0x21-0x7E would be `_` |
| `elf` | 8 hex digits | the ELF's SHA-256, as About and the boot banner show it (`version::elfSha()`) |
| `card` | `none`, `fat32`, `fat16`, `exfat`, `ntfs`, `gpt`, `other`, `unreadable` | mounted: FatFs's volume type (`fat16` for FAT12 too: a card of a few MB, which FatFs mounts as well). Not mounted: what the card's first sectors said at the boot or at the last Try again (`cardformat`, as the empty state's message): `none` nothing answered; `exfat`, `ntfs` (an MBR partition of type 0x07, or a superfloppy) and `gpt` don't mount on the Core2; `other` a card answered and nothing was recognised (blank, Linux's, a FAT that didn't mount); `unreadable` a card answered and its first sector couldn't be read. A mounted card that FatFs lost since (pulled, and it didn't come back) reads `none` |
| `size` | bytes, or `-` | the card's CSD sector count × 512, kept by the SD driver since the mount (About's "microSD card, 63.9 GB": `SD.cardSize()`); for a card that didn't mount, the same count read with its first sectors. `-`: no card answered |
| `free` | bytes, `?` or `counting` | FatFs's free cluster count × the cluster size, **only when it is a count** (the FSINFO rule below); `?` when it isn't, and with no FAT card mounted; `counting` while a `@count` runs. Never a guess, never 0 or the size for unknown |
| `tracks` | a count, `building` or `-` | the library index's tracks; `building` behind the library update's fence, while its build has the index (docs/METADATA.md 3.4.2); `-` with no card (the internal flash's test tracks aren't counted) |
| `music` | bytes or `?` | the indexed tracks' bytes. This firmware's index keeps no file sizes (a track's record is 32 bytes: its names, numbers and length), so it is always `?`; it is not read from the card. A later release can sum the sizes the walk records hold at the build and save the sum with `library.idx` |
| `bat` | 0-100, or `-` | the power chip's battery reading the UI takes every 10 s (the tab bar's); `-` until the first |
| `state` | `playing`, `paused`, `idle`, `host` | the player: `playing` also while a play waits for the headphones; `idle` stopped; `host` while a computer drives the dancer (the visualizer's host mode) |
| `cid` | the card's id and its commit's generation, `<cardId>.<generation>`: 16 and 8 hex digits in lower case, leading zeros kept (`5eedc0de0a1b2c3d.0000002a`); or `-`. Since 0.12.0 | the root the boot read (docs/METADATA.md 2.5.2, 2.5.4: `/.mstream/manifest.bin` or `.tmp`, the one the election picked, checked whole; `Library::root()`): its `cardId`, and its `generation`, the number in the commit's file names (`tags-0000002a.bin`). Kept when the root's tags file then fails the boot's checks (the boot log's `[lib] the card:` line names that file, the generation and why: `no transfer data (the root's T tags-0000002a.bin, generation 42: missing)`; the library is the walk's alone): the root still names the card. `-`: no root (no `/.mstream`, or none valid), no PSRAM to read it, the internal flash, or no card mounted now (`card=` isn't `fat32` or `fat16`: a card pulled reads `none`). **Read once, at the boot**, never during `@status`: another card put in while on is a restart (the card's guard, at FatFs's remount, its first read of that card: ARCHITECTURE.md, Storage), and from the moment the guard finds it until that restart (about a second: FatFs has the other card mounted then, and `card=` says its kind) `cid=` is `-` (before that first read nothing on the board knows the card changed); but the same card pulled and put back keeps the boot's root until a restart, even if a computer wrote a new commit to it meanwhile |
| `clock` | `ok` or `unset`. Since 0.12.0 | the board's clock ([@time](#time); `boardclock::ok()`): `ok` while it has a time it believes, the RTC's read at the boot or a computer's `@time` since; `unset` when the boot's reading wasn't believed (the RTC's voltage-low flag set, a date outside 2026-2099, no RTC) and no `@time` came since. A reader takes any other value as `unset` |
| `bt` | the paired headphones' name, percent-encoded, or `-` | their own name once linked, before that the one saved at the pairing (`BtSink::shownName()`, the Output tab's); `-` with nothing paired, or a pairing whose name the board never learnt |

**Percent-encoding** (`bt=`): RFC 3986's unreserved bytes (`A-Z a-z 0-9 -
. _ ~`) as they are, every other byte of the name's UTF-8 as `%XX` in
upper-case hex: `Alex's headphones` is `Alex%27s%20headphones`. Any URL
decoder reads it back to the same bytes.

### The budget

**At most 255 bytes**, as any host line. The fields at their widest, as a
board can send them:

| Field | Bytes | At its widest |
|---|---|---|
| `@status` | 7 | |
| ` fw=` | 4 + 31 | the app description's version: 31 characters (its field is 32 bytes with the terminator) |
| ` elf=` | 5 + 8 | always 8 |
| ` card=` | 6 + 10 | `unreadable` |
| ` size=` | 6 + 13 | the CSD's sector count × 512: SDXC's most, 2 TiB (FatFs's sector numbers are 32 bits), `2199023255552` |
| ` free=` | 6 + 13 | never more than the size (`counting` is 8) |
| ` tracks=` | 8 + 10 | a u32 (`building` is 8) |
| ` music=` | 7 + 13 | never more than the size (`?` in this firmware) |
| ` bat=` | 5 + 3 | `100` |
| ` state=` | 7 + 7 | `playing` |
| ` cid=` | 5 + 25 | always 25 (`-` is 1) |
| ` clock=` | 7 + 5 | `unset` (`ok` is 2) |
| ` bt=` | 4 | |
| **before `bt=`'s value** | **215** | |

So `bt=` keeps 40 bytes at least. The section's first line leaves it 95,
a dev build's on a 1 TB card about 80, so a name of up to 63 bytes (all
BtSink keeps) fits whole unless much of it needs encoding. A name that
doesn't fit is cut at a character (never inside a UTF-8 sequence or a
`%XX`) and ends with `%E2%80%A6`, "…".

**The give-way rule.** `bt=` keeps 36 bytes whatever the other fields hold
(`hoststatus::kBtRoom`). The keys after `state=` (`cid=` and `clock=`,
today) are each put whole or left out, never cut: while they would leave
`bt=` under 36, the last of them is left out (`clock=`), then the one
before it (`cid=`). With the widths above they all fit, with 4 bytes to
spare. Only the formatter's own caps, wider than any field a board sends,
can make one give way: with `fw=` at its cut of 48 bytes, `elf=` at 16,
`size=`, `free=` and `music=` (64-bit) at 20 digits and `tracks=` (32-bit)
at its 10, the fields to `state=` and ` bt=` take 219 bytes, which leaves
`bt=` its 36 exactly and no room for `cid=` or `clock=`. A key added later
goes in after the last of these (so it gives way first), with its row in
the table above; a key wider than the spare bytes (4) would give way on a
real line. `test_host_status` checks the widest line, the line at the cap,
and the rule's edges (`clock=` left out first, then `cid=`).

### The FSINFO rule

FatFs keeps the volume's free cluster count in RAM (`FATFS::free_clst`).
The mount takes it from FAT32's FSINFO sector as it is, and from then on
FatFs follows every cluster it takes or frees, as long as the count is one.
The rule, FatFs's own (`f_getfree()`, `create_chain()`): **it is a count
when it is at most the volume's clusters** (`n_fatent - 2`). FSINFO's
"unknown" is 0xFFFFFFFF, which FatFs also sets on FAT12/16 (they have no
FSINFO) and when the FSINFO sector is missing or broken; any other value
above the clusters is a broken FSINFO, unknown too. Unknown is `free=?`.
A card Windows or a camera wrote usually has a valid count; a card
formatted by some tools, or whose last writer didn't update FSINFO,
doesn't. `hoststatus::freeCountValid()`.

The firmware reads that word and the cluster count as they are, with no
lock and no card access. It never calls `f_getfree()` for this (nor
`SD.totalBytes()` or `usedBytes()`, which are `f_getfree()`): with the
count unknown it reads every sector of the FAT, holding the volume's lock
throughout (minutes on a 1 TB card), and the decoder's reads would time
out (ARCHITECTURE.md, Storage).

## @count

```
→ @count
← @count 0
← @count 10
   ...
← @count 100
← @count done free=38214565888
```

One count of the card's free clusters, **only when a computer asks**, never
at boot. `@count 0` comes at once, then `@count <percent>` each time the
percent reaches another tenth (or the count moved and 5 s passed: a
tenth of a big card's FAT can take a while), `@count 100`, and the last
line `@count done free=<bytes>`. From then on `@status` says the counted
value (`free=` is FatFs's count again, which it now follows).

**Refused**, with `@err 4 count <why>`, and nothing starts:

| Why | When |
|---|---|
| `playing` | the player plays, or a play waits for the headphones (or the console's test track plays) |
| `library` | the library update holds the card: its build behind the fence, then its save |
| `card` | no FAT card mounted (`card=` isn't `fat32` or `fat16`), or the count can't run on it (no memory for its table and its 32 KB buffer; a build without the sector cache's wrapper, which carries the write watch) |
| `counting` | a count is under way already (it goes on) |

**Stopped part way** for the same reasons: a play that starts during the
count (`@err 4 count playing`), the library update beginning (`@err 4
count library`), a read error, the card mounted again, or FatFs's volume
lock not had in its own 10 s (`@err 4 count card`). That `@err` is the
count's last line; nothing changed (`free=` is what it was).

**How it counts** (`lib/core/FreeCount`, `storage/CardSpace`):

- The FAT is read in **pieces of 32 KB** (64 sectors: about 13 ms on the
  Core2's card at 25 MHz, one card command), **one piece per loop pass**,
  each under **FatFs's own volume lock**: `ff_mutex_take()` of the
  volume's drive, the lock every FatFs call takes (`lock_volume()` in
  FatFs; the decoder's reads, the card worker's, the queue's saves all
  wait on it). Between pieces the lock is let go and the loop runs on (it
  sleeps 5-20 ms a pass, below the decoder's priority), so the card's
  other readers wait one piece at most. The lock isn't recursive: while it
  is held nothing but the disk functions is called. The reads go through
  the sector cache's wrapper as multi-sector reads, which pass the cache
  by (it keeps single sectors).
- The pieces' free entries are counted as `f_getfree()` counts them:
  entries 2 to the last cluster's, free when 0 (FAT32: their low 28 bits).
  FAT32, FAT16 and FAT12 (whose whole FAT is one piece).
- **FatFs may write the FAT between two pieces** (the card worker's
  journals, a cover, the queue's save, a delete). The sector cache's
  wrapper sees every write FatFs makes, under that lock, and tells the
  count (`sectordisk::watchWrites()`); a piece already counted that a write
  reached is read again at the end. FatFs also keeps the FAT sector it last
  changed in its window (RAM) until it writes it: a piece read while that
  is so takes the window's bytes for that sector, and the commit reads the
  window's piece once more.
- **The commit**, under the lock that read the last piece (or, if writes
  left more than two pieces to read again, a later one): the count is
  exact as of that hold, what `f_getfree()` would have counted holding the
  lock all along. FatFs's count is set to it, and **FSINFO is marked to be
  written** (`fsi_flag`'s bit 0), exactly what `f_getfree()` does after
  its own count. FatFs writes FSINFO at its next sync (a file it wrote
  closed or synced, a delete, a rename: the next time the board saves
  anything on the card, the queue, a resume point, the tag scan's
  journal), FAT32 with an FSINFO sector only. So the next mount reads the
  count at once, and `free=` is known after a restart too. FatFs writes the FSINFO sector this way after any
  file that grows; only the value is new (a count instead of "unknown").
  Nothing else is written. A FAT16 card has no FSINFO: its count lasts
  until the next boot.
- **How long**: one piece a pass, so about 1.6 MB of FAT a second with
  the screen lit (a pass every 5-7 ms) and about 1 MB with it dark (20 ms):
  a 64 GB card's FAT (7.8 MB at 32 KB clusters) in about 5-8 s, a 1 TB
  card's (122 MB) in about 2 minutes. ESTIMATED: the device check measures
  it.
- **Costs**: a 32 KB buffer and a table of 2 bytes per piece and a bit
  (16 KB for a 2 TB card), PSRAM, while it runs; nothing in internal RAM
  but the code. The count holds the idle power-off off while it runs (a
  computer waits for it).
- Tested on the host (`test_free_count`): synthetic FATs of each type, and
  ChaN's FatFs R0.15 on a RAM disk with files written, left open, grown,
  cut and deleted between the pieces (FatFs's next-free hint sent all over
  the FAT): every count equals `f_getfree()` at the commit, and the count
  is in FSINFO at the next mount once FatFs synced.

## @identify

```
→ @identify COM5
← @identify ok
```

The board shows a full-screen banner, "This one" (white, Title) over the
label (the Output tab's blue), in a 3 px frame of that blue, for **5 s**,
and gives **one 0.2 s buzz** on the vibration motor (not when the
listener turned haptics off: the Output tab's Haptics, or `ah0`). Then
everything it covered is drawn again: the tab, the page, a sheet or dialog
that was open, a toast. A touch on the glass ends it sooner and does
nothing else. The player, the queue and the headphones carry on
underneath: nothing pauses, nothing is saved. The screen wakes for it if
it was dim or off, and stays lit while it shows. Sent again while it is
up: the new label, 5 s from then.

- **The label** is one field: the computer's name for the port (`COM5`;
  on Linux and macOS its short name, `ttyACM0`, `cu.usbmodem14101`), **1
  to 16 bytes** of printable ASCII (a host line's field can't hold a space
  anyway). None: `@err 1 identify`; longer: `@err 5 identify`. The banner
  draws it in Title when it fits, else in Bold (16 of the widest character
  fit: `test_ui_library`).
- **Refused** with `@err 4 identify <why>`: `ui` the start-up screen (the
  UI starts about 3 s after the boot, later on a new card's first boot);
  `screen` another screen has the display (the touch calibration, a spike
  tool); `viz` a computer drives the dancer (the Dance tab shows it
  already).
- **The visualizer waits for it**: an `@hello` while the banner is up is
  refused `@err 4 hello screen` (its sender retries every second, so the
  session starts once the banner goes). Entering under the banner would
  stop the dancer the session needs, and end the session in the same
  loop pass (`@bye dance`).

## @time

```
→ @time
← @time unset                         (the RTC lost its time, or was never set)
→ @time 1792332202123
← @time ok                            (2026-10-18 14:03:22.123 UTC)
→ @time
← @time 1792332204870                 (2.7 s later)
```

The board's clock, since 0.12.0: the Core2's RTC (a BM8563 on its
internal I2C bus, M5Unified's `M5.Rtc`), read once at the boot, and set
by a computer. The play log (docs/METADATA.md 2.20) takes its plays'
start times from it, so the MP3 Player tab sets it when it sees the board
and the clock is `unset` or more than 2 s off. Always UTC: the board keeps no time zone.

- **`@time`** alone asks: `@time <unix-ms>`, the board's time now in ms
  since 1970-01-01 00:00 UTC, or `@time unset`. At once, with no read of
  the chip (the time is the boot's reading, or the last `@time`, plus the
  board's monotonic ms since); not logged.
- **`@time <unix-ms>`** sets it: the computer's time in ms since
  1970-01-01 00:00 UTC (JavaScript's `Date.now()`; in Rust,
  `SystemTime::now()` since `UNIX_EPOCH`, as ms), digits only (the
  protocol's numbers: no sign, point or exponent; 64 bits at most).
  **Accepted: `1767225600000` (2026-01-01 00:00:00.000 UTC) to
  `4102444799999` (2099-12-31 23:59:59.999 UTC)** (`wallclock::kMinMs`,
  `kMaxMs`): no board ran this firmware earlier, and the RTC's calendar
  ends with 2099. So a computer whose own clock was never set (1970,
  1980), seconds sent for ms (10 digits: January 1970) and microseconds
  (16 digits: past 2099) are refused, `@err 5 time`; not a number is
  `@err 1 time`. Either way nothing changes.
- **Setting it**: the board's clock takes the ms at once, and the
  system time with it (FatFs dates the files the board writes by it, and
  `M5.begin()` copies it from the RTC at every boot): `@time ok`, and the
  log's `[clock] 2026-10-18 14:03:22.123 UTC, set by a computer's @time
  (it was 1.400 s behind); the RTC at its next second` (or `(it was
  unset)`). The RTC (which counts whole seconds; writing them clears its
  voltage-low flag) is written **on a second**: when the clock just set
  reaches its next whole second (at most a second later; at once on a
  whole second, and in 2099's last), on the loop's first pass then,
  nothing waiting on it (a pass more than 30 ms late waits for the next
  second, twice at most: `lib/core/RtcSync`). It is read back at once
  (the flag clear, that second or one after) and the chip's next tick
  caught, as the boot's catch below: `[clock] the RTC set to 2026-10-18
  14:03:23 UTC on its second (written 4 ms after it); its seconds begin
  +5 ms from the true ones (within 6 ms): kept for the boots`. Refused at
  once, nothing changed, only when no RTC answered at the boot or a read
  of its time fails now: `@err 4 time rtc`, and `[clock] @time: ...;
  nothing changed`. A write that doesn't go through, or a read-back that
  isn't the second (or still has the flag, or none goes through), is
  written again at the next second, three times in all (`[clock] @time:
  the RTC didn't take 2026-10-18 14:03:23 UTC (...); written again at the
  next second (2 of 3)`, then `given up`): this boot's clock stays the
  computer's, and the RTC is as the cut rule below leaves it (as it was,
  unset, or the time), which the tab's next look sets right. Nothing else
  refuses it: while music plays, in a session, declined or not. Sent
  again, the latest wins (a write or a catch under way dropped).
- **Cut short**: the chip takes each byte as it comes, so a reset in the
  middle of the write (the flasher's RTS pulse, a crash, a brown-out)
  would leave part new, part old, and one write of its seven time
  registers, the seconds first, could leave a time the next boot believes
  wrongly (the new seconds and minutes with the old hour or date). So it
  goes in three writes (`wallclock::rtcWrites()`): the month register's
  century bit set first (M5Unified then reads 19yy: outside the range),
  then the seven with the bit still set, then the seven with it clear.
  Cut at any byte, the next boot finds the RTC as it was, unset (`the RTC
  says 19..`, or its voltage-low flag), or at the time sent; never a
  mix (`test_wall_clock`'s `test_rtc_write_cut`, every byte over chips
  never set, believed, and a carry away).
- **At the boot** (an early `[clock]` line): the RTC's time is believed
  only when the chip answered, its **voltage-low flag** is clear (VL, bit
  7 of its seconds register: set at the chip's first power-up and whenever
  its supply failed since, cleared only by setting the time), and its date
  is a real one in the range above: `[clock] 2026-10-18 14:03:22 UTC, from
  the RTC (to the second; to a few ms at its next tick)`. Otherwise
  `clock=unset`, and the line says why: `the RTC lost its time (its
  voltage-low flag: ...)`, `the RTC says 2000-01-01 00:00:12 UTC, outside
  2026-2099 (never set, set wrong, or a @time cut short)` (a chip never
  set counts from 2000; the year is read first, so a 19yy is this too),
  `the RTC's reading isn't a date`, `no RTC answered`.
- **The catch**: the chip's fraction of a second can't be read, so the
  boot's clock takes the second it read as that second's start (up to a
  second behind) until the chip's next tick. From the loop's first pass
  the RTC is read every 10 ms or more (one run of its seven time
  registers, the boot's read's burst; nothing longer on the bus it shares
  with the touch panel and the AXP192) until its second changes, and the
  clock becomes the new second, plus the phase below, at the moment
  between the last read of the old second and the first of the new
  (within half the gap: a loop pass is 5-25 ms): `[clock] 2026-10-18
  14:03:23.005 UTC at the RTC's tick, within 6 ms (its seconds +5 ms from
  the true ones, the last @time's); the boot's second was 0.432 s
  behind`. Earlier moments of the boot follow it (`at()`: a play's start,
  dated at its end). A tick between reads more than 30 ms apart (a slow
  pass) is kept while the catch looks for a closer one, 3.3 s at most;
  reads that don't go through are skipped. Not caught (its second
  unchanged for 1.1 s: the chip isn't counting; too few reads went
  through in 3.3 s; a tick more than a second from the boot's reading):
  `[clock] the RTC's tick not caught (...): the boot's reading kept, to
  the second`.
- **The phase**: where the chip's seconds begin depends on what it does
  at a write (ESTIMATED either way: the BM8563's datasheet doesn't say;
  the PCF8563's divider runs on through one). A chip that restarts its
  divider at a write begins its seconds where the write was, so a write on
  a second makes them the true ones, to the write's few ms (one that
  holds its first tick after a write, as the PCF8563 does after its STOP
  bit, begins them that much later); one that doesn't keeps its own,
  anywhere within a second of the true ones. So a
  `@time`'s catch measures how far after the true seconds the chip's
  begin (the clock the `@time` set, at the chip's tick, less its new
  second): the phase, kept in NVS (`clock`/`phase`, an i32 of ms; schema
  5, ARCHITECTURE.md's NVS rules) and added at every boot's catch. It is
  removed just before the write and kept once caught, so it is always the
  phase of the time in the chip: a restart in between (a second or two)
  leaves none, and the boots add 0 until the next `@time` (exact to the
  write's lateness for the first kind of chip, within a second for the
  other). **After a restart the clock is within a few tens of ms of the
  time set** (the two catches' half-gaps: within 30 ms at the loop's
  pace, either kind of chip, wherever its seconds and the `@time` fall:
  `test_rtc_sync`'s model), plus the chip's drift while off: far under
  the tab's 2 s, so a restart alone never makes it set the clock. (Before
  the catch and the write on a second, a restart read to the second:
  0.4-1.3 s behind on the device, 2026-10-20.)
- **With the board off** the RTC runs on its own supply (ESTIMATED: step
  10 of the plan checks that it keeps the time while off). It drifts, by
  a second or two a day (ESTIMATED: a watch crystal's 20 ppm), which the
  tab's 2 s rule takes back whenever it sees the board.
- **Who reads it**: `app/BoardClock`: `now()` (Unix ms, 0 while unset),
  `at()` (the time at an earlier moment of the boot, before the clock was
  set too: a play's start, dated at its end), `source()` (the play log's
  clock byte: 1 the RTC's, 2 a computer's). Any task may read them.

## @format

```
→ @format
← @format ok                          ("Format this card?" on the board's screen)
   (the listener taps Format..., then holds "Hold to erase" 2 s, on the board)
← @format 0                           (the board restarts to format: its boot lines follow)
← @format 10
   ...
← @format 100
← @format done size=63864569856 free=63843467264 cluster=32768
```

The card formatted **in the player**, since 0.12.0. The computer can't
format a card: `@format` only brings up the question on the board's
screen, the one the board asks itself on a card it can't use (the
no-card page's Format...), over any card in the slot (one the player
reads too: its text names the music), and the listener answers it there,
twice (a tap, then a 2 s hold), or not. Nothing on the computer's side
formats, partitions or writes the card. Older firmware: `@err 7 format`.
The software asks only when its listener asked it to (never during a
sync: the card is in the computer's reader then, and the board answers
`card` with an empty slot).

- **The answer**, at once: `@format ok`, the question is on the screen
  (the screen wakes for it); or `@err 4 format <why>`, and nothing is
  asked: `ui` the start-up screen; `screen` another screen has the
  display (the touch calibration, a spike tool); `viz` a computer drives
  the dancer; `busy` a restart is under way (the format's own, the CPU
  speed's, the card guard's); `card` nothing answered in the slot
  (`card=none`). Sent again while the question is up: `@format ok`
  again, and the answer comes once.
- **The question** (`ui/Ui::askFormat()`): two dialogs, each with Cancel.
  - The first, "Format this card?" with Cancel and Format... (red), its
    body by the card: one that isn't FAT32 (`card=exfat`, `ntfs`, `gpt`):
    "It becomes an empty FAT32 card for this player. Everything on it is
    erased, even what the player can't read." One the player reads (only
    `@format` asks that): "Everything on it is erased: your music, the
    player's files, and what the player can't read."; and when a
    computer's transfer didn't finish on it (the root's plan,
    `/.mstream/pending.bin`, docs/METADATA.md 2.12.5), that first: "A
    transfer to it didn't finish. Everything on it is erased, ...". A
    damaged card (`card=other`: blank, Linux's, a FAT that didn't mount;
    or `unreadable`) is first sent to a computer: "Check it on a computer
    first", "A computer's disk check may save what's on it. Formatting
    erases it all, even what the player can't read."; erasing it is the
    second, explicit step.
  - The second, "Erase the whole card?", "This can't be undone. The
    player restarts and formats the <n> GB card: leave it in until it's
    back.", with "Hold to erase" (`format.sure_button`) on the left,
    where the first one's Cancel was, and Cancel on the right; under the
    body, from the start, "Hold the red button for 2 seconds"
    (`format.hold_hint`, in the body's colour). **"Hold to erase" answers
    only to a hold** (the Dialog's hold mode, `lib/core/DialogTouch` and
    `HoldButton`): drawn empty with a red outline, it fills with red from
    the left while a finger stays on it, and at 2 s
    (`HoldButton::kHoldMs`, the finger still on it) it is the answer,
    with the double tick. Lifted sooner, however quick or slow the tap,
    or slid off it (past its hit area by 10 px: `kSlackPx`, so a resting
    finger's drift and the panel's noise don't count), it empties and
    nothing happens but the inert buzz and the hint turning amber. Each
    press starts from nothing: no tap, double tap or run of taps adds up
    to an answer, and a lift the panel reads for a moment ends the hold.
    A touch that lands in the dialog's first 0.6 s goes nowhere
    (`formatask::kSettleMs`, from the tap that answered the first
    question, its own time: a double tap's second touch, which would
    otherwise land on its Cancel). A hold under way when its touch is
    taken (the screen dark, the identify banner, a Cancel from the input
    layer) drops, nothing done.
  - **Cancel is the default**: anything but a hold on the second one's
    "Hold to erase" kept to its end ends it as Cancel: Cancel, a tab,
    another dialog in its place (the headphones lost), another screen
    taking the display, the language changed, a minute with no answer
    (each dialog's own minute, `formatask::kAskMs`; it wins over a hold under
    way). Then `@format no` (to a question `@format` asked) and the log's
    `[format] the question went (<how>): nothing formatted`. Taps tick,
    as every dialog's.
  - **The touch buttons** (A, B, C on the strip under the screen) keep
    their work under the question, as under every dialog (ButtonPolicy:
    previous, play/pause, next and their holds): they never answer it,
    start a hold or end one. Only the panel's first touch point counts,
    so a second finger on the strip during a hold does nothing. The PWR
    key wakes the screen as ever.
- **Format** (the second one's): `@format 0`, the note "Restarting to
  format the card", and the CPU speed's orderly restart: paused first
  (after it nothing plays by itself), the queue and the card jobs' state
  saved, the headphones let go and the speaker's amp off (at most 3 s;
  never while the library's files are written), the card worker's step
  finished, the play log flushed. Just before it, the card's identity
  (the card guard's: its size from the CSD, the CRC-32 of sector 0 and of
  its first partition's first sector) and who asked go into RTC memory,
  which a restart keeps and a power cut doesn't: a later power-on never
  formats. Refused then, nothing restarts (the note "Couldn't format the
  card: try again", the log's line, and to a question `@format` asked its
  `@err 4 format <why>`): `card` (another card in the slot since the
  boot, no card answering, no identity kept), `size` (a card FAT32 can't
  take: under 41.9 MB, or 2 TiB and more), `busy`; and on the console's
  pretend card states (`uiF...`), whose card isn't the one in the slot.
- **The format, at the boot** (`storage/CardFormatter`, over
  `lib/core/CardMkfs`), before anything mounts the card: the request
  taken and cleared first (a crash or a cut doesn't format again), the
  card initialised, its identity read and held to the request's (another
  card, or none answering: nothing written, `@err 4 format card`); then
  1. the partition's first 8 sectors zeroed, the MBR (one primary
     partition, type 0x0C, from sector 8192, 4 MiB in, to the card's last
     sector; a random disk signature), the gap before it zeroed (a GPT's
     header and table, an old volume's boot sector) and the card's last
     64 sectors (a GPT's backups);
  2. FatFs's `f_mkfs()` in that partition: FAT32, 2 FATs, **32 KB
     clusters on every card from 2.16 GB up** (a smaller one: the largest
     cluster that keeps FAT32's 65,526), the data area from a 4 MiB
     boundary, FSINFO's free count valid (the FSINFO rule: `free=` is
     known at the next `@status`). Its boot sector and backup are held
     back and written last, so a format cut short leaves no FAT volume
     (the card reads "Can't read this card", `card=other`, and is
     formatted again), never a FAT32 whose FAT is half the old card's;
  3. read back: the MBR, the boot sector and its backup, FSINFO and its
     backup, both FATs' first entries, as planned;
  4. `/music` and `/.player` made, `/.player/device.txt` written (this
     firmware's: the software can write Opus and Auto DJ before the card's
     first boot in a player; docs/METADATA.md 2.2, "A card the player
     formats");
  5. the boot goes on and mounts it, another card to the card's guard (a
     new volume serial and disk signature): `cid=-`.

  The boot screen shows "Formatting the card" over a bar and "Leave the
  card in and the player on"; the first screen after it, "Card
  formatted: ready for your music", or "Couldn't format the card: try
  again".
- **The lines**: `@format <percent>` each time the percent reaches
  another tenth, and every 5 s while it moves (`@count`'s rule,
  `hoststatus::Progress`), the percent being the sectors written of the
  format's whole (the gap's 4 MiB and the FATs, mostly); `@format 100`;
  `@format done size=<bytes> free=<bytes> cluster=<bytes>`: the card's
  size (`@status size=`), the free space FatFs counts once `/music`,
  `/.player` and `device.txt` took a cluster each (the next `@status
  free=`), the cluster size. Failed: `@err 4 format <why>`, the last
  line: `card` (another card, or none answered), `size`, `write` (the
  card refused a write), `mkfs` (`f_mkfs()` refused: the log names its
  FRESULT), `check` (the card read back isn't what was written), `files`
  (`/music` or `device.txt`). Between `@format 0` and the first percent
  the board restarts: the ROM's and the boot's lines come between (a
  reader skips what doesn't start with `@`), and nothing is answered
  until the format ends and the loop runs. A format the board asked
  itself (its no-card page) says the same lines, but for `ok` and `no`.
- **The log**: `[format] asked before the restart (on a computer's
  @format): the card of <n> sectors (<n> GB)`, `[format] FAT32 on an MBR:
  the partition from sector 8192, ...; clusters of 32768 B (<n>); 2 FATs
  of <n> sectors; the data area from sector <n>; <n> sectors to write`,
  then `[format] done in <s> s: ...` or `[format] NOT FORMATTED (<why>):
  <what was written>`.
- **How long** (ESTIMATED: the device check measures it): it writes about
  4 MB and the FATs, 8 bytes a cluster: a "64 GB" card 20 MB, 10-20 s at
  1-2 MB/s over the card's SPI; "1 TB" 250 MB, 2-4 minutes; "2 TB" 490 MB,
  4-8 minutes. The rest of the card isn't written: its old data stays in
  clusters FAT32 calls free.

The layouts (`cardmkfs::plan()`, which is `f_mkfs()`'s own arithmetic:
`test_card_mkfs` formats each on ChaN's FatFs as the Core2 builds it and
reads back the same):

| Card (CSD sectors) | Clusters | Reserved | Each FAT | Data area from | Written |
|---|---|---|---|---|---|
| "4 GB" (7,744,512) | 120,752 of 32 KB | 6,302 | 945 | 16,384 | 5.2 MB |
| "64 GB" (124,735,488) | 1,948,352 of 32 KB | 2,316 | 15,226 | 40,960 | 19.9 MB |
| "1 TB" (1,953,525,168) | 30,516,150 of 32 KB | 6,394 | 238,467 | 491,520 | 248.5 MB |

## Errors

The visualizer's codes ([USB-VISUALIZER.md, "Errors"](USB-VISUALIZER.md#errors)),
with the same rate limit: at most 4 `@err` a second, the rest only
counted. An error changes nothing.

| Line | Code | Detail |
|---|---|---|
| `@status` on v0.6.0-v0.8.0 | `@err 7 status` | (verb: not in this firmware) |
| `@count` | `@err 4 count <why>` | `playing`, `library`, `card`, `counting`: refused; the same (but `counting`) as the last line of a count stopped part way |
| `@identify` with no label | `@err 1 identify` | (syntax) |
| `@identify` with a label over 16 bytes | `@err 5 identify` | (range) |
| `@identify` | `@err 4 identify <why>` | `ui`, `screen`, `viz` |
| `@time` with a time that isn't digits | `@err 1 time` | (syntax) |
| `@time` before 2026 or after 2099 | `@err 5 time` | (range) |
| `@time <unix-ms>` | `@err 4 time rtc` | no RTC answered at the boot, or a read of it fails now (a write that fails later is tried again and logged) |
| `@time` on v0.9.0-v0.11.0 | `@err 7 time` | (verb: not in this firmware) |
| `@format` | `@err 4 format <why>` | the question not shown: `ui`, `screen`, `viz`, `busy`, `card`; the listener's Format refused: `card`, `size`, `busy`; the boot's format failed (its last line): `card`, `size`, `write`, `mkfs`, `check`, `files` |
| `@format` on v0.9.0-v0.11.0 | `@err 7 format` | (verb: not in this firmware) |

## A visit, line by line

```
(the port opened with DTR and RTS low: no reset, the music plays on)
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=fat32 size=63864569856 free=? tracks=1284 music=? bat=87 state=playing cid=5eedc0de0a1b2c3d.0000002a clock=ok bt=Alex%27s%20headphones
                                      (FSINFO's count unknown: the page offers a count;
                                       the card is the one whose root is generation 42)
→ @count
← @err 4 count playing                (paused on the board, or from the player)
→ @count
← @count 0
← @count 10
   ...
← @count 100
← @count done free=38214565888
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87 state=paused cid=5eedc0de0a1b2c3d.0000002a clock=ok bt=Alex%27s%20headphones
→ @identify COM5
← @identify ok                        ("This one / COM5" on the board for 5 s, one buzz)
→ @time
← @time 1792332201694                 (0.4 s behind the computer's: nothing to do)

(a board whose card has no /.mstream)
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=fat32 size=31914983424 free=20401094656 tracks=412 music=? bat=100 state=idle cid=- clock=ok bt=-

(a board whose RTC lost its time: its battery ran flat)
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=12 state=idle cid=5eedc0de0a1b2c3d.0000002a clock=unset bt=Alex%27s%20headphones
→ @time 1792332202
← @err 5 time                         (seconds, not ms: 1970-01-21)
→ @time 1792332202123
← @time ok

(a board whose card is exFAT, as it came: the tab offers to format it)
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=exfat size=63864569856 free=? tracks=- music=? bat=87 state=idle cid=- clock=ok bt=-
→ @format
← @format ok                          ("Format this card?" on the board)
← @format no                          (the listener tapped Cancel)
→ @format
← @format ok
← @format 0                           (Format..., then its red button held 2 s: the board restarts)
   (the ROM's and the boot's lines)
← @format 10
   ...
← @format 100
← @format done size=63864569856 free=63843467264 cluster=32768
→ @status
← @status fw=v0.12.0 elf=63ee7a2b card=fat32 size=63864569856 free=63843467264 tracks=0 music=? bat=87 state=idle cid=- clock=ok bt=-

(a v0.11.0 board: no cid=, no clock=, no @format)
→ @status
← @status fw=v0.11.0 elf=5d0c9e41 card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87 state=paused bt=Alex%27s%20headphones
→ @time 1792332202123
← @err 7 time                         (older than 0.12.0: no clock to set)
→ @format
← @err 7 format

(a v0.8.0 board)
→ @status
← @err 7 status                       (older than 0.9.0: send L for the version)
```

## Implementation

- **`lib/core/HostStatus`** (portable, host-tested in `test_host_status`):
  the `@status` line from its facts (`format()`: the order, the cases, the
  255-byte cap, how `bt=` gives way and how the keys after `state=`
  (`cid=`, `clock=`) give way to its 36 bytes, `kBtRoom`),
  `percentEncode()`, the label's rule (`validLabel()`), the FSINFO rule
  (`freeCountValid()`), and when the count's progress lines go out
  (`Progress`).
- **`lib/core/CardRoot`** (`test_card_jobs`' `test_the_root`): the root
  that won the election keeps its card id and generation (`Root::hasRoot`,
  `cardId`, `generation`), also when its tags file then fails: `cid=`.
  The boot log's line (`describe()`) then names that file, the generation
  and why, never "none on the card".
- **`lib/core/WallClock`** (portable, `test_wall_clock`): the clock
  (`Clock`): the boot's rule (`boot()`: the chip, its voltage-low flag, a
  real date, the range), a `@time` (`set()`), the time at any moment of
  the boot (`at()`) and where it came from (`source()`); the range
  (`believable()`), the UTC calendar both ways (`toCivil()`,
  `fromCivil()`: every day from 1970 to 2100 round-trips), the RTC's
  write (`rtcWrites()`, cut at every byte against a model of the chip and
  M5Unified's read), the boot's rule on any reading (`judge()`), the
  registers as one read gives them (`fromRegisters()`, M5Unified's
  decode), and the boot's reading made exact (`refine()`).
- **`lib/core/RtcSync`** (portable, `test_rtc_sync`): the clock against
  the RTC's seconds, a loop pass at a time (`Sync::step()`: read, drop the
  phase, write; the firmware does it and says what came of it): the
  boot's catch (`boot()`, the closest bracket around the chip's tick, the
  phase added, the check against the boot's reading), a `@time`'s write
  on its second (`plan()`, the pass's lateness and its waits, the
  read-back, the attempts) and its phase (the catch after it), the events
  for the log, the phase as NVS holds it (`phaseFrom()`). Tested against a
  model of the chip that restarts its divider at a seconds write, one
  whose divider runs on and one that holds its first tick after a write,
  read from a model of the loop: a `@time` then a
  restart over a grid of the chip's own seconds and the `@time`'s
  fraction (within 30 ms either kind, right on average), a slow loop, a
  late pass, reads lost (every third, a burst over the tick, all), a chip
  that stopped, a tick that disagrees, a restart before the write, after
  the drop and after the write, the monotonic ms over 2^32, writes and
  read-backs refused, the latest `@time` winning, 2099's last second.
- **`lib/core/FreeCount`** (portable, `test_free_count`): the count's
  pieces, the per-piece counts, the pieces to read again, FatFs's window
  over the card's sectors, and one step under the lock (`step()` over a
  `Source`: the card's sectors and FatFs's window).
- **`lib/core/CardMkfs`** (portable, `test_card_mkfs`): the format's
  layout (`plan()`: the partition at 8192, the cluster by the card's
  size, `f_mkfs()`'s own arithmetic for the reserved sectors, the FATs,
  the data area and the clusters, the sectors written; the refusals), the
  MBR (`mbr()`), the `Writer` (what comes before `f_mkfs()`, its writes
  counted, the boot sector and its backup held back to the last), the
  read-back (`check()`), the files after (`finish()` over the platform's
  `Files`), the whole (`run()`), and the request across the restart
  (`Request`: the identity, its magic and CRC; `sameCard()`). Tested on
  ChaN's FatFs as the Core2 builds it (`test/support/FatModel.h`, now
  with ESP-IDF's `FF_MULTI_PARTITION` and `VolToPart[]`): cluster sizes
  from 512 B at 41.9 MB to 32 KB at FatFs's last sector, each layout
  against what `f_mkfs()` itself writes, the MBR's bytes, blank, exFAT,
  GPT and old FAT32 cards formatted and mounted (FSINFO's count against a
  count of the FAT; `/music`, `device.txt`; nothing of the old schemes
  left; the guard's identity changed), a format cut at 14 points never
  mounting as FAT, each step's refusal, the request's every bit.
- **`lib/core/CachedDrive`**: `identityOf()`, the guard's identity rule
  over any card (the format holds the card to it across the restart).
- **`lib/core/HostLink`**: `@status`, `@count` and `@format` are events
  (`Status`, `Count`, `Format`) with no reply of its own, `@identify` one
  (`Identify`) with its label checked, `@time` one (`Time`) with its time
  checked (digits: `parseHostU64()`, `HostLine`; the range), 0 for the
  question; in a session or not, declined or not, and none of them keeps
  a session alive or a decline going. `refuse()` gives the firmware's own
  refusals HostLink's `@err` and rate limit. `test_host_link`'s
  `test_board_questions`, `test_time` and `test_format`.
- **`lib/core/HoldButton`**, **`lib/core/DialogTouch`**,
  **`lib/core/FormatAsk`** and **`lib/core/DialogLayout`** (portable,
  `test_hold_button`): the Dialog's hold mode (a press that lands on the
  button, its fill, the end at `kHoldMs`, a lift or a slide past
  `kSlackPx` that empties it, a dropped touch), which touch answers a
  dialog (`DialogTouch`, `ui/Overlays`' Dialog's own: a press, its
  highlight, a Tap its answer; in the hold mode no tap on the hold
  button, the hint's alert), the dialog's buttons and their hit areas
  (`dialog::buttonAt()`), and the question (`formatask::Question`: each
  step's minute, the second's settle, a step's answers and the second's
  clock from the answering tap's time; `formatask::pass()`, `Ui::loop()`'s
  order: the minute, then the hold dropped or ticked). Tested on the bits
  and then as the Ui routes the glass (the real `DialogTouch` and
  `formatask`; `Ui::onEvent()`'s slow tap and `Ui::route()`'s settle
  modelled, each place in `ui/Ui.cpp` naming the model), a finger on
  the real `TouchRecognizer` every 10 ms pass: a hold formats at 2 s to
  the pass; a lift at 1.98 s, a quick tap, a slide onto Cancel or up to
  the body don't (the hint); jitter inside the recogniser's slop and
  25 px drags on the button do; a panel that reads the finger lifted
  for a pass every 0.4-1.9 s never formats in 20 s; the dark taking the
  touch drops it; 300 runs of random taps over both dialogs for two
  minutes each (quick and up to 1.98 s down) never format; the minute,
  each step's own, wins over a hold begun a second before it.
- **`src/app/HostQuery`**: answers the five. `@status` from main.cpp's
  facts hook, `storage/CardSpace`'s free count and `app/BoardClock`'s
  `ok()`; `@count` started, stepped every loop pass and ended, with its
  lines; `@identify` through main.cpp's hook; `@time` through
  `app/BoardClock`; `@format` through main.cpp's hook (`@format ok`).
  `app/UsbViz` hands it the events.
- **`src/storage/CardFormatter`**: the request in RTC slow memory
  (`ask()`, a word at a time), and the boot's format (`runIfAsked()`):
  the SD driver's disk functions behind the `Writer` for its time
  (`ff_diskio_register()`), `f_mkfs()` with `VolToPart[]`'s partition 1,
  its 64 KB work area in PSRAM, the files through FatFs (its `FATFS` and
  `FIL` in PSRAM), the lines and the log.
- **`src/app/BoardClock`**: the RTC read at the boot (`begin()`, after
  `M5.begin()`, which found the chip) with the kept phase (NVS
  `clock`/`phase`); a `@time` (`set()`: a read of the chip, then the
  clock and the system time at once, the write planned); `loop()`, every
  pass, `RtcSync`'s steps (the registers read in one run, the phase
  dropped and kept, `rtcWrites()`' three through M5Unified's driver) and
  their `[clock]` lines; the clock's words behind a spinlock, so any task
  reads `now()`; the RTC itself only on the loop task (its I2C port is
  the AXP192's and the touch panel's).
- **`src/storage/CardSpace`**: the volume's `FATFS`, found once after the
  mount (a FatFs directory of the root names it: no sector read), FatFs's
  type, cluster size and count read as words, and the count's driver (the
  lock, the reads through the disk functions, the window, the commit).
- **`src/storage/SectorDisk`**: `watchWrites()`, the count's write watch,
  called in the wrapper's write under FatFs's lock; `mountIdentity()`,
  the mounted card's identity for the format.
- **`src/storage/LocalStorage`**: `cardspace::begin()` after the mount; a
  card that didn't mount keeps whether it answered, its CSD size and its
  identity (`card=unreadable` against `none`, `size=` for an exFAT card,
  the format's card); `formatIfAsked()`, the boot's format, before
  `begin()`.
- **`src/ui/Ui`**: `identify()`, the banner, drawn through `ui/Gfx` over
  everything (the list band's hardware scroll mapped), then the UI's own
  drawing gated as while the screen is dark (`gfx::setDark()`), the list's
  frames held, the dancer stopped; at its end `redrawAll()`, the wake's.
  A touch ends it (swallowed); the screen going dark or another screen
  taking the display end it too. `ui/Input::identifyBuzz()`. Its texts:
  `UiText.h` (`kIdentifyTitle` and its room), measured in
  `test_ui_library`. `askFormat()`, the format's two questions (Ui's own
  dialogs, `OwnDialog::FormatAsk` and `FormatSure`: the second's red
  button on the left, `Dialog::open()`'s `primary`, in the hold mode
  (`Dialog::open()`'s `holdHint`, drawn from the start, amber once a
  press fell short; the fill drawn through the shared
  strip, its button alone, every 40 ms at most; `Dialog::tick()` every
  loop pass; `Input::noHold()` for the touch that began it, so it stays
  a press and says its moves; the hold dropped when the touch is no
  longer the dialog's); its settle and the minute (`formatask`); Cancel
  for anything else); the no-card pages' Format...
  (`ui/EmptyState`); the boot screen's bar (`ui/BootScreen::formatting()`,
  `BootLayout`'s rows, `test_boot_logo`). Their texts, in the ten
  languages: `format.*` (`i18n/`; `test_ui_strings` measures each room).
- **`src/main.cpp`**: `boardclock::begin()` early in `setup()`,
  `boardclock::loop()` every pass; the facts hook (`hostFacts()`; `cid=` from
  `Library::root()`, which `setup()` reads before the loop answers
  anything, and nothing writes again; `-` once the card's guard has found
  another card, `sectordisk::guard().foreign`), what blocks a count
  (`countBlocked()`), the identify hook (the screen woken first, the
  banner once the panel is awake), `hostQuery.loop()` every pass, the
  screen kept lit for the banner, the idle power-off held while a count
  runs; the format's hook (`askFormatQuestion()`), its Format
  (`MainUiHost::formatCard()`: the card's identity, the pause, the saves,
  the headphones let go; `stepFormatRestart()`: the request, then
  `restartNow()`), a question gone (`formatDeclined()`: `@format no`),
  the boot's format before the storage's mount (the boot screen's bar),
  and its note at the first screen.
- **Cost**, 9899eb4 against aed9014 (v0.8.0 and the 0.9.0 version
  bump), both builds: IRAM unchanged (`.iram0.text` 124,883 B); internal
  DRAM +224 B (`.dram0.data` +16, `.dram0.bss` +208: the count's state,
  `HostQuery`'s hooks, the pending label); flash `.text` +7,024 B and
  `.rodata` +1,336 B; `firmware.bin` +9,200 B (core2 2,633,728 to
  2,642,928; core2-dio 2,633,760 to 2,642,960), the app 42 % of its slot.
  Every guard passes in both (`cache_guard`, `flash_guard`, the version's). While a count
  runs, 32 KB and its table in PSRAM; `@status` takes about 0.6 KB of the
  loop task's stack for its line (ESTIMATED: the console's stack line
  after it, `[console] ...`, isn't printed for an `@` line).
  **`cid=`** (0.12.0), against 12a748e, core2: IRAM and internal DRAM
  unchanged (`.iram0.text` 124,883 B); flash `.text` +320 B, `.rodata`
  -32 B; `firmware.bin` +288 B (4,358,528 to 4,358,816). Its review's
  fixes (the guard's flag read, the boot log's line for a root whose tags
  file failed) add 144 B more (4,358,960), IRAM still 124,883 B. Every
  guard passes. `@status` takes about 50 bytes more of the loop task's
  stack (ESTIMATED: the key's 31-byte buffer and its lengths).
  **`@time` and `clock=`** (0.12.0), against 8db6c24, core2: IRAM
  unchanged (`.iram0.text` 124,883 B); internal DRAM +40 B (`.dram0.data`
  +16, `.dram0.bss` +24: the clock's words and its spinlock); flash
  `.text` +3,268 B (the calendar, the RTC's write and read-back, the
  verb), `.rodata` +1,008 B; `firmware.bin` +4,576 B (4,358,944 to
  4,363,520). Every guard passes. The RTC is read once at the boot and
  written once a `@time` (a few I2C transactions, about a millisecond:
  ESTIMATED), on the loop task. Its review's fixes (the nearest second,
  the three writes, the year read first) add `.text` 228 B and `.rodata`
  64 B: `firmware.bin` +256 B (4,363,776), IRAM and internal DRAM
  unchanged; the write is 15 bytes in three transactions where it was 7
  in one.
  **`@format`** (0.12.0), against 015a3c5, core2: IRAM unchanged
  (`.iram0.text` 124,883 B); internal DRAM +120 B (`.dram0.data` +32,
  `.dram0.bss` +88: the format's restart and its outcome); RTC slow memory
  28 B (`.rtc_noinit`: the request); flash `.text` +8,376 B, `.rodata`
  +11,196 B (the format's texts in ten languages, its log lines);
  `firmware.bin` +20,752 B (4,377,344 to 4,398,096), the app 70 % of its
  slot. `f_mkfs()` was linked already (the SD library's
  `format_if_empty`, never used), so it adds nothing. Every guard passes.
  While the boot's format runs: 64 KB of PSRAM for its work area and
  about 8.5 KB for FatFs's `FATFS` and `FIL`; nothing else runs then.
  **The second step's hold** (0.12.0), core2, against the image that
  left out a card the player reads (d8e797b, `firmware.bin` 4,395,792):
  that card's question back (its texts in ten languages and the mounted
  card's branch: the tree as it was at 64577ec, 4,398,096) and the hold
  (`HoldButton`, `FormatAsk`, the Dialog's hold mode, `format.hold_hint`
  in ten languages): IRAM, internal DRAM and RTC memory unchanged
  (`.iram0.text` 124,883 B); flash `.text` +1,772 B, `.rodata` +2,912 B;
  `firmware.bin` +4,848 B (4,400,640), of which the hold's is 2,544 B;
  the app 70 % of its slot. Every guard passes. While a finger holds
  the red button (131 x 36 px, 9.4 KB), it is drawn through the shared
  strip and pushed about 25 times a second, 2 s at most. **The hold's
  review fixes**, core2, against that image (4,400,640): the dialog's
  touch and the loop's order moved into `lib/core` (`DialogTouch`,
  `formatask::pass()`, `Question::answered()`) +176 B; "Hold to erase"
  in ten languages and the hint drawn from the start +80 B (`.rodata`);
  `firmware.bin` 4,400,896 (+256 B; core2-dio 4,400,960). IRAM
  (`.iram0.text` 124,883 B), internal DRAM and RTC memory unchanged
  (57,928 B of `.dram0.data` and `.bss`, as before). Every guard passes.
  **The clock's catch and its write on a second** (0.12.0), core2,
  against d8855d7 (both built from the source alone, so with the same
  version string): IRAM unchanged (`.iram0.text` 124,883 B); internal
  DRAM +240 B (`.dram0.data`: `RtcSync`'s state, which starts with the
  phase's "none"; `.bss` unchanged); RTC memory unchanged; flash `.text`
  +4,056 B, `.rodata` +1,452 B (its log lines); `firmware.bin` +6,016 B
  (4,401,520 to 4,407,536 in that pair; 4,406,928 built in the repository
  with git's version, the image with the hold's room below), the app 70 %
  of its slot. Every guard passes. While a catch runs (about a second
  from the loop's start after a boot, up to two after a `@time`; 3.3 s at
  most), the RTC's seven time registers are read every 10 ms or more, one
  7-byte read each (about 0.25 ms of the bus at 400 kHz: ESTIMATED);
  nothing otherwise. A `@time` adds two NVS writes (the phase removed,
  then kept) and the RTC's write moves to the loop.

## Device test plan

On a Core2 with the user's card, from a script that opens the port with
DTR and RTS low (pyserial: `dtr = False` and `rts = False` on an unopened
`serial.Serial()`, then `open()`, as `tools/usb_viz.py` does) and writes
each line whole with its `\n`. Close every other owner of the port first.

1. **No reset.** Open the port: no ROM line, no banner. The screen and any
   music carry on.
2. **@status, playing.** Play a track on the board, then `@status`. One
   line at once: `fw=` the build's version (as `L` and the boot banner
   say), `elf=` About's, `card=fat32`, `size=` About's GB × 10^9 within a
   rounding, `free=` bytes or `?` (the boot log's `[storage] the card's
   volume: ... its free count: known (FSINFO)` or `unknown (@count)` says
   which), `tracks=` the Library row's count, `music=?`, `bat=` near the
   tab bar's, `state=playing`, `cid=` (step 9), `bt=` the headphones' name
   encoded (or `-`). The music doesn't skip. Every second for a minute:
   the same, no skip, no `[console]` line.
3. **@count refused.** While it plays: `@count` gives `@err 4 count
   playing`, and `@status` still says the same `free=`.
4. **@count.** Pause, `@count`: `@count 0` at once, a line about every
   tenth, `@count 100`, `@count done free=<bytes>`, and the log's `[host]
   count: <bytes> B free (<n> clusters of <n> B) in <s> s; ...`: note the
   time. The UI stays usable meanwhile (scroll a list). `@status` during
   it: `free=counting`; after it: `free=` the counted bytes. Compare with
   the card's free space on a computer (Windows: Properties, in bytes):
   equal, less what the board wrote since (`/.player`).
5. **Stopped part way.** `@count` again, and press play at 30-50 %: the
   next line is `@err 4 count playing`, the log says `stopped at ... (playing);
   nothing changed`, and `@status` keeps the earlier `free=`.
6. **FSINFO.** After step 4, let the board save something (skip a track:
   the queue's save), restart it (an RTS pulse, or off and on), and
   `@status`: `free=` known at once (the boot log's `known (FSINFO)`).
7. **@identify.** `@identify COM3`: `@identify ok`, the banner "This one /
   COM3" in the blue frame, one buzz (with haptics on: `as` shows them),
   for 5 s, then the page exactly as before (try it over an open sheet,
   and on the Library with a scrolled list). With music playing: no skip.
   Again, and tap the glass after 2 s: the banner goes, the tap does
   nothing else. With the screen off: it wakes and shows the banner.
   `@identify` alone: `@err 1 identify`; `@identify cu.usbserial-14130`:
   `@err 5 identify`.
8. **Old firmware.** On a v0.8.0 board: `@status` gives `@err 7 status`.
9. **cid=** (0.12.0). With a card a computer filled (a `/.mstream` root):
   `cid=` the root's card id and generation, the generation the one the
   boot log's `[lib] the card: T tags-<8 hex>.bin, generation <n>` names
   (`<n>` in decimal there, 8 hex digits here). With a card that has no
   `/.mstream`: `cid=-`. Pull the card while on: `card=none` and `cid=-`;
   put it back: the boot's `cid=` again. Pull it and put in another card
   while it plays from the card and `@status` goes every second: from the
   log's `[card] ANOTHER CARD` line on, `cid=-` until the restart, then
   the other card's. A root whose tags file is gone (renamed on a
   computer): `cid=` still that root's, and the boot log's line `no
   transfer data (the root's T tags-<8 hex>.bin, generation <n>:
   missing)`. Every line still ends in `bt=`, whole for a usual name.
10. **The clock** (0.12.0). The computer's time in ms comes from Python's
    `int(time.time() * 1000)`; note it beside each answer.
    - **The boot's line.** After a restart, the log's `[clock]` line:
      `... UTC, from the RTC (to the second; to a few ms at its next
      tick)` or `unset: ...` and why (a board fresh from the factory may
      hold a time of M5's, or count from 2000); with a time, within about
      a second of the loop's start `[clock] ... UTC at the RTC's tick,
      within <n> ms (...); the boot's second was <s> s behind`, `<n>`
      under 15. `@status`'s `clock=` agrees (`ok` or `unset`), and so does
      `@time` (a time, or `@time unset`).
    - **Set.** `@time <the computer's ms>`: `@time ok` at once, and the
      log's `[clock] ..., set by a computer's @time (it was unset); the
      RTC at its next second` or `(it was <n> s behind/ahead)`; then
      within two seconds `[clock] the RTC set to <the next second> UTC on
      its second (written <l> ms after it); its seconds begin <p> ms from
      the true ones (within <n> ms): kept for the boots`. Note `<l>` and
      `<p>`: `<p>` within about 15 ms of `<l>` says the BM8563 restarts
      its divider at a write; anything else (up to a second either way),
      that it keeps its own. Either is right. `@status`: `clock=ok`.
      `@time`: within about 50 ms of the computer's (note the offset: A).
      With music playing: no skip.
    - **Refused.** `@time 1792332202` (seconds): `@err 5 time`;
      `@time 2026-10-18`: `@err 1 time`; `@time 4102444800000`: `@err 5
      time`. `@time` still says the time set above.
    - **Restart.** An RTS pulse (or the power button): `[clock] <the time>
      UTC, from the RTC (to the second; ...)`, then `[clock] ... at the
      RTC's tick, within <n> ms (its seconds <p> ms from the true ones,
      the last @time's)`, `<p>` the set's. Then `@time`: **the offset
      against the computer's within about 50 ms of A** (restart four
      times or more and note each: the previous image read 0.4-1.3 s
      behind). A `@time <the computer's ms>` then says `(it was 0.0<n> s
      behind/ahead)`, the same. `unset: the RTC says 19..` here would mean
      the write's last step didn't take: a bug; `the RTC's tick not
      caught`, a bug too (or a bus in trouble: note the reason).
    - **Restart in the write's second.** `@time <ms>` with the ms part
      near 000 (the write a second later), and an RTS pulse within that
      second: the boot reads the RTC as before the `@time` (its line says
      the older time, its catch the older phase). Again with the pulse
      about 1.5 s after the `@time` (the write done, its tick maybe not
      caught): `the RTC's tick ... (no phase kept: its seconds taken as
      the true ones)` when it wasn't, and the offset within the write's
      lateness of A (or within a second, for a chip that keeps its own
      seconds) until the next `@time`.
    - **Off** (the RTC keeps time, ESTIMATED until this). On battery,
      off (a long press on the power button, or the idle power-off) for an
      hour at least, overnight better, then on: `from the RTC`, and `@time`
      against the computer's; note the difference and how long it was off
      (the drift, a second or two a day expected). Then a set again:
      the log's `(it was <n> s ...)` says the same.
    - **Flat** (only if it happens anyway: don't drain a battery for it).
      A boot after the battery ran flat: `unset: the RTC lost its time
      (its voltage-low flag ...)`, `clock=unset`, until a `@time`.
    - **Old firmware.** On a v0.11.0 board: `@time <ms>` gives `@err 7
      time`.
11. **@format** (0.12.0). **On the spare card only, never the music
    card**: take the music card out first. Have a computer with a card
    reader at hand to look at the spare after each format. The second
    question's red button, "Hold to erase", answers only to a hold:
    "hold the red button" below is a finger kept on it until it has
    filled (2 s), then lifted.
    - **Declined.** With any card in: `@format` gives `@format ok` and the
      question on the board. Tap Cancel: `@format no` and the log's
      `[format] the question went (Cancel): nothing formatted`. Again, and
      tap a tab: `@format no` (`closed ...`). Again, and wait a minute:
      `@format no` (`no answer in a minute`). Again: Format..., then
      Cancel on the second question: `@format no`. Again: double-tap
      Format... quickly: the second question stays up, the log's `a touch
      right after the second question opened, ignored`; Cancel. Nothing
      on the card changed (the computer shows it as before).
    - **The hold, not taken.** Again: Format..., and on the second
      question ("Hold to erase" on the left, empty, outlined in red; under
      the body "Hold the red button for 2 seconds", in the body's grey;
      in each language the label and the hint fit, uncut):
      - Tap the red button: nothing but the inert buzz and the hint
        turning amber; the log's `[ui] format: Format let go after <n>
        ms: nothing formatted (the hint amber)`.
        Tap it five times quickly, then double-tap it: the same each
        time, the question still up.
      - Hold it about a second and let go: it fills about half way and
        empties (`let go after ~1000 ms`). From the console, a scripted
        finger resting on it 1.88 s, `uid91,194,91,194,1700`: it fills
        nearly whole and empties (`let go after ~1880 ms`).
      - Hold it a second, slide onto Cancel and lift there: it empties as
        the finger leaves it (`slid off`), and that lift is no Cancel:
        the question stays.
      - Rest a finger on it and roll it a little, as a resting finger
        does: it goes on filling; let go before it is full.
      - With a finger filling it, press C on the strip with another:
        C does nothing (`[button] ignored: C ...: a second finger`) and
        the fill goes on; let go before it is full. With no finger on
        the glass, B and C do as ever (play/pause, next) under the
        question, which stays.
      - Cancel: `@format no`. Again: Format..., wait about 58 s on the
        second question, then hold the red button: at its minute the
        question goes under the finger (the log's `the minute ran out
        while Format was held`), `@format no`, nothing formatted.
      Nothing on the card changed.
    - **exFAT.** Format the spare exFAT on the computer (or use it as it
      came). The board: "This card is exFAT" with Try again and
      Format...; Format..., then hold the red button: it fills, the
      double tick at its end (`[ui] format: Format held 2000 ms`, or a
      few ms more: the loop pass that saw it), the note
      "Restarting to format the card", `@format 0`, the restart, the boot
      screen's "Formatting the card" and its bar, `@format 10` ...
      `@format 100`, `@format done size=<bytes> free=<bytes>
      cluster=32768`, the log's `[format] done in <s> s` (note the time
      and the card's size: the estimate above), then "Card formatted:
      ready for your music" and "No music found". `@status`:
      `card=fat32`, `free=` the done line's, `cid=-`. On the computer:
      one MBR partition, FAT32, 32 KB allocation units (Windows: the
      drive's Properties; `chkdsk X:` read-only finds no errors),
      `/music` empty, `/.player/device.txt` with `history=1`; a file
      copied on and off. The music doesn't skip afterwards (play a track
      copied to `/music` and listen for a minute).
    - **GPT.** Erase the spare on a Mac with "GUID Partition Map" (or
      `diskpart` `convert gpt` on Windows): "This card uses GPT", then as
      above; on the computer the card is MBR now (`diskpart` `list disk`:
      no `*` under Gpt), one partition.
    - **A card the player reads.** With the formatted spare in and a
      file in `/music`: `@format` (the board can't be asked otherwise):
      "Everything on it is erased: your music, ..."; Format..., hold the
      red button: as above, and the file is gone.
    - **Pulled.** `@format` again, Format..., hold the red button, and
      pull the card at about 50 %: `@err 4 format write`, the log's `NOT
      FORMATTED`, "Couldn't format the card: try again", and the card
      reads "Can't read this card". Put it back, Try again: the same.
      Format...: the damaged card's "Check it on a computer first";
      Format..., hold the red button: whole again.
    - **Playing.** A track playing on the speaker (and once on the
      headphones): `@format`, Format..., hold the red button: paused
      first, no pop, the headphones let go, then the restart.
    - **Old firmware.** On a v0.11.0 board: `@format` gives `@err 7
      format`.
