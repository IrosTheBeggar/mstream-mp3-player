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

}  // namespace icons
