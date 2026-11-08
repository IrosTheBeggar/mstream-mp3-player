**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.12.0

- **The power button turns the screen off.** A short press of the
  button on the Core2's side turns a lit screen off at once, bright or
  dim, and the music plays on; another short press lights it again, as
  before. With **Screen off after: Never**, that is now how you turn the
  screen off. While something keeps the screen lit (the start-up
  screen, the touch calibration, a pairing, a play waiting for the
  headphones, or a computer showing something on it), the press leaves
  it on. A screen held lit goes off all the same: under the idle
  power-off's warning, the sleep timer's fade, or the Pair screen's
  search, which the press stops. A long hold still turns the Core2 off.
- **The A-Z rail follows your finger.** On a long list, a finger
  sliding down the letter rail moves its thumb and letter at every
  frame, and the list catches up just behind: 60 ms after its last
  draw, or at once when the finger rests. In 0.11.0 the thumb moved
  only as often as the whole list could be drawn, 15 times a second on
  the Artists list of a card of about 19,500 tracks, where it now moves
  19 to 30 times; and the player's work on the card (checking it for
  new music, reading tags) went on under the finger, on the bus the
  screen shares. That work now waits until the list has been still for
  half a second. Measured with nothing playing.
- **A card that comes out while music plays no longer runs the queue
  out.** In 0.11.0 every track after it failed to open, about 2 s each,
  with a note for each, until the whole queue had failed, and the
  player kept that as its place: a 25 s pull ended a 12-track album.
  Now the screen lights with "Still no card: is it all the way in?" and
  the player holds at the next track, still playing but silent: it
  tries it again 2 s later, then after 4, 8 and 16 s, then every 30 s,
  and once the card is back the music starts again by itself, from
  where that track would have started. Still out after 2 minutes, it
  pauses there, and Play tries again. The track that was playing when
  the card came out still stops there, the rest of it lost. Each try
  with the card out can hold the screen for a second or two, and the
  idle power-off waits up to 2 minutes longer. Another card put in
  meanwhile makes the player restart for it, as before, with no track
  skipped. And a count of the free space asked from a computer over USB
  now notices that the card was out: in 0.11.0 the player could then
  write over files a computer had copied to it meanwhile.
- **A card that misses a read for a moment no longer cuts the track.**
  In 0.11.0 a single read the card didn't answer ended the track that
  played, with no word, and restarted the player if it was working on
  the card in the background. Now the player asks the card there and
  then and, finding it back, reads again: the track plays on, and the
  covers, Auto DJ and the work in the background carry on the same way.
  A part of the card that keeps failing still ends the track.
- **A card read that fails no longer costs you the queue.** In 0.11.0
  a start that couldn't read the saved queue, or had no memory free to
  read it, put the whole library in its place; one that couldn't read
  the library's files cut the queue to the tracks it could still see
  (on a test card, to nothing, for good); and a save the card refused
  could leave the saved queue empty or gone. Now a queue it can't read
  is left as it is on the card: the player reads it again a few times
  over about 1.4 s, then starts with an empty queue, and Now Playing and
  the Queue tab say "Card read failed at startup: restart to try
  again". A restart that reads it brings it back, at its track and
  second; until then, anything you play or add replaces it, about 2 s
  later. A queued track the library leaves out (from a folder it
  couldn't read) stays in the queue under its file name, with no
  artist, album or cover, and plays, unless the card says it is gone. A
  save counts only once its file is whole on the card; one that fails
  is tried again 10 s later, then twice as long each time up to 5
  minutes, and at once when the card is back. And when a card put back
  makes the player restart ("Card back: restarting", when it was
  working on the card in the background), it now waits up to 30 s for
  the queue's save to work: 0.11.0 restarted 1.2 s on, and the queue's
  last changes could be lost.
- **A card with a bad spot no longer restarts the player at every
  start.** A part of the card that never answers, read at every start,
  could restart the player over and over (every 8 to 35 s on a test
  card). Now such a read fails and the player carries on, keeping what
  is on the card (below). Should the card make it restart twice in a
  row all the same, the next start skips its work on the card and says
  "Card read failed at startup: restart to try again" for a few
  seconds; turning the Core2 off and on clears that, as do 30 minutes
  of running. A card that keeps going silent (a dozen times) is left
  alone until a restart: the player reads no covers, Auto DJ index or
  waveforms from it and does no work on it in the background, and the
  music plays on. Also fixed: a card slow to answer at the start passing
  for another card, and one write the card refused stopping every track
  from opening (if it keeps refusing, the player now stops writing to it
  until a restart, and plays on).
- **A card read that fails at a start no longer costs the library.** In
  0.11.0 it could pass for a file that was gone: the player forgot the
  tags it had read and read every file's again (about 3 to 6 minutes at
  20,000 tracks), and on a card the transfer filled it dropped the
  transfer's tags too, then did it all again at the next start. Now it
  reads again a few times over about 1.4 s, then keeps what is on the
  card: it plays from the library it saved (failing that, from the
  folders by their names, for that session), saves nothing over it, and
  leaves finding new music and reading tags to a start that can read
  the card. Rescan, on the Output tab, says why: "Card read failed at
  startup: restart to try again", the one new message, a machine
  translation in the nine other languages like the rest.
