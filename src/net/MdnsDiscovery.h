#pragma once
#include <vector>

#include "hal/IDiscovery.h"

// IDiscovery backed by the ESP32 mDNS stack. Browses `_mstream._tcp` and maps
// each result (host/port + TXT records) to a ServerCandidate.
//
// queryService() is blocking, so we rate-limit it and serve a cached snapshot
// between queries; DiscoveryController applies TTL/dedupe on top.
class MdnsDiscovery : public IDiscovery {
public:
  void begin() override {}                  // mDNS is started lazily once WiFi is up
  void start() override { browsing_ = true; firstQuery_ = true; }
  void stop() override { browsing_ = false; }
  std::vector<ServerCandidate> poll(uint32_t nowMs) override;

private:
  bool browsing_ = false;
  bool firstQuery_ = true;
  bool mdnsStarted_ = false;
  uint32_t lastQueryMs_ = 0;
  std::vector<ServerCandidate> cached_;
};
