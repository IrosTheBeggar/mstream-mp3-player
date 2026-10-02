// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Synthetic MP3 streams and a model of the device's decoder, for the seek
// tests (docs/SEEK.md section 10). Header-only, test-only.
//
// - make(): a stream of Layer III frame headers at chosen bitrates, padded
//   as LAME pads (a byte whenever the fraction carries), with side
//   information whose main_data_begin follows a bit reservoir (a frame's
//   main data may begin up to 511 bytes, 255 for MPEG-2/2.5, back in the
//   frames before it), payload bytes that are never 0xFF (no false sync
//   unless a test puts one), an optional ID3v2 tag in front, and an
//   optional Xing ("Xing" or "Info") header frame with the frame and byte
//   counts, a TOC as LAME's bag computes it (VbrTag.c), and LAME's
//   extension (the encoder string, delay and padding, the tag's CRC).
// - ModelDecoder: ESP8266Audio's AudioGeneratorMP3::loop() as it meets its
//   output (the {0,0} lead, its rate said before the first decoded frame's
//   first sample, a refused sample offered again) over libmad's reservoir
//   rule (layer3.c: a frame whose main_data_begin reaches past the bytes
//   held isn't decoded, and outputs nothing; the preload after each frame
//   as libmad does it). Each decoded frame gives spf samples, each a hash
//   of (frame, index); a frame is exact only with the frame before it
//   decoded (its IMDCT overlap and the filterbank's history; MPEG-2's
//   one-granule frames: the two before it), else its samples are marked
//   (the right channel differs). It is a FrameCursor, as PinnedMp3 is.
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "FrameCursor.h"
#include "LameTag.h"
#include "TrackSeek.h"
#include "TrimFeed.h"

namespace mp3synth {

enum class Header : uint8_t { None, Info, Xing };

struct Options {
  int version = 3;    // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
  int rateIndex = 0;  // 44.1 kHz (MPEG-1), 22.05 (MPEG-2), 11.025 (MPEG-2.5)
  bool mono = false;
  std::vector<uint32_t> kbps;  // one per audio frame
  uint32_t id3 = 0;            // an ID3v2 tag of this many bytes in front (>= 10)
  Header header = Header::None;
  uint32_t headerKbps = 0;     // the header frame's bitrate (0: 128, 64 for MPEG-2/2.5)
  bool lame = true;            // LAME's extension after the Xing fields
  const char* encoder = "LAME3.100";
  uint16_t delay = 576;
  uint16_t padding = 1200;
  uint32_t seed = 1;
  // How full a frame's main data is (0-100: % of what it can take); a
  // quiet frame (low) leaves its bytes to the reservoir.
  std::vector<uint8_t> fullness;  // per frame (empty: random 30-100)
  uint32_t trailing = 0;          // junk bytes after the last frame (an ID3v1 tag's 128, say)
  int32_t xingBytesDelta = 0;     // the Xing byte count this much off the stream's (an edited file)
};

struct Frame {
  uint32_t byte = 0;
  uint32_t length = 0;
  uint32_t kbps = 0;
  uint32_t mdb = 0;  // main_data_begin
};

struct Stream {
  std::vector<uint8_t> bytes;
  uint32_t audioStart = 0;  // after the ID3v2 tag
  uint32_t first = 0;       // the first frame (the header frame, if any)
  uint32_t firstAudio = 0;  // the first audio frame
  uint32_t rate = 0;
  uint32_t spf = 0;
  int version = 3;
  int sideInfo = 32;
  std::vector<Frame> frames;  // the audio frames
  Header header = Header::None;
  bool lame = false;
  uint16_t delay = 0;
  uint16_t padding = 0;
  uint8_t toc[100] = {};

