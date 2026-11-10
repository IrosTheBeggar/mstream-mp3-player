// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/Fonts.h"

#include <M5Unified.h>

#include <algorithm>
#include <new>

#include "CjkGlyphs16.h"
#include "GlyphStore.h"
#include "TextFold.h"
#include "app/Psram.h"
#include "ui/VlwFonts.h"

namespace ui {

namespace {

struct Source {
  const uint8_t* data;
  const size_t* size;
};
const Source kSources[4] = {
    {kVlwSans16, &kVlwSans16Size},
    {kVlwSans13, &kVlwSans13Size},
    {kVlwSansBold16, &kVlwSansBold16Size},
    {kVlwSansBold22, &kVlwSansBold22Size},
};

// Without the VLW fonts (no PSRAM for their tables): M5GFX's built-in
// bitmap fonts, ASCII only (TextFit folds everything else).
const lgfx::IFont* fallback(Font f) {
  return f == Font::Title ? static_cast<const lgfx::IFont*>(&fonts::Font4) : &fonts::Font2;
}

struct FallbackCtx {
  lgfx::LovyanGFX* g;
};
int fallbackWidth(void* ctx, const char* s) { return static_cast<FallbackCtx*>(ctx)->g->textWidth(s); }
bool asciiOnly(void*, uint32_t cp) { return cp >= 0x20 && cp < 0x7F; }

// The built-in CJK and Hangul glyphs (docs/I18N.md, phase 2), read where
// they lie in flash. Opened at the first lookup (its header and its 135
// page records looked at, nothing copied), so load() doesn't wait for it.
const glyphstore::Store& cjk() {
  static const glyphstore::Store store = [] {
    glyphstore::Store s;
    s.open(kCjkGlyphs16, kCjkGlyphs16Size);
    return s;
  }();
  return store;
}

// The VLW fonts' rows (TextInk: where M5GFX draws each glyph), read where
// they lie in flash; opened at the first ink() (a pass over the glyph
// table, as VLWfont::loadFont() makes).
const textink::Vlw& inkFont(Font f) {
  static textink::Vlw fonts[4];
  static bool opened[4] = {};
  const int i = static_cast<int>(f);
  if (!opened[i]) {
    fonts[i].open(kSources[i].data, *kSources[i].size);
    opened[i] = true;
  }
  return fonts[i];
}

// Each loaded font's place for U+0021, when it has U+0021..U+007E in a row
// (DejaVu's files do): an ASCII glyph is then found by its place, with no
// search through the glyph table (which lies in PSRAM: nine probes, each a
// cache line the row sprite's fill may just have evicted). Set by load().
constexpr uint32_t kAsciiFirst = 0x21, kAsciiCount = 0x7F - 0x21;
constexpr uint16_t kNoRun = 0xFFFF;
struct AsciiRun {
  const lgfx::VLWfont* font = nullptr;
  uint16_t first = kNoRun;
};
AsciiRun asciiRuns[4];

bool vlwHas(const lgfx::VLWfont& v, uint32_t cp, uint16_t* idx) {
  if (cp - kAsciiFirst < kAsciiCount) {
    for (const AsciiRun& r : asciiRuns) {
      if (r.font != &v) continue;
      if (r.first == kNoRun) break;
      *idx = static_cast<uint16_t>(r.first + (cp - kAsciiFirst));
      return true;
    }
  }
  return cp <= 0xFFFF && v.getUnicodeIndex(static_cast<uint16_t>(cp), idx);
}

// The Bold and Title roles draw the store's glyphs bold (glyphstore::
// embolden(): a column wider, a pixel further), beside DejaVu Sans Bold.
bool boldCjk(Font f) { return f == Font::Bold || f == Font::Title; }

// A glyph from the store, as the VLW font `ctx` sees it (fromStore()): one
// the font lacks and the store has.
bool storeGlyph(void* ctx, uint32_t cp) {
  uint16_t idx = 0;
  return !vlwHas(*static_cast<const lgfx::VLWfont*>(ctx), cp, &idx) && cjk().contains(cp);
}

// A string's width as drawn: each glyph's advance, the VLW font's, else the
// store's (bold: a pixel more), else the blank M5GFX draws for a missing one.
int widthIn(const lgfx::VLWfont& v, const char* s, bool bold) {
  int w = 0;
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp == 0) break;
    uint16_t idx = 0;
    if (vlwHas(v, cp, &idx)) {
      w += v.gxAdvance[idx];
      continue;
    }
    const int a = cjk().advance(cp);
    if (a < 0) {
      w += v.spaceWidth;  // M5GFX draws a missing glyph as a blank of this width
    } else {
      w += bold ? glyphstore::boldAdvance(a) : a;
    }
  }
  return w;
}

