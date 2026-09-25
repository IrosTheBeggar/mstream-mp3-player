#pragma once
#include <cstdint>

// Removes the clicks an output would make when its audio starts, stops or
// jumps: pause and resume, a skip, an output switch, an underrun. Works in
// place on interleaved stereo int16 frames, one output's stream at a time, and
// never allocates, blocks or logs, so the Bluetooth data callback can run it.
//
// Each process() call gets `total` frames, of which the first `valid` are real
// audio and the rest are missing (the ring had nothing). Three things happen:
//   - Real audio is multiplied by a gain that ramps linearly to 1 while open
//     (setOpen(true)) and to 0 while closed (a pause), over `rampFrames`.
//   - When the audio breaks off (missing frames, or cut() for a skip), the
//     last frame that went out is held and ramped to 0 over `rampFrames`, so
//     the output never steps. Real audio after a break fades in from 0 while
//     that held frame fades out: a crossfade from the old track to the new.
//   - At full gain with nothing fading it is bit-exact and costs almost nothing.
//
// Gains are Q15 (32768 = 1). With rampFrames a power of two (the default 64,
// ~1.5 ms at 44.1 kHz) the ramps are exact and a crossfade's two gains always
// add up to exactly 1, so it never overshoots.
class Declicker {
public:
  static constexpr uint32_t kDefaultRampFrames = 64;
  static constexpr int32_t kUnity = 32768;

  // Missing frames it takes a held frame to reach 0, for a given ramp length:
  // how much room to leave after the real audio in an output's buffer.
  static constexpr uint32_t decayFramesFor(uint32_t rampFrames) {
    return static_cast<uint32_t>((kUnity + stepFor(rampFrames) - 1) / stepFor(rampFrames));
  }

  explicit Declicker(uint32_t rampFrames = kDefaultRampFrames);

  // Open: real audio ramps to full level. Closed: it ramps to silence (pause).
  void setOpen(bool open) { open_ = open; }
  bool isOpen() const { return open_; }

  // The next real frame doesn't follow on from the last one (skip): hold the
  // last output frame and crossfade from it.
  void cut();
  // Forget everything: the listener has heard silence (a new Bluetooth link).
  void reset();

  // How many of the next `count` frames should be real audio. All of them
  // while open; while closed, only those still needed to finish the fade-out
  // (0 once it's done), so a pause leaves the rest in the ring for resume.
  uint32_t framesWanted(uint32_t count) const;
  // Missing frames still needed before the output is 0 (the decay after a
  // break); 0 when it already is.
  uint32_t decayFrames() const;
  // Nothing audible is left: no fade in progress and no real audio at a
  // non-zero gain. An output may stop sending frames.
  bool silent() const { return (!live_ || gain_ == 0) && decayFrames() == 0; }

  // In place: `frames` holds `total` interleaved stereo frames, the first
  // `valid` of them real. The rest are overwritten (decay, then zeros).
  void process(int16_t* frames, uint32_t valid, uint32_t total);

private:
  static constexpr int32_t stepFor(uint32_t rampFrames) {
    return rampFrames == 0 || rampFrames >= static_cast<uint32_t>(kUnity)
               ? (rampFrames == 0 ? kUnity : 1)
               : kUnity / static_cast<int32_t>(rampFrames);
  }
  void startTail();

  const int32_t step_;
  bool open_ = true;
  // Real audio is flowing without a break since the last fade-in started.
  bool live_ = false;
  int32_t gain_ = 0;      // on real audio
  int32_t tailGain_ = 0;  // on the held frame
  int16_t tailL_ = 0, tailR_ = 0;
  int16_t lastL_ = 0, lastR_ = 0;  // the last frame that went out
};
