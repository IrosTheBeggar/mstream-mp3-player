#include "ui/Overlays.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "UiText.h"
#include "app/Psram.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace ui {

namespace {

M5Canvas* panel_ = nullptr;  // PSRAM, 320 x 168: a sheet, a dialog, the jump grid

void copy(char* dst, size_t size, const char* src) {
  snprintf(dst, size, "%s", src ? src : "");
}

// A sheet's card: rounded at the top only, with its handle.
void sheetCard(M5Canvas& s, int h) {
  s.fillRect(0, 0, kW, h, col::BG);
  s.fillRoundRect(0, 0, kW, h + 12, 12, col::CARD);
  s.fillRoundRect(kW / 2 - 18, 4, 36, 4, 2, col::FAINT);
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

namespace {
// Undo at x 256-309 (its hit area 250 to the edge); View at 196-249 (its
// hit area 190-249). The text has what's left of them.
constexpr int kUndoX = 256, kUndoW = 54, kUndoHitX = 250;
constexpr int kViewX = 196, kViewW = 54, kViewHitX = 190;
// Two lines ("Plays next" over a long name): the buttons as icons (Undo's
// arrow, View as the Queue tab's icon), so the name has most of the width
// (uitext::kToastCompactTextRight; test_ui_library measures).
constexpr int kUndoCX = 280, kUndoCW = 32, kUndoCHitX = 264;
constexpr int kViewCX = 244, kViewCW = 32, kViewCHitX = 228;
constexpr int kTextX = uitext::kToastTextX;
constexpr int kBackZone = 56;  // the page header's ‹ (Header::hit)

int textRightOf(bool compact, bool undo, bool view) {
  if (compact) return view ? uitext::kToastCompactTextRight : undo ? kUndoCX - 6 : 306;
  return view ? kViewX - 6 : undo ? kUndoX - 6 : 306;
}
}  // namespace

void Toast::show(const char* text, bool undo, bool view, uint16_t accent, uint32_t nowMs) {
  copy(text_, sizeof(text_), text);
  undo_ = undo;
  view_ = view;
  accent_ = accent;
  up_ = true;
  untilMs_ = nowMs + (undo ? 4000 : 1800);
  // One line if it fits beside the buttons; else "what: <name>" on two,
  // with the buttons as icons.
  compact_ = strstr(text_, ": ") && Fonts::instance().width(Font::Body, text_) > textRightOf(false, undo, view) - kTextX;
  draw();
}

void Toast::draw(int pressed) {
  if (!up_) return;
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kH, col::BG);
  s.fillRoundRect(4, 2, 312, 32, 8, col::CARD);
  s.drawRoundRect(4, 2, 312, 32, 8, accent_);
  s.fillRect(10, 10, 4, 16, accent_);
  const int textW = textRightOf(compact_, undo_, view_) - kTextX;
  if (!compact_) {
    f.draw(s, Font::Body, text_, kTextX, 18, textW, col::TXT, col::CARD);
  } else {
    // "Plays next: <a long name>": what happened (small) over the name, in
    // Body if it fits, else Small ("Harder, Better, Faster, Stronger").
    const char* colon = strstr(text_, ": ");
    const char* name = colon + 2;
    // Glyphs only (no line fill): each line's fill cut the other's
    // descenders and the box's border (seen on the device).
    f.draw(s, Font::Small, text_, static_cast<size_t>(colon - text_), kTextX, 10, textW, col::DIM, col::DIM);
    f.draw(s, f.width(Font::Body, name) <= textW ? Font::Body : Font::Small, name, kTextX, 24, textW, col::TXT,
           col::TXT);
  }
  if (undo_) {
    const uint16_t b = pressed == 2 ? col::BTN_HI : col::BTN;
    if (compact_) {
      s.fillRoundRect(kUndoCX, 6, kUndoCW, 24, 6, b);
      icons::drawCentred(s, icons::kUndo, kUndoCX + kUndoCW / 2, 18, accent_);
    } else {
      s.fillRoundRect(kUndoX, 6, kUndoW, 24, 6, b);
      f.draw(s, Font::Bold, "Undo", kUndoX + kUndoW / 2, 18, kUndoW - 4, accent_, b, Fonts::Align::Centre);
    }
  }
  if (view_) {
    const uint16_t b = pressed == 3 ? col::BTN_HI : col::BTN;
    if (compact_) {
      s.fillRoundRect(kViewCX, 6, kViewCW, 24, 6, b);
      icons::drawCentred(s, icons::kQueue, kViewCX + kViewCW / 2, 18, col::TXT);
    } else {
      s.fillRoundRect(kViewX, 6, kViewW, 24, 6, b);
      f.draw(s, Font::Bold, "View", kViewX + kViewW / 2, 18, kViewW - 4, col::TXT, b, Fonts::Align::Centre);
    }
  }
  gfx::push(s, 0, kY, kW, kH, true);
}

int Toast::hit(const InputEvent& e) const {
  if (!up_ || e.y < kY || e.y >= kY + kH) return 0;
  const int undoHit = compact_ ? kUndoCHitX : kUndoHitX;
  const int viewHit = compact_ ? kViewCHitX : kViewHitX;
  // Undo reaches the screen's edge (and takes a clamped reading).
  if (undo_ && e.inRightEdgeZone(undoHit)) return 2;
  if (view_ && e.x >= viewHit && e.x < undoHit) return 3;
  return 1;
}

bool Toast::passesThrough(const InputEvent& e) const {
  if (!up_ || e.y < kY || e.y >= kY + kHeaderH) return false;  // the header row only
  if (e.x < kBackZone) return true;
  // The header pill's zone (x 240 to the edge), unless Undo or View is there.
  return !undo_ && !view_ && e.inRightEdgeZone(240);
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

void Sheet::open(const char* title, const char* const* rows, int n, uint16_t accent, const char* const* details,
                 int primary, int danger) {
  copy(title_, sizeof(title_), title);
  primary_ = primary;
  danger_ = danger;
  n_ = std::max(0, std::min(kMaxRows, n));
  for (int i = 0; i < n_; ++i) {
    copy(rows_[i], sizeof(rows_[i]), rows[i]);
    copy(details_[i], sizeof(details_[i]), details ? details[i] : "");
  }
  accent_ = accent;
  pressed_ = -1;
  up_ = true;
  y0_ = std::max(kListY, kH - (kTitleH + n_ * kRowH + 4));
  draw();
}

// The ✕ pill: x 262-311 of the title row (its hit area 250 to the edge).
namespace {
constexpr int kSheetCrossX = 262, kSheetCrossW = 50, kSheetCrossHitX = 250;
}  // namespace

void Sheet::render() {
  M5Canvas& s = *panel_;
  Fonts& f = Fonts::instance();
  const int h = kH - y0_;
  sheetCard(s, h);
  f.draw(s, Font::Small, title_, 16, 22, kSheetCrossHitX - 8 - 16, col::DIM, col::CARD);
  const uint16_t pill = pressed_ == kCross ? col::BTN_HI : col::BTN;
  s.fillRoundRect(kSheetCrossX, 9, kSheetCrossW, 24, 12, pill);
  icons::drawCentred(s, icons::kCross, kSheetCrossX + kSheetCrossW / 2, 21, col::TXT);
  for (int i = 0; i < n_; ++i) {
    const int y = kTitleH + i * kRowH;
    const uint16_t bg = i == pressed_ ? col::ROW_SEL : col::CARD;
    s.fillRect(0, y, kW, kRowH, bg);
    s.drawFastHLine(16, y, kW - 32, col::DIV);
    // Every row alike, but the main choice (Bold, the accent) and the one
    // that can't be taken back (red).
    const bool primary = i == primary_;
    const uint16_t ink = primary ? accent_ : i == danger_ ? col::RED : col::TXT;
    const int w = f.draw(s, primary ? Font::Bold : Font::Body, rows_[i], 16, y + kRowH / 2, kW - 32, ink, bg);
    // The detail, right-aligned in what the label left.
    const int room = kW - 16 - (16 + w + 16);
    if (details_[i][0] && room > 24) {
      f.draw(s, Font::Small, details_[i], kW - 16, y + kRowH / 2, room, col::DIM, bg, Fonts::Align::Right);
    }
  }
}

void Sheet::draw() {
  if (!up_ || !panel_) return;
  render();
  gfx::push(*panel_, 0, y0_, kW, kH - y0_, true);
}

void Sheet::pushRow(int i) {
  if (i == kCross) {
    pushTitle();
    return;
  }
  if (i < 0 || !panel_) return;
  const int y = kTitleH + i * kRowH;
  gfx::pushRows(*panel_, 0, y0_, kW, y, y + kRowH, true);
}

void Sheet::pushTitle() {
  if (panel_) gfx::pushRows(*panel_, 0, y0_, kW, 0, kTitleH, true);
}

int Sheet::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!up_) return -1;
  const int row = e.y >= y0_ + kTitleH ? (e.y - y0_ - kTitleH) / kRowH : -1;
  const bool onCross = e.y >= y0_ && e.y < y0_ + kTitleH && e.inRightEdgeZone(kSheetCrossHitX);
  const int inRow = onCross ? kCross : row >= 0 && row < n_ ? row : -1;
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
  const int was = pressed_;
  pressed_ = -1;
  if (e.y < y0_ || was == kCross) return -2;  // outside, or ✕: close
  return inRow == kCross ? -1 : inRow;
}