// The largest glyph box drawGlyph() takes: the built-in store's is 16 x 18,
// 17 x 18 bold (306 B of coverage and 612 B of pixels on the stack while it
// draws). test_glyph_store checks the store against it.
constexpr int kMaxBox = kCjkGlyphs16MaxBox;

// fore over back (0xRRGGBB) at coverage a (0-255), as M5GFX blends a VLW
// glyph's pixels (VLWfont::drawChar).
uint32_t blend(uint32_t fore, uint32_t back, uint32_t a) {
  const uint32_t p = 1 + a;
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    out |= ((((fore >> shift) & 0xFF) * p + ((back >> shift) & 0xFF) * (257 - p)) >> 8) << shift;
  }
  return out;
}

// A colour as a 16-bit sprite and the panel keep it: RGB565 with its two
// bytes swapped (M5GFX's swap565_t, color depth rgb565_2Byte), so
// pushImage() and readRect() copy it as it is.
lgfx::swap565_t panelColour(uint32_t rgb) {
  return lgfx::swap565_t(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
}
uint32_t rgb888(const lgfx::swap565_t& c) {
  return static_cast<uint32_t>(c.R8()) << 16 | static_cast<uint32_t>(c.G8()) << 8 | c.B8();
}

// One glyph from the store at the pen, in the line whose top row is `top`
// and `lineH` rows tall, on the store's `baseline` (rows from the top);
// `bold`, emboldened (its box a column wider, its advance a pixel longer).
// With a background (fore != back), first the glyph's cell: the line's
// whole height, from where the last glyph's fill stopped (`filled`) to its
// advance, as M5GFX's VLW glyphs fill theirs. Then its coverage, fore
// blended over what is there: over the background just filled, composed in
// memory and pushed in one piece; over other pixels (no background, or a
// mark reaching back over the glyph before it) read back, blended and
// pushed, or, on a target that can't be read, blended with its base colour
// a pixel at a time (as M5GFX does). Nothing outside the line or the clip
// rectangle. Returns the advance. A function of its own (never inlined), so
// its 918 B of buffers are on the loop task's stack only while it draws.
__attribute__((noinline)) int drawGlyph(lgfx::LovyanGFX& g, const glyphstore::Glyph& glyph, int pen, int top,
                                        int baseline, int lineH, int32_t& filled, bool bold) {
  const glyphstore::Store& store = cjk();
  const glyphstore::Metrics stored = store.metrics(glyph);
  const glyphstore::Metrics m = bold ? glyphstore::bold(stored) : stored;
  const lgfx::TextStyle& st = g.getTextStyle();
  const bool opaque = st.fore_rgb888 != st.back_rgb888;
  const int boxX = pen + m.x;
  const bool over = !opaque || boxX < filled;
  if (opaque) {
    const int left = std::max<int>(filled, pen + std::min<int>(0, m.x));
    const int right = pen + std::max<int>(m.advance, m.x + m.w);
    if (left < right) {
      g.setRawColor(g.getColorConverter()->convert(st.back_rgb888));
      g.writeFillRect(left, top, right - left, lineH);
      filled = right;
    }
  }
  const glyphstore::Placement at = glyphstore::place(m, baseline, lineH);
  if (m.w == 0 || at.rows <= 0) return m.advance;
  // Its coverage, 4 bits (times 17 where it is blended), in rows as wide as
  // the box drawn: a bold glyph's last column is embolden()'s.
  uint8_t alpha[kMaxBox];
  if (m.w * m.h > kMaxBox || !store.decode(glyph, alpha, m.w, sizeof(alpha), glyphstore::Alpha::Nibble)) {
    return m.advance;
  }
  if (bold) glyphstore::embolden(alpha, stored.w, m.h, m.w);
  int32_t cx, cy, cw, ch;
  g.getClipRect(&cx, &cy, &cw, &ch);
  const int x0 = std::max<int>(boxX, cx), x1 = std::min<int>(boxX + m.w, cx + cw);
  const int y0 = std::max<int>(top + at.top, cy), y1 = std::min<int>(top + at.top + at.rows, cy + ch);
  if (x0 >= x1 || y0 >= y1) return m.advance;
  const int w = x1 - x0, h = y1 - y0;
  const uint8_t* a0 = alpha + (y0 - top - at.top) * m.w + (x0 - boxX);
  if (!over || (g.isReadable() && !g.hasPalette())) {
    lgfx::swap565_t px[kMaxBox];
    if (over) g.readRect(x0, y0, w, h, px);
    const lgfx::swap565_t ink = panelColour(st.fore_rgb888), paper = panelColour(st.back_rgb888);
    for (int r = 0; r < h; ++r) {
      for (int c = 0; c < w; ++c) {
        const uint8_t a = a0[r * m.w + c] * 17;
        lgfx::swap565_t& p = px[r * w + c];
        if (a == 255) {
          p = ink;
        } else if (!over) {
          p = a ? panelColour(blend(st.fore_rgb888, st.back_rgb888, a)) : paper;
        } else if (a) {
          p = panelColour(blend(st.fore_rgb888, rgb888(p), a));
        }
      }
    }
    g.pushImage(x0, y0, w, h, static_cast<const lgfx::swap565_t*>(px));
  } else {
    const uint32_t back = opaque ? st.back_rgb888 : g.getBaseColor();
    for (int r = 0; r < h; ++r) {
      for (int c = 0; c < w; ++c) {
        if (const uint8_t a = a0[r * m.w + c] * 17) {
          g.setColor(blend(st.fore_rgb888, back, a));
          g.writeFillRect(x0 + c, y0 + r, 1, 1);
        }
      }
    }
  }
  return m.advance;
}

}  // namespace

