#include "TouchCalibration.h"

#include <cmath>
#include <cstring>

const int16_t TouchCalibration::kXKnotRaw[kXKnots] = {0, 40, 80, 120, 160, 200, 240, 280, 319};
const int16_t TouchCalibration::kYKnotRaw[kYKnots] = {0, 60, 120, 180, 239};

namespace {

// fitAxis() on the input lab's target practice (targets_thumb.log and
// targets_index.log: 72 taps on 10 targets, thumb and index finger; the
// "last list row" taps left out, as that target spans the whole width),
// rounded to 0.1 px: labFitX(), no longer the default (other panels read
// differently). test_touch_input checks that the fit still gives these.
constexpr float kLabX[TouchCalibration::kXKnots] = {25.6f, 43.2f, 78.5f, 122.7f, 154.4f,
                                                        181.9f, 212.3f, 253.1f, 281.3f};

constexpr uint8_t kMagic[4] = {'T', 'C', 'A', 'L'};
constexpr uint8_t kVersion = 1;

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

void put16(uint8_t* p, int16_t v) {
  const auto u = static_cast<uint16_t>(v);
  p[0] = static_cast<uint8_t>(u);
  p[1] = static_cast<uint8_t>(u >> 8);
}
int16_t get16(const uint8_t* p) { return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8))); }
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

TouchCalibration::Axis makeAxis(const int16_t* raw, const float* value, int n) {
  TouchCalibration::Axis a;
  a.n = static_cast<uint8_t>(n);
  for (int i = 0; i < n; ++i) {
    a.raw[i] = raw[i];
    a.value[i] = value[i];
  }
  return a;
}

TouchCalibration::Axis identityAxis(const int16_t* raw, int n) {
  TouchCalibration::Axis a;
  a.n = static_cast<uint8_t>(n);
  for (int i = 0; i < n; ++i) {
    a.raw[i] = raw[i];
    a.value[i] = static_cast<float>(raw[i]);
  }
  return a;
}

}  // namespace

float TouchCalibration::Axis::unmap(float v) const {
  if (n == 0) return v;
  if (v <= value[0]) return raw[0] + (v - value[0]);
  if (v >= value[n - 1]) return raw[n - 1] + (v - value[n - 1]);
  int j = 1;
  while (j < n - 1 && v > value[j]) ++j;
  const float a = (v - value[j - 1]) / (value[j] - value[j - 1]);
  return raw[j - 1] + a * static_cast<float>(raw[j] - raw[j - 1]);
}

float TouchCalibration::Axis::map(float r) const {
  if (n == 0) return r;
  if (r <= raw[0]) return value[0] + (r - raw[0]);
  if (r >= raw[n - 1]) return value[n - 1] + (r - raw[n - 1]);
  int j = 1;
  while (j < n - 1 && r > raw[j]) ++j;
  const float a = (r - raw[j - 1]) / static_cast<float>(raw[j] - raw[j - 1]);
  return value[j - 1] + a * (value[j] - value[j - 1]);
}

bool TouchCalibration::Axis::valid() const {
  if (n < 2 || n > kMaxKnots) return false;
  for (int i = 0; i < n; ++i) {
    // Written so that NaN fails too. (No isfinite() or fmax() in this file:
    // on the Core2 they pull newlib's __fpclassifyd into IRAM, which is full.)
    if (!(value[i] >= -64.0f && value[i] <= kRawMaxX + 64.0f)) return false;
    if (i > 0 && (raw[i] <= raw[i - 1] || value[i] <= value[i - 1])) return false;
  }
  return true;
}

bool TouchCalibration::Axis::operator==(const Axis& o) const {
  if (n != o.n) return false;
  for (int i = 0; i < n; ++i) {
    if (raw[i] != o.raw[i] || value[i] != o.value[i]) return false;
  }
  return true;
}

TouchCalibration TouchCalibration::defaults() { return identity(); }

TouchCalibration::Axis TouchCalibration::labFitX() { return makeAxis(kXKnotRaw, kLabX, kXKnots); }

