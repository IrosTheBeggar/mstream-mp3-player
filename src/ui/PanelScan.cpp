// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/PanelScan.h"

#include <M5Unified.h>
#include <esp_timer.h>

#include "app/Psram.h"

namespace ui {

namespace {
constexpr int kSamples = 240;       // ~60 ms: four refreshes at 70 Hz
constexpr uint32_t kGapUs = 240;      // between two reads: ~4 scan lines
constexpr int32_t kMaxLine = 1100;    // no panel here has more
constexpr uint32_t kMinFrameUs = 5000, kMaxFrameUs = 40000;
constexpr uint32_t kSlowReadUs = 150; // a read that waited for the bus isn't a sample
}  // namespace

bool PanelScan::calibrate() {
  ok_ = false;
  samples_ = wraps_ = 0;
  auto& d = M5.Display;
  // The line at each read's midpoint, a read every ~1/55 of a refresh over
  // about four refreshes.
  struct Buffers {
    int16_t line[kSamples];
    uint32_t at[kSamples];
  };
  auto* buffers = static_cast<Buffers*>(psramAlloc(sizeof(Buffers)));
  if (!buffers) return false;
  struct Free {
    Buffers* b;
    ~Free() { psramFree(b); }
  } freeIt{buffers};
  int16_t* line = buffers->line;
  uint32_t* at = buffers->at;
  const int64_t t0 = esp_timer_get_time();
  int n = 0;
  for (int i = 0; i < kSamples; ++i) {
    const int64_t a = esp_timer_get_time();
    const int32_t v = d.getScanLine();
    const int64_t b = esp_timer_get_time();
    if (v >= 0 && v <= kMaxLine && b - a <= kSlowReadUs) {
      line[n] = static_cast<int16_t>(v);
      at[n] = static_cast<uint32_t>((a + b) / 2 - t0);
      ++n;
    }
    delayMicroseconds(kGapUs);
  }
  samples_ = static_cast<uint32_t>(n);
  if (n < kSamples / 2) return false;
  // Runs of climbing lines, each ending at a wrap. A run's rate from its
  // ends; a run's start as the time its line 0 would have been read.
  double rateSum = 0;  // lines a microsecond
  int runs = 0;
  double firstZero = 0, lastZero = 0;
  int start = 0;
  for (int i = 1; i <= n; ++i) {
    const bool wrap = i == n || line[i] < line[i - 1];
    if (!wrap) continue;
    const int last = i - 1;
    // A whole run only (not the piece before the first wrap, nor after the last).
    const bool whole = start > 0 && i < n;
    if (whole && line[last] - line[start] >= 60 && at[last] > at[start]) {
      const double rate = static_cast<double>(line[last] - line[start]) / (at[last] - at[start]);
      const double zero = at[start] - line[start] / rate;
      if (runs == 0) firstZero = zero;
      lastZero = zero;
      rateSum += rate;
      ++runs;
    }
    if (i < n) ++wraps_;
    start = i;
  }
  if (runs < 2) return false;
  const double rate = rateSum / runs;
  const double frameUs = (lastZero - firstZero) / (runs - 1);
  if (frameUs < kMinFrameUs || frameUs > kMaxFrameUs) return false;
  const int lines = static_cast<int>(rate * frameUs + 0.5);
  const int rows = static_cast<int>(d.height());
  if (lines < rows || lines > rows + 64) return false;
  // Every sample where that refresh says it should be (a few lines' slack:
  // the read's own time, the porches).
  int off = 0;
  for (int i = 0; i < n; ++i) {
    double expect = (at[i] - firstZero) * rate;
    expect -= static_cast<long>(expect / lines) * static_cast<double>(lines);
    if (expect < 0) expect += lines;
    double diff = line[i] - expect;
    if (diff > lines / 2) diff -= lines;
    if (diff < -lines / 2) diff += lines;
    if (diff > 6 || diff < -6) ++off;
  }
  if (off > n / 10) return false;
  ScanSync::Panel p;
  p.frameUs = static_cast<uint32_t>(frameUs + 0.5);
  p.lines = lines;
  p.firstRow = (lines - rows) / 2;  // the porches split evenly (2 and 2 on the ILI9342C)
  sync_.setPanel(p);
  ok_ = true;
  return true;
}

int PanelScan::line() {
  if (!ok_) return -1;
  const int32_t v = M5.Display.getScanLine();
  return v >= 0 && v < sync_.panel().lines ? static_cast<int>(v) : -1;
}

void PanelScan::print() const {
  if (!ok_) {
    Serial.printf("[ui] panel scan: not readable (%lu samples, %lu wraps): list frames on the frame clock\n",
                  (unsigned long)samples_, (unsigned long)wraps_);
    return;
  }
  const ScanSync::Panel& p = sync_.panel();
  Serial.printf("[ui] panel scan: %d lines a refresh, %lu us (%.1f Hz), row 0 at line %d: list frames follow it\n",
                p.lines, (unsigned long)p.frameUs, 1e6 / p.frameUs, p.firstRow);
}

}  // namespace ui