Fonts& Fonts::instance() {
  static Fonts fonts;
  return fonts;
}

bool Fonts::load() {
  if (loaded_) return true;
  if (!slots_) {
    void* mem = psramAlloc(4 * sizeof(Slot));
    if (!mem) return false;
    slots_ = static_cast<Slot*>(mem);
    for (int i = 0; i < 4; ++i) new (&slots_[i]) Slot();
  }
  bool ok = true;
  for (int i = 0; i < 4; ++i) {
    slots_[i].data.set(kSources[i].data, *kSources[i].size);
    ok = slots_[i].font.loadFont(&slots_[i].data) && ok;
  }
  loaded_ = ok;
  // Where each font keeps its ASCII glyphs (vlwHas()).
  for (int i = 0; i < 4 && ok; ++i) {
    const lgfx::VLWfont& v = slots_[i].font;
    asciiRuns[i].font = &v;
    asciiRuns[i].first = kNoRun;
    uint16_t at = 0;
    if (!v.getUnicodeIndex(kAsciiFirst, &at) || at + kAsciiCount > v.gCount) continue;
    bool run = true;
    for (uint32_t k = 0; k < kAsciiCount && run; ++k) run = v.gUnicode[at + k] == kAsciiFirst + k;
    if (run) asciiRuns[i].first = at;
  }
  return ok;
}

const lgfx::IFont* Fonts::get(Font f) const {
  if (const lgfx::VLWfont* v = vlw(f)) return v;
  return fallback(f);
}

int Fonts::widthOf(void* ctx, const char* s) { return widthIn(*static_cast<const lgfx::VLWfont*>(ctx), s, false); }

int Fonts::boldWidthOf(void* ctx, const char* s) { return widthIn(*static_cast<const lgfx::VLWfont*>(ctx), s, true); }

