#include "ui/CalibrationScreen.h"

#include <M5Unified.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "SheetLayout.h"
#include "TextFit.h"
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/LcdLock.h"
#include "ui/Theme.h"

using ui::Font;
using ui::Fonts;
namespace col = ui::col;

namespace {

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kHeaderH = uitext::kCalHeaderH;
constexpr uint16_t kAccent = ui::accent::Output;  // it opens from Output
// A sample's flash (the cross turns green), before the next cross.
constexpr uint32_t kFlashMs = 150;
// The check page starts over after this many taps (the rings pile up).
constexpr int kCheckTapsPerPage = 16;
constexpr int kRowH = sheet::kRowH;  // 40, the touch minimum
// The check page's taps draw above its Done row, the header included (its
// lines go at the first tap); the probe's result shows the dots and marks in their band only.
constexpr int kCheckMarksBottom = uitext::kCalRowsBottom - kRowH;  // 180: the Done row's top
constexpr int kDotsBandY = 94, kDotsBandH = 45;
// The two places a hint goes (hintY()), cleared as each cross comes up.
constexpr int kHintTopY = 56, kHintBottomY = 180, kHintBottomOneY = 192;

auto& lcd() { return M5.Display; }

// Where the hint goes: away from the cross (or dot), under it if it's in
// the top half, else over it; two lines (the first cross's) take 20 px more.
int hintY(int crossY, int lines) {
  if (crossY < 130) return lines > 1 ? kHintBottomY : kHintBottomOneY;
  return kHintTopY;
}

}  // namespace

// The crosses: TouchCheck's (host-tested for their spread and clearances).
static const touchcheck::Dot* const kTargets = touchcheck::kCross;
// judgeCross()'s A label zone is the band drawAHint() draws.
static_assert(touchcheck::kAHintY == uitext::kCalHintY, "the A hint's band");
static_assert(touchcheck::kAHintW >= uitext::kCalAHintX + uitext::kCalAHintW, "the A hint's label");

// ---- drawing ----

void CalibrationScreen::clear() { ui::gfx::fill(0, 0, kW, kH, col::BG, true); }

// The page (or cross) is up now: touches that went down before, or within
// kSettleMs, don't count.
void CalibrationScreen::shown() {
  shownMs_ = millis();
  touchOk_ = false;
  pressedRow_ = -1;
}

// A miss: the same dot or cross stays up, so a quick retap on it is a real
// attempt; only a bounce of the missed tap (the panel finds the finger
// again within ~145 ms) is ignored.
void CalibrationScreen::rearm() {
  shownMs_ = millis() - (kSettleMs - kBounceMs);
  touchOk_ = false;
  pressedRow_ = -1;
}

void CalibrationScreen::drawHeader(const char* title, const char* pill, const char* right, uint16_t rightColour) {
  using namespace uitext;
  M5Canvas& s = ui::gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kHeaderH, col::SURF);
  int x = 10;
  int room = kCalTitleX + kCalTitleW - x;
  if (pill) {
    const int pw = f.width(Font::Bold, pill) + kCalPillPad;
    s.fillRoundRect(kCalPillX, 3, pw, kHeaderH - 6, (kHeaderH - 6) / 2, col::BTN);
    f.draw(s, Font::Bold, pill, kCalPillX + pw / 2, kHeaderH / 2, pw - 8, col::TXT, col::BTN, Fonts::Align::Centre);
    x = kCalTitleX;
    room = kCalTitleW;
  }
  f.draw(s, Font::Bold, title, x, kHeaderH / 2, room, col::TXT, col::SURF);
  if (right) {
    f.draw(s, Font::Small, right, 312, kHeaderH / 2, kCalProgressW, rightColour, col::SURF, Fonts::Align::Right);
  }
  s.fillRect(0, kHeaderH - 2, kW, 2, kAccent);
  ui::gfx::push(s, 0, 0, kW, kHeaderH, true);
}

