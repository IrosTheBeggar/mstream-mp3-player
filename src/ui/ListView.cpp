#include "ui/ListView.h"

#include <M5Unified.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/Psram.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace ui {

namespace {
constexpr int kPitch = ListLayout::kPitch;
constexpr int kRailW = 36;   // the rail's column, x 284-319 (its touch zone starts at kEdgeHitX)
constexpr int kThumbH = 30;
}  // namespace

bool ListView::begin(ListScroller& scroller, Input& input) {
  vs_ = &scroller;
  input_ = &input;
  if (!slots_) {
    void* mem = psramAlloc(kSlots * sizeof(Slot));
    if (!mem) return false;
    slots_ = static_cast<Slot*>(mem);
    for (int i = 0; i < kSlots; ++i) new (&slots_[i]) Slot();
    for (int i = 0; i < kSlots; ++i) {
      M5Canvas& s = slots_[i].sprite;
      s.setPsram(true);  // before createSprite(): otherwise internal RAM
      s.setColorDepth(16);
      if (!s.createSprite(kW, kRowH)) {
        dropSlots();
        return false;
      }
    }
  }
  if (!rail_) {
    rail_ = psramNew<M5Canvas>();
    if (!rail_) return false;
    rail_->setPsram(true);
    rail_->setColorDepth(16);
    if (!rail_->createSprite(kRailW, kHeight)) {  // the tallest band
      psramDelete(rail_);
      rail_ = nullptr;
      return false;
    }
  }
  return true;
}

void ListView::setHeight(int h) {
  h = std::max(kPitch, std::min(kHeight, h));
  if (h == height_) return;
  height_ = h;
  if (src_) {
    setEdge();
    scroll_.setExtent(static_cast<float>(layout_.contentPx()), height_);
    scroll_.jumpTo(static_cast<float>(ListLayout::clamp(offset(), layout_.maxOffset(height_))));
    jumped_ = true;
  }
  invalidate();
}

void ListView::dropSlots() {
  if (!slots_) return;
  for (int i = 0; i < kSlots; ++i) {
    slots_[i].sprite.deleteSprite();
    slots_[i].~Slot();
  }
  psramFree(slots_);
  slots_ = nullptr;
}

// ---- attaching, data changes ----

void ListView::setEdge() {
  const bool long_ = layout_.contentPx() > height_;
  if (src_ && src_->alphabetical() && src_->railRows() > kRailMinRows && long_) {
    edge_ = Edge::Rail;
  } else if (long_) {
    edge_ = Edge::Bar;
  } else {
    edge_ = Edge::None;
  }
}

void ListView::attach(Source* src, NavModel::PageRef* ref, int32_t defaultOffset) {
  src_ = src;
  ref_ = ref;
  touchOn_ = TouchOn::None;
  pressedItem_ = downItem_ = -1;
  pressedButton_ = -1;
  pressedX_ = -1;
  emptyPressed_ = -1;
  railKeyShown_ = -1;
  layout_ = ListLayout();
  layout_.set(src->rows(), src->topBar());
  const int32_t exp = ref ? ref->expanded : -1;
  if (exp >= 0 && static_cast<uint32_t>(exp) < layout_.rows()) layout_.setExpanded(exp, 0, height_);
  setEdge();
  scroll_.setExtent(static_cast<float>(layout_.contentPx()), height_);
  const int32_t want = ref && ref->scrollPx >= 0 ? ref->scrollPx : defaultOffset;
  scroll_.jumpTo(static_cast<float>(ListLayout::clamp(want, layout_.maxOffset(height_))));
  jumped_ = true;
  invalidate();
}

void ListView::saveRef() {
  if (!ref_) return;
  ref_->scrollPx = static_cast<int32_t>(std::lround(scroll_.rowTarget(scroll_.offset(), 0.0f)));
  ref_->expanded = layout_.expanded();
}

void ListView::detach() {
  if (!src_) return;
  saveRef();
  src_ = nullptr;
  ref_ = nullptr;
  touchOn_ = TouchOn::None;
}

