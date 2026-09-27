#include "ui/Input.h"

#include <M5Unified.h>
#include <Preferences.h>

#include "ButtonPolicy.h"

namespace {

constexpr const char* kNvsNamespace = "input";
constexpr const char* kKeyCal = "cal";
constexpr const char* kKeyHaptics = "haptics";
constexpr const char* kKeyRailTick = "railtick";

m5::Button_Class& button(int b) { return b == 0 ? M5.BtnA : b == 1 ? M5.BtnB : M5.BtnC; }

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
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error.
  if (!p.begin(kNvsNamespace, false)) {
    Serial.println("[input] NVS unavailable: the default touch table, haptics on");
    return;
  }
  hapticsOn_ = p.getBool(kKeyHaptics, true);
  railTicks_ = p.getBool(kKeyRailTick, true);
  uint8_t blob[TouchCalibration::kMaxBlob];
  const size_t len = p.isKey(kKeyCal) ? p.getBytesLength(kKeyCal) : 0;
  if (len > 0 && len <= sizeof(blob) && p.getBytes(kKeyCal, blob, len) == len) {
    TouchCalibration c;
    if (c.load(blob, len)) {
      cal_ = c;
      custom_ = true;
    } else {
      Serial.println("[input] the saved touch calibration is damaged: the default table instead");
    }
  }
  p.end();
  Serial.printf("[input] touch table: %s; haptics %s, rail ticks %s\n", custom_ ? "calibrated" : "default",
                hapticsOn_ ? "on" : "off", railTicks_ ? "on" : "off");
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

// The buttons' feedback: every click and hold does something (ButtonPolicy).
// The glass's is played by whoever acts on the touch (tapTick, holdTick).
void Input::feedback(const InputEvent& e) {
  if (!hapticsOn_) return;
  using T = InputEvent::Type;
  switch (e.type) {
    case T::Click:
      haptics_.tap();
      break;
    case T::Hold:
      haptics_.doubleTick();
      break;
    default:
      break;
  }
}

void Input::tapTick() {
  if (hapticsOn_) haptics_.tap();
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
  // The buttons (M5Unified makes them from panel points at y >= 240).
  for (int b = 0; b < 3; ++b) {
    const ButtonGesture::Event g = buttons_[b].update(nowMs, button(b).isPressed());
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
    const uint32_t r = buttons_[b].repeats();
    e.repeat = static_cast<uint8_t>(r > 255 ? 255 : r);
    if (suspended_) continue;
    feedback(e);
    push(e);
  }

  // The glass: the first touch point, as M5Unified converts it (screen
  // pixels, what the input lab logged as "raw"), then corrected.
  TouchRecognizer::Sample s;
  if (M5.Touch.getCount() > 0 && M5.Touch.getDetail(0).isPressed()) {
    m5gfx::touch_point_t tp = M5.Touch.getTouchPointRaw(0);
    M5.Display.convertRawXY(&tp, 1);
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
  }
  InputEvent out[TouchRecognizer::kMaxEvents];
  const int n = glass_.update(nowMs, s, out);
  if (suspended_) return;
  for (int i = 0; i < n; ++i) push(out[i]);
}

void Input::simulate(int x0, int y0, int x1, int y1, uint32_t dwellMs, uint32_t moveMs, uint32_t restMs) {
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
  s.x = s.rawX = static_cast<int16_t>(x);
  s.y = s.rawY = static_cast<int16_t>(y);
  if (x >= 319) s.edges |= InputEvent::kEdgeRight;
  if (x <= 0) s.edges |= InputEvent::kEdgeLeft;
  return true;
}

void Input::cancelTouch(uint32_t nowMs) {
  const InputEvent e = glass_.cancel(nowMs);
  if (e.type != InputEvent::Type::None && !suspended_) push(e);
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

void Input::printStatus() const {
  Serial.printf("[input] touch table: %s (raw -> screen px)\n",
                custom_ ? "calibrated on this device" : "the default, fitted to the input lab's logs");
  printAxis("x", cal_.x);
  printAxis("y", cal_.y);
  Serial.printf("[input] haptics %s (tap %u ms at %u, hold: double tick), rail ticks %s; buttons: hold %lu ms, "
                "A/C repeat every %lu ms; flings capped at %.0f px/s; events dropped %lu\n",
                hapticsOn_ ? "on" : "off", Haptics::kTapMs, Haptics::kTapLevel, railTicks_ ? "on" : "off",
                (unsigned long)ButtonPolicy::kHoldMs, (unsigned long)ButtonPolicy::kVolumeRepeatMs,
                glass_.config().maxFlingPxPerS, (unsigned long)dropped_);
}