void CalibrationScreen::drawLine(int y, const char* text, uint16_t colour, int font) {
  M5Canvas& s = ui::gfx::strip();
  Fonts& f = Fonts::instance();
  const Font use = font >= 0 ? static_cast<Font>(font)
                   : f.width(Font::Body, text) <= uitext::kCalLineW ? Font::Body
                                                                    : Font::Small;
  const int kLineH = use == Font::Title ? 30 : 22;
  s.fillRect(0, 0, kW, kLineH, col::BG);
  f.draw(s, use, text, kW / 2, kLineH / 2, uitext::kCalLineW, colour, col::BG, Fonts::Align::Centre);
  ui::gfx::push(s, 0, y - kLineH / 2, kW, kLineH, true);
}

void CalibrationScreen::drawHintBand(int y, int lines) {
  ui::gfx::fill(0, y - 11, kW, 22 + (lines - 1) * 20, col::BG, true);
}

// "A: Cancel" over the A dot: a red arrow down to it (as the tips draw
// them), the page's verb beside it; on the test taps page, the grey mark's
// key to the right (a grey dot, "without calibration").
void CalibrationScreen::drawAHint(const char* verb, const char* key) {
  M5Canvas& s = ui::gfx::strip();
  Fonts& f = Fonts::instance();
  constexpr int kBandH = kH - uitext::kCalHintY;  // 18
  s.fillRect(0, 0, kW, kBandH, col::BG);
  s.fillRect(52, 0, 4, 8, col::RED);
  s.fillTriangle(46, 7, 62, 7, 54, 16, col::RED);
  char t[32];
  snprintf(t, sizeof(t), "A: %s", verb);
  f.draw(s, Font::Small, t, uitext::kCalAHintX, kBandH / 2, uitext::kCalAHintW, col::SOFT, col::BG);
  if (key) {
    s.fillCircle(uitext::kCalKeyX + 3, kBandH / 2, 3, col::DIM);
    f.draw(s, Font::Small, key, uitext::kCalKeyTextX, kBandH / 2, uitext::kCalKeyTextW, col::DIM, col::BG);
  }
  ui::gfx::push(s, 0, uitext::kCalHintY, kW, kBandH, true);
}

void CalibrationScreen::setRows(const Row* rows, int n) {
  nRows_ = n < 0 ? 0 : n > kMaxRows ? kMaxRows : n;
  for (int i = 0; i < nRows_; ++i) rows_[i] = rows[i];
  for (int i = 0; i < nRows_; ++i) drawRow(i, false);
}

// A full-width row (40 px, the touch minimum): the primary in the accent
// (Bold, dark text), the others as buttons (Body).
void CalibrationScreen::drawRow(int i, bool pressed) {
  using namespace uitext;
  if (i < 0 || i >= nRows_) return;
  const Row& r = rows_[i];
  M5Canvas& s = ui::gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kRowH, col::BG);
  const uint16_t bg = r.primary ? (pressed ? col::SOFT : kAccent) : (pressed ? col::BTN_HI : col::BTN);
  const uint16_t ink = r.primary ? col::DARK : col::TXT;
  s.fillRoundRect(kCalRowX, 2, kCalRowW, kRowH - 4, 10, bg);
  const Font font = r.primary ? Font::Bold : Font::Body;
  if (r.detail && r.detail[0]) {
    const int w = f.draw(s, font, r.label, kCalRowX + kCalRowPad, kRowH / 2, kCalRowW - 2 * kCalRowPad, ink, bg);
    const int room = kCalRowW - 3 * kCalRowPad - w;
    f.draw(s, Font::Small, r.detail, kCalRowX + kCalRowW - kCalRowPad, kRowH / 2, room,
           r.primary ? col::DARK : col::DIM, bg, Fonts::Align::Right);
  } else {
    f.draw(s, font, r.label, kW / 2, kRowH / 2, kCalRowW - 2 * kCalRowPad, ink, bg, Fonts::Align::Centre);
  }
  ui::gfx::push(s, 0, kCalRowsBottom - (nRows_ - i) * kRowH, kW, kRowH, true);
}

