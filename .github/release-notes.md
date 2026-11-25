**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.15.0

- **Listening stats.** The Library's Artists list starts with a
  **Listening stats** row: the time listened on this card since
  counting began ("74 h 10 min since 1 Sep"). A tap opens its page, a
  row each for **Last 7 days** (the week's time and plays, a bar a day,
  your big day, and how often Auto DJ's picks were played rather than
  skipped), **Most played** (opening on All time, with 7 days and 30
  days beside it), **New finds** (first heard in the last 30 days, and
  whether Auto DJ or the shuffle found them), **Old favourites** (5
  plays or more, none in 30 days), **Not played yet** (the albums with
  no track played, A-Z) and **Recently played** (newest first, under
  Today, Yesterday and the date). Their tracks play as an album's do: a
  tap for Play, Play next or + Queue, a hold for the track's sheet, the
  top bar for the whole list. With nothing played yet, the stats offer
  Shuffle all.
- **How plays count.** A play is 30 s of a track, or half of it when
  that is shorter, as mStream counts them; a track skipped before that
  is a skip, and every second heard counts toward the time. Every
  player's plays on the card count, 0.14.0's included: the player works
  them out from the play history it keeps on the card and saves them
  there at each start and every 24 tracks, skipped ones too
  (`/.player/stats.bin`, and beside it 24 bytes a track played: 467 KB
  for 19,478 tracks). Nothing is kept on the player itself. The
  history's oldest plays are saved into the stats before it makes room
  for new ones, and once a sync has reported them (the mStream terminal
  player's MP3 Player tab, since its 0.16.0), its files are cleared only
  when the stats hold their plays.
- **Opening them** takes 2.1 to 2.3 s of "Counting plays..." with nothing
  playing and 2.8 to 2.9 s with an MP3 (on a test card of 19,417
  tracks), and for about a minute after a start, "Waiting for the
  card...". The lists stay while you move between the stats and the
  albums opened from them, and are counted again after another tab, Now
  Playing or the dark screen: a play counted while a page is open shows
  the next time. With the card out they say so, and offer Try again
  once it is back ("The card is back").
- **Before the clock is set, and in the first month.** Plays get a date
  only once a computer has set the player's clock (0.14.0's `@time`,
  which the terminal player's MP3 Player tab sends). Until then the
  time listened, Most played (All time) and Recently played work ("This
  session", then "Earlier"), and the rest says it needs dates ("Plug in
  to the MP3 Player tab"), but Not played yet on a card whose dated
  plays span a month. New finds, Old favourites and Not played yet open
  after a month of counting ("Opens 6 Dec, after a month of plays"),
  and Most played offers 7 days after a week and 30 days after a month.
  Days are UTC for now: Last 7 days says so.
- **Headphones connect sooner.** After a restart or a power-on the
  player can be reached, and calls them, a third of a second after
  Bluetooth is up, not 10 s later: on the Core2 they linked 6.6 to
  7.1 s after a restart instead of 14.7 to 17.3 s, and 4.5 to 5.2 s
  after a minute off instead of 14.6 to 15.6 s. Connect, a B hold or a
  play now tries 8 times, about a minute, instead of 3 ("try 2 of 8"
  on Output): headphones connected to a phone hardly answer anyone else
  (twice they answered only after 38 and 41 s), so disconnecting them
  there first is still quicker. "Couldn't connect" comes after about a
  minute, not 20 to 25 s; a tap still cancels the wait.
- **No more 5 s stalls on a nearly full card.** On a card with almost
  no room left (one free cluster on a 32 GB card), every second save of
  the queue held the player about 5.3 s, long enough to break the music
  (148 underruns after one); each save now takes 135 to 146 ms. The fix
  acts only with about 3.9 MB or less free on such a card. The first
  write after a computer filled the card still takes 5.3 to 5.5 s, at
  the start that follows.
- **More memory to spare when the headphones link at a start.** The
  player's background work on the card now waits while the headphones'
  link is set up and until their remote control is ready, and each open
  file's 4 KB buffer goes to the Core2's larger memory first. In the
  start that ran lowest (an MP3 playing as they linked during the walk
  of the card) the least free internal memory rose from 35.9 to 45.1 KB
  in a silent test: 42.8 KB with the listening stats, 50.6 to 50.8 KB
  with the headphones really playing. The speaker was muted, and the
  headphones played an album of silence. New music on the card shows
  about 4 s later after a link, and an MP3 at 320 kbit/s takes half a
  point more of a core (25.5 % instead of 25 %), a CD-quality FLAC
  (16-bit, 44.1 kHz) about 1.5 points more (estimated).
- **An empty Now Playing no longer redraws itself all the time.** Since
  0.14.0, Now Playing with nothing queued and a card in drew its whole
  "Nothing playing" screen again on every pass while the screen was
  lit: touches on that screen were read about 8 times less often (the
  loop's 16 passes a second, now 123 to 140), and Auto DJ's join at a
  start took 5 to 6.5 s instead of under 2 s (on the Core2, 6.2 to 6.4 s
  before the listening stats came in and 5.3 s with them, now 1.6 s).
  It is drawn once again, and again only when it changes.
- **The new messages** (the listening stats', 84 of them) are in all
  ten languages. Eleven are the mStream app's own words, as its
  translations have them, with 20 of those shortened in seven languages
  where they didn't fit; the other 73 were translated by Claude agents
  in the nine languages other than English, each language reviewed by
  a second. No native speaker has read them yet.
- Tested on a Core2 v1.3 (2026-11-17 to 25), in a silent test mode
  (but for three runs with the headphones really playing), through the
  console, with nobody's finger on the screen but to tap Connect for
  the headphones' asks. Before they were merged, on a spare 32 GB card
  with a made-up library (19,417 tracks) and made-up play histories:
  each start's count saved before the history's files were cleared
  after a sync made by a test tool; the history's rotations (in a test
  build keeping 8 plays), each saved first, no play lost or counted
  twice; a full history's first start; the nearly full card; the lists
  equal to a reference worked out on a PC; every stats page in each of
  its states, drawn, opened and timed (79 screenshots read beside the
  designs), Most played's tray tapped by a scripted finger; the
  headphones' connect times (one pair, by a build that left the card
  alone; an ask with 3 and with 6 tries) and a start with an MP3
  playing as they linked; the empty Now Playing; each with the numbers
  given here. Then, with this release's code and nothing played, the
  1 TB music card (19,671 tracks), which an earlier test build had
  begun counting: the 15 tracks it had logged since counted (12 of them
  plays), a sync, the history's file cleared at the next start, and the
  track lists, each as worked out beforehand on a PC from copies of the
  card (Not played yet counted on the player alone). Last, the update
  itself (2026-11-25), on the spare card with its stats files taken off
  as 0.14.0 leaves cards: the published 0.14.0 played three tracks
  from it and paused a fourth; this release's first start then counted
  the 6,682 tracks the history held (5,889 of them plays and 777 skips;
  6,537 made up on a PC, the other 145 this Core2's own test plays) and
  saved them in 1.7 s (the save 1.1 s of it), its stats files equal
  byte for byte to those worked out beforehand on a PC, with the
  settings, the paired headphones, the queue at its paused second and
  the library kept; the next start changed nothing.
  **Not checked on the Core2 for this release:** going back to 0.14.0;
  the update with a full history (about 13,000 tracks) on a card with
  no stats yet (that start was measured over made-up stats already on
  the card); a real finger on the stats pages, and their haptics; a card
  pulled and put back with a stats page open; the clock set while a
  page shows, a UTC midnight on a page, and the stats with the clock
  unset (drawn only from made-up figures); the nine other languages on
  the screen (their fits were measured on a computer); the stats while
  Bluetooth plays; a connect with 8 tries, a play waiting for the
  headphones, and headphones that open their own link just as a start's
  walk of the card begins (only the host tests reach that wait); a queue
  of over 400 tracks on a nearly full card; the idle power-off on
  battery; and a long listening session.
- **The firmware is a little bigger:** about 4.70 MB, up from 4.52 MB
  in 0.14.0, most of it the listening stats and their messages (75 % of
  the space it has on the flash, 72 % before). The steps below are the
  same.
- **The update from 0.14.0** keeps the settings, the paired headphones,
  the queue and the library (their files are the same: no rebuild). Its
  first start with a card 0.14.0 played from counts the play history
  0.14.0 kept, so the stats begin with those plays (up to about 13,000),
  counting since the day of the first one with a date (with none, the
  day of the update if the clock is set; with it unset, the day the card
  was last synced, or else the first day it plays or starts with the
  clock set). With a full history that start takes longer, once a card:
  on the Core2, 13,106 plays (made up, on a card whose stats already
  held 19,000 made-up tracks) were counted in 1.2 to 1.4 s and saved in
  2.5 to 2.7 s, a track asked for meanwhile started 4.4 to 4.8 s late,
  and the next start tidied the files in 1.3 to 1.4 s. From 0.13.0 it
  is the same, with 0.14.0's changes (Touch when off, the clock: read
  its notes) and stats that start at the first play; from older
  versions, read their notes too.
- **Going back to 0.14.0 later** keeps the settings, the paired
  headphones, the queue and the library. 0.14.0 leaves the stats' files
  alone and goes on keeping the play history, which 0.15.0 counts when
  it comes back; but on a card synced meanwhile 0.14.0 clears the
  synced history without counting it, so the stats miss those plays
  (none is counted twice). An ask for the headphones has 3 tries again
  (from the code: not tried on the Core2).

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
  empty `/music`. Formatting erases the whole card, the listening stats
  with it: copy anything on it off first.
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
  read yet, Auto DJ's and the listening stats' texts included; the
  serial console is in English, but for the lines that quote the
  screen. In three of Auto DJ's messages on two lines in Japanese and
  Chinese, shown only with an Auto DJ index, the first line touches the
  message's outline.
- **Auto DJ's matching, the waveforms and the mood filters need files
  only the transfer software writes:** the mStream terminal player's
  MP3 Player tab (its 0.16.0 and later) puts them on the card at a
  sync. On a card filled another way, Auto DJ adds random tracks from
  the library, Now Playing shows the plain line, the Albums list has no
  Filters row, and Auto DJ's Smart filters row is dim. With an index, tracks it doesn't
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
- **The listening stats are the card's.** They count only the plays
  from this card, whichever player made them, and live only on it: a
  format on the player (or `/.player` deleted) starts them again, and
  another card has its own. Days are UTC days until the player is told
  the time zone: in the evening west of UTC, or the morning east of it,
  "today" isn't the listener's. Plays made while the clock was unset
  count in the totals, Most played (All time) and Recently played, but
  in no day. A renamed or converted file is a new track to them: its
  old plays stay in the totals and leave the lists. Very heavy
  listening (over 4,096 tracks in a month, a track counted once for
  each day it played) shortens Most played's 30 days, which then says
  how many days it holds ("23 days"). Not played yet can very rarely
  hide an album with no play (at most about a 1 in 10,000 chance with
  20,000 tracks, more in a larger library, estimated; an album hidden
  this way stays hidden); it never shows a played one. Past 18,432
  albums the lists can't be counted ("Not enough memory right now").
  The stats wait for the card ("Waiting for the card...") for about a
  minute after a start, and on a fresh card or after a Rescan until the
  tags have been read: minutes for 20,000 tracks (3 to 6, estimated).
  Opening the Dance tab lets their lists go: they are counted again on
  the way back. A stats file the card can't read keeps the stats off,
  their row gone, until a disk check on a computer mends it; a damaged
  one is put aside as `stats.bad`, and counting starts again from the
  play history.
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
