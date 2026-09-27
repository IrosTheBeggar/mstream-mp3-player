#include "QueueModel.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

bool isSet(const uint32_t* bits, uint32_t i) { return (bits[i >> 5] >> (i & 31)) & 1u; }

}  // namespace

QueueModel::QueueModel(AllocFn alloc, FreeFn release)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree) {}

QueueModel::~QueueModel() {
  drop(q_);
  drop(undo_);
}

bool QueueModel::reserve(Array& a, uint32_t n) {
  if (n <= a.cap) return true;
  uint32_t cap = a.cap ? a.cap * 2 : 16;
  if (cap < n) cap = n;
  auto* p = static_cast<Entry*>(allocFn_(static_cast<size_t>(cap) * sizeof(Entry)));
  if (!p) return false;
  if (a.size) std::memcpy(p, a.data, static_cast<size_t>(a.size) * sizeof(Entry));
  if (a.data) freeFn_(a.data);
  a.data = p;
  a.cap = cap;
  return true;
}

void QueueModel::drop(Array& a) {
  if (a.data) freeFn_(a.data);
  a = Array{};
}

void QueueModel::changed() {
  ++contentVersion_;
  ++positionVersion_;
}

uint32_t QueueModel::positionOf(uint32_t key) const {
  if (key == kNone) return kNone;
  for (uint32_t i = 0; i < q_.size; ++i) {
    if (q_.data[i].key == key) return i;
  }
  return kNone;
}

bool QueueModel::snapshot(Edit edit) {
  undoEdit_ = Edit::None;  // the older snapshot is gone either way
  if (!reserve(undo_, q_.size)) return false;
  if (q_.size) std::memcpy(undo_.data, q_.data, static_cast<size_t>(q_.size) * sizeof(Entry));
  undo_.size = q_.size;
  undoCurrent_ = current_;
  undoEdit_ = edit;
  return true;
}

uint32_t* QueueModel::selection(const uint32_t* positions, uint32_t n, uint32_t* count) {
  *count = 0;
  if (q_.size == 0 || n == 0 || !positions) return nullptr;
  const size_t bytes = (static_cast<size_t>(q_.size) + 31) / 32 * sizeof(uint32_t);
  auto* bits = static_cast<uint32_t*>(allocFn_(bytes));
  if (!bits) return nullptr;
  std::memset(bits, 0, bytes);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t p = positions[k];
    if (p >= q_.size || isSet(bits, p)) continue;
    bits[p >> 5] |= 1u << (p & 31);
    ++*count;
  }
  return bits;
}

bool QueueModel::insertAt(uint32_t at, const uint32_t* tracks, uint32_t n, Edit edit) {
  if (n == 0) return true;
  if (!tracks || q_.size + n < q_.size || !reserve(q_, q_.size + n)) return false;
  snapshot(edit);
  std::memmove(q_.data + at + n, q_.data + at, static_cast<size_t>(q_.size - at) * sizeof(Entry));
  for (uint32_t i = 0; i < n; ++i) {
    q_.data[at + i] = Entry{tracks[i], nextKey_++};
    if (nextKey_ == kNone) nextKey_ = 0;
  }
  q_.size += n;
  if (current_ < 0) {
    current_ = static_cast<int32_t>(at);  // the queue was empty: the first new entry
  } else if (static_cast<int32_t>(at) <= current_) {
    current_ += static_cast<int32_t>(n);
  }
  changed();
  return true;
}

bool QueueModel::insertNext(const uint32_t* tracks, uint32_t n) {
  const uint32_t at = current_ < 0 ? q_.size : static_cast<uint32_t>(current_) + 1;
  return insertAt(at, tracks, n, Edit::InsertNext);
}

bool QueueModel::append(const uint32_t* tracks, uint32_t n) { return insertAt(q_.size, tracks, n, Edit::Append); }

bool QueueModel::replace(const uint32_t* tracks, uint32_t n, uint32_t start) {
  if (n == 0) return clear();
  if (!tracks || !reserve(q_, n)) return false;
  snapshot(Edit::Replace);
  for (uint32_t i = 0; i < n; ++i) {
    q_.data[i] = Entry{tracks[i], nextKey_++};
    if (nextKey_ == kNone) nextKey_ = 0;
  }
  q_.size = n;
  current_ = static_cast<int32_t>(start < n ? start : n - 1);
  changed();
  return true;
}

