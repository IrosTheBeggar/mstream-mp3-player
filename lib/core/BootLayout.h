// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "LogoArt.h"

// The boot screen (ui/BootScreen), 320 x 240, on the UI's dark background:
// the mStream logo centred, the player's version under it, and at the
// bottom the rescue hold's line (uitext::kBootTouchHint). It shows from
// M5.begin() until the UI starts (3 s at least; ~20 s on a new card's
// first boot, while the library is built). The device's facts it used to
// list are Output > About > Device info (DeviceInfo) and the boot log's
// [diag] lines. Host-tested: test_boot_logo (the layout, the logo's runs);
// test_ui_library (the texts in the fonts).
namespace bootlayout {

constexpr int kW = 320, kH = 240;

// The logo's top-left: centred across; the logo and the version together
// sit a little above the screen's middle.
constexpr int kLogoX = (kW - logo::kW) / 2;
constexpr int kLogoY = 80;

// The version (Body, centred on y kVersionY, in x 8-312): a release's
// "v0.8.0", a dev build's "v0.8.0-dev+abc1234-dirty" (Small when Body
// doesn't fit).
constexpr int kVersionY = 150;
constexpr int kTextW = 304;
// The rescue line (Small, centred on y kHintY), as before.
constexpr int kHintY = 229;
// A text line's height: DejaVu 16 is 19 px from its ascent to its
// descent, 13 is 17 (test_ui_library reads them from the fonts).
constexpr int kBodyH = 19, kSmallH = 17;
// The room kept between the logo and the version.
constexpr int kGap = 10;

// The colours, RGB888. The logo is navy on white on mStream's pages; here
// it is recoloured for the dark: the bars of the "m" keep their two blues,
// the word is white as the UI's text (col::TXT). The edges blend into the
// UI's background (col::BG).
constexpr uint32_t kLogoBg = 0x0A0C12;     // col::BG
constexpr uint32_t kLogoPart1 = 0xF0F3F8;  // "stream" (col::TXT)
constexpr uint32_t kLogoPart2 = 0x6684B2;  // the outer bars: mStream's light blue
constexpr uint32_t kLogoPart3 = 0x26477B;  // the middle bar: mStream's navy
constexpr uint32_t kLogoParts[logo::kParts + 1] = {0, kLogoPart1, kLogoPart2, kLogoPart3};

}  // namespace bootlayout
