// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/Input.h"

#include <M5Unified.h>
#include <Preferences.h>

#include <cmath>

#include "ButtonPolicy.h"

namespace {

constexpr const char* kNvsNamespace = "input";
constexpr const char* kKeyCal = "cal";
constexpr const char* kKeyCalAsk = "cal_ask";
constexpr const char* kKeyHaptics = "haptics";
constexpr const char* kKeyRailTick = "railtick";

void printAxis(const char* name, const TouchCalibration::Axis& a) {
  char line[200];
  int n = snprintf(line, sizeof(line), "[input] touch %s:", name);
  for (int i = 0; i < a.n && n < static_cast<int>(sizeof(line)) - 16; ++i) {
    n += snprintf(line + n, sizeof(line) - n, " %d->%.1f", a.raw[i], a.value[i]);
  }
  Serial.println(line);
}

}  // namespace

void Input::begin() {
  for (int b = 0; b < 3; ++b) buttons_[b].setConfig(ButtonPolicy::gestureFor(b));
  StripButtons::Config sc = strip_.config();
  sc.holdMs = ButtonPolicy::kHoldMs;  // a press that has held never scrolls
  strip_.setConfig(sc);
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error.
  if (!p.begin(kNvsNamespace, false)) {
    Serial.println("[input] NVS unavailable: no touch correction, haptics on");
    return;
  }
  hapticsOn_ = p.getBool(kKeyHaptics, true);
  railTicks_ = p.getBool(kKeyRailTick, true);
  checkAnswered_ = p.getBool(kKeyCalAsk, false);
  uint8_t blob[TouchCalibration::kMaxBlob];
  const size_t len = p.isKey(kKeyCal) ? p.getBytesLength(kKeyCal) : 0;
  if (len > 0 && len <= sizeof(blob) && p.getBytes(kKeyCal, blob, len) == len) {
    TouchCalibration c;
    if (c.load(blob, len)) {
      cal_ = c;
      custom_ = true;
    } else {
      Serial.println("[input] the saved touch calibration is damaged: no correction instead");
    }
  }
  p.end();
  Serial.printf("[input] touch: %s%s; haptics %s, rail ticks %s\n",
                custom_ ? "calibrated on this device" : "no correction (default)",
                custom_ || checkAnswered_ ? "" : ", the touch check is due", hapticsOn_ ? "on" : "off",
                railTicks_ ? "on" : "off");
}

void Input::push(const InputEvent& e) {
  if (count_ == kQueue) {  // the oldest goes: the loop is far behind anyway
    head_ = static_cast<uint8_t>((head_ + 1) % kQueue);
    --count_;
    ++dropped_;
  }
  queue_[(head_ + count_) % kQueue] = e;
  ++count_;
}

bool Input::poll(InputEvent& e) {
  if (count_ == 0) return false;
  e = queue_[head_];
  head_ = static_cast<uint8_t>((head_ + 1) % kQueue);
  --count_;
  return true;
}

// The buttons' feedback, once ButtonPolicy has acted on the event. The
// glass's is played by whoever acts on the touch (tapTick, holdTick).
void Input::buttonFeedback(const InputEvent& e, bool acted) {
  if (!hapticsOn_) return;
  using T = InputEvent::Type;
  switch (e.type) {
    case T::Click:
      if (acted) {
        haptics_.tap();
      } else {
        missBuzz();
      }
      break;
    case T::Hold:
      haptics_.doubleTick();
      break;
    default:
      break;
  }
}

void Input::connectedTick() {
  if (hapticsOn_) haptics_.doubleTick();
}

void Input::alertBuzz() {
  if (hapticsOn_) haptics_.tick(80, Haptics::kTapLevel);
}

void Input::tapTick() {
  if (hapticsOn_) haptics_.tap();
}

// Inert: two short, softer pulses, quicker than the hold's.
void Input::missBuzz() {
  if (hapticsOn_) haptics_.pulses(20, Haptics::kMedium, 2, 50);
}

void Input::holdTick() {
  holdUsed_ = true;
  if (hapticsOn_) haptics_.doubleTick();
}

bool Input::takeHoldUsed() {
  const bool used = holdUsed_;
  holdUsed_ = false;
  return used;
}

