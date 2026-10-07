// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "OpusOpenCache.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include "OggPage.h"  // ogg::crc32: the blob's sum

namespace {

void* heapAlloc(size_t n) { return std::malloc(n); }
void heapFree(void* p) { std::free(p); }

// Little-endian words into and out of the blob's fixed fields.
void put16(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32); }

constexpr uint8_t kContinues = 1;
constexpr uint8_t kChained = 2;

}  // namespace

const char* OpusOpenCache::loadName(Load l) {
  switch (l) {
    case Load::Loaded: return "loaded";
    case Load::Empty: return "no cache yet";
    case Load::Outdated: return "an older version's: rebuilt";
    case Load::Corrupt: return "unreadable: rebuilt";
    case Load::NoMemory: return "no memory";
  }
  return "?";
}

uint64_t OpusOpenCache::hashPath(const char* path) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const char* p = path; p && *p; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 0x100000001b3ull;
  }
  return h;
}

OpusOpenCache::~OpusOpenCache() {
  if (entries_ && free_) free_(entries_);
}

bool OpusOpenCache::begin(uint32_t entries, AllocFn alloc, FreeFn release) {
  if (entries_ && free_) free_(entries_);
  entries_ = nullptr;
  cap_ = n_ = 0;
  tick_ = 0;
  dirty_ = false;
  alloc_ = alloc ? alloc : heapAlloc;
  free_ = release ? release : heapFree;
  if (entries == 0) return false;
  void* p = alloc_(static_cast<size_t>(entries) * sizeof(Entry));
  if (!p) return false;
  entries_ = static_cast<Entry*>(p);
  for (uint32_t i = 0; i < entries; ++i) new (&entries_[i]) Entry();
  cap_ = entries;
  return true;
}

size_t OpusOpenCache::bytes() const { return static_cast<size_t>(cap_) * sizeof(Entry); }

int32_t OpusOpenCache::find(uint64_t pathHash, uint32_t fileSize) const {
  for (uint32_t i = 0; i < n_; ++i) {
    if (entries_[i].pathHash == pathHash && entries_[i].rec.fileSize == fileSize) return static_cast<int32_t>(i);
  }
  return -1;
}

int32_t OpusOpenCache::lruSlot() const {
  if (n_ < cap_) return static_cast<int32_t>(n_);
  int32_t oldest = 0;
  for (uint32_t i = 1; i < n_; ++i) {
    if (entries_[i].used < entries_[oldest].used) oldest = static_cast<int32_t>(i);
  }
  return oldest;
}

bool OpusOpenCache::find(uint64_t pathHash, uint32_t fileSize, oggopus::OpenRecord* out) {
  const int32_t i = find(pathHash, fileSize);
  if (i < 0) {
    ++stats_.misses;
    return false;
  }
  ++stats_.hits;
  entries_[i].used = ++tick_;
  *out = entries_[i].rec;
  return true;
}

void OpusOpenCache::put(uint64_t pathHash, const oggopus::OpenRecord& rec) {
  if (!entries_) return;
  // The same path at another size is the file changed: its entry goes
  // first, so one path never holds two.
  for (uint32_t i = 0; i < n_; ++i) {
    if (entries_[i].pathHash == pathHash && entries_[i].rec.fileSize != rec.fileSize) {
      entries_[i] = entries_[--n_];
      ++stats_.evicted;
      break;
    }
  }
  int32_t i = find(pathHash, rec.fileSize);
  if (i >= 0) {
    ++stats_.replaced;
  } else {
    i = lruSlot();
    if (static_cast<uint32_t>(i) < n_) {
      ++stats_.evicted;
    } else {
      ++n_;
    }
    ++stats_.stored;
  }
  entries_[i].pathHash = pathHash;
  entries_[i].rec = rec;
  entries_[i].used = ++tick_;
  dirty_ = true;
}

void OpusOpenCache::forget(uint64_t pathHash, uint32_t fileSize) {
  const int32_t i = find(pathHash, fileSize);
  if (i < 0) return;
  entries_[i] = entries_[--n_];
  ++stats_.forgotten;
  dirty_ = true;
}

void OpusOpenCache::clear() {
  n_ = 0;
  tick_ = 0;
  dirty_ = true;
}

void OpusOpenCache::write(const Entry& e, uint8_t out[kEntryBytes]) {
  const oggopus::OpenRecord& r = e.rec;
  std::memset(out, 0, kEntryBytes);
  put64(out, e.pathHash);
  put32(out + 8, r.fileSize);
  put32(out + 12, r.serial);
  put32(out + 16, r.headCrc);
  put32(out + 20, r.firstAudio);
  put32(out + 24, r.firstGranuleAt);
  put32(out + 28, r.firstGranuleEnd);
  put32(out + 32, r.firstGranuleSeq);
  out[36] = static_cast<uint8_t>((r.firstGranuleContinues ? kContinues : 0) | (r.chained ? kChained : 0));
  out[37] = r.head.version;
  out[38] = r.head.channels;
  out[39] = r.head.family;
  out[40] = r.head.streams;
  out[41] = r.head.coupled;
  out[42] = r.head.map[0];
  out[43] = r.head.map[1];
  put16(out + 44, r.head.preSkip);
  put32(out + 46, r.head.inputRate);
  put16(out + 50, static_cast<uint16_t>(r.head.gain));
  put64(out + 52, static_cast<uint64_t>(r.g0));
  put64(out + 60, static_cast<uint64_t>(r.firstGranule));
  put64(out + 68, static_cast<uint64_t>(r.lastGranule));
  put32(out + 76, r.lastPageAt);
  put32(out + 80, r.linkEnd);
  put32(out + 84, r.tagsBytes);
  put32(out + 88, r.tagsPages);
  // 92-95: spare, zero.
}