void ListView::invalidate() {
  if (slots_) {
    for (int i = 0; i < kSlots; ++i) slots_[i].item = -1;
  }
  force_ = true;
  railUp_ = false;  // the frame redraws the band; the rail with it if it's wanted
}

void ListView::reload() {
  if (!src_) return;
  const int32_t exp = layout_.expanded();
  layout_.set(src_->rows(), src_->topBar());
  if (exp >= 0 && layout_.expanded() < 0) pressedItem_ = -1;
  setEdge();
  scroll_.setExtent(static_cast<float>(layout_.contentPx()), height_);
  if (!scroll_.moving()) scroll_.jumpTo(static_cast<float>(ListLayout::clamp(offset(), layout_.maxOffset(height_))));
  invalidate();
}

void ListView::refreshAll() {
  if (!src_) return;
  invalidate();
}

void ListView::refreshRow(uint32_t row) {
  if (!src_ || row >= layout_.rows()) return;
  const uint32_t item = layout_.itemOfRow(row);
  for (int i = 0; i < kSlots; ++i) {
    if (slots_[i].item == static_cast<int32_t>(item)) slots_[i].item = -1;
  }
  pushItemInPlace(item);
}

int32_t ListView::offset() const { return static_cast<int32_t>(std::lround(scroll_.offset())); }

void ListView::scrollTo(int32_t off) {
  scroll_.jumpTo(static_cast<float>(ListLayout::clamp(off, layout_.maxOffset(height_))));
  jumped_ = true;
}

bool ListView::visibleRows(uint32_t* first, uint32_t* last) const {
  if (!src_ || layout_.rows() == 0) return false;
  const int32_t off = std::max<int32_t>(0, offset());
  const int32_t i0 = layout_.itemAt(off);
  int32_t i1 = layout_.itemAt(off + height_ - 1);
  if (i1 < 0) i1 = static_cast<int32_t>(layout_.itemCount()) - 1;
  if (i0 < 0 || i1 < i0) return false;
  int32_t r0 = -1, r1 = -1;
  for (int32_t i = i0; i <= i1; ++i) {
    const ListLayout::Item it = layout_.item(static_cast<uint32_t>(i));
    if (it.kind != ListLayout::Kind::Row) continue;
    if (r0 < 0) r0 = it.row;
    r1 = it.row;
  }
  if (r0 < 0) return false;
  *first = static_cast<uint32_t>(r0);
  *last = static_cast<uint32_t>(r1);
  return true;
}

void ListView::scrollToRow(uint32_t row) { scrollTo(layout_.topOf(layout_.itemOfRow(row), height_)); }

void ListView::revealRow(uint32_t row) { scrollTo(layout_.reveal(layout_.itemOfRow(row), offset(), height_)); }

void ListView::collapse() {
  if (layout_.expanded() >= 0) setExpanded(-1);
}

void ListView::setExpanded(int32_t row) {
  const int32_t o = layout_.setExpanded(row, offset(), height_);
  scroll_.setExtent(static_cast<float>(layout_.contentPx()), height_);
  scroll_.jumpTo(static_cast<float>(o));
  jumped_ = true;
  setEdge();
  // Every item from the row down moved: the band is redrawn in place.
  invalidate();
  if (ref_) ref_->expanded = layout_.expanded();
}

// ---- rendering ----

ListView::Slot* ListView::slotFor(uint32_t item) {
  for (int i = 0; i < kSlots; ++i) {
    if (slots_[i].item == static_cast<int32_t>(item)) return &slots_[i];
  }
  for (int i = 0; i < kSlots; ++i) {
    const int32_t r = slots_[i].item;
    if (r < 0 || r < static_cast<int32_t>(keepFirst_) || r > static_cast<int32_t>(keepLast_)) {
      slots_[i].item = -1;
      return &slots_[i];
    }
  }
  slots_[0].item = -1;
  return &slots_[0];
}

ListView::Slot* ListView::renderedSlot(uint32_t item) {
  Slot* s = slotFor(item);
  if (s->item != static_cast<int32_t>(item)) {
    const uint32_t t0 = micros();
    renderItem(s->sprite, item);
    ++cost_.renders;
    cost_.renderUs += micros() - t0;
    s->item = static_cast<int32_t>(item);
  }
  return s;
}

