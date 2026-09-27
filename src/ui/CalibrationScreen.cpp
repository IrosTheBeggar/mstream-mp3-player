#include "ui/CalibrationScreen.h"

#include <M5Unified.h>

#include <cstdlib>

#include "ui/LcdLock.h"

namespace {

// The tab bar design's colours (RGB565).
constexpr uint16_t kBg = 0x0862;
constexpr uint16_t kSurf = 0x10C4;
constexpr uint16_t kBtn = 0x2988;
constexpr uint16_t kTxt = 0xF79F;
constexpr uint16_t kDim = 0x8CB5;
constexpr uint16_t kCoral = 0xFB49;
constexpr uint16_t kCoralDk = 0x1861;
constexpr uint16_t kAmber = 0xFE07;
constexpr uint16_t kGrey = 0x8410;

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kHeaderH = 26;
constexpr int kBand = 40;  // a full clear in 40-line pieces, each its own bus hold
// A tap further than this from the cross (raw) is asked again: the panel
// reads up to ~45 px too far right, so 70 px leaves room for a finger.
constexpr int kAcceptDx = 70;
constexpr int kAcceptDy = 50;
// The fit is kept only if it brings every axis within this (rms, px).
constexpr float kMaxRmsAfter = 15.0f;
// The check page starts over after this many taps (the dots pile up).
constexpr int kCheckTapsPerPage = 16;

auto& lcd() { return M5.Display; }

}  // namespace

// Spread over the screen, with distinct x and y each, so that any first
// 5-9 of them give each axis as many points; the right side (where the
// panel is off the most) gets its share.
const CalibrationScreen::Target CalibrationScreen::kTargets[kMaxTargets] = {
    {160, 120}, {20, 40}, {300, 200}, {90, 170}, {230, 60}, {55, 215}, {265, 140}, {125, 55}, {195, 185},
};

void CalibrationScreen::clear() {
  for (int y = 0; y < kH; y += kBand) {
    LcdLock lock;
    lcd().fillRect(0, y, kW, kBand, kBg);
  }
}

void CalibrationScreen::drawHeader(const char* title, const char* right) {
  auto& d = lcd();
  LcdLock lock;
  d.fillRect(0, 0, kW, kHeaderH, kSurf);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(kTxt, kSurf);
  d.setTextDatum(textdatum_t::middle_left);
  d.setTextPadding(0);
  d.drawString(title, 8, kHeaderH / 2);
  if (right) {
    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextColor(kCoral, kSurf);
    d.setTextDatum(textdatum_t::middle_right);
    d.drawString(right, kW - 8, kHeaderH / 2);
  }
  d.setTextDatum(textdatum_t::top_left);
}

void CalibrationScreen::drawHint(const char* text, uint16_t colour) {
  // Away from the cross: under it if it's in the top half, else over it.
  const int y = kTargets[next_].y < 130 ? 200 : 70;
  auto& d = lcd();
  LcdLock lock;
  d.fillRect(0, y - 11, kW, 22, kBg);
  d.setFont(&fonts::Font2);
  d.setTextColor(colour, kBg);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextPadding(0);
  d.drawString(text, kW / 2, y);
  d.setTextDatum(textdatum_t::top_left);
}

void CalibrationScreen::drawTarget() {
  const Target& t = kTargets[next_];
  char title[40];
  snprintf(title, sizeof(title), "Touch calibration  %d / %d", next_ + 1, count_);
  drawHeader(title, "Cancel");
  {
    auto& d = lcd();
    LcdLock lock;
    d.drawFastHLine(t.x - 12, t.y, 25, kCoral);
    d.drawFastVLine(t.x, t.y - 12, 25, kCoral);
    d.drawCircle(t.x, t.y, 6, kCoral);
  }
  drawHint("Tap the centre of the cross", kDim);
}

void CalibrationScreen::open(int targets) {
  count_ = targets < kMinTargets ? kMinTargets : targets > kMaxTargets ? kMaxTargets : targets;
  next_ = 0;
  page_ = Page::Targets;
  clear();
  drawTarget();
  Serial.printf("[cal] touch calibration: tap the %d crosshairs (Cancel at the top right, or aq)\n", count_);
}

void CalibrationScreen::openCheck() {
  page_ = Page::Check;
  drawCheckPage();
}

void CalibrationScreen::close() {
  if (page_ == Page::Closed) return;
  page_ = Page::Closed;
  clear();
  Serial.println("[cal] closed");
  if (closed_) closed_();
}

void CalibrationScreen::drawButton(int x, int y, int w, int h, const char* label, bool primary) {
  auto& d = lcd();
  LcdLock lock;
  const uint16_t bg = primary ? kCoral : kBtn;
  d.fillRoundRect(x, y, w, h, 10, bg);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(primary ? kCoralDk : kTxt, bg);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextPadding(0);
  d.drawString(label, x + w / 2, y + h / 2);
  d.setTextDatum(textdatum_t::top_left);
}

void CalibrationScreen::finish() {
  fitOk_ = TouchCalibration::fitAxis(sx_, count_, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots, &fitted_.x,
                                     &repX_) &&
           TouchCalibration::fitAxis(sy_, count_, TouchCalibration::kYKnotRaw, TouchCalibration::kYKnots, &fitted_.y,
                                     &repY_);
  const bool agree = fitOk_ && repX_.rmsAfter <= kMaxRmsAfter && repY_.rmsAfter <= kMaxRmsAfter;
  Serial.printf("[cal] fit %s: x off by %.1f px rms (max %.1f) raw, %.1f (max %.1f) corrected; y %.1f (max %.1f) raw, "
                "%.1f (max %.1f) corrected; with the table in use x %.1f, y %.1f\n",
                !fitOk_ ? "FAILED" : agree ? "ok" : "rejected (the taps don't agree)", repX_.rmsBefore,
                repX_.maxBefore, repX_.rmsAfter, repX_.maxAfter, repY_.rmsBefore, repY_.maxBefore, repY_.rmsAfter,
                repY_.maxAfter, TouchCalibration::evaluate(input_.calibration().x, sx_, count_).rmsAfter,
                TouchCalibration::evaluate(input_.calibration().y, sy_, count_).rmsAfter);
  fitOk_ = agree;
  page_ = Page::Result;
  drawResult();
}

