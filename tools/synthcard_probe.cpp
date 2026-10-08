// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
//
// synthcard_probe: tools/synthcard.py's stubs read by the repo's own parsers
// on the host (tools/test_synthcard.py builds it with g++ when ffprobe isn't
// installed). One file path per line on stdin; one JSON line per file on
// stdout:
//   MP3:  the ID3v2 tag's size (progress::id3v2Size), the first frame and
//         the Xing/Info/LAME header (lametag::parse), the length the
//         firmware gives it (progress::mp3HeaderDurationMs), and the frames
//         counted header by header to the tags at the end (every one a
//         valid Layer III header: progress::parseMp3Frame);
//   FLAC: the STREAMINFO length (progress::flacDurationMs);
//   Opus: oggopus::Reader's open with the tail scan (the firmware's open):
//         its result, the head, the tags' size, the exact length.
// Built with -DSYNTHCARD_TAGSCAN (lib/core/TagScan, docs/METADATA.md 6.1
// N6) it adds the tag record N6 reads: the fields, the numbers, the
// elected picture's anchor.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "LameTag.h"
#include "OggOpus.h"
#include "TrackProgress.h"
#include "TrackSeek.h"
#ifdef SYNTHCARD_TAGSCAN
#include "TagScan.h"
#endif

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::vector<uint8_t> d;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return d;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  return d;
}

std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o;
}

bool endsWith(const std::string& s, const char* e) {
  const size_t n = std::strlen(e);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != e[i]) return false;
  }
  return true;
}

struct MemReader : trackseek::FileReader {
  const std::vector<uint8_t>& d;
  explicit MemReader(const std::vector<uint8_t>& data) : d(data) {}
  uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n) override {
    if (offset >= d.size()) return 0;
    const uint32_t k = static_cast<uint32_t>(std::min<size_t>(n, d.size() - offset));
    std::memcpy(buf, d.data() + offset, k);
    return k;
  }
};

std::string mp3(const std::vector<uint8_t>& d) {
  char b[512];
  const uint32_t tag = progress::id3v2Size(d.data(), d.size());
  // The tags at the end: ID3v1 (128 B) and an APEv2 tag before it.
  size_t end = d.size();
  bool v1 = false, ape = false;
  if (end >= 128 && std::memcmp(d.data() + end - 128, "TAG", 3) == 0) {
    v1 = true;
    end -= 128;
  }
  if (end >= 32 && std::memcmp(d.data() + end - 32, "APETAGEX", 8) == 0) {
    ape = true;
    const uint32_t size = d[end - 20] | (d[end - 19] << 8) | (d[end - 18] << 16) | (static_cast<uint32_t>(d[end - 17]) << 24);
    const bool header = (d[end - 9] & 0x80) != 0;
    end -= size + (header ? 32 : 0);
  }
  lametag::Info li;
  const bool lameOk = tag < d.size() && lametag::parse(d.data() + tag, d.size() - tag, &li);
  const uint32_t ms = tag < d.size() ? progress::mp3HeaderDurationMs(d.data() + tag, d.size() - tag, d.size(), tag) : 0;
  // Walk the frames from the first one to `end`.
  uint32_t frames = 0, bad = 0;
  size_t at = tag;
  bool chained = true;
  while (at + 4 <= end) {
    progress::Mp3Frame f;
    if (!progress::parseMp3Frame(d.data() + at, &f) || f.length <= 0) {
      bad = 1;
      chained = false;
      break;
    }
    ++frames;
    at += static_cast<size_t>(f.length);
  }
  if (at != end) chained = false;
  std::snprintf(b, sizeof(b),
                "\"id3v2\":%u,\"v1\":%s,\"ape\":%s,\"lame\":{\"ok\":%s,\"header\":%s,\"xing\":%s,\"info\":%s,"
                "\"frames\":%u,\"lame\":%s},\"durationMs\":%u,\"framesWalked\":%u,\"chained\":%s,\"bad\":%u",
                tag, v1 ? "true" : "false", ape ? "true" : "false", lameOk ? "true" : "false",
                li.header ? "true" : "false", li.xing ? "true" : "false", li.info ? "true" : "false", li.frames,
                li.lame ? "true" : "false", ms, frames, chained ? "true" : "false", bad);
  return b;
}

