**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.7.0

- **Repeat is off by default, so the queue now stops at its end.** 0.6.0
  always started it again from the top: for that, choose Repeat **All**
  (Now Playing's "..."; below).
- **Opus playback:** `.opus` files (Ogg Opus: what mStream's transcoding
  makes and what yt-dlp downloads) play like any other track, mono or
  stereo, at any bitrate, on the headphones and the speaker. A gapless
  album joins without a gap, and seeks and the resume point land on the
  exact sample, as on a FLAC. What the player learns about a file the
  first time it opens it is kept on the card (`/.player/opus.idx`), so a
  seek and the next play of it start sooner. Opus is always 48 kHz and
  costs what a 48 kHz MP3 does: it plays at the 160 MHz CPU speed too
  (Output > CPU speed), where lists scroll more slowly. The artist, album
  and title come from the folders and the file name, as for the other
  formats (the tags inside the file aren't read). Skipped, with a note on
  Now Playing: surround files (more than two channels), files with frames
  under 10 ms (encoders write 20 ms unless told otherwise), and the other
  Ogg codecs (a Vorbis file renamed `.opus`; `.ogg` and `.oga` files
  aren't listed). The first boot after the update builds the library's
  index again, once (0.6.0's didn't list `.opus` files). Opus decoding
  uses libopus under the IETF royalty-free patent grants (Xiph.Org
  [#1524](https://datatracker.ietf.org/ipr/1524/), Microsoft
  [#1914](https://datatracker.ietf.org/ipr/1914/), Broadcom
  [#1526](https://datatracker.ietf.org/ipr/1526/)). Separately, members
  of the Vectis Opus patent pool (Dolby, Fraunhofer, NTT) assert patents
  against makers of hardware that decodes Opus. If you sell devices with
  this firmware installed, that may concern you.
- **File names read better:** artists and albums sort past a leading
  "The" ("The Lantern Choir" under L, shown whole); `1-03 Title` and
  `103 Title` give the disc and the number, and `Artist - 03 - Title`
  the number, so an album plays disc after disc and in track order; and
  `03 - Artist - Title` shows as "Title" when the artist is the
  folder's. The Library's index rebuilds once, with the Opus one above.
- **The Now Playing seek bar:** tap the progress line, or drag along it
  and lift, to move in the track (never into its last 6 s; slide off it
  to cancel). Paused, it stays paused, and play (or the next boot) starts
  there.
- **Now Playing's menus, shuffle and repeat:** a tap on the cover, the
  title, the artist or the album offers Go to artist, Go to album and Go
  to folder; "..." holds **Shuffle**, **Repeat** (Off, All, One) and the
  Sleep timer, with a small sign under it for what is on. Both modes are
  kept across a restart. Shuffle shuffles what's up next and Off puts the
  queue's own order back; Shuffle all (the empty queue's button) turns it
  on, and its Undo puts both back.
- **MP3 decoding at full speed again:** in the builds after 0.6.0, new
  code had moved the MP3 decoder's busiest loop onto flash-cache lines it
  then fought over (3.8x realtime on a track 0.6.0 decodes at 4.8x). That
  loop now has a fixed place in the cache, and the build fails if it ever
  loses it. 0.6.0 itself wasn't affected.
- **The Pair screen stays lit while it searches** (2 minutes at most):
  the screen used to dim and then go off mid-search, which stopped it.
- **Output > About and the card:** the card's size now comes from the card
  itself; it was a count of the free space, which on some large FAT32
  cards took minutes and could stop the music. A card that doesn't mount
  says what it is ("This card is exFAT", "This card is NTFS", "This card
  uses GPT", "Can't read this card"), and the card's section below has the
  steps per computer.
- Going back to 0.6.0 later: a pause saved on an Opus track loses its
  resume point once, and a shuffled queue is lost once (0.6.0 can't read
  the shuffled queue's file).

## Hardware

- **Tested:** M5Stack Core2 **v1.3** (AXP192 power chip).
- **Untested** (it should work; reports welcome, with a photo of Output >
  About): the original Core2 (v1.0), the Core2 for AWS, and the Core2
  v1.1 (AXP2101 power chip).
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
- Text is drawn in Latin scripts only.
- Progressive JPEG covers aren't shown (the album shows a note instead).
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

The source for this binary is `{{SOURCE_TAR}}`, attached here: this
repository at the tag ({{SOURCE_URL}}), the ESP8266Audio (GPL) and
ESP32-A2DP checkouts the build used, and the parts of the Arduino-ESP32
core (LGPL) it compiled. The platform, ESP-IDF's prebuilt libraries and
the toolchain are named, with their versions, in `platformio.ini` and
`THIRD-PARTY-NOTICES.md`.

Built from commit {{COMMIT}} ({{DATE}}) by this repository's GitHub
Actions workflow (`.github/workflows/firmware.yml`).