int ListView::buttonAt(int x, int n) const {
  if (n <= 0) return -1;
  const int w = layoutWidth();
  const int b = x * n / w;
  return b < 0 ? 0 : b >= n ? n - 1 : b;
}

void ListView::renderBar(M5Canvas& c, int32_t row, bool inline_) {
  Fonts& f = Fonts::instance();
  const uint16_t bg = inline_ ? col::ROW_SEL : col::BG;
  c.fillSprite(bg);
  const char* labels[3] = {"", "", ""};
  const int n = std::min(3, src_->actions(row, labels));
  if (n <= 0) return;
  const int x0 = 8, x1 = layoutWidth() - 8, gap = 6;
  const int bw = (x1 - x0 - gap * (n - 1)) / n;
  const bool thisPressed = pressedItem_ >= 0 && layout_.item(static_cast<uint32_t>(pressedItem_)).row == row &&
                           layout_.item(static_cast<uint32_t>(pressedItem_)).kind !=
                               ListLayout::Kind::Row;
  for (int i = 0; i < n; ++i) {
    const int x = x0 + i * (bw + gap);
    const bool primary = i == 0;
    const bool down = thisPressed && pressedButton_ == i;
    const uint16_t fill = primary ? (down ? col::SOFT : src_->accent()) : (down ? col::BTN_HI : col::BTN);
    c.fillRoundRect(x, 5, bw, 32, 8, fill);
    f.draw(c, primary ? Font::Bold : Font::Body, labels[i], x + bw / 2, 21, bw - 8, primary ? col::DARK : col::TXT,
           fill, Fonts::Align::Centre);
  }
}

void ListView::renderItem(M5Canvas& c, uint32_t item) {
  const ListLayout::Item it = layout_.item(item);
  switch (it.kind) {
    case ListLayout::Kind::TopBar:
      renderBar(c, -1, false);
      return;
    case ListLayout::Kind::InlineBar:
      renderBar(c, it.row, true);
      return;
    case ListLayout::Kind::Row:
      break;
    default:
      c.fillSprite(col::BG);
      return;
  }
  const auto row = static_cast<uint32_t>(it.row);
  const bool selecting = src_->selecting();
  const bool selected = selecting && src_->selected(row);
  const bool pressed = pressedItem_ == static_cast<int32_t>(item);
  const bool expanded = layout_.expanded() == it.row;
  const uint16_t bg = pressed || expanded ? col::ROW_SEL
                     : selected           ? col::SELECTED
                     : src_->tinted(row)  ? col::ROW_SEL
                                          : col::BG;
  c.fillSprite(bg);
  Row r{c, row, 0, layoutWidth(), bg, pressed, expanded, selected, pressed ? pressedX_ : -1};
  if (selecting) {
    // The checkbox: an empty ring, or the accent disc with a check.
    if (selected) {
      c.fillCircle(22, 21, 11, src_->accent());
      icons::drawCentred(c, icons::kCheck, 22, 21, col::DARK);
    } else {
      c.drawCircle(22, 21, 10, col::FAINT);
      c.drawCircle(22, 21, 11, col::FAINT);
    }
    r.x = 40;
  }
  src_->drawRow(r);
  if (!expanded) c.drawFastHLine(r.x + 8, kRowH - 1, layoutWidth() - r.x - 8, col::ROW_DIV);
}

void ListView::render(int32_t off) {
  if (force_) vs_->invalidate();
  const uint32_t n = layout_.itemCount();
  const int32_t top = std::max<int32_t>(off, 0);
  keepFirst_ = static_cast<uint32_t>(top / kPitch);
  keepLast_ = std::min<uint32_t>(n ? n - 1 : 0, static_cast<uint32_t>((top + height_ - 1) / kPitch));
  vs_->scrollTo(off, *this);
  if (n == 0) drawEmpty();
}

