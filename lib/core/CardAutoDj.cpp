// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardAutoDj.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace cardcontract {
namespace mpdj {

namespace {

const SectionSpec kSections[kSecCount] = {
    {kDjsl, kSmallSlot, true},  // slotBytes: checked against the header
    {kDjrf, kFilterStride, true},
    {kDjph, kPathStride, true},
    {kStrs, 0, true},
};

const char kHex[] = "0123456789abcdef";

}  // namespace

const FormatSpec kSpec = {kMagicMpdj, kMinHeaderBytes, kSections, kSecCount, kMajor2};

uint32_t slotBytesFor(uint32_t k) {
  if (k >= kMinK && k <= kMaxSmallK) return kSmallSlot;
  if (k > kMaxSmallK && k <= kMaxK) return kLargeSlot;
  return 0;
}

uint32_t headerBytesFor(uint32_t sectionCount, uint32_t slotBytes, uint32_t minimum) {
  if (slotBytes == 0 || slotBytes % 8) return 0;
  uint32_t hb = (minimum + 7) & ~7u;
  while ((hb + kDirEntryBytes * sectionCount) % slotBytes) hb += 8;
  return hb;
}

uint32_t strideRank(uint32_t j, uint32_t k, uint32_t head, uint32_t m) {
  // j itself in the head, and wherever the list isn't strided (m <= k: the
  // top k; a head the header can't hold). (j - H)(m - 1 - H) is at most
  // 166 x 65,533: 32 bits hold it.
  if (j < head || m <= k || head + 2 > k) return j;
  return head + (j - head) * (m - 1 - head) / (k - 1 - head);
}

uint8_t level(double cos) {
  const double s = std::round(cos * 255.0);  // half away from zero
  if (!(s > 0)) return 0;
  if (s > 255) return 255;
  return static_cast<uint8_t>(s);
}

uint8_t level(const float* a, const float* b, uint32_t dim) {
  // Each product of two f32 is exact in f64, so a fused multiply-add can't
  // change the sum: only its order matters, and it is the index order.
  double dot = 0;
  for (uint32_t i = 0; i < dim; ++i) dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  return level(dot);
}

uint32_t slotCrc(const uint8_t* slot, uint32_t slotBytes, uint32_t row) {
  uint8_t r[4];
  put32(r, row);
  return crc32(slot, slotBytes - 4, crc32(r, sizeof(r)));
}

SlotRead checkSlot(const uint8_t* slot, uint32_t slotBytes, uint32_t k, uint32_t row, uint32_t rowCount, Entry* out,
                   uint32_t* count, Why* why) {
  if (count) *count = 0;
  auto fail = [&](SlotRead r, Why w) {
    if (why) *why = w;
    return r;
  };
  if (slotBytes < 4 || k == 0 || 3 * k > slotBytes - 4) return fail(SlotRead::Broken, Why::Shape);
  // S1: the CRC, seeded with the row (a slot read for another row fails).
  if (slotCrc(slot, slotBytes, row) != get32(slot + slotBytes - 4)) return fail(SlotRead::Damaged, Why::DjSlotCrc);
  const uint8_t* levels = slot + 2 * k;
  uint32_t c = k;
  for (uint32_t j = 0; j < k; ++j)
    if (get16(slot + 2 * j) == kUnused) {
      c = j;
      break;
    }
  // S2: unused entries only after every used one.
  for (uint32_t j = c; j < k; ++j)
    if (get16(slot + 2 * j) != kUnused) return fail(SlotRead::Broken, Why::DjSlotUnused);
  // S3: every used index a row, and not this one.
  for (uint32_t j = 0; j < c; ++j) {
    const uint32_t idx = get16(slot + 2 * j);
    if (idx >= rowCount || idx == row) return fail(SlotRead::Broken, Why::DjSlotIndex);
  }
  // S4: (level desc, row asc), as the candidates are ranked.
  for (uint32_t j = 1; j < c; ++j) {
    if (levels[j] > levels[j - 1] || (levels[j] == levels[j - 1] && get16(slot + 2 * j) <= get16(slot + 2 * j - 2)))
      return fail(SlotRead::Broken, Why::DjSlotOrder);
  }
  if (out)
    for (uint32_t j = 0; j < c; ++j) {
      out[j].row = get16(slot + 2 * j);
      out[j].level = levels[j];
    }
  if (count) *count = c;
  return fail(SlotRead::Ok, Why::Ok);
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------
Why Table::open(Source& src) {
  uint8_t th[kMinHeaderBytes - kCommonHeaderBytes];
  const Why w = c_.open(src, kSpec, th);
  if (w != Why::Ok) return w;
  // The type header (4.1), file offsets 40-103.
  rowCount_ = get32(th);                    // 40
  pathCount_ = get32(th + 4);               // 44
  k_ = get16(th + 8);                       // 48
  slotBytes_ = get16(th + 10);              // 50
  const uint8_t indexBytes = th[12];        // 52
  const uint8_t scoreKind = th[13];         // 53
  selKind_ = th[14];                        // 54
  selHead_ = th[15];                        // 55
  builtTime_ = get32(th + 16);              // 56
  tagsGeneration_ = get32(th + 20);         // 60
  std::memcpy(sig_, th + 24, 16);           // 64
  for (uint32_t i = 0; i < 5; ++i) strings_[i] = get32(th + 40 + 4 * i);  // 80-99
  selFloor_ = th[60];                       // 100
  // 8.2, in its order.
  if (rowCount_ < 1 || rowCount_ > kMaxRows) return Why::Enum;
  if (k_ < kMinK || k_ > kMaxK) return Why::Enum;
  if (slotBytes_ != slotBytesFor(k_)) return Why::Enum;
  if (indexBytes != kIndexBytes || scoreKind != kScoreCosine) return Why::Enum;
  if (selKind_ == kSelTop) {
    if (selHead_ != k_ || selFloor_ != 0) return Why::DjSelection;
  } else if (selKind_ == kSelStrided) {
    if (selHead_ < 1 || selHead_ + 2 > k_ || selFloor_ < 1) return Why::DjSelection;
  } else if (selHead_ > k_) {  // a later kind: readers use only selHead and selFloor
    return Why::DjSelection;
  }
  const Section& slots = c_.section(kSecSlots);
  if (slots.count != rowCount_ || c_.section(kSecFilters).count != rowCount_ ||
      c_.section(kSecPaths).count != pathCount_)
    return Why::Counts;
  if (slots.stride != slotBytes_) return Why::Shape;
  // DJSL first and on a slot boundary, the four in order (unknown sections
  // may stand between them): their places in the directory, read again.
  uint32_t at[kSecCount];
  for (uint32_t& a : at) a = kMaxSections;
  const Header& h = c_.header();
  for (uint32_t i = 0; i < h.sectionCount; ++i) {
    uint8_t t[4];
    if (!src.read(h.headerBytes + kDirEntryBytes * i, t, sizeof(t))) return Why::Io;
    for (uint32_t s = 0; s < kSecCount; ++s)
      if (get32(t) == kSections[s].type) at[s] = i;
  }
  if (at[kSecSlots] != 0 || slots.offset % slotBytes_) return Why::DjLayout;
  for (uint32_t s = 1; s < kSecCount; ++s)
    if (at[s] <= at[s - 1]) return Why::DjLayout;
  return Why::Ok;
}

Why Table::check(uint32_t uses, uint8_t* scratch, uint32_t scratchBytes) const {
  if (!scratch || scratchBytes < 64 || ((uses & kUseSlots) && scratchBytes < 1024)) return Why::Io;
  Source* src = c_.source();
  // STRS (8.3): the header's strings, front to back, then its CRC.
  {
    const Section& strs = c_.section(kSecStrs);
    Stream s;
    s.begin(src, strs.offset, strs.offset + strs.bytes, scratch, scratchBytes);
    uint8_t b;
    if (strs.bytes == 0 || !s.readByte(&b) || b != 0) return Why::String;
    for (uint32_t off : strings_) {
      if (off == 0) continue;
      if (off >= strs.bytes) return Why::String;
      if (strs.offset + off < s.position()) return Why::StringOrder;
      if (!s.skipTo(strs.offset + off) || !s.readString(nullptr, 0, nullptr)) return Why::String;
    }
    if (!s.drain()) return Why::Io;
    if (s.crc() != strs.crc) return Why::SectionCrc;
  }
  // DJRF: its CRC (its values are any; a camelot above 24 reads as 0).
  if (uses & kUseFilters) {
    const Section& sec = c_.section(kSecFilters);
    Stream s;
    s.begin(src, sec.offset, sec.offset + sec.bytes, scratch, scratchBytes);
    if (!s.drain()) return Why::Io;
    if (s.crc() != sec.crc) return Why::SectionCrc;
  }
  // DJPH: rows in range, hashes strictly increasing (a hash two card files
  // share was left out by the writer), then its CRC.
  if (uses & kUsePaths) {
    const Section& sec = c_.section(kSecPaths);
    Stream s;
    s.begin(src, sec.offset, sec.offset + sec.bytes, scratch, scratchBytes);
    uint64_t prev = 0;
    for (uint32_t i = 0; i < sec.count; ++i) {
      uint8_t e[kPathStride];
      if (!s.read(e, kPathStride) || !s.skipTo(s.position() + sec.stride - kPathStride)) return Why::Io;
      const uint64_t h = get64(e);
      if (get16(e + 8) >= rowCount_ || (i > 0 && h <= prev)) return Why::DjPaths;
      prev = h;
    }
    if (!s.drain()) return Why::Io;
    if (s.crc() != sec.crc) return Why::SectionCrc;
  }
  // DJSL: every slot in row order (a whole-file check makes any bad slot
  // the file's absence), then the section's CRC. The slot goes into the
  // scratch's front, the stream reads through the rest.
  if (uses & kUseSlots) {
    const Section& sec = c_.section(kSecSlots);
    uint8_t* slot = scratch;
    Stream s;
    s.begin(src, sec.offset, sec.offset + sec.bytes, scratch + slotBytes_, scratchBytes - slotBytes_);
    for (uint32_t r = 0; r < rowCount_; ++r) {
      if (!s.read(slot, slotBytes_)) return Why::Io;
      Why why = Why::Ok;
      if (checkSlot(slot, slotBytes_, k_, r, rowCount_, nullptr, nullptr, &why) != SlotRead::Ok) return why;
    }
    if (!s.drain()) return Why::Io;
    if (s.crc() != sec.crc) return Why::SectionCrc;
  }
  return Why::Ok;
}

Why Table::string(uint32_t which, char* out, size_t cap) const {
  if (which >= 5) return Why::String;
  if (strings_[which] == 0) {
    if (cap) out[0] = 0;
    return Why::Ok;
  }
  return readStringAt(*c_.source(), c_.section(kSecStrs), strings_[which], out, cap);
}

bool Table::filter(uint32_t r, Filter* out) const {
  const Section& sec = c_.section(kSecFilters);
  if (r >= sec.count) return false;
  uint8_t e[kFilterStride];
  if (!c_.source()->read(sec.offset + r * sec.stride, e, kFilterStride)) return false;
  out->bpm10 = get16(e);
  out->camelot = e[2] <= 24 ? e[2] : 0;
  out->flags = e[3] & kFilterBpmAnalysed;
  out->artistKey = get32(e + 4);
  return true;
}

uint32_t Table::rowOf(uint64_t pathHash) const {
  const Section& sec = c_.section(kSecPaths);
  uint32_t lo = 0, hi = sec.count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    uint8_t e[kPathStride];
    if (!c_.source()->read(sec.offset + mid * sec.stride, e, kPathStride)) return kNoRow;
    if (get64(e) < pathHash)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo >= sec.count) return kNoRow;
  uint8_t e[kPathStride];
  if (!c_.source()->read(sec.offset + lo * sec.stride, e, kPathStride) || get64(e) != pathHash) return kNoRow;
  const uint32_t r = get16(e + 8);
  return r < rowCount_ ? r : kNoRow;
}

SlotRead Table::slot(uint32_t r, uint8_t* buf, Entry* out, uint32_t* count, Why* why) const {
  if (count) *count = 0;
  // Every row a reader holds came from DJPH or a list, both checked below
  // rowCount: one past it is the reader's own state gone wrong, which no
  // re-read mends.
  if (r >= rowCount_) {
    if (why) *why = Why::DjSlotIndex;
    return SlotRead::Broken;
  }
  if (!c_.source()->read(slotsOffset() + r * slotBytes_, buf, slotBytes_)) {
    if (why) *why = Why::Io;
    return SlotRead::Damaged;
  }
  return checkSlot(buf, slotBytes_, k_, r, rowCount_, out, count, why);
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------
namespace {
const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() {
  static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, kInit, sizeof(h_));
}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
           static_cast<uint32_t>(p[4 * i + 2]) << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  bytes_ += n;
  while (n) {
    const uint32_t k = static_cast<uint32_t>(n < 64 - fill_ ? n : 64 - fill_);
    std::memcpy(buf_ + fill_, p, k);
    fill_ += k;
    p += k;
    n -= k;
    if (fill_ == 64) {
      block(buf_);
      fill_ = 0;
    }
  }
}

void Sha256::final(uint8_t out[32]) {
  const uint64_t bits = bytes_ * 8;
  const uint8_t one = 0x80, zero = 0;
  update(&one, 1);
  while (fill_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<uint8_t>(h_[i] >> 24);
    out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
    out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
    out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
  }
}

// ---------------------------------------------------------------------------
// The selection signature (6)
// ---------------------------------------------------------------------------
void vecDigest(const float* const* rows, uint32_t rowCount, uint32_t dim, uint8_t out[32]) {
  Sha256 sha;
  uint8_t buf[64];
  for (uint32_t r = 0; r < rowCount; ++r)
    for (uint32_t i = 0; i < dim;) {
      uint32_t n = 0;
      for (; n < sizeof(buf) / 4 && i < dim; ++n, ++i) {
        uint32_t u;
        std::memcpy(&u, &rows[r][i], 4);
        put32(buf + 4 * n, u);
      }
      sha.update(buf, 4 * n);
    }
  sha.final(out);
}

void selectionSignature(const SigIn& in, uint8_t out[16]) {
  Sha256 sha;
  auto text = [&](const char* s) {
    if (s) sha.update(s, std::strlen(s));
  };
  auto hex = [&](const uint8_t* b, size_t n) {
    char h[2];
    for (size_t i = 0; i < n; ++i) {
      h[0] = kHex[b[i] >> 4];
      h[1] = kHex[b[i] & 15];
      sha.update(h, 2);
    }
  };
  char buf[48];
  text("MPDJ-SEL 2\nmodel ");
  text(in.modelId);
  text(" ");
  text(in.modelVersion);
  text("\nmetric ");
  text(in.metric);
  std::snprintf(buf, sizeof(buf), "\nk %u\n", static_cast<unsigned>(in.k));
  text(buf);
  if (in.selKind == kSelTop) {
    text("sel top\n");
  } else {
    std::snprintf(buf, sizeof(buf), "sel strided %u %u\n", static_cast<unsigned>(in.selHead),
                  static_cast<unsigned>(in.selFloor));
    text(buf);
  }
  std::snprintf(buf, sizeof(buf), "vec %u ", static_cast<unsigned>(in.dim));
  text(buf);
  hex(in.vecSha256, 32);
  text("\n");
  for (uint32_t r = 0; r < in.rowCount; ++r) {
    hex(in.hashes[r], 16);
    text("\n");
  }
  uint32_t at = 0;
  for (uint32_t g = 0; g < in.groupCount; ++g) {
    text("same");
    for (; at < in.groupEnds[g]; ++at) {
      std::snprintf(buf, sizeof(buf), " %u", static_cast<unsigned>(in.groupRows[at]));
      text(buf);
    }
    text("\n");
  }
  uint8_t digest[32];
  sha.final(digest);
  std::memcpy(out, digest, 16);
}

// ---------------------------------------------------------------------------
// The selection (5.3, 5.4) and the writer
// ---------------------------------------------------------------------------
uint32_t selectEntries(const uint16_t* levels, uint32_t n, uint8_t kind, uint32_t k, uint32_t head, uint32_t floor,
                       Entry* out, uint16_t* order) {
  // One counting sort (12.2): each level's candidates in a run, the best
  // level's first, rows ascending within a run.
  uint32_t start[256] = {};
  for (uint32_t b = 0; b < n; ++b)
    if (levels[b] < kNotCandidate) ++start[levels[b]];
  uint32_t candidates = 0, m = 0;
  for (int lv = 255; lv >= 0; --lv) {
    const uint32_t c = start[lv];
    start[lv] = candidates;
    candidates += c;
    if (static_cast<uint32_t>(lv) >= floor) m = candidates;
  }
  for (uint32_t b = 0; b < n; ++b)
    if (levels[b] < kNotCandidate) order[start[levels[b]]++] = static_cast<uint16_t>(b);
  // The top k; strided by rank when more than k candidates reach the floor.
  const bool strided = kind == kSelStrided && m > k;
  const uint32_t c = candidates < k ? candidates : k;
  for (uint32_t j = 0; j < c; ++j) {
    const uint16_t b = order[strided ? strideRank(j, k, head, m) : j];
    out[j].row = b;
    out[j].level = static_cast<uint8_t>(levels[b]);
  }
  return c;
}

bool write(Sink& out, const TableIn& in, uint8_t sig[16], uint32_t* fileBytes, uint32_t* headerCrc, const char** error) {
  auto bad = [&](const char* e) {
    if (error) *error = e;
    return false;
  };
  const uint32_t n = in.rowCount;
  if (!in.rows || n < 1 || n > kMaxRows) return bad("rowCount must be 1-65,535");
  const uint32_t k = in.k;
  const uint32_t slotBytes = slotBytesFor(k);
  if (!slotBytes) return bad("k must be 2-169");
  uint32_t head = k, floor = 0;  // selKind 0
  if (in.selKind == kSelStrided) {
    head = in.selHead;
    floor = in.selFloor;
    if (head < 1 || head + 2 > k) return bad("selHead must be 1 to k - 2");
    if (floor < 1 || floor > 255) return bad("selFloor must be 1-255");
  } else if (in.selKind != kSelTop) {
    return bad("selKind: this writer knows 0 and 1");
  }
  const char* strings[5] = {in.modelId, in.modelVersion, in.metric, in.license, in.attribution};
  for (const char*& s : strings) {
    if (!s) s = "";
    if (std::strlen(s) > kMaxString) return bad("a string over 255 bytes");
  }
  if (in.dim == 0) return bad("dim must be at least 1");
  std::vector<uint32_t> order(n);  // row r is in.rows[order[r]]
  for (uint32_t i = 0; i < n; ++i) {
    const RowIn& r = in.rows[i];
    order[i] = i;
    if (!r.paths || r.pathCount == 0) return bad("a row with no path");
    if (!r.embedding) return bad("a row with no embedding");
    for (uint32_t d = 0; d < in.dim; ++d)
      if (!std::isfinite(r.embedding[d])) return bad("an embedding value isn't finite");
  }
  // Rows in canonical-hash order (5.1).
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return std::memcmp(in.rows[a].hash, in.rows[b].hash, 16) < 0; });
  for (uint32_t i = 1; i < n; ++i)
    if (std::memcmp(in.rows[order[i]].hash, in.rows[order[i - 1]].hash, 16) == 0) return bad("a hash twice");
  auto row = [&](uint32_t r) -> const RowIn& { return in.rows[order[r]]; };

  // The same-song groups: rows sharing a song key (rows without one never
  // group). song[r] is the group's first row, or r itself.
  std::vector<uint32_t> song(n), keyed;
  for (uint32_t r = 0; r < n; ++r) {
    song[r] = r;
    if (row(r).songKey) keyed.push_back(r);
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [&](uint32_t a, uint32_t b) { return std::strcmp(row(a).songKey, row(b).songKey) < 0; });
  std::vector<std::pair<uint32_t, uint32_t>> runs;  // [begin, end) of keyed, two or more rows
  for (size_t i = 0; i < keyed.size();) {
    size_t e = i + 1;
    while (e < keyed.size() && std::strcmp(row(keyed[i]).songKey, row(keyed[e]).songKey) == 0) ++e;
    for (size_t j = i; j < e; ++j) song[keyed[j]] = keyed[i];  // stable: keyed[i] is the group's lowest row
    if (e - i >= 2) runs.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(e));
    i = e;
  }
  std::sort(runs.begin(), runs.end(),
            [&](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return keyed[a.first] < keyed[b.first];
            });
  std::vector<uint32_t> groupRows, groupEnds;
  for (const auto& g : runs) {
    for (uint32_t j = g.first; j < g.second; ++j) groupRows.push_back(keyed[j]);
    groupEnds.push_back(static_cast<uint32_t>(groupRows.size()));
  }

  // The signature (6).
  std::vector<const float*> emb(n);
  std::vector<uint8_t> hashes(static_cast<size_t>(n) * 16);
  for (uint32_t r = 0; r < n; ++r) {
    emb[r] = row(r).embedding;
    std::memcpy(hashes.data() + static_cast<size_t>(r) * 16, row(r).hash, 16);
  }
  SigIn si;
  si.modelId = strings[0];
  si.modelVersion = strings[1];
  si.metric = strings[2];
  si.k = k;
  si.selKind = in.selKind;
  si.selHead = head;
  si.selFloor = floor;
  si.dim = in.dim;
  vecDigest(emb.data(), n, in.dim, si.vecSha256);
  si.hashes = reinterpret_cast<const uint8_t(*)[16]>(hashes.data());
  si.rowCount = n;
  si.groupRows = groupRows.data();
  si.groupEnds = groupEnds.data();
  si.groupCount = static_cast<uint32_t>(groupEnds.size());
  selectionSignature(si, sig);

  // DJRF (4.4) and DJPH (4.5): every card file's path hash once, sorted; a
  // hash two or more card files hold (a collision) left out altogether.
  std::vector<uint8_t> djrf(static_cast<size_t>(n) * kFilterStride, 0);
  std::vector<std::pair<uint64_t, uint32_t>> all;
  for (uint32_t r = 0; r < n; ++r) {
    const RowIn& x = row(r);
    uint8_t* e = djrf.data() + static_cast<size_t>(r) * kFilterStride;
    put16(e, x.bpm10);
    e[2] = x.camelot <= 24 ? x.camelot : 0;
    e[3] = x.flags & kFilterBpmAnalysed;
    put32(e + 4, x.artistKey);
    for (uint32_t p = 0; p < x.pathCount; ++p) all.emplace_back(x.paths[p], r);
  }
  std::sort(all.begin(), all.end());
  std::vector<uint8_t> djph;
  for (size_t i = 0; i < all.size();) {
    size_t e = i + 1;
    while (e < all.size() && all[e].first == all[i].first) ++e;
    if (e - i == 1) {
      uint8_t ent[kPathStride];
      put64(ent, all[i].first);
      put16(ent + 8, static_cast<uint16_t>(all[i].second));
      djph.insert(djph.end(), ent, ent + kPathStride);
    }
    i = e;
  }
  const uint32_t pathEntries = static_cast<uint32_t>(djph.size() / kPathStride);

  // STRS (4.6) and the header (4.1).
  std::vector<uint8_t> strs{0};
  uint32_t offsets[5];
  for (int i = 0; i < 5; ++i) {
    offsets[i] = 0;
    if (!*strings[i]) continue;
    offsets[i] = static_cast<uint32_t>(strs.size());
    strs.insert(strs.end(), strings[i], strings[i] + std::strlen(strings[i]));
    strs.push_back(0);
  }
  uint8_t th[kMinHeaderBytes - kCommonHeaderBytes] = {};
  put32(th, n);
  put32(th + 4, pathEntries);
  put16(th + 8, static_cast<uint16_t>(k));
  put16(th + 10, static_cast<uint16_t>(slotBytes));
  th[12] = kIndexBytes;
  th[13] = kScoreCosine;
  th[14] = in.selKind;
  th[15] = static_cast<uint8_t>(head);
  put32(th + 16, in.builtTime);
  put32(th + 20, in.tagsGeneration);
  std::memcpy(th + 24, sig, 16);
  for (int i = 0; i < 5; ++i) put32(th + 40 + 4 * i, offsets[i]);
  th[60] = static_cast<uint8_t>(floor);

  SectionOut s[4] = {
      SectionOut{kDjsl, kSectionRequired, n, slotBytes, n * slotBytes},
      SectionOut{kDjrf, kSectionRequired, n, kFilterStride, static_cast<uint32_t>(djrf.size())},
      SectionOut{kDjph, kSectionRequired, pathEntries, kPathStride, static_cast<uint32_t>(djph.size())},
      SectionOut{kStrs, kSectionRequired, 0, 0, static_cast<uint32_t>(strs.size())},
  };
  FileMeta fm;
  fm.magic = kMagicMpdj;
  fm.major = kMajor2;
  fm.minor = in.minor;
  fm.generation = in.generation;
  fm.cardId = in.cardId;
  ContainerWriter cw;
  if (!cw.begin(out, fm, th, headerBytesFor(4, slotBytes), s, 4, sizeof(th))) return bad("layout");

  // DJSL (4.3): each row's levels to every other row by the rule (5.2), its
  // candidates ranked and selected, its slot written in row order.
  std::vector<uint16_t> levels(n), ranked(n);
  Entry entries[kMaxK];
  uint8_t slot[kLargeSlot];
  for (uint32_t a = 0; a < n; ++a) {
    for (uint32_t b = 0; b < n; ++b)
      levels[b] = b == a || song[b] == song[a] ? kNotCandidate : level(emb[a], emb[b], in.dim);
    const uint32_t c = selectEntries(levels.data(), n, in.selKind, k, head, floor, entries, ranked.data());
    std::memset(slot, 0, slotBytes);
    for (uint32_t j = 0; j < k; ++j) {
      put16(slot + 2 * j, j < c ? entries[j].row : kUnused);
      slot[2 * k + j] = j < c ? entries[j].level : 0;
    }
    put32(slot + slotBytes - 4, slotCrc(slot, slotBytes, a));
    if (!cw.write(slot, slotBytes)) return bad("sink");
  }
  if (!cw.write(djrf.data(), static_cast<uint32_t>(djrf.size())) ||
      (!djph.empty() && !cw.write(djph.data(), static_cast<uint32_t>(djph.size()))) ||
      !cw.write(strs.data(), static_cast<uint32_t>(strs.size())))
    return bad("sink");
  if (!cw.finish(fileBytes, headerCrc)) return bad("sink");
  return true;
}

}  // namespace mpdj
}  // namespace cardcontract
