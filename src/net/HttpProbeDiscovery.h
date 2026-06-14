#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "hal/IDiscovery.h"

// A non-mDNS discovery source: probes a fixed base URL over HTTP or HTTPS and
// emits a *verified* ServerCandidate only if the server actually answers
// (GET /api/).
//
// Purpose: free Wokwi can't do LAN mDNS. Two ways this makes the sim reach a
// real server: with the Private Gateway, point it at `host.wokwi.internal` for a
// local mStream; or, on the free Public Gateway (internet only), point it at a
// public `https://` mStream URL. On hardware, MdnsDiscovery is the source
// instead; the two compose because DiscoveryController merges sightings from any
// number of sources.
class HttpProbeDiscovery : public IDiscovery {
public:
  explicit HttpProbeDiscovery(const char* baseUrl) : baseUrl_(baseUrl) {}

  void begin() override;
  void start() override { browsing_ = true; firstProbe_ = true; }
  void stop() override { browsing_ = false; }
  std::vector<ServerCandidate> poll(uint32_t nowMs) override;

private:
  void parseUrl();

  std::string baseUrl_;
  std::string scheme_;
  std::string host_;
  uint16_t port_ = 0;
  bool parsed_ = false;
  bool browsing_ = false;
  bool firstProbe_ = true;
  uint32_t lastProbeMs_ = 0;
  std::vector<ServerCandidate> cached_;
};
