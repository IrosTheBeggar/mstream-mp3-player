#include "TouchCheck.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace touchcheck {

namespace {

// (No hypot() here: it would pull more of newlib into the Core2's IRAM.)
float dist(float dx, float dy) { return std::sqrt(dx * dx + dy * dy); }

int round5(float v) { return static_cast<int>(std::lround(v / 5.0f)) * 5; }

// At the panel's clamp on its axis (`max`: kRawMaxX or kRawMaxY).
bool atClamp(int raw, int max) { return TouchCalibration::clampedLow(raw) || raw >= max; }

}  // namespace

int offBy(const Dot& d, const Tap& t) {
  return static_cast<int>(std::lround(dist(static_cast<float>(t.x - d.x), static_cast<float>(t.y - d.y))));
}

bool askAgain(const Dot& d, const Tap& t) { return offBy(d, t) > kAskAgainPx; }

Verdict verdict(const Tap* taps, int n) {
  Verdict v;
  if (!taps || n <= 0) return v;
  if (n > kDots) n = kDots;
  int far = 0, off = 0;
  bool clamped = false;
  float sumDx = 0, sumDy = 0;
  for (int i = 0; i < n; ++i) {
    const int e = offBy(kDot[i], taps[i]);
    if (e > kFarPx) ++far;
    if (e > kOffPx) ++off;
    if (taps[i].clamped && kDot[i].x >= kEdgeMarginPx && kDot[i].x <= TouchCalibration::kRawMaxX - kEdgeMarginPx) {
      clamped = true;
    }
    sumDx += static_cast<float>(taps[i].x - kDot[i].x);
    sumDy += static_cast<float>(taps[i].y - kDot[i].y);
  }
  v.calibrate = far > 0 || off >= 2 || clamped;
  // The way they land: the axis the taps lean along on average; how far:
  // the furthest of them that way (the right side is where the lab's panel
  // is off the most, and that is the number the listener feels).
  const bool alongX = std::fabs(sumDx) >= std::fabs(sumDy);
  const float lean = alongX ? sumDx : sumDy;
  float most = 0;
  for (int i = 0; i < n; ++i) {
    const float d = alongX ? static_cast<float>(taps[i].x - kDot[i].x) : static_cast<float>(taps[i].y - kDot[i].y);
    const float along = lean >= 0 ? d : -d;
    if (along > most) most = along;
  }
  if (lean == 0 || most <= 0) return v;
  v.dir = alongX ? (lean > 0 ? Dir::Right : Dir::Left) : (lean > 0 ? Dir::Below : Dir::Above);
  v.px = round5(most);
  if (v.px < 5) v.px = 5;
  return v;
}

void verdictText(const Verdict& v, char* buf, size_t size) {
  if (!buf || size == 0) return;
  const char* way = v.dir == Dir::Right   ? "to the right of"
                    : v.dir == Dir::Left  ? "to the left of"
                    : v.dir == Dir::Below ? "below"
                    : v.dir == Dir::Above ? "above"
                                          : nullptr;
  if (!way) {
    std::snprintf(buf, size, "Taps land away from your finger.");
    return;
  }
  std::snprintf(buf, size, "Taps land about %d px %s your finger.", v.px, way);
}

bool due(bool calibrated, bool answered) { return !calibrated && !answered; }

bool answers(CheckEnd end) {
  switch (end) {
    case CheckEnd::Skip:
    case CheckEnd::NotNow:
    case CheckEnd::Calibrate:
    case CheckEnd::GoOn:
    case CheckEnd::Calibrated: return true;
    case CheckEnd::TimedOut:
    case CheckEnd::Cancelled:
    case CheckEnd::Closed: return false;
  }
  return false;
}

Take judgeCross(CrossTries& tries, int tx, int ty, int rawX, int rawY, int x, int y) {
  if ((x < kCancelW && y < kCancelH) || (x < kAHintW && y >= kAHintY)) return Take::Cancel;
  if (std::abs(rawX - tx) <= kAcceptDx && std::abs(rawY - ty) <= kAcceptDy) return Take::Sample;
  if (tries.misses >= 2 && std::abs(rawX - tries.lastX) <= kAgreePx && std::abs(rawY - tries.lastY) <= kAgreePx &&
      std::abs(rawX - tx) <= kAgreeMaxPx && std::abs(rawY - ty) <= kAgreeMaxPx) {
    return Take::Sample;
  }
  ++tries.misses;
  tries.lastX = static_cast<int16_t>(rawX);
  tries.lastY = static_cast<int16_t>(rawY);
  return Take::Miss;
}

