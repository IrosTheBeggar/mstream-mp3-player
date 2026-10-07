// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TrackName.h"

#include <cstring>

#include "TextFold.h"

namespace trackname {

namespace {

bool isSep(char c) { return c == ' ' || c == '-' || c == '.' || c == '_'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// How many digits from `at`, counting one past `most` at most (so a run
// longer than `most` shows as most + 1).
size_t digitsAt(const char* s, size_t len, size_t at, size_t most) {
  size_t n = 0;
  while (at + n < len && n <= most && isDigit(s[at + n])) ++n;
  return n;
}

uint32_t value(const char* s, size_t n) {
  uint32_t v = 0;
  for (size_t i = 0; i < n; ++i) v = v * 10 + static_cast<uint32_t>(s[i] - '0');
  return v;
}

// Past the separators from `at`; 0 when only separators are left (no title).
size_t titleFrom(const char* s, size_t len, size_t at) {
  while (at < len && isSep(s[at])) ++at;
  return at < len ? at : 0;
}

// The " - " at `k` in `s` (its first byte).
bool spacedDash(const char* s, size_t len, size_t k) {
  return k + 3 <= len && s[k] == ' ' && s[k + 1] == '-' && s[k + 2] == ' ';
}

// "CD2", "Disc 2", "disk 12" (the whole of `s`, spaces around allowed): the disc, else 0.
uint8_t discPart(const char* s, size_t len) {
  while (len && s[len - 1] == ' ') --len;
  size_t i = 0;
  while (i < len && s[i] == ' ') ++i;
  auto word = [&](const char* w) {
    const size_t n = std::strlen(w);
    if (len - i < n) return false;
    for (size_t k = 0; k < n; ++k) {
      char c = s[i + k];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != w[k]) return false;
    }
    i += n;
    return true;
  };
  if (!word("cd") && !word("disc") && !word("disk")) return 0;
  if (i < len && s[i] == ' ') ++i;
  const size_t n = digitsAt(s, len, i, 2);
  if (n < 1 || n > 2 || i + n != len) return 0;
  return static_cast<uint8_t>(value(s + i, n));
}

// A disc part at the end of a prefix: the whole of it ("CD2"), its last
// " - " part ("Album - Disc 2"), or in brackets at its end ("Album (Disc 2)",
// "Album [CD 2]"). The disc, else 0; *keyLen: the prefix without it.
uint8_t trailingDisc(const char* s, size_t len, size_t* keyLen) {
  *keyLen = len;
  uint8_t d = discPart(s, len);
  if (d) {
    *keyLen = 0;
    return d;
  }
  for (size_t k = len; k-- > 1;) {  // the last " - "
    if (!spacedDash(s, len, k)) continue;
    d = discPart(s + k + 3, len - k - 3);
    if (d) {
      *keyLen = k;
      return d;
    }
    break;
  }
  size_t end = len;
  while (end && s[end - 1] == ' ') --end;
  if (end < 2 || (s[end - 1] != ')' && s[end - 1] != ']')) return 0;
  const char open = s[end - 1] == ')' ? '(' : '[';
  size_t o = end - 1;  // just past the opener, once found
  while (o > 0 && s[o - 1] != open) --o;
  if (o == 0) return 0;
  d = discPart(s + o, end - 1 - o);
  if (!d) return 0;
  size_t k = o - 1;
  while (k && s[k - 1] == ' ') --k;
  *keyLen = k;
  return d;
}

// Two slices the same: byte for byte (so two empty ones, or a script Full
// folding can't spell), or as textfold::sameName() compares names.
bool sameText(const char* a, size_t aLen, const char* b, size_t bLen) {
  if (aLen == bLen && std::memcmp(a, b, aLen) == 0) return true;
  return textfold::sameName(a, aLen, b, bLen);
}

// "2-04 04-Title": where the title starts past the track's digits written
// again and joined to it by '-', '_' or '.'; 0 when it isn't so ("1-10 10
// Years": a space, the title's own number).
size_t pastRepeat(const char* stem, size_t len, const Name& n, size_t trackAt, size_t trackLen) {
  const size_t t = n.titleAt;
  if (t == 0 || t + trackLen >= len || std::memcmp(stem + t, stem + trackAt, trackLen) != 0) return 0;
  const char join = stem[t + trackLen];
  if (!isSep(join) || join == ' ') return 0;
  return titleFrom(stem, len, t + trackLen);
}

}  // namespace

bool plain(const char* stem, size_t len, Name* out) {
  *out = Name{};
  // "(06) Title", "[06] Title".
  if (len && (stem[0] == '(' || stem[0] == '[')) {
    const char close = stem[0] == '(' ? ')' : ']';
    const size_t n = digitsAt(stem, len, 1, 3);
    if (n < 1 || n > 3 || 1 + n + 1 >= len || stem[1 + n] != close || !isSep(stem[2 + n])) return false;
    const uint32_t v = value(stem + 1, n);
    const size_t at = titleFrom(stem, len, 2 + n);
    if (v > 255 || at == 0) return false;
    out->number = static_cast<uint8_t>(v);
    out->titleAt = static_cast<uint16_t>(at);
    return true;
  }
  // "06 - Title": up to three digits and a separator. "06 - " alone keeps
  // its number and the whole stem as its title.
  const size_t n = digitsAt(stem, len, 0, 3);
  if (n < 1 || n > 3 || n >= len || !isSep(stem[n])) return false;
  const uint32_t v = value(stem, n);
  if (v > 255) return false;
  out->number = static_cast<uint8_t>(v);
  out->titleAt = static_cast<uint16_t>(titleFrom(stem, len, n));
  return true;
}

bool discTrack(const char* stem, size_t len, Name* out, uint16_t* trackAt, uint8_t* trackLen) {
  const size_t d = digitsAt(stem, len, 0, 2);
  if (d < 1 || d > 2 || d >= len || (stem[d] != '-' && stem[d] != '.')) return false;
  const size_t t = digitsAt(stem, len, d + 1, 2);
  if (t < 1 || t > 2 || d + 1 + t >= len || !isSep(stem[d + 1 + t])) return false;
  const uint32_t disc = value(stem, d);
  const size_t at = titleFrom(stem, len, d + 1 + t);
  if (disc == 0 || at == 0) return false;
  out->disc = static_cast<uint8_t>(disc);
  out->number = static_cast<uint8_t>(value(stem + d + 1, t));
  out->titleAt = static_cast<uint16_t>(at);
  if (trackAt) *trackAt = static_cast<uint16_t>(d + 1);
  if (trackLen) *trackLen = static_cast<uint8_t>(t);
  return true;
}

bool hundreds(const char* stem, size_t len, Name* out) {
  if (digitsAt(stem, len, 0, 3) != 3 || len <= 3 || !isSep(stem[3])) return false;
  const uint32_t v = value(stem, 3);
  const size_t at = titleFrom(stem, len, 3);
  if (v < 100 || at == 0) return false;
  out->disc = static_cast<uint8_t>(v / 100);
  out->number = static_cast<uint8_t>(v % 100);
  out->titleAt = static_cast<uint16_t>(at);
  return true;
}

bool prefixed(const char* stem, size_t len, Prefixed* out, bool digitLead) {
  *out = Prefixed{};
  if (len == 0 || (!digitLead && isDigit(stem[0]))) return false;
  for (size_t k = 1; k + 3 < len; ++k) {  // the first " - " a number follows
    if (!spacedDash(stem, len, k)) continue;
    const size_t n = digitsAt(stem, len, k + 3, 3);
    const size_t end = k + 3 + n;
    if (n < 1 || n > 3 || end >= len || !isSep(stem[end])) continue;
    const uint32_t v = value(stem + k + 3, n);
    const size_t at = titleFrom(stem, len, end);
    if (v > 255 || at == 0) return false;
    bool spaced = false;
    for (size_t i = end; i < at; ++i) {
      spaced = spaced || stem[i] == ' ';
      out->dash = out->dash || (stem[i] == '-' && i > end && i + 1 < at && stem[i - 1] == ' ' && stem[i + 1] == ' ');
    }
    // "1-800 Lines", "24-7", "1.5 Hours": one number that runs on.
    if (!spaced && isDigit(stem[at])) {
      *out = Prefixed{};
      return false;
    }
    out->name.number = static_cast<uint8_t>(v);
    out->name.titleAt = static_cast<uint16_t>(at);
    out->prefixLen = static_cast<uint16_t>(k);
    size_t first = 0;
    while (first + 3 <= k && !spacedDash(stem, k, first)) ++first;
    out->firstLen = static_cast<uint16_t>(first + 3 <= k ? first : k);
    size_t keyLen;
    out->name.disc = trailingDisc(stem, k, &keyLen);
    out->keyLen = static_cast<uint16_t>(keyLen);
    out->padded = n >= 2 && stem[k + 3] == '0';
    return true;
  }
  return false;
}

size_t afterArtist(const char* title, size_t len, const char* artist) {
  if (!artist || !*artist) return 0;
  const size_t artistLen = std::strlen(artist);
  for (size_t k = 1; k + 3 <= len; ++k) {  // each " - ", the first that ends the artist
    if (!spacedDash(title, len, k)) continue;
    if (!textfold::sameName(title, k, artist, artistLen)) continue;
    size_t at = k + 3;
    while (at < len && title[at] == ' ') ++at;
    return at < len ? at : 0;  // nothing after it: keep the whole title
  }
  return 0;
}

Folder::Folder(const char* artist) : artist_(artist ? artist : "") {}

bool Folder::shape(const char* stem, size_t len, Prefixed* p, size_t* artistEnd) const {
  *artistEnd = 0;
  if (!prefixed(stem, len, p, true)) return false;
  if (*artist_) {
    const size_t artistLen = std::strlen(artist_);
    for (size_t k = 1; k <= p->prefixLen; ++k) {  // each " - " in the prefix, then its end
      if (k < p->prefixLen && !spacedDash(stem, p->prefixLen, k)) continue;
      if (textfold::startsWithName(stem, k, artist_, artistLen)) {
        *artistEnd = k;
        break;
      }
    }
  }
  return *artistEnd != 0 || !isDigit(stem[0]);
}

bool Folder::numbered(const Prefixed& p) const {
  if (p.dash) return true;
  return prefixed_ >= 2 && 2 * prefixed_ >= files_ && !numberTwice_ && (prefixLowest_ <= 1 || padded_);
}

void Folder::add(const char* stem, size_t len) {
  ++files_;
  Prefixed p;
  size_t artistEnd;
  const bool pre = shape(stem, len, &p, &artistEnd);
  // "10 Lanterns - 01 - Title": the digits are the artist's, not a number.
  if (len && isDigit(stem[0]) && !pre) {
    ++lead_;
    Name n;
    uint16_t trackAt = 0;
    uint8_t trackLen = 0;
    if (discTrack(stem, len, &n, &trackAt, &trackLen)) {
      ++discTrack_;
      if (pastRepeat(stem, len, n, trackAt, trackLen)) ++repeats_;
    }
    if (hundreds(stem, len, &n)) {
      ++hundreds_;
      if (n.number < lowest_[n.disc]) lowest_[n.disc] = n.number;
    }
  }
  if (pre) {
    ++prefixed_;
    const Name& n = p.name;
    const bool copy = lastStem_ && lastLen_ == len && std::memcmp(lastStem_, stem, len) == 0;
    if (!copy) {
      const uint8_t bit = static_cast<uint8_t>(1u << (n.disc < 7 ? n.disc : 7));
      if (seen_[n.number] & bit) numberTwice_ = true;
      seen_[n.number] |= bit;
    }
    if (n.number < prefixLowest_) prefixLowest_ = n.number;
    padded_ = padded_ || p.padded;
    if (!firstKey_) {
      firstKey_ = stem;
      firstKeyLen_ = p.keyLen;
    } else if (!sameText(firstKey_, firstKeyLen_, stem, p.keyLen)) {
      samePrefix_ = false;
    }
    if (artistEnd) {
      const char* rest = stem + artistEnd;
      size_t restLen = 0;
      if (p.keyLen > artistEnd + 3) {
        rest += 3;
        restLen = p.keyLen - artistEnd - 3;
      }
      if (!firstRest_) {
        firstRest_ = rest;
        firstRestLen_ = restLen;
      } else if (!sameText(firstRest_, firstRestLen_, rest, restLen)) {
        sameRest_ = false;
      }
    }
  }
  lastStem_ = stem;
  lastLen_ = len;
}

bool Folder::hundredsRule() const {
  if (hundreds_ < 2 || hundreds_ != lead_) return false;
  for (uint8_t low : lowest_) {
    if (low != 0xFF && low > 1) return false;
  }
  return true;
}

Name Folder::read(const char* stem, size_t len) const {
  Name n;
  uint16_t trackAt = 0;
  uint8_t trackLen = 0;
  Prefixed p;
  size_t artistEnd;
  const bool pre = shape(stem, len, &p, &artistEnd);
  const bool artistDigits = pre && len && isDigit(stem[0]);  // "10 Lanterns - 01 - Title"
  if (!artistDigits && discTracks() && discTrack(stem, len, &n, &trackAt, &trackLen)) {
    // "2-04 04-Title": the number written again goes too, when the folder
    // writes it so.
    const size_t at = 2 * repeats_ >= discTrack_ ? pastRepeat(stem, len, n, trackAt, trackLen) : 0;
    if (at) n.titleAt = static_cast<uint16_t>(at);
  } else if (!artistDigits && hundredsRule() && hundreds(stem, len, &n)) {
  } else if (pre && numbered(p) &&
             ((artistEnd && sameRest_) || (p.keyLen == 0 && p.name.disc) || samePrefix())) {
    n = p.name;
  } else if (!artistDigits) {
    plain(stem, len, &n);
  }
  const size_t cut = afterArtist(stem + n.titleAt, len - n.titleAt, artist_);
  if (cut) n.titleAt = static_cast<uint16_t>(n.titleAt + cut);
  return n;
}

}  // namespace trackname
