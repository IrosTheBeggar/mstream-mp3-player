// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TextFold.h"

#include <cstring>

namespace textfold {

namespace {

// U+00C0..U+00FF without accents; '\0' marks the ones that need more than
// one letter (handled in latin1Multi) or aren't letters.
constexpr char kLatin1[64 + 1] =
    "AAAAAA\0CEEEEIIII"   // C0-CF: Æ is multi
    "DNOOOOO\0OUUUUY\0\0"  // D0-DF: × Þ ß are multi / symbols
    "aaaaaa\0ceeeeiiii"    // E0-EF: æ
    "dnooooo\0ouuuuy\0y";  // F0-FF: ÷ þ

// U+0100..U+017F, base letters.
constexpr char kLatinExtA[128 + 1] =
    "AaAaAaCcCcCcCcDd"
    "DdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIi"
    "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo"
    "OoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUu"
    "UuUuWwYyYZzZzZzs";

const char* latin1Multi(uint32_t cp) {
  switch (cp) {
    case 0xC6: return "AE";
    case 0xD7: return "x";
    case 0xDE: return "Th";
    case 0xDF: return "ss";
    case 0xE6: return "ae";
    case 0xF7: return "/";
    case 0xFE: return "th";
    default: return nullptr;
  }
}

// Replacements used in both modes: punctuation, spaces, invisible characters.
const char* punctuation(uint32_t cp) {
  switch (cp) {
    case 0x00A0: case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
    case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: case 0x205F:
    case 0x3000:
      return " ";
    case 0x00AD: case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF:
      return "";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
      return "-";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x2035: case 0x00B4: case 0x02BC:
      return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0x00AB: case 0x00BB:
      return "\"";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x2026: return "...";
    case 0x2022: return "*";
    case 0x00B7: return ".";
    case 0x2044: return "/";
    case 0x00D7: return "x";
    default: return nullptr;
  }
}

// Full mode only: symbols in Latin-1 that aren't letters.
const char* symbols(uint32_t cp) {
  switch (cp) {
    case 0xA1: return "!";
    case 0xBF: return "?";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0xB0: return "o";
    case 0xB9: return "1";
    case 0xB2: return "2";
    case 0xB3: return "3";
    case 0xBC: return "1/4";
    case 0xBD: return "1/2";
    case 0xBE: return "3/4";
    case 0xB5: return "u";
    case 0xAA: return "a";
    case 0xBA: return "o";
    case 0xA7: return "S";
    case 0xA2: return "c";
    case 0xA3: return "L";
    case 0xA5: return "Y";
    case 0xA6: return "|";
    case 0xAC: return "-";
    case 0xB1: return "+-";
    case 0xB6: return "P";
    case 0xB8: return ",";
    case 0xA8: return "";
    case 0xAF: return "";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    case 0x0132: return "IJ";
    case 0x0133: return "ij";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    default: return nullptr;
  }
}

// One-character strings for the table lookups.
const char* single(char c) {
  static const char kChars[] =
      "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0N\0O\0P\0Q\0R\0S\0T\0U\0V\0W\0X\0Y\0Z\0"
      "a\0b\0c\0d\0e\0f\0g\0h\0i\0j\0k\0l\0m\0n\0o\0p\0q\0r\0s\0t\0u\0v\0w\0x\0y\0z\0";
  if (c >= 'A' && c <= 'Z') return kChars + 2 * (c - 'A');
  if (c >= 'a' && c <= 'z') return kChars + 52 + 2 * (c - 'a');
  return "?";
}

bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Order of a folded, lowercased character: other < digits < letters. One
// rank per character (a folded character is ASCII, so `u` < 128 for the
// others), which keeps compare() a strict weak ordering for std::sort.
int rank(char c) {
  const auto u = static_cast<unsigned char>(c);
  if (isAlpha(c)) return 256 + u;
  if (isDigit(c)) return 128 + u;
  return u;
}

// Full folding of a string, one ASCII character at a time.
struct Cursor {
  const char* s;
  const char* pending = nullptr;
  explicit Cursor(const char* str) : s(str) {}
  char next() {
    for (;;) {
      if (pending && *pending) return *pending++;
      const uint32_t cp = decode(s);
      if (cp == 0) return 0;
      if (cp < 0x80) return static_cast<char>(cp);
      pending = replacement(cp, Mode::Full);
    }
  }
};

}  // namespace

uint32_t decode(const char*& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char c = p[0];
  if (c == 0) return 0;
  if (c < 0x80) {
    ++s;
    return c;
  }
  int extra = 0;
  uint32_t cp = 0;
  uint32_t min = 0;
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    cp = c & 0x1F;
    min = 0x80;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    cp = c & 0x0F;
    min = 0x800;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    cp = c & 0x07;
    min = 0x10000;
  } else {
    ++s;
    return 0xFFFD;
  }
  for (int i = 1; i <= extra; ++i) {
    if ((p[i] & 0xC0) != 0x80) {
      ++s;
      return 0xFFFD;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++s;
    return 0xFFFD;
  }
  s += extra + 1;
  return cp;
}

const char* replacement(uint32_t cp, Mode mode) {
  if (cp < 0x80) return nullptr;
  if (const char* p = punctuation(cp)) return p;
  if (mode == Mode::Punctuation) return nullptr;
  if (const char* m = latin1Multi(cp)) return m;
  if (const char* sym = symbols(cp)) return sym;
  if (cp >= 0xC0 && cp <= 0xFF && kLatin1[cp - 0xC0]) return single(kLatin1[cp - 0xC0]);
  if (cp >= 0x100 && cp <= 0x17F) return single(kLatinExtA[cp - 0x100]);
  return "?";
}

Result fold(const char* in, char* out, size_t outSize, Mode mode) {
  return fold(in, in ? std::strlen(in) : 0, out, outSize, mode);
}

Result fold(const char* in, size_t inLen, char* out, size_t outSize, Mode mode) {
  Result r;
  if (outSize == 0) {
    r.truncated = inLen > 0;
    return r;
  }
  size_t n = 0;
  const char* s = in;
  const char* end = in + inLen;
  auto put = [&](const char* bytes, size_t len) {
    if (n + len > outSize - 1) {
      r.truncated = true;
      return false;
    }
    std::memcpy(out + n, bytes, len);
    n += len;
    return true;
  };
  while (s < end) {
    const char* start = s;
    const uint32_t cp = decode(s);
    if (cp == 0) break;
    if (s > end) {  // a sequence cut by inLen
      r.unknown++;
      break;
    }
    if (cp < 0x80) {
      if (!put(start, 1)) break;
      continue;
    }
    const char* rep = replacement(cp, mode);
    if (!rep) {  // kept (Punctuation mode): copy the original bytes
      if (cp == 0xFFFD && mode == Mode::Punctuation) r.unknown++;
      r.ascii = false;
      if (!put(start, static_cast<size_t>(s - start))) break;
      continue;
    }
    r.changed = true;
    if (mode == Mode::Full && rep[0] == '?' && rep[1] == 0) r.unknown++;
    if (!put(rep, std::strlen(rep))) break;
  }
  out[n] = 0;
  r.length = n;
  return r;
}

int compare(const char* a, const char* b) {
  Cursor ca(a), cb(b);
  for (;;) {
    const char x = lower(ca.next());
    const char y = lower(cb.next());
    if (x != y) {
      if (x == 0) return -1;
      if (y == 0) return 1;
      return rank(x) < rank(y) ? -1 : 1;
    }
    if (x == 0) break;
  }
  const int raw = std::strcmp(a, b);
  return raw < 0 ? -1 : raw > 0 ? 1 : 0;
}

const char* sortName(const char* s) {
  if (!s) return s;
  static const char* const kArticles[] = {"the", "el", "la", "los", "las", "le", "les"};
  for (const char* a : kArticles) {
    const size_t n = std::strlen(a);
    size_t i = 0;
    while (i < n && lower(s[i]) == a[i]) ++i;  // stops at the NUL too
    if (i < n || s[n] != ' ') continue;
    const char* p = s + n;
    while (*p == ' ') ++p;
    return *p ? p : s;
  }
  return s;
}

int compareSorted(const char* a, const char* b) {
  const int c = compare(sortName(a), sortName(b));
  return c != 0 ? c : compare(a, b);
}

namespace {

bool isAlnum(char c) { return isAlpha(c) || isDigit(c); }

// A slice's characters, Full-folded and lower-cased, one at a time; with
// `article`, past a leading "the " that a letter or digit follows.
struct SliceCursor {
  const char* s;
  const char* end;
  const char* pending = nullptr;
  SliceCursor(const char* str, size_t len, bool article) : s(str), end(str + len) {
    if (!article) return;
    const char* p = s;
    while (p < end && *p == ' ') ++p;
    if (end - p < 4 || lower(p[0]) != 't' || lower(p[1]) != 'h' || lower(p[2]) != 'e' || p[3] != ' ') return;
    SliceCursor rest(p + 4, static_cast<size_t>(end - (p + 4)), false);
    if (rest.nextKey()) s = p + 4;
  }
  // The next character, 0 at the end.
  char next() {
    for (;;) {
      if (pending && *pending) return lower(*pending++);
      if (s >= end) return 0;
      const uint32_t cp = decode(s);
      if (cp == 0 || s > end) {  // the string's end, or a sequence the slice cuts
        s = end;
        return 0;
      }
      if (cp < 0x80) return lower(static_cast<char>(cp));
      pending = replacement(cp, Mode::Full);
    }
  }
  // The next letter or digit, 0 at the end.
  char nextKey() {
    for (;;) {
      const char c = next();
      if (c == 0 || isAlnum(c)) return c;
    }
  }
};

}  // namespace

bool sameName(const char* a, size_t aLen, const char* b, size_t bLen) {
  if (!a || !b) return false;
  SliceCursor x(a, aLen, true), y(b, bLen, true);
  bool any = false;
  for (;;) {
    const char cx = x.nextKey();
    const char cy = y.nextKey();
    if (cx != cy) return false;
    if (cx == 0) return any;
    any = true;
  }
}

bool startsWithName(const char* s, size_t sLen, const char* name, size_t nameLen) {
  if (!s || !name) return false;
  SliceCursor n(name, nameLen, true), x(s, sLen, true);
  char want = n.nextKey();
  if (want == 0) return false;
  for (;;) {
    const char c = x.next();
    if (c == 0) return false;  // `s` ended first
    if (!isAlnum(c)) continue;
    if (c != want) return false;
    want = n.nextKey();
    if (want == 0) return !isAlnum(x.next());  // the word ends with the name
  }
}

char railKey(const char* s) {
  Cursor c(s);
  const char first = c.next();
  if (isAlpha(first)) return first >= 'a' ? static_cast<char>(first - 'a' + 'A') : first;
  return '#';
}

char secondKey(const char* s) {
  Cursor c(s);
  if (c.next() == 0) return '#';
  const char second = lower(c.next());
  return isAlpha(second) ? second : '#';
}

int bucketOf(char key) {
  if (key >= 'A' && key <= 'Z') return 1 + (key - 'A');
  if (key >= 'a' && key <= 'z') return 1 + (key - 'a');
  return 0;
}

}  // namespace textfold
