// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RateConverter.h"

#include <cstdio>   // std::snprintf
#include <cstring>  // std::memset, std::memcpy

#if defined(__GNUC__)
#define RC_INLINE inline __attribute__((always_inline))
#else
#define RC_INLINE inline
#endif

namespace {

enum : uint8_t { kNone = 0, kPoly = 1, kHalfband = 2 };

struct Route {
  uint32_t hz;
  uint8_t stageA;  // kNone (passthrough), kPoly, kHalfband
  uint8_t upL;     // a U12 stage's ratio upL/upM; 0: stage A is 147/160 itself
  uint8_t upM;
  bool thenD147;   // stage A's output (48 kHz) goes through 147/160
  bool hiRes;      // needs kHiResMinMhz
  const char* text;
};

constexpr Route kRoutes[] = {
    {44100, kNone, 0, 0, false, false, "passthrough"},
    {48000, kPoly, 0, 0, false, false, "147/160"},
    {96000, kHalfband, 0, 0, true, true, "halfband /2, then 147/160"},
    {88200, kHalfband, 0, 0, false, true, "halfband /2"},
    {32000, kPoly, 3, 2, true, false, "x3/2 to 48 kHz, then 147/160"},
    {24000, kPoly, 2, 1, true, false, "x2 to 48 kHz, then 147/160"},
    {22050, kPoly, 2, 1, false, false, "x2"},
    {16000, kPoly, 3, 1, true, false, "x3 to 48 kHz, then 147/160"},
    {12000, kPoly, 4, 1, true, false, "x4 to 48 kHz, then 147/160"},
    {11025, kPoly, 4, 1, false, false, "x4"},
    {8000, kPoly, 6, 1, true, false, "x6 to 48 kHz, then 147/160"},
};
constexpr uint8_t kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);

constexpr uint32_t kHalfTaps = resampler::kTaps / 2;
static_assert(resampler::kTaps == 48, "the kernel below is unrolled for two halves of 24 taps");

