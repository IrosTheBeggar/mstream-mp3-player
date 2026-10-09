// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DancePack.h"

namespace dance {

static_assert(sizeof(Pack) <= 192, "a Pack may sit in internal RAM (D.1)");
static_assert(sizeof(Report) <= 48, "D.1");
static_assert(sizeof(Reason) == 20, "D.1: a 20-byte Reason");
static_assert(kMaxMoves <= 64, "one kept bit a move, in a uint64_t");

// The region views' reads are forced inline: open() reads every FDAT byte
// through u8 (A.10's dry run), and out of line that cost a windowed call per
// byte on the ESP32 (-Os doesn't inline it by itself).
#if defined(__GNUC__)
#define DANCE_VIEW_INLINE __attribute__((always_inline)) inline
#else
#define DANCE_VIEW_INLINE inline
#endif

// A region of the pack (the header, the directory or one chunk): every byte
// open() and scan() read comes through one. A read at an index outside it
// returns 0 and, with a counter, counts a fault instead of reading.
//
// Indices are 32-bit: every one the reader forms stays below 2^20 (a chunk
// offset is below 2^17, a string offset below 2^16 + 2^10, a STEP index
// times 8 below 8 * 65,567), so no sum of an index and a field's width wraps.
struct PackRegion {
  const uint8_t* base = nullptr;  // the pack's first byte
  uint32_t off = 0, len = 0;      // [off, off + len), inside the bytes present
  uint16_t* faults = nullptr;     // null: the accessors, which check their arguments first

  DANCE_VIEW_INLINE uint8_t u8(uint32_t i) const {
    if (i < len) return base[off + i];
    if (faults && *faults < 0xFFFF) ++*faults;
    return 0;
  }
  DANCE_VIEW_INLINE uint16_t u16(uint32_t i) const { return static_cast<uint16_t>(u8(i) | (u8(i + 1) << 8)); }
  DANCE_VIEW_INLINE uint32_t u32(uint32_t i) const {
    return static_cast<uint32_t>(u16(i)) | (static_cast<uint32_t>(u16(i + 2)) << 16);
  }
  const char* text(uint32_t i) const { return reinterpret_cast<const char*>(base + off + i); }
};

namespace {

constexpr uint8_t kRequired[9][4] = {{'M', 'E', 'T', 'A'}, {'G', 'E', 'O', 'M'}, {'P', 'A', 'L', 'S'},
                                     {'T', 'H', 'M', 'B'}, {'F', 'T', 'A', 'B'}, {'S', 'T', 'R', 'S'},
                                     {'M', 'O', 'V', 'S'}, {'S', 'T', 'E', 'P'}, {'F', 'D', 'A', 'T'}};
enum Chunk : int { kMeta, kGeom, kPals, kThmb, kFtab, kStrs, kMovs, kStep, kFdat, kChunkCount };

constexpr int kRecordBytes = 32;  // A.12: the reader takes each record's first 32 B

// A.1's validity over n bytes from a byte source: strict UTF-8 as Python's
// bytes.decode("utf-8") accepts it, and no NUL or control character
// (U+0000-U+001F, U+007F-U+009F).
template <class Get>
bool validBytes(Get get, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t c = get(i);
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7F) return false;
      ++i;
      continue;
    }
    size_t extra = 0;
    uint8_t lo = 0x80, hi = 0xBF;  // the second byte's range
    uint32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
      cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
      cp = c & 0x0Fu;
      if (c == 0xE0) lo = 0xA0;  // no overlong form
      if (c == 0xED) hi = 0x9F;  // no surrogate
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
      cp = c & 0x07u;
      if (c == 0xF0) lo = 0x90;  // no overlong form
      if (c == 0xF4) hi = 0x8F;  // nothing past U+10FFFF
    } else {
      return false;  // a continuation byte, C0, C1 or F5-FF
    }
    if (n - i <= extra) return false;  // cut by the end
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t ck = get(i + k);
      if (k == 1 ? (ck < lo || ck > hi) : (ck & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (ck & 0x3Fu);
    }
    if (cp >= 0x80 && cp <= 0x9F) return false;  // C1 controls
    i += extra + 1;
  }
  return true;
}

using View = PackRegion;

// A.1: the string at p inside the view (whose end is e), checked in A.1's
// order so no byte is read before a check puts it inside: p + 2 <= e; then
// n <= e - p - 2; then the NUL at p + 1 + n; then the n bytes valid text.
// *n is its length; it ends at p + 2 + n.
bool readString(const View& v, uint32_t p, uint32_t* n) {
  if (p > v.len || v.len - p < 2) return false;
  const uint32_t len = v.u8(p);
  if (len > v.len - p - 2) return false;
  if (v.u8(p + 1 + len) != 0) return false;
  if (!validBytes([&](size_t i) { return v.u8(p + 1 + static_cast<uint32_t>(i)); }, len)) return false;
  *n = len;
  return true;
}

