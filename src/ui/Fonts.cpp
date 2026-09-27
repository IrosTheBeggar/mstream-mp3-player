#include "ui/Fonts.h"

#include <M5Unified.h>

#include <new>

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
    if (cp <= 0xFFFF && v->getUnicodeIndex(static_cast<uint16_t>(cp), &idx)) {
      w += v->gxAdvance[idx];
    } else {
      w += v->spaceWidth;  // M5GFX draws a missing glyph as a blank of this width
    }
  }
  return w;
}

bool Fonts::hasGlyph(void* ctx, uint32_t cp) {
  const auto* v = static_cast<const lgfx::VLWfont*>(ctx);
  uint16_t idx = 0;
  return cp <= 0xFFFF && v->getUnicodeIndex(static_cast<uint16_t>(cp), &idx);
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
  if (const lgfx::VLWfont* v = vlw(f)) return widthOf(const_cast<lgfx::VLWfont*>(v), s);
  M5.Display.setFont(fallback(f));
  return M5.Display.textWidth(s);
}

int Fonts::height(Font f) const {
  if (const lgfx::VLWfont* v = vlw(f)) return v->yAdvance;
  return f == Font::Title ? 26 : 16;
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
  g.setTextColor(fg, bg);
  g.setTextPadding(0);
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
