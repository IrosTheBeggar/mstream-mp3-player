#pragma once
#include <BluetoothA2DPSource.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstdint>

#include "AudioTap.h"
#include "DeclickReader.h"
#include "GainRamp.h"
#include "OutputModel.h"
#include "PcmRing.h"
#include "SinkSearch.h"
#include "StreamRestart.h"
#include "VolumeMath.h"
#include "audio/AudioShared.h"

class PlayerA2dp;  // BtSink.cpp: ESP32-A2DP's source with the fixes below

// Streams the ring to Bluetooth headphones with ESP32-A2DP (A2DP source). The
// Bluetooth stack pulls 44.1 kHz stereo on its BTC task (128 frames at a
// time, several times per ~30 ms tick) while the media stream runs; that
// callback must never block, so it takes what the ring has, pads the rest with
// silence and applies our gain stage (GainRamp). A DeclickReader fades that in
// and out (pause, skip, underrun, output switch) so none of it clicks.
//
// Which headphones: the ones the listener paired on the Pair screen
// (Output > Pair new headphones: startPairScan(), pairWith()). Once
// connected, that device is remembered: after a drop (or a boot, or a
// listener's ask) the Core2 pages it 3 times (~25 s), then once at 30 s, 1,
// 2 and 5 min and every 5 min, and rests after 15 min (at once after the 3
// tries while nobody is around: setQuiet()); it never scans for remembered
// headphones (ReconnectPlanner). All along it stays connectable (not
// discoverable) so the headphones can come back by themselves, as they do
// to a phone. Other devices can't connect in. forgetDevice() clears it.
// With none remembered it never picks a device by itself (SinkSearch): a
// release build doesn't scan at all, at the boot or on any ask, and stays
// quiet, connectable only. A developer build with a name (BT_SINK_NAME)
// scans for a device whose name contains it for 2 min after the boot or a
// connect(). Never by signal strength (once a TV in the next room was
// paired that way), except for one scan after the console's Bs.
//
// Volume works like a phone's (AbsVolumePolicy): headphones that take AVRCP
// absolute volume get the Bluetooth volume as their own, their buttons change
// it, and the PCM goes out at full resolution with a fixed 2 dB of headroom.
// Headphones without it get a software volume. Every link starts at the safe
// software level and at most 60 %. The volume is handed to the headphones
// before anything plays on a link, or, when their remote control comes up
// later, during a short dip to silence (their level changes at once when
// asked). Our gain falls fast and rises slowly.
//
// The media stream (StreamControl) starts as soon as there is something to
// play and is suspended 3 s after playback stops, again like a phone
// (headphones pick PLAY or PAUSE for their button from it), or at once after
// a pause from their own key (suspendPromptly()). A stream the headphones
// suspend themselves pauses the player (Event::Suspended) and stays down
// until the player plays again. BtControl (lib/core) makes these decisions.
//
// Headphone buttons and link changes arrive as events for the loop task
// (takeEvent()); nothing is acted on inside the Bluetooth callbacks.
//
// The Output screen (OutputModel) drives the link as a listener would:
// connect() pages the remembered headphones now, disconnect() lets go and
// stops trying (and refuses them coming back by themselves) until the next
// connect(), and the Pair screen scans (startPairScan(): audio devices are
// listed, none is connected to) and pairs with one it picked (pairWith():
// it replaces the remembered headphones once it is linked; a pairing that
// fails keeps the old ones). link() says what it is doing, for the card.
class BtSink {
public:
  static constexpr uint8_t kConsumerId = 1;
  static constexpr uint8_t kDefaultVolume = 30;  // percent, until changed (not saved)

  enum class Event : uint8_t {
    None,
    Connected,      // headphones connected: switch the output to them
    Disconnected,   // link lost: pause, like a phone
    Suspended,      // the headphones stopped our stream themselves: pause
    VolumeChanged,  // volume() changed by itself (headphone buttons, or capped on connect)
    Play,           // headphone buttons (AVRCP passthrough), on press
    Pause,          //   (STOP arrives as Pause)
    Next,
    Prev,
    VolumeUp,       // only from headphones without absolute volume
    VolumeDown,
  };

