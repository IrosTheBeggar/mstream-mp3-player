// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RateConverter.h"

#if defined(__XTENSA__)
#include <xtensa/config/core-isa.h>  // XCHAL_HAVE_MAC16
#endif

#include <atomic>
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

// The C kernel, the specification. Two int32 sums per output, one per
// contiguous half of the row (each half's sum of |c| is under 65536, so
// neither can overflow), added in 64 bits. Unrolled by 8 with constant
// offsets: GCC for the ESP32 then keeps each sum in MAC16's accumulator
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

// A Q15 sum, rounded half up: (sum + 16384) >> 15. At most 2.5e9 / 32768
// in size, so it fits an int32 (the saturation to int16 comes after).
RC_INLINE int32_t roundQ15(int64_t sum) { return static_cast<int32_t>((sum + 16384) >> 15); }

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

// To int16: saturated, never wrapped. The C kernel's (the specification),
// and with the fast kernel on the ESP32 the CLAMPS instruction, without
// branches (saturateFast(): the self-test checks it against this one). Each
// kernel brings its own (K::saturate), so falling back to the C kernel
// after a failed self-test falls back from CLAMPS too.
RC_INLINE int16_t saturateC(int32_t v, uint32_t& clamped) {
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

RC_INLINE int16_t saturateFast(int32_t v, uint32_t& clamped) {
#if defined(__XTENSA__) && XCHAL_HAVE_CLAMPS
  int32_t s;
  asm("clamps %0, %1, 15" : "=r"(s) : "r"(v));
  clamped += s != v ? 1u : 0u;
  return static_cast<int16_t>(s);
#else
  return saturateC(v, clamped);
#endif
}

// A Q15 sum to int16: rounded half up, then saturated, never wrapped (the
// halfbands, C only).
RC_INLINE int16_t toSample(int64_t sum, uint32_t& clamped) { return saturateC(roundQ15(sum), clamped); }

// Stores without the PSRAM workaround's MEMW after each 16-bit store
// (-mfix-esp32-psram-cache-issue): the converter's histories and the stage
// are in internal RAM (RingOutput is kept under 4 KB for that), where the
// workaround isn't needed. Elsewhere plain C.
// One input into a polyphase history: copy 0 at [p] and [p + 48], copy 1 at
// [p + 1] and [p + 49] (`c0`, `c1` point at [p] of each).
RC_INLINE void storeHistory(int16_t* c0, int16_t* c1, int16_t v) {
#if defined(__XTENSA__)
  asm volatile(
      "s16i %[v], %[a], 0\n"
      "s16i %[v], %[a], 96\n"
      "s16i %[v], %[b], 2\n"
      "s16i %[v], %[b], 98\n"
      :
      : [v] "r"(v), [a] "r"(c0), [b] "r"(c1)
      : "memory");
#else
  c0[0] = c0[resampler::kTaps] = v;
  c1[1] = c1[1 + resampler::kTaps] = v;
#endif
}

// One stereo output frame: a single 32-bit store when `o` is 4-byte aligned.
RC_INLINE void storeFrame(int16_t* o, int16_t l, int16_t r, bool aligned) {
#if defined(__XTENSA__)
  if (aligned) {
    const uint32_t v = static_cast<uint16_t>(l) | static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16;
    asm volatile("s32i %[v], %[o], 0\n" : : [v] "r"(v), [o] "r"(o) : "memory");
    return;
  }
#endif
  (void)aligned;
  o[0] = l;
  o[1] = r;
}

struct CKernel {
  static RC_INLINE int16_t saturate(int32_t v, uint32_t& clamped) { return saturateC(v, clamped); }
  static RC_INLINE int32_t dot(const int16_t* c, const int16_t* x) {
    return roundQ15(static_cast<int64_t>(half24(c, x)) + half24(c + kHalfTaps, x + kHalfTaps));
  }
  static RC_INLINE int32_t dotRev(const int16_t* cEnd, const int16_t* x) {
    const int16_t* last = cEnd - 1;
    return roundQ15(static_cast<int64_t>(half24rev(last, x)) + half24rev(last - kHalfTaps, x + kHalfTaps));
  }
};

#if defined(__XTENSA__) && XCHAL_HAVE_MAC16
#define RC_FAST_KERNEL 1
// The MAC16 kernel (the ESP32). One 40-bit accumulator holds a whole row's
// sum exactly (at most 2.5e9 < 2^39), seeded with the rounding constant
// 16384, so the result is the C kernel's to the bit. Each MULA.DD ... LDINC
// multiplies two 16-bit halves of the m registers and loads the next 32
// bits (two samples, or two taps) in the same instruction: 48 multiplies in
// 48 instructions, plus 4 loads ahead.
//
// A multiply has to wait for a value loaded fewer than 3 instructions
// before it (measured: 3 cycles per MULA.LDINC when the next one uses its
// load). The window therefore starts at an odd sample (the history keeps a
// copy where it does: polyBlock()) and is read from one sample before it,
// 4-byte aligned: each sample word then meets halves of two tap words,
// staggered, so every word is loaded 3 instructions before its first use.
// Window words in m0/m1, the row's in m2/m3. Forwards, x[2k] = W[k].hi meets
// c[2k] = C[k].lo and x[2k+1] = W[k+1].lo meets C[k].hi. Backwards (the row
// read from its end with LDDEC, D[k] = (c[46-2k], c[47-2k])): x[2k] =
// W[k].hi meets D[k].hi, x[2k+1] = W[k+1].lo meets D[k].lo. 25 window words
// and 24 row words are loaded, nothing past either. Flash, not IRAM.
#define RC_MAC_GROUP(a, b, dec)                   \
  "mula.dd." a ".ldinc m0, %[px], m0, m2\n"       \
  "mula.dd." b "." dec " m2, %[pc], m1, m2\n"     \
  "mula.dd." a ".ldinc m1, %[px], m1, m3\n"       \
  "mula.dd." b "." dec " m3, %[pc], m0, m3\n"
#define RC_MAC_DOT(a, b, dec)                     \
  "ldinc m0, %[px]\n"                             \
  dec " m2, %[pc]\n"                              \
  "wsr.acchi %[zero]\n"                           \
  "ldinc m1, %[px]\n"                             \
  "wsr.acclo %[round]\n"                          \
  dec " m3, %[pc]\n"                              \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  RC_MAC_GROUP(a, b, dec)                         \
  "mula.dd." a ".ldinc m0, %[px], m0, m2\n"       \
  "mula.dd." b " m1, m2\n"                        \
  "mula.dd." a " m1, m3\n"                        \
  "mula.dd." b " m0, m3\n"                        \
  "rsr.acclo %[lo]\n"                             \
  "rsr.acchi %[hi]\n"

// The 40-bit accumulator shifted right by 15 (floor). ACCHI's 8 bits are
// sign-extended here, whatever RSR leaves above them.
RC_INLINE int32_t accShift15(uint32_t hi, uint32_t lo) {
  const int32_t top = static_cast<int8_t>(hi & 0xff);
  return static_cast<int32_t>(static_cast<uint32_t>(top) << 17 | lo >> 15);
}

// `x` is the window's first sample, at an odd sample from 4-byte alignment
// (2 bytes past it); `c` the row (4-byte aligned), `cEnd` one past its end.
struct FastKernel {
  static RC_INLINE int16_t saturate(int32_t v, uint32_t& clamped) { return saturateFast(v, clamped); }
  static RC_INLINE int32_t dot(const int16_t* c, const int16_t* x) {
    uint32_t hi, lo;
    const int16_t* pc = c - 2;  // LDINC adds its 4 bytes first
    const int16_t* px = x - 3;  // ...and the window is read from x - 1
    asm volatile(RC_MAC_DOT("hl", "lh", "ldinc")
                 : [hi] "=r"(hi), [lo] "=r"(lo), [px] "+r"(px), [pc] "+r"(pc)
                 : [zero] "r"(0), [round] "r"(16384)
                 : "acc", "memory");
    return accShift15(hi, lo);
  }
  static RC_INLINE int32_t dotRev(const int16_t* cEnd, const int16_t* x) {
    uint32_t hi, lo;
    const int16_t* pc = cEnd;  // LDDEC takes its 4 bytes off first: taps 46 and 47
    const int16_t* px = x - 3;
    asm volatile(RC_MAC_DOT("hh", "ll", "lddec")
                 : [hi] "=r"(hi), [lo] "=r"(lo), [px] "+r"(px), [pc] "+r"(pc)
                 : [zero] "r"(0), [round] "r"(16384)
                 : "acc", "memory");
    return accShift15(hi, lo);
  }
};
#else
#define RC_FAST_KERNEL 0
using FastKernel = CKernel;  // no fast kernel off the ESP32
#endif

// Which kernel every converter uses: the C one until the caller turns the
// fast one on (RateConverter::useFastKernel(), after its self-test).
std::atomic<bool> gFast{false};

// The polyphase tables the stages read: flash, or the caller's copy
// (RateConverter::useTables()), and who makes that copy when a stream needs
// it, or frees it when a stream starts without (setTablesWanted()).
const int16_t (*gD147)[resampler::kTaps] = resampler::kD147;
const int16_t (*gU12)[resampler::kTaps] = resampler::kU12;
void (*gTablesWanted)(bool) = nullptr;

// The route reads the polyphase tables (88.2 kHz is a halfband alone).
bool readsTables(const Route& rt) { return rt.stageA == kPoly || rt.thenD147; }

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
  std::memset(&histA_, 0, sizeof(histA_));
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
  if (gTablesWanted) {  // before configure() reads them
    const bool reads = p.ok && readsTables(kRoutes[p.index]);
    if (reads || state_ == State::Unconfigured) gTablesWanted(reads);  // (none mid-stream: see the header)
  }
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
  polyA_.rows = nullptr;  // (no stale table: a freed copy, after a route that read it)
  if (rt.stageA == kPoly && rt.upL == 0) {
    polyA_.rows = gD147;
    polyA_.tableRows = resampler::kD147Rows;
    polyA_.step = 1;
    polyA_.L = resampler::kD147Rows;
    polyA_.M = 160;
  } else if (rt.stageA == kPoly) {
    polyA_.rows = gU12;
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
  polyB_.rows = gD147;
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

uint32_t RateConverter::maxOutFor(uint32_t frames) const {
  switch (state_) {
    case State::Refused:
      return 0;
    case State::Unconfigured:  // held, then played at 44.1 kHz: one each
      return pendingN_ + frames;
    case State::Configured:
    default:
      return carryN_ + frames * routeMaxOut_;
  }
}

// A polyphase stage over a block: for each input, the history (both
// channels, both copies), then every output whose phase falls before the
// next input. The same order of operations as one input at a time, so the
// same bits; the stage's state lives in registers for the whole block.
template <class K>
uint32_t RateConverter::polyBlock(PolyStage& s, PolyHist& h, const int16_t* in, uint32_t n, int16_t* out,
                                  bool monoIn) {
  uint32_t w = s.w;
  uint32_t lead = s.lead;
  uint32_t phase = s.phase;
  const uint32_t L = s.L, M = s.M, step = s.step, tableRows = s.tableRows;
  const uint32_t middle = tableRows / 2u;
  const int16_t(*rows)[kTaps] = s.rows;
  const bool mono = mono_;
  uint32_t clamped = clamped_;
  int16_t* o = out;
  const bool aligned = (reinterpret_cast<uintptr_t>(out) & 3u) == 0;
  int16_t* l0 = h[0][0];
  int16_t* l1 = h[0][1];
  int16_t* r0 = h[1][0];
  int16_t* r1 = h[1][1];
  for (uint32_t i = 0; i < n; ++i) {
    const int16_t l = in[2 * i];
    const int16_t r = monoIn ? l : in[2 * i + 1];
    storeHistory(l0 + w, l1 + w, l);
    storeHistory(r0 + w, r1 + w, r);
    w = w + 1 == static_cast<uint32_t>(kTaps) ? 0 : w + 1;
    if (lead != 0) {
      --lead;
      continue;
    }
    // The last 48 inputs, oldest first, from the copy where they start at
    // an odd sample, 2 bytes past a 4-byte boundary (the MAC16 kernel reads
    // them from the sample before, two at a time).
    const bool odd = (w & 1u) != 0;
    const int16_t* x0 = odd ? l0 + w : l1 + w + 1;
    const int16_t* x1 = odd ? r0 + w : r1 + w + 1;
    while (phase < L) {
      const uint32_t row = phase * step;
      int32_t a, b = 0;
      if (row > middle) {  // stored as row L - p, read backwards
        const int16_t* end = rows[tableRows - row] + kTaps;
        a = K::dotRev(end, x0);
        if (!mono) b = K::dotRev(end, x1);
      } else {
        const int16_t* c = rows[row];
        a = K::dot(c, x0);
        if (!mono) b = K::dot(c, x1);
      }
      const int16_t left = K::saturate(a, clamped);
      storeFrame(o, left, mono ? left : K::saturate(b, clamped), aligned);
      o += 2;
      phase += M;
    }
    phase -= L;
  }
  s.w = static_cast<uint16_t>(w);
  s.lead = static_cast<uint16_t>(lead);
  s.phase = static_cast<uint16_t>(phase);
  clamped_ = clamped;
  return static_cast<uint32_t>(o - out) / 2;
}

uint32_t RateConverter::pushHalfband(int16_t l, int16_t r, int16_t* out) {
  HalfbandStage& s = hb_;
  int16_t* h0 = histA_.hb[0];
  int16_t* h1 = histA_.hb[1];
  h0[s.w] = h0[s.w + s.taps] = l;
  h1[s.w] = h1[s.w + s.taps] = r;
  if (++s.w == s.taps) s.w = 0;
  if (s.lead != 0) {
    --s.lead;
    return 0;
  }
  s.skip = !s.skip;  // one output per two inputs, centred on the even ones
  if (!s.skip) return 0;
  const int16_t* x0 = h0 + s.w;
  const int16_t* x1 = h1 + s.w;
  if (s.taps == resampler::kHb96Taps) {
    out[0] = toSample(halfbandSum<resampler::kHb96Taps>(s.side, x0), clamped_);
    out[1] = mono_ ? out[0] : toSample(halfbandSum<resampler::kHb96Taps>(s.side, x1), clamped_);
  } else {
    out[0] = toSample(halfbandSum<resampler::kHb88Taps>(s.side, x0), clamped_);
    out[1] = mono_ ? out[0] : toSample(halfbandSum<resampler::kHb88Taps>(s.side, x1), clamped_);
  }
  return 1;
}

// The halfbands (88.2/96 kHz, off in this build) stay frame by frame, in C.
uint32_t RateConverter::halfbandBlock(const int16_t* in, uint32_t n, int16_t* out, bool monoIn) {
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; ++i) k += pushHalfband(in[2 * i], monoIn ? in[2 * i] : in[2 * i + 1], out + 2 * k);
  return k;
}

// `n` frames through the route. A cascade runs stage A over a few frames
// into a buffer on the stack, then stage B over that: each stage sees its
// inputs in the same order as frame by frame.
template <class K>
uint32_t RateConverter::route(const int16_t* in, uint32_t n, int16_t* out, bool monoIn) {
  if (stageA_ == kNone) {
    for (uint32_t i = 0; i < n; ++i) {
      out[2 * i] = in[2 * i];
      out[2 * i + 1] = monoIn ? in[2 * i] : in[2 * i + 1];
    }
    return n;
  }
  if (!thenD147_) {
    return stageA_ == kPoly ? polyBlock<K>(polyA_, histA_.poly, in, n, out, monoIn) : halfbandBlock(in, n, out, monoIn);
  }
  constexpr uint32_t kSub = 16;  // x6 (8 kHz): 96 frames of 48 kHz
  alignas(4) int16_t mid[kSub * 6 * 2];
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; i += kSub) {
    const uint32_t m = n - i < kSub ? n - i : kSub;
    const uint32_t made = stageA_ == kPoly ? polyBlock<K>(polyA_, histA_.poly, in + 2 * i, m, mid, monoIn)
                                           : halfbandBlock(in + 2 * i, m, mid, monoIn);
    k += polyBlock<K>(polyB_, histB_, mid, made, out + 2 * k, false);
  }
  return k;
}

