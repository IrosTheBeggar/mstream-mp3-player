#pragma once
#include <cstdint>

// When the UI may take core 1 ahead of the audio decoder (docs/UI-SPIKE.md,
// "Scroll round 2"). On the Core2 the decoder normally runs above the UI
// loop, so a list being dragged gets only what the decoder leaves; but the
// decoder only needs ~40 % of the core and its ring holds ~1.45 s, so for
// the length of an interaction the UI can go first and the ring pays for
// it. The rule, a Schmitt trigger on the ring fill:
//
//   - wanted while the user interacts (finger down, list moving) and for
//     lingerMs after the last sign of it;
//   - on only from resumeMs of ring upwards, and off the moment the ring is
//     below floorMs (so the boost itself can never take the ring below
//     floorMs by more than one check's worth of audio);
//   - after a drop below the floor, off for at least holdOffMs even if the
//     ring is back over resumeMs sooner (fewer, longer on and off periods
//     instead of a flip every few hundred ms);
//   - with nothing playing (stopped, paused) there is nothing to protect:
//     on whenever wanted.
//
// Portable: the caller passes the time and the numbers; apply() is theirs.
class UiBoost {
public:
  struct Config {
    uint32_t floorMs = 900;   // off at once below this much audio buffered
    // On (again) only from this much. Under the ring's steady fill at 48 kHz
    // too (65,536 frames: ~1.32 s full at 48 kHz, ~1.44 s at 44.1 kHz).
    uint32_t resumeMs = 1200;
    uint32_t holdOffMs = 300; // off at least this long after a floor drop
    uint32_t lingerMs = 300;  // still wanted this long after the last activity
  };

  // Why the boost is what it is (for the logs).
  enum class Why : uint8_t {
    Idle,        // off: no interaction (or it ended lingerMs ago)
    Interacting, // on: interaction, ring at or above the floor
    NotPlaying,  // on: interaction, and no audio to protect
    BelowFloor,  // off: interaction, but the ring fell below floorMs
    BelowResume, // off: interaction, the ring is under resumeMs (filling)
  };

  UiBoost() = default;
  explicit UiBoost(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  // `lastActiveMs`: the last time the UI reported activity; `everActive`
  // false if it never did. `playing`: a track is decoding or draining (not
  // stopped, not paused). Returns whether the boost is on now.
  bool update(uint32_t nowMs, bool everActive, uint32_t lastActiveMs, uint32_t ringMs, bool playing);

  bool on() const { return on_; }
  Why why() const { return why_; }
  static const char* name(Why w);

private:
  Config config_;
  bool on_ = false;
  Why why_ = Why::Idle;
  bool dropped_ = false;       // off because of the floor, hold-off pending
  uint32_t droppedAtMs_ = 0;
};

// How fast the decoder refills its ring once it holds enough (docs/UI-SPIKE.md,
// "Scroll round 2"). At a track start or skip the ring is empty and the
// decoder, above the UI on core 1, fills all ~1.45 s of it as fast as it can:
// for an MP3 that is ~2.3x realtime, the whole core, for ~0.7 s, and the UI
// stalls that long. Below gentleFromMs the decoder still runs flat out (the
// time to first audio and the early ring are as before); from there it sleeps
// after each pass so it produces at most capX10 / 10 times realtime, and the
// rest of the core goes to the UI. The caller applies it to the fill after a
// start or skip only (until the ring is first full), not to later dips.
struct RefillPacer {
  // The lowest cap it applies (1.5x): at ~1x the paced ring would stop
  // growing and settle at gentleFromMs (the sleep is rounded up).
  static constexpr uint32_t kMinCapX10 = 15;
  struct Config {
    bool enabled = false;
    uint32_t gentleFromMs = 500;  // flat out below this much buffered
    uint32_t capX10 = 15;         // at most 1.5x realtime above it (kMinCapX10 at least)
    uint32_t maxSleepMs = 40;     // never sleep longer than this per pass
  };

  // How long the decode task sleeps after a pass that produced `frames` at
  // `rate` Hz in `passUs` of its time, with `ringMs` buffered after it.
  // 1 (today's vTaskDelay(1)) when disabled, below gentleFromMs, or when
  // the pass is already slower than the cap.
  static uint32_t sleepMs(const Config& c, uint32_t ringMs, uint32_t frames, uint32_t rate, uint32_t passUs);
};
