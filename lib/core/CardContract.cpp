// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardContract.h"

#include <cstdio>
#include <cstring>

#include "ThumbCache.h"  // thumbfile::path: the transfer thumbnails are named as the device's

namespace cardcontract {

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kCrcPoly = 0xEDB88320u;

struct CrcTable {
  uint32_t v[256];
  constexpr CrcTable() : v() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ kCrcPoly : c >> 1;
      v[i] = c;
    }
  }
};
constexpr CrcTable kCrc;  // 1 KB of rodata, built by the compiler

// a(x) b(x) mod p(x), both reflected (zlib's multmodp).
uint32_t multModP(uint32_t a, uint32_t b) {
  uint32_t m = 1u << 31, p = 0;
  for (;;) {
    if (a & m) {
      p ^= b;
      if ((a & (m - 1)) == 0) break;
    }
    m >>= 1;
    b = (b & 1) ? (b >> 1) ^ kCrcPoly : b >> 1;
  }
  return p;
}

}  // namespace

uint32_t crc32(const void* data, size_t n, uint32_t crc) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = kCrc.v[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t crc32Combine(uint32_t crcA, uint32_t crcB, uint64_t lenB) {
  // crcA shifted by lenB zero bytes is crcA times x^(8 lenB) mod p: square
  // x^1 up through the bits of 8 lenB (zlib's x2nmodp).
  uint32_t xp = 1u << 31;  // x^0
  uint32_t sq = 1u << 30;  // x^1, then x^2, x^4, ...
  uint64_t n = lenB * 8;
  while (n) {
    if (n & 1) xp = multModP(sq, xp);
    n >>= 1;
    sq = multModP(sq, sq);
  }
  return multModP(xp, crcA) ^ crcB;
}

// ---------------------------------------------------------------------------
// FNV-1a 64, the path hash, the server's key
// ---------------------------------------------------------------------------
uint64_t fnv1a64(const void* data, size_t n, uint64_t h) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

uint64_t fnv1a64Str(const char* s, uint64_t h) { return s ? fnv1a64(s, std::strlen(s), h) : h; }

uint64_t pathHash(const char* rel, size_t len) {
  if (!rel || len == 0) return kMusicHash;
  return fnv1a64(rel, len, fnv1a64("/", 1, kMusicHash));
}

uint64_t pathHash(const char* rel) { return pathHash(rel, rel ? std::strlen(rel) : 0); }

namespace {
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
}  // namespace

size_t normaliseServerUrl(const char* url, char* out, size_t cap) {
  if (cap) out[0] = 0;
  if (!url || !cap) return 0;
  const char* sep = std::strstr(url, "://");
  if (!sep || sep == url) return 0;
  size_t n = 0;
  auto putc = [&](char c) {
    if (n + 1 < cap) out[n] = c;
    ++n;
  };
  for (const char* p = url; p < sep; ++p) putc(lowerAscii(*p));
  const bool http = (sep - url == 4) && (n < cap) && std::strncmp(out, "http", 4) == 0;
  const bool https = (sep - url == 5) && (n < cap) && std::strncmp(out, "https", 5) == 0;
  putc(':');
  putc('/');
  putc('/');
  // The authority runs to the first '/', '?' or '#'; the user name and
  // password end at its last '@'.
  const char* auth = sep + 3;
  const char* authEnd = auth;
  while (*authEnd && *authEnd != '/' && *authEnd != '?' && *authEnd != '#') ++authEnd;
  for (const char* p = auth; p < authEnd; ++p)
    if (*p == '@') auth = p + 1;
  // The port: after the last ':' that isn't inside an IPv6 literal's [].
  const char* hostEnd = authEnd;
  for (const char* p = authEnd; p > auth; --p) {
    if (p[-1] == ']') break;
    if (p[-1] == ':') {
      hostEnd = p - 1;
      break;
    }
  }
  if (hostEnd == auth) return 0;  // no host
  for (const char* p = auth; p < hostEnd; ++p) putc(lowerAscii(*p));
  if (hostEnd < authEnd) {
    uint64_t port = 0;
    bool digits = true, any = false;
    for (const char* p = hostEnd + 1; p < authEnd; ++p) {
      if (*p < '0' || *p > '9') {
        digits = false;
        break;
      }
      any = true;
      if (port < 1000000) port = port * 10 + static_cast<uint64_t>(*p - '0');
    }
    if (!digits) return 0;
    if (any && !((http && port == 80) || (https && port == 443))) {
      char num[24];
      std::snprintf(num, sizeof(num), ":%llu", static_cast<unsigned long long>(port));
      for (const char* p = num; *p; ++p) putc(*p);
    }
  }
  // The path, to the query or the fragment, without its trailing slashes.
  const char* path = authEnd;
  const char* pathEnd = path;
  while (*pathEnd && *pathEnd != '?' && *pathEnd != '#') ++pathEnd;
  while (pathEnd > path && pathEnd[-1] == '/') --pathEnd;
  for (const char* p = path; p < pathEnd; ++p) putc(*p);
  if (n + 1 > cap) {
    out[0] = 0;
    return 0;
  }
  out[n] = 0;
  return n;
}

uint64_t serverUrlKey(const char* url) {
  char buf[1024];
  const size_t n = normaliseServerUrl(url, buf, sizeof(buf));
  return n ? fnv1a64(buf, n, kFnvBasis) : 0;
}

// ---------------------------------------------------------------------------
// qfp
// ---------------------------------------------------------------------------
QfpRanges qfpRanges(uint32_t size) {
  QfpRanges r;
  r.headBytes = size < kQfpPart ? size : kQfpPart;
  if (size > kQfpPart) {
    r.tailOffset = size - kQfpPart > kQfpPart ? size - kQfpPart : kQfpPart;
    r.tailBytes = size - r.tailOffset;
  } else {
    r.tailOffset = size;
  }
  return r;
}

uint64_t qfp(uint32_t size, const uint8_t* head, const uint8_t* tail) {
  const QfpRanges r = qfpRanges(size);
  uint8_t sz[8];
  put64(sz, size);
  uint64_t h = fnv1a64(sz, 8);
  if (r.headBytes) h = fnv1a64(head, r.headBytes, h);
  if (r.tailBytes) h = fnv1a64(tail, r.tailBytes, h);
  return h;
}

// ---------------------------------------------------------------------------
// FAT time and the skew
// ---------------------------------------------------------------------------
namespace {

bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int monthDays(int y, int m) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leapYear(y) ? 29 : kDays[m - 1];
}

// Days from 1980-01-01 to y-m-d (a valid date from 1980 on).
int64_t daysSince1980(int y, int m, int d) {
  int64_t days = 0;
  for (int yy = 1980; yy < y; ++yy) days += leapYear(yy) ? 366 : 365;
  for (int mm = 1; mm < m; ++mm) days += monthDays(y, mm);
  return days + d - 1;
}

struct Fields {
  int year, month, day, hour, minute, sec2;
};

Fields split(uint32_t t) {
  const uint32_t date = t >> 16, time = t & 0xFFFF;
  return Fields{static_cast<int>(1980 + (date >> 9)), static_cast<int>((date >> 5) & 15), static_cast<int>(date & 31),
                static_cast<int>(time >> 11), static_cast<int>((time >> 5) & 63), static_cast<int>(time & 31)};
}

bool fieldsValid(const Fields& f) {
  return f.month >= 1 && f.month <= 12 && f.day >= 1 && f.day <= monthDays(f.year, f.month) && f.hour < 24 &&
         f.minute < 60 && f.sec2 < 30;
}

}  // namespace