- **A stopped track shows its length.** A track nothing has played yet
  since the start (no place saved in it) showed a dotted line and
  "--:--" on Now Playing; it now shows 0:00 and the track's length, and
  a tap on the line picks where Play will start, kept as a paused
  track's place is. The length can read a second more until the track
  plays. A track the library doesn't list keeps the dots.
- **Taps answer sooner.** A press on a menu's row, the sleep timer's or
  the volume panel's buttons or a letter of the A-Z grid draws that
  control alone, in 2 to 10 ms where 0.11.0 took 36 to 58 ms, and a
  dialog's buttons do the same; Now Playing's buttons draw their own
  patch (3 ms for the volume, 12 before). A track change no longer draws
  the whole Library or Queue list again (50 to 110 ms), only the rows
  whose mark changes; going from a scrolled list to Now Playing takes
  53 ms (76 before); a setting's tap draws its row alone (12 ms, 49
  before); and the lines the player writes to its USB serial port no
  longer hold it up 10 to 30 ms at a tap. A setting a tap changes is
  saved once the taps rest (1.5 s after the last, 6 s at most), and
  before the player restarts or turns itself off, rather than under the
  finger.
- **Now Playing's line can be the track's waveform.** On a card where
  the transfer software wrote the tracks' waveforms (from mStream's
  waveform data, as `/.mstream/waves-<gen>.bin`), the progress line is
  the playing track's shape: 99 mirrored bars, what has played in
  colour, a thin playhead. A tap or a drag on it seeks as on the line.
  A track without one, or a card without the file, keeps the plain line
  in the same place. **The transfer software isn't available yet,** so
  no card has one today. From 0.12.0 the player says on the card that it
  reads them (`read.mpwf=1` in `/.player/device.txt`, checked at each
  start): that is how the transfer software will know to write them, so
  once it is out, start the player once with the card in before the
  first transfer. The times under the line sit 5 px lower, with or
  without bars. The file's format, for any transfer software, is
  `docs/WAVEFORM-INDEX.md` in the source.
- **Auto DJ with an index rides out card faults and library updates**
  (an index too needs the transfer software). A card read that failed
  as Auto DJ opened its index, or read the transfer's files, left its
  picks random for the whole session; it now tries again 2, 10 and 30 s
  later, keeping its session, before it says "Can't read the Auto DJ
  index". After a library update, Auto DJ could say "Not enough memory
  for the Auto DJ index", with memory to spare, and pick at random until
  a restart: fixed.
- **The serial console says more about a stutter,** for bug reports:
  the stats line counts the breaks in the sound beside its underruns
  and gives a stall of any length whole, and the Bluetooth line the
  link's signal (`rssi=`) and power mode (`mode=`). `@status`, which a
  computer asks over USB, also says which transfer's card is in (`cid=`,
  `-` on a card filled by hand).
- Tested on a Core2 v1.3 as each change went in (2026-10-30 to 11-05):
  the card's faults on a spare 32 GB card of 19,412 tracks with made-up
  names, through test builds that leave chosen card commands unanswered
  or failing (about 210 starts for the restarts alone), and by hand,
  pulling the card and putting it or another one back, during a
  Bluetooth play too; the saved queue compared by CRC after the faults,
  and once after a Windows disk check (`chkdsk /f`); the screen's
  timings on a card of about 19,500 tracks; the waveforms with test
  shapes, and once with a file from a test build of the transfer
  software; the power button by script and by hand; and, on this
  release's code, the update from the published 0.11.0 and back again:
  the library, the queue, the second a stopped track was paused at and
  the settings kept both ways, nothing rebuilt.
- **The firmware is a little bigger:** about 4.40 MB, up from 4.36 MB
  in 0.11.0, for the waveforms and the card's checks (70 % of the space
  it has on the flash, 69 % before). The steps below are the same.
- **The update from 0.11.0** keeps the settings, the paired headphones,
  the queue and the library as they are: nothing is rebuilt. From
  0.10.0 it is the same, Auto DJ starting off, its settings at their
  defaults. A card that 0.9.0 or 0.8.0 used last gets 0.10.0's one
  rebuild of the library from the tags read before (about 10 s for
  20,000 tracks; no file is read again, and the queue is kept), and a
  unit updated from 0.9.0 or older stays in English, every setting
  kept, until a language is picked in Output > Language. A card that
  0.7.0 used last gets 0.8.0's first start instead (0.8.0's notes say
  what it does).
- **Going back to 0.11.0 later** keeps them too: 0.12.0 saves the
  settings, the queue and the library as 0.11.0 does. 0.11.0 writes its
  own `device.txt`, which doesn't ask for waveforms: start 0.12.0 once
  with the card in before a transfer.

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
- **Auto DJ's matching and the waveforms need files only the transfer
  software will write,** and that isn't available yet: until it is,
  Auto DJ adds random tracks from the library, and Now Playing shows the
  plain line. With an index, tracks it doesn't cover are never picked
  and don't steer it. Its session (what steers it, what it avoids) and
  the DJ marks aren't kept across a restart; its switch and its
  settings are. Over a long session, Stay on seed repeats: a seed has at
  most 169 neighbours in the index, and once they have played, it
  starts again among them.
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
