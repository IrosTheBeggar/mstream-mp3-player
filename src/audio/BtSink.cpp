#include "audio/BtSink.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstring>

namespace {
BluetoothA2DPSource a2dp;
BtSink* sink = nullptr;  // ESP32-A2DP takes plain function pointers

// Without a configured name, only connect to a device this close (dBm). -70
// wasn't enough: a TV in the next room fluctuated past it and got paired.
constexpr int kMinRssi = -55;
// Reconnect attempts (~10 s apart) to the remembered device before scanning.
// The library's default of 1000 blocks discovery for hours if it's gone.
constexpr int kReconnectTries = 3;

constexpr const char* kPrefsNamespace = "player";
constexpr const char* kPrefsSinkName = "bt_name";

bool containsIgnoreCase(const char* haystack, const char* needle) {
  const size_t n = std::strlen(needle);
  for (const char* h = haystack; *h; ++h) {
    size_t i = 0;
    while (i < n && h[i] &&
           std::tolower(static_cast<unsigned char>(h[i])) ==
               std::tolower(static_cast<unsigned char>(needle[i]))) {
      ++i;
    }
    if (i == n) return true;
  }
  return false;
}
}  // namespace

void BtSink::begin(PcmRing& ring, AudioShared& shared, const char* defaultSinkName) {
  ring_ = &ring;
  shared_ = &shared;
  sink = this;

  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  const String saved = prefs.getString(kPrefsSinkName, defaultSinkName ? defaultSinkName : "");
  prefs.end();
  strlcpy(sinkNames_[0], saved.c_str(), sizeof(sinkNames_[0]));

  a2dp.set_local_name("mStream Player");
  a2dp.set_ssid_callback(onDeviceFound);
  a2dp.set_on_connection_state_changed(onConnectionChanged);
  a2dp.set_data_callback_in_frames(onData);
  a2dp.set_auto_reconnect(true, kReconnectTries);  // off by default in ESP32-A2DP
  a2dp.start();
}

bool BtSink::connected() const { return a2dp.is_connected(); }

void BtSink::setVolume(uint8_t percent) {
  a2dp.set_volume(static_cast<uint8_t>(percent * 127 / 100));
}

void BtSink::forgetDevice() { a2dp.clean_last_connection(); }

void BtSink::setSinkName(const char* name) {
  const uint8_t next = sinkNameIdx_.load() ^ 1;
  strlcpy(sinkNames_[next], name, sizeof(sinkNames_[next]));
  sinkNameIdx_.store(next);
  lastLogged_[0] = '\0';  // report devices again under the new rule

  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putString(kPrefsSinkName, sinkNames_[next]);
  prefs.end();
}

// Bluetooth task, every ~30 ms: no blocking, no logging.
int32_t BtSink::onData(Frame* frames, int32_t count) {
  if (count <= 0) return 0;
  BtSink& s = *sink;
  auto* out = reinterpret_cast<int16_t*>(frames);
  const bool playing = !s.shared_->paused.load(std::memory_order_relaxed);
  const uint32_t want = static_cast<uint32_t>(count);
  const uint32_t got = playing ? s.ring_->read(kConsumerId, out, want) : 0;
  if (got < want) {
    std::memset(out + got * 2, 0, (want - got) * sizeof(Frame));
    if (playing && s.ring_->consumer() == kConsumerId &&
        s.shared_->expectingAudio.load(std::memory_order_relaxed)) {
      s.shared_->underruns.fetch_add(1, std::memory_order_relaxed);
    }
  }
  s.framesPulled_.fetch_add(want, std::memory_order_relaxed);
  return count;
}

// Bluetooth task, during discovery. Only called for devices that advertise
// themselves as audio sinks.
bool BtSink::onDeviceFound(const char* name, esp_bd_addr_t, int rssi) {
  BtSink& s = *sink;
  const char* target = s.sinkName();
  const bool named = target[0] != '\0';
  const bool wanted = named ? containsIgnoreCase(name, target) : rssi >= kMinRssi;
  if (wanted) {
    strlcpy(s.deviceName_, name, sizeof(s.deviceName_));
    Serial.printf("[bt] connecting to \"%s\" (rssi %d)\n", name, rssi);
  } else if (std::strcmp(name, s.lastLogged_) != 0) {  // discovery repeats; log once
    Serial.printf("[bt] found \"%s\" (rssi %d), not using it: %s\n", name, rssi,
                  named ? "name doesn't match BT_SINK_NAME" : "not close enough, or set BT_SINK_NAME");
  }
  strlcpy(s.lastLogged_, name, sizeof(s.lastLogged_));
  return wanted;
}

void BtSink::onConnectionChanged(esp_a2d_connection_state_t state, void*) {
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    sink->linked_ = true;
    sink->event_ = Event::Connected;
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED && sink->linked_) {
    // Only a link that was up: a failed (re)connect attempt reports this too.
    sink->linked_ = false;
    sink->event_ = Event::Disconnected;
  }
}