// An id, a pose key or a move name: 1-23 B of [a-z0-9][a-z0-9-]* (A.5, A.11).
bool isKey(const View& v, uint32_t start, uint32_t n) {
  if (n < 1 || n > static_cast<uint32_t>(kMaxKeyBytes)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t c = v.u8(start + i);
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && !(i > 0 && c == '-')) return false;
  }
  return true;
}

// A.11: a string referenced by its offset o in STRS: o >= 4, then A.1's
// checks. *n is its length.
bool strsString(const View& strs, uint32_t o, uint32_t* n) {
  return o >= 4 && readString(strs, o, n);
}

// A.11: a move's name, by offset: a good STRS string and a key.
bool goodName(const View& strs, uint32_t o) {
  uint32_t n = 0;
  return strsString(strs, o, &n) && isKey(strs, o + 1, n);
}

// A.10's dry run of one frame's h rows at [off, off + len) of FDAT: each
// row's two bytes and n tokens inside the frame's bytes, skip + the runs <= w
// (a row with no tokens included), and exactly len bytes read.
bool dryRun(const View& fdat, uint32_t off, uint32_t len, uint32_t w, uint32_t h) {
  uint32_t p = off;
  const uint32_t end = off + len;  // A.9's bounds passed: no wrap
  for (uint32_t y = 0; y < h; ++y) {
    if (end - p < 2) return false;
    const uint32_t skip = fdat.u8(p), n = fdat.u8(p + 1);
    p += 2;
    if (n > end - p || skip > w) return false;
    uint32_t x = skip;
    for (uint32_t t = 0; t < n; ++t) {
      x += (fdat.u8(p + t) >> 4) + 1u;
      if (x > w) return false;
    }
    p += n;
  }
  return p == end;
}

MoveRecord readRecord(const View& movs, uint32_t r) {
  MoveRecord m;
  m.name = movs.u16(r + 0);
  m.kind = movs.u8(r + 2);
  m.beats = movs.u8(r + 3);
  m.paceLo = movs.u8(r + 4);
  m.paceHi = movs.u8(r + 5);
  m.weight10 = movs.u8(r + 6);
  m.when = movs.u8(r + 7);
  m.flags = movs.u8(r + 8);
  m.repeat = movs.u8(r + 9);
  m.needs = movs.u8(r + 10);
  m.travel = static_cast<int8_t>(movs.u8(r + 11));
  m.startAt = movs.u8(r + 12);
  m.travelEase = movs.u8(r + 13);
  m.bounce = movs.u8(r + 14);
  m.sway = movs.u8(r + 15);
  m.fromKey = movs.u8(r + 16);
  m.toKey = movs.u8(r + 17);
  m.fromKeyM = movs.u8(r + 18);
  m.toKeyM = movs.u8(r + 19);
  m.genreFirst = movs.u16(r + 20);
  m.genreCount = movs.u8(r + 22);
  m.stepCount = movs.u8(r + 23);
  m.stepFirst = movs.u16(r + 24);
  m.songLo = movs.u8(r + 26);
  m.songHi = movs.u8(r + 27);
  return m;
}

StepRecord readStep(const View& step, uint32_t index) {
  const uint32_t s = 8u * index;  // index <= 65,535 + 32: no wrap
  const uint16_t word = step.u16(s);
  StepRecord r;
  r.frame = static_cast<uint16_t>(word & 0x7FFFu);
  r.mirrored = (word & kFrameMirrored) != 0;
  r.dur = step.u16(s + 2);
  r.dx = static_cast<int8_t>(step.u8(s + 4));
  r.dy = static_cast<int8_t>(step.u8(s + 5));
  r.ease = step.u8(s + 6);
  r.go = (step.u8(s + 7) & kStepGo) != 0;
  return r;
}

Reason because(Why why, uint32_t a = 0, uint32_t b = 0) {
  Reason r;
  r.why = why;
  r.a = a;
  r.b = b;
  return r;
}

Reason becauseTag(Why why, const uint8_t* tag, const uint8_t* tag2 = nullptr) {
  Reason r;
  r.why = why;
  for (int i = 0; i < 4; ++i) {
    r.tag[i] = tag[i];
    if (tag2) r.tag2[i] = tag2[i];
  }
  return r;
}

uint32_t bits(int32_t v) {
  return static_cast<uint32_t>(v);  // two's complement, read back by formatReason
}

int abs8(int8_t v) { return v < 0 ? -static_cast<int>(v) : v; }

// ---- formatReason

