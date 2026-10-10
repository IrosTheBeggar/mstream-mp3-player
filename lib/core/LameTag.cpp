// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LameTag.h"

#include <cstring>

#include "TrackProgress.h"

namespace lametag {

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool trustedEncoder(const uint8_t* v) {
  return std::memcmp(v, "LAME", 4) == 0 || std::memcmp(v, "Lavf", 4) == 0 || std::memcmp(v, "Lavc", 4) == 0;
}

}  // namespace

uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b) crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
  }
  return crc;
}

namespace {

// The frame at buf[i] (its header read into `f`) into *out: its Xing/Info or
// VBRI header, and LAME's extension when it fits in the frame and the buffer.
void readFrame(const uint8_t* buf, size_t n, size_t i, const progress::Mp3Frame& f, Info* out) {
  const size_t next = i + static_cast<size_t>(f.length);
  Info& o = *out;
  o.frame = true;
  o.frameAt = static_cast<uint32_t>(i);
  o.rate = static_cast<uint32_t>(f.rate);
  o.spf = static_cast<uint32_t>(f.samples);
  o.kbps = static_cast<uint32_t>(f.kbps);
  const bool crc = (buf[i + 1] & 1) == 0;  // protection bit 0: a CRC after the header
  const size_t xing = i + 4 + (crc ? 2 : 0) + static_cast<size_t>(f.sideInfo);
  const size_t vbri = i + 4 + 32;
  if (xing + 8 <= n && (std::memcmp(buf + xing, "Xing", 4) == 0 || std::memcmp(buf + xing, "Info", 4) == 0)) {
    o.header = true;
    o.xing = true;
    o.info = buf[xing] == 'I';
    o.headerLength = static_cast<uint32_t>(f.length);
    const uint32_t flags = be32(buf + xing + 4);
    size_t p = xing + 8;
    if (flags & 1) {
      if (p + 4 > n) return;
      o.frames = be32(buf + p);
      p += 4;
    }
    if (flags & 2) p += 4;
    if (flags & 4) p += 100;
    if (flags & 8) p += 4;
    // LAME's extension, if it fits in the frame and the buffer.
    if (p + 36 > n || p + 36 > next) return;
    const uint8_t* e = buf + p;
    std::memcpy(o.encoder, e, 9);
    o.encoder[9] = 0;
    for (int k = 0; k < 9; ++k) {
      if (o.encoder[k] < 0x20 || o.encoder[k] > 0x7E) o.encoder[k] = '.';
    }
    const uint16_t delay = static_cast<uint16_t>((e[21] << 4) | (e[22] >> 4));
    const uint16_t padding = static_cast<uint16_t>(((e[22] & 0x0F) << 8) | e[23]);
    o.crcChecked = true;
    o.crcBytes = static_cast<uint32_t>(p + 34 - i);
    o.crcStored = static_cast<uint16_t>((e[34] << 8) | e[35]);
    o.crcComputed = crc16(buf + i, o.crcBytes);
    o.crcOk = o.crcStored == o.crcComputed;
    if (trustedEncoder(e) && o.frames > 0 &&
        static_cast<uint64_t>(delay) + padding < static_cast<uint64_t>(o.frames) * o.spf) {
      o.lame = true;
      o.delay = delay;
      o.padding = padding;
    }
  } else if (vbri + 18 <= n && std::memcmp(buf + vbri, "VBRI", 4) == 0) {
    o.header = true;
    o.vbri = true;
    o.headerLength = static_cast<uint32_t>(f.length);
    o.frames = be32(buf + vbri + 14);
  }
}

}  // namespace

bool parse(const uint8_t* buf, size_t n, Info* out) {
  *out = Info{};
  for (size_t i = 0; i + 4 <= n; ++i) {
    progress::Mp3Frame f;
    if (!progress::parseMp3Frame(buf + i, &f)) continue;
    const size_t next = i + static_cast<size_t>(f.length);
    progress::Mp3Frame g;
    if (next + 4 <= n && !progress::parseMp3Frame(buf + next, &g)) continue;
    readFrame(buf, n, i, f, out);
    return true;
  }
  return false;
}

bool parseFirst(const uint8_t* buf, size_t n, Info* out) {
  *out = Info{};
  progress::Mp3Frame f;
  if (n < 4 || !progress::parseMp3Frame(buf, &f)) return false;
  readFrame(buf, n, 0, f, out);
  return true;
}

uint64_t keptSamples(const Info& info) {
  if (info.frames == 0 || info.spf == 0) return 0;
  const uint64_t all = static_cast<uint64_t>(info.frames) * info.spf;
  return info.lame ? all - info.delay - info.padding : all;
}

uint32_t lengthMs(const Info& info) {
  if (info.rate == 0) return 0;
  return static_cast<uint32_t>(keptSamples(info) * 1000 / info.rate);
}

uint32_t untrimmedMs(const Info& info, uint32_t ms) {
  if (!info.lame || info.rate == 0) return ms;
  return ms + static_cast<uint32_t>((static_cast<uint64_t>(info.delay) + kDecoderDelay) * 1000 / info.rate);
}

Trim trim(const Info& info, uint32_t lead, bool fromTop, bool useTag) {
  Trim t;
  t.skip = lead;
  if (!useTag || !info.lame) return t;
  if (fromTop) t.skip += info.delay + kDecoderDelay;
  // Under 529 the decoder's output ends before the encoder's input does:
  // nothing to cut (Rockbox clamps it the same way [5]).
  t.hold = info.padding > kDecoderDelay ? info.padding - kDecoderDelay : 0;
  return t;
}

}  // namespace lametag
