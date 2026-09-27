#include "ui/Overlays.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app/Psram.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace ui {

namespace {

M5Canvas* panel_ = nullptr;  // PSRAM, 320 x 168: a sheet or a dialog

void copy(char* dst, size_t size, const char* src) {
  snprintf(dst, size, "%s", src ? src : "");
}

}  // namespace

bool overlaysBegin() {
  if (panel_) return true;
  panel_ = psramNew<M5Canvas>();
  if (!panel_) return false;
  panel_->setPsram(true);  // before createSprite(): otherwise internal RAM
  panel_->setColorDepth(16);
  if (!panel_->createSprite(kW, kListH)) {
    psramDelete(panel_);
    panel_ = nullptr;
    return false;
  }
  return true;
}

// ---- Toast ----

void Toast::show(const char* text, bool undo, bool view, uint16_t accent, uint32_t nowMs) {
  copy(text_, sizeof(text_), text);
  undo_ = undo;
  view_ = view;
  accent_ = accent;
  up_ = true;
  untilMs_ = nowMs + (undo ? 4000 : 1800);
  draw();
}

// Undo at x 240-309 (its hit area 230 to the edge); View at 170-229 (its
// hit area 160-229).
void Toast::draw(int pressed) {
  if (!up_) return;
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kH, col::BG);
  s.fillRoundRect(4, 2, 312, 32, 8, col::CARD);
  s.drawRoundRect(4, 2, 312, 32, 8, accent_);
  s.fillRect(10, 10, 4, 16, accent_);
  const int textRight = view_ ? 162 : undo_ ? 232 : 306;
  f.draw(s, Font::Body, text_, 22, 18, textRight - 22, col::TXT, col::CARD);
  if (undo_) {
    const uint16_t b = pressed == 2 ? col::BTN_HI : col::BTN;
    s.fillRoundRect(240, 6, 70, 24, 6, b);
    f.draw(s, Font::Bold, "Undo", 275, 18, 64, accent_, b, Fonts::Align::Centre);
  }
  if (view_) {
    const uint16_t b = pressed == 3 ? col::BTN_HI : col::BTN;
    s.fillRoundRect(170, 6, 62, 24, 6, b);
    f.draw(s, Font::Bold, "View", 201, 18, 56, col::TXT, b, Fonts::Align::Centre);
  }
  gfx::push(s, 0, kY, kW, kH, true);
}

int Toast::hit(const InputEvent& e) const {
  if (!up_ || e.y < kY || e.y >= kY + kH) return 0;
  // Undo reaches the screen's edge (x 230 on, or a clamped reading).
  if (undo_ && e.inRightEdgeZone(230)) return 2;
  if (view_ && e.x >= 160 && e.x < 230) return 3;
  return 1;
}

// ---- Hud ----

void Hud::showVolume(int percent, bool bluetooth, uint32_t nowMs) {
  kind_ = Kind::Volume;
  volume_ = std::max(0, std::min(100, percent));
  bluetooth_ = bluetooth;
  up_ = true;
  untilMs_ = nowMs + kShowMs;
  draw();
}

void Hud::showOutput(bool bluetooth, const char* line, uint32_t nowMs) {
  kind_ = Kind::Output;
  bluetooth_ = bluetooth;
  copy(line_, sizeof(line_), line);
  up_ = true;
  untilMs_ = nowMs + kShowMs + 500;  // a sentence: a little longer
  draw();
}

void Hud::draw() {
  if (!up_) return;
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kBarH, col::HUD);
  const uint16_t ic = bluetooth_ ? col::CYAN : col::SOFT;
  icons::draw(s, bluetooth_ ? icons::kHeadphones : icons::kSpeaker, 8, 8, ic);
  if (kind_ == Kind::Volume) {
    // 20 blocks of 5 %, lit up to the level.
    const int lit = (volume_ + 2) / 5;
    for (int i = 0; i < 20; ++i) s.fillRect(38 + i * 11, 10, 9, 14, i < lit ? col::CORAL : col::BTN);
    char v[6];
    snprintf(v, sizeof(v), "%d%%", volume_);
    f.draw(s, Font::Bold, v, 312, 17, 60, col::TXT, col::HUD, Fonts::Align::Right);
  } else {
    f.draw(s, Font::Body, line_, 38, 17, 274, col::TXT, col::HUD);
  }
  s.fillRect(0, kBarH - 2, kW, 2, col::CORAL);
  gfx::push(s, 0, 0, kW, kBarH, true);
}

// ---- Sheet ----

void Sheet::open(const char* title, const char* const* rows, int n, uint16_t accent) {
  copy(title_, sizeof(title_), title);
  n_ = std::max(0, std::min(kMaxRows, n));
  for (int i = 0; i < n_; ++i) copy(rows_[i], sizeof(rows_[i]), rows[i]);
  accent_ = accent;
  pressed_ = -1;
  up_ = true;
  y0_ = std::max(kListY, kH - (kTitleH + n_ * kRowH + 4));
  draw();
}

