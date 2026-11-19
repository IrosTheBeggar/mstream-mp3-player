// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/ListView.h"

#include <M5Unified.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include <esp_timer.h>

#include "UiText.h"
#include "app/Psram.h"
#include "ui/Gfx.h"
#include "ui/HotMem.h"
#include "ui/Icons.h"
#include "ui/PanelScan.h"
#include "ui/Theme.h"

namespace ui {

namespace {
constexpr int kPitch = ListLayout::kPitch;
constexpr int kRailW = 36;   // the rail's column, x 284-319 (its touch zone starts at kEdgeHitX)
constexpr int kThumbH = 30;
constexpr size_t kRowBytes = static_cast<size_t>(kW) * kRowH * 2;
constexpr size_t kRailBytes = static_cast<size_t>(kRailW) * ListView::kHeight * 2;
constexpr size_t line32(size_t n) { return (n + 31) & ~static_cast<size_t>(31); }

// ---- frames on the panel's scan (lib/core ScanSync) ----
// A frame under a finger is taken up this long before its window opens
// (the loop's passes come a millisecond or two apart): it sleeps to the
// window, then holds the bus.
constexpr int32_t kNearUs = 1500;
// ... and the sleep ends this long before the window: the hold takes the
// bus and waits out the rest there.
constexpr int32_t kSpinUs = 250;
// A hold begun this long after its window closed still counts as in it
// (the window ends 8 scan lines before the band: half of that).
constexpr int32_t kLateOkUs = 230;
// A hold's time besides its lines (the window's commands, the clip).
constexpr uint32_t kHoldExtraUs = 60;
// A second row is rendered ahead from this many lines a frame.
constexpr int kAheadTwoStep = 10;
// A list moving by itself sleeps up to this long for its frame's window
// (a finger's frames poll instead: its latest place counts).
constexpr int32_t kSleepMaxUs = 13000;
// The loop task's priority for a frame's last wait and its hold: above the
// audio decoder (2, on this core too), level with the speaker's pump.
constexpr UBaseType_t kHoldPriority = 3;

// The loop task above the decoder while a frame waits out the last of its
// window and holds the bus (a millisecond or two, then 4 ms at a fling's
// top speed, 12 at the most): an MP3's decoder runs some 10 ms at a time,
// and a pass of it across a window made every other frame miss its
// refresh, one across a hold sent the address late (a tear). It has 1.4 s
// of audio buffered and every moment between the holds; rendering stays
// below it.
class HoldBoost {
public:
  HoldBoost() : was_(uxTaskPriorityGet(nullptr)) {
    if (was_ < kHoldPriority) vTaskPrioritySet(nullptr, kHoldPriority);
  }
  ~HoldBoost() {
    if (was_ < kHoldPriority) vTaskPrioritySet(nullptr, was_);
  }
  HoldBoost(const HoldBoost&) = delete;
  HoldBoost& operator=(const HoldBoost&) = delete;

private:
  UBaseType_t was_;
};

// The calling task asleep until `atUs` (esp_timer_get_time()), to a tenth
// of a millisecond: a one-shot timer wakes it (a tick's sleep would end up
// to a millisecond early, every frame, and the wait for the window is
// spent above the decoder). Made at the first use (a semaphore and a
// timer: ~150 B of internal RAM); without them, a tick's sleep that ends
// early.
void sleepUntil(int64_t atUs) {
  static SemaphoreHandle_t woke = nullptr;
  static esp_timer_handle_t timer = nullptr;
  static bool made = false;
  if (!made) {
    made = true;
    woke = xSemaphoreCreateBinary();
    if (woke) {
      esp_timer_create_args_t args = {};
      args.callback = [](void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); };
      args.arg = woke;
      args.dispatch_method = ESP_TIMER_TASK;
      args.name = "list frame";
      if (esp_timer_create(&args, &timer) != ESP_OK) timer = nullptr;
    }
  }
  const int64_t wait = atUs - esp_timer_get_time();
  if (wait < 150) return;
  if (!timer) {
    if (wait >= 1000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    return;
  }
  xSemaphoreTake(woke, 0);  // (one given after a wait that timed out)
  if (esp_timer_start_once(timer, static_cast<uint64_t>(wait)) != ESP_OK) return;
  if (xSemaphoreTake(woke, pdMS_TO_TICKS(wait / 1000 + 4)) != pdTRUE) esp_timer_stop(timer);
}
}  // namespace

size_t ListView::hotBytes() {
  return line32(kSlots * sizeof(Slot)) + kSlots * line32(kRowBytes) + line32(sizeof(M5Canvas)) + line32(kRailBytes);
}

bool ListView::begin(ListScroller& scroller, Input& input) {
  vs_ = &scroller;
  input_ = &input;
  // The sprites and their pixels from the block reserved at boot (the fast
  // half of the PSRAM: ui/HotMem), else their own, wherever the heap has room.
  if (!slots_) {
    void* mem = hotmem::take(kSlots * sizeof(Slot));
    slotsHot_ = mem != nullptr;
    if (!mem) mem = psramAlloc(kSlots * sizeof(Slot));
    if (!mem) return false;
    slots_ = static_cast<Slot*>(mem);
    for (int i = 0; i < kSlots; ++i) new (&slots_[i]) Slot();
    for (int i = 0; i < kSlots; ++i) {
      M5Canvas& s = slots_[i].sprite;
      if (void* px = hotmem::take(kRowBytes)) {
        s.setBuffer(px, kW, kRowH, 16);
        continue;
      }
      s.setPsram(true);  // before createSprite(): otherwise internal RAM
      s.setColorDepth(16);
      if (!s.createSprite(kW, kRowH)) {
        dropSlots();
        return false;
      }
    }
  }
  if (!rail_) {
    void* mem = hotmem::take(sizeof(M5Canvas));
    railHot_ = mem != nullptr;
    rail_ = mem ? new (mem) M5Canvas() : psramNew<M5Canvas>();
    if (!rail_) return false;
    if (void* px = hotmem::take(kRailBytes)) {
      rail_->setBuffer(px, kRailW, kHeight, 16);
    } else {
      rail_->setPsram(true);
      rail_->setColorDepth(16);
      if (!rail_->createSprite(kRailW, kHeight)) {  // the tallest band
        if (railHot_) {
          rail_->~M5Canvas();
        } else {
          psramDelete(rail_);
        }
        rail_ = nullptr;
        return false;
      }
    }
  }
  if (scan_) scan_->setBand(kTop, height_);
  // A scrub's bands follow one another: each is drawn a row a loop pass
  // (bandStep()), so none holds the loop, and the next may start when the
  // last is done. (uir0's bands, drawn in one go, come as the finger moves.)
  ScrubPacer::Config pc = pacer_.config();
  pc.cadenceMs = 0;
  pacer_.setConfig(pc);
  return true;
}

void ListView::setScan(PanelScan* scan) {
  scan_ = scan;
  if (scan_) scan_->setBand(kTop, height_);
}

void ListView::setHeight(int h) {
  h = std::max(kPitch, std::min(kHeight, h));
  if (h == height_) return;
  height_ = h;
  if (scan_) scan_->setBand(kTop, height_);
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
  if (!slotsHot_) psramFree(slots_);  // (the reserved block's is never given back)
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
  band_.active = false;  // (a band a row a pass: the frame draws it whole)
  railUp_ = false;  // the frame redraws the band; the rail with it if it's wanted
  railOffset_ = -1;
}

void ListView::redraw() {
  force_ = true;
  band_.active = false;
  railUp_ = false;
  railOffset_ = -1;
}

bool ListView::pushOnRow(uint32_t row, M5Canvas& c, int sx, int sy, int w, int h, int x, int y) {
  if (!src_ || !slots_ || row >= layout_.rows() || !vs_->active() || !vs_->map().valid() || drawnOffset_ < 0 ||
      force_ || gfx::dark()) {
    return false;
  }
  const int32_t off = vs_->map().offset();
  const int32_t top = static_cast<int32_t>(layout_.itemOfRow(row)) * kPitch + y;  // the piece's first content line
  const int32_t c0 = std::max(top, off);
  const int32_t c1 = std::min(top + h, off + height_);
  if (c0 >= c1) return false;  // off screen
  vs_->pushRectAtScreen(c, sx, sy + static_cast<int>(c0 - top), w, static_cast<int>(c1 - c0), x,
                        kTop + static_cast<int>(c0 - off), &gfx::holds());
  return true;
}

int32_t ListView::pressedRow() const {
  if (pressedItem_ < 0) return -1;
  const ListLayout::Item it = layout_.item(static_cast<uint32_t>(pressedItem_));
  return it.kind == ListLayout::Kind::Row ? it.row : -1;
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
    const uint32_t us = micros() - t0;
    ++cost_.renders;
    cost_.renderUs += us;
    noteRowTime(us);
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
    // Centred, in Small when its font doesn't fit ("Play all 19410" beside
    // the rail fits in Bold: uitext::kListBarPad).
    const int room = bw - uitext::kListBarPad;
    f.draw(c, f.fitting(primary ? Font::Bold : Font::Body, labels[i], room), labels[i], x + bw / 2, 21, room,
           primary ? col::DARK : col::TXT, fill, Fonts::Align::Centre);
  }
}

