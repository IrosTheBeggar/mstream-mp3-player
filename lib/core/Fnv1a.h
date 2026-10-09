// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// FNV-1a 32 (docs/DANCE-SPRITES.md, B.10): h starts at 0x811C9DC5 and, for
// each byte, h = (h XOR byte) * 0x01000193 mod 2^32. The dance code uses it
// for two things that tools/dancepack.py computes the same way:
//   - a genre's key: the hash of its folded text (GenreFold.h, B.8);
//   - a frame's hash: the hash of its cell grid, cellH rows of cellW palette
//     indices (A.16), which the tests compare with the generated tables.
// It is QueueSaver's hash; LibraryIndex, TouchCalibration and QueueSaver keep
// their own file-local copies.
//
// `h` continues an earlier result, so a hash can be built from pieces:
// fnv1a32(b, nb, fnv1a32(a, na)) is the hash of a then b. Header-only,
// portable, no allocation.
namespace dance {

constexpr uint32_t kFnvBasis = 0x811C9DC5u;

// FNV-1a 32 (B.10) of n bytes, continued from h: fnv1a32("a", 1) is
// 0xE40C292C, and n = 0 gives h. data may be null when n is 0.
inline uint32_t fnv1a32(const void* data, size_t n, uint32_t h = kFnvBasis) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x01000193u;
  }
  return h;
}

}  // namespace dance