// By y alone: the panel reads y true.
int CalibrationScreen::rowAt(int y) const {
  for (int i = 0; i < nRows_; ++i) {
    const int top = uitext::kCalRowsBottom - (nRows_ - i) * kRowH;
    if (y >= top && y < top + kRowH) return i;
  }
  return -1;
}

// ---- opening, closing, the way out ----

void CalibrationScreen::open(Start how, int targets) {
  count_ = targets < kMinTargets ? kMinTargets : targets > kMaxTargets ? kMaxTargets : targets;
  justSaved_ = undone_ = false;
  flashing_ = false;
  lastInputMs_ = millis();
  switch (how) {
    case Start::FirstBoot:
      Serial.println("[cal] the touch check (first boot, nothing calibrated): tap the 3 dots (A or Skip: not now)");
      startProbe();
      break;
    case Start::Check:
      openCheck();
      break;
    case Start::Crosses:
      Serial.printf("[cal] touch calibration: tap the %d crosses (A or Cancel at the top left, or aq: cancel)\n",
                    count_);
      startTargets();
      break;
  }
}

void CalibrationScreen::close() {
  if (page_ == Page::Closed) return;
  page_ = Page::Closed;
  flashing_ = false;
  clear();
  Serial.println("[cal] closed");
  if (closed_) closed_();
}

void CalibrationScreen::leave() {
  if (page_ == Page::Closed) return;
  lastInputMs_ = millis();
  input_.tapTick();
  switch (page_) {
    case Page::Probe:
      answer(touchcheck::CheckEnd::Skip);
      Serial.println("[cal] A: the touch check skipped (Output > Touch calibration has it)");
      close();
      break;
    case Page::ProbeResult:
      answer(verdict_.calibrate ? touchcheck::CheckEnd::NotNow : touchcheck::CheckEnd::GoOn);
      Serial.println(verdict_.calibrate ? "[cal] A: not now (Output > Touch calibration has it)" : "[cal] A: go on");
      close();
      break;
    case Page::Targets:
      Serial.println("[cal] A: cancelled");
      close();
      break;
    case Page::Result:
      Serial.println("[cal] A: discarded: the table in use stays");
      close();
      break;
    case Page::Check:
      if (justSaved_) {
        undo();
      } else {
        close();
      }
      break;
    case Page::Closed: break;
  }
}

void CalibrationScreen::loop(uint32_t nowMs) {
  if (page_ == Page::Closed) return;
  if (flashing_ && static_cast<int32_t>(nowMs - flashUntilMs_) >= 0) {
    flashing_ = false;
    eraseCross(kTargets[next_]);
    if (++next_ >= count_) {
      finish();
    } else {
      drawTarget();
    }
  }
  if (static_cast<int32_t>(nowMs - lastInputMs_) >= static_cast<int32_t>(kIdleCloseMs)) {
    Serial.printf("[cal] abandoned: no touch for %lu s (%s)\n", (unsigned long)(kIdleCloseMs / 1000),
                  justSaved_ ? "the new table stays" : "nothing saved");
    // Not an answer: a check nobody touched shows again at the next boot.
    answer(touchcheck::CheckEnd::TimedOut);
    close();
  }
}

void CalibrationScreen::act(Act a) {
  switch (a) {
    case Act::Calibrate:
      answer(touchcheck::CheckEnd::Calibrate);
      Serial.println("[cal] calibrate");
      startTargets();
      break;
    case Act::NotNow:
      answer(touchcheck::CheckEnd::NotNow);
      Serial.println("[cal] not now (Output > Touch calibration has it)");
      close();
      break;
    case Act::Save: save(); break;
    case Act::TryAgain:
      Serial.println("[cal] try again");
      startTargets();
      break;
    case Act::Discard:
      Serial.println("[cal] discarded: the table in use stays");
      close();
      break;
    case Act::Done: close(); break;
    case Act::None: break;
  }
}