bool OpusOpenCache::read(const uint8_t in[kEntryBytes], Entry* e) {
  oggopus::OpenRecord& r = e->rec;
  r = oggopus::OpenRecord{};
  e->pathHash = get64(in);
  r.fileSize = get32(in + 8);
  r.serial = get32(in + 12);
  r.headCrc = get32(in + 16);
  r.firstAudio = get32(in + 20);
  r.firstGranuleAt = get32(in + 24);
  r.firstGranuleEnd = get32(in + 28);
  r.firstGranuleSeq = get32(in + 32);
  r.firstGranuleContinues = (in[36] & kContinues) != 0;
  r.chained = (in[36] & kChained) != 0;
  r.head.version = in[37];
  r.head.channels = in[38];
  r.head.family = in[39];
  r.head.streams = in[40];
  r.head.coupled = in[41];
  r.head.map[0] = in[42];
  r.head.map[1] = in[43];
  r.head.preSkip = get16(in + 44);
  r.head.inputRate = get32(in + 46);
  r.head.gain = static_cast<int16_t>(get16(in + 50));
  r.g0 = static_cast<int64_t>(get64(in + 52));
  r.firstGranule = static_cast<int64_t>(get64(in + 60));
  r.lastGranule = static_cast<int64_t>(get64(in + 68));
  r.lastPageAt = get32(in + 76);
  r.linkEnd = get32(in + 80);
  r.tagsBytes = get32(in + 84);
  r.tagsPages = get32(in + 88);
  // What no record of a real open holds (the reader's openFrom() checks
  // the rest against the file).
  return r.fileSize > 0 && r.lastGranule >= 0 && r.firstGranule >= 0 && r.g0 >= 0 && r.head.channels >= 1 &&
         r.head.channels <= 2;
}

bool OpusOpenCache::save(ByteSink& out) {
  uint8_t head[kHeaderBytes];
  put32(head, kMagic);
  put16(head + 4, kVersion);
  put16(head + 6, kEntryBytes);
  put32(head + 8, n_);
  uint32_t sum = ogg::crc32(head, sizeof(head));
  if (!out.write(head, sizeof(head))) return false;
  // Oldest first: a load puts them back in this order, so the least
  // recently used is the least recently used again. n_ is at most
  // kDefaultEntries or so: a selection by the use counter is fine.
  uint32_t written = 0;
  uint32_t lastUsed = 0;
  for (; written < n_; ++written) {
    int32_t next = -1;
    for (uint32_t i = 0; i < n_; ++i) {
      if (written > 0 && entries_[i].used <= lastUsed) continue;
      if (next < 0 || entries_[i].used < entries_[next].used) next = static_cast<int32_t>(i);
    }
    if (next < 0) break;  // (can't happen: the use counters are distinct)
    lastUsed = entries_[next].used;
    uint8_t buf[kEntryBytes];
    write(entries_[next], buf);
    sum = ogg::crc32(buf, sizeof(buf), sum);
    if (!out.write(buf, sizeof(buf))) return false;
  }
  uint8_t tail[kSumBytes];
  put32(tail, sum);
  if (!out.write(tail, sizeof(tail))) return false;
  dirty_ = false;
  return true;
}

OpusOpenCache::Load OpusOpenCache::load(ByteSource& in) {
  if (!entries_) return Load::NoMemory;
  n_ = 0;
  tick_ = 0;
  dirty_ = false;
  uint8_t head[kHeaderBytes];
  if (in.read(head, 1) == 0) return Load::Empty;  // (nothing at all: no file yet)
  if (!readFully(in, head + 1, sizeof(head) - 1) || get32(head) != kMagic) return Load::Corrupt;
  if (get16(head + 4) != kVersion || get16(head + 6) != kEntryBytes) return Load::Outdated;
  const uint32_t count = get32(head + 8);
  if (count > cap_) return Load::Corrupt;
  uint32_t sum = ogg::crc32(head, sizeof(head));
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t buf[kEntryBytes];
    if (!readFully(in, buf, sizeof(buf))) {
      n_ = 0;
      return Load::Corrupt;
    }
    sum = ogg::crc32(buf, sizeof(buf), sum);
    Entry e;
    if (!read(buf, &e)) {
      n_ = 0;
      return Load::Corrupt;
    }
    e.used = ++tick_;
    entries_[n_++] = e;
  }
  uint8_t tail[kSumBytes];
  if (!readFully(in, tail, sizeof(tail)) || get32(tail) != sum) {
    n_ = 0;
    tick_ = 0;
    return Load::Corrupt;
  }
  return Load::Loaded;
}