// Each Why's text: %a and %b its numbers, %s and %t the same signed, %T and
// %U its tags, %M and %G a META or GEOM field, %X a needs byte in hex.
constexpr const char* kReasonText[] = {
    "",                                  // None
    "short read (%a of %b bytes)",       // ShortRead
    "too short",                         // TooShort
    "not an MPDN pack",                  // NotMpdn
    "needs a newer player",              // NeedsNewerPlayer
    "size %a isn't the file's %b",       // SizeMismatch
    "too big (%a bytes)",                // TooBig
    "bad checksum",                      // BadChecksum
    "bad chunk count %a",                // BadChunkCount
    "header unreadable",                 // HeaderUnreadable
    "chunk %T: misaligned",              // ChunkMisaligned
    "chunk %T: out of bounds",           // ChunkOutOfBounds
    "chunk %T twice",                    // ChunkTwice
    "chunks %T and %U overlap",          // ChunksOverlap
    "no %T chunk",                       // NoChunk
    "META: bad %M",                      // MetaBad
    "GEOM: too short",                   // GeomTooShort
    "GEOM: bad %G",                      // GeomBad
    "PALS: %a bytes, not 96",            // PalsSize
    "THMB: %a bytes, not 800",           // ThmbSize
    "FTAB: %a bytes for %b frames",      // FtabSize
    "frame %a: bad box",                 // FrameBadBox
    "frame %a: data out of bounds",      // FrameDataOutOfBounds
    "frame %a: bad data",                // FrameBadData
    "the rest frame is empty",           // RestFrameEmpty
    "STRS: too short",                   // StrsTooShort
    "STRS: bad pose key %a",             // StrsBadPoseKey
    "MOVS: too short",                   // MovsTooShort
    "MOVS: %a moves",                    // MovsCount
    "MOVS: record length %a",            // MovsRecordLength
    "STEP: %a bytes",                    // StepSize
    "bad name",                          // BadName
    "bad kind",                          // BadKind
    "bad steps",                         // BadSteps
    "frame %a out of range",             // FrameOutOfRange
    "%a beats",                          // Beats
    "step %a: %b ticks",                 // StepTicks
    "%a of %b ticks",                    // TickSum
    "step %a: %b ms",                    // StepMs
    "%a ms in all",                      // IdleTotal
    "beat fields on a no-BPM move",      // BeatFields
    "bad when",                          // BadWhen
    "pace %a-%b",                        // Pace
    "range %a-%b unreachable",           // RangeUnreachable
    "weight %a",                         // Weight
    "repeat %a",                         // Repeat
    "bounce %a",                         // Bounce
    "sway %a",                           // Sway
    "travel %s",                         // Travel
    "bad start_at",                      // BadStartAt
    "bad ease",                          // BadEase
    "bad place",                         // BadPlace
    "offset %s,%t",                      // Offset
    "bad pose key",                      // BadPoseKey
    "bad genre",                         // BadGenre
    "song_bpm needs track BPM data: skipped in this version",  // SongBpm
    "needs 0x%X",                        // Needs
    "travels from no spot",              // TravelsFromNoSpot
    "budget red (%a px)",                // BudgetRed
};
static_assert(sizeof(kReasonText) / sizeof(kReasonText[0]) == static_cast<size_t>(Why::BudgetRed) + 1,
              "one text per Why");

constexpr const char* kMetaFields[] = {"id", "name", "author", "licence", "url", "version", "count"};
constexpr const char* kGeomFields[] = {"scale",   "faces",   "cellW",       "cellH",     "anchorX",
                                       "anchorY", "shadowW", "frame count", "rest frame"};

// Writes into out, cut to cap - 1 bytes, while counting the whole length.
struct TextOut {
  char* out;
  size_t cap;
  size_t length = 0;

  void put(char c) {
    if (out && length + 1 < cap) out[length] = c;
    ++length;
  }
  void put(const char* s) {
    while (*s) put(*s++);
  }
  void number(uint64_t v) {
    char digits[20];
    int n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v);
    while (n) put(digits[--n]);
  }
  void signedNumber(uint32_t raw) {
    const int64_t v = raw >= 0x80000000u ? static_cast<int64_t>(raw) - 0x100000000LL : raw;
    if (v < 0) put('-');
    number(static_cast<uint64_t>(v < 0 ? -v : v));
  }
  void hex(uint32_t v) {  // "%02X"
    char digits[8];
    int n = 0;
    do {
      digits[n++] = "0123456789ABCDEF"[v & 15u];
      v >>= 4;
    } while (v);
    if (n < 2) digits[n++] = '0';
    while (n) put(digits[--n]);
  }
  void tag(const uint8_t* t) {
    for (int i = 0; i < 4; ++i) put(t[i] >= 0x20 && t[i] <= 0x7E ? static_cast<char>(t[i]) : '?');
  }
};

}  // namespace

size_t formatReason(const Reason& r, char* out, size_t cap) {
  TextOut w{out, cap};
  const auto index = static_cast<size_t>(r.why);
  if (index < sizeof(kReasonText) / sizeof(kReasonText[0])) {
    for (const char* f = kReasonText[index]; *f; ++f) {
      if (*f != '%') {
        w.put(*f);
        continue;
      }
      switch (*++f) {
        case 'a': w.number(r.a); break;
        case 'b': w.number(r.b); break;
        case 's': w.signedNumber(r.a); break;
        case 't': w.signedNumber(r.b); break;
        case 'T': w.tag(r.tag); break;
        case 'U': w.tag(r.tag2); break;
        case 'M': w.put(r.a < 7 ? kMetaFields[r.a] : "?"); break;
        case 'G': w.put(r.a < 9 ? kGeomFields[r.a] : "?"); break;
        case 'X': w.hex(r.a); break;
        default: break;
      }
    }
  }
  if (out && cap > 0) out[w.length < cap ? w.length : cap - 1] = '\0';
  return w.length;
}