bool Fonts::hasGlyph(void* ctx, uint32_t cp) {
  uint16_t idx = 0;
  return vlwHas(*static_cast<const lgfx::VLWfont*>(ctx), cp, &idx) || cjk().contains(cp);
}

textfit::Font Fonts::fit(Font f) const {
  textfit::Font t;
  if (const lgfx::VLWfont* v = vlw(f)) {
    t.ctx = const_cast<lgfx::VLWfont*>(v);
    t.width = boldCjk(f) ? boldWidthOf : widthOf;
    t.has = hasGlyph;
  }
  return t;
}

int Fonts::width(Font f, const char* s) const {
  if (vlw(f)) return textfit::drawnWidth(fit(f), s);  // composed and folded, as draw() draws it
  M5.Display.setFont(fallback(f));
  return M5.Display.textWidth(s);
}

bool Fonts::fromStore(Font f, const char* s) const {
  const lgfx::VLWfont* v = vlw(f);
  return v && textfit::drawnAny(fit(f), s, storeGlyph, const_cast<lgfx::VLWfont*>(v));
}

int Fonts::height(Font f) const {
  if (const lgfx::VLWfont* v = vlw(f)) return v->yAdvance;
  return f == Font::Title ? 26 : 16;
}

textink::Rows Fonts::ink(Font f, const char* text, size_t len, int maxW) const {
  textink::Rows whole;
  whole.top = 0;
  whole.bottom = height(f) - 1;
  if (!loaded_) return whole;
  char buf[200];  // (as draw()'s)
  textfit::fit(fit(f), text, len, buf, sizeof(buf), maxW);
  return textink::inkOf(inkFont(f), cjk(), buf);
}

// Never inlined into draw(): a Latin line's stack stays as it was (and six
// arguments, all in registers: a seventh would grow draw()'s frame).
__attribute__((noinline)) int Fonts::drawMixed(lgfx::LovyanGFX& g, Font f, const char* s, int x, int y,
                                               Align align) {
  const lgfx::VLWfont& v = *instance().vlw(f);
  const bool bold = boldCjk(f);
  // Its width as M5GFX measures a line (LGFXBase::text_width: to the last
  // glyph's ink or advance, the first one's ink left of the pen included),
  // a store glyph by its own box and advance as drawn (bold or not):
  // M5GFX's textWidth() would take it to be as wide as the line is tall.
  int lead = 0, left = 0, right = 0;
  bool first = true;
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp < 0x20 || (cp >= 0xFE00 && cp < 0xFE10)) continue;  // M5GFX skips them
    int xo = 0, w = v.spaceWidth, adv = v.spaceWidth;
    uint16_t idx = 0;
    if (vlwHas(v, cp, &idx)) {
      xo = v.gdX[idx], w = v.gWidth[idx], adv = v.gxAdvance[idx];
    } else if (const glyphstore::Glyph glyph = cjk().find(cp)) {
      const glyphstore::Metrics stored = cjk().metrics(glyph);
      const glyphstore::Metrics m = bold ? glyphstore::bold(stored) : stored;
      xo = m.x, w = m.w, adv = m.advance;
    }
    if (first && xo < 0) lead = left = right = -xo;
    first = false;
    right = left + std::max(adv, w + xo);
    left += adv;
  }
  const int x0 = align == Align::Right ? x - right : align == Align::Centre ? x - (right >> 1) : x;
  const int top = y - (v.yAdvance >> 1);  // as drawString() centres on y (middle datums)
  const int baseline = cjk().baselineIn(v.maxAscent, v.yAdvance);
  lgfx::FontMetrics fm;
  v.getDefaultMetric(&fm);
  int32_t filled = 0;
  int pen = x0 + lead;
  g.startWrite();
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp < 0x20 || (cp >= 0xFE00 && cp < 0xFE10)) continue;
    uint16_t idx = 0;
    const glyphstore::Glyph glyph = vlwHas(v, cp, &idx) ? glyphstore::Glyph() : cjk().find(cp);
    if (glyph) {
      pen += drawGlyph(g, glyph, pen, top, baseline, v.yAdvance, filled, bold);
    } else {
      pen += static_cast<int>(
          v.drawChar(&g, pen, top, static_cast<uint16_t>(cp <= 0xFFFF ? cp : 0), &g.getTextStyle(), &fm, filled));
    }
  }
  g.endWrite();
  return pen - x0;
}