// A move's new lines, before its commit: every item they come from, drawn now.
void ListView::prepare(const VScrollMap::Span* spans, int n) {
  if (n <= 0) return;
  const auto items = static_cast<int32_t>(layout_.itemCount());
  const int32_t first = spans[0].contentY;
  const int32_t end = spans[n - 1].contentY + spans[n - 1].h;
  const int32_t i0 = std::max<int32_t>(0, first / kPitch);
  const int32_t i1 = std::min<int32_t>(items - 1, (end - 1) / kPitch);
  if (i1 < i0) return;
  keepFirst_ = static_cast<uint32_t>(i0);
  keepLast_ = static_cast<uint32_t>(i1);
  for (int32_t i = i0; i <= i1; ++i) renderedSlot(static_cast<uint32_t>(i));
}

// Content lines [contentY, + h) into GRAM lines [gramY, + h), item by item.
// Committing (a move): whole runs, no yields; otherwise (a full redraw in
// place) in 14-line slices, each its own bus hold, with yields.
void ListView::push(const VScrollMap::Span& span, bool committing) {
  auto& d = M5.Display;
  const auto items = static_cast<int32_t>(layout_.itemCount());
  const int w = pushWidth();
  int32_t c = span.contentY;
  const int32_t end = span.contentY + span.h;
  int g = span.gramY;
  constexpr int kSlice = 14;
  while (c < end) {
    const int32_t item = c >= 0 ? c / kPitch : -1;
    const int32_t itemTop = item * kPitch;
    const int32_t stop = std::min<int32_t>(end, c >= 0 ? itemTop + kPitch : 0);
    const int lines = static_cast<int>(stop - c);
    const int step = committing ? lines : kSlice;
    if (item < 0 || item >= items) {
      LcdLock lock(&gfx::holds());
      d.fillRect(0, g, w, lines, col::BG);
    } else {
      Slot* slot = renderedSlot(static_cast<uint32_t>(item));
      const int spriteY = g - static_cast<int>(c - itemTop);
      for (int k = 0; k < lines; k += step) {
        const int h = std::min(step, lines - k);
        {
          LcdLock lock(&gfx::holds());
          d.setClipRect(0, g + k, w, h);
          slot->sprite.pushSprite(&d, 0, spriteY);
          d.clearClipRect();
        }
        if (!committing) taskYIELD();
      }
    }
    g += lines;
    c = stop;
  }
}

void ListView::prepareFixed(int32_t off) {
  railDue_ = edge_ != Edge::None && railWanted_ && rail_;
  if (railDue_) drawRailSprite(off);
}

// Under a scrub the rail stays up: it goes back to its place after every
// move, in the move's bus hold (~2 ms).
void ListView::pushFixed() {
  if (!railDue_) return;
  railDue_ = false;
  vs_->pushAtScreen(*rail_, railX(), kTop, &gfx::holds(), height_);
  railUp_ = true;
}

// Redraws an item where the panel shows it now (a press, a toggle), without
// moving anything: its lines go to the GRAM lines they live at.
void ListView::pushItemInPlace(uint32_t item) {
  if (!vs_->active() || !vs_->map().valid() || drawnOffset_ < 0 || force_) return;
  const int32_t off = vs_->map().offset();
  const int32_t top = static_cast<int32_t>(item) * kPitch;
  int32_t c = std::max(top, off);
  const int32_t end = std::min(top + kPitch, off + height_);
  if (c >= end) return;
  Slot* slot = renderedSlot(item);
  auto& d = M5.Display;
  const int w = pushWidth();
  const int bandEnd = vs_->map().top() + vs_->map().height();
  while (c < end) {
    const int g = vs_->map().gramLineForContent(c);
    const int n = std::min<int32_t>(end - c, bandEnd - g);  // up to the wrap
    {
      LcdLock lock(&gfx::holds());
      d.setClipRect(0, g, w, n);
      slot->sprite.pushSprite(&d, 0, g - static_cast<int>(c - top));
      d.clearClipRect();
    }
    c += n;
  }
}