bool validText(const char* s, size_t n) {
  if (n == 0) return true;
  if (!s) return false;
  return validBytes([s](size_t i) { return static_cast<uint8_t>(s[i]); }, n);
}

// ---- open() and scan()

PackRegion Pack::region(uint32_t off, uint32_t len, uint16_t* faults) const {
  PackRegion v;
  v.base = data_;
  v.off = off;
  v.len = len;
  v.faults = faults;
  return v;
}

void Pack::reset() {
  const uint16_t faults = faults_;
  *this = Pack();
  faults_ = faults;
}

void Pack::clear() { *this = Pack(); }

bool Pack::reject(Report* report, const Reason& why) {
  reset();
  if (report) {
    report->rejected = why;
    report->dropped = 0;
  }
  return false;
}

bool Pack::open(const uint8_t* data, uint32_t size, uint32_t avail, Report* report,
                const OpenOptions* options) {
  clear();
  if (report) {
    report->rejected = Reason();
    report->dropped = 0;
  }
  if (!data) avail = 0;  // data may be null only when avail is 0
  if (avail < size) return reject(report, because(Why::ShortRead, avail, size));
  data_ = data;  // only data[0, size) is read: size is the file's length
  return load(report, size, size, options, false);
}

bool Pack::scan(const uint8_t* buf, uint32_t avail, uint32_t size, Report* report) {
  clear();
  if (report) {
    report->rejected = Reason();
    report->dropped = 0;
  }
  if (!buf) avail = 0;  // buf may be null only when avail is 0
  if (avail > size) avail = size;
  data_ = buf;
  return load(report, size, avail, nullptr, true);
}

