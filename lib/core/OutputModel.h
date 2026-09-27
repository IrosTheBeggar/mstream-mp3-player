#pragma once
#include <cstddef>
#include <cstdint>

// The Output screen's decisions (the tab bar spec §6.6, with the review's
// grafts): what the Bluetooth card says and offers, when a connection the
// listener asked for has failed, the pairing scan's list, and the second
// tap that confirms Forget. Portable, host-tested; ui/OutputPage draws it
// and main.cpp carries it out through BtSink.
//
// The safety rules it keeps (the design's, and the user's decisions):
//   - The audio stays on the output it is on until the headphones are
//     connected: asking for Bluetooth (the card, Connect, the B hold) only
//     starts the connection, and the output moves once the link is up.
//   - Nothing moves to the speaker by itself: a link lost while playing
//     pauses (main.cpp), and every move to the speaker pauses first.
//   - Forget is never one tap: a second tap within 3 s confirms it.

// What BtSink reports of the link, every loop pass.
struct BtLink {
  enum class Phase : uint8_t {
    Off,       // not trying: the listener disconnected or cancelled, or forgot the pair
    Paging,    // connecting to the remembered headphones (try `attempt` of `attempts`)
    Scanning,  // looking for them by name (the background cycle, after the tries)
    Linked,    // connected
    PairScan,  // the Pair screen's scan: listing devices, connecting to none
    Pairing,   // connecting to headphones picked on the Pair screen
  };
  Phase phase = Phase::Off;
  uint8_t attempt = 0;      // Paging / Pairing: this try (1..attempts)
  uint8_t attempts = 0;
  bool remembered = false;  // a device address is remembered (NVS)
};
const char* btPhaseName(BtLink::Phase p);

// What the Bluetooth card shows (spec §6.6's state table, plus pairing).
enum class BtCard : uint8_t {
  NotPaired,   // "No headphones paired"        [Pair new headphones]
  Off,         // "Not connected"                [Connect] [Forget]
  Connecting,  // "Connecting... try 2 of 3"     [Cancel]
  Searching,   // "Looking for SPYDRONE..."      [Cancel] (also a connect asked
               // for with none remembered: a scan by name)
  Pairing,     // "Pairing... try 1 of 3"        [Cancel]
  Connected,   // "Connected" + codec, delay     [Disconnect] [More] [volume]
               // (More: a sheet with Disconnect, Pair new, Forget: Forget
               // kept away from the buttons used every day)
  Failed,      // "Couldn't connect. On and nearby?"   [Try again] [Forget]
               // (a pairing: "Couldn't pair..." [Try again] [Connect]: the old ones;
               // none remembered: [Try again] [Pair new headphones])
  Lost,        // "Lost: trying to reconnect"    [Cancel]
};
const char* btCardName(BtCard c);

// The card's buttons, in order (the last reaches the screen's edge).
enum class BtButton : uint8_t { None, Pair, Connect, Forget, Cancel, Disconnect, TryAgain, Volume, More };
// `narrow`: for a box of ~100 px ("Pair new", not "Pair new headphones").
const char* btButtonLabel(BtButton b, bool narrow = false);

// The status line's colour (the UI maps it to the theme).
enum class BtTone : uint8_t { Dim, Cyan, Amber, Red };

// The listener's Bluetooth session: what they asked for, and whether it
// failed. Fed the link every pass; the UI and the buttons tell it what
// the listener did.
class BtSession {
public:
  // How long a pairing may take before it counts as failed (the tries are
  // BtSink's; this is the backstop).
  static constexpr uint32_t kPairTimeoutMs = 45000;
  // A connect with no headphones remembered is a scan by name: failed if
  // nothing linked in this long (two inquiry rounds and a connection).
  static constexpr uint32_t kFindTimeoutMs = 30000;
  // A drop expected of a link that stays up this long isn't coming (the
  // link wasn't let go after all): no longer expected.
  static constexpr uint32_t kDropWaitMs = 10000;

  // The listener asked for the headphones (the card, Connect, Try again,
  // the B hold): the audio moves to them once they are linked.
  void connect(uint32_t nowMs);
  // Cancel, Disconnect, Forget: no longer wanted; the link is let go.
  void cancel();
  // Pairing with the headphones picked on the Pair screen started. With
  // other headphones linked now, only a new link counts: theirs is going.
  void pairStarted(uint32_t nowMs);
  // Every loop pass: the link now.
  void update(const BtLink& link, uint32_t nowMs);

