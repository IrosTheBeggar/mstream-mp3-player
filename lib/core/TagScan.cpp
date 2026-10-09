// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagScan.h"

#include <cstring>

#include "TagRules.h"
#include "TrackProgress.h"

namespace tagscan {

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

// ===========================================================================
// The record's fields
// ===========================================================================
void SingleField::clear() {
  buf_[0] = 0;
  len_ = 0;
  set_ = false;
  truncated_ = false;
}

bool SingleField::add(const char* value, size_t n) {
  if (set_ || n == 0 || !value) return false;
  const size_t keep = cc::utf8CutLength(value, n, kMax);
  for (size_t i = 0; i < keep; ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    buf_[i] = (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
  }
  buf_[keep] = 0;
  len_ = static_cast<uint16_t>(keep);
  set_ = true;
  truncated_ = keep < n;
  return true;
}

void Record::clear() {
  rec = mptg::Record();
  title.clear();
  album.clear();
  titleSort.clear();
  artistSort.clear();
  albumSort.clear();
  albumArtistSort.clear();
  mbAlbumId.clear();
  mbRecordingId.clear();
  artist.clear();
  albumArtist.clear();
  genre.clear();
  composer.clear();
}

const char* Record::field(uint32_t f) const {
  switch (f) {
    case cc::kTitle: return title.data();
    case cc::kArtist: return artist.data();
    case cc::kAlbum: return album.data();
    case cc::kAlbumArtist: return albumArtist.data();
    case cc::kGenre: return genre.data();
    case cc::kComposer: return composer.data();
    case cc::kTitleSort: return titleSort.data();
    case cc::kArtistSort: return artistSort.data();
    case cc::kAlbumSort: return albumSort.data();
    case cc::kAlbumArtistSort: return albumArtistSort.data();
    case cc::kMbAlbumId: return mbAlbumId.data();
    case cc::kMbRecordingId: return mbRecordingId.data();
    default: return "";
  }
}

size_t Record::fieldLength(uint32_t f) const {
  switch (f) {
    case cc::kTitle: return title.size();
    case cc::kArtist: return artist.size();
    case cc::kAlbum: return album.size();
    case cc::kAlbumArtist: return albumArtist.size();
    case cc::kGenre: return genre.size();
    case cc::kComposer: return composer.size();
    case cc::kTitleSort: return titleSort.size();
    case cc::kArtistSort: return artistSort.size();
    case cc::kAlbumSort: return albumSort.size();
    case cc::kAlbumArtistSort: return albumArtistSort.size();
    case cc::kMbAlbumId: return mbAlbumId.size();
    case cc::kMbRecordingId: return mbRecordingId.size();
    default: return 0;
  }
}

void Record::fields(const char* out[cc::kRunFields]) const {
  for (uint32_t f = 0; f < cc::kRunFields; ++f) out[f] = field(f);
}

void Record::toRunFields(cc::RunFields* out) const {
  out->clear();
  uint32_t n = 0;
  for (uint32_t f = 0; f < cc::kRunFields; ++f) {
    const size_t len = fieldLength(f);
    if (!len) continue;
    out->set(f, field(f), len);
    n = f + 1;
  }
  out->n = static_cast<uint8_t>(n);
}

bool Record::truncated() const {
  return title.truncated() || album.truncated() || titleSort.truncated() || artistSort.truncated() ||
         albumSort.truncated() || albumArtistSort.truncated() || mbAlbumId.truncated() ||
         mbRecordingId.truncated() || artist.truncated() || albumArtist.truncated() || genre.truncated() ||
         composer.truncated();
}

namespace {

bool asciiIEq(const char* a, size_t an, const char* b) {
  const size_t bn = std::strlen(b);
  if (an != bn) return false;
  for (size_t i = 0; i < an; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
    if (x != y) return false;
  }
  return true;
}

}  // namespace

Kind kindOf(const char* name) {
  if (!name) return Kind::Unknown;
  const char* dot = std::strrchr(name, '.');
  if (!dot) return Kind::Unknown;
  const size_t n = std::strlen(dot + 1);
  if (asciiIEq(dot + 1, n, "mp3")) return Kind::Mp3;
  if (asciiIEq(dot + 1, n, "flac")) return Kind::Flac;
  if (asciiIEq(dot + 1, n, "opus")) return Kind::Opus;
  return Kind::Unknown;
}

namespace {

using Value = Scanner::Value;
using FrameRef = Scanner::FrameRef;

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
// lofty's unsynch(): seven bits a byte, the high bit ignored.
uint32_t syncsafe(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0] & 0x7F) << 21) | (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
         (static_cast<uint32_t>(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

uint32_t encodeUtf8(uint32_t cp, uint8_t b[4]) {
  if (cp < 0x80) {
    b[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    b[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    b[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    b[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    b[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    b[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 3;
  }
  b[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
  b[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
  b[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  b[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 4;
}

// ===========================================================================
// The cursor: one buffer over the file. Every byte the parsers see comes
// through at(), which checks the range against the file and the buffer.
//
// The budget (Limits): a walk of the tags (walk(true)) reads within
// readBudget and maxReads; the rest (the audio's first frame, the tail, the
// length, the values located) within the reserve on top, whatever the walk
// spent. The budget stopping a walk ends the walk only (stopped()); the
// scan goes on in the reserve, and the result is Partial. An Ogg page header
// stepped over (`page`) counts its bytes but not as a read.
// ===========================================================================
class Cursor {
public:
  Cursor(Source& s, uint8_t* buf, uint32_t cap, const Limits& lim, Stats* st, uint32_t* issues)
      : src_(s), buf_(buf), cap_(cap >= 1024 ? (cap & ~511u) : cap), size_(s.size()), lim_(lim), st_(st),
        issues_(issues) {}

  uint32_t size() const { return size_; }
  bool failed() const { return failed_; }
  bool ioError() const { return ioError_; }
  bool stopped() const { return stopped_; }  // the budget ran out, in a walk or after it
  bool walking() const { return walking_; }
  uint32_t cap() const { return cap_; }
  uint32_t generation() const { return gen_; }
  // Bytes from `off` already in the buffer (0: none).
  uint32_t have(uint32_t off) const {
    return (len_ && off >= base_ && off - base_ < len_) ? len_ - (off - base_) : 0;
  }

  // A walk of the tags begins (true) or ends (false). At its end, a budget
  // stop inside it lets go: the rest reads in the reserve.
  void walk(bool on) {
    walking_ = on;
    if (!on && walkStop_) {
      walkStop_ = false;
      failed_ = false;
    }
  }
  // A v2.2/2.3 tag under tag-level unsynchronisation is read through:
  // `bytes` of it (up to Limits::maxUnsyncTag a file) on top of the budget,
  // for the walk and so for the rest, as the reads that take them in
  // sequence (each moves on by the buffer's aligned part, cap & ~511, and
  // asks for the whole buffer).
  void extend(uint32_t bytes) {
    const uint32_t room = lim_.maxUnsyncTag > extendedTag_ ? lim_.maxUnsyncTag - extendedTag_ : 0;
    if (bytes > room) bytes = room;
    if (!bytes) return;
    extendedTag_ += bytes;
    const uint32_t reads = bytes / (cap_ & ~511u) + 2;
    extraReads_ += reads;
    extraBytes_ += static_cast<uint64_t>(reads) * cap_;
    if (st_) {
      st_->extraBytes = extraBytes_ > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(extraBytes_);
      st_->extraReads = extraReads_;
    }
  }

  // [off, off + n) in the buffer (n <= the buffer), read when it isn't
  // there: nullptr past the end of the file, over the budget, or on a read
  // error. `probe`: read only that much (a header among skipped bytes);
  // `page`: an Ogg page header stepped over.
  const uint8_t* at(uint32_t off, uint32_t n, uint32_t probe = 0, bool page = false) {
    if (failed_ || n == 0 || n > cap_ || off > size_ || n > size_ - off) return nullptr;
    if (len_ && off >= base_ && off - base_ <= len_ && n <= len_ - (off - base_)) return buf_ + (off - base_);
    uint32_t start = off & ~511u;
    if (off - start + n > cap_) start = off;
    uint32_t want = cap_;
    // After a seek, a short read: what follows a skipped picture is usually
    // a few frame headers, not a buffer's worth.
    if (!probe && gen_ > 1 && start != lastEnd_) probe = lim_.seekRead;
    if (probe && probe < want) want = (off - start + n > probe) ? (off - start + n) : probe;
    if (want > size_ - start) want = size_ - start;
    const uint64_t byteCap = static_cast<uint64_t>(lim_.readBudget) + extraBytes_ + (walking_ ? 0 : lim_.reserveBytes);
    const uint32_t readCap = lim_.maxReads + extraReads_ + (walking_ ? 0u : lim_.reserveReads);
    if (spent_ + want > byteCap || (!page && reads_ >= readCap)) {
      failed_ = stopped_ = true;
      if (walking_) walkStop_ = true;
      *issues_ |= kIssueBudget;
      return nullptr;
    }
    spent_ += want;
    if (!page) ++reads_;
    if (st_) {
      ++st_->reads;
      if (page) ++st_->pages;
      st_->bytes += want;
      if (start != lastEnd_) ++st_->seeks;
    }
    lastEnd_ = start + want;
    ++gen_;
    if (!src_.read(start, buf_, want)) {
      failed_ = ioError_ = true;
      walkStop_ = false;
      len_ = 0;
      *issues_ |= kIssueReadError;
      return nullptr;
    }
    base_ = start;
    len_ = want;
    return buf_ + (off - base_);
  }

  // One byte (-1: past the end, or a failed read).
  int byte(uint32_t off) {
    const uint8_t* p = at(off, 1);
    return p ? *p : -1;
  }

private:
  Source& src_;
  uint8_t* buf_;
  uint32_t cap_;
  uint32_t size_;
  const Limits& lim_;
  Stats* st_;
  uint32_t* issues_;
  uint32_t base_ = 0, len_ = 0, lastEnd_ = 0;
  uint64_t spent_ = 0;
  uint32_t gen_ = 1;
  uint32_t reads_ = 0;  // against maxReads (an Ogg page header stepped over isn't one)
  uint32_t extendedTag_ = 0;  // an unsynchronised tag's bytes the budget was extended for
  uint64_t extraBytes_ = 0;   // ... and what that extension gives
  uint32_t extraReads_ = 0;
  bool failed_ = false;
  bool ioError_ = false;
  bool stopped_ = false;
  bool walking_ = false;
  bool walkStop_ = false;  // the budget stopped the walk under way
};

// ===========================================================================
// Byte inputs: a region of the file, an unsynchronised one (ID3), a limit
// inside another, an Ogg packet across pages, base64.
// ===========================================================================
class ByteIn {
public:
  virtual int get() = 0;                 // the next byte, -1: the end (or a failed read)
  virtual bool skip(uint32_t n) = 0;     // false: the end came first
  virtual uint32_t left() const = 0;     // bytes left (an upper bound under unsynchronisation)
  virtual uint32_t filePos() const = 0;  // the file offset of the next stored byte
  // Steps over what lies between the bytes (an Ogg page header) so that
  // filePos() is the next byte's own offset.
  virtual void settle() {}
  bool getN(uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      const int b = get();
      if (b < 0) return false;
      out[i] = static_cast<uint8_t>(b);
    }
    return true;
  }
  bool be32v(uint32_t* v) {
    uint8_t b[4];
    if (!getN(b, 4)) return false;
    *v = be32(b);
    return true;
  }
  bool le32v(uint32_t* v) {
    uint8_t b[4];
    if (!getN(b, 4)) return false;
    *v = le32(b);
    return true;
  }

protected:
  ~ByteIn() = default;
};

class PlainIn final : public ByteIn {
public:
  PlainIn(Cursor& c, uint32_t pos, uint32_t end) : c_(c), pos_(pos), end_(end < c.size() ? end : c.size()) {
    if (pos_ > end_) pos_ = end_;
  }
  int get() override {
    if (pos_ >= end_) return -1;
    if (gen_ != c_.generation() || pos_ < winStart_ || pos_ - winStart_ >= winLen_) {
      const uint8_t* p = c_.at(pos_, 1);
      if (!p) {
        pos_ = end_;
        return -1;
      }
      win_ = p;
      winStart_ = pos_;
      winLen_ = c_.have(pos_);
      gen_ = c_.generation();
    }
    return win_[pos_++ - winStart_];
  }
  bool skip(uint32_t n) override {
    if (n > end_ - pos_) {
      pos_ = end_;
      return false;
    }
    pos_ += n;
    return true;
  }
  uint32_t left() const override { return end_ - pos_; }
  uint32_t filePos() const override { return pos_; }

private:
  Cursor& c_;
  uint32_t pos_, end_;
  const uint8_t* win_ = nullptr;
  uint32_t winStart_ = 0, winLen_ = 0;
  uint32_t gen_ = 0;
};

// ID3 unsynchronisation undone: every FF 00 stored is FF (lofty's
// UnsynchronizedStream: after an FF, a 00 is dropped).
class UnsyncIn final : public ByteIn {
public:
  UnsyncIn(ByteIn& raw, bool prevFF) : raw_(raw), prevFF_(prevFF) {}
  int get() override {
    int b = raw_.get();
    if (b < 0) return -1;
    if (prevFF_ && b == 0) {
      b = raw_.get();
      if (b < 0) {
        prevFF_ = false;
        return -1;
      }
    }
    prevFF_ = b == 0xFF;
    return b;
  }
  bool skip(uint32_t n) override {
    while (n--)
      if (get() < 0) return false;
    return true;
  }
  uint32_t left() const override { return raw_.left(); }
  uint32_t filePos() const override { return raw_.filePos(); }
  bool prevFF() const { return prevFF_; }

private:
  ByteIn& raw_;
  bool prevFF_;
};

class LimitIn final : public ByteIn {
public:
  LimitIn(ByteIn& in, uint32_t n) : in_(in), n_(n) {}
  int get() override {
    if (n_ == 0) return -1;
    const int b = in_.get();
    if (b < 0) {
      n_ = 0;
      return -1;
    }
    --n_;
    return b;
  }
  bool skip(uint32_t n) override {
    if (n > n_) {
      in_.skip(n_);
      n_ = 0;
      return false;
    }
    n_ -= n;
    return in_.skip(n);
  }
  uint32_t left() const override {
    const uint32_t l = in_.left();
    return l < n_ ? l : n_;
  }
  uint32_t filePos() const override { return in_.filePos(); }
  void settle() override {
    if (n_) in_.settle();
  }
  bool finish() { return skip(n_); }
  uint32_t limit() const { return n_; }

private:
  ByteIn& in_;
  uint32_t n_;
};

// One Ogg packet's bytes across pages (RFC 3533): page headers read and
// stepped over, bodies only where the packet's bytes are wanted.
class OggIn final : public ByteIn {
public:
  static constexpr uint32_t kProbe = 1024;  // a page header with its lacing (<= 282 bytes)
  OggIn(Cursor& c, uint32_t page, uint32_t serial, uint16_t maxPages, uint32_t* issues)
      : c_(c), next_(page), serial_(serial), maxPages_(maxPages), issues_(issues) {}
  bool start() { return nextPage(kStarts); }
  // Into the packet again at a byte of the page at `page` (its body's
  // `inBody`th byte), wherever in the packet that page is.
  bool resume(uint32_t page, uint32_t inBody) {
    next_ = page;
    if (!nextPage(kAny)) return false;
    if (inBody > end_ - pos_) return false;
    pos_ += inBody;
    return true;
  }
  // The page the next byte is in (after settle()), and that byte's offset
  // in the page's body.
  uint32_t page() const { return pageAt_; }
  uint32_t inBody() const { return pos_ - bodyAt_; }
  int get() override {
    while (pos_ >= end_)
      if (done_ || !nextPage(kContinues)) return -1;
    const uint8_t* p = c_.at(pos_, 1);
    if (!p) return -1;
    ++pos_;
    return *p;
  }
  bool skip(uint32_t n) override {
    while (n) {
      if (pos_ >= end_ && (done_ || !nextPage(kContinues))) return false;
      const uint32_t step = n < end_ - pos_ ? n : end_ - pos_;
      pos_ += step;
      n -= step;
    }
    return true;
  }
  uint32_t left() const override { return done_ ? end_ - pos_ : c_.size() - pos_; }
  uint32_t filePos() const override { return pos_; }
  void settle() override {
    while (pos_ >= end_ && !done_)
      if (!nextPage(kContinues)) return;
  }

private:
  enum Mode : uint8_t { kStarts, kContinues, kAny };
  bool nextPage(Mode mode) {
    if (pages_ >= maxPages_) {
      *issues_ |= kIssueTooMany;
      done_ = true;
      return false;
    }
    // A page header is a probe that counts its bytes, not a read: a picture
    // of 8 MB is about 130 of them on 64 KB pages (maxOggPages bounds them).
    const uint8_t* h = c_.at(next_, 27, kProbe, true);
    if (!h || std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) return fail();
    const uint32_t serial = le32(h + 14);
    if (serial != serial_) return fail();  // another stream's page: not followed
    const bool continued = (h[5] & 1) != 0;
    // The packet starts a page; its later pages continue it.
    if ((mode == kStarts && continued) || (mode == kContinues && !continued)) return fail();
    const uint8_t segs = h[26];
    const uint8_t* lace = segs ? c_.at(next_ + 27, segs, kProbe, true) : nullptr;
    if (segs && !lace) return fail();
    uint32_t body = 0, packet = 0;
    bool ends = false;
    for (uint8_t i = 0; i < segs; ++i) {
      body += lace[i];
      if (!ends) {
        packet += lace[i];
        if (lace[i] < 255) ends = true;
      }
    }
    pageAt_ = next_;
    pos_ = bodyAt_ = next_ + 27 + segs;
    end_ = pos_ + packet;
    next_ = pos_ + body;
    done_ = ends;
    ++pages_;
    if (end_ > c_.size()) {
      *issues_ |= kIssueTruncatedTag;
      end_ = c_.size();
      done_ = true;
    }
    return true;
  }
  bool fail() {
    done_ = true;
    return false;
  }
  Cursor& c_;
  uint32_t next_, serial_;
  uint16_t maxPages_;
  uint32_t* issues_;
  uint32_t pos_ = 0, end_ = 0, pageAt_ = 0, bodyAt_ = 0;
  uint16_t pages_ = 0;
  bool done_ = false;
};

int b64(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Base64 decoded on the fly (a picture's head: lofty's BASE64 is strict, so
// a character outside the alphabet ends it).
class B64In final : public ByteIn {
public:
  explicit B64In(ByteIn& in) : in_(in) {}
  int get() override {
    while (bits_ < 8) {
      const int c = in_.get();
      if (c < 0 || c == '=') return -1;
      const int v = b64(c);
      if (v < 0) return -1;
      acc_ = (acc_ << 6) | static_cast<uint32_t>(v);
      bits_ += 6;
    }
    bits_ -= 8;
    return static_cast<int>((acc_ >> bits_) & 0xFF);
  }
  bool skip(uint32_t n) override {
    while (n--)
      if (get() < 0) return false;
    return true;
  }
  uint32_t left() const override { return in_.left() / 4 * 3; }
  uint32_t filePos() const override { return in_.filePos(); }

private:
  ByteIn& in_;
  uint32_t acc_ = 0;
  int bits_ = 0;
};

// ===========================================================================
// Text: code points from an encoding (ID3v2's 0-3; Vorbis and APE are UTF-8,
// where lofty drops a value that isn't), then values.
// ===========================================================================
enum Enc : uint8_t { kLatin1 = 0, kUtf16 = 1, kUtf16Be = 2, kUtf8 = 3 };
enum Bom : uint8_t { kBomNone = 0, kBomLe = 1, kBomBe = 2 };

class TextIn {
public:
  // `bom`: for UTF-16 (1), the byte order when the text has no BOM of its
  // own (a TXXX value takes its description's).
  TextIn(ByteIn& in, uint8_t enc, uint8_t bom = kBomNone) : in_(in), enc_(enc > 3 ? static_cast<uint8_t>(kLatin1) : enc), hint_(bom) {}
  // A code point (0: a NUL), -1 at the end.
  int32_t next() {
    if (enc_ == kLatin1) return in_.get();
    if (enc_ == kUtf8) return utf8();
    return utf16();
  }
  // A sequence wasn't valid (UTF-8), or a surrogate or a byte was stray (UTF-16).
  bool invalid() const { return invalid_; }
  // The UTF-16 byte order found (kBomLe or kBomBe), for a TXXX's value.
  uint8_t order() const { return endian_ < 0 ? kBomNone : (endian_ ? kBomBe : kBomLe); }

private:
  // UTF-8 as Rust's from_utf8_lossy reads it: each maximal invalid subpart is
  // one U+FFFD.
  int32_t utf8() {
    for (;;) {
      int b = pushback_;
      pushback_ = -1;
      if (b < 0) b = in_.get();
      if (b < 0) {
        if (need_) {
          need_ = 0;
          invalid_ = true;
          return 0xFFFD;
        }
        return -1;
      }
      if (need_ == 0) {
        if (b < 0x80) return b;
        if (b >= 0xC2 && b <= 0xDF) {
          need_ = 1;
          cp_ = static_cast<uint32_t>(b & 0x1F);
          lo_ = 0x80;
          hi_ = 0xBF;
        } else if (b >= 0xE0 && b <= 0xEF) {
          need_ = 2;
          cp_ = static_cast<uint32_t>(b & 0x0F);
          lo_ = b == 0xE0 ? 0xA0 : 0x80;
          hi_ = b == 0xED ? 0x9F : 0xBF;
        } else if (b >= 0xF0 && b <= 0xF4) {
          need_ = 3;
          cp_ = static_cast<uint32_t>(b & 0x07);
          lo_ = b == 0xF0 ? 0x90 : 0x80;
          hi_ = b == 0xF4 ? 0x8F : 0xBF;
        } else {
          invalid_ = true;
          return 0xFFFD;
        }
        continue;
      }
      if (b >= lo_ && b <= hi_) {
        cp_ = (cp_ << 6) | static_cast<uint32_t>(b & 0x3F);
        lo_ = 0x80;
        hi_ = 0xBF;
        if (--need_ == 0) return static_cast<int32_t>(cp_);
        continue;
      }
      need_ = 0;
      invalid_ = true;
      pushback_ = b;
      return 0xFFFD;
    }
  }

  // UTF-16 as lofty decodes it (after the repair pass): the byte order from
  // the first pair when it is a BOM (else the hint; else little-endian, where
  // lofty fails the file), every FF FE or FE FF pair dropped wherever it is;
  // an unpaired surrogate and an odd last byte become U+FFFD.
  int32_t utf16() {
    for (;;) {
      if (pending_ >= 0) {
        const int32_t cp = pending_;
        pending_ = -1;
        return cp;
      }
      if (done_) {
        if (high_) {
          high_ = 0;
          invalid_ = true;
          return 0xFFFD;
        }
        if (stray_) {
          stray_ = false;
          invalid_ = true;
          return 0xFFFD;
        }
        return -1;
      }
      const int a = in_.get();
      if (a < 0) {
        done_ = true;
        continue;
      }
      const int b = in_.get();
      if (b < 0) {
        done_ = true;
        stray_ = true;
        continue;
      }
      if (endian_ < 0) {
        if (enc_ == kUtf16Be) endian_ = 1;
        else if (a == 0xFF && b == 0xFE) endian_ = 0;
        else if (a == 0xFE && b == 0xFF) endian_ = 1;
        else endian_ = hint_ == kBomBe ? 1 : 0;
      }
      if ((a == 0xFF && b == 0xFE) || (a == 0xFE && b == 0xFF)) continue;
      const uint32_t u = endian_ ? (static_cast<uint32_t>(a) << 8 | static_cast<uint32_t>(b))
                                 : (static_cast<uint32_t>(b) << 8 | static_cast<uint32_t>(a));
      if (high_) {
        if (u >= 0xDC00 && u <= 0xDFFF) {
          const uint32_t cp = 0x10000 + ((high_ - 0xD800) << 10) + (u - 0xDC00);
          high_ = 0;
          return static_cast<int32_t>(cp);
        }
        high_ = 0;
        invalid_ = true;
        if (u >= 0xD800 && u <= 0xDBFF) {
          high_ = u;
          return 0xFFFD;
        }
        pending_ = (u >= 0xDC00 && u <= 0xDFFF) ? 0xFFFD : static_cast<int32_t>(u);
        return 0xFFFD;
      }
      if (u >= 0xD800 && u <= 0xDBFF) {
        high_ = u;
        continue;
      }
      if (u >= 0xDC00 && u <= 0xDFFF) {
        invalid_ = true;
        return 0xFFFD;
      }
      return static_cast<int32_t>(u);
    }
  }

  ByteIn& in_;
  uint8_t enc_;
  uint8_t hint_;
  bool invalid_ = false;
  // UTF-8
  int pushback_ = -1;
  int need_ = 0;
  uint32_t cp_ = 0;
  int lo_ = 0x80, hi_ = 0xBF;
  // UTF-16
  int endian_ = -1;
  uint32_t high_ = 0;
  int32_t pending_ = -1;
  bool stray_ = false;
  bool done_ = false;
};

// One code point into a value (UTF-8), whole or not at all; past the buffer
// the value is `longer` and keeps what it has.
void put(Value& v, uint32_t cp) {
  if (v.longer) return;
  uint8_t b[4];
  const uint32_t n = encodeUtf8(cp, b);
  if (v.len + n > Scanner::kValueBytes) {
    v.longer = true;
    return;
  }
  std::memcpy(v.buf + v.len, b, n);
  v.len = static_cast<uint16_t>(v.len + n);
}

void clearValue(Value& v) {
  v.len = 0;
  v.longer = false;
}

// One NUL-separated value (an ID3v2 text frame's): true when a NUL ended it
// (more may follow).
bool readValue(TextIn& t, Value& v) {
  clearValue(v);
  for (;;) {
    const int32_t cp = t.next();
    if (cp < 0) return false;
    if (cp == 0) return true;
    put(v, static_cast<uint32_t>(cp));
  }
}

// The whole text, NULs kept as bytes, the trailing ones dropped (lofty's
// trim_end_nulls).
void readWhole(TextIn& t, Value& v) {
  clearValue(v);
  uint32_t nuls = 0;
  for (;;) {
    const int32_t cp = t.next();
    if (cp < 0) break;
    if (cp == 0) {
      ++nuls;
      continue;
    }
    for (; nuls; --nuls) put(v, 0);
    put(v, static_cast<uint32_t>(cp));
  }
}

// Any code point other than NUL (lofty's frame is_empty(): the decoded value
// empty once its trailing NULs go).
bool anyText(TextIn& t) {
  for (;;) {
    const int32_t cp = t.next();
    if (cp < 0) return false;
    if (cp != 0) return true;
  }
}

// ===========================================================================
// Pictures: the election (5.3), the first front cover, else the first.
// ===========================================================================
struct Pic {
  uint32_t offset = 0, length = 0;
  uint8_t type = 0, mime = 0, coding = 0;
  bool set = false;
};
struct PicVote {
  Pic front, first;
  void add(const Pic& p) {
    if (!first.set) first = p;
    if (p.type == 3 && !front.set) front = p;
  }
  void clear() { front = first = Pic(); }
};
// Over two lists in order (a FLAC's comment pictures, then its blocks').
Pic elect(const PicVote& a, const PicVote& b) {
  if (a.front.set) return a.front;
  if (b.front.set) return b.front;
  if (a.first.set) return a.first;
  return b.first;
}

// ===========================================================================
// The ID3v2 frames a record field takes a value from.
// ===========================================================================
enum Key : uint8_t {
  kKeyNone = 0,
  // One frame per id survives in lofty's list (text frames compare by id).
  kTIT2, kTPE1, kTALB, kTPE2, kTCON, kTCOM, kTSOT, kTSOP, kTSOA, kTSO2, kTRCK, kTPOS, kTCMP, kTBPM, kTKEY,
  kTDAT, kTIME,  // v2.3's date and time, for its TYER (lofty's construct_tdrc_from_v3)
  kUFID,         // the MusicBrainz owner's: the recording id
  // Several may: a timestamp compares by its value, a TXXX by its
  // description, a picture never.
  kTDRC,
  kRgTrackGain, kRgTrackPeak, kRgAlbumGain, kRgAlbumPeak, kMbAlbum, kAlbumArtistTxxx,
  kAPIC,
  // Frames kept for their place in the tag's list only (they can replace
  // one another, which moves the frames after them): other text frames (by
  // id), other timestamps, other TXXX frames (by description), other UFID
  // frames (by owner).
  kPosText, kPosTimestamp, kPosTxxx, kPosUfid,
};
bool isTxxxKey(uint8_t k) { return k >= kRgTrackGain && k <= kAlbumArtistTxxx; }

// How a FrameRef's content is stored.
constexpr uint8_t kModeUnsync = 1;   // read through an UnsyncIn
constexpr uint8_t kModeLogical = 2;  // `limit` counts bytes after re-synchronising (tag-level)
constexpr uint8_t kModePrevFF = 4;   // the UnsyncIn starts after an FF

// A timestamp FrameRef's aux.
constexpr uint8_t kTsVerified = 1;
constexpr uint8_t kTsMonth = 2;

struct IdMap {
  char id[5];
  uint8_t key;
};
// The frames whose id gives a key (v2.3 and v2.4 ids; v2.2's are upgraded
// first, as lofty does).
const IdMap kIds[] = {
    {"TIT2", kTIT2}, {"TPE1", kTPE1}, {"TALB", kTALB}, {"TPE2", kTPE2}, {"TCON", kTCON}, {"TCOM", kTCOM},
    {"TSOT", kTSOT}, {"TSOP", kTSOP}, {"TSOA", kTSOA}, {"TSO2", kTSO2}, {"TRCK", kTRCK}, {"TPOS", kTPOS},
    {"TCMP", kTCMP}, {"TBPM", kTBPM}, {"TKEY", kTKEY}, {"TDRC", kTDRC},
};
// lofty's upgrade_v2 (id3/v2/util/upgrade.rs), whole: a v2.2 id (or a v2.3
// tag's 3-character one) is its v2.4 id for keys, equality and parsing.
const char* const kV22[][2] = {
    {"BUF", "RBUF"}, {"CNT", "PCNT"}, {"COM", "COMM"}, {"CRA", "AENC"}, {"ETC", "ETCO"}, {"GEO", "GEOB"},
    {"IPL", "TIPL"}, {"MCI", "MCDI"}, {"MLL", "MLLT"}, {"PIC", "APIC"}, {"POP", "POPM"}, {"REV", "RVRB"},
    {"SLT", "SYLT"}, {"STC", "SYTC"}, {"TAL", "TALB"}, {"TBP", "TBPM"}, {"TCM", "TCOM"}, {"TCO", "TCON"},
    {"TCP", "TCMP"}, {"TCR", "TCOP"}, {"TDY", "TDLY"}, {"TEN", "TENC"}, {"TFT", "TFLT"}, {"TKE", "TKEY"},
    {"TLA", "TLAN"}, {"TLE", "TLEN"}, {"TMT", "TMED"}, {"TOA", "TOAL"}, {"TOF", "TOFN"}, {"TOL", "TOLY"},
    {"TOR", "TDOR"}, {"TOT", "TOAL"}, {"TP1", "TPE1"}, {"TP2", "TPE2"}, {"TP3", "TPE3"}, {"TP4", "TPE4"},
    {"TPA", "TPOS"}, {"TPB", "TPUB"}, {"TRC", "TSRC"}, {"TRD", "TDRC"}, {"TRK", "TRCK"}, {"TS2", "TSO2"},
    {"TSA", "TSOA"}, {"TSC", "TSOC"}, {"TSP", "TSOP"}, {"TSS", "TSSE"}, {"TST", "TSOT"}, {"TT1", "TIT1"},
    {"TT2", "TIT2"}, {"TT3", "TIT3"}, {"TXT", "TOLY"}, {"TXX", "TXXX"}, {"TYE", "TDRC"}, {"UFI", "UFID"},
    {"ULT", "USLT"}, {"WAF", "WOAF"}, {"WAR", "WOAR"}, {"WAS", "WOAS"}, {"WCM", "WCOM"}, {"WCP", "WCOP"},
    {"WPB", "WPUB"}, {"WXX", "WXXX"}, {"PCS", "PCST"}, {"TCT", "TCAT"}, {"TDS", "TDES"}, {"TID", "TGID"},
    {"WFD", "WFED"}, {"MVI", "MVIN"}, {"MVN", "MVNM"}, {"GP1", "GRP1"}, {"TDR", "TDRL"},
};

bool idChar(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

uint32_t timestampEq(uint8_t enc, const tagrules::Timestamp& ts) {
  uint32_t h = 2166136261u;
  const uint32_t parts[8] = {enc, ts.year, ts.fields, ts.month, ts.day, ts.hour, ts.minute, ts.second};
  for (uint32_t p : parts) h = (h ^ p) * 16777619u;
  return h;
}

}  // namespace

// ===========================================================================
// The scan's state.
// ===========================================================================
struct ScanCtx {
  Scanner& s;
  Record& r;
  Cursor& c;
  const Limits& lim;
  uint32_t& issues;
  Kind kind;

  // ---- ID3v2 ----
  bool id3 = false;        // an ID3v2 tag was read
  uint8_t tags = 0;        // tags in the chain so far
  uint32_t uniq = 0;       // pictures never compare equal
  uint32_t regionEnd = 0;  // the tag being walked: where its frames end
  uint16_t tagN = 0;       // the frames in its list so far (lofty's FrameList)
  uint8_t tagVer = 0;      // its major version

  // ---- the tail ----
  bool hasV1 = false;
  bool hasApe = false;
  uint32_t apeItems = 0, apeCount = 0, apeEnd = 0;

  // ---- what the chosen tag said ----
  bool haveYear = false;   // a year item, an explicit 0 included (the ID3v1 fill)
  bool haveTrack = false;  // a track number, the same
  uint8_t compilation = 0;
  Pic picture;

  // ---- Vorbis comments and APE: each numeric key's first item ----
  struct Num {
    bool seen = false;
    bool longer = false;
    uint8_t len = 0;
    char text[64];
  };
  Num bpm, key, comp, rgTG, rgTP, rgAG, rgAP, r128T, r128A, year, date, trackNum, trackTot, discNum, discTot;
  PicVote commentPics, blockPics, apePics;
  bool vorbisTag = false;     // FLAC: a comment block was read
  bool blockPicSeen = false;  // FLAC: a PICTURE block was read
  uint16_t itemN = 0;         // the comments in lofty's item list so far
  bool itemsShed = false;     // a full table had nothing to let go of, and nothing changed since
  uint32_t serial = 0;        // Opus: the stream's

  // An MP3's first frame, read for its length: its Xing/Info, VBRI and LAME
  // fields lie in its first 194 bytes, and the smallest buffer holds 512.
  static constexpr uint32_t kFrameWindow = 512;

  ScanCtx(Scanner& sc, Cursor& cur, const Limits& l, Kind k)
      : s(sc), r(sc.rec_), c(cur), lim(l), issues(sc.issues_), kind(k) {}

  Value& value() { return s.value_; }
  Value& spare() { return s.spare_; }

  // -------------------------------------------------------------------------
  // The frame table: lofty's FrameList, the frames a field may come from
  // and what it takes to know their order. While a tag is read, each frame
  // has its place in that tag's list (every frame lofty keeps counts, kept
  // here or not); at the tag's end its frames join the merged list in their
  // order (seq), each replacing an equal frame of an earlier tag.
  // -------------------------------------------------------------------------
  bool add(const FrameRef& f) {
    if (s.nFrames_ == Scanner::kMaxFrames) {
      uint32_t w = 0;
      for (uint32_t i = 0; i < s.nFrames_; ++i)
        if (s.frames_[i].live) s.frames_[w++] = s.frames_[i];
      s.nFrames_ = w;
      if (w == Scanner::kMaxFrames) {
        issues |= kIssueTooMany;
        if (f.key >= kPosText) return false;  // its place still counts
        // A frame a field comes from takes the entry of one kept for its
        // place only (v2.3's, the earliest): that frame can't be seen
        // replaced any more, which only v2.3's date removal would read.
        FrameRef* out = nullptr;
        for (uint32_t i = 0; i < s.nFrames_; ++i) {
          FrameRef& e = s.frames_[i];
          if (e.key >= kPosText && (!out || e.place < out->place)) out = &e;
        }
        if (!out) return false;
        *out = f;
        return true;
      }
    }
    s.frames_[s.nFrames_++] = f;
    return true;
  }
  // The frames after place p of tag t move up one (Vec::remove).
  void shiftAfter(uint8_t t, uint16_t p) {
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      FrameRef& e = s.frames_[i];
      if (e.live && e.tag == t && e.place > p) --e.place;
    }
  }
  // lofty's FrameList::insert within the tag being read: a frame equal to
  // one in the list (same key and eq) replaces it and goes last; an empty
  // frame doesn't replace a non-empty one, which goes last instead.
  void insertInTag(FrameRef f) {
    // A frame kept for its place only matters to v2.3's date removal, which
    // reads places as they are: elsewhere only the order of the frames a
    // field comes from counts, which such a frame's replacement (the frames
    // after it moving up one) doesn't change. So outside v2.3 its place is
    // counted and nothing kept.
    if (f.key >= kPosText && tagVer != 3) {
      ++tagN;
      return;
    }
    const bool emptiness = f.key != kTDRC && f.key != kPosTimestamp;  // a timestamp has no is_empty()
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      FrameRef& e = s.frames_[i];
      if (!e.live || e.tag != f.tag || e.key != f.key || e.eq != f.eq) continue;
      if (emptiness && f.empty && !e.empty) {
        shiftAfter(f.tag, e.place);
        e.place = static_cast<uint16_t>(tagN - 1);
        return;
      }
      shiftAfter(f.tag, e.place);
      e.live = 0;
      --tagN;
      break;
    }
    f.place = tagN++;
    f.seq = 0;
    f.live = 1;
    add(f);
  }
  // lofty's FrameList::remove(id), a swap partition: the matching frames go
  // to the front one by one, each trading places with the frame there, and
  // are drained, so the frames left can change order.
  void swapRemove(uint8_t t, uint8_t k) {
    uint16_t places[Scanner::kMaxFrames];
    uint32_t n = 0;
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      const FrameRef& e = s.frames_[i];
      if (e.live && e.tag == t && e.key == k) places[n++] = e.place;
    }
    for (uint32_t a = 1; a < n; ++a)
      for (uint32_t b = a; b > 0 && places[b - 1] > places[b]; --b) {
        const uint16_t x = places[b];
        places[b] = places[b - 1];
        places[b - 1] = x;
      }
    for (uint32_t j = 0; j < n; ++j) {
      if (places[j] == j) continue;
      for (uint32_t i = 0; i < s.nFrames_; ++i) {
        FrameRef& e = s.frames_[i];
        if (e.live && e.tag == t && e.key != k && e.place == j) {
          e.place = places[j];
          break;
        }
      }
    }
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      FrameRef& e = s.frames_[i];
      if (!e.live || e.tag != t) continue;
      if (e.key == k) e.live = 0;
      else e.place = static_cast<uint16_t>(e.place - n);
    }
    tagN = static_cast<uint16_t>(tagN - n);
  }
  // The end of tag t: its frames, in their order, into the merged list
  // (lofty's existing_tag.insert(frame): an equal frame of an earlier tag
  // goes, and no empty-frame rule).
  void mergeTag(uint8_t t) {
    for (;;) {
      FrameRef* next = nullptr;
      for (uint32_t i = 0; i < s.nFrames_; ++i) {
        FrameRef& e = s.frames_[i];
        if (e.live && e.tag == t && e.seq == 0 && (!next || e.place < next->place)) next = &e;
      }
      if (!next) break;
      if (next->key >= kPosText) {  // there for its place only
        next->live = 0;
        continue;
      }
      for (uint32_t i = 0; i < s.nFrames_; ++i) {
        FrameRef& o = s.frames_[i];
        if (o.live && o.tag != t && o.key == next->key && o.eq == next->eq) o.live = 0;
      }
      next->seq = ++s.seq_;
    }
  }

  // The live frame of a key next in the merged list after `after`: nullptr
  // when none.
  const FrameRef* nextOf(uint8_t k, uint16_t after) const {
    const FrameRef* best = nullptr;
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      const FrameRef& e = s.frames_[i];
      if (!e.live || e.key != k || e.seq <= after) continue;
      if (!best || e.seq < best->seq) best = &e;
    }
    return best;
  }
  const FrameRef* firstOf(uint8_t k) const { return nextOf(k, 0); }

  // A FrameRef's content again, as a stream (`in`).
  struct Reopen {
    PlainIn raw;
    UnsyncIn un;
    LimitIn lim;
    LimitIn in;
    Reopen(Cursor& c, const FrameRef& f, uint32_t cap)
        : raw(c, f.at, f.end), un(raw, (f.mode & kModePrevFF) != 0),
          lim((f.mode & kModeUnsync) ? static_cast<ByteIn&>(un) : static_cast<ByteIn&>(raw), f.limit),
          in(lim, cap) {}
  };

  // -------------------------------------------------------------------------
  // ID3v2
  // -------------------------------------------------------------------------
  struct Id3Header {
    uint32_t start = 0;
    uint32_t size = 0;   // the body's, from the syncsafe bytes (their high bits ignored, as lofty does)
    uint32_t total = 0;  // header, body and footer
    uint8_t ver = 0;
    uint8_t flags = 0;
  };

  // The header at `off`: false when there is none ("ID3", a major version 2
  // to 4, not v2.2's compression), as lofty's Id3v2Header::parse decides.
  bool id3Header(uint32_t off, Id3Header* h) {
    const uint8_t* p = c.at(off, 10);
    if (!p || p[0] != 'I' || p[1] != 'D' || p[2] != '3') return false;
    if (p[3] < 2 || p[3] > 4 || (p[3] == 2 && (p[5] & 0x40))) {
      issues |= kIssueLofty;  // lofty fails the file
      return false;
    }
    h->start = off;
    h->ver = p[3];
    h->flags = p[5];
    h->size = syncsafe(p + 6);
    const bool footer = h->ver >= 3 && (h->flags & 0x10);
    h->total = 10 + h->size + (footer ? 10 : 0);
    return true;
  }

  // mStream's repair pass (rule 6): `at` is where a frame may start in the
  // tag body ending at `end`: the end itself, padding, or a frame id.
  bool boundaryOk(uint32_t end, uint64_t at) {
    if (at == end) return true;
    if (at > end) return false;
    const uint32_t a = static_cast<uint32_t>(at);
    const int b = c.byte(a);
    if (b == 0) return true;
    if (b < 0 || static_cast<uint64_t>(a) + 4 > end) return false;
    const uint8_t* p = c.at(a, 4);
    return p && idChar(p[0]) && idChar(p[1]) && idChar(p[2]) && idChar(p[3]);
  }

  // One tag's frames into the table. `repairs`: mStream's repair pass reads
  // this tag first (the one at offset 0, without an extended header).
  void walkId3(const Id3Header& h, bool repairs) {
    const uint8_t t = tags++;
    id3 = true;
    tagN = 0;
    const uint8_t ver = h.ver;
    tagVer = ver;
    const uint32_t bodyStart = h.start + 10;
    const uint64_t bodyEnd64 = static_cast<uint64_t>(bodyStart) + h.size;
    regionEnd = bodyEnd64 < c.size() ? static_cast<uint32_t>(bodyEnd64) : c.size();
    if (bodyEnd64 > c.size()) issues |= kIssueTruncatedTag;
    if (h.size == 0) return;
    uint32_t pos = bodyStart;
    const bool wholeUnsync = (h.flags & 0x80) && ver < 4;
    // The extended header, as lofty reads it: the size, two bytes it takes
    // for a flag count and v2.4's flags, then the CRC and restrictions they
    // name. In v2.3 that leaves the 4-byte padding size, whose first byte is
    // 0: lofty reads no frame from such a tag, and mStream shows it untagged.
    if (ver >= 3 && (h.flags & 0x40)) {
      issues |= kIssueExtendedHeader;
      repairs = false;
      if (ver == 3 && wholeUnsync) {
        issues |= kIssueLofty;  // lofty's buffered reader swallows the tag
        return;
      }
      const uint8_t* e = c.at(pos, 6);
      if (!e) return;
      const uint32_t ext = ver == 3 ? be32(e) : syncsafe(e);
      if (ext < 6 || ext >= h.size) {
        issues |= kIssueLofty;
        return;
      }
      const uint8_t f2 = e[5];
      pos += 6;
      if (f2 & 0x20) pos += 6;
      if (f2 & 0x10) pos += 2;
      const uint64_t end = static_cast<uint64_t>(pos) + (h.size - ext);
      regionEnd = end < c.size() ? static_cast<uint32_t>(end) : c.size();
    }
    // A v2.2/2.3 tag under tag-level unsynchronisation is read through: a
    // frame's size counts its bytes resynchronised, so where the next frame
    // starts is known only by reading through this one (a picture's
    // included, as lofty reads the whole tag). Its bytes come on top of the
    // budget (Limits::maxUnsyncTag), as sequential reads of the buffer.
    if (wholeUnsync && regionEnd > pos) c.extend(regionEnd - pos);
    PlainIn raw(c, pos, regionEnd);
    UnsyncIn un(raw, false);
    ByteIn& in = wholeUnsync ? static_cast<ByteIn&>(un) : static_cast<ByteIn&>(raw);
    const uint32_t hdr = ver == 2 ? 6 : 10;
    for (uint32_t i = 0;; ++i) {
      if (i >= lim.maxFrames) {
        issues |= kIssueTooMany;
        break;
      }
      if (c.failed()) break;
      const uint32_t framePos = in.filePos();
      uint8_t fh[10];
      if (!in.getN(fh, hdr)) break;
      if (fh[0] == 0) break;  // padding
      char id[5] = {0, 0, 0, 0, 0};
      uint32_t size = 0;
      uint16_t fflags = 0;
      bool idOk = true;
      if (ver == 2) {
        for (int k = 0; k < 3; ++k) idOk = idOk && idChar(fh[k]);
        std::memcpy(id, fh, 3);
        size = (static_cast<uint32_t>(fh[3]) << 16) | (static_cast<uint32_t>(fh[4]) << 8) | fh[5];
      } else {
        // lofty trims trailing NULs from an id; a v2.3 tag may carry a v2.2
        // id padded with one.
        int n = 4;
        while (n > 0 && fh[n - 1] == 0) --n;
        if (ver == 3 && fh[3] == 0) n = n < 3 ? n : 3;
        for (int k = 0; k < n; ++k) idOk = idOk && idChar(fh[k]);
        if (n < 3) idOk = false;
        std::memcpy(id, fh, static_cast<size_t>(n));
        fflags = static_cast<uint16_t>((fh[8] << 8) | fh[9]);
        if (ver == 3) {
          size = be32(fh + 4);
        } else {
          const uint32_t ss = syncsafe(fh + 4), plain = be32(fh + 4);
          size = ss;
          if (repairs && plain != ss) {
            const bool allLow = !((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80);
            const uint64_t at = static_cast<uint64_t>(framePos) + 10;
            const bool okSs = allLow && boundaryOk(regionEnd, at + ss);
            const bool okBe = boundaryOk(regionEnd, at + plain);
            if (!okSs && okBe) {
              size = plain;
              issues |= kIssueNonSyncsafe | kIssueRepaired;
            }
          }
        }
      }
      // Upgrades: v2.2's ids (and v2.3's 3-character ones), v2.3's TYER.
      if (std::strlen(id) == 3 && ver <= 3) {
        for (const auto& m : kV22)
          if (std::memcmp(id, m[0], 3) == 0) {
            std::memcpy(id, m[1], 5);
            break;
          }
      } else if (ver == 3 && std::memcmp(id, "TYER", 5) == 0) {
        std::memcpy(id, "TDRC", 5);
      } else if (ver == 3 && std::memcmp(id, "TORY", 5) == 0) {
        std::memcpy(id, "TDOR", 5);
      } else if (ver == 3 && std::memcmp(id, "IPLS", 5) == 0) {
        std::memcpy(id, "TIPL", 5);
      }
      if (!idOk) {
        // lofty (relaxed): a frame whose header it can't read is skipped by
        // its declared size.
        issues |= kIssueBadFrame;
        if (!in.skip(size)) break;
        continue;
      }
      if (size == 0) continue;
      const bool truncated = !wholeUnsync && size > in.left();
      if (truncated) issues |= kIssueTruncatedTag;
      LimitIn frame(in, size);
      bool compressed = false, encrypted = false, unsync = false, grouping = false, dli = false;
      if (ver == 3) {
        compressed = fflags & 0x0080;
        encrypted = fflags & 0x0040;
        grouping = fflags & 0x0020;
      } else if (ver == 4) {
        grouping = fflags & 0x0040;
        compressed = fflags & 0x0008;
        encrypted = fflags & 0x0004;
        unsync = (fflags & 0x0002) || (h.flags & 0x80);
        dli = fflags & 0x0001;
      }
      const uint32_t lead = (encrypted ? 1 : 0) + (grouping ? 1 : 0) + ((dli || compressed) ? 4 : 0);
      uint8_t k = kKeyNone;
      for (const IdMap& m : kIds)
        if (std::strcmp(id, m.id) == 0) k = m.key;
      if (ver == 3 && std::strcmp(id, "TDAT") == 0) k = kTDAT;
      if (ver == 3 && std::strcmp(id, "TIME") == 0) k = kTIME;
      const bool isTxxx = std::strcmp(id, "TXXX") == 0;
      const bool isApic = std::strcmp(id, "APIC") == 0;
      const bool isUfid = std::strcmp(id, "UFID") == 0;
      const bool isUrl = id[0] == 'W' && std::strcmp(id, "WXXX") != 0;
      const bool wanted = k != kKeyNone || isTxxx || isApic || isUfid;
      // The text frames lofty compares by id (and its other timestamps):
      // not wanted, but kept for their places.
      const bool textLike = (id[0] == 'T' && std::strcmp(id, "TIPL") != 0 && std::strcmp(id, "TMCL") != 0) ||
                            std::strcmp(id, "GRP1") == 0 || std::strcmp(id, "MVNM") == 0 ||
                            std::strcmp(id, "MVIN") == 0;
      if (lead > size) {
        issues |= kIssueLofty;  // lofty: an undersized frame fails the tag
        frame.finish();
        continue;
      }
      if (compressed || encrypted) {
        // lofty keeps it (inflated, or as a binary frame): a place here, and
        // no value (5.3: never an elected picture).
        if (wanted) issues |= compressed ? kIssueCompressedFrame : kIssueEncryptedFrame;
        if (isApic) issues |= kIssueSkippedPicture;
        ++tagN;
        frame.finish();
      } else if (wanted || textLike) {
        frame.skip(lead);
        if (unsync) {
          UnsyncIn fu(frame, false);
          readFrame(id, k, isTxxx, isApic, isUfid, fu, &fu, t, ver, frame, nullptr);
        } else {
          readFrame(id, k, isTxxx, isApic, isUfid, frame, nullptr, t, ver, frame, wholeUnsync ? &un : nullptr);
        }
        frame.finish();
      } else if (isUrl && !unsync) {
        // lofty reads a URL to its first NUL and leaves the rest of the frame
        // unread: the next header is read from there. The repair pass drops
        // a NUL in front of the URL first. A URL of no bytes isn't kept.
        frame.skip(lead);
        const uint32_t content = frame.left();
        bool first = true, nul = false, any = false;
        for (;;) {
          const int b = frame.get();
          if (b < 0) break;
          if (first && b == 0 && repairs && content > 1) {
            first = false;
            issues |= kIssueRepaired;
            continue;
          }
          first = false;
          any = true;
          if (b == 0) {
            nul = true;
            break;
          }
        }
        if (any) ++tagN;
        if (!nul) frame.finish();
      } else {
        ++tagN;
        frame.finish();
      }
      if (truncated && repairs) break;  // the repair pass cuts the frame there and stops
    }
    // The tag's end. When the budget stopped the walk, the frames located
    // stand (the one it stopped in doesn't: its entry is only made once it
    // is read), and v2.3's two date frames are read in the reserve.
    const bool walking = c.walking();
    c.walk(false);
    if (ver == 3) v23Date(t);
    mergeTag(t);
    c.walk(walking);
  }

  // Where a frame's content continues, for its FrameRef: never past the
  // tag (a frame running past it is cut there, as lofty's reader of the tag
  // cuts it).
  void locate(FrameRef* f, const ByteIn& in, const UnsyncIn* frameUnsync, const LimitIn& frame,
              const UnsyncIn* tagUnsync) const {
    f->at = in.filePos();
    if (tagUnsync) {
      f->mode = kModeUnsync | kModeLogical | (tagUnsync->prevFF() ? kModePrevFF : 0);
      f->end = regionEnd;  // the logical limit ends it first, unless the tag does
      f->limit = frame.limit();
    } else if (frameUnsync) {
      f->mode = kModeUnsync | (frameUnsync->prevFF() ? kModePrevFF : 0);
      f->end = frame.filePos() + frame.left();
      f->limit = 0xFFFFFFFFu;
    } else {
      f->mode = 0;
      f->end = frame.filePos() + frame.left();
      f->limit = f->end - f->at;
    }
  }

  static uint32_t idHash(const char* id) { return static_cast<uint32_t>(cc::fnv1a64Str(id)); }
  static bool timestampId(const char* id) {
    return std::strcmp(id, "TDEN") == 0 || std::strcmp(id, "TDOR") == 0 || std::strcmp(id, "TDRL") == 0 ||
           std::strcmp(id, "TDTG") == 0;
  }

  // One frame lofty would parse, into the tag's list when lofty keeps it:
  // its entry (located, its emptiness and equality known), or only its
  // place.
  void readFrame(const char* id, uint8_t k, bool isTxxx, bool isApic, bool isUfid, ByteIn& in, const UnsyncIn* fu,
                 uint8_t tag, uint8_t ver, LimitIn& frame, const UnsyncIn* tagUnsync) {
    FrameRef f;
    std::memset(&f, 0, sizeof(f));
    f.tag = tag;
    if (isApic) {
      readApic(in, fu, frame, tagUnsync, &f, ver);
      return;
    }
    if (isUfid) {
      // The owner, ISO-8859-1 to its NUL (or the frame's end): only
      // MusicBrainz's gives the recording id; another is kept by its owner.
      static const char kOwner[] = "http://musicbrainz.org";
      char owner[sizeof(kOwner)];
      size_t n = 0;
      uint64_t hash = cc::kFnvBasis;
      int b;
      while ((b = in.get()) > 0) {
        if (n < sizeof(kOwner) - 1) owner[n] = static_cast<char>(b);
        const uint8_t byte = static_cast<uint8_t>(b);
        hash = cc::fnv1a64(&byte, 1, hash);
        ++n;
      }
      if (n == 0) issues |= kIssueLofty;  // lofty: an empty owner fails the tag
      const bool mb = n == sizeof(kOwner) - 1 && std::memcmp(owner, kOwner, n) == 0;
      f.key = mb ? kUFID : kPosUfid;
      f.eq = mb ? 0 : static_cast<uint32_t>(hash ^ (hash >> 32));
      locate(&f, in, fu, frame, tagUnsync);
      f.empty = in.get() < 0;
      if (c.failed()) return;  // stopped inside it (the budget): not read whole
      insertInTag(f);
      return;
    }
    const int e = in.get();
    if (e < 0) return;  // no encoding byte: lofty keeps no frame
    const uint8_t enc = static_cast<uint8_t>(e > 3 ? 0 : e);
    f.enc = enc;
    if (k == kTDRC || timestampId(id)) {
      // lofty's TimestampFrame: the whole text through its timestamp parse;
      // one that doesn't parse (or whose encoding is bad) isn't kept, one
      // that fails verify() is kept without a year.
      if (e > 3) return;
      f.key = k == kTDRC ? kTDRC : kPosTimestamp;
      locate(&f, in, fu, frame, tagUnsync);
      Value& v = value();
      LimitIn capped(in, lim.maxText);
      TextIn t(capped, enc);
      readWhole(t, v);
      // The parse reads at most 19 bytes after the leading ASCII
      // whitespace: a text longer than the value holds parses as its start
      // does, unless that whitespace runs past what is held.
      if (v.longer) {
        size_t ws = 0;
        while (ws < v.len && (v.buf[ws] == ' ' || v.buf[ws] == '\t' || v.buf[ws] == '\n' || v.buf[ws] == '\f' ||
                              v.buf[ws] == '\r'))
          ++ws;
        if (ws + 19 > v.len) return;
      }
      tagrules::Timestamp ts;
      if (tagrules::parseTimestamp(v.buf, v.len, &ts) != tagrules::TsParse::Ok) return;
      f.year = ts.year;
      f.aux = static_cast<uint8_t>((tagrules::verifyTimestamp(ts) ? kTsVerified : 0) | (ts.fields ? kTsMonth : 0));
      f.eq = timestampEq(enc, ts) ^ (k == kTDRC ? 0u : idHash(id));
      if (c.failed()) return;
      insertInTag(f);
      return;
    }
    if (e > 3 || (ver == 2 && e > 1)) issues |= kIssueLofty;  // lofty: a bad encoding fails the tag
    if (isTxxx) {
      // The description, to its terminator: it names the key and is what two
      // TXXX frames compare by.
      Value& v = value();
      uint64_t hash = cc::kFnvBasis;
      TextIn d(in, enc);
      clearValue(v);
      for (;;) {
        const int32_t cp = d.next();
        if (cp <= 0) break;
        uint8_t b[4];
        hash = cc::fnv1a64(b, encodeUtf8(static_cast<uint32_t>(cp), b), hash);
        put(v, static_cast<uint32_t>(cp));
      }
      const size_t n = v.longer ? 0 : v.len;
      uint8_t key = kPosTxxx;
      if (asciiIEq(v.buf, n, "REPLAYGAIN_TRACK_GAIN")) key = kRgTrackGain;
      else if (asciiIEq(v.buf, n, "REPLAYGAIN_TRACK_PEAK")) key = kRgTrackPeak;
      else if (asciiIEq(v.buf, n, "REPLAYGAIN_ALBUM_GAIN")) key = kRgAlbumGain;
      else if (asciiIEq(v.buf, n, "REPLAYGAIN_ALBUM_PEAK")) key = kRgAlbumPeak;
      else if (asciiIEq(v.buf, n, "MusicBrainz Album Id")) key = kMbAlbum;
      else if (asciiIEq(v.buf, n, "ALBUMARTIST") || asciiIEq(v.buf, n, "ALBUM ARTIST")) key = kAlbumArtistTxxx;
      f.key = key;
      f.eq = static_cast<uint32_t>(hash ^ (hash >> 32));
      f.aux = d.order();  // a UTF-16 value without a BOM takes the description's
      locate(&f, in, fu, frame, tagUnsync);
      LimitIn capped(in, lim.maxText);
      TextIn content(capped, enc, f.aux);
      f.empty = !anyText(content);
      if (c.failed()) return;
      insertInTag(f);
      return;
    }
    // A text frame (compared by its id).
    f.key = k != kKeyNone ? k : static_cast<uint8_t>(kPosText);
    f.eq = k != kKeyNone ? 0u : idHash(id);
    locate(&f, in, fu, frame, tagUnsync);
    LimitIn capped(in, lim.maxText);
    TextIn t(capped, enc);
    f.empty = !anyText(t);
    if (c.failed()) return;
    insertInTag(f);
  }

  // An APIC frame: its MIME, type and description, then the image data,
  // located (2.6.4: picCoding 0 raw, 1 unsynchronised).
  void readApic(ByteIn& in, const UnsyncIn* fu, LimitIn& frame, const UnsyncIn* tagUnsync, FrameRef* f,
                uint8_t ver) {
    const int e = in.get();
    if (e < 0) return;
    const uint8_t enc = static_cast<uint8_t>(e > 3 ? 0 : e);
    if (e > 3) issues |= kIssueLofty;
    uint8_t mime = 3;
    if (ver == 2) {
      uint8_t fmt[3];
      if (!in.getN(fmt, 3)) return;
      if (std::memcmp(fmt, "JPG", 3) == 0) mime = 1;
      else if (std::memcmp(fmt, "PNG", 3) == 0) mime = 2;
      else issues |= kIssueLofty;  // lofty fails the tag
    } else {
      char m[16];
      size_t n = 0;
      int b;
      while ((b = in.get()) > 0) {
        if (n < sizeof(m)) m[n] = static_cast<char>(b);
        ++n;
      }
      if (b < 0) return;
      mime = n <= sizeof(m) ? tagrules::mimeOf(m, n) : 3;
    }
    const int type = in.get();
    if (type < 0) return;
    // The description, to its terminator (a NUL, or a NUL pair in UTF-16).
    if (enc == kUtf16 || enc == kUtf16Be) {
      for (;;) {
        const int a = in.get(), b = in.get();
        if (a < 0 || b < 0) return;
        if (a == 0 && b == 0) break;
      }
    } else {
      int b;
      while ((b = in.get()) > 0) {
      }
      if (b < 0) return;
    }
    f->key = kAPIC;
    f->enc = static_cast<uint8_t>(type);
    f->aux = mime;
    f->eq = ++uniq;
    const uint32_t dataPos = in.filePos();
    uint32_t stored;
    if (tagUnsync) {
      // The stored length is known only by reading through the picture
      // (the walk's budget is extended for it: walkId3()).
      frame.finish();
      if (c.failed()) return;  // stopped inside it: its length isn't known
      stored = in.filePos() - dataPos;
      f->mode = kModeUnsync;
    } else {
      stored = frame.filePos() + frame.left() - dataPos;
      f->mode = fu ? kModeUnsync : 0;
    }
    f->at = dataPos;
    f->limit = stored;
    f->end = dataPos + stored;
    f->empty = stored == 0;
    if (f->empty) {
      ++tagN;  // lofty keeps it; mStream drops a picture with no bytes
      return;
    }
    insertInTag(*f);
  }

  // lofty's construct_tdrc_from_v3, at the end of a v2.3 tag: its TDRC frames
  // (TYER, upgraded) are taken out (a swap partition: the frames they trade
  // places with move); the first, unless it has a month (then none is
  // left), comes back last with TDAT's day and month and TIME's hour and
  // minute, and must still pass verify(); a TDAT and a TIME so used are
  // taken out the same way.
  void v23Date(uint8_t t) {
    const FrameRef* first = nullptr;
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      const FrameRef& e = s.frames_[i];
      if (e.live && e.key == kTDRC && e.tag == t && (!first || e.place < first->place)) first = &e;
    }
    if (!first) return;
    FrameRef f = *first;
    swapRemove(t, kTDRC);
    if (f.aux & kTsMonth) return;
    tagrules::Timestamp ts;
    ts.year = f.year;
    uint8_t a = 0, b = 0;
    bool dateUsed = false, timeUsed = false;
    if (pairOf(kTDAT, t, &a, &b)) {
      ts.day = a;
      ts.month = b;
      ts.fields = 2;
      dateUsed = true;
      if (pairOf(kTIME, t, &a, &b)) {
        ts.hour = a;
        ts.minute = b;
        ts.fields = 4;
        timeUsed = true;
      }
    }
    f.aux = tagrules::verifyTimestamp(ts) ? kTsVerified : 0;
    f.eq = timestampEq(f.enc, ts);
    f.place = tagN++;
    f.seq = 0;
    f.live = 1;
    add(f);
    if (dateUsed) swapRemove(t, kTDAT);
    if (timeUsed) swapRemove(t, kTIME);
  }

  // A v2.3 TDAT ("DDMM") or TIME ("HHMM") of tag t (lofty's get_text: the
  // first in the list): 4 ASCII bytes, two Rust u8s.
  bool pairOf(uint8_t k, uint8_t t, uint8_t* a, uint8_t* b) {
    const FrameRef* f = nullptr;
    for (uint32_t i = 0; i < s.nFrames_; ++i) {
      const FrameRef& e = s.frames_[i];
      if (e.live && e.key == k && e.tag == t && (!f || e.place < f->place)) f = &e;
    }
    if (!f) return false;
    Reopen ro(c, *f, lim.maxText);
    TextIn txt(ro.in, f->enc);
    Value& v = value();
    readWhole(txt, v);
    if (v.longer || v.len != 4) return false;
    for (int i = 0; i < 4; ++i)
      if (static_cast<uint8_t>(v.buf[i]) >= 0x80) return false;
    uint32_t x = 0, y = 0;
    if (!tagrules::parseU32(v.buf, 2, &x) || !tagrules::parseU32(v.buf + 2, 2, &y) || x > 255 || y > 255) return false;
    *a = static_cast<uint8_t>(x);
    *b = static_cast<uint8_t>(y);
    return true;
  }

  // ---- the resolve: the list's values into the record ----

  // Each NUL-separated value of a frame's text, to `fn(value)` (false: stop).
  template <typename Fn>
  void eachValue(const FrameRef& f, Fn fn) {
    Reopen ro(c, f, lim.maxText);
    TextIn t(ro.in, f.enc, isTxxxKey(f.key) ? f.aux : static_cast<uint8_t>(kBomNone));
    Value& v = value();
    for (;;) {
      const bool more = readValue(t, v);
      if (!fn(v) || !more) return;
    }
  }
  // The whole text of a frame (NULs kept, the trailing ones dropped).
  void wholeOf(const FrameRef& f, uint8_t enc, Value& out) {
    Reopen ro(c, f, lim.maxText);
    TextIn t(ro.in, enc, isTxxxKey(f.key) ? f.aux : static_cast<uint8_t>(kBomNone));
    readWhole(t, out);
  }
  // The first NUL-separated value (lofty's get_string(): the first item, an
  // empty one included). False when there is no frame.
  bool firstValue(const FrameRef* f, Value& out) {
    clearValue(out);
    if (!f) return false;
    eachValue(*f, [&](Value& v) {
      out = v;
      return false;
    });
    return !out.longer;
  }

  void resolveId3() {
    Value& tmp = spare();
    // The text fields: every value of the list, or the first non-empty one.
    auto list = [&](uint8_t k, cc::FieldBuilder& fb) {
      if (const FrameRef* f = firstOf(k))
        eachValue(*f, [&](Value& v) {
          fb.add(v.buf, v.len);
          return true;
        });
    };
    auto single = [&](uint8_t k, SingleField& sf) {
      if (const FrameRef* f = firstOf(k))
        eachValue(*f, [&](Value& v) {
          sf.add(v.buf, v.len);
          return !sf.isSet();
        });
    };
    single(kTIT2, r.title);
    list(kTPE1, r.artist);
    single(kTALB, r.album);
    list(kTCOM, r.composer);
    single(kTSOT, r.titleSort);
    single(kTSOP, r.artistSort);
    single(kTSOA, r.albumSort);
    single(kTSO2, r.albumArtistSort);
    // The album artist (5.3): TPE2's values; else the first TXXX ALBUMARTIST
    // or ALBUM ARTIST whose value isn't blank, whole (mStream's rule).
    list(kTPE2, r.albumArtist);
    if (!r.albumArtist.values()) {
      for (const FrameRef* f = firstOf(kAlbumArtistTxxx); f; f = nextOf(kAlbumArtistTxxx, f->seq)) {
        wholeOf(*f, f->enc, tmp);
        if (tagrules::isBlank(tmp.buf, tmp.len)) continue;
        r.albumArtist.add(tmp.buf, tmp.len);
        break;
      }
    }
    // The genres: TCON through lofty's GenresIter.
    if (const FrameRef* f = firstOf(kTCON)) genres(*f);
    // MusicBrainz: the release id's TXXX values, the recording id's UFID,
    // each trimmed (5.3).
    for (const FrameRef* f = firstOf(kMbAlbum); f && !r.mbAlbumId.isSet(); f = nextOf(kMbAlbum, f->seq))
      eachValue(*f, [&](Value& v) {
        const char* p = v.buf;
        size_t n = v.len;
        tagrules::trim(p, n);
        r.mbAlbumId.add(p, n);
        return !r.mbAlbumId.isSet();
      });
    if (const FrameRef* f = firstOf(kUFID)) {
      wholeOf(*f, kLatin1, tmp);
      const char* p = tmp.buf;
      size_t n = tmp.len;
      tagrules::trim(p, n);
      r.mbRecordingId.add(p, n);
    }
    // The year: the first timestamp in list order that converts (verified).
    for (const FrameRef* f = firstOf(kTDRC); f; f = nextOf(kTDRC, f->seq))
      if (f->aux & kTsVerified) {
        r.rec.year = f->year;
        haveYear = true;
        break;
      }
    // Track and disc: TRCK and TPOS, whole.
    auto pair = [&](uint8_t k, uint16_t* number, uint16_t* total, bool* have) {
      const FrameRef* f = firstOf(k);
      if (!f) return;
      wholeOf(*f, f->enc, tmp);
      uint32_t n = 0, tot = 0;
      const uint8_t got = tagrules::id3Pair(tmp.buf, tmp.len, &n, &tot, tmp.longer);
      *number = tagrules::clampNumber(n);
      *total = tagrules::clampNumber(tot);
      if (have) *have = (got & tagrules::kHaveNumber) != 0;
    };
    pair(kTRCK, &r.rec.track, &r.rec.trackTotal, &haveTrack);
    pair(kTPOS, &r.rec.disc, &r.rec.discTotal, nullptr);
    // The first values: compilation, BPM, key, ReplayGain.
    if (firstValue(firstOf(kTCMP), tmp)) compilation = tagrules::compilationOf(tmp.buf, tmp.len);
    if (firstValue(firstOf(kTBPM), tmp)) cc::bpm10FromText(tmp.buf, tmp.len, &r.rec.bpm10);
    if (firstValue(firstOf(kTKEY), tmp)) r.rec.camelot = tagrules::camelotOf(tmp.buf, tmp.len);
    if (firstValue(firstOf(kRgTrackGain), tmp) && cc::gainFromText(tmp.buf, tmp.len, &r.rec.rgTrackGain))
      r.rec.flags |= mptg::kHasRgTrack;
    if (firstValue(firstOf(kRgAlbumGain), tmp) && cc::gainFromText(tmp.buf, tmp.len, &r.rec.rgAlbumGain))
      r.rec.flags |= mptg::kHasRgAlbum;
    uint16_t pk = 0;
    if (firstValue(firstOf(kRgTrackPeak), tmp) && cc::peakFromText(tmp.buf, tmp.len, &pk)) r.rec.rgTrackPeak = pk;
    if (firstValue(firstOf(kRgAlbumPeak), tmp) && cc::peakFromText(tmp.buf, tmp.len, &pk)) r.rec.rgAlbumPeak = pk;
    // The picture.
    PicVote pics;
    for (const FrameRef* f = firstOf(kAPIC); f; f = nextOf(kAPIC, f->seq)) {
      Pic p;
      p.offset = f->at;
      p.length = f->limit;
      p.type = f->enc;
      p.mime = f->aux;
      p.coding = f->mode ? mptg::kCodingUnsync : mptg::kCodingRaw;
      p.set = true;
      pics.add(p);
    }
    picture = elect(pics, PicVote());
  }

  // TCON's items (lofty's GenresIter): a segment followed by a NUL is one
  // item, whole; the last one (lofty trims the trailing NULs first) splits on
  // its "(...)" groups.
  void genres(const FrameRef& f) {
    Reopen ro(c, f, lim.maxText);
    TextIn t(ro.in, f.enc);
    Value& v = value();
    Value& held = spare();
    bool haveHeld = false;
    for (;;) {
      const bool more = readValue(t, v);
      if (v.len) {
        if (haveHeld) addGenre(held.buf, held.len);
        held = v;
        haveHeld = true;
      }
      if (!more) break;
    }
    if (!haveHeld) return;
    size_t pos = 0, at = 0, len = 0;
    while (tagrules::nextParenItem(held.buf, held.len, &pos, &at, &len)) addGenre(held.buf + at, len);
  }
  void addGenre(const char* g, size_t n) {
    size_t len = 0;
    const char* name = tagrules::parseGenre(g, n, &len);
    r.genre.add(name, len);
  }

  // -------------------------------------------------------------------------
  // ID3v1 (lofty's Id3v1Tag): 30-byte texts cut at their first NUL, as
  // ISO-8859-1, trimmed (mStream's id3v1_text); a year of four digits; a
  // track number when byte 125 is 0; a genre byte through the table.
  // -------------------------------------------------------------------------
  void v1Trimmed(uint32_t at, uint32_t len, const char** out, size_t* n) {
    Value& v = value();
    clearValue(v);
    const uint8_t* p = s.v1_ + at;
    for (uint32_t i = 0; i < len && p[i]; ++i) put(v, p[i]);
    const char* q = v.buf;
    size_t k = v.len;
    tagrules::trim(q, k);
    *out = q;
    *n = k;
  }
  bool v1Year(uint16_t* y) {
    uint32_t digits = 0, num = 0;
    for (uint32_t i = 0; i < 4 && s.v1_[93 + i] >= '0' && s.v1_[93 + i] <= '9'; ++i) {
      num = num * 10 + (s.v1_[93 + i] - '0');
      ++digits;
    }
    // lofty's Year item is the number written out: four digits only from 1000.
    if (digits != 4 || num < 1000) return false;
    *y = static_cast<uint16_t>(num);
    return true;
  }
  bool v1Track(uint16_t* t) {
    if (s.v1_[125] != 0 || s.v1_[126] == 0) return false;
    *t = s.v1_[126];
    return true;
  }

  // ID3v1 as the chosen tag.
  void resolveV1() {
    const char* p;
    size_t n;
    v1Trimmed(3, 30, &p, &n);
    r.title.add(p, n);
    v1Trimmed(33, 30, &p, &n);
    r.artist.add(p, n);
    v1Trimmed(63, 30, &p, &n);
    r.album.add(p, n);
    uint16_t y = 0;
    if (v1Year(&y)) r.rec.year = y;
    v1Track(&r.rec.track);
    if (const char* g = tagrules::genreName(s.v1_[127])) r.genre.add(g, std::strlen(g));
  }

  // Every value blank (or none).
  static bool blankList(const cc::FieldBuilder& fb) {
    const char* d = fb.data();
    size_t start = 0;
    for (size_t i = 0; i <= fb.size(); ++i)
      if (i == fb.size() || d[i] == cc::kSeparator) {
        if (!tagrules::isBlank(d + start, i - start)) return false;
        start = i + 1;
      }
    return true;
  }

  // ID3v1 filling a blank title, artist, album or genre and an absent year
  // or track number of the chosen tag (5.1).
  void fillFromV1() {
    const char* p;
    size_t n;
    if (tagrules::isBlank(r.title.data(), r.title.size())) {
      v1Trimmed(3, 30, &p, &n);
      if (n) {
        r.title.clear();
        r.title.add(p, n);
      }
    }
    if (blankList(r.artist)) {
      v1Trimmed(33, 30, &p, &n);
      if (n) {
        r.artist.clear();
        r.artist.add(p, n);
      }
    }
    if (tagrules::isBlank(r.album.data(), r.album.size())) {
      v1Trimmed(63, 30, &p, &n);
      if (n) {
        r.album.clear();
        r.album.add(p, n);
      }
    }
    if (blankList(r.genre)) {
      if (const char* g = tagrules::genreName(s.v1_[127])) {
        r.genre.clear();
        r.genre.add(g, std::strlen(g));
      }
    }
    uint16_t y = 0;
    if (!haveYear && v1Year(&y)) r.rec.year = y;
    uint16_t tr = 0;
    if (!haveTrack && v1Track(&tr)) r.rec.track = tr;
  }

  // -------------------------------------------------------------------------
  // Values from Vorbis comments and APE items, in lofty's item order.
  // -------------------------------------------------------------------------
  static void keep(Num* n, const char* text, size_t len, bool longer) {
    if (n->seen) return;
    n->seen = true;
    n->longer = longer || len > sizeof(n->text);
    n->len = static_cast<uint8_t>(n->longer ? 0 : len);
    std::memcpy(n->text, text, n->len);
  }

  enum VKey : uint8_t {
    vNone, vTitle, vArtist, vAlbum, vAlbumArtist, vGenre, vComposer, vTitleSort, vArtistSort, vAlbumSort,
    vAlbumArtistSort, vMbAlbum, vMbRecording, vDate, vYear, vTrack, vTrackTotal, vDisc, vDiscTotal, vBpm, vKey,
    vCompilation, vRgTG, vRgTP, vRgAG, vRgAP, vR128T, vR128A, vPicture, vCoverArt,
  };

  // A text value of key k (UTF-8, its trailing NULs gone) into the record:
  // a list's every value, a single field's first non-empty one, a number's
  // first item.
  void textValue(uint8_t k, const char* p, size_t n, bool longer) {
    switch (k) {
      case vTitle: r.title.add(p, n); break;
      case vArtist: r.artist.add(p, n); break;
      case vAlbum: r.album.add(p, n); break;
      case vAlbumArtist: r.albumArtist.add(p, n); break;
      case vGenre: r.genre.add(p, n); break;
      case vComposer: r.composer.add(p, n); break;
      case vTitleSort: r.titleSort.add(p, n); break;
      case vArtistSort: r.artistSort.add(p, n); break;
      case vAlbumSort: r.albumSort.add(p, n); break;
      case vAlbumArtistSort: r.albumArtistSort.add(p, n); break;
      case vMbAlbum:
      case vMbRecording: {
        tagrules::trim(p, n);
        (k == vMbAlbum ? r.mbAlbumId : r.mbRecordingId).add(p, n);
        break;
      }
      case vDate: keep(&date, p, n, longer); break;
      case vYear: keep(&year, p, n, longer); break;
      case vTrack: keep(&trackNum, p, n, longer); break;
      case vTrackTotal: keep(&trackTot, p, n, longer); break;
      case vDisc: keep(&discNum, p, n, longer); break;
      case vDiscTotal: keep(&discTot, p, n, longer); break;
      case vBpm: keep(&bpm, p, n, longer); break;
      case vKey: keep(&key, p, n, longer); break;
      case vCompilation: keep(&comp, p, n, longer); break;
      case vRgTG: keep(&rgTG, p, n, longer); break;
      case vRgTP: keep(&rgTP, p, n, longer); break;
      case vRgAG: keep(&rgAG, p, n, longer); break;
      case vRgAP: keep(&rgAP, p, n, longer); break;
      case vR128T: keep(&r128T, p, n, longer); break;
      case vR128A: keep(&r128A, p, n, longer); break;
      default: break;
    }
  }

  // Track or disc (mStream: lofty's track() and track_total() from the
  // first items, then parse_num_of() of the number's item for what's
  // missing).
  static void finishPair(const Num& num, const Num& tot, uint16_t* number, uint16_t* total) {
    uint32_t n = 0, t = 0;
    bool haveN = num.seen && !num.longer && tagrules::parseU32(num.text, num.len, &n);
    bool haveT = tot.seen && !tot.longer && tagrules::parseU32(tot.text, tot.len, &t);
    if ((!haveN || !haveT) && num.seen && !num.longer) {
      uint32_t a = 0, b = 0;
      const uint8_t got = tagrules::numOf(num.text, num.len, &a, &b);
      if (!haveN && (got & tagrules::kHaveNumber)) {
        n = a;
        haveN = true;
      }
      if (!haveT && (got & tagrules::kHaveTotal)) {
        t = b;
        haveT = true;
      }
    }
    *number = haveN ? tagrules::clampNumber(n) : 0;
    *total = haveT ? tagrules::clampNumber(t) : 0;
  }

  void finishValues(bool opus) {
    uint16_t y = 0;
    if (year.seen) {
      if (!year.longer && tagrules::yearOf(year.text, year.len, &y)) r.rec.year = y;
    } else if (date.seen) {
      if (!date.longer && tagrules::yearOf(date.text, date.len, &y)) r.rec.year = y;
    }
    finishPair(trackNum, trackTot, &r.rec.track, &r.rec.trackTotal);
    finishPair(discNum, discTot, &r.rec.disc, &r.rec.discTotal);
    if (comp.seen && !comp.longer) compilation = tagrules::compilationOf(comp.text, comp.len);
    if (bpm.seen && !bpm.longer) cc::bpm10FromText(bpm.text, bpm.len, &r.rec.bpm10);
    if (key.seen && !key.longer) r.rec.camelot = tagrules::camelotOf(key.text, key.len);
    if (opus) {
      // Opus (RFC 7845, 5.2.1): the gains are R128's, a REPLAYGAIN gain isn't
      // read there. The peaks are REPLAYGAIN's.
      if (r128T.seen && !r128T.longer && cc::r128FromText(r128T.text, r128T.len, &r.rec.rgTrackGain))
        r.rec.flags |= mptg::kHasRgTrack | mptg::kRgFromR128;
      if (r128A.seen && !r128A.longer && cc::r128FromText(r128A.text, r128A.len, &r.rec.rgAlbumGain))
        r.rec.flags |= mptg::kHasRgAlbum | mptg::kRgFromR128;
    } else {
      if (rgTG.seen && !rgTG.longer && cc::gainFromText(rgTG.text, rgTG.len, &r.rec.rgTrackGain))
        r.rec.flags |= mptg::kHasRgTrack;
      if (rgAG.seen && !rgAG.longer && cc::gainFromText(rgAG.text, rgAG.len, &r.rec.rgAlbumGain))
        r.rec.flags |= mptg::kHasRgAlbum;
    }
    uint16_t pk = 0;
    if (rgTP.seen && !rgTP.longer && cc::peakFromText(rgTP.text, rgTP.len, &pk)) r.rec.rgTrackPeak = pk;
    if (rgAP.seen && !rgAP.longer && cc::peakFromText(rgAP.text, rgAP.len, &pk)) r.rec.rgAlbumPeak = pk;
  }

  // -------------------------------------------------------------------------
  // Vorbis comments (lofty's read_comments): FLAC's VORBIS_COMMENT block, an
  // Opus file's OpusTags packet after its magic. Two passes. The first
  // follows lofty's item list: each comment it keeps takes the next place,
  // and its reading of TRACKNUMBER and DISCNUMBER ("N/M": insert() drops the
  // items of a key and keeps the others' order; remove() is a swap
  // partition, which can change it) moves the places; the comments a field
  // takes are located. The second reads those values in their final order.
  // -------------------------------------------------------------------------
  // What insert() and remove() match an item by (its key, ignoring case).
  enum ItemKind : uint8_t {
    kOther, kTrackNumber, kTrackNum, kTrackTotal, kTotalTracks, kDiscNumber, kDiscTotal, kTotalDiscs,
  };
  static constexpr uint8_t kSynth = 1;  // lofty wrote the value: the number
  static constexpr uint8_t kSets = 2;   // the value would set its field (not empty; an MB id not blank)
  static constexpr uint8_t kCut = 4;    // a list cuts the value (past 255 bytes)

  // What a key's items are to the record: a field taking its first value, a
  // list, or a number taking its first item.
  enum KeyClass : uint8_t { kcNone, kcSingle, kcList, kcNumber };
  static uint8_t classOf(uint8_t k) {
    switch (k) {
      case vTitle: case vAlbum: case vTitleSort: case vArtistSort: case vAlbumSort: case vAlbumArtistSort:
      case vMbAlbum: case vMbRecording: return kcSingle;
      case vArtist: case vAlbumArtist: case vGenre: case vComposer: return kcList;
      case vDate: case vYear: case vTrack: case vTrackTotal: case vDisc: case vDiscTotal: case vBpm: case vKey:
      case vCompilation: case vRgTG: case vRgTP: case vRgAG: case vRgAP: case vR128T: case vR128A: return kcNumber;
      default: return kcNone;
    }
  }

  static uint8_t vorbisKey(const char* k, size_t n) {
    static const struct {
      const char* name;
      uint8_t key;
    } kKeys[] = {
        {"TITLE", vTitle}, {"ARTIST", vArtist}, {"ALBUM", vAlbum}, {"ALBUMARTIST", vAlbumArtist},
        {"ALBUM ARTIST", vAlbumArtist}, {"GENRE", vGenre}, {"COMPOSER", vComposer}, {"TITLESORT", vTitleSort},
        {"ARTISTSORT", vArtistSort}, {"ALBUMSORT", vAlbumSort}, {"ALBUMARTISTSORT", vAlbumArtistSort},
        {"MUSICBRAINZ_ALBUMID", vMbAlbum}, {"MUSICBRAINZ_TRACKID", vMbRecording}, {"DATE", vDate},
        {"YEAR", vYear}, {"TRACKNUMBER", vTrack}, {"TRACKTOTAL", vTrackTotal}, {"TOTALTRACKS", vTrackTotal},
        {"DISCNUMBER", vDisc}, {"DISCTOTAL", vDiscTotal}, {"TOTALDISCS", vDiscTotal}, {"BPM", vBpm},
        {"INITIALKEY", vKey}, {"KEY", vKey}, {"COMPILATION", vCompilation},
        {"REPLAYGAIN_TRACK_GAIN", vRgTG}, {"REPLAYGAIN_TRACK_PEAK", vRgTP}, {"REPLAYGAIN_ALBUM_GAIN", vRgAG},
        {"REPLAYGAIN_ALBUM_PEAK", vRgAP}, {"R128_TRACK_GAIN", vR128T}, {"R128_ALBUM_GAIN", vR128A},
        {"METADATA_BLOCK_PICTURE", vPicture}, {"COVERART", vCoverArt},
    };
    for (const auto& e : kKeys)
      if (asciiIEq(k, n, e.name)) return e.key;
    return vNone;
  }
  static uint8_t vorbisKind(const char* k, size_t n) {
    if (asciiIEq(k, n, "TRACKNUMBER")) return kTrackNumber;
    if (asciiIEq(k, n, "TRACKNUM")) return kTrackNum;
    if (asciiIEq(k, n, "TRACKTOTAL")) return kTrackTotal;
    if (asciiIEq(k, n, "TOTALTRACKS")) return kTotalTracks;
    if (asciiIEq(k, n, "DISCNUMBER")) return kDiscNumber;
    if (asciiIEq(k, n, "DISCTOTAL")) return kDiscTotal;
    if (asciiIEq(k, n, "TOTALDISCS")) return kTotalDiscs;
    return kOther;
  }

  // A UTF-8 value of `n` bytes from `in` (lofty's utf8_decode_str: a value
  // that isn't UTF-8 is dropped; trailing NULs trimmed). False: dropped.
  bool utf8Value(ByteIn& in, uint32_t n, Value& v) {
    LimitIn val(in, n);
    LimitIn capped(val, lim.maxText);
    TextIn t(capped, kUtf8);
    readWhole(t, v);
    val.finish();
    return !t.invalid();
  }

  // The first pass's reading of a value (`v`, as the second pass reads it
  // again), for a full table: whether it sets its field (kSets), whether a
  // list cuts it (kCut), and its hash as a list keeps it (FieldBuilder: cut
  // to 255 bytes at a code point, control characters to spaces).
  static uint8_t valueFacts(uint8_t k, const Value& v, uint32_t* hash) {
    const char* p = v.buf;
    size_t n = v.len;
    if (k == vMbAlbum || k == vMbRecording) tagrules::trim(p, n);
    uint8_t flags = n ? kSets : 0;
    const size_t keep = cc::utf8CutLength(v.buf, v.len, cc::FieldBuilder::kValueMax);
    if (keep < v.len) flags |= kCut;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < keep; ++i) {
      uint8_t c = static_cast<uint8_t>(v.buf[i]);
      if (c < 0x20 || c == 0x7F) c = ' ';
      h = (h ^ c) * 16777619u;
    }
    *hash = h;
    return flags;
  }

  // Item x can't change the record, whatever follows it: an item before it
  // in the list settles its field (a single field's first value, a number's
  // first item, a list's own value or its 17 distinct ones: the list has
  // ended), or it sets nothing. Only an item lofty's removals don't match
  // (kOther): two TRACKNUMBER or DISCNUMBER items' swap could still move an
  // earlier one after it, a difference the header lists.
  bool dominated(const Scanner::Item& x) const {
    if (x.kind != kOther || (x.flags & kSynth)) return false;
    const uint8_t cls = classOf(x.key);
    if (cls == kcNone) return false;
    if (cls != kcNumber && !(x.flags & kSets)) return true;  // empty: it sets nothing
    uint32_t seen[17];
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < s.nItems_; ++i) {
      const Scanner::Item& y = s.items_[i];
      if (&y == &x || y.key != x.key || y.place >= x.place) continue;
      if (cls == kcNumber) return true;  // the number is its first item's
      if (!(y.flags & kSets)) continue;
      if (cls == kcSingle) return true;  // the field is its first value
      // A list: a repeat of a value it holds goes (a cut one only after a
      // cut one: its cut marks the list TRUNCATED), and after 17 distinct
      // values the list has ended.
      if (y.hash == x.hash && (!(x.flags & kCut) || (y.flags & kCut))) return true;
      bool known = false;
      for (uint32_t j = 0; j < distinct && !known; ++j) known = seen[j] == y.hash;
      if (!known) seen[distinct++] = y.hash;
      if (distinct == 17) return true;
    }
    return false;
  }

  // A full table: the items that can't change the record go (their places
  // still count: the others keep theirs). False: none could.
  bool shedItems() {
    if (itemsShed) return false;  // nothing changed since the last try
    bool drop[Scanner::kMaxItems];
    bool any = false;
    for (uint32_t i = 0; i < s.nItems_; ++i) any |= (drop[i] = dominated(s.items_[i]));
    uint32_t w = 0;
    for (uint32_t i = 0; i < s.nItems_; ++i)
      if (!drop[i]) s.items_[w++] = s.items_[i];
    s.nItems_ = w;
    itemsShed = !any;
    return any;
  }

  // ---- the item list's moves ----
  void pushItem(uint8_t k, uint8_t kind, uint32_t at, uint32_t len, uint32_t page, uint32_t number, uint8_t flags,
                uint32_t hash = 0) {
    if (k == vNone && kind == kOther) {  // nothing to keep: its place only
      ++itemN;
      return;
    }
    Scanner::Item it;
    std::memset(&it, 0, sizeof(it));
    it.at = at;
    it.len = len;
    it.number = number;
    it.page = page;
    it.hash = hash;
    it.place = itemN++;
    it.key = k;
    it.kind = kind;
    it.flags = flags;
    if (s.nItems_ == Scanner::kMaxItems) {
      issues |= kIssueTooMany;
      if (dominated(it) || !shedItems()) return;  // its place counts
    }
    s.items_[s.nItems_++] = it;
    itemsShed = false;
  }
  void dropItem(uint32_t i) {
    s.items_[i] = s.items_[--s.nItems_];
    itemsShed = false;
  }
  // insert()'s retain: the items of a kind go, the others keep their order.
  void retainRemove(uint8_t kind) {
    for (uint32_t i = 0; i < s.nItems_;) {
      if (s.items_[i].kind != kind) {
        ++i;
        continue;
      }
      const uint16_t p = s.items_[i].place;
      dropItem(i);
      for (uint32_t j = 0; j < s.nItems_; ++j)
        if (s.items_[j].place > p) --s.items_[j].place;
      --itemN;
    }
  }
  // remove()'s swap partition (VorbisComments::remove).
  void swapRemove(uint8_t kind) {
    uint16_t places[Scanner::kMaxItems];
    uint32_t n = 0;
    for (uint32_t i = 0; i < s.nItems_; ++i)
      if (s.items_[i].kind == kind) places[n++] = s.items_[i].place;
    for (uint32_t a = 1; a < n; ++a)
      for (uint32_t b = a; b > 0 && places[b - 1] > places[b]; --b) {
        const uint16_t x = places[b];
        places[b] = places[b - 1];
        places[b - 1] = x;
      }
    for (uint32_t j = 0; j < n; ++j) {
      if (places[j] == j) continue;
      for (uint32_t i = 0; i < s.nItems_; ++i)
        if (s.items_[i].kind != kind && s.items_[i].place == j) {
          s.items_[i].place = places[j];
          break;
        }
    }
    for (uint32_t i = 0; i < s.nItems_;) {
      if (s.items_[i].kind == kind) {
        dropItem(i);
        continue;
      }
      s.items_[i].place = static_cast<uint16_t>(s.items_[i].place - n);
      ++i;
    }
    itemN = static_cast<uint16_t>(itemN - n);
  }

  // lofty's TRACKNUMBER / DISCNUMBER read: "N/M" split at its first '/', a
  // total that parses set (set_track_total: insert, then remove the other
  // spelling), a number that parses set (set_track: remove TRACKNUMBER and
  // TRACKNUM, then insert; set_disk: insert), else the value pushed as it
  // is.
  void numberComment(bool trackKey, const Value& v, uint32_t at, uint32_t len, uint32_t page) {
    size_t cut = v.len;
    for (size_t i = 0; i < v.len; ++i)
      if (v.buf[i] == '/') {
        cut = i;
        break;
      }
    uint32_t cur = 0, tot = 0;
    const bool curOk = !v.longer && tagrules::parseU32(v.buf, cut, &cur);
    const bool totOk = !v.longer && cut < v.len && tagrules::parseU32(v.buf + cut + 1, v.len - cut - 1, &tot);
    if (trackKey) {
      if (totOk) {
        retainRemove(kTrackTotal);
        pushItem(vTrackTotal, kTrackTotal, 0, 0, 0, tot, kSynth);
        swapRemove(kTotalTracks);
      }
      if (curOk) {
        swapRemove(kTrackNumber);
        swapRemove(kTrackNum);
        retainRemove(kTrackNumber);
        pushItem(vTrack, kTrackNumber, 0, 0, 0, cur, kSynth);
      } else {
        pushItem(vTrack, kTrackNumber, at, len, page, 0, 0);
      }
    } else {
      if (totOk) {
        retainRemove(kDiscTotal);
        pushItem(vDiscTotal, kDiscTotal, 0, 0, 0, tot, kSynth);
        swapRemove(kTotalDiscs);
      }
      if (curOk) {
        retainRemove(kDiscNumber);
        pushItem(vDisc, kDiscNumber, 0, 0, 0, cur, kSynth);
      } else {
        pushItem(vDisc, kDiscNumber, at, len, page, 0, 0);
      }
    }
  }

  // A FLAC PICTURE (a block's content, or a METADATA_BLOCK_PICTURE value's
  // decoded bytes) as lofty's from_flac_bytes_inner reads it: false when it
  // drops it. `size` is the content's length; *head the bytes before the
  // image data.
  static bool flacPicture(ByteIn& in, uint32_t size, uint8_t* type, uint8_t* mime, uint32_t* dataLen,
                          uint32_t* head) {
    if (size < 32) return false;
    uint32_t ty = 0, ml = 0, dl = 0, len = 0;
    if (!in.be32v(&ty) || !in.be32v(&ml)) return false;
    uint32_t rest = size - 8;
    if (ml > rest) return false;
    {
      char m[16];
      LimitIn mm(in, ml);
      TextIn t(mm, kUtf8);
      uint32_t k = 0;
      for (;;) {
        const int32_t cp = t.next();
        if (cp < 0) break;
        if (k < sizeof(m)) m[k] = static_cast<char>(cp);
        ++k;
      }
      if (mm.left()) return false;  // the MIME string ran out of bytes
      if (t.invalid()) return false;
      *mime = (k > 0 && k <= sizeof(m)) ? tagrules::mimeOf(m, k) : 3;
    }
    rest -= ml;
    if (rest < 4 || !in.be32v(&dl)) return false;
    rest -= 4;
    uint32_t skipped = 0;
    if (dl > 0 && dl < rest) {
      if (!in.skip(dl)) return false;
      rest -= dl;
      skipped = dl;
    }
    uint8_t dims[16];
    if (rest < 20 || !in.getN(dims, 16) || !in.be32v(&len)) return false;
    rest -= 20;
    if (len > rest) return false;
    *type = static_cast<uint8_t>(ty);
    *dataLen = len;
    *head = 32 + ml + skipped;
    return true;
  }

  // The first pass. `ogg`: the packet's reader, when it spans Ogg pages.
  void readComments(ByteIn& in, OggIn* ogg) {
    uint32_t vendor = 0, count = 0;
    if (!in.le32v(&vendor) || vendor > in.left() || !in.skip(vendor) || !in.le32v(&count)) {
      issues |= kIssueTruncatedTag | kIssueLofty;
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (i >= lim.maxComments) {
        issues |= kIssueTooMany;
        return;
      }
      if (c.failed()) return;
      uint32_t len = 0;
      if (!in.le32v(&len)) return;
      if (len > in.left()) {
        issues |= kIssueTruncatedTag | kIssueLofty;
        return;
      }
      // The key, to its '=' (lofty's valid_vorbis_comments_key: 0x20-0x7D).
      char kbuf[32];
      uint32_t k = 0;
      bool eq = false, keyOk = true;
      while (k < len) {
        const int b = in.get();
        if (b < 0) return;
        if (b == '=') {
          eq = true;
          break;
        }
        if (b < 0x20 || b > 0x7D) keyOk = false;
        if (k < sizeof(kbuf)) kbuf[k] = static_cast<char>(b);
        ++k;
      }
      if (!eq) continue;  // no separator: lofty drops it
      const uint32_t vlen = len - k - 1;
      const bool known = k <= sizeof(kbuf);
      const uint8_t vk = known ? vorbisKey(kbuf, k) : static_cast<uint8_t>(vNone);
      if (vk == vPicture) {
        vorbisPicture(in, vlen);
        continue;
      }
      if (vk == vCoverArt) {
        issues |= kIssueSkippedPicture;  // no picCoding anchors a base64 image (2.6.4)
        if (!in.skip(vlen)) return;
        continue;
      }
      // Where the value is, then whether lofty keeps it.
      in.settle();
      const uint32_t at = ogg ? ogg->inBody() : in.filePos();
      const uint32_t page = ogg ? ogg->page() : 0;
      Value& v = value();
      const bool valid = utf8Value(in, vlen, v);
      if (c.failed()) return;  // stopped inside it (the budget): not read whole
      if (vk == vTrack || vk == vDisc) {
        if (valid) numberComment(vk == vTrack, v, at, vlen, page);
        continue;
      }
      if (!keyOk || !valid) continue;  // lofty drops it
      uint32_t hash = 0;
      const uint8_t facts = valueFacts(vk, v, &hash);
      pushItem(vk, known ? vorbisKind(kbuf, k) : static_cast<uint8_t>(kOther), at, vlen, page, 0, facts, hash);
    }
  }

  // The second pass: the items in their final order, into the record.
  void finishComments(bool opus) {
    // The order: an insertion sort of the items by place.
    uint8_t order[Scanner::kMaxItems];
    const uint32_t n = s.nItems_;
    for (uint32_t i = 0; i < n; ++i) {
      order[i] = static_cast<uint8_t>(i);
      for (uint32_t j = i; j > 0 && s.items_[order[j - 1]].place > s.items_[order[j]].place; --j) {
        const uint8_t x = order[j];
        order[j] = order[j - 1];
        order[j - 1] = x;
      }
    }
    for (uint32_t i = 0; i < n; ++i) {
      const Scanner::Item& it = s.items_[order[i]];
      if (it.key == vNone) continue;
      if (it.flags & kSynth) {
        char text[12];
        size_t len = 0;
        uint32_t v = it.number;
        char digits[12];
        do {
          digits[len++] = static_cast<char>('0' + v % 10);
          v /= 10;
        } while (v);
        for (size_t d = 0; d < len; ++d) text[d] = digits[len - 1 - d];
        textValue(it.key, text, len, false);
        continue;
      }
      Value& v = value();
      if (opus) {
        OggIn in(c, it.page, serial, lim.maxOggPages, &issues);
        if (!in.resume(it.page, it.at)) continue;
        LimitIn val(in, it.len);
        LimitIn capped(val, lim.maxText);
        TextIn t(capped, kUtf8);
        readWhole(t, v);
      } else {
        PlainIn in(c, it.at, it.at + it.len);
        LimitIn capped(in, lim.maxText);
        TextIn t(capped, kUtf8);
        readWhole(t, v);
      }
      textValue(it.key, v.buf, v.len, v.longer);
    }
    finishValues(opus);
  }

  // METADATA_BLOCK_PICTURE: base64 of a FLAC PICTURE. Its anchor is the
  // value's first character (2.6.4, picCoding 2); the head is decoded and
  // checked as lofty would, the rest only stepped over (page headers read).
  void vorbisPicture(ByteIn& in, uint32_t vlen) {
    LimitIn val(in, vlen);
    val.settle();
    const uint32_t at = val.filePos();
    // lofty decodes the whole value as strict base64: its length is a
    // multiple of 4.
    if (vlen < 4 || vlen % 4 != 0) {
      val.finish();
      return;
    }
    uint8_t type = 0, mime = 0;
    uint32_t dataLen = 0, head = 0;
    bool ok;
    {
      B64In d(val);
      ok = flacPicture(d, vlen / 4 * 3, &type, &mime, &dataLen, &head);
    }
    // The padding, from the last two characters.
    const uint32_t left = val.left();
    if (left > 2) val.skip(left - 2);
    int pad = 0;
    for (int b; (b = val.get()) >= 0;)
      if (b == '=') ++pad;
    const uint32_t decoded = vlen / 4 * 3 - static_cast<uint32_t>(pad);
    if (c.failed()) return;  // stopped inside it (the budget): its end wasn't seen
    if (!ok || static_cast<uint64_t>(head) + dataLen > decoded || dataLen == 0) return;
    Pic p;
    p.offset = at;
    p.length = vlen;
    p.type = type;
    p.mime = mime;
    p.coding = mptg::kCodingOggBase64;
    p.set = true;
    commentPics.add(p);
  }

  // -------------------------------------------------------------------------
  // APEv2 (lofty's read_ape_tag: items keyed ignoring ASCII case, a later
  // one replacing an earlier and going last; a text item's value split on
  // NUL).
  // -------------------------------------------------------------------------
  static uint8_t apeKey(const char* k, size_t n) {
    static const struct {
      const char* name;
      uint8_t key;
    } kKeys[] = {
        {"Title", vTitle}, {"Artist", vArtist}, {"Album", vAlbum}, {"Album Artist", vAlbumArtist},
        {"ALBUMARTIST", vAlbumArtist}, {"Genre", vGenre}, {"Composer", vComposer}, {"TITLESORT", vTitleSort},
        {"ARTISTSORT", vArtistSort}, {"ALBUMSORT", vAlbumSort}, {"ALBUMARTISTSORT", vAlbumArtistSort},
        {"MUSICBRAINZ_ALBUMID", vMbAlbum}, {"MUSICBRAINZ_TRACKID", vMbRecording}, {"Year", vDate},
        {"Track", vTrack}, {"Disc", vDisc}, {"Compilation", vCompilation},
        {"REPLAYGAIN_TRACK_GAIN", vRgTG}, {"REPLAYGAIN_TRACK_PEAK", vRgTP}, {"REPLAYGAIN_ALBUM_GAIN", vRgAG},
        {"REPLAYGAIN_ALBUM_PEAK", vRgAP},
    };
    for (const auto& e : kKeys)
      if (asciiIEq(k, n, e.name)) return e.key;
    if (tagrules::apePictureType(k, n) >= 0) return vPicture;
    return vNone;
  }

  struct ApeItem {
    uint32_t valueAt = 0, valueLen = 0, flags = 0;
    uint32_t keyLen = 0;
    char key[32];
  };
  // The item at `at`, within [.., end): false when it doesn't fit.
  bool apeItem(uint32_t at, uint32_t end, ApeItem* it) {
    PlainIn in(c, at, end);
    if (!in.le32v(&it->valueLen) || !in.le32v(&it->flags)) return false;
    it->keyLen = 0;
    int b;
    while ((b = in.get()) > 0) {
      if (it->keyLen < sizeof(it->key)) it->key[it->keyLen] = static_cast<char>(b);
      ++it->keyLen;
    }
    if (b < 0) return false;
    it->valueAt = in.filePos();
    return it->valueLen <= end - it->valueAt;
  }
  static uint32_t apeKeyHash(const ApeItem& it) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < it.keyLen && i < sizeof(it.key); ++i) {
      char ch = it.key[i];
      if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
      h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    }
    return h ^ it.keyLen;
  }

  void readApe() {
    // Two passes: which item of each key is the last (lofty keeps that one,
    // at its place), then those items in order.
    uint32_t keys[48];
    uint32_t lastIndex[48];
    uint32_t nKeys = 0;
    for (int pass = 0; pass < 2; ++pass) {
      uint32_t at = apeItems;
      for (uint32_t i = 0; i < apeCount && i < lim.maxComments; ++i) {
        ApeItem it;
        if (c.failed() || !apeItem(at, apeEnd, &it)) break;
        at = it.valueAt + it.valueLen;
        const uint8_t k = it.keyLen > sizeof(it.key) ? static_cast<uint8_t>(vNone) : apeKey(it.key, it.keyLen);
        if (k == vNone) continue;
        const uint32_t h = apeKeyHash(it);
        uint32_t j = 0;
        while (j < nKeys && keys[j] != h) ++j;
        if (pass == 0) {
          if (j == nKeys) {
            if (nKeys == 48) {
              issues |= kIssueTooMany;
              continue;
            }
            keys[nKeys++] = h;
          }
          lastIndex[j] = i;
          continue;
        }
        if (j == nKeys || lastIndex[j] != i) continue;
        apeValue(k, it);
      }
    }
  }

  void apeValue(uint8_t k, const ApeItem& it) {
    const uint32_t type = (it.flags >> 1) & 3;
    if (k == vPicture) {
      if (type != 1) return;
      // A file name, its NUL, then the image (lofty's from_ape_bytes, which
      // mStream applies whatever the key's case).
      PlainIn in(c, it.valueAt, it.valueAt + it.valueLen);
      uint32_t pos = 0;
      int b = 0;
      while ((b = in.get()) >= 0) {
        ++pos;
        if (b == 0) break;
      }
      if (b != 0) return;
      uint8_t magic[8];
      if (!in.getN(magic, 8)) return;
      const uint8_t mime = tagrules::mimeOfMagic(magic);
      if (!mime) return;
      Pic p;
      p.offset = it.valueAt + pos;
      p.length = it.valueLen - pos;
      p.type = static_cast<uint8_t>(tagrules::apePictureType(it.key, it.keyLen));
      p.mime = mime;
      p.coding = mptg::kCodingApe;
      p.set = true;
      apePics.add(p);
      return;
    }
    if (type != 0) {
      if (type == 3) issues |= kIssueLofty;
      return;  // binary or a locator: not text
    }
    PlainIn in(c, it.valueAt, it.valueAt + it.valueLen);
    Value& v = value();
    if (!utf8Value(in, it.valueLen, v)) {
      issues |= kIssueLofty;  // lofty fails the file on a text item that isn't UTF-8
      return;
    }
    if (k == vTrack || k == vDisc) {
      // lofty's APE split_pair: the number is the text before the first '/',
      // as it is; the total the rest.
      size_t cut = v.len;
      for (size_t i = 0; i < v.len; ++i)
        if (v.buf[i] == '/') {
          cut = i;
          break;
        }
      keep(k == vTrack ? &trackNum : &discNum, v.buf, cut, v.longer && cut == v.len);
      if (cut < v.len) keep(k == vTrack ? &trackTot : &discTot, v.buf + cut + 1, v.len - cut - 1, v.longer);
      return;
    }
    // A text item with NULs is several values, each read whole from the
    // item (one past the value's buffer doesn't hide those after it).
    PlainIn again(c, it.valueAt, it.valueAt + it.valueLen);
    LimitIn capped(again, lim.maxText);
    TextIn t(capped, kUtf8);
    for (;;) {
      const bool more = readValue(t, v);
      textValue(k, v.buf, v.len, v.longer);
      if (!more) break;
    }
  }

  // -------------------------------------------------------------------------
  // The tail: ID3v1, and an APEv2 footer before it (lofty's find_id3v1 and
  // read_ape_tag; lofty 0.25 doesn't recognise Lyrics3v2, so neither does
  // this). The bytes they take.
  // -------------------------------------------------------------------------
  uint32_t readTail() {
    const uint32_t size = c.size();
    uint32_t used = 0;
    if (size >= 128) {
      const uint8_t* p = c.at(size - 128, 128);
      if (p && p[0] == 'T' && p[1] == 'A' && p[2] == 'G') {
        std::memcpy(s.v1_, p, 128);
        hasV1 = true;
        used = 128;
      }
    }
    if (size >= used + 32) {
      const uint32_t footer = size - used - 32;
      const uint8_t* f = c.at(footer, 32);
      if (f && std::memcmp(f, "APETAGEX", 8) == 0) {
        const uint32_t version = le32(f + 8);
        const uint32_t tagSize = le32(f + 12);
        const uint32_t count = le32(f + 16);
        if (tagSize < 32 || static_cast<uint64_t>(tagSize) > footer + 32ull) {
          issues |= kIssueLofty;
          return used;
        }
        hasApe = true;
        apeItems = footer + 32 - tagSize;
        apeEnd = footer;
        apeCount = count;
        used += tagSize + ((version == 2000 && apeItems >= 32) ? 32 : 0);
      }
    }
    return used;
  }

  // -------------------------------------------------------------------------
  // The record's finish.
  // -------------------------------------------------------------------------
  void finishRecord(uint8_t container) {
    mptg::Record& rec = r.rec;
    rec.container = container;
    rec.known = mptg::kKnownRules1;
    rec.flags = static_cast<uint16_t>((rec.flags & ~mptg::kCompilationMask) | compilation);
    if (picture.set) {
      rec.picOffset = picture.offset;
      rec.picLength = picture.length;
      rec.picType = picture.type;
      rec.picMime = picture.mime;
      rec.picCoding = picture.coding;
    }
    if (r.truncated()) rec.flags |= mptg::kTruncated;
    // NO_TAGS: no field came out of the file's tags (the length isn't a tag).
    bool any = picture.set || compilation || rec.year || rec.track || rec.trackTotal || rec.disc ||
               rec.discTotal || rec.bpm10 || rec.camelot || rec.rgTrackPeak || rec.rgAlbumPeak ||
               (rec.flags & (mptg::kHasRgTrack | mptg::kHasRgAlbum));
    for (uint32_t f = 0; f < cc::kRunFields && !any; ++f) any = r.fieldLength(f) > 0;
    if (!any) rec.flags |= mptg::kNoTags;
  }

  // -------------------------------------------------------------------------
  // The containers.
  // -------------------------------------------------------------------------
  // An MPEG audio frame header as lofty's Header::read takes it: false when
  // it isn't one. `len` its length, `kbps` its bitrate.
  static bool mpegHeader(uint32_t h, uint32_t* len, uint32_t* kbps) {
    static const uint16_t kRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    static const uint16_t kBitrates[2][3][16] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    static const uint16_t kSamples[3][2] = {{384, 384}, {1152, 1152}, {1152, 576}};
    const uint32_t v = (h >> 19) & 3;
    if (v == 1) return false;
    const uint32_t version = v == 3 ? 0 : (v == 2 ? 1 : 2);  // MPEG-1, 2, 2.5
    const uint32_t l = (h >> 17) & 3;
    if (l == 0) return false;
    const uint32_t layer = 3 - l;  // 0 Layer I, 1 II, 2 III
    const uint32_t vi = version == 0 ? 0 : 1;
    const uint32_t br = kBitrates[vi][layer][(h >> 12) & 0xF];
    if (br == 0) return false;
    const uint32_t ri = (h >> 10) & 3;
    if (ri == 3) return false;
    const uint32_t rate = kRates[version][ri];
    const uint32_t pad = ((h >> 9) & 1) ? (layer == 0 ? 4 : 1) : 0;
    *len = static_cast<uint32_t>(kSamples[layer][vi]) * br * 125 / rate + pad;
    *kbps = br;
    return true;
  }

  // lofty's find_next_frame from `from`: a frame sync whose header reads and
  // whose next frame's header matches it (mask FFFE0C00). False: none.
  bool firstFrame(uint32_t from, uint32_t* at, uint32_t* kbps) {
    uint32_t pos = from;
    const uint32_t size = c.size();
    const uint64_t stop = static_cast<uint64_t>(from) + lim.maxSync;
    while (pos + 1 < size && pos < stop && !c.failed()) {
      const uint8_t* p = c.at(pos, 2);
      if (!p) return false;
      if (!(p[0] == 0xFF && (p[1] >> 5) == 7)) {
        ++pos;
        continue;
      }
      const uint8_t* q = c.at(pos, 4);
      if (!q) return false;
      const uint32_t h = be32(q);
      uint32_t len = 0, br = 0;
      if (!mpegHeader(h, &len, &br)) {
        pos += 4;
        continue;
      }
      if (static_cast<uint64_t>(pos) + len + 4 > size) return false;  // lofty: undetermined, no frame
      const uint8_t* n = c.at(pos + len, 4);
      if (!n) return false;
      if ((be32(n) & 0xFFFE0C00u) == (h & 0xFFFE0C00u)) {
        *at = pos;
        *kbps = br;
        return true;
      }
      pos += len;
    }
    return false;
  }

  bool scanMp3() {
    // lofty skips zero bytes before the first tag.
    uint32_t pos = 0;
    while (pos < c.size() && pos < lim.maxSync) {
      const int b = c.byte(pos);
      if (b < 0) return false;
      if (b != 0) break;
      ++pos;
    }
    if (pos >= c.size()) return false;
    // The tags at the head: the walk.
    c.walk(true);
    for (uint8_t k = 0; k < lim.maxId3Tags; ++k) {
      Id3Header h;
      if (!id3Header(pos, &h)) break;
      // mStream's repair pass reads the tag at offset 0, up to 64 MB.
      walkId3(h, k == 0 && pos == 0 && h.size <= 64u * 1024 * 1024);
      // Past the tag, as its header says (where the audio is looked for
      // even when the budget stopped the walk inside it).
      if (static_cast<uint64_t>(pos) + h.total >= c.size()) {
        pos = c.size();
        break;
      }
      pos += h.total;
      if (c.stopped() || c.failed()) break;
    }
    c.walk(false);
    if (const uint8_t* p = c.at(pos, 8))
      if (std::memcmp(p, "APETAGEX", 8) == 0) issues |= kIssueLofty;  // an APE tag at the head: not read here
    uint32_t first = 0, kbps = 0;
    const bool audio = firstFrame(pos, &first, &kbps);
    // No audio: not an MP3, unless the budget stopped the scan first (the
    // tags located are still the file's: Partial).
    if (!audio && !c.stopped()) return false;
    // A tag in junk before the audio (lofty: when none was at the head,
    // within max_junk_bytes of the file's start).
    if (audio && !id3 && first > 0) {
      const uint32_t window = first < lim.maxJunk ? first : lim.maxJunk;
      for (uint32_t i = 0; i + 3 <= window; ++i) {
        const uint8_t* p = c.at(i, 3);
        if (!p) break;
        if (p[0] != 'I' || p[1] != 'D' || p[2] != '3') continue;
        Id3Header h;
        if (id3Header(i, &h)) {
          if (static_cast<uint64_t>(i) + 10 + h.size > c.size()) {
            issues |= kIssueLofty;  // lofty reads it whole first
          } else {
            c.walk(true);
            walkId3(h, false);
            c.walk(false);
          }
        }
        break;
      }
    }
    // The length: the first frame's Xing/Info/VBRI (with LAME's trim), else
    // the bitrate over the audio. The frame is the one lofty's rule found;
    // its header's fields lie in its first 194 bytes, so the window is the
    // same whatever the buffer, and so is the length.
    const uint32_t tail = readTail();
    if (audio) {
      uint32_t n = c.size() - first;
      if (n > kFrameWindow) n = kFrameWindow;
      if (const uint8_t* b = c.at(first, n)) r.rec.durationMs = progress::mp3FrameDurationMs(b, n, c.size(), first);
      if (!r.rec.durationMs && kbps) {
        const uint32_t end = c.size() > tail ? c.size() - tail : 0;
        if (end > first)
          r.rec.durationMs = static_cast<uint32_t>((static_cast<uint64_t>(end - first) * 8 + kbps / 2) / kbps);
      }
    }
    // The chosen tag (5.1): ID3v2 (with ID3v1's fill), else ID3v1, else APE.
    if (id3) {
      resolveId3();
      if (hasV1) fillFromV1();
    } else if (hasV1) {
      resolveV1();
    } else if (hasApe) {
      readApe();
      finishValues(false);
      picture = elect(apePics, PicVote());
    }
    return audio;
  }

  bool scanFlac() {
    uint32_t pos = 0;
    Id3Header h;
    c.walk(true);
    if (id3Header(0, &h)) {
      if (static_cast<uint64_t>(10) + h.size > c.size()) issues |= kIssueLofty;  // lofty reads it whole first
      walkId3(h, h.size <= 64u * 1024 * 1024);
      pos = h.total;
    }
    c.walk(false);
    const uint8_t* m = c.at(pos, 8);
    if (!m || std::memcmp(m, "fLaC", 4) != 0 || (m[4] & 0x7F) != 0) {  // STREAMINFO first
      // The budget stopped it in a front ID3v2: that tag, as far as it went.
      if (c.stopped() && id3) resolveId3();
      return false;
    }
    const uint32_t siLen = (static_cast<uint32_t>(m[5]) << 16) | (static_cast<uint32_t>(m[6]) << 8) | m[7];
    if (siLen < 18) return false;
    if (const uint8_t* si = c.at(pos, 26)) r.rec.durationMs = progress::flacDurationMs(si, 26);
    // The metadata blocks: the walk.
    c.walk(true);
    uint32_t off = pos + 4;
    bool last = false;
    for (uint32_t i = 0; !last; ++i) {
      if (i >= lim.maxFlacBlocks) {
        issues |= kIssueTooMany;
        break;
      }
      const uint8_t* bh = c.at(off, 4);
      if (!bh) break;
      last = (bh[0] & 0x80) != 0;
      const uint8_t type = bh[0] & 0x7F;
      const uint32_t len = (static_cast<uint32_t>(bh[1]) << 16) | (static_cast<uint32_t>(bh[2]) << 8) | bh[3];
      if (static_cast<uint64_t>(off) + 4 + len > c.size()) {
        issues |= kIssueTruncatedTag;
        if (type == 4 || type == 6) issues |= kIssueLofty;
        break;
      }
      if (i > 0 && type == 4 && len > 0) {
        // A later comment block replaces an earlier one (lofty, relaxed).
        clearVorbis();
        vorbisTag = true;
        PlainIn in(c, off + 4, off + 4 + len);
        readComments(in, nullptr);
      } else if (i > 0 && type == 6 && len > 0) {
        PlainIn in(c, off + 4, off + 4 + len);
        uint8_t ty = 0, mime = 0;
        uint32_t dl = 0, head = 0;
        if (flacPicture(in, len, &ty, &mime, &dl, &head)) {
          blockPicSeen = true;
          if (dl) {
            Pic p;
            p.offset = off + 4 + head;
            p.length = dl;
            p.type = ty;
            p.mime = mime;
            p.coding = mptg::kCodingRaw;
            p.set = true;
            blockPics.add(p);
          }
        }
      }
      off += 4 + len;
    }
    c.walk(false);
    // The chosen tag (5.1): the comments (a picture alone makes them), else a
    // front ID3v2.
    if (vorbisTag || blockPicSeen) {
      finishComments(false);
      picture = elect(commentPics, blockPics);
    } else if (id3) {
      resolveId3();
    }
    return true;
  }

  // A later comment block replaces an earlier one (lofty, relaxed): its
  // items and its pictures go.
  void clearVorbis() {
    s.nItems_ = 0;
    itemN = 0;
    itemsShed = false;
    commentPics.clear();
  }

  bool scanOpus() {
    const uint8_t* h = c.at(0, 27);
    if (!h || std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) return false;
    serial = le32(h + 14);
    uint64_t firstGranule = 0;
    for (int k = 7; k >= 0; --k) firstGranule = (firstGranule << 8) | h[6 + k];
    const uint8_t segs = h[26];
    if (segs == 0) return false;
    const uint8_t* lace = c.at(27, segs);
    if (!lace) return false;
    uint32_t body = 0;
    for (uint8_t i = 0; i < segs; ++i) body += lace[i];
    const uint8_t* head = c.at(27 + segs, 19);
    if (!head || std::memcmp(head, "OpusHead", 8) != 0) return false;
    const uint8_t channels = head[9];
    const uint32_t preskip = head[10] | (static_cast<uint32_t>(head[11]) << 8);
    const uint8_t family = head[18];
    if ((family == 0 && channels > 2) || (family == 1 && channels > 8)) return false;
    OggIn in(c, 27 + segs + body, serial, lim.maxOggPages, &issues);
    if (!in.start()) return false;
    uint8_t magic[8];
    if (!in.getN(magic, 8) || std::memcmp(magic, "OpusTags", 8) != 0) return false;
    // The comment packet's pages: the walk.
    c.walk(true);
    readComments(in, &in);
    c.walk(false);
    finishComments(true);
    picture = elect(commentPics, PicVote());
    // The length: the last page's granule less the first's and the pre-skip
    // (a tail scan through windows of the buffer, at most 64 KB and one
    // window back).
    const uint32_t size = c.size();
    const uint32_t win = c.cap() < 4096 ? c.cap() : 4096;
    for (uint32_t back = 0; back < 69632 && back < size;) {
      const uint32_t end = size - back;
      const uint32_t n = end < win ? end : win;
      const uint8_t* w = c.at(end - n, n);
      if (!w) break;
      bool found = false;
      for (uint32_t i = n >= 27 ? n - 27 + 1 : 0; i-- > 0;) {
        if (w[i] != 'O' || std::memcmp(w + i, "OggS", 4) != 0 || w[i + 4] != 0) continue;
        if (le32(w + i + 14) != serial) continue;
        uint64_t g = 0;
        for (int k = 7; k >= 0; --k) g = (g << 8) | w[i + 6 + k];
        if (g == ~0ull) continue;
        const uint64_t base = firstGranule + preskip;
        if (g > base) r.rec.durationMs = static_cast<uint32_t>(((g - base) * 1000 + 24000) / 48000);
        found = true;
        break;
      }
      if (found) break;
      back += n > 27 ? n - 27 : n;
    }
    return true;
  }
};

Result Scanner::scan(Source& src, Kind kind, uint8_t* buf, uint32_t bufBytes, const Limits& limits) {
  rec_.clear();
  stats_ = Stats();
  issues_ = 0;
  nFrames_ = 0;
  nItems_ = 0;
  seq_ = 0;
  std::memset(v1_, 0, sizeof(v1_));
  clearValue(value_);
  clearValue(spare_);
  bool ok = false;
  bool ioError = false;
  bool stopped = false;
  if (buf && bufBytes >= 512) {
    Cursor c(src, buf, bufBytes, limits, &stats_, &issues_);
    ScanCtx x(*this, c, limits, kind);
    switch (kind) {
      case Kind::Mp3: ok = x.scanMp3(); break;
      case Kind::Flac: ok = x.scanFlac(); break;
      case Kind::Opus: ok = x.scanOpus(); break;
      default: break;
    }
    ioError = c.ioError();
    // The budget never makes a file UNREADABLE: what was found stands.
    stopped = c.stopped() && !ioError;
    if ((ok || stopped) && !ioError) x.finishRecord(static_cast<uint8_t>(kind));
  }
  if (stopped) return Result::Partial;
  if (ok && !ioError) return Result::Ok;
  rec_.clear();
  rec_.rec.container = static_cast<uint8_t>(kind);
  rec_.rec.flags = mptg::kUnreadable;
  return ioError ? Result::ReadError : Result::Unreadable;
}

}  // namespace tagscan
