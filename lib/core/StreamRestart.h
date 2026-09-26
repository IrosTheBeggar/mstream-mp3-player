#pragma once
#include <cstdint>

// Whether the Bluetooth data callback's next audio starts a new or resumed
// stream, so it fades in from 0 (GainRamp::restart(), DeclickReader::reset()):
// back to the level heard before, or the level a handover lifted it to.
// Portable, so the rule can be tested on the host against the callback
// sequences ESP-IDF produces; BtSink::onData feeds it every callback.
//
// ESP-IDF pulls audio on Bluedroid's BTC task, 128 frames at a time, several
// callbacks per ~30 ms tick while the stream runs, none while it is
// suspended. A restart is:
//   - ESP-IDF's flush (a callback with no frames): the stream stopped, ours
//     (SUSPEND), the headphones' own, or the link dropped;
//   - the first callback ever;
//   - a wait of kGapResetUs or more: the listener certainly heard silence (the
//     headphones buffer a few hundred ms at most);
//   - a new stream epoch (BtSink bumps it before every START of ours and on
//     every link) with a wait of kRestartGapUs or more.
// A shorter wait without a new epoch is congestion or a flash write stalling
// the task: the audio simply goes on (a fade from 0 would click). So does a
// new epoch with a wait under kRestartGapUs (normal is ~10-30 ms): no audio
// was missed.
//
// Data callback only (one task); nothing here is shared.
class StreamRestart {
public:
  static constexpr int64_t kGapResetUs = 1000000;
  static constexpr int64_t kRestartGapUs = 100000;

  // One data callback at nowUs (esp_timer_get_time()) asking for `count`
  // frames (<= 0: ESP-IDF's flush), with the stream epoch as it is now.
  // true: its audio fades in from 0. Always false for the flush itself; the
  // next callback with frames restarts.
  bool callback(int64_t nowUs, int32_t count, uint32_t epoch);

  // The wait before the last callback with frames, for the gap statistics; 0
  // for the first one and for waits of kGapResetUs or more (silence, not a
  // late callback).
  uint32_t statGapUs() const { return statGapUs_; }

private:
  bool flushed_ = false;  // a flush since the last callback with frames
  bool any_ = false;      // a callback with frames has run
  int64_t lastUs_ = 0;
  uint32_t seenEpoch_ = 0;
  uint32_t statGapUs_ = 0;
};
