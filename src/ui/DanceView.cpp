#include "ui/DanceView.h"

#include <M5Unified.h>

#include <algorithm>
#include <cmath>

namespace {
// The same palette as DisplayView.
constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kFg = TFT_WHITE;
constexpr uint16_t kDim = 0x7BEF;     // grey
constexpr uint16_t kHeader = 0x001F;  // blue
constexpr uint16_t kAccent = 0x07FF;  // cyan
constexpr uint16_t kIdle = 0xAD55;    // light grey: swaying, no beat
constexpr uint16_t kGround = 0x4208;  // dark grey

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kHeaderH = 24;
constexpr int kStatusY = DanceView::kBoxY + DanceView::kBoxH + 14;  // under the box, above the labels

constexpr float kLimb = 2.5f;  // half the line width
constexpr int kFlashX = DanceView::kBoxW - 10;  // the beat dot's centre, in the box
constexpr int kFlashY = 10;
}  // namespace

bool DanceView::begin(dance::Skin skin) {
  sprite_.setPsram(true);  // before createSprite(): otherwise internal RAM
  skin_ = skin;
  return create();
}

bool DanceView::setSkin(dance::Skin skin) {
  if (skin == skin_ && ready_) return true;
  skin_ = skin;
  return create();
}

bool DanceView::create() {
  sprite_.deleteSprite();
  sprite_.deletePalette();  // or an 8-bit sprite would be a palette one
  paletteStep_ = -1;
  full_ = true;             // the next frame pushes the whole box
  last_ = Rect{};
  // Crab: RGB565, the panel's own format, so a push is a plain copy (a
  // 4-bit palette sprite, tried first, converted every pixel through the
  // palette during the push: ~4 ms more of held SPI bus per frame while an
  // MP3 played). Stick figure: RGB332.
  sprite_.setColorDepth(static_cast<uint8_t>(skin_ == dance::Skin::Crab ? 16 : 8));
  ready_ = sprite_.createSprite(kBoxW, kBoxH) != nullptr;
  if (ready_) sprite_.fillSprite(0);  // black
  return ready_;
}

void DanceView::enter() {
  full_ = true;  // the screen is cleared: the next frame pushes the whole box
  auto& d = M5.Display;
  d.fillScreen(kBg);
  title_ = "";
  status_ = "";
  d.fillRect(0, 0, kW, kHeaderH, kHeader);
  // Labels for the touch buttons under the screen, as on the now-playing screen.
  d.setFont(&fonts::Font0);
  d.setTextColor(kDim, kBg);
  d.setTextPadding(0);
  d.setTextDatum(textdatum_t::middle_center);
  d.drawString("prev / vol-", 53, kH - 8);
  d.drawString("play / output", 160, kH - 8);
  d.drawString("next / vol+", 267, kH - 8);
  d.setTextDatum(textdatum_t::middle_left);
}

void DanceView::setTitle(const String& title) {
  if (title == title_) return;
  title_ = title;
  auto& d = M5.Display;
  d.setFont(&fonts::Font2);
  d.setTextColor(kFg, kHeader);
  d.setTextDatum(textdatum_t::middle_left);
  d.setTextPadding(kW - 8);  // to the screen's edge: a shorter title clears a longer one's tail
  d.drawString(title, 8, kHeaderH / 2);
}

void DanceView::setStatus(const String& status) {
  if (status == status_) return;
  status_ = status;
  auto& d = M5.Display;
  d.setFont(&fonts::Font2);
  d.setTextColor(kFg, kBg);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextPadding(kW);
  d.drawString(status, kW / 2, kStatusY);
  d.setTextDatum(textdatum_t::middle_left);
}