// A.15's pack rows in order (open), or 5.5's rows 1-8 (scan). data_ holds
// the bytes, `avail` of them present (open: all `size`).
bool Pack::load(Report* report, uint32_t size, uint32_t avail, const OpenOptions* options, bool scanning) {
  uint16_t* const faults = &faults_;
  if (avail < 32) return reject(report, because(Why::TooShort));
  const View head = region(0, 32, faults);
  if (head.u8(0) != 'M' || head.u8(1) != 'P' || head.u8(2) != 'D' || head.u8(3) != 'N') {
    return reject(report, because(Why::NotMpdn));
  }
  if (head.u16(6) > kReaderVersion) return reject(report, because(Why::NeedsNewerPlayer));
  const uint32_t total = head.u32(8);
  if (total != size) return reject(report, because(Why::SizeMismatch, total, size));
  if (total > kMaxPackBytes) return reject(report, because(Why::TooBig, total));
  if (!scanning && !(options && (options->flags & kSkipCrc))) {
    // The CRC's span is read directly: [16, size), every byte present.
    if (crc32(data_ + 16, size - 16) != head.u32(12)) return reject(report, because(Why::BadChecksum));
  }
  const uint32_t count = head.u16(16);
  if (count == 0 || count > static_cast<uint32_t>(kMaxChunks) || 32 + 12 * count > total) {
    return reject(report, because(Why::BadChunkCount, count));
  }
  const uint32_t dirEnd = 32 + 12 * count;
  if (scanning && dirEnd > avail) return reject(report, because(Why::HeaderUnreadable));
  const View dir = region(32, 12 * count, faults);

  // A.3, every entry, known or not.
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t e = 12 * i;
    const uint8_t tag[4] = {dir.u8(e), dir.u8(e + 1), dir.u8(e + 2), dir.u8(e + 3)};
    const uint32_t off = dir.u32(e + 4), len = dir.u32(e + 8);
    if (off % 4) return reject(report, becauseTag(Why::ChunkMisaligned, tag));
    // Off first, then the size against what is left, so off + len never wraps.
    if (off < dirEnd || off > total || len > total - off) {
      return reject(report, becauseTag(Why::ChunkOutOfBounds, tag));
    }
    for (uint32_t j = 0; j < i; ++j) {
      const uint32_t f = 12 * j;
      if (dir.u8(f) == tag[0] && dir.u8(f + 1) == tag[1] && dir.u8(f + 2) == tag[2] &&
          dir.u8(f + 3) == tag[3]) {
        return reject(report, becauseTag(Why::ChunkTwice, tag));
      }
    }
    for (uint32_t j = 0; j < i; ++j) {
      const uint32_t f = 12 * j;
      const uint32_t off2 = dir.u32(f + 4), len2 = dir.u32(f + 8);  // both <= total: no wrap
      if (len && len2 && off < off2 + len2 && off2 < off + len) {
        const uint8_t earlier[4] = {dir.u8(f), dir.u8(f + 1), dir.u8(f + 2), dir.u8(f + 3)};
        return reject(report, becauseTag(Why::ChunksOverlap, earlier, tag));
      }
    }
  }
  // The nine required chunks.
  uint32_t coff[kChunkCount] = {}, clen[kChunkCount] = {};
  for (int c = 0; c < kChunkCount; ++c) {
    bool found = false;
    for (uint32_t i = 0; i < count && !found; ++i) {
      const uint32_t e = 12 * i;
      if (dir.u8(e) == kRequired[c][0] && dir.u8(e + 1) == kRequired[c][1] &&
          dir.u8(e + 2) == kRequired[c][2] && dir.u8(e + 3) == kRequired[c][3]) {
        coff[c] = dir.u32(e + 4);
        clen[c] = dir.u32(e + 8);
        found = true;
      }
    }
    if (!found) return reject(report, becauseTag(Why::NoChunk, kRequired[c]));
  }
  if (scanning) {  // 5.5's row 5: off first, then the size, so nothing wraps
    for (int c = kMeta; c <= kThmb; ++c) {
      if (coff[c] > avail || clen[c] > avail - coff[c]) return reject(report, because(Why::HeaderUnreadable));
    }
  }

  // META (A.5)
  constexpr uint32_t kLimits[kMetaStrings][2] = {{1, 23}, {1, 24}, {1, 40}, {1, 40}, {0, 80}, {0, 16}};
  const View meta = region(coff[kMeta], clen[kMeta], faults);
  uint32_t p = 0;
  uint32_t metaText[kMetaStrings] = {};
  for (uint32_t f = 0; f < kMetaStrings; ++f) {
    uint32_t n = 0;
    if (!readString(meta, p, &n) || n < kLimits[f][0] || n > kLimits[f][1] ||
        (f == 0 && !isKey(meta, p + 1, n))) {
      return reject(report, because(Why::MetaBad, f));
    }
    metaText[f] = coff[kMeta] + p + 1;
    p += 2 + n;
  }
  if (p >= clen[kMeta]) return reject(report, because(Why::MetaBad, 6));
  const uint8_t declared = meta.u8(p);

  // GEOM (A.6), every field in table order
  const View geomView = region(coff[kGeom], clen[kGeom], faults);
  if (clen[kGeom] < 12) return reject(report, because(Why::GeomTooShort));
  PackGeom g;
  g.scale = geomView.u8(0);
  g.faces = geomView.u8(1);
  g.cellW = geomView.u8(2);
  g.cellH = geomView.u8(3);
  g.anchorX = geomView.u8(4);
  g.anchorY = geomView.u8(5);
  g.shadowW = geomView.u8(6);
  g.frameCount = geomView.u16(8);
  g.restFrame = geomView.u16(10);
  const int scale = g.scale;
  const bool good[9] = {
      scale >= 1 && scale <= kMaxScale,
      g.faces <= 2,
      g.cellW >= 1 && g.cellW * scale <= kMaxCellPx,
      g.cellH >= 1 && g.cellH * scale <= kMaxCellPx,
      g.anchorX < g.cellW,
      g.anchorY >= 1 && g.anchorY <= g.cellH && g.anchorY * scale <= kMaxAnchorPx &&
          (g.cellH - g.anchorY) * scale <= kMaxBelowGroundPx,
      g.shadowW <= kMaxShadowPx,
      g.frameCount >= 1 && g.frameCount <= kMaxFrames,
      (g.restFrame & 0x7FFFu) < g.frameCount,
  };
  for (uint32_t f = 0; f < 9; ++f) {
    if (!good[f]) return reject(report, because(Why::GeomBad, f));
  }

  // PALS and THMB
  if (clen[kPals] != static_cast<uint32_t>(kPalsBytes)) {
    return reject(report, because(Why::PalsSize, clen[kPals]));
  }
  if (clen[kThmb] != static_cast<uint32_t>(kThumbBytes)) {
    return reject(report, because(Why::ThmbSize, clen[kThmb]));
  }

  if (!scanning) {
    // FTAB, and every frame (A.9, A.10)
    const uint32_t frames = g.frameCount;
    if (clen[kFtab] != 12 * frames) return reject(report, because(Why::FtabSize, clen[kFtab], frames));
    const View ftab = region(coff[kFtab], clen[kFtab], faults);
    const View fdat = region(coff[kFdat], clen[kFdat], faults);
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t e = 12 * i;
      const uint32_t x0 = ftab.u8(e), y0 = ftab.u8(e + 1), w = ftab.u8(e + 2), h = ftab.u8(e + 3);
      const uint32_t off = ftab.u32(e + 4), len = ftab.u16(e + 8);
      if ((w == 0) != (h == 0) || (w == 0) != (len == 0) || x0 + w > g.cellW || y0 + h > g.cellH) {
        return reject(report, because(Why::FrameBadBox, i));
      }
      // Off first, then len, so a u32 + u16 sum never wraps.
      if (off > clen[kFdat] || len > clen[kFdat] - off) {
        return reject(report, because(Why::FrameDataOutOfBounds, i));
      }
      if (w && !dryRun(fdat, off, len, w, h)) return reject(report, because(Why::FrameBadData, i));
    }
    if (ftab.u8(12 * (g.restFrame & 0x7FFFu) + 2) == 0) return reject(report, because(Why::RestFrameEmpty));

    // STRS's head and pose keys (A.11)
    const View strs = region(coff[kStrs], clen[kStrs], faults);
    if (clen[kStrs] < 4) return reject(report, because(Why::StrsTooShort));
    const uint32_t keys = strs.u8(0);
    uint32_t q = 4;
    for (uint32_t i = 1; i <= keys; ++i) {
      uint32_t n = 0;
      if (!readString(strs, q, &n) || !isKey(strs, q + 1, n)) {
        return reject(report, because(Why::StrsBadPoseKey, i));
      }
      q += 2 + n;
    }

    // MOVS's head, in A.12's order
    const View movs = region(coff[kMovs], clen[kMovs], faults);
    if (clen[kMovs] < 4) return reject(report, because(Why::MovsTooShort));
    const uint32_t moves = movs.u8(0), recordLen = movs.u8(1);
    if (moves > static_cast<uint32_t>(kMaxMoves)) return reject(report, because(Why::MovsCount, moves));
    if (recordLen < static_cast<uint32_t>(kRecordBytes)) {
      return reject(report, because(Why::MovsRecordLength, recordLen));
    }
    if (clen[kMovs] < 4 + moves * recordLen) return reject(report, because(Why::MovsTooShort));

    // STEP
    if (clen[kStep] % 8) return reject(report, because(Why::StepSize, clen[kStep]));

    ftab_ = coff[kFtab];
    fdat_ = coff[kFdat];
    fdatLen_ = clen[kFdat];
    strs_ = coff[kStrs];
    strsLen_ = clen[kStrs];
    movs_ = coff[kMovs];
    step_ = coff[kStep];
    stepLen_ = clen[kStep];
    poseKeys_ = static_cast<uint8_t>(keys);
    moveCount_ = static_cast<uint8_t>(moves);
    recordLen_ = static_cast<uint8_t>(recordLen);
  }

  size_ = size;
  for (int f = 0; f < kMetaStrings; ++f) meta_[f] = metaText[f];
  declared_ = declared;
  geom_ = g;
  pals_ = coff[kPals];
  thmb_ = coff[kThmb];
  state_ = scanning ? State::Scanned : State::Open;
  if (scanning) return true;

  // Each move, in MOVS order (A.15's move rows; D.2's hook for the start
  // spots; the budget comes after open(), through dropMove).
  for (int i = 0; i < moveCount_; ++i) {
    Reason why = checkMove(i, faults);
    bool keep = why.why == Why::None;
    if (keep && options && options->late) {
      Reason late;
      keep = options->late(options->lateCtx, *this, i, &late);
      if (!keep) why = late;
    }
    if (keep) {
      kept_ |= uint64_t{1} << i;
      ++keptCount_;
    } else if (report) {
      ++report->dropped;
      if (report->onDrop) report->onDrop(report->ctx, i, why);
    }
  }
  return true;
}

