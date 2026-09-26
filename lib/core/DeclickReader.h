#pragma once
#include <cstdint>

#include "Declicker.h"
#include "PcmRing.h"

// One output's view of the PcmRing, de-clicked: reads only while this output
// is the ring's consumer, only what the Declicker wants (so a pause fades out
// over ~64 real frames and leaves the rest for resume), and turns a skip (the
// ring's epoch moved) into a crossfade. Never blocks: PcmRing::read() only
// tries the lock. Used by exactly one task (the Bluetooth data callback, or
// the speaker pump).
class DeclickReader {
public:
  struct Result {
    uint32_t wanted;  // real frames asked of the ring: 0 when not the consumer or paused and faded
    uint32_t read;    // real frames it had
    uint32_t total;   // frames written to `out`
    // Where the real frames came from (valid when read > 0): the ring's epoch
    // and the first one's frame in it (PcmRing::read()'s position), so an
    // output's AudioTap can place them in the track.
    uint32_t epoch;
    uint32_t position;
  };

  explicit DeclickReader(uint8_t consumerId, uint32_t rampFrames = Declicker::kDefaultRampFrames)
      : id_(consumerId), fader_(rampFrames) {}

  // Before the first fill()/pull().
  void bind(PcmRing& ring) {
    ring_ = &ring;
    epoch_ = ring.epoch();
  }

  // Exactly `count` frames, as the Bluetooth stack wants: the ring's audio,
  // then a fade to silence if it ran short, then zeros. `playing`: not paused.
  Result fill(int16_t* out, uint32_t count, bool playing) {
    Result r = readReal(out, count, playing);
    fader_.process(out + 2 * r.read, 0, count - r.read);
    r.total = count;
    return r;
  }

  // Up to `count` frames of the ring's audio for a queued output (the
  // speaker), plus the fade to silence when it ran short, paused or lost the
  // ring, so what's queued always ends at 0. `out` must hold
  // count + Declicker::decayFramesFor(rampFrames) frames. total 0: nothing to send.
  Result pull(int16_t* out, uint32_t count, bool playing) {
    Result r = readReal(out, count, playing);
    r.total = r.read;
    if (r.read < count) {
      // Whatever comes next isn't continuous with this: hold the last frame,
      // fade it out here, and fade the next audio in. Afterwards silent().
      fader_.cut();
      const uint32_t pad = fader_.decayFrames();
      fader_.process(out + 2 * r.read, 0, pad);
      r.total += pad;
    }
    return r;
  }

  // Nothing left to fade: the output can go quiet.
  bool silent() const { return fader_.silent(); }
  // The listener heard silence (a new Bluetooth stream): start from 0.
  void reset() { fader_.reset(); }

private:
  Result readReal(int16_t* out, uint32_t count, bool playing) {
    fader_.setOpen(playing);
    Result r{0, 0, 0, epoch_, 0};
    if (ring_ && ring_->consumer() == id_) r.wanted = fader_.framesWanted(count);
    if (r.wanted > 0) {
      uint32_t epoch = epoch_;
      r.read = ring_->read(id_, out, r.wanted, &epoch, &r.position);
      r.epoch = epoch;
      if (epoch != epoch_) {  // discardAll() since our last read: a skip
        epoch_ = epoch;
        fader_.cut();
      }
    }
    fader_.process(out, r.read, r.read);
    return r;
  }

  PcmRing* ring_ = nullptr;
  const uint8_t id_;
  uint32_t epoch_ = 0;
  Declicker fader_;
};
