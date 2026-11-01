**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.11.0

- **Auto DJ keeps the music going.** When the queue's last track starts,
  it adds one more, picked on the player itself (no network, no server),
  so the music goes on without a gap. With an Auto DJ index on the card
  it picks one that sounds like what has been playing; no card has one
  yet (the index needs transfer software that isn't out: below), and
  until then it picks at random from the library. It never starts music
  by itself, and never changes the output or the volume. It is off until
  you switch it on: in Now Playing's **"..."** menu, whose new fourth
  row, Auto DJ, reads On, Off, Waiting or Paused (a tap switches it), or
  in **Output > Auto DJ**. It stays on after a restart and waits there
  for Play; switched on with nothing queued, it follows the next track
  you play.
- **Its matching needs an Auto DJ index on the card**
  (`/.mstream/autodj-<gen>.bin`), which the transfer software is to
  write from mStream's analysis of the audio. **The transfer software
  isn't available yet,** so no card has an index today, and Auto DJ
  picks at random (the next point). From 0.11.0 the player says on the
  card that it reads the index (`read.mpdj=2` in `/.player/device.txt`,
  checked at each start): that is how the transfer software will know to
  write one. Once it is out, start the player once with the card in
  before the first transfer. With an index, the tracks it doesn't cover
  (files you copied yourself, tracks mStream hasn't analysed) play as
  usual, but Auto DJ doesn't pick them and they don't steer it. The
  index's format, for any transfer software, is `docs/AUTODJ-INDEX.md` in
  the source, and `docs/AUTODJ.md` describes Auto DJ as built.
- **Without an index** (a card filled by hand: every card, for now),
  Auto DJ still keeps the queue going, with random tracks from the
  library: never the one playing and, where it can, none of the last 50
  tracks. It says so once ("No Auto DJ index: picks are random", when it
  is switched on, or at its first pick after a start). Output > Auto DJ
  says "No Auto DJ index on this card" over "picks are random"; Anchor,
  Match strictness, BPM continuity and Harmonic mixing are dimmed, and a
  tap on one says "Needs an Auto DJ index: a transfer to the card adds
  one". The empty Queue and Now Playing keep Shuffle all, and a long
  press has no "Auto DJ from here".
- **Output > Auto DJ** ("keeps the queue going") has the switch;
  **Anchor**, Follow the vibe (each pick close to one of the last five
  tracks that played: the default) or Stay on seed (every pick close to
  the first track, the seed); **Match strictness**, from "55% or closer"
  (the default, the most variety) to "75% or closer" by 5, a tap for the
  next step; **BPM continuity** (within 8 BPM, or at half or double the
  tempo) and **Harmonic mixing** (keys that mix well), **both off by
  default**; and a status line: how many of the library's tracks have
  neighbours, and how many of those have a BPM and a key, or why Auto DJ
  has no index, or why it waits ("Waiting: Repeat One is on"). The names
  are the mobile app's.
- **What steers it,** with an index: every track that plays, the ones
  you add (Play next, + Queue) as well as its own, except one you skip in
  its first 30 s. Each Play (a new queue) starts it afresh from the track
  it plays. Where it can, it avoids the last 50 tracks and the last 3
  artists. It draws at random among the tracks that are close enough, as
  the mobile app does, rather than always taking the closest; when none
  is, it draws from the 50 closest; and when every track it could draw
  from played lately, from the 50 closest that didn't, before it repeats
  one.
- **An empty queue offers Auto DJ:** on a card with an Auto DJ index,
  the empty Queue and the empty Now Playing show **Open Library** and
  **Auto DJ** in Shuffle all's place. A tap plays a random track that has
  neighbours and switches Auto DJ on ("Auto DJ: Tidewater", with Undo).
  Without an index, Shuffle all stays. The play/pause button still does
  nothing on an empty queue.
- **Auto DJ from here:** a long press on a track the index knows has it
  as its first row, with Play, Play next and + Queue in their places
  under it. It plays the track as a new queue, Auto DJ on and starting
  from it ("Auto DJ from here: Glass Harbour", with Undo). The Undo of
  either puts the queue back and, if that tap switched Auto DJ on,
  switches it off again.
- **In the Queue,** a track Auto DJ added has a small DJ mark at its
  right (on the last 64 it added; the marks aren't kept across a
  restart). Its additions don't flash the Queue tab's badge, and don't
  take your Undo: the Undo of your last change still works after Auto DJ
  adds a track.
- **At the end of the queue:** if the queue had already stopped at its
  end (Auto DJ switched on after an album finished, or the sleep timer's
  End of queue stopped it there), Auto DJ gets the next track ready,
  paused at 0:00, and says "Auto DJ: the next track is ready, press
  play". Nothing plays until you do (the headphones' play button doesn't
  start it), so after a night's End of queue, Play starts a new track.
  Repeat All doesn't come round to the first track while Auto DJ adds.
  Repeat One and the sleep timer's End of queue make it wait (Waiting),
  and three of its tracks in a row that fail to play pause it (Paused,
  with a note) until a track plays or it is switched off and on.
- **What it costs:** with an index, switching Auto DJ on matches the
  index to the library in the background, about 1.5 s for 19,412 tracks
  (3 s while an MP3 plays): a Play doesn't wait for it, only the first
  pick does. The match holds about 155 KB of PSRAM at that size while
  Auto DJ is on, and is given back when it is switched off; Auto DJ's
  own 19 KB are held from the start, on or off. The match is made again
  after each library update (about 6 s with an MP3 playing). A pick
  takes a millisecond or two and reads at most two 512-byte pieces of
  the card, usually none.
- **In the ten languages:** Auto DJ's 51 new texts are in all ten, with
  the mobile app's words where it has them (German's "Anker", "Der
  Stimmung folgen", "Ähnlichkeitsschwelle"), and translated by an AI
  agent (Anthropic's Claude Fable) otherwise: machine translations, not
  yet read by a native speaker, like the rest of the nine languages.
  Each was fitted to its place in every language: where Match
  strictness's value doesn't fit beside its title (German, Spanish,
  Italian and Portuguese), it goes on the line under it.
- Tested on a Core2 in three rounds before the release (2026-10-13 to
  16), on a synthetic card of 19,412 tracks with made-up names and an
  Auto DJ index made by the repository's test tools: a two-hour run of
  7,520 picks with no reset and no underrun; picks with each setting, at
  240 and 160 MHz; library updates during a session; the card pulled and
  put back, and its index replaced on a computer; damaged, broken and
  missing indexes; the end of the queue; Undo from paused, stopped and
  playing queues; and the screens in all ten languages. What those
  rounds turned up was fixed first and checked again on the device:
  picks that took 5-9 ms (now 0.4-1.7 ms), the queue's saves falling
  behind over a long session, a library update put off to the next start
  for the match's memory, Stay on seed at 75 % looping the few tracks
  close enough to a seed, the match's time and the memory it left free
  for a moment, Japanese and Chinese messages on two lines, and the Undo
  below. The release build then ran on a card of 113 tracks with no
  index: the update from 0.10.0, Auto DJ's random picks and its screens
  without an index, and the library's work under music.
- **Undo never starts the music after a silence.** The Undo of a Play
  (the Library's Play, Play all, Shuffle all, Auto DJ's) or of a Clear
  never starts what was silent when you made it: a queue that was paused
  comes back paused, at the start of its track, and one that was stopped
  or had finished stays stopped. Undoing a Play made over a playing queue
  plays the old queue's track again from the start. 0.10.0 started the
  old queue's track playing when a Library Play made over a paused,
  stopped or finished queue was undone.
- **Messages on two lines, placed by their letters.** A message with a
  long name puts what happened over the name ("Plays next" over the
  title). The two lines sat at fixed rows, and in Japanese and Chinese,
  whose characters are taller than the Latin letters, the first ran into
  the second and over the message's top edge (seen on Auto DJ's); in
  Russian a "й" touched the line above. Each line now goes by the rows
  its letters take, with a clear row between the two, and the message's
  full height when they need it.
- **Fixed: a list could hold the card's background work while the
  screen was off.** A list reloaded, or a fling stopped, while the screen
  was dark counted as moving until the screen woke, and held the tag
  reading, the library update's save and the rest of the card's
  background work: once over four minutes, for a save. 0.10.0 does this
  too. Now a list that isn't drawn holds nothing: on the synthetic card,
  a library update with the screen off saved 1.8 s after its build. The
  same goes for a list under a dialog that opens by itself (the
  headphones lost, a track that failed to play).
- **Fixed: the serial console's `gs` froze the player's loop.** `gs`
  (the check of the card's tag files) read them in one go, which stopped
  the screen, the queue and everything else on the player's loop for
  12.4 s on the synthetic card. It now reads a piece a pass (about
  10 ms), and takes about 32 s there with the screen lit and 60 s dark
  with playback paused, 52-75 s while tracks play; on a card of 113
  tracks, under a second. It waits for a compaction under way rather
  than read the file that compaction replaces, which could call a good
  file ABSENT; `gt` says it can't read during one.
- **The firmware is a little bigger:** about 4.36 MB, up from 4.27 MB in
  0.10.0, for Auto DJ (69 % of the space it has on the flash, 68 %
  before). The steps below are the same.
- **The update from 0.10.0** keeps the settings, the paired headphones,
  the queue and the library as they are: nothing is rebuilt, and Auto DJ
  starts off, its settings at their defaults. A card that 0.9.0 or 0.8.0
  used last gets 0.10.0's one rebuild of the library from the tags read
  before (about 10 s for 20,000 tracks; no file is read again, and the
  queue is kept), and a unit updated from 0.9.0 or older stays in
  English, every setting kept, until a language is picked in Output >
  Language. A card that 0.7.0 used last gets 0.8.0's first start instead
  (0.8.0's notes say what it does).
- **Going back to 0.10.0 later:** it sees that a newer version saved the
  settings, leaves them as they are and uses every one it knows; Auto
  DJ's switch and settings stay saved, unread, and are as they were back
  on 0.11.0. The tracks Auto DJ added stay in the queue like any others.
  0.10.0 writes its own `device.txt`, which doesn't ask for the Auto DJ
  index (it says `read.mpdj=1`): start 0.11.0 once with the card in
  before a transfer.

## Hardware

- **Tested:** M5Stack Core2 **v1.3** (AXP192 power chip).
- **Untested** (it should work; reports welcome, with a photo of Output >
  About > Device info): the original Core2 (v1.0), the Core2 for AWS, and
  the Core2 v1.1 (AXP2101 power chip).
- Other M5Stack devices (Basic, Fire, Tough, CoreS3) are not supported.

## Install

{{INSTALL_LINE}}

Or with [esptool](https://docs.espressif.com/projects/esptool/)
(`pip install esptool`; add `-p PORT` if it doesn't find the Core2):

- **First install** over other firmware (M5Stack's demo, UIFlow): erase
  first, which also clears what that firmware left behind.

  ```
  esptool --chip esp32 erase-flash
  esptool --chip esp32 -b 921600 write-flash 0x0 {{FULL_BIN}}
  ```

- **Update** (keeps the settings, the paired headphones, the touch
  calibration and where you were): the same file, without erasing.

  ```
  esptool --chip esp32 -b 921600 write-flash 0x0 {{FULL_BIN}}
  ```

The settings sit above everything the image writes, so an update never
touches them.

**If your Core2 keeps restarting after installing, flash the
`-dio-full.bin` instead (same firmware, slower flash mode):**

```
esptool --chip esp32 -b 921600 write-flash 0x0 {{DIO_FULL_BIN}}
```

The firmware runs the flash in QIO, 4 data lines instead of 2, which
makes the lists, the dance and MP3 decoding faster. M5Stack ships the
Core2 in DIO, and only one Core2 (v1.3) has been tried in QIO. A unit
whose flash can't take it restarts over and over, but it still takes a
USB flash (the download mode is in the chip's ROM), and the DIO image
keeps the settings like any update. Please say so in the issues, with
which Core2 it is.

**No port shows up?** Use a USB-C cable that carries data (some only
charge), and install the USB serial driver: a Core2 has a CH9102 or a
CP2104 chip, depending on when it was made. Both drivers are under "USB
Driver" at [docs.m5stack.com/en/download](https://docs.m5stack.com/en/download);
if unsure, install both. On Linux, add yourself to the `dialout` group.

## The microSD card

- Format it **FAT32**, with an **MBR** partition table ("Master Boot
  Record"). Cards of 64 GB and up come exFAT, and a card a Mac erased
  whole gets a GPT ("GUID Partition Map"): the player doesn't mount those
  and says what it found ("This card is exFAT", "This card is NTFS",
  "This card uses GPT", "Can't read this card"). Formatting erases the
  card: copy anything on it off first, and check the disk you pick is the
  card.
- Up to 32 GB (and not GPT), the computer's own Format, FAT32, does it.
  Over 32 GB, or a GPT card:
  - **Windows 11** updated since May 2026 (KB5089549): in a Terminal run
    as administrator, `format X: /FS:FAT32 /Q /V:MUSIC` (`X:` the card's
    letter). File Explorer's Format still stops at 32 GB.
  - **Windows 10**, or when `format` answers "The volume is too big for
    FAT32": Ridgecrop's free **FAT32 Format** (`guiformat.exe`, or
    `winget install -e --id Ridgecrop.guiformat`).
  - **A GPT card on Windows** stays GPT through both: make it MBR first.
    In that Terminal, `diskpart`, then `list disk` (the card by its size),
    `select disk N`, `clean` (it erases the selected disk: check N twice),
    `convert mbr`, `create partition primary`, `assign`, `exit`; cancel
    Windows' offer to format it, then format it as above.
  - **macOS:** Disk Utility, View > Show All Devices, select the card
    itself, Erase: Format **MS-DOS (FAT)**, Scheme **Master Boot Record**.
    Or `sudo diskutil eraseDisk FAT32 MUSIC MBRFormat /dev/diskN`
    (`diskutil list` finds N).
  - **Linux:** `lsblk` to find the card (`/dev/sdX`, or `/dev/mmcblk0`
    whose partition is `/dev/mmcblk0p1`), unmount it
    (`sudo umount /dev/sdX1`), then
    `sudo parted /dev/sdX --script mklabel msdos mkpart primary fat32 4MiB 100%`
    and `sudo mkfs.fat -F 32 -s 64 -n MUSIC /dev/sdX1`.
  - Not the **SD Card Formatter**: over 32 GB it makes exFAT.
- Put the music under **`/music`**, e.g. `/music/Artist/Album/01 - Title.mp3`.
  An album's cover is the `cover.jpg` (or `folder.jpg`) next to its tracks.
- Pair headphones from **Output > Pair new headphones**. The player never
  pairs with anything by itself.

## Known limits

- MP3, FLAC and Opus only (`.mp3`, `.flac`, `.opus`): no AAC/M4A, Ogg
  Vorbis or WAV. Surround Opus and Opus with frames under 10 ms are
  skipped with a note.
- Files from 8 to 48 kHz play, on the headphones and the speaker alike;
  88.2 kHz and higher are skipped with a message that names the rate.
- The tags name the artists and the albums but don't regroup them: the
  artists are the folders under `/music`, and each album folder is one
  album. No Genres view yet.
- Covers come from the image files in the album's folder: a cover inside
  the music files isn't shown yet, and neither is a progressive JPEG (the
  album shows a note instead).
- The queue holds up to 5,000 tracks.
- Text is drawn in Latin (Vietnamese and some rarer letters without
  their accents), Greek and Cyrillic, and Chinese, Japanese and Korean
  names from a built-in 16 px set (KS X 1001's Hangul, JIS X 0208,
  GB2312's level 1 and the kana; Han drawn the Japanese way, in the
  Chinese menus too). Other scripts (Hebrew, Arabic, Thai, Devanagari
  and others) and rarer characters show as `?`. The menus are in ten
  languages, nine of them machine translations no native speaker has
  read yet, Auto DJ's texts included; the serial console is in English,
  but for the lines that quote the screen. In three of Auto DJ's
  messages on two lines in Japanese and Chinese, shown only with an Auto
  DJ index, the first line touches the message's outline.
- **Auto DJ's matching needs an Auto DJ index,** which only the transfer
  software will write, and that isn't available yet: until it is, Auto
  DJ adds random tracks from the library. With an index, tracks it
  doesn't cover are never picked and don't steer it. Its session (what
  steers it, what it avoids) and the DJ marks aren't kept across a
  restart; its switch and its settings are. Over a long session, Stay on
  seed repeats: a seed has at most 169 neighbours in the index, and once
  they have played, it starts again among them.
- The card taken out while the player runs and put back (music copied
  to it on a computer meanwhile) has its tag files and the Auto DJ index
  read again, but its folders aren't walked again until the next start:
  the new music shows up then. Another card makes the player restart.
- No WiFi yet: syncing with an mStream server comes later.

## Files

| File | What it is |
|---|---|
| `{{FULL_BIN}}` | Everything in one image, written at 0x0: install or update |
| `{{DIO_FULL_BIN}}` | The same firmware with the flash in DIO (slower): only if the Core2 keeps restarting with the one above |
| `{{APP_BIN}}` | The app alone, at 0x10000 |
| `{{PARTS_ZIP}}` | The pieces (bootloader 0x1000, partition table 0x8000, boot_app0 0xe000, app 0x10000): unzip, then `esptool --chip esp32 write-flash @flash_args.txt` |
| `{{ELF_ZIP}}` | `firmware.elf` and `firmware.map`, for decoding a crash's backtrace |
| `{{DIO_ELF_ZIP}}` | The same for `{{DIO_FULL_BIN}}` |
| `LICENSE`, `THIRD-PARTY-NOTICES.md`, `{{LICENSES_ZIP}}` | The licences: below |
| `{{SOURCE_TAR}}` | The source this binary was built from: below |
| `SHA256SUMS` | `sha256sum -c SHA256SUMS` |

## Licence

Copyright (C) 2026 IrosTheBeggar. This firmware is free software under the
GNU General Public License, version 3 or (at your option) any later version
(`LICENSE`), and comes with NO WARRANTY. It also contains other people's
code, fonts and binary libraries under their own licences, listed in
`THIRD-PARTY-NOTICES.md` with the texts in `{{LICENSES_ZIP}}`.

Opus decoding uses libopus under the IETF royalty-free patent grants
(Xiph.Org [#1524](https://datatracker.ietf.org/ipr/1524/), Microsoft
[#1914](https://datatracker.ietf.org/ipr/1914/), Broadcom
[#1526](https://datatracker.ietf.org/ipr/1526/)). Separately, members of
the Vectis Opus patent pool (Dolby, Fraunhofer, NTT) assert patents
against makers of hardware that decodes Opus. If you sell devices with
this firmware installed, that may concern you.

The source for this binary is `{{SOURCE_TAR}}`, attached here: this
repository at the tag ({{SOURCE_URL}}), the ESP8266Audio (GPL) and
ESP32-A2DP checkouts the build used, and the parts of the Arduino-ESP32
core (LGPL) it compiled. The platform, ESP-IDF's prebuilt libraries and
the toolchain are named, with their versions, in `platformio.ini` and
`THIRD-PARTY-NOTICES.md`.

Built from commit {{COMMIT}} ({{DATE}}) by this repository's GitHub
Actions workflow (`.github/workflows/firmware.yml`).