// A.15's move rows down to `needs 0x<hh>`, in order: the first that fails.
Reason Pack::checkMove(int move, uint16_t* faults) const {
  const View movs = region(movs_, 4 + uint32_t{moveCount_} * recordLen_, faults);
  const View strs = region(strs_, strsLen_, faults);
  const View steps = region(step_, stepLen_, faults);
  const MoveRecord m = readRecord(movs, 4 + static_cast<uint32_t>(move) * recordLen_);
  const bool beat = m.kind == 0;

  if (!goodName(strs, m.name)) return because(Why::BadName);
  if (m.kind > 1) return because(Why::BadKind);
  if (m.stepCount == 0 || m.stepCount > kMaxSteps ||
      uint32_t{m.stepFirst} + m.stepCount > stepLen_ / 8) {
    return because(Why::BadSteps);
  }
  for (uint32_t k = 0; k < m.stepCount; ++k) {
    const StepRecord s = readStep(steps, m.stepFirst + k);
    if (s.frame >= geom_.frameCount) return because(Why::FrameOutOfRange, s.frame);
  }
  int32_t sum = 0;  // at most 32 * 65535
  if (beat) {
    if (m.beats != 1 && m.beats != 2 && m.beats != 4 && m.beats != 8) return because(Why::Beats, m.beats);
    for (uint32_t k = 0; k < m.stepCount; ++k) {
      const uint32_t d = readStep(steps, m.stepFirst + k).dur;
      if (d < static_cast<uint32_t>(kMinStepTicks)) return because(Why::StepTicks, k + 1, d);
      sum += static_cast<int32_t>(d);
    }
    const int32_t need = kTicksPerBeat * m.beats;
    if (sum != need) return because(Why::TickSum, static_cast<uint32_t>(sum), static_cast<uint32_t>(need));
  } else {
    for (uint32_t k = 0; k < m.stepCount; ++k) {
      const uint32_t d = readStep(steps, m.stepFirst + k).dur;
      if (d < static_cast<uint32_t>(kMinIdleStep) || d > static_cast<uint32_t>(kMaxIdleStep)) {
        return because(Why::StepMs, k + 1, 10 * d);
      }
      sum += static_cast<int32_t>(d);
    }
    if (sum < kMinIdleTotal || sum > kMaxIdleTotal) {
      return because(Why::IdleTotal, 10 * static_cast<uint32_t>(sum));
    }
    if (m.beats || m.paceLo || m.paceHi || m.repeat || m.bounce || m.sway || m.travel || m.genreCount ||
        (m.flags & (kFreeze | kPlaceStart | kPlaceEnd))) {
      return because(Why::BeatFields);
    }
  }
  if (beat ? m.when != 0 : (m.when < 1 || m.when > 3)) return because(Why::BadWhen);
  if (beat && !(m.paceLo == 0 && m.paceHi == 0)) {
    if (m.paceLo < kMinPace || m.paceLo > kMaxPace || m.paceHi < kMinPace || m.paceHi > kMaxPace ||
        m.paceLo > m.paceHi) {
      return because(Why::Pace, m.paceLo, m.paceHi);
    }
    if (!reach(m.paceLo, m.paceHi, m.beats, m.travel != 0, nullptr, nullptr)) {
      return because(Why::RangeUnreachable, m.paceLo, m.paceHi);
    }
  }
  if (m.weight10 == 0 || m.weight10 > kMaxWeight10) return because(Why::Weight, m.weight10);
  if (m.repeat > kMaxRepeat || m.repeat * m.beats > kMaxRepeatBeats) return because(Why::Repeat, m.repeat);
  if (m.bounce > kMaxBounce) return because(Why::Bounce, m.bounce);
  if (m.sway > kMaxSway) return because(Why::Sway, m.sway);
  if (m.travel < -kMaxTravel || m.travel > kMaxTravel || (m.travel != 0 && m.beats < 2)) {
    return because(Why::Travel, bits(m.travel));
  }
  if (m.startAt >= kStartAtCount) return because(Why::BadStartAt);
  bool easeBad = m.travelEase >= kEaseCount;
  for (uint32_t k = 0; k < m.stepCount && !easeBad; ++k) {
    easeBad = readStep(steps, m.stepFirst + k).ease >= kEaseCount;
  }
  if (easeBad) return because(Why::BadEase);
  if ((m.flags & kPlaceStart) && (m.flags & kPlaceEnd)) return because(Why::BadPlace);
  for (uint32_t k = 0; k < m.stepCount; ++k) {
    const StepRecord s = readStep(steps, m.stepFirst + k);
    if (abs8(s.dx) * geom_.scale > kMaxOffsetPx || abs8(s.dy) * geom_.scale > kMaxOffsetPx) {
      return because(Why::Offset, bits(s.dx), bits(s.dy));
    }
  }
  if (m.fromKey > poseKeys_ || m.toKey > poseKeys_ || m.fromKeyM > poseKeys_ || m.toKeyM > poseKeys_) {
    return because(Why::BadPoseKey);
  }
  if (m.genreCount > kMaxGenreTags) return because(Why::BadGenre);
  uint32_t o = m.genreFirst;
  for (uint32_t t = 0; t < m.genreCount; ++t) {
    uint32_t n = 0;
    if (!strsString(strs, o, &n) || n < 1 || n > static_cast<uint32_t>(kMaxTagBytes)) {
      return because(Why::BadGenre);
    }
    // B.8: a tag folding to nothing drops the move. The tag is copied out of
    // STRS's view (at most 63 bytes, by the row above) and folded from the
    // copy. The copy is bounded by the buffer too, so the stack never
    // depends on that row.
    char tag[kMaxTagBytes + 1];
    static_assert(sizeof(tag) == static_cast<size_t>(kMaxTagBytes) + 1, "a tag and its NUL");
    const uint32_t copied = n < sizeof(tag) - 1 ? n : static_cast<uint32_t>(sizeof(tag) - 1);
    for (uint32_t i = 0; i < copied; ++i) tag[i] = static_cast<char>(strs.u8(o + 1 + i));
    tag[copied] = '\0';
    if (!genreKey(tag, nullptr)) return because(Why::BadGenre);
    o += 2 + n;
  }
  if (m.needs & kNeedsSongBpm) return because(Why::SongBpm);
  if (m.needs) return because(Why::Needs, m.needs);
  return Reason();
}