void ListView::drawEmpty() {
  EmptyState e;
  if (src_ && src_->emptyState(e)) {
    drawEmptyState(e, kTop, height_, src_->accent(), emptyPressed_);
    return;
  }
  M5Canvas& s = gfx::strip();
  s.fillSprite(col::BG);
  Fonts::instance().draw(s, Font::Body, src_ ? src_->emptyText() : "", kW / 2, 24, kW - 24, col::DIM, col::BG,
                         Fonts::Align::Centre);
  gfx::push(s, 0, kTop + 40, kW, gfx::kStripH);
}

// ---- the rail / scrollbar ----

void ListView::drawRailSprite(int32_t off) {
  M5Canvas& s = *rail_;
  s.fillSprite(col::BG);
  const int32_t maxOff = layout_.maxOffset(height_);
  if (edge_ == Edge::Bar) {
    // A thin scrollbar, its thumb as long as the share of the list on screen.
    const int track = height_ - 8;
    const int th = std::max(16, static_cast<int>(static_cast<int64_t>(track) * height_ / layout_.contentPx()));
    const int y = 4 + ListLayout::thumbTop(off, maxOff, track, th);
    s.fillRoundRect(2, y, 3, th, 1, col::FAINT);
    return;
  }
  const int y = 2 + ListLayout::thumbTop(off, maxOff, height_ - 4, kThumbH);
  const int32_t item = std::max<int32_t>(0, off / kPitch);
  const ListLayout::Item it = layout_.item(static_cast<uint32_t>(item));
  const char key[2] = {it.kind == ListLayout::Kind::Row || it.kind == ListLayout::Kind::InlineBar
                           ? src_->railKey(static_cast<uint32_t>(it.row))
                           : src_->railKey(0),
                       0};
  s.drawFastVLine(22, 4, height_ - 8, col::DIV);
  const uint16_t thumb = touchOn_ == TouchOn::Rail ? src_->accent() : col::BTN_HI;
  s.fillRoundRect(11, y, 22, kThumbH, 5, thumb);
  Fonts::instance().draw(s, Font::Bold, key, 22, y + kThumbH / 2, 20, touchOn_ == TouchOn::Rail ? col::DARK : col::TXT,
                         thumb, Fonts::Align::Centre);
}

void ListView::hideRail() {
  gfx::fill(railX(), kTop, kW - railX(), height_, col::BG);
  railUp_ = false;
}

void ListView::showRail(int32_t off) {
  if (!rail_ || !vs_->active()) return;
  drawRailSprite(off);
  gfx::push(*rail_, railX(), kTop, kW - railX(), height_);
  railUp_ = true;
}

void ListView::scrub(int y) {
  const int32_t o = ListLayout::scrubOffset(y - kTop, layout_.maxOffset(height_), height_, kThumbH);
  scroll_.jumpTo(static_cast<float>(o));
  jumped_ = true;
  const ListLayout::Item it = layout_.item(static_cast<uint32_t>(o / kPitch));
  const int key = it.row >= 0 ? src_->railKey(static_cast<uint32_t>(it.row)) : 0;
  if (key != railKeyShown_) {
    if (railKeyShown_ >= 0) input_->railTick();  // a tick at each new letter
    railKeyShown_ = key;
  }
}

// ---- frames ----

bool ListView::moving() const {
  const KineticScroll::Phase p = scroll_.phase();
  return p == KineticScroll::Phase::Flinging || p == KineticScroll::Phase::Snapping ||
         (p == KineticScroll::Phase::Dragging && dragged_);
}

bool ListView::animating() const {
  return src_ && (scroll_.moving() || force_ || offset() != drawnOffset_);
}

