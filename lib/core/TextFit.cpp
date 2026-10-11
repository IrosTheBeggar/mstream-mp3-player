// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TextFit.h"

#include <cstdio>
#include <cstring>

#include "TextFold.h"

namespace textfit {

namespace {

constexpr size_t kTmp = 256;

bool has(const Font& f, uint32_t cp) { return !f.has || f.has(f.ctx, cp); }

const char* ellipsis(const Font& f) { return has(f, 0x2026) ? "\xE2\x80\xA6" : "..."; }

int width(const Font& f, const char* s) { return f.width ? f.width(f.ctx, s) : 0; }

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Back to a character boundary at or before n.
size_t boundary(const char* s, size_t n) {
  while (n > 0 && continuation(s[n])) --n;
  return n;
}

// The code point at s[n] (a boundary), 0 at the end.
uint32_t at(const char* s, size_t n) {
  const char* p = s + n;
  return textfold::decode(p);
}

// Back to where a character and the marks on it begin, at or before n: a
// cut never parts a letter from its combining mark (the kana's voicing
// mark, U+3099, which the glyph store draws over the letter before it).
size_t cluster(const char* s, size_t n) {
  n = boundary(s, n);
  while (n > 0 && textfold::combiningClass(at(s, n)) > 0) n = boundary(s, n - 1);
  return n;
}

// A space a line may end or begin with, dropped there: ' ' and the
// ideographic space U+3000 (which the glyph store draws 16 px wide).
size_t spaceAt(const char* s) {
  if (s[0] == ' ') return 1;
  return s[0] == '\xE3' && s[1] == '\x80' && s[2] == '\x80' ? 3 : 0;
}
// `keep` less the spaces it ends with.
size_t trimEnd(const char* s, size_t keep) {
  for (;;) {
    if (keep > 0 && s[keep - 1] == ' ') {
      --keep;
    } else if (keep >= 3 && spaceAt(s + keep - 3) == 3) {
      keep -= 3;
    } else {
      return keep;
    }
  }
}

// ---- where a line may break (wrap()) ----
//
// After a space, as always. And between two characters of which one is
// Chinese or Japanese (Han, kana, CJK punctuation, the fullwidth forms),
// which are written without spaces: a line may end between two of them, or
// between one of them and a Latin letter or digit; but it never starts
// with a closing mark (the curly quotes ” ’ too, which Chinese uses), an
// ellipsis, 〜, a small kana, ー, 々 or an iteration mark (the commonest of
// the kinsoku rules), nor ends with an opening mark (“ ‘ too). Hangul is
// written with spaces between its words: it breaks at them, and inside a
// word only when the word alone is wider than the line (as any word).

bool cjk(uint32_t cp) {
  const textfold::Script s = textfold::scriptOf(cp);
  return s == textfold::Script::Han || s == textfold::Script::Hiragana || s == textfold::Script::Katakana ||
         (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

// The small kana (ぁ ァ っ ッ ゃ ャ ゎ ヶ ㇰ...): kanaKey() makes them their large kana.
bool smallKana(uint32_t cp) {
  if (cp >= 0x31F0 && cp <= 0x31FF) return true;  // Katakana Phonetic Extensions: all small
  const uint32_t h = cp >= 0x30A1 && cp <= 0x30F6 ? cp - 0x60 : cp;  // katakana as hiragana
  return h >= 0x3041 && h <= 0x3096 && textfold::kanaKey(h) != h;
}

// A line never starts with it.
bool noBreakBefore(uint32_t cp) {
  switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0x30FB: case 0xFF1A: case 0xFF1B: case 0xFF1F:
    case 0xFF01: case 0xFF09: case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x3009: case 0x300B:
    case 0x3019: case 0x3017:  // 、。，．・：；？！）」』】〕〉》〙〗
    case 0x201D: case 0x2019: case 0x301F: case 0x301E:  // ” ’ 〟 〞
    case 0x2026: case 0x2025: case 0x301C:  // … ‥ 〜
    case 0x309D: case 0x309E: case 0x30FD: case 0x30FE: case 0x30FC: case 0x3005:  // ゝゞヽヾー々
    case 0x309B: case 0x309C:  // ゛゜
    case ',': case '.': case ':': case ';': case '?': case '!': case ')': case ']': case '}':
      return true;
    default:
      return smallKana(cp) || textfold::combiningClass(cp) > 0;
  }
}

// A line never ends with it.
bool noBreakAfter(uint32_t cp) {
  switch (cp) {
    case 0xFF08: case 0x300C: case 0x300E: case 0x3010: case 0x3014: case 0x3008: case 0x300A: case 0x3018:
    case 0x3016:  // （「『【〔〈《〘〖
    case 0x201C: case 0x2018: case 0x301D:  // “ ‘ 〝
    case '(': case '[': case '{':
      return true;
    default:
      return false;
  }
}

// Whether a line may break at s[n], a boundary inside the text.
bool breakAt(const char* s, size_t n) {
  const uint32_t a = at(s, boundary(s, n - 1)), b = at(s, n);
  if (textfold::combiningClass(b) > 0) return false;
  if (a == ' ') return true;
  if (!cjk(a) && !cjk(b)) return false;
  return !noBreakBefore(b) && !noBreakAfter(a);
}

// `text` cut to its first `keep` bytes (a boundary), trailing spaces
// dropped, plus the ellipsis, into out.
void cutInto(const char* text, size_t keep, const char* ell, char* out, size_t size) {
  keep = trimEnd(text, keep);
  const size_t el = std::strlen(ell);
  if (keep + el + 1 > size) keep = size > el + 1 ? cluster(text, size - el - 1) : 0;
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
    const size_t cut = cluster(text, mid);
    cutInto(text, cut, ell, tmp, cap);
    if (width(f, tmp) <= maxW) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  cutInto(text, cluster(text, lo), ell, text, size);
}

// What prepare() puts out for a composed code point: its bytes (in `utf8`,
// or a TextFold replacement) and their length in *len; *folded set when
// they aren't the code point's own.
const char* spell(const Font& f, uint32_t cp, char utf8[4], size_t* len, bool* folded) {
  if ((cp == 0x3099 || cp == 0x309A) && has(f, cp + 2)) {
    // A kana voicing mark the composer couldn't put on its letter (ア +
    // U+3099: no such kana) as the spacing mark, ゛ or ゜: drawn
    // combining, it would sit on the letter's own strokes at 16 px.
    *len = textfold::encode(cp + 2, utf8);
    *folded = true;
    return utf8;
  }
  if (has(f, cp)) {
    *len = textfold::encode(cp, utf8);
    return utf8;
  }
  if (textfold::fromC1(cp) != cp && has(f, textfold::fromC1(cp))) {
    *len = textfold::encode(textfold::fromC1(cp), utf8);  // a cp1252 byte, as its character
    *folded = true;
    return utf8;
  }
  if (const char* rep = textfold::replacement(cp, textfold::Mode::Full)) {
    *len = std::strlen(rep);
    *folded = true;
    return rep;
  }
  *len = textfold::encode(cp, utf8);  // ASCII the font lacks: as it is
  return utf8;
}

Result prepareBounded(const Font& f, const char* in, size_t inLen, char* out, size_t outSize, bool* truncated) {
  Result r;
  *truncated = false;
  if (outSize == 0) return r;
  textfold::Composer c(in, in + inLen);
  size_t o = 0;
  while (const uint32_t cp = c.next()) {
    char utf8[4];
    size_t len;
    const char* bytes = spell(f, cp, utf8, &len, &r.folded);
    if (o + len + 1 > outSize) {
      *truncated = true;
      break;
    }
    std::memcpy(out + o, bytes, len);
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

int drawnWidth(const Font& f, const char* text) {
  if (!text) return 0;
  // prepare()'s text a piece at a time, measured in pieces (the widths add
  // up): no buffer as long as the text.
  char piece[64];
  size_t n = 0;
  int w = 0;
  textfold::Composer c(text);
  while (const uint32_t cp = c.next()) {
    char utf8[4];
    size_t len;
    bool folded = false;
    const char* bytes = spell(f, cp, utf8, &len, &folded);
    if (n + len >= sizeof(piece)) {
      piece[n] = 0;
      w += width(f, piece);
      n = 0;
    }
    std::memcpy(piece + n, bytes, len);
    n += len;
  }
  piece[n] = 0;
  return n ? w + width(f, piece) : w;
}

bool drawnAny(const Font& f, const char* text, bool (*test)(void* ctx, uint32_t cp), void* ctx) {
  if (!text || !test) return false;
  textfold::Composer c(text);
  while (const uint32_t cp = c.next()) {
    char utf8[4];
    size_t len;
    bool folded = false;
    const char* bytes = spell(f, cp, utf8, &len, &folded);
    for (const char* p = bytes; p < bytes + len;) {
      const uint32_t drawn = textfold::decode(p, bytes + len);
      if (drawn == 0) break;
      if (test(ctx, drawn)) return true;
    }
  }
  return false;
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
    if (keep + el + 1 > outSize) keep = outSize > el + 1 ? cluster(out, outSize - el - 1) : 0;
    keep = cluster(out, keep > 0 ? keep - 1 : 0);  // one character less: something was dropped
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

void cutPathLeft(const Font& f, const char* path, int maxW, char* out, size_t size) {
  if (!out || size == 0) return;
  const char* p = path ? path : "";
  if (drawnWidth(f, p) > maxW) {
    // Whole folders off the left until "…/the rest" fits (a cut path
    // always says so, even where the rest alone would fit without it),
    // measured as drawn (a halfwidth or NFD folder name composed).
    for (const char* rest = std::strchr(p + 1, '/'); rest; rest = std::strchr(p + 1, '/')) {
      std::snprintf(out, size, "\xE2\x80\xA6%s", rest);
      if (drawnWidth(f, out) <= maxW) return;
      p = rest;
    }
  }
  std::snprintf(out, size, "%s", p);  // whole, or "/<last>" (the draw cuts its end)
}

int wrap(const Font& f, const char* in, size_t inLen, int maxW, int maxLines, char* out, size_t lineSize) {
  if (maxLines <= 0 || lineSize == 0) return 0;
  char text[kTmp];
  prepare(f, in, inLen, text, sizeof(text));
  const char* rest = text;
  while (const size_t sp = spaceAt(rest)) rest += sp;
  int lines = 0;
  char tmp[kTmp];
  while (*rest && lines < maxLines) {
    char* line = out + static_cast<size_t>(lines) * lineSize;
    const size_t n = std::strlen(rest);
    if (lines == maxLines - 1 || width(f, rest) <= maxW) {
      // The last line (or all that is left fits): the rest, ellipsised.
      const size_t keep = n < lineSize ? n : cluster(rest, lineSize - 1);
      std::memcpy(line, rest, keep);
      line[keep] = 0;
      if (keep < n) {
        cutInto(rest, cluster(rest, keep > 0 ? keep - 1 : 0), ellipsis(f), line, lineSize);
      }
      ellipsize(f, line, lineSize, maxW);
      ++lines;
      break;
    }
    // The longest prefix that fits.
    size_t lo = 0, hi = n;
    while (lo < hi) {
      size_t mid = boundary(rest, (lo + hi + 1) / 2);
      if (mid <= lo) {  // inside the character at lo (CJK is 3 bytes): the boundary after it
        mid = lo + 1;
        while (mid < hi && continuation(rest[mid])) ++mid;
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
    if (take < n) {
      // Break at the last place in it a line may end (after a space,
      // between CJK characters); a word with none is cut where it
      // overflows, a letter kept with its marks.
      size_t brk = take;
      while (brk > 0 && !breakAt(rest, brk)) brk = boundary(rest, brk - 1);
      take = brk > 0 ? brk : cluster(rest, take);
    }
    if (take == 0) {  // not even one character fits: take one anyway, with its marks
      take = 1;
      while (take < n && (continuation(rest[take]) || textfold::combiningClass(at(rest, take)) > 0)) ++take;
    }
    size_t keep = trimEnd(rest, take);
    if (keep >= lineSize) keep = cluster(rest, lineSize - 1);
    std::memcpy(line, rest, keep);
    line[keep] = 0;
    ++lines;
    rest += take;
    while (const size_t sp = spaceAt(rest)) rest += sp;
  }
  return lines;
}

}  // namespace textfit
