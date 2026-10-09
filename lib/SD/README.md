# SD (arduino-esp32 3.3.12, patched)

The Arduino core's SD library, copied from framework-arduinoespressif32
3.3.12 (`libraries/SD`, https://github.com/espressif/arduino-esp32/tree/3.3.12/libraries/SD)
with one file changed. A library named `SD` in the project's `lib/` comes
before the framework's in PlatformIO's search, so `#include <SD.h>` and
`<sd_diskio.h>` find this copy and the framework's isn't compiled (the
build's dependency graph lists `SD @ 3.3.12+mstream.1`).

Licence: Apache-2.0, as each file's header says
([LICENSES/Apache-2.0.txt](../../LICENSES/Apache-2.0.txt)): "Copyright
2015-2016 Espressif Systems (Shanghai) PTE LTD"; `sd_diskio_crc.c`
"Copyright (c) 2014 Neil Thiessen". THIRD-PARTY-NOTICES.md lists it.

## What changed

Only `src/sd_diskio.cpp`; its header says so, and every change is marked
`mstream-mp3-player:`. The other files are the framework's, byte for byte
(SHA-256 of the originals in 3.3.12):

| File | SHA-256 |
|---|---|
| SD.cpp | 9728e9fd4abe200048291384b7ffbf12d2e8fe182f3685171442767565bc4fe0 |
| SD.h | 191d3051c5d8281986ce6a94da9c2291ab4ba365c6d35679720e5f854dd3de1f |
| sd_defines.h | 59073605b7170498247daba456b07788c1bc6a62c21d6738550daec260502e36 |
| sd_diskio.cpp (before the patch) | fda00b53f23d4d93ab63a506b3197f27e24278a3f0d5da9c7590fff5ec37aa05 |
| sd_diskio.h | 6dafcfea585b34ae3bdf0fe92131e0287836ef0a2169b67eadee02de2c8d393a |
| sd_diskio_crc.c | c0305b3157516d016d68751786cbfe318b2367b35231a1016d64d636507efbc3 |

The patch (docs/METADATA.md 3.8; the rules are lib/core `SdBusy.h`,
host-tested in test_sd_busy):

- **`sdWait()`**: the card is ready after two bytes of 0xFF in a row. The
  stock loop returned on the first byte that wasn't 0x00, which can be the
  pull-up's bits before a just-selected busy card drives DO, or the byte a
  card sends after a Stop Tran token before its busy starts.
- **The writes wait out the busy before they deselect**: a single block's
  programming after its data response, and a multiple write's after the
  Stop Tran token (its one byte read first). The stock code deselected at
  once and sent CMD13 to a card still busy, whose response loop then took
  the busy line's rising edge for the R1: 0x01 (a write that failed with
  no line), 0x03 or 0x07 ("token error"), 0x0F to 0x7F ("crc error").
- **`ff_sd_status()`** asks CMD13 again once after an R1 with an error bit
  before it reports `STA_NOINIT`, which makes FatFs mount the volume again
  (every open file gone). No answer at all (0xFF) is reported at once.

Upgrading the Arduino core: compare its `libraries/SD/src/sd_diskio.cpp`
with the hash above. If it changed, copy the new library here and apply
the marked changes again (or drop this copy if upstream waits for 0xFF).