// ---- a Latin line straight into a 16-bit sprite's memory ----
//
// M5GFX paints a VLW glyph with a writeFillRect() a run: one for each row
// of its cell's background, one for each anti-aliased pixel. Through the
// sprite's panel that is some 100 us a glyph, and a list row's three lines
// took 2.9 of its 5.6 ms (MEASURED, the Artists list, 240 MHz). drawFast()
// writes the same pixels itself: the glyph's rows read where they lie in
// flash, blended and stored straight into the sprite's memory.
//
// It is M5GFX's drawString() at a middle datum and VLWfont::drawChar() at
// size 1 with a background (fore != back), step for step: the line's width
// as LGFXBase::text_width() takes it, each glyph's cell filled where
// drawChar() fills it (the rows above and below the glyph, then each of its
// rows), each pixel blended as it blends (fore over back at 1 + coverage,
// in RGB888, then the sprite's own conversion), the clip rectangle honoured
// as writeFillRect() honours it. Console uiBv draws lines both ways and
// compares the sprites' memory.

namespace {

// A 16-bit sprite's pixels (RGB565 with its bytes swapped, as the panel
// keeps them) and its clip rectangle, both ends included.
struct Pixels {
  uint16_t* px;
  int stride;
  int left, top, right, bottom;
};

// LGFXBase::writeFillRect() on them.
inline void fillPixels(const Pixels& t, int x, int y, int w, int h, uint16_t raw) {
  if (x < t.left) {
    w += x - t.left;
    x = t.left;
  }
  if (w > t.right - x + 1) w = t.right - x + 1;
  if (w < 1) return;
  if (y < t.top) {
    h += y - t.top;
    y = t.top;
  }
  if (h > t.bottom - y + 1) h = t.bottom - y + 1;
  if (h < 1) return;
  uint16_t* row = t.px + y * t.stride + x;
  do {
    for (int i = 0; i < w; ++i) row[i] = raw;
    row += t.stride;
  } while (--h);
}

// The line's two colours and what lies between them: fore over back at
// each coverage, made the first time a pixel asks (a line's glyphs share a
// few dozen levels).
struct Ink {
  lgfx::color_conv_t* conv;
  uint32_t fore, back;  // 0xRRGGBB
  uint16_t foreRaw, backRaw;
  uint16_t level[256];
  uint32_t made[8];
  uint16_t at(uint8_t a) {
    uint32_t& word = made[a >> 5];
    const uint32_t bit = 1u << (a & 31);
    if (!(word & bit)) {
      level[a] = static_cast<uint16_t>(conv->convert(blend(fore, back, a)));
      word |= bit;
    }
    return level[a];
  }
};

inline uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

// One glyph of `file` (the VLW font `v` was loaded from) at the pen, in the
// line whose top row is `top`: VLWfont::drawChar() with a background at
// size 1. Returns its advance.
int blitGlyph(const Pixels& t, const lgfx::VLWfont& v, const uint8_t* file, uint32_t cp, int pen, int top, Ink& ink,
              int32_t& filled) {
  // Its record: 28 bytes of big-endian words (the code point, the height,
  // the width, the advance, dY, dX, padding). A space only moves the pen
  // (its cell is filled): drawChar() takes its advance and nothing else.
  int32_t h = 0, w = 0, advance = v.spaceWidth, dY = 0, xo = 0;
  const uint8_t* pixel = nullptr;
  uint16_t idx = 0;
  if (cp == 0x20) {
    if (vlwHas(v, cp, &idx)) advance = static_cast<int32_t>(be32(file + 36 + idx * 28));
  } else {
    vlwHas(v, cp, &idx);  // (it has it: drawFast() looked)
    const uint8_t* rec = file + 28 + idx * 28;
    h = static_cast<int32_t>(be32(rec));
    w = static_cast<int32_t>(be32(rec + 4));
    advance = static_cast<int32_t>(be32(rec + 8));
    dY = static_cast<int16_t>(be32(rec + 12));
    xo = static_cast<int8_t>(be32(rec + 16));
    pixel = file + v.gBitmap[idx];
  }
  const int yoff = static_cast<int>(v.maxAscent) - dY;
  const int lineH = v.yAdvance;
  const int left = std::max<int>(filled, pen + (xo < 0 ? xo : 0));
  const int right = pen + std::max<int>(w + xo, advance);
  filled = right;
  const int gx = pen + xo;
  int bx = gx, bw = w;
  if (gx < t.left) {
    bw += gx - t.left;
    bx = t.left;
  }
  if (bw > t.right + 1 - bx) bw = t.right + 1 - bx;
  if (bw < 0) return advance;
  if (left < right) {
    if (yoff > 0) fillPixels(t, left, top, right - left, yoff, ink.backRaw);
    if (yoff + h < lineH) fillPixels(t, left, top + yoff + h, right - left, lineH - (yoff + h), ink.backRaw);
  }
  if (w <= 0) return advance;
  // The glyph's columns inside the clip rectangle.
  const int j0 = gx < t.left ? t.left - gx : 0;
  const int j1 = gx + w - 1 > t.right ? t.right - gx + 1 : w;
  for (int i = 0; i < h; ++i, pixel += w) {
    const int y0 = yoff + i;
    if (y0 > t.bottom - top) break;
    if (left < right) fillPixels(t, left, top + y0, right - left, 1, ink.backRaw);
    if (top + y0 < t.top) continue;
    uint16_t* d = t.px + (top + y0) * t.stride + gx;
    for (int j = j0; j < j1; ++j) {
      const uint8_t a = pixel[j];
      if (a == 0) continue;
      d[j] = a == 0xFF ? ink.foreRaw : ink.at(a);
    }
  }
  return advance;
}

bool fastText = true;
uint32_t fastCount = 0;

}  // namespace

