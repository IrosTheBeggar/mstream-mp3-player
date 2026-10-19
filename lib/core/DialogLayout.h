// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// Where a dialog sits (ui/Overlays' Dialog): a card in the content area,
// kW x kH from (kX, kY), with a bold title, up to three lines of body (two
// with a status line under them; three and a hint in the hold mode), and
// one or two buttons, kButtonH tall, kButtonTop rows down the card, 12 px
// from its sides and 6 px apart. A button's hit area (buttonAt()) is the
// card's last kHitRows rows, the card's width split in two halves (one
// button: all of it), the right-hand half to the screen's edge (a clamped
// reading there too). The hold mode's hint (the format's second question:
// HoldButton) is a line of Small at kHintY, between the body's third line
// and the buttons. Portable, host-tested (test_hold_button).
namespace dialog {

inline constexpr int kX = 14, kY = 58, kW = 292, kH = 164;
inline constexpr int kButtonTop = kH - 46, kButtonH = 36;
inline constexpr int kHitRows = 50;
// The body's lines (Small, centred on kBodyY + i * kLineH), the status
// line kStatusGap under the last, and the hold mode's hint.
inline constexpr int kBodyY = 50, kLineH = 18, kStatusGap = 6;
inline constexpr int kHintY = kBodyY + 3 * kLineH;

// Button i's width and x (in the card) with `n` buttons.
constexpr int buttonW(int n) { return n == 1 ? kW - 24 : (kW - 30) / 2; }
constexpr int buttonX(int i, int n) { return 12 + i * (buttonW(n) + 6); }

// The button a point (screen px) is on, -1 none (Dialog::onEvent()).
constexpr int buttonAt(int x, int y, int n) {
  if (y < kY + kH - kHitRows || y >= kY + kH || x < kX) return -1;
  if (n <= 1) return 0;
  const int half = (x - kX) * 2 / kW;
  return half < 1 ? 0 : 1;
}

// Button i's hit area on the screen, as buttonAt() has it: [x0, x1) x
// [y0, y1); `toEdge`: it reaches the screen's right edge (a reading
// clamped at 319 is on it, wherever the finger was).
struct Area {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool toEdge = false;
};
constexpr Area hitArea(int i, int n) {
  Area a;
  a.y0 = kY + kH - kHitRows;
  a.y1 = kY + kH;
  if (n <= 1) {
    a.x0 = kX;
    a.x1 = 320;
    a.toEdge = true;
  } else if (i == 0) {
    a.x0 = kX;
    a.x1 = kX + kW / 2;
  } else {
    a.x0 = kX + kW / 2;
    a.x1 = 320;
    a.toEdge = true;
  }
  return a;
}

static_assert(kY + kH <= 240, "the card stays on the screen");
static_assert(kHintY + 9 <= kButtonTop, "the hint's row ends above the buttons");
static_assert(buttonAt(kX + buttonX(0, 2) + 10, kY + kButtonTop + 10, 2) == 0, "the left button is hit on itself");
static_assert(buttonAt(kX + buttonX(1, 2) + 10, kY + kButtonTop + 10, 2) == 1, "the right button is hit on itself");

}  // namespace dialog
