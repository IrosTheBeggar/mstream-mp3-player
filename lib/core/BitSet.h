#pragma once
#include <cstddef>
#include <cstdint>

// A set of row numbers, a bit each, in a block from allocator hooks (the
// firmware points them at PSRAM): a list's selection mode (the Queue's
// checkboxes). 10,000 rows are 1.25 KB. Portable, host-tested.
class BitSet {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  explicit BitSet(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~BitSet();
  BitSet(const BitSet&) = delete;
  BitSet& operator=(const BitSet&) = delete;

  // Room for rows [0, n), all clear. False: no memory (the set is then empty, size 0).
  bool resize(uint32_t n);
  uint32_t size() const { return size_; }
  void clear();
  // Out-of-range rows are ignored (get: false).
  bool get(uint32_t i) const { return i < size_ && (bits_[i >> 5] >> (i & 31)) & 1u; }
  void set(uint32_t i, bool on);
  void toggle(uint32_t i) { set(i, !get(i)); }
  void setAll(bool on);
  uint32_t count() const { return count_; }
  // The set rows in order into out (up to max); returns how many.
  uint32_t list(uint32_t* out, uint32_t max) const;

private:
  AllocFn allocFn_;
  FreeFn freeFn_;
  uint32_t* bits_ = nullptr;
  uint32_t size_ = 0;
  uint32_t count_ = 0;
};
