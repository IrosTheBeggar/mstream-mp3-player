#pragma once
#include <cstddef>
#include <cstdint>

// Standard base64 (RFC 4648, with padding), for dumping screenshots over the
// serial console. Writes 4 * ceil(n / 3) characters plus a terminating 0;
// returns the characters written (without the 0). Not named base64.h: on a
// case-insensitive disk that would shadow the Arduino core's (HTTPClient).
inline size_t base64Encode(const uint8_t* in, size_t n, char* out) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    const uint32_t v = (uint32_t{in[i]} << 16) | (uint32_t{in[i + 1]} << 8) | in[i + 2];
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = kAlphabet[(v >> 6) & 63];
    out[o++] = kAlphabet[v & 63];
  }
  if (i < n) {
    const uint32_t v = (uint32_t{in[i]} << 16) | (i + 1 < n ? uint32_t{in[i + 1]} << 8 : 0);
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

inline constexpr size_t base64Length(size_t n) { return 4 * ((n + 2) / 3); }