// ---- VolumeSheet ----
//
// Screen y (the sheet from 124):
//   124-162  the output's icon and name, "volume", the value (Bold 22)
//   166-218  [ − ]  slider 76-243 (10 % ticks, a 22 px knob)  [ + ]
//   220-239  a hint

namespace {
constexpr int kVolH = kH - VolumeSheet::kY;  // 116
constexpr int kRowY = 44;                   // the controls' row, sheet coordinates
constexpr int kCtlH = 50;
constexpr int kTrackX = 76, kTrackW = 168;
constexpr int kMinusX = 8, kPlusX = 260, kButtonW = 52;
constexpr int kPlusHitX = 250;
}  // namespace

void VolumeSheet::open(bool bluetooth, const char* name, int volume, uint16_t accent, uint32_t nowMs) {
  bluetooth_ = bluetooth;
  copy(name_, sizeof(name_), name);
  volume_ = std::max(0, std::min(100, volume));
  accent_ = accent;
  pressed_ = None;
  touching_ = false;
  ignoring_ = false;
  ownUntilMs_ = 0;
  up_ = true;
  openedMs_ = nowMs;
  untilMs_ = nowMs + kCloseMs;
  draw();
}

int VolumeSheet::valueAt(int x) {
  const int v = (x - kTrackX) * 100 / kTrackW;
  const int stepped = (std::max(0, std::min(100, v)) + kStep / 2) / kStep * kStep;
  return std::min(100, stepped);
}

