// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DecoderParts.h"

DecoderParts::DecoderParts(DecoderParts&& other) noexcept
    : arena_(other.arena_), count_(other.count_), failed_(other.failed_) {
  for (size_t i = 0; i < count_; ++i) {
    part_[i] = other.part_[i];
    free_[i] = other.free_[i];
  }
  other.arena_ = nullptr;
  other.count_ = 0;
  other.failed_ = false;
}

bool DecoderParts::claim(DecoderArena& arena) {
  if (count_ > 0 || arena_ || failed_) return false;
  if (arena.parts() > kMaxParts || !arena.claim()) return false;
  arena_ = &arena;
  for (size_t i = 0; i < arena.parts(); ++i) {
    part_[i] = arena.part(i);
    free_[i] = nullptr;
  }
  count_ = arena.parts();
  return true;
}

void DecoderParts::add(void* p, FreeFn free) {
  if (!p) {
    failed_ = true;
    return;
  }
  if (count_ >= kMaxParts || !free) {
    failed_ = true;
    if (free) free(p);
    return;
  }
  part_[count_] = p;
  free_[count_] = free;
  ++count_;
}

void DecoderParts::release() {
  for (size_t i = 0; i < count_; ++i) {
    if (free_[i]) free_[i](part_[i]);
    part_[i] = nullptr;
    free_[i] = nullptr;
  }
  count_ = 0;
  failed_ = false;
  if (arena_) {
    arena_->release();
    arena_ = nullptr;
  }
}
