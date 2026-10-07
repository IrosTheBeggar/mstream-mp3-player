// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardAutoDj.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cardcontract {
namespace mpdj {

namespace {

const SectionSpec kSections[kSecCount] = {
    {kDjrw, kRowStride, true},
    {kDjnb, 1, true},  // k x (indexBytes + 1): checked against the header
    {kDjph, kPathStride, true},
    {kStrs, 0, true},
};

void decodeRow(const uint8_t* e, Row* r) {
  r->pathHash = get64(e);
  std::memcpy(r->hashPrefix, e + 8, 8);
  r->bpm10 = get16(e + 16);
  r->camelot = e[18];
  r->flags = e[19];
  r->artistKey = get32(e + 20);
}

}  // namespace

const FormatSpec kSpec = {kMagicMpdj, kHeaderBytes, kSections, kSecCount};

Why Table::open(Source& src) {
  uint8_t th[kHeaderBytes - kCommonHeaderBytes];
  const Why w = c_.open(src, kSpec, th);
  if (w != Why::Ok) return w;
  rowCount_ = get32(th);
  pathCount_ = get32(th + 4);
  k_ = get16(th + 8);
  indexBytes_ = th[10];
  const uint8_t scoreKind = th[11];
  builtTime_ = get32(th + 12);
  std::memcpy(sig_, th + 16, 16);
  strings_[0] = get32(th + 32);  // modelId
  strings_[1] = get32(th + 36);  // modelVersion
  strings_[2] = get32(th + 40);  // metric
  tagsGeneration_ = get32(th + 44);
  strings_[3] = get32(th + 48);  // license
  strings_[4] = get32(th + 52);  // attribution
  if (scoreKind != kScoreCosine) return Why::Enum;
  if (indexBytes_ != (rowCount_ <= 65535 ? 2u : 4u)) return Why::Enum;
  if (k_ < 1) return Why::Enum;
  const Section& rows = c_.section(kSecRows);
  const Section& nb = c_.section(kSecNeighbours);
  const Section& paths = c_.section(kSecPaths);
  if (rows.count != rowCount_ || nb.count != rowCount_ || paths.count != pathCount_) return Why::Counts;
  if (nb.stride != k_ * (indexBytes_ + 1)) return Why::Shape;
  return Why::Ok;
}

Why Table::check(uint32_t uses, uint8_t* scratch, uint32_t scratchBytes) const {
  if (!scratch || scratchBytes < 64) return Why::Io;
  Source* src = c_.source();
  const Section& strs = c_.section(kSecStrs);
  // The header's strings, front to back.
  {
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
  if (uses & kUseRows) {
    const Section& sec = c_.section(kSecRows);
    Stream s;
    s.begin(src, sec.offset, sec.offset + sec.bytes, scratch, scratchBytes);
    uint8_t prev[8] = {};
    for (uint32_t i = 0; i < sec.count; ++i) {
      uint8_t e[kRowStride];
      if (!s.read(e, kRowStride) || !s.skipTo(s.position() + sec.stride - kRowStride)) return Why::Io;
      if (i > 0 && std::memcmp(e + 8, prev, 8) < 0) return Why::DjRows;
      std::memcpy(prev, e + 8, 8);
    }
    if (!s.drain()) return Why::Io;
    if (s.crc() != sec.crc) return Why::SectionCrc;
  }
  if (uses & kUsePaths) {
    const Section& sec = c_.section(kSecPaths);
    Stream s;
    s.begin(src, sec.offset, sec.offset + sec.bytes, scratch, scratchBytes);
    uint64_t prevHash = 0;
    uint32_t prevRow = 0;
    for (uint32_t i = 0; i < sec.count; ++i) {
      uint8_t e[kPathStride];
      if (!s.read(e, kPathStride) || !s.skipTo(s.position() + sec.stride - kPathStride)) return Why::Io;
      const uint64_t h = get64(e);
      const uint32_t r = get32(e + 8);
      if (r >= rowCount_) return Why::DjPaths;
      if (i > 0 && (h < prevHash || (h == prevHash && r <= prevRow))) return Why::DjPaths;
      prevHash = h;
      prevRow = r;
    }
    if (!s.drain()) return Why::Io;
    if (s.crc() != sec.crc) return Why::SectionCrc;
  }
  if (uses & kUseNeighbours) {
    const Section& sec = c_.section(kSecNeighbours);
    Stream s;
    s.begin(src, sec.offset, sec.offset + sec.bytes, scratch, scratchBytes);
    const uint32_t unused = indexBytes_ == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    for (uint32_t i = 0; i < sec.count; ++i) {
      for (uint32_t j = 0; j < k_; ++j) {
        uint8_t e[5];
        if (!s.read(e, indexBytes_ + 1)) return Why::Io;
        const uint32_t idx = indexBytes_ == 2 ? get16(e) : get32(e);
        if (idx >= rowCount_ && idx != unused) return Why::DjNeighbours;
      }
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

bool Table::row(uint32_t i, Row* out) const {
  const Section& sec = c_.section(kSecRows);
  if (i >= sec.count) return false;
  uint8_t e[kRowStride];
  if (!c_.source()->read(sec.offset + i * sec.stride, e, kRowStride)) return false;
  decodeRow(e, out);
  return true;
}

uint32_t Table::neighbours(uint32_t i, Neighbour* out, uint32_t cap) const {
  const Section& sec = c_.section(kSecNeighbours);
  if (i >= sec.count) return 0;
  const uint32_t unused = indexBytes_ == 2 ? 0xFFFFu : 0xFFFFFFFFu;
  const uint32_t entry = indexBytes_ + 1;
  uint32_t n = 0;
  uint8_t buf[256];
  for (uint32_t j = 0; j < k_ && n < cap;) {
    uint32_t batch = static_cast<uint32_t>(sizeof(buf)) / entry;
    if (batch > k_ - j) batch = k_ - j;
    if (!c_.source()->read(sec.offset + i * sec.stride + j * entry, buf, batch * entry)) return n;
    for (uint32_t b = 0; b < batch && n < cap; ++b) {
      const uint8_t* e = buf + b * entry;
      const uint32_t idx = indexBytes_ == 2 ? get16(e) : get32(e);
      if (idx == unused || idx >= rowCount_) continue;
      out[n].row = idx;
      out[n].score = e[indexBytes_];
      ++n;
    }
    j += batch;
  }
  return n;
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
  const uint32_t r = get32(e + 8);
  return r < rowCount_ ? r : kNoRow;
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

void selectionSignature(const char* modelId, const char* modelVersion, const char* metric, uint32_t k,
                        const uint8_t (*hashes)[16], size_t n, uint8_t out[16]) {
  Sha256 sha;
  auto line = [&](const char* s) { sha.update(s, std::strlen(s)); };
  char buf[32];
  line("MPDJ-SEL 1\nmodel ");
  line(modelId ? modelId : "");
  line(" ");
  line(modelVersion ? modelVersion : "");
  line("\nmetric ");
  line(metric ? metric : "");
  std::snprintf(buf, sizeof(buf), "\nk %u\n", static_cast<unsigned>(k));
  line(buf);
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    char hex[33];
    for (int b = 0; b < 16; ++b) {
      hex[2 * b] = kHex[hashes[i][b] >> 4];
      hex[2 * b + 1] = kHex[hashes[i][b] & 15];
    }
    hex[32] = '\n';
    sha.update(hex, 33);
  }
  uint8_t digest[32];
  sha.final(digest);
  std::memcpy(out, digest, 16);
}

uint8_t score(const float* a, const float* b, uint32_t dim) {
  // Each product of two f32 is exact in f64, so a fused multiply-add can't
  // change the sum: only its order matters, and it is the index order.
  double dot = 0;
  for (uint32_t i = 0; i < dim; ++i) dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  const double s = std::round(dot * 255.0);  // half away from zero
  if (!(s > 0)) return 0;
  if (s > 255) return 255;
  return static_cast<uint8_t>(s);
}

bool write(Sink& out, const TableIn& in, uint8_t sig[16], uint32_t* fileBytes, uint32_t* headerCrc, const char** error) {
  auto bad = [&](const char* e) {
    if (error) *error = e;
    return false;
  };
  if (in.k < 1 || in.k > 65535) return bad("k");
  const uint32_t n = in.rowCount;
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) {
    order[i] = i;
    if (!in.rows[i].paths || in.rows[i].pathCount == 0) return bad("a row with no path");
    if (!in.rows[i].embedding && in.dim) return bad("a row with no embedding");
  }
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return std::memcmp(in.rows[a].hash, in.rows[b].hash, 16) < 0;
  });
  for (uint32_t i = 1; i < n; ++i)
    if (std::memcmp(in.rows[order[i]].hash, in.rows[order[i - 1]].hash, 16) == 0) return bad("a hash twice");
  const uint32_t ib = n <= 65535 ? 2 : 4;
  const uint32_t unused = ib == 2 ? 0xFFFFu : 0xFFFFFFFFu;

  // DJRW, DJPH.
  std::vector<uint8_t> rows(static_cast<size_t>(n) * kRowStride, 0);
  std::vector<std::pair<uint64_t, uint32_t>> paths;
  std::vector<uint8_t> hashes(static_cast<size_t>(n) * 16);
  for (uint32_t r = 0; r < n; ++r) {
    const RowIn& in_ = in.rows[order[r]];
    std::vector<uint64_t> ph(in_.paths, in_.paths + in_.pathCount);
    std::sort(ph.begin(), ph.end());
    for (size_t k = 1; k < ph.size(); ++k)
      if (ph[k] == ph[k - 1]) return bad("a path twice in a row");
    for (uint64_t h : ph) paths.emplace_back(h, r);
    uint8_t* e = rows.data() + static_cast<size_t>(r) * kRowStride;
    put64(e, ph[0]);
    std::memcpy(e + 8, in_.hash, 8);
    put16(e + 16, in_.bpm10);
    e[18] = in_.camelot;
    e[19] = in_.flags;
    put32(e + 20, in_.artistKey);
    std::memcpy(hashes.data() + static_cast<size_t>(r) * 16, in_.hash, 16);
  }
  std::sort(paths.begin(), paths.end());
  std::vector<uint8_t> djph(paths.size() * kPathStride);
  for (size_t i = 0; i < paths.size(); ++i) {
    put64(djph.data() + i * kPathStride, paths[i].first);
    put32(djph.data() + i * kPathStride + 8, paths[i].second);
  }

  // DJNB: quantise first, then the K best by (score desc, row asc).
  const uint32_t stride = in.k * (ib + 1);
  std::vector<uint8_t> djnb(static_cast<size_t>(n) * stride, 0);
  std::vector<std::pair<int, uint32_t>> cand;  // (-score, row): ascending is the order wanted
  for (uint32_t a = 0; a < n; ++a) {
    const RowIn& ra = in.rows[order[a]];
    cand.clear();
    for (uint32_t b = 0; b < n; ++b) {
      if (b == a) continue;
      const RowIn& rb = in.rows[order[b]];
      if (std::strcmp(ra.songKey ? ra.songKey : "", rb.songKey ? rb.songKey : "") == 0) continue;
      cand.emplace_back(-static_cast<int>(score(ra.embedding, rb.embedding, in.dim)), b);
    }
    std::sort(cand.begin(), cand.end());
    uint8_t* list = djnb.data() + static_cast<size_t>(a) * stride;
    for (uint32_t j = 0; j < in.k; ++j) {
      uint8_t* e = list + j * (ib + 1);
      const bool used = j < cand.size();
      const uint32_t idx = used ? cand[j].second : unused;
      if (ib == 2)
        put16(e, static_cast<uint16_t>(idx));
      else
        put32(e, idx);
      e[ib] = used ? static_cast<uint8_t>(-cand[j].first) : 0;
    }
  }

  // The signature, the strings, the header.
  selectionSignature(in.modelId, in.modelVersion, in.metric, in.k, reinterpret_cast<const uint8_t(*)[16]>(hashes.data()),
                     n, sig);
  std::vector<uint8_t> strs{0};
  auto add = [&](const char* s) -> uint32_t {
    if (!s || !*s) return 0;
    const uint32_t off = static_cast<uint32_t>(strs.size());
    strs.insert(strs.end(), s, s + std::strlen(s));
    strs.push_back(0);
    return off;
  };
  uint8_t th[kHeaderBytes - kCommonHeaderBytes] = {};
  put32(th, n);
  put32(th + 4, static_cast<uint32_t>(paths.size()));
  put16(th + 8, static_cast<uint16_t>(in.k));
  th[10] = static_cast<uint8_t>(ib);
  th[11] = kScoreCosine;
  put32(th + 12, in.builtTime);
  std::memcpy(th + 16, sig, 16);
  put32(th + 32, add(in.modelId));
  put32(th + 36, add(in.modelVersion));
  put32(th + 40, add(in.metric));
  put32(th + 44, in.tagsGeneration);
  put32(th + 48, add(in.license));
  put32(th + 52, add(in.attribution));

  SectionOut s[4] = {
      SectionOut{kDjrw, kSectionRequired, n, kRowStride, static_cast<uint32_t>(rows.size())},
      SectionOut{kDjnb, kSectionRequired, n, stride, static_cast<uint32_t>(djnb.size())},
      SectionOut{kDjph, kSectionRequired, static_cast<uint32_t>(paths.size()), kPathStride,
                 static_cast<uint32_t>(djph.size())},
      SectionOut{kStrs, kSectionRequired, 0, 0, static_cast<uint32_t>(strs.size())},
  };
  const std::vector<uint8_t>* data[4] = {&rows, &djnb, &djph, &strs};
  FileMeta fm;
  fm.magic = kMagicMpdj;
  fm.minor = in.minor;
  fm.generation = in.generation;
  fm.cardId = in.cardId;
  ContainerWriter cw;
  if (!cw.begin(out, fm, th, kHeaderBytes, s, 4)) return bad("layout");
  for (int i = 0; i < 4; ++i)
    if (s[i].bytes && !cw.write(data[i]->data(), s[i].bytes)) return bad("sink");
  if (!cw.finish(fileBytes, headerCrc)) return bad("sink");
  return true;
}

}  // namespace mpdj
}  // namespace cardcontract