VolumeSheet::Part VolumeSheet::partAt(const InputEvent& e) const {
  const int y = e.y - kY;
  if (y < kRowY - 4 || y >= kRowY + kCtlH + 6) return None;
  if (e.inRightEdgeZone(kPlusHitX)) return Plus;  // + reaches the screen's edge
  if (e.x < kTrackX - 8) return Minus;
  return Slider;
}

void VolumeSheet::render() {
  M5Canvas& s = *panel_;
  Fonts& f = Fonts::instance();
  sheetCard(s, kVolH);
  // The output and the value.
  const uint16_t ic = bluetooth_ ? col::CYAN : col::SOFT;
  icons::drawCentred(s, bluetooth_ ? icons::kHeadphones : icons::kSpeaker, 28, 26, ic);
  char v[8];
  snprintf(v, sizeof(v), "%d%%", volume_);
  const int vw = f.draw(s, Font::Title, v, kW - 12, 26, 90, col::TXT, col::CARD, Fonts::Align::Right);
  const int nameW = f.draw(s, Font::Bold, name_, 46, 26, kW - 12 - vw - 12 - 46, col::TXT, col::CARD);
  const int room = kW - 12 - vw - 12 - (46 + nameW + 6);
  if (room > 40) f.draw(s, Font::Small, "volume", 46 + nameW + 6, 27, room, col::DIM, col::CARD);
  // − and +.
  const uint16_t minus = pressed_ == Minus ? col::BTN_HI : col::BTN;
  const uint16_t plus = pressed_ == Plus ? col::BTN_HI : col::BTN;
  s.fillRoundRect(kMinusX, kRowY + 3, kButtonW, 44, 10, minus);
  icons::drawCentred(s, icons::kMinus, kMinusX + kButtonW / 2, kRowY + 25, col::TXT);
  s.fillRoundRect(kPlusX, kRowY + 3, kButtonW, 44, 10, plus);
  icons::drawCentred(s, icons::kPlus, kPlusX + kButtonW / 2, kRowY + 25, col::TXT);
  // The slider: its track, the level in the accent, ticks every 10 %, the knob.
  const int ty = kRowY + 23;
  const int kx = kTrackX + volume_ * kTrackW / 100;
  s.fillRoundRect(kTrackX, ty, kTrackW, 5, 2, col::DIV);
  if (kx > kTrackX) s.fillRoundRect(kTrackX, ty, kx - kTrackX, 5, 2, accent_);
  for (int t = 0; t <= 10; ++t) s.fillRect(kTrackX + t * kTrackW / 10, ty + 13, 1, 2, col::FAINT);
  s.fillCircle(kx, ty + 2, 11, pressed_ == Slider ? col::SOFT : col::TXT);
  s.fillCircle(kx, ty + 2, 5, accent_);
  // The hint.
  f.draw(s, Font::Small,
         bluetooth_ ? "The headphones' own buttons change it too" : "Hold A or C to change it on any screen", kW / 2,
         kRowY + kCtlH + 12, kW - 24, col::FAINT, col::CARD, Fonts::Align::Centre);
}

