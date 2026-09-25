#pragma once
#include <BluetoothA2DPSource.h>

#include <atomic>
#include <cstdint>

#include "PcmRing.h"
#include "audio/AudioShared.h"

// Streams the ring to Bluetooth headphones with ESP32-A2DP (A2DP source). The
// Bluetooth stack pulls 44.1 kHz stereo from its own task every ~30 ms; that
// callback must never block, so it takes what the ring has and pads the rest
// with silence.
//
// Which headphones: the first audio device whose name contains the sink name
// (case-insensitive). Without a name, only a device practically touching the
// Core2 — signal strength alone once picked a TV in the next room. After
// connecting it reconnects to that device by itself, for ~30 s after boot
// before going back to scanning; forgetDevice() clears it.
class BtSink {
public:
  static constexpr uint8_t kConsumerId = 1;
  enum class Event : uint8_t { None, Connected, Disconnected };

  // Starts the Bluetooth stack. Call early: it claims ~70 KB of internal RAM,
  // best taken before the heap fragments. The sink name saved by setSinkName()
  // wins over `defaultSinkName` (the BT_SINK_NAME build flag).
  void begin(PcmRing& ring, AudioShared& shared, const char* defaultSinkName);

  // Changes which headphones to look for and saves it; applies from the next
  // device discovered. Empty = any very close device.
  void setSinkName(const char* name);
  const char* sinkName() const { return sinkNames_[sinkNameIdx_.load()]; }

  bool connected() const;
  // The device picked during discovery; empty after an automatic reconnect.
  const char* deviceName() const { return deviceName_; }
  void setVolume(uint8_t percent);
  // Drops the remembered device, so the next boot scans instead of reconnecting.
  void forgetDevice();
  // The latest connection change, for the UI loop to act on (then cleared).
  Event takeEvent() { return event_.exchange(Event::None); }
  // Frames handed to the Bluetooth stack, silence included: ~44100/s while
  // streaming.
  uint32_t framesPulled() const { return framesPulled_.load(std::memory_order_relaxed); }

private:
  static int32_t onData(Frame* frames, int32_t count);
  static bool onDeviceFound(const char* name, esp_bd_addr_t address, int rssi);
  static void onConnectionChanged(esp_a2d_connection_state_t state, void* obj);

  PcmRing* ring_ = nullptr;
  AudioShared* shared_ = nullptr;
  // Two copies, so setSinkName() on the loop task never rewrites the one the
  // Bluetooth task is comparing names against.
  char sinkNames_[2][64] = {"", ""};
  std::atomic<uint8_t> sinkNameIdx_{0};
  char deviceName_[64] = "";
  char lastLogged_[64] = "";
  std::atomic<Event> event_{Event::None};
  bool linked_ = false;  // touched only by the connection callback
  std::atomic<uint32_t> framesPulled_{0};
};