void Fonts::setFast(bool on) { fastText = on; }
bool Fonts::fast() { return fastText; }
uint32_t Fonts::fastLines() { return fastCount; }

// Never inlined into draw(): its colours' table (Ink, 560 B) is on the
// stack only while a line is drawn.
__attribute__((noinline)) bool Fonts::drawFast(M5Canvas& c, Font f, const char* s, int x, int y, Align align,
                                               int* drawn) const {
  const lgfx::VLWfont* font = vlw(f);
  if (!font) return false;
  const lgfx::VLWfont& v = *font;
  const lgfx::TextStyle& st = c.getTextStyle();
  if (st.fore_rgb888 == st.back_rgb888 || st.size_x != 1.0f || st.size_y != 1.0f || !st.utf8 || st.padding_x != 0) {
    return false;
  }
  if (c.getColorDepth() != lgfx::rgb565_2Byte || c.getRotation() != 0) return false;
  auto* px = static_cast<uint16_t*>(c.getBuffer());
  if (!px) return false;
  // Every character one this path draws (one the font has, or a space), and
  // the line's width as LGFXBase::text_width() takes it: to the last
  // glyph's ink or advance, with the ink a first glyph has left of the pen
  // (draw_string() starts the pen that far in).
  int left = 0, right = 0, lead = 0;
  bool first = true;
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp < 0x20 || cp == 0xFFFD || cp > 0xFFFF || (cp >= 0xFE00 && cp < 0xFE10)) return false;
    int w = v.spaceWidth, adv = v.spaceWidth, xo = 0;
    uint16_t idx = 0;
    if (vlwHas(v, cp, &idx)) {
      w = v.gWidth[idx], adv = v.gxAdvance[idx], xo = v.gdX[idx];
    } else if (cp != 0x20) {
      return false;
    }
    if (first && xo < 0) lead = -xo;
    first = false;
    if (left == 0 && right == 0 && xo < 0) left = right = -xo;
    right = left + std::max(adv, w + xo);
    left += adv;
  }
  const int x0 = align == Align::Right ? x - right : align == Align::Centre ? x - (right >> 1) : x;
  const int top = y - (v.yAdvance >> 1);
  int32_t cx, cy, cw, ch;
  c.getClipRect(&cx, &cy, &cw, &ch);
  const Pixels t{px, static_cast<int>(c.width()), cx, cy, cx + cw - 1, cy + ch - 1};
  Ink ink;
  ink.conv = c.getColorConverter();
  ink.fore = st.fore_rgb888;
  ink.back = st.back_rgb888;
  ink.foreRaw = static_cast<uint16_t>(ink.conv->convert(st.fore_rgb888));
  ink.backRaw = static_cast<uint16_t>(ink.conv->convert(st.back_rgb888));
  memset(ink.made, 0, sizeof(ink.made));
  const uint8_t* file = kSources[static_cast<int>(f)].data;
  int32_t filled = 0;
  int sum = lead;
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    sum += blitGlyph(t, v, file, cp, x0 + sum, top, ink, filled);
  }
  *drawn = sum;
  return true;
}

