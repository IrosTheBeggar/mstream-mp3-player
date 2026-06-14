#pragma once
#include <cstdint>
#include <string>

// A single playable item. `path` is the SD path on-device, or later a remote
// id when streaming from mStream. Metadata is best-effort.
//
// Kept as a plain aggregate (no default member initializers) so brace-init
// lists work under the Arduino-ESP32 core's gnu++11. Fields omitted from a
// brace list are still value-initialized (durationMs -> 0).
struct Track {
  std::string path;
  std::string title;
  std::string artist;
  uint32_t durationMs;
};