uint32_t fatTime(int year, int month, int day, int hour, int minute, int second) {
  if (year < 1980 || year > 2107 || month < 1 || month > 12 || day < 1 || day > monthDays(year, month) ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    return 0;
  const uint32_t date = static_cast<uint32_t>((year - 1980) << 9 | month << 5 | day);
  const uint32_t time = static_cast<uint32_t>(hour << 11 | minute << 5 | second / 2);
  return date << 16 | time;
}

bool fatTimeValid(uint32_t t) { return t != 0 && fieldsValid(split(t)); }

int64_t fatWallSeconds(uint32_t t) {
  if (!fatTimeValid(t)) return -1;
  const Fields f = split(t);
  return daysSince1980(f.year, f.month, f.day) * 86400 + f.hour * 3600 + f.minute * 60 + 2 * f.sec2;
}

uint32_t fatTimeFromWall(int64_t w) {
  if (w < 0) return 0;
  int64_t days = w / 86400;
  const int64_t rem = w % 86400;
  int y = 1980;
  for (;; ++y) {
    const int64_t len = leapYear(y) ? 366 : 365;
    if (days < len) break;
    days -= len;
    if (y >= 2107) return 0;
  }
  int m = 1;
  while (days >= monthDays(y, m)) days -= monthDays(y, m++);
  return fatTime(y, m, static_cast<int>(days) + 1, static_cast<int>(rem / 3600), static_cast<int>(rem / 60 % 60),
                 static_cast<int>(rem % 60));
}

bool SkewHistogram::add(uint32_t recorded, uint32_t observed) {
  const int64_t r = fatWallSeconds(recorded), o = fatWallSeconds(observed);
  if (r < 0 || o < 0) return false;
  ++pairs_;
  const int64_t delta = o - r;
  if (delta == 0) return true;
  const int32_t half = static_cast<int32_t>(delta / 2);  // W is even, |delta| < 2^32
  for (uint32_t i = 0; i < n_; ++i) {
    if (half_[i] == half) {
      ++count_[i];
      return true;
    }
  }
  if (n_ < kMaxDeltas) {
    half_[n_] = half;
    count_[n_] = 1;
    ++n_;
    return true;
  }
  // Every slot taken: one from each count, the slots at 0 freed, and this
  // delta not kept (Misra-Gries). (add() is the first pass: it comes before
  // any recount.)
  summary_ = true;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < n_; ++i) {
    if (--count_[i] == 0) continue;
    half_[kept] = half_[i];
    count_[kept] = count_[i];
    ++kept;
  }
  n_ = kept;
  return true;
}

