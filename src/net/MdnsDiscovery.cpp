#include "net/MdnsDiscovery.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <string>

namespace {
constexpr uint32_t kQueryIntervalMs = 5000;
}

std::vector<ServerCandidate> MdnsDiscovery::poll(uint32_t nowMs) {
  if (!browsing_ || WiFi.status() != WL_CONNECTED) return cached_;

  // Start the mDNS responder once, after WiFi is up (MDNS.begin needs the link).
  if (!mdnsStarted_) {
    if (!MDNS.begin("mstream-player")) {
      Serial.println("[mdns] MDNS.begin failed");
      return cached_;
    }
    mdnsStarted_ = true;
  }

  // Rate-limit the (blocking) query; serve the cached snapshot in between.
  if (!firstQuery_ && (nowMs - lastQueryMs_) < kQueryIntervalMs) return cached_;
  firstQuery_ = false;
  lastQueryMs_ = nowMs;

  std::vector<ServerCandidate> found;
  const int n = MDNS.queryService("mstream", "tcp");
  Serial.printf("[mdns] query: %d mStream service(s)\n", n);
  for (int i = 0; i < n; ++i) {
    ServerCandidate s;
    s.instanceId = MDNS.txt(i, "id").c_str();
    const String name = MDNS.txt(i, "name");
    s.instanceName = name.length() ? std::string(name.c_str()) : std::string(MDNS.hostname(i).c_str());
    s.host = MDNS.IP(i).toString().c_str();
    s.port = MDNS.port(i);
    const String scheme = MDNS.txt(i, "scheme");
    s.scheme = scheme.length() ? std::string(scheme.c_str()) : std::string("http");
    s.version = MDNS.txt(i, "v").c_str();
    s.publicUrl = MDNS.txt(i, "pub").c_str();
    s.baseUrl = s.scheme + "://" + s.host + ":" + std::to_string(s.port);
    found.push_back(s);
  }

  cached_ = found;
  return cached_;
}
