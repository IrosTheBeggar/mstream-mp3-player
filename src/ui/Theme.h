// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "NavModel.h"

// The tab bar design's look (spec §3): colours, the screen's regions, and
// one accent colour per section (the review's graft: the active tab's icon,
// underline and label, the page header's rule and the primary buttons all
// carry their section's colour, so the colour says where you are).
namespace ui {

// Colours, RGB565 (spec §3.4).
namespace col {
constexpr uint16_t BG = 0x0862;        // #0A0C12 screen
constexpr uint16_t SURF = 0x10C4;      // #161B26 tab bar
constexpr uint16_t HEAD = 0x10A3;      // #10141D header row
constexpr uint16_t CARD = 0x18E5;      // #181D28 cards, sheets
constexpr uint16_t ROW_SEL = 0x2147;   // #222938 pressed / expanded / current row
constexpr uint16_t ACT_BG = 0x29A8;    // #2C3446 active tab plate
constexpr uint16_t BTN = 0x2988;       // #283040 secondary buttons
constexpr uint16_t BTN_HI = 0x3A2B;    // #3A445A pressed button, rail thumb, badge
constexpr uint16_t DIV = 0x2167;       // #262E3E dividers, tracks
constexpr uint16_t ROW_DIV = 0x18E5;   // #1A202C row dividers
constexpr uint16_t SELECTED = 0x3925;  // #3A242E a selected row (selection mode)
constexpr uint16_t HUD = 0x1926;       // #1E2432 the volume HUD
constexpr uint16_t TXT = 0xF79F;       // #F0F3F8
constexpr uint16_t SOFT = 0xCE7B;      // #C8CEDA
constexpr uint16_t DIM = 0x8CB5;       // #8C96A8
constexpr uint16_t FAINT = 0x5B0E;     // #586172
constexpr uint16_t ICON = 0x9516;      // #96A0B2 inactive tab icons
constexpr uint16_t CORAL = 0xFB49;     // #FF6B4A
constexpr uint16_t DARK = 0x1861;      // #1A0C08 text on an accent
constexpr uint16_t CYAN = 0x3EBE;      // #38D6F0 Bluetooth connected
constexpr uint16_t AMBER = 0xFE07;     // #FFC23D connecting, paused, warnings
constexpr uint16_t RED = 0xFA6B;       // #FF4D5E destructive, lost
constexpr uint16_t GREEN = 0x4ECF;     // #4CD97B locked, battery
}  // namespace col

// Each section's accent: Now Playing coral (the crab's), Library violet,
// Queue teal, Dance pink, Output blue. Dark text reads on all of them.
namespace accent {
constexpr uint16_t NowPlaying = col::CORAL;  // #FF6B4A
constexpr uint16_t Library = 0xA45F;         // #A78BFA
constexpr uint16_t Queue = 0x2EB7;           // #2DD4BF
constexpr uint16_t Dance = 0xF396;           // #F472B6
constexpr uint16_t Output = 0x653F;          // #60A5FA
inline uint16_t of(NavModel::Tab t) {
  switch (t) {
    case NavModel::Tab::NowPlaying: return NowPlaying;
    case NavModel::Tab::Library: return Library;
    case NavModel::Tab::Queue: return Queue;
    case NavModel::Tab::Dance: return Dance;
    case NavModel::Tab::Output: return Output;
  }
  return NowPlaying;
}
}  // namespace accent

// Regions (spec §3.1), screen pixels.
constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kBarH = 36;       // the tab bar, y 0-35 (fixed)
constexpr int kContentY = 36;   // the content area, y 36-239
constexpr int kHeaderY = 36;    // a page header, y 36-71
constexpr int kHeaderH = 36;
constexpr int kMinPathRoom = 60;  // a folder header's path line, or none
constexpr int kListY = 72;      // a list's band, y 72-239: 4 rows of 42
constexpr int kListH = kH - kListY;
constexpr int kRowH = 42;
// The right edge: the panel reads it too far right and stops at 319 (the
// correction puts that at ~282), so a control there reaches the edge and
// its hit area starts at least ~40 px in.
constexpr int kEdgeHitX = 280;

}  // namespace ui
