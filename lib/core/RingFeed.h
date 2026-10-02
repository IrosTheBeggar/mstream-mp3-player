// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "PcmRing.h"
#include "RateConverter.h"

// The decode side of the ring: what the decoder (or a built-in track) hands
// over, converted to 44.1 kHz (RateConverter) and staged, then pushed into
// the PcmRing. The ring always holds 44.1 kHz, whatever the track's rate, so
// an output switch mid-track can't change the speed (docs/RESAMPLER.md,
// section 5). Portable, host-tested; the firmware's RingOutput wraps it as
// an ESP8266Audio AudioOutput.
//
// It never blocks. The rule that keeps the generators' "ring full: return
// false, offer the same sample again" contract: a source frame is taken
// only when the stage has room for everything it can make (maxOut()), after
// pushing the stage into the ring if need be; otherwise it is refused whole
// and nothing is lost or duplicated. The per-pass budget caps both sides:
// source frames, so a pass's decode work is as before, and ring frames, so
// a low rate's pass doesn't convert 5.5 times as much (8 kHz makes 441/80
// ring frames per source frame) above the UI loop.
//
// Counters: made() is ring frames (44.1 kHz) since reset(), the units of
// positions, durations and the decode load. The decoders' own seeks stay in
// their source units.
//
// Three paths for the generator's one-frame-at-a-time calls
// (docs/RESAMPLER.md, section 10):
// - 44.1 kHz, the passthrough (every normal track): the frame goes straight
//   into the stage, as the old RingOutput did, inline, with no converter
//   call and no 64-bit counting per frame;
// - another rate, the block path: frames are held in a small block and
//   converted kBlockFrames at a time (RateConverter::convert()). A frame is
//   taken only while the stage has room for the worst case of every frame
//   held plus it (perFrameMax() each), so the rule above holds: what is
//   taken always fits, nothing is lost or duplicated. commit(), flush and
//   finish() convert what is held first;
// - before the rate is known (an MP3's first frames), or refused: one at a
//   time through the converter, as before.
//
// One task (the decode task). ~3.2 KB, all in the object: keep it in
// internal RAM (the converter reads its histories 96 times per output).
class RingFeed {
  // Pass: 44.1 kHz with nothing waiting in the converter. Block: another
  // rate, configured. Hold: no rate yet (the converter holds the frames).
  // Refused: nothing is taken.
  enum class Mode : uint8_t { Hold, Pass, Block, Refused };

public:
  static constexpr uint32_t kStageFrames = 256;
  // Source frames held for the block path before they are converted.
  static constexpr uint32_t kBlockFrames = 32;

  explicit RingFeed(PcmRing& ring) : ring_(ring) {}

  // A new stream: nothing staged, the converter's history zeroed, no rate
  // yet. Every start, stop, skip and seek, right after the ring's
  // discardAll(), so nothing of the last track can come out after it.
  // `cpuMhz`: the speed the CPU was set to at boot (88.2/96 kHz need 240).
  // `hiRes`: whether 88.2/96 kHz play at all (RateConverter::kHiResOn; the
  // benches pass true).
  void reset(uint32_t cpuMhz, bool hiRes = RateConverter::kHiResOn);

