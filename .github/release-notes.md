**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.13.0

- **The Albums list can filter by mood.** On a card with mood scores
  (mStream's analysis of each track, which only the transfer software
  writes to the card; it isn't out yet: below), the list starts with a
  row that reads "Filters" over the count of albums. A tap opens one
  screen of moods: Happy, Neutral or Sad; Relaxed, Neutral or
  Aggressive; Instrumental or Vocals; any or all of Acoustic, Party and
  Danceable (a track must have each one picked); and how sure each must
  be, Strict, Standard or Loose. Each tap applies at once, with the
  count of albums that fit ("585 of 1,747 albums" for Happy, on a 1 TB
  card of 19,533 tracks); a second tap lets a mood go, Clear lets them
  all go and sets Standard again, and the cross closes the screen. The
  list then shows only the albums where more than half of the tracks
  with scores fit (mStream's rule, counted over the tracks on this
  card). The row says what is set and how many albums are left, and its
  cross clears the filter; scrolled past it, a dot beside Albums at the
  top says a filter is on, and a tap on Albums there goes back to the
  top. With none left, the list says "No albums match these filters".
  On a test card of 1,707 albums, a tap on a mood answered within 35 ms
  with nothing playing, and in 50 to 71 ms while an MP3 played.
- **Auto DJ can keep to a mood.** Output > Auto DJ has a fourth row,
  Smart filters ("Any" with none set), which opens the same screen in
  Output's blue, counting tracks ("3,085 of 19,413 tracks" for Relaxed
  and Instrumental on a test card). The filter is saved, and every
  track Auto DJ adds then fits it: neighbours first, as before, then any
  other track that fits ("No close match: any track that fits"), then
  repeats, the one played longest ago first. In Stay on seed, when no
  track near the seed fits, or all that do have played lately, the
  playing track becomes the one Auto DJ stays near, if it fits and the
  index lists it, until it is skipped quickly or Anchor changes. The
  Auto DJ button of an empty queue keeps to the filter too, and Now
  Playing's "..." menu shows what is set on its Auto DJ row. Without an
  Auto DJ index the picks are random, inside the filter; with no
  filter, Auto DJ picks exactly as in 0.12.0. On the 1 TB card, under
  three filters (157 to 8,117 tracks fitting), Follow the vibe found a
  close match for all 200 picks of each test run. Picking any track
  that fits, when no close match does, took 13 to 42 ms on a test card
  with music playing (about 12 ms paused): a short hitch on the screen
  at most, and no break in the sound.
- **Auto DJ says how many tracks fit.** The status row on its page reads
  "3,085 of 19,413 tracks fit", and under it the share of tracks with
  scores beside BPM and key ("BPM 95% · key 97% · mood 92%"). At 50 or
  fewer (Auto DJ waits 50 tracks before it plays one again), it says
  "Only 28 tracks fit" in amber over "They will repeat · BPM 95% · key
  97%", and a note says "Only 28 tracks fit: they will repeat". With
  none, Auto DJ adds nothing and says "Waiting: no track fits the
  filters", and the empty Queue and Now Playing say "No track on this
  card fits these filters", with only the Open Library button under it.
  The track Auto DJ has already queued, if a new filter leaves it out,
  plays once: "Plays once, then the filters apply".
- **The transfer software isn't available yet,** so no card has mood
  scores today. Until it is out, the Albums list has no Filters row, as
  before, and the Smart filters row is dim and reads "No mood data"; a
  tap on it says "No mood data: sync this card from mStream", and Auto
  DJ picks from every track (a filter set before is kept for a card
  with scores). Once it is out, a sync from an mStream server that
  analyses moods (v6.33.1 or later) writes each track's scores into the
  card's tag file, `/.mstream/tags-<gen>.bin`, with nothing to set on
  the player, and the filters appear at the next start with that card.
  On a card it makes for someone else it writes no scores, as it writes
  no Auto DJ index there, so the filters don't appear on it, though the
  Smart filters row still says to sync. Older players read a card with
  scores as before, leaving the scores aside (0.12.0 did, in the test
  below). The scores' format, for any transfer software, is
  `docs/METADATA.md` 2.6.4 in the source.
- **The mood words read as in mStream's mobile app,** in all ten
  languages: Filters, the moods, Strict, Standard, Loose, Clear, Smart
  filters, Any and No mood data. In the nine other languages, the
  filters' 14 new messages are machine translations, like the rest of
  the menus.
