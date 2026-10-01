// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The knobs of TouchCalibration::fitAxis(). The knots are in raw space; the
// fit finds their values.
struct TouchFitOptions {
  // Weight of the smoothness term: the change of slope at each inner knot,
  // scaled so that two 40 px segments whose slopes differ by 1 cost as much
  // as a 40 px error on one sample, times this.
  float smooth = 0.3f;
  // Pull of every knot towards the identity (keeps knots with no samples
  // near them sensible), per knot, as a fraction of one sample's weight.
  float identity = 0.02f;
  // Every segment's slope stays within [minSlope, maxSlope]: monotonic.
  float minSlope = 0.25f;
  float maxSlope = 4.0f;
};

// Where the finger really was, from where the Core2's touch panel says it
// was. Panels differ: the default is no correction at all (identity()), and
// the calibration screen (ui/CalibrationScreen) fits a table to the owner's
// own taps. The one panel measured, the user's (the input lab's target
// practice, thumb and index finger alike, so it's the sensor), reads x too
// far right, more so the further right the finger is: about 0 at x 60-150,
// about +20 px at x 190, +35-45 px from x 240 on, and it saturates at 319
// (a target at x 273 read 302-319, one at 299 read 319). At the left edge
// it saturates at 0 (a target at x 27 read 0). y is right. labFitX() is
// that panel's table: fitAxis() on those logs (the host tests check the two
// agree, and use it as a skewed panel: Axis::unmap()).
//
// The correction is one monotonic piecewise-linear table per axis: knots at
// raw positions, each with the true position it stands for; in between,
// linear; outside the knots, slope 1 (the offset of the end knot). A
// clamped reading can't say how far past the clamp the finger was: the lab
// fit puts raw 319 at about x 282, the average of where the fingers that
// read 319 were aimed. So a control at the right edge needs a hit area that
// reaches the screen's edge and at least ~40 px wide, or a check of the
// saturation flag (clampedHighX(), InputEvent::atRightEdge()); and two
// controls side by side at the right need their split well right of the
// left one's centre, so that an uncalibrated panel like the lab's still
// hits the left one (test_ui_library audits them).
//
// Portable: no clock, no storage; save()/load() give the bytes NVS keeps.
class TouchCalibration {
public:
  static constexpr int kMaxKnots = 12;
  // The raw range the panel reports on the Core2 (x 0-319 on the glass;
  // y 0-239 on the glass, 240-279 on the button strip).
  static constexpr int kRawMaxX = 319;
  static constexpr int kRawMaxY = 279;

  struct Axis {
    uint8_t n = 0;
    int16_t raw[kMaxKnots] = {};  // strictly increasing
    float value[kMaxKnots] = {};  // strictly increasing: the table is monotonic

    // The true position for a raw reading.
    float map(float r) const;
    // The raw reading for a true position: map()'s inverse (the table is
    // monotonic). A skewed panel for tests: what it reads for a finger.
    float unmap(float v) const;
    // Knots strictly increasing on both sides, 2..kMaxKnots of them, values
    // finite and within -64..kRawMaxX+64.
    bool valid() const;
    bool operator==(const Axis& o) const;
  };

  // A tap on a target: what the panel read, and where the target was.
  struct Sample {
    int16_t raw;
    int16_t target;
  };

  // How well a table fits some samples (px).
  struct FitReport {
    int samples = 0;
    float rmsBefore = 0, maxBefore = 0;  // the raw readings against the targets
    float rmsAfter = 0, maxAfter = 0;    // the table's
  };

  // The fit's knobs (TouchFitOptions, above).
  using FitOptions = TouchFitOptions;

  // The knots the fits use: x every 40 px (9), y every 60 (5).
  static constexpr int kXKnots = 9;
  static constexpr int kYKnots = 5;
  static const int16_t kXKnotRaw[kXKnots];
  static const int16_t kYKnotRaw[kYKnots];

  // The default, until a table is saved: no correction (identity()).
  static TouchCalibration defaults();
  // No correction at all.
  static TouchCalibration identity();
  // The x table fitted to the input lab's logs (the user's panel): a test
  // fixture, and the console's skewed scripted finger (its unmap()).
  static Axis labFitX();

  // A monotonic table for the knots `knotRaw` (strictly increasing, 2 to
  // kMaxKnots) that fits the samples in the least-squares sense, smoothed
  // (options above). False (out untouched) if the knots are bad or there
  // are no samples. `report` (optional) compares before and after.
  static bool fitAxis(const Sample* samples, int n, const int16_t* knotRaw, int knots, Axis* out,
                      FitReport* report = nullptr, const FitOptions& options = FitOptions{});
  // How `axis` does on `samples` (the before figures are the raw readings').
  static FitReport evaluate(const Axis& axis, const Sample* samples, int n);

  // Corrected screen coordinates, rounded (x clamped to 0..kRawMaxX, y to
  // 0..kRawMaxY).
  int mapX(int rawX) const;
  int mapY(int rawY) const;
  // The panel's reading is at its limit: the finger may be further out.
  static bool clampedLow(int raw) { return raw <= 0; }
  static bool clampedHighX(int rawX) { return rawX >= kRawMaxX; }

  bool valid() const { return x.valid() && y.valid(); }
  bool operator==(const TouchCalibration& o) const { return x == o.x && y == o.y; }
  bool operator!=(const TouchCalibration& o) const { return !(*this == o); }

  // The table as bytes (for NVS): "TCAL", version, the knots of both axes,
  // an FNV-1a checksum. save() returns the size written (0 if `cap` is too
  // small: kMaxBlob is always enough); load() accepts only an intact, valid
  // table of this version and leaves *this untouched otherwise.
  static constexpr size_t kMaxBlob = 8 + 2 * kMaxKnots * 6 + 4;
  size_t save(uint8_t* buf, size_t cap) const;
  bool load(const uint8_t* buf, size_t len);

  Axis x, y;
};
