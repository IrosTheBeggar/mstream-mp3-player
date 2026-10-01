// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <esp_heap_caps.h>

#include <new>
#include <utility>

// PSRAM (MALLOC_CAP_SPIRAM) for anything big or long-lived: a malloc under
// 4 KB would otherwise land in internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL),
// which is scarce (~48-50 KB free while playing). The allocator hooks of
// the portable stores (LibraryIndex, QueueModel, MemorySink) point here.
inline void* psramAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
inline void psramFree(void* p) { heap_caps_free(p); }
template <typename T, typename... Args>
T* psramNew(Args&&... args) {
  void* p = psramAlloc(sizeof(T));
  return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
}
template <typename T>
void psramDelete(T* p) {
  if (!p) return;
  p->~T();
  psramFree(p);
}
