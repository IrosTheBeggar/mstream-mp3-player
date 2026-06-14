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

#ifdef DISCOVERY_FALLBACK_URL
  // Dev aid: Wokwi's virtual network may not forward mDNS multicast to the host
  // LAN, so the sim can't always see a real server. Define
  //   -DDISCOVERY_FALLBACK_URL=\"192.168.1.71:3000\"
  // to inject that server when no real results come back, exercising the whole
  // discovery UI in simulation against the Dockerized mStream.
  if (found.empty()) {
    const String hp = DISCOVERY_FALLBACK_URL;
    const int colon = hp.lastIndexOf(':');
    ServerCandidate s;
    s.instanceId = "fallback";
    s.instanceName = "mStream (fallback)";
    s.host = (colon > 0 ? hp.substring(0, colon) : hp).c_str();
    s.port = colon > 0 ? static_cast<uint16_t>(hp.substring(colon + 1).toInt()) : 3000;
    s.scheme = "http";
    s.baseUrl = s.scheme + "://" + s.host + ":" + std::to_string(s.port);
    found.push_back(s);
  }
#endif

  cached_ = found;
  return cached_;
}