void Input::update(uint32_t nowMs) {
  // The touch point: the first one, as M5Unified converts it (screen
  // pixels, what the input lab logged as "raw"), then corrected; or the
  // scripted finger's.
  TouchRecognizer::Sample s;
  int id = -1;
  // Any real finger on the panel (a second one too): while one is, the
  // wake's latch holds. The scripted finger doesn't count.
  bool fingers = false;
  int16_t firstRawX = 0, firstRawY = 0;
  for (int i = 0; i < M5.Touch.getCount(); ++i) {
    const auto& d = M5.Touch.getDetail(i);
    if (!d.isPressed()) continue;
    if (!fingers) {
      firstRawX = d.x;
      firstRawY = d.y;
    }
    fingers = true;
  }
  const WakeLatch::Result w = latch_.update(nowMs, fingers, lit_);
  if (w.took && !suspended_) {
    // A finger resting as the screen dimmed: taken, not a wake. Its touch
    // ends here for the page (nothing more comes of it until it lifts).
    const InputEvent c = glass_.cancel(nowMs);
    if (c.type != InputEvent::Type::None) push(c);
  }
  if (w.woke) {
    woke_ = true;
    wakeX_ = firstRawX;
    wakeY_ = firstRawY;
  }
  drop_ = suspended_ || w.hold;
  if (M5.Touch.getCount() > 0 && M5.Touch.getDetail(0).isPressed()) {
    m5gfx::touch_point_t tp = M5.Touch.getTouchPointRaw(0);
    M5.Display.convertRawXY(&tp, 1);
    id = tp.id;
    s.pressed = true;
    s.rawX = tp.x;
    s.rawY = tp.y;
    s.x = static_cast<int16_t>(cal_.mapX(tp.x));
    s.y = static_cast<int16_t>(cal_.mapY(tp.y));
    if (TouchCalibration::clampedLow(tp.x)) s.edges |= InputEvent::kEdgeLeft;
    if (TouchCalibration::clampedHighX(tp.x)) s.edges |= InputEvent::kEdgeRight;
    sim_.on = false;  // a real finger wins
    scripted_ = false;
  } else if (sim_.on && simSample(nowMs, s)) {
    scripted_ = true;
    id = kScriptedId;
  }
  // The first point is another finger than last pass's, with no lift
  // between (the first lifted while a second stayed on).
  const bool newTouch = id >= 0 && touchId_ >= 0 && id != touchId_;
  touchId_ = id;
  if (id >= 0 && id != kScriptedId) {
    logOtherFingers(id);
  } else {
    otherLogged_ = 0;
  }

  // The buttons, from the same point (a touch that went down in the strip;
  // a swipe up from there is handed to the glass).
  updateButtons(nowMs, s, newTouch);

  // The glass (a touch that went down in the strip is the buttons', until
  // it is handed over: a drag from this very point).
  InputEvent out[TouchRecognizer::kMaxEvents];
  const int n = glass_.update(nowMs, s, out);
  // Input for the screen's countdown: a finger that acts, landing or
  // moving (one resting still stops counting: FingerActivity).
  touching_ = still_.update(nowMs, s.pressed && !w.hold, s.rawX, s.rawY);
  glassLanded_ = false;
  if (drop_) return;
  for (int i = 0; i < n; ++i) {
    // A touch landing on the glass: the listener is looking (ends the
    // screen's unattended state; a swipe from the strip isn't one).
    if (out[i].type == InputEvent::Type::Down && !out[i].fromStrip) glassLanded_ = true;
    push(out[i]);
  }
}

bool Input::takeWake(int* rawX, int* rawY) {
  if (!woke_) return false;
  woke_ = false;
  *rawX = wakeX_;
  *rawY = wakeY_;
  return true;
}

// Only the first touch point is followed (StripButtons, like the glass): a
// strip press by any other finger does nothing. Logged once per finger, so a
// press that "didn't work" shows in the log.
void Input::logOtherFingers(int firstId) {
  uint8_t seen = 0;
  for (int i = 0; i < M5.Touch.getCount(); ++i) {
    const auto& d = M5.Touch.getDetail(i);
    if (!d.isPressed() || d.id == firstId || d.id >= 8) continue;
    const uint8_t bit = static_cast<uint8_t>(1u << d.id);
    seen |= bit;
    // base: where that finger went down (screen pixels, as the first point's).
    if ((otherLogged_ & bit) || d.base_y < strip_.config().stripY) continue;
    otherLogged_ |= bit;
    if (drop_) continue;
    Serial.printf("[button] ignored: %c at %d,%d (raw): a second finger (only the first touch counts)\n",
                  static_cast<char>('A' + StripButtons::column(d.base_x)), d.base_x, d.base_y);
  }
  otherLogged_ &= seen;
}