TouchCalibration TouchCalibration::identity() {
  TouchCalibration c;
  c.x = identityAxis(kXKnotRaw, kXKnots);
  c.y = identityAxis(kYKnotRaw, kYKnots);
  return c;
}

// Least squares over the knots' values v, with the slopes bounded. In the
// increments z (z0 = v0, zj = vj - v(j-1)) the bounds are a box, so a
// projected coordinate descent on the normal equations solves it exactly
// (the problem is convex); K is at most 12, so each sweep is ~150 multiply-
// adds, and the samples are folded into the K x K system once.
bool TouchCalibration::fitAxis(const Sample* samples, int n, const int16_t* knotRaw, int knots, Axis* out,
                               FitReport* report, const FitOptions& o) {
  if (!samples || n <= 0 || !knotRaw || knots < 2 || knots > kMaxKnots || !out) return false;
  for (int j = 1; j < knots; ++j) {
    if (knotRaw[j] <= knotRaw[j - 1]) return false;
  }
  const int K = knots;
  double M[kMaxKnots][kMaxKnots] = {};
  double c[kMaxKnots] = {};
  // One row of the least-squares system, given in v (the knots' values):
  // converted to z (v_j is the sum of z_0..z_j, so z_i's coefficient is the
  // sum of the row's v coefficients from i on) and added to the normal
  // equations.
  auto addRow = [&](const double* rowV, double rhs) {
    double rowZ[kMaxKnots];
    double acc = 0;
    for (int i = K - 1; i >= 0; --i) {
      acc += rowV[i];
      rowZ[i] = acc;
    }
    for (int i = 0; i < K; ++i) {
      if (rowZ[i] == 0) continue;
      c[i] += rowZ[i] * rhs;
      for (int k = 0; k < K; ++k) M[i][k] += rowZ[i] * rowZ[k];
    }
  };
  double row[kMaxKnots];
  for (int s = 0; s < n; ++s) {
    for (int i = 0; i < K; ++i) row[i] = 0;
    const double r = samples[s].raw;
    if (r <= knotRaw[0]) {
      row[0] = 1;
    } else if (r >= knotRaw[K - 1]) {
      row[K - 1] = 1;
    } else {
      int j = 1;
      while (j < K - 1 && r > knotRaw[j]) ++j;
      const double a = (r - knotRaw[j - 1]) / (knotRaw[j] - knotRaw[j - 1]);
      row[j - 1] = 1 - a;
      row[j] = a;
    }
    addRow(row, samples[s].target);
  }
  // Smoothness: the change of slope at each inner knot, scaled so that a
  // 40 px segment is the unit.
  const double ws = std::sqrt(static_cast<double>(o.smooth)) * 40.0;
  for (int j = 1; j < K - 1; ++j) {
    for (int i = 0; i < K; ++i) row[i] = 0;
    const double h1 = knotRaw[j] - knotRaw[j - 1];
    const double h2 = knotRaw[j + 1] - knotRaw[j];
    row[j - 1] = ws / h1;
    row[j] = -ws / h1 - ws / h2;
    row[j + 1] = ws / h2;
    addRow(row, 0);
  }
  // A weak pull of each knot towards the identity.
  const double wi = std::sqrt(static_cast<double>(o.identity));
  for (int j = 0; j < K; ++j) {
    for (int i = 0; i < K; ++i) row[i] = 0;
    row[j] = wi;
    addRow(row, wi * knotRaw[j]);
  }

  double z[kMaxKnots], lo[kMaxKnots], hi[kMaxKnots];
  z[0] = knotRaw[0];
  lo[0] = -1e9;
  hi[0] = 1e9;
  for (int j = 1; j < K; ++j) {
    const double h = knotRaw[j] - knotRaw[j - 1];
    lo[j] = o.minSlope * h;
    hi[j] = o.maxSlope * h;
    z[j] = h < lo[j] ? lo[j] : h > hi[j] ? hi[j] : h;  // start from the identity
  }
  for (int sweep = 0; sweep < 20000; ++sweep) {
    double moved = 0;
    for (int i = 0; i < K; ++i) {
      if (M[i][i] <= 0) continue;
      double r = c[i];
      for (int k = 0; k < K; ++k) {
        if (k != i) r -= M[i][k] * z[k];
      }
      double v = r / M[i][i];
      if (v < lo[i]) v = lo[i];
      if (v > hi[i]) v = hi[i];
      const double d = std::fabs(v - z[i]);
      if (d > moved) moved = d;
      z[i] = v;
    }
    if (moved < 1e-7) break;
  }

  Axis a;
  a.n = static_cast<uint8_t>(K);
  double v = 0;
  for (int j = 0; j < K; ++j) {
    v += z[j];
    a.raw[j] = knotRaw[j];
    a.value[j] = static_cast<float>(v);
  }
  if (!a.valid()) return false;
  *out = a;
  if (report) *report = evaluate(a, samples, n);
  return true;
}

