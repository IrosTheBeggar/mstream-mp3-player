#pragma once
#include <cstdint>

// Minimal station-mode WiFi bring-up. Slice 1 brackets the nice onboarding UX
// (captive portal, etc.) and connects with build-time credentials — defaulting
// to Wokwi's open `Wokwi-GUEST` network so the simulator just works.
namespace Wifi {
void begin();
void loop(uint32_t nowMs);   // periodic reconnect if the link drops
bool isConnected();
}  // namespace Wifi