  struct Stats {
    const char* reconnect;      // ReconnectPlanner's phase: "idle", "burst", "backoff", "resting", "scan"
    int radioBusyPercent;       // the last minute's share paging or scanning, -1 before a minute
    uint8_t volume;             // percent, as the UI shows it
    const char* volumeControl;  // "headphones" (absolute volume), "asking", "software"
    int headsetVolume;          // last absolute volume the headphones reported (0-127), -1 unknown
    uint16_t gainQ15;           // our gain stage now (32768 = 0 dB, headroom included)
    uint16_t headroomQ15;       // the fixed attenuation it keeps (setHeadroomDb())
    const char* stream;         // "started", "starting", "suspending", "suspended", "held off"
    uint32_t maxGapMs;          // longest wait between data callbacks since the last stats()
    uint32_t eventsDropped;     // events lost because the loop didn't collect them
    uint32_t appTaskStackFree;  // bytes of ESP32-A2DP's app task (BtAppT) stack never used
  };

  // Starts the Bluetooth stack. Call early: it claims ~70 KB of internal RAM,
  // best taken before the heap fragments. `defaultSinkName`: the BT_SINK_NAME
  // build flag ("" in a release build: no scan by name, ever). With one, the
  // name saved by setSinkName() wins over it. A fresh-unit test armed for
  // this boot (armFreshBoot()) is used up here.
  void begin(PcmRing& ring, AudioShared& shared, const char* defaultSinkName);

  // Loop task, every pass. `wantAudio`: Bluetooth is the output and the player
  // is playing (a rising edge is a fresh play). Hands that and pending volume
  // changes to BtAppT, and lets it check its timers a few times a second.
  void update(uint32_t nowMs, bool wantAudio);

  // Changes which headphones to look for and saves it; applies from the next
  // device discovered. Only a build with BT_SINK_NAME scans by name
  // (scansByName()); empty: nothing is looked for. The Pair screen's
  // pairing saves the headphones' name here too (the name shown until
  // their own is read). Not saved during a fresh-unit test.
  void setSinkName(const char* name);
  const char* sinkName() const { return sinkNames_[sinkNameIdx_.load()]; }
  // A scan by name can run at all: a build with BT_SINK_NAME, not in a
  // fresh-unit test (it still takes a name, and none remembered).
  bool scansByName() const { return buildName_ && fresh() == Fresh::No; }

  bool connected() const;
  // The link as the loop hears of it: up from CONNECTED until DISCONNECTED.
  // connected() (the library's) goes false as soon as a disconnect starts
  // (the stack's DISCONNECTING, right after esp_a2d_source_disconnect()),
  // 0.15-1.5 s before the link is really gone (measured). A release that
  // waits before a restart or a power-off (the CPU speed, the idle
  // power-off) waits for this, so the headphones see the disconnect finish.
  bool linkUp() const { return linked_.load(); }
  // The headphones' name: from discovery, or read after each connection.
  const char* deviceName() const { return deviceNames_[deviceNameIdx_.load()]; }

  // The Bluetooth volume, 0-100 %, kept across reconnects. Loop task. Changes
  // are applied on BtAppT, in order; volume() shows the result a moment later.
  void setVolume(uint8_t percent);
  void stepVolume(int delta);  // relative to whatever it is by then (no lost steps)
  // Diagnostic: the fixed attenuation our gain stage keeps, -db dB (default
  // 2, vol::kHeadroomDb), to find where loud masters start to distort in SBC.
  // Not saved. Loop task; applied on BtAppT.
  void setHeadroomDb(uint8_t db);
  // The player was paused from the headphones' key: suspend the stream as
  // soon as the pause reaches it, not 3 s later. ESP-IDF's AVRCP target can't
  // tell them the play status, so they pick their next key (PLAY or PAUSE)
  // from the stream; a quick pause and play on them works only this way.
  void suspendPromptly();
  uint8_t volume() const { return volume_.load(std::memory_order_relaxed); }
  // The connected headphones apply the volume themselves (AVRCP absolute volume).
  bool headphonesControlVolume() const;
  // The media stream is running: the headphones are being sent audio.
  bool streaming() const { return streaming_.load(); }

