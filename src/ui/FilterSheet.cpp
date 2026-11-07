// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/FilterSheet.h"

#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Overlays.h"
#include "ui/Theme.h"

namespace ui {

using filtergrid::Cell;
using moodfilter::Filter;
namespace fg = filtergrid;

namespace {

constexpr int kPanelH = kH - FilterSheet::kY;  // 204: the overlays' panel (sheet::kPanelH)
static_assert(kPanelH == sheet::kPanelH, "the editor is the overlays' panel, whole");
// A word's room in its cell: 3 px a side (test_ui_library measures every
// language's against it).
constexpr int kTextInset = 3;

// The rows' cells, left to right (row 3 has two, then the level cell).
constexpr Cell kTrayCells[3][3] = {
    {Cell::Happy, Cell::NeutralV, Cell::Sad},
    {Cell::Relaxed, Cell::NeutralE, Cell::Aggressive},
    {Cell::Instrumental, Cell::Vocals, Cell::None},
};
constexpr Cell kChips[3] = {Cell::Acoustic, Cell::Party, Cell::Danceable};

// The panel's row for screen row `y`.
constexpr int py(int y) { return y - FilterSheet::kY; }

bool isChip(Cell c) { return c == Cell::Acoustic || c == Cell::Party || c == Cell::Danceable; }

}  // namespace

void FilterSheet::open(FilterOwner* owner, uint16_t accent) {
  owner_ = owner;
  accent_ = accent;
  pressed_ = Cell::None;
  filter_ = Filter::unpack(owner ? owner->filterWord() : 0);
  opened_ = filter_.pack();
  // The words and the grid in the UI's language (a language change closes
  // it: Ui::languageChanged()).
  Fonts& f = Fonts::instance();
  words_ = fg::words();
  layout_ = fg::layout(words_, f.fit(Font::Body), f.fit(Font::Small));
  up_ = owner != nullptr;
  draw();
}

void FilterSheet::sync() {
  if (!up_) return;
  filter_ = Filter::unpack(owner_->filterWord());
  draw();
}

// ---- drawing ----

void FilterSheet::render() {
  M5Canvas* panel = overlayPanel();
  if (!panel) return;
  M5Canvas& s = *panel;
  // Its card, rounded at the top only, on the header row's colour (as the
  // design's frames: what it covers there is the page's header).
  s.fillRect(0, 0, kW, kPanelH, col::HEAD);
  s.fillRoundRect(0, 0, kW, kPanelH + 12, 12, col::CARD);
  setPanelOwner(this);
  renderClear();
  renderCount();
  renderCross();
  for (int row = 0; row < 3; ++row) renderTray(row);
  renderLevel();
  for (Cell c : kChips) renderChip(c);
}

void FilterSheet::renderClear() {
  M5Canvas& s = *overlayPanel();
  const fg::Box b = fg::box(layout_, Cell::Clear);
  const int y = py(b.y);
  // TXT when there is something to clear, a level included; else FAINT.
  const uint16_t bg = pressed_ == Cell::Clear ? col::BTN_HI : col::BTN;
  s.fillRect(b.x, y, b.w, b.h, col::CARD);  // (its corners, drawn again alone)
  s.fillRoundRect(b.x, y, b.w, b.h, fg::kPillR, bg);
  Fonts::instance().draw(s, Font::Small, words_.cell[fg::index(Cell::Clear)], b.x + b.w / 2, y + b.h / 2,
                         b.w - fg::kClearPad, fg::anythingSet(filter_) ? col::TXT : col::FAINT, bg,
                         Fonts::Align::Centre);
}

void FilterSheet::renderCount() {
  M5Canvas& s = *overlayPanel();
  Fonts& f = Fonts::instance();
  // Between the two pills: "312 of 1,741 albums" (or its short form), SOFT,
  // amber at 0; with nothing set, the whole list's "1,741 albums".
  const int left = fg::kClearX + layout_.w[fg::index(Cell::Clear)];
  s.fillRect(left, py(fg::kPillY), fg::kCrossX - left, fg::kPillH, col::CARD);
  if (!owner_) return;
  const bool active = filter_.active();
  const uint32_t fits = owner_->filterFits(), total = owner_->filterTotal();
  char text[fg::kCountBytes];
  fg::countText(f.fit(Font::Small), layout_.countW, owner_->filterScope(), active, fits, total, text, sizeof(text));
  const uint16_t ink = (active ? fits : total) == 0 ? col::AMBER : col::SOFT;
  f.draw(s, Font::Small, text, layout_.countX, py(fg::kPillY) + fg::kPillH / 2, layout_.countW, ink, col::CARD,
         Fonts::Align::Centre);
}

void FilterSheet::renderCross() {
  M5Canvas& s = *overlayPanel();
  const int y = py(fg::kPillY);
  const uint16_t bg = pressed_ == Cell::Cross ? col::BTN_HI : col::BTN;
  s.fillRect(fg::kCrossX, y, fg::kCrossW, fg::kPillH, col::CARD);
  s.fillRoundRect(fg::kCrossX, y, fg::kCrossW, fg::kPillH, fg::kPillR, bg);
  icons::drawCentred(s, icons::kCross, fg::kCrossX + fg::kCrossW / 2, y + fg::kPillH / 2, col::TXT);
}

// A tray (rows 1-3): its cells side by side, the pick a segment in the
// accent (DARK text; SOFT under a finger), a cell under a finger BTN_HI,
// and a 1 px line between two plain neighbours (7.1).
void FilterSheet::renderTray(int row) {
  M5Canvas& s = *overlayPanel();
  Fonts& f = Fonts::instance();
  const Font font = layout_.small ? Font::Small : Font::Body;
  const int y = py(fg::kRowY[row]);
  const int trayW = layout_.trayW[row];
  s.fillRect(fg::kRowX, y, trayW, fg::kRowH, col::CARD);
  s.fillRoundRect(fg::kRowX, y, trayW, fg::kRowH, fg::kTrayR, col::BTN);
  bool plain[3] = {};
  for (int k = 0; k < 3 && kTrayCells[row][k] != Cell::None; ++k) {
    const Cell c = kTrayCells[row][k];
    const int x = layout_.x[fg::index(c)], w = layout_.w[fg::index(c)];
    const bool on = fg::picked(filter_, c), down = pressed_ == c;
    uint16_t bg = col::BTN;
    if (on || down) {
      bg = on ? (down ? col::SOFT : accent_) : col::BTN_HI;
      s.fillRoundRect(x, y + fg::kSegmentInset, w, fg::kRowH - 2 * fg::kSegmentInset, fg::kSegmentR, bg);
    }
    plain[k] = !on && !down;
    f.draw(s, font, words_.cell[fg::index(c)], x + w / 2, y + fg::kRowH / 2, w - 2 * kTextInset,
           on ? col::DARK : col::TXT, bg, Fonts::Align::Centre);
  }
  for (int k = 1; k < 3 && kTrayCells[row][k] != Cell::None; ++k) {
    if (!plain[k - 1] || !plain[k]) continue;
    s.drawFastVLine(layout_.x[fg::index(kTrayCells[row][k])], y + fg::kSeparatorTop, fg::kSeparatorH, col::DIV);
  }
}

// The level cell: its word (Small) over three dots, the level's lit. Live
// (TXT, the dot in the accent) only while a group is set: a level alone
// filters nothing (4.1), so it is dim then (DIM), its word kept.
void FilterSheet::renderLevel() {
  M5Canvas& s = *overlayPanel();
  const fg::Box b = fg::box(layout_, Cell::Level);
  const int y = py(b.y);
  const bool live = filter_.active(), down = pressed_ == Cell::Level;
  const uint16_t bg = down ? col::BTN_HI : col::CARD;
  s.fillRect(b.x, y, b.w, b.h, col::CARD);
  if (down) s.fillRoundRect(b.x, y, b.w, b.h, fg::kTrayR, bg);
  s.drawRoundRect(b.x, y, b.w, b.h, fg::kTrayR, col::FAINT);
  const int cx = b.x + b.w / 2;
  Fonts::instance().draw(s, Font::Small, fg::levelWord(filter_.level), cx, y + fg::kLevelLabelY, b.w - fg::kLevelPad,
                         live ? col::TXT : col::DIM, bg, Fonts::Align::Centre);
  const int lit = fg::levelDot(filter_.level);
  for (int k = 0; k < 3; ++k) {
    const uint16_t ink = k != lit ? col::FAINT : live ? accent_ : col::DIM;
    s.fillCircle(cx + (k - 1) * fg::kLevelDotStep, y + fg::kLevelDotsY, fg::kLevelDotD / 2, ink);
  }
}

// A chip (row 4): picked in the accent (DARK text; SOFT under a finger),
// else BTN (BTN_HI under one). A tag wrapped in two (Russian's "Для
// вечеринки") is drawn glyphs only, each line's fill would cut the other's
// descenders (as the toast's two lines).
void FilterSheet::renderChip(Cell c) {
  M5Canvas& s = *overlayPanel();
  Fonts& f = Fonts::instance();
  const Font font = layout_.small ? Font::Small : Font::Body;
  const fg::Box b = fg::box(layout_, c);
  const int y = py(b.y);
  const bool on = fg::picked(filter_, c), down = pressed_ == c;
  const uint16_t bg = on ? (down ? col::SOFT : accent_) : down ? col::BTN_HI : col::BTN;
  const uint16_t ink = on ? col::DARK : col::TXT;
  s.fillRect(b.x, y, b.w, b.h, col::CARD);
  s.fillRoundRect(b.x, y, b.w, b.h, fg::kChipR, bg);
  const char* word = words_.cell[fg::index(c)];
  const int wrap = layout_.wrap[fg::index(c)];
  const int cx = b.x + b.w / 2, room = b.w - 2 * kTextInset;
  if (wrap) {
    f.draw(s, font, word, static_cast<size_t>(wrap - 1), cx, y + fg::kWrapLineY[0], room, ink, ink,
           Fonts::Align::Centre);
    f.draw(s, font, word + wrap, cx, y + fg::kWrapLineY[1], room, ink, ink, Fonts::Align::Centre);
  } else {
    f.draw(s, font, word, cx, y + fg::kRowH / 2, room, ink, bg, Fonts::Align::Centre);
  }
}

void FilterSheet::push(int x, int y, int w, int h) {
  // Not `over`: a toast over the title row stays on top (4.3).
  if (M5Canvas* panel = overlayPanel()) gfx::pushRect(*panel, x, py(y), w, h, x, y, false);
}

void FilterSheet::draw() {
  M5Canvas* panel = overlayPanel();
  if (!up_ || !panel) return;
  render();
  gfx::push(*panel, 0, kY, kW, kPanelH, false);
}

void FilterSheet::drawTop() {
  M5Canvas* panel = overlayPanel();
  if (!up_ || !panel) return;
  if (panelOwner() != this) render();
  gfx::pushRows(*panel, 0, kY, kW, 0, kListY - kY, false);
}

void FilterSheet::drawUnit(Cell c) {
  if (!up_ || !overlayPanel() || c == Cell::None) return;
  if (panelOwner() != this) render();
  const int row = fg::rowOf(c);
  if (row >= 0 && row < 3 && c != Cell::Level) {
    renderTray(row);
    push(fg::kRowX, fg::kRowY[row], layout_.trayW[row], fg::kRowH);
    return;
  }
  switch (c) {
    case Cell::Level: renderLevel(); break;
    case Cell::Clear: renderClear(); break;
    case Cell::Cross: renderCross(); break;
    default:
      if (!isChip(c)) return;
      renderChip(c);
      break;
  }
  const fg::Box b = fg::box(layout_, c);
  push(b.x, b.y, b.w, b.h);
}

void FilterSheet::drawCount() {
  if (!up_ || !overlayPanel()) return;
  if (panelOwner() != this) render();
  renderCount();
  const int left = fg::kClearX + layout_.w[fg::index(Cell::Clear)];
  push(left, fg::kPillY, fg::kCrossX - left, fg::kPillH);
}

// ---- touch ----

Cell FilterSheet::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!up_) return Cell::None;
  // A press draws its cell alone (a tray, a chip, a pill): the whole panel
  // would be about 55 ms twice a tap, as the sleep timer's sheet measured.
  if (e.type == T::Down) {
    pressed_ = fg::hit(layout_, e.x, e.y, e.atRightEdge());
    drawUnit(pressed_);
    return Cell::None;
  }
  if (e.type == T::DragStart || e.type == T::Cancel || e.type == T::Release) {
    const Cell was = pressed_;
    pressed_ = Cell::None;
    drawUnit(was);
    return Cell::None;
  }
  if (e.type != T::Tap) return Cell::None;
  // (A Tap is where its Down landed: the cell pressed.)
  const Cell was = pressed_;
  pressed_ = Cell::None;
  const Cell c = was != Cell::None ? was : fg::hit(layout_, e.x, e.y, e.atRightEdge());
  if (c == Cell::Cross) return c;
  if (c == Cell::None || (c == Cell::Clear && !fg::anythingSet(filter_))) {
    drawUnit(was);  // nothing to do: its press look goes
    return Cell::None;
  }
  return c;  // tap() draws it, in its new state
}

