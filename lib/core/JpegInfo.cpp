#include "JpegInfo.h"

namespace jpeg {

Info parse(const uint8_t* p, size_t len) {
  Info out;
  if (!p || len < 4 || p[0] != 0xFF || p[1] != 0xD8) return out;
  size_t i = 2;
  while (i + 4 <= len) {
    if (p[i] != 0xFF) {  // junk between segments: skip it
      ++i;
      continue;
    }
    const uint8_t m = p[i + 1];
    if (m == 0xFF) {  // fill bytes
      ++i;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {  // markers without a length
      i += 2;
      continue;
    }
    if (m == 0xD9 || m == 0xDA) return out;  // the end, or scan data before any frame header
    const size_t segLen = static_cast<size_t>(p[i + 2]) << 8 | p[i + 3];
    if (segLen < 2) return out;
    // SOF0-SOF15, except DHT (C4), JPG (C8) and DAC (CC).
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (i + 10 > len) return out;
      out.height = static_cast<uint16_t>(p[i + 5] << 8 | p[i + 6]);
      out.width = static_cast<uint16_t>(p[i + 7] << 8 | p[i + 8]);
      out.components = p[i + 9];
      out.progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
      out.ok = out.width > 0 && out.height > 0;
      return out;
    }
    i += 2 + segLen;
  }
  return out;
}

}  // namespace jpeg
