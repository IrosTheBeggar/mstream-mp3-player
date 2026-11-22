// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "CardContract.h"

// A run of track ids read in place, whatever holds them: the queue's id
// view (docs/LISTENING-STATS.md 7.6, "How a shelf acts"). A Library list
// hands the queue a u32 array (LibraryIndex::Span's ids); a stats shelf's
// rows are u16 ids inside the visit's snapshot, each between its row's
// facts (statsvisit::Snapshot::ids(), at kStrides' 4, 6 or 5 B). The queue
// copies the ids as it reads them (QueueModel's edits), so a view over the
// snapshot needs nothing copied or carved for the act: a transient u32
// array would be held across playNow()'s decoder open.
//
// Built only through of() and u16(): the one constructor that sets the
// fields is private, so IdView isn't an aggregate and no braced form
// ({&id, 1}, or {p, n} from a byte pointer, which would read u32 words)
// compiles. No public constructor takes a pointer, so a call with a u32
// array or nullptr (playNow(&id, 1, 0), replace(nullptr, 0, 0)) still
// picks the u32 forms. The default is an empty u32 view.
//
// A u16 id is read a byte at a time, little-endian as the snapshot writes
// it (cardcontract::get16): stride 5 puts every other id at an odd address,
// and an unaligned 16-bit load can fault on the Xtensa (INFERRED, not
// tried). A u32 id is read with memcpy, in the host's own order (it is a
// uint32_t array). Portable, host-tested (test_queue). No allocation.
struct IdView {
  const uint8_t* first = nullptr;  // the first id's address; nullptr: none (refused when count > 0)
  uint32_t count = 0;
  uint8_t stride = 4;  // bytes from one id to the next
  uint8_t width = 4;   // 2: u16, little-endian; 4: u32, the host's order

  IdView() = default;

  // A u32 array's `n` ids (nullptr: none).
  static IdView of(const uint32_t* ids, uint32_t n) {
    return IdView(reinterpret_cast<const uint8_t*>(ids), n, 4, 4);
  }
  // `n` u16 ids from `first`, one every `stride` bytes (2 to 255: a stats
  // list's kStrides[l], 4 to 6).
  static IdView u16(const uint8_t* first, uint32_t n, uint32_t stride) {
    return IdView(first, n, static_cast<uint8_t>(stride), 2);
  }

  // The i-th id (i < count, first not null).
  uint32_t operator[](uint32_t i) const {
    const uint8_t* p = first + static_cast<size_t>(i) * stride;
    if (width == 2) return cardcontract::get16(p);
    uint32_t id;
    std::memcpy(&id, p, sizeof(id));
    return id;
  }

 private:
  IdView(const uint8_t* f, uint32_t n, uint8_t s, uint8_t w) : first(f), count(n), stride(s), width(w) {}
};
// The overloads stay apart (the comment above): nothing turns a pointer or
// nullptr into a view.
static_assert(!std::is_convertible<const uint32_t*, IdView>::value, "a u32 array picks the array forms");
static_assert(!std::is_convertible<std::nullptr_t, IdView>::value, "nullptr picks the array forms");
static_assert(!std::is_aggregate<IdView>::value, "no braced view: only of() and u16() make one");
static_assert(!std::is_constructible<IdView, const uint8_t*, uint32_t>::value, "a byte pointer makes no u32 view");