uint32_t gcd(uint32_t a, uint32_t b) {
  while (b != 0) {
    const uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

uint32_t ceilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// The kernel. Two int32 sums per output, one per contiguous half of the row
// (each half's sum of |c| is under 65536, so neither can overflow), added in
// 64 bits. Unrolled by 8 with constant offsets: GCC for the ESP32 then keeps
// each sum in MAC16's accumulator, at l16ui + l16ui + mula.aa.ll per tap
// (docs/RESAMPLER.md, "CPU"). Planar histories: one channel per call.

// sum c[k] * x[k], k = 0..23
RC_INLINE int32_t half24(const int16_t* c, const int16_t* x) {
  int32_t a = 0;
  for (uint32_t k = 0; k < kHalfTaps; k += 8) {
    a += c[k] * x[k];
    a += c[k + 1] * x[k + 1];
    a += c[k + 2] * x[k + 2];
    a += c[k + 3] * x[k + 3];
    a += c[k + 4] * x[k + 4];
    a += c[k + 5] * x[k + 5];
    a += c[k + 6] * x[k + 6];
    a += c[k + 7] * x[k + 7];
  }
  return a;
}

// The same with the row read backwards: `c` points at the half's last tap.
RC_INLINE int32_t half24rev(const int16_t* c, const int16_t* x) {
  int32_t a = 0;
  for (uint32_t k = 0; k < kHalfTaps; k += 8) {
    const int16_t* r = c - k;
    a += r[0] * x[k];
    a += r[-1] * x[k + 1];
    a += r[-2] * x[k + 2];
    a += r[-3] * x[k + 3];
    a += r[-4] * x[k + 4];
    a += r[-5] * x[k + 5];
    a += r[-6] * x[k + 6];
    a += r[-7] * x[k + 7];
  }
  return a;
}

// A polyphase row against a 48-sample window, oldest first. Rows past the
// table's middle are stored rows read backwards (row L - p = row p reversed).
RC_INLINE int64_t rowSum(const int16_t* row, bool reversed, const int16_t* x) {
  if (!reversed) return static_cast<int64_t>(half24(row, x)) + half24(row + kHalfTaps, x + kHalfTaps);
  const int16_t* last = row + resampler::kTaps - 1;
  return static_cast<int64_t>(half24rev(last, x)) + half24rev(last - kHalfTaps, x + kHalfTaps);
}

// A halfband against an N-sample window, oldest first: the centre (16384)
// and the side taps at odd distances, which sit at the window's even
// positions. Two int32 sums again, the left side with the centre and the
// right side, each under 65536 x 32768.
template <int N>
RC_INLINE int64_t halfbandSum(const int16_t* side, const int16_t* x) {
  constexpr int kC = (N - 1) / 2;
  constexpr int kS = (N + 1) / 4;
  int32_t a = 16384 * x[kC];
  for (int i = 0; i < kS; ++i) a += side[i] * x[2 * i];
  int32_t b = 0;
  for (int j = 0; j < kS; ++j) b += side[kS - 1 - j] * x[kC + 1 + 2 * j];
  return static_cast<int64_t>(a) + b;
}

// A Q15 sum to int16: rounded half up, then saturated, never wrapped.
RC_INLINE int16_t toSample(int64_t sum, uint32_t& clamped) {
  const int64_t v = (sum + 16384) >> 15;
  if (v > 32767) {
    ++clamped;
    return 32767;
  }
  if (v < -32768) {
    ++clamped;
    return -32768;
  }
  return static_cast<int16_t>(v);
}

}  // namespace

RateConverter::Plan RateConverter::plan(uint32_t srcHz, uint32_t cpuMhz, bool hiRes) {
  Plan p;
  uint8_t i = 0;
  while (i < kRouteCount && kRoutes[i].hz != srcHz) ++i;
  if (i == kRouteCount) {
    p.refusal = Refusal::Unsupported;
    p.reason = "isn't supported (8-48 kHz, 88.2 and 96 kHz)";
    return p;
  }
  const Route& rt = kRoutes[i];
  if (rt.hiRes && !hiRes) {
    p.refusal = Refusal::Off;
    p.reason = "is off in this build (88.2/96 kHz wait for the device check: MSTREAM_HIRES_RATES)";
    return p;
  }
  if (rt.hiRes && cpuMhz < kHiResMinMhz) {
    p.refusal = Refusal::NeedsCpu;
    p.reason = "needs the 240 MHz CPU speed";
    return p;
  }
  uint32_t num = 1, den = 1;
  if (rt.stageA == kHalfband) {
    den = 2;
  } else if (rt.stageA == kPoly) {
    num = rt.upL == 0 ? resampler::kD147Rows : rt.upL;
    den = rt.upL == 0 ? 160 : rt.upM;
  }
  if (rt.thenD147) {
    num *= resampler::kD147Rows;
    den *= 160;
  }
  const uint32_t g = gcd(num, den);
  p.ok = true;
  p.route = rt.text;
  p.num = num / g;
  p.den = den / g;
  p.index = i;
  return p;
}

void RateConverter::rateText(uint32_t hz, char* out, uint32_t size) {
  if (size == 0) return;
  if (hz % 1000 == 0) {
    std::snprintf(out, size, "%lu kHz", static_cast<unsigned long>(hz / 1000));
  } else if (hz % 100 == 0) {
    std::snprintf(out, size, "%lu.%lu kHz", static_cast<unsigned long>(hz / 1000),
                  static_cast<unsigned long>(hz % 1000 / 100));
  } else {
    std::snprintf(out, size, "%lu Hz", static_cast<unsigned long>(hz));  // (44056 Hz is no 44.1 kHz)
  }
}

void RateConverter::reset() {
  state_ = State::Unconfigured;
  plan_ = Plan{};
  rate_ = 0;
  mono_ = false;
  stageA_ = kNone;
  thenD147_ = false;
  routeMaxOut_ = 1;
  pendingN_ = 0;
  carryN_ = 0;
  clamped_ = 0;
  restartFilters();
}

void RateConverter::restartFilters() {
  std::memset(histA_, 0, sizeof(histA_));
  std::memset(histB_, 0, sizeof(histB_));
  polyA_.phase = polyA_.w = 0;
  polyA_.lead = kTaps / 2;
  polyB_.phase = polyB_.w = 0;
  polyB_.lead = kTaps / 2;
  hb_.w = 0;
  hb_.lead = static_cast<uint16_t>((hb_.taps - 1) / 2);
  hb_.skip = false;
  taken_ = 0;
  produced_ = 0;
}

bool RateConverter::setRate(uint32_t hz, uint32_t cpuMhz, bool hiRes) {
  if (hz == 0) return state_ != State::Refused;
  if (state_ == State::Configured && hz == rate_) return true;  // a FLAC seek, a reopen: nothing changes
  const Plan p = plan(hz, cpuMhz, hiRes);
  rate_ = hz;
  if (!p.ok) {
    state_ = State::Refused;
    plan_ = p;
    pendingN_ = 0;
    carryN_ = 0;
    return false;
  }
  carryN_ = 0;  // a rate change mid-stream: the old rate's frames still waiting go
  configure(p);
  return true;
}

void RateConverter::configure(const Plan& p) {
  const Route& rt = kRoutes[p.index];
  plan_ = p;
  state_ = State::Configured;
  stageA_ = rt.stageA;
  thenD147_ = rt.thenD147;
  if (rt.stageA == kPoly && rt.upL == 0) {
    polyA_.rows = resampler::kD147;
    polyA_.tableRows = resampler::kD147Rows;
    polyA_.step = 1;
    polyA_.L = resampler::kD147Rows;
    polyA_.M = 160;
  } else if (rt.stageA == kPoly) {
    polyA_.rows = resampler::kU12;
    polyA_.tableRows = resampler::kU12Rows;
    polyA_.step = static_cast<uint16_t>(resampler::kU12Rows / rt.upL);
    polyA_.L = rt.upL;
    polyA_.M = rt.upM;
  }
  if (rt.stageA == kHalfband) {
    const bool is96 = rt.hz == 96000;
    hb_.side = is96 ? resampler::kHb96Side : resampler::kHb88Side;
    hb_.taps = static_cast<uint16_t>(is96 ? resampler::kHb96Taps : resampler::kHb88Taps);
  }
  polyB_.rows = resampler::kD147;
  polyB_.tableRows = resampler::kD147Rows;
  polyB_.step = 1;
  polyB_.L = resampler::kD147Rows;
  polyB_.M = 160;

  // The most frames one source frame can make: ceil(n * L / M), stage by stage.
  uint32_t m = 1;
  if (rt.stageA == kPoly) m = ceilDiv(m * polyA_.L, polyA_.M);
  if (rt.stageA == kHalfband) m = ceilDiv(m, 2);
  if (rt.thenD147) m = ceilDiv(m * resampler::kD147Rows, 160);
  routeMaxOut_ = static_cast<uint8_t>(m);

  restartFilters();
  // The frames handed over before the rate was known, replayed. A converting
  // route's delay (24 frames or more) is longer than the hold, so they only
  // fill its history; at 44.1 kHz they wait in carry_ for the next push().
  const uint8_t n = pendingN_;
  pendingN_ = 0;
  carryN_ = 0;
  int16_t out[kMaxOut * 2];
  for (uint8_t i = 0; i < n; ++i) {
    const uint32_t k = runRoute(pending_[i][0], pending_[i][1], out);
    for (uint32_t j = 0; j < k && carryN_ < kMaxPending; ++j, ++carryN_) {
      carry_[carryN_][0] = out[2 * j];
      carry_[carryN_][1] = out[2 * j + 1];
    }
    ++taken_;
  }
}

bool RateConverter::passthrough() const { return state_ == State::Configured && stageA_ == kNone; }

uint32_t RateConverter::maxOut() const {
  switch (state_) {
    case State::Refused:
      return 0;
    case State::Unconfigured:  // a push() that fills the hold, or finishPush(), plays it at 44.1 kHz
      return pendingN_ + 1u;
    case State::Configured:
    default:
      return carryN_ + routeMaxOut_;
  }
}

uint32_t RateConverter::pushPoly(PolyStage& s, int16_t* h0, int16_t* h1, int16_t l, int16_t r, int16_t* out) {
  h0[s.w] = h0[s.w + kTaps] = l;
  h1[s.w] = h1[s.w + kTaps] = r;
  if (++s.w == kTaps) s.w = 0;
  if (s.lead != 0) {
    --s.lead;
    return 0;
  }
  const int16_t* x0 = h0 + s.w;  // the last 48 inputs, oldest first
  const int16_t* x1 = h1 + s.w;
  const uint32_t middle = s.tableRows / 2u;
  uint32_t n = 0;
  while (s.phase < s.L) {
    const uint32_t row = static_cast<uint32_t>(s.phase) * s.step;
    const bool reversed = row > middle;
    const int16_t* c = s.rows[reversed ? s.tableRows - row : row];
    out[2 * n] = toSample(rowSum(c, reversed, x0), clamped_);
    out[2 * n + 1] = mono_ ? out[2 * n] : toSample(rowSum(c, reversed, x1), clamped_);
    ++n;
    s.phase = static_cast<uint16_t>(s.phase + s.M);
  }
  s.phase = static_cast<uint16_t>(s.phase - s.L);
  return n;
}

uint32_t RateConverter::pushHalfband(int16_t l, int16_t r, int16_t* out) {
  HalfbandStage& s = hb_;
  histA_[0][s.w] = histA_[0][s.w + s.taps] = l;
  histA_[1][s.w] = histA_[1][s.w + s.taps] = r;
  if (++s.w == s.taps) s.w = 0;
  if (s.lead != 0) {
    --s.lead;
    return 0;
  }
  s.skip = !s.skip;  // one output per two inputs, centred on the even ones
  if (!s.skip) return 0;
  const int16_t* x0 = histA_[0] + s.w;
  const int16_t* x1 = histA_[1] + s.w;
  if (s.taps == resampler::kHb96Taps) {
    out[0] = toSample(halfbandSum<resampler::kHb96Taps>(s.side, x0), clamped_);
    out[1] = mono_ ? out[0] : toSample(halfbandSum<resampler::kHb96Taps>(s.side, x1), clamped_);
  } else {
    out[0] = toSample(halfbandSum<resampler::kHb88Taps>(s.side, x0), clamped_);
    out[1] = mono_ ? out[0] : toSample(halfbandSum<resampler::kHb88Taps>(s.side, x1), clamped_);
  }
  return 1;
}

// One source frame through the route (in mono, r is already l).
uint32_t RateConverter::runRoute(int16_t l, int16_t r, int16_t* out) {
  int16_t mid[kMaxOut * 2];
  uint32_t n = 0;
  switch (stageA_) {
    case kPoly:
      if (!thenD147_) return pushPoly(polyA_, histA_[0], histA_[1], l, r, out);
      n = pushPoly(polyA_, histA_[0], histA_[1], l, r, mid);
      break;
    case kHalfband:
      if (!thenD147_) return pushHalfband(l, r, out);
      n = pushHalfband(l, r, mid);
      break;
    case kNone:
    default:
      out[0] = l;
      out[1] = r;
      return 1;
  }
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; ++i) k += pushPoly(polyB_, histB_[0], histB_[1], mid[2 * i], mid[2 * i + 1], out + 2 * k);
  return k;
}