void SkewHistogram::beginRecount() {
  for (uint32_t i = 0; i < n_; ++i) count_[i] = 0;
  recounted_ = true;
}

void SkewHistogram::recount(int64_t delta) {
  if (!recounted_ || delta == 0) return;
  const int32_t half = static_cast<int32_t>(delta / 2);
  for (uint32_t i = 0; i < n_; ++i) {
    if (half_[i] == half) {
      ++count_[i];
      return;
    }
  }
}

int32_t SkewHistogram::skew() const {
  // The most frequent non-zero delta; ties to the smaller |D|, then the
  // negative one. Only that one is tried: when it fails a condition there is
  // no skew (a second choice would be a guess). On a summary not counted
  // again, the counts are lower bounds, so a delta passing with its own is D
  // (half the pairs: no other delta can tie it, more than 256 others came)
  // and anything else is no skew.
  uint32_t best = n_;
  for (uint32_t i = 0; i < n_; ++i) {
    if (best == n_) {
      best = i;
      continue;
    }
    const int64_t a = half_[i], b = half_[best];
    const int64_t absA = a < 0 ? -a : a, absB = b < 0 ? -b : b;
    if (count_[i] > count_[best] || (count_[i] == count_[best] && (absA < absB || (absA == absB && a < b)))) best = i;
  }
  if (best == n_) return 0;
  const int64_t d = static_cast<int64_t>(half_[best]) * 2;
  const uint32_t c = count_[best];
  if (c < kMinPairs || static_cast<uint64_t>(c) * 2 < pairs_) return 0;
  if (d > kMaxSkew || d < -kMaxSkew || d % kSkewStep != 0) return 0;
  return static_cast<int32_t>(d);
}

bool timeMatches(uint32_t recorded, uint32_t observed, int32_t skew) {
  const int64_t r = fatWallSeconds(recorded), o = fatWallSeconds(observed);
  if (r < 0 || o < 0) return false;
  const int64_t delta = o - r;
  return delta == 0 || (skew != 0 && delta == skew);
}

