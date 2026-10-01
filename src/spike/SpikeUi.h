// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <M5GFX.h>
#include <esp_heap_caps.h>

#include <new>
#include <utility>

#include "TextFold.h"
#include "app/Psram.h"
#include "ui/LcdLock.h"

// Shared bits of the UI spike (docs/UI-SPIKE.md): the tab bar design's
// colours and regions, a stand-in tab bar, text fitting, and PSRAM
// allocation. The spike's objects live in PSRAM (psramNew), so they cost no
// internal RAM until used, and little then.
namespace spike {

// Colours (tab bar spec §3.4), RGB565.
namespace col {
constexpr uint16_t BG = 0x0862;
constexpr uint16_t SURF = 0x10C4;
constexpr uint16_t HEAD = 0x10A3;
constexpr uint16_t CARD = 0x18E5;
constexpr uint16_t ROW_SEL = 0x2147;
constexpr uint16_t ACT_BG = 0x29A8;
constexpr uint16_t BTN = 0x2988;
constexpr uint16_t BTN_HI = 0x3A2B;
constexpr uint16_t DIV = 0x2167;
constexpr uint16_t ROW_DIV = 0x18E5;  // #1A202C
constexpr uint16_t TXT = 0xF79F;
constexpr uint16_t SOFT = 0xCE7B;
constexpr uint16_t DIM = 0x8CB5;
constexpr uint16_t FAINT = 0x5B0E;
constexpr uint16_t ICON = 0x9516;
constexpr uint16_t CORAL = 0xFB49;
constexpr uint16_t CORAL_DK = 0x1861;
constexpr uint16_t CYAN = 0x3EBE;
constexpr uint16_t AMBER = 0xFE07;
constexpr uint16_t RED = 0xFA6B;
constexpr uint16_t GREEN = 0x4ECF;
}  // namespace col

// Regions (spec §3.1).
constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kBarH = 36;       // tab bar, y 0-35
constexpr int kTabW = 56;       // 5 tabs, x 0-279
constexpr int kChipX = 280;     // battery / volume chip, 280-319
constexpr int kHeaderY = 36;    // header row, y 36-71
constexpr int kListY = 72;      // list viewport, y 72-239
constexpr int kRowH = 42;
constexpr int kRailX = 290;     // A-Z rail, x 290-319

enum class Tab : uint8_t { NowPlaying, Library, Queue, Dance, Output };
const char* tabName(int tab);

// The tab bar at the top, as the spec draws it, with the grafts: a text
// label under the active tab, and a volume chip ("60%" over a thin battery
// gauge) in the corner. `active` -1: none highlighted. It takes no bus lock
// of its own: on the LCD, draw it under one LcdLock (one ~36 px band, a few
// ms of bus), so the hold is short and counted.
void drawTabBar(lgfx::LovyanGFX& g, int active, int volumePercent = 60, int batteryPercent = 87);

// Fills rows [y, y + h) of the LCD in bands of at most kFillBand rows, each
// under its own LcdLock: a full-screen clear is ~31 ms of bus in one piece,
// which the SD card (and so the decoder) would wait out.
constexpr int kFillBand = 40;
void fillLcd(int y, int h, uint16_t colour, SpiHoldStats* stats = nullptr);

// Folds `in` (UTF-8) for a 7-bit GFX font and cuts it with "..." to fit
// `maxW` px in g's current font. Returns `out`.
const char* fitText(lgfx::LovyanGFX& g, const char* in, size_t inLen, char* out, size_t outSize, int maxW,
                    textfold::Mode mode = textfold::Mode::Full);
// Cuts `text` (UTF-8, NUL-terminated, in a buffer of `size` bytes) in place
// with "..." so it fits `maxW` px in g's current font, at a code point
// boundary. The width pass every list row needs, whatever the font.
void ellipsize(lgfx::LovyanGFX& g, char* text, size_t size, int maxW);
// Copies the first `inLen` bytes of `in` as they are (no folding; cut at a
// code point boundary if `out` is short) and ellipsizes: fitText for
// Unicode fonts.
const char* fitRaw(lgfx::LovyanGFX& g, const char* in, size_t inLen, char* out, size_t outSize, int maxW);

// PSRAM allocation (app/Psram.h), under the spike's names too.
using ::psramAlloc;
using ::psramDelete;
using ::psramFree;
using ::psramNew;

// Internal-RAM free now and lowest since boot, in bytes.
uint32_t internalFree();
uint32_t internalMinEver();

}  // namespace spike