  uint32_t size() const { return static_cast<uint32_t>(bytes.size()); }
  // The trimmed timeline's offset in the decoded stream (LAME's delay + 529).
  uint32_t trimSkip() const { return lame ? delay + lametag::kDecoderDelay : 0; }
  // Samples the trimmed timeline holds.
  uint64_t keptSamples() const {
    const uint64_t all = static_cast<uint64_t>(frames.size()) * spf;
    return lame ? all - delay - padding : all;
  }
  uint32_t lengthMs() const { return static_cast<uint32_t>(keptSamples() * 1000 / rate); }
  // The frame whose samples hold decoded sample d (from the first audio frame).
  uint32_t frameOf(uint64_t d) const { return static_cast<uint32_t>(d / spf); }
};

inline const int* kbpsTable(int version) {
  static const int k1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static const int k2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  return version == 3 ? k1 : k2;
}

inline int bitrateIndex(int version, uint32_t kbps) {
  const int* t = kbpsTable(version);
  for (int i = 1; i < 15; ++i) {
    if (static_cast<uint32_t>(t[i]) == kbps) return i;
  }
  return -1;
}

inline uint32_t rateOf(int version, int rateIndex) {
  static const uint32_t kRates[3] = {44100, 48000, 32000};
  return kRates[rateIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
}

inline void putBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

// LAME's TOC (VbrTag.c): AddVbrFrame()'s bag of sums every `want` frames
// (400 slots, halved when full, want doubled), then Xing_seek_table() with
// its float index.
inline void lameToc(const std::vector<uint32_t>& kbps, uint8_t toc[100]) {
  std::vector<uint64_t> bag(400, 0);
  uint64_t sum = 0;
  uint32_t want = 1, seen = 0, pos = 0;
  for (const uint32_t k : kbps) {
    sum += k;
    ++seen;
    if (seen < want) continue;
    if (pos < 400) {
      bag[pos++] = sum;
      seen = 0;
    }
    if (pos == 400) {
      for (uint32_t i = 1; i < 400; i += 2) bag[i / 2] = bag[i];
      want *= 2;
      pos /= 2;
    }
  }
  toc[0] = 0;
  for (int i = 1; i < 100; ++i) {
    if (pos == 0) {
      toc[i] = 0;
      continue;
    }
    const float j = static_cast<float>(i) / 100.0f;
    int indx = static_cast<int>(j * static_cast<float>(pos));
    if (indx > static_cast<int>(pos) - 1) indx = static_cast<int>(pos) - 1;
    int seek = static_cast<int>(256.0 * static_cast<double>(bag[indx]) / static_cast<double>(sum));
    if (seek > 255) seek = 255;
    toc[i] = static_cast<uint8_t>(seek);
  }
}

// One frame's 4 header bytes.
inline void putHeader(uint8_t* p, int version, int rateIndex, int brIndex, bool pad, bool mono) {
  p[0] = 0xFF;
  p[1] = static_cast<uint8_t>(0xE0 | (version << 3) | (1 << 1) | 1);  // Layer III, no CRC
  p[2] = static_cast<uint8_t>((brIndex << 4) | (rateIndex << 2) | (pad ? 2 : 0));
  p[3] = mono ? 0xC4 : 0x44;
}

inline Stream make(const Options& o) {
  Stream s;
  std::mt19937 rng(o.seed);
  s.version = o.version;
  s.rate = rateOf(o.version, o.rateIndex);
  s.spf = o.version == 3 ? 1152 : 576;
  s.sideInfo = o.version == 3 ? (o.mono ? 17 : 32) : (o.mono ? 9 : 17);
  const uint32_t slotsPerKbps = o.version == 3 ? 144000 : 72000;
  const uint32_t maxRes = o.version == 3 ? 511 : 255;
  std::vector<uint8_t>& b = s.bytes;
  if (o.id3 >= 10) {
    b.resize(o.id3, 0);
    b[0] = 'I';
    b[1] = 'D';
    b[2] = '3';
    b[3] = 3;
    const uint32_t body = o.id3 - 10;
    b[6] = static_cast<uint8_t>((body >> 21) & 0x7F);
    b[7] = static_cast<uint8_t>((body >> 14) & 0x7F);
    b[8] = static_cast<uint8_t>((body >> 7) & 0x7F);
    b[9] = static_cast<uint8_t>(body & 0x7F);
  }
  s.audioStart = static_cast<uint32_t>(b.size());
  s.first = s.audioStart;
  // The header frame, filled in once the stream's bytes are known.
  size_t headerAt = 0, headerLen = 0, xingAt = 0;
  if (o.header != Header::None) {
    const uint32_t hk = o.headerKbps ? o.headerKbps
                        : o.header == Header::Info && !o.kbps.empty() ? o.kbps[0]
                        : (o.version == 3 ? 128 : 64);
    headerLen = slotsPerKbps * hk / s.rate;
    headerAt = b.size();
    b.resize(b.size() + headerLen, 0);
    putHeader(&b[headerAt], o.version, o.rateIndex, bitrateIndex(o.version, hk), false, o.mono);
    xingAt = headerAt + 4 + static_cast<size_t>(s.sideInfo);
  }
  s.firstAudio = static_cast<uint32_t>(b.size());
  // The audio frames: LAME's padding (the fraction carries), the reservoir.
  int64_t lag = 0;
  uint32_t reservoir = 0;
  for (size_t k = 0; k < o.kbps.size(); ++k) {
    const uint32_t kbps = o.kbps[k];
    const uint32_t whole = slotsPerKbps * kbps / s.rate;
    const uint32_t frac = slotsPerKbps * kbps % s.rate;
    bool pad = false;
    if (frac != 0) {
      lag -= frac;
      if (lag < 0) {
        lag += s.rate;
        pad = true;
      }
    }
    Frame f;
    f.byte = static_cast<uint32_t>(b.size());
    f.length = whole + (pad ? 1 : 0);
    f.kbps = kbps;
    f.mdb = reservoir;
    s.frames.push_back(f);
    b.resize(b.size() + f.length);
    uint8_t* p = &b[f.byte];
    putHeader(p, o.version, o.rateIndex, bitrateIndex(o.version, kbps), pad, o.mono);
    for (uint32_t i = 4; i < f.length; ++i) p[i] = static_cast<uint8_t>(rng() % 255);  // never 0xFF
    // main_data_begin: 9 bits (MPEG-1) or 8 at the side information's start.
    if (o.version == 3) {
      p[4] = static_cast<uint8_t>(f.mdb >> 1);
      p[5] = static_cast<uint8_t>((p[5] & 0x7F) | ((f.mdb & 1) << 7));
    } else {
      p[4] = static_cast<uint8_t>(f.mdb);
    }
    // This frame's main data: up to what the reservoir and its own payload
    // hold; what it leaves is the next frame's reservoir.
    const uint32_t cap = f.length - 4 - static_cast<uint32_t>(s.sideInfo);
    const uint32_t full = k < o.fullness.size() ? o.fullness[k] : 30 + rng() % 71;
    const uint32_t data = static_cast<uint32_t>(static_cast<uint64_t>(f.mdb + cap) * full / 100);
    const uint32_t left = f.mdb + cap - data;
    reservoir = left > maxRes ? maxRes : left;
  }
  const uint32_t streamEnd = static_cast<uint32_t>(b.size());
  b.resize(b.size() + o.trailing, 0x55);
  if (o.header != Header::None) {
    uint8_t* x = &b[xingAt];
    std::memcpy(x, o.header == Header::Info ? "Info" : "Xing", 4);
    putBe32(x + 4, 0x0F);
    putBe32(x + 8, static_cast<uint32_t>(s.frames.size()));
    putBe32(x + 12, static_cast<uint32_t>(static_cast<int64_t>(streamEnd - headerAt) + o.xingBytesDelta));
    lameToc(o.kbps, s.toc);
    std::memcpy(x + 16, s.toc, 100);
    putBe32(x + 116, 57);  // quality
    if (o.lame) {
      uint8_t* e = x + 120;
      std::memcpy(e, o.encoder, std::strlen(o.encoder) < 9 ? std::strlen(o.encoder) : 9);
      e[21] = static_cast<uint8_t>(o.delay >> 4);
      e[22] = static_cast<uint8_t>(((o.delay & 0x0F) << 4) | (o.padding >> 8));
      e[23] = static_cast<uint8_t>(o.padding & 0xFF);
      const size_t crcBytes = static_cast<size_t>(e + 34 - &b[headerAt]);
      const uint16_t crc = lametag::crc16(&b[headerAt], crcBytes);
      e[34] = static_cast<uint8_t>(crc >> 8);
      e[35] = static_cast<uint8_t>(crc);
    }
    lametag::Info info;
    lametag::parse(&b[s.audioStart], b.size() - s.audioStart, &info);
    s.lame = info.lame;
    s.delay = info.delay;
    s.padding = info.padding;
  }
  s.header = o.header;
  return s;
}

// Random VBR bitrates in stretches (quiet, loud, mixed), as LAME's VBR
// files have them.
inline std::vector<uint32_t> vbrRates(int version, size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  const int* t = kbpsTable(version);
  std::vector<uint32_t> v;
  while (v.size() < frames) {
    const uint32_t len = 20 + rng() % 400;
    const int kind = static_cast<int>(rng() % 3);
    for (uint32_t i = 0; i < len && v.size() < frames; ++i) {
      int idx;
      if (kind == 0) {
        idx = 1 + static_cast<int>(rng() % 5);   // quiet
      } else if (kind == 1) {
        idx = 10 + static_cast<int>(rng() % 5);  // loud
      } else {
        idx = 1 + static_cast<int>(rng() % 14);
      }
      v.push_back(static_cast<uint32_t>(t[idx]));
    }
  }
  return v;
}

// trackseek::FileReader over a stream's bytes.
class Reader : public trackseek::FileReader {
public:
  explicit Reader(const std::vector<uint8_t>& b) : b_(b) {}
  uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n) override {
    ++reads;
    if (offset >= b_.size()) return 0;
    const uint32_t got = static_cast<uint32_t>(b_.size() - offset < n ? b_.size() - offset : n);
    std::memcpy(buf, b_.data() + offset, got);
    return got;
  }
  uint32_t reads = 0;

private:
  const std::vector<uint8_t>& b_;
};

// A decoded sample: (frame, index), exact or marked.
inline void sampleOf(uint32_t frame, uint32_t i, bool exact, int16_t out[2]) {
  uint32_t h = (frame * 2654435761u) ^ (i * 40503u + 0x9E37u);
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  out[0] = static_cast<int16_t>(h & 0x3FFF);
  out[1] = static_cast<int16_t>(exact ? (out[0] ^ 0x1234) : (out[0] ^ 0x0F0F));
}

class ModelDecoder : public FrameCursor {
public:
  // A decoder begun at `startByte` (a frame's start; else it resyncs to the
  // next one). `lose`: frames that fail to decode whatever their reservoir
  // (bad data).
  ModelDecoder(const Stream& s, uint32_t startByte, std::vector<uint32_t> lose = {})
      : s_(s), lose_(std::move(lose)) {
    next_ = 0;
    while (next_ < s_.frames.size() && s_.frames[next_].byte < startByte) ++next_;
  }