// ---------------------------------------------------------------------------
// The number rule
// ---------------------------------------------------------------------------
namespace {

bool asciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

void trim(const char*& s, size_t& n) {
  while (n && asciiSpace(*s)) ++s, --n;
  while (n && asciiSpace(s[n - 1])) --n;
}

// [+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+) at `scale` fraction digits, rounded half
// away from zero. The magnitude stops growing past `limit` (it is then only
// known to be over it), so a long digit string can't overflow.
bool decimal(const char* s, size_t n, int scale, uint64_t limit, bool* negative, uint64_t* magnitude) {
  size_t i = 0;
  *negative = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) *negative = s[i++] == '-';
  uint64_t m = 0;
  size_t intDigits = 0, fracDigits = 0;
  bool over = false;
  auto push = [&](int d) {
    if (over) return;
    m = m * 10 + static_cast<uint64_t>(d);
    if (m > limit) over = true;
  };
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    push(s[i] - '0');
    ++intDigits;
    ++i;
  }
  int round = 0;
  if (i < n && s[i] == '.') {
    ++i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (static_cast<int>(fracDigits) < scale)
        push(s[i] - '0');
      else if (static_cast<int>(fracDigits) == scale)
        round = s[i] >= '5' ? 1 : 0;
      ++fracDigits;
      ++i;
    }
  }
  if (i != n || (intDigits == 0 && fracDigits == 0)) return false;
  for (int k = static_cast<int>(fracDigits); k < scale; ++k) push(0);
  if (!over && round) {
    ++m;
    if (m > limit) over = true;
  }
  *magnitude = over ? limit + 1 : m;
  return true;
}

}  // namespace

bool bpm10FromText(const char* s, size_t n, uint16_t* out) {
  trim(s, n);
  bool neg;
  uint64_t m;
  if (!decimal(s, n, 0, 1000, &neg, &m)) return false;
  if (neg && m != 0) return false;  // below 20 anyway
  if (m < 20 || m > 300) return false;
  *out = static_cast<uint16_t>(m * 10);
  return true;
}

bool gainFromText(const char* s, size_t n, int16_t* out) {
  trim(s, n);
  if (n >= 2 && (s[n - 2] == 'd' || s[n - 2] == 'D') && (s[n - 1] == 'b' || s[n - 1] == 'B')) {
    n -= 2;
    trim(s, n);
  }
  bool neg;
  uint64_t m;
  if (!decimal(s, n, 2, 40000, &neg, &m)) return false;
  if (neg ? m > 32768 : m > 32767) return false;
  *out = static_cast<int16_t>(neg ? -static_cast<int64_t>(m) : static_cast<int64_t>(m));
  return true;
}

bool peakFromText(const char* s, size_t n, uint16_t* out) {
  trim(s, n);
  bool neg;
  uint64_t m;
  if (!decimal(s, n, 4, 65535, &neg, &m)) return false;
  if (neg && m != 0) return false;
  *out = static_cast<uint16_t>(m > 65535 ? 65535 : m);
  return true;
}

int16_t r128ToGain(int16_t q) {
  // (q + 1280) x 25 / 64, half away from zero, in integers: |x| x 25 + 32
  // floored by 64 rounds the magnitude half up.
  const int32_t x = static_cast<int32_t>(q) + 1280;
  const int32_t mag = ((x < 0 ? -x : x) * 25 + 32) / 64;
  return static_cast<int16_t>(x < 0 ? -mag : mag);
}

