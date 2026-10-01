// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HostLine.h"

#include <cmath>
#include <cstdlib>

HostLine::Byte HostLine::startLine() {
  inLine_ = true;
  len_ = 0;
  bad_ = long_ = false;
  buf_[len_++] = '@';
  return Byte::Start;
}

HostLine::Byte HostLine::push(char c, bool atIsText, uint32_t nowMs) {
  lastMs_ = nowMs;
  if (!inLine_ && sync_) {
    // (No console command waits in Sync: atIsText doesn't apply.)
    const bool after = held_ != 0 || tail_;  // a byte came just before this one
    if (held_) ++dropped_;
    held_ = 0;
    if (c == '@') {  // a line, whatever came before it
      tail_ = false;
      return startLine();
    }
    if (c == '\n' || c == '\r') {  // the end of a tail (or a lone terminator)
      tail_ = false;
      return Byte::Dropped;
    }
    if (after) {  // a byte after another, before any quiet: a line's tail
      tail_ = true;
      ++dropped_;
      return Byte::Dropped;
    }
    held_ = c;
    return Byte::Held;
  }
  if (!inLine_) {
    if (c != '@' || atIsText) return Byte::Console;
    return startLine();
  }
  if (c == '\n' || c == '\r') {
    inLine_ = false;
    buf_[len_] = '\0';
    return long_ ? Byte::Long : bad_ ? Byte::Bad : Byte::Line;
  }
  if (c == '@') {  // the partial line is dropped; a new one starts here
    len_ = 0;
    bad_ = long_ = false;
    buf_[len_++] = '@';
    return Byte::Restart;
  }
  if (len_ >= kMaxLine) {
    long_ = true;  // swallowed up to the terminator
    return Byte::Taken;
  }
  const auto u = static_cast<unsigned char>(c);
  if (u < 0x20 || u > 0x7E) bad_ = true;
  buf_[len_++] = c;
  return Byte::Taken;
}

bool HostLine::quiet(uint32_t nowMs, char* key) {
  if (!sync_ || inLine_ || tail_ || nowMs - lastMs_ < kQuietMs) return false;
  sync_ = false;
  if (!held_) return false;
  *key = held_;
  held_ = 0;
  return true;
}

void HostLine::resync(uint32_t nowMs) {
  if (inLine_) bad_ = true;  // it ends Bad at its terminator
  sync_ = true;
  tail_ = false;
  held_ = 0;
  lastMs_ = nowMs;
  dropped_ = 0;
}

namespace {
bool isLower(char c) { return c >= 'a' && c <= 'z'; }
bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

// [a-z][a-z0-9]* segments joined by single dots, at most kMaxVerb bytes.
bool validVerb(const char* v, size_t len) {
  if (len == 0 || len > HostFields::kMaxVerb) return false;
  bool segmentStart = true;
  for (size_t i = 0; i < len; ++i) {
    const char c = v[i];
    if (c == '.') {
      if (segmentStart) return false;  // ".x", "a..b"
      segmentStart = true;
      continue;
    }
    if (segmentStart ? !isLower(c) : !(isLower(c) || isDigitChar(c))) return false;
    segmentStart = false;
  }
  return !segmentStart;  // not "a."
}
}  // namespace

bool splitHostLine(char* line, HostFields* out) {
  *out = HostFields{};
  if (!line || line[0] != '@') return false;
  char* p = line + 1;
  char* verb = p;
  while (*p && *p != ' ') ++p;
  const size_t verbLen = static_cast<size_t>(p - verb);
  if (!validVerb(verb, verbLen)) return false;
  if (*p) *p++ = '\0';
  out->verb = verb;
  for (;;) {
    while (*p == ' ') ++p;
    if (!*p) break;
    if (out->count == HostFields::kMaxFields) {
      out->more = true;
      break;
    }
    out->field[out->count++] = p;
    while (*p && *p != ' ') ++p;
    if (*p) *p++ = '\0';
  }
  return true;
}

bool parseHostU32(const char* s, uint32_t* out) {
  if (!s || !*s) return false;
  uint64_t v = 0;
  for (const char* p = s; *p; ++p) {
    if (!isDigitChar(*p)) return false;
    v = v * 10 + static_cast<uint64_t>(*p - '0');
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

bool parseHostI32(const char* s, int32_t* out) {
  if (!s || !*s) return false;
  const bool neg = *s == '-';
  const char* p = neg ? s + 1 : s;
  if (!*p) return false;
  int64_t v = 0;
  for (; *p; ++p) {
    if (!isDigitChar(*p)) return false;
    v = v * 10 + (*p - '0');
    if (v > 2147483648LL) return false;
  }
  if (neg) v = -v;
  if (v > 2147483647LL) return false;
  *out = static_cast<int32_t>(v);
  return true;
}

bool parseHostF32(const char* s, float* out) {
  if (!s || !*s) return false;
  // The grammar first: strtof alone would take "nan", "inf", "0x1p3", " 1".
  const char* p = s;
  if (*p == '-') ++p;
  if (!isDigitChar(*p)) return false;
  while (isDigitChar(*p)) ++p;
  if (*p == '.') {
    ++p;
    while (isDigitChar(*p)) ++p;
  }
  if (*p == 'e' || *p == 'E') {
    ++p;
    if (*p == '+' || *p == '-') ++p;
    if (!isDigitChar(*p)) return false;
    while (isDigitChar(*p)) ++p;
  }
  if (*p) return false;
  char* end = nullptr;
  const float v = std::strtof(s, &end);
  if (end != p || !std::isfinite(v)) return false;  // (1e39: inf as a float)
  *out = v;
  return true;
}
