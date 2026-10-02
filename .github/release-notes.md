**{{VERSION}} is a {{CHANNEL}}.** It works on the hardware below, and things
may still change between versions, including what it saves on the device.
Bug reports are welcome in the [issues]({{REPO_URL}}/issues): please
include the Version row from **Output > About** (it reads {{VERSION}},
ELF {{ELF}} for this build; ELF {{DIO_ELF}} for the `-dio-full.bin`).

Source for this binary: {{SOURCE_URL}}, and with its libraries in
`{{SOURCE_TAR}}` below.

## What's new in 0.6.0

- **Gapless playback:** albums mixed without gaps (live sets, DJ mixes)
  play straight through, MP3 (with a LAME header) and FLAC alike.
- **Resume and seeks to the exact spot:** after a restart a track picks up
  at the second it paused at, VBR MP3s included.
- **Steadier MP3 decoding:** the decoder's working memory stays in the
  faster half of the PSRAM, so its speed no longer varies from boot to boot.
- **The dancer keeps the beat better:** the beat tracker was reworked and
  scored on a 77-track library. It is on the beat more of the time and
  locks onto a wrong beat less often.
- **USB visualizer (a novelty):** with the Core2 plugged into a computer,
  `tools/usb_viz.py` (in the source) plays a song on the computer and sends
  the Core2 its beat, and the crab dances to it (docs/USB-VISUALIZER.md).
- **Faster flash mode (QIO)** by default, with the `-dio-full.bin`
  fallback below, and a second round of power savings.

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

- Format it **FAT32** (MBR). exFAT cards, which is how most cards of 64 GB
  and up come, and GPT cards don't mount: the player says "This card isn't
  FAT32". Windows only offers FAT32 up to 32 GB; for a bigger card use a
  FAT32 formatting tool.
- Put the music under **`/music`**, e.g. `/music/Artist/Album/01 - Title.mp3`.
  An album's cover is the `cover.jpg` (or `folder.jpg`) next to its tracks.
- Pair headphones from **Output > Pair new headphones**. The player never
  pairs with anything by itself.

## Known limits

- MP3 and FLAC only.
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