  // AudioGeneratorMP3::loop(): offers samples until `out` refuses one (the
  // pass's end: true) or the stream ends (false).
  bool loop(TrimFeed& out) {
    if (!running_) return false;
    if (!out.consume(last_)) return true;
    for (;;) {
      if (cur_ < 0 || ns_ >= s_.spf) {
        if (!decodeNext()) {
          running_ = false;
          return false;
        }
        if (!said_) {  // GetOneSample(): the rate before the first decoded sample
          out.setRate(44100);
          out.setChannels(2);
          said_ = true;
        }
      }
      sampleOf(static_cast<uint32_t>(cur_), ns_, exact_, last_);
      ++ns_;
      if (!out.consume(last_)) return true;
    }
  }

  bool at(uint32_t* frameByte, uint32_t* sampleInFrame) const override {
    if (cur_ < 0 || ns_ == 0) return false;
    *frameByte = s_.frames[static_cast<size_t>(cur_)].byte;
    *sampleInFrame = ns_ - 1;
    return true;
  }
  const uint8_t* frameBytes() const override {
    return cur_ < 0 ? nullptr : s_.bytes.data() + s_.frames[static_cast<size_t>(cur_)].byte;
  }

  uint32_t decoded() const { return decodedCount_; }
  uint32_t failed() const { return failedCount_; }

private:
  // layer3.c's reservoir rule for frame f: decoded or not, and the preload.
  bool decodeFrame(size_t f) {
    const Frame& fr = s_.frames[f];
    const uint32_t mdb = fr.mdb;
    const uint32_t frameSpace = fr.length - 4 - static_cast<uint32_t>(s_.sideInfo);
    uint32_t nextMd = f + 1 < s_.frames.size() ? s_.frames[f + 1].mdb : 0;
    if (nextMd > mdb + frameSpace) nextMd = 0;
    const uint32_t mdLen = mdb + frameSpace - nextMd;
    uint32_t frameUsed = 0;
    bool ok = true;
    if (mdb == 0) {
      held_ = 0;
      frameUsed = mdLen;
    } else if (mdb > held_) {
      ok = false;  // MAD_ERROR_BADDATAPTR
    } else if (mdLen > mdb) {
      frameUsed = mdLen - mdb;
      held_ += frameUsed;
    }
    const uint32_t frameFree = frameSpace - frameUsed;
    if (frameFree >= nextMd) {
      held_ = nextMd;
    } else {
      if (mdLen < mdb) {
        uint32_t extra = mdb - mdLen;
        if (extra + frameFree > nextMd) extra = nextMd - frameFree;
        if (extra < held_) held_ = extra;
      } else {
        held_ = 0;
      }
      held_ += frameFree;
    }
    for (const uint32_t l : lose_) {
      if (l == f) ok = false;
    }
    return ok;
  }

