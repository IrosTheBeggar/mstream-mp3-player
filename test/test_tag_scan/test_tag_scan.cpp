// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the device's tag reader (lib/core/TagScan, TagRules;
// docs/METADATA.md part 5, 2.17 item 3): part 5's small rules one by one;
// the synthetic parity corpus (test/fixtures/tags, made by
// tools/tag_corpus.py) read into records that match the reference reader's
// (expected.json, made by tools/tagref on lofty 0.25) field for field, the
// length within 100 ms; the same records from every buffer size; the
// pictures' anchors read back to the image bytes by an independent reader
// of each picCoding; the records into an MPTG file and back; the read
// counts; the budget (generated files past the corpus's sizes: a cover of
// megabytes behind tag-level unsynchronisation, an Opus picture of 190
// pages; a stop is Partial, never UNREADABLE or a silent Ok); an MP3's
// length the same whatever the buffer; truncation at every byte, a failing
// source, a mutation fuzz pass over the corpus and a pass over generated
// files, with the record's invariants checked each time.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../support/MiniJson.h"
#include "../support/TagFixtures.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "TagRules.h"
#include "TagScan.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
using Bytes = std::vector<uint8_t>;
using tagscan::Kind;
using tagscan::Result;

namespace {

// ---------------------------------------------------------------------------
// The corpus
// ---------------------------------------------------------------------------
Bytes fileBytes(const std::string& name) { return tagfixtures::fileBytes(tagfixtures::dir() + "/" + name); }

const minijson::Value& expected() {
  static minijson::Value v;
  static bool loaded = false;
  if (!loaded) {
    std::string text;
    TEST_ASSERT_TRUE_MESSAGE(minijson::readFile(tagfixtures::dir() + "/expected.json", &text), "expected.json");
    TEST_ASSERT_TRUE_MESSAGE(minijson::parse(text, &v), "expected.json parses");
    loaded = true;
  }
  return v;
}

const minijson::Value& anchors() {
  static minijson::Value v;
  static bool loaded = false;
  if (!loaded) {
    std::string text;
    TEST_ASSERT_TRUE_MESSAGE(minijson::readFile(tagfixtures::dir() + "/anchors.json", &text), "anchors.json");
    TEST_ASSERT_TRUE_MESSAGE(minijson::parse(text, &v), "anchors.json parses");
    loaded = true;
  }
  return v;
}

// A source over memory that counts its reads and can be made to fail.
class TestSource : public cc::Source {
public:
  explicit TestSource(const Bytes& b) : b_(b) {}
  uint32_t size() const override { return static_cast<uint32_t>(b_.size()); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    ++reads;
    if (failAfter >= 0 && reads > failAfter) return false;
    if (offset > b_.size() || n > b_.size() - offset) return false;
    if (n) std::memcpy(out, b_.data() + offset, n);
    return true;
  }
  int reads = 0;
  int failAfter = -1;

private:
  const Bytes& b_;
};

struct Scanned {
  std::unique_ptr<tagscan::Scanner> s{new tagscan::Scanner()};
  Result result = Result::Unreadable;
  const tagscan::Record& rec() const { return s->record(); }
};

Kind kindOfName(const std::string& name) { return tagscan::kindOf(name.c_str()); }

void scanInto(Scanned& sc, const Bytes& b, Kind kind, uint32_t bufBytes = 4096,
              const tagscan::Limits& lim = tagscan::Limits()) {
  std::vector<uint8_t> buf(bufBytes);
  TestSource src(b);
  sc.result = sc.s->scan(src, kind, buf.data(), bufBytes, lim);
}

const char* kFieldNames[cc::kRunFields] = {"title", "artist", "album", "albumArtist", "genre", "composer",
                                           "titleSort", "artistSort", "albumSort", "albumArtistSort",
                                           "mbAlbumId", "mbRecordingId"};

bool sameRecord(const tagscan::Record& a, const tagscan::Record& b) {
  for (uint32_t f = 0; f < cc::kRunFields; ++f)
    if (a.fieldLength(f) != b.fieldLength(f) || std::memcmp(a.field(f), b.field(f), a.fieldLength(f)) != 0)
      return false;
  uint8_t ra[mptg::kRecsStride], rb[mptg::kRecsStride];
  mptg::encodeRecord(a.rec, ra);
  mptg::encodeRecord(b.rec, rb);
  return std::memcmp(ra, rb, sizeof(ra)) == 0;
}

std::vector<std::string> corpusNames() {
  std::vector<std::string> names;
  for (const auto& m : anchors().members) names.push_back(m.first);
  return names;
}

// ---------------------------------------------------------------------------
// The record's invariants (2.3.6, 2.6.4), whatever the file held.
// ---------------------------------------------------------------------------
bool validUtf8(const char* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    size_t need;
    uint32_t cp;
    if (b < 0x80) {
      ++i;
      continue;
    }
    if (b >= 0xC2 && b <= 0xDF) need = 1, cp = b & 0x1F;
    else if (b >= 0xE0 && b <= 0xEF) need = 2, cp = b & 0x0F;
    else if (b >= 0xF0 && b <= 0xF4) need = 3, cp = b & 0x07;
    else return false;
    if (i + need >= n) return false;
    for (size_t k = 1; k <= need; ++k) {
      const uint8_t c = static_cast<uint8_t>(s[i + k]);
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += need + 1;
  }
  return true;
}

std::string invariants(const tagscan::Scanner& s, Result result, uint32_t fileSize, const tagscan::Limits& lim) {
  const tagscan::Record& r = s.record();
  const mptg::Record& x = r.rec;
  const tagscan::Stats& st = s.stats();
  // The walk's budget, the reserve after it, an unsynchronised tag's
  // extension; an Ogg page header stepped over counts its bytes only.
  if (st.pages > st.reads || st.reads - st.pages > uint32_t(lim.maxReads) + lim.reserveReads + st.extraReads)
    return "reads past the limit";
  if (uint64_t(st.bytes) > uint64_t(lim.readBudget) + lim.reserveBytes + st.extraBytes) return "bytes past the budget";
  // A budget stop is Partial, and only a budget stop (a read error after it
  // is a ReadError).
  const bool budget = (s.issues() & tagscan::kIssueBudget) != 0;
  if (result != Result::ReadError && budget != (result == Result::Partial)) return "a budget stop isn't Partial";
  if (result != Result::Ok && result != Result::Partial) {
    if (!(x.flags & mptg::kUnreadable) || x.known != 0 || x.durationMs != 0) return "an unreadable record isn't empty";
    for (uint32_t f = 0; f < cc::kRunFields; ++f)
      if (r.fieldLength(f)) return "an unreadable record has a field";
    return "";
  }
  if (x.known != mptg::kKnownRules1) return "known isn't readRules 1's";
  if (x.flags & mptg::kUnreadable) return "an Ok record is UNREADABLE";
  for (uint32_t f = 0; f < cc::kRunFields; ++f) {
    const char* p = r.field(f);
    const size_t n = r.fieldLength(f);
    if (std::strlen(p) != n) return std::string(kFieldNames[f]) + " has a NUL";
    if (!validUtf8(p, n)) return std::string(kFieldNames[f]) + " isn't UTF-8";
    const bool list = cc::isListField(f);
    if (n > cc::fieldMax(f)) return std::string(kFieldNames[f]) + " is too long";
    uint32_t values = n ? 1 : 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
      if (i < n) {
        const uint8_t c = static_cast<uint8_t>(p[i]);
        if (c == 0x1F && list) {
          ++values;
        } else if (c < 0x20 || c == 0x7F) {
          return std::string(kFieldNames[f]) + " keeps a control character";
        }
      }
      if (i == n || (list && p[i] == 0x1F)) {
        if (i - start == 0 && n) return std::string(kFieldNames[f]) + " has an empty value";
        if (i - start > cc::FieldBuilder::kValueMax) return std::string(kFieldNames[f]) + " has a value past 255";
        start = i + 1;
      }
    }
    if (values > cc::FieldBuilder::kListValues) return std::string(kFieldNames[f]) + " has more than 16 values";
  }
  if (x.picLength) {
    if (x.picCoding > 3 || x.picMime < 1 || x.picMime > 3) return "a picture's enums";
    if (x.picCoding != mptg::kCodingOggBase64 && static_cast<uint64_t>(x.picOffset) + x.picLength > fileSize)
      return "a picture past the file";
    if (x.picOffset >= fileSize) return "a picture past the file";
  } else if (x.picOffset || x.picType || x.picMime || x.picCoding) {
    return "a picture's fields without a picture";
  }
  if ((x.flags & mptg::kCompilationMask) == 3) return "compilation 3";
  if ((x.flags & mptg::kRgFromR128) && !(x.flags & (mptg::kHasRgTrack | mptg::kHasRgAlbum))) return "R128 with no gain";
  if (x.camelot > 24) return "camelot past 24";
  if (x.bpm10 && (x.bpm10 < 200 || x.bpm10 > 3000 || x.bpm10 % 10)) return "bpm10 out of its rule";
  if (r.truncated() != ((x.flags & mptg::kTruncated) != 0)) return "TRUNCATED disagrees";
  return "";
}

