// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DecoderArena.h"

DecoderArena::Where DecoderArena::where(const void* p, size_t bytes) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  if (!p || a < kPsramStart || a >= kPsramEnd) return Where::Elsewhere;
  const uintptr_t last = a + (bytes > 0 ? bytes - 1 : 0);
  return last < kPsramFastEnd ? Where::PsramLow : Where::PsramHigh;
}

const char* DecoderArena::whereName(Where w) {
  switch (w) {
    case Where::PsramLow:
      return "PSRAM, its lower 2 MB";
    case Where::PsramHigh:
      return "PSRAM above 0x3FA00000: slow";
    case Where::Elsewhere:
    default:
      return "not PSRAM";
  }
}

DecoderArena::DecoderArena(const size_t* sizes, size_t count) {
  count_ = count < kMaxParts ? count : kMaxParts;
  size_t at = 0;
  for (size_t i = 0; i < count_; ++i) {
    size_[i] = sizes[i];
    offset_[i] = at;
    at += alignUp(sizes[i]);
  }
  bytes_ = at;
}

bool DecoderArena::attach(void* block) {
  if (inUse_) return false;
  if (block && reinterpret_cast<uintptr_t>(block) % kAlign != 0) block = nullptr;
  block_ = static_cast<uint8_t*>(block);
  return block_ != nullptr;
}

void* DecoderArena::part(size_t i) const {
  if (!block_ || i >= count_) return nullptr;
  return block_ + offset_[i];
}

bool DecoderArena::claim() {
  if (!block_ || inUse_) {
    if (block_) ++refused_;
    return false;
  }
  inUse_ = true;
  ++claims_;
  return true;
}

void DecoderArena::release() { inUse_ = false; }
