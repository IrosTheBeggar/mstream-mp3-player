// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HostStatus.h"

#include <cstring>

namespace hoststatus {

namespace {

// The line as it is put together (kMaxLine bytes at most, then its
// terminator).
struct Line {
  char buf[kMaxLine + 1];
  size_t n = 0;
  size_t room() const { return kMaxLine - n; }
  void put(const char* s, size_t len) {
    if (len > room()) len = room();
    std::memcpy(buf + n, s, len);
    n += len;
    buf[n] = '\0';
  }
  void put(const char* s) { put(s, std::strlen(s)); }
  void put(char c) { put(&c, 1); }
  // A decimal, digits only (no printf: %llu isn't on every host).
  void num(uint64_t v) {
    char d[20];
    int k = 0;
    do {
      d[k++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v);
    while (k) put(d[--k]);
  }
  // A token as it is, each byte outside 0x21-0x7E made '_' (as @ok's
  // version), at most `max` bytes; "?" when empty.
  void token(const char* s, size_t max) {
    size_t k = 0;
    for (; s && s[k] && k < max; ++k) {
      const auto c = static_cast<unsigned char>(s[k]);
      put(c > 0x20 && c < 0x7F ? static_cast<char>(c) : '_');
    }
    if (k == 0) put('?');
  }
};

bool unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

// A character's bytes from its first: a UTF-8 lead byte and the
// continuation bytes that follow it (as many as it says, while they are
// 0x80-0xBF); any other byte alone.
size_t charLen(const unsigned char* s, size_t left) {
  const unsigned char b = s[0];
  size_t want = b >= 0xF0 && b <= 0xF7 ? 4 : b >= 0xE0 && b <= 0xEF ? 3 : b >= 0xC0 && b <= 0xDF ? 2 : 1;
  size_t n = 1;
  while (n < want && n < left && (s[n] & 0xC0) == 0x80) ++n;
  return n;
}

}  // namespace

const char* cardName(Card c) {
  switch (c) {
    case Card::None: return "none";
    case Card::Fat32: return "fat32";
    case Card::Fat16: return "fat16";
    case Card::ExFat: return "exfat";
    case Card::Ntfs: return "ntfs";
    case Card::Gpt: return "gpt";
    case Card::Other: return "other";
    case Card::Unreadable: return "unreadable";
  }
  return "other";
}

const char* stateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Paused: return "paused";
    case State::Playing: return "playing";
    case State::Host: return "host";
  }
  return "idle";
}

size_t percentEncode(const char* in, size_t len, char* out, size_t room, size_t* taken) {
  static const char kHex[] = "0123456789ABCDEF";
  const auto* s = reinterpret_cast<const unsigned char*>(in);
  size_t i = 0, n = 0;
  while (i < len) {
    const size_t c = charLen(s + i, len - i);
    size_t need = 0;
    for (size_t k = 0; k < c; ++k) need += unreserved(s[i + k]) ? 1 : 3;
    if (need > room - n) break;
    for (size_t k = 0; k < c; ++k) {
      const unsigned char b = s[i + k];
      if (unreserved(b)) {
        out[n++] = static_cast<char>(b);
      } else {
        out[n++] = '%';
        out[n++] = kHex[b >> 4];
        out[n++] = kHex[b & 0x0F];
      }
    }
    i += c;
  }
  out[n] = '\0';
  if (taken) *taken = i;
  return n;
}

bool validLabel(const char* s) {
  if (!s) return false;
  size_t n = 0;
  for (; s[n]; ++n) {
    const auto c = static_cast<unsigned char>(s[n]);
    if (c < 0x21 || c > 0x7E || n >= kMaxLabel) return false;
  }
  return n > 0;
}

size_t format(const Facts& f, char* out, size_t size) {
  Line l;
  l.put("@status fw=");
  l.token(f.fw, kMaxFw);
  l.put(" elf=");
  l.token(f.elf, 16);
  l.put(" card=");
  l.put(cardName(f.card));
  l.put(" size=");
  if (f.sizeBytes) l.num(f.sizeBytes);
  else l.put('-');
  l.put(" free=");
  if (f.counting) l.put("counting");
  else if (f.freeKnown) l.num(f.freeBytes);
  else l.put('?');
  l.put(" tracks=");
  if (f.tracks == Tracks::Count) l.num(f.trackCount);
  else l.put(f.tracks == Tracks::Building ? "building" : "-");
  l.put(" music=");
  if (f.musicKnown) l.num(f.musicBytes);
  else l.put('?');
  l.put(" bat=");
  if (f.battery < 0) l.put('-');
  else l.num(static_cast<uint64_t>(f.battery > 100 ? 100 : f.battery));
  l.put(" state=");
  l.put(stateName(f.state));
  l.put(" bt=");
  // The headphones' name in what is left: whole characters, and "…" when
  // it doesn't all fit (the fields before it take 219 bytes at most, so it
  // keeps 36 at least: test_host_status).
  const size_t len = f.bt ? std::strlen(f.bt) : 0;
  if (len == 0) {
    l.put('-');
  } else {
    // (Encoded straight into the line: percentEncode() writes its
    // terminator inside the room it is given.)
    size_t taken = 0;
    size_t n = percentEncode(f.bt, len, l.buf + l.n, l.room(), &taken);
    if (taken < len) {
      const size_t e = std::strlen(kEllipsis);
      n = l.room() > e ? percentEncode(f.bt, len, l.buf + l.n, l.room() - e, &taken) : 0;
      l.n += n;
      l.buf[l.n] = '\0';
      l.put(n ? kEllipsis : "-");  // ('-': no room for a character, never with the fields as they are)
    } else {
      l.n += n;
    }
  }
  if (size == 0) return 0;
  const size_t n = l.n < size - 1 ? l.n : size - 1;
  std::memcpy(out, l.buf, n);
  out[n] = '\0';
  return n;
}

bool Progress::due(uint8_t percent, uint32_t nowMs) {
  if (percent > 100) percent = 100;
  if (percent <= last_) return false;
  const bool tenth = percent / kStep > last_ / kStep;
  const bool quiet = nowMs - lastMs_ >= kQuietMs;
  if (!tenth && !quiet) return false;
  last_ = percent;
  lastMs_ = nowMs;
  return true;
}

}  // namespace hoststatus
