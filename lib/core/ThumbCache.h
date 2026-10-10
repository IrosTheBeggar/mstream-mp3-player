// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The album covers the UI has made into thumbnails, in memory (an LRU in
// PSRAM on the device), and the ones it still wants. Portable bookkeeping,
// host-tested; the pixels are made elsewhere (ui/Thumbs' worker task) and
// copied into a slot here. Loop task only.
//
//   - Two sizes, each its own pool of fixed slots: Small (40 x 40, the list
//     rows, 3.2 KB) and Large (96 x 96, Now Playing's cover, 18 KB). A new
//     one takes the least recently used slot of its size.
//   - want(): a row (or Now Playing) drew a placeholder and asks. Requests
//     are served newest first, so the rows on screen when a list stops come
//     before the ones it scrolled past; only the last kWanted are kept.
//   - A cover that can't be made (progressive, damaged, unreadable) is
//     remembered as failed and not asked for again (until clear()).
//
// Ids are the caller's (album ids of the library index); clear() when they
// change meaning (a rebuilt library).
class ThumbCache {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  enum class Size : uint8_t { Small = 0, Large = 1 };
  static constexpr int kSmallPx = 40;
  static constexpr int kLargePx = 96;
  static constexpr int px(Size s) { return s == Size::Small ? kSmallPx : kLargePx; }
  static constexpr size_t slotBytes(Size s) { return static_cast<size_t>(px(s)) * px(s) * 2; }
  static constexpr uint8_t sizeBit(Size s) { return static_cast<uint8_t>(1u << static_cast<int>(s)); }
  static constexpr int kWanted = 24;
  static constexpr int kFailed = 128;

  struct Stats {
    uint32_t hits = 0, misses = 0, stored = 0, evicted = 0, failed = 0;
  };

  explicit ThumbCache(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~ThumbCache();
  ThumbCache(const ThumbCache&) = delete;
  ThumbCache& operator=(const ThumbCache&) = delete;

  // The pools: `small` and `large` slots. False: no memory (nothing held).
  bool begin(uint32_t small, uint32_t large);
  uint32_t slots(Size s) const { return pool(s).n; }
  size_t bytes() const;

  // The pixels (px * px, big-endian RGB565) of `id` at `s`, or nullptr. A
  // hit makes it the most recently used.
  const uint16_t* get(uint32_t id, Size s);
  bool has(uint32_t id, Size s) const;
  bool failed(uint32_t id) const;

  // Asks for `id` at `s` (nothing if it's here, failed, or being made).
  void want(uint32_t id, Size s);
  // The newest request, and the sizes asked for it (bits: sizeBit(Size)); it
  // is now being made (done() ends that). False: nothing asked, or one is
  // being made already.
  bool next(uint32_t* id, uint8_t* sizes);
  uint32_t making() const { return making_; }
  uint32_t wanted() const { return wantedN_; }
  // Stores size `s` of `id` (copied in), in the least recently used slot.
  bool put(uint32_t id, Size s, const uint16_t* pixels);
  void markFailed(uint32_t id);
  // The one being made is finished (stored, failed or dropped).
  void done(uint32_t id);

  // Every id means something else now: all of it goes (the pools stay).
  void clear();
  // The pools given back (the library's update step, docs/METADATA.md
  // 3.4.2: about 315 KB for its build), with everything clear() drops;
  // begin() makes them again. Meanwhile nothing is stored or asked for.
  void release();
  const Stats& stats() const { return stats_; }

private:
  struct Pool {
    uint16_t* pixels = nullptr;  // n slots of slotBytes()
    uint32_t* ids = nullptr;
    uint32_t* used = nullptr;    // the use counter at its last use (LRU)
    uint32_t n = 0;
  };
  struct Want {
    uint32_t id;
    uint8_t sizes;
  };
  Pool& pool(Size s) { return pools_[static_cast<int>(s)]; }
  const Pool& pool(Size s) const { return pools_[static_cast<int>(s)]; }
  int find(const Pool& p, uint32_t id) const;
  void drop();

  AllocFn allocFn_;
  FreeFn freeFn_;
  Pool pools_[2];
  uint32_t tick_ = 0;
  Want wanted_[kWanted] = {};
  uint32_t wantedN_ = 0;  // oldest first
  uint32_t failed_[kFailed] = {};
  uint32_t failedN_ = 0;
  uint32_t failedNext_ = 0;
  uint32_t making_ = kNone;
  Stats stats_;
};

// The thumbnails on the card, so the next boot reads them instead of
// decoding the covers again: one file per cover image, named by a hash of
// its path, holding both sizes. 8.3 names (no long-name entries, which
// make a FAT folder slower to search) in 16 subfolders
// ("/.player/thumbs/7/7A0C31F2.565"), since opening a file searches its
// folder entry by entry. A cover that can't be decoded gets a header-only
// file with kNoPicture, so it isn't tried again at every boot.
//
//   0     header, 24 bytes (little-endian words): magic "MPTH", version,
//         flags, the path's 64-bit hash (a clash of the 32 bits in the
//         name reads as a miss), the cover file's size, the two sizes
//   24    40 x 40 big-endian RGB565 (3,200 bytes)
//   3224  96 x 96 (18,432 bytes)
namespace thumbfile {

constexpr uint32_t kMagic = 0x4854504Du;  // "MPTH"
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderBytes = 24;
constexpr uint8_t kNoPicture = 1;

struct Header {
  uint64_t pathHash = 0;
  uint32_t sourceBytes = 0;
  uint8_t flags = 0;
};

void write(const Header& h, uint8_t out[kHeaderBytes]);
// False: not a thumbnail file of this version and these sizes.
bool read(const uint8_t* in, size_t len, Header* h);
constexpr size_t offsetOf(ThumbCache::Size s) {
  return s == ThumbCache::Size::Small ? kHeaderBytes : kHeaderBytes + ThumbCache::slotBytes(ThumbCache::Size::Small);
}
constexpr size_t kFileBytes = kHeaderBytes + ThumbCache::slotBytes(ThumbCache::Size::Small) +
                              ThumbCache::slotBytes(ThumbCache::Size::Large);

// FNV-1a 64 of the path.
uint64_t pathHash(const char* path);
// "<dir>/<h>/<HHHHHHHH>.565" (h: the shard, the name's first hex digit);
// the shard's folder alone with `folderOnly`. False: didn't fit.
bool path(const char* dir, uint64_t hash, char* buf, size_t size, bool folderOnly = false);

}  // namespace thumbfile
