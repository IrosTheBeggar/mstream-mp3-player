// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueView.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace queueview {

// ---- DurationBook ----

DurationBook::DurationBook(AllocFn alloc, FreeFn release) : alloc_(alloc), free_(release) {}

DurationBook::~DurationBook() {
  if (s_) (free_ ? free_ : ::free)(s_);
}

bool DurationBook::reset(uint32_t tracks) {
  if (s_) (free_ ? free_ : ::free)(s_);
  s_ = nullptr;
  n_ = 0;
  ++version_;
  if (tracks == 0) return true;
  const size_t bytes = static_cast<size_t>(tracks) * sizeof(uint16_t);
  s_ = static_cast<uint16_t*>(alloc_ ? alloc_(bytes) : ::malloc(bytes));
  if (!s_) return false;
  std::memset(s_, 0, bytes);
  n_ = tracks;
  return true;
}

void DurationBook::note(uint32_t id, uint32_t ms) {
  if (id >= n_ || ms == 0) return;
  uint32_t s = (ms + 500) / 1000;
  if (s == 0) s = 1;
  if (s > 65535) s = 65535;
  if (s_[id] == s) return;
  s_[id] = static_cast<uint16_t>(s);
  ++version_;
}

Time upNextTime(const QueueModel& q, const DurationBook& book, uint32_t (*hintMs)(void* ctx, uint32_t id),
                void* ctx) {
  Time t;
  const int32_t c = q.current();
  if (c < 0) return t;
  for (uint32_t p = static_cast<uint32_t>(c) + 1; p < q.size(); ++p) {
    const uint32_t id = q.trackAt(p);
    uint32_t s = book.seconds(id);
    if (!s && hintMs) s = (hintMs(ctx, id) + 500) / 1000;
    if (s) {
      t.knownS += s;
    } else {
      ++t.unknown;
    }
  }
  return t;
}

namespace {
// " · 49 min" (or " · 2 h 5 min", "+" when only part is known) at buf + n.
void appendTime(const Time& t, char* buf, size_t bufSize, int n) {
  if (t.knownS == 0 || n < 0 || static_cast<size_t>(n) >= bufSize) return;
  const uint32_t mins = (t.knownS + 59) / 60;
  const char* more = t.unknown ? "+" : "";
  if (mins > 99) {
    snprintf(buf + n, bufSize - n, " \xC2\xB7 %lu h %lu%s min", static_cast<unsigned long>(mins / 60),
             static_cast<unsigned long>(mins % 60), more);
  } else {
    snprintf(buf + n, bufSize - n, " \xC2\xB7 %lu%s min", static_cast<unsigned long>(mins), more);
  }
}
}  // namespace

char* summary(uint32_t upNext, const Time& t, uint32_t position, uint32_t size, char* buf, size_t bufSize,
              bool withUpNext) {
  if (!buf || bufSize == 0) return buf;
  if (!withUpNext && position > 0) {
    const int n = snprintf(buf, bufSize, "%lu of %lu", static_cast<unsigned long>(position),
                           static_cast<unsigned long>(size));
    if (upNext > 0) appendTime(t, buf, bufSize, n);
    return buf;
  }
  int n = 0;
  if (position > 0) {
    n = snprintf(buf, bufSize, "%lu of %lu \xC2\xB7 ", static_cast<unsigned long>(position),
                 static_cast<unsigned long>(size));
    if (n < 0 || static_cast<size_t>(n) >= bufSize) return buf;
  }
  if (upNext == 0) {
    snprintf(buf + n, bufSize - n, "Nothing up next");
    return buf;
  }
  n += snprintf(buf + n, bufSize - n, "%lu up next", static_cast<unsigned long>(upNext));
  appendTime(t, buf, bufSize, n);
  return buf;
}

// ---- AddedMark ----

uint32_t AddedMark::firstPosition(const QueueModel& q) const {
  if (since_ == kNone) return kNone;
  for (uint32_t p = 0; p < q.size(); ++p) {
    if (marks(q.keyAt(p))) return p;
  }
  return kNone;
}

// ---- KeyRing ----

void KeyRing::add(uint32_t key) {
  if (key == QueueModel::kNone || has(key)) return;
  k_[next_] = key;
  next_ = (next_ + 1) % kSize;
  if (n_ < kSize) ++n_;
}

bool KeyRing::has(uint32_t key) const {
  for (int i = 0; i < n_; ++i) {
    if (k_[i] == key) return true;
  }
  return false;
}

// ---- shuffle ----

void shuffle(uint32_t* ids, uint32_t n, uint32_t seed) {
  if (!ids || n < 2) return;
  uint32_t x = seed ? seed : 1u;
  for (uint32_t i = n - 1; i > 0; --i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    const uint32_t j = static_cast<uint32_t>((static_cast<uint64_t>(x) * (i + 1)) >> 32);
    const uint32_t t = ids[i];
    ids[i] = ids[j];
    ids[j] = t;
  }
}

}  // namespace queueview
