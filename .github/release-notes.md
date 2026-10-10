**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.8.0

- **The library comes from the tags.** The player reads each file's tags
  itself (ID3 in MP3s, Vorbis comments in FLAC, OpusTags in Opus), in the
  background after it starts, while it plays and on battery too. The
  Library, Now Playing and the Queue then name each track by its tags: its
  title and artist (a guest artist shows under the title), and its
  album's name, year and artist; an album plays in the tags' disc and
  track order. Where a file has no tags, the names come from the file
  name, as before. The artists and the albums are still the folders
  (`/music/Artist/Album/`): the tags name them but don't regroup them. An
  artist's albums are listed newest first, the A-Z lists follow the tags'
  sort names where there are any, and the Folders list shows the card as
  it is.
- **Disc dividers:** an album of more than one disc (by its tags, or
  `1-01`, `2-01` names) has a "Disc 1", "Disc 2" row before each disc's
  first track. Now Playing shows the album with its year ("Glass Harbour ·
  2001") when both fit.
- **The Library says what the player is doing**, on a line at its bottom:
  "Checking the card…", "Reading tags 1,234 / 19,410", "Updating
  library…", then "Library updated". Files added on a computer are found
  at the next start ("Found 12 new tracks") and show once their tags are
  read (a big batch shows by its file names first). **Output > Library**
  says how many tracks have tags, and its **Rescan** reads them all again
  (it asks first). Below 10 % battery the reading waits for USB or 15 %.
- **While the library updates** (a moment on a small card, up to about
  20 s at 20,000 tracks) the music plays on. The lists say "Updating the
  library…", and a skip, a seek or a change to the queue waits ("Updating
  the library: a moment"). An update starts only when the playing track
  has enough time left; should a track end meanwhile all the same, the
  next one waits, paused, unless it was already joined gaplessly.
- **Big cards start in seconds.** The player no longer reads through the
  whole card at every start: it loads the library it saved and checks the
  card in the background ("Checking the card…"). On a card of 20,000
  tracks the library is ready about 1.5 s after the card mounts, where
  0.7.0 took about 90 s at every start; the check takes about 20 s more,
  behind the scenes. A new card of 20,000 tracks is ready in about 20 s the
  first time (0.7.0: about 3 minutes).
- **The first start of 0.8.0 on a card 0.7.0 used** lists the card once by
  its folders and file names, as 0.7.0 did at every start (the start-up
  screen stays up meanwhile: about 20 s for 20,000 tracks). Then it reads
  every file's tags in the background, and until that's done the Library
  shows the file names, as 0.7.0 did. For 20,000 tracks that takes several
  minutes with nothing playing, and longer while music plays, which it
  gives way to. Then "Updating library…" and "Library updated". It happens
  once: the next starts load the library as it was left. The queue, its
  place and the resume point are kept.
- **The queue holds up to 5,000 tracks.** Play all, an artist's or a big
  folder's Play, and Shuffle all on a bigger library take 5,000 of them
  (the first 5,000 in order, or with shuffle on a random 5,000: each
  Shuffle all another), and the message says so. Play next and + Queue on
  a full queue first push out tracks you have heard, oldest first: the
  message says how many, and its Undo puts them back. When no heard track
  can make way the add is refused ("No played track can make way: the
  queue holds 5,000 tracks"). A longer queue that 0.7.0 saved (on a new
  card the first queue was the whole library) comes back as the 5,000
  tracks around the one it was on.
- **The start-up screen** shows the mStream logo and the player's version,
  for about 3 s or until the library is ready; then the touch rescue's
  line ("Touch trouble? Hold a finger on the screen.") shows at the bottom
  for at least 1.5 s. What it used to list (the board and its chips,
  memory, the battery, the library) is in **Output > About > Device
  info**, with the CPU's clock, the battery's voltage, the uptime and the
  build; it updates every 3 s while it's open.
- **Writes to the card are more reliable.** The SD card driver (a patched
  copy of the Arduino core's, in this repository) now waits until the card
  has finished each write. The stock one sometimes took the card's busy
  signal for its answer, and the write failed.
- **A card swapped while the player is on** is never written to: the
  player says "Another card: restarting" and restarts to use it, so one
  card's queue or library can't end up on another. The same card taken
  out and put back carries on (or restarts, "Card back: restarting", if it
  was busy with the card).
- All of this was tested on a Core2 with a card of 19,410 tracks, and what
  those tests turned up was fixed first: the card writes above, a console
  command that ran out of stack and restarted the player, and too little
  stack to spare while the library rebuilds.
- **Going back to 0.7.0 later:** 0.7.0 can't read 0.8.0's library file,
  so its first start builds its own again from the folders and file names
  (minutes on a big card). Coming back to 0.8.0 then builds the library
  from the tags it read before, without reading them again. The queue is
  kept both ways.

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
- Text is drawn in Latin scripts only.
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
