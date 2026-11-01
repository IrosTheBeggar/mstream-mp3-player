// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The library's order as it was at c97ff3c (before docs/I18N.md's phase
// 0), frozen: TextFold's compare(), compareSorted() and railKey() of then,
// for test_text_fold's old-against-new test. Full folding knew Latin-1 and
// Latin Extended-A only; everything else was '?'. Not for the firmware.
#pragma once
#include <cstdint>
#include <cstring>

#include "TextFold.h"

namespace legacy {

inline const char* single(char c) {
  static const char kChars[] =
      "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0N\0O\0P\0Q\0R\0S\0T\0U\0V\0W\0X\0Y\0Z\0"
      "a\0b\0c\0d\0e\0f\0g\0h\0i\0j\0k\0l\0m\0n\0o\0p\0q\0r\0s\0t\0u\0v\0w\0x\0y\0z\0";
  if (c >= 'A' && c <= 'Z') return kChars + 2 * (c - 'A');
  if (c >= 'a' && c <= 'z') return kChars + 52 + 2 * (c - 'a');
  return "?";
}

inline const char* replacement(uint32_t cp) {
  static const char kLatin1[64 + 1] =
      "AAAAAA\0CEEEEIIII"
      "DNOOOOO\0OUUUUY\0\0"
      "aaaaaa\0ceeeeiiii"
      "dnooooo\0ouuuuy\0y";
  static const char kLatinExtA[128 + 1] =
      "AaAaAaCcCcCcCcDd"
      "DdEeEeEeEeEeGgGg"
      "GgGgHhHhIiIiIiIi"
      "IiIiJjKkkLlLlLlL"
      "lLlNnNnNnnNnOoOo"
      "OoOoRrRrRrSsSsSs"
      "SsTtTtTtUuUuUuUu"
      "UuUuWwYyYZzZzZzs";
  if (cp < 0x80) return nullptr;
  switch (cp) {  // punctuation
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
    default: break;
  }
  switch (cp) {  // latin1Multi
    case 0xC6: return "AE";
    case 0xDE: return "Th";
    case 0xDF: return "ss";
    case 0xE6: return "ae";
    case 0xF7: return "/";
    case 0xFE: return "th";
    default: break;
  }
  switch (cp) {  // symbols
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
    default: break;
  }
  if (cp >= 0xC0 && cp <= 0xFF && kLatin1[cp - 0xC0]) return single(kLatin1[cp - 0xC0]);
  if (cp >= 0x100 && cp <= 0x17F) return single(kLatinExtA[cp - 0x100]);
  return "?";
}

inline bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
inline int rank(char c) {
  const auto u = static_cast<unsigned char>(c);
  if (isAlpha(c)) return 256 + u;
  if (isDigit(c)) return 128 + u;
  return u;
}

struct Cursor {
  const char* s;
  const char* pending = nullptr;
  explicit Cursor(const char* str) : s(str) {}
  char next() {
    for (;;) {
      if (pending && *pending) return *pending++;
      const uint32_t cp = textfold::decode(s);  // unchanged since
      if (cp == 0) return 0;
      if (cp < 0x80) return static_cast<char>(cp);
      pending = replacement(cp);
    }
  }
};

inline int compare(const char* a, const char* b) {
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

inline int compareSorted(const char* a, const char* b) {
  const int c = compare(textfold::sortName(a), textfold::sortName(b));  // sortName unchanged since
  return c != 0 ? c : compare(a, b);
}

inline char railKey(const char* s) {
  Cursor c(s);
  const char first = c.next();
  if (isAlpha(first)) return first >= 'a' ? static_cast<char>(first - 'a' + 'A') : first;
  return '#';
}

// Every code point of `s` folded by the old Full folding to something other
// than '?': the names whose old order the new one must keep.
inline bool oldSpells(const char* s) {
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp == 0) break;
    const char* rep = replacement(cp);
    if (rep && rep[0] == '?' && rep[1] == 0 && cp != 0xBF) return false;
  }
  return true;
}

}  // namespace legacy
