# Third-party notices

The mstream-mp3-player firmware for the M5Stack Core2 is Copyright (C) 2026
IrosTheBeggar. It is free software under the GNU General Public License,
version 3 or (at your option) any later version (GPL-3.0-or-later; the text
is [LICENSE](LICENSE)), and it comes with NO WARRANTY. The source is at
https://github.com/IrosTheBeggar/mstream-mp3-player.

The firmware binary also contains the third-party components below. Each
entry gives the exact version, its licence, the copyright lines found in its
sources, and where it comes from. The full licence texts are in
[LICENSES/](LICENSES); GPL-3.0 is [LICENSE](LICENSE). Every licence here is
compatible with distributing the whole under GPL-3.0-or-later.

A binary release (firmware.bin, or firmware.factory.bin, the merged image
flashed at 0x0) is built from the tagged source with the versions pinned in
`platformio.ini`. Its Corresponding Source is that tag plus the library and
framework versions listed here. Each release also carries
`mstream-player-core2-<version>-source.tar.gz` (tools/package_release.py):
this repository's files at the tag, the ESP8266Audio checkout the build used
(GPL) and the Arduino core's sources it compiled (LGPL), so that source
stays available from the same place as the binary even if an upstream
download goes away.

## How this list was checked

Against a build of the `core2` environment (pioarduino 55.03.312-1:
Arduino-ESP32 3.3.12 on ESP-IDF 5.5.5, GCC 14.2.0) with `lib_archive = yes`,
so only the objects the firmware uses are linked:

- `.pio/build/core2/firmware.map`, "Linker script and memory map" section:
  the bytes each object file contributes to the image, summed per archive
  member (debug sections and zero-size sections left out). An object that is
  pulled into the link but garbage-collected to 0 bytes is not in the binary.
- `xtensa-esp32-elf-nm -C -S firmware.elf` for data inside a linked object
  (which fonts, the BMI270 blob, which codecs).
- The headers ESP-IDF ships for every linked component, searched for
  licence tags other than Apache-2.0 and for notices that must travel with
  a binary ("Redistributions in binary form"): that is how TinyCrypt, TLSF
  and FreeBSD's endian.h below were found. ESP-IDF's prebuilt libraries
  come without their sources, so a notice only in a .c file would not show
  up this way.

Linked and listed below: ESP8266Audio's MP3 (libmad), FLAC (libFLAC) and
Opus (libopus) decoders and file source; ESP32-A2DP's source and common classes;
M5Unified; M5GFX (with LovyanGFX, five FreeFont faces, the TFT_eSPI fonts 2
and 4, the Adafruit glcdfont, TJpgDec); the Arduino core with FS, SD,
LittleFS, SPI, Wire and Preferences; ESP-IDF and its binary Bluetooth, PHY,
RTC and coexistence libraries; newlib; the GCC runtime.

Not in the binary (0 bytes, or no symbols): ESP8266Audio's Helix MP3/AAC
(RealNetworks RPSL/RCSL), its AudioGeneratorOpus (libopus itself is linked,
below, behind our own Ogg reader), its ID3 tag source (AudioFileSourceID3:
the player skips the tags by their size itself), TinySoundFont, MOD, MIDI,
WAV and HTTP sources; ESP32-A2DP's sink and the AudioTools parts; M5GFX's efont, IPA
fonts, PNG (pngle), QR code, QOI and miniz; the libhelix-mp3 component of
the prebuilt ESP-IDF libraries; WiFi (net80211, pp, wpa_supplicant, lwIP,
mesh, ESP-NOW: pulled into the link, then collected to 0 bytes); the Arduino
core's HTTPClient, Network and SPIFFS; Mbed TLS itself (only Espressif's
SHA-256 port file is linked, see ESP-IDF). The Unity test framework is used
by the host tests only.

A new dependency, a version bump or a change of what the firmware calls can
change this list: check the map again before a release.

## Libraries (PlatformIO `lib_deps`)

### ESP8266Audio 2.4.2

- Version: git tag `2.4.2`, commit `74fc1f09bbba5e5c5450b445452ba64ef2d8bbad`
  (its library.json still says 2.4.1).