  // ---- the generator's side (AudioOutput) ----
  // The stream's rate. <= 0 is ignored (FLAC before its header); the same
  // rate again changes nothing. False: refused (rejected(), refusal()):
  // nothing more is taken and the caller fails the track.
  bool setRate(int hz);
  // 1: mono (the left channel, copied). Never resets anything.
  void setChannels(int channels);
  // A test: every rate the stream says is taken as `hz` (a 44.1 kHz file
  // decoded and converted as a 48 kHz one would be); 0: off. reset() turns
  // it off.
  void forceRate(int hz) { forced_ = hz; }
  // A pass: the generator may hand over up to `frames` source frames, and
  // none once the pass has made `frames` ring frames (the last one taken may
  // add up to maxOut() - 1 more; frames held for the block path count at
  // the most they can make until they are converted). Then its loop()
  // returns. At 44.1 kHz the source frames are the ring frames: the
  // passthrough counts only budget_ per frame, and its frames come off the
  // ring budget when they are counted (countPassed()), so the pass's cap
  // holds across a rate change into or out of 44.1 kHz too.
  void setBudget(uint32_t frames) {
    countPassed();  // (the last pass's, if it had no commit(): not this one's)
    budget_ = frames;
    ringBudget_ = frames;
  }
  uint32_t budgetLeft() const { return budget_; }
  // One source frame; false: not taken (refused rate, budget spent, or the
  // ring full), the generator keeps it and offers it again.
  bool consume(const int16_t sample[2]) {
    if (mode_ == Mode::Pass) {  // 44.1 kHz: the old RingOutput's path
      if (budget_ == 0) return false;
      if (staged_ == kStageFrames) {
        commit();
        if (staged_ == kStageFrames) return false;
      }
      storeFrame(stage_ + 2 * staged_, sample[0], mono_ ? sample[0] : sample[1]);
      ++staged_;
      ++passed_;
      --budget_;
      return true;
    }
    if (mode_ == Mode::Block && accept_ != 0 && budget_ != 0) {  // (accept_ keeps to the ring budget too)
      storeFrame(held_ + 2 * heldN_, sample[0], sample[1]);
      ++heldN_;
      --accept_;
      --budget_;
      return true;
    }
    return consumeSlow(sample);
  }

  // ---- a block (the built-in tracks, the bench) ----
  // Takes as many of `frames` source frames as fit (no budget), making at
  // most `maxMade` ring frames; returns how many. The caller keeps the rest
  // and offers them again.
  uint32_t write(const int16_t* frames, uint32_t n, uint32_t maxMade = UINT32_MAX);

  // Converts what is held, then pushes staged frames into the ring; true
  // when nothing is left staged.
  bool commit();
  // The end of the stream: pushes the converter's tail (its last K/2
  // frames' worth) through the same rule. True once all of it is in the
  // ring; false: the ring is full, call again later.
  bool finish();

  // Ring space a pass of `srcFrames` source frames may need, with what is
  // staged (before the rate is known: at the largest ratio, 8 kHz's).
  uint32_t roomFor(uint32_t srcFrames) const;

  // ---- gapless joins (docs/GAPLESS.md sections 3.2 and 5.1) ----
  // The next track can follow in the same stream: its frames go on through
  // the converter as if the two files were one (no reset, no tail). Or,
  // at another rate, the tail goes in (finish()) and a new stream starts
  // at the same ring position (restartStream()). Either way the feed as it
  // was at the end of the first track is kept (mark()), so that what was
  // decoded after it can be taken back out of the ring (PcmRing::cutBack())
  // and the feed put back as it was (rewind()), the same bits as if the
  // cut track had never been fed.
  struct Mark {
    RateConverter conv;
    uint64_t made = 0;
    int rate = 0;
    int forced = 0;
    uint32_t perFrame = 1;
    Mode mode = Mode::Hold;
    bool mono = false;
  };
  // The feed now, into `m` (~2 KB: the firmware keeps two in PSRAM). Only
  // with nothing staged or held (commit() or finish() returned true): false
  // otherwise, `m` untouched.
  bool mark(Mark* m) const;
  // Back to `m`: nothing staged or held, the converter and the counters as
  // they were. The converter's table pointers are kept as they were too:
  // the firmware never frees the tables' copy while a track decoded ahead
  // could be cut (Core2AudioBackend, RESAMPLER.md section 10c).
  void rewind(const Mark& m);
  // Whether a stream at `hz` can carry on from here as one stream: the
  // converter configured at that rate (not refused, not forced by a test).
  // Never after finish(), which the caller knows: its tail is in the ring
  // (restartStream() then).
  bool continues(int hz) const {
    return hz > 0 && forced_ == 0 && conv_.configured() && rate_ == hz;
  }
  // The ring frames the converter still owes for what it took
  // (RateConverter::tailFrames()): a stream that carries on has its next
  // source frame that many frames past what is in the ring (0 at the
  // passthrough). With nothing staged or held.
  uint32_t tailFrames() const { return conv_.tailFrames(); }
  // After finish() (nothing staged): a new stream from here, as reset() but
  // with made() running on and the test's forced rate kept.
  void restartStream();

