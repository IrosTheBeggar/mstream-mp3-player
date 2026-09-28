#include "ui/Gfx.h"

#include <M5Unified.h>

#include <algorithm>

#include "app/Psram.h"
#include "ui/ListScroller.h"
#include "ui/Theme.h"

namespace ui {
namespace gfx {

namespace {

M5Canvas* strip_ = nullptr;  // PSRAM
int coverY0_ = 0, coverY1_ = 0;
SpiHoldStats holds_;
bool dark_ = false;

// Screen rows [y, y + h) as runs whose GRAM rows are consecutive (and not
// covered, unless `over`), at most kBand long: fn(screenY, gramY, n).
template <typename Fn>
void forRuns(int y, int h, bool over, Fn fn) {
  const int end = std::min(y + h, kH);
  y = std::max(y, 0);
  while (y < end) {
    if (!over && covered(y)) {
      ++y;
      continue;
    }
    const int g = ListScroller::gramLineForScreen(y);
    int n = 1;
    while (y + n < end && n < kBand && (over || !covered(y + n)) && ListScroller::gramLineForScreen(y + n) == g + n) ++n;
    fn(y, g, n);
    y += n;
  }
}

}  // namespace

bool begin() {
  if (strip_) return true;
  strip_ = psramNew<M5Canvas>();
  if (!strip_) return false;
  strip_->setPsram(true);  // before createSprite(): otherwise internal RAM
  strip_->setColorDepth(16);
  if (!strip_->createSprite(kW, kStripH)) {
    psramDelete(strip_);
    strip_ = nullptr;
    return false;
  }
  return true;
}

M5Canvas& strip() { return *strip_; }

void setDark(bool on) { dark_ = on; }
bool dark() { return dark_; }

void setCover(int y0, int y1) {
  coverY0_ = y0;
  coverY1_ = y1;
}

bool covered(int y) { return y >= coverY0_ && y < coverY1_; }

void fill(int x, int y, int w, int h, uint16_t colour, bool over) {
  if (w <= 0 || h <= 0 || dark_) return;
  auto& d = M5.Display;
  forRuns(y, h, over, [&](int, int g, int n) {
    LcdLock lock(&holds_);
    d.fillRect(x, g, w, n, colour);
  });
}

void pushRows(M5Canvas& s, int x, int y, int w, int sy0, int sy1, bool over) {
  if (w <= 0 || sy1 <= sy0 || dark_) return;
  auto& d = M5.Display;
  forRuns(y + sy0, sy1 - sy0, over, [&](int sy, int g, int n) {
    LcdLock lock(&holds_);
    d.setClipRect(x, g, w, n);
    s.pushSprite(&d, x, g - (sy - y));  // the sprite's row (sy - y) lands on GRAM row g
    d.clearClipRect();
  });
}

void push(M5Canvas& s, int x, int y, int w, int h, bool over) { pushRows(s, x, y, w, 0, h, over); }

SpiHoldStats& holds() { return holds_; }

}  // namespace gfx
}  // namespace ui