std::string flac(const std::vector<uint8_t>& d) {
  char b[128];
  std::snprintf(b, sizeof(b), "\"durationMs\":%u", progress::flacDurationMs(d.data(), d.size()));
  return b;
}

std::string opus(const std::vector<uint8_t>& d) {
  static std::vector<uint8_t> page(oggopus::kPageBytes), packet(oggopus::kMaxPacketBytes);
  MemReader r(d);
  oggopus::Reader rd(r, static_cast<uint32_t>(d.size()), page.data(), packet.data());
  const oggopus::Reader::Open o = rd.open(true);
  char b[256];
  std::snprintf(b, sizeof(b), "\"open\":\"%s\",\"channels\":%u,\"preSkip\":%u,\"tagsBytes\":%u,\"durationMs\":%u",
                oggopus::Reader::openName(o), rd.head().channels, rd.head().preSkip, rd.tagsBytes(),
                o == oggopus::Reader::Open::Ok ? rd.lengthMs() : 0);
  return b;
}

#ifdef SYNTHCARD_TAGSCAN
std::string tags(const std::vector<uint8_t>& d, tagscan::Kind kind) {
  static tagscan::Scanner* sc = new tagscan::Scanner();
  cardcontract::MemSource src(d.data(), static_cast<uint32_t>(d.size()));
  static uint8_t buf[4096];
  const tagscan::Result r = sc->scan(src, kind, buf, sizeof(buf));
  const tagscan::Record& rec = sc->record();
  static const char* const kNames[] = {"title",     "artist",          "album",      "albumArtist",
                                       "genre",     "composer",        "titleSort",  "artistSort",
                                       "albumSort", "albumArtistSort", "mbAlbumId",  "mbRecordingId"};
  std::string o = "\"tagscan\":{\"result\":\"";
  o += r == tagscan::Result::Ok ? "ok" : r == tagscan::Result::Unreadable ? "unreadable" : "readerror";
  o += "\"";
  for (uint32_t f = 0; f < cardcontract::kRunFields; ++f) {
    const char* v = rec.field(f);
    if (v && v[0]) o += ",\"" + std::string(kNames[f]) + "\":\"" + esc(v) + "\"";
  }
  char b[512];
  const cardcontract::mptg::Record& m = rec.rec;
  std::snprintf(b, sizeof(b),
                ",\"year\":%u,\"track\":%u,\"trackTotal\":%u,\"disc\":%u,\"discTotal\":%u,\"durationMs\":%u,"
                "\"flags\":%u,\"compilation\":%u,\"picOffset\":%u,\"picLength\":%u,\"picMime\":%u,\"picCoding\":%u,"
                "\"hasPicture\":%s,\"issues\":%u}",
                m.year, m.track, m.trackTotal, m.disc, m.discTotal, m.durationMs, m.flags,
                cardcontract::mptg::compilationOf(m), m.picOffset, m.picLength, m.picMime, m.picCoding,
                cardcontract::mptg::hasPicture(m) ? "true" : "false", sc->issues());
  o += b;
  return o;
}
#endif

}  // namespace

int main() {
  char line[4096];
  while (std::fgets(line, sizeof(line), stdin)) {
    std::string path(line);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (path.empty()) continue;
    const std::vector<uint8_t> d = readFile(path);
    std::string out = "{\"path\":\"" + esc(path) + "\",\"bytes\":" + std::to_string(d.size());
    if (d.empty()) {
      out += ",\"error\":\"unreadable\"}";
      std::printf("%s\n", out.c_str());
      continue;
    }
    if (endsWith(path, ".mp3")) {
      out += ",\"kind\":\"mp3\"," + mp3(d);
#ifdef SYNTHCARD_TAGSCAN
      out += "," + tags(d, tagscan::Kind::Mp3);
#endif
    } else if (endsWith(path, ".flac")) {
      out += ",\"kind\":\"flac\"," + flac(d);
#ifdef SYNTHCARD_TAGSCAN
      out += "," + tags(d, tagscan::Kind::Flac);
#endif
    } else if (endsWith(path, ".opus")) {
      out += ",\"kind\":\"opus\"," + opus(d);
#ifdef SYNTHCARD_TAGSCAN
      out += "," + tags(d, tagscan::Kind::Opus);
#endif
    }
    out += "}";
    std::printf("%s\n", out.c_str());
  }
  return 0;
}
