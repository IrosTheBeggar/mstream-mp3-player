// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TableCopy.h"

#include <cstring>  // std::memcpy

#include "RateConverter.h"

TableCopy::Event TableCopy::want(bool wanted) {
  if (!wanted) {
    if (!mem_) return Event::None;
    RateConverter::useTables(nullptr, nullptr);  // flash first: nothing points at the copy after this
    free_(mem_);
    mem_ = nullptr;
    return Event::Freed;
  }
  if (mem_) return Event::None;
  void* p = alloc_(kBytes);
  if (p && (reinterpret_cast<uintptr_t>(p) & 3u) != 0) {  // the kernel reads two taps at a time
    free_(p);
    p = nullptr;
  }
  if (!p) return ++failures_ == 1 ? Event::NoRoom : Event::StillNoRoom;
  auto* b = static_cast<uint8_t*>(p);
  std::memcpy(b, resampler::kD147, kD147Bytes);
  std::memcpy(b + kD147Bytes, resampler::kU12, kU12Bytes);
  mem_ = p;
  RateConverter::useTables(reinterpret_cast<const int16_t(*)[resampler::kTaps]>(b),
                           reinterpret_cast<const int16_t(*)[resampler::kTaps]>(b + kD147Bytes));
  return Event::Copied;
}

const char* TableCopy::eventName(Event e) {
  switch (e) {
    case Event::Copied: return "copied";
    case Event::Freed: return "freed";
    case Event::NoRoom: return "no room";
    case Event::StillNoRoom: return "still no room";
    case Event::None:
    default: return "none";
  }
}
