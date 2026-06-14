#include "DiscoveryController.h"

#include <algorithm>

std::string DiscoveryController::keyOf(const ServerCandidate& s) const {
  // Prefer the stable advertised id; fall back to host:port when a server
  // doesn't advertise one (so we still dedupe and don't double-list it).
  if (!s.instanceId.empty()) return s.instanceId;
  return s.host + ":" + std::to_string(s.port);
}

void DiscoveryController::update(uint32_t nowMs, const std::vector<ServerCandidate>& sightings) {
  // Remember which server was selected so the cursor follows it across re-sorts.
  std::string selectedKey;
  if (selected_ >= 0 && selected_ < static_cast<int>(servers_.size())) {
    selectedKey = keyOf(servers_[selected_]);
  }

  // Upsert each sighting (update in place on a key match, else append).
  for (const auto& sighting : sightings) {
    const std::string k = keyOf(sighting);
    bool merged = false;
    for (auto& existing : servers_) {
      if (keyOf(existing) == k) {
        existing = sighting;
        existing.lastSeenMs = nowMs;
        merged = true;
        break;
      }
    }
    if (!merged) {
      ServerCandidate added = sighting;
      added.lastSeenMs = nowMs;
      servers_.push_back(added);
    }
  }

  // Expire servers not seen within the TTL window.
  std::vector<ServerCandidate> kept;
  kept.reserve(servers_.size());
  for (const auto& s : servers_) {
    if (nowMs - s.lastSeenMs <= ttlMs_) kept.push_back(s);
  }
  servers_ = std::move(kept);

  // Stable order so the list doesn't jump around as servers come and go.
  std::sort(servers_.begin(), servers_.end(),
            [this](const ServerCandidate& a, const ServerCandidate& b) {
              if (a.instanceName != b.instanceName) return a.instanceName < b.instanceName;
              return keyOf(a) < keyOf(b);
            });

  // Restore the cursor onto the same server it was on, else clamp to the top.
  selected_ = 0;
  if (!servers_.empty() && !selectedKey.empty()) {
    for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
      if (keyOf(servers_[i]) == selectedKey) { selected_ = i; break; }
    }
  }
}

void DiscoveryController::moveSelection(int delta) {
  if (servers_.empty()) { selected_ = 0; return; }
  const int n = static_cast<int>(servers_.size());
  selected_ += delta;
  if (selected_ < 0) selected_ = 0;
  if (selected_ >= n) selected_ = n - 1;
}

const ServerCandidate* DiscoveryController::selected() const {
  if (servers_.empty()) return nullptr;
  return &servers_[selected_];
}
