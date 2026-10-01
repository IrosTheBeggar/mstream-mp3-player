// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <M5GFX.h>

#include <cstdint>

// The UI's icons: 1-bit bitmaps in flash (tools/ui_icons.py makes
// IconData.cpp), drawn in any colour. The tab icons are ~24x18.
namespace icons {

struct Icon {
  const uint8_t* bits;  // rows of whole bytes, MSB first (M5GFX drawBitmap)
  uint8_t w, h;
};

extern const Icon kLibrary;       // a record half out of its sleeve (not bars: Now Playing has those)
extern const Icon kQueue;         // a play triangle and list lines
extern const Icon kCrab;          // the Dance tab
extern const Icon kHeadphones;
extern const Icon kSpeaker;
extern const Icon kNote;          // Now Playing with nothing loaded
extern const Icon kPlay;
extern const Icon kPause;
extern const Icon kNext;
extern const Icon kPrev;
extern const Icon kMore;
extern const Icon kChevronRight;
extern const Icon kChevronLeft;
extern const Icon kCheck;
extern const Icon kCross;
extern const Icon kPlus;
extern const Icon kMinus;
extern const Icon kFolder;        // the Folders explorer's folders
extern const Icon kFile;          // its audio files
extern const Icon kTrash;         // Remove (the Queue's edit bar)
extern const Icon kUndo;          // the toast's compact Undo
extern const Icon kSdCard;        // the no-card state
extern const Icon kShuffle;       // Shuffle all
extern const Icon kJack;          // line out (the 3.5 mm / RCA module)
extern const Icon kWarn;          // a track that couldn't be played
extern const Icon kGear;          // settings rows
extern const Icon kInfo;          // About
extern const Icon kVibrate;       // Haptics

// Draws `icon` with its top-left at (x, y), or centred on (cx, cy).
inline void draw(lgfx::LovyanGFX& g, const Icon& icon, int x, int y, uint16_t colour) {
  g.drawBitmap(x, y, icon.bits, icon.w, icon.h, colour);
}
inline void drawCentred(lgfx::LovyanGFX& g, const Icon& icon, int cx, int cy, uint16_t colour) {
  draw(g, icon, cx - icon.w / 2, cy - icon.h / 2, colour);
}

// The sleep timer's moon: a crescent in a `d` x `d` box with its top-left
// at (x, y), pixel by pixel (drawn, not a bitmap: the tab bar's badge is
// 7 px, Now Playing's 11; nothing outside the box is touched, so it sits on
// any background). In doubled coordinates from the box's centre: inside
// the disc, outside the bite (a disc of the same size up and to the right).
inline void drawMoon(lgfx::LovyanGFX& g, int x, int y, int d, uint16_t colour) {
  const int r2 = d * d;
  const int bx = d, by = d * 6 / 10;  // the bite's centre, doubled
  for (int py = 0; py < d; ++py) {
    for (int px = 0; px < d; ++px) {
      const int dx = 2 * px + 1 - d, dy = 2 * py + 1 - d;
      const int ex = dx - bx, ey = dy + by;
      if (dx * dx + dy * dy <= r2 && ex * ex + ey * ey > r2) g.drawPixel(x + px, y + py, colour);
    }
  }
}

}  // namespace icons
