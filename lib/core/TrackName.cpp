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

bool prefixed(const char* stem, size_t len, Prefixed* out) {
  *out = Prefixed{};
  if (len == 0 || isDigit(stem[0])) return false;
  for (size_t k = 1; k + 3 < len; ++k) {  // the first " - " a number follows
    if (stem[k] != ' ' || stem[k + 1] != '-' || stem[k + 2] != ' ') continue;
    const size_t n = digitsAt(stem, len, k + 3, 3);
    const size_t end = k + 3 + n;
    if (n < 1 || n > 3 || end >= len || !isSep(stem[end])) continue;
    const uint32_t v = value(stem + k + 3, n);
    const size_t at = titleFrom(stem, len, end);
    if (v > 255 || at == 0) return false;
    out->name.number = static_cast<uint8_t>(v);
    out->name.titleAt = static_cast<uint16_t>(at);
    out->prefixLen = static_cast<uint16_t>(k);
    size_t first = 0;
    while (first + 3 <= k && !(stem[first] == ' ' && stem[first + 1] == '-' && stem[first + 2] == ' ')) ++first;
    out->firstLen = static_cast<uint16_t>(first + 3 <= k ? first : k);
    out->name.disc = discPart(stem, k);
    for (size_t i = end; i < at; ++i) out->dash = out->dash || stem[i] == '-';
    return true;
  }
  return false;
}

size_t afterArtist(const char* title, size_t len, const char* artist) {
  if (!artist || !*artist) return 0;
  for (size_t k = 1; k + 3 <= len; ++k) {
    if (title[k] != ' ' || title[k + 1] != '-' || title[k + 2] != ' ') continue;
    size_t at = k + 3;
    while (at < len && title[at] == ' ') ++at;
    if (at >= len) return 0;  // nothing after it: keep the whole title
    return textfold::sameName(title, k, artist, std::strlen(artist)) ? at : 0;
  }
  return 0;
}

void Folder::add(const char* stem, size_t len) {
  ++files_;
  if (len && isDigit(stem[0])) ++lead_;
  Name n;
  if (discTrack(stem, len, &n)) ++discTrack_;
  if (hundreds(stem, len, &n)) {
    ++hundreds_;
    if (n.number < lowest_[n.disc]) lowest_[n.disc] = n.number;
  }
  Prefixed p;
  if (prefixed(stem, len, &p)) {
    ++prefixed_;
    if (!firstPrefix_) {
      firstPrefix_ = stem;
      firstPrefixLen_ = p.prefixLen;
    } else if (!textfold::sameName(firstPrefix_, firstPrefixLen_, stem, p.prefixLen)) {
      samePrefix_ = false;
    }
  }
}

bool Folder::hundredsRule() const {
  if (hundreds_ < 2 || hundreds_ != lead_) return false;
  for (uint8_t low : lowest_) {
    if (low != 0xFF && low > 1) return false;
  }
  return true;
}

Name Folder::read(const char* stem, size_t len, const char* artist) const {
  if (!artist) artist = "";
  Name n;
  uint16_t trackAt = 0;
  uint8_t trackLen = 0;
  Prefixed p;
  if (discTracks() && discTrack(stem, len, &n, &trackAt, &trackLen)) {
    // "2-04 04-Title": the number written again goes too.
    const size_t t = n.titleAt;
    if (t + trackLen < len && std::memcmp(stem + t, stem + trackAt, trackLen) == 0 && isSep(stem[t + trackLen])) {
      const size_t at = titleFrom(stem, len, t + trackLen);
      if (at) n.titleAt = static_cast<uint16_t>(at);
    }
  } else if (hundredsRule() && hundreds(stem, len, &n)) {
  } else if (prefixed(stem, len, &p) &&
             ((prefixed_ >= 2 && 2 * prefixed_ >= files_) || p.dash) &&
             (textfold::startsWithName(stem, p.firstLen, artist, std::strlen(artist)) || p.name.disc ||
              samePrefix())) {
    n = p.name;
  } else {
    plain(stem, len, &n);
  }
  const size_t cut = afterArtist(stem + n.titleAt, len - n.titleAt, artist);
  if (cut) n.titleAt = static_cast<uint16_t>(n.titleAt + cut);
  return n;
}

}  // namespace trackname
