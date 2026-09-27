#include "BitSet.h"

#include <cstdlib>
#include <cstring>

namespace {
void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }
uint32_t words(uint32_t n) { return (n + 31) / 32; }
}  // namespace

BitSet::BitSet(AllocFn alloc, FreeFn release)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree) {}

BitSet::~BitSet() {
  if (bits_) freeFn_(bits_);
}

bool BitSet::resize(uint32_t n) {
  if (bits_ && words(n) == words(size_)) {
    size_ = n;
    clear();
    return true;
  }
  if (bits_) freeFn_(bits_);
  bits_ = nullptr;
  size_ = count_ = 0;
  if (n == 0) return true;
  bits_ = static_cast<uint32_t*>(allocFn_(words(n) * sizeof(uint32_t)));
  if (!bits_) return false;
  size_ = n;
  clear();
  return true;
}

void BitSet::clear() {
  if (bits_) std::memset(bits_, 0, words(size_) * sizeof(uint32_t));
  count_ = 0;
}

void BitSet::set(uint32_t i, bool on) {
  if (i >= size_ || get(i) == on) return;
  bits_[i >> 5] ^= 1u << (i & 31);
  if (on) {
    ++count_;
  } else {
    --count_;
  }
}

void BitSet::setAll(bool on) {
  clear();
  if (!on || !size_) return;
  std::memset(bits_, 0xFF, words(size_) * sizeof(uint32_t));
  if (size_ & 31) bits_[size_ >> 5] = (1u << (size_ & 31)) - 1u;  // no bits past the end
  count_ = size_;
}

uint32_t BitSet::list(uint32_t* out, uint32_t max) const {
  uint32_t n = 0;
  for (uint32_t w = 0; w < words(size_) && n < max; ++w) {
    uint32_t b = bits_[w];
    while (b && n < max) {
      const uint32_t low = b & (~b + 1u);
      uint32_t bit = 0;
      while ((low >> bit) != 1u) ++bit;
      out[n++] = w * 32 + bit;
      b &= b - 1u;
    }
  }
  return n;
}