  // Drops the remembered device, so the next boot doesn't page it (a build
  // with BT_SINK_NAME scans by name instead; a release build stays quiet).
  // Carried out on the Bluetooth task (which pages and re-arms
  // with that address); waits up to waitMs for it, and with a wait (for a
  // restart that follows) erases the stored copy itself if that task can't.
  // true: done. Without a wait it is posted by update().
  // `forGood` (the Output screen's Forget): nothing is looked for by name
  // either (saved), until new headphones link: the forgotten ones can't
  // come back by a scan for their name, at the next boot or a B hold.
  // Without it (the console's f), a build's scan by name is back.
  bool forgetDevice(uint32_t waitMs = 0, bool forGood = false);
  // Nothing remembered and nothing to look for (no name to scan by: a
  // release build, a fresh-unit test; or forgotten for good), and Bs not
  // armed: a connect can't find anything. Nothing is scanned for; pairing
  // (Output > Pair new headphones) is the way.
  bool nothingToFind() const {
    return !linkRemembered_.load(std::memory_order_relaxed) && !scanAllowed(false);
  }
  // The headphones' name for the screen: their own (deviceName()), else
  // the one saved or scanned for (sinkName()); none while nothingToFind(),
  // so the card reads "Bluetooth headphones" then, as on a fresh unit (a
  // fresh-unit test or a Forget leaves the old name in RAM or NVS).
  const char* shownName() const {
    if (nothingToFind()) return "";
    return deviceName()[0] ? deviceName() : sinkName();
  }
  // The device linked now has this address (the Pair screen's pick).
  bool isLinkedTo(const uint8_t addr[6]) const;
  // Next queued event, oldest first, or Event::None. Loop task.
  Event takeEvent();

  // ---- the Output screen (loop task; carried out on BtAppT) ----
  // What the link is doing (published by BtAppT a few times a second).
  BtLink link() const;
  // Page the remembered headphones now (a burst of 3 tries, then the
  // back-off); with none remembered, scan by name for 2 min if there is a
  // name to look for (or Bs), else nothing (nothingToFind()). Undoes
  // disconnect().
  void connect();
  // Let go of the link (or stop connecting) and stop trying: the
  // headphones can't come back by themselves until connect().
  void disconnect();
  // The Pair screen: scan and list audio devices, connecting to none;
  // stopPairScan() (the screen closed) goes back to what it was doing.
  // pausePairScan(): the scan stopped by itself (2 min, the screen off)
  // with the screen still up: nothing is tried until it closes (the
  // remembered headphones would link while new ones are picked).
  void startPairScan();
  void stopPairScan();
  void pausePairScan();
  // Pair with a device the scan listed (its address): the link that is up
  // goes first; it becomes the remembered headphones once linked.
  void pairWith(const uint8_t addr[6]);
  // Power measurements (the console's Pr): false rests the background
  // search for the remembered headphones now (ReconnectPlanner's Resting:
  // no pages, a scan by name running is stopped, a page on its way ends by
  // itself within ~5 s); true starts a burst again. The Core2 stays
  // connectable, so the headphones can still come back by themselves.
  // connect() and the Pair screen start the search again too.
  void setBackgroundReconnect(bool on);
  // Bluetooth power (the Output tab's setting, ENERGY.md item 7): the BR/EDR
  // TX power levels the controller may use, esp_power_level_t 0-7 (-12..+9
  // dBm, 3 dB apart). Before begin(): applied as the controller comes up,
  // before Bluedroid starts, so before any page, scan or page scan. After:
  // at once, from the next page, scan or connection; a link that is up
  // keeps its level (measured). Not called: the controller's default. False:
  // the controller refused it. Loop task.
  bool setTxPower(uint8_t minLevel, uint8_t maxLevel);
  // The sleep timer, 5 min after its pause (ENERGY.md section 3), and the
  // idle power-off before the power goes (item 4): let go of
  // the headphones and rest (no pages, no scans), still connectable, so
  // they come back when switched on, and not refused as after
  // disconnect(); connect() (a play through PlayGate) pages them again.
  // Unlinked: the search rests. The caller expects the drop
  // (BtSession::expectDrop()) and leaves the output as it is.
  void releaseHeadphones();
  // Nobody is around (the screen is off, nothing plays or waits): after a
  // burst the search rests at once instead of backing off. Loop task,
  // every pass (the screen policy's hook; today the probe's Ps0).
  void setQuiet(bool quiet) { quiet_.store(quiet, std::memory_order_relaxed); }
  // The search's phase (ReconnectPlanner::phaseName) and the share of the
  // last minute the radio spent paging or scanning (-1: none measured yet).
  const char* reconnectPhase() const;
  int radioBusyPercent() const { return radioBusy_.load(std::memory_order_relaxed); }
  // The scan's list, copied; returns its version (bumped when it changes).
  uint32_t scanList(BtScanList& out) const;
  uint32_t scanVersion() const { return scanVersion_.load(std::memory_order_relaxed); }
  // "SBC 44.1 kHz" once the link's codec is known, else "".
  const char* codec() const { return codecs_[codecIdx_.load()]; }
  // Frames handed to the Bluetooth stack, silence included: ~44100/s while
  // streaming, 0 while the stream is suspended.
  uint32_t framesPulled() const { return framesPulled_.load(std::memory_order_relaxed); }
  // For the serial stats line. Resets maxGapMs.
  Stats stats();
  // The media stream's state as stats() names it, without resetting anything.
  const char* streamState() const;
  // What the data callback played (before our gain stage), for the beat
  // tracker. nullptr if its PSRAM couldn't be had.
  const AudioTap* tap() const { return tap_; }
  // Whether the data callback copies to its tap (only while the Dance tab
  // is up). Any task.
  void setTapOn(bool on) {
    if (tap_) tap_->setEnabled(on);
  }
  // The headphones' last delay report (AVDTP), in microseconds; 0 when they
  // haven't sent one on this link.
  uint32_t delayReportUs() const { return delayReport_.load(std::memory_order_relaxed) * 100u; }

