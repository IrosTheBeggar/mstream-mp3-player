#pragma once
#include <BluetoothA2DPSource.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstdint>

#include "DeclickReader.h"
#include "GainRamp.h"
#include "PcmRing.h"
#include "audio/AudioShared.h"

class PlayerA2dp;  // BtSink.cpp: ESP32-A2DP's source with the fixes below

// Streams the ring to Bluetooth headphones with ESP32-A2DP (A2DP source). The
// Bluetooth stack pulls 44.1 kHz stereo from its own task (128 frames at a
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
// software level and at most 60 %, and the volume is only handed to the
// headphones before anything has played on a link (else Software until the
// next one): once audio has played, nothing but the user's own volume step
// makes it louder. Our gain falls fast and rises slowly.
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
  bool forgetDevice(uint32_t waitMs = 0);
  // Next queued event, oldest first, or Event::None. Loop task.
  Event takeEvent();
  // Frames handed to the Bluetooth stack, silence included: ~44100/s while
  // streaming, 0 while the stream is suspended.
  uint32_t framesPulled() const { return framesPulled_.load(std::memory_order_relaxed); }
  // For the serial stats line. Resets maxGapMs.
  Stats stats();

private:
  friend class PlayerA2dp;

  static int32_t onData(Frame* frames, int32_t count);
  static bool onDeviceFound(const char* name, esp_bd_addr_t address, int rssi);
  static void onConnectionChanged(esp_a2d_connection_state_t state, void* obj);
  static void onKey(uint8_t key, bool released);

  void post(Event e);                    // any task; never blocks
  void setDeviceName(const char* name);  // Bluedroid's BTC task only

  PcmRing* ring_ = nullptr;
  AudioShared* shared_ = nullptr;
  QueueHandle_t events_ = nullptr;
  std::atomic<uint32_t> eventsDropped_{0};

  // Two copies each, so a writer never rewrites the one another task reads.
  char sinkNames_[2][64] = {"", ""};
  std::atomic<uint8_t> sinkNameIdx_{0};
  char deviceNames_[2][64] = {"", ""};
  std::atomic<uint8_t> deviceNameIdx_{0};
  char lastLogged_[64] = "";

  // Set on Bluetooth tasks, read anywhere.
  std::atomic<bool> linked_{false};     // the A2DP link as reported to the loop
  std::atomic<bool> streaming_{false};  // the stack reports the stream started
  std::atomic<uint8_t> streamState_{0};  // for stats(): StreamControl::State, bit 7 = held off
  // The volume state, published by PlayerA2dp (BtAppT) for the UI and stats.
  std::atomic<uint8_t> volume_{kDefaultVolume};
  std::atomic<uint8_t> control_{0};  // AbsVolumePolicy::Mode
  std::atomic<int16_t> headsetVolume_{-1};

  // Loop task only.
  bool wantAudio_ = false;      // as last handed to BtAppT
  uint32_t nextTickMs_ = 0;
  int16_t unsentVolume_ = -1;   // a setVolume() BtAppT's queue had no room for yet
  int unsentStep_ = 0;          // stepVolume()s it had no room for yet
  bool forgetPending_ = false;  // a forgetDevice() not handed to BtAppT yet
  bool suspendPending_ = false; // a suspendPromptly() waiting for the pause to reach BtAppT
  uint32_t suspendPendingMs_ = 0;

  // The gain stage: requests from BtAppT, applied by the data callback.
  GainRamp gain_;
  // ESP-IDF flushed the stream (it stopped): the next audio starts from silence.
  std::atomic<bool> restartFade_{false};
  // Bumped by BtAppT before every START and on every link: the next audio
  // after a pause in the callbacks belongs to a new stream (fade in).
  std::atomic<uint32_t> streamEpoch_{0};

  std::atomic<bool> forgotten_{false};  // set by BtAppT once it forgot the device

  // Data callback only, apart from the atomics.
  DeclickReader reader_{kConsumerId};
  int64_t lastDataUs_ = 0;
  uint32_t seenEpoch_ = 0;
  std::atomic<uint32_t> maxGapUs_{0};
  std::atomic<uint32_t> framesPulled_{0};
};