// The first-boot check's answer, stored once (TouchCheck's answers():
// never the 60 s close).
void CalibrationScreen::answer(touchcheck::CheckEnd end) {
  if (input_.touchCheckAnswered()) return;
  if (!touchcheck::answers(end)) {
    if (page_ == Page::Probe || page_ == Page::ProbeResult) {
      Serial.println("[cal] the touch check isn't answered: it shows again at the next boot");
    }
    return;
  }
  input_.setTouchCheckAnswered();
}

// ---- touches ----

void CalibrationScreen::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (page_ == Page::Closed) return;
  if (e.type == T::Down) {
    lastInputMs_ = millis();
    // Only a touch that landed on this page, once it has settled.
    touchOk_ = !flashing_ && static_cast<int32_t>(e.ms - shownMs_) >= static_cast<int32_t>(kSettleMs);
    if (touchOk_ && nRows_ > 0) {
      pressedRow_ = rowAt(e.y);
      drawRow(pressedRow_, true);
    }
    return;
  }
  if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
    if (pressedRow_ >= 0) {
      drawRow(pressedRow_, false);
      pressedRow_ = -1;
    }
    if (e.type != T::Release) touchOk_ = false;  // (a Release is a LongPress's end: already handled)
    return;
  }
  // A press held past the hold time counts like a tap (careful aiming is slow).
  if (e.type != T::Tap && e.type != T::LongPress) return;
  if (!touchOk_ || flashing_) return;
  touchOk_ = false;
  if (pressedRow_ >= 0) drawRow(pressedRow_, false);
  pressedRow_ = -1;
  // The "A: <verb>" label is on the glass: a tap on it (by y alone, like
  // the rows) does what it says. On the crosses judgeCross() decides.
  if (e.y >= uitext::kCalHintY && page_ != Page::Targets) {
    Serial.println("[cal] the A label tapped on the glass");
    leave();
    return;
  }

  switch (page_) {
    case Page::Probe: onProbeTap(e); return;
    case Page::Targets: onTargetTap(e); return;
    case Page::ProbeResult:
      if (!verdict_.calibrate) {
        input_.tapTick();
        answer(touchcheck::CheckEnd::GoOn);
        Serial.println("[cal] the touch is accurate: go on");
        close();
        return;
      }
      break;
    case Page::Check:
      if (e.y >= kCheckMarksBottom) {
        input_.tapTick();
        close();
        return;
      }
      break;
    default: break;
  }
  const int row = rowAt(e.y);
  if (row >= 0) {
    input_.tapTick();
    act(rows_[row].act);
    return;
  }
  if (page_ != Page::Check) return;
  // The check page: where the Core2 reads the tap (a ring that shows past
  // the fingertip once it lifts), and with a table saved, where it would
  // without one (grey).
  input_.tapTick();
  // The page's first tap clears its lines (so no mark draws over them, and
  // none is hidden: every tap shows where it landed, on the header too); a
  // page full of rings starts over, its header drawn again.
  if (checkTaps_ == 0 || checkTaps_ >= kCheckTapsPerPage) {
    if (checkTaps_ > 0) drawCheckHeader();
    ui::gfx::fill(0, kHeaderH, kW, kCheckMarksBottom - kHeaderH, col::BG, true);
    checkTaps_ = 0;
  }
  ++checkTaps_;
  Serial.printf("[cal] check: raw (%d,%d) -> (%d,%d)%s%s\n", e.rawX, e.rawY, e.x, e.y,
                e.atRightEdge() ? " (at the right clamp)" : "", input_.scriptedNote());
  LcdLock lock;
  lcd().setClipRect(0, 0, kW, kCheckMarksBottom);
  if (input_.calibrated()) lcd().fillCircle(e.rawX, e.rawY, 3, col::DIM);
  lcd().drawCircle(e.x, e.y, 10, col::CORAL);
  lcd().drawCircle(e.x, e.y, 9, col::CORAL);
  lcd().clearClipRect();
}

// ---- the first-boot check ----

