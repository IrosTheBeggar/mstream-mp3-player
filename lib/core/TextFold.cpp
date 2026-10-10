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
    case 0x02C6: return "^";  // ˆ and ˜: cp1252's 0x88 and 0x98, which no font of ours has
    case 0x02DC: return "~";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    default: return nullptr;
  }
}

// One-character strings for the table lookups: printable ASCII.
struct AsciiStrings {
  char s[95][2];
  constexpr AsciiStrings() : s{} {
    for (int i = 0; i < 95; ++i) {
      s[i][0] = static_cast<char>(0x20 + i);
      s[i][1] = 0;
    }
  }
};
constexpr AsciiStrings kAscii{};
const char* single(char c) { return c >= 0x20 && c < 0x7F ? kAscii.s[c - 0x20] : "?"; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The combining marks Full folding drops: the generic blocks, the Cyrillic
// ones and kana's voicing marks (what the composer didn't put on a letter),
// and the variation selectors.
bool dropped(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
         (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x3099 || cp == 0x309A;
}

const char* foldOf(uint32_t cp) {
  uint32_t lo = 0, hi = tables::kFoldCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (tables::kFolds[mid].cp < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < tables::kFoldCount && tables::kFolds[lo].cp == cp ? tables::kFolds[lo].to : nullptr;
}

// ---- the order's units ----
//
// compare() walks two texts a unit at a time: a character of the Full
// folding, lower-cased. 0 is the end. Where two texts first differ, rank()
// orders the two units: other < digits < letters, one rank per character,
// so compare() stays a strict weak ordering for std::sort.
uint32_t rank(uint32_t u) {
  if (u >= 'a' && u <= 'z') return 256 + u;
  if (u >= '0' && u <= '9') return 128 + u;
  return u;
}

// A unit is a letter or a digit (sameName()'s key).
bool isKey(uint32_t u) { return (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9'); }

struct Units {
  Composer c;
  const char* pending = nullptr;
  Units(const char* s, const char* end) : c(s, end) {}
  // The next unit, 0 at the end.
  uint32_t next() {
    for (;;) {
      if (pending && *pending) return static_cast<unsigned char>(lower(*pending++));
      const uint32_t cp = c.next();
      if (cp < 0x80) return static_cast<unsigned char>(lower(static_cast<char>(cp)));  // 0 too: the end
      pending = replacement(cp, Mode::Full);
    }
  }
  // The next letter or digit, 0 at the end.
  uint32_t nextKey() {
    for (;;) {
      const uint32_t u = next();
      if (u == 0 || isKey(u)) return u;
    }
  }
};

// A unit's ASCII letter, upper-cased (railKey()) or not, else '#'.
char letterOf(uint32_t u, bool upper) {
  if (u < 'a' || u > 'z') return '#';
  return static_cast<char>(upper ? u - 'a' + 'A' : u);
}

// Hangul's composition (Unicode 3.12).
constexpr uint32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
constexpr uint32_t kLCount = 19, kVCount = 21, kTCount = 28, kSCount = kLCount * kVCount * kTCount;

// The composer's longest run of marks (a bit each in its taken_).
constexpr int kMaxRun = 16;

// Whether a mark the composer knows can start at `p` (its lead byte): the
// fast path for the text in between.
bool maybeMark(const char* p, const char* end) {
  if (end && p >= end) return false;
  const auto b = static_cast<unsigned char>(*p);
  return b == 0xCC || b == 0xCD || b == 0xD2 || b == 0xE3;
}

}  // namespace

uint32_t decode(const char*& s) { return decode(s, nullptr); }

uint32_t decode(const char*& s, const char* end) {
  if (end && s >= end) return 0;
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
  if (end && end - s <= extra) {  // cut by `end`
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

uint32_t fromC1(uint32_t cp) {
  if (cp < 0x80 || cp > 0x9F) return cp;
  const uint32_t to = tables::kCp1252[cp - 0x80];
  return to ? to : cp;
}

const char* replacement(uint32_t cp, Mode mode) {
  if (cp < 0x80) return nullptr;
  cp = fromC1(cp);
  if (const char* p = punctuation(cp)) return p;
  if (mode == Mode::Punctuation) return nullptr;
  if (dropped(cp)) return "";
  if (const char* m = latin1Multi(cp)) return m;
  if (const char* sym = symbols(cp)) return sym;
  if (cp >= 0xC0 && cp <= 0xFF && kLatin1[cp - 0xC0]) return single(kLatin1[cp - 0xC0]);
  if (cp >= 0x100 && cp <= 0x17F) return single(kLatinExtA[cp - 0x100]);
  if (cp >= 0x180 && cp <= 0x24F && tables::kLatinExtB[cp - 0x180]) return single(tables::kLatinExtB[cp - 0x180]);
  if (cp >= 0x1E00 && cp <= 0x1EFF && tables::kLatinExtAdditional[cp - 0x1E00]) {
    return single(tables::kLatinExtAdditional[cp - 0x1E00]);
  }
  if (cp >= 0xFF01 && cp <= 0xFF5E) return single(static_cast<char>(cp - 0xFEE0));  // fullwidth ASCII
  if (const char* f = foldOf(cp)) return f;
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

// ---- composition ----

uint8_t combiningClass(uint32_t cp) {
  if (cp >= 0x0300 && cp <= 0x036F) return tables::kCombiningClass[cp - 0x0300];
  if (cp >= 0x0483 && cp <= 0x0487) return 230;
  if (cp == 0x3099 || cp == 0x309A) return 8;
  return 0;
}

uint32_t composePair(uint32_t first, uint32_t second) {
  if (first >= kLBase && first < kLBase + kLCount && second >= kVBase && second < kVBase + kVCount) {
    return kSBase + ((first - kLBase) * kVCount + (second - kVBase)) * kTCount;
  }
  if (first >= kSBase && first < kSBase + kSCount && (first - kSBase) % kTCount == 0 && second > kTBase &&
      second < kTBase + kTCount) {
    return first + (second - kTBase);
  }
  if (first >= 0x4000) return 0;
  uint32_t m = 0;
  while (m < tables::kPairMarkCount && tables::kPairMarks[m] != second) ++m;
  if (m == tables::kPairMarkCount) return 0;
  const uint32_t key = first << 5 | m;
  uint32_t lo = 0, hi = tables::kPairCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if ((tables::kPairs[mid] >> 13) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == tables::kPairCount || (tables::kPairs[lo] >> 13) != key) return 0;
  return (tables::kPairs[lo] & 0x1FFF) | (first & 0x2000);
}

uint32_t Composer::next() {
  if (run_) {
    if (const uint32_t m = leftover()) return m;
    s_ = runEnd_;
    run_ = nullptr;
  }
  // ASCII with no mark after it (most of every name): as it is.
  if (!end_ || s_ < end_) {
    const auto b = static_cast<unsigned char>(*s_);
    if (b != 0 && b < 0x80 && !maybeMark(s_ + 1, end_)) {
      ++s_;
      return b;
    }
  }
  uint32_t cp = decode(s_, end_);
  if (cp == 0) return 0;
  // Hangul: L + V, then LV + T (jamo are starters: nothing blocks them).
  while (cp >= kLBase && ((cp < kLBase + kLCount) || (cp >= kSBase && cp < kSBase + kSCount))) {
    const char* p = s_;
    const uint32_t c = composePair(cp, decode(p, end_));
    if (!c) break;
    cp = c;
    s_ = p;
  }
  if (!maybeMark(s_, end_)) return cp;
  // The marks after it: [s_, p), n of them.
  const char* p = s_;
  int n = 0;
  for (;;) {
    const char* q = p;
    const uint32_t m = decode(q, end_);
    if (m == 0 || combiningClass(m) == 0) break;
    if (n == kMaxRun) return cp;  // too many to track: left as they are
    ++n;
    p = q;
  }
  if (n == 0) return cp;
  // NFC's composition over the marks in canonical order (by class, each
  // class in text order): a mark joins the letter unless a mark it didn't
  // take, of the same class, comes first (blocked), or there's no pair.
  uint16_t taken = 0;
  int level = 0, blocked = 0;
  for (;;) {
    int nextLevel = 256;
    const char* q = s_;
    for (int i = 0; i < n; ++i) {
      const int c = combiningClass(decode(q, end_));
      if (c > level && c < nextLevel) nextLevel = c;
    }
    if (nextLevel == 256) break;
    level = nextLevel;
    q = s_;
    for (int i = 0; i < n; ++i) {
      const uint32_t m = decode(q, end_);
      if (combiningClass(m) != level) continue;
      if (blocked != level) {
        if (const uint32_t c = composePair(cp, m)) {
          cp = c;
          taken = static_cast<uint16_t>(taken | 1u << i);
          continue;
        }
      }
      blocked = level;
    }
  }
  if (taken == (1u << n) - 1) {
    s_ = p;
    return cp;
  }
  run_ = s_;
  runEnd_ = p;
  taken_ = taken;
  count_ = static_cast<uint8_t>(n);
  level_ = 0;
  at_ = 0;
  return cp;
}

// The next mark of the run the letter didn't take, in canonical order; 0
// when none is left.
uint32_t Composer::leftover() {
  for (;;) {
    const char* q = run_;
    int nextLevel = 256;
    for (int i = 0; i < count_; ++i) {
      const uint32_t m = decode(q, end_);
      if (taken_ >> i & 1) continue;
      const int c = combiningClass(m);
      if (c == level_ && i >= at_) {
        at_ = static_cast<uint8_t>(i + 1);
        return m;
      }
      if (c > level_ && c < nextLevel) nextLevel = c;
    }
    if (nextLevel == 256) return 0;
    level_ = static_cast<uint8_t>(nextLevel);
    at_ = 0;
  }
}

// ---- the order ----

int compare(const char* a, const char* b) {
  Units ua(a, nullptr), ub(b, nullptr);
  for (;;) {
    const uint32_t x = ua.next();
    const uint32_t y = ub.next();
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

// A slice's units; with `article`, past a leading "the " that a letter or
// digit follows.
Units sliceUnits(const char* s, size_t len, bool article) {
  const char* end = s + len;
  if (article) {
    const char* p = s;
    while (p < end && *p == ' ') ++p;
    if (end - p >= 4 && lower(p[0]) == 't' && lower(p[1]) == 'h' && lower(p[2]) == 'e' && p[3] == ' ') {
      Units rest(p + 4, end);
      if (rest.nextKey()) return Units(p + 4, end);
    }
  }
  return Units(s, end);
}

}  // namespace

bool sameName(const char* a, size_t aLen, const char* b, size_t bLen) {
  if (!a || !b) return false;
  Units x = sliceUnits(a, aLen, true), y = sliceUnits(b, bLen, true);
  bool any = false;
  for (;;) {
    const uint32_t cx = x.nextKey();
    const uint32_t cy = y.nextKey();
    if (cx != cy) return false;
    if (cx == 0) return any;
    any = true;
  }
}

bool startsWithName(const char* s, size_t sLen, const char* name, size_t nameLen) {
  if (!s || !name) return false;
  Units n = sliceUnits(name, nameLen, true), x = sliceUnits(s, sLen, true);
  uint32_t want = n.nextKey();
  if (want == 0) return false;
  for (;;) {
    const uint32_t c = x.next();
    if (c == 0) return false;  // `s` ended first
    if (!isKey(c)) continue;
    if (c != want) return false;
    want = n.nextKey();
    if (want == 0) return !isKey(x.next());  // the word ends with the name
  }
}

char railKey(const char* s) {
  Units u(s, nullptr);
  return letterOf(u.next(), true);
}

char secondKey(const char* s) {
  Units u(s, nullptr);
  if (u.next() == 0) return '#';
  return letterOf(u.next(), false);
}

int bucketOf(char key) {
  if (key >= 'A' && key <= 'Z') return 1 + (key - 'A');
  if (key >= 'a' && key <= 'z') return 1 + (key - 'a');
  return 0;
}

}  // namespace textfold