uint32_t RateConverter::drainCarry(int16_t* out) {
  const uint32_t n = carryN_;
  if (n != 0) std::memcpy(out, carry_, n * 2 * sizeof(int16_t));
  carryN_ = 0;
  return n;
}

uint32_t RateConverter::push(const int16_t in[2], int16_t* out) {
  const int16_t l = in[0];
  const int16_t r = mono_ ? in[0] : in[1];
  if (state_ == State::Refused) return 0;
  if (state_ == State::Unconfigured) {
    if (pendingN_ < kMaxPending) {
      pending_[pendingN_][0] = l;
      pending_[pendingN_][1] = r;
      ++pendingN_;
      return 0;
    }
    rate_ = kOutRate;  // the hold is full and still no rate: the stream is 44.1 kHz
    configure(plan(kOutRate, 0));
  }
  uint32_t n = drainCarry(out);
  n += runRoute(l, r, out + 2 * n);
  ++taken_;
  produced_ += n;
  return n;
}

uint32_t RateConverter::process(const int16_t* in, uint32_t frames, int16_t* out, uint32_t room, uint32_t* written) {
  uint32_t taken = 0;
  uint32_t w = 0;
  while (taken < frames && state_ != State::Refused && room - w >= maxOut()) {
    w += push(in + 2 * taken, out + 2 * w);
    ++taken;
  }
  *written = w;
  return taken;
}

uint32_t RateConverter::finishPush(int16_t* out) {
  if (state_ == State::Refused) return 0;
  if (state_ == State::Unconfigured) {
    if (pendingN_ == 0) return 0;
    rate_ = kOutRate;
    configure(plan(kOutRate, 0));
  }
  uint32_t n = drainCarry(out);
  const uint64_t target = plan_.ringFrames(taken_);
  if (produced_ + n < target) {
    uint32_t k = runRoute(0, 0, out + 2 * n);  // the tail: the stream followed by silence
    if (produced_ + n + k > target) k = static_cast<uint32_t>(target - produced_ - n);  // past the end: dropped
    n += k;
  }
  produced_ += n;
  return n;
}

bool RateConverter::finished() const {
  switch (state_) {
    case State::Refused:
      return true;
    case State::Unconfigured:
      return pendingN_ == 0;
    case State::Configured:
    default:
      return carryN_ == 0 && produced_ >= plan_.ringFrames(taken_);
  }
}