void CalibrationScreen::startProbe() {
  page_ = Page::Probe;
  dot_ = 0;
  dotRetried_ = false;
  nRows_ = 0;
  clear();
  drawProbeDot();
  drawAHint(uitext::kCalSkip);
}

void CalibrationScreen::drawProbeDot() {
  using namespace uitext;
  char progress[16];
  snprintf(progress, sizeof(progress), "%d of %d", dot_ + 1, touchcheck::kDots);
  drawHeader(kCheckTitle, kCalSkip, progress, col::DIM);
  const touchcheck::Dot& d = touchcheck::kDot[dot_];
  {
    LcdLock lock;
    lcd().fillCircle(d.x, d.y, 9, kAccent);
    lcd().fillCircle(d.x, d.y, 2, col::TXT);
  }
  const int lines = dot_ == 0 ? 2 : 1;
  const int y = hintY(d.y, lines);
  drawHintBand(kHintBottomY, 2);  // (the last dot's, and its miss)
  if (dot_ == 0) {
    drawLine(y, kCheckFirstHint[0], col::SOFT);
    drawLine(y + 20, kCheckFirstHint[1], col::SOFT);
  } else {
    drawLine(y, kCheckHint, col::SOFT);
  }
  shown();
}

void CalibrationScreen::onProbeTap(const InputEvent& e) {
  const touchcheck::Dot& d = touchcheck::kDot[dot_];
  // The Skip pill, unless the tap could be this dot's (it never is: the
  // dots are well below the header).
  const bool plausible =
      std::abs(e.rawX - d.x) <= touchcheck::kAcceptDx && std::abs(e.rawY - d.y) <= touchcheck::kAcceptDy;
  if (!plausible && e.x < touchcheck::kCancelW && e.y < touchcheck::kCancelH) {
    input_.tapTick();
    answer(touchcheck::CheckEnd::Skip);
    Serial.println("[cal] the touch check skipped (Output > Touch calibration has it)");
    close();
    return;
  }
  touchcheck::Tap t;
  t.x = e.x;
  t.y = e.y;
  t.clamped = TouchCalibration::clampedLow(e.rawX) || TouchCalibration::clampedHighX(e.rawX);
  const int off = touchcheck::offBy(d, t);
  if (touchcheck::askAgain(d, t) && !dotRetried_) {
    dotRetried_ = true;
    input_.missBuzz();
    Serial.printf("[cal] check %d/%d: the dot at (%d,%d) read (%d,%d), %d px off: asked again%s\n", dot_ + 1,
                  touchcheck::kDots, d.x, d.y, e.x, e.y, off, input_.scriptedNote());
    const int lines = dot_ == 0 ? 2 : 1;
    const int y = hintY(d.y, lines);
    drawHintBand(y, lines);
    drawLine(y, uitext::kCheckMissed, col::AMBER);
    rearm();
    return;
  }
  input_.tapTick();
  dotTaps_[dot_] = t;
  Serial.printf("[cal] check %d/%d: the dot at (%d,%d) read (%d,%d) (raw %d,%d), %d px off%s\n", dot_ + 1,
                touchcheck::kDots, d.x, d.y, e.x, e.y, e.rawX, e.rawY, off, input_.scriptedNote());
  {
    LcdLock lock;
    lcd().fillCircle(d.x, d.y, 9, col::BG);
    lcd().drawCircle(d.x, d.y, 9, col::FAINT);
    lcd().setClipRect(0, kHeaderH, kW, uitext::kCalHintY - kHeaderH);
    lcd().fillCircle(e.x, e.y, 4, col::DIM);
    lcd().clearClipRect();
  }
  dotRetried_ = false;
  if (++dot_ >= touchcheck::kDots) {
    finishProbe();
    return;
  }
  drawProbeDot();
}

void CalibrationScreen::finishProbe() {
  // (Not answered yet: the verdict page's buttons answer it.)
  verdict_ = touchcheck::verdict(dotTaps_);
  if (verdict_.calibrate) {
    char text[80];
    touchcheck::verdictText(verdict_, text, sizeof(text));
    Serial.printf("[cal] the touch check: off: calibrate? (%s)\n", text);
  } else {
    Serial.println("[cal] the touch check: accurate");
  }
  page_ = Page::ProbeResult;
  drawProbeResult();
}