// ---------------------------------------------------------------------------
// Generated files: past what the corpus's small files reach (a picture of
// megabytes behind tag-level unsynchronisation, an Opus picture of more
// pages than the read budget's count). Their records were checked against
// the reference reader (tools/tagref) when this was written: "Song" by
// "Band", the front cover elected, 521 ms (MP3) or 10 s (Opus).
// ---------------------------------------------------------------------------
namespace gen {

void put(Bytes& b, const void* p, size_t n) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  b.insert(b.end(), q, q + n);
}
void str(Bytes& b, const char* s) { put(b, s, std::strlen(s)); }
void be32(Bytes& b, uint32_t v) {
  const uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  put(b, x, 4);
}
void le32(Bytes& b, uint32_t v) {
  const uint8_t x[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  put(b, x, 4);
}
void syncsafe(Bytes& b, uint32_t v) {
  const uint8_t x[4] = {uint8_t((v >> 21) & 0x7F), uint8_t((v >> 14) & 0x7F), uint8_t((v >> 7) & 0x7F),
                        uint8_t(v & 0x7F)};
  put(b, x, 4);
}

// Bytes that start like a JPEG, then noise (FF bytes included: what
// unsynchronisation stuffs).
Bytes jpeg(size_t n, uint32_t seed) {
  Bytes d = {0xFF, 0xD8, 0xFF, 0xE0};
  while (d.size() < n) {
    seed = seed * 1103515245u + 12345u;
    d.push_back(static_cast<uint8_t>(seed >> 16));
  }
  return d;
}

// ID3 unsynchronisation: a 00 after an FF before 00, E0 and up, or the end.
Bytes unsync(const Bytes& in) {
  Bytes out;
  for (size_t i = 0; i < in.size(); ++i) {
    out.push_back(in[i]);
    if (in[i] == 0xFF && (i + 1 == in.size() || in[i + 1] >= 0xE0 || in[i + 1] == 0)) out.push_back(0);
  }
  return out;
}

// A v2.2 (3-byte ids and sizes) or v2.3 frame.
Bytes frame(int ver, const char* id, const Bytes& body) {
  Bytes f;
  str(f, id);
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (ver == 2) {
    const uint8_t s[3] = {uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
    put(f, s, 3);
  } else {
    be32(f, n);
    f.push_back(0);
    f.push_back(0);
  }
  put(f, body.data(), body.size());
  return f;
}
Bytes latin1(const char* s) {
  Bytes b = {0};
  str(b, s);
  return b;
}
Bytes cover(int ver, const Bytes& image) {
  Bytes b = {0};
  if (ver == 2) {
    str(b, "JPG");
  } else {
    str(b, "image/jpeg");
    b.push_back(0);
  }
  b.push_back(3);  // the front cover
  b.push_back(0);  // no description
  put(b, image.data(), image.size());
  return b;
}
// 20 MPEG-1 Layer III frames, 128 kbps at 44.1 kHz: 521 ms by the bitrate.
Bytes audio() {
  Bytes a;
  for (int i = 0; i < 20; ++i) {
    const uint8_t h[4] = {0xFF, 0xFB, 0x90, 0x00};
    put(a, h, 4);
    a.insert(a.end(), 413, 0x55);
  }
  return a;
}
// "Song", the cover, "Band" in a v2.2 or v2.3 tag under tag-level
// unsynchronisation, then the audio.
Bytes unsyncMp3(int ver, const Bytes& image) {
  const bool v2 = ver == 2;
  Bytes body = frame(ver, v2 ? "TT2" : "TIT2", latin1("Song"));
  const Bytes pic = frame(ver, v2 ? "PIC" : "APIC", cover(ver, image));
  const Bytes artist = frame(ver, v2 ? "TP1" : "TPE1", latin1("Band"));
  put(body, pic.data(), pic.size());
  put(body, artist.data(), artist.size());
  Bytes stored = unsync(body);
  stored.insert(stored.end(), 256, 0);  // padding
  Bytes f;
  str(f, "ID3");
  f.push_back(static_cast<uint8_t>(ver));
  f.push_back(0);
  f.push_back(0x80);  // the whole tag unsynchronised
  syncsafe(f, static_cast<uint32_t>(stored.size()));
  put(f, stored.data(), stored.size());
  const Bytes a = audio();
  put(f, a.data(), a.size());
  return f;
}

// Ogg: one packet over pages of at most 255 segments (64 KB, as libopusenc
// and FFmpeg write a comment header).
void oggPacket(Bytes& file, const Bytes& pkt, uint32_t& seq, uint8_t firstFlags, uint64_t granule,
               uint32_t maxSegs = 255) {
  size_t off = 0;
  bool first = true;
  for (;;) {
    Bytes lacing;
    size_t take = 0;
    bool ends = false;
    while (lacing.size() < maxSegs) {
      const size_t left = pkt.size() - off - take;
      if (left >= 255) {
        lacing.push_back(255);
        take += 255;
      } else {
        lacing.push_back(static_cast<uint8_t>(left));
        take += left;
        ends = true;
        break;
      }
    }
    str(file, "OggS");
    file.push_back(0);
    file.push_back(first ? firstFlags : 1);
    const uint64_t g = ends ? granule : ~0ull;
    for (int k = 0; k < 8; ++k) file.push_back(static_cast<uint8_t>(g >> (8 * k)));
    le32(file, 0x1234);
    le32(file, seq++);
    le32(file, 0);  // the CRC (not checked)
    file.push_back(static_cast<uint8_t>(lacing.size()));
    put(file, lacing.data(), lacing.size());
    put(file, pkt.data() + off, take);
    off += take;
    first = false;
    if (ends) break;
  }
}
std::string base64(const Bytes& in) {
  static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
    o += a[v >> 18];
    o += a[(v >> 12) & 63];
    o += a[(v >> 6) & 63];
    o += a[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(in[i]) << 16;
    o += a[v >> 18];
    o += a[(v >> 12) & 63];
    o += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
    o += a[v >> 18];
    o += a[(v >> 12) & 63];
    o += a[(v >> 6) & 63];
    o += "=";
  }
  return o;
}
// METADATA_BLOCK_PICTURE's value: a FLAC PICTURE (a front JPEG), in base64.
std::string pictureComment(const Bytes& image) {
  Bytes b;
  be32(b, 3);
  be32(b, 10);
  str(b, "image/jpeg");
  be32(b, 0);
  for (int k = 0; k < 4; ++k) be32(b, k == 2 ? 24 : (k == 3 ? 0 : 500));
  be32(b, static_cast<uint32_t>(image.size()));
  put(b, image.data(), image.size());
  return "METADATA_BLOCK_PICTURE=" + base64(b);
}
// An Opus file of 10 s: OpusHead, OpusTags with these comments (on pages of
// `segs` segments), one audio packet.
Bytes opus(const std::vector<std::string>& comments, uint32_t segs = 255) {
  Bytes f;
  uint32_t seq = 0;
  Bytes head;
  str(head, "OpusHead");
  const uint8_t h[11] = {1, 2, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0};  // 2 ch, pre-skip 312, 48 kHz
  put(head, h, sizeof(h));
  oggPacket(f, head, seq, 2, 0);
  Bytes tags;
  str(tags, "OpusTags");
  le32(tags, 3);
  str(tags, "gen");
  le32(tags, static_cast<uint32_t>(comments.size()));
  for (const std::string& kv : comments) {
    le32(tags, static_cast<uint32_t>(kv.size()));
    put(tags, kv.data(), kv.size());
  }
  oggPacket(f, tags, seq, 0, 0, segs);
  Bytes pkt(100, 0x11);
  pkt[0] = 0xFC;
  oggPacket(f, pkt, seq, 4, 48000ull * 10 + 312);
  return f;
}


// ---- Random files, for the generated fuzz pass ----
// v2.4's frame: a syncsafe size and its flags.
Bytes frame4(const char* id, const Bytes& body) {
  Bytes f;
  str(f, id);
  syncsafe(f, static_cast<uint32_t>(body.size()));
  f.push_back(0);
  f.push_back(0);
  put(f, body.data(), body.size());
  return f;
}
Bytes value(std::mt19937& rng) {
  static const char* const kPool[] = {"", "Song", "Band", "Folk", "Same", "1999", "2004-05-01", "3/12", " 7 ", "x",
                                      "120.5", "-6.5 dB", "8A", "1", "(17)Rock"};
  Bytes v;
  if (rng() % 12 == 0) {
    v.assign(200 + rng() % 200, static_cast<uint8_t>('a' + rng() % 26));  // past a list's 255 bytes
  } else {
    str(v, kPool[rng() % (sizeof(kPool) / sizeof(kPool[0]))]);
    if (rng() % 3 == 0) str(v, std::to_string(rng() % 40).c_str());
  }
  return v;
}
// An MP3 with an ID3v2.2, 2.3 or 2.4 tag of up to 150 frames: text frames a
// field comes from and others, TXXX, pictures up to 64 KB, private frames;
// v2.2/2.3 sometimes under tag-level unsynchronisation; sometimes an ID3v1.
Bytes randomMp3(std::mt19937& rng) {
  const int ver = 2 + static_cast<int>(rng() % 3);
  static const char* const kIds[] = {"TIT2", "TPE1", "TALB", "TPE2", "TCON", "TYER", "TDRC", "TRCK",
                                     "TXXX", "TXXX", "TZ01", "TZ02", "TSSE", "APIC", "APIC", "PRIV"};
  static const char* const kIds2[] = {"TT2", "TP1", "TAL", "TP2", "TCO", "TYE", "TYE", "TRK",
                                      "TXX", "TXX", "TZ1", "TZ2", "TSS", "PIC", "PIC", "XYZ"};
  static const char* const kDesc[] = {"REPLAYGAIN_TRACK_GAIN", "ALBUMARTIST", "MusicBrainz Album Id", "CUSTOM_1",
                                      "CUSTOM_2", ""};
  Bytes body;
  const uint32_t frames = rng() % 150;
  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t k = rng() % 16;
    const char* id = ver == 2 ? kIds2[k] : kIds[k];
    Bytes content;
    if (k == 13 || k == 14) {
      content = cover(ver, jpeg(16 + rng() % 65536, rng()));
      if (rng() % 2) content[ver == 2 ? 4 : 12] = 0;  // the type: other, not the front cover
    } else if (k == 15) {
      content.assign(rng() % 20000, static_cast<uint8_t>(rng()));
    } else if (k == 8 || k == 9) {
      content = {0};
      str(content, kDesc[rng() % (sizeof(kDesc) / sizeof(kDesc[0]))]);
      content.push_back(0);
      const Bytes v = value(rng);
      put(content, v.data(), v.size());
    } else {
      content = {0};
      const Bytes v = value(rng);
      put(content, v.data(), v.size());
    }
    const Bytes fr = ver == 4 ? frame4(id, content) : frame(ver, id, content);
    put(body, fr.data(), fr.size());
  }
  const bool whole = ver < 4 && rng() % 3 == 0;
  Bytes stored = whole ? unsync(body) : body;
  stored.insert(stored.end(), rng() % 300, 0);
  Bytes f;
  str(f, "ID3");
  f.push_back(static_cast<uint8_t>(ver));
  f.push_back(0);
  f.push_back(whole ? 0x80 : 0);
  syncsafe(f, static_cast<uint32_t>(stored.size()));
  put(f, stored.data(), stored.size());
  const Bytes a = audio();
  put(f, a.data(), a.size());
  if (rng() % 4 == 0) {
    Bytes v1(128, 0);
    std::memcpy(v1.data(), "TAGOld Title", 12);
    v1[127] = 17;
    put(f, v1.data(), v1.size());
  }
  return f;
}
// Comments for FLAC or Opus: up to 300, keys a field comes from and others,
// values empty, repeated, long, numbers.
std::vector<std::string> comments(std::mt19937& rng) {
  static const char* const kKeys[] = {"TITLE", "ARTIST", "ALBUM", "GENRE", "COMPOSER", "BPM", "DATE", "YEAR",
                                      "TRACKNUMBER", "DISCNUMBER", "TRACKTOTAL", "TOTALTRACKS", "ALBUMARTIST",
                                      "MUSICBRAINZ_ALBUMID", "COMMENT", "KEY", "ARTIST", "GENRE"};
  std::vector<std::string> c;
  const uint32_t n = rng() % 300;
  for (uint32_t i = 0; i < n; ++i) {
    const Bytes v = value(rng);
    c.push_back(std::string(kKeys[rng() % (sizeof(kKeys) / sizeof(kKeys[0]))]) + "=" + std::string(v.begin(), v.end()));
  }
  return c;
}
Bytes randomFlac(std::mt19937& rng) {
  Bytes f;
  str(f, "fLaC");
  Bytes si(34, 0);
  si[10] = 0x0A;  // 44.1 kHz, 2 channels, 16 bits, 30 s
  si[11] = 0xC4;
  si[12] = 0x42;
  si[13] = 0xF0;
  si[14] = 0x00;
  si[15] = 0x14;
  si[16] = 0x2F;
  si[17] = 0xD8;
  f.push_back(0);
  f.push_back(0);
  f.push_back(0);
  f.push_back(34);
  put(f, si.data(), si.size());
  Bytes c;
  le32(c, 3);
  str(c, "gen");
  const std::vector<std::string> items = comments(rng);
  le32(c, static_cast<uint32_t>(items.size()));
  for (const std::string& kv : items) {
    le32(c, static_cast<uint32_t>(kv.size()));
    put(c, kv.data(), kv.size());
  }
  f.push_back(0x84);
  f.push_back(static_cast<uint8_t>(c.size() >> 16));
  f.push_back(static_cast<uint8_t>(c.size() >> 8));
  f.push_back(static_cast<uint8_t>(c.size()));
  put(f, c.data(), c.size());
  f.insert(f.end(), 64, 0);
  return f;
}
Bytes randomOpus(std::mt19937& rng) {
  std::vector<std::string> c = comments(rng);
  if (rng() % 3 == 0) c.insert(c.begin() + (c.empty() ? 0 : rng() % c.size()), pictureComment(jpeg(16 + rng() % 300000, rng())));
  return opus(c, 16 + rng() % 240);
}

}  // namespace gen

