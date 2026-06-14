#pragma once
#include "hal/IStorage.h"

// SD-card library source over the shared SPI bus (Wokwi's microSD part).
//
// On the real board this becomes 4-bit SDIO via SD_MMC for throughput; the
// interface is identical, only begin()/scan internals change.
class SdStorage : public IStorage {
public:
  bool begin() override;
  std::vector<Track> listTracks() override;
  bool available() const override { return mounted_ && !releasedToHost_; }

  void releaseToHost() override;
  void reclaim() override;

private:
  bool mounted_ = false;
  bool releasedToHost_ = false;
};