void ListView::renderItem(M5Canvas& c, uint32_t item) {
  const ListLayout::Item it = layout_.item(item);
  switch (it.kind) {
    case ListLayout::Kind::TopBar:
      if (topRow(it)) {
        // The source's row in the bar's place (MOOD-FILTERS.md 7.6): a
        // row's press and divider, no checkbox, never expanded.
        const bool pressed = pressedItem_ == static_cast<int32_t>(item);
        const uint16_t bg = pressed ? col::ROW_SEL : col::BG;
        c.fillSprite(bg);
        Row r{c, 0, 0, layoutWidth(), bg, pressed, false, false, pressed ? pressedX_ : -1};
        src_->drawTopRow(r);
        c.drawFastHLine(8, kRowH - 1, layoutWidth() - 8, col::ROW_DIV);
        return;
      }
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

// `gate`: when a move's hold starts (ListScroller::Gate). Returns the
// lines pushed.
int ListView::render(int32_t off, ListScroller::Gate* gate) {
  if (force_) vs_->invalidate();
  const uint32_t n = layout_.itemCount();
  const int32_t top = std::max<int32_t>(off, 0);
  keepFirst_ = static_cast<uint32_t>(top / kPitch);
  keepLast_ = std::min<uint32_t>(n ? n - 1 : 0, static_cast<uint32_t>((top + height_ - 1) / kPitch));
  const int lines = vs_->scrollTo(off, *this, nullptr, gate);
  if (n == 0 || emptyUnderTopRow()) drawEmpty();
  return lines;
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
  if (!railDue_) return;
  railY_ = drawRailSprite(off);
  railOffset_ = off;  // (pushFixed() follows in the same frame)
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
  // (The screen off: nothing drawn; the band is redrawn whole on the wake.)
  if (!vs_->active() || !vs_->map().valid() || drawnOffset_ < 0 || force_ || gfx::dark()) return;
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
  // Under a top row (the Albums list's filters, nothing fits: MOOD-FILTERS.md
  // 7.6), the line alone: the strip from the row's foot (y 114), the text
  // at y 150. Its buttons couldn't be pressed there (onEvent()).
  const bool under = emptyUnderTopRow();
  EmptyState e;
  if (!under && src_ && src_->emptyState(e)) {
    drawEmptyState(e, kTop, height_, src_->accent(), emptyPressed_);
    return;
  }
  const int top = under ? kTop + kPitch : kTop + 40;  // the strip's
  const int lineY = under ? 36 : 24;                   // in the strip: y 150, else 136
  M5Canvas& s = gfx::strip();
  s.fillSprite(col::BG);
  Fonts::instance().draw(s, Font::Body, src_ ? src_->emptyText() : "", kW / 2, lineY, uitext::kListLineW, col::DIM,
                         col::BG, Fonts::Align::Centre);
  gfx::push(s, 0, top, kW, gfx::kStripH);
}

// ---- the rail / scrollbar ----

// Returns the A-Z thumb's top row in it (-1: the scrollbar's sprite).
int ListView::drawRailSprite(int32_t off) {
  M5Canvas& s = *rail_;
  s.fillSprite(col::BG);
  const int32_t maxOff = layout_.maxOffset(height_);
  if (edge_ == Edge::Bar) {
    // A thin scrollbar, its thumb as long as the share of the list on screen.
    const int track = height_ - 8;
    const int th = std::max(16, static_cast<int>(static_cast<int64_t>(track) * height_ / layout_.contentPx()));
    const int y = 4 + ListLayout::thumbTop(off, maxOff, track, th);
    s.fillRoundRect(2, y, 3, th, 1, col::FAINT);
    return -1;
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
  return y;
}

void ListView::hideRail() {
  gfx::fill(railX(), kTop, kW - railX(), height_, col::BG);
  railUp_ = false;
  railOffset_ = -1;
  railY_ = -1;
}

void ListView::showRail(int32_t off) {
  if (!rail_ || !vs_->active()) return;
  railY_ = drawRailSprite(off);
  gfx::push(*rail_, railX(), kTop, kW - railX(), height_);
  railUp_ = true;
  railOffset_ = off;
}

// A scrub's frame between two band draws: the thumb and its letter where
// the finger is now, over the band as it stands (through the scroll
// mapping, like pushFixed()). With the rail on the glass, only the rows
// the thumb left and the rows it takes (the rail's other rows are the same
// pixels: the whole column was 12 KB on the bus, 2.4-2.9 ms; a thumb moved
// a few rows is a tenth of that).
void ListView::pushRail(int32_t off) {
  if (!rail_ || !vs_->active()) return;
  const int was = railUp_ ? railY_ : -1;
  const int y = drawRailSprite(off);
  if (was >= 0 && y >= 0) {
    const int y0 = std::max(0, std::min(was, y));
    const int y1 = std::min(height_, std::max(was, y) + kThumbH);
    if (y1 > y0) vs_->pushRectAtScreen(*rail_, 0, y0, kW - railX(), y1 - y0, railX(), kTop + y0, &gfx::holds());
  } else {
    vs_->pushAtScreen(*rail_, railX(), kTop, &gfx::holds(), height_);
  }
  railY_ = y;
  railUp_ = true;
  railOffset_ = off;
}

void ListView::scrub(uint32_t ms, int y) {
  const int32_t o = ListLayout::scrubOffset(y - kTop, layout_.maxOffset(height_), height_, kThumbH);
  if (o != offset()) pacer_.targetChanged(ms);
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
  return src_ && (scroll_.moving() || force_ || band_.active || offset() != drawnOffset_);
}

// ---- a scrub's band, a row a pass ----
//
// Every position a scrub asks for is a jump: the band's 168 lines drawn
// again in place, 35-47 ms with nothing playing and 80-100 ms with an MP3
// (MEASURED). Drawn in one go, that was the loop standing still: no touch
// read, the thumb at 10-19 frames a second, the next band 60 ms after (so
// the loop could breathe): 5-7 bands a second. So under a finger on the
// rail the band is drawn a row a loop pass instead: the row's render and
// its push (8-15 ms), then the loop's pass (the finger read, the thumb
// drawn where it is now), then the next row. The band takes as long as it
// did; the thumb never waits for it, and the next band starts when this
// one is done (ScrubPacer's cadence, 0 here).

// Whether a jump to `off` under the finger is drawn so (a jump the
// hardware scroll takes as a move is one hold already).
bool ListView::bandInRows(int32_t off) const {
  if (touchOn_ != TouchOn::Rail || !pacing_ || force_ || drawnOffset_ < 0 || gfx::dark()) return false;
  if (!vs_->active() || !vs_->map().valid()) return false;
  const int32_t d = off - vs_->map().offset();
  return d > vs_->map().maxStep() || -d > vs_->map().maxStep();
}

void ListView::startBand(int32_t off) {
  band_ = Band{};
  band_.spans = vs_->planInPlace(off, band_.span);
  if (band_.spans <= 0) return;
  band_.active = true;
  band_.from = drawnOffset_;
  band_.off = off;
  band_.line = band_.span[0].contentY;
  band_.startUs = micros();
  drawnOffset_ = off;  // (what the band is becoming)
  jumped_ = false;
  lastDir_ = 0;
  if (!railUp_ && railWanted_) showRail(off);
}

// The band's next row (and first the thumb, if the finger moved on and a
// frame is due). True: something was drawn (lastFrame() says what).
bool ListView::bandStep(bool frameDue) {
  cost_ = FrameCost{};
  cost_.kind = FrameCost::Kind::Piece;
  cost_.from = band_.from;
  cost_.to = band_.off;
  const int32_t finger = offset();
  if (frameDue && touchOn_ == TouchOn::Rail && rail_ && railWanted_ && railOffset_ != finger) {
    pushRail(finger);
    cost_.rail = true;
  }
  // The rows of the band keep their slots while it is drawn.
  const uint32_t n = layout_.itemCount();
  const int32_t top = std::max<int32_t>(band_.off, 0);
  keepFirst_ = static_cast<uint32_t>(top / kPitch);
  keepLast_ = std::min<uint32_t>(n ? n - 1 : 0, static_cast<uint32_t>((top + height_ - 1) / kPitch));
  const VScrollMap::Span& span = band_.span[band_.at];
  const int32_t end = span.contentY + span.h;
  // Up to the end of this line's item (a row), or of the span.
  const int32_t c = band_.line;
  const int32_t itemEnd = c >= 0 ? (c / kPitch + 1) * kPitch : 0;
  VScrollMap::Span piece;
  piece.contentY = c;
  piece.gramY = span.gramY + (c - span.contentY);
  piece.h = std::min(end, itemEnd) - c;
  push(piece, false);
  band_.renders += cost_.renders;
  band_.renderUs += cost_.renderUs;
  band_.line = c + piece.h;
  if (band_.line >= end) {
    if (++band_.at < band_.spans) {
      band_.line = band_.span[band_.at].contentY;
    } else {
      // Done: a band frame, its time from its first row to its last.
      band_.active = false;
      cost_.kind = FrameCost::Kind::Band;
      cost_.renders = band_.renders;
      cost_.renderUs = band_.renderUs;
      cost_.bandUs = micros() - band_.startUs;
      if (n == 0 || emptyUnderTopRow()) drawEmpty();
      pacer_.bandDrawn(millis());
      ++frames_;
      if (!moving() && ref_) saveRef();
    }
  }
  return true;
}

// The offset to draw now: where the scroll is, or a step towards it.
int32_t ListView::frameOffset(bool rows) const {
  const int32_t off = offset();
  // A step of at most kMaxStep lines a frame (one bus hold of new lines):
  // a finger faster than that is followed a frame or two late instead of
  // costing a full redraw. Jumps and redraws in place go straight there.
  // In whole rows (the governor, when the audio is short of time): the
  // rounding is part of the step, never on top of it.
  if (!force_ && !jumped_ && drawnOffset_ >= 0) {
    return rows ? ListLayout::stepTowardRows(drawnOffset_, off, kMaxStep, kPitch)
                : ListLayout::stepToward(drawnOffset_, off, kMaxStep);
  }
  return rows ? off / kPitch * kPitch : off;
}

// A frame at `off`, drawn now.
void ListView::drawFrame(int32_t off) {
  cost_ = FrameCost{};
  cost_.from = drawnOffset_;
  cost_.to = off;
  if (drawnOffset_ >= 0 && off != drawnOffset_) {
    lastDir_ = off > drawnOffset_ ? 1 : -1;
    lastStep_ = off > drawnOffset_ ? off - drawnOffset_ : drawnOffset_ - off;
  }
  // A move: where the scan was when its address went out (the report's).
  const bool move = !force_ && !jumped_ && drawnOffset_ >= 0 && off != drawnOffset_;
  const int line = move && scan_ && scan_->ready() ? scan_->line() : -1;
  const int64_t readUs = esp_timer_get_time();
  ListScroller::Gate gate;
  render(off, line >= 0 ? &gate : nullptr);
  if (line >= 0 && gate.sentUs) {
    noteAddress(line, readUs, gate.sentUs);
    ++scanStats_.unsynced;
  }
  if (touchOn_ == TouchOn::Rail) pacer_.bandDrawn(millis());
  drawnOffset_ = off;
  force_ = false;
  jumped_ = false;
  ++frames_;
  if (!moving() && ref_) saveRef();
}

bool ListView::onScan() const {
  return scan_ && scanOn_ && scan_->ready() && !railWanted_ && vs_->active() && vs_->map().valid() && !gfx::dark();
}

uint32_t ListView::holdFor(int d) const { return static_cast<uint32_t>(d < 0 ? -d : d) * lineUs_ + kHoldExtraUs; }

// A row took `us` to render: what the next one is taken to cost (a render
// the decoder cut into counts for less: it would only scare the look-ahead
// off).
void ListView::noteRowTime(uint32_t us) {
  if (us > rowUs_ * 3) us = rowUs_ * 3;
  rowUs_ = (rowUs_ * 3 + us) / 4;
}

// Where the scan was when a move's address went out: `line` read at
// `readUs`, the address at `sentUs`. Counted from the band's last row: up
// to the porches and the fixed rows above the band it is outside the band
// (the frame is whole); past that the scan was inside it (a tear).
void ListView::noteAddress(int line, int64_t readUs, int64_t sentUs) {
  const ScanSync& sync = scan_->sync();
  const ScanSync::Panel& p = sync.panel();
  const int64_t lines = (sentUs - readUs) * p.lines / p.frameUs;
  int phase = static_cast<int>((line + lines - (kTop + height_ + p.firstRow)) % p.lines);
  if (phase < 0) phase += p.lines;
  if (phase < scanStats_.phaseMin) scanStats_.phaseMin = static_cast<int16_t>(phase);
  if (phase > scanStats_.phaseMax) scanStats_.phaseMax = static_cast<int16_t>(phase);
  if (phase >= p.lines - height_) ++scanStats_.inBand;
}

// The rows a move from the drawn offset to `off` brings on, rendered now:
// before the bus, and before the move's window.
void ListView::prerender(int32_t off) {
  const auto items = static_cast<int32_t>(layout_.itemCount());
  const bool up = off > drawnOffset_;
  const int32_t a = up ? drawnOffset_ + height_ : off;  // content lines [a, b)
  const int32_t b = up ? off + height_ : drawnOffset_;
  const int32_t i0 = std::max<int32_t>(0, a / kPitch);
  const int32_t i1 = std::min<int32_t>(items - 1, (b - 1) / kPitch);
  // The rows on screen after the move keep their slots.
  const int32_t top = std::max<int32_t>(off, 0);
  keepFirst_ = static_cast<uint32_t>(top / kPitch);
  keepLast_ = std::min<uint32_t>(items ? items - 1 : 0, static_cast<uint32_t>((top + height_ - 1) / kPitch));
  for (int32_t i = i0; i <= i1; ++i) renderedSlot(static_cast<uint32_t>(i));
}

// A move drawn where the panel's scan doesn't see it half done (lib/core
// ScanSync): its rows rendered first, its hold started inside the window of
// the refresh under way. False: its window opens later (the loop comes
// back: nothing is owed to this pass), or nothing moved.
//
// A window the loop comes too late for (the audio decoder runs above it,
// some 10 ms at a time while an MP3 plays) is never waited out to the next
// refresh: the frame is drawn at once, late. Its address then goes out
// with the scan back in the band, and the rows the scan had passed show it
// a refresh later (a tear, for that one refresh, as every frame had before
// the scan was read), but the list keeps its pace. Nor does a frame wait
// for its window when the refresh before had none: it is drawn at once
// too. (MEASURED with an MP3, whose decoder's passes are 10 ms and now and
// then 21: waiting out the next window lost a refresh in three, with 37-46
// ms between two frames at the worst.)
bool ListView::scanFrame(bool rows) {
  const ScanSync& sync = scan_->sync();
  cost_ = FrameCost{};
  cost_.from = drawnOffset_;
  const int line = scan_->line();
  const int64_t readUs = esp_timer_get_time();
  if (line < 0) {  // not readable just now: a frame as without it
    drawFrame(frameOffset(rows));
    return true;
  }
  int32_t off = frameOffset(rows);
  int d = off - drawnOffset_;
  if (d == 0) return false;
  const int32_t refreshUs = static_cast<int32_t>(sync.panel().frameUs);
  // The window of the refresh under way, ahead, open or just over (never
  // the next one's: see above). But one frame a refresh: once this one has
  // its frame, the next one's window.
  auto plan = [&](int step) {
    ScanSync::Window win = sync.plan(line, step, holdFor(step), static_cast<uint32_t>(refreshUs));
    if (lastShowUs_ && readUs + win.showsUs < lastShowUs_ + refreshUs / 2) {
      win = sync.plan(line, step, holdFor(step));
      if (readUs + win.showsUs < lastShowUs_ + refreshUs / 2) {  // (its window is still open)
        win.earliestUs += refreshUs;
        win.latestUs += refreshUs;
        win.addressUs += refreshUs;
        win.showsUs += refreshUs;
      }
    }
    return win;
  };
  ScanSync::Window w = plan(d);
  // A refresh went by without a frame (the loop was held: a decoder pass
  // of 20 ms, a row rendered in the frame's way; or the list stood still):
  // this frame doesn't wait for its window on top of that, it is drawn
  // now (as a late one: the list moves again a few ms sooner).
  const bool behind = !lastShowUs_ || readUs + w.showsUs - lastShowUs_ > refreshUs + refreshUs / 2;
  const KineticScroll::Phase phase = scroll_.phase();
  const bool coasting = phase == KineticScroll::Phase::Flinging || phase == KineticScroll::Phase::Snapping;
  if (coasting) {
    // A list moving by itself is drawn where it will be when the frame
    // shows: frames a refresh apart then move it a refresh's worth,
    // whenever in the refresh the loop got to them. (One drawn now shows
    // as its address goes out.)
    const int32_t showsUs = w.late || behind ? static_cast<int32_t>(holdFor(d)) : w.showsUs;
    scroll_.update(millis() + static_cast<uint32_t>((showsUs + 500) / 1000));
    off = frameOffset(rows);
    d = off - drawnOffset_;
    if (d == 0) return false;
    w = plan(d);  // (the hold is as long as the step)
  }
  prerender(off);
  const int64_t now = esp_timer_get_time();
  const bool atOnce = w.late || behind;
  const int64_t earliest = atOnce ? now : readUs + w.earliestUs;
  const int64_t wait = earliest - now;
  // Under a finger the loop comes back for the window (its passes read
  // the finger meanwhile: the frame is drawn where it is then).
  if (wait > (coasting ? kSleepMaxUs : kNearUs)) return false;
  ListScroller::Gate gate;
  gate.earliestUs = earliest;
  gate.addressUs = atOnce ? 0 : readUs + w.addressUs;
  cost_.to = off;
  {
    // From here to the address nothing below the speaker's pump runs on
    // this core: asleep to just before the window (the timer wakes this
    // task over the decoder), then the hold.
    HoldBoost boost;
    sleepUntil(earliest - kSpinUs);
    render(off, &gate);
  }
  const int64_t began = gate.sentUs - gate.linesUs;  // (about: its lines' time back from the address)
  // (The frame's cost is its work: not the wait for its window.)
  if (began > now) cost_.waitUs = static_cast<uint32_t>(began - now);
  if (atOnce || began > readUs + w.latestUs + kLateOkUs) {
    ++scanStats_.late;
  } else {
    ++scanStats_.frames;
    if (!w.clean) ++scanStats_.early;
  }
  noteAddress(line, readUs, gate.sentUs);
  lastHoldUs_ = began;
  // The refresh this frame was for (one drawn at once is whole on the
  // glass by then too: the frame after it is the next refresh's, in its
  // window again).
  lastShowUs_ = readUs + w.showsUs;
  // What a line costs on the bus, from the hold itself (one a higher task
  // cut into doesn't count).
  const int n = d < 0 ? -d : d;
  if (n >= 8 && gate.linesUs > kHoldExtraUs) {
    const uint32_t per = (gate.linesUs - kHoldExtraUs) / static_cast<uint32_t>(n);
    if (per < lineUs_ * 2) lineUs_ = (lineUs_ * 3 + per) / 4;
  }
  lastDir_ = d > 0 ? 1 : -1;
  lastStep_ = n;
  drawnOffset_ = off;
  ++frames_;
  if (!moving() && ref_) saveRef();
  return true;
}

bool ListView::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  if (!src_ || !slots_) return false;
  scroll_.update(nowMs);
  const bool rows = wholeRows && moving();
  const int32_t off = frameOffset(rows);
  // The rail sits in the scrolled band: hidden while the list moves, back
  // once it settles, or at once under a finger.
  // (Still catching up with the finger, a step a frame, is moving too.)
  const bool stepped = !force_ && !jumped_ && drawnOffset_ >= 0;
  const bool catchingUp = stepped && off != offset();
  railWanted_ = edge_ != Edge::None && (touchOn_ == TouchOn::Rail || (!moving() && !catchingUp));
  if (!railWanted_ && railUp_) hideRail();
  // A scrub's band under way: its next row, whatever the frame clock says.
  if (band_.active) return bandStep(frameDue);
  const bool owed = off != drawnOffset_ || force_;
  if (owed && frameDue) {
    // A scrub's position is a jump (the band drawn whole): it waits for
    // the pacer's cadence, or the finger to rest, and until then the rail
    // alone follows the finger (ScrubPacer).
    if (pacing_ && touchOn_ == TouchOn::Rail && !force_ && drawnOffset_ >= 0 && pacer_.owed() &&
        !pacer_.bandDue(nowMs)) {
      if (railOffset_ == off || !rail_) return false;  // the rail is there already
      cost_ = FrameCost{};
      cost_.kind = FrameCost::Kind::Rail;
      cost_.from = cost_.to = off;
      pushRail(off);
      ++frames_;
      return true;
    }
    // A move of a list that moves: on the panel's scan when it can be read.
    if (stepped && onScan()) return scanFrame(rows);
    // A scrub's jump: the band a row a pass, the thumb between them.
    if (bandInRows(off)) {
      startBand(off);
      if (band_.active) return bandStep(false);
    }
    drawFrame(off);
    return true;
  }
  if (!owed && railWanted_ && !railUp_) showRail(drawnOffset_);
  return false;
}

// `untilDueMs`: how long the frame clock gives until the next frame (the
// Ui's; on the panel's scan the list knows better: its last hold's start,
// a refresh before the next).
bool ListView::renderAhead(uint32_t untilDueMs) {
  if (!src_ || !slots_ || drawnOffset_ < 0 || force_ || jumped_ || lastDir_ == 0 || !moving()) return false;
  const auto n = static_cast<int32_t>(layout_.itemCount());
  if (n == 0) return false;
  // Time for a row before the next frame's window (or its deadline)?
  if (onScan() && lastHoldUs_) {
    // The next hold begins about a refresh after the last one did (later,
    // as a fling slows: its window follows the step). A row must be done a
    // millisecond before; once that moment is near, or just past, the
    // frame comes first. (A hold two refreshes ago says nothing: a
    // motion's first frames take what comes.)
    const int64_t frameUs = scan_->sync().panel().frameUs;
    const int64_t since = esp_timer_get_time() - lastHoldUs_;
    if (since < 2 * frameUs && since + rowUs_ + 1000 > frameUs) return false;
  } else if (untilDueMs * 1000 < rowUs_ + 2000) {
    return false;
  }
  const int32_t first = std::max<int32_t>(0, drawnOffset_ / kPitch);
  const int32_t last = std::min<int32_t>(n - 1, (drawnOffset_ + height_ - 1) / kPitch);
  // The row past the edge the list moves toward, and from a fast step the
  // one after it (the frame that brings two rows on would render one).
  for (int k = 1; k <= (lastStep_ >= kAheadTwoStep ? 2 : 1); ++k) {
    const int32_t next = lastDir_ > 0 ? last + k : first - k;
    if (next < 0 || next >= n) return false;
    bool ready = false;
    for (int i = 0; i < kSlots; ++i) ready = ready || slots_[i].item == next;
    if (ready) continue;
    // The rows on screen keep their slots (the next move pushes the rest
    // of the edge ones), and the first row ahead; a spare one takes this.
    // With none spare, the second row ahead takes the slot of the row
    // about to leave at the other edge: its lines are on the glass, and
    // only a turn back would want them again (it is rendered again then).
    keepFirst_ = static_cast<uint32_t>(lastDir_ > 0 ? first : std::max<int32_t>(0, first - (k - 1)));
    keepLast_ = static_cast<uint32_t>(lastDir_ > 0 ? std::min<int32_t>(n - 1, last + (k - 1)) : last);
    if (k == 2) {
      bool spare = false;
      for (int i = 0; i < kSlots; ++i) {
        const int32_t r = slots_[i].item;
        spare = spare || r < 0 || r < static_cast<int32_t>(keepFirst_) || r > static_cast<int32_t>(keepLast_);
      }
      if (!spare) {
        if (lastDir_ > 0) {
          ++keepFirst_;
        } else {
          --keepLast_;
        }
      }
    }
    Slot* s = slotFor(static_cast<uint32_t>(next));
    const uint32_t t0 = micros();
    renderItem(s->sprite, static_cast<uint32_t>(next));
    noteRowTime(micros() - t0);
    s->item = next;
    ++aheadRenders_;
    return true;
  }
  return false;
}

// While the list rests: the row just above the band and the row just
// below it, one a call, into the two slots a list at rest has spare (it
// rests on a row boundary: four rows show, of six slots).
bool ListView::renderNeighbours() {
  if (!src_ || !slots_ || force_ || jumped_ || drawnOffset_ < 0 || band_.active || gfx::dark()) return false;
  if (scroll_.moving() || touchOn_ != TouchOn::None || offset() != drawnOffset_) return false;
  const auto n = static_cast<int32_t>(layout_.itemCount());
  if (n == 0) return false;
  const int32_t first = std::max<int32_t>(0, drawnOffset_ / kPitch);
  const int32_t last = std::min<int32_t>(n - 1, (drawnOffset_ + height_ - 1) / kPitch);
  keepFirst_ = static_cast<uint32_t>(std::max<int32_t>(0, first - 1));
  keepLast_ = static_cast<uint32_t>(std::min<int32_t>(n - 1, last + 1));
  for (const int32_t next : {last + 1, first - 1}) {
    if (next < 0 || next >= n) continue;
    bool ready = false, spare = false;
    for (int i = 0; i < kSlots; ++i) {
      const int32_t r = slots_[i].item;
      ready = ready || r == next;
      spare = spare || r < 0 || r < static_cast<int32_t>(keepFirst_) || r > static_cast<int32_t>(keepLast_);
    }
    if (ready || !spare) continue;
    Slot* s = slotFor(static_cast<uint32_t>(next));
    renderItem(s->sprite, static_cast<uint32_t>(next));
    s->item = next;
    ++aheadRenders_;
    return true;
  }
  return false;
}

void ListView::forgetHidden() {
  if (!src_ || !slots_ || drawnOffset_ < 0) return;
  const int32_t first = drawnOffset_ / kPitch;
  const int32_t last = (drawnOffset_ + height_ - 1) / kPitch;
  for (int i = 0; i < kSlots; ++i) {
    if (slots_[i].item < first || slots_[i].item > last) slots_[i].item = -1;
  }
}

// Console uiB: what the list's frames are made of, timed on the list shown
// (the numbers docs/UI-SPIKE.md's "Frames on the panel's scan" has; run it
// with nothing playing, and again with a track, to see what the decoder
// takes). The band is drawn again when it is done.
void ListView::bench() {
  if (!src_ || !slots_ || !vs_->active() || layout_.itemCount() == 0 || gfx::dark() || band_.active) {
    Serial.println("[ui] bench: no list on a lit screen");
    return;
  }
  const int32_t off = std::max<int32_t>(0, offset());
  const uint32_t n = layout_.itemCount();
  const uint32_t first = static_cast<uint32_t>(off / kPitch);
  uint32_t t0;
  Serial.printf("[ui] bench: row sprites at %p .. %p (PSRAM from 0x3FA00000 up shares core 1's cache with the code)\n",
                slots_[0].sprite.getBuffer(), slots_[kSlots - 1].sprite.getBuffer());
  // A row sprite's memory (26,880 B): filled cold (each sprite in turn) and
  // warm (the same one again).
  t0 = micros();
  for (int k = 0; k < 12; ++k) slots_[k % kSlots].sprite.fillSprite(col::BG);
  const uint32_t fillUs = (micros() - t0) / 12;
  t0 = micros();
  for (int k = 0; k < 12; ++k) slots_[0].sprite.fillSprite(col::BG);
  const uint32_t fillSameUs = (micros() - t0) / 12;
  // Rows: each item on screen once (cold), then the first one 8 times (warm).
  uint32_t coldUs = 0;
  int rows = 0;
  for (uint32_t i = first; i < n && rows < 5; ++i, ++rows) {
    t0 = micros();
    renderItem(slots_[rows].sprite, i);
    coldUs += micros() - t0;
  }
  t0 = micros();
  for (int k = 0; k < 8; ++k) renderItem(slots_[0].sprite, first);
  const uint32_t warmUs = (micros() - t0) / 8;
  Serial.printf("[ui] bench: a row sprite filled in %lu us (the same one again %lu); a row rendered in %lu us (%d "
                "rows, each once), %lu us (the first, again and again); text %s\n",
                (unsigned long)fillUs, (unsigned long)fillSameUs, rows ? (unsigned long)(coldUs / rows) : 0UL, rows,
                (unsigned long)warmUs, Fonts::fast() ? "straight into the sprites" : "by M5GFX (uiBf0)");
  // The band: rendered and pushed in place; then pushed alone.
  invalidate();
  cost_ = FrameCost{};
  t0 = micros();
  render(off);
  const uint32_t drawUs = micros() - t0;
  const uint32_t renders = cost_.renders, renderUs = cost_.renderUs;
  force_ = false;
  jumped_ = false;
  drawnOffset_ = off;
  redraw();
  t0 = micros();
  render(off);
  const uint32_t pushUs = micros() - t0;
  force_ = false;
  Serial.printf("[ui] bench: the band in place (%d lines, %d px wide): %lu us (%lu rows rendered in %lu us); pushed "
                "alone %lu us\n",
                height_, pushWidth(), (unsigned long)drawUs, (unsigned long)renders, (unsigned long)renderUs,
                (unsigned long)pushUs);
  // A move of one row and back, and of 7 lines (the hardware scroll's
  // path), the rows rendered first.
  if (off + kPitch <= layout_.maxOffset(height_)) {
    keepFirst_ = first;
    keepLast_ = std::min<uint32_t>(n - 1, static_cast<uint32_t>((off + kPitch + height_ - 1) / kPitch));
    for (uint32_t i = keepFirst_; i <= keepLast_; ++i) renderedSlot(i);
    uint32_t us[4];
    const int32_t to[4] = {off + kPitch, off, off + 7, off};
    for (int k = 0; k < 4; ++k) {
      t0 = micros();
      vs_->scrollTo(to[k], *this);
      us[k] = micros() - t0;
    }
    Serial.printf("[ui] bench: a move of 42 lines %lu us, back %lu us; of 7 lines %lu us, back %lu us%s\n",
                  (unsigned long)us[0], (unsigned long)us[1], (unsigned long)us[2], (unsigned long)us[3],
                  railWanted_ ? " (the rail put back after each)" : "");
  }
  invalidate();
  jumped_ = true;
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
      if (topRow(it)) {
        // The source's row (7.6): its own zones (the cross, the editor).
        input_->tapTick();
        src_->onTopRowTap(e.x, e.atRightEdge());
        break;
      }
      [[fallthrough]];
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
      if (!src_->pressable(row)) break;  // a divider: no control, no tick
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
      pacer_.begin(e.ms);
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
      if (it.kind == ListLayout::Kind::Row && !src_->pressable(static_cast<uint32_t>(it.row))) return;
      int button = -1;  // a row, or the source's top row: pressed whole
      if (it.kind != ListLayout::Kind::Row && !topRow(it)) {
        const char* labels[3] = {"", "", ""};
        button = e.atRightEdge() ? src_->actions(it.row, labels) - 1 : buttonAt(e.x, src_->actions(it.row, labels));
      }
      setPressed(downItem_, button);
    }
    return;
  }
  const bool ends = e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel;
  if (touchOn_ == TouchOn::Rail) {
    if (e.type == T::DragStart || e.type == T::DragMove) scrub(e.ms, e.y);
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
