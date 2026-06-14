#pragma once
#include <vector>
#include "Track.h"

// HAL seam for the music library source. On-device this is the SD card; when
// docked, the card is handed to the host as USB mass-storage and `available()`
// goes false until it is reclaimed.
class IStorage {
public:
  virtual ~IStorage() = default;

  virtual bool begin() = 0;
  virtual std::vector<Track> listTracks() = 0;
  virtual bool available() const = 0;

  // Release/reclaim the card around a USB-MSC dock handoff.
  virtual void releaseToHost() = 0;
  virtual void reclaim() = 0;
};