QueueModel::Removed QueueModel::remove(const uint32_t* positions, uint32_t n) {
  Removed r;
  uint32_t count = 0;
  uint32_t* bits = selection(positions, n, &count);
  if (!bits) return r;  // nothing selected, or no memory for the selection
  if (count == 0) {
    freeFn_(bits);
    return r;
  }
  snapshot(Edit::Remove);
  const int32_t old = current_;
  int32_t kept = -1, firstAfter = -1, lastBefore = -1;
  uint32_t w = 0;
  for (uint32_t i = 0; i < q_.size; ++i) {
    const auto pos = static_cast<int32_t>(i);
    if (isSet(bits, i)) {
      if (pos == old) r.current = true;
      continue;
    }
    if (pos < old) {
      lastBefore = static_cast<int32_t>(w);
    } else if (pos == old) {
      kept = static_cast<int32_t>(w);
    } else if (firstAfter < 0) {
      firstAfter = static_cast<int32_t>(w);
    }
    q_.data[w++] = q_.data[i];
  }
  freeFn_(bits);
  r.count = q_.size - w;
  q_.size = w;
  if (!r.current) {
    current_ = kept;
  } else if (firstAfter >= 0) {
    current_ = firstAfter;
  } else {
    current_ = lastBefore;  // -1 when the queue is now empty
    r.pastEnd = true;
  }
  changed();
  return r;
}

bool QueueModel::moveNext(const uint32_t* positions, uint32_t n) {
  if (current_ < 0) return true;  // nothing to move after
  uint32_t count = 0;
  uint32_t* bits = selection(positions, n, &count);
  if (!bits) return n == 0 || !positions;  // no memory, unless nothing was asked
  const auto cur = static_cast<uint32_t>(current_);
  if (isSet(bits, cur)) {  // the current entry can't go after itself
    bits[cur >> 5] &= ~(1u << (cur & 31));
    --count;
  }
  if (count == 0) {
    freeFn_(bits);
    return true;
  }
  // The snapshot is also the source: the queue is rebuilt from it.
  if (!snapshot(Edit::MoveNext)) {
    freeFn_(bits);
    return false;
  }
  uint32_t w = 0;
  for (uint32_t i = 0; i < undo_.size; ++i) {
    if (isSet(bits, i)) continue;
    q_.data[w++] = undo_.data[i];
    if (i != cur) continue;
    current_ = static_cast<int32_t>(w - 1);
    for (uint32_t j = 0; j < undo_.size; ++j) {
      if (isSet(bits, j)) q_.data[w++] = undo_.data[j];
    }
  }
  freeFn_(bits);
  changed();
  return true;
}

bool QueueModel::clearUpNext() {
  if (current_ < 0 || static_cast<uint32_t>(current_) + 1 >= q_.size) return true;
  snapshot(Edit::ClearUpNext);
  q_.size = static_cast<uint32_t>(current_) + 1;
  changed();
  return true;
}

bool QueueModel::clear() {
  if (q_.size == 0) return true;
  snapshot(Edit::Clear);
  q_.size = 0;
  current_ = -1;
  changed();
  return true;
}

bool QueueModel::setCurrent(uint32_t pos) {
  if (pos >= q_.size) return false;
  if (current_ != static_cast<int32_t>(pos)) {
    current_ = static_cast<int32_t>(pos);
    ++positionVersion_;
  }
  return true;
}

bool QueueModel::step(int delta, bool wrap) {
  if (current_ < 0) return false;
  const int64_t n = q_.size;
  int64_t next = static_cast<int64_t>(current_) + delta;
  if (next < 0 || next >= n) {
    if (!wrap) return false;
    next = ((next % n) + n) % n;
  }
  current_ = static_cast<int32_t>(next);
  ++positionVersion_;
  return true;
}

bool QueueModel::undo() {
  if (undoEdit_ == Edit::None) return false;
  const uint32_t key = currentKey();
  std::swap(q_, undo_);  // undo_ keeps the edited entries' memory for the next snapshot
  undoEdit_ = Edit::None;
  const uint32_t pos = positionOf(key);
  current_ = pos != kNone ? static_cast<int32_t>(pos) : undoCurrent_;
  if (current_ >= static_cast<int32_t>(q_.size)) current_ = static_cast<int32_t>(q_.size) - 1;
  changed();
  return true;
}

void QueueModel::dropUndo() {
  undoEdit_ = Edit::None;
  drop(undo_);
}

bool QueueModel::assign(const uint32_t* tracks, uint32_t n, int32_t current) {
  if (n && (!tracks || !reserve(q_, n))) return false;
  for (uint32_t i = 0; i < n; ++i) {
    q_.data[i] = Entry{tracks[i], nextKey_++};
    if (nextKey_ == kNone) nextKey_ = 0;
  }
  q_.size = n;
  if (n == 0) {
    current_ = -1;
  } else {
    current_ = current < 0 ? 0 : (current >= static_cast<int32_t>(n) ? static_cast<int32_t>(n) - 1 : current);
  }
  dropUndo();
  changed();
  return true;
}
