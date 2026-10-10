# Host status

The mStream terminal player (a Rust desktop app) shows each plugged-in
Core2 on its MP3 Player tab: whether its firmware is up to date, its
microSD card's size and free space, its library, its battery, and a way to
tell two identical boards apart. It asks the **running firmware**, over
the USB serial port, without resetting the board (the bootloader probe it
used before resets it, stops the music and can't see the card). Three
questions, on the USB visualizer's `@` lines ([USB-VISUALIZER.md](USB-VISUALIZER.md)):

| Line | Answer |
|---|---|
| `@status` | at once, one line: `@status fw=... elf=... card=... size=... free=... tracks=... music=... bat=... state=... bt=...` |
| `@count` | one count of the card's free clusters, on request: `@count 0`, `@count <percent>` about every tenth, `@count 100`, `@count done free=<bytes>` |
| `@identify <label>` | "This one" and the label on the board's screen for 5 s, one buzz: `@identify ok` |

Status: **built** (October 2026), in the release after v0.8.0 (0.9.0):
the host tests below pass, and both firmware builds pass their guards. The
device checks are the [plan](#device-test-plan) at the end. The design is
the MP3 Player tab's set in the mStream repository
(`docs/designs/mp3-tab`, cards 01 section 5 and 05: alternate A).

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
- **No session.** The three are answered with or without a visualizer
  session (`@hello`), and while the visualizer is declined (the listener
  ended it on the Core2): they drive nothing on the board's screen but the
  identify banner. They don't keep a session alive, and they don't count as
  the computer talking for a decline's quiet, so a player that asks for
  `@status` every second can still start the dancer once its own user asks.
- **Older firmware.** A verb a firmware doesn't know gets `@err 7 <verb>`:
  v0.6.0 to v0.8.0 answer `@status` with `@err 7 status`, which says "this
  firmware is older than 0.9.0". The player then sends the console's `L`
  for the version (on its own, not with an Enter in the same write: right
  after a boot the console's Sync drops a key sent with its Enter) and says
  the card needs a firmware update. A board that answers `@status` is new
  enough for all three. No answer to either within about a second: not
  this firmware running (another firmware, or none booted); only a reset
  can say more.

## @status

```
→ @status
← @status fw=v0.9.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87 state=playing bt=Paul%27s%20headphones
```

One line, answered in the loop pass that reads the question, **never a
read of the card**: every field is something the firmware holds already.
The fields come in this order, each `key=value`, one space between them; a
value never holds a space. `?` is unknown, `-` is none. **A reader must
ignore keys it doesn't know**: a field added later goes in before `bt=`,
which stays last (it is the one that gives way when the line runs long).

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
| `bt` | the paired headphones' name, percent-encoded, or `-` | their own name once linked, before that the one saved at the pairing (`BtSink::shownName()`, the Output tab's); `-` with nothing paired, or a pairing whose name the board never learnt |

**Percent-encoding** (`bt=`): RFC 3986's unreserved bytes (`A-Z a-z 0-9 -
. _ ~`) as they are, every other byte of the name's UTF-8 as `%XX` in
upper-case hex: `Paul's headphones` is `Paul%27s%20headphones`. Any URL
decoder reads it back to the same bytes.

**At most 255 bytes**, as any host line. The fields before `bt=` take 219
bytes at their widest (a 48-byte version, every number at 20 digits), so
`bt=` keeps 36 at least; the line above leaves it 135, a dev build's on a
1 TB card about 110, so a name of up to 63 bytes (all BtSink keeps) fits
whole unless most of it needs encoding. A name
that doesn't fit is cut at a character (never inside a UTF-8 sequence or a
`%XX`) and ends with `%E2%80%A6`, "…".

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

## A visit, line by line

```
(the port opened with DTR and RTS low: no reset, the music plays on)
→ @status
← @status fw=v0.9.0 elf=63ee7a2b card=fat32 size=63864569856 free=? tracks=1284 music=? bat=87 state=playing bt=Paul%27s%20headphones
                                      (FSINFO's count unknown: the page offers a count)
→ @count
← @err 4 count playing                (paused on the board, or from the player)
→ @count
← @count 0
← @count 10
   ...
← @count 100
← @count done free=38214565888
→ @status
← @status fw=v0.9.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87 state=paused bt=Paul%27s%20headphones
→ @identify COM5
← @identify ok                        ("This one / COM5" on the board for 5 s, one buzz)

(a v0.8.0 board)
→ @status
← @err 7 status                       (older than 0.9.0: send L for the version)
```

## Implementation

- **`lib/core/HostStatus`** (portable, host-tested in `test_host_status`):
  the `@status` line from its facts (`format()`: the order, the cases, the
  255-byte cap and how `bt=` gives way), `percentEncode()`, the label's
  rule (`validLabel()`), the FSINFO rule (`freeCountValid()`), and when
  the count's progress lines go out (`Progress`).
- **`lib/core/FreeCount`** (portable, `test_free_count`): the count's
  pieces, the per-piece counts, the pieces to read again, FatFs's window
  over the card's sectors, and one step under the lock (`step()` over a
  `Source`: the card's sectors and FatFs's window).
