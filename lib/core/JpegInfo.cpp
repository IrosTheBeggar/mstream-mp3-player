// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "JpegInfo.h"

namespace jpeg {

namespace {

// The marker walk over any bytes: `at(i)` is byte i (0 <= i < len), or -1
// when it can't be read.
template <typename At>
Info walk(At& at, size_t len) {
  Info out;
  if (len < 4 || at(0) != 0xFF || at(1) != 0xD8) return out;
  size_t i = 2;
  while (i + 4 <= len) {
    const int b0 = at(i);
    if (b0 < 0) return out;
    if (b0 != 0xFF) {  // junk between segments: skip it
      ++i;
      continue;
    }
    const int m = at(i + 1);
    if (m < 0) return out;
    if (m == 0xFF) {  // fill bytes
      ++i;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {  // markers without a length
      i += 2;
      continue;
    }
    if (m == 0xD9 || m == 0xDA) return out;  // the end, or scan data before any frame header
    const int l0 = at(i + 2), l1 = at(i + 3);
    if (l0 < 0 || l1 < 0) return out;
    const size_t segLen = static_cast<size_t>(l0) << 8 | static_cast<size_t>(l1);
    if (segLen < 2) return out;
    // SOF0-SOF15, except DHT (C4), JPG (C8) and DAC (CC).
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (i + 10 > len) return out;
      int f[5];
      for (int k = 0; k < 5; ++k) {
        f[k] = at(i + 5 + static_cast<size_t>(k));
        if (f[k] < 0) return out;
      }
      out.height = static_cast<uint16_t>(f[0] << 8 | f[1]);
      out.width = static_cast<uint16_t>(f[2] << 8 | f[3]);
      out.components = static_cast<uint8_t>(f[4]);
      out.progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
      out.ok = out.width > 0 && out.height > 0;
      return out;
    }
    i += 2 + segLen;
  }
  return out;
}

}  // namespace

Info parse(const uint8_t* p, size_t len) {
  if (!p) return Info();
  struct Mem {
    const uint8_t* p;
    int operator()(size_t i) const { return p[i]; }
  } m{p};
  return walk(m, len);
}

Info parseFile(ReadFn read, void* ctx, uint32_t size) {
  if (!read) return Info();
  // A 64-byte window, read again when the walk leaves it.
  struct Window {
    ReadFn read;
    void* ctx;
    uint32_t size;
    uint8_t buf[64];
    uint32_t at = 0;
    uint32_t n = 0;
    int operator()(size_t i) {
      if (i >= size) return -1;
      if (i < at || i >= static_cast<size_t>(at) + n) {
        const uint32_t want = size - static_cast<uint32_t>(i) < sizeof(buf) ? size - static_cast<uint32_t>(i)
                                                                             : static_cast<uint32_t>(sizeof(buf));
        if (!read(static_cast<uint32_t>(i), buf, want, ctx)) {
          n = 0;
          return -1;
        }
        at = static_cast<uint32_t>(i);
        n = want;
      }
      return buf[i - at];
    }
  } w{read, ctx, size, {}, 0, 0};
  return walk(w, size);
}

}  // namespace jpeg
