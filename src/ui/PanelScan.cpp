// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/PanelScan.h"

#include <M5Unified.h>

#include <algorithm>
#include <climits>
#include <esp_timer.h>
#include <freertos/semphr.h>

#include "app/Psram.h"
#include "ui/Gfx.h"

namespace ui {

void sleepUntil(int64_t atUs) {
  static SemaphoreHandle_t woke = nullptr;
  static esp_timer_handle_t timer = nullptr;
  static bool made = false;
  if (!made) {
    made = true;
    woke = xSemaphoreCreateBinary();
    if (woke) {
      esp_timer_create_args_t args = {};
      args.callback = [](void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); };
      args.arg = woke;
      args.dispatch_method = ESP_TIMER_TASK;
      args.name = "list frame";
      if (esp_timer_create(&args, &timer) != ESP_OK) timer = nullptr;
    }
  }
  const int64_t wait = atUs - esp_timer_get_time();
  if (wait < 150) return;
  if (!timer) {
    if (wait >= 1000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    return;
  }
  xSemaphoreTake(woke, 0);  // (one given after a wait that timed out)
  if (esp_timer_start_once(timer, static_cast<uint64_t>(wait)) != ESP_OK) return;
  if (xSemaphoreTake(woke, pdMS_TO_TICKS(wait / 1000 + 4)) != pdTRUE) esp_timer_stop(timer);
}

namespace {
constexpr int kSamples = 240;       // ~60 ms: four refreshes at 70 Hz (PanelScan keeps the count in a byte)
static_assert(kSamples <= 0xFF, "PanelScan::samples_ is a byte");
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
  samples_ = static_cast<uint8_t>(n);  // (kSamples at most)
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
    if (i < n && wraps_ < 0xFF) ++wraps_;
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

// ---- a picture put in place, whole ----

namespace {
// What a picture's hold takes on the bus is known between two figures, and
// lib/core ScanSync::rows() takes both. At the quickest the wire's own
// speed: 16 bits a pixel at the bus's 40 MHz, 2/5 us (a full-width line
// 128 us: a whole band's 168 take 21.5 ms). At the slowest 7/16 us a pixel
// (140 us for that line: ListView's figure for it, the hold's share in
// it), 8 us a line besides (a narrow picture's line has the pushing's own
// cost to carry) and 100 us a hold.
constexpr uint32_t kQuickPxFifthsUs = 2;
constexpr uint32_t kSlowPxSixteenthsUs = 7;
constexpr uint32_t kSlowLineUs = 8;
constexpr uint32_t kSlowHoldUs = 100;
// The sleep for the window ends this long before it: the bus is taken and
// the rest waited out there (the scan read again: neither the sleep's end
// nor the bus's wait is exact).
constexpr int32_t kWholeSpinUs = 250;
// ... and the most that is waited out with the bus held.
constexpr int32_t kWholeSpinMaxUs = 600;
// A hold that took over twice its slowest and this: something cut in.
constexpr uint32_t kCutSlackUs = 1500;
}  // namespace

PanelScan::WholeStats PanelScan::takeWhole() {
  const WholeStats s = whole_;
  whole_ = WholeStats{};
  return s;
}

void PanelScan::noteWhole(bool atOnce, bool met, bool cut) {
  if (whole_.pictures < 0xFFFF) ++whole_.pictures;
  if (atOnce) {
    if (whole_.atOnce < 0xFFFF) ++whole_.atOnce;
  } else if (met && whole_.met < 0xFFFF) {
    ++whole_.met;
  }
  if (cut && whole_.cut < 0xFFFF) ++whole_.cut;
}

Whole::Whole(PanelScan* scan, int y, int lines, int widthPx, bool wait) : scan_(scan) {
  // No scan to go by, or inside a hold already (a move's fixed parts go out
  // under the move's own): the push as it is.
  if (!scan_ || !scan_->ok() || !scan_->sync().panelKnown() || lines <= 0 || widthPx <= 0 || gfx::dark() ||
      M5.Display.getStartCount() > 0) {
    scan_ = nullptr;
    return;
  }
  const ScanSync& sync = scan_->sync();
  const uint32_t n = static_cast<uint32_t>(lines), w = static_cast<uint32_t>(widthPx);
  const uint32_t quickestUs = n * w * kQuickPxFifthsUs / 5u;
  slowestUs_ = n * (w * kSlowPxSixteenthsUs / 16u + kSlowLineUs) + kSlowHoldUs;
  int line = scan_->line();
  int64_t readUs = esp_timer_get_time();
  if (line < 0) {  // (a panel asleep, or a read that made no sense)
    scan_ = nullptr;
    return;
  }
  ScanSync::Rows r = sync.rows(line, y, lines, quickestUs, slowestUs_);
  counted_ = true;
  atOnce_ = !wait;
  y_ = static_cast<int16_t>(y);
  lines_ = static_cast<int16_t>(lines);
  // The writing begins (the caller's push is next): when, and where the
  // scan is then (the line last read, and what has gone by since).
  auto begins = [&] {
    beganUs_ = esp_timer_get_time();
    const ScanSync::Panel& p = sync.panel();
    line0_ = static_cast<int16_t>((line + (beganUs_ - readUs) * p.lines / p.frameUs) % p.lines);
  };
  // As before (uiw0): no wait, each run in its own hold, the loop's own
  // priority. So is a picture too tall for its hold to stay clear of the
  // scan: a hold that long is the card's wait too (ui/Gfx: no hold over a
  // band's lines).
  if (!scan_->wholeOn() || !r.clean) {
    begins();
    return;
  }
  // From here to the picture's last row nothing below the speaker's pump
  // runs on this core.
  was_ = uxTaskPriorityGet(nullptr);
  if (was_ < kHoldPriority) {
    vTaskPrioritySet(nullptr, kHoldPriority);
    boosted_ = true;
  }
  if (!wait) {
    // The caller's frames come first (a list that moves): at once, where
    // the scan is.
    lock_.emplace(&gfx::holds());
    begins();
    return;
  }
  bool missed = false;
  for (int round = 0;; ++round) {
    if (r.waitUs > kWholeSpinUs) sleepUntil(readUs + r.waitUs - kWholeSpinUs);
    lock_.emplace(&gfx::holds());
    const int now = scan_->line();
    if (now < 0) {  // (unreadable now: it goes as it is)
      missed = true;
      break;
    }
    line = now;
    readUs = esp_timer_get_time();
    r = sync.rows(line, y, lines, quickestUs, slowestUs_);
    if (r.waitUs <= kWholeSpinMaxUs) break;
    // The window went by while the bus was another's (the decoder's read
    // of the card) or the sleep ran over: its next opening, once.
    if (round == 1) {
      missed = true;
      break;
    }
    lock_.reset();
  }
  if (!missed && r.waitUs > 0) {
    const int64_t at = readUs + r.waitUs;
    while (esp_timer_get_time() < at) {
    }
  }
  begins();
}

Whole::~Whole() {
  const int64_t took = counted_ ? esp_timer_get_time() - beganUs_ : 0;
  lock_.reset();
  if (boosted_) vTaskPrioritySet(nullptr, was_);
  if (!counted_) return;
  // What the scan saw of it, by how long the writing really took.
  const uint32_t us = static_cast<uint32_t>(std::min<int64_t>(took, INT32_MAX));
  scan_->noteWhole(atOnce_, scan_->sync().met(line0_, y_, lines_, us),
                   took > static_cast<int64_t>(slowestUs_) * 2 + kCutSlackUs);
}

}  // namespace ui