Error measure(const TouchCalibration& table, const TouchCalibration::Sample* sx, const TouchCalibration::Sample* sy,
              int n) {
  Error e;
  if (!sx || !sy || n <= 0) return e;
  float sum = 0;
  for (int i = 0; i < n; ++i) {
    const float d = dist(table.x.map(sx[i].raw) - sx[i].target, table.y.map(sy[i].raw) - sy[i].target);
    sum += d;
    if (d > e.max) e.max = d;
  }
  e.mean = sum / static_cast<float>(n);
  return e;
}

Error measureUnseen(const TouchCalibration& inUse, const TouchCalibration::Sample* sx,
                    const TouchCalibration::Sample* sy, int n) {
  Error e;
  if (!sx || !sy || n <= 1 || n > kCrosses) return e;
  TouchCalibration::Sample ox[kCrosses], oy[kCrosses];
  // Each tap's leave-one-out miss per axis, whether it read a clamp.
  float dx[kCrosses], dy[kCrosses];
  bool cx[kCrosses], cy[kCrosses];
  float innerUnseen = 0, innerNow = 0;
  int inner = 0;
  for (int k = 0; k < n; ++k) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
      if (i == k) continue;
      ox[m] = sx[i];
      oy[m] = sy[i];
      ++m;
    }
    // (A fit that fails leaves that axis uncorrected.)
    TouchCalibration t = TouchCalibration::identity();
    TouchCalibration::fitAxis(ox, m, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots, &t.x);
    TouchCalibration::fitAxis(oy, m, TouchCalibration::kYKnotRaw, TouchCalibration::kYKnots, &t.y);
    dx[k] = t.x.map(sx[k].raw) - sx[k].target;
    dy[k] = t.y.map(sy[k].raw) - sy[k].target;
    cx[k] = atClamp(sx[k].raw, TouchCalibration::kRawMaxX);
    cy[k] = atClamp(sy[k].raw, TouchCalibration::kRawMaxY);
    if (!cx[k] && !cy[k]) {
      innerUnseen += dist(dx[k], dy[k]);
      innerNow += dist(inUse.x.map(sx[k].raw) - sx[k].target, inUse.y.map(sy[k].raw) - sy[k].target);
      ++inner;
    }
  }
  // The clamped readings are judged on the fit only on a panel the taps
  // inside the clamps show is off (the new table clearly better there).
  const bool skewed = inner > 0 && innerUnseen <= innerNow - kMinGainPx * static_cast<float>(inner);
  TouchCalibration all = TouchCalibration::identity();
  if (skewed) {
    TouchCalibration::fitAxis(sx, n, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots, &all.x);
    TouchCalibration::fitAxis(sy, n, TouchCalibration::kYKnotRaw, TouchCalibration::kYKnots, &all.y);
  }
  float sum = 0;
  for (int k = 0; k < n; ++k) {
    const float ex = skewed && cx[k] ? all.x.map(sx[k].raw) - sx[k].target : dx[k];
    const float ey = skewed && cy[k] ? all.y.map(sy[k].raw) - sy[k].target : dy[k];
    const float d = dist(ex, ey);
    sum += d;
    if (d > e.max) e.max = d;
  }
  e.mean = sum / static_cast<float>(n);
  return e;
}

Outcome outcome(bool fitOk, const Error& now, const Error& unseen) {
  if (!fitOk) return Outcome::Disagree;
  if (unseen.mean <= now.mean - kMinGainPx) return Outcome::Better;
  if (now.mean <= kAccurateMeanPx) return Outcome::Accurate;
  return Outcome::NoBetter;
}

void errorText(const char* what, const Error& e, char* buf, size_t size) {
  if (!buf || size == 0) return;
  std::snprintf(buf, size, "%s: up to %ld px off, average %ld", what ? what : "", std::lround(e.max),
                std::lround(e.mean));
}

}  // namespace touchcheck
