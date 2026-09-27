#include "ui/EmptyState.h"

#include <algorithm>

#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"

namespace ui {

namespace {

constexpr int kDiscR = 24;
constexpr int kButtonH = 34;

// Where the pieces go in a band of height h (relative to its top).
struct Layout {
  int discY, titleY, line1Y, line2Y, buttonY;
};

Layout layoutOf(const EmptyState& e, int h) {
  const bool two = e.line2 && e.line2[0];
  const bool buttons = e.buttonCount() > 0;
  const int content = 2 * kDiscR + 6 + 22 + 18 + (two ? 16 : 0) + (buttons ? 10 + kButtonH : 0);
  const int top = std::max(4, (h - content) / 2);
  Layout l;
  l.discY = top + kDiscR;
  l.titleY = top + 2 * kDiscR + 6 + 11;
  l.line1Y = l.titleY + 20;
  l.line2Y = l.line1Y + 16;
  l.buttonY = (two ? l.line2Y : l.line1Y) + 10 + 8;
  return l;
}

// The buttons' boxes: one centred; two side by side, the primary wider
// ("Open Library" and its icon need 150 px; test_ui_library measures).
void buttonBox(const EmptyState& e, int i, int* x, int* w) {
  if (e.buttonCount() <= 1) {
    *x = 70;
    *w = kW - 140;
    return;
  }
  *x = i == 0 ? uitext::kEmptyPrimaryX : uitext::kEmptySecondX;
  *w = i == 0 ? uitext::kEmptyPrimaryW : uitext::kEmptySecondW;
}

}  // namespace

void drawEmptyState(const EmptyState& e, int y0, int h, uint16_t accent, int pressed) {
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  const Layout l = layoutOf(e, h);
  // The same picture drawn into each strip at its own offset (the sprite
  // clips what falls outside it).
  for (int sy = 0; sy < h; sy += gfx::kStripH) {
    const int sh = std::min(gfx::kStripH, h - sy);
    s.fillRect(0, 0, kW, gfx::kStripH, col::BG);
    const int o = -sy;
    if (e.icon) {
      s.fillCircle(kW / 2, l.discY + o, kDiscR, col::CARD);
      icons::drawCentred(s, *e.icon, kW / 2, l.discY + o, e.iconColour ? e.iconColour : col::SOFT);
    }
    if (l.titleY + o > -20 && l.titleY + o < gfx::kStripH + 20) {
      f.draw(s, Font::Title, e.title, kW / 2, l.titleY + o, kW - 16, col::TXT, col::BG, Fonts::Align::Centre);
    }
    if (e.line1 && e.line1[0] && l.line1Y + o > -12 && l.line1Y + o < gfx::kStripH + 12) {
      f.draw(s, Font::Small, e.line1, kW / 2, l.line1Y + o, kW - 16, col::DIM, col::BG, Fonts::Align::Centre);
    }
    if (e.line2 && e.line2[0] && l.line2Y + o > -12 && l.line2Y + o < gfx::kStripH + 12) {
      f.draw(s, Font::Small, e.line2, kW / 2, l.line2Y + o, kW - 16, col::DIM, col::BG, Fonts::Align::Centre);
    }
    for (int i = 0; i < e.buttonCount(); ++i) {
      int x, w;
      buttonBox(e, i, &x, &w);
      const int by = l.buttonY + o;
      if (by + kButtonH < 0 || by > gfx::kStripH) continue;
      const bool primary = i == 0;
      const bool down = pressed == i;
      const uint16_t fill = primary ? (down ? col::SOFT : accent) : (down ? col::BTN_HI : col::BTN);
      const uint16_t ink = primary ? col::DARK : col::TXT;
      s.fillRoundRect(x, by, w, kButtonH, 10, fill);
      const Font font = primary ? Font::Bold : Font::Body;
      // The icon makes way when the label wouldn't fit beside it.
      const icons::Icon* ic = e.buttonIcons[i];
      if (ic && f.width(font, e.buttons[i]) + ic->w + 8 > w - 12) ic = nullptr;
      const int tw = f.width(font, e.buttons[i]) + (ic ? ic->w + 8 : 0);
      int tx = x + (w - std::min(tw, w - 12)) / 2;
      if (ic) {
        icons::drawCentred(s, *ic, tx + ic->w / 2, by + kButtonH / 2, ink);
        tx += ic->w + 8;
      }
      f.draw(s, font, e.buttons[i], tx, by + kButtonH / 2, x + w - 6 - tx, ink, fill);
    }
    gfx::push(s, 0, y0 + sy, kW, sh);
  }
}

int emptyStateButtonAt(const EmptyState& e, int y0, int h, const InputEvent& ev) {
  const int n = e.buttonCount();
  if (n == 0) return -1;
  const Layout l = layoutOf(e, h);
  const int by = y0 + l.buttonY;
  if (ev.y < by - 6 || ev.y >= by + kButtonH + 6) return -1;
  if (n == 1) return ev.x >= 40 || ev.atRightEdge() ? 0 : -1;
  // Split between the two boxes.
  return ev.atRightEdge() || ev.x >= (uitext::kEmptyPrimaryX + uitext::kEmptyPrimaryW + uitext::kEmptySecondX) / 2 ? 1 : 0;
}

}  // namespace ui
