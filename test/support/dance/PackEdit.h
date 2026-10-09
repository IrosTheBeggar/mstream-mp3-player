// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Test-only helpers for the dance pack suites (test_dance_pack and
// test_dance_blit): docs/DANCE-SPRITES.md, Appendix D.4. Header-only; it
// may use std::vector because nothing here reaches the firmware.
//
// - HeapCopy: an exact-size `new uint8_t[n]` copy of some bytes, freed by
//   its destructor. Every pack, scan buffer and frame either suite hands
//   the reader or the blitter is one, so AddressSanitizer reports a read
//   one byte past what the reader was given (and LeakSanitizer a leak).
//   A copy of 0 bytes is a null pointer, which open() and scan() accept
//   with avail 0.
// - refCrc32 and refFnv1a32: zlib's CRC-32 (A.2) and FNV-1a 32 (B.10)
//   written again, bit by bit, from the doc, so the packs the tests build
//   don't depend on the implementation under test (test_dance_pack checks
//   dance::crc32 and dance::fnv1a32 against them).
// - Little-endian reads and writes; fixSize and fixCrc (the header's total
//   size at byte 8 and the CRC of bytes 16.. at byte 12); and patch8/16/32
//   and patchRaw, which write and then fix the CRC, as test_dancepack.py's
//   patch() does (the size is left alone).
// - assemble(chunks): the header and directory of A.2-A.4 around chunks
//   laid out in the order given, each at the next multiple of 4, as
//   test_dancepack.py's assemble_mpdn. dotChunks()/dotPack(): A.17's pack
//   built from its chunk table, not from dancepack.py. dotWith(changes): its
//   a17_with: A.4's nine chunks with some replaced, extra tags after them,
//   sorted by tag.
// - directory(): the directory entries of a pack (the test's own parse).
// - Xorshift32: D.3's generator (x ^= x << 13; x ^= x >> 17; x ^= x << 5).
// - Drops: a Report::onDrop collector of up to 64 (move, text) pairs.
// - CanaryCanvas: a BlitCanvas inside an exact-size heap buffer with a
//   border of canary pixels on every side.
// - readerCase(i): ReaderVectors.h case i's bytes, D.3's steps 1-3.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "DanceBlit.h"
#include "DancePack.h"
#include "ReaderVectors.h"
#include "TestPack.h"

namespace packedit {

using Bytes = std::vector<uint8_t>;

// --- exact-size heap copies -------------------------------------------------

class HeapCopy {
 public:
  HeapCopy() = default;
  HeapCopy(const uint8_t* src, size_t n) { assign(src, n); }
  ~HeapCopy() { delete[] p_; }
  HeapCopy(const HeapCopy&) = delete;
  HeapCopy& operator=(const HeapCopy&) = delete;

  // n bytes from src (src may be null when n is 0): a fresh block of exactly
  // n bytes, or null for n = 0.
  void assign(const uint8_t* src, size_t n) {
    delete[] p_;
    p_ = nullptr;
    n_ = n;
    if (n == 0) return;
    p_ = new uint8_t[n];
    std::memcpy(p_, src, n);
  }
  void assign(const Bytes& b, size_t n) { assign(b.data(), std::min(n, b.size())); }
  void assign(const Bytes& b) { assign(b.data(), b.size()); }

  const uint8_t* data() const { return p_; }
  uint8_t* data() { return p_; }
  size_t size() const { return n_; }