// The dots (dim rings), where each tap landed (a coral dot) and a line
// between: what the verdict is made of.
void CalibrationScreen::drawProbeMarks() {
  LcdLock lock;
  lcd().setClipRect(0, kDotsBandY, kW, kDotsBandH);
  for (int i = 0; i < touchcheck::kDots; ++i) {
    const touchcheck::Dot& d = touchcheck::kDot[i];
    const touchcheck::Tap& t = dotTaps_[i];
    lcd().drawCircle(d.x, d.y, 9, col::FAINT);
    lcd().drawLine(d.x, d.y, t.x, t.y, col::DIM);
    lcd().fillCircle(t.x, t.y, 4, col::CORAL);
  }
  lcd().clearClipRect();
}

void CalibrationScreen::drawProbeResult() {
  using namespace uitext;
  clear();
  drawHeader(kCheckTitle, nullptr, nullptr, 0);
  drawProbeMarks();
  if (!verdict_.calibrate) {
    nRows_ = 0;
    drawLine(52, kCheckAccurate, col::GREEN, static_cast<int>(Font::Title));
    drawLine(170, kCheckGoOn, col::SOFT);
    drawAHint(kCalDone);
    shown();
    return;
  }
  char text[80];
  touchcheck::verdictText(verdict_, text, sizeof(text));
  char lines[2][64];
  Fonts& f = Fonts::instance();
  const int n = textfit::wrap(f.fit(Font::Body), text, strlen(text), kCalLineW, 2, &lines[0][0], sizeof(lines[0]));
  for (int i = 0; i < n; ++i) drawLine(40 + i * 20, lines[i], col::TXT);
  drawLine(84, kCheckAsk, col::TXT, static_cast<int>(Font::Bold));
  const Row rows[2] = {{kCalCalibrate, kCalCalibrateDetail, Act::Calibrate, true},
                       {kCheckNotNow, kCheckLater, Act::NotNow, false}};
  setRows(rows, 2);
  drawAHint(kCheckNotNow);
  shown();
}

// ---- the crosses ----

void CalibrationScreen::startTargets() {
  page_ = Page::Targets;
  next_ = 0;
  nRows_ = 0;
  flashing_ = false;
  clear();
  drawTarget();
  drawAHint(uitext::kCalCancel);
}

void CalibrationScreen::drawCross(const Target& t, uint16_t colour) {
  LcdLock lock;
  lcd().drawFastHLine(t.x - 12, t.y, 25, colour);
  lcd().drawFastVLine(t.x, t.y - 12, 25, colour);
  lcd().drawCircle(t.x, t.y, 6, colour);
}

void CalibrationScreen::eraseCross(const Target& t) {
  LcdLock lock;
  lcd().fillRect(t.x - 13, t.y - 13, 27, 27, col::BG);
}

// A hint in place of the one up (the first cross's has two lines).
void CalibrationScreen::targetHint(const char* text, uint16_t colour) {
  const Target& t = kTargets[next_];
  const int lines = next_ == 0 ? 2 : 1;
  const int y = hintY(t.y, lines);
  drawHintBand(y, lines);
  drawLine(y, text, colour);
}

void CalibrationScreen::drawTarget() {
  using namespace uitext;
  const Target& t = kTargets[next_];
  char progress[16];
  snprintf(progress, sizeof(progress), "%d of %d", next_ + 1, count_);
  drawHeader(kCalTitle, kCalCancel, progress, col::DIM);
  // The last cross's hint goes (it may have been at the other place).
  drawHintBand(kHintTopY, 2);
  drawHintBand(kHintBottomY, 2);
  drawCross(t, kAccent);
  tries_ = touchcheck::CrossTries{};
  if (next_ == 0) {
    const int y = hintY(t.y, 2);
    char second[48];
    snprintf(second, sizeof(second), kCalFirstHint[1], count_);
    drawLine(y, kCalFirstHint[0], col::SOFT);
    drawLine(y + 20, second, col::SOFT);
  } else {
    targetHint(kCalHint, col::SOFT);
  }
  shown();
}