void CalibrationScreen::drawResult() {
  clear();
  drawHeader("Touch calibration", nullptr);
  auto& d = lcd();
  char line[64];
  {
    LcdLock lock;
    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextDatum(textdatum_t::top_left);
    d.setTextPadding(0);
    d.setTextColor(kTxt, kBg);
    snprintf(line, sizeof(line), "%d taps. Off by (px, rms):", count_);
    d.drawString(line, 10, 40);
  }
  {
    LcdLock lock;
    d.setFont(&fonts::Font2);
    d.setTextColor(kDim, kBg);
    const TouchCalibration& now = input_.calibration();
    d.drawString("raw / table in use / new table", 10, 64);
    d.setTextColor(kTxt, kBg);
    snprintf(line, sizeof(line), "x:  %.0f / %.0f / %.0f", repX_.rmsBefore,
             TouchCalibration::evaluate(now.x, sx_, count_).rmsAfter, repX_.rmsAfter);
    d.drawString(line, 10, 84);
    snprintf(line, sizeof(line), "y:  %.0f / %.0f / %.0f", repY_.rmsBefore,
             TouchCalibration::evaluate(now.y, sy_, count_).rmsAfter, repY_.rmsAfter);
    d.drawString(line, 10, 102);
    if (!fitOk_) {
      d.setTextColor(kAmber, kBg);
      d.drawString("The taps don't agree with each other.", 10, 128);
      d.drawString("Try again: tap each cross at its centre.", 10, 146);
    } else {
      d.setTextColor(kDim, kBg);
      d.drawString("Save to use the new table from now on.", 10, 134);
    }
  }
  // Big buttons, hit tested by halves of the bottom of the screen.
  drawButton(10, 172, 140, 56, fitOk_ ? "Discard" : "Cancel", false);
  drawButton(170, 172, 140, 56, fitOk_ ? "Save" : "Try again", true);
}

void CalibrationScreen::drawCheckPage() {
  clear();
  drawHeader("Check the touch", "Done");
  auto& d = lcd();
  LcdLock lock;
  d.setFont(&fonts::Font2);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextPadding(0);
  d.setTextColor(kDim, kBg);
  d.drawString("Tap anywhere. Coral: where the Core2 reads", 10, 36);
  d.drawString("the touch now. Grey: the panel's own reading.", 10, 54);
  checkTaps_ = 0;
}

void CalibrationScreen::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (page_ == Page::Closed) return;
  // A press held past the hold time counts like a tap (careful aiming is slow).
  const bool tap = e.type == T::Tap || e.type == T::LongPress;
  if (!tap) return;

  if (page_ == Page::Targets) {
    if (e.y < kHeaderH + 4 && e.inRightEdgeZone(240)) {
      input_.tapTick();
      Serial.println("[cal] cancelled");
      close();
      return;
    }
    const Target& t = kTargets[next_];
    if (std::abs(e.rawX - t.x) > kAcceptDx || std::abs(e.rawY - t.y) > kAcceptDy) {
      Serial.printf("[cal] tap at raw (%d,%d), too far from the cross at (%d,%d): again\n", e.rawX, e.rawY, t.x,
                    t.y);
      drawHint("Missed: tap the cross itself", kAmber);
      return;
    }
    input_.tapTick();  // taken (a miss gets no tick)
    sx_[next_] = {e.rawX, t.x};
    sy_[next_] = {e.rawY, t.y};
    Serial.printf("[cal] %d/%d: the cross at (%d,%d) read (%d,%d), the table in use says (%d,%d)\n", next_ + 1,
                  count_, t.x, t.y, e.rawX, e.rawY, e.x, e.y);
    {
      LcdLock lock;
      lcd().fillRect(t.x - 13, t.y - 13, 27, 27, kBg);
    }
    if (++next_ >= count_) {
      finish();
      return;
    }
    drawTarget();
    return;
  }

  if (page_ == Page::Result) {
    if (e.y < 150) return;
    input_.tapTick();
    const bool right = e.x >= 160 || e.atRightEdge();
    if (!right) {
      Serial.println(fitOk_ ? "[cal] discarded: the table in use stays" : "[cal] cancelled");
      close();
    } else if (!fitOk_) {
      open(count_);
    } else if (input_.setCalibration(fitted_)) {
      Serial.println("[cal] saved: the new table is in use");
      input_.printStatus();
      openCheck();
    } else {
      Serial.println("[cal] couldn't save the table (NVS): the one in use stays");
      close();
    }
    return;
  }

  if (page_ == Page::Check) {
    input_.tapTick();
    if (e.y < kHeaderH + 4 && e.inRightEdgeZone(240)) {
      close();
      return;
    }
    if (++checkTaps_ > kCheckTapsPerPage) drawCheckPage();
    Serial.printf("[cal] check: raw (%d,%d) -> (%d,%d)%s\n", e.rawX, e.rawY, e.x, e.y,
                  e.atRightEdge() ? " (at the right clamp)" : "");
    LcdLock lock;
    lcd().fillCircle(e.rawX, e.rawY, 3, kGrey);
    lcd().fillCircle(e.x, e.y, 4, kCoral);
  }
}
