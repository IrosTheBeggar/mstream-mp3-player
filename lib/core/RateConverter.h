// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "ResamplerTables.h"

// 88.2/96 kHz playback: off until a 24/96 FLAC has played 10 minutes on the
// speaker without an underrun (docs/RESAMPLER.md, section 6, step 5). A
// build with -DMSTREAM_HIRES_RATES=1 turns them on, for that test and after
// it. The benches (Rb, b<n>) convert them either way, output dropped.
#ifndef MSTREAM_HIRES_RATES
#define MSTREAM_HIRES_RATES 0
#endif

// Converts a decoded stream at any supported rate to 44.1 kHz, one source
// frame at a time, so the ring between the decoder and the outputs holds one
// rate (docs/RESAMPLER.md). Interleaved stereo int16 in and out.
//
// Routes (exact rational ratios; Q15 polyphase FIRs, tables in flash):
//   44.1 kHz        passthrough, bit-exact
//   48 kHz          147/160
//   96 kHz          halfband /2, then 147/160            (240 MHz only; off, below)
//   88.2 kHz        halfband /2                          (240 MHz only; off, below)
//   22.05, 11.025   x2, x4
//   8, 12, 16, 24   x6, x4, x3, x2 to 48 kHz, then 147/160
//   32 kHz          x3/2 to 48 kHz, then 147/160
// Anything else is refused with a reason.
//
// Never louder: every filter row sums to exactly 32768 (unity DC gain), the
// two int32 partial sums per output can't overflow (each half-row's sum of
// |c| is under 65536), and the output is rounded half up and saturated,
// never wrapped. Silence in gives exact zeros out. Output frame n sits at
// source time n * 44100 / rate exactly: each stage swallows its first K/2
// inputs (its delay), and finishPush() pushes zeros until exactly
// ceil(taken * num / den) frames have come out, dropping any excess.
//
// The ring-full contract: push() takes one source frame whole, history and
// all, and writes 0..maxOut() frames. A caller with less room than maxOut()
// must not push; it keeps the frame and offers it again later. Output never
// depends on how the input is split up.
//
// Before the first setRate() (an MP3 hands over two frames before it knows
// its rate), up to kMaxPending frames are held and replayed through the
// route when the rate arrives; at a converting route they only fill the
// history (every route's delay is longer), at 44.1 kHz they come out ahead
// of the next frame. A stream that never says its rate is taken as 44.1 kHz
// once the hold is full. The same rate again changes nothing; another rate
// mid-stream restarts the filters (a possible click, never louder).
// setMono() never resets: both channels' histories are always written.
//
// Pure: no allocation, no Arduino. ~1.5 KB, all of it in the object; place
// it in internal RAM (docs/RESAMPLER.md, section 5). One task at a time.
class RateConverter {
public:
  static constexpr uint32_t kOutRate = 44100;
  static constexpr uint32_t kHiResMinMhz = 240;   // 88.2/96 kHz below this: refused
  static constexpr bool kHiResOn = MSTREAM_HIRES_RATES != 0;  // 88.2/96 kHz played at all
  static constexpr uint32_t kMaxPending = 8;      // frames held before the rate is known
  static constexpr uint32_t kMaxOut = kMaxPending + 1;  // the most one push() can ever write

  // Why a rate is refused: no route; 88.2/96 kHz while they are off
  // (kHiResOn); 88.2/96 kHz below kHiResMinMhz (a setting would fix it).
  enum class Refusal : uint8_t { None, Unsupported, Off, NeedsCpu };

  // What a source rate takes: a route, or the reason it's refused.
  struct Plan {
    bool ok = false;
    Refusal refusal = Refusal::None;
    const char* reason = "";  // when refused, after the rate: "isn't supported (...)"
    const char* route = "";   // for the log: "halfband /2, then 147/160"
    uint32_t num = 1;         // ring frames per source frame: num / den, exactly
    uint32_t den = 1;
    uint8_t index = 0;        // internal: the route
    // Ring frames for `src` source frames: ceil(src * num / den).
    uint64_t ringFrames(uint64_t src) const { return (src * num + den - 1) / den; }
  };
  // cpuMhz: the CPU setting the user chose, not the clock at this moment
  // (the console can lower it for a while). hiRes: whether 88.2/96 kHz are
  // played at all (the benches pass true).
  static Plan plan(uint32_t srcHz, uint32_t cpuMhz, bool hiRes = kHiResOn);
  // A rate for the UI's note: "96 kHz", "88.2 kHz", "44056 Hz".
  static void rateText(uint32_t hz, char* out, uint32_t size);