// ---- the accessors

Pack::State Pack::state() const { return state_; }
uint32_t Pack::size() const { return state_ == State::None ? 0 : size_; }
uint16_t Pack::regionFaults() const { return faults_; }

const char* Pack::id() const {
  return state_ == State::None ? nullptr : reinterpret_cast<const char*>(data_ + meta_[0]);
}
const char* Pack::name() const {
  return state_ == State::None ? nullptr : reinterpret_cast<const char*>(data_ + meta_[1]);
}
const char* Pack::author() const {
  return state_ == State::None ? nullptr : reinterpret_cast<const char*>(data_ + meta_[2]);
}
const char* Pack::licence() const {
  return state_ == State::None ? nullptr : reinterpret_cast<const char*>(data_ + meta_[3]);
}
const char* Pack::url() const {
  return state_ == State::None ? nullptr : reinterpret_cast<const char*>(data_ + meta_[4]);
}
const char* Pack::version() const {
  return state_ == State::None ? nullptr : reinterpret_cast<const char*>(data_ + meta_[5]);
}
uint8_t Pack::declaredMoves() const { return state_ == State::None ? 0 : declared_; }
const PackGeom& Pack::geom() const { return geom_; }  // zeroed in State::None
const uint8_t* Pack::palette() const { return state_ == State::None ? nullptr : data_ + pals_; }
const uint8_t* Pack::idlePalette() const { return state_ == State::None ? nullptr : data_ + pals_ + 48; }
const uint8_t* Pack::thumb() const { return state_ == State::None ? nullptr : data_ + thmb_; }