bool r128FromText(const char* s, size_t n, int16_t* out) {
  trim(s, n);
  size_t i = 0;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
  if (i == n) return false;
  int32_t m = 0;
  for (; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    if (m <= 32768) m = m * 10 + (s[i] - '0');
  }
  if (neg ? m > 32768 : m > 32767) return false;
  *out = r128ToGain(static_cast<int16_t>(neg ? -m : m));
  return true;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
namespace {

// Writes code point `cp` as UTF-8 if it fits whole before `cap - 1`.
bool putCp(uint32_t cp, char* out, size_t cap, size_t& n) {
  char b[4];
  size_t k;
  if (cp < 0x80) {
    b[0] = static_cast<char>(cp);
    k = 1;
  } else if (cp < 0x800) {
    b[0] = static_cast<char>(0xC0 | cp >> 6);
    b[1] = static_cast<char>(0x80 | (cp & 0x3F));
    k = 2;
  } else if (cp < 0x10000) {
    b[0] = static_cast<char>(0xE0 | cp >> 12);
    b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    b[2] = static_cast<char>(0x80 | (cp & 0x3F));
    k = 3;
  } else {
    b[0] = static_cast<char>(0xF0 | cp >> 18);
    b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    b[3] = static_cast<char>(0x80 | (cp & 0x3F));
    k = 4;
  }
  if (n + k + 1 > cap) return false;
  std::memcpy(out + n, b, k);
  n += k;
  return true;
}

constexpr uint32_t kReplacement = 0xFFFD;

DecodeResult finish(char* out, size_t cap, size_t n, bool cut) {
  DecodeResult r;
  if (cap) out[n] = 0;
  r.length = n;
  r.cut = cut;
  return r;
}

}  // namespace

DecodeResult latin1ToUtf8(const uint8_t* in, size_t n, char* out, size_t cap) {
  while (n && in[n - 1] == 0) --n;
  size_t w = 0;
  for (size_t i = 0; i < n; ++i)
    if (!putCp(in[i], out, cap, w)) return finish(out, cap, w, true);
  return finish(out, cap, w, false);
}

DecodeResult utf8Lossy(const uint8_t* in, size_t n, char* out, size_t cap) {
  // Unicode's "maximal subpart" practice, as Rust's from_utf8_lossy: a lead
  // byte and the continuation bytes that could still begin a valid
  // sequence after it are one invalid subpart, one U+FFFD.
  size_t w = 0, i = 0;
  while (i < n) {
    const uint8_t b = in[i];
    if (b < 0x80) {
      if (!putCp(b, out, cap, w)) return finish(out, cap, w, true);
      ++i;
      continue;
    }
    size_t need = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
      need = 1;
    } else if (b >= 0xE0 && b <= 0xEF) {
      need = 2;
      if (b == 0xE0) lo = 0xA0;
      if (b == 0xED) hi = 0x9F;
    } else if (b >= 0xF0 && b <= 0xF4) {
      need = 3;
      if (b == 0xF0) lo = 0x90;
      if (b == 0xF4) hi = 0x8F;
    }
    size_t k = 1;
    bool ok = need > 0;
    while (ok && k <= need) {
      if (i + k >= n) {
        ok = false;
        break;
      }
      const uint8_t c = in[i + k];
      const uint8_t l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
      if (c < l || c > h) {
        ok = false;
        break;
      }
      ++k;
    }
    if (ok) {
      if (w + need + 2 > cap) return finish(out, cap, w, true);
      std::memcpy(out + w, in + i, need + 1);
      w += need + 1;
      i += need + 1;
    } else {
      if (!putCp(kReplacement, out, cap, w)) return finish(out, cap, w, true);
      i += k;  // the lead byte and the continuation bytes that fitted
    }
  }
  return finish(out, cap, w, false);
}

DecodeResult utf16ToUtf8(const uint8_t* in, size_t n, bool bigEndian, char* out, size_t cap) {
  size_t w = 0;
  auto unit = [&](size_t i) {
    return static_cast<uint32_t>(bigEndian ? (in[i] << 8 | in[i + 1]) : (in[i] | in[i + 1] << 8));
  };
  size_t i = 0;
  for (; i + 1 < n; i += 2) {
    uint32_t cp = unit(i);
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
      const uint32_t lo = unit(i + 2);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      } else {
        cp = kReplacement;
      }
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = kReplacement;
    }
    if (!putCp(cp, out, cap, w)) return finish(out, cap, w, true);
  }
  if (i < n && !putCp(kReplacement, out, cap, w)) return finish(out, cap, w, true);  // the stray byte
  return finish(out, cap, w, false);
}

size_t utf8CutLength(const char* s, size_t len, size_t max) {
  if (len <= max) return len;
  size_t k = max;
  while (k > 0 && (static_cast<uint8_t>(s[k]) & 0xC0) == 0x80) --k;
  return k;
}

void FieldBuilder::clear() {
  len_ = 0;
  values_ = 0;
  ended_ = false;
  truncated_ = false;
  buf_[0] = 0;
}

void FieldBuilder::add(const char* value) { add(value, value ? std::strlen(value) : 0); }

