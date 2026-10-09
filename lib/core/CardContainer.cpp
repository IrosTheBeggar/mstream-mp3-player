// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardContainer.h"

#include <cstring>

namespace cardcontract {

bool MemSource::read(uint32_t offset, void* out, uint32_t n) {
  if (offset > n_ || n > n_ - offset) return false;
  if (n) std::memcpy(out, p_ + offset, n);
  return true;
}

bool BufferSink::write(uint32_t offset, const void* data, uint32_t n) {
  if (offset > cap_ || n > cap_ - offset) return false;
  if (n) std::memcpy(p_ + offset, data, n);
  if (offset + n > size_) size_ = offset + n;
  return true;
}

const char* whyName(Why w) {
  switch (w) {
    case Why::Ok: return "ok";
    case Why::Io: return "io";
    case Why::Short: return "short";
    case Why::Magic: return "magic";
    case Why::Major: return "major";
    case Why::HeaderBytes: return "headerBytes";
    case Why::SectionCount: return "sectionCount";
    case Why::FileBytes: return "fileBytes";
    case Why::HeaderCrc: return "headerCrc";
    case Why::Layout: return "layout";
    case Why::UnknownRequired: return "unknownRequired";
    case Why::Missing: return "missing";
    case Why::Shape: return "shape";
    case Why::SectionCrc: return "sectionCrc";
    case Why::Counts: return "counts";
    case Why::Enum: return "enum";
    case Why::String: return "string";
    case Why::StringOrder: return "stringOrder";
    case Why::Name: return "name";
    case Why::PathLength: return "pathLength";
    case Why::FoldRoot: return "foldRoot";
    case Why::FoldParent: return "foldParent";
    case Why::FoldOrder: return "foldOrder";
    case Why::FirstRecord: return "firstRecord";
    case Why::RecsFolder: return "recsFolder";
    case Why::RecsOrder: return "recsOrder";
    case Why::Hidx: return "hidx";
    case Why::Orig: return "orig";
    case Why::Comp: return "comp";
    case Why::Libr: return "libr";
    case Why::DjRows: return "djRows";
    case Why::DjPaths: return "djPaths";
    case Why::DjNeighbours: return "djNeighbours";
    case Why::PendOrder: return "pendOrder";
    case Why::PendPath: return "pendPath";
    case Why::PendOp: return "pendOp";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------
Why Container::open(Source& src, const FormatSpec& spec, uint8_t* typeHeader) {
  src_ = &src;
  header_ = Header();
  for (uint32_t i = 0; i < kMaxKnown; ++i) known_[i] = Section();
  const uint32_t size = src.size();
  // One small buffer for the common header, the header's CRC and each
  // directory entry: this runs under the card's reads on the loop task's
  // 8 KB and the card worker's 6 KB (the boot's compaction, gs, gt; with
  // the SD driver's log line on top), so its frame stays small (2026-10-09:
  // 672 B with a 256 B buffer and a table of the types seen).
  uint8_t buf[64];
  static_assert(sizeof(buf) >= kCommonHeaderBytes && sizeof(buf) >= kDirEntryBytes, "the buffer holds both");
  if (size < kCommonHeaderBytes) return Why::Short;
  if (!src.read(0, buf, kCommonHeaderBytes)) return Why::Io;
  header_.magic = get32(buf);
  header_.major = get16(buf + 4);
  header_.minor = get16(buf + 6);
  header_.headerBytes = get32(buf + 8);
  header_.sectionCount = get32(buf + 12);
  header_.fileBytes = get32(buf + 16);
  header_.generation = get32(buf + 20);
  header_.cardId = get64(buf + 24);
  header_.headerCrc = get32(buf + 32);
  if (header_.magic != spec.magic) return Why::Magic;
  if (header_.major != kMajor) return Why::Major;
  if (header_.headerBytes % 8 || header_.headerBytes < spec.headerBytes) return Why::HeaderBytes;
  if (header_.sectionCount > kMaxSections) return Why::SectionCount;
  const uint64_t dirEnd = static_cast<uint64_t>(header_.headerBytes) + kDirEntryBytes * header_.sectionCount;
  if (dirEnd > size) return Why::Short;
  if (header_.fileBytes != size) return Why::FileBytes;

  // The header's CRC, over the header and the directory with the CRC field
  // as 0: read through the small buffer, the type's known fields kept.
  uint32_t crc = 0;
  for (uint32_t at = 0; at < dirEnd;) {
    const uint32_t n = static_cast<uint32_t>(dirEnd - at < sizeof(buf) ? dirEnd - at : sizeof(buf));
    if (!src.read(at, buf, n)) return Why::Io;
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t o = at + k;
      if (o >= 32 && o < 36) buf[k] = 0;
      if (o >= kCommonHeaderBytes && o < spec.headerBytes && typeHeader) typeHeader[o - kCommonHeaderBytes] = buf[k];
    }
    crc = crc32(buf, n, crc);
    at += n;
  }
  if (crc != header_.headerCrc) return Why::HeaderCrc;

  // The directory.
  uint64_t end = dirEnd;  // where the previous section ended
  for (uint32_t i = 0; i < header_.sectionCount; ++i) {
    const uint8_t* e = buf;
    if (!src.read(header_.headerBytes + kDirEntryBytes * i, buf, kDirEntryBytes)) return Why::Io;
    Section s;
    s.type = get32(e);
    s.flags = get32(e + 4);
    s.offset = get32(e + 8);
    s.bytes = get32(e + 12);
    s.count = get32(e + 16);
    s.stride = get32(e + 20);
    s.crc = get32(e + 24);
    if (s.offset % 8 || s.offset < end || s.offset - end >= 8) return Why::Layout;
    const uint64_t sEnd = static_cast<uint64_t>(s.offset) + s.bytes;
    if (sEnd > header_.fileBytes) return Why::Layout;
    // A type once: the entries before it read again (their type's 4 bytes,
    // a few files' sections), not kept in a table of 64 on the stack.
    for (uint32_t k = 0; k < i; ++k) {
      uint8_t t[4];
      if (!src.read(header_.headerBytes + kDirEntryBytes * k, t, sizeof(t))) return Why::Io;
      if (get32(t) == s.type) return Why::Layout;
    }
    end = sEnd;
    uint32_t known = spec.count;
    for (uint32_t k = 0; k < spec.count; ++k)
      if (spec.sections[k].type == s.type) known = k;
    if (known == spec.count) {
      if (s.flags & kSectionRequired) return Why::UnknownRequired;
      continue;
    }
    const SectionSpec& ks = spec.sections[known];
    if (ks.stride == 0) {
      if (s.count != 0 || s.stride != 0) return Why::Shape;
    } else if (s.stride < ks.stride || static_cast<uint64_t>(s.count) * s.stride != s.bytes) {
      return Why::Shape;
    }
    known_[known] = s;
  }
  if (end != header_.fileBytes) return Why::Layout;
  for (uint32_t k = 0; k < spec.count; ++k)
    if (spec.sections[k].required && !known_[k].present()) return Why::Missing;
  return Why::Ok;
}

Why Container::checkCrc(uint32_t i, uint8_t* buf, uint32_t bufLen) const {
  const Section& s = known_[i];
  uint32_t crc = 0;
  for (uint32_t at = 0; at < s.bytes;) {
    const uint32_t n = s.bytes - at < bufLen ? s.bytes - at : bufLen;
    if (!src_->read(s.offset + at, buf, n)) return Why::Io;
    crc = crc32(buf, n, crc);
    at += n;
  }
  return crc == s.crc ? Why::Ok : Why::SectionCrc;
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------
void Stream::begin(Source* src, uint32_t start, uint32_t end, uint8_t* buf, uint32_t bufLen) {
  src_ = src;
  buf_ = buf;
  bufLen_ = bufLen;
  start_ = pos_ = bufAt_ = start;
  end_ = end < start ? start : end;
  bufFill_ = 0;
  crc_ = 0;
  failed_ = !src || !buf || bufLen == 0;
}

bool Stream::fill() {
  // The buffer is spent: the next bytes, up to the end.
  bufAt_ = pos_;
  const uint32_t left = end_ - pos_;
  const uint32_t n = left < bufLen_ ? left : bufLen_;
  if (n == 0 || !src_->read(pos_, buf_, n)) {
    failed_ = true;
    bufFill_ = 0;
    return false;
  }
  bufFill_ = n;
  return true;
}

bool Stream::read(void* out, uint32_t n) {
  if (failed_) return false;
  if (n > end_ - pos_) {
    failed_ = true;
    return false;
  }
  uint8_t* o = static_cast<uint8_t*>(out);
  while (n) {
    if (pos_ >= bufAt_ + bufFill_ && !fill()) return false;
    const uint32_t at = pos_ - bufAt_;
    uint32_t k = bufFill_ - at;
    if (k > n) k = n;
    if (o) {
      std::memcpy(o, buf_ + at, k);
      o += k;
    }
    crc_ = crc32(buf_ + at, k, crc_);
    pos_ += k;
    n -= k;
  }
  return true;
}

bool Stream::skipTo(uint32_t pos) {
  if (failed_ || pos < pos_ || pos > end_) {
    failed_ = true;
    return false;
  }
  return read(nullptr, pos - pos_);
}

bool Stream::readString(char* out, size_t cap, size_t* len) {
  size_t n = 0;
  for (;;) {
    if (failed_) return false;
    if (pos_ >= end_) {
      failed_ = true;
      return false;
    }
    if (pos_ >= bufAt_ + bufFill_ && !fill()) return false;
    const uint32_t at = pos_ - bufAt_;
    const uint8_t* p = buf_ + at;
    const uint32_t avail = bufFill_ - at;
    const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(p, 0, avail));
    const uint32_t k = nul ? static_cast<uint32_t>(nul - p) : avail;
    if (out && cap && n + 1 < cap) {
      const size_t room = cap - 1 - n;
      std::memcpy(out + n, p, k < room ? k : room);
    }
    n += k;
    crc_ = crc32(p, nul ? k + 1 : k, crc_);
    pos_ += nul ? k + 1 : k;
    if (nul) break;
  }
  if (out && cap) out[n < cap - 1 ? n : cap - 1] = 0;
  if (len) *len = n;
  return true;
}

Why readStringAt(Source& src, const Section& strs, uint32_t off, char* out, size_t cap, size_t* len) {
  if (cap) out[0] = 0;
  if (off >= strs.bytes) return Why::String;
  size_t n = 0;
  uint8_t buf[64];
  for (uint32_t at = off; at < strs.bytes;) {
    const uint32_t k = strs.bytes - at < sizeof(buf) ? strs.bytes - at : static_cast<uint32_t>(sizeof(buf));
    if (!src.read(strs.offset + at, buf, k)) return Why::Io;
    const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(buf, 0, k));
    const uint32_t take = nul ? static_cast<uint32_t>(nul - buf) : k;
    if (cap && n + 1 < cap) {
      const size_t room = cap - 1 - n;
      std::memcpy(out + n, buf, take < room ? take : room);
    }
    n += take;
    if (nul) {
      if (cap) out[n < cap - 1 ? n : cap - 1] = 0;
      if (len) *len = n;
      return Why::Ok;
    }
    at += k;
  }
  if (cap) out[0] = 0;
  return Why::String;
}

// ---------------------------------------------------------------------------
// ContainerWriter
// ---------------------------------------------------------------------------
bool ContainerWriter::begin(Sink& sink, const FileMeta& meta, const uint8_t* typeHeader, uint32_t headerBytes,
                            const SectionOut* s, uint32_t n) {
  sink_ = &sink;
  meta_ = meta;
  bad_ = headerBytes < kCommonHeaderBytes || headerBytes % 8 || headerBytes - kCommonHeaderBytes > sizeof(head_) ||
         n > kMaxSections;
  if (bad_) return false;
  headerBytes_ = headerBytes;
  std::memset(head_, 0, sizeof(head_));
  if (typeHeader) std::memcpy(head_, typeHeader, headerBytes - kCommonHeaderBytes);
  n_ = n;
  uint64_t at = static_cast<uint64_t>(headerBytes) + kDirEntryBytes * n;
  for (uint32_t i = 0; i < n; ++i) {
    s_[i] = s[i];
    crc_[i] = 0;
    at = (at + 7) & ~static_cast<uint64_t>(7);
    if (at > 0xFFFFFFFFull) bad_ = true;
    offset_[i] = static_cast<uint32_t>(at);
    at += s[i].bytes;
  }
  if (at > 0xFFFFFFFFull) bad_ = true;
  fileBytes_ = static_cast<uint32_t>(at);
  cur_ = 0;
  written_ = 0;
  // The gaps' zeros, and room for the header (written last).
  static const uint8_t kZeros[8] = {};
  for (uint32_t i = 0; i + 1 < n && !bad_; ++i) {
    const uint32_t end = offset_[i] + s_[i].bytes;
    if (offset_[i + 1] > end) bad_ = !sink_->write(end, kZeros, offset_[i + 1] - end);
  }
  return !bad_;
}

bool ContainerWriter::write(const void* data, uint32_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (n && !bad_) {
    while (cur_ < n_ && written_ == s_[cur_].bytes) {
      ++cur_;
      written_ = 0;
    }
    if (cur_ >= n_) {
      bad_ = true;
      break;
    }
    uint32_t k = s_[cur_].bytes - written_;
    if (k > n) k = n;
    if (!sink_->write(offset_[cur_] + written_, p, k)) {
      bad_ = true;
      break;
    }
    crc_[cur_] = crc32(p, k, crc_[cur_]);
    written_ += k;
    p += k;
    n -= k;
  }
  return !bad_;
}

bool ContainerWriter::finish(uint32_t* fileBytes, uint32_t* headerCrc) {
  while (cur_ < n_ && written_ == s_[cur_].bytes) {
    ++cur_;
    written_ = 0;
  }
  if (bad_ || cur_ != n_) return false;
  uint8_t common[kCommonHeaderBytes] = {};
  put32(common, meta_.magic);
  put16(common + 4, meta_.major);
  put16(common + 6, meta_.minor);
  put32(common + 8, headerBytes_);
  put32(common + 12, n_);
  put32(common + 16, fileBytes_);
  put32(common + 20, meta_.generation);
  put64(common + 24, meta_.cardId);
  uint32_t crc = crc32(common, kCommonHeaderBytes);
  crc = crc32(head_, headerBytes_ - kCommonHeaderBytes, crc);
  for (uint32_t i = 0; i < n_; ++i) {
    uint8_t e[kDirEntryBytes] = {};
    put32(e, s_[i].type);
    put32(e + 4, s_[i].flags);
    put32(e + 8, offset_[i]);
    put32(e + 12, s_[i].bytes);
    put32(e + 16, s_[i].count);
    put32(e + 20, s_[i].stride);
    put32(e + 24, crc_[i]);
    crc = crc32(e, kDirEntryBytes, crc);
    if (!sink_->write(headerBytes_ + kDirEntryBytes * i, e, kDirEntryBytes)) return false;
  }
  put32(common + 32, crc);
  if (!sink_->write(kCommonHeaderBytes, head_, headerBytes_ - kCommonHeaderBytes)) return false;
  if (!sink_->write(0, common, kCommonHeaderBytes)) return false;
  if (fileBytes) *fileBytes = fileBytes_;
  if (headerCrc) *headerCrc = crc;
  return true;
}

// ---------------------------------------------------------------------------
// The election and the guard
// ---------------------------------------------------------------------------
Election elect(const RootCandidate& bin, const RootCandidate& tmp) {
  Election e;
  if (bin.valid && tmp.valid) {
    e.pick = tmp.generation > bin.generation ? Pick::Tmp : Pick::Bin;
    e.sameCommit = bin.commitId == tmp.commitId && bin.headerCrc == tmp.headerCrc;
  } else if (bin.valid) {
    e.pick = Pick::Bin;
  } else if (tmp.valid) {
    e.pick = Pick::Tmp;
  }
  return e;
}

bool unknownMajor(const uint8_t* head, size_t n) {
  if (!head || n < 6) return false;
  const uint32_t magic = get32(head);
  if (magic != kMagicMsmf && magic != kMagicMptg && magic != kMagicMpdj && magic != kMagicMspd) return false;
  return get16(head + 4) != kMajor;
}

}  // namespace cardcontract
