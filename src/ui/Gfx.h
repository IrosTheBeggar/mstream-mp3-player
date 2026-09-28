#pragma once
#include <M5GFX.h>

#include <cstdint>

#include "ui/LcdLock.h"

// How the UI puts pixels on the LCD. Everything is drawn into a PSRAM
// sprite first (anti-aliased text needs its background; a sprite never
// flickers), then pushed in small pieces, each under its own LcdLock: the
// LCD shares its SPI bus with the SD card that feeds the decoder, so no
// single hold is long (at most kBand lines, ~5 ms).
//
// Two things every push and fill here takes care of, so the pages don't:
//
//   - The list band's hardware scroll (ui/ListScroller). While a list page
//     is up, the panel shows the band's GRAM rotated: a screen row is
//     written to the GRAM row the panel shows there
//     (ListScroller::gramLineForScreen), in runs of consecutive rows. So an
//     overlay (a sheet, a dialog) lands where it is meant to over a list
//     that scrolled.
//   - The cover: rows an overlay holds (the toast over the header row) are
//     left alone by the pages' drawing, so a page that redraws under the
//     toast doesn't wipe it. Overlays draw with `over` set.
//
// Loop task only (the one task that draws).
namespace ui {
namespace gfx {

constexpr int kBand = 40;     // lines per bus hold
constexpr int kStripH = 56;   // the shared strip sprite: 320 x 56 (35 KB)

// Allocates the strip sprite (PSRAM). False: no memory.
bool begin();
// The shared strip: draw something up to 320 x 56 into it, push it, done.
// Not kept between uses.
M5Canvas& strip();

// The screen is off (ScreenPower): nothing reaches the panel, every fill and
// push is dropped. The UI draws it all again when it comes back
// (Ui::setDark). Only the UI's own drawing is gated: a screen of its own
// (the calibration, a spike tool) keeps it lit.
void setDark(bool on);
bool dark();

// Rows [y0, y1) belong to an overlay: page drawing skips them. (0, 0): none.
void setCover(int y0, int y1);
bool covered(int y);

// Fills a screen rectangle (mapped through the hardware scroll, the cover
// skipped unless `over`).
void fill(int x, int y, int w, int h, uint16_t colour, bool over = false);
// Pushes the top-left w x h of `sprite` with its (0, 0) at screen (x, y).
void push(M5Canvas& sprite, int x, int y, int w, int h, bool over = false);
inline void push(M5Canvas& sprite, int x, int y, bool over = false) {
  push(sprite, x, y, sprite.width(), sprite.height(), over);
}
// Pushes rows [sy0, sy1) of the top-left w-wide part of `sprite` (sprite
// row sy lands on screen row y + sy).
void pushRows(M5Canvas& sprite, int x, int y, int w, int sy0, int sy1, bool over = false);

// Every hold the UI's drawing took (for the 'ui' console report).
SpiHoldStats& holds();

}  // namespace gfx
}  // namespace ui