void FieldBuilder::add(const char* value, size_t n) {
  if (ended_ || n == 0 || !value) return;  // a value empty even before its control characters go
  if (!list_ && values_ == 1) return;      // a single field is its first value
  // Control characters to spaces, then the cut to 255 bytes.
  char v[kValueMax];
  const size_t keep = utf8CutLength(value, n, kValueMax);
  const bool cut = keep < n;
  for (size_t i = 0; i < keep; ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    v[i] = (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
  }
  if (keep == 0) {
    // Only a cut can empty a non-empty value (a code point over 255 bytes
    // can't be): nothing is left of it.
    truncated_ = truncated_ || cut;
    return;
  }
  if (cut) truncated_ = true;
  if (!list_) {
    std::memcpy(buf_, v, keep);
    len_ = keep;
    buf_[len_] = 0;
    values_ = 1;
    return;
  }
  // A byte-identical repeat of a value kept is dropped.
  size_t start = 0;
  for (uint32_t k = 0; k < values_; ++k) {
    size_t end = start;
    while (end < len_ && buf_[end] != kSeparator) ++end;
    if (end - start == keep && std::memcmp(buf_ + start, v, keep) == 0) return;
    start = end + 1;
  }
  // The list limits: the first value that would pass one ends the list.
  const size_t grown = len_ + (values_ ? 1 : 0) + keep;
  if (values_ + 1 > kListValues || grown > kListMax) {
    ended_ = true;
    truncated_ = true;
    return;
  }
  if (values_) buf_[len_++] = kSeparator;
  std::memcpy(buf_ + len_, v, keep);
  len_ += keep;
  buf_[len_] = 0;
  ++values_;
}

size_t encodeRun(const char* const fields[kRunFields], uint8_t* out, size_t cap) {
  int last = -1;
  for (int f = 0; f < kRunFields; ++f)
    if (fields[f] && fields[f][0]) last = f;
  if (last < 0) return 0;
  size_t n = 0;
  if (cap < 1) return 0;
  out[n++] = static_cast<uint8_t>(last + 1);
  for (int f = 0; f <= last; ++f) {
    const char* s = fields[f] ? fields[f] : "";
    const size_t len = std::strlen(s);
    if (n + len + 1 > cap) return 0;
    std::memcpy(out + n, s, len);
    n += len;
    out[n++] = 0;
  }
  return n;
}

void RunFields::clear() {
  n = 0;
  cut = false;
  text[0] = 0;
  for (uint32_t f = 0; f < kRunFields; ++f) {
    offset[f] = 0;
    length[f] = 0;
  }
}

void RunFields::set(uint32_t f, const char* s, size_t len) {
  if (f >= kRunFields) return;
  // Field f's slot starts after the slots of the fields before it, each at
  // its limit plus its NUL, so a cut field never moves the others.
  size_t at = 1;
  for (uint32_t k = 0; k < f; ++k) at += fieldMax(k) + 1;
  const size_t keep = utf8CutLength(s, len, fieldMax(f));
  if (keep < len) cut = true;
  std::memcpy(text + at, s, keep);
  text[at + keep] = 0;
  offset[f] = static_cast<uint16_t>(at);
  length[f] = static_cast<uint16_t>(keep);
}

bool parseRun(const uint8_t* run, size_t len, RunFields* out, size_t* used) {
  out->clear();
  if (len < 1) return false;
  const uint8_t n = run[0];
  size_t p = 1;
  for (uint32_t f = 0; f < n; ++f) {
    const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(run + p, 0, len - p));
    if (!nul) return false;
    const size_t l = static_cast<size_t>(nul - (run + p));
    out->set(f, reinterpret_cast<const char*>(run + p), l);
    p += l + 1;
  }
  out->n = n;
  if (used) *used = p;
  return true;
}

// ---------------------------------------------------------------------------
// Names and the canonical order
// ---------------------------------------------------------------------------
bool validName(const char* s, size_t n) {
  if (!s || n == 0) return false;
  if (std::memchr(s, '/', n) || std::memchr(s, 0, n)) return false;
  if (n == 1 && s[0] == '.') return false;
  if (n == 2 && s[0] == '.' && s[1] == '.') return false;
  return true;
}

bool validRelPath(const char* s, size_t n) {
  if (!s || n == 0 || n > kMaxRelPath) return false;
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i == n || s[i] == '/') {
      if (!validName(s + start, i - start)) return false;
      start = i + 1;
    }
  }
  return true;
}

