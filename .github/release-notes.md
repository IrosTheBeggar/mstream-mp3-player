**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.10.0

- **The menus in ten languages:** English, Deutsch, Español, Français,
  Italiano, 日本語, Polski, Português (Brazilian), Русский and 中文
  (Simplified), the languages of mStream's mobile app and terminal
  player. **Output > Language** lists each in its own name, and a tap
  changes the whole screen to it at once, without a restart; the player
  keeps it. The menus, the notes, the dialogs, the tips and the touch
  check are all in it, and counts are written its way (19.410 in German,
  19 410 in French). The serial console stays in English.
- **A new unit asks first.** Installed fresh (the first install below,
  which erases), the player starts on the Language page, before the
  touch check and the tips, which then speak the language picked; the
  page's header says where to change it later. Leaving it by a tab keeps
  English. A unit updated from 0.9.0 or older doesn't ask: it stays in
  English, with every setting it had.
- **The nine translations are machine translations**, not yet read by a
  native speaker. They were made from the words mStream's mobile app and
  terminal player already use, so the three use the same words for the
  same things, and by Fable agents for the rest, each language then
  checked by another agent. Picking one of the nine shows a note in it
  that says so ("Maschinell übersetzt: Fehler sind möglich"), as the
  terminal player does. A native speaker's corrections are welcome, as a
  pull request on the language's file in `i18n/` (its README says how).
- **Room for every language.** Where a translation is longer than the
  English, a button's label is drawn in the smaller font (Spanish's
  "Desconectar" on the Bluetooth card), the Undo and View buttons on a
  note show their icons, a tab whose name is too long shows its icon
  alone (Japanese's and Russian's Library, German's Queue), a header puts
  what doesn't fit beside its title on a second line under it (German's
  Queue: "Warteschlange" over "100 von 113 · noch 13 · 45 min", the list
  keeping its rows), and a menu row puts its detail under its label (the
  Sleep timer's state in Spanish and Portuguese). Every text of the ten
  languages was measured with the player's own fonts, in each place it
  is drawn and with the biggest counts: each fits whole or gives way to
  a shorter form, and only names are cut, with "…".
- **"Various Artists" in the menus' language:** the line a compilation
  gets when its tracks say compilation and none names an album artist
  reads "Verschiedene Künstler" in German, "Artistes divers" in French,
  "群星" in Chinese. An album artist tag that says "Various Artists"
  itself is a name, and is shown as tagged.
- **English, fixed where it was wrong:** "Shuffling 1 track", "Added 1
  track", "Plays next: 1 track" and "Library: 1 track", where 0.9.0 said
  "1 tracks", and "Here until the headphones connect" for headphones
  without a name. Four texts that didn't fit are shorter: "Stopped
  looking: Play tries again", "Waits for headphones", "Already connected:
  WH-1000XM4" and "Speaker only (silent test mode)". Others that 0.9.0
  cut are whole: "Headphones disconnected", "WH-1000XM4 (not connected)"
  on Now Playing, "No music found: put folders in /music" and "Play all
  19410"; About's library line leaves out the artists when the three
  counts don't fit.
- **Long names and long queues in English:** the menu a long press opens
  cuts a long name, never the count of tracks after it (0.9.0 cut "14
  tr…"); the volume menu leaves out its "volume" label beside a long name
  rather than draw "vol…"; an artist's All tracks puts a long name under
  its title; an album opened from its artist puts its line under the
  title, the year left out before the count ("Daft Punk · 14 tracks"
  under "Discovery"); a folder's counts say "files" where "audio files"
  doesn't fit ("12 folders, 140 files"). The Queue's header gives the
  position before the count: near the end of a queue of 1,000 tracks or
  more with hours to go, "1980 of 2000" where 0.9.0 showed "20 up next ·
  1 h 40+ min".
- Tested on a Core2: each of the ten languages picked in turn, on Now
  Playing, the Library, the Queue, the Output tab, the Language page,
  About, both tips cards, a new unit's first start and the note on
  picking a language; and, on a card of 113 tracks, the update from
  0.9.0 and the way back. What those tests turned up was fixed first and
  checked again on the device: German's Queue header, which cut its line
  mid-number, and the tips' hint, drawn under their button in French,
  German and Russian.
- **The first start of 0.10.0 on a card 0.9.0 used** builds the library
  again, once, from the tags 0.9.0 read, so that a compilation's line
  follows the language: 60-62 ms on a card of 113 tracks, and about 10 s
  to expect for 20,000 (the same kind of rebuild took 10.1 s for 19,410
  in 0.9.0). No file is read again, and the queue is kept. So are the
  settings, and the menus stay in English until a language is picked
  (the serial log says `[nvs] schema 2 -> 3: migrated (the UI's language:
  English, as before)`). A card that 0.8.0 used last gets the same
  rebuild; one that 0.7.0 used last gets 0.8.0's first start instead
  (0.8.0's notes say what it does).
- **The firmware is a little bigger:** about 4.27 MB, up from 4.13 MB in
  0.9.0, for the nine languages' texts (68 % of the space it has on the
  flash, 66 % before). The steps below are the same, and an update still
  keeps the settings.
- **Going back to 0.9.0 later:** its first start rebuilds the library the
  same way, from the tags read before (63 ms on the card of 113 tracks;
  about 10 s for 20,000): nothing is read from the files again, and the
  queue and the settings are kept. 0.9.0 is in English only: it sees
  that a newer version saved the settings, leaves them as they are and
  uses all it knows, which is every one but the language; that stays
  saved, unread. Coming back to 0.10.0 rebuilds the library once more,
  the same way, and its menus are in the language picked before.

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
  read yet; the serial console is in English.
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
