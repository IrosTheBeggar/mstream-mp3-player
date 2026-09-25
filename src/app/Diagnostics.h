#pragma once
#include <cstdint>

// System facts and heap numbers for the bring-up screen and the serial log.
namespace diag {

struct Heap {
  uint32_t internalFree;     // internal RAM free now
  uint32_t internalMin;      // lowest internal free since boot
  uint32_t internalLargest;  // largest free internal block (fragmentation)
  uint32_t psramFree;
};

Heap heap();

// Logs "[heap] <stage> ..." so memory can be compared across boot stages
// (boot, storage, bluetooth, playing).
void logHeap(const char* stage);

const char* boardName();
const char* pmicName();
const char* imuName();
const char* resetReason();

}  // namespace diag