  // ---- developer tests (the console's B; nothing saved but Bf's flag) ----
  // Bs: the next scan may take an audio device at sinksearch::kMinRssi or
  // closer, whatever its name (main.cpp starts one: connect()). RAM only,
  // off at boot; used up by the device it takes, a link, the scan's end,
  // or anything that ends that scan (a Disconnect, the Pair screen, a
  // pairing, the search resting).
  void setBySignal(bool on);
  bool bySignal() const { return bySignal_.load(); }
  // The fresh-unit test: BtSink behaves as if no headphones were
  // remembered and the build had no BT_SINK_NAME, without reading,
  // erasing or changing the stored address (NVS) or the stack's bond: the
  // remembered address lives in RAM meanwhile (a pairing made during the
  // test too), and the sink name and Forget aren't saved.
  //   Boot: armFreshBoot() (Bf) saves a flag that the next begin() uses and
  //     clears: that boot is the test, the one after is normal again.
  //   Session: setFreshSession(true) (Bn), for the running session, RAM
  //     only; refused (logged) while linked, pairing or on the Pair screen.
  //     false (Bn0), or any restart, ends it: the stored address is read
  //     again. Loop task.
  enum class Fresh : uint8_t { No, Boot, Session };
  Fresh fresh() const { return static_cast<Fresh>(fresh_.load()); }
  static void armFreshBoot();
  void setFreshSession(bool on);

private:
  friend class PlayerA2dp;

  static int32_t onData(Frame* frames, int32_t count);
  static bool onDeviceFound(const char* name, esp_bd_addr_t address, int rssi);
  static void onConnectionChanged(esp_a2d_connection_state_t state, void* obj);
  static void onKey(uint8_t key, bool released);

  void post(Event e);                    // any task; never blocks
  void setForgotForGood(bool on);        // saves it (loop task, or BtAppT on a link); RAM only in a fresh-unit test
  // What a scan may look for (SinkSearch): the name only in a build with
  // one, outside a fresh-unit test; Forget; Bs. Any task.
  sinksearch::Setup searchSetup() const;
  bool scanAllowed(bool remembered) const { return sinksearch::mayScan(remembered, searchSetup()); }
  void reloadSaved();                    // the saved name and Forget, from NVS again (loop task)
  void setDeviceName(const char* name);  // Bluedroid's BTC task only
  bool applyTxPower(const char* when);    // setTxPower()'s levels; `when` ends the log line

  PcmRing* ring_ = nullptr;
  AudioShared* shared_ = nullptr;
  QueueHandle_t events_ = nullptr;
  std::atomic<uint32_t> eventsDropped_{0};

  void setCodec(const char* text);        // BtAppT
  void flushAsks();                        // hands the Output screen's asks to BtAppT
  void noteScanResult(const uint8_t* addr, const char* name, int rssi, uint32_t cod);  // BTC task

