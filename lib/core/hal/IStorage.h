#pragma once
#include <vector>
#include "Track.h"

// HAL seam for the local music library. On the Core2 this is the SD card, or
// the internal-flash filesystem when no card is inserted.
class IStorage {
public:
  virtual ~IStorage() = default;

  virtual bool begin() = 0;
  virtual std::vector<Track> listTracks() = 0;
  virtual bool available() const = 0;
};
