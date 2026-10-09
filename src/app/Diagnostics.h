// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// System facts and heap numbers for the bring-up screen and the serial log.
namespace diag {

struct Heap {
  uint32_t internalFree;     // internal RAM free now
  uint32_t internalMin;      // lowest internal free since boot
  uint32_t internalLargest;  // largest free internal block (fragmentation)
  uint32_t psramFree;
  uint32_t psramMin;         // lowest PSRAM free since the lows restarted (restartLows())
};

Heap heap();

// Logs "[heap] <stage> ..." so memory can be compared across boot stages
// (boot, storage, bluetooth, playing).
void logHeap(const char* stage);

// The lowest free of the heaps with `caps` (MALLOC_CAP_INTERNAL or
// MALLOC_CAP_SPIRAM): internal RAM's since the boot; PSRAM's since the
// last restartLows() (since the boot before the first). PSRAM's since-boot
// lowest reads 0 B from the first line of setup() on: the IDF's and the
// Arduino core's init emptied it for a moment before any of the player's
// code ran (the 2026-10-09 device run, B3), and it never moves again.
// heap_caps' minimum is reset only by its local window
// (heap_caps_monitor_local_minimum_free_size_start()), one at a time: these
// keep that window running from restartLows() on and their own floor across
// the windows they close (the update step's fence below reopens it). A dip
// in the few microseconds between closing a window and opening the next
// isn't seen.
uint32_t lowest(uint32_t caps);
// PSRAM's lowest counts from now (setup()'s first line, then its end: the
// running player's, which [stats]' pmin= and the [heap] lines' min= give);
// internal RAM's since-boot lowest goes on. Returns PSRAM's lowest until
// now (since the last restart; since the boot before the first).
uint32_t restartLows();
// PSRAM's since-boot lowest when the lows first restarted (0 B on the
// Core2: the init's moment); UINT32_MAX before.
uint32_t psramLowBeforeSetup();
// A window's own lowest PSRAM free (the update step's fence, METADATA.md
// 3.9 and 6.3.1 L4.1), lowest() still counting it. One window at a time;
// false: one already runs (or the IDF refused).
bool beginLowWindow();
// The window's lowest PSRAM free (lowest() keeps it too). Without a window:
// lowest(MALLOC_CAP_SPIRAM).
uint32_t endLowWindow();

// From setup(): a line for each failed allocation of PSRAM or of 4 KB or
// more (the first 16, then one in 256), from the task that asked:
// "[heap] FAILED: N B (caps 0x..) in <function>, task <name>; PSRAM free F
// B, largest block L B". The 2026-10-09 device run found PSRAM's lowest
// since the boot at 0 B in every session (a moment the heap was full) with
// no allocation failure logged by the code that asked: this names the
// request that met it.
void watchFailedAllocs();
uint32_t failedAllocs();  // counted, every one

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
