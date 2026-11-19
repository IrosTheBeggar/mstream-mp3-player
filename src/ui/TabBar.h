// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "TabBarModel.h"

// The tab bar at the top of the screen (TabBarModel has its layout and
// what it shows): drawn cell by cell into the strip sprite and pushed, and
// only the cells whose state changed (the Now Playing cell's EQ bars 4 times
// a second while playing, a 54 x 36 push of ~1 ms). Rows 0-35 are the
// panel's fixed area, outside the lists' hardware scroll.
namespace ui {

class TabBar {
public:
  // The next update() redraws every cell (after the HUD, or a resume).
  void invalidate() { valid_ = false; }
  // Draws what differs from the last state drawn.
  void update(const tabbar::State& s);
  // The Now Playing cell's hairline and bars, and the rest, for 'ui'.
  const tabbar::State& drawn() const { return drawn_; }
  // The glance's top row (Ui::setGlance(): "Touch when off": Track), y
  // 0-35 over the dark UI: the background (no tabs, no surface, no rule;
  // x 0-215 on `all`), and the Output cell's drawing (the output icon in
  // its state's colour, the volume, the battery with its low blink and
  // charging colour) on it at the tab bar's own pixels, drawn on `all` and
  // when tabbar::glanceDirty() says (never for the EQ's steps). The bar is
  // left invalid: the next update() draws it whole (the wake's redrawAll()
  // does so anyway). The caller lifts the gfx gate around it.
  void updateGlance(const tabbar::State& s, bool all);

private:
  void drawCell(int tab, const tabbar::State& s);

  tabbar::State drawn_;
  bool valid_ = false;
  tabbar::State glanceDrawn_;  // updateGlance()'s, as tabbar::glanceOf() has it
};

}  // namespace ui