uint32_t RateConverter::routeAny(const int16_t* in, uint32_t n, int16_t* out, bool monoIn) {
  if (RC_FAST_KERNEL && gFast.load(std::memory_order_relaxed)) return route<FastKernel>(in, n, out, monoIn);
  return route<CKernel>(in, n, out, monoIn);
}

// One source frame through the route (in mono, r is already l).
uint32_t RateConverter::runRoute(int16_t l, int16_t r, int16_t* out) {
  const int16_t f[2] = {l, r};
  return routeAny(f, 1, out, false);
}

uint32_t RateConverter::drainCarry(int16_t* out) {
  const uint32_t n = carryN_;
  if (n != 0) std::memcpy(out, carry_, n * 2 * sizeof(int16_t));
  carryN_ = 0;
  return n;
}

uint32_t RateConverter::convert(const int16_t* in, uint32_t n, int16_t* out) {
  if (state_ != State::Configured) return 0;
  uint32_t k = drainCarry(out);
  k += routeAny(in, n, out + 2 * k, mono_);
  taken_ += n;
  produced_ += k;
  return k;
}

uint32_t RateConverter::push(const int16_t in[2], int16_t* out) {
  if (state_ == State::Refused) return 0;
  if (state_ == State::Unconfigured) {
    if (pendingN_ < kMaxPending) {
      pending_[pendingN_][0] = in[0];
      pending_[pendingN_][1] = mono_ ? in[0] : in[1];
      ++pendingN_;
      return 0;
    }
    rate_ = kOutRate;  // the hold is full and still no rate: the stream is 44.1 kHz
    configure(plan(kOutRate, 0));
  }
  return convert(in, 1, out);
}

