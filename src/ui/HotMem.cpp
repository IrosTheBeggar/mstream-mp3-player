// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/HotMem.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace ui {
namespace hotmem {

namespace {
constexpr size_t kLine = 32;  // the cache's line
constexpr uintptr_t kSlowFrom = 0x3FA00000;
uint8_t* block_ = nullptr;
size_t size_ = 0;
size_t used_ = 0;
}  // namespace

bool reserve(size_t bytes) {
  if (block_) return true;
  bytes = (bytes + kLine - 1) & ~(kLine - 1);
  block_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(kLine, bytes, MALLOC_CAP_SPIRAM));
  size_ = block_ ? bytes : 0;
  used_ = 0;
  print();
  return block_ != nullptr;
}

void* take(size_t bytes) {
  bytes = (bytes + kLine - 1) & ~(kLine - 1);
  if (!block_ || bytes > size_ - used_) return nullptr;
  void* p = block_ + used_;
  used_ += bytes;
  return p;
}

void print() {
  if (!block_) {
    Serial.println("[ui] list memory: no block reserved (the sprites allocate their own, anywhere in the PSRAM)");
    return;
  }
  const bool slow = reinterpret_cast<uintptr_t>(block_) + size_ > kSlowFrom;
  Serial.printf("[ui] list memory: %u B reserved at %p, PSRAM, %s; %u B handed out\n", static_cast<unsigned>(size_),
                block_, slow ? "above 0x3FA00000: slow (it shares core 1's cache with the code)" : "its lower 2 MB",
                static_cast<unsigned>(used_));
}

}  // namespace hotmem
}  // namespace ui
