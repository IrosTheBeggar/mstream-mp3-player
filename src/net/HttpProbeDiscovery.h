#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "hal/IDiscovery.h"

// A non-mDNS discovery source: probes a fixed base URL over HTTP and emits a
// *verified* ServerCandidate only if the server actually answers (GET /api/).
//
// Purpose: free Wokwi can't do LAN mDNS, but with the Private Gateway the sim
// can reach the host at `host.wokwi.internal`. Pointing this probe there makes
// discovery genuinely end-to-end in simulation against the real mStream. On
// hardware, MdnsDiscovery is the source instead; the two compose because
// DiscoveryController merges sightings from any number of sources.
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
