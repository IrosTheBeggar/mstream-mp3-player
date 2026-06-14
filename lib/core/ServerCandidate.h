#pragma once
#include <cstdint>
#include <string>

// A discovered mStream server.
//
// Base-URL-shaped on purpose (per the one-binding design): the player cares
// about a reachable base URL, not how the server was found. mDNS fills these
// fields on the LAN today; a manually entered or invited URL can populate the
// same struct for a remote server later, with no change to anything downstream.
struct ServerCandidate {
  std::string instanceId;    // stable id from the TXT 'id' record — the dedupe key
  std::string instanceName;  // friendly name for the UI
  std::string host;          // resolved address (or hostname)
  uint16_t port = 0;
  std::string scheme;        // "http" | "https"
  std::string baseUrl;       // assembled scheme://host:port
  std::string version;       // server version, if advertised
  std::string publicUrl;     // optional public URL (sync-from-anywhere, later)
  uint32_t lastSeenMs = 0;   // stamped by DiscoveryController on each sighting
};
