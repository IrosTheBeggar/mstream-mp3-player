// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/ListScroller.h"

#include <M5Unified.h>
#include <esp_timer.h>

namespace {
// MIPI DCS commands (ILI9342C datasheet 8.2.26, 8.2.30, 8.2.13).
constexpr uint8_t kVscrdef = 0x33;   // TFA, VSA, BFA: 16 bits each, high byte first
constexpr uint8_t kVscrsadd = 0x37;  // VSP: 16 bits
constexpr uint8_t kNoron = 0x13;     // Normal Display Mode On: leaves scroll mode
constexpr int kPanelLines = 240;     // the controller's frame-memory lines (TFA + VSA + BFA)
}  // namespace

ListScroller* ListScroller::active_ = nullptr;

bool ListScroller::begin(int top, int height, SpiHoldStats* stats, int maxStep) {
  if (active_ == this) end();
  if (active_) {
    Serial.println("[vscroll] another list scroller is active");
    return false;
  }
  auto& d = M5.Display;
  // Only the orientation checked against the datasheet and M5GFX's setup:
  // rotation 1 on the Core2 = internal rotation 0 = MADCTL without MV/MY/ML.
  if (M5.getBoard() != m5::board_t::board_M5StackCore2 || d.getRotation() != 1 || d.width() != 320 ||
      d.height() != kPanelLines) {
    Serial.printf("[vscroll] refused: board %d, rotation %d, %ldx%ld (only the Core2 in rotation 1)\n",
                  static_cast<int>(M5.getBoard()), static_cast<int>(d.getRotation()), (long)d.width(),
                  (long)d.height());
    return false;
  }
  if (top < 0 || height < 1 || top + height > kPanelLines) return false;
  stats_ = stats;
  map_.configure(top, height, maxStep);
  {
    LcdLock lock(stats_);
    d.writeCommand(kVscrdef);
    d.writeData16(static_cast<uint16_t>(top));
    d.writeData16(static_cast<uint16_t>(height));
    d.writeData16(static_cast<uint16_t>(kPanelLines - top - height));
    d.writeCommand(kVscrsadd);
    d.writeData16(static_cast<uint16_t>(top));  // the identity: nothing moves yet
  }
  sentVsp_ = static_cast<uint16_t>(top);
  active_ = this;
  return true;
}

void ListScroller::end() {
  if (active_ != this) return;
  {
    LcdLock lock(stats_);
    auto& d = M5.Display;
    d.writeCommand(kVscrsadd);
    d.writeData16(static_cast<uint16_t>(map_.top()));
    d.writeCommand(kNoron);
  }
  sentVsp_ = 0;
  map_.invalidate();
  active_ = nullptr;
}

void ListScroller::resend() {
  if (active_ != this) return;
  LcdLock lock(stats_);
  auto& d = M5.Display;
  d.writeCommand(kVscrdef);
  d.writeData16(static_cast<uint16_t>(map_.top()));
  d.writeData16(static_cast<uint16_t>(map_.height()));
  d.writeData16(static_cast<uint16_t>(kPanelLines - map_.top() - map_.height()));
  d.writeCommand(kVscrsadd);
  d.writeData16(sentVsp_);
}

void ListScroller::sendStartAddress(uint16_t vsp) {
  if (vsp == sentVsp_) return;
  LcdLock lock(stats_);
  M5.Display.writeCommand(kVscrsadd);
  M5.Display.writeData16(vsp);
  sentVsp_ = vsp;
}

int ListScroller::scrollTo(int32_t offset, Painter& painter, uint32_t* sendUs) {
  if (sendUs) *sendUs = 0;
  if (active_ != this) return 0;
  const bool moved = !map_.valid() || map_.offset() != offset;
  VScrollMap::Span spans[VScrollMap::kMaxSpans];
  const int n = map_.plan(offset, spans);
  int lines = 0;
  for (int i = 0; i < n; ++i) lines += spans[i].h;
  if (map_.vsp() == sentVsp_) {
    // In place (a full redraw, or no move): nothing on screen shifts, so the
    // band is written where it shows, progressively, like a plain redraw.
    for (int i = 0; i < n; ++i) painter.push(spans[i], false);
    if (moved) {
      painter.prepareFixed(offset);
      painter.pushFixed();
    }
    return lines;
  }
  // A move: render first; then push the new lines (into GRAM lines that are
  // still on screen at the far edge), send the address and put back what
  // must not move, back to back in one bus hold, so the wrong strip and the
  // displaced fixed parts show for that bus time only.
  painter.prepare(spans, n);
  painter.prepareFixed(offset);
  {
    LcdLock lock(stats_);
    for (int i = 0; i < n; ++i) painter.push(spans[i], true);
    const int64_t t0 = esp_timer_get_time();
    sendStartAddress(map_.vsp());
    if (sendUs) *sendUs = static_cast<uint32_t>(esp_timer_get_time() - t0);
    painter.pushFixed();
  }
  return lines;
}

void ListScroller::pushAtScreen(M5Canvas& sprite, int x, int screenY, SpiHoldStats* stats, int lines) {
  auto& d = M5.Display;
  const int h = lines > 0 && lines < sprite.height() ? lines : sprite.height();
  const int w = sprite.width();
  int y = screenY;
  const int end = screenY + h;
  while (y < end) {
    // A run of screen lines whose GRAM lines are consecutive.
    const int g = gramLineForScreen(y);
    int run = 1;
    while (y + run < end && gramLineForScreen(y + run) == g + run) ++run;
    {
      LcdLock lock(stats ? stats : stats_);
      d.setClipRect(x, g, w, run);
      sprite.pushSprite(&d, x, g - (y - screenY));
      d.clearClipRect();
    }
    y += run;
  }
}

int ListScroller::gramLineForScreen(int y) {
  // From the address the panel really has (the map may be invalidated, or
  // mid-plan): screen line top + k shows GRAM line top + (vsp - top + k) mod height.
  if (!active_) return y;
  const int top = active_->map_.top();
  const int height = active_->map_.height();
  if (y < top || y >= top + height) return y;
  return top + (y - top + (active_->sentVsp_ - top)) % height;
}

uint16_t ListScroller::activeVsp() { return active_ ? active_->sentVsp_ : 0; }
