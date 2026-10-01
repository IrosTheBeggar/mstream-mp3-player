// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for RateConverter and its tables (docs/RESAMPLER.md).
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include "RateConverter.h"
#include "ResamplerTables.h"

namespace {
using Frames = std::vector<int16_t>;  // interleaved stereo
using cd = std::complex<double>;

constexpr uint32_t kRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000};
constexpr uint32_t kCpu = 240;
constexpr bool kHiRes = true;  // 88.2/96 kHz are tested whether or not this build plays them
constexpr double kOut = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int K = resampler::kTaps;

// Big objects live here, not on the stack.
RateConverter gConv;
RateConverter gConv2;

uint64_t ceilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

int16_t sat16(int64_t v) { return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// floor(a / 2^15), whatever the platform's >> does with negatives.
int64_t floorDiv32768(int64_t a) { return a >= 0 ? a / 32768 : -((-a + 32767) / 32768); }

void append(Frames& out, const int16_t* buf, uint32_t frames) { out.insert(out.end(), buf, buf + 2 * frames); }

// One-shot: the rate first, every frame pushed, then the tail.
Frames convert(RateConverter& c, uint32_t hz, const Frames& in, bool mono = false) {
  c.reset();
  TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
  c.setMono(mono);
  Frames out;
  int16_t buf[RateConverter::kMaxOut * 2];
  for (size_t i = 0; i + 1 < in.size(); i += 2) {
    const uint32_t m = c.maxOut();
    const uint32_t n = c.push(&in[i], buf);
    TEST_ASSERT_TRUE(n <= m);
    append(out, buf, n);
  }
  while (!c.finished()) {
    const uint32_t m = c.maxOut();
    const uint32_t n = c.finishPush(buf);
    TEST_ASSERT_TRUE(n <= m);
    append(out, buf, n);
  }
  return out;
}

Frames noise(size_t frames, uint32_t seed, int amp) {
  std::mt19937 rng(seed);
  Frames v(frames * 2);
  for (auto& s : v) s = sat16(static_cast<int64_t>(rng() % (2u * amp + 1)) - amp);
  return v;
}

// ---- An independent reference: direct convolution at each output instant ----

// Row p of an L-row table (all rows, rebuilt from the stored half).
std::vector<int> fullRow(const int16_t (*rows)[K], int tableRows, int p) {
  std::vector<int> r(K);
  for (int j = 0; j < K; ++j)
    r[j] = p <= tableRows / 2 ? rows[p][j] : rows[tableRows - p][K - 1 - j];
  return r;
}

struct PolyDesign {
  const int16_t (*rows)[K];
  int tableRows;
  int L, M, step;
};
constexpr PolyDesign kD147Design{resampler::kD147, resampler::kD147Rows, 147, 160, 1};
PolyDesign up(int L, int M) { return PolyDesign{resampler::kU12, resampler::kU12Rows, L, M, resampler::kU12Rows / L}; }

// One channel. Output n sits at source time n * M / L; the row is phase
// (n * M) mod L, and its window's newest input is floor(n * M / L) + K/2.
std::vector<int16_t> refPoly(const std::vector<int16_t>& x, const PolyDesign& d, size_t count) {
  std::vector<int16_t> y(count);
  auto at = [&](int64_t i) -> int64_t { return i >= 0 && i < static_cast<int64_t>(x.size()) ? x[i] : 0; };
  for (size_t n = 0; n < count; ++n) {
    const uint64_t t = static_cast<uint64_t>(n) * d.M;
    const int64_t newest = static_cast<int64_t>(t / d.L) + K / 2;
    const std::vector<int> row = fullRow(d.rows, d.tableRows, static_cast<int>(t % d.L) * d.step);
    int64_t s = 0;
    for (int j = 0; j < K; ++j) s += static_cast<int64_t>(row[j]) * at(newest - (K - 1) + j);
    y[n] = sat16(floorDiv32768(s + 16384));
  }
  return y;
}

// One channel. Output n is centred on input 2n.
std::vector<int16_t> refHalfband(const std::vector<int16_t>& x, const int16_t* side, int taps, size_t count) {
  const int c = (taps - 1) / 2;
  const int sides = (taps + 1) / 4;
  std::vector<int16_t> y(count);
  auto at = [&](int64_t i) -> int64_t { return i >= 0 && i < static_cast<int64_t>(x.size()) ? x[i] : 0; };
  for (size_t n = 0; n < count; ++n) {
    const int64_t m = 2 * static_cast<int64_t>(n);
    int64_t s = 16384 * at(m);
    for (int i = 0; i < sides; ++i) {
      const int d = c - 2 * i;
      s += side[i] * (at(m - d) + at(m + d));
    }
    y[n] = sat16(floorDiv32768(s + 16384));
  }
  return y;
}

std::vector<int16_t> refChannel(uint32_t hz, const std::vector<int16_t>& x) {
  const size_t n = x.size();
  const RateConverter::Plan p = RateConverter::plan(hz, kCpu, kHiRes);
  const size_t count = static_cast<size_t>(p.ringFrames(n));
  const size_t spare = 64;  // the first stage's output past the end, which the second stage reads
  switch (hz) {
    case 44100: return x;
    case 48000: return refPoly(x, kD147Design, count);
    case 22050: return refPoly(x, up(2, 1), count);
    case 11025: return refPoly(x, up(4, 1), count);
    case 88200: return refHalfband(x, resampler::kHb88Side, resampler::kHb88Taps, count);
    case 96000: return refPoly(refHalfband(x, resampler::kHb96Side, resampler::kHb96Taps, ceilDiv(n, 2) + spare), kD147Design, count);
    default: break;
  }
  int L = 0, M = 1;
  switch (hz) {
    case 32000: L = 3; M = 2; break;
    case 24000: L = 2; break;
    case 16000: L = 3; break;
    case 12000: L = 4; break;
    case 8000: L = 6; break;
    default: TEST_FAIL_MESSAGE("no reference for this rate");
  }
  return refPoly(refPoly(x, up(L, M), ceilDiv(n * L, M) + spare), kD147Design, count);
}

Frames reference(uint32_t hz, const Frames& in) {
  std::vector<int16_t> l(in.size() / 2), r(in.size() / 2);
  for (size_t i = 0; i < l.size(); ++i) {
    l[i] = in[2 * i];
    r[i] = in[2 * i + 1];
  }
  const auto yl = refChannel(hz, l), yr = refChannel(hz, r);
  Frames out(yl.size() * 2);
  for (size_t i = 0; i < yl.size(); ++i) {
    out[2 * i] = yl[i];
    out[2 * i + 1] = yr[i];
  }
  return out;
}

// ---- Spectra ----

void fft(std::vector<cd>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / static_cast<double>(len);
    const cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const cd u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

constexpr int kN = 16384;   // analysis window, in output frames
constexpr int kSkip = 2048;  // past the start's transient

struct Tone {
  double ampDb = -999;  // the output's fundamental against the input's amplitude
  double thdnDb = 0;    // everything else in 20 Hz-20 kHz against the fundamental
  double spurDb = -999; // the largest other component in 20 Hz-20 kHz, against the input
  double spurHz = 0;
  double offsetUs = 0;  // the output's timing against n / 44100
};

// A sine at the source rate, converted. In band, f is moved onto an analysis
// bin, so the window holds whole cycles (no leakage).
Tone measure(uint32_t hz, double f, double dbfs, bool snap = true) {
  if (snap) f = std::round(f * kN / kOut) * kOut / kN;
  const double amp = 32767.0 * std::pow(10.0, dbfs / 20.0);
  const double ph0 = 0.3;
  const size_t frames = static_cast<size_t>((kSkip + kN + 64) * hz / kOut) + 256;
  Frames in(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const int16_t v = sat16(std::lround(amp * std::sin(2 * kPi * f * static_cast<double>(i) / hz + ph0)));
    in[2 * i] = v;
    in[2 * i + 1] = v;
  }
  const Frames out = convert(gConv, hz, in);
  TEST_ASSERT_TRUE(out.size() / 2 >= static_cast<size_t>(kSkip + kN));
  Tone t;
  const bool inBand = f < kOut / 2;
  const int kf = static_cast<int>(std::lround(f * kN / kOut));
  {
    std::vector<cd> a(kN);
    for (int i = 0; i < kN; ++i) a[i] = out[2 * (kSkip + i)];
    fft(a);
    double fund = 0, rest = 0;
    for (int k = 1; k < kN / 2; ++k) {
      const double bin = k * kOut / kN;
      const double p = std::norm(a[k]);
      if (inBand && k == kf) {
        fund = p;
      } else if (bin >= 20 && bin <= 20000) {
        rest += p;
      }
    }
    if (inBand) {
      t.ampDb = db(2 * std::sqrt(fund) / kN / amp);
      t.thdnDb = 10 * std::log10(std::max(rest, 1e-30) / fund);
      // x[m] = A sin(w (m + skip) + ph0) has its bin at angle w skip + ph0 - pi/2.
      double d = std::arg(a[kf]) - (2 * kPi * f * kSkip / kOut + ph0 - kPi / 2);
      d = std::remainder(d, 2 * kPi);
      t.offsetUs = -d / (2 * kPi * f) * 1e6;
    }
  }
  {  // Blackman-Harris: whole cycles leak nothing past +-3 bins, so the spurs are clean
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    std::vector<cd> a(kN);
    for (int i = 0; i < kN; ++i) {
      const double x = 2 * kPi * i / kN;
      a[i] = out[2 * (kSkip + i)] * (a0 - a1 * std::cos(x) + a2 * std::cos(2 * x) - a3 * std::cos(3 * x));
    }
    fft(a);
    double best = 0;
    int bk = 0;
    for (int k = 1; k < kN / 2; ++k) {
      const double bin = k * kOut / kN;
      if (bin < 20 || bin > 20000) continue;
      if (inBand && std::abs(k - kf) <= 4) continue;
      const double m = std::abs(a[k]);
      if (m > best) {
        best = m;
        bk = k;
      }
    }
    t.spurDb = db(2 * best / (kN * a0) / amp);
    t.spurHz = bk * kOut / kN;
  }
  return t;
}

// |H| of a whole polyphase prototype (all L rows interleaved by their
// offsets), normalised to 1 at DC; frequencies as fractions of the source rate.
struct Response {
  std::vector<double> mag;  // bins 0..size/2
  double binHz;             // in source-rate fractions
};
Response polyResponse(const int16_t (*rows)[K], int L, size_t fftSize) {
  std::vector<cd> a(fftSize, 0.0);
  // Row p's tap j sits at 23 - j + p/L source samples: index L (23 - j) + p + 24 L.
  for (int p = 0; p < L; ++p) {
    const std::vector<int> row = fullRow(rows, L, p);
    for (int j = 0; j < K; ++j) a[static_cast<size_t>(L * (K / 2 - 1 - j) + p + L * K / 2)] = row[j];
  }
  fft(a);
  Response r;
  r.binHz = static_cast<double>(L) / static_cast<double>(fftSize);
  for (size_t k = 0; k <= fftSize / 2; ++k) r.mag.push_back(std::abs(a[k]) / (32768.0 * L));
  return r;
}
Response halfbandResponse(const int16_t* side, int taps, size_t fftSize) {
  std::vector<cd> a(fftSize, 0.0);
  const int c = (taps - 1) / 2;
  a[c] = 16384;
  for (int i = 0; i < (taps + 1) / 4; ++i) {
    const int d = c - 2 * i;
    a[c - d] = side[i];
    a[c + d] = side[i];
  }
  fft(a);
  Response r;
  r.binHz = 1.0 / static_cast<double>(fftSize);
  for (size_t k = 0; k <= fftSize / 2; ++k) r.mag.push_back(std::abs(a[k]) / 32768.0);
  return r;
}
void band(const Response& r, double from, double to, double* lo, double* hi) {
  *lo = 1e9;
  *hi = -1e9;
  for (size_t k = 0; k < r.mag.size(); ++k) {
    const double f = k * r.binHz;
    if (f < from || f > to) continue;
    *lo = std::min(*lo, db(r.mag[k]));
    *hi = std::max(*hi, db(r.mag[k]));
  }
}

// RingOutput's side of the contract, modelled: a 256-frame stage, a budget
// of source frames per pass, and a ring that takes what it has room for. A
// source frame is taken only when the stage has room for maxOut(); when it
// hasn't even after a commit, the generator keeps the frame and offers it
// again on its next pass.
struct RingSim {
  RateConverter& conv;
  Frames ring;
  int16_t stage[256 * 2];
  uint32_t staged = 0;
  uint32_t budget = 0;
  uint32_t ringRoom = 0;
  uint64_t refusedFull = 0;

  explicit RingSim(RateConverter& c) : conv(c) {}
  void commit() {
    const uint32_t n = std::min(staged, ringRoom);
    append(ring, stage, n);
    ringRoom -= n;
    std::copy(stage + 2 * n, stage + 2 * staged, stage);
    staged -= n;
  }
  bool roomFor() {
    if (256 - staged >= conv.maxOut()) return true;
    commit();
    return 256 - staged >= conv.maxOut();
  }
  bool consume(const int16_t s[2]) {
    if (budget == 0) return false;
    if (!roomFor()) {
      ++refusedFull;
      return false;
    }
    const uint32_t m = conv.maxOut();
    const uint32_t n = conv.push(s, stage + 2 * staged);
    TEST_ASSERT_TRUE(n <= m);
    TEST_ASSERT_TRUE(m <= RateConverter::kMaxOut);
    staged += n;
    --budget;
    return true;
  }
  bool finishSome() {
    while (!conv.finished()) {
      if (!roomFor()) return false;
      const uint32_t m = conv.maxOut();
      const uint32_t n = conv.finishPush(stage + 2 * staged);
      TEST_ASSERT_TRUE(n <= m);
      staged += n;
    }
    return true;
  }
};

// A decoder handing over `src` the way ESP8266Audio's generators do, through
// the ring above with random refusals: `rateAfter` frames go over before
// setRate() (an MP3 sends its constructor's {0,0}, then its first sample).
Frames streamThroughRing(RateConverter& c, uint32_t hz, const Frames& src, uint32_t seed, size_t rateAfter = 0) {
  std::mt19937 rng(seed);
  c.reset();
  if (rateAfter == 0) TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
  RingSim sim(c);
  const size_t frames = src.size() / 2;
  size_t next = 0;
  bool holding = false;
  int16_t held[2] = {0, 0};
  while (next < frames || holding) {
    sim.budget = 1024;
    sim.ringRoom = rng() % 3 == 0 ? 0 : rng() % 1500;
    for (;;) {
      if (!holding) {
        if (next >= frames) break;
        held[0] = src[2 * next];
        held[1] = src[2 * next + 1];
        ++next;
        holding = true;
      }
      if (rateAfter != 0 && next == rateAfter + 1 && !c.configured()) TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
      if (!sim.consume(held)) break;
      holding = false;
    }
    sim.commit();
  }
  for (;;) {
    sim.ringRoom = rng() % 1500;
    const bool done = sim.finishSome();
    sim.commit();
    if (done && sim.staged == 0) break;
  }
  return sim.ring;
}

void assertSame(const Frames& want, const Frames& got) {
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) {
    if (want[i] != got[i]) {
      char msg[96];
      snprintf(msg, sizeof(msg), "first difference at frame %u of %u", static_cast<unsigned>(i / 2),
               static_cast<unsigned>(want.size() / 2));
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

// A 1 kHz full-scale square wave at the source rate.
Frames square(uint32_t hz, size_t frames) {
  Frames v(frames * 2);
  for (size_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = ((i * 2000 / hz) & 1) ? -32768 : 32767;
  return v;
}
}  // namespace

void setUp() {}
void tearDown() {}

// ---- The tables ----

void test_every_row_sums_to_unity() {
  for (int p = 0; p < resampler::kD147Stored; ++p) {
    int32_t s = 0;
    for (int j = 0; j < K; ++j) s += resampler::kD147[p][j];
    TEST_ASSERT_EQUAL_INT32(32768, s);
  }
  for (int p = 0; p < resampler::kU12Stored; ++p) {
    int32_t s = 0;
    for (int j = 0; j < K; ++j) s += resampler::kU12[p][j];
    TEST_ASSERT_EQUAL_INT32(32768, s);
  }
  int32_t s96 = 16384, s88 = 16384;  // the centre tap, then both sides
  for (int i = 0; i < resampler::kHb96SideTaps; ++i) s96 += 2 * resampler::kHb96Side[i];
  for (int i = 0; i < resampler::kHb88SideTaps; ++i) s88 += 2 * resampler::kHb88Side[i];
  TEST_ASSERT_EQUAL_INT32(32768, s96);
  TEST_ASSERT_EQUAL_INT32(32768, s88);
}

// Each of the kernel's int32 sums covers at most 65535 x 32768 < 2^31.
void test_the_partial_sums_cannot_overflow() {
  auto halves = [](const int16_t* row) {
    int32_t a = 0, b = 0;
    for (int j = 0; j < K / 2; ++j) a += std::abs(row[j]);
    for (int j = K / 2; j < K; ++j) b += std::abs(row[j]);
    return std::max(a, b);
  };
  int32_t worst = 0;
  for (int p = 0; p < resampler::kD147Stored; ++p) worst = std::max(worst, halves(resampler::kD147[p]));
  for (int p = 0; p < resampler::kU12Stored; ++p) worst = std::max(worst, halves(resampler::kU12[p]));
  TEST_ASSERT_TRUE(worst < 65536);
  for (const auto& hb : {std::make_pair(resampler::kHb96Side, resampler::kHb96SideTaps),
                         std::make_pair(resampler::kHb88Side, resampler::kHb88SideTaps)}) {
    int32_t side = 0;
    for (int i = 0; i < hb.second; ++i) side += std::abs(hb.first[i]);
    TEST_ASSERT_TRUE(16384 + side < 65536);  // the left side with the centre; the right side alone
  }
  // ...while a single sum over a whole row could: the split is needed.
  int32_t whole = 0;
  for (int p = 0; p < resampler::kD147Stored; ++p) {
    int32_t s = 0;
    for (int j = 0; j < K; ++j) s += std::abs(resampler::kD147[p][j]);
    whole = std::max(whole, s);
  }
  TEST_ASSERT_TRUE(static_cast<int64_t>(whole) * 32768 > INT32_MAX);
}

void test_the_filters_responses() {
  double lo, hi;
  const Response d = polyResponse(resampler::kD147, resampler::kD147Rows, size_t(1) << 18);
  band(d, 20.0 / 48000, 20000.0 / 48000, &lo, &hi);
  TEST_ASSERT_TRUE(lo > -0.001 && hi < 0.001);  // 20 Hz-20 kHz flat
  band(d, 25950.0 / 48000, resampler::kD147Rows / 2.0, &lo, &hi);
  TEST_ASSERT_TRUE(hi < -85.0);                 // everything that would image below 22.05 kHz

  const Response u = polyResponse(resampler::kU12, resampler::kU12Rows, size_t(1) << 16);
  band(u, 0, 0.4375, &lo, &hi);
  TEST_ASSERT_TRUE(lo > -0.001 && hi < 0.001);  // to 0.875 of the source's Nyquist
  band(u, 0.5575, resampler::kU12Rows / 2.0, &lo, &hi);
  TEST_ASSERT_TRUE(hi < -77.5);

  const Response h96 = halfbandResponse(resampler::kHb96Side, resampler::kHb96Taps, 1 << 14);
  band(h96, 0, 20000.0 / 96000, &lo, &hi);
  TEST_ASSERT_TRUE(lo > -0.002 && hi < 0.002);
  band(h96, 28000.0 / 96000, 0.5, &lo, &hi);
  TEST_ASSERT_TRUE(hi < -80.0);

  const Response h88 = halfbandResponse(resampler::kHb88Side, resampler::kHb88Taps, 1 << 14);
  band(h88, 0, 20000.0 / 88200, &lo, &hi);
  TEST_ASSERT_TRUE(lo > -0.002 && hi < 0.002);
  band(h88, 24100.0 / 88200, 0.5, &lo, &hi);
  TEST_ASSERT_TRUE(hi < -75.0);
}

// ---- Plans ----

void test_plans_and_refusals() {
  for (uint32_t hz : kRates) TEST_ASSERT_TRUE(RateConverter::plan(hz, 240, true).ok);
  RateConverter::Plan p = RateConverter::plan(48000, 160);
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL_UINT32(147, p.num);
  TEST_ASSERT_EQUAL_UINT32(160, p.den);
  TEST_ASSERT_EQUAL_STRING("147/160", p.route);
  TEST_ASSERT_EQUAL_UINT64(26460000, p.ringFrames(28800000));  // 600 s
  TEST_ASSERT_EQUAL_UINT64(44101, p.ringFrames(48001));
  p = RateConverter::plan(44100, 80);
  TEST_ASSERT_TRUE(p.ok && p.num == 1 && p.den == 1);
  p = RateConverter::plan(96000, 240, true);
  TEST_ASSERT_TRUE(p.num == 147 && p.den == 320);
  p = RateConverter::plan(8000, 240);
  TEST_ASSERT_TRUE(p.num == 441 && p.den == 80);
  p = RateConverter::plan(32000, 240);
  TEST_ASSERT_TRUE(p.num == 441 && p.den == 320);
  p = RateConverter::plan(22050, 240);
  TEST_ASSERT_TRUE(p.num == 2 && p.den == 1);
  p = RateConverter::plan(88200, 240, true);
  TEST_ASSERT_TRUE(p.num == 1 && p.den == 2);
  TEST_ASSERT_EQUAL(RateConverter::Refusal::None, p.refusal);
  for (uint32_t hz : {88200u, 96000u}) {
    for (uint32_t mhz : {160u, 80u}) {
      p = RateConverter::plan(hz, mhz, true);
      TEST_ASSERT_FALSE(p.ok);
      TEST_ASSERT_EQUAL(RateConverter::Refusal::NeedsCpu, p.refusal);
      TEST_ASSERT_EQUAL_STRING("needs the 240 MHz CPU speed", p.reason);
    }
  }
  for (uint32_t hz : {176400u, 192000u, 37800u, 50000u, 64000u, 44056u, 1u, 47999u}) {
    for (bool hiRes : {false, true}) {
      p = RateConverter::plan(hz, 240, hiRes);
      TEST_ASSERT_FALSE(p.ok);
      TEST_ASSERT_EQUAL(RateConverter::Refusal::Unsupported, p.refusal);
      TEST_ASSERT_EQUAL_STRING("isn't supported (8-48 kHz, 88.2 and 96 kHz)", p.reason);
    }
  }
}

// 88.2/96 kHz don't play until the device check (docs/RESAMPLER.md,
// section 6, step 5): refused at any CPU speed unless the build turns them
// on (MSTREAM_HIRES_RATES=1, kHiResOn) or the caller is a bench (true).
// Everything up to 48 kHz plays either way.
void test_hi_res_is_off_until_the_device_check() {
  TEST_ASSERT_FALSE(RateConverter::kHiResOn);  // this build: the gate is closed
  for (uint32_t hz : {88200u, 96000u}) {
    for (uint32_t mhz : {240u, 160u}) {
      RateConverter::Plan p = RateConverter::plan(hz, mhz);  // the default: kHiResOn
      TEST_ASSERT_FALSE(p.ok);
      TEST_ASSERT_EQUAL(RateConverter::Refusal::Off, p.refusal);
      p = RateConverter::plan(hz, mhz, false);
      TEST_ASSERT_EQUAL(RateConverter::Refusal::Off, p.refusal);
    }
    RateConverter& c = gConv;
    c.reset();
    TEST_ASSERT_FALSE(c.setRate(hz, 240));
    TEST_ASSERT_TRUE(c.refused());
    TEST_ASSERT_EQUAL(RateConverter::Refusal::Off, c.currentPlan().refusal);
    c.reset();
    TEST_ASSERT_TRUE(c.setRate(hz, 240, true));  // the benches
  }
  for (uint32_t hz : kRates) {
    if (hz > 48000) continue;
    TEST_ASSERT_TRUE(RateConverter::plan(hz, 160).ok);
    TEST_ASSERT_TRUE(RateConverter::plan(hz, 160, false).ok);
  }
}

// The UI's note names a refused rate the way people write it, never as a
// supported one (44056 Hz is no "44.1 kHz").
void test_rate_text() {
  char t[16];
  const std::pair<uint32_t, const char*> want[] = {{96000, "96 kHz"},      {88200, "88.2 kHz"},  {176400, "176.4 kHz"},
                                                   {192000, "192 kHz"},    {37800, "37.8 kHz"},  {44056, "44056 Hz"},
                                                   {22050, "22050 Hz"},    {1, "1 Hz"},          {705600, "705.6 kHz"}};
  for (const auto& w : want) {
    RateConverter::rateText(w.first, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING(w.second, t);
  }
  RateConverter::rateText(4294967295u, t, sizeof(t));  // the longest fits its buffer
  TEST_ASSERT_EQUAL_STRING("4294967295 Hz", t);
  char small[4] = "abc";
  RateConverter::rateText(96000, small, sizeof(small));  // cut, never past the end
  TEST_ASSERT_EQUAL_STRING("96 ", small);
}

// The most frames one source frame can make, per route: what RingOutput's
// stage must have room for before it takes a frame.
void test_max_out_per_rate() {
  const std::pair<uint32_t, uint32_t> want[] = {{8000, 6}, {11025, 4}, {12000, 4}, {16000, 3}, {22050, 2}, {24000, 2},
                                                {32000, 2}, {44100, 1}, {48000, 1}, {88200, 1}, {96000, 1}};
  for (const auto& w : want) {
    gConv.reset();
    TEST_ASSERT_TRUE(gConv.setRate(w.first, kCpu, kHiRes));
    TEST_ASSERT_EQUAL_UINT32(w.second, gConv.maxOut());
  }
  gConv.reset();
  TEST_ASSERT_EQUAL_UINT32(1, gConv.maxOut());  // no rate yet: a push only holds the frame
}

void test_a_refused_rate_takes_nothing() {
  RateConverter& c = gConv;
  c.reset();
  TEST_ASSERT_FALSE(c.setRate(192000, kCpu, kHiRes));
  TEST_ASSERT_TRUE(c.refused());
  TEST_ASSERT_EQUAL_UINT32(0, c.maxOut());
  const int16_t s[2] = {1000, 1000};
  int16_t buf[RateConverter::kMaxOut * 2];
  TEST_ASSERT_EQUAL_UINT32(0, c.push(s, buf));
  TEST_ASSERT_EQUAL_UINT64(0, c.taken());
  uint32_t written = 99;
  TEST_ASSERT_EQUAL_UINT32(0, c.process(s, 1, buf, RateConverter::kMaxOut, &written));
  TEST_ASSERT_EQUAL_UINT32(0, written);
  TEST_ASSERT_TRUE(c.finished());
  TEST_ASSERT_FALSE(c.setRate(0, kCpu, kHiRes));  // still refused
  TEST_ASSERT_FALSE(c.setRate(96000, 160, true));
}

// ---- Bit-exactness and counts ----

void test_passthrough_is_bit_exact() {
  const Frames in = noise(5000, 1, 32768);
  assertSame(in, convert(gConv, 44100, in));
  TEST_ASSERT_TRUE(gConv.passthrough());
  TEST_ASSERT_EQUAL_UINT32(0, gConv.clamped());
}

// Every route against a direct convolution at each output instant: the
// phases, the mirrored rows, the cascades, the delay, the rounding, the
// saturation and the count, for lengths around every stage's delay.
void test_every_route_matches_an_independent_reference() {
  uint32_t seed = 10;
  for (uint32_t hz : kRates) {
    for (size_t n : {0, 1, 2, 23, 24, 25, 47, 48, 49, 61, 62, 63, 100, 1001, 4801}) {
      for (int amp : {12000, 32768}) {  // moderate, and full-scale noise that saturates
        const Frames in = noise(n, ++seed, amp);
        const Frames got = convert(gConv, hz, in);
        assertSame(reference(hz, in), got);
        TEST_ASSERT_EQUAL_UINT64(RateConverter::plan(hz, kCpu, kHiRes).ringFrames(n), got.size() / 2);
      }
    }
  }
}

void test_exact_counts_for_any_length() {
  std::mt19937 rng(5);
  for (uint32_t hz : kRates) {
    const RateConverter::Plan p = RateConverter::plan(hz, kCpu, kHiRes);
    std::vector<size_t> lengths;
    for (size_t n = 0; n <= 130; ++n) lengths.push_back(n);
    for (int i = 0; i < 12; ++i) lengths.push_back(rng() % 12000);
    for (size_t n : lengths) {
      const Frames out = convert(gConv, hz, Frames(n * 2, 0));
      TEST_ASSERT_EQUAL_UINT64(p.ringFrames(n), out.size() / 2);
      TEST_ASSERT_EQUAL_UINT64(n, gConv.taken());
      TEST_ASSERT_EQUAL_UINT64(p.ringFrames(n), gConv.produced());
    }
  }
}

// Long runs through RingOutput's rule with a ring that is often full: the
// exact count, and the same bits as a run that was never refused.
void test_long_runs_through_a_full_ring_are_exact() {
  uint32_t seed = 100;
  for (uint32_t hz : kRates) {
    const double seconds = hz == 48000 ? 60.0 : 10.0;
    const size_t n = static_cast<size_t>(hz * seconds) + 7;
    Frames in(n * 2);
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / hz;
      in[2 * i] = sat16(std::lround(12000 * std::sin(2 * kPi * 997 * t) + 3000 * std::sin(2 * kPi * 3119 * t)));
      in[2 * i + 1] = sat16(static_cast<int64_t>((i * 2654435761u) >> 20 & 4095) - 2048);
    }
    const Frames once = convert(gConv, hz, in);
    const Frames streamed = streamThroughRing(gConv2, hz, in, ++seed);
    TEST_ASSERT_EQUAL_UINT64(RateConverter::plan(hz, kCpu, kHiRes).ringFrames(n), once.size() / 2);
    assertSame(once, streamed);
  }
}

void test_output_does_not_depend_on_chunking() {
  std::mt19937 rng(9);
  for (uint32_t hz : kRates) {
    const Frames in = noise(6000, hz, 20000);
    const Frames once = convert(gConv, hz, in);
    RateConverter& c = gConv2;
    c.reset();
    TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
    Frames out;
    Frames buf(2 * 64);
    size_t at = 0;
    while (at < in.size() / 2) {
      const uint32_t frames = std::min<uint32_t>(1 + rng() % 300, static_cast<uint32_t>(in.size() / 2 - at));
      const uint32_t room = rng() % 4 == 0 ? rng() % RateConverter::kMaxOut : rng() % 64;  // sometimes too small to take any
      uint32_t written = 0;
      const uint32_t before = c.maxOut();
      const uint32_t taken = c.process(&in[2 * at], frames, buf.data(), room, &written);
      TEST_ASSERT_TRUE(written <= room);
      if (room < before) TEST_ASSERT_EQUAL_UINT32(0, taken);  // no room for a whole frame's output: nothing taken
      append(out, buf.data(), written);
      at += taken;
    }
    int16_t tail[RateConverter::kMaxOut * 2];
    while (!c.finished()) append(out, tail, c.finishPush(tail));
    assertSame(once, out);
  }
}

// ---- Level: never louder ----

void test_dc_is_exact_at_every_rate() {
  for (uint32_t hz : kRates) {
    for (int v : {32767, -32768, 12345, -1, 1}) {
      const Frames out = convert(gConv, hz, Frames(4000 * 2, static_cast<int16_t>(v)));
      const size_t frames = out.size() / 2;
      for (size_t i = 400; i + 400 < frames; ++i) {  // past the filters' step at each end
        TEST_ASSERT_EQUAL_INT16(v, out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(v, out[2 * i + 1]);
      }
    }
  }
}

void test_silence_gives_exact_zeros() {
  for (uint32_t hz : kRates) {
    const Frames out = convert(gConv, hz, Frames(3000 * 2, 0));
    TEST_ASSERT_TRUE(!out.empty());
    for (int16_t s : out) TEST_ASSERT_EQUAL_INT16(0, s);
    TEST_ASSERT_EQUAL_UINT32(0, gConv.clamped());
  }
}

// The worst input for the 48 kHz table's largest row: every tap's sign at
// full scale. The exact sum is far past int16 (and past one int32 sum); the
// output saturates and keeps its sign.
void test_the_worst_input_saturates_and_never_wraps() {
  int best = 0;
  int64_t bestSum = 0;
  for (int p = 0; p < resampler::kD147Stored; ++p) {
    int64_t s = 0;
    for (int j = 0; j < K; ++j) s += std::abs(resampler::kD147[p][j]);
    if (s > bestSum) {
      bestSum = s;
      best = p;
    }
  }
  // Output n = best uses row p = (n * 160) mod 147 = best when n * 13 = best (mod 147);
  // find n, and the window under it: inputs floor(n * 160 / 147) + 24 - 47 .. + 24.
  uint64_t n = 0;
  while (static_cast<int>((n * 160) % 147) != best) ++n;
  const int64_t newest = static_cast<int64_t>(n * 160 / 147) + K / 2;
  for (int sign : {1, -1}) {
    Frames in(static_cast<size_t>(newest + 200) * 2, 0);
    for (int j = 0; j < K; ++j) {
      const int c = resampler::kD147[best][j];
      const int16_t v = (c >= 0) == (sign > 0) ? 32767 : -32768;
      in[2 * (newest - (K - 1) + j)] = v;
      in[2 * (newest - (K - 1) + j) + 1] = v;
    }
    const Frames out = convert(gConv, 48000, in);
    TEST_ASSERT_EQUAL_INT16(sign > 0 ? 32767 : -32768, out[2 * n]);
    TEST_ASSERT_EQUAL_INT16(sign > 0 ? 32767 : -32768, out[2 * n + 1]);
    TEST_ASSERT_TRUE(gConv.clamped() > 0);
    assertSame(reference(48000, in), out);
  }
  TEST_ASSERT_TRUE(bestSum * 32767 > INT32_MAX);
}

// A full-scale square overshoots (Gibbs) at every rate: clamped, the right
// sign throughout each plateau, and the same as the 64-bit reference.
void test_full_scale_square_saturates_at_every_rate() {
  for (uint32_t hz : kRates) {
    const Frames in = square(hz, hz / 4);
    const Frames out = convert(gConv, hz, in);
    if (hz != 44100) TEST_ASSERT_TRUE(gConv.clamped() > 0);
    for (size_t i = 0; i < out.size() / 2; ++i) {
      const double t = i / kOut * 2000;  // half-periods
      const double phase = t - std::floor(t);
      if (phase < 0.35 || phase > 0.65) continue;  // mid-plateau only
      const int want = (static_cast<long>(t) & 1) ? -1 : 1;
      TEST_ASSERT_TRUE(out[2 * i] * want > 8000);  // a wrap would flip the sign
    }
    assertSame(reference(hz, in), out);
  }
}

// ---- State ----

void test_reset_leaves_nothing_behind() {
  const Frames loud = noise(5000, 77, 32768);
  const Frames quiet = noise(3000, 78, 3000);
  for (uint32_t first : {96000u, 88200u, 8000u, 48000u}) {
    for (uint32_t hz : kRates) {
      const Frames fresh = convert(gConv2, hz, quiet);
      RateConverter& c = gConv;
      c.reset();
      c.setRate(first, kCpu, kHiRes);
      int16_t buf[RateConverter::kMaxOut * 2];
      for (size_t i = 0; i + 1 < loud.size(); i += 2) c.push(&loud[i], buf);  // stopped mid-stream
      c.setMono(true);
      // convert() resets: the same output as a converter that never saw `loud`
      assertSame(fresh, convert(c, hz, quiet));
      TEST_ASSERT_EQUAL_UINT32(gConv2.clamped(), c.clamped());
    }
  }
  RateConverter& c = gConv;
  c.reset();
  TEST_ASSERT_FALSE(c.configured());
  TEST_ASSERT_EQUAL_UINT32(0, c.rate());
  TEST_ASSERT_EQUAL_UINT64(0, c.taken());
  TEST_ASSERT_EQUAL_UINT64(0, c.produced());
  TEST_ASSERT_TRUE(c.finished());
}

void test_mono_is_stereo_with_equal_channels() {
  for (uint32_t hz : kRates) {
    Frames in = noise(3000, hz + 1, 25000);
    Frames dup = in;
    for (size_t i = 0; i < dup.size(); i += 2) dup[i + 1] = dup[i];
    const Frames mono = convert(gConv, hz, in, true);  // in[1] ignored
    assertSame(convert(gConv2, hz, dup), mono);
    for (size_t i = 0; i < mono.size(); i += 2) TEST_ASSERT_EQUAL_INT16(mono[i], mono[i + 1]);
  }
}

void test_channels_are_independent() {
  for (uint32_t hz : kRates) {
    Frames in = noise(2000, hz + 2, 25000);
    for (size_t i = 0; i < in.size(); i += 2) in[i + 1] = 0;
    const Frames out = convert(gConv, hz, in);
    for (size_t i = 0; i < out.size(); i += 2) TEST_ASSERT_EQUAL_INT16(0, out[i + 1]);
  }
}

// An MP3 says SetChannels(2) at begin() and SetChannels(1) after its first
// samples: switching never resets the filters.
void test_a_channel_change_never_resets() {
  for (uint32_t hz : {48000u, 96000u, 8000u, 22050u}) {
    Frames in = noise(4000, hz + 3, 20000);
    Frames dup = in;  // what the stream is once mono: L in both channels
    for (size_t i = 2 * 1500; i < 2 * 2500; i += 2) dup[i + 1] = dup[i];
    RateConverter& c = gConv;
    c.reset();
    c.setRate(hz, kCpu, kHiRes);
    Frames out;
    int16_t buf[RateConverter::kMaxOut * 2];
    for (size_t f = 0; f < in.size() / 2; ++f) {
      if (f == 1500) c.setMono(true);
      if (f == 2500) c.setMono(false);
      append(out, buf, c.push(&in[2 * f], buf));
    }
    while (!c.finished()) append(out, buf, c.finishPush(buf));
    const Frames want = convert(gConv2, hz, dup);
    // Left: exactly as if nothing happened. Right: the stereo run with L = R while mono,
    // except that mono outputs copy the left (they don't mix old right history in).
    for (size_t i = 0; i < want.size(); i += 2) TEST_ASSERT_EQUAL_INT16(want[i], out[i]);
    const size_t settle = 400;  // well past every filter's span after each switch
    const RateConverter::Plan p = RateConverter::plan(hz, kCpu, kHiRes);
    const size_t back = static_cast<size_t>(p.ringFrames(2500)) + settle;
    for (size_t i = 2 * back; i < want.size(); i += 2) TEST_ASSERT_EQUAL_INT16(want[i + 1], out[i + 1]);
  }
}

// ESP8266Audio's real order: an MP3 hands over its constructor's {0,0}, then
// its first decoded sample, and only then SetRate(). The frames are held and
// replayed: the same output as setting the rate first, at every rate.
void test_frames_before_the_rate_are_kept() {
  for (uint32_t hz : kRates) {
    Frames in = noise(3000, hz + 4, 20000);
    in[0] = in[1] = 0;  // the constructor's {0,0}
    const Frames want = convert(gConv2, hz, in);
    RateConverter& c = gConv;
    c.reset();
    Frames out;
    int16_t buf[RateConverter::kMaxOut * 2];
    for (size_t f = 0; f < in.size() / 2; ++f) {
      if (f == 2) TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
      const uint32_t m = c.maxOut();
      const uint32_t n = c.push(&in[2 * f], buf);
      TEST_ASSERT_TRUE(n <= m);
      append(out, buf, n);
    }
    while (!c.finished()) append(out, buf, c.finishPush(buf));
    assertSame(want, out);
    // ...and through the full ring, with refusals.
    assertSame(want, streamThroughRing(gConv, hz, in, hz, 2));
  }
}

void test_a_stream_that_never_says_its_rate_is_44k() {
  RateConverter& c = gConv;
  const Frames in = noise(100, 6, 20000);
  c.reset();
  Frames out;
  int16_t buf[RateConverter::kMaxOut * 2];
  for (size_t f = 0; f < 100; ++f) {
    const uint32_t m = c.maxOut();
    const uint32_t n = c.push(&in[2 * f], buf);
    TEST_ASSERT_TRUE(n <= m);
    append(out, buf, n);
  }
  TEST_ASSERT_TRUE(c.passthrough());
  assertSame(in, out);
  // A short one ends before the hold is full: the tail plays it.
  c.reset();
  out.clear();
  for (size_t f = 0; f < 3; ++f) TEST_ASSERT_EQUAL_UINT32(0, c.push(&in[2 * f], buf));
  TEST_ASSERT_FALSE(c.finished());
  TEST_ASSERT_TRUE(c.maxOut() >= 3);
  while (!c.finished()) append(out, buf, c.finishPush(buf));
  assertSame(Frames(in.begin(), in.begin() + 6), out);
}

// A FLAC seek sets the same rate again (then hands over a {0,0}): nothing changes.
void test_the_same_rate_again_changes_nothing() {
  for (uint32_t hz : kRates) {
    const Frames in = noise(3000, hz + 5, 20000);
    const Frames want = convert(gConv2, hz, in);
    RateConverter& c = gConv;
    c.reset();
    c.setRate(hz, kCpu, kHiRes);
    Frames out;
    int16_t buf[RateConverter::kMaxOut * 2];
    for (size_t f = 0; f < in.size() / 2; ++f) {
      if (f == 1000) {
        TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
        TEST_ASSERT_TRUE(c.setRate(0, kCpu, kHiRes));  // FLAC before its header: ignored
      }
      append(out, buf, c.push(&in[2 * f], buf));
    }
    while (!c.finished()) append(out, buf, c.finishPush(buf));
    assertSame(want, out);
  }
}

// Another rate mid-stream starts the filters again: from there on, the same
// as a fresh converter at the new rate.
void test_a_rate_change_mid_stream_restarts() {
  const Frames a = noise(2000, 41, 20000), b = noise(3000, 42, 20000);
  for (uint32_t from : {48000u, 44100u, 8000u}) {
    for (uint32_t to : {96000u, 22050u, 44100u, 48000u}) {
      if (from == to) continue;
      const Frames want = convert(gConv2, to, b);
      RateConverter& c = gConv;
      c.reset();
      c.setRate(from, kCpu, kHiRes);
      int16_t buf[RateConverter::kMaxOut * 2];
      for (size_t f = 0; f < a.size() / 2; ++f) c.push(&a[2 * f], buf);
      TEST_ASSERT_TRUE(c.setRate(to, kCpu, kHiRes));
      TEST_ASSERT_EQUAL_UINT64(0, c.taken());
      Frames out;
      for (size_t f = 0; f < b.size() / 2; ++f) append(out, buf, c.push(&b[2 * f], buf));
      while (!c.finished()) append(out, buf, c.finishPush(buf));
      assertSame(want, out);
    }
  }
}

// ---- Quality ----

// The passband: tones up to its edge come out within 0.005 dB of the input.
void test_the_passband_is_flat() {
  for (uint32_t hz : kRates) {
    if (hz == 44100) continue;
    const double edge = hz > 44100 ? 20000.0 : 0.875 * hz / 2;
    for (double frac : {0.005, 0.1, 0.5, 0.75, 0.9, 1.0}) {
      const Tone t = measure(hz, std::max(50.0, frac * edge), -1);
      TEST_ASSERT_TRUE(std::fabs(t.ampDb) < 0.005);
    }
  }
}

// THD+N (20 Hz-20 kHz) at -1 dBFS and the largest spur, per rate, a couple of dB
// inside what was measured (docs/RESAMPLER.md); the second tone is 10 kHz, or the
// passband's edge when that is lower. For comparison, a 16-bit sine made at
// 44.1 kHz measures -97.5 dB.
void test_thd_n_and_spurs() {
  struct Limit {
    uint32_t hz;
    double thdn1k, thdnHigh, spur;
  };
  const Limit limits[] = {
      {8000, -81, -80, -85},    {11025, -92, -81, -81},  {12000, -82, -79, -82},  {16000, -80, -83, -84},
      {22050, -92, -82, -82},   {24000, -83, -82, -90},  {32000, -82, -78, -80},  {48000, -83.5, -83.5, -94},
      {88200, -93, -93, -105},  {96000, -83.5, -83.5, -94},
  };
  for (const Limit& l : limits) {
    const double high = std::min(10000.0, l.hz > 44100 ? 10000.0 : 0.875 * l.hz / 2);
    const Tone a = measure(l.hz, 1000, -1), b = measure(l.hz, high, -1);
    char msg[128];
    snprintf(msg, sizeof(msg), "%u Hz: THD+N %.1f / %.1f dB, spurs %.1f / %.1f dB", static_cast<unsigned>(l.hz), a.thdnDb,
             b.thdnDb, a.spurDb, b.spurDb);
    TEST_ASSERT_TRUE_MESSAGE(a.thdnDb < l.thdn1k && b.thdnDb < l.thdnHigh, msg);
    TEST_ASSERT_TRUE_MESSAGE(a.spurDb < l.spur && b.spurDb < l.spur, msg);
  }
}

// Content the output can't hold: what it leaves in 20 Hz-20 kHz.
void test_aliasing_of_content_above_22k() {
  struct Case {
    uint32_t hz;
    double f, limit;
  };
  const Case cases[] = {
      {48000, 20000, -91},   // in band, near the edge: no alias
      {48000, 22000, -85},   // just under 22.05 kHz
      {48000, 23000, -34},   // the accepted trade-off: 22.05-24 kHz lands at 18-20 kHz
      {96000, 30000, -87},   {96000, 40000, -85}, {96000, 47000, -88},
      {88200, 30000, -79},   {88200, 40000, -95}, {88200, 24100, -73},
  };
  for (const Case& k : cases) {
    const Tone t = measure(k.hz, k.f, -1, false);
    char msg[96];
    snprintf(msg, sizeof(msg), "%u Hz, a %.0f Hz tone: %.1f dB at %.0f Hz", static_cast<unsigned>(k.hz), k.f, t.spurDb, t.spurHz);
    TEST_ASSERT_TRUE_MESSAGE(t.spurDb < k.limit, msg);
  }
}

// Output frame n sits at source time n / 44100: a tone's phase matches the
// ideal to within a few nanoseconds, so positions and resume starts stay exact.
void test_time_alignment() {
  for (uint32_t hz : kRates) {
    for (double f : {100.0, 1000.0}) {
      const Tone t = measure(hz, f, -1);
      TEST_ASSERT_TRUE(std::fabs(t.offsetUs) < 0.05);
    }
  }
}

// ---- The block path and the kernel ----

// convert() over random block sizes gives the same bits as one push() per
// frame, writes no more than maxOutFor(n), and clamps the same samples;
// in stereo and mono, at full scale (the saturation too).
void test_convert_blocks_match_push() {
  std::mt19937 rng(41);
  std::vector<int16_t> buf(2 * (300 * 6 + RateConverter::kMaxOut));
  for (uint32_t hz : kRates) {
    for (bool mono : {false, true}) {
      const Frames in = noise(5000, hz + 5, 32767);
      const Frames once = convert(gConv, hz, in, mono);
      RateConverter& c = gConv2;
      c.reset();
      TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
      c.setMono(mono);
      TEST_ASSERT_EQUAL_UINT32(c.maxOut(), c.maxOutFor(1));
      Frames out;
      size_t at = 0;
      const size_t frames = in.size() / 2;
      while (at < frames) {
        const uint32_t n = std::min<uint32_t>(1 + rng() % 300, static_cast<uint32_t>(frames - at));
        const uint32_t bound = c.maxOutFor(n);
        TEST_ASSERT_EQUAL_UINT32(c.perFrameMax() * n, bound);
        const uint32_t k = c.convert(&in[2 * at], n, buf.data());
        TEST_ASSERT_TRUE(k <= bound);
        append(out, buf.data(), k);
        at += n;
      }
      int16_t tail[RateConverter::kMaxOut * 2];
      while (!c.finished()) append(out, tail, c.finishPush(tail));
      assertSame(once, out);
      TEST_ASSERT_EQUAL_UINT32(gConv.clamped(), c.clamped());
      TEST_ASSERT_EQUAL_UINT64(frames, c.taken());
    }
  }
}

// The kernel's own interface: the C kernel (the specification) is the
// direct 64-bit sum rounded half up, for every stored row forwards and
// backwards, at full scale and at each row's worst case. The self-test the
// firmware runs at boot and in Rb compares the fast kernel with it (on the
// host both are the C kernel, and there is none to turn on).
void test_the_kernel_and_its_self_test() {
  std::mt19937 rng(9);
  alignas(4) int16_t window[K + 2];
  int16_t* x = window + 1;  // as the converter's windows: 2 bytes past a 4-byte boundary
  struct Table {
    const int16_t (*rows)[K];
    int count;
  };
  for (const Table& tb : {Table{resampler::kD147, resampler::kD147Stored}, Table{resampler::kU12, resampler::kU12Stored}}) {
    for (int r = 0; r < tb.count; ++r) {
      const int16_t* row = tb.rows[r];
      for (int rev = 0; rev < 2; ++rev) {
        for (int w = 0; w < 12; ++w) {
          int64_t sum = 0;
          for (int j = 0; j < K; ++j) {
            const int16_t c = rev ? row[K - 1 - j] : row[j];
            x[j] = w == 0 ? (c >= 0 ? 32767 : -32768) : w == 1 ? (c >= 0 ? -32768 : 32767) : static_cast<int16_t>(rng());
            sum += static_cast<int64_t>(c) * x[j];
          }
          const int64_t want = floorDiv32768(sum + 16384);
          TEST_ASSERT_EQUAL_INT64(want, rev ? RateConverter::dotRevC(row + K, x) : RateConverter::dotC(row, x));
          TEST_ASSERT_EQUAL_INT64(want, rev ? RateConverter::dotRevFast(row + K, x) : RateConverter::dotFast(row, x));
        }
      }
    }
  }
  const RateConverter::SelfTest t = RateConverter::kernelSelfTest(12345, 4);
  TEST_ASSERT_EQUAL_UINT32((resampler::kD147Stored + resampler::kU12Stored) * 2 * 6, t.dots);
  TEST_ASSERT_EQUAL_UINT32(0, t.mismatches);
  TEST_ASSERT_FALSE(RateConverter::fastKernelBuilt());
  RateConverter::useFastKernel(true);
  TEST_ASSERT_FALSE(RateConverter::fastKernel());
  RateConverter::useFastKernel(false);
}

// The tables read from a copy (the firmware copies them into internal RAM):
// the same bits, from the next setRate() on; the hook that makes the copy
// is called before a stream at another rate is configured, never for
// 44.1 kHz.
int gWanted = 0;
alignas(4) int16_t gD147Copy[resampler::kD147Stored][K];
alignas(4) int16_t gU12Copy[resampler::kU12Stored][K];

void test_tables_from_a_copy() {
  std::memcpy(gD147Copy, resampler::kD147, sizeof(gD147Copy));
  std::memcpy(gU12Copy, resampler::kU12, sizeof(gU12Copy));
  const Frames in = noise(3000, 61, 30000);
  RateConverter::setTablesWanted([] { ++gWanted; });
  gWanted = 0;
  const Frames pass = convert(gConv, 44100, in);
  TEST_ASSERT_EQUAL_INT(0, gWanted);
  for (uint32_t hz : {48000u, 8000u, 22050u}) {
    const Frames flash = convert(gConv, hz, in);
    TEST_ASSERT_FALSE(RateConverter::tablesCopied());
    RateConverter::useTables(gD147Copy, gU12Copy);
    TEST_ASSERT_TRUE(RateConverter::tablesCopied());
    gWanted = 0;
    assertSame(flash, convert(gConv, hz, in));
    TEST_ASSERT_EQUAL_INT(1, gWanted);
    RateConverter::useTables(nullptr, nullptr);
  }
  RateConverter::setTablesWanted(nullptr);
  TEST_ASSERT_FALSE(RateConverter::tablesCopied());
  TEST_ASSERT_EQUAL_UINT32(in.size(), pass.size());
}

void test_it_is_small() {
  TEST_ASSERT_TRUE(sizeof(RateConverter) <= 2048);  // internal RAM, inside RingOutput (~1.9 KB on the ESP32)
  TEST_ASSERT_TRUE(RateConverter::kMaxOut <= 16);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_row_sums_to_unity);
  RUN_TEST(test_the_partial_sums_cannot_overflow);
  RUN_TEST(test_the_filters_responses);
  RUN_TEST(test_plans_and_refusals);
  RUN_TEST(test_hi_res_is_off_until_the_device_check);
  RUN_TEST(test_rate_text);
  RUN_TEST(test_max_out_per_rate);
  RUN_TEST(test_a_refused_rate_takes_nothing);
  RUN_TEST(test_passthrough_is_bit_exact);
  RUN_TEST(test_every_route_matches_an_independent_reference);
  RUN_TEST(test_exact_counts_for_any_length);
  RUN_TEST(test_long_runs_through_a_full_ring_are_exact);
  RUN_TEST(test_output_does_not_depend_on_chunking);
  RUN_TEST(test_dc_is_exact_at_every_rate);
  RUN_TEST(test_silence_gives_exact_zeros);
  RUN_TEST(test_the_worst_input_saturates_and_never_wraps);
  RUN_TEST(test_full_scale_square_saturates_at_every_rate);
  RUN_TEST(test_reset_leaves_nothing_behind);
  RUN_TEST(test_mono_is_stereo_with_equal_channels);
  RUN_TEST(test_channels_are_independent);
  RUN_TEST(test_a_channel_change_never_resets);
  RUN_TEST(test_frames_before_the_rate_are_kept);
  RUN_TEST(test_a_stream_that_never_says_its_rate_is_44k);
  RUN_TEST(test_the_same_rate_again_changes_nothing);
  RUN_TEST(test_a_rate_change_mid_stream_restarts);
  RUN_TEST(test_the_passband_is_flat);
  RUN_TEST(test_thd_n_and_spurs);
  RUN_TEST(test_aliasing_of_content_above_22k);
  RUN_TEST(test_time_alignment);
  RUN_TEST(test_convert_blocks_match_push);
  RUN_TEST(test_the_kernel_and_its_self_test);
  RUN_TEST(test_tables_from_a_copy);
  RUN_TEST(test_it_is_small);
  return UNITY_END();
}