- **The Library's status line no longer wipes a menu.** In 0.12.0, when
  the line ("Checking the card…", or "Reading tags" and its count)
  ended while a long-press menu, a dialog or the A-Z grid was open, it
  wiped it and left a blank list. Now it stays whole.
- Tested on a Core2 v1.3 as each change went in (2026-11-06 to 11-08):
  the filters on a spare 32 GB card of 19,413 tracks with made-up names
  and made-up mood scores, their counts of tracks and albums matched
  against a computer's; the screens compared pixel by pixel with their
  designs, Auto DJ's page seen in all ten languages and the filters'
  screen in four; Auto DJ's picks in scripted runs of 200 under four
  filters on each card (with none, the same picks as before), and with
  MP3, FLAC and Opus tracks playing in a silent test mode, with no break
  in the sound; the free memory, compared with a build that leaves the
  scores aside; the status line's fix, first seen failing on 0.12.0;
  the first real sync, on a 1 TB card of 19,533 tracks with a test
  build of the transfer software and mStream v6.33.1, which gave 19,249
  of them scores: the Core2, the transfer software and the computer's
  check agreed on every mood count and on every filter's tracks and
  albums; and, on this release's code with that card, the update from
  0.12.0 and back again: one library build each way, and the queue, the
  settings and Auto DJ's filter kept. Not seen on the Core2: what every
  card shows today, the Albums list without its row and the dim Smart
  filters row (the test cards had scores).
- **The firmware is a little bigger:** about 4.45 MB, up from 4.40 MB
  in 0.12.0, for the mood filters (71 % of the space it has on the
  flash, 70 % before). The steps below are the same.
- **The update from 0.12.0** rebuilds the library once, at the first
  start (its file changed for the mood scores): about 11 s at 19,400
  tracks on a card filled by hand, from the tags read before (no file is
  read again), and 17.3 s on the 1 TB card, which a test build of the
  transfer software filled. It keeps the settings, the paired headphones
  and the queue. From 0.11.0 it is the same; from older versions, read
  0.12.0's notes too.
- **Going back to 0.12.0 later** rebuilds the library once too: 0.12.0
  takes the new library file for a damaged one and makes its own (15 s
  on the 1 TB card), and coming back to 0.13.0 rebuilds it again. 0.12.0
  leaves the card's mood scores aside and never reads Auto DJ's filter,
  so its picks are unfiltered; the settings, the paired headphones and
  the queue are kept, and the filter is there again back on 0.13.0.

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
- **Auto DJ's matching, the waveforms and the mood filters need files
  only the transfer software will write,** and that isn't available
  yet: until it is, Auto DJ adds random tracks from the library, Now
  Playing shows the plain line, the Albums list has no Filters row, and
  Auto DJ's Smart filters row is dim. With an index, tracks it doesn't
  cover are never picked and don't steer it. Its session (what steers
  it, what it avoids) and the DJ marks aren't kept across a restart; its
  switch and its settings are. Over a long session, Stay on seed
  repeats: a seed has at most 169 neighbours in the index, and once they
  have played, it starts again among them.
- **The mood filters go by the scores on the card.** A track without
  any (one mStream hasn't analysed, one from another mStream server, or
  one copied to the card or changed on it by hand since the card's last
  sync) never fits a filter, and an album with no scored track leaves
  the Albums list under any filter (12 of 1,747 on the 1 TB test card).
  An album is its folder, and it fits when more than half of its scored
  tracks on this card fit. mStream's apps count all of the album's songs
  on the server, so an album that is only partly on the card can fit
  here and not there, or the other way round. The scores are a copy:
  mStream's later changes to them reach the card with a later sync, not
  always the next one. Only the Albums list and Auto DJ filter: Artists
  and Folders always show everything. The Albums filter is forgotten at
  a restart; Auto DJ's is kept.
- The card taken out while the player runs and put back (music copied
  to it on a computer meanwhile) has its tag files, the Auto DJ index
  and the waveforms read again, but its folders aren't walked again
  until the next start: the new music shows up then. Put back while the
  player was working on the card in the background, it makes the player
  restart instead ("Card back: restarting"), and another card always
  does. The track that was playing when the card came out stops there.
- **A card with a part that can't be read** among the library's files
  can keep the player from its work on the card (no new music found, no
  tags read) at every start, until a disk check on a computer mends
  that part or the file there is removed; it plays from the library it
  saved meanwhile. If that file is the library's own
  (`/.player/library.idx`), each start also builds the library for the
  session, 12 to 17 s more at 19,412 tracks.
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