int Fonts::drawOn(lgfx::LovyanGFX& g, M5Canvas* sprite, Font f, const char* text, size_t len, int x, int y, int maxW,
                  uint16_t fg, uint16_t bg, Align align) const {
  char buf[200];
  g.setFont(get(f));
  if (loaded_) {
    textfit::fit(fit(f), text, len, buf, sizeof(buf), maxW);
  } else {
    FallbackCtx ctx{&g};
    textfit::Font t;
    t.ctx = &ctx;
    t.width = fallbackWidth;
    t.has = asciiOnly;
    textfit::fit(t, text, len, buf, sizeof(buf), maxW);
  }
  if (!buf[0]) return 0;
  if (bg == fg) {
    g.setTextColor(fg);  // transparent: no fill, blended with the pixels there
  } else {
    g.setTextColor(fg, bg);
  }
  g.setTextPadding(0);
  if (const lgfx::VLWfont* v = vlw(f)) {
    // A glyph the font lacks is the store's (TextFit left no other).
    for (const char* p = buf; *p;) {
      if (static_cast<unsigned char>(*p) < 0x80) {
        ++p;
        continue;
      }
      uint16_t idx = 0;
      if (!vlwHas(*v, textfold::decode(p), &idx)) return drawMixed(g, f, buf, x, y, align);
    }
  }
  // Into a 16-bit sprite: straight into its memory (the same pixels).
  int w = 0;
  if (sprite && fastText && drawFast(*sprite, f, buf, x, y, align, &w)) {
    ++fastCount;
    g.setTextDatum(textdatum_t::top_left);  // (as drawString() below leaves it)
    return w;
  }
  g.setTextDatum(align == Align::Left ? textdatum_t::middle_left
                 : align == Align::Right ? textdatum_t::middle_right
                                         : textdatum_t::middle_center);
  w = g.drawString(buf, x, y);
  g.setTextDatum(textdatum_t::top_left);
  return w;
}

int Fonts::draw(lgfx::LovyanGFX& g, Font f, const char* text, size_t len, int x, int y, int maxW, uint16_t fg,
                uint16_t bg, Align align) const {
  return drawOn(g, nullptr, f, text, len, x, y, maxW, fg, bg, align);
}

int Fonts::draw(lgfx::LovyanGFX& g, Font f, const char* text, int x, int y, int maxW, uint16_t fg, uint16_t bg,
                Align align) const {
  return drawOn(g, nullptr, f, text, strlen(text), x, y, maxW, fg, bg, align);
}

int Fonts::draw(M5Canvas& c, Font f, const char* text, size_t len, int x, int y, int maxW, uint16_t fg, uint16_t bg,
                Align align) const {
  return drawOn(c, &c, f, text, len, x, y, maxW, fg, bg, align);
}

int Fonts::draw(M5Canvas& c, Font f, const char* text, int x, int y, int maxW, uint16_t fg, uint16_t bg,
                Align align) const {
  return drawOn(c, &c, f, text, strlen(text), x, y, maxW, fg, bg, align);
}

}  // namespace ui