// The record's picture reads back to `image` (2.6.4's anchor).
bool pictureIs(const Bytes& file, const tagscan::Record& r, bool ogg, const Bytes& image) {
  Bytes got;
  return r.rec.picLength && tagfixtures::readPicture(file, r.rec, ogg, &got) && got == image;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ===========================================================================
// Part 5's rules (TagRules)
// ===========================================================================
void test_rust_u32() {
  uint32_t v = 0;
  TEST_ASSERT_TRUE(tagrules::parseU32("3", 1, &v));
  TEST_ASSERT_EQUAL_UINT32(3, v);
  TEST_ASSERT_TRUE(tagrules::parseU32("+3", 2, &v));
  TEST_ASSERT_EQUAL_UINT32(3, v);
  TEST_ASSERT_TRUE(tagrules::parseU32("007", 3, &v));
  TEST_ASSERT_EQUAL_UINT32(7, v);
  TEST_ASSERT_TRUE(tagrules::parseU32("4294967295", 10, &v));
  TEST_ASSERT_EQUAL_UINT32(4294967295u, v);
  TEST_ASSERT_FALSE(tagrules::parseU32("4294967296", 10, &v));
  TEST_ASSERT_FALSE(tagrules::parseU32("", 0, &v));
  TEST_ASSERT_FALSE(tagrules::parseU32("+", 1, &v));
  TEST_ASSERT_FALSE(tagrules::parseU32("-3", 2, &v));
  TEST_ASSERT_FALSE(tagrules::parseU32(" 3", 2, &v));
  TEST_ASSERT_FALSE(tagrules::parseU32("3a", 2, &v));
}

void test_trim_and_blank() {
  // Unicode White_Space (U+3000, U+00A0), not U+FEFF.
  const char* s = "\xE3\x80\x80 x\t\xC2\xA0";
  size_t n = std::strlen(s);
  tagrules::trim(s, n);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_CHAR('x', s[0]);
  const char* bom = "\xEF\xBB\xBFx";
  size_t bn = 4;
  tagrules::trim(bom, bn);
  TEST_ASSERT_EQUAL(4, bn);
  TEST_ASSERT_TRUE(tagrules::isBlank("", 0));
  TEST_ASSERT_TRUE(tagrules::isBlank(" \t\r\n", 4));
  TEST_ASSERT_FALSE(tagrules::isBlank(" a ", 3));
  TEST_ASSERT_FALSE(tagrules::isBlank("\x01", 1));  // a control character isn't White_Space
}

void test_genre_table_and_tcon() {
  TEST_ASSERT_EQUAL_STRING("Blues", tagrules::genreName(0));
  TEST_ASSERT_EQUAL_STRING("Classic rock", tagrules::genreName(1));  // lofty's spelling, not Winamp's
  TEST_ASSERT_EQUAL_STRING("Psybient", tagrules::genreName(191));
  TEST_ASSERT_NULL(tagrules::genreName(192));
  auto genre = [](const char* g) {
    size_t n = 0;
    const char* p = tagrules::parseGenre(g, std::strlen(g), &n);
    return std::string(p, n);
  };
  TEST_ASSERT_EQUAL_STRING("Rock", genre("17").c_str());
  TEST_ASSERT_EQUAL_STRING("Rock", genre("+17").c_str());  // Rust's usize parse takes a '+'
  TEST_ASSERT_EQUAL_STRING("192", genre("192").c_str());
  TEST_ASSERT_EQUAL_STRING("0017", genre("0017").c_str());  // longer than 3: as it is
  TEST_ASSERT_EQUAL_STRING("Remix", genre("RX").c_str());
  TEST_ASSERT_EQUAL_STRING("Cover", genre("CR").c_str());
  TEST_ASSERT_EQUAL_STRING("rx", genre("rx").c_str());
  TEST_ASSERT_EQUAL_STRING("", genre("").c_str());
  auto items = [](const char* s) {
    std::string out;
    size_t pos = 0, at = 0, len = 0;
    while (tagrules::nextParenItem(s, std::strlen(s), &pos, &at, &len)) out += "[" + std::string(s + at, len) + "]";
    return out;
  };
  TEST_ASSERT_EQUAL_STRING("[17][Rock]", items("(17)Rock").c_str());
  TEST_ASSERT_EQUAL_STRING("[4][17]", items("(4)(17)").c_str());
  TEST_ASSERT_EQUAL_STRING("[(I think)][Rock]", items("((I think)Rock").c_str());
  TEST_ASSERT_EQUAL_STRING("[]", items("()").c_str());
  TEST_ASSERT_EQUAL_STRING("[(17]", items("(17").c_str());
  TEST_ASSERT_EQUAL_STRING("[Rock]", items("Rock").c_str());
  TEST_ASSERT_EQUAL_STRING("", items("").c_str());
}

void test_lofty_timestamps() {
  using tagrules::TsParse;
  tagrules::Timestamp t;
  auto parse = [&](const char* s) { return tagrules::parseTimestamp(s, std::strlen(s), &t); };
  TEST_ASSERT_TRUE(parse("2004") == TsParse::Ok);
  TEST_ASSERT_EQUAL(2004, t.year);
  TEST_ASSERT_EQUAL(0, t.fields);
  TEST_ASSERT_TRUE(parse("2004-05-01T12:30:15") == TsParse::Ok);
  TEST_ASSERT_EQUAL(5, t.fields);
  TEST_ASSERT_EQUAL(5, t.month);
  TEST_ASSERT_EQUAL(15, t.second);
  TEST_ASSERT_TRUE(parse("  1999") == TsParse::Ok);
  TEST_ASSERT_EQUAL(1999, t.year);
  TEST_ASSERT_TRUE(parse("1959.") == TsParse::Ok);  // old rips: the year alone
  TEST_ASSERT_EQUAL(1959, t.year);
  TEST_ASSERT_TRUE(parse("20040501") == TsParse::Ok);  // no separators: none needed
  TEST_ASSERT_EQUAL(2, t.fields);
  TEST_ASSERT_EQUAL(1, t.day);
  TEST_ASSERT_TRUE(parse("2004-5-1") == TsParse::Ok);  // a short segment stops before the separator
  TEST_ASSERT_EQUAL(1, t.fields);
  TEST_ASSERT_EQUAL(5, t.month);
  TEST_ASSERT_TRUE(parse("20 4") == TsParse::Ok);  // a space for a digit
  TEST_ASSERT_EQUAL(204, t.year);
  TEST_ASSERT_TRUE(parse("2004/05") == TsParse::Error);  // '/' isn't a separator
  TEST_ASSERT_TRUE(parse("x004") == TsParse::Error);
  TEST_ASSERT_TRUE(parse("199") == TsParse::None);
  TEST_ASSERT_TRUE(parse("") == TsParse::None);
  TEST_ASSERT_TRUE(parse("    ") == TsParse::Error);
  TEST_ASSERT_TRUE(parse("2010-13-01") == TsParse::Ok);
  TEST_ASSERT_FALSE(tagrules::verifyTimestamp(t));
  TEST_ASSERT_TRUE(parse("2010-12-32") == TsParse::Ok);
  TEST_ASSERT_FALSE(tagrules::verifyTimestamp(t));
  TEST_ASSERT_TRUE(parse("2010-00-00") == TsParse::Ok);
  TEST_ASSERT_TRUE(tagrules::verifyTimestamp(t));
}

void test_year_rule() {
  uint16_t y = 0;
  TEST_ASSERT_TRUE(tagrules::yearOf("2004-05-01", 10, &y));
  TEST_ASSERT_EQUAL(2004, y);
  TEST_ASSERT_TRUE(tagrules::yearOf("\xE3\x80\x80 1999x", 9, &y));
  TEST_ASSERT_EQUAL(1999, y);
  TEST_ASSERT_TRUE(tagrules::yearOf("0000", 4, &y));
  TEST_ASSERT_EQUAL(0, y);
  TEST_ASSERT_FALSE(tagrules::yearOf("199", 3, &y));
  TEST_ASSERT_FALSE(tagrules::yearOf("x2004", 5, &y));
  TEST_ASSERT_FALSE(tagrules::yearOf("20a4", 4, &y));
}

void test_number_pairs() {
  uint32_t n = 0, t = 0;
  using tagrules::kHaveNumber;
  using tagrules::kHaveTotal;
  TEST_ASSERT_EQUAL(kHaveNumber | kHaveTotal, tagrules::id3Pair("3/12", 4, &n, &t));
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL(12, t);
  TEST_ASSERT_EQUAL(kHaveNumber | kHaveTotal, tagrules::id3Pair(" 7 / 9 ", 7, &n, &t));
  TEST_ASSERT_EQUAL(7, n);
  TEST_ASSERT_EQUAL(9, t);
  TEST_ASSERT_EQUAL(0, tagrules::id3Pair("5/x", 3, &n, &t));  // both or neither
  TEST_ASSERT_EQUAL(0, tagrules::id3Pair("3/", 2, &n, &t));
  TEST_ASSERT_EQUAL(0, tagrules::id3Pair("A1", 2, &n, &t));
  TEST_ASSERT_EQUAL(kHaveTotal, tagrules::id3Pair("5\0abc", 5, &n, &t));  // lofty's map: TRCK to TrackTotal
  TEST_ASSERT_EQUAL(5, t);
  TEST_ASSERT_EQUAL(kHaveNumber, tagrules::id3Pair("0", 1, &n, &t));
  TEST_ASSERT_EQUAL(0, n);
  // A text longer than the value buffer: its last part, cut, parses as nothing.
  TEST_ASSERT_EQUAL(kHaveTotal, tagrules::id3Pair("32\0xxxx", 7, &n, &t, true));
  TEST_ASSERT_EQUAL(32, t);
  TEST_ASSERT_EQUAL(0, tagrules::id3Pair("3/12", 4, &n, &t, true));
  TEST_ASSERT_EQUAL(0, tagrules::id3Pair("1234", 4, &n, &t, true));
  TEST_ASSERT_EQUAL(kHaveNumber | kHaveTotal, tagrules::numOf(" 3 / 12 ", 8, &n, &t));
  TEST_ASSERT_EQUAL(kHaveTotal, tagrules::numOf("x/2", 3, &n, &t));
  TEST_ASSERT_EQUAL(2, t);
  TEST_ASSERT_EQUAL(kHaveNumber, tagrules::numOf("4", 1, &n, &t));
  TEST_ASSERT_EQUAL(65535, tagrules::clampNumber(99999));
}

void test_compilation_camelot_mime() {
  TEST_ASSERT_EQUAL(1, tagrules::compilationOf("1", 1));
  TEST_ASSERT_EQUAL(1, tagrules::compilationOf("TRUE", 4));
  TEST_ASSERT_EQUAL(2, tagrules::compilationOf("0", 1));
  TEST_ASSERT_EQUAL(2, tagrules::compilationOf("False", 5));
  TEST_ASSERT_EQUAL(0, tagrules::compilationOf("yes", 3));
  TEST_ASSERT_EQUAL(0, tagrules::compilationOf(" 1", 2));
  auto key = [](const char* s) { return tagrules::camelotOf(s, std::strlen(s)); };
  TEST_ASSERT_EQUAL(8, key("Am"));
  TEST_ASSERT_EQUAL(8, key("  am "));
  TEST_ASSERT_EQUAL(8, key("8A"));
  TEST_ASSERT_EQUAL(20, key("8b"));
  TEST_ASSERT_EQUAL(11, key("F#m"));
  TEST_ASSERT_EQUAL(13, key("B"));
  TEST_ASSERT_EQUAL(14, key("Gbmaj"));
  TEST_ASSERT_EQUAL(1, key("G# minor"));
  TEST_ASSERT_EQUAL(0, key("H minor"));
  TEST_ASSERT_EQUAL(0, key(""));
  TEST_ASSERT_EQUAL(0, key("A minor      x"));  // 12 characters: "A minor     "
  auto mime = [](const char* s) { return tagrules::mimeOf(s, std::strlen(s)); };
  TEST_ASSERT_EQUAL(1, mime("image/jpeg"));
  TEST_ASSERT_EQUAL(1, mime("IMAGE/JPG"));
  TEST_ASSERT_EQUAL(2, mime("image/png"));
  TEST_ASSERT_EQUAL(3, mime("image/gif"));
  TEST_ASSERT_EQUAL(3, mime(""));
  TEST_ASSERT_EQUAL(3, mime("jpeg"));
  const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  const uint8_t jpg[8] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0};
  const uint8_t gif[8] = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
  const uint8_t none[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL(2, tagrules::mimeOfMagic(png));
  TEST_ASSERT_EQUAL(1, tagrules::mimeOfMagic(jpg));
  TEST_ASSERT_EQUAL(3, tagrules::mimeOfMagic(gif));
  TEST_ASSERT_EQUAL(0, tagrules::mimeOfMagic(none));
  TEST_ASSERT_EQUAL(3, tagrules::apePictureType("Cover Art (Front)", 17));
  TEST_ASSERT_EQUAL(3, tagrules::apePictureType("cover art (front)", 17));
  TEST_ASSERT_EQUAL(0, tagrules::apePictureType("Cover Art (Other)", 17));
  TEST_ASSERT_EQUAL(-1, tagrules::apePictureType("Cover Art", 9));
}

void test_single_field() {
  tagscan::SingleField f;
  TEST_ASSERT_FALSE(f.add("", 0));
  TEST_ASSERT_TRUE(f.add("A\tB", 3));
  TEST_ASSERT_EQUAL_STRING("A B", f.data());  // control characters to spaces
  TEST_ASSERT_FALSE(f.add("later", 5));      // the first value is the field
  f.clear();
  std::string long300(300, 'x');
  TEST_ASSERT_TRUE(f.add(long300.data(), long300.size()));
  TEST_ASSERT_EQUAL(255, f.size());
  TEST_ASSERT_TRUE(f.truncated());
  f.clear();
  std::string cut = std::string(254, 'a') + "\xC3\xA9";
  TEST_ASSERT_TRUE(f.add(cut.data(), cut.size()));
  TEST_ASSERT_EQUAL(254, f.size());  // cut at a code point
  TEST_ASSERT_TRUE(f.truncated());
}

void test_kind_of() {
  TEST_ASSERT_TRUE(tagscan::kindOf("a/b/x.MP3") == Kind::Mp3);
  TEST_ASSERT_TRUE(tagscan::kindOf("y.flac") == Kind::Flac);
  TEST_ASSERT_TRUE(tagscan::kindOf("z.Opus") == Kind::Opus);
  TEST_ASSERT_TRUE(tagscan::kindOf("a.ogg") == Kind::Unknown);
  TEST_ASSERT_TRUE(tagscan::kindOf("mp3") == Kind::Unknown);
}

// ===========================================================================
// The corpus against the reference reader (2.17, item 3)
// ===========================================================================
void test_corpus_parity() {
  const minijson::Value& exp = expected();
  int checked = 0, unreadable = 0;
  std::string failures;
  for (const auto& m : exp.members) {
    const std::string& name = m.first;
    const minijson::Value& e = m.second;
    const Bytes b = fileBytes(name);
    TEST_ASSERT_FALSE_MESSAGE(b.empty(), name.c_str());
    Scanned sc;
    scanInto(sc, b, kindOfName(name));
    if (e.has("error")) {
      // lofty fails the file: the reference record is UNREADABLE (without
      // FROM_API), and the device reads it itself (2.9). Its reading is
      // pinned in test_where_lofty_fails.
      ++unreadable;
      continue;
    }
    if (sc.result != Result::Ok) {
      failures += "\n  " + name + ": not read";
      continue;
    }
    const std::string d = tagfixtures::compare(sc.rec(), e, b, kindOfName(name) == Kind::Opus);
    if (!d.empty()) failures += "\n  " + name + ":" + d;
    ++checked;
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.c_str());
  TEST_ASSERT_GREATER_OR_EQUAL(70, checked);
  TEST_ASSERT_EQUAL(2, unreadable);
}

// Where lofty fails the whole file, the device's reading (lenient).
void test_where_lofty_fails() {
  Scanned sc;
  scanInto(sc, fileBytes("lofty_fails_surrogate.mp3"), Kind::Mp3);
  TEST_ASSERT_TRUE(sc.result == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("Lone Surrogate", sc.rec().title.data());
  TEST_ASSERT_EQUAL_STRING("A\xEF\xBF\xBD" "B", sc.rec().artist.data());  // the surrogate is U+FFFD (5.2)
  scanInto(sc, fileBytes("lofty_fails_nobom.mp3"), Kind::Mp3);
  TEST_ASSERT_TRUE(sc.result == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("No BOM", sc.rec().title.data());  // little-endian
}

// The same records whatever the buffer (the cursor's reads differ).
void test_corpus_buffer_sizes() {
  std::string failures;
  for (const std::string& name : corpusNames()) {
    const Bytes b = fileBytes(name);
    Scanned ref;
    scanInto(ref, b, kindOfName(name), 4096);
    for (uint32_t size : {512u, 600u, 1000u, 1024u, 1500u, 1536u, 2048u, 8192u, 65536u}) {
      Scanned sc;
      scanInto(sc, b, kindOfName(name), size);
      if (sc.result != ref.result || !sameRecord(sc.rec(), ref.rec()))
        failures += "\n  " + name + " with a buffer of " + std::to_string(size);
    }
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.c_str());
}

// ---------------------------------------------------------------------------
// The anchors (2.6.4), read back by an independent reader of each coding
// (TagFixtures.h): the image bytes hash to anchors.json's.
// ---------------------------------------------------------------------------
void test_picture_anchors_read_back() {
  int pictures = 0;
  std::string failures;
  for (const auto& m : anchors().members) {
    const std::string& name = m.first;
    const Bytes b = fileBytes(name);
    Scanned sc;
    scanInto(sc, b, kindOfName(name));
    const mptg::Record& r = sc.rec().rec;
    if (!r.picLength) continue;
    Bytes image;
    if (!tagfixtures::readPicture(b, r, kindOfName(name) == Kind::Opus, &image)) {
      failures += "\n  " + name + ": the anchor doesn't read";
      continue;
    }
    const std::string h = tagfixtures::hex16(cc::fnv1a64(image.data(), image.size()));
    bool known = false;
    for (const auto& p : m.second["pictures"].items) known = known || p["fnv"].str() == h;
    if (!known) failures += "\n  " + name + ": the anchor's bytes are no picture of the file";
    // The MIME the record says matches the bytes.
    if (r.picMime == mptg::kMimeJpeg && !(image.size() > 2 && image[0] == 0xFF && image[1] == 0xD8))
      failures += "\n  " + name + ": JPEG said, not read";
    if (r.picMime == mptg::kMimePng && !(image.size() > 2 && image[0] == 0x89 && image[1] == 'P'))
      failures += "\n  " + name + ": PNG said, not read";
    ++pictures;
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.c_str());
  TEST_ASSERT_GREATER_OR_EQUAL(15, pictures);
}

// ---------------------------------------------------------------------------
// The record in the contract's hands: the run (2.6.5), RunFields, the
// builder's view, an MPTG file written and walked back.
// ---------------------------------------------------------------------------
void test_record_into_the_contract() {
  const char* names[] = {"v24_full.mp3", "flac_full.flac", "opus_basic.opus", "multivalue.mp3", "no_tags.mp3",
                         "long_values.mp3", "v1_fill.mp3", "ape_only.mp3"};
  std::vector<std::unique_ptr<Scanned>> scans;
  std::vector<mptg::RecordIn> ins;
  std::vector<std::string> paths;
  for (const char* n : names) {
    scans.emplace_back(new Scanned());
    scanInto(*scans.back(), fileBytes(n), kindOfName(n));
    TEST_ASSERT_TRUE_MESSAGE(scans.back()->result == Result::Ok, n);
    paths.push_back(std::string("Artist/Album/") + n);
  }
  for (size_t i = 0; i < scans.size(); ++i) {
    const tagscan::Record& r = scans[i]->rec();
    // The run: encoded, parsed back, field for field.
    const char* f[cc::kRunFields];
    r.fields(f);
    std::vector<uint8_t> run(cc::kRunMax);
    const size_t n = cc::encodeRun(f, run.data(), run.size());
    std::unique_ptr<cc::RunFields> parsed(new cc::RunFields());
    std::unique_ptr<cc::RunFields> direct(new cc::RunFields());
    r.toRunFields(direct.get());
    if (n) {
      TEST_ASSERT_TRUE(cc::parseRun(run.data(), n, parsed.get()));
      TEST_ASSERT_EQUAL(parsed->n, direct->n);
    } else {
      TEST_ASSERT_EQUAL(0, direct->n);
    }
    for (uint32_t k = 0; k < cc::kRunFields; ++k) {
      TEST_ASSERT_EQUAL(r.fieldLength(k), direct->len(k));
      if (!r.fieldLength(k)) continue;
      TEST_ASSERT_EQUAL_MEMORY(r.field(k), direct->get(k), r.fieldLength(k));
      if (n) TEST_ASSERT_EQUAL_MEMORY(r.field(k), parsed->get(k), r.fieldLength(k));
    }
    // The builder's view takes the record as a transfer record would be.
    const LibraryIndex::TagView v = LibraryBuilder::viewOf(r.rec, direct.get(), LibraryIndex::kFromDevice);
    TEST_ASSERT_EQUAL(r.title.size(), v.titleLen);
    TEST_ASSERT_EQUAL(r.rec.year, v.year);
    TEST_ASSERT_EQUAL(r.rec.durationMs, v.durationMs);
    mptg::RecordIn in;
    in.path = paths[i].c_str();
    in.rec = r.rec;
    in.rec.size = 1000 + static_cast<uint32_t>(i);
    r.fields(in.fields);
    ins.push_back(in);
  }
  // An MPTG file of these records (the device's tags.bin, source 1).
  struct VecSink : cc::Sink {
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      if (offset + n > b.size()) b.resize(offset + n);
      if (n) std::memcpy(b.data() + offset, data, n);
      return true;
    }
    Bytes b;
  } sink;
  mptg::Meta meta;
  meta.source = mptg::kSourceDevice;
  meta.parserVersion = tagscan::kParserVersion;
  meta.producer = "test_tag_scan";
  mptg::Written w;
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mptg::write(sink, meta, ins.data(), ins.size(), nullptr, 0, nullptr, 0, &w, &error),
                           error ? error : "write");
  cc::MemSource src(sink.b.data(), static_cast<uint32_t>(sink.b.size()));
  std::vector<uint8_t> scratch(8192);
  std::unique_ptr<cc::RunFields> run(new cc::RunFields());
  std::unique_ptr<mptg::Walker> walker(new mptg::Walker());
  TEST_ASSERT_TRUE(walker->begin(src, mptg::kUseHidx, scratch.data(), static_cast<uint32_t>(scratch.size()),
                                 run.get()) == cc::Why::Ok);
  int records = 0;
  for (;;) {
    const mptg::Walker::Step s = walker->next();
    if (s == mptg::Walker::Step::End) break;
    TEST_ASSERT_TRUE(s != mptg::Walker::Step::Bad);
    if (s != mptg::Walker::Step::Record) continue;
    const std::string path(walker->path(), walker->pathLength());
    for (size_t i = 0; i < ins.size(); ++i) {
      if (path != paths[i]) continue;
      const tagscan::Record& r = scans[i]->rec();
      for (uint32_t k = 0; k < cc::kRunFields; ++k) {
        TEST_ASSERT_EQUAL(r.fieldLength(k), walker->run()->len(k));
        if (r.fieldLength(k)) TEST_ASSERT_EQUAL_MEMORY(r.field(k), walker->run()->get(k), r.fieldLength(k));
      }
      TEST_ASSERT_EQUAL(r.rec.flags, walker->record().flags);
      TEST_ASSERT_EQUAL(r.rec.picOffset, walker->record().picOffset);
      ++records;
    }
  }
  TEST_ASSERT_EQUAL(static_cast<int>(ins.size()), records);
}

// ---------------------------------------------------------------------------
// Reads: the design's 1-3 reads of 4 KB a file (3.3.1), a picture skipped
// by a seek, the budget kept.
// ---------------------------------------------------------------------------
void test_read_counts() {
  struct Case {
    const char* name;
    uint32_t maxReads;
  } cases[] = {
      {"v24_full.mp3", 1},       // the tag and the first frame in the head; the tail in it too
      {"v1_fill.mp3", 1},        {"flac_full.flac", 1}, {"opus_basic.opus", 1},
      {"picture_first.mp3", 3},  // the head, the frames after the picture, the tail
      {"opus_picture.opus", 16},  // a page header per page of the picture, then the tail
  };
  for (const Case& c : cases) {
    Scanned sc;
    scanInto(sc, fileBytes(c.name), kindOfName(c.name));
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s: %u reads", c.name, sc.s->stats().reads);
    TEST_ASSERT_TRUE_MESSAGE(sc.result == Result::Ok, c.name);
    TEST_ASSERT_TRUE_MESSAGE(sc.s->stats().reads <= c.maxReads, msg);
  }
  // A 20 KB picture is stepped over, not read.
  Scanned sc;
  const Bytes b = fileBytes("picture_first.mp3");
  scanInto(sc, b, Kind::Mp3);
  TEST_ASSERT_LESS_THAN(b.size() / 2, sc.s->stats().bytes);
}

// What was met (issues): diagnostics that the corpus's edges set.
void test_issues() {
  struct Case {
    const char* name;
    uint32_t issue;
  } cases[] = {
      {"compressed_apic.mp3", tagscan::kIssueCompressedFrame | tagscan::kIssueSkippedPicture},
      {"encrypted_apic.mp3", tagscan::kIssueEncryptedFrame | tagscan::kIssueSkippedPicture},
      {"nonsyncsafe.mp3", tagscan::kIssueNonSyncsafe},
      {"v23_exthdr.mp3", tagscan::kIssueExtendedHeader},
      {"frame_overrun.mp3", tagscan::kIssueTruncatedTag},
  };
  for (const Case& c : cases) {
    Scanned sc;
    scanInto(sc, fileBytes(c.name), kindOfName(c.name));
    TEST_ASSERT_TRUE_MESSAGE((sc.s->issues() & c.issue) == c.issue, c.name);
  }
}

void test_unreadable_and_read_errors() {
  // Not a file of its kind: UNREADABLE, known 0 (2.6.4).
  Bytes junk(3000, 0x41);
  Scanned sc;
  for (Kind k : {Kind::Mp3, Kind::Flac, Kind::Opus, Kind::Unknown}) {
    scanInto(sc, junk, k);
    TEST_ASSERT_TRUE(sc.result == Result::Unreadable);
    TEST_ASSERT_EQUAL(mptg::kUnreadable, sc.rec().rec.flags);
    TEST_ASSERT_EQUAL(0, sc.rec().rec.known);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(k), sc.rec().rec.container);
  }
  // One MPEG frame and nothing after it: lofty finds no audio either.
  Bytes one(144, 0);
  one[0] = 0xFF, one[1] = 0xFB, one[2] = 0x18;
  scanInto(sc, one, Kind::Mp3);
  TEST_ASSERT_TRUE(sc.result == Result::Unreadable);
  // A source that fails: ReadError, whatever read it fails at.
  const Bytes b = fileBytes("picture_first.mp3");
  scanInto(sc, b, Kind::Mp3);
  const int reads = static_cast<int>(sc.s->stats().reads);
  TEST_ASSERT_GREATER_THAN(1, reads);
  for (int after = 0; after < reads; ++after) {
    std::vector<uint8_t> buf(4096);
    TestSource src(b);
    src.failAfter = after;
    std::unique_ptr<tagscan::Scanner> s(new tagscan::Scanner());
    const Result r = s->scan(src, Kind::Mp3, buf.data(), static_cast<uint32_t>(buf.size()));
    TEST_ASSERT_TRUE(r == Result::ReadError);
    TEST_ASSERT_EQUAL(mptg::kUnreadable, s->record().rec.flags);
    TEST_ASSERT_TRUE(s->issues() & tagscan::kIssueReadError);
  }
  // A buffer too small is refused.
  std::vector<uint8_t> tiny(256);
  TestSource src(b);
  std::unique_ptr<tagscan::Scanner> s(new tagscan::Scanner());
  TEST_ASSERT_TRUE(s->scan(src, Kind::Mp3, tiny.data(), 256) == Result::Unreadable);
}

