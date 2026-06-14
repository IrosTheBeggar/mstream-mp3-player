#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ServerCandidate.h"

// Turns a stream of raw server sightings (from an IDiscovery) into a stable,
// deduped, TTL-pruned, sorted list the UI can scroll — plus a selection cursor.
//
// Framework-agnostic and driven by an injected clock (update(nowMs, ...)), so it
// is unit-tested on the host like PlaybackController / DockController.
class DiscoveryController {
public:
  // ttlMs: drop a server not seen again within this window (handles servers
  // that quietly disappear without a goodbye).
  explicit DiscoveryController(uint32_t ttlMs = 15000) : ttlMs_(ttlMs) {}

  // Merge a fresh batch of sightings observed at nowMs, then expire stale ones.
  void update(uint32_t nowMs, const std::vector<ServerCandidate>& sightings);

  const std::vector<ServerCandidate>& servers() const { return servers_; }
  size_t count() const { return servers_.size(); }

  // Selection cursor (driven by the encoder). -1 when the list is empty.
  int selectedIndex() const { return servers_.empty() ? -1 : selected_; }
  void moveSelection(int delta);
  const ServerCandidate* selected() const;

private:
  std::string keyOf(const ServerCandidate& s) const;

  uint32_t ttlMs_;
  std::vector<ServerCandidate> servers_;
  int selected_ = 0;
};
