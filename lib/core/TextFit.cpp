// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TextFit.h"

#include <cstring>

#include "TextFold.h"

namespace textfit {

namespace {

constexpr size_t kTmp = 256;

// One code point of [p, end), advancing p; a malformed or cut sequence is
// U+FFFD and one byte.
uint32_t next(const char*& p, const char* end) {
  const auto c = static_cast<unsigned char>(*p);
  const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (n == 0 || p + n > end) {
    ++p;
    return 0xFFFD;
  }
  uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
  for (int i = 1; i < n; ++i) {
    const auto cc = static_cast<unsigned char>(p[i]);
    if ((cc & 0xC0) != 0x80) {
      ++p;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  p += n;
  return cp;
}

bool has(const Font& f, uint32_t cp) { return !f.has || f.has(f.ctx, cp); }

const char* ellipsis(const Font& f) { return has(f, 0x2026) ? "\xE2\x80\xA6" : "..."; }

int width(const Font& f, const char* s) { return f.width ? f.width(f.ctx, s) : 0; }

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Back to a character boundary at or before n.
size_t boundary(const char* s, size_t n) {
  while (n > 0 && continuation(s[n])) --n;
  return n;
}

// `text` cut to its first `keep` bytes (a boundary), trailing spaces
// dropped, plus the ellipsis, into out.
void cutInto(const char* text, size_t keep, const char* ell, char* out, size_t size) {
  while (keep > 0 && text[keep - 1] == ' ') --keep;
  const size_t el = std::strlen(ell);
  if (keep + el + 1 > size) keep = size > el + 1 ? boundary(text, size - el - 1) : 0;
  if (out != text) std::memmove(out, text, keep);
  std::memcpy(out + keep, ell, el + 1);
}

// The longest prefix of `text` that fits with the ellipsis, written in place.
void cutToFit(const Font& f, char* text, size_t size, int maxW) {
  const char* ell = ellipsis(f);
  const size_t n = std::strlen(text);
  char tmp[kTmp];
  const size_t cap = size < kTmp ? size : kTmp;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = (lo + hi + 1) / 2;
    const size_t cut = boundary(text, mid);
    cutInto(text, cut, ell, tmp, cap);
    if (width(f, tmp) <= maxW) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  cutInto(text, boundary(text, lo), ell, text, size);
}

Result prepareBounded(const Font& f, const char* in, size_t inLen, char* out, size_t outSize, bool* truncated) {
  Result r;
  *truncated = false;
  if (outSize == 0) return r;
  const char* p = in;
  const char* end = in + inLen;
  size_t o = 0;
  while (p < end && *p) {
    const char* start = p;
    const uint32_t cp = next(p, end);
    const char* rep = nullptr;
    size_t len = static_cast<size_t>(p - start);
    if (!has(f, cp)) {
      rep = textfold::replacement(cp, textfold::Mode::Full);
      if (rep) {
        len = std::strlen(rep);
        r.folded = true;
      }
    }
    if (o + len + 1 > outSize) {
      *truncated = true;
      break;
    }
    std::memcpy(out + o, rep ? rep : start, len);
    o += len;
  }
  out[o] = 0;
  r.length = o;
  return r;
}

}  // namespace

Result prepare(const Font& f, const char* in, size_t inLen, char* out, size_t outSize) {
  bool truncated = false;
  return prepareBounded(f, in, inLen, out, outSize, &truncated);
}

bool ellipsize(const Font& f, char* text, size_t size, int maxW) {
  if (width(f, text) <= maxW) return false;
  cutToFit(f, text, size, maxW);
  return true;
}

Result fit(const Font& f, const char* in, size_t inLen, char* out, size_t outSize, int maxW) {
  bool truncated = false;
  Result r = prepareBounded(f, in, inLen, out, outSize, &truncated);
  if (outSize == 0) return r;
  if (truncated) {
    // The buffer cut it: it ends in an ellipsis whatever its width.
    const char* ell = ellipsis(f);
    const size_t el = std::strlen(ell);
    size_t keep = r.length;
    if (keep + el + 1 > outSize) keep = outSize > el + 1 ? boundary(out, outSize - el - 1) : 0;
    keep = boundary(out, keep > 0 ? keep - 1 : 0);  // one character less: something was dropped
    cutInto(out, keep, ell, out, outSize);
    r.cut = true;
  }
  if (width(f, out) > maxW) {
    cutToFit(f, out, outSize, maxW);
    r.cut = true;
  }
  r.length = std::strlen(out);
  return r;
}

int wrap(const Font& f, const char* in, size_t inLen, int maxW, int maxLines, char* out, size_t lineSize) {
  if (maxLines <= 0 || lineSize == 0) return 0;
  char text[kTmp];
  prepare(f, in, inLen, text, sizeof(text));
  const char* rest = text;
  while (*rest == ' ') ++rest;
  int lines = 0;
  char tmp[kTmp];
  while (*rest && lines < maxLines) {
    char* line = out + static_cast<size_t>(lines) * lineSize;
    const size_t n = std::strlen(rest);
    if (lines == maxLines - 1 || width(f, rest) <= maxW) {
      // The last line (or all that is left fits): the rest, ellipsised.
      const size_t keep = n < lineSize ? n : boundary(rest, lineSize - 1);
      std::memcpy(line, rest, keep);
      line[keep] = 0;
      if (keep < n) {
        cutInto(rest, boundary(rest, keep > 0 ? keep - 1 : 0), ellipsis(f), line, lineSize);
      }
      ellipsize(f, line, lineSize, maxW);
      ++lines;
      break;
    }
    // The longest prefix that fits.
    size_t lo = 0, hi = n;
    while (lo < hi) {
      const size_t mid = boundary(rest, (lo + hi + 1) / 2);
      if (mid <= lo) {
        hi = lo;
        break;
      }
      const size_t m = mid < sizeof(tmp) ? mid : sizeof(tmp) - 1;
      std::memcpy(tmp, rest, m);
      tmp[m] = 0;
      if (width(f, tmp) <= maxW) {
        lo = mid;
      } else {
        // The next boundary below mid.
        hi = boundary(rest, mid - 1);
        if (hi < lo) hi = lo;
      }
    }
    size_t take = lo;
    // Break after the last space in it, if there is one.
    size_t space = take;
    while (space > 0 && rest[space - 1] != ' ') --space;
    if (space > 0 && take < n) take = space;
    if (take == 0) {  // not even one character fits: take one anyway
      take = 1;
      while (take < n && continuation(rest[take])) ++take;
    }
    size_t keep = take;
    while (keep > 0 && rest[keep - 1] == ' ') --keep;
    if (keep >= lineSize) keep = boundary(rest, lineSize - 1);
    std::memcpy(line, rest, keep);
    line[keep] = 0;
    ++lines;
    rest += take;
    while (*rest == ' ') ++rest;
  }
  return lines;
}

}  // namespace textfit
