// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "CardFormat.h"
#include "InputEvent.h"
#include "ui/Icons.h"

// The empty and error states (the tab bar spec §7, mockups 18 and 22): a
// disc with an icon, a title, one or two lines, and up to two buttons (the
// first is the primary, in the section's colour):
//
//   No microSD card     "Insert a card with your music in /music, ..."  [Try again]
//   This card is exFAT  "Format it FAT32 (MBR) on a computer, ..."      [Try again]
//     (and "... NTFS", "This card uses GPT", "Can't read this card")
//   No music found      "Put folders in /music/Artist/Album/"           [Try again]
//   Your queue is empty "Pick an album, folder or track in the Library." [Open Library] [Shuffle all]
//   Nothing playing     ...                                             [Open Library] [Shuffle all]
//
// Drawn into any band of the screen (a list's band, through its hardware
// scroll; Now Playing's content area), in strips of the shared sprite.
// Loop task only.
namespace ui {

struct EmptyState {
  const icons::Icon* icon = nullptr;
  uint16_t iconColour = 0;
  const char* title = "";
  const char* line1 = "";
  const char* line2 = "";
  const char* buttons[2] = {nullptr, nullptr};
  const icons::Icon* buttonIcons[2] = {nullptr, nullptr};
  int buttonCount() const { return (buttons[0] ? 1 : 0) + (buttons[1] ? 1 : 0); }
};

// No card mounted (and no music on the flash): what is in the slot
// (AppState::cardKind) says which (uitext::cardMessage()): "No microSD
// card", or for a card that is in but didn't mount (the icon amber) "This
// card is exFAT", "This card is NTFS", "This card uses GPT" or "Can't read
// this card"; each with Try again (Ui::retryCard()).
void noCardState(EmptyState& e, cardformat::Kind card);

// Draws `e` over screen lines [y0, y0 + h); `pressed`: the button under a
// finger (-1 none).
void drawEmptyState(const EmptyState& e, int y0, int h, uint16_t accent, int pressed);
// The button a touch is on (-1: none). The last one's hit area reaches the
// screen's edge; each is the full half (or the middle) of the width.
int emptyStateButtonAt(const EmptyState& e, int y0, int h, const InputEvent& ev);

}  // namespace ui