void VolumeSheet::draw() {
  if (!up_ || !panel_) return;
  render();
  gfx::push(*panel_, 0, kY, kW, kVolH, true);
}

void VolumeSheet::setVolume(int volume, bool bluetooth, uint32_t nowMs) {
  if (!up_ || touching_ || static_cast<int32_t>(nowMs - ownUntilMs_) < 0) return;
  volume = std::max(0, std::min(100, volume));
  if (volume == volume_ && bluetooth == bluetooth_) return;
  volume_ = volume;
  bluetooth_ = bluetooth;
  draw();
}

VolumeSheet::Result VolumeSheet::onEvent(const InputEvent& e, uint32_t nowMs) {
  using T = InputEvent::Type;
  Result r;
  if (!up_) return r;
  untilMs_ = nowMs + kCloseMs;
  auto set = [&](int v) {
    v = std::max(0, std::min(100, v));
    if (v == volume_) return;
    volume_ = v;
    r.target = v;
    ownUntilMs_ = nowMs + 1000;  // the state catches up (Bluetooth applies it on its own task)
  };
  if (e.type == T::Down) {
    // A touch right after it opened is the finger that opened it coming
    // back (a double tap on the chip, or a retap): it lands on the slider.
    ignoring_ = static_cast<int32_t>(nowMs - openedMs_) < static_cast<int32_t>(kSettleMs);
  }
  if (ignoring_) {
    if (e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel) ignoring_ = false;
    return r;
  }
  switch (e.type) {
    case T::Down:
      // Only pressed: nothing changes until a tap or a drag (a finger
      // resting on the slider must not move the volume).
      touching_ = e.y >= kY;
      pressed_ = partAt(e);
      render();
      gfx::push(*panel_, 0, kY, kW, kVolH, true);
      break;
    case T::DragStart:
    case T::DragMove:
      if (pressed_ == Slider) {
        const int before = volume_;
        set(valueAt(e.x));
        if (volume_ != before) draw();
      } else if (pressed_ != None) {
        pressed_ = None;  // a drag off a button doesn't press it
        draw();
      }
      break;
    case T::Tap:
      if (e.y < kY) {
        r.close = true;
      } else if (pressed_ == Minus) {
        set(volume_ - kStep);
        r.tapped = true;
      } else if (pressed_ == Plus) {
        set(volume_ + kStep);
        r.tapped = true;
      } else if (pressed_ == Slider) {
        set(valueAt(e.x));  // the knob to the tap
        r.tapped = true;
      }
      pressed_ = None;
      touching_ = false;
      if (!r.close) draw();
      break;
    case T::Release:
    case T::DragEnd:
    case T::Cancel:
      pressed_ = None;
      touching_ = false;
      draw();
      break;
    default:
      break;
  }
  return r;
}