  // The bench: committed frames are dropped instead of written.
  void setDiscard(bool discard) { discard_ = discard; }

  int rate() const { return rate_; }  // the source's, 0 until known
  bool rejected() const { return conv_.refused(); }
  // Why the rate was refused ("isn't supported (...)"), "" when it wasn't.
  const char* refusal() const { return conv_.refused() ? conv_.currentPlan().reason : ""; }
  RateConverter::Refusal refusalKind() const {
    return conv_.refused() ? conv_.currentPlan().refusal : RateConverter::Refusal::None;
  }
  // Ring frames made since reset() (staged or in the ring). Frames held for
  // the block path aren't made until commit() (or finish()) converts them.
  uint64_t made() const { return made_ + staged_; }
  const RateConverter& converter() const { return conv_; }
  // The bench's own runs (the kernel check, Rb): RingOutput's converter,
  // with nothing playing (reset() after).
  RateConverter& benchConverter() { return conv_; }

private:
  // One frame into the stage or the block (4-byte aligned): a single 32-bit
  // store on the ESP32, without the PSRAM workaround's MEMW after each
  // 16-bit one (both are in internal RAM: RingOutput stays under 4 KB for
  // that).
  static void storeFrame(int16_t* d, int16_t l, int16_t r) {
#if defined(__XTENSA__)
    const uint32_t v = static_cast<uint16_t>(l) | static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16;
    asm volatile("s32i %[v], %[d], 0" : : [v] "r"(v), [d] "r"(d) : "memory");
#else
    d[0] = l;
    d[1] = r;
#endif
  }

  bool consumeSlow(const int16_t sample[2]);
  // The block path's room: converts what is held, pushes the stage into the
  // ring if a block's worst case doesn't fit, and sets how many frames may
  // be taken next (accept_), at the most each can make, within the stage's
  // room and the pass's ring budget. False: not even one (the ring is full,
  // or the ring budget is spent).
  bool reserve();
  // Converts the held frames into the stage (their room was kept for them).
  void convertHeld();
  // The passthrough frames copied here, counted in the converter and
  // against the pass's ring budget.
  void countPassed() {
    if (passed_ == 0) return;
    conv_.countPassthrough(passed_);
    ringBudget_ -= passed_ < ringBudget_ ? passed_ : ringBudget_;
    passed_ = 0;
  }
  void updateMode();
  // Room in the stage for the converter's next push(), pushing the stage
  // into the ring first if it hasn't.
  bool room();

  PcmRing& ring_;
  RateConverter conv_;
  alignas(4) int16_t stage_[kStageFrames * 2];
  alignas(4) int16_t held_[kBlockFrames * 2];
  Mode mode_ = Mode::Hold;
  bool mono_ = false;
  bool discard_ = false;
  bool hiRes_ = RateConverter::kHiResOn;
  uint32_t staged_ = 0;
  uint32_t heldN_ = 0;
  uint32_t accept_ = 0;    // frames the block path may take before reserve() again
  uint32_t perFrame_ = 1;  // the route's perFrameMax()
  uint32_t passed_ = 0;    // passthrough frames not yet counted in conv_
  uint32_t budget_ = 0;
  uint32_t ringBudget_ = 0;
  uint32_t cpuMhz_ = 0;
  int rate_ = 0;
  int forced_ = 0;
  uint64_t made_ = 0;      // frames that have left the stage (to the ring, or dropped)
};
