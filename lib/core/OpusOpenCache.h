// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ByteStream.h"
#include "OggOpus.h"

// What the Opus reader learnt about each file it opened (oggopus::
// OpenRecord: the headers, g0, the exact length, the last page), kept per
// path so the next open of the same file reads nothing but the one check
// (Reader::openFrom(): the BOS page's header): a seek on the playing
// track (the backend opens the file again for it), a track played
// before, the resume point at a boot (docs/OPUS.md section 10; M3's
// second open of a 128k file cost 13 reads and ~84 ms, the tail scan the
// most of it).
//
// An LRU of fixed entries in one block from the caller's allocator (PSRAM
// on the device: kDefaultEntries x 104 B, 4,992 B; two static_asserts
// below pin the sizes), keyed by the path's hash
// (FNV-1a 64, as the thumbnails' files are) and the file's size: a file
// that changed size is a miss, one rewritten at the same size is caught
// by the record's check at the open (the serial number and the head's
// CRC: re-encoded, a file has new ones; retagged with the same padding it
// keeps both and the record stays right). A hit is the most recently
// used; put() replaces the same key or takes the least recently used
// slot.
//
// Persisted on the card as one blob (the backend writes
// /.player/opus.idx from the loop task when it is dirty, as the queue is
// saved; the library's cache rules: versioned, summed, rebuilt from
// nothing when it doesn't read back): "MPOC", the version, the entry
// size, the count, the entries oldest first (a load puts them back in
// that order, so the recency survives), and Ogg's CRC-32 over all of it.
// A blob of another version or entry size is Outdated, one that doesn't
// sum or doesn't parse Corrupt: either way the cache starts empty and
// fills again as files are opened. Nothing in it is believed on its own:
// every record is checked against the file by Reader::openFrom() before
// it is used, which is what makes a stale or wrong entry harmless (one
// read, then the full open).
//
// Portable, host-tested (test_opus_open_cache). On the device the decode
// task finds and puts, the loop task saves: the backend holds a lock
// around each call.
class OpusOpenCache {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  static constexpr uint32_t kDefaultEntries = 48;
  // The blob (the class comment).
  static constexpr uint32_t kMagic = 0x434F504Du;  // "MPOC"
  static constexpr uint16_t kVersion = 1;
  static constexpr uint16_t kEntryBytes = 96;
  static constexpr size_t kHeaderBytes = 12;  // the magic, the version, the entry size, the count
  static constexpr size_t kSumBytes = 4;
  static size_t blobBytes(uint32_t entries) { return kHeaderBytes + static_cast<size_t>(entries) * kEntryBytes + kSumBytes; }

  struct Stats {
    uint32_t hits = 0, misses = 0, stored = 0, replaced = 0, evicted = 0, forgotten = 0;
  };
  enum class Load : uint8_t { Loaded, Empty, Outdated, Corrupt, NoMemory };
  static const char* loadName(Load l);

  // FNV-1a 64 of the path.
  static uint64_t hashPath(const char* path);

  OpusOpenCache() = default;
  ~OpusOpenCache();
  OpusOpenCache(const OpusOpenCache&) = delete;
  OpusOpenCache& operator=(const OpusOpenCache&) = delete;

  // `entries` slots from `alloc` (freed by `release`; null hooks: the
  // heap). False: no memory (the cache then holds nothing: every find is
  // a miss, every put is dropped).
  bool begin(uint32_t entries, AllocFn alloc = nullptr, FreeFn release = nullptr);
  uint32_t capacity() const { return cap_; }
  uint32_t size() const { return n_; }
  size_t bytes() const;

  // The record for `pathHash` and `fileSize`, copied out; a hit is the
  // most recently used.
  bool find(uint64_t pathHash, uint32_t fileSize, oggopus::OpenRecord* out);
  // Stores `rec` (its fileSize is the key's) for `pathHash`: the same key
  // replaced, else the least recently used slot taken (the entry of
  // another size under the same path goes: a file that changed). Dirty
  // after.
  void put(uint64_t pathHash, const oggopus::OpenRecord& rec);
  // The record for the key goes (its check refused it: the file changed
  // at the same size).
  void forget(uint64_t pathHash, uint32_t fileSize);
  void clear();

  // Changed since the last save() or load(), or clean(). markDirty(): the
  // blob save() made didn't reach the card (the backend's write or rename
  // failed), so the next save is owed again; a put() meanwhile has set it
  // already.
  bool dirty() const { return dirty_; }
  void clean() { dirty_ = false; }
  void markDirty() { dirty_ = true; }

  // The blob (the class comment), oldest entry first. False: the sink
  // refused (the cache stays dirty).
  bool save(ByteSink& out);
  // The blob back: Loaded (the entries, in their order; clean), Empty
  // (nothing read: no file yet), Outdated (another version's or entry
  // size: the cache is cleared), Corrupt (the magic, the count, the sum
  // or a short read: cleared), NoMemory (begin() hasn't a block).
  Load load(ByteSource& in);

  const Stats& stats() const { return stats_; }

private:
  struct Entry {
    uint64_t pathHash = 0;
    uint32_t used = 0;  // the use counter at its last use (the LRU)
    oggopus::OpenRecord rec;
  };
  // The sizes docs/OPUS.md section 10 quotes (the record on the decode
  // task's stack, the cache's block of PSRAM: 48 x 104 B), the same on
  // the Core2's xtensa-esp32-elf-g++ and the host's: a field added to the
  // record moves them, and the figures with them.
  static_assert(sizeof(oggopus::OpenRecord) == 88, "docs/OPUS.md section 10 quotes the record's size");
  static_assert(sizeof(Entry) == 104, "docs/OPUS.md section 10 quotes the cache's entry size");
  int32_t find(uint64_t pathHash, uint32_t fileSize) const;
  int32_t lruSlot() const;
  static void write(const Entry& e, uint8_t out[kEntryBytes]);
  static bool read(const uint8_t in[kEntryBytes], Entry* e);

  AllocFn alloc_ = nullptr;
  FreeFn free_ = nullptr;
  Entry* entries_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t n_ = 0;
  uint32_t tick_ = 0;
  bool dirty_ = false;
  Stats stats_;
};