int compareNames(const char* a, size_t an, const char* b, size_t bn) {
  const int c = std::memcmp(a, b, an < bn ? an : bn);
  if (c) return c < 0 ? -1 : 1;
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

namespace {

// The component at `pos` of a path: its length, and whether another follows.
size_t component(const char* s, size_t n, size_t pos, bool* more) {
  size_t e = pos;
  while (e < n && s[e] != '/') ++e;
  *more = e < n;
  return e - pos;
}

}  // namespace

int compareFilePaths(const char* a, size_t an, const char* b, size_t bn) {
  size_t pa = 0, pb = 0;
  for (;;) {
    bool moreA, moreB;
    const size_t la = component(a, an, pa, &moreA), lb = component(b, bn, pb, &moreB);
    if (moreA != moreB) return moreA ? 1 : -1;  // in one folder, its files before its subfolders
    const int c = compareNames(a + pa, la, b + pb, lb);
    if (c || !moreA) return c;
    pa += la + 1;
    pb += lb + 1;
  }
}

int compareFilePaths(const char* a, const char* b) { return compareFilePaths(a, std::strlen(a), b, std::strlen(b)); }

int compareFolderPaths(const char* a, size_t an, const char* b, size_t bn) {
  if (an == 0 || bn == 0) return an == bn ? 0 : (an == 0 ? -1 : 1);
  size_t pa = 0, pb = 0;
  for (;;) {
    bool moreA, moreB;
    const size_t la = component(a, an, pa, &moreA), lb = component(b, bn, pb, &moreB);
    const int c = compareNames(a + pa, la, b + pb, lb);
    if (c) return c;
    if (!moreA || !moreB) return moreA == moreB ? 0 : (moreA ? 1 : -1);  // the ancestor first
    pa += la + 1;
    pb += lb + 1;
  }
}

int compareFolderPaths(const char* a, const char* b) { return compareFolderPaths(a, std::strlen(a), b, std::strlen(b)); }

size_t albumFolderLength(const char* rel, size_t len, const char* const* roots, size_t nRoots) {
  // The file's root: the longest root whose folder contains it.
  size_t root = 0;
  for (size_t i = 0; i < nRoots; ++i) {
    const size_t rl = roots[i] ? std::strlen(roots[i]) : 0;
    if (rl == 0 || rl >= len || rl <= root) continue;
    if (std::memcmp(rel, roots[i], rl) == 0 && rel[rl] == '/') root = rl;
  }
  // Depth below it: the folders between the root and the file.
  const size_t start = root ? root + 1 : 0;
  size_t slashes[2];
  int found = 0;
  for (size_t i = start; i < len && found < 2; ++i)
    if (rel[i] == '/') slashes[found++] = i;
  size_t last = len;  // the file's own folder: up to its last '/'
  while (last > 0 && rel[last - 1] != '/') --last;
  const size_t own = last ? last - 1 : 0;
  if (found < 2) return own < root ? root : own;  // depth 0 or 1: its own folder
  // Depth 2 or more: the root's grandchild, unless the file is in it.
  const size_t album = slashes[1];
  // slashes[1] ends the second folder only when the file is deeper still.
  return album < own ? album : own;
}

bool transferThumbPath(const char* dir, uint64_t folderHash, char* out, size_t cap) {
  return thumbfile::path(dir, folderHash, out, cap);
}

// ---------------------------------------------------------------------------
// device.txt
// ---------------------------------------------------------------------------
namespace devicetxt {

namespace {

void copyValue(char* dst, const char* s, size_t n) {
  if (n > kValueMax - 1) n = kValueMax - 1;
  std::memcpy(dst, s, n);
  dst[n] = 0;
}

// "1,2" as bits (bit 0: major 1); "" is none. Items that aren't a number
// from 1 to 32 are skipped.
uint32_t majors(const char* s, size_t n) {
  uint32_t mask = 0;
  size_t i = 0;
  while (i < n) {
    uint32_t v = 0;
    bool digits = false, other = false;
    while (i < n && s[i] != ',') {
      if (s[i] >= '0' && s[i] <= '9') {
        if (v < 100) v = v * 10 + static_cast<uint32_t>(s[i] - '0');
        digits = true;
      } else if (s[i] != ' ') {
        other = true;
      }
      ++i;
    }
    if (digits && !other && v >= 1 && v <= 32) mask |= 1u << (v - 1);
    ++i;  // past the comma
  }
  return mask;
}

uint32_t number(const char* s, size_t n, uint32_t fallback) {
  if (n == 0 || n > 9) return fallback;
  uint32_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return fallback;
    v = v * 10 + static_cast<uint32_t>(s[i] - '0');
  }
  return v;
}

size_t putMajors(char* out, size_t cap, size_t n, const char* key, uint32_t mask) {
  n += static_cast<size_t>(std::snprintf(out + (n < cap ? n : cap), n < cap ? cap - n : 0, "%s=", key));
  bool first = true;
  for (uint32_t m = 1; m <= 32; ++m) {
    if (!readsMajor(mask, m)) continue;
    n += static_cast<size_t>(
        std::snprintf(out + (n < cap ? n : cap), n < cap ? cap - n : 0, first ? "%u" : ",%u", static_cast<unsigned>(m)));
    first = false;
  }
  n += static_cast<size_t>(std::snprintf(out + (n < cap ? n : cap), n < cap ? cap - n : 0, "\n"));
  return n;
}

}  // namespace

Info defaults() { return Info(); }

Info current(const char* firmware) {
  Info i;
  copyValue(i.firmware, firmware ? firmware : "", firmware ? std::strlen(firmware) : 0);
  copyValue(i.codecs, "mp3,flac,opus", 13);
  copyValue(i.extensions, "mp3,flac,opus", 13);
  return i;
}

size_t format(const Info& info, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  auto add = [&](const char* fmt, auto... args) {
    n += static_cast<size_t>(std::snprintf(out + (n < cap ? n : cap), n < cap ? cap - n : 0, fmt, args...));
  };
  add("contract=%u\n", static_cast<unsigned>(info.contract));
  add("firmware=%s\n", info.firmware);
  n = putMajors(out, cap, n, "read.msmf", info.readMsmf);
  n = putMajors(out, cap, n, "read.mptg", info.readMptg);
  n = putMajors(out, cap, n, "read.mpdj", info.readMpdj);
  n = putMajors(out, cap, n, "read.mpth", info.readMpth);
  add("codecs=%s\n", info.codecs);
  add("extensions=%s\n", info.extensions);
  add("max_rate=%u\n", static_cast<unsigned>(info.maxRate));
  add("max_channels=%u\n", static_cast<unsigned>(info.maxChannels));
  if (n >= cap) {
    out[0] = 0;
    return 0;
  }
  return n;
}

bool parse(const char* text, size_t n, Info* out) {
  *out = defaults();
  bool contract = false;
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && text[e] != '\n') ++e;
    size_t le = e;
    if (le > i && text[le - 1] == '\r') --le;
    const char* line = text + i;
    const size_t len = le - i;
    const char* eq = static_cast<const char*>(std::memchr(line, '=', len));
    if (eq) {
      const size_t kl = static_cast<size_t>(eq - line);
      const char* v = eq + 1;
      const size_t vl = len - kl - 1;
      auto key = [&](const char* k) { return std::strlen(k) == kl && std::memcmp(line, k, kl) == 0; };
      if (key("contract")) {
        out->contract = static_cast<uint16_t>(number(v, vl, 0));
        contract = true;
      } else if (key("firmware")) {
        copyValue(out->firmware, v, vl);
      } else if (key("read.msmf")) {
        out->readMsmf = majors(v, vl);
      } else if (key("read.mptg")) {
        out->readMptg = majors(v, vl);
      } else if (key("read.mpdj")) {
        out->readMpdj = majors(v, vl);
      } else if (key("read.mpth")) {
        out->readMpth = majors(v, vl);
      } else if (key("codecs")) {
        copyValue(out->codecs, v, vl);
      } else if (key("extensions")) {
        copyValue(out->extensions, v, vl);
      } else if (key("max_rate")) {
        out->maxRate = number(v, vl, out->maxRate);
      } else if (key("max_channels")) {
        out->maxChannels = number(v, vl, out->maxChannels);
      }
    }
    i = e + 1;
  }
  if (!contract) *out = defaults();
  return contract;
}

bool listHas(const char* list, const char* name) {
  if (!list || !name) return false;
  const size_t nl = std::strlen(name);
  const char* p = list;
  while (*p) {
    const char* e = p;
    while (*e && *e != ',') ++e;
    if (static_cast<size_t>(e - p) == nl) {
      bool eq = true;
      for (size_t k = 0; k < nl && eq; ++k) eq = lowerAscii(p[k]) == lowerAscii(name[k]);
      if (eq) return true;
    }
    p = *e ? e + 1 : e;
  }
  return false;
}

}  // namespace devicetxt

}  // namespace cardcontract
