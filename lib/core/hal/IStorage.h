#pragma once

// HAL seam for local storage. On the Core2 this is the SD card, or the
// internal-flash filesystem when no card is inserted. What's on it is read
// into the LibraryIndex (the library's single store), not listed here.
class IStorage {
public:
  virtual ~IStorage() = default;

  virtual bool begin() = 0;
  virtual bool available() const = 0;
};