TouchCalibration::FitReport TouchCalibration::evaluate(const Axis& axis, const Sample* samples, int n) {
  FitReport r;
  if (!samples || n <= 0) return r;
  double sb = 0, sa = 0;
  for (int i = 0; i < n; ++i) {
    const float before = static_cast<float>(samples[i].raw - samples[i].target);
    const float after = axis.map(samples[i].raw) - samples[i].target;
    sb += static_cast<double>(before) * before;
    sa += static_cast<double>(after) * after;
    if (std::fabs(before) > r.maxBefore) r.maxBefore = std::fabs(before);
    if (std::fabs(after) > r.maxAfter) r.maxAfter = std::fabs(after);
  }
  r.samples = n;
  r.rmsBefore = static_cast<float>(std::sqrt(sb / n));
  r.rmsAfter = static_cast<float>(std::sqrt(sa / n));
  return r;
}

int TouchCalibration::mapX(int rawX) const {
  const long v = std::lround(x.map(static_cast<float>(rawX)));
  return static_cast<int>(v < 0 ? 0 : v > kRawMaxX ? kRawMaxX : v);
}

int TouchCalibration::mapY(int rawY) const {
  const long v = std::lround(y.map(static_cast<float>(rawY)));
  return static_cast<int>(v < 0 ? 0 : v > kRawMaxY ? kRawMaxY : v);
}

size_t TouchCalibration::save(uint8_t* buf, size_t cap) const {
  const size_t size = 8 + static_cast<size_t>(x.n + y.n) * 6 + 4;
  if (!buf || cap < size || !valid()) return 0;
  std::memcpy(buf, kMagic, 4);
  buf[4] = kVersion;
  buf[5] = x.n;
  buf[6] = y.n;
  buf[7] = 0;
  uint8_t* p = buf + 8;
  const Axis* axes[2] = {&x, &y};
  for (const Axis* a : axes) {
    for (int i = 0; i < a->n; ++i) {
      put16(p, a->raw[i]);
      uint32_t bits;
      std::memcpy(&bits, &a->value[i], 4);
      put32(p + 2, bits);
      p += 6;
    }
  }
  put32(p, fnv1a(buf, static_cast<size_t>(p - buf)));
  return size;
}

bool TouchCalibration::load(const uint8_t* buf, size_t len) {
  if (!buf || len < 12 || std::memcmp(buf, kMagic, 4) != 0 || buf[4] != kVersion) return false;
  const int nx = buf[5], ny = buf[6];
  if (nx > kMaxKnots || ny > kMaxKnots) return false;
  const size_t size = 8 + static_cast<size_t>(nx + ny) * 6 + 4;
  if (len != size || get32(buf + size - 4) != fnv1a(buf, size - 4)) return false;
  TouchCalibration c;
  const uint8_t* p = buf + 8;
  Axis* axes[2] = {&c.x, &c.y};
  const int counts[2] = {nx, ny};
  for (int k = 0; k < 2; ++k) {
    axes[k]->n = static_cast<uint8_t>(counts[k]);
    for (int i = 0; i < counts[k]; ++i) {
      axes[k]->raw[i] = get16(p);
      const uint32_t bits = get32(p + 2);
      std::memcpy(&axes[k]->value[i], &bits, 4);
      p += 6;
    }
  }
  if (!c.valid()) return false;
  *this = c;
  return true;
}