- **`lib/core/HostLink`**: `@status` and `@count` are events (`Status`,
  `Count`) with no reply of its own, `@identify` one (`Identify`) with its
  label checked; in a session or not, declined or not, and none of them
  keeps a session alive or a decline going. `refuse()` gives the
  firmware's own refusals HostLink's `@err` and rate limit.
  `test_host_link`'s `test_board_questions`.
- **`src/app/HostQuery`**: answers the three. `@status` from main.cpp's
  facts hook and `storage/CardSpace`'s free count; `@count` started,
  stepped every loop pass and ended, with its lines; `@identify` through
  main.cpp's hook. `app/UsbViz` hands it the events.
- **`src/storage/CardSpace`**: the volume's `FATFS`, found once after the
  mount (a FatFs directory of the root names it: no sector read), FatFs's
  type, cluster size and count read as words, and the count's driver (the
  lock, the reads through the disk functions, the window, the commit).
- **`src/storage/SectorDisk`**: `watchWrites()`, the count's write watch,
  called in the wrapper's write under FatFs's lock.
- **`src/storage/LocalStorage`**: `cardspace::begin()` after the mount; a
  card that didn't mount keeps whether it answered and its CSD size
  (`card=unreadable` against `none`, `size=` for an exFAT card).
- **`src/ui/Ui`**: `identify()`, the banner, drawn through `ui/Gfx` over
  everything (the list band's hardware scroll mapped), then the UI's own
  drawing gated as while the screen is dark (`gfx::setDark()`), the list's
  frames held, the dancer stopped; at its end `redrawAll()`, the wake's.
  A touch ends it (swallowed); the screen going dark or another screen
  taking the display end it too. `ui/Input::identifyBuzz()`. Its texts:
  `UiText.h` (`kIdentifyTitle` and its room), measured in
  `test_ui_library`.
- **`src/main.cpp`**: the facts hook (`hostFacts()`), what blocks a count
  (`countBlocked()`), the identify hook (the screen woken first, the
  banner once the panel is awake), `hostQuery.loop()` every pass, the
  screen kept lit for the banner, the idle power-off held while a count
  runs.
- **Cost**, against aed9014 (v0.8.0 and the 0.9.0 version bump), both
  builds: IRAM unchanged (`.iram0.text` 124,883 B); internal DRAM +224 B
  (`.dram0.data` +16, `.dram0.bss` +208: the count's state, `HostQuery`'s
  hooks, the pending label); flash `.text` +7,024 B and `.rodata` +1,352
  B; `firmware.bin` +9,216 B (core2 2,633,728 to 2,642,944; core2-dio
  2,633,760 to 2,642,976), the app 42 % of its slot. Every guard passes
  in both (`cache_guard`, `flash_guard`, the version's). While a count
  runs, 32 KB and its table in PSRAM; `@status` takes about 0.6 KB of the
  loop task's stack for its line (ESTIMATED: the console's stack line
  after it, `[console] ...`, isn't printed for an `@` line).

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
   tab bar's, `state=playing`, `bt=` the headphones' name encoded (or
   `-`). The music doesn't skip. Every second for a minute: the same, no
   skip, no `[console]` line.
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
