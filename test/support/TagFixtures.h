// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// The tag reader's parity corpus (test/fixtures/tags, docs/METADATA.md 2.17
// item 3) for the host tests: where it is, its files, a record against the
// reference reader's (expected.json, made by tools/tagref), and a reader of
// each picCoding (2.6.4) that takes an anchor back to the image's bytes.
// Header-only, test-only.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CardContract.h"
#include "CardTags.h"
#include "MiniJson.h"
#include "TagScan.h"

namespace tagfixtures {

using Bytes = std::vector<uint8_t>;

inline bool exists(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

// test/fixtures/tags, found from the working directory or this file's path.
inline const std::string& dir() {
  static const std::string d = [] {
    const char* tries[] = {"test/fixtures/tags", "../test/fixtures/tags", "../../test/fixtures/tags"};
    for (const char* t : tries)
      if (exists(std::string(t) + "/expected.json")) return std::string(t);
    std::string self = __FILE__;  // .../test/support/TagFixtures.h
    for (int up = 0; up < 2; ++up) {
      const size_t k = self.find_last_of("/\\");
      if (k == std::string::npos) break;
      self = self.substr(0, k);
    }
    return self + "/fixtures/tags";
  }();
  return d;
}

inline Bytes fileBytes(const std::string& path) {
  std::string s;
  minijson::readFile(path, &s);
  return Bytes(s.begin(), s.end());
}

inline std::string hex16(uint64_t v) {
  char h[20];
  std::snprintf(h, sizeof(h), "%016llx", static_cast<unsigned long long>(v));
  return h;
}

// A field's bytes made readable: U+001F as '|', other bytes past ASCII as
// <XX>.
inline std::string show(const char* s, size_t n) {
  std::string o;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if (c == 0x1F) {
      o += '|';
    } else if (c < 0x20 || c >= 0x7F) {
      char h[8];
      std::snprintf(h, sizeof(h), "<%02X>", c);
      o += h;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o;
}

// ---------------------------------------------------------------------------
// The anchors, read back (2.6.4).
// ---------------------------------------------------------------------------
inline uint32_t be32At(const Bytes& b, size_t at) {
  return (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16) |
         (static_cast<uint32_t>(b[at + 2]) << 8) | b[at + 3];
}

// picCoding 2: the base64 characters from the anchor (Ogg page headers
// stepped over in an Opus file), decoded; the FLAC PICTURE's image data.
inline bool readBase64Picture(const Bytes& file, uint32_t offset, uint32_t length, bool ogg, Bytes* image) {
  std::string chars;
  size_t pos = offset;
  size_t bodyEnd = file.size();
  if (ogg) {
    size_t p = 0;
    bool found = false;
    while (p + 27 <= file.size()) {
      if (std::memcmp(&file[p], "OggS", 4) != 0) return false;
      const size_t segs = file[p + 26];
      if (p + 27 + segs > file.size()) return false;
      size_t body = 0;
      for (size_t i = 0; i < segs; ++i) body += file[p + 27 + i];
      const size_t start = p + 27 + segs;
      if (offset >= start && offset < start + body) {
        bodyEnd = start + body;
        found = true;
        break;
      }
      p = start + body;
    }
    if (!found) return false;
  }
  while (chars.size() < length) {
    if (pos >= file.size()) return false;
    if (ogg && pos == bodyEnd) {
      if (pos + 27 > file.size() || std::memcmp(&file[pos], "OggS", 4) != 0) return false;
      const size_t segs = file[pos + 26];
      size_t body = 0;
      for (size_t i = 0; i < segs && pos + 27 + i < file.size(); ++i) body += file[pos + 27 + i];
      pos += 27 + segs;
      bodyEnd = pos + body;
      continue;
    }
    chars.push_back(static_cast<char>(file[pos++]));
  }
  Bytes block;
  uint32_t acc = 0;
  int bits = 0;
  for (char ch : chars) {
    int v = -1;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else if (ch == '=') break;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      block.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  if (block.size() < 32) return false;
  size_t at = 4;
  const uint32_t ml = be32At(block, at);
  at += 4 + ml;
  if (at + 4 > block.size()) return false;
  const uint32_t dl = be32At(block, at);
  // lofty skips the description only when it fits before the rest.
  const size_t rest = block.size() - at - 4;
  at += 4 + ((dl > 0 && dl < rest) ? dl : 0) + 16;
  if (at + 4 > block.size()) return false;
  const uint32_t len = be32At(block, at);
  at += 4;
  if (at + len > block.size()) return false;
  image->assign(block.begin() + static_cast<long>(at), block.begin() + static_cast<long>(at + len));
  return true;
}

// The image bytes a record's anchor names.
inline bool readPicture(const Bytes& file, const cardcontract::mptg::Record& r, bool ogg, Bytes* image) {
  namespace mptg = cardcontract::mptg;
  image->clear();
  if (static_cast<uint64_t>(r.picOffset) > file.size()) return false;
  switch (r.picCoding) {
    case mptg::kCodingRaw:
    case mptg::kCodingApe:
      if (static_cast<uint64_t>(r.picOffset) + r.picLength > file.size()) return false;
      image->assign(file.begin() + r.picOffset, file.begin() + r.picOffset + r.picLength);
      return true;
    case mptg::kCodingUnsync: {
      if (static_cast<uint64_t>(r.picOffset) + r.picLength > file.size()) return false;
      bool ff = false;
      for (uint32_t i = 0; i < r.picLength; ++i) {
        const uint8_t c = file[r.picOffset + i];
        if (ff && c == 0) {
          ff = false;
          continue;
        }
        image->push_back(c);
        ff = c == 0xFF;
      }
      return true;
    }
    case mptg::kCodingOggBase64: return readBase64Picture(file, r.picOffset, r.picLength, ogg, image);
    default: return false;
  }
}

// ---------------------------------------------------------------------------
// A record against the reference reader's: "" when they agree. Every field,
// every number, the flags and `known`; the length within 100 ms (2.17: the
// device trims the MP3 encoder delay, lofty doesn't); the picture's type
// and MIME, its anchor where the corpus knows it, and always the image
// bytes the anchor reads back to (picFnv).
// ---------------------------------------------------------------------------
inline std::string compare(const tagscan::Record& r, const minijson::Value& e, const Bytes& file, bool ogg) {
  namespace cc = cardcontract;
  static const char* kNames[cc::kRunFields] = {"title", "artist", "album", "albumArtist", "genre", "composer",
                                               "titleSort", "artistSort", "albumSort", "albumArtistSort",
                                               "mbAlbumId", "mbRecordingId"};
  std::string d;
  char line[200];
  for (uint32_t f = 0; f < cc::kRunFields; ++f) {
    const std::string& want = e["fields"][static_cast<size_t>(f)].str();
    const std::string got(r.field(f), r.fieldLength(f));
    if (want != got)
      d += std::string(" ") + kNames[f] + "='" + show(got.data(), got.size()) + "' want '" +
           show(want.data(), want.size()) + "'";
  }
  const cc::mptg::Record& x = r.rec;
  const bool anchorKnown = e["picLength"].i64() != 0;
  struct Num {
    const char* name;
    int64_t got;
    bool check;
  } nums[] = {
      {"container", x.container, true},       {"flags", x.flags, true},
      {"known", x.known, true},               {"year", x.year, true},
      {"track", x.track, true},               {"trackTotal", x.trackTotal, true},
      {"disc", x.disc, true},                 {"discTotal", x.discTotal, true},
      {"bpm10", x.bpm10, true},               {"camelot", x.camelot, true},
      {"rgTrackGain", x.rgTrackGain, true},   {"rgAlbumGain", x.rgAlbumGain, true},
      {"rgTrackPeak", x.rgTrackPeak, true},   {"rgAlbumPeak", x.rgAlbumPeak, true},
      {"picType", x.picType, true},           {"picMime", x.picMime, true},
      {"picOffset", x.picOffset, anchorKnown}, {"picLength", x.picLength, anchorKnown},
      {"picCoding", x.picCoding, anchorKnown},
  };
  for (const Num& n : nums) {
    if (!n.check) continue;
    const int64_t want = e[n.name].i64();
    if (want != n.got) {
      std::snprintf(line, sizeof(line), " %s=%lld want %lld", n.name, static_cast<long long>(n.got),
                    static_cast<long long>(want));
      d += line;
    }
  }
  const std::string& picFnv = e["picFnv"].str();
  if (picFnv.empty()) {
    if (x.picLength) d += " a picture where the reference has none";
  } else {
    Bytes image;
    if (!x.picLength || !readPicture(file, x, ogg, &image)) {
      d += " no picture to read where the reference elects one";
    } else if (hex16(cc::fnv1a64(image.data(), image.size())) != picFnv) {
      d += " the anchor reads other bytes than the reference's picture";
    }
  }
  const int64_t dur = e["durationMs"].i64();
  if (static_cast<int64_t>(x.durationMs) + 100 < dur || static_cast<int64_t>(x.durationMs) > dur + 100) {
    std::snprintf(line, sizeof(line), " durationMs=%u want %lld (+-100)", x.durationMs, static_cast<long long>(dur));
    d += line;
  }
  return d;
}

}  // namespace tagfixtures
