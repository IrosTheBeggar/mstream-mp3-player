// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ResumeAnchor.h"

#include <cstdio>

#include "TrackProgress.h"

namespace resumeanchor {

uint32_t frameHash(const uint8_t* frame, size_t avail) {
  size_t n = kHashBytes;
  progress::Mp3Frame f;
  if (avail >= 4 && progress::parseMp3Frame(frame, &f) && static_cast<size_t>(f.length) < n) {
    n = static_cast<size_t>(f.length);
  }
  if (n > avail) n = avail;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ frame[i]) * 16777619u;
  return h;
}

uint32_t ms(const ResumeAnchor& a) {
  if (a.rate == 0) return 0;
  return static_cast<uint32_t>(a.sample * 1000 / a.rate);
}

void describe(const ResumeAnchor& a, char* buf, size_t size) {
  switch (a.kind) {
    case ResumeAnchor::Kind::Mp3:
      snprintf(buf, size, "MP3 frame at %lu + %lu samples, preroll %lu, %s", (unsigned long)a.frameByte,
               (unsigned long)a.skip, (unsigned long)a.prerollByte, a.exact ? "exact" : "as a TOC start showed it");
      return;
    case ResumeAnchor::Kind::Flac:
      snprintf(buf, size, "FLAC sample %llu", (unsigned long long)a.sample);
      return;
    case ResumeAnchor::Kind::Opus:
      snprintf(buf, size, "Opus sample %llu", (unsigned long long)a.sample);
      return;
    case ResumeAnchor::Kind::None:
      break;
  }
  snprintf(buf, size, "none");
}

}  // namespace resumeanchor
