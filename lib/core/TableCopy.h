// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ResamplerTables.h"

// The rate converter's polyphase tables copied into faster memory (on the
// ESP32, internal RAM: docs/RESAMPLER.md, section 10), as the stream at
// hand needs them: RateConverter's tables-wanted hook calls want().
//
// - want(true), a stream at a converted rate: the copy, made if there is
//   none (7,776 bytes); RateConverter::useTables() points at it.
// - want(false), a stream that starts at 44.1 kHz (the usual track):
//   useTables(nullptr, nullptr) first, then the copy freed, so the RAM is
//   back for everything else while nothing converts. RateConverter calls
//   this only at a stream's first rate after reset(), when nothing reads
//   the tables (RateConverter.h: setTablesWanted()).
// - No room (fragmentation): the flash tables, the same bits, only slower.
//   NoRoom the first time, StillNoRoom after, so the caller logs it once;
//   each later stream that wants the copy tries again.
//
// The allocation is handed in (heap_caps_malloc on the ESP32), so the host
// tests can fail it, count it, and poison what is freed. One task (the
// converter's).
class TableCopy {
public:
  static constexpr size_t kD147Bytes = sizeof(resampler::kD147);
  static constexpr size_t kU12Bytes = sizeof(resampler::kU12);
  static constexpr size_t kBytes = kD147Bytes + kU12Bytes;
  static_assert(kD147Bytes % 4 == 0, "the U12 copy must start 4-byte aligned");

  enum class Event : uint8_t { None, Copied, Freed, NoRoom, StillNoRoom };

  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  TableCopy(AllocFn alloc, FreeFn free) : alloc_(alloc), free_(free) {}

  // What the stream at hand needs; what was done about it.
  Event want(bool wanted);

  bool copied() const { return mem_ != nullptr; }
  // Allocations that failed (or came back unaligned), since boot.
  uint32_t failures() const { return failures_; }
  static const char* eventName(Event e);

private:
  AllocFn alloc_;
  FreeFn free_;
  void* mem_ = nullptr;
  uint32_t failures_ = 0;
};
