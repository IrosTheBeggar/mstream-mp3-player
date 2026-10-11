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

bool vlwHas(const lgfx::VLWfont& v, uint32_t cp, uint16_t* idx) {
  return cp <= 0xFFFF && v.getUnicodeIndex(static_cast<uint16_t>(cp), idx);
}

// The largest glyph box drawGlyph() takes: the built-in store's is 16 x 18
// (288 B of coverage and 576 B of pixels on the stack while it draws).
// test_glyph_store checks the store against it.
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
// and `lineH` rows tall, on the store's `baseline` (rows from the top).
// With a background (fore != back), first the glyph's cell: the line's
// whole height, from where the last glyph's fill stopped (`filled`) to its
// advance, as M5GFX's VLW glyphs fill theirs. Then its coverage, fore
// blended over what is there: over the background just filled, composed in
// memory and pushed in one piece; over other pixels (no background, or a
// mark reaching back over the glyph before it) read back, blended and
// pushed, or, on a target that can't be read, blended with its base colour
// a pixel at a time (as M5GFX does). Nothing outside the line or the clip
// rectangle. Returns the advance. A function of its own (never inlined), so
// its 864 B of buffers are on the loop task's stack only while it draws.
__attribute__((noinline)) int drawGlyph(lgfx::LovyanGFX& g, const glyphstore::Glyph& glyph, int pen, int top,
                                        int baseline, int lineH, int32_t& filled) {
  const glyphstore::Store& store = cjk();
  const glyphstore::Metrics m = store.metrics(glyph);
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
  uint8_t alpha[kMaxBox];
  if (!store.decode(glyph, alpha, m.w, sizeof(alpha))) return m.advance;
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
        const uint8_t a = a0[r * m.w + c];
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
        if (const uint8_t a = a0[r * m.w + c]) {
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
  return ok;
}

const lgfx::IFont* Fonts::get(Font f) const {
  if (const lgfx::VLWfont* v = vlw(f)) return v;
  return fallback(f);
}

int Fonts::widthOf(void* ctx, const char* s) {
  const auto* v = static_cast<const lgfx::VLWfont*>(ctx);
  int w = 0;
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp == 0) break;
    uint16_t idx = 0;
    if (vlwHas(*v, cp, &idx)) {
      w += v->gxAdvance[idx];
      continue;
    }
    const int a = cjk().advance(cp);
    w += a >= 0 ? a : v->spaceWidth;  // M5GFX draws a missing glyph as a blank of this width
  }
  return w;
}

bool Fonts::hasGlyph(void* ctx, uint32_t cp) {
  uint16_t idx = 0;
  return vlwHas(*static_cast<const lgfx::VLWfont*>(ctx), cp, &idx) || cjk().contains(cp);
}

textfit::Font Fonts::fit(Font f) const {
  textfit::Font t;
  if (const lgfx::VLWfont* v = vlw(f)) {
    t.ctx = const_cast<lgfx::VLWfont*>(v);
    t.width = widthOf;
    t.has = hasGlyph;
  }
  return t;
}

int Fonts::width(Font f, const char* s) const {
  if (vlw(f)) return textfit::drawnWidth(fit(f), s);  // composed and folded, as draw() draws it
  M5.Display.setFont(fallback(f));
  return M5.Display.textWidth(s);
}

int Fonts::height(Font f) const {
  if (const lgfx::VLWfont* v = vlw(f)) return v->yAdvance;
  return f == Font::Title ? 26 : 16;
}

// Never inlined into draw(): a Latin line's stack stays as it was.
__attribute__((noinline)) int Fonts::drawMixed(lgfx::LovyanGFX& g, const lgfx::VLWfont& v, const char* s, int x,
                                               int y, Align align) {
  // Its width as M5GFX measures a line (LGFXBase::text_width: to the last
  // glyph's ink or advance, the first one's ink left of the pen included),
  // a store glyph by its own box and advance: M5GFX's textWidth() would
  // take it to be as wide as the line is tall.
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
      const glyphstore::Metrics m = cjk().metrics(glyph);
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
      pen += drawGlyph(g, glyph, pen, top, baseline, v.yAdvance, filled);
    } else {
      pen += static_cast<int>(
          v.drawChar(&g, pen, top, static_cast<uint16_t>(cp <= 0xFFFF ? cp : 0), &g.getTextStyle(), &fm, filled));
    }
  }
  g.endWrite();
  return pen - x0;
}

int Fonts::draw(lgfx::LovyanGFX& g, Font f, const char* text, size_t len, int x, int y, int maxW, uint16_t fg,
                uint16_t bg, Align align) const {
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
      if (!vlwHas(*v, textfold::decode(p), &idx)) return drawMixed(g, *v, buf, x, y, align);
    }
  }
  g.setTextDatum(align == Align::Left ? textdatum_t::middle_left
                 : align == Align::Right ? textdatum_t::middle_right
                                         : textdatum_t::middle_center);
  const int w = g.drawString(buf, x, y);
  g.setTextDatum(textdatum_t::top_left);
  return w;
}

int Fonts::draw(lgfx::LovyanGFX& g, Font f, const char* text, int x, int y, int maxW, uint16_t fg, uint16_t bg,
                Align align) const {
  return draw(g, f, text, strlen(text), x, y, maxW, fg, bg, align);
}

}  // namespace ui