  RateConverter() { reset(); }

  // A new stream: no rate, zeroed histories and counters. Nothing of the
  // previous stream can come out after this.
  void reset();
  // The stream's rate. 0 is ignored (FLAC before its header). False when
  // refused: push() then takes nothing and the caller fails the track.
  bool setRate(uint32_t hz, uint32_t cpuMhz, bool hiRes = kHiResOn);
  // Mono: in[1] is ignored, the left channel is converted once and copied.
  void setMono(bool mono) { mono_ = mono; }

  // The most frames the next push() or finishPush() can write.
  uint32_t maxOut() const;
  // Takes one source frame; writes 0..maxOut() frames to `out`.
  uint32_t push(const int16_t in[2], int16_t* out);
  // As many frames of `in` as fit, taking each only while `room` has space
  // for maxOut() more; returns the frames taken, *written the frames out.
  uint32_t process(const int16_t* in, uint32_t frames, int16_t* out, uint32_t room, uint32_t* written);

  // The end of the stream: pushes one frame of the tail (zeros); writes
  // 0..maxOut() frames. Repeat until finished().
  uint32_t finishPush(int16_t* out);
  bool finished() const;

  bool configured() const { return state_ == State::Configured; }
  bool refused() const { return state_ == State::Refused; }
  bool passthrough() const;
  uint32_t rate() const { return rate_; }
  const Plan& currentPlan() const { return plan_; }
  // Since the last reset() or rate change: source frames taken, frames written.
  uint64_t taken() const { return taken_; }
  uint64_t produced() const { return produced_; }
  // Output samples that were clamped to int16 since reset().
  uint32_t clamped() const { return clamped_; }

private:
  enum class State : uint8_t { Unconfigured, Configured, Refused };
  static constexpr int kTaps = resampler::kTaps;
  static constexpr int kMaxHbTaps = resampler::kHb88Taps;

  struct PolyStage {
    const int16_t (*rows)[resampler::kTaps] = nullptr;
    uint16_t tableRows = 0;  // rows in the whole table (147, 12)
    uint16_t step = 1;       // table rows per phase (12 / L for the upsamplers)
    uint16_t L = 1, M = 1;
    uint16_t phase = 0;
    uint16_t lead = 0;
    uint16_t w = 0;
  };
  struct HalfbandStage {
    const int16_t* side = nullptr;
    uint16_t taps = 0;
    uint16_t lead = 0;
    uint16_t w = 0;
    bool skip = false;
  };

  void configure(const Plan& p);
  void restartFilters();
  uint32_t runRoute(int16_t l, int16_t r, int16_t* out);
  uint32_t pushPoly(PolyStage& s, int16_t* h0, int16_t* h1, int16_t l, int16_t r, int16_t* out);
  uint32_t pushHalfband(int16_t l, int16_t r, int16_t* out);
  uint32_t drainCarry(int16_t* out);

  State state_ = State::Unconfigured;
  Plan plan_;
  uint32_t rate_ = 0;
  bool mono_ = false;
  uint8_t stageA_ = 0;  // 0 none (passthrough), 1 polyphase, 2 halfband
  bool thenD147_ = false;
  uint8_t routeMaxOut_ = 1;
  PolyStage polyA_;
  HalfbandStage hb_;
  PolyStage polyB_;     // always 147/160
  uint64_t taken_ = 0;
  uint64_t produced_ = 0;
  uint32_t clamped_ = 0;
  // Frames handed over before the rate was known, and (44.1 kHz) the same
  // frames once replayed, waiting to come out ahead of the next one.
  int16_t pending_[kMaxPending][2];
  int16_t carry_[kMaxPending][2];
  uint8_t pendingN_ = 0;
  uint8_t carryN_ = 0;
  // Planar histories, each written twice so a window is contiguous. Stage A
  // is a halfband (up to 123 taps) or a 48-tap polyphase; stage B is 147/160.
  int16_t histA_[2][2 * kMaxHbTaps];
  int16_t histB_[2][2 * kTaps];
};