- Licence: GPL-3.0-or-later ([LICENSE](LICENSE)).
- Copyright: "Copyright (C) 2017 Earle F. Philhower, III" (the headers of
  AudioGeneratorMP3.cpp, AudioGeneratorFLAC.cpp, AudioFileSourceFS.cpp and
  the other linked files).
- Source: https://github.com/earlephilhower/ESP8266Audio
- Linked: AudioGeneratorMP3, AudioGeneratorFLAC, AudioFileSourceFS,
  AudioLogger, and the three decoders below.

#### libmad 0.15.1b (inside ESP8266Audio, src/libmad)

- Licence: GPL-2.0-or-later ([LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt),
  from src/libmad/COPYING); used here under GPL-3.0 as its "any later
  version" allows.
- Copyright: "Copyright (C) 2000-2004 Underbit Technologies, Inc."
  (src/libmad/COPYRIGHT and every source file). Ported to the ESP8266 by
  Earle F. Philhower, III (src/libmad/README.ESP8266;
  https://github.com/earlephilhower/libmad-8266).
- Source: https://www.underbit.com/products/mad/ (upstream), the
  ESP8266Audio commit above (as built).
- Linked: bit, fixed, frame, huffman, layer3, stream, synth, timer.

#### libFLAC 1.3.2 (inside ESP8266Audio, src/libflac)

- Licence: BSD-3-Clause, Xiph.Org variant
  ([LICENSES/BSD-3-Clause-libFLAC.txt](LICENSES/BSD-3-Clause-libFLAC.txt),
  from src/libflac/COPYING.Xiph). md5.c is in the public domain (written by
  Colin Plumb; "no copyright is claimed").
- Copyright: "Copyright (C) 2000-2009 Josh Coalson", "Copyright (C)
  2011-2016 Xiph.Org Foundation" (COPYING.Xiph and the source files).
- Source: https://github.com/xiph/flac (upstream), the ESP8266Audio commit
  above (as built; "LIBFLAC 1.3.2 ported to the ESP8266",
  src/libflac/README.ESP8266).
- Linked: bitreader, cpu, crc, fixed, format, lpc, md5, memory,
  stream_decoder.

#### libopus 1.5.1 (inside ESP8266Audio, src/libopus)

- Version: the sources of libopus v1.5.1 (ESP8266Audio's `lib/opus`
  submodule at `ab4e83598e7fc8b2ce82dc633a0fc0c452b629aa`, tag v1.5.1,
  copied into src/libopus by its lib/install-opus.sh; the bundled
  src/libopus/include/config.h claims 1.5.2, so `opus_get_version_string()`
  says "libopus 1.5.2-fixed"). The decoder only (no encoder, no multistream
  decoder, no repacketizer), fixed point (`FIXED_POINT`, `DISABLE_FLOAT_API`),
  its scratch on the stack (`VAR_ARRAYS`), compiled as every ESP8266Audio
  file is. Used through its public API alone (`libopus/include/opus.h`), by
  src/audio/OpusGenerator and our own Ogg Opus reader (lib/core/OggPage,
  OggOpus; docs/OPUS.md); ESP8266Audio's AudioGeneratorOpus is not linked.
- Licence: BSD-3-Clause, the Xiph/IETF variant
  ([LICENSES/BSD-3-Clause-libopus.txt](LICENSES/BSD-3-Clause-libopus.txt),
  from src/libopus/COPYING; its third clause names the Internet Society,
  IETF and IETF Trust). The same COPYING and src/libopus/LICENSE_PLEASE_READ.txt
  point to the royalty-free patent licences the Opus format and this
  implementation are subject to, the IPR statements filed with the IETF:
  Xiph.Org Foundation https://datatracker.ietf.org/ipr/1524/, Microsoft
  Corporation https://datatracker.ietf.org/ipr/1914/ (and Skype Limited's
  https://datatracker.ietf.org/ipr/1602/, which it supersedes), Broadcom
  Corporation https://datatracker.ietf.org/ipr/1526/. Patents asserted
  against Opus decoders outside those grants are the README's Opus section.
- Copyright: "Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
  Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark
  Borgerding, Erik de Castro Lopo, Mozilla, Amazon" (COPYING). In the linked
  files: "Copyright (c) 2006-2011, Skype Limited. All rights reserved."
  (silk/*), "Copyright (c) 2013, Koen Vos. All rights reserved."
  (silk/LPC_fit.c), "Copyright (c) 2003-2004, Mark Borgerding"
  (celt/kiss_fft.c), "Copyright (c) 2001-2011 Timothy B. Terriberry"
  (celt/entcode.c, entdec.c, entenc.c, cwrs.c), "Copyright (c) 2007-2008
  CSIRO", "Copyright (c) 2007-2009 Xiph.Org Foundation", "Copyright (c)
  2008 Gregory Maxwell", "Copyright (c) 2002-2008 Jean-Marc Valin" and
  similar year ranges (celt/*, src/*), "Copyright (C) 2001 Erik de Castro
  Lopo" (celt/float_cast.h); in a header compiled with them, "Copyright (c)
  2010 Xiph.Org Foundation / Copyright (c) 2013 Parrot" (celt/cpu_support.h).
  (include/opus_projection.h's "Copyright (c) 2017 Google Inc." is not in
  the binary: no linked object includes it.)
- Source: https://gitlab.xiph.org/xiph/opus (upstream; https://opus-codec.org/),
  the ESP8266Audio commit above (as built; the 172 bundled files that exist
  upstream are byte for byte v1.5.1's but for install-opus.sh's
  `HAVE_CONFIG_H` to `__STDC__` substitution).
- Linked (63 objects, about 75 KB of code and tables, nothing in IRAM,
  `.data` or `.bss`): src: opus, opus_decoder; celt: bands, celt,
  celt_decoder, celt_lpc, cwrs, entcode, entdec, entenc, kiss_fft, laplace,
  mathops, mdct, modes, pitch, quant_bands, rate, vq; silk: CNG,
  LPC_analysis_filter, LPC_fit, LPC_inv_pred_gain, NLSF2A, NLSF_decode,
  NLSF_stabilize, NLSF_unpack, PLC, bwexpander, bwexpander_32, code_signs,
  dec_API, decode_core, decode_frame, decode_indices, decode_parameters,
  decode_pitch, decode_pulses, decoder_set_fs, gain_quant, init_decoder,
  lin2log, log2lin, pitch_est_tables, resampler, resampler_private_AR2,
  resampler_private_IIR_FIR, resampler_private_down_FIR,
  resampler_private_up2_HQ, resampler_rom, shell_coder, sort,
  stereo_MS_to_LR, stereo_decode_pred, sum_sqr_shift, table_LSF_cos,
  tables_LTP, tables_NLSF_CB_NB_MB, tables_NLSF_CB_WB, tables_gain,
  tables_other, tables_pitch_lag, tables_pulses_per_block.

### ESP32-A2DP 1.8.11

- Version: git tag `v1.8.11`, commit `b6da4744286ca15b6f1dee607c077a4747294dd4`.
- Licence: Apache-2.0 ([LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt),
  the library's own LICENSE file). It has no NOTICE file.
- Copyright: "Copyright 2020 Phil Schatzmann", "Copyright 2015-2016 Espressif
  Systems (Shanghai) PTE LTD" (src/BluetoothA2DPSource.cpp).
- Source: https://github.com/pschatzmann/ESP32-A2DP
- Linked: BluetoothA2DPSource, BluetoothA2DPCommon. src/audio/BtSink.cpp
  subclasses them and restates a few of their file-local constants.

### M5Unified 0.2.23

- Version: PlatformIO registry `m5stack/M5Unified@0.2.23`.
- Licence: MIT ([LICENSES/MIT-M5Stack.txt](LICENSES/MIT-M5Stack.txt), its
  LICENSE file).
- Copyright: "Copyright (c) 2021 M5Stack" (LICENSE); "Copyright (c) M5Stack.
  All rights reserved." (source headers).
- Source: https://github.com/m5stack/M5Unified
- Linked: M5Unified.cpp (power, speaker, buttons, touch, IMU and the rest).
- src/audio/SpeakerSink takes the idea of its buffer handshake (a buffer is
  refilled only after the speaker task released it) from M5Unified's
  example examples/Advanced/MP3_with_ESP8266Audio (MIT, as above); the code
  is our own.

#### Bosch BMI270 configuration data (inside M5Unified)

- `m5::bmi270_config_file`, 8 KB of microcode for the BMI270 IMU, linked as
  part of M5Unified.cpp (src/utility/imu/BMI270_config.inl, which carries no
  notice of its own). The Core2 v1.3 this was tested on has a BMI270.
- Origin: Bosch Sensortec GmbH's BMI270 Sensor API,
  https://github.com/boschsensortec/BMI270_SensorAPI. The 8,192 bytes are
  identical to `bmi270_config_file` in its bmi270.c at the tags v2.71.8,
  v2.86.1 and v2.113.0 (compared byte for byte).
- Licence: BSD-3-Clause, "Copyright (c) 2023 Bosch Sensortec GmbH. All
  rights reserved." ([LICENSES/BSD-3-Clause-Bosch-BMI270.txt](LICENSES/BSD-3-Clause-Bosch-BMI270.txt),
  its LICENSE file at v2.113.0; v2.71.8's says 2021, otherwise the same).

### M5GFX 0.2.30

- Version: PlatformIO registry `m5stack/M5GFX@0.2.30`.
- Licence: MIT ([LICENSES/MIT-M5Stack.txt](LICENSES/MIT-M5Stack.txt); its
  LICENSE file is the same text as M5Unified's).
- Copyright: "Copyright (c) 2021 M5Stack" (LICENSE); "Copyright (c) M5Stack.
  All rights reserved." (source headers).
- Source: https://github.com/m5stack/M5GFX
- Linked: M5GFX.cpp, lgfx_v1.cpp (LovyanGFX), lgfx_tjpgd.c.

It contains:

- **LovyanGFX 1.2.30** (src/lgfx/v1: the drawing, font and panel code;
  the version is M5GFX's src/lgfx/v1/gitTagVersion.h). Licence: FreeBSD
  (BSD-2-Clause), as its headers say ("Licence: [FreeBSD]
  (https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)").
  "Copyright (c) 2020 lovyan03 (https://github.com/lovyan03)"; contributors
  ciniml, mongonta0716, tobozo (headers). M5GFX does not ship LovyanGFX's
  license.txt; [LICENSES/LovyanGFX.txt](LICENSES/LovyanGFX.txt) is that file
  from LovyanGFX's tag 1.2.30 (commit 3527e99d), which also carries the
  Adafruit_ILI9341, Adafruit_GFX and TFT_eSPI licences it started from.
  Source: https://github.com/lovyan03/LovyanGFX
- **Fonts 2 and 4 from TFT_eSPI** (`chrtbl_f16`, `chrtbl_f32`: M5GFX's
  Font16.h and Font32rle.h, no header of their own). Licence: FreeBSD,
  "Copyright (c) 2023 Bodmer (https://github.com/Bodmer)"; TFT_eSPI started
  from Adafruit_ILI9341 (MIT) and Adafruit_GFX (BSD). Text:
  [LICENSES/TFT_eSPI.txt](LICENSES/TFT_eSPI.txt), TFT_eSPI 2.5.43's
  license.txt. Source: https://github.com/Bodmer/TFT_eSPI
- **Adafruit GFX glcdfont** (`lgfx::v1::fonts::font`, Font0, 1280 bytes;
  src/lgfx/Fonts/glcdfont.h). Licence: BSD, "Copyright (c) 2012 Adafruit
  Industries. All rights reserved."
  ([LICENSES/BSD-Adafruit-GFX.txt](LICENSES/BSD-Adafruit-GFX.txt), M5GFX's
  src/lgfx/Fonts/GFXFF/license.txt). Source:
  https://github.com/adafruit/Adafruit-GFX-Library
- **GNU FreeFont faces in Adafruit GFX format** (FreeSans9pt7b,
  FreeSans12pt7b, FreeSansBold9pt7b, FreeSansBold12pt7b,
  FreeSansBold18pt7b; src/lgfx/Fonts/GFXFF, used by the spike labs in
  src/spike). Converted by Adafruit's fontconvert (the
  GFXFF/license.txt above is Adafruit's). GNU FreeFont itself is
  GPL-3.0-or-later with the GNU font exception; the font files in M5GFX
  carry no notice and M5GFX ships no FreeFont licence file, so the licence
  statement and exception are in [LICENSES/GNU-FreeFont.txt](LICENSES/GNU-FreeFont.txt),
  from https://www.gnu.org/software/freefont/license.html. GPL-3.0 is
  [LICENSE](LICENSE): the firmware, under GPL-3.0-or-later too, needs no
  exception to include them.
- **TJpgDec R0.01c** (lgfx_tjpgd.c, JPEG decoding for the album covers).
  "(C)ChaN, 2019", "Copyright (C) 2019, ChaN, all right reserved"; modified
  for LGFX by lovyan03, 2020. Its own permissive licence
  ([LICENSES/TJpgDec.txt](LICENSES/TJpgDec.txt), the file's header).
  Source: http://elm-chan.org/fsw/tjpgd/00index.html
- **result.hpp** (src/lgfx/utility/result.hpp, header-only, compiled into
  lgfx_v1.cpp). MIT, "Copyright (c) 2017-2021 Matthew Rodusek All rights
  reserved." ([LICENSES/MIT-result-hpp.txt](LICENSES/MIT-result-hpp.txt),
  the file's header).

## Framework and platform

### Arduino core for the ESP32 3.3.12

- Version: framework-arduinoespressif32 3.3.12 (pioarduino platform
  55.03.312-1).
- Licence: the package declares LGPL-2.1-or-later
  ([LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt); the package doesn't
  ship its LICENSE.md, so this copy of the LGPL-2.1 text is libFLAC's
  COPYING.LGPL from ESP8266Audio). Many of its files are Apache-2.0 by
  their own headers (below). The firmware's full source is public, so it
  can be relinked against a modified core (LGPL-2.1 section 6).
- Copyright lines in the linked files:
  - LGPL-2.1-or-later: "Copyright (c) 2008 David A. Mellis" (Print.cpp,
    Stream.cpp); "Copyright (c) 2009-10 Hernando Barragan", "Copyright
    2011, Paul Stoffregen" (WString.cpp); "Copyright (c) 2014 Ivan
    Grokhotkov" (stdlib_noniso.c); "Copyright (c) 2015 Ivan Grokhotkov"
    (Esp.cpp); "Copyright (c) 2015 Hristo Gochkov" (SPI.cpp); "Copyright
    (c) 2006 Nicholas Zambetti" (Wire.cpp).
  - Apache-2.0: "Copyright 2015-2026 Espressif Systems (Shanghai) PTE LTD"
    and similar year ranges (esp32-hal-uart.c, esp32-hal-i2c-ng.c,
    esp32-hal-i2c-slave.c, esp32-hal-periman.c and the other esp32-hal
    files, sd_diskio.cpp, vfs_api.cpp, LittleFS.cpp, Preferences.cpp);
    "Copyright (c) 2014 Neil Thiessen" (sd_diskio_crc.c).
  - HardwareSerial.cpp and chip-debug-report.cpp carry no notice (the
    package's LGPL-2.1-or-later applies).
- Source: https://github.com/espressif/arduino-esp32/tree/3.3.12
- Also in the merged image: boot_app0.bin (at 0xE000), from this package.

### ESP-IDF 5.5.5

- Version: v5.5.5, commit b774170ff46, precompiled in
  framework-arduinoespressif32-libs 5.5.5+sha.b774170ff46 (its
  versions.txt), plus its bootloader (bootloader.bin at 0x1000 in the
  merged image).
- Licence: Apache-2.0 ([LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt));
  "SPDX-FileCopyrightText: ... Espressif Systems (Shanghai) CO LTD" in its
  files. The full list of its components' licences:
  https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32/COPYRIGHT.html
- Source: https://github.com/espressif/esp-idf/tree/v5.5.5
- Linked components with their own licence or copyright:
  - **Bluedroid** (libbt, the Bluetooth host: A2DP, AVRCP, GAP). Apache-2.0
    (the SPDX tags in its shipped headers are all Apache-2.0); copyright
    lines there: Espressif Systems, Intel Corporation, Nordic Semiconductor
    ASA, Google, Inc., Vikrant More. Except:
    - **TinyCrypt** (bt/common/tinycrypt; Bluetooth pairing's AES, CCM,
      CMAC and P-256 ECDH). Linked: aes_encrypt, ccm_mode, cmac_mode, ecc,
      ecc_dh, utils (about 8 KB). Its headers carry Espressif's 2025
      Apache-2.0 SPDX tag above the original notices: BSD-3-Clause,
      "Copyright (C) 2017 by Intel Corporation, All Rights Reserved.", and
      for ecc and ecc_dh (from micro-ecc) also BSD-2-Clause, "Copyright (c)
      2014, Kenneth MacKay". Both notices: [LICENSES/TinyCrypt.txt](LICENSES/TinyCrypt.txt).
      Source: https://github.com/intel/tinycrypt,
      https://github.com/kmackay/micro-ecc
  - **FreeRTOS Kernel V10.5.1** (ESP-IDF SMP modified). MIT, "Copyright (C)
    2021 Amazon.com, Inc. or its affiliates. All Rights Reserved."
    ([LICENSES/MIT-FreeRTOS.txt](LICENSES/MIT-FreeRTOS.txt), from
    FreeRTOS.h).
  - **TLSF 3.1** (heap/tlsf.c, the heap allocator; espressif/tlsf at
    2867f688). BSD-3-Clause, "Copyright (c) 2006-2016, Matthew Conte"
    ([LICENSES/TLSF.txt](LICENSES/TLSF.txt): Espressif's copy carries only
    the SPDX tag, so the text is Matthew Conte's own notice from
    https://github.com/mattconte/tlsf).
  - **FatFs R0.15** (the SD card's FAT). "Copyright (C) 2022, ChaN, all
    right reserved.", ChaN's one-clause BSD-style licence
    ([LICENSES/FatFs.txt](LICENSES/FatFs.txt), from ff.h).
  - **esp_littlefs 1.22.3** (joltwallet/littlefs, the no-card fallback
    filesystem). MIT, "Copyright 2020 Brian Pugh"
    ([LICENSES/MIT-esp_littlefs.txt](LICENSES/MIT-esp_littlefs.txt), its
    LICENSE at the tag v1.22.3). Source: https://github.com/joltwallet/esp_littlefs
  - **littlefs** (inside esp_littlefs 1.22.3: its submodule at 6cb4e865).
    BSD-3-Clause, "Copyright (c) 2022, The littlefs authors." and
    "Copyright (c) 2017, Arm Limited. All rights reserved."
    ([LICENSES/BSD-3-Clause-littlefs.txt](LICENSES/BSD-3-Clause-littlefs.txt),
    its LICENSE.md at that commit). Source:
    https://github.com/littlefs-project/littlefs
  - **endian.h** (newlib/platform_include, byte-order macros and inline
    functions that compiled code may carry). Reworked from FreeBSD's
    sys/endian.h: BSD-2-Clause-FreeBSD, "Copyright (c) 2002 Thomas Moestl
    <tmm@FreeBSD.org>", with Espressif's and Francesco Giancane's changes
    under Apache-2.0 ([LICENSES/BSD-2-Clause-FreeBSD-endian.txt](LICENSES/BSD-2-Clause-FreeBSD-endian.txt)).
  - **Xtensa HAL and vectors** (libxtensa, libxt_hal). MIT: "Copyright (c)
    1999-2016 Tensilica Inc." ([LICENSES/MIT-Xtensa.txt](LICENSES/MIT-Xtensa.txt),
    from core-isa.h), "SPDX-FileCopyrightText: 2015-2019 Cadence Design
    Systems, Inc." (xtensa_context.h, MIT).
  - **esp_diagnostics 1.3.4** (espressif/esp_diagnostics). Apache-2.0,
    "SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD".
  - **Mbed TLS component**: only Espressif's own SHA port files
    (esp_sha256.c, sha.c) are linked, Apache-2.0.
  - The rest (hal, soc, esp_system, esp_hw_support, drivers, nvs_flash,
    spi_flash, esp_timer, espcoredump, pthread, vfs, log and others):
    Espressif, Apache-2.0.
- **Binary-only libraries** (object code, no source published), linked,
  each from the ESP-IDF v5.5.5 submodule commit named:
  - libbtdm_app (the Bluetooth controller): https://github.com/espressif/esp32-bt-lib
    (b4b7c54b).
  - libble_mesh (five small objects, about 4 KB):
    https://github.com/espressif/esp-ble-mesh-lib (e4476238).
  - libphy (RF calibration), librtc: https://github.com/espressif/esp-phy-lib
    (59c1234e).
  - libcoexist: https://github.com/espressif/esp-coex-lib (79e618f0).
  - Each repository's LICENSE file is the Apache License 2.0
    ([LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)), checked at those
    commits; esp32-bt-lib's and esp-phy-lib's READMEs add that the
    libraries are provided under it in "Object" form. No WiFi library
    (esp32-wifi-lib: net80211, pp, core and the rest) is in the binary.
  - They are needed to run the ESP32's radio at all. Whether they count as
    GPLv3 "System Libraries" on bare-metal firmware is not settled; linking
    them with GPL code is common practice for ESP32 firmware (ESP8266Audio
    itself targets the ESP32).

### newlib (C library) and the GCC runtime

- From the toolchain toolchain-xtensa-esp-elf 14.2.0+20260121 (Espressif's
  crosstool-NG build): libc and libm (newlib), libstdc++, libgcc.
- newlib: a collection of files under several BSD-style and similar
  licences, each with its own notice; the list is
  [LICENSES/newlib.txt](LICENSES/newlib.txt) (the toolchain's
  share/licenses/newlib/COPYING.NEWLIB).
- libstdc++ and libgcc: GPL-3.0-or-later with the GCC Runtime Library
  Exception 3.1 ([LICENSES/GCC-exception-3.1.txt](LICENSES/GCC-exception-3.1.txt),
  the toolchain's share/licenses/gcc/COPYING.RUNTIME), which lets a program
  compiled with GCC be distributed under its own terms.
- Source: https://github.com/espressif/crosstool-NG,
  https://github.com/espressif/newlib-esp32

## Fonts and art in this repository

- **DejaVu Sans and DejaVu Sans Bold 2.37**, rasterised to VLW by
  tools/vlw_font.py into src/ui/VlwFonts.cpp (16 and 13 px; Bold 16 and
  22 px; ASCII, Latin-1 and common Latin Extended-A). Licence: the Bitstream
  Vera Fonts licence ("Copyright (c) 2003 by Bitstream, Inc. All Rights
  Reserved. Bitstream Vera is a trademark of Bitstream, Inc."); DejaVu's
  changes are in the public domain; glyphs from Arev are "(c) Tavmjong Bah".
  Text: [LICENSES/DejaVu-Fonts.txt](LICENSES/DejaVu-Fonts.txt), checked
  against the licence text inside the TTF files used. Source:
  https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37
- **The UI icons** (src/ui/IconData.cpp): drawn for this project by
  tools/ui_icons.py (Pillow shapes and ASCII art in the script; no
  third-party images or icon fonts). GPL-3.0-or-later, like the rest.
- **The dancing crab** (lib/core/CrabArt.*, from tools/art/crab.json):
  drawn for this project. GPL-3.0-or-later.
- **The mStream logo** on the boot screen (lib/core/LogoArt.*, made by
  tools/make_logo.py from tools/art/mstream-logo.svg): the logo of
  mStream, the music server this player syncs from, by the same author
  (IrosTheBeggar). The SVG is mStream's webapp/assets/img/mstream-logo.svg,
  unchanged, as it is in mStream v6.24.0 (where it last changed in commit
  b0cfd005b331cdc2362d7e43bf24a0f0be175332, 2021-02-02; git blob
  164177d7a5283463bd48e82aae20bb31077a9581); the firmware has it rendered
  at 240 px and recoloured for the dark screen. Licence: mStream declares
  GPL-3.0 (its package.json); the logo's author, who is also this
  firmware's, licenses it here (the SVG copy and the files made from it)
  under GPL-3.0-or-later, like the rest of the firmware (the text is
  [LICENSE](LICENSE)). Source:
  https://github.com/IrosTheBeggar/mStream/blob/v6.24.0/webapp/assets/img/mstream-logo.svg
