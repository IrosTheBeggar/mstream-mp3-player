#include "net/Wifi.h"

#include <Arduino.h>
#include <WiFi.h>

// Credentials come from build flags; defaults target the Wokwi simulator's open
// network. For real hardware, override -DWIFI_SSID / -DWIFI_PASS (or wire up a
// proper onboarding flow later).
#ifndef WIFI_SSID
#define WIFI_SSID "Wokwi-GUEST"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

namespace {
bool started = false;
uint32_t lastAttemptMs = 0;
constexpr uint32_t kRetryMs = 5000;
}  // namespace

namespace Wifi {

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  started = true;
  lastAttemptMs = 0;
  Serial.printf("[wifi] connecting to '%s'...\n", WIFI_SSID);
}

void loop(uint32_t nowMs) {
  if (!started) return;
  if (WiFi.status() != WL_CONNECTED && (nowMs - lastAttemptMs) >= kRetryMs) {
    lastAttemptMs = nowMs;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

}  // namespace Wifi