 private:
  uint8_t* p_ = nullptr;
  size_t n_ = 0;
};

// --- the reference checksums --------------------------------------------------

// zlib's CRC-32 (A.2): reflected 0xEDB88320, init and final XOR 0xFFFFFFFF;
// prev continues an earlier result, as zlib.crc32's second argument does.
inline uint32_t refCrc32(const uint8_t* p, size_t n, uint32_t prev = 0) {
  uint32_t c = prev ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return c ^ 0xFFFFFFFFu;
}

// FNV-1a 32 (B.10).
inline uint32_t refFnv1a32(const void* data, size_t n, uint32_t h = 0x811C9DC5u) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

// --- little-endian fields -----------------------------------------------------

inline uint32_t get8(const Bytes& b, size_t off) { return b.at(off); }
inline uint32_t get16(const Bytes& b, size_t off) { return b.at(off) | (b.at(off + 1) << 8); }
inline uint32_t get32(const Bytes& b, size_t off) {
  return b.at(off) | (b.at(off + 1) << 8) | (b.at(off + 2) << 16) | (static_cast<uint32_t>(b.at(off + 3)) << 24);
}
inline void put8(Bytes& b, size_t off, uint32_t v) { b.at(off) = static_cast<uint8_t>(v); }
inline void put16(Bytes& b, size_t off, uint32_t v) {
  b.at(off) = static_cast<uint8_t>(v);
  b.at(off + 1) = static_cast<uint8_t>(v >> 8);
}
inline void put32(Bytes& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.at(off + i) = static_cast<uint8_t>(v >> (8 * i));
}
// Writes `width` (1, 2 or 4) bytes of v.
inline void putN(Bytes& b, size_t off, int width, uint32_t v) {
  for (int i = 0; i < width; ++i) b.at(off + i) = static_cast<uint8_t>(v >> (8 * i));
}

inline void fixSize(Bytes& b) {
  if (b.size() >= 12) put32(b, 8, static_cast<uint32_t>(b.size()));
}
inline void fixCrc(Bytes& b) {
  if (b.size() >= 16) put32(b, 12, refCrc32(b.data() + 16, b.size() - 16));
}

// Copies with a field written and the CRC recomputed (test_dancepack.py's patch()).
inline Bytes patch8(Bytes b, size_t off, uint32_t v) {
  put8(b, off, v);
  fixCrc(b);
  return b;
}
inline Bytes patch16(Bytes b, size_t off, uint32_t v) {
  put16(b, off, v);
  fixCrc(b);
  return b;
}
inline Bytes patch32(Bytes b, size_t off, uint32_t v) {
  put32(b, off, v);
  fixCrc(b);
  return b;
}
inline Bytes patchRaw(Bytes b, size_t off, const void* src, size_t n) {
  std::memcpy(&b.at(off), src, n);
  if (n) (void)b.at(off + n - 1);
  fixCrc(b);
  return b;
}
inline Bytes patchRaw(Bytes b, size_t off, const char* s) { return patchRaw(std::move(b), off, s, std::strlen(s)); }

inline Bytes hex(const char* s) {
  Bytes out;
  int hi = -1;
  for (; *s; ++s) {
    int v;
    if (*s >= '0' && *s <= '9') v = *s - '0';
    else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
    else continue;
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<uint8_t>(hi << 4 | v));
      hi = -1;
    }
  }
  return out;
}
inline Bytes cat(Bytes a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
inline Bytes head(const Bytes& b, size_t n) { return Bytes(b.begin(), b.begin() + std::min(n, b.size())); }

// --- packs ------------------------------------------------------------------

struct Chunk {
  std::string tag;  // 4 characters
  Bytes data;
};

// A.4's order of the nine required chunks.
static const char* const kTags[9] = {"META", "GEOM", "PALS", "THMB", "FTAB", "STRS", "MOVS", "STEP", "FDAT"};

// The header and the directory (A.2-A.4) around `chunks`, in that order.
inline Bytes assemble(const std::vector<Chunk>& chunks) {
  const size_t n = chunks.size();
  size_t pos = 32 + 12 * n;
  Bytes out(pos, 0);
  std::memcpy(out.data(), "MPDN", 4);
  put16(out, 4, 1);
  put16(out, 6, 1);
  put16(out, 16, static_cast<uint32_t>(n));
  for (size_t i = 0; i < n; ++i) {
    const size_t off = (pos + 3) & ~static_cast<size_t>(3);
    out.resize(off, 0);
    out.insert(out.end(), chunks[i].data.begin(), chunks[i].data.end());
    const size_t e = 32 + 12 * i;
    std::memcpy(&out[e], chunks[i].tag.data(), 4);
    put32(out, e + 4, static_cast<uint32_t>(off));
    put32(out, e + 8, static_cast<uint32_t>(chunks[i].data.size()));
    pos = off + chunks[i].data.size();
  }
  out.resize((pos + 3) & ~static_cast<size_t>(3), 0);
  fixSize(out);
  fixCrc(out);
  return out;
}

// A.17's chunks, from the doc's table.
inline std::vector<Chunk> dotChunks() {
  Bytes pals(96, 0);
  pals[3] = 0xFF;    // dance entry 1: ff 00 00
  pals[51] = 0x74;   // idle entry 1: 74 1b 1b
  pals[52] = 0x1B;
  pals[53] = 0x1B;
  return {
      {"META", hex("03 64 6f 74 00 03 44 6f 74 00 03 41 6e 61 00 07 43 43 30 2d 31 2e 30 00 00 00 00 00 01")},
      {"GEOM", hex("01 00 04 04 02 04 00 00 01 00 00 00")},
      {"PALS", pals},
      {"THMB", Bytes(800, 0x11)},
      {"FTAB", hex("01 02 02 02 00 00 00 00 06 00 00 00")},
      {"STRS", hex("00 00 00 00 03 68 6f 70 00")},
      {"MOVS", hex("01 20 00 00  04 00 00 01 00 00 0a 00 00 00 00 00 00 00 02 00"
                   "00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00")},
      {"STEP", hex("00 00 0c 00 00 00 00 00")},
      {"FDAT", hex("00 01 11 00 01 11")},
  };
}
inline Bytes dotPack() { return assemble(dotChunks()); }

// A.17's offsets (its table): the first MOVS record, STEP, FTAB, FDAT, STRS.
constexpr size_t kDotMov = 1108, kDotStep = 1140, kDotFtab = 1080, kDotFdat = 1148, kDotStrs = 1092;

// test_dancepack.py's a17_with: A.4's nine chunks, those named in `changes`
// replaced, then the other tags of `changes` after them, sorted by tag.
inline Bytes dotWith(std::vector<Chunk> changes) {
  std::vector<Chunk> out = dotChunks();
  for (auto& c : out) {
    for (auto it = changes.begin(); it != changes.end(); ++it) {
      if (it->tag == c.tag) {
        c.data = it->data;
        changes.erase(it);
        break;
      }
    }
  }
  std::stable_sort(changes.begin(), changes.end(), [](const Chunk& a, const Chunk& b) { return a.tag < b.tag; });
  out.insert(out.end(), changes.begin(), changes.end());
  return assemble(out);
}

struct Entry {
  std::string tag;
  uint32_t off, size;
  size_t at;  // the entry's own position in the directory
};
// The directory as stored (no checks: the caller's pack is well formed).
inline std::vector<Entry> directory(const Bytes& b) {
  std::vector<Entry> d;
  const uint32_t n = get16(b, 16);
  for (uint32_t i = 0; i < n; ++i) {
    const size_t e = 32 + 12 * i;
    d.push_back({std::string(reinterpret_cast<const char*>(&b.at(e)), 4), get32(b, e + 4), get32(b, e + 8), e});
  }
  return d;
}
inline Entry entry(const Bytes& b, const char* tag) {
  for (const Entry& e : directory(b))
    if (e.tag == tag) return e;
  return {std::string(), 0, 0, 0};
}
inline Bytes chunkOf(const Bytes& b, const char* tag) {
  const Entry e = entry(b, tag);
  return Bytes(b.begin() + e.off, b.begin() + e.off + e.size);
}
// The largest off + size in the directory: the last chunk's end.
inline uint32_t lastChunkEnd(const Bytes& b) {
  uint32_t end = 0;
  for (const Entry& e : directory(b)) end = std::max(end, e.off + e.size);
  return end;
}

inline Bytes testPack() { return Bytes(dance::kTestPack, dance::kTestPack + dance::kTestPackSize); }

// --- randomness ---------------------------------------------------------------

struct Xorshift32 {
  uint32_t x;
  explicit Xorshift32(uint32_t seed) : x(seed) {}
  uint32_t next() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

// --- Report::onDrop -------------------------------------------------------------

struct Drops {
  static constexpr int kMax = 64;
  int calls = 0;
  int lastMove = -1;
  bool outOfOrder = false;  // a move not after the previous one
  int move[kMax];
  char text[kMax][dance::kReasonChars];

  void reset() {
    calls = 0;
    lastMove = -1;
    outOfOrder = false;
  }
  static void on(void* ctx, int move, const dance::Reason& why) {
    Drops* d = static_cast<Drops*>(ctx);
    if (move <= d->lastMove) d->outOfOrder = true;
    d->lastMove = move;
    if (d->calls < kMax) {
      d->move[d->calls] = move;
      dance::formatReason(why, d->text[d->calls], dance::kReasonChars);
    }
    ++d->calls;
  }
  // "move:text" pairs joined with "|", for one-line comparisons.
  std::string str() const {
    std::string s;
    for (int i = 0; i < calls && i < kMax; ++i) {
      if (i) s += "|";
      s += std::to_string(move[i]) + ":" + text[i];
    }
    return s;
  }
  void attach(dance::Report& r) {
    reset();
    r = dance::Report();
    r.onDrop = &Drops::on;
    r.ctx = this;
  }
};

// --- a canvas with a canary border ------------------------------------------------

struct CanaryCanvas {
  int w, h, border, stride;
  uint16_t canary;
  size_t n;
  uint16_t* buf;

  CanaryCanvas(int w_, int h_, int border_, uint16_t canary_ = 0xA5A5, uint16_t fill = 0x1234)
      : w(w_), h(h_), border(border_), stride(w_ + 2 * border_), canary(canary_),
        n(static_cast<size_t>(w_ + 2 * border_) * (h_ + 2 * border_)), buf(new uint16_t[n]) {
    reset(fill);
  }
  ~CanaryCanvas() { delete[] buf; }
  CanaryCanvas(const CanaryCanvas&) = delete;
  CanaryCanvas& operator=(const CanaryCanvas&) = delete;

  void reset(uint16_t fill) {
    for (size_t i = 0; i < n; ++i) buf[i] = canary;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) at(x, y) = fill;
  }
  uint16_t& at(int x, int y) { return buf[static_cast<size_t>(y + border) * stride + (x + border)]; }
  uint16_t at(int x, int y) const { return buf[static_cast<size_t>(y + border) * stride + (x + border)]; }
  dance::BlitCanvas canvas() {
    dance::BlitCanvas c;
    c.px = &at(0, 0);
    c.w = static_cast<int16_t>(w);
    c.h = static_cast<int16_t>(h);
    c.stride = stride;
    return c;
  }
  bool borderIntact() const {
    for (int y = -border; y < h + border; ++y)
      for (int x = -border; x < w + border; ++x)
        if ((x < 0 || x >= w || y < 0 || y >= h) && at(x, y) != canary) return false;
    return true;
  }
};

// --- ReaderVectors ----------------------------------------------------------------

// D.3's steps 1-3: the base cut or zero-extended to the case's length, its
// patches in order, then the size and the CRC when the case fixes them.
inline Bytes readerCase(int i) {
  const dancevec::ReaderCase& c = dancevec::kReaderCases[i];
  const uint8_t* base = c.base == dancevec::kBaseTest ? dance::kTestPack : dancevec::kDotPack;
  const size_t baseLen = c.base == dancevec::kBaseTest ? dance::kTestPackSize : dancevec::kDotPackSize;
  Bytes b(c.length, 0);
  if (c.length) std::memcpy(b.data(), base, std::min<size_t>(baseLen, c.length));
  for (uint32_t k = 0; k < c.patchCount; ++k) {
    const dancevec::ReaderPatch& p = dancevec::kReaderPatches[c.firstPatch + k];
    for (uint32_t j = 0; j < p.len; ++j) b.at(p.off + j) = dancevec::kReaderBytes[p.at + j];
  }
  if ((c.fix & dancevec::kFixSize) && c.length >= 12) put32(b, 8, c.length);
  if ((c.fix & dancevec::kFixCrc) && c.length >= 16) put32(b, 12, refCrc32(b.data() + 16, c.length - 16));
  return b;
}

}  // namespace packedit
