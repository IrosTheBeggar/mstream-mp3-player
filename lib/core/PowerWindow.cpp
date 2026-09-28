#include "PowerWindow.h"

namespace power {

namespace {
uint32_t u12(const uint8_t* b) { return (static_cast<uint32_t>(b[0]) << 4) | (b[1] & 0x0Fu); }
uint32_t u13(const uint8_t* b) { return (static_cast<uint32_t>(b[0]) << 5) | (b[1] & 0x1Fu); }
}  // namespace

Sample decode(uint8_t status, const uint8_t in[10], const uint8_t bat[8]) {
  Sample s;
  s.status = status;
  s.acinV = u12(in + 0) * 1.7f / 1000.0f;
  s.acinMa = u12(in + 2) * 0.625f;
  s.vbusV = u12(in + 4) * 1.7f / 1000.0f;
  s.vbusMa = u12(in + 6) * 0.375f;
  s.tempC = u12(in + 8) * 0.1f - 144.7f;
  s.batV = u12(bat + 0) * 1.1f / 1000.0f;
  const float charge = u13(bat + 2) * 0.5f;
  const float discharge = u13(bat + 4) * 0.5f;
  s.batMa = charge - discharge;
  s.apsV = u12(bat + 6) * 1.4f / 1000.0f;
  return s;
}

uint32_t adcHz(uint8_t reg84) { return 25u << ((reg84 >> 6) & 0x3u); }

double coulombMah(uint32_t count, uint32_t hz) {
  if (hz == 0) return 0.0;
  return 65536.0 * 0.5 * static_cast<double>(count) / 3600.0 / static_cast<double>(hz);
}

void Window::Acc::add(float v, bool first) {
  sum += v;
  if (first || v < min) min = v;
  if (first || v > max) max = v;
}

Stat Window::Acc::stat(uint32_t n) const {
  Stat s;
  if (n == 0) return s;
  s.mean = static_cast<float>(sum / n);
  s.min = min;
  s.max = max;
  return s;
}

void Window::reset(uint32_t startMs) {
  // The glitch filter carries over: a held sample is decided by the next.
  const bool haveLast = haveLast_, held = held_;
  const float lastInMa = lastInMa_;
  const Sample heldSample = heldSample_;
  *this = Window{};
  startMs_ = startMs;
  haveLast_ = haveLast;
  lastInMa_ = lastInMa;
  held_ = held;
  heldSample_ = heldSample;
}

namespace {
bool supplied(const Sample& s) { return (s.status & 0xA0) != 0; }  // ACIN or VBUS present
}  // namespace

void Window::add(const Sample& s) {
  if (held_) {
    held_ = false;
    if (s.inMa() > 2.0f * Window::kGlitchMa) {
      ++glitches_;  // back up at once: the held one was a glitch
    } else {
      accept(heldSample_);  // it stayed low: a real drop
    }
  }
  if (supplied(s) && s.inMa() <= kGlitchMa && haveLast_ && lastInMa_ > 2.0f * kGlitchMa) {
    held_ = true;
    heldSample_ = s;
    return;
  }
  accept(s);
}

void Window::accept(const Sample& s) {
  haveLast_ = true;
  lastInMa_ = s.inMa();
  const bool first = n_ == 0;
  inMa_.add(s.inMa(), first);
  inW_.add(s.inW(), first);
  batMa_.add(s.batMa, first);
  batW_.add(s.batW(), first);
  acinV_.add(s.acinV, first);
  acinMa_.add(s.acinMa, first);
  vbusV_.add(s.vbusV, first);
  vbusMa_.add(s.vbusMa, first);
  batV_.add(s.batV, first);
  apsV_.add(s.apsV, first);
  tempC_.add(s.tempC, first);
  status_ |= s.status;
  ++n_;
}

}  // namespace power
