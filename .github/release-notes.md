**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.9.0

- **Names in Greek, Cyrillic and more Latin.** Titles, artists, albums
  and folders in Greek (Χάρις Αλεξίου) and Cyrillic (Кино, Земфира) are
  drawn as they are, where 0.8.0 showed `?`. So are the accented letters
  of Romanian (Ș, Ț), Turkish (İ, Ğ), Croatian and Serbian Latin (Đ),
  Slovak (Ľ, Ĺ), the Baltic languages, Maltese, Esperanto and Welsh, and
  Pinyin's tones (Lǚ), which 0.8.0 drew bare or as `?`. The rows are as
  high as before. Latin letters the player still has no glyph for lose
  only their accents instead of turning to `?` (Vietnamese: Mỹ Tâm shows
  as My Tâm). A tag that keeps Windows' curly quotes, dashes, € or ™ in
  an old Latin-1 frame shows them (Don’t Stop – Live, not Don?t Stop ?
  Live), full-width letters (ＡＢＣ) show as ABC, and a name stored
  decomposed, as macOS and some tag editors write them, keeps its accents
  (Beyoncé, not Beyonce?).
- **Chinese, Japanese and Korean names** are drawn from a set of 10,557
  characters built into the firmware, at 16 px: the 2,350 common Korean
  Hangul syllables (KS X 1001), the kana and 6,355 kanji of Japanese's
  JIS X 0208, the 3,755 common Chinese characters (GB2312's level 1), and
  their punctuation and symbols (☆, ♪, → and ℃ among them): 宇多田ヒカル,
  王菲, 아이유. Han characters are drawn the Japanese way, which a Chinese
  reader reads too, though a few differ by a stroke. Rarer characters
  still show as `?`: the Hangul syllables outside the 2,350 (똠),
  Traditional Chinese characters outside these sets, and the very rare
  Han of Unicode's extension blocks. Half-width katakana (ｶﾀｶﾅ) show as
  the full-width ones (カタカナ). In bold text (Now Playing's title, the
  Queue's current track) they are drawn semi-bold, to sit beside the bold
  Latin.
- **Now Playing's title:** a title with any of these characters, ☆ or ♪
  included, is drawn in the 16 px bold rows (up to three lines) instead of
  the 22 px ones, so its Latin letters are the size of its CJK; one or two
  lines are centred. A Latin title too long for the large rows is centred
  in the small ones too, a little lower than in 0.8.0, and its lines no
  longer clip the bottom of the g, j, p and y in the line above. A
  Chinese or Japanese title breaks between characters, by the usual rules
  (never before 。, 」, ー or a small kana, never after 「); Korean breaks at
  its spaces.
- **The order across scripts.** 0.8.0 sorted names in other scripts by
  their length, the scripts mixed. Now they sort by script, then by
  letter, all under the A-Z rail's `#` after the digits: Greek, Cyrillic,
  Korean, the Japanese kana, then Han (by code point, not by reading).
  Hiragana and katakana are one alphabet (ひかる next to ヒカル, ガ just
  after カ, ヴ next to ウ, a small kana with its large one), half-width
  katakana sort as full-width, and a lone Hangul consonant (ㄱ) sorts just
  before the syllables it starts (가). Latin names keep their order,
  except that a letter 0.8.0 showed as `?` now sorts as its base letter:
  a name that starts with one (Romanian's Ș or Ț, Vietnamese's Ư,
  full-width ＡＢＣ), under `#` in 0.8.0, is under S, T, U or A. An
  artist folder in another script whose tags give a sort name
  (ARTISTSORT) is listed by it: `Кино` with the sort name `Kino` under K.
- **A computer can ask the player how it is**, over USB, without
  restarting it or stopping the music: its version, the card's size and
  free space, how many tracks it has, the battery, whether it's playing,
  and the paired headphones. Asked to, it counts the card's free space
  (never while music plays: about 2½ minutes for a 1 TB card), and it
  shows "This one" and the port's name full screen for 5 s, with a buzz,
  to tell two players apart. This is for the MP3 Player tab of the mStream
  terminal player, which is coming. 0.6.0 to 0.8.0 answer that they
  don't know the question, so the tab can tell they need an update.
- **Fixed: a long listing on the serial console could restart the
  player.** While a computer read a long list from the console (a
  20,000-track library's albums take about 6 s), a line the player logged
  meanwhile, as it does when it calls headphones that are out of reach,
  waited for the list to end, and after 5 s the watchdog restarted the
  player. 0.8.0 does this too. Now both print whole and the player
  carries on.
- Tested on a Core2 with a card of 19,410 tracks: Japanese kana names on
  Now Playing, in the Queue and in the Library's lists, a Cyrillic title
  on Now Playing and in the Queue, a Korean one on Now Playing, and Greek
  album names in the Library, in their new order; the lists scroll as
  smoothly with them as before (30 frames a second). What those tests
  turned up was fixed first: the console restart above.
- **The first start of 0.9.0 on a card 0.8.0 used** builds the library
  again, once, from the tags 0.8.0 read, since the order changed: about
  10 s for 20,000 tracks, with the start-up screen up meanwhile (on a card
  of 19,410 tracks the library was ready 11.8 s after the card mounted).
  No file is read again, and the queue and its place are kept. The next
  starts load the library as it was left, in about 1.5 s. A card that
  0.7.0 used last gets 0.8.0's first start instead (0.8.0's notes say
  what it does).
- **The firmware is bigger:** about 4.1 MB, up from 2.6 MB in 0.8.0, for
  the Chinese, Japanese and Korean characters and the larger fonts (66 %
  of the space it has on the flash, 42 % before). Installing and updating
  take a little longer; the steps below are the same, and an update still
  keeps the settings.
- **Going back to 0.8.0 later:** its first start rebuilds the library the
  same way, from the tags read before (about 10 s for 20,000 tracks):
  nothing is read from the files again, and the queue and its place are
  kept. It then draws and sorts the names as 0.8.0 did. Coming back to
  0.9.0 rebuilds it once more, the same way.

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
  GB2312's level 1 and the kana; Han drawn the Japanese way). Other
  scripts (Hebrew, Arabic, Thai, Devanagari and others) and rarer
  characters show as `?`. The menus are in English.
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
