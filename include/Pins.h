#pragma once
//
// Central pin map. Values come from build flags (platformio.ini) so the same
// header serves the Wokwi sim and the real board. Defaults below are a safety
// net for editors/intellisense; the build flags are the source of truth.
//
// Display pins (TFT_MOSI/SCLK/CS/DC/RST/MISO/BL) are owned by TFT_eSPI and set
// via its own -D flags — they are intentionally NOT redefined here.

#ifndef PIN_SD_CS
#define PIN_SD_CS 7
#endif

#ifndef PIN_ENC_CLK
#define PIN_ENC_CLK 4
#endif
#ifndef PIN_ENC_DT
#define PIN_ENC_DT 5
#endif
#ifndef PIN_ENC_SW
#define PIN_ENC_SW 6
#endif

#ifndef PIN_BTN_PLAY
#define PIN_BTN_PLAY 15
#endif
#ifndef PIN_BTN_PREV
#define PIN_BTN_PREV 16
#endif
#ifndef PIN_BTN_NEXT
#define PIN_BTN_NEXT 17
#endif

// Dock-detect line. In the sim this is a pushbutton (toggle = dock/undock).
// On hardware it becomes USB VBUS / host-enumeration detection (see SimDock).
#ifndef PIN_DOCK
#define PIN_DOCK 18
#endif

// Shared SPI bus (display + SD card) — matches the TFT_eSPI flags above.
#define PIN_SPI_SCLK 12
#define PIN_SPI_MOSI 11
#define PIN_SPI_MISO 13