int Pack::frameCount() const { return state_ == State::Open ? geom_.frameCount : 0; }

FrameBox Pack::box(int frame) const {
  FrameBox b = {0, 0, 0, 0};
  if (state_ != State::Open || frame < 0 || frame >= geom_.frameCount) return b;
  const View ftab = region(ftab_, 12u * geom_.frameCount, nullptr);
  const uint32_t e = 12u * static_cast<uint32_t>(frame);
  b.x0 = ftab.u8(e);
  b.y0 = ftab.u8(e + 1);
  b.w = ftab.u8(e + 2);
  b.h = ftab.u8(e + 3);
  return b;
}

PackFrame Pack::frame(int frame) const {
  PackFrame f = {{0, 0, 0, 0}, nullptr, 0};
  if (state_ != State::Open || frame < 0 || frame >= geom_.frameCount) return f;
  const View ftab = region(ftab_, 12u * geom_.frameCount, nullptr);
  const uint32_t e = 12u * static_cast<uint32_t>(frame);
  f.box = box(frame);
  const uint32_t off = ftab.u32(e + 4);  // open() checked off <= FDAT's size
  f.data = data_ + fdat_ + off;
  f.len = ftab.u16(e + 8);
  return f;
}

int Pack::poseKeyCount() const { return state_ == State::Open ? poseKeys_ : 0; }

const char* Pack::poseKey(int id) const {
  if (state_ != State::Open || id < 1 || id > poseKeys_) return nullptr;
  const View strs = region(strs_, strsLen_, nullptr);
  uint32_t p = 4;
  for (int i = 1; i <= id; ++i) {
    uint32_t n = 0;
    if (!readString(strs, p, &n)) return nullptr;  // open() checked every key
    if (i == id) return strs.text(p + 1);
    p += 2 + n;
  }
  return nullptr;
}

int Pack::moveCount() const { return state_ == State::Open ? moveCount_ : 0; }
int Pack::keptMoves() const { return state_ == State::Open ? keptCount_ : 0; }

bool Pack::moveKept(int move) const {
  return state_ == State::Open && move >= 0 && move < moveCount_ && ((kept_ >> move) & 1u);
}

bool Pack::dropMove(int move, const Reason& why, Report* report) {
  if (!moveKept(move)) return false;
  kept_ &= ~(uint64_t{1} << move);
  --keptCount_;
  if (report) {
    ++report->dropped;
    if (report->onDrop) report->onDrop(report->ctx, move, why);
  }
  return true;
}

MoveRecord Pack::moveRecord(int move) const {
  if (state_ != State::Open || move < 0 || move >= moveCount_) return MoveRecord{};
  const View movs = region(movs_, 4 + uint32_t{moveCount_} * recordLen_, nullptr);
  return readRecord(movs, 4 + static_cast<uint32_t>(move) * recordLen_);
}

const char* Pack::moveName(int move) const {
  if (state_ != State::Open || move < 0 || move >= moveCount_) return nullptr;
  const View strs = region(strs_, strsLen_, nullptr);
  const uint16_t o = moveRecord(move).name;
  return goodName(strs, o) ? strs.text(o + 1u) : nullptr;
}

StepRecord Pack::step(int move, int k) const {
  if (state_ != State::Open || move < 0 || move >= moveCount_ || k < 0) return StepRecord{};
  const MoveRecord m = moveRecord(move);
  if (k >= m.stepCount || uint32_t{m.stepFirst} + m.stepCount > stepLen_ / 8) return StepRecord{};
  return readStep(region(step_, stepLen_, nullptr), m.stepFirst + static_cast<uint32_t>(k));
}

const char* Pack::genreTag(int move, int g) const {
  if (state_ != State::Open || move < 0 || move >= moveCount_ || g < 0) return nullptr;
  const MoveRecord m = moveRecord(move);
  if (g >= m.genreCount) return nullptr;
  const View strs = region(strs_, strsLen_, nullptr);
  uint32_t o = m.genreFirst;
  for (int t = 0; t <= g; ++t) {
    uint32_t n = 0;
    if (!strsString(strs, o, &n) || n < 1 || n > static_cast<uint32_t>(kMaxTagBytes)) return nullptr;
    if (t == g) return strs.text(o + 1);
    o += 2 + n;
  }
  return nullptr;
}

}  // namespace dance
