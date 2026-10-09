// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// System facts and heap numbers for Device info (Output > About) and the
// serial log.
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

// The flash layout (partitions.csv), so a device run and bug reports can
// confirm it. At boot, one line: "[flash] running ota_0 at 0x10000 (6144K,
// OTA state undefined), next update ota_1 at 0x610000" after a serial or
// single-file install.
void logRunningPartition();
// The console's L: every partition as flashed (esp_partition_find), the
// running one marked, the version in the running app's description (what
// OTA compares), and how full NVS is.
void printPartitionTable();

}  // namespace diag
