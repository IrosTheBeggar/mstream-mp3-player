**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.14.0

- **The player formats a card itself.** A card it can't use, exFAT (as
  cards of 64 GB and up come), NTFS, GPT (a card a Mac erased whole) or
  one it can't read, now shows **Format...** beside Try again, on the
  Library, the Queue and Now Playing. It asks twice: "Format this card?"
  (Format... or Cancel), then "Erase the whole card?", where **Hold to
  erase**, the red button, has to be held for 2 seconds by a hand: it
  fills from the left while the finger stays on it, and a tap, a run of
  taps, a lift or a slide off it does nothing. Either question goes away
  with Cancel, or by itself after a minute with no answer. Then the
  player restarts, shows "Formatting the card" over a bar ("Leave the
  card in and the player on"), and makes the card FAT32 on an MBR with
  an empty `/music`: 7.8 s for a 32 GB card on the Core2, a few minutes
  for a card of a terabyte or two (estimated: only the 32 GB card was
  formatted). "Card formatted: ready for your music" says it is done.
  Everything on the card is erased, even what the player can't read; a
  card it can't read at all is first sent to a computer ("Check it on a
  computer first": a disk check may save what is on it), and erasing it
  is the second step. With no card in the slot, there is no Format...
  button. So an exFAT, NTFS or GPT card no longer needs a computer
  ([The microSD card](#the-microsd-card), below).
- **The player has a clock, set by a computer.** The Core2's own clock
  chip keeps the time, in UTC; a computer on USB sets it (below), and
  the player believes it from then on, at every start. It is there to
  date the play history; nothing on the screen shows it yet. Until a
  computer sets it once, the clock reads as unset, even when the chip
  has a time: the Core2 comes with one, and other firmware sets it to
  local time, hours out read as UTC.
- **The player keeps a play history on the card,** in
  `/.player/plays.jnl`, for the transfer software: each track played
  from the card, when it started (once the clock is set), how long it
  played, how it ended (to its end, skipped, or stopped) and whether
  Auto DJ or the shuffle picked it. A sync reads it and reports the
  plays to the mStream server the card came from, which then tells the
  player which it has, so the player can clear them. Each play is
  written about 2 s into the next track, away from the moment one track
  joins the next (15 to 17 ms a play on the Core2); plays under a
  second aren't kept, nor the built-in tracks. The transfer software
  isn't available yet: until it is, the history only grows, to about
  13,000 plays (1 MB on the card), and the oldest 6,500 then make room.
- **A touch on the dark screen does nothing now, by default.** Output
  has a new row after Brightness, **Touch when off**, with three
  choices (a tap takes the next): **Nothing**, the default, so a pocket
  or a bag can't light the screen or keep the player from turning
  itself off: the power button turns it on; **Wake**, what every touch
  did before 0.14.0; **Track**, a screen of what is playing for 10 s,
  with nothing to tap (the cover, the title, the artist and the album,
  the time and the waveform, and the output, volume and battery at the
  top); another touch shows it 10 s more, and the power button opens
  the whole screen. A dim screen still brightens at a touch, whatever
  the choice, and the headphones lost, a track that failed or USB
  plugged in or out still light it.
- **Smoother lists.** A moving list draws a frame for each refresh of
  the panel, about 70 a second, instead of 30, timed to the panel's
  scan so it doesn't show a frame half drawn, and a row draws in 3.0 to
  3.4 ms instead of 5.6. A fling on the Artists list of a 1 TB card
  went from 30 to 68 frames a second with nothing playing, and from 30
  to 54 with an MP3 playing; the A-Z rail's scrub follows the finger 16
  times a second instead of 6.6 (8.5 instead of 5.3 with an MP3). The
  lists feel the same: the same speed cap, the same snap to a row.
- **For a computer on USB** (the software that talks to the player:
  `docs/HOST-STATUS.md` in the source): `@time` asks the player's time
  or sets it (`@time <unix-ms>`, answered `@time ok`); `@format` puts
  the format's two questions on the player's screen, and only the
  listener's tap and 2 s hold there format the card: nothing a computer
  sends can erase it. `@status` gains `clock=ok` or `clock=unset`, and
  `/.player/device.txt` says `history=1` (this firmware keeps a play
  history). An older firmware answers `@time` and `@format` with
  `@err 7`.
- **The new messages** (the format's, Touch when off and its choices)
  are in all ten languages, machine translations in the nine other
  than English; the French and Polish "Hold to erase" are shortened to
  fit their button.
- Tested on a Core2 v1.3 (2026-11-09 and 10), in a silent test mode:
  on a spare 32 GB card, the clock read at each start and set from a
  computer (the chip had lost 74.6 s in about three weeks); plays
  written while tracks played back to back; the format's questions
  cancelled and left to their minute (nothing formatted), and holds the
  console made that never format (a computer can't stand in for a
  hand); a real format by hand, 7.8 s, the card then checked on a
  computer (one FAT32 partition, no errors); the update from settings
  written by earlier test builds. Then, with no card in the slot, the
  update from settings 0.13.0 wrote: every setting kept, the clock unset
  until a computer set it, then kept through a restart. Before they were
  merged: Touch when off's three choices, and the lists, with every
  number above, on the 1 TB card.
  **Not checked on the Core2 for this release:** formatting an exFAT or
  a GPT card (only the host tests format such cards, simulated), a card
  pulled during a format, a card pulled and put back around plays, a
  play or the library on a card right after the update from 0.13.0, the
  idle power-off on battery, Bluetooth playback, a long listening
  session, a real finger on the lists, and the 1 TB card on this build.
- **The firmware is a little bigger:** about 4.52 MB, up from 4.45 MB
  in 0.13.0, for the format, the clock, the play history, Touch when off
  and the lists (72 % of the space it has on the flash, 71 % before).
  The steps below are the same.
- **The update from 0.13.0** keeps the settings, the paired headphones,
  the queue and the library (its file is the same: no rebuild); a touch
  on the dark screen then does nothing (**Touch when off**, Nothing):
  set it to Wake for 0.13.0's way. The clock reads as unset until a
  computer sets it once, even if the Core2's chip has a time.
  From 0.12.0 it is the same, with 0.13.0's library rebuild; from older
  versions, read their notes too.
- **Going back to 0.13.0 later** keeps the settings, the paired
  headphones and the queue; a touch wakes the screen again, no play
  history is kept, and coming back to 0.14.0 finds Touch when off, the
  clock and the history as they were (from the code: not tried on the
  Core2).

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

- The player reads **FAT32** cards with an **MBR** partition table
  ("Master Boot Record"). Cards of 64 GB and up come exFAT, and a card a
  Mac erased whole gets a GPT ("GUID Partition Map"): the player doesn't
  mount those and says what it found ("This card is exFAT", "This card
  is NTFS", "This card uses GPT", "Can't read this card").
- **The player can format it** (since 0.14.0): put the card in, tap
  **Format...** under that message, then hold the red **Hold to erase**
  for 2 seconds. It restarts and makes the card FAT32 on an MBR with an
  empty `/music`. Formatting erases the whole card: copy anything on it
  off first.
- Or format it on a computer (check the disk you pick is the card). Up
  to 32 GB (and not GPT), the computer's own Format, FAT32, does it.
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