  // What BtSink's Connected event answers: whether the listener asked for
  // this link (the audio moves, a toast says so) and whether it was a
  // pairing (its name is kept). Whichever comes first, the event or the
  // link's phase, the answer is the same: update() keeps it for this.
  struct Answer {
    bool asked = false;
    bool paired = false;
  };
  Answer onConnected();

  // The audio waits to move to Bluetooth until the link is up.
  bool wanted() const { return wanted_; }
  // A connection or pairing the listener asked for didn't come up.
  bool failed() const { return failed_; }
  bool pairing() const { return pairing_; }
  // The link that is going down was let go on purpose (Disconnect,
  // Forget, a new pairing): no "headphones lost" dialog for it. Forgotten
  // by the next link, or when the link stays up kDropWaitMs.
  bool dropExpected() const { return dropExpected_; }
  void dropSeen() { dropExpected_ = false; }
  void expectDrop(uint32_t nowMs) {
    dropExpected_ = true;
    dropSinceMs_ = nowMs;
  }

private:
  bool wanted_ = false;
  bool failed_ = false;
  bool pairing_ = false;
  bool dropExpected_ = false;
  bool sawTrying_ = false;     // the link was Paging/Pairing since the ask
  bool awaitNewLink_ = false;  // asked while linked: the link up now isn't the answer
  bool answered_ = false;      // update() saw the asked-for link come up (for onConnected())
  bool answeredPairing_ = false;
  uint32_t askedMs_ = 0;
  uint32_t pairSinceMs_ = 0;
  uint32_t dropSinceMs_ = 0;
  BtLink::Phase last_ = BtLink::Phase::Off;
};

// The card for the link and the session. `lost`: dropped while it was the
// output (the player paused; it tries to come back).
struct BtCardView {
  BtCard card = BtCard::NotPaired;
  BtTone tone = BtTone::Dim;
  BtButton buttons[3] = {BtButton::None, BtButton::None, BtButton::None};
  int buttonCount = 0;
  bool spinner = false;     // something is under way (connecting, searching)
  bool pairFailed = false;  // Failed: it was a pairing
};
BtCardView btCardView(const BtLink& link, const BtSession& session, bool lost);
// The status line: "Connecting... try 2 of 3". `name`: the headphones'
// ("" if unknown); `detail`: the connected line's codec and delay ("SBC
// 44.1 kHz, 175 ms"), or "". Returns buf.
char* btStatusLine(const BtCardView& v, const BtLink& link, const char* name, const char* detail, char* buf,
                   size_t size);

// A destructive control's second tap (Forget): the first arms it ("Tap
// again" in red) for kWindowMs, a second within that confirms.
class ConfirmTap {
public:
  static constexpr uint32_t kWindowMs = 3000;
  // True: confirmed (the second tap). False: armed now.
  bool tap(uint32_t nowMs);
  bool armed(uint32_t nowMs) const { return armed_ && static_cast<int32_t>(nowMs - untilMs_) < 0; }
  // Armed, but its window just ran out (the UI redraws the button once).
  bool expired(uint32_t nowMs) const { return armed_ && !armed(nowMs); }
  void reset() { armed_ = false; }

private:
  bool armed_ = false;
  uint32_t untilMs_ = 0;
};

// The Pair screen's list: audio devices found by the scan, each once (by
// address), in the order they were found: a list that reordered itself by
// signal would move a row away from a finger about to tap it. Fixed size, no allocation: BtSink keeps
// one, written on the Bluetooth task under its lock, copied out by the UI.
struct BtDevice {
  enum class Kind : uint8_t { Headphones, Speaker, Car, Other };
  uint8_t addr[6] = {};
  char name[32] = "";
  int8_t rssi = -127;
  Kind kind = Kind::Other;
};
class BtScanList {
public:
  static constexpr int kMax = 12;
  void clear() { n_ = 0; }
  // A scan result. `cod`: its class of device (0: unknown). A device seen
  // again updates its name (if it has one now) and signal. The weakest is
  // dropped for a stronger one when the list is full.
  void note(const uint8_t addr[6], const char* name, int rssi, uint32_t cod);
  int count() const { return n_; }
  const BtDevice& at(int i) const { return d_[i]; }

  // 0-4 bars for a signal strength (dBm).
  static int bars(int rssi);
  // From the class of device: its major class Audio/Video (4) and minor.
  static BtDevice::Kind kindOf(uint32_t cod);
  static const char* kindName(BtDevice::Kind k);

private:
  BtDevice d_[kMax];
  int n_ = 0;
};