  bool decodeNext() {
    while (next_ < s_.frames.size()) {
      const size_t f = next_++;
      if (!decodeFrame(f)) {
        ++failedCount_;
        lastDecoded_ = -1;
        prevDecoded_ = -1;
        continue;
      }
      ++decodedCount_;
      // Exact with the frame before decoded (MPEG-2: the two before).
      const bool one = f == 0 || lastDecoded_ == static_cast<int64_t>(f) - 1;
      const bool two = s_.spf == 1152 || f <= 1 || prevDecoded_ == static_cast<int64_t>(f) - 2;
      exact_ = one && two;
      prevDecoded_ = lastDecoded_;
      lastDecoded_ = static_cast<int64_t>(f);
      cur_ = static_cast<int64_t>(f);
      ns_ = 0;
      return true;
    }
    return false;
  }

  const Stream& s_;
  std::vector<uint32_t> lose_;
  size_t next_ = 0;
  int64_t cur_ = -1;
  uint32_t ns_ = 0;
  bool exact_ = false;
  int64_t lastDecoded_ = -1;
  int64_t prevDecoded_ = -1;
  uint32_t held_ = 0;
  int16_t last_[2] = {0, 0};  // the generator's lead
  bool said_ = false;
  bool running_ = true;
  uint32_t decodedCount_ = 0;
  uint32_t failedCount_ = 0;
};

}  // namespace mp3synth