void CalibrationScreen::onTargetTap(const InputEvent& e) {
  const Target& t = kTargets[next_];
  switch (touchcheck::judgeCross(tries_, t.x, t.y, e.rawX, e.rawY, e.x, e.y)) {
    case touchcheck::Take::Cancel:
      input_.tapTick();
      Serial.println("[cal] cancelled");
      close();
      return;
    case touchcheck::Take::Miss:
      input_.missBuzz();
      Serial.printf("[cal] tap at raw (%d,%d), too far from the cross at (%d,%d): again (miss %d)%s\n", e.rawX, e.rawY,
                    t.x, t.y, tries_.misses, input_.scriptedNote());
      targetHint(uitext::kCalMissed, col::AMBER);
      rearm();
      return;
    case touchcheck::Take::Sample: break;
  }
  input_.tapTick();
  sx_[next_] = {e.rawX, t.x};
  sy_[next_] = {e.rawY, t.y};
  Serial.printf("[cal] %d/%d: the cross at (%d,%d) read (%d,%d), the table in use says (%d,%d)%s%s\n", next_ + 1,
                count_, t.x, t.y, e.rawX, e.rawY, e.x, e.y, tries_.misses >= 2 ? " (two taps agreed)" : "",
                input_.scriptedNote());
  // Taken: the cross flashes green, then the next one (loop()).
  drawCross(t, col::GREEN);
  {
    LcdLock lock;
    lcd().fillCircle(t.x, t.y, 4, col::GREEN);
  }
  flashing_ = true;
  flashUntilMs_ = millis() + kFlashMs;
}

void CalibrationScreen::finish() {
  answer(touchcheck::CheckEnd::Calibrated);
  const bool fitOk = TouchCalibration::fitAxis(sx_, count_, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots,
                                               &fitted_.x, &repX_) &&
                     TouchCalibration::fitAxis(sy_, count_, TouchCalibration::kYKnotRaw, TouchCalibration::kYKnots,
                                               &fitted_.y, &repY_);
  const bool agree =
      fitOk && repX_.rmsAfter <= touchcheck::kMaxRmsAfter && repY_.rmsAfter <= touchcheck::kMaxRmsAfter;
  nowErr_ = touchcheck::measure(input_.calibration(), sx_, sy_, count_);
  // The new table judged (and shown) on taps it wasn't fitted to: on its
  // own taps it always looks better (TouchCheck's measureUnseen()).
  const uint32_t t0 = millis();
  newErr_ = agree ? touchcheck::measureUnseen(input_.calibration(), sx_, sy_, count_) : touchcheck::Error{};
  const uint32_t unseenMs = millis() - t0;
  const touchcheck::Error own = agree ? touchcheck::measure(fitted_, sx_, sy_, count_) : touchcheck::Error{};
  outcome_ = touchcheck::outcome(agree, nowErr_, newErr_);
  static const char* const kOutcome[] = {"the taps don't agree", "already accurate", "no better", "better"};
  Serial.printf("[cal] fit %s (%s): x off by %.1f px rms (max %.1f) raw, %.1f (max %.1f) fitted; y %.1f (max %.1f) "
                "raw, %.1f (max %.1f) fitted; with the table in use up to %.0f px (average %.0f), with the new one "
                "up to %.0f (average %.0f) on taps it wasn't fitted to (a skewed panel's clamped readings on the "
                "fit; %lu ms), %.0f (average %.0f) on its own\n",
                !fitOk ? "FAILED" : agree ? "ok" : "rejected", kOutcome[static_cast<int>(outcome_)], repX_.rmsBefore,
                repX_.maxBefore, repX_.rmsAfter, repX_.maxAfter, repY_.rmsBefore, repY_.maxBefore, repY_.rmsAfter,
                repY_.maxAfter, nowErr_.max, nowErr_.mean, newErr_.max, newErr_.mean, (unsigned long)unseenMs, own.max,
                own.mean);
  page_ = Page::Result;
  drawResult();
}