  // Two copies each, so a writer never rewrites the one another task reads.
  char sinkNames_[2][64] = {"", ""};
  std::atomic<uint8_t> sinkNameIdx_{0};
  char deviceNames_[2][64] = {"", ""};
  std::atomic<uint8_t> deviceNameIdx_{0};
  char lastLogged_[64] = "";
  char codecs_[2][24] = {"", ""};
  std::atomic<uint8_t> codecIdx_{0};

  // The link for the Output screen, published by BtAppT (BtLink's fields).
  std::atomic<uint8_t> linkPhase_{0};
  std::atomic<uint8_t> linkAttempt_{0};
  std::atomic<uint8_t> linkAttempts_{0};
  std::atomic<bool> linkRemembered_{false};
  std::atomic<uint8_t> reconnectPhase_{0};  // ReconnectPlanner::Phase, published by BtAppT
  std::atomic<int8_t> radioBusy_{-1};       // RadioMeter's last minute, %
  std::atomic<bool> quiet_{false};          // setQuiet()
  // The pairing scan's list (PSRAM), written on the BTC task, copied out
  // by the loop, under scanLock_.
  BtScanList* scan_ = nullptr;
  mutable portMUX_TYPE scanLock_ = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<uint32_t> scanVersion_{0};
  uint8_t pairAddr_[6] = {};  // pairWith()'s, read by BtAppT after the work is posted
  // The Output screen's asks not handed to BtAppT yet (its queue was full).
  uint16_t askPending_ = 0;

  // Set on Bluetooth tasks, read anywhere.
  std::atomic<bool> linked_{false};     // the A2DP link as reported to the loop
  std::atomic<bool> streaming_{false};  // the stack reports the stream started
  std::atomic<uint8_t> streamState_{0};  // for stats(): StreamControl::State, bit 7 = held off
  // The volume state, published by PlayerA2dp (BtAppT) for the UI and stats.
  std::atomic<uint8_t> volume_{kDefaultVolume};
  std::atomic<uint8_t> control_{0};  // AbsVolumePolicy::Mode
  std::atomic<int16_t> headsetVolume_{-1};
  std::atomic<uint16_t> headroom_{vol::kHeadroomQ15};
  std::atomic<uint16_t> delayReport_{0};  // 1/10 ms, from BtAppT

  // Loop task only.
  int8_t txMin_ = -1, txMax_ = -1;  // setTxPower()'s levels, -1: never set
  bool wantAudio_ = false;      // as last handed to BtAppT
  uint32_t nextTickMs_ = 0;
  int16_t unsentVolume_ = -1;   // a setVolume() BtAppT's queue had no room for yet
  int unsentStep_ = 0;          // stepVolume()s it had no room for yet
  int16_t unsentHeadroom_ = -1; // a setHeadroomDb() it had no room for yet
  bool forgetPending_ = false;  // a forgetDevice() not handed to BtAppT yet
  bool suspendPending_ = false; // a suspendPromptly() waiting for the pause to reach BtAppT
  uint32_t suspendPendingMs_ = 0;

  // The gain stage: requests from BtAppT, applied by the data callback.
  GainRamp gain_;
  // Bumped by BtAppT before every START and on every link: the next audio
  // after a pause in the callbacks belongs to a new stream (fade in).
  std::atomic<uint32_t> streamEpoch_{0};

  std::atomic<bool> forgotten_{false};  // set by BtAppT once it forgot the device
  std::atomic<bool> forgotForGood_{false};  // forgetDevice(forGood): no scan by name (NVS "bt_forgot")
  bool buildName_ = false;                  // begin()'s defaultSinkName isn't empty (a developer build)
  std::atomic<bool> bySignal_{false};       // Bs: the next scan takes a device close enough
  std::atomic<uint8_t> fresh_{0};           // Fresh: set by begin() (Boot) or BtAppT (Session)

  // Data callback only, apart from the atomics.
  DeclickReader reader_{kConsumerId};
  AudioTap* tap_ = nullptr;  // written only here; read by the loop
  StreamRestart restart_;  // when its audio fades in from 0
  std::atomic<uint32_t> maxGapUs_{0};
  std::atomic<uint32_t> framesPulled_{0};
};