void FilterSheet::show(Cell c, const Filter& f) {
  const Filter old = filter_;
  filter_ = f;
  if (!overlayPanel()) return;
  if (panelOwner() != this) render();
  if (c == Cell::Clear) {
    // The whole grid, and Clear itself (FAINT now).
    for (int row = 0; row < 3; ++row) renderTray(row);
    renderLevel();
    for (Cell chip : kChips) renderChip(chip);
    renderClear();
    gfx::pushRows(*overlayPanel(), 0, kY, kW, py(fg::kRowY[0]), kPanelH, false);
    const fg::Box b = fg::box(layout_, Cell::Clear);
    push(b.x, b.y, b.w, b.h);
    return;
  }
  drawUnit(c);
  if (old.active() != f.active() && c != Cell::Level) drawUnit(Cell::Level);
  if (fg::anythingSet(old) != fg::anythingSet(f)) drawUnit(Cell::Clear);
}

void FilterSheet::tap(Cell c) {
  if (!up_ || !owner_ || c == Cell::None || c == Cell::Cross) return;
  const Filter was = filter_;
  const Filter now = fg::tap(was, c);
  show(c, now);
  if (!owner_->setFilterWord(now.pack())) {
    show(c, was);  // the old one stays (the owner's toast says why)
    return;
  }
  drawCount();
}

}  // namespace ui