bool ListView::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  if (!src_ || !slots_) return false;
  scroll_.update(nowMs);
  int32_t off = offset();
  // A step of at most kMaxStep lines a frame (one bus hold of new lines):
  // a finger faster than that is followed a frame or two late instead of
  // costing a full redraw. Jumps and redraws in place go straight there.
  // In whole rows (the governor, when the audio is short of time): the
  // rounding is part of the step, never on top of it.
  const bool rows = wholeRows && moving();
  if (!force_ && !jumped_ && drawnOffset_ >= 0) {
    off = rows ? ListLayout::stepTowardRows(drawnOffset_, off, kMaxStep, kPitch)
               : ListLayout::stepToward(drawnOffset_, off, kMaxStep);
  } else if (rows) {
    off = off / kPitch * kPitch;
  }
  // The rail sits in the scrolled band: hidden while the list moves, back
  // once it settles, or at once under a finger.
  // (Still catching up with the finger, a step a frame, is moving too.)
  const bool catchingUp = !force_ && !jumped_ && drawnOffset_ >= 0 && off != offset();
  railWanted_ = edge_ != Edge::None && (touchOn_ == TouchOn::Rail || (!moving() && !catchingUp));
  if (!railWanted_ && railUp_) hideRail();
  const bool owed = off != drawnOffset_ || force_;
  if (owed && frameDue) {
    cost_ = FrameCost{};
    cost_.from = drawnOffset_;
    cost_.to = off;
    if (drawnOffset_ >= 0 && off != drawnOffset_) lastDir_ = off > drawnOffset_ ? 1 : -1;
    render(off);
    drawnOffset_ = off;
    force_ = false;
    jumped_ = false;
    ++frames_;
    if (!moving() && ref_) saveRef();
    return true;
  }
  if (!owed && railWanted_ && !railUp_) showRail(drawnOffset_);
  return false;
}

bool ListView::renderAhead() {
  if (!src_ || !slots_ || drawnOffset_ < 0 || force_ || jumped_ || lastDir_ == 0 || !moving()) return false;
  const auto n = static_cast<int32_t>(layout_.itemCount());
  if (n == 0) return false;
  const int32_t first = std::max<int32_t>(0, drawnOffset_ / kPitch);
  const int32_t last = std::min<int32_t>(n - 1, (drawnOffset_ + height_ - 1) / kPitch);
  const int32_t next = lastDir_ > 0 ? last + 1 : first - 1;
  if (next < 0 || next >= n) return false;
  for (int i = 0; i < kSlots; ++i) {
    if (slots_[i].item == next) return false;  // ready
  }
  // The items on screen keep their slots (the next move pushes the rest of
  // the edge ones); the spare one takes this.
  keepFirst_ = static_cast<uint32_t>(first);
  keepLast_ = static_cast<uint32_t>(last);
  Slot* s = slotFor(static_cast<uint32_t>(next));
  renderItem(s->sprite, static_cast<uint32_t>(next));
  s->item = next;
  ++aheadRenders_;
  return true;
}

// ---- touch ----

void ListView::setPressed(int32_t item, int button) {
  const int32_t old = pressedItem_;
  const int oldButton = pressedButton_;
  pressedItem_ = item;
  pressedButton_ = button;
  auto redraw = [&](int32_t it) {
    if (it < 0) return;
    for (int i = 0; i < kSlots; ++i) {
      if (slots_[i].item == it) slots_[i].item = -1;
    }
    pushItemInPlace(static_cast<uint32_t>(it));
  };
  if (old != item || oldButton != button) {
    redraw(old);
    if (item != old) redraw(item);
  }
}

void ListView::tapItem(int32_t item, const InputEvent& e) {
  if (item < 0) return;
  const ListLayout::Item it = layout_.item(static_cast<uint32_t>(item));
  switch (it.kind) {
    case ListLayout::Kind::TopBar:
    case ListLayout::Kind::InlineBar: {
      const char* labels[3] = {"", "", ""};
      const int n = src_->actions(it.row, labels);
      // The last button's hit area reaches the screen's edge.
      int b = buttonAt(e.x, n);
      if (e.atRightEdge()) b = n - 1;
      if (b >= 0) {
        input_->tapTick();
        src_->onAction(it.row, b);
      }
      break;
    }
    case ListLayout::Kind::Row: {
      const auto row = static_cast<uint32_t>(it.row);
      input_->tapTick();
      if (src_->selecting()) {
        src_->onTap(row);
        refreshRow(row);
        break;
      }
      if (src_->onTapAt(row, e.x, e.atRightEdge()) == Tap::Expand) {
        setExpanded(layout_.expanded() == it.row ? -1 : it.row);
      }
      break;
    }
    default:
      break;
  }
}

