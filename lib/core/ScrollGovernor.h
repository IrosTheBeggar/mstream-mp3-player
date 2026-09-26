#pragma once
#include <cstdint>

// How hard a scrolling list may use the SPI bus it shares with the SD card,
// from how much audio the decoder has buffered (PcmRing, ~1.5 s when full)
// and whether an output just ran dry. The tab bar spec (§10.2): lists redraw
// at up to 15 fps while they move, and back off when the audio is at risk.
//
//   Normal     full frame rate (normalFrameMs)
//   Reduced    fewer frames (reducedFrameMs): the ring is below reducedBelowMs,
//              or an underrun happened in the last holdMs
//   WholeRows  a few frames a second (wholeRowFrameMs), and the list only
//              moves in whole rows: below wholeRowsBelowMs
//   Paused     no frames at all until the ring recovers: below pauseBelowMs
//
// Getting worse is immediate; getting better goes one level at a time, once
// the ring has stayed recoverMarginMs above that level's threshold (and no
// underrun came) for holdMs. With no audio playing nothing is at risk and
// the level is Normal. Portable: the caller passes the numbers and the time.
class ScrollGovernor {
public:
  enum class Level : uint8_t { Normal, Reduced, WholeRows, Paused };

  struct Config {
    bool enabled = true;  // false: always Normal (stress tests without the safety net)
    uint32_t normalFrameMs = 66;
    uint32_t reducedFrameMs = 125;
    uint32_t wholeRowFrameMs = 250;
    uint32_t reducedBelowMs = 900;
    uint32_t wholeRowsBelowMs = 500;
    uint32_t pauseBelowMs = 250;
    uint32_t recoverMarginMs = 200;
    uint32_t holdMs = 2000;
  };

  struct Budget {
    Level level = Level::Normal;
    uint32_t frameMs = 66;  // at most one frame per this long
    bool wholeRows = false; // snap what's drawn to whole rows while moving
    bool draw = true;       // false: skip frames
  };

  ScrollGovernor() = default;
  explicit ScrollGovernor(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }
  void reset() {
    level_ = Level::Normal;
    primed_ = false;
  }

  // Call every loop pass (or every frame). `underruns` is the outputs'
  // running count; `audioActive` whether audio should be flowing.
  Budget update(uint32_t nowMs, uint32_t ringMs, uint32_t underruns, bool audioActive);
  Level level() const { return level_; }
  static const char* name(Level l);

private:
  Level fromRing(uint32_t ringMs, uint32_t margin) const;
  Budget budget() const;

  Config config_;
  Level level_ = Level::Normal;
  uint32_t lastBadMs_ = 0;
  uint32_t lastUnderruns_ = 0;
  bool primed_ = false;
};
