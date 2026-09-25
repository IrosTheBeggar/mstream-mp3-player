#pragma once
#include <cstdint>

// Decides when the A2DP media stream to the headphones is started and
// suspended, from what the player wants and what the Bluetooth stack reports.
// Portable (no ESP-IDF types) so every ordering of stack events can be tested
// on the host; on the Core2 all calls run on ESP32-A2DP's app task (BtAppT),
// which also issues the commands it returns.
//
// ESP-IDF's rules it is built around (btc_a2dp_control.c, btc_av.c, v5.5):
//  - At most one media-control command may be outstanding; another is
//    answered BUSY. Every call here returns at most one command, and only
//    when none of ours is waiting for its answer.
//  - A start is CHECK_SRC_RDY, then START once that is acknowledged, as in
//    Espressif's a2dp_source example. Our START is acknowledged *before* the
//    stream reports STARTED.
//  - A stream the headphones start by themselves reports STARTED without our
//    START, and ESP-IDF suspends it again at once. Its SUSPEND is not the
//    headphones stopping *our* stream, so it must not pause the player.
//  - An acknowledgement can be lost (e.g. a START that meets a suspend in
//    progress). A command unanswered for kAckTimeoutMs is given up on.
//  - An acknowledgement can also be false: the media task's stop_tx acks
//    whatever command is pending with SUCCESS (btc_a2dp_source.c,
//    btc_a2dp_source_aa_stop_tx). A START acknowledged but not followed by
//    STARTED within kAckTimeoutMs is given up on too, and retried.
//
// Behaviour:
//  - Wanted (Bluetooth is the output and the player plays) and allowed (the
//    volume is settled, see AbsVolumePolicy::audioReady()): start at once,
//    then retry after 1 s, 3 s, then every 10 s while it doesn't run.
//  - Not wanted for kSuspendAfterMs: suspend (like a phone; the headphones
//    show PLAY or PAUSE from it). Wanted again before that: nothing to do.
//    suspendNow() skips the wait (a pause from the headphones' own key: they
//    pick their next key from the stream, so it has to follow at once).
//  - The headphones suspending a stream we started (out of the ear, another
//    source): report it (the player pauses) and hold off until the player
//    plays again (setWanted false -> true).
//
// Clock: milliseconds, wraparound-safe.
class StreamControl {
public:
  enum class Cmd : uint8_t { None, CheckReady, Start, Suspend };
  enum class State : uint8_t {
    Idle,        // no stream, no command outstanding
    Checking,    // CHECK_SRC_RDY sent
    Starting,    // START sent
    Started,     // the stream runs (or START was acknowledged)
    Suspending,  // SUSPEND sent
  };

  struct Actions {
    Cmd send = Cmd::None;        // issue this media-control command now
    bool remoteSuspend = false;  // the headphones stopped our stream: pause the player
    bool gaveUp = false;         // a command went unanswered for kAckTimeoutMs (worth a log line)
    int32_t startedAfterMs = -1; // the stream started this long after playback asked for it
  };

  static constexpr uint32_t kSuspendAfterMs = 3000;
  static constexpr uint32_t kSuspendRetryMs = 5000;
  static constexpr uint32_t kFirstRetryMs = 1000;
  static constexpr uint32_t kMaxRetryMs = 10000;
  static constexpr uint32_t kAckTimeoutMs = 3000;

  // The A2DP link. linkUp() on a link that is already up changes nothing.
  Actions linkUp(uint32_t nowMs);
  void linkDown();

  // The player wants the stream. A rising edge is a fresh play: it ends a
  // hold-off after the headphones suspended us, and starts at once.
  Actions setWanted(bool wanted, uint32_t nowMs);
  // Starting is allowed (false while the headphones' volume is being settled).
  // A stream already running is left alone.
  Actions setStartAllowed(bool allowed, uint32_t nowMs);

  // Media-control acknowledgement (BUSY counts as not ok).
  Actions ack(Cmd cmd, bool ok, uint32_t nowMs);
  // Audio state: started, or suspended/stopped.
  Actions audioState(bool started, uint32_t nowMs);
  // A command could not even be queued (esp_a2d_media_ctrl failed).
  Actions sendFailed(uint32_t nowMs);
  // Not wanted: suspend now instead of after kSuspendAfterMs. Nothing while wanted.
  Actions suspendNow(uint32_t nowMs);
  // Retries, the delayed suspend and the watchdogs. Call a few times a second.
  Actions tick(uint32_t nowMs);

  State state() const { return state_; }
  bool linked() const { return linked_; }
  bool streaming() const { return audioStarted_; }  // the stack reports the stream started
  // Media may flow: a START is out or answered, the stream runs, or it is
  // being suspended. Wider than streaming(): IDF acknowledges our START (and
  // data can flow) before it reports STARTED.
  bool active() const {
    return audioStarted_ || state_ == State::Starting || state_ == State::Started || state_ == State::Suspending;
  }
  bool heldOff() const { return heldOff_; }
  bool wanted() const { return wanted_; }

private:
  static bool reached(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }
  void send(Actions& a, Cmd c, State next, uint32_t nowMs);
  void evaluate(Actions& a, uint32_t nowMs);

  bool linked_ = false;
  bool wanted_ = false;
  bool allowed_ = true;
  bool heldOff_ = false;       // the headphones suspended our stream; wait for a fresh play
  bool audioStarted_ = false;  // last audio state reported
  bool ours_ = false;          // the running stream is the one our START started
  State state_ = State::Idle;
  uint32_t sentMs_ = 0;        // when the outstanding command was sent
  uint32_t nextStartMs_ = 0;
  uint32_t retryMs_ = kFirstRetryMs;
  uint32_t unwantedMs_ = 0;    // when wanted_ fell
  uint32_t nextSuspendMs_ = 0;
  uint32_t askedMs_ = 0;       // when playback asked for a stream
  bool asked_ = false;
};
