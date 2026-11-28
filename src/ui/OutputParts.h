// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// What the Output tab's pages draw alike (ui/OutputPage.cpp has them): the
// radio, the spinner and its step pushed alone, the signal bars; and a
// place on the tab for Ui::push().
#pragma once
#include <cstdint>

#include "ui/Pages.h"

namespace ui {
namespace outputparts {

constexpr uint32_t kSpinMs = 125;  // the spinner: 8 steps a second

// A radio button: a ring, filled in the accent when it is the one on.
void radio(M5Canvas& c, int cx, int cy, bool on, uint16_t acc, uint16_t bg);
// A ring of 8 dots, one bright (the step): something is under way.
void spinner(M5Canvas& c, int cx, int cy, uint8_t step, uint16_t colour);
// The spinner's step alone, pushed onto `row` at (cx, cy) over `bg`
// (ListView::pushOnRow), not the row rendered and pushed whole (~11 ms,
// 8 times a second while it spins). False: the row must be drawn whole.
bool spinPatch(ListView& list, uint32_t row, int cx, int cy, uint8_t step, uint16_t colour, uint16_t bg);
// Four signal bars, `lit` of them in the ink.
void bars(M5Canvas& c, int x, int bottom, int lit, uint16_t ink);
// A page of the tab, for Ui::push().
NavModel::PageRef page(PageKind kind);

}  // namespace outputparts
}  // namespace ui
