#pragma once
#include <atomic>
#include <cstdint>

// Where the decode task is with the most recent transport request.
enum class Phase : uint8_t {
  Idle,      // stopped / nothing requested
  Pending,   // requested, the decode task hasn't picked it up yet
  Decoding,  // producing audio
  Draining,  // source finished; waiting for the output to play what's buffered
  Ended,     // everything played
  Failed,    // couldn't be played (open/decode error, unsupported for this output)
};

// Hand-off between the control side (loop task: play/stop) and the decode task.
// Every request gets a new generation; the decode task reports its progress
// tagged with the generation it is working on, and reports for anything but the
// latest generation are dropped. That keeps a stale "Ended" from the previous
// track from being mistaken for the end of a track that was just requested.
//
// Generation and phase share one atomic word so they always change together.
class TransportSync {
public:
  // Control side: start a new request (phase Pending). Returns its generation.
  uint32_t post(Phase initial = Phase::Pending) {
    uint32_t cur = word_.load(std::memory_order_relaxed);
    uint32_t next;
    do {
      next = pack(gen(cur) + 1, initial);
    } while (!word_.compare_exchange_weak(cur, next, std::memory_order_acq_rel));
    return gen(next);
  }

  // Decode task: record progress on `generation`. Returns false (and changes
  // nothing) if a newer request has been posted since.
  bool report(uint32_t generation, Phase p) {
    uint32_t cur = word_.load(std::memory_order_relaxed);
    do {
      if (gen(cur) != (generation & kGenMask)) return false;
    } while (!word_.compare_exchange_weak(cur, pack(generation, p), std::memory_order_acq_rel));
    return true;
  }

  uint32_t generation() const { return gen(word_.load(std::memory_order_acquire)); }
  Phase phase() const { return phaseOf(word_.load(std::memory_order_acquire)); }

private:
  static constexpr uint32_t kPhaseBits = 4;
  static constexpr uint32_t kGenMask = 0xFFFFFFFFu >> kPhaseBits;

  static uint32_t pack(uint32_t g, Phase p) {
    return ((g & kGenMask) << kPhaseBits) | static_cast<uint32_t>(p);
  }
  static uint32_t gen(uint32_t word) { return word >> kPhaseBits; }
  static Phase phaseOf(uint32_t word) {
    return static_cast<Phase>(word & ((1u << kPhaseBits) - 1));
  }

  std::atomic<uint32_t> word_{0};  // generation 0, Idle
};
