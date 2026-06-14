#pragma once
#include <vector>
#include "ServerCandidate.h"

// HAL seam for finding mStream servers on the network.
//
//   Real build:  MdnsDiscovery browses `_mstream._tcp` via the ESP32 mDNS stack.
//   Test build:  a fake returns scripted sightings.
//
// poll() returns a *snapshot* of the servers currently visible. Turning those
// raw sightings into a stable, deduped, TTL'd list is DiscoveryController's job
// (host-tested) — this interface stays deliberately dumb.
class IDiscovery {
public:
  virtual ~IDiscovery() = default;

  virtual void begin() = 0;
  virtual void start() = 0;   // begin browsing
  virtual void stop() = 0;
  virtual std::vector<ServerCandidate> poll(uint32_t nowMs) = 0;
};