uint32_t RateConverter::process(const int16_t* in, uint32_t frames, int16_t* out, uint32_t room, uint32_t* written) {
  uint32_t taken = 0;
  uint32_t w = 0;
  while (taken < frames && state_ != State::Refused && room - w >= maxOut()) {
    if (state_ != State::Configured) {  // the hold: one at a time
      w += push(in + 2 * taken, out + 2 * w);
      ++taken;
      continue;
    }
    // As many as surely fit: the same frames a frame-by-frame loop takes
    // (each would still have found maxOut() of room), in one block.
    uint32_t k = (room - w - carryN_) / routeMaxOut_;
    if (k > frames - taken) k = frames - taken;
    w += convert(in + 2 * taken, k, out + 2 * w);
    taken += k;
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

// ---- the kernel ----

int32_t RateConverter::dotC(const int16_t* row, const int16_t* x) { return CKernel::dot(row, x); }
int32_t RateConverter::dotRevC(const int16_t* rowEnd, const int16_t* x) { return CKernel::dotRev(rowEnd, x); }
int32_t RateConverter::dotFast(const int16_t* row, const int16_t* x) { return FastKernel::dot(row, x); }
int32_t RateConverter::dotRevFast(const int16_t* rowEnd, const int16_t* x) { return FastKernel::dotRev(rowEnd, x); }

bool RateConverter::fastKernelBuilt() { return RC_FAST_KERNEL != 0; }

void RateConverter::useTables(const int16_t (*d147)[resampler::kTaps], const int16_t (*u12)[resampler::kTaps]) {
  gD147 = d147 ? d147 : resampler::kD147;
  gU12 = u12 ? u12 : resampler::kU12;
}

bool RateConverter::tablesCopied() { return gD147 != resampler::kD147; }

void RateConverter::setTablesWanted(void (*hook)(bool)) { gTablesWanted = hook; }

void RateConverter::useFastKernel(bool on) { gFast.store(on && RC_FAST_KERNEL != 0, std::memory_order_relaxed); }

bool RateConverter::fastKernel() { return RC_FAST_KERNEL != 0 && gFast.load(std::memory_order_relaxed); }

RateConverter::SelfTest RateConverter::kernelSelfTest(uint32_t seed, uint32_t windows) {
  SelfTest t;
  alignas(4) int16_t window[kTaps + 2];
  int16_t* x = window + 1;  // as the converter's windows: 2 bytes past a 4-byte boundary
  uint32_t state = seed;
  struct Table {
    const int16_t (*rows)[kTaps];
    int count;
  };
  const Table tables[] = {{resampler::kD147, resampler::kD147Stored}, {resampler::kU12, resampler::kU12Stored}};
  for (const Table& tb : tables) {
    for (int r = 0; r < tb.count; ++r) {
      const int16_t* row = tb.rows[r];
      for (int rev = 0; rev < 2; ++rev) {
        for (uint32_t k = 0; k < windows + 2; ++k) {
          for (int j = 0; j < kTaps; ++j) {
            const int16_t c = rev ? row[kTaps - 1 - j] : row[j];
            if (k == 0) {
              x[j] = c >= 0 ? 32767 : -32768;  // the row's largest sum: saturates, uses the top bits
            } else if (k == 1) {
              x[j] = c >= 0 ? -32768 : 32767;  // and its most negative
            } else {
              state = state * 1664525u + 1013904223u;
              x[j] = static_cast<int16_t>(state >> 16);
            }
          }
          const int32_t want = rev ? dotRevC(row + kTaps, x) : dotC(row, x);
          const int32_t got = rev ? dotRevFast(row + kTaps, x) : dotFast(row, x);
          uint32_t clampsC = 0, clamps = 0;
          ++t.dots;
          if (want != got || saturateC(want, clampsC) != saturateFast(got, clamps) || clampsC != clamps) {
            if (t.mismatches == 0) {
              t.wantC = want;
              t.gotFast = got;
            }
            ++t.mismatches;
          }
        }
      }
    }
  }
  // The fast kernel's saturation (CLAMPS on the ESP32) against plain C at
  // its edges. A mismatch turns it off with the fast kernel: the C kernel
  // saturates in C.
  static constexpr int32_t kEdges[] = {0, 1, -1, 32766, 32767, 32768, 40000, 76000, -32767, -32768, -32769, -40000, -76000};
  for (const int32_t v : kEdges) {
    uint32_t clampsC = 0, clamps = 0;
    if (saturateC(v, clampsC) != saturateFast(v, clamps) || clampsC != clamps) {
      if (t.mismatches == 0) t.wantC = t.gotFast = v;
      ++t.mismatches;
    }
  }
  return t;
}
