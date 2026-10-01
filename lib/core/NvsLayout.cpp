// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "NvsLayout.h"

namespace nvslayout {
namespace {

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

// The five words, from `p`.
QueueResume words(const uint8_t* p) {
  QueueResume r;
  r.valid = true;
  r.generation = get32(p);
  r.entry = static_cast<int32_t>(get32(p + 4));
  r.pathHash = get32(p + 8);
  r.positionMs = get32(p + 12);
  r.durationMs = get32(p + 16);
  return r;
}

}  // namespace

SchemaStep schemaStep(bool have, uint16_t stored, uint16_t current) {
  SchemaStep s;
  s.from = have ? stored : current;
  if (!have) {
    s.action = SchemaStep::Action::Write;
  } else if (stored == current) {
    s.action = SchemaStep::Action::None;
  } else if (stored < current) {
    s.action = SchemaStep::Action::Migrate;
  } else {
    s.action = SchemaStep::Action::Newer;
  }
  return s;
}

size_t encodeResume(const QueueResume& r, uint8_t out[kResumeBytes]) {
  out[0] = kResumeVersion;
  out[1] = out[2] = out[3] = 0;
  put32(out + 4, r.generation);
  put32(out + 8, static_cast<uint32_t>(r.entry));
  put32(out + 12, r.pathHash);
  put32(out + 16, r.positionMs);
  put32(out + 20, r.durationMs);
  return kResumeBytes;
}

bool decodeResume(const uint8_t* b, size_t n, QueueResume* out) {
  if (n == kResumeV0Bytes) {  // v0.5.0-beta.1's: the ESP32's struct of five words, as is
    *out = words(b);
    return true;
  }
  if (n == kResumeBytes && b[0] == kResumeVersion) {
    *out = words(b + 4);
    return true;
  }
  return false;
}

}  // namespace nvslayout