DanceView::Rect DanceView::bounds(const dance::Pose& p) {
  // Everything drawn for the figure: joints (the hip joints are within the
  // hip +- 6 px, the feet's strokes 5 px out), the head, and the ground line,
  // plus the wide lines' half width and their anti-aliased fringe.
  const dance::Point pts[] = {p.neck,      p.hip,    p.shoulderL, p.elbowL, p.handL, p.shoulderR, p.elbowR,
                              p.handR,     p.kneeL,  p.footL,     p.kneeR,  p.footR};
  float x0 = p.head.x - p.headR, x1 = p.head.x + p.headR, y0 = p.head.y - p.headR, y1 = p.head.y + p.headR;
  for (const auto& q : pts) {
    x0 = std::min(x0, q.x);
    x1 = std::max(x1, q.x);
    y0 = std::min(y0, q.y);
    y1 = std::max(y1, q.y);
  }
  x0 = std::min({x0, p.hip.x - 6.0f, p.footR.x - 5.0f});
  x1 = std::max({x1, p.hip.x + 6.0f, p.footL.x + 5.0f});
  const dance::Box box;
  y1 = std::max(y1, box.groundY + 5.0f);  // the ground line under the feet
  const float m = kLimb + 2.0f;
  Rect r;
  r.x0 = std::max(0, static_cast<int>(std::floor(x0 - m)));
  r.y0 = std::max(0, static_cast<int>(std::floor(y0 - m)));
  r.x1 = std::min(kBoxW, static_cast<int>(std::ceil(x1 + m)) + 1);
  r.y1 = std::min(kBoxH, static_cast<int>(std::ceil(y1 + m)) + 1);
  return r;
}

DanceView::Rect DanceView::unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

void DanceView::beginFrame(const Rect& now, int bg) {
  dirty_ = full_ ? Rect{0, 0, kBoxW, kBoxH} : unite(now, last_);
  last_ = now;
  // Only the dirty rectangle is cleared and drawn (the rest of the sprite
  // still holds what the LCD shows).
  sprite_.setClipRect(dirty_.x0, dirty_.y0, dirty_.x1 - dirty_.x0, dirty_.y1 - dirty_.y0);
  sprite_.fillRect(dirty_.x0, dirty_.y0, dirty_.x1 - dirty_.x0, dirty_.y1 - dirty_.y0, bg);
}

void DanceView::drawDot(bool flash, int bg, int colour, bool smooth) {
  const Rect dot{kFlashX - 8, kFlashY - 8, kFlashX + 8, kFlashY + 8};
  const bool overlap = dirty_.x0 < dot.x1 && dot.x0 < dirty_.x1 && dirty_.y0 < dot.y1 && dot.y0 < dirty_.y1;
  flashDirty_ = full_ || overlap || flash != flash_;
  if (!flashDirty_) return;
  sprite_.fillRect(dot.x0, dot.y0, dot.x1 - dot.x0, dot.y1 - dot.y0, bg);
  if (flash) {
    if (smooth) {
      sprite_.fillSmoothCircle(kFlashX, kFlashY, 5, colour);
    } else {
      sprite_.fillCircle(kFlashX, kFlashY, 5, colour);  // crisp, as the crab's pixels
    }
  }
  flash_ = flash;
}

