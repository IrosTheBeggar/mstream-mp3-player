// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TrackProgress.h"

#include <cstring>

#include "LameTag.h"

namespace progress {

uint32_t estimateDurationMs(uint64_t frames, int rate, uint32_t pos0, uint32_t pos, uint32_t size,
                            uint64_t minFrames) {
  if (rate <= 0 || frames < minFrames || frames == 0 || pos <= pos0 || size < pos) return 0;
  const uint64_t read = pos - pos0;
  const uint64_t left = size - pos;
  // Frames still to come at the bytes-per-frame seen so far.
  const uint64_t total = frames + left * frames / read;
  return static_cast<uint32_t>(total * 1000 / static_cast<uint64_t>(rate));
}

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

bool parseMp3Frame(const uint8_t* p, Mp3Frame* f) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  const int version = (p[1] >> 3) & 3;  // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
  const int layer = (p[1] >> 1) & 3;    // 1: Layer III
  const int bitrateIndex = p[2] >> 4;
  const int rateIndex = (p[2] >> 2) & 3;
  if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return false;
  static const int kRates[3] = {44100, 48000, 32000};
  static const int kKbps1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static const int kKbps2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  const bool mpeg1 = version == 3;
  const bool mono = (p[3] >> 6) == 3;
  f->version = version;
  f->rateIndex = rateIndex;
  f->rate = kRates[rateIndex] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
  f->samples = mpeg1 ? 1152 : 576;
  f->sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  f->kbps = mpeg1 ? kKbps1[bitrateIndex] : kKbps2[bitrateIndex];
  const int padding = (p[2] >> 1) & 1;
  f->length = (mpeg1 ? 144 : 72) * f->kbps * 1000 / f->rate + padding;
  return f->length > 4;
}

uint32_t id3v2Size(const uint8_t* h, size_t n) {
  if (n < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
  if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;  // not a syncsafe size
  const uint32_t body = (static_cast<uint32_t>(h[6]) << 21) | (static_cast<uint32_t>(h[7]) << 14) |
                        (static_cast<uint32_t>(h[8]) << 7) | h[9];
  const bool footer = (h[5] & 0x10) != 0;
  return 10 + body + (footer ? 10 : 0);
}

uint32_t truncatedMs(uint32_t lengthMs, uint32_t headerBytes, uint32_t haveBytes) {
  if (headerBytes <= kTruncatedSlack || haveBytes + kTruncatedSlack >= headerBytes) return lengthMs;
  return static_cast<uint32_t>(static_cast<uint64_t>(lengthMs) * haveBytes / headerBytes);
}

namespace {

// The length the frame at buf[i] (its header read into `f`) says, with
// LAME's reading of the same frame (`lame`).
uint32_t frameMs(const uint8_t* buf, size_t n, size_t i, const Mp3Frame& f, const lametag::Info& lame,
                 bool lameRead, uint32_t fileSize, uint32_t audioStart) {
  uint32_t frames = 0, bytes = 0;
  const size_t xing = i + 4 + static_cast<size_t>(f.sideInfo);
  const size_t vbri = i + 4 + 32;
  if (xing + 12 <= n && (std::memcmp(buf + xing, "Xing", 4) == 0 || std::memcmp(buf + xing, "Info", 4) == 0)) {
    const uint32_t flags = be32(buf + xing + 4);
    size_t p = xing + 8;
    if (flags & 1) {  // the frame count is there
      frames = be32(buf + p);
      p += 4;
    }
    if ((flags & 2) && p + 4 <= n) bytes = be32(buf + p);
  } else if (vbri + 18 <= n && std::memcmp(buf + vbri, "VBRI", 4) == 0) {
    bytes = be32(buf + vbri + 10);
    frames = be32(buf + vbri + 14);
  }
  // LAME's extension says what the encoder was given: the trimmed length,
  // the one gapless playback plays (docs/GAPLESS.md section 4.6). The
  // header frame itself is silent: not counted.
  uint32_t ms = 0;
  if (lameRead && lame.lame) {
    ms = lametag::lengthMs(lame);
  } else {
    ms = static_cast<uint32_t>(static_cast<uint64_t>(frames) * static_cast<uint64_t>(f.samples) * 1000 /
                               static_cast<uint64_t>(f.rate));
  }
  const uint32_t first = audioStart + static_cast<uint32_t>(i);
  if (fileSize > 0 && bytes > 0) ms = truncatedMs(ms, bytes, fileSize > first ? fileSize - first : 0);
  return ms;
}

}  // namespace

uint32_t mp3HeaderDurationMs(const uint8_t* buf, size_t n, uint32_t fileSize, uint32_t audioStart) {
  // The first frame: the first header whose next frame starts with a header
  // too (or runs past the buffer), so a stray 0xFF in junk before the audio
  // doesn't count.
  for (size_t i = 0; i + 4 <= n; ++i) {
    Mp3Frame f;
    if (!parseMp3Frame(buf + i, &f)) continue;
    const size_t next = i + static_cast<size_t>(f.length);
    Mp3Frame g;
    if (next + 4 <= n && !parseMp3Frame(buf + next, &g)) continue;
    lametag::Info lame;
    const bool lameRead = lametag::parse(buf, n, &lame);  // (the same first frame)
    return frameMs(buf, n, i, f, lame, lameRead, fileSize, audioStart);
  }
  return 0;
}

uint32_t mp3FrameDurationMs(const uint8_t* buf, size_t n, uint32_t fileSize, uint32_t audioStart) {
  Mp3Frame f;
  if (n < 4 || !parseMp3Frame(buf, &f)) return 0;
  lametag::Info lame;
  const bool lameRead = lametag::parseFirst(buf, n, &lame);
  return frameMs(buf, n, 0, f, lame, lameRead, fileSize, audioStart);
}

uint32_t flacDurationMs(const uint8_t* b, size_t n) {
  // "fLaC", then the STREAMINFO block: its header (type 0, length 34), 10
  // bytes of block and frame sizes, then 20 bits of sample rate, 3 of
  // channels, 5 of bits per sample and 36 of total samples.
  if (n < 26 || std::memcmp(b, "fLaC", 4) != 0 || (b[4] & 0x7F) != 0) return 0;
  const uint32_t rate = (static_cast<uint32_t>(b[18]) << 12) | (static_cast<uint32_t>(b[19]) << 4) | (b[20] >> 4);
  const uint64_t total = (static_cast<uint64_t>(b[21] & 0x0F) << 32) | be32(b + 22);
  if (rate == 0 || total == 0) return 0;
  return static_cast<uint32_t>(total * 1000 / rate);
}

}  // namespace progress