void CalibrationScreen::drawResult() {
  using namespace uitext;
  using O = touchcheck::Outcome;
  clear();
  drawHeader(kCalTitle, nullptr, nullptr, 0);
  if (outcome_ == O::Disagree) {
    drawLine(52, kCalDisagree[0], col::AMBER);
    drawLine(74, kCalDisagree[1], col::AMBER);
    const Row rows[2] = {{kCalTryAgain, nullptr, Act::TryAgain, true}, {kCalDiscard, nullptr, Act::Discard, false}};
    setRows(rows, 2);
  } else {
    char line[64];
    touchcheck::errorText(kCalNow, nowErr_, line, sizeof(line));
    drawLine(42, line, col::TXT);
    touchcheck::errorText(kCalNew, newErr_, line, sizeof(line));
    drawLine(64, line, col::GREEN);
    if (outcome_ == O::Better) {
      drawLine(86, kCalSaveLine, col::DIM);
      const Row rows[3] = {{kCalSave, nullptr, Act::Save, true},
                           {kCalTryAgain, nullptr, Act::TryAgain, false},
                           {kCalDiscard, nullptr, Act::Discard, false}};
      setRows(rows, 3);
    } else {
      // A panel that reads true (or a table already good): a table fitted
      // to finger scatter helps nothing.
      drawLine(86, outcome_ == O::Accurate ? kCalAccurate : kCalNoBetter, col::SOFT);
      const Row rows[3] = {{kCalDiscard, nullptr, Act::Discard, true},
                           {kCalSave, nullptr, Act::Save, false},
                           {kCalTryAgain, nullptr, Act::TryAgain, false}};
      setRows(rows, 3);
    }
  }
  drawAHint(kCalDiscard);
  shown();
}

// ---- the check page ----

void CalibrationScreen::openCheck() {
  page_ = Page::Check;
  Serial.println("[cal] test taps: tap anywhere (A or Done: close)");
  drawCheckPage();
}

// (Its taps' marks draw on it too: a tap on the header shows.)
void CalibrationScreen::drawCheckHeader() {
  using namespace uitext;
  drawHeader(kCalCheckTitle, nullptr, justSaved_ ? kCalSaved : undone_ ? kCalUndone : nullptr,
             justSaved_ ? col::GREEN : col::DIM);
}

void CalibrationScreen::drawCheckPage() {
  using namespace uitext;
  clear();
  drawCheckHeader();
  drawLine(42, kCalCheckLines[0], col::SOFT);
  drawLine(62, kCalCheckLines[1], col::SOFT);
  const Row done[1] = {{kCalDone, nullptr, Act::Done, true}};
  setRows(done, 1);
  drawAHint(justSaved_ ? kCalUndo : kCalDone, input_.calibrated() ? kCalCheckKey : nullptr);
  checkTaps_ = 0;
  shown();
}

void CalibrationScreen::save() {
  before_ = input_.calibration();
  beforeCustom_ = input_.calibrated();
  if (!input_.setCalibration(fitted_)) {
    Serial.println("[cal] couldn't save the table (NVS): the one in use stays");
    close();
    return;
  }
  Serial.println("[cal] saved: the new table is in use (A on the check page undoes it)");
  input_.printStatus();
  justSaved_ = true;
  undone_ = false;
  openCheck();
}

// A on the check page right after a Save: the table before it again.
void CalibrationScreen::undo() {
  if (beforeCustom_) {
    if (!input_.setCalibration(before_)) Serial.println("[cal] couldn't save the table before (NVS)");
  } else {
    input_.resetCalibration();
  }
  Serial.printf("[cal] A: undone: %s again\n", beforeCustom_ ? "the table before" : "no correction");
  justSaved_ = false;
  undone_ = true;
  drawCheckPage();
}