// StripButtons says which button is down (if any), whether the press in
// progress was dropped, and whether the touch is a swipe up from the strip
// (the glass follows it from here: it scrolls); ButtonGesture makes the
// events. A strip touch that doesn't count is logged, once per touch, and
// so is a swipe handed over.
void Input::updateButtons(uint32_t nowMs, const TouchRecognizer::Sample& s, bool newTouch) {
  const StripButtons::Result r = strip_.update(nowMs, s.pressed, s.rawX, s.rawY, newTouch);
  if (r.scroll) {
    glass_.fromStrip();
    if (!drop_) {
      Serial.printf("[button] %c at %d,%d (raw): a swipe from the strip (%d px): scrolling%s\n",
                    static_cast<char>('A' + r.button), s.rawX, s.rawY, r.moved, scripted_ ? " (scripted)" : "");
    }
  }
  if (r.ignored != StripButtons::Why::None && !drop_) {
    char detail[80] = "";
    if (r.ignored == StripButtons::Why::Moved) {
      snprintf(detail, sizeof(detail), " (%d px): cancelled", r.moved);
    } else if (r.ignored == StripButtons::Why::Bounce) {
      snprintf(detail, sizeof(detail), " (%lu ms, %d px from where it lifted%s)", (unsigned long)r.sinceLiftMs,
               r.fromLiftPx, r.afterSwipe ? ", after a swipe" : "");
    }
    Serial.printf("[button] ignored: %c at %d,%d (raw): %s%s%s%s\n", static_cast<char>('A' + r.button), s.rawX,
                  s.rawY, StripButtons::name(r.ignored), detail, newTouch ? " (another finger took over)" : "",
                  scripted_ ? " (scripted)" : "");
  }
  for (int b = 0; b < 3; ++b) {
    // A press dropped (it moved, or it became a swipe): no click or hold
    // from it (a hold it had ends).
    const ButtonGesture::Event g =
        r.cancelled && r.button == b ? buttons_[b].cancel() : buttons_[b].update(nowMs, r.pressed == b);
    InputEvent e;
    switch (g) {
      case ButtonGesture::Event::Click: e.type = InputEvent::Type::Click; break;
      case ButtonGesture::Event::Hold: e.type = InputEvent::Type::Hold; break;
      case ButtonGesture::Event::Repeat: e.type = InputEvent::Type::Repeat; break;
      case ButtonGesture::Event::HoldEnd: e.type = InputEvent::Type::HoldEnd; break;
      default: continue;
    }
    e.button = static_cast<uint8_t>(b);
    e.ms = nowMs;
    const uint32_t rp = buttons_[b].repeats();
    e.repeat = static_cast<uint8_t>(rp > 255 ? 255 : rp);
    if (drop_) continue;
    push(e);  // its feedback once it's handled (buttonFeedback())
  }
}

void Input::simulate(int x0, int y0, int x1, int y1, uint32_t dwellMs, uint32_t moveMs, uint32_t restMs) {
  // uk2's jitter: a fixed walk, so a run can be repeated.
  static const int8_t kJitter[] = {3, -2, 4, -4, 1, -3, 2, 0, -1, 4, -2, 3, -4, 1};
  constexpr int kJitterN = sizeof(kJitter);
  jitterX_ = simSkew_ == 2 ? kJitter[simTouches_ % kJitterN] : 0;
  jitterY_ = simSkew_ == 2 ? kJitter[(simTouches_ + 5) % kJitterN] : 0;
  ++simTouches_;
  sim_ = Sim{};
  sim_.on = true;
  sim_.x0 = static_cast<int16_t>(x0);
  sim_.y0 = static_cast<int16_t>(y0);
  sim_.x1 = static_cast<int16_t>(x1);
  sim_.y1 = static_cast<int16_t>(y1);
  sim_.dwell = dwellMs;
  sim_.move = moveMs;
  sim_.rest = restMs;
}

