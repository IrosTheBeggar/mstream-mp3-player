#include "ThumbCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }
}  // namespace

ThumbCache::ThumbCache(AllocFn alloc, FreeFn release)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree) {}

ThumbCache::~ThumbCache() { drop(); }

void ThumbCache::drop() {
  for (Pool& p : pools_) {
    if (p.pixels) freeFn_(p.pixels);
    if (p.ids) freeFn_(p.ids);
    if (p.used) freeFn_(p.used);
    p = Pool{};
  }
}

bool ThumbCache::begin(uint32_t small, uint32_t large) {
  drop();
  const uint32_t counts[2] = {small, large};
  for (int k = 0; k < 2; ++k) {
    Pool& p = pools_[k];
    const uint32_t n = counts[k];
    if (n == 0) continue;
    p.pixels = static_cast<uint16_t*>(allocFn_(n * slotBytes(static_cast<Size>(k))));
    p.ids = static_cast<uint32_t*>(allocFn_(n * sizeof(uint32_t)));
    p.used = static_cast<uint32_t*>(allocFn_(n * sizeof(uint32_t)));
    if (!p.pixels || !p.ids || !p.used) {
      if (p.pixels) freeFn_(p.pixels);
      if (p.ids) freeFn_(p.ids);
      if (p.used) freeFn_(p.used);
      p = Pool{};
      drop();
      return false;
    }
    p.n = n;
  }
  clear();
  return true;
}

size_t ThumbCache::bytes() const {
  size_t b = 0;
  for (int k = 0; k < 2; ++k) b += pools_[k].n * (slotBytes(static_cast<Size>(k)) + 2 * sizeof(uint32_t));
  return b;
}

int ThumbCache::find(const Pool& p, uint32_t id) const {
  for (uint32_t i = 0; i < p.n; ++i) {
    if (p.ids[i] == id) return static_cast<int>(i);
  }
  return -1;
}

const uint16_t* ThumbCache::get(uint32_t id, Size s) {
  Pool& p = pool(s);
  const int i = id == kNone ? -1 : find(p, id);
  if (i < 0) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  p.used[i] = ++tick_;
  return p.pixels + static_cast<size_t>(i) * (slotBytes(s) / 2);
}

bool ThumbCache::has(uint32_t id, Size s) const { return id != kNone && find(pool(s), id) >= 0; }

bool ThumbCache::failed(uint32_t id) const {
  for (uint32_t i = 0; i < failedN_; ++i) {
    if (failed_[i] == id) return true;
  }
  return false;
}

void ThumbCache::want(uint32_t id, Size s) {
  if (id == kNone || pool(s).n == 0 || has(id, s) || failed(id) || id == making_) return;
  uint8_t sizes = sizeBit(s);
  // Asked already: it moves to the newest end, with the sizes of both asks.
  for (uint32_t i = 0; i < wantedN_; ++i) {
    if (wanted_[i].id != id) continue;
    sizes = static_cast<uint8_t>(sizes | wanted_[i].sizes);
    std::memmove(&wanted_[i], &wanted_[i + 1], (wantedN_ - i - 1) * sizeof(Want));
    --wantedN_;
    break;
  }
  if (wantedN_ == kWanted) {  // the oldest goes
    std::memmove(&wanted_[0], &wanted_[1], (kWanted - 1) * sizeof(Want));
    --wantedN_;
  }
  wanted_[wantedN_++] = Want{id, sizes};
}

bool ThumbCache::next(uint32_t* id, uint8_t* sizes) {
  if (wantedN_ == 0 || making_ != kNone) return false;
  const Want w = wanted_[--wantedN_];
  making_ = w.id;
  *id = w.id;
  *sizes = w.sizes;
  return true;
}

bool ThumbCache::put(uint32_t id, Size s, const uint16_t* pixels) {
  Pool& p = pool(s);
  if (p.n == 0 || !pixels || id == kNone) return false;
  int slot = find(p, id);
  if (slot < 0) {
    // A free slot, else the least recently used.
    uint32_t best = 0;
    for (uint32_t i = 0; i < p.n; ++i) {
      if (p.ids[i] == kNone) {
        best = i;
        break;
      }
      if (p.used[i] < p.used[best]) best = i;
    }
    if (p.ids[best] != kNone) ++stats_.evicted;
    slot = static_cast<int>(best);
  }
  std::memcpy(p.pixels + static_cast<size_t>(slot) * (slotBytes(s) / 2), pixels, slotBytes(s));
  p.ids[slot] = id;
  p.used[slot] = ++tick_;
  ++stats_.stored;
  return true;
}

void ThumbCache::markFailed(uint32_t id) {
  if (id == kNone || failed(id)) return;
  ++stats_.failed;
  if (failedN_ < kFailed) {
    failed_[failedN_++] = id;
  } else {
    failed_[failedNext_] = id;  // the oldest goes: at worst it is tried again
    failedNext_ = (failedNext_ + 1) % kFailed;
  }
}

void ThumbCache::done(uint32_t id) {
  if (making_ == id) making_ = kNone;
}

void ThumbCache::clear() {
  for (Pool& p : pools_) {
    for (uint32_t i = 0; i < p.n; ++i) {
      p.ids[i] = kNone;
      p.used[i] = 0;
    }
  }
  wantedN_ = 0;
  failedN_ = failedNext_ = 0;
  making_ = kNone;
}

namespace thumbfile {

namespace {
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
constexpr uint32_t kSizes =
    static_cast<uint32_t>(ThumbCache::kSmallPx) | static_cast<uint32_t>(ThumbCache::kLargePx) << 16;
}  // namespace

void write(const Header& h, uint8_t out[kHeaderBytes]) {
  std::memset(out, 0, kHeaderBytes);
  put32(out, kMagic);
  out[4] = static_cast<uint8_t>(kVersion);
  out[5] = static_cast<uint8_t>(kVersion >> 8);
  out[6] = h.flags;
  put32(out + 8, static_cast<uint32_t>(h.pathHash));
  put32(out + 12, static_cast<uint32_t>(h.pathHash >> 32));
  put32(out + 16, h.sourceBytes);
  put32(out + 20, kSizes);
}

bool read(const uint8_t* in, size_t len, Header* h) {
  if (!in || len < kHeaderBytes || get32(in) != kMagic) return false;
  if (static_cast<uint16_t>(in[4] | in[5] << 8) != kVersion || get32(in + 20) != kSizes) return false;
  h->flags = in[6];
  h->pathHash = static_cast<uint64_t>(get32(in + 8)) | static_cast<uint64_t>(get32(in + 12)) << 32;
  h->sourceBytes = get32(in + 16);
  return true;
}

uint64_t pathHash(const char* path) {
  uint64_t h = 14695981039346656037ull;
  for (const char* p = path; p && *p; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 1099511628211ull;
  }
  return h;
}

bool path(const char* dir, uint64_t hash, char* buf, size_t size, bool folderOnly) {
  const auto name = static_cast<unsigned>(hash >> 32);
  const int n = folderOnly ? snprintf(buf, size, "%s/%X", dir, name >> 28)
                           : snprintf(buf, size, "%s/%X/%08X.565", dir, name >> 28, name);
  return n > 0 && static_cast<size_t>(n) < size;
}

}  // namespace thumbfile
