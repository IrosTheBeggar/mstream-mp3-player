#pragma once
#include <M5GFX.h>

#include <cstddef>
#include <cstdint>

#include "TextFit.h"

// The UI's text layer: DejaVu Sans as anti-aliased VLW fonts from flash
// (ui/VlwFonts, as the UI spike recommended: it looked best and has every
// character in the library's names, ’ ‐ é É included), and the fitting that
// every label goes through (TextFit): a character the font lacks is folded
// to ASCII (TextFold), and a name too wide for its column is cut with "…".
//
// Four roles:
//   Body   DejaVu Sans 16      row titles, buttons, the artist on Now Playing
//   Small  DejaVu Sans 13      secondary lines, labels, times
//   Bold   DejaVu Sans Bold 16 headers, the current row, primary buttons
//   Title  DejaVu Sans Bold 22 Now Playing's title, big numbers
//
// load() reads the fonts' glyph tables into PSRAM (M5GFX does, ~5 KB each)
// once; the glyph bitmaps stay in flash. Text is drawn with a background
// colour (sprites, or the LCD in small strips), so a redraw in place never
// flickers. Loop task only.
namespace ui {

enum class Font : uint8_t { Body, Small, Bold, Title };

class Fonts {
public:
  // False: a font failed to load (no PSRAM); text then falls back to Font2.
  bool load();
  bool loaded() const { return loaded_; }
  const lgfx::IFont* get(Font f) const;
  // The TextFit view of a font (width and glyph coverage).
  textfit::Font fit(Font f) const;
  // Pixel width of a UTF-8 string (what fit() measures with).
  int width(Font f, const char* s) const;
  int height(Font f) const;

  // Draws `text` (the first `len` bytes; UTF-8, not necessarily
  // NUL-terminated) fitted to `maxW` px, vertically centred on `y`,
  // left-aligned at `x` (or right-aligned / centred by `datum`). Returns
  // the width drawn. The line's whole height is filled with `bg` behind
  // the glyphs, which cuts into a line drawn close above or below (its
  // descenders, a border); bg == fg draws the glyphs only, blended with
  // what is there (a sprite can be read back).
  enum class Align : uint8_t { Left, Centre, Right };
  int draw(lgfx::LovyanGFX& g, Font f, const char* text, size_t len, int x, int y, int maxW, uint16_t fg,
           uint16_t bg, Align align = Align::Left) const;
  int draw(lgfx::LovyanGFX& g, Font f, const char* text, int x, int y, int maxW, uint16_t fg, uint16_t bg,
           Align align = Align::Left) const;

  static Fonts& instance();

private:
  struct Slot {
    lgfx::PointerWrapper data;
    lgfx::VLWfont font;
  };
  const lgfx::VLWfont* vlw(Font f) const { return loaded_ ? &slots_[static_cast<int>(f)].font : nullptr; }
  static int widthOf(void* ctx, const char* s);
  static bool hasGlyph(void* ctx, uint32_t cp);

  Slot* slots_ = nullptr;  // PSRAM, 4
  bool loaded_ = false;
};

}  // namespace ui