// The scripted finger's point at nowMs; false (and off) once it has lifted.
// The first sample only lands it (its clock starts there), so the recogniser
// always sees the Down before any move.
bool Input::simSample(uint32_t nowMs, TouchRecognizer::Sample& s) {
  if (!sim_.started) {
    sim_.started = true;
    sim_.t0 = nowMs;
  }
  const uint32_t t = nowMs - sim_.t0;
  int x = sim_.x0, y = sim_.y0;
  if (t >= sim_.dwell + sim_.move + sim_.rest) {
    sim_.on = false;
    return false;  // lifted
  }
  if (t >= sim_.dwell + sim_.move) {
    x = sim_.x1;
    y = sim_.y1;
  } else if (t > sim_.dwell && sim_.move > 0) {
    const float f = static_cast<float>(t - sim_.dwell) / static_cast<float>(sim_.move);
    x = sim_.x0 + static_cast<int>((sim_.x1 - sim_.x0) * f);
    y = sim_.y0 + static_cast<int>((sim_.y1 - sim_.y0) * f);
  }
  s.pressed = true;
  if (simSkew_ == 0) {
    s.x = s.rawX = static_cast<int16_t>(x);
    s.y = s.rawY = static_cast<int16_t>(y);
    if (x >= 319) s.edges |= InputEvent::kEdgeRight;
    if (x <= 0) s.edges |= InputEvent::kEdgeLeft;
    return true;
  }
  // A skewed panel: what the lab's panel reads for a finger at (x, y) (y
  // reads true), then the same path as a real touch's.
  const TouchCalibration::Axis lab = TouchCalibration::labFitX();
  long rx = std::lround(lab.unmap(static_cast<float>(x))) + jitterX_;
  long ry = static_cast<long>(y) + jitterY_;
  rx = rx < 0 ? 0 : rx > TouchCalibration::kRawMaxX ? TouchCalibration::kRawMaxX : rx;
  ry = ry < 0 ? 0 : ry > TouchCalibration::kRawMaxY ? TouchCalibration::kRawMaxY : ry;
  s.rawX = static_cast<int16_t>(rx);
  s.rawY = static_cast<int16_t>(ry);
  s.x = static_cast<int16_t>(cal_.mapX(s.rawX));
  s.y = static_cast<int16_t>(cal_.mapY(s.rawY));
  if (TouchCalibration::clampedLow(s.rawX)) s.edges |= InputEvent::kEdgeLeft;
  if (TouchCalibration::clampedHighX(s.rawX)) s.edges |= InputEvent::kEdgeRight;
  return true;
}

const char* Input::scriptedNote() const {
  if (!scripted_) return "";
  return simSkew_ ? " (scripted, skewed)" : " scripted";
}

void Input::cancelTouch(uint32_t nowMs) {
  const InputEvent e = glass_.cancel(nowMs);
  if (e.type != InputEvent::Type::None && !suspended_ && !latch_.holding()) push(e);
}

void Input::saveFlag(const char* key, bool on) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putBool(key, on);
  p.end();
}

void Input::setHapticsOn(bool on) {
  hapticsOn_ = on;
  saveFlag(kKeyHaptics, on);
}

void Input::setRailTicksOn(bool on) {
  railTicks_ = on;
  saveFlag(kKeyRailTick, on);
}

void Input::railTick() {
  if (hapticsOn_ && railTicks_) haptics_.tap();
}

bool Input::setCalibration(const TouchCalibration& c) {
  uint8_t blob[TouchCalibration::kMaxBlob];
  const size_t len = c.save(blob, sizeof(blob));
  if (len == 0) return false;
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return false;
  const bool ok = p.putBytes(kKeyCal, blob, len) == len;
  p.end();
  if (!ok) return false;
  cal_ = c;
  custom_ = true;
  return true;
}

void Input::resetCalibration() {
  Preferences p;
  if (p.begin(kNvsNamespace, false)) {
    p.remove(kKeyCal);
    p.end();
  }
  cal_ = TouchCalibration::defaults();
  custom_ = false;
}

void Input::setTouchCheckAnswered(bool on) {
  checkAnswered_ = on;
  saveFlag(kKeyCalAsk, on);
}

void Input::printStatus() const {
  Serial.printf("[input] touch table: %s (raw -> screen px); the first-boot touch check %s%s\n",
                custom_ ? "calibrated on this device" : "no correction (default)",
                checkAnswered_ ? "answered" : custom_ ? "not needed" : "due at the next boot",
                simSkew_ == 2 ? "; the scripted finger is skewed, with jitter (uk2)"
                : simSkew_    ? "; the scripted finger is skewed (uk1)"
                              : "");
  printAxis("x", cal_.x);
  printAxis("y", cal_.y);
  Serial.printf("[input] haptics %s (tap %u ms at %u, hold: double tick), rail ticks %s; buttons: hold %lu ms, "
                "A/C repeat every %lu ms; flings capped at %.0f px/s; events dropped %lu\n",
                hapticsOn_ ? "on" : "off", Haptics::kTapMs, Haptics::kTapLevel, railTicks_ ? "on" : "off",
                (unsigned long)ButtonPolicy::kHoldMs, (unsigned long)ButtonPolicy::kVolumeRepeatMs,
                glass_.config().maxFlingPxPerS, (unsigned long)dropped_);
}
