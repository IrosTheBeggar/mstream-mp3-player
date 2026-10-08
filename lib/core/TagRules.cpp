// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagRules.h"

#include <cstring>

#include "NameKey.h"

namespace tagrules {

namespace {

// lofty 0.25.1's id3/v1/constants.rs, GENRES, in order (the reference
// reader's names: mStream shows these).
const char* const kGenres[kGenreCount] = {
    "Blues", "Classic rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative",
    "Ska", "Death metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz & Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native US",
    "Cabaret", "New Wave", "Psychedelic", "Rave", "Show tunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk",
    "Acid Jazz", "Polka", "Retro", "Musical", "Rock \xE2\x80\x99n\xE2\x80\x99 Roll", "Hard Rock", "Folk", "Folk-Rock",
    "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber music", "Sonata", "Symphony",
    "Booty bass", "Primus", "Porn groove", "Satire", "Slow jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A cappella", "Euro-House",
    "Dance Hall", "Goa music", "Drum & Bass", "Club-House", "Hardcore Techno", "Terror", "Indie", "BritPop",
    "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "Jpop", "Synthpop",
    "Abstract", "Art Rock", "Baroque", "Bhangra", "Big beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM",
    "Eclectic", "Electro", "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient",
    "Industro-Goth", "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
    "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical",
    "Audiobook", "Audio theatre", "Neue Deutsche Welle", "Podcast", "Indie-Rock", "G-Funk", "Dubstep",
    "Garage Rock", "Psybient",
};

// One code point of UTF-8 at s[i] (i < n): its value and length; an invalid
// byte reads as itself with length 1 and value 0xFFFFFFFF (never whitespace).
uint32_t decodeAt(const char* s, size_t n, size_t i, size_t* len) {
  const uint8_t b = static_cast<uint8_t>(s[i]);
  if (b < 0x80) {
    *len = 1;
    return b;
  }
  size_t need = 0;
  uint32_t cp = 0;
  if ((b & 0xE0) == 0xC0) {
    need = 1;
    cp = b & 0x1F;
  } else if ((b & 0xF0) == 0xE0) {
    need = 2;
    cp = b & 0x0F;
  } else if ((b & 0xF8) == 0xF0) {
    need = 3;
    cp = b & 0x07;
  } else {
    *len = 1;
    return 0xFFFFFFFFu;
  }
  if (i + 1 + need > n) {
    *len = 1;
    return 0xFFFFFFFFu;
  }
  for (size_t k = 1; k <= need; ++k) {
    const uint8_t c = static_cast<uint8_t>(s[i + k]);
    if ((c & 0xC0) != 0x80) {
      *len = 1;
      return 0xFFFFFFFFu;
    }
    cp = (cp << 6) | (c & 0x3F);
  }
  *len = need + 1;
  return cp;
}

// The start of the last code point ending at s[end - 1] (end > 0).
size_t lastStart(const char* s, size_t end) {
  size_t k = end - 1;
  for (size_t back = 0; back < 3 && k > 0 && (static_cast<uint8_t>(s[k]) & 0xC0) == 0x80; ++back) --k;
  return k;
}

bool asciiIEq(const char* a, size_t an, const char* b) {
  const size_t bn = std::strlen(b);
  if (an != bn) return false;
  for (size_t i = 0; i < an; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
  }
  return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

const char* genreName(uint32_t index) { return index < kGenreCount ? kGenres[index] : nullptr; }

bool parseU32(const char* s, size_t n, uint32_t* out) {
  size_t i = 0;
  if (n > 0 && s[0] == '+') i = 1;
  if (i >= n) return false;
  uint64_t v = 0;
  for (; i < n; ++i) {
    if (!isDigit(s[i])) return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

void trim(const char*& s, size_t& n) {
  size_t a = 0;
  while (a < n) {
    size_t len = 1;
    const uint32_t cp = decodeAt(s, n, a, &len);
    if (cp == 0xFFFFFFFFu || !namekey::isWhiteSpace(cp)) break;
    a += len;
  }
  size_t b = n;
  while (b > a) {
    const size_t k = lastStart(s, b);
    size_t len = 1;
    const uint32_t cp = decodeAt(s, b, k, &len);
    if (cp == 0xFFFFFFFFu || k + len != b || !namekey::isWhiteSpace(cp)) break;
    b = k;
  }
  s += a;
  n = b - a;
}

bool isBlank(const char* s, size_t n) {
  trim(s, n);
  return n == 0;
}

const char* parseGenre(const char* g, size_t n, size_t* outLen) {
  *outLen = n;
  if (n > 3) return g;
  uint32_t id = 0;
  if (parseIndex(g, n, &id)) {
    if (id < kGenreCount) {
      *outLen = std::strlen(kGenres[id]);
      return kGenres[id];
    }
    return g;
  }
  if (n == 2 && g[0] == 'R' && g[1] == 'X') {
    *outLen = 5;
    return "Remix";
  }
  if (n == 2 && g[0] == 'C' && g[1] == 'R') {
    *outLen = 5;
    return "Cover";
  }
  return g;
}

bool nextParenItem(const char* s, size_t n, size_t* pos, size_t* itemAt, size_t* itemLen) {
  if (*pos >= n) return false;
  const char* rem = s + *pos;
  const size_t remLen = n - *pos;
  const void* close = std::memchr(rem, ')', remLen);
  if (remLen > 0 && rem[0] == '(' && close) {
    const size_t closeAt = static_cast<size_t>(static_cast<const char*>(close) - rem);
    const size_t start = *pos + 1;
    size_t end = *pos + closeAt;
    *pos = end + 1;
    if (remLen >= 2 && rem[1] == '(') ++end;  // "((": a bracketed refinement keeps its bracket
    *itemAt = start;
    *itemLen = end > start ? end - start : 0;
    return true;
  }
  *itemAt = *pos;
  *itemLen = remLen;
  *pos = n;
  return true;
}

namespace {

bool asciiWs(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

// lofty's Timestamp::segment::<SIZE> (relaxed).
enum class Seg : uint8_t { Value, Stop, Error };
Seg segment(const char* c, size_t n, size_t* at, size_t size, const char* seps, uint16_t* value) {
  static const char kSeparators[] = "-.T:";
  if (*at >= n) return Seg::Stop;
  if (seps) {
    const char b = c[(*at)++];  // consumed whether it is a separator or not
    if (!std::strchr(seps, b) || b == 0) return Seg::Stop;
  }
  if (n - *at < size) return Seg::Stop;
  bool any = false;
  uint16_t num = 0;
  size_t count = 0;
  for (size_t k = 0; k < size; ++k) {
    const char i = c[*at + k];
    if (i == ' ') {
      ++count;
      continue;
    }
    if (!isDigit(i)) {
      if (seps && i != 0 && std::strchr(kSeparators, i)) break;
      return Seg::Error;
    }
    num = static_cast<uint16_t>(num * 10 + (i - '0'));
    any = true;
    ++count;
  }
  if (!any) return Seg::Stop;
  *at += count;
  *value = num;
  return Seg::Value;
}

}  // namespace

TsParse parseTimestamp(const char* s, size_t n, Timestamp* out) {
  *out = Timestamp();
  size_t i = 0;
  if (n == 0) return TsParse::None;
  while (i < n && asciiWs(static_cast<uint8_t>(s[i]))) ++i;
  if (i >= n) return TsParse::Error;  // lofty reads past the end looking for a non-space: an I/O error
  const size_t len = (n - i) < 19 ? (n - i) : 19;
  const char* c = s + i;
  bool hasSep = false;
  for (size_t k = 0; k < len; ++k)
    if (c[k] == '-' || c[k] == '.' || c[k] == ':') hasSep = true;
  size_t at = 0;
  uint16_t v = 0;
  // The year: exactly 4 bytes read.
  if (len < 4) return TsParse::None;
  {
    bool any = false;
    uint16_t num = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char ch = c[k];
      if (ch == ' ') continue;
      if (!isDigit(ch)) return TsParse::Error;
      num = static_cast<uint16_t>(num * 10 + (ch - '0'));
      any = true;
    }
    if (!any) return TsParse::None;
    out->year = num;
    at = 4;
  }
  if (at >= len) return TsParse::Ok;
  uint8_t* fields[5] = {&out->month, &out->day, &out->hour, &out->minute, &out->second};
  const char* seps[5] = {hasSep ? "-." : nullptr, hasSep ? "-." : nullptr, "T", hasSep ? ":" : nullptr,
                         hasSep ? ":" : nullptr};
  for (int f = 0; f < 5; ++f) {
    const Seg r = segment(c, len, &at, 2, seps[f], &v);
    if (r == Seg::Error) return TsParse::Error;
    if (r == Seg::Stop) break;
    *fields[f] = static_cast<uint8_t>(v);
    out->fields = static_cast<uint8_t>(f + 1);
  }
  return TsParse::Ok;
}

bool verifyTimestamp(const Timestamp& t) {
  if (t.year > 9999) return false;
  if (t.fields >= 1 && t.month > 12) return false;
  if (t.fields >= 2 && t.day > 31) return false;
  if (t.fields >= 3 && t.hour > 23) return false;
  if (t.fields >= 4 && t.minute > 59) return false;
  if (t.fields >= 5 && t.second > 59) return false;
  return true;
}

bool yearOf(const char* s, size_t n, uint16_t* out) {
  size_t i = 0;
  while (i < n) {
    size_t len = 1;
    const uint32_t cp = decodeAt(s, n, i, &len);
    if (cp == 0xFFFFFFFFu || !namekey::isWhiteSpace(cp)) break;
    i += len;
  }
  if (n - i < 4) return false;
  uint16_t y = 0;
  for (size_t k = 0; k < 4; ++k) {
    if (!isDigit(s[i + k])) return false;
    y = static_cast<uint16_t>(y * 10 + (s[i + k] - '0'));
  }
  *out = y;
  return true;
}

uint8_t id3Pair(const char* s, size_t n, uint32_t* number, uint32_t* total, bool longer) {
  *number = *total = 0;
  size_t cut = n;
  for (size_t i = 0; i < n; ++i)
    if (s[i] == 0 || s[i] == '/') {
      cut = i;
      break;
    }
  const char* a = s;
  size_t an = cut;
  trim(a, an);
  uint32_t num = 0, tot = 0;
  bool split = !(longer && cut == n) && an > 0 && parseU32(a, an, &num);
  bool hasTotal = false;
  if (split && cut < n) {
    const char* b = s + cut + 1;
    size_t bn = n - cut - 1;
    trim(b, bn);
    split = !longer && bn > 0 && parseU32(b, bn, &tot);
    hasTotal = split;
  }
  if (split) {
    *number = num;
    if (hasTotal) *total = tot;
    return static_cast<uint8_t>(kHaveNumber | (hasTotal ? kHaveTotal : 0));
  }
  // Not split: lofty's map sends the frame to the total, split on NUL; the
  // first part, untrimmed.
  size_t first = n;
  for (size_t i = 0; i < n; ++i)
    if (s[i] == 0) {
      first = i;
      break;
    }
  uint32_t v = 0;
  if (!(longer && first == n) && parseU32(s, first, &v)) {
    *total = v;
    return kHaveTotal;
  }
  return 0;
}

uint8_t numOf(const char* s, size_t n, uint32_t* number, uint32_t* total) {
  *number = *total = 0;
  uint8_t have = 0;
  size_t cut = n;
  for (size_t i = 0; i < n; ++i)
    if (s[i] == '/') {
      cut = i;
      break;
    }
  const char* a = s;
  size_t an = cut;
  trim(a, an);
  uint32_t v = 0;
  if (parseU32(a, an, &v)) {
    *number = v;
    have |= kHaveNumber;
  }
  if (cut < n) {
    const char* b = s + cut + 1;
    size_t bn = n - cut - 1;
    trim(b, bn);
    if (parseU32(b, bn, &v)) {
      *total = v;
      have |= kHaveTotal;
    }
  }
  return have;
}

uint8_t compilationOf(const char* s, size_t n) {
  if ((n == 1 && s[0] == '1') || asciiIEq(s, n, "true")) return 1;
  if ((n == 1 && s[0] == '0') || asciiIEq(s, n, "false")) return 2;
  return 0;
}

namespace {

// mStream's CAMELOT_TO_KEYS (src/api/random.js at 926b97b1), row by row: 1A,
// 1B, 2A, ... 12B. A row's code is its first alias.
const char* const kCamelot[24][8] = {
    {"1A", "Ab minor", "Abmin", "G# minor", "G#min", "Abm", "G#m", nullptr},
    {"1B", "B major", "Bmaj", "B", nullptr},
    {"2A", "Eb minor", "Ebmin", "D# minor", "D#min", "Ebm", "D#m", nullptr},
    {"2B", "F# major", "F#maj", "Gb major", "Gbmaj", "F#", "Gb", nullptr},
    {"3A", "Bb minor", "Bbmin", "A# minor", "A#min", "Bbm", "A#m", nullptr},
    {"3B", "Db major", "Dbmaj", "C# major", "C#maj", "Db", "C#", nullptr},
    {"4A", "F minor", "Fmin", "Fm", nullptr},
    {"4B", "Ab major", "Abmaj", "G# major", "G#maj", "Ab", "G#", nullptr},
    {"5A", "C minor", "Cmin", "Cm", nullptr},
    {"5B", "Eb major", "Ebmaj", "D# major", "D#maj", "Eb", "D#", nullptr},
    {"6A", "G minor", "Gmin", "Gm", nullptr},
    {"6B", "Bb major", "Bbmaj", "A# major", "A#maj", "Bb", "A#", nullptr},
    {"7A", "D minor", "Dmin", "Dm", nullptr},
    {"7B", "F major", "Fmaj", "F", nullptr},
    {"8A", "A minor", "Amin", "Am", nullptr},
    {"8B", "C major", "Cmaj", "C", nullptr},
    {"9A", "E minor", "Emin", "Em", nullptr},
    {"9B", "G major", "Gmaj", "G", nullptr},
    {"10A", "B minor", "Bmin", "Bm", nullptr},
    {"10B", "D major", "Dmaj", "D", nullptr},
    {"11A", "F# minor", "F#min", "Gb minor", "Gbmin", "F#m", "Gbm", nullptr},
    {"11B", "A major", "Amaj", "A", nullptr},
    {"12A", "C# minor", "C#min", "Db minor", "Dbmin", "C#m", "Dbm", nullptr},
    {"12B", "E major", "Emaj", "E", nullptr},
};

}  // namespace

uint8_t camelotOf(const char* s, size_t n) {
  trim(s, n);
  // The first 12 characters (code points).
  size_t i = 0;
  for (int c = 0; c < 12 && i < n; ++c) {
    size_t len = 1;
    decodeAt(s, n, i, &len);
    i += len;
  }
  n = i;
  if (n == 0) return 0;
  for (int row = 0; row < 24; ++row)
    for (int k = 0; k < 8 && kCamelot[row][k]; ++k)
      if (asciiIEq(s, n, kCamelot[row][k])) {
        // Row r is (r / 2 + 1) A or B: A (minor) 1-12, B (major) 13-24.
        const int number = row / 2 + 1;
        return static_cast<uint8_t>((row % 2) ? 12 + number : number);
      }
  return 0;
}

uint8_t mimeOf(const char* s, size_t n) {
  if (asciiIEq(s, n, "image/jpeg") || asciiIEq(s, n, "image/jpg")) return 1;
  if (asciiIEq(s, n, "image/png")) return 2;
  return 3;
}

uint8_t mimeOfMagic(const uint8_t m[8]) {
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(m, kPng, 8) == 0) return 2;
  if (m[0] == 0xFF && m[1] == 0xD8) return 1;
  if (m[0] == 'G' && m[1] == 'I' && m[2] == 'F' && m[3] == 0x38 && (m[4] == 0x37 || m[4] == 0x39) && m[5] == 'a')
    return 3;
  if (m[0] == 'B' && m[1] == 'M') return 3;
  if ((m[0] == 'I' && m[1] == 'I' && m[2] == '*' && m[3] == 0) || (m[0] == 'M' && m[1] == 'M' && m[2] == 0 && m[3] == '*'))
    return 3;
  return 0;
}

int apePictureType(const char* key, size_t n) {
  static const char* const kKeys[21] = {
      "Cover Art (Other)",        "Cover Art (Png Icon)",       "Cover Art (Icon)",
      "Cover Art (Front)",        "Cover Art (Back)",           "Cover Art (Leaflet)",
      "Cover Art (Media)",        "Cover Art (Lead Artist)",    "Cover Art (Artist)",
      "Cover Art (Conductor)",    "Cover Art (Band)",           "Cover Art (Composer)",
      "Cover Art (Lyricist)",     "Cover Art (Recording Location)", "Cover Art (During Recording)",
      "Cover Art (During Performance)", "Cover Art (Video Capture)", "Cover Art (Fish)",
      "Cover Art (Illustration)", "Cover Art (Band Logotype)",  "Cover Art (Publisher Logotype)",
  };
  for (int t = 0; t < 21; ++t)
    if (asciiIEq(key, n, kKeys[t])) return t;
  return -1;
}

}  // namespace tagrules