// ---------------------------------------------------------------------------
// The budget (3.3.1: a file's scan is a bounded read). The tags are walked
// within it, the rest (the audio, the tail, the length, the values found)
// in a reserve: a stop is Partial with what was found, never UNREADABLE,
// and never an Ok that lost something.
// ---------------------------------------------------------------------------

// A v2.2/2.3 tag under tag-level unsynchronisation is read through (a
// frame's stored length is known no other way): a cover of megabytes behind
// it, read whole within the extension, whatever the buffer. (Before the
// extension, past about 500 KB the budget ran out and the file read as
// UNREADABLE.)
void test_unsynchronised_tag_with_a_big_cover() {
  struct Case {
    int ver;
    uint32_t kb;
  } const cases[] = {{3, 700}, {3, 2000}, {2, 1024}};
  for (const Case& k : cases) {
    const Bytes image = gen::jpeg(k.kb * 1024u, 7);
    const Bytes f = gen::unsyncMp3(k.ver, image);
    for (uint32_t size : {512u, 600u, 1000u, 4096u}) {
      Scanned sc;
      scanInto(sc, f, Kind::Mp3, size);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "v2.%d, a %u KB cover, a buffer of %u", k.ver, k.kb, size);
      TEST_ASSERT_TRUE_MESSAGE(sc.result == Result::Ok, msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("Song", sc.rec().title.data(), msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("Band", sc.rec().artist.data(), msg);  // after the cover
      TEST_ASSERT_INT_WITHIN_MESSAGE(100, 521, sc.rec().rec.durationMs, msg);
      TEST_ASSERT_EQUAL_MESSAGE(3, sc.rec().rec.picType, msg);
      TEST_ASSERT_EQUAL_MESSAGE(mptg::kCodingUnsync, sc.rec().rec.picCoding, msg);
      TEST_ASSERT_TRUE_MESSAGE(pictureIs(f, sc.rec(), false, image), msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(
          "", invariants(*sc.s, sc.result, static_cast<uint32_t>(f.size()), tagscan::Limits()).c_str(), msg);
    }
  }
}

// An Opus picture of 9 MB: about 190 pages of 64 KB, each page header a
// probe that counts its bytes, not a read. The comments after it and the
// length from the last page are read. (Before, the read count ran out near
// 7.5 MB and the record silently lost its fields and length.)
void test_opus_picture_of_many_pages() {
  const Bytes image = gen::jpeg(9u << 20, 11);
  const std::string pic = gen::pictureComment(image);
  const Bytes first = gen::opus({"TITLE=Song", "ARTIST=Band", pic});
  const Bytes last = gen::opus({pic, "TITLE=Song", "ARTIST=Band"});
  for (const Bytes* f : {&first, &last}) {
    for (uint32_t size : {512u, 4096u}) {
      Scanned sc;
      scanInto(sc, *f, Kind::Opus, size);
      TEST_ASSERT_TRUE(sc.result == Result::Ok);
      TEST_ASSERT_EQUAL_STRING("Song", sc.rec().title.data());
      TEST_ASSERT_EQUAL_STRING("Band", sc.rec().artist.data());
      TEST_ASSERT_INT_WITHIN(100, 10000, sc.rec().rec.durationMs);
      TEST_ASSERT_EQUAL(3, sc.rec().rec.picType);
      TEST_ASSERT_EQUAL(mptg::kCodingOggBase64, sc.rec().rec.picCoding);
      TEST_ASSERT_TRUE(pictureIs(*f, sc.rec(), true, image));
      TEST_ASSERT_GREATER_THAN(180, sc.s->stats().pages);
      TEST_ASSERT_EQUAL_STRING(
          "", invariants(*sc.s, sc.result, static_cast<uint32_t>(f->size()), tagscan::Limits()).c_str());
    }
  }
}

// A budget stop: what the walk found before it, the length (the reserve),
// Partial; never UNREADABLE.
void test_a_budget_stop_is_partial() {
  // Inside an unsynchronised tag's cover (the extension cut to 64 KB): the
  // title before the cover; not the artist after it, nor the cover.
  {
    const Bytes f = gen::unsyncMp3(3, gen::jpeg(700 * 1024, 7));
    tagscan::Limits lim;
    lim.maxUnsyncTag = 64 * 1024;
    for (uint32_t size : {512u, 4096u}) {
      Scanned sc;
      scanInto(sc, f, Kind::Mp3, size, lim);
      TEST_ASSERT_TRUE(sc.result == Result::Partial);
      TEST_ASSERT_TRUE(sc.s->issues() & tagscan::kIssueBudget);
      TEST_ASSERT_EQUAL_STRING("Song", sc.rec().title.data());
      TEST_ASSERT_EQUAL_STRING("", sc.rec().artist.data());
      TEST_ASSERT_EQUAL(0, sc.rec().rec.picLength);
      TEST_ASSERT_INT_WITHIN(100, 521, sc.rec().rec.durationMs);
      TEST_ASSERT_EQUAL_STRING("", invariants(*sc.s, sc.result, static_cast<uint32_t>(f.size()), lim).c_str());
    }
  }
  // Inside an Opus picture (16 KB of page headers): the comments before it
  // and the length; neither those after it nor the picture.
  {
    const Bytes image = gen::jpeg(2u << 20, 13);
    const std::string pic = gen::pictureComment(image);
    const Bytes first = gen::opus({"TITLE=Song", "ARTIST=Band", pic});
    const Bytes last = gen::opus({pic, "TITLE=Song", "ARTIST=Band"});
    tagscan::Limits lim;
    lim.readBudget = 16 * 1024;
    Scanned sc;
    scanInto(sc, first, Kind::Opus, 4096, lim);
    TEST_ASSERT_TRUE(sc.result == Result::Partial);
    TEST_ASSERT_EQUAL_STRING("Song", sc.rec().title.data());
    TEST_ASSERT_EQUAL_STRING("Band", sc.rec().artist.data());
    TEST_ASSERT_EQUAL(0, sc.rec().rec.picLength);
    TEST_ASSERT_INT_WITHIN(100, 10000, sc.rec().rec.durationMs);
    TEST_ASSERT_EQUAL_STRING("", invariants(*sc.s, sc.result, static_cast<uint32_t>(first.size()), lim).c_str());
    scanInto(sc, last, Kind::Opus, 4096, lim);
    TEST_ASSERT_TRUE(sc.result == Result::Partial);
    TEST_ASSERT_EQUAL_STRING("", sc.rec().title.data());
    TEST_ASSERT_TRUE(sc.rec().rec.flags & mptg::kNoTags);
    TEST_ASSERT_INT_WITHIN(100, 10000, sc.rec().rec.durationMs);
    TEST_ASSERT_EQUAL_STRING("", invariants(*sc.s, sc.result, static_cast<uint32_t>(last.size()), lim).c_str());
  }
  // Every corpus file under tight budgets: where the whole scan is Ok, Ok
  // with the same record (no stop) or Partial (a stop), never UNREADABLE.
  tagscan::Limits tight[4];
  tight[0].maxReads = 2;
  tight[0].readBudget = 2048;
  tight[1].maxReads = 1;
  tight[1].readBudget = 512;
  tight[1].reserveReads = 2;
  tight[1].reserveBytes = 1024;
  tight[2].maxReads = 0;
  tight[2].readBudget = 0;
  tight[2].reserveReads = 0;
  tight[2].reserveBytes = 0;
  tight[3].readBudget = 6000;
  tight[3].maxUnsyncTag = 0;
  std::string failures;
  int partial = 0;
  for (const std::string& name : corpusNames()) {
    const Bytes b = fileBytes(name);
    Scanned whole;
    scanInto(whole, b, kindOfName(name));
    if (whole.result != Result::Ok) continue;
    for (const tagscan::Limits& lim : tight) {
      for (uint32_t size : {512u, 4096u}) {
        Scanned sc;
        scanInto(sc, b, kindOfName(name), size, lim);
        std::string bad = invariants(*sc.s, sc.result, static_cast<uint32_t>(b.size()), lim);
        if (sc.result == Result::Partial) ++partial;
        if (bad.empty() && sc.result != Result::Ok && sc.result != Result::Partial) bad = "not read";
        if (bad.empty() && sc.result == Result::Ok && !sameRecord(sc.rec(), whole.rec())) bad = "a silent Ok";
        if (!bad.empty()) failures += "\n  " + name + " (" + std::to_string(lim.maxReads) + " reads): " + bad;
      }
    }
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.substr(0, 3000).c_str());
  TEST_ASSERT_GREATER_THAN(100, partial);
}

// An MP3's length is its first frame's, the frame lofty's rule found, read
// in a window that doesn't depend on the buffer. Here the frame after the
// first has a bitrate TrackProgress's own search refuses (lofty's mask
// takes it), and a frame inside the first claims a Xing count of years: a
// search from the first frame landed on it in some windows (73 hours, the
// count cut to 32 bits, with a 512-byte buffer; the bitrate's 0.57 s with
// 4 KB).
void test_mp3_length_whatever_the_buffer() {
  Bytes f;
  const uint8_t hdr[4] = {0xFF, 0xFB, 0x90, 0x00};  // 128 kbps, 44.1 kHz: 417 bytes
  gen::put(f, hdr, 4);
  f.insert(f.end(), 413, 0x55);
  const uint8_t fake[4] = {0xFF, 0xFB, 0x90, 0x00};
  std::memcpy(f.data() + 100, fake, 4);
  std::memcpy(f.data() + 100 + 4 + 32, "Xing", 4);
  const uint8_t xing[8] = {0, 0, 0, 1, 0x7F, 0xFF, 0xFF, 0xFF};
  std::memcpy(f.data() + 100 + 4 + 32 + 4, xing, 8);
  const uint8_t badRate[4] = {0xFF, 0xFB, 0xF0, 0x00};  // bitrate index 15
  gen::put(f, badRate, 4);
  f.insert(f.end(), 413, 0x55);
  for (int i = 0; i < 20; ++i) {
    gen::put(f, hdr, 4);
    f.insert(f.end(), 413, 0x55);
  }
  const uint32_t want = static_cast<uint32_t>((f.size() * 8 + 64) / 128);  // the bitrate over the audio
  for (uint32_t size : {512u, 600u, 1000u, 1024u, 1500u, 2048u, 4096u, 8192u}) {
    Scanned sc;
    scanInto(sc, f, Kind::Mp3, size);
    char msg[64];
    std::snprintf(msg, sizeof(msg), "a buffer of %u", size);
    TEST_ASSERT_TRUE_MESSAGE(sc.result == Result::Ok, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, sc.rec().rec.durationMs, msg);
  }
}

// Generated files (gen::random*: ID3v2.2-2.4 tags of up to 150 frames with
// pictures up to 64 KB and tag-level unsynchronisation, FLAC and Opus with
// up to 300 comments, Opus pictures up to 400 KB over pages of 16 to 255
// segments): the invariants; the same record from a buffer that isn't a
// multiple of 512; and under a random tight budget, Ok only with the whole
// scan's record (else Partial), never UNREADABLE.
void test_generated_fuzz() {
  std::mt19937 rng(0x5EEDu);
  std::string failures;
  long scans = 0, partial = 0;
  static const uint32_t kOdd[] = {512, 600, 1000, 1500, 2048};
  Scanned a, b, t;
  for (int i = 0; i < 450; ++i) {
    const int which = i % 3;
    const Bytes f = which == 0 ? gen::randomMp3(rng) : which == 1 ? gen::randomFlac(rng) : gen::randomOpus(rng);
    const Kind kind = which == 0 ? Kind::Mp3 : which == 1 ? Kind::Flac : Kind::Opus;
    const uint32_t size = static_cast<uint32_t>(f.size());
    scanInto(a, f, kind, 4096);
    scanInto(b, f, kind, kOdd[i % 5]);
    std::string bad = invariants(*a.s, a.result, size, tagscan::Limits());
    if (bad.empty()) bad = invariants(*b.s, b.result, size, tagscan::Limits());
    if (bad.empty() && a.result != Result::Ok) bad = "the whole scan isn't Ok";
    if (bad.empty() && b.result != Result::Partial && (b.result != a.result || !sameRecord(a.rec(), b.rec())))
      bad = "another record with another buffer";
    tagscan::Limits lim;
    lim.maxReads = static_cast<uint16_t>(rng() % 40);
    lim.readBudget = rng() % 65536;
    lim.reserveReads = static_cast<uint16_t>(rng() % 10);
    lim.reserveBytes = rng() % 16384;
    lim.maxUnsyncTag = rng() % 262144;
    scanInto(t, f, kind, kOdd[(i + 2) % 5], lim);
    if (bad.empty()) bad = invariants(*t.s, t.result, size, lim);
    if (bad.empty() && t.result == Result::Ok && !sameRecord(t.rec(), a.rec())) bad = "a silent Ok under a tight budget";
    if (bad.empty() && t.result != Result::Ok && t.result != Result::Partial) bad = "a budget made it UNREADABLE";
    if (t.result == Result::Partial) ++partial;
    if (!bad.empty()) {
      failures += "\n  file " + std::to_string(i) + ": " + bad;
      if (failures.size() > 3000) break;
    }
    scans += 3;
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.c_str());
  char msg[80];
  std::snprintf(msg, sizeof(msg), "%ld scans of generated files, %ld under a tight budget Partial", scans, partial);
  TEST_MESSAGE(msg);
  TEST_ASSERT_GREATER_THAN(100, partial);
}

void test_memory() {
  // A PSRAM object (about 10 KB); never the card worker's stack.
  TEST_ASSERT_LESS_OR_EQUAL(10 * 1024 + 512, sizeof(tagscan::Scanner));
  TEST_ASSERT_LESS_OR_EQUAL(6400, sizeof(tagscan::Record));
}

// ---------------------------------------------------------------------------
// Hardening: every corpus file cut at every byte (the larger ones at a
// stride), then mutated copies, each checked against the invariants.
// ---------------------------------------------------------------------------
void test_truncation() {
  const tagscan::Limits lim;
  std::string failures;
  long scans = 0;
  for (const std::string& name : corpusNames()) {
    const Bytes full = fileBytes(name);
    const size_t step = full.size() > 4096 ? full.size() / 997 + 1 : 1;
    Scanned sc;
    for (size_t n = 0; n <= full.size(); n += step) {
      const Bytes b(full.begin(), full.begin() + static_cast<long>(n));
      scanInto(sc, b, kindOfName(name));
      const std::string bad = invariants(*sc.s, sc.result, static_cast<uint32_t>(b.size()), lim);
      if (!bad.empty()) failures += "\n  " + name + " cut at " + std::to_string(n) + ": " + bad;
      ++scans;
    }
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.substr(0, 4000).c_str());
  TEST_ASSERT_GREATER_THAN(20000, scans);
}

void test_fuzz() {
  std::mt19937 rng(0x7A6Bu);
  const tagscan::Limits lim;
  std::string failures;
  const std::vector<std::string> names = corpusNames();
  const int rounds = 120;  // per file
  long scans = 0;
  Scanned sc, again;
  for (const std::string& name : names) {
    const Bytes full = fileBytes(name);
    const Kind kind = kindOfName(name);
    for (int round = 0; round < rounds; ++round) {
      Bytes b = full;
      const int edits = 1 + static_cast<int>(rng() % 8);
      for (int e = 0; e < edits && !b.empty(); ++e) {
        // Mostly in the tags at the head (where the parsing is), some anywhere.
        const size_t span = (rng() % 4) ? (b.size() < 2048 ? b.size() : 2048) : b.size();
        const size_t at = rng() % span;
        switch (rng() % 9) {
          case 0: b[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
          case 1: b[at] = static_cast<uint8_t>(rng()); break;
          case 2: b[at] = 0xFF; break;
          case 3: b[at] = 0x00; break;
          case 4: b[at] = 0x80; break;
          case 5:
            // A size field blown up.
            for (size_t k = 0; k < 4 && at + k < b.size(); ++k) b[at + k] = static_cast<uint8_t>(rng() | 0x40);
            break;
          case 6: b.insert(b.begin() + static_cast<long>(at), static_cast<uint8_t>(rng())); break;
          case 7: b.erase(b.begin() + static_cast<long>(at)); break;
          case 8: b.resize(at); break;
        }
      }
      scanInto(sc, b, kind);
      std::string bad = invariants(*sc.s, sc.result, static_cast<uint32_t>(b.size()), lim);
      // The same bytes read the same (no state carried between files),
      // whatever the buffer (sizes that aren't multiples of 512 included);
      // a budget stop may come at another place with another buffer.
      static const uint32_t kOther[] = {1024, 600, 1000, 1500, 512};
      scanInto(again, b, kind, kOther[round % 5]);
      if (bad.empty()) bad = invariants(*again.s, again.result, static_cast<uint32_t>(b.size()), lim);
      if (bad.empty() && sc.result != Result::Partial && again.result != Result::Partial &&
          (again.result != sc.result || !sameRecord(again.rec(), sc.rec())))
        bad = "not the same record with another buffer";
      if (!bad.empty()) {
        failures += "\n  " + name + " round " + std::to_string(round) + ": " + bad;
        if (failures.size() > 3000) break;
      }
      ++scans;
    }
  }
  if (!failures.empty()) TEST_FAIL_MESSAGE(failures.c_str());
  char msg[64];
  std::snprintf(msg, sizeof(msg), "%ld fuzzed scans", scans);
  TEST_MESSAGE(msg);
  TEST_ASSERT_GREATER_THAN(9000, scans);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rust_u32);
  RUN_TEST(test_trim_and_blank);
  RUN_TEST(test_genre_table_and_tcon);
  RUN_TEST(test_lofty_timestamps);
  RUN_TEST(test_year_rule);
  RUN_TEST(test_number_pairs);
  RUN_TEST(test_compilation_camelot_mime);
  RUN_TEST(test_single_field);
  RUN_TEST(test_kind_of);
  RUN_TEST(test_corpus_parity);
  RUN_TEST(test_where_lofty_fails);
  RUN_TEST(test_corpus_buffer_sizes);
  RUN_TEST(test_picture_anchors_read_back);
  RUN_TEST(test_record_into_the_contract);
  RUN_TEST(test_read_counts);
  RUN_TEST(test_issues);
  RUN_TEST(test_unreadable_and_read_errors);
  RUN_TEST(test_unsynchronised_tag_with_a_big_cover);
  RUN_TEST(test_opus_picture_of_many_pages);
  RUN_TEST(test_a_budget_stop_is_partial);
  RUN_TEST(test_mp3_length_whatever_the_buffer);
  RUN_TEST(test_memory);
  RUN_TEST(test_truncation);
  RUN_TEST(test_fuzz);
  RUN_TEST(test_generated_fuzz);
  return UNITY_END();
}