// ---- JumpGrid ----

void JumpGrid::open(const char* title, const char (*labels)[4], const bool* enabled, int current, uint16_t accent) {
  copy(title_, sizeof(title_), title);
  for (int i = 0; i < jump::kGridCells; ++i) {
    copy(labels_[i], sizeof(labels_[i]), labels[i]);
    enabled_[i] = enabled[i];
  }
  current_ = current;
  accent_ = accent;
  pressed_ = -1;
  up_ = true;
  draw();
}

void JumpGrid::drawHeader() {
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kHeaderH, col::HEAD);
  const int w = f.draw(s, Font::Bold, "Jump to", 12, 17, 120, col::TXT, col::HEAD);
  f.draw(s, Font::Small, title_, 12 + w + 8, 18, 250 - (12 + w + 8), col::DIM, col::HEAD);
  const uint16_t pill = pressed_ == -2 ? col::BTN_HI : col::BTN;
  s.fillRoundRect(262, 5, 50, 26, 13, pill);
  icons::drawCentred(s, icons::kCross, 287, 18, col::TXT);
  s.fillRect(0, kHeaderH - 2, kW, 2, accent_);
  gfx::push(s, 0, kHeaderY, kW, kHeaderH, true);
}

void JumpGrid::renderGrid() {
  M5Canvas& s = *panel_;
  Fonts& f = Fonts::instance();
  s.fillSprite(col::BG);
  for (int i = 0; i < jump::kGridCells; ++i) {
    const int x = kGridX + (i % 7) * kCellW + 2;
    const int y = kGridY - kListY + (i / 7) * kCellH + 2;
    const int w = kCellW - 4, h = kCellH - 4;
    if (!labels_[i][0]) continue;
    const bool on = enabled_[i];
    const bool down = pressed_ == i;
    uint16_t fg = col::FAINT, bg = col::BG;
    if (on) {
      bg = down ? accent_ : col::BTN;
      fg = down ? col::DARK : col::TXT;
      s.fillRoundRect(x, y, w, h, 8, bg);
    }
    if (i == current_ && on) {
      s.drawRoundRect(x, y, w, h, 8, col::TXT);
      s.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, col::TXT);
    }
    if (i == kBack && labels_[i][0] == '<') {
      icons::drawCentred(s, icons::kChevronLeft, x + w / 2, y + h / 2, fg);
      continue;
    }
    const Font font = labels_[i][1] ? Font::Bold : Font::Title;  // "Ka" smaller than "K"
    f.draw(s, font, labels_[i], x + w / 2, y + h / 2, w - 2, fg, bg, Fonts::Align::Centre);
  }
}

void JumpGrid::draw() {
  if (!up_ || !panel_) return;
  drawHeader();
  renderGrid();
  gfx::push(*panel_, 0, kListY, kW, kListH, true);
}

int JumpGrid::cellAt(const InputEvent& e) const {
  int x = e.x;
  // The last column's hit area reaches the screen's edge.
  if (e.atRightEdge() || x >= kGridX + 7 * kCellW) x = kGridX + 7 * kCellW - 1;
  return jump::cellAt(x, e.y, kGridX, kGridY, kCellW, kCellH);
}

int JumpGrid::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!up_) return -1;
  const bool onCross = e.y >= kHeaderY && e.y < kHeaderY + kHeaderH && e.inRightEdgeZone(250);
  if (e.type == T::Down) {
    const int c = onCross ? -2 : cellAt(e);
    pressed_ = c == -2 || (c >= 0 && enabled_[c]) ? c : -1;
    if (pressed_ != -1) draw();
    return -1;
  }
  if (e.type == T::DragStart || e.type == T::Cancel || e.type == T::Release) {
    if (pressed_ != -1) {
      pressed_ = -1;
      draw();
    }
    return -1;
  }
  if (e.type != T::Tap) return -1;
  const int c = pressed_;
  pressed_ = -1;
  return c;
}

// ---- Dialog ----