void ListView::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!src_) return;
  if (layout_.rows() == 0 && layout_.itemCount() == 0) {
    // The empty state's buttons.
    EmptyState es;
    if (!src_->emptyState(es)) return;
    const int b = emptyStateButtonAt(es, kTop, height_, e);
    if (e.type == T::Down) {
      emptyPressed_ = b;
      if (b >= 0) drawEmpty();
    } else if (e.type == T::Tap) {
      const int was = emptyPressed_;
      emptyPressed_ = -1;
      if (was >= 0) {
        drawEmpty();
        input_->tapTick();
        src_->onEmptyAction(was);
      }
    } else if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
      if (emptyPressed_ >= 0) {
        emptyPressed_ = -1;
        drawEmpty();
      }
    }
    return;
  }
  if (e.type == T::DragStart && e.fromStrip) {
    // A swipe up from the button strip (no Down): a drag of the list from
    // here, as if it had landed at this point, so the list moves with the
    // finger from now on (no jump). Nothing pressed, not even at the rail;
    // it stops a moving list like a finger landing.
    touchOn_ = TouchOn::List;
    dragged_ = true;
    downItem_ = -1;
    if (pressedItem_ >= 0) setPressed(-1, -1);
    scroll_.press(e.ms, e.y);
    return;
  }
  if (e.type == T::Down) {
    if (edge_ == Edge::Rail && e.inRightEdgeZone(kEdgeHitX)) {
      // The rail comes up at once (railWanted_), the thumb in the accent.
      // A drag scrubs; a tap opens the jump grid (the list doesn't move
      // under it). A finger resting on it to read the letter is no hold:
      // it still scrubs when it slides.
      touchOn_ = TouchOn::Rail;
      railKeyShown_ = -1;
      railUp_ = false;
      input_->noHold();
      if (scroll_.moving()) {  // it stops the list, on a row
        scroll_.press(e.ms, e.y);
        scroll_.release(e.ms, 0.0f);
      }
      return;
    }
    // A press that stops a moving list is not a tap on a row. (The offset
    // can't tell: in the pass after a frame it equals the drawn one while
    // the fling goes on.)
    const bool wasMoving = scroll_.moving();
    touchOn_ = TouchOn::List;
    dragged_ = false;
    scroll_.press(e.ms, e.y);
    // Highlight what's under the finger (only once the list is still).
    downItem_ = layout_.itemAt(offset() + e.y - kTop);
    pressedX_ = e.atRightEdge() ? kW - 1 : e.x;
    if (downItem_ >= 0 && !wasMoving && drawnOffset_ == offset()) {
      const ListLayout::Item it = layout_.item(static_cast<uint32_t>(downItem_));
      int button = -1;
      if (it.kind != ListLayout::Kind::Row) {
        const char* labels[3] = {"", "", ""};
        button = e.atRightEdge() ? src_->actions(it.row, labels) - 1 : buttonAt(e.x, src_->actions(it.row, labels));
      }
      setPressed(downItem_, button);
    }
    return;
  }
  const bool ends = e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel;
  if (touchOn_ == TouchOn::Rail) {
    if (e.type == T::DragStart || e.type == T::DragMove) scrub(e.y);
    if (e.type == T::Tap && railHost_) {
      input_->tapTick();
      touchOn_ = TouchOn::None;
      railUp_ = false;
      railHost_->onRailTap();  // the jump grid, over the band
      return;
    }
    if (ends) {
      touchOn_ = TouchOn::None;
      railUp_ = false;  // redrawn in the resting colours
    }
    return;
  }
  if (touchOn_ != TouchOn::List) return;
  switch (e.type) {
    case T::DragStart:
    case T::DragMove:
      dragged_ = true;
      if (pressedItem_ >= 0) setPressed(-1, -1);
      scroll_.drag(e.ms, e.y);
      break;
    case T::DragEnd:
      scroll_.release(e.ms, -e.vy);  // the input layer's velocity, capped at 2,000 px/s
      break;
    case T::Tap: {
      scroll_.release(e.ms, 0.0f);
      const int32_t item = pressedItem_;
      setPressed(-1, -1);
      tapItem(item, e);
      break;
    }
    case T::LongPress: {
      // A row with a hold: the double tick, then its action. Anything else
      // stays pressed: the Ui makes the lift a tap (a slow one).
      const int32_t item = pressedItem_;
      if (item < 0) break;
      const ListLayout::Item it = layout_.item(static_cast<uint32_t>(item));
      if (it.kind != ListLayout::Kind::Row || !src_->holds(static_cast<uint32_t>(it.row))) break;
      setPressed(-1, -1);  // before the action: it may open a sheet over the band
      input_->holdTick();
      src_->onHold(static_cast<uint32_t>(it.row));
      break;
    }
    case T::Release:
    case T::Cancel:
      setPressed(-1, -1);
      scroll_.release(e.ms, 0.0f);
      break;
    default:
      break;
  }
  if (ends) touchOn_ = TouchOn::None;
}