void DanceView::drawFigure(const dance::Pose& p, bool flash, bool dancing) {
  if (!ready_ || skin_ != dance::Skin::Stick) return;
  auto& s = sprite_;
  const uint16_t c = dancing ? kFg : kIdle;
  beginFrame(bounds(p), kBg);
  const dance::Box box;
  s.fillRect(4, static_cast<int>(box.groundY) + 3, kBoxW - 8, 2, kGround);
  auto line = [&](dance::Point a, dance::Point b) {
    s.drawWideLine(static_cast<int32_t>(a.x + 0.5f), static_cast<int32_t>(a.y + 0.5f),
                   static_cast<int32_t>(b.x + 0.5f), static_cast<int32_t>(b.y + 0.5f), kLimb, c);
  };
  // Legs from the hip joints either side of the hip centre (as DancePose places them).
  const dance::Point hipL{p.hip.x + 6.0f, p.hip.y}, hipR{p.hip.x - 6.0f, p.hip.y};
  line(hipL, p.kneeL);
  line(p.kneeL, p.footL);
  line(hipR, p.kneeR);
  line(p.kneeR, p.footR);
  line(hipR, hipL);
  line(p.hip, p.neck);
  line(p.shoulderR, p.shoulderL);
  line(p.shoulderL, p.elbowL);
  line(p.elbowL, p.handL);
  line(p.shoulderR, p.elbowR);
  line(p.elbowR, p.handR);
  // Feet: short strokes along the ground.
  line(p.footL, {p.footL.x + 5.0f, p.footL.y});
  line(p.footR, {p.footR.x - 5.0f, p.footR.y});
  // Head: a ring.
  const auto hx = static_cast<int32_t>(p.head.x + 0.5f), hy = static_cast<int32_t>(p.head.y + 0.5f);
  const auto hr = static_cast<int32_t>(p.headR + 0.5f);
  s.fillSmoothCircle(hx, hy, hr, c);
  s.fillSmoothCircle(hx, hy, hr - 4, kBg);
  s.clearClipRect();
  drawDot(flash, kBg, kAccent, true);
}

void DanceView::drawCrab(const crab::Pose& p, float weight, bool flash) {
  if (!ready_ || skin_ != dance::Skin::Crab) return;
  auto& s = sprite_;
  // The palette, idle to dance, as RGB565. Every pixel in a crab colour is
  // inside this frame's dirty rectangle (it holds the last frame's crab), so
  // the crab is redrawn in the new colours everywhere; the background,
  // shadow, ground and dot entries are the same in both palettes.
  const int step = crab::paletteStep(weight);
  if (step != paletteStep_) {
    uint8_t pal[crab::kPaletteSize][3];
    crab::paletteAt(step, pal);
    for (int i = 0; i < crab::kPaletteSize; ++i) ink_[i] = lgfx::color565(pal[i][0], pal[i][1], pal[i][2]);
    paletteStep_ = step;
  }
  // The blitter hands out palette indices; this canvas turns them into colours.
  struct Ink {
    M5Canvas& s;
    const uint16_t* ink;
    void fillRect(int x, int y, int w, int h, int ix) { s.fillRect(x, y, w, h, ink[ix]); }
  } ink{s, ink_};
  const crab::Rect r = crab::drawnRect(p);
  beginFrame(Rect{r.x0, r.y0, r.x1, r.y1}, ink_[0]);
  ink.fillRect(4, crab::kOriginY, kBoxW - 8, 2, crab::kGroundIx);  // the stick figure's ground line
  const crab::Rect sh = crab::shadowRect(p);
  ink.fillRect(sh.x0, sh.y0, sh.x1 - sh.x0, sh.y1 - sh.y0, crab::kShadowIx);
  // Integer blocks of one colour: nothing is smoothed or blended.
  crab::forEachLayer(p, [&](crab::FrameId f, int x, int y, bool flip, int repeat) {
    crab::blit(ink, f, x, y, flip, repeat);
  });
  s.clearClipRect();
  drawDot(flash, ink_[0], ink_[crab::kBeatDotIx], false);
}

void DanceView::pushRect(const Rect& r) {
  if (r.empty()) return;
  auto& d = M5.Display;
  // M5GFX clips the push to the display's clip rectangle: only these pixels
  // are converted and sent.
  d.setClipRect(kBoxX + r.x0, kBoxY + r.y0, r.x1 - r.x0, r.y1 - r.y0);
  sprite_.pushSprite(&d, kBoxX, kBoxY);
  d.clearClipRect();
}

void DanceView::push() {
  if (!ready_) return;
  pushRect(dirty_);
  if (flashDirty_ && !full_) pushRect({kFlashX - 8, kFlashY - 8, kFlashX + 8, kFlashY + 8});
  full_ = false;
}