void Sheet::render() {
  M5Canvas& s = *panel_;
  Fonts& f = Fonts::instance();
  const int h = kH - y0_;
  s.fillRect(0, 0, kW, h, col::BG);
  s.fillRoundRect(0, 0, kW, h + 12, 12, col::CARD);  // rounded at the top only
  s.fillRoundRect(kW / 2 - 18, 4, 36, 4, 2, col::FAINT);
  f.draw(s, Font::Small, title_, 16, 22, kW - 32, col::DIM, col::CARD);
  for (int i = 0; i < n_; ++i) {
    const int y = kTitleH + i * kRowH;
    const uint16_t bg = i == pressed_ ? col::ROW_SEL : col::CARD;
    s.fillRect(0, y, kW, kRowH, bg);
    s.drawFastHLine(16, y, kW - 32, col::DIV);
    f.draw(s, i == 0 ? Font::Bold : Font::Body, rows_[i], 16, y + kRowH / 2, kW - 32, i == 0 ? accent_ : col::TXT, bg);
  }
}

void Sheet::draw() {
  if (!up_ || !panel_) return;
  render();
  gfx::push(*panel_, 0, y0_, kW, kH - y0_, true);
}

void Sheet::pushRow(int i) {
  if (i < 0 || !panel_) return;
  const int y = kTitleH + i * kRowH;
  gfx::pushRows(*panel_, 0, y0_, kW, y, y + kRowH, true);
}

int Sheet::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!up_) return -1;
  const int row = e.y >= y0_ + kTitleH ? (e.y - y0_ - kTitleH) / kRowH : -1;
  const int inRow = row >= 0 && row < n_ ? row : -1;
  if (e.type == T::Down) {
    pressed_ = inRow;
    render();
    pushRow(inRow);
    return -1;
  }
  if (e.type == T::DragStart || e.type == T::Cancel || e.type == T::Release) {
    const int was = pressed_;
    pressed_ = -1;
    render();
    pushRow(was);
    return -1;
  }
  if (e.type != T::Tap) return -1;
  pressed_ = -1;
  if (e.y < y0_) return -2;  // outside: close
  return inRow;
}

// ---- Dialog ----

void Dialog::open(const char* title, const char* body, const char* const* buttons, int n, uint16_t accent) {
  copy(title_, sizeof(title_), title);
  copy(body_, sizeof(body_), body);
  n_ = std::max(1, std::min(2, n));
  for (int i = 0; i < n_; ++i) copy(buttons_[i], sizeof(buttons_[i]), buttons[i]);
  accent_ = accent;
  pressed_ = -1;
  up_ = true;
  draw();
}

int Dialog::buttonAt(int x, int y) const {
  if (y < kY + kH - 50 || y >= kY + kH) return -1;
  if (x < kX) return -1;
  // The right-hand button's hit area reaches the screen's edge.
  return n_ == 1 ? 0 : std::min(1, (x - kX) * 2 / kW);
}

void Dialog::render() {
  M5Canvas& s = *panel_;
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kH, col::CARD);
  s.drawRoundRect(0, 0, kW, kH, 12, accent_);
  s.drawRoundRect(1, 1, kW - 2, kH - 2, 11, accent_);
  f.draw(s, Font::Bold, title_, 16, 22, kW - 32, col::TXT, col::CARD);
  char lines[3][64];
  const int nl = textfit::wrap(f.fit(Font::Small), body_, strlen(body_), kW - 32, 3, &lines[0][0], sizeof(lines[0]));
  for (int i = 0; i < nl; ++i) f.draw(s, Font::Small, lines[i], 16, 50 + i * 18, kW - 32, col::SOFT, col::CARD);
  const int bw = n_ == 1 ? kW - 24 : (kW - 30) / 2;
  for (int i = 0; i < n_; ++i) {
    const int x = 12 + i * (bw + 6);
    const bool primary = i == n_ - 1;
    const uint16_t b = i == pressed_ ? col::BTN_HI : primary ? accent_ : col::BTN;
    s.fillRoundRect(x, kH - 46, bw, 36, 8, b);
    f.draw(s, primary ? Font::Bold : Font::Body, buttons_[i], x + bw / 2, kH - 28, bw - 8,
           primary && i != pressed_ ? col::DARK : col::TXT, b, Fonts::Align::Centre);
  }
}

void Dialog::draw() {
  if (!up_ || !panel_) return;
  render();
  gfx::push(*panel_, kX, kY, kW, kH, true);
}

int Dialog::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!up_) return -1;
  if (e.type == T::Down) {
    pressed_ = buttonAt(e.x, e.y);
    if (pressed_ >= 0) draw();
    return -1;
  }
  if (e.type == T::DragStart || e.type == T::Cancel || e.type == T::Release) {
    if (pressed_ >= 0) {
      pressed_ = -1;
      draw();
    }
    return -1;
  }
  if (e.type != T::Tap) return -1;
  const int b = pressed_;
  pressed_ = -1;
  return b;
}

}  // namespace ui