// ---- the standard row pieces ----

int ListView::number(Row& r, uint32_t n, uint16_t colour) {
  char t[8];
  snprintf(t, sizeof(t), "%02lu", static_cast<unsigned long>(n));
  Fonts::instance().draw(r.c, Font::Small, t, r.x + 30, 21, 30, colour, r.bg, Fonts::Align::Right);
  return r.x + 40;
}

int ListView::playing(Row& r, uint16_t colour) {
  // A still EQ glyph (it doesn't animate in lists: that would cost frames).
  const int h[4] = {8, 14, 10, 16};
  for (int i = 0; i < 4; ++i) r.c.fillRect(r.x + 10 + i * 5, 29 - h[i], 3, h[i], colour);
  return r.x + 40;
}

int ListView::disc(Row& r, char initial, uint16_t colour) {
  r.c.fillCircle(r.x + 25, 21, 15, colour);
  const char t[2] = {initial, 0};
  Fonts::instance().draw(r.c, Font::Bold, t, r.x + 25, 21, 28, col::DARK, colour, Fonts::Align::Centre);
  return r.x + 50;
}

int ListView::thumb(Row& r, const uint16_t* pixels) {
  if (pixels) {
    r.c.pushImage(r.x + 6, 1, 40, 40, reinterpret_cast<const lgfx::swap565_t*>(pixels));
  } else {
    r.c.fillRoundRect(r.x + 6, 1, 40, 40, 4, col::BTN);
    icons::drawCentred(r.c, icons::kNote, r.x + 26, 21, col::FAINT);
  }
  return r.x + 54;
}

int ListView::icon(Row& r, const icons::Icon& icon, uint16_t colour) {
  icons::drawCentred(r.c, icon, r.x + 22, 21, colour);
  return r.x + 44;
}

int ListView::badge(Row& r, const char* text) {
  Fonts& f = Fonts::instance();
  const int w = f.width(Font::Small, text) + 10;
  const int x = r.right - 8 - w;
  r.c.drawRoundRect(x, 12, w, 18, 5, col::FAINT);
  f.draw(r.c, Font::Small, text, x + w / 2, 21, w - 4, col::DIM, r.bg, Fonts::Align::Centre);
  return x - 8;
}

int ListView::chevron(Row& r) {
  icons::drawCentred(r.c, icons::kChevronRight, r.right - 14, 21, col::FAINT);
  return r.right - 26;
}

void ListView::lines(Row& r, int x, int right, const char* title, size_t titleLen, const char* sub, size_t subLen,
                     uint16_t titleColour, Font titleFont) {
  Fonts& f = Fonts::instance();
  const int w = right - x;
  if (!sub || subLen == 0) {
    f.draw(r.c, titleFont, title, titleLen, x, 21, w, titleColour, r.bg);
    return;
  }
  f.draw(r.c, titleFont, title, titleLen, x, 13, w, titleColour, r.bg);
  f.draw(r.c, Font::Small, sub, subLen, x, 31, w, col::DIM, r.bg);
}

}  // namespace ui
