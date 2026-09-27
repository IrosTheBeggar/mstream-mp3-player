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
// Which headphones: the first audio device whose name contains the sink name
// (case-insensitive). Without a name, only a device practically touching the
// Core2 — signal strength alone once picked a TV in the next room. Once
// connected, that device is remembered: after a drop (or a boot) the Core2
// tries it for ~30 s, then scans for a minute, then tries it again, and so
// on; all along it stays connectable (not discoverable) so the headphones can
// come back by themselves, as they do to a phone. Other devices can't connect
// in. forgetDevice() clears it.
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
  // best taken before the heap fragments. The sink name saved by setSinkName()
  // wins over `defaultSinkName` (the BT_SINK_NAME build flag).
  void begin(PcmRing& ring, AudioShared& shared, const char* defaultSinkName);

  // Loop task, every pass. `wantAudio`: Bluetooth is the output and the player
  // is playing (a rising edge is a fresh play). Hands that and pending volume
  // changes to BtAppT, and lets it check its timers a few times a second.
  void update(uint32_t nowMs, bool wantAudio);

  // Changes which headphones to look for and saves it; applies from the next
  // device discovered. Empty = any very close device.
  void setSinkName(const char* name);
  const char* sinkName() const { return sinkNames_[sinkNameIdx_.load()]; }

  bool connected() const;
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

  // Drops the remembered device, so the next boot scans instead of
  // reconnecting. Carried out on the Bluetooth task (which pages and re-arms
  // with that address); waits up to waitMs for it, and with a wait (for a
  // restart that follows) erases the stored copy itself if that task can't.
  // true: done. Without a wait it is posted by update().
  // `forGood` (the Output screen's Forget): nothing is looked for by name
  // either (saved), until new headphones link: the forgotten ones can't
  // come back by a scan for their name, at the next boot or a B hold.
  // Without it (the console's f), the scan by name is back.
  bool forgetDevice(uint32_t waitMs = 0, bool forGood = false);
  // Nothing remembered and nothing to look for (forgotten for good): a
  // connect can't find anything; pairing is the way.
  bool nothingToFind() const { return !linkRemembered_.load(std::memory_order_relaxed) && forgotForGood_.load(); }
  // The device linked now has this address (the Pair screen's pick).
  bool isLinkedTo(const uint8_t addr[6]) const;
  // Next queued event, oldest first, or Event::None. Loop task.
  Event takeEvent();

  // ---- the Output screen (loop task; carried out on BtAppT) ----
  // What the link is doing (published by BtAppT a few times a second).
  BtLink link() const;
  // Page the remembered headphones now (3 tries, then the scan by name);
  // with none remembered, scan by name. Undoes disconnect().
  void connect();
  // Let go of the link (or stop connecting) and stop trying: the
  // headphones can't come back by themselves until connect().
  void disconnect();
  // The Pair screen: scan and list audio devices, connecting to none;
  // stopPairScan() goes back to what it was doing.
  void startPairScan();
  void stopPairScan();
  // Pair with a device the scan listed (its address): the link that is up
  // goes first; it becomes the remembered headphones once linked.
  void pairWith(const uint8_t addr[6]);
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
  // What the data callback played (before our gain stage), for the beat
  // tracker. nullptr if its PSRAM couldn't be had.
  const AudioTap* tap() const { return tap_; }
  // The headphones' last delay report (AVDTP), in microseconds; 0 when they
  // haven't sent one on this link.
  uint32_t delayReportUs() const { return delayReport_.load(std::memory_order_relaxed) * 100u; }

private:
  friend class PlayerA2dp;

  static int32_t onData(Frame* frames, int32_t count);
  static bool onDeviceFound(const char* name, esp_bd_addr_t address, int rssi);
  static void onConnectionChanged(esp_a2d_connection_state_t state, void* obj);
  static void onKey(uint8_t key, bool released);

  void post(Event e);                    // any task; never blocks
  void setForgotForGood(bool on);        // saves it (loop task, or BtAppT on a link)
  void setDeviceName(const char* name);  // Bluedroid's BTC task only

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
  // The pairing scan's list (PSRAM), written on the BTC task, copied out
  // by the loop, under scanLock_.
  BtScanList* scan_ = nullptr;
  mutable portMUX_TYPE scanLock_ = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<uint32_t> scanVersion_{0};
  uint8_t pairAddr_[6] = {};  // pairWith()'s, read by BtAppT after the work is posted
  // The Output screen's asks not handed to BtAppT yet (its queue was full).
  uint8_t askPending_ = 0;

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

  // Data callback only, apart from the atomics.
  DeclickReader reader_{kConsumerId};
  AudioTap* tap_ = nullptr;  // written only here; read by the loop
  StreamRestart restart_;  // when its audio fades in from 0
  std::atomic<uint32_t> maxGapUs_{0};
  std::atomic<uint32_t> framesPulled_{0};
};