void Dialog::open(const char* title, const char* body, const char* const* buttons, int n, uint16_t accent,
                  bool danger) {
  copy(title_, sizeof(title_), title);
  copy(body_, sizeof(body_), body);
  n_ = std::max(1, std::min(2, n));
  for (int i = 0; i < n_; ++i) copy(buttons_[i], sizeof(buttons_[i]), buttons[i]);
  accent_ = accent;
  danger_ = danger;
  status_[0] = 0;
  icon_ = nullptr;
  slashed_ = false;
  pressed_ = -1;
  up_ = true;
  draw();
}

void Dialog::setIcon(const icons::Icon* icon, uint16_t colour, bool slashed) {
  icon_ = icon;
  iconColour_ = colour;
  slashed_ = slashed;
  draw();
}

void Dialog::setStatus(const char* text, uint16_t colour) {
  if (!text) text = "";
  if (strcmp(text, status_) == 0 && colour == statusColour_) return;
  copy(status_, sizeof(status_), text);
  statusColour_ = colour;
  draw();
}

int Dialog::titleRoom(bool icon) {
  // As render() lays it out: the icon (the headphones, 24 px) and a gap.
  const int tx = icon ? 14 + icons::kHeadphones.w + 8 : 16;
  return kW - 16 - tx;
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
  int tx = 16;
  if (icon_) {
    icons::draw(s, *icon_, 14, 22 - icon_->h / 2, iconColour_);
    if (slashed_) s.drawLine(14, 22 + icon_->h / 2, 14 + icon_->w, 22 - icon_->h / 2, col::RED);
    tx = 14 + icon_->w + 8;
  }
  f.draw(s, Font::Bold, title_, tx, 22, kW - 16 - tx, col::TXT, col::CARD);
  char lines[3][64];
  const int maxLines = status_[0] ? 2 : 3;
  const int nl =
      textfit::wrap(f.fit(Font::Small), body_, strlen(body_), kW - 32, maxLines, &lines[0][0], sizeof(lines[0]));
  for (int i = 0; i < nl; ++i) f.draw(s, Font::Small, lines[i], 16, 50 + i * 18, kW - 32, col::SOFT, col::CARD);
  if (status_[0]) f.draw(s, Font::Small, status_, 16, 50 + nl * 18 + 6, kW - 32, statusColour_, col::CARD);
  const int bw = n_ == 1 ? kW - 24 : (kW - 30) / 2;
  for (int i = 0; i < n_; ++i) {
    const int x = 12 + i * (bw + 6);
    const bool primary = i == n_ - 1;
    const uint16_t b = i == pressed_ ? col::BTN_HI : primary ? (danger_ ? col::RED : accent_) : col::BTN;
    s.fillRoundRect(x, kH - 46, bw, 36, 8, b);
    // A label too wide for its button ("Play on speaker": 127 px of 123)
    // in Small, not cut.
    Font font = primary ? Font::Bold : Font::Body;
    if (f.width(font, buttons_[i]) > bw - 8) font = Font::Small;
    f.draw(s, font, buttons_[i], x + bw / 2, kH - 28, bw - 8,
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

// ---- Coach (the first-boot tips) ----
//
// Screen y: 36-71 its header ("Tip 1 of 2", the Next / Got it pill), 72-239
// the card (the panel sprite).

void Coach::open(int card, uint16_t accent) {
  card_ = std::max(0, std::min(kCards - 1, card));
  accent_ = accent;
  pressed_ = false;
  up_ = true;
  draw();
}

void Coach::drawHeader() {
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kHeaderH, col::HEAD);
  char t[24];
  snprintf(t, sizeof(t), "Tip %d of %d", card_ + 1, kCards);
  const int w = f.draw(s, Font::Bold, t, 12, 17, 140, col::TXT, col::HEAD);
  f.draw(s, Font::Small, "tap to go on", 12 + w + 8, 18, 120, col::DIM, col::HEAD);
  const char* label = card_ + 1 < kCards ? "Next" : "Got it";
  const int pw = f.width(Font::Bold, label) + 28;
  const uint16_t pill = pressed_ ? col::SOFT : accent_;
  s.fillRoundRect(kW - 6 - pw, 5, pw, 26, 13, pill);
  f.draw(s, Font::Bold, label, kW - 6 - pw / 2, 17, pw - 6, col::DARK, pill, Fonts::Align::Centre);
  s.fillRect(0, kHeaderH - 2, kW, 2, accent_);
  gfx::push(s, 0, kHeaderY, kW, kHeaderH, true);
}

void Coach::render() {
  using namespace uitext;
  M5Canvas& s = *panel_;
  Fonts& f = Fonts::instance();
  s.fillSprite(col::BG);
  if (card_ == 0) {
    // The three red dots under the glass, and what each does (spec §4).
    // Every text fits its room (test_ui_library measures them).
    f.draw(s, Font::Bold, kCoachTitle, kW / 2, 14, kCoachTextW, col::TXT, col::BG, Fonts::Align::Centre);
    f.draw(s, Font::Small, kCoachLine, kW / 2, 34, kCoachTextW, col::DIM, col::BG, Fonts::Align::Centre);
    for (int b = 0; b < 3; ++b) {
      const int cx = 54 + b * 106;  // boxes of 102 with 4 px between them
      s.fillRoundRect(cx - kCoachBoxW / 2, 48, kCoachBoxW, 90, 10, col::CARD);
      const int tw = kCoachBoxTextW;
      f.draw(s, Font::Bold, kCoachClick[b], cx, 64, tw, col::TXT, col::CARD, Fonts::Align::Centre);
      f.draw(s, Font::Small, "hold:", cx, 86, tw, col::DIM, col::CARD, Fonts::Align::Centre);
      f.draw(s, Font::Small, kCoachHold[b], cx, 103, tw, col::SOFT, col::CARD, Fonts::Align::Centre);
      if (kCoachHold2[b][0]) {
        f.draw(s, Font::Small, kCoachHold2[b], cx, 119, tw, col::SOFT, col::CARD, Fonts::Align::Centre);
      }
      // An arrow down to its dot.
      s.fillRect(cx - 2, 142, 4, 12, col::RED);
      s.fillTriangle(cx - 8, 152, cx + 8, 152, cx, 164, col::RED);
    }
  } else {
    // Tap the tab you're on again: back to its start.
    const int cx = kW / 2;
    s.fillTriangle(cx - 12, 16, cx + 12, 16, cx, 2, accent_);
    s.fillRect(cx - 3, 14, 6, 16, accent_);
    f.draw(s, Font::Title, kCoachTab[0], cx, 52, kCoachTextW, col::TXT, col::BG, Fonts::Align::Centre);
    f.draw(s, Font::Title, kCoachTab[1], cx, 78, kCoachTextW, col::TXT, col::BG, Fonts::Align::Centre);
    f.draw(s, Font::Small, kCoachTabMore[0], cx, 108, kCoachTextW, col::DIM, col::BG, Fonts::Align::Centre);
    f.draw(s, Font::Small, kCoachTabMore[1], cx, 125, kCoachTextW, col::DIM, col::BG, Fonts::Align::Centre);
    f.draw(s, Font::Small, kCoachTabMore[2], cx, 152, kCoachTextW, col::FAINT, col::BG, Fonts::Align::Centre);
  }
}

void Coach::draw() {
  if (!up_ || !panel_) return;
  drawHeader();
  render();
  gfx::push(*panel_, 0, kListY, kW, kListH, true);
}

Coach::Result Coach::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!up_) return Result::None;
  const bool onPill = e.y >= kHeaderY && e.y < kHeaderY + kHeaderH && e.inRightEdgeZone(200);
  if (e.type == T::Down) {
    if (onPill) {
      pressed_ = true;
      drawHeader();
    }
    return Result::None;
  }
  if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
    if (pressed_) {
      pressed_ = false;
      drawHeader();
    }
    return Result::None;
  }
  if (e.type != T::Tap) return Result::None;
  pressed_ = false;
  // Anywhere on the card goes on: the pill is only the obvious place.
  if (card_ + 1 < kCards) {
    ++card_;
    draw();
    return Result::Next;
  }
  up_ = false;
  return Result::Done;
}

}  // namespace ui
