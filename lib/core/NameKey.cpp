// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "NameKey.h"

#include <cstring>

namespace namekey {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;
constexpr uint32_t kCapitalSigma = 0x03A3;
constexpr uint32_t kSmallSigma = 0x03C3;
constexpr uint32_t kFinalSigma = 0x03C2;
constexpr char kSeparator = '\x1F';
constexpr uint64_t kFnvPrime = 0x00000100000001B3ull;

// One code point of s[*i, n), advancing *i past it. A byte that doesn't
// start a valid sequence (a stray continuation, an overlong form, a
// surrogate, past U+10FFFF, or cut short) is U+FFFD and one byte.
uint32_t decodeAt(const char* s, size_t n, size_t* i) {
  const auto* p = reinterpret_cast<const uint8_t*>(s);
  const uint8_t b = p[*i];
  if (b < 0x80) {
    ++*i;
    return b;
  }
  uint32_t cp;
  size_t need;
  uint32_t min;
  if ((b & 0xE0) == 0xC0) {
    cp = b & 0x1F;
    need = 1;
    min = 0x80;
  } else if ((b & 0xF0) == 0xE0) {
    cp = b & 0x0F;
    need = 2;
    min = 0x800;
  } else if ((b & 0xF8) == 0xF0) {
    cp = b & 0x07;
    need = 3;
    min = 0x10000;
  } else {
    ++*i;
    return kReplacement;
  }
  if (*i + need >= n) {  // cut short by the end
    ++*i;
    return kReplacement;
  }
  for (size_t k = 1; k <= need; ++k) {
    const uint8_t c = p[*i + k];
    if ((c & 0xC0) != 0x80) {
      ++*i;
      return kReplacement;
    }
    cp = cp << 6 | (c & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++*i;
    return kReplacement;
  }
  *i += need + 1;
  return cp;
}

size_t encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | cp >> 6);
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | cp >> 12);
    out[1] = static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | cp >> 18);
  out[1] = static_cast<char>(0x80 | (cp >> 12 & 0x3F));
  out[2] = static_cast<char>(0x80 | (cp >> 6 & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Where the key's bytes go: a buffer (cut at a code point, the whole length
// counted) or a hash.
struct Out {
  char* buf = nullptr;
  size_t cap = 0;
  size_t len = 0;       // the whole key
  size_t written = 0;   // in buf
  bool full = false;    // a code point didn't fit: nothing more is written
  bool hashing = false;
  uint64_t h = 0;

  void put(uint32_t cp) {
    char b[4];
    const size_t k = encode(cp, b);
    if (hashing) {
      for (size_t i = 0; i < k; ++i) {
        h ^= static_cast<uint8_t>(b[i]);
        h *= kFnvPrime;
      }
    } else if (buf && !full) {
      if (written + k + 1 <= cap) {
        std::memcpy(buf + written, b, k);
        written += k;
      } else {
        full = true;
      }
    }
    len += k;
  }
  void end() {
    if (buf && cap) buf[written] = 0;
  }
};

// Whether the code point after a capital sigma at [from, n) is cased, as
// Final_Sigma looks: case-ignorable characters skipped, the folded string's
// own (whitespace is a space there, or the trimmed end: neither cased).
bool casedAhead(const char* s, size_t n, size_t from) {
  for (size_t i = from; i < n;) {
    const uint32_t cp = decodeAt(s, n, &i);
    if (isWhiteSpace(cp)) return false;
    const uint8_t c = sigmaClass(foldPunctuation(cp));
    if (c == kIgnorable) continue;
    return c == kCased;
  }
  return false;
}

void keyInto(const char* s, size_t n, Out* out) {
  bool started = false, space = false;
  bool lastCased = false;  // the last non-ignorable character before this one is cased
  for (size_t i = 0; i < n;) {
    const uint32_t raw = decodeAt(s, n, &i);
    if (isWhiteSpace(raw)) {
      if (started) space = true;
      continue;
    }
    if (space) {
      out->put(' ');
      space = false;
      lastCased = false;
    }
    started = true;
    const uint32_t cp = foldPunctuation(raw);
    if (cp == kCapitalSigma) {
      out->put(lastCased && !casedAhead(s, n, i) ? kFinalSigma : kSmallSigma);
    } else {
      uint32_t low[3];
      const uint32_t k = toLower(cp, low);
      for (uint32_t j = 0; j < k; ++j) out->put(low[j]);
    }
    const uint8_t c = sigmaClass(cp);
    if (c != kIgnorable) lastCased = c == kCased;
  }
}

}  // namespace

bool isWhiteSpace(uint32_t cp) {
  for (uint32_t i = 0; i < tables::kWhiteSpaceCount; ++i) {
    if (cp < tables::kWhiteSpace[i].first) return false;
    if (cp <= tables::kWhiteSpace[i].last) return true;
  }
  return false;
}

uint32_t toLower(uint32_t cp, uint32_t out[3]) {
  for (uint32_t i = 0; i < tables::kLowerSpecialCount; ++i) {
    const tables::LowerSpecial& sp = tables::kLowerSpecial[i];
    if (sp.cp == cp) {
      for (uint32_t k = 0; k < sp.n && k < 3; ++k) out[k] = sp.to[k];
      return sp.n < 3 ? sp.n : 3;
    }
  }
  // The last run starting at or before cp.
  uint32_t lo = 0, hi = tables::kLowerCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (tables::kLower[mid].first <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo > 0) {
    const tables::LowerRun& r = tables::kLower[lo - 1];
    const uint32_t off = cp - r.first;
    if (off % r.step == 0 && off / r.step < r.count) {
      out[0] = static_cast<uint32_t>(static_cast<int64_t>(cp) + r.delta);
      return 1;
    }
  }
  out[0] = cp;
  return 1;
}

uint8_t sigmaClass(uint32_t cp) {
  uint32_t lo = 0, hi = tables::kSigmaContextCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (tables::kSigmaContext[mid].first <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return kOther;
  const tables::ClassRange& r = tables::kSigmaContext[lo - 1];
  return cp <= (r.lastAndClass & 0xFFFFFFu) ? static_cast<uint8_t>(r.lastAndClass >> 24) : kOther;
}

uint32_t foldPunctuation(uint32_t cp) {
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    default: return cp;
  }
}

size_t nameKey(const char* s, size_t n, char* out, size_t cap) {
  Out o;
  o.buf = out;
  o.cap = cap;
  keyInto(s, n, &o);
  o.end();
  return o.len;
}

uint64_t nameKeyHash(const char* s, size_t n, uint64_t h) {
  Out o;
  o.hashing = true;
  o.h = h;
  keyInto(s, n, &o);
  return o.h;
}

size_t orderName(const char* name, size_t n, const char* sortTag, size_t sortLen, char* out, size_t cap) {
  // The sort tag's key when it isn't empty (a tag of spaces alone keys to "").
  size_t sortKeyLen = 0;
  if (sortTag && sortLen) {
    Out probe;
    probe.hashing = true;
    keyInto(sortTag, sortLen, &probe);
    sortKeyLen = probe.len;
  }
  const bool useSort = sortKeyLen > 0;
  const char* s = useSort ? sortTag : name;
  const size_t sn = useSort ? sortLen : n;
  // The key, then the article off its front: written whole first (the
  // article is ASCII, so the cut moves by whole bytes of it).
  Out o;
  o.buf = out;
  o.cap = cap;
  keyInto(s, sn, &o);
  o.end();
  static const char* const kArticles[] = {"the", "el", "la", "los", "las", "le", "les"};
  if (!out || cap == 0) return o.len;
  for (const char* a : kArticles) {
    const size_t al = std::strlen(a);
    if (o.written > al && std::memcmp(out, a, al) == 0 && out[al] == ' ') {
      std::memmove(out, out + al + 1, o.written - al);  // and the NUL
      return o.len - al - 1;
    }
  }
  return o.len;
}

const char* trim(const char* s, size_t n, size_t* len) {
  size_t a = 0;
  while (a < n) {
    size_t i = a;
    if (!isWhiteSpace(decodeAt(s, n, &i))) break;
    a = i;
  }
  // From the end: the last code point that isn't whitespace ends it.
  size_t end = a;
  for (size_t i = a; i < n;) {
    const uint32_t cp = decodeAt(s, n, &i);
    if (!isWhiteSpace(cp)) end = i;
  }
  *len = end - a;
  return s + a;
}

size_t displayJoin(const char* list, size_t n, char* out, size_t cap) {
  constexpr uint32_t kMaxKeys = 32;  // beyond 2.3.6's 16 values: no more deduplication
  uint64_t keys[kMaxKeys];
  uint32_t nKeys = 0;
  size_t len = 0, written = 0;
  bool full = false;
  if (out && cap) out[0] = 0;
  auto append = [&](const char* p, size_t k) {
    len += k;
    if (!out || full) return;
    if (written + k + 1 <= cap) {
      std::memcpy(out + written, p, k);
      written += k;
      out[written] = 0;
      return;
    }
    // Cut at a code point boundary inside what's left.
    size_t room = cap - 1 - written;
    while (room > 0 && (static_cast<uint8_t>(p[room]) & 0xC0) == 0x80) --room;
    std::memcpy(out + written, p, room);
    written += room;
    out[written] = 0;
    full = true;
  };
  size_t start = 0;
  bool first = true;
  while (start <= n) {
    const char* sep = static_cast<const char*>(std::memchr(list + start, kSeparator, n - start));
    const size_t end = sep ? static_cast<size_t>(sep - list) : n;
    size_t vl = 0;
    const char* v = trim(list + start, end - start, &vl);
    if (vl > 0) {
      const uint64_t k = nameKeyHash(v, vl);
      bool seen = false;
      for (uint32_t i = 0; i < nKeys; ++i) seen = seen || keys[i] == k;
      if (!seen) {
        if (nKeys < kMaxKeys) keys[nKeys++] = k;
        if (!first) append(", ", 2);
        append(v, vl);
        first = false;
      }
    }
    if (!sep) break;
    start = end + 1;
  }
  return len;
}

uint32_t artistKey(const char* list, size_t n) {
  const char* sep = static_cast<const char*>(std::memchr(list, kSeparator, n));
  Out o;
  o.hashing = true;
  o.h = 0xCBF29CE484222325ull;
  keyInto(list, sep ? static_cast<size_t>(sep - list) : n, &o);
  return o.len ? static_cast<uint32_t>(o.h) : 0;
}

}  // namespace namekey
