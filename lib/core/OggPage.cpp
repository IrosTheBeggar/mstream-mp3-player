// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "OggPage.h"

#include <cstring>

namespace ogg {

namespace {

// The CRC's table, made at compile time: 1 KB of rodata (flash on the
// ESP32, nothing in IRAM or .data).
struct CrcTable {
  uint32_t t[256];
};

constexpr CrcTable makeCrcTable() {
  CrcTable c{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t r = i << 24;
    for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
    c.t[i] = r;
  }
  return c;
}

constexpr CrcTable kCrc = makeCrcTable();

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int64_t le64(const uint8_t* p) {
  return static_cast<int64_t>(static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32));
}

}  // namespace

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
  for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ kCrc.t[((crc >> 24) ^ p[i]) & 0xFF];
  return crc;
}

bool isCapture(const uint8_t* p) { return p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S'; }

bool parseHeader(const uint8_t* buf, size_t n, Header* out) {
  if (n < kHeaderBytes || !isCapture(buf) || buf[4] != 0) return false;
  const uint8_t segments = buf[26];
  if (n < kHeaderBytes + segments) return false;
  Header& h = *out;
  h.flags = buf[5];
  h.granule = le64(buf + 6);
  h.serial = le32(buf + 14);
  h.sequence = le32(buf + 18);
  h.crc = le32(buf + 22);
  h.segments = segments;
  h.continues = segments > 0 && buf[kHeaderBytes + segments - 1] == 255;
  h.headerBytes = kHeaderBytes + segments;
  uint32_t body = 0;
  for (uint32_t i = 0; i < segments; ++i) body += buf[kHeaderBytes + i];
  h.bodyBytes = body;
  return true;
}

bool crcOk(const uint8_t* page, const Header& h) {
  // The CRC field counts as zeros, without touching the buffer.
  static const uint8_t kZeros[4] = {0, 0, 0, 0};
  uint32_t c = crc32(page, 22);
  c = crc32(kZeros, 4, c);
  c = crc32(page + 26, h.bytes() - 26, c);
  return c == h.crc;
}

int32_t findCapture(const uint8_t* buf, size_t n, size_t from) {
  for (size_t i = from; i + 4 <= n; ++i) {
    if (buf[i] == 'O' && isCapture(buf + i)) return static_cast<int32_t>(i);
  }
  return -1;
}

int32_t lastCompleting(const uint8_t* lacing, uint32_t segments) {
  int32_t last = -1;
  for (uint32_t i = 0; i < segments; ++i) {
    if (lacing[i] < 255) last = static_cast<int32_t>(i);
  }
  return last;
}

const char* PageReader::readName(Read r) {
  switch (r) {
    case Read::Ok: return "ok";
    case Read::NotPage: return "no page header";
    case Read::Short: return "the file ends inside the page";
    case Read::BadCrc: return "a bad CRC";
    case Read::Partial: return "a slice of the page read, the rest to come";
  }
  return "?";
}

uint32_t PageReader::readAt(uint32_t offset, uint8_t* buf, uint32_t n) {
  ++reads_;
  const uint32_t got = file_.readAt(offset, buf, n);
  bytes_ += got;
  return got;
}

PageReader::Read PageReader::parseAt(const uint8_t* buf, uint32_t got, Header* out) {
  if (got < kHeaderBytes) return Read::Short;
  if (!isCapture(buf) || buf[4] != 0) return Read::NotPage;
  if (got < kHeaderBytes + buf[26]) return Read::Short;
  parseHeader(buf, got, out);
  return Read::Ok;
}

uint32_t PageReader::readChunk(uint32_t offset, uint32_t n) {
  if (n > kMaxPageBytes) n = kMaxPageBytes;
  partGot_ = 0;  // the buffer is the chunk's now: a page read in slices starts over
  chunkAt_ = offset;
  chunkGot_ = offset < size_ ? readAt(offset, buf_, n) : 0;
  return chunkGot_;
}

bool PageReader::headerInChunk(uint32_t offset, Header* out) const {
  if (chunkGot_ == 0 || offset < chunkAt_ || offset >= chunkAt_ + chunkGot_) return false;
  const uint32_t i = offset - chunkAt_;
  return parseHeader(buf_ + i, chunkGot_ - i, out);
}

bool PageReader::pageInChunk(uint32_t offset, Page* out) const {
  Header h;
  if (!headerInChunk(offset, &h)) return false;
  const uint32_t i = offset - chunkAt_;
  if (static_cast<uint64_t>(i) + h.bytes() > chunkGot_) return false;
  if (!crcOk(buf_ + i, h)) return false;
  out->h = h;
  out->offset = offset;
  out->lacing = buf_ + i + kHeaderBytes;
  out->body = buf_ + i + h.headerBytes;
  return true;
}

PageReader::Read PageReader::read(uint32_t offset, Page* out, uint32_t slice) {
  chunkGot_ = 0;  // the buffer is the page's now
  if (offset >= size_) {
    partGot_ = 0;
    return Read::Short;
  }
  uint32_t got;
  if (partGot_ > 0 && partAt_ == offset) {
    got = partGot_;  // the page's slices so far, its header parsed
    out->h = partHdr_;
  } else {
    partGot_ = 0;
    got = readAt(offset, buf_, kMaxHeaderBytes);
    const Read r = parseAt(buf_, got, &out->h);
    if (r != Read::Ok) return r;
  }
  const uint32_t total = out->h.bytes();
  while (got < total) {
    uint32_t n = total - got;
    if (slice > 0 && n > slice) n = slice;
    if (readAt(offset + got, buf_ + got, n) != n) {
      partGot_ = 0;
      return Read::Short;
    }
    got += n;
    if (slice > 0 && got < total) {
      partAt_ = offset;
      partGot_ = got;
      partHdr_ = out->h;
      return Read::Partial;
    }
  }
  partGot_ = 0;
  out->offset = offset;
  out->lacing = buf_ + kHeaderBytes;
  out->body = buf_ + out->h.headerBytes;
  return crcOk(buf_, out->h) ? Read::Ok : Read::BadCrc;
}

PageReader::Read PageReader::readHeader(uint32_t offset, Header* out) {
  peek_ = nullptr;
  peekBytes_ = 0;
  if (offset >= size_) return Read::Short;
  const uint32_t got = readAt(offset, hdr_, sizeof(hdr_));
  const Read r = parseAt(hdr_, got, out);
  if (r != Read::Ok) return r;
  peek_ = hdr_ + out->headerBytes;
  uint32_t n = got - out->headerBytes;
  if (n > kPeekBytes) n = kPeekBytes;
  if (n > out->bodyBytes) n = out->bodyBytes;
  peekBytes_ = n;
  return Read::Ok;
}

void PageReader::beginScan(uint32_t from, uint32_t limit, uint32_t serial, bool anySerial, Scan* out, uint32_t chunk,
                           uint64_t budget) const {
  if (chunk > kMaxPageBytes) chunk = kMaxPageBytes;
  if (chunk < kHeaderBytes) chunk = kHeaderBytes;
  const uint64_t stop64 = static_cast<uint64_t>(from) + limit;
  Scan& s = *out;
  s = Scan{};
  s.active = true;
  s.at = from;
  s.stop = stop64 < size_ ? static_cast<uint32_t>(stop64) : size_;
  s.serial = serial;
  s.anySerial = anySerial;
  s.chunk = chunk;
  s.budget = budget;
}

PageReader::Found PageReader::findStep(Scan* s, Page* out, uint32_t slice) {
  if (!s->active) return Found::None;
  if (s->budget && s->spent >= s->budget) {
    s->active = false;
    return Found::None;
  }
  const uint64_t spent0 = bytes_;
  if (s->reading) {
    // A candidate whose rest lay past its chunk: its own read, which takes
    // the buffer (a slice of it a step, with `slice`). A page of ours is
    // the one looked for; a good page of another serial is stepped over
    // whole; a damaged or false one is scanned past from the byte after
    // its capture pattern (the chunk again, next step).
    const Read r = read(s->candidate, out, slice);
    s->spent += bytes_ - spent0;
    if (r == Read::Partial) return Found::More;
    s->reading = false;
    if (r == Read::Ok && (s->anySerial || out->h.serial == s->serial)) {
      s->active = false;
      return Found::Page;
    }
    s->at = r == Read::Ok ? out->end() : s->candidate + 1;
    if (s->at >= s->stop) s->active = false;
    return s->active ? Found::More : Found::None;
  }
  if (s->at >= s->stop) {
    s->active = false;
    return Found::None;
  }
  const uint32_t got = readChunk(s->at, s->chunk);
  s->spent += bytes_ - spent0;
  if (got < 4) {
    s->active = false;
    return Found::None;
  }
  uint32_t scanFrom = 0;
  for (;;) {
    const int32_t i = findCapture(buf_, got, scanFrom);
    if (i < 0) break;
    const uint32_t candidate = s->at + static_cast<uint32_t>(i);
    if (candidate >= s->stop) {
      s->active = false;
      return Found::None;
    }
    // The header from the chunk when all of it is there: a wrong version
    // or another stream's serial is passed over in memory, and a page
    // that lies in the chunk whole is checked in memory too (a page of
    // ours: done; a damaged or false one: the scan goes on past its
    // capture pattern).
    const uint32_t left = got - static_cast<uint32_t>(i);
    if (left >= kHeaderBytes && left >= kHeaderBytes + buf_[i + 26]) {
      Header h;
      if (parseAt(buf_ + i, left, &h) != Read::Ok || (!s->anySerial && h.serial != s->serial)) {
        scanFrom = static_cast<uint32_t>(i) + 1;
        continue;
      }
      if (h.bytes() <= left) {
        if (crcOk(buf_ + i, h)) {
          out->h = h;
          out->offset = candidate;
          out->lacing = buf_ + i + kHeaderBytes;
          out->body = buf_ + i + h.headerBytes;
          s->active = false;
          return Found::Page;
        }
        scanFrom = static_cast<uint32_t>(i) + 1;
        continue;
      }
    }
    // The rest of it is past the chunk: its own read, the next step's.
    s->candidate = candidate;
    s->reading = true;
    return Found::More;
  }
  // The pattern could straddle the chunk's end: the last 3 bytes again.
  if (got < s->chunk || static_cast<uint64_t>(s->at) + got >= s->stop) {  // the file's end, or the scan's
    s->active = false;
    return Found::None;
  }
  s->at += got - 3;
  return Found::More;
}

bool PageReader::find(uint32_t from, uint32_t limit, uint32_t serial, bool anySerial, Page* out, uint32_t chunk,
                      uint64_t budget) {
  Scan s;
  beginScan(from, limit, serial, anySerial, &s, chunk, budget);
  for (;;) {
    const Found f = findStep(&s, out);
    if (f != Found::More) return f == Found::Page;
  }
}

bool PageReader::findHeaderInChunk(uint32_t from, uint32_t stop, Header* out, uint32_t* offset) const {
  *offset = from;
  if (chunkGot_ == 0 || from < chunkAt_ || from >= chunkAt_ + chunkGot_) return false;
  uint32_t scanFrom = from - chunkAt_;
  for (;;) {
    const int32_t i = findCapture(buf_, chunkGot_, scanFrom);
    if (i < 0) break;
    const uint32_t candidate = chunkAt_ + static_cast<uint32_t>(i);
    if (candidate >= stop) {
      *offset = stop;
      return false;
    }
    const uint32_t left = chunkGot_ - static_cast<uint32_t>(i);
    if (left < kHeaderBytes || left < kHeaderBytes + buf_[i + 26]) {
      *offset = candidate;  // its header runs past the chunk: the file's scan judges it
      return false;
    }
    if (parseHeader(buf_ + i, left, out) && static_cast<uint64_t>(candidate) + out->bytes() <= size_) {
      *offset = candidate;
      return true;
    }
    scanFrom = static_cast<uint32_t>(i) + 1;
  }
  // The pattern could straddle the chunk's end: the last 3 bytes again.
  const uint32_t end = chunkAt_ + chunkGot_;
  *offset = chunkGot_ > 3 && end - 3 > from ? end - 3 : from;
  return false;
}

bool PageReader::findHeader(uint32_t from, uint32_t limit, Header* out, uint32_t* offset, uint32_t chunk,
                            uint64_t budget) {
  if (chunk > kMaxPageBytes) chunk = kMaxPageBytes;
  if (chunk < kHeaderBytes) chunk = kHeaderBytes;
  const uint64_t stop64 = static_cast<uint64_t>(from) + limit;
  const uint32_t stop = stop64 < size_ ? static_cast<uint32_t>(stop64) : size_;
  const uint64_t spent0 = bytes_;
  uint32_t at = from;
  while (at < stop) {
    if (budget && bytes_ - spent0 >= budget) return false;
    const uint32_t got = readChunk(at, chunk);
    if (got < 4) return false;
    uint32_t scanFrom = 0;
    for (;;) {
      const int32_t i = findCapture(buf_, got, scanFrom);
      if (i < 0) break;
      const uint32_t candidate = at + static_cast<uint32_t>(i);
      if (candidate >= stop) return false;
      // The header from the chunk when all of it is there, else its own read.
      Read r;
      if (static_cast<uint32_t>(i) + kMaxHeaderBytes <= got) {
        r = parseAt(buf_ + i, got - static_cast<uint32_t>(i), out);
      } else {
        if (budget && bytes_ - spent0 >= budget) return false;
        r = readHeader(candidate, out);
      }
      if (r == Read::Ok && static_cast<uint64_t>(candidate) + out->bytes() <= size_) {
        *offset = candidate;
        return true;
      }
      scanFrom = static_cast<uint32_t>(i) + 1;
    }
    // The file's end, or the scan's: nothing past this chunk (a chunk that
    // ends exactly at the stop would otherwise cost a 3-byte read there).
    if (got < chunk || static_cast<uint64_t>(at) + got >= stop) return false;
    at += got - 3;
  }
  return false;
}

}  // namespace ogg
