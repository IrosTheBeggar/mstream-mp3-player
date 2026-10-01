// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ToneTrack.h"

namespace {

// 1-6 digits, no sign, no leading zero: its value; 0 otherwise.
uint32_t number(const std::string& s) {
  if (s.empty() || s.size() > 6 || s[0] == '0') return 0;
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return 0;
    v = v * 10 + static_cast<uint32_t>(c - '0');
  }
  return v;
}

}  // namespace

bool ToneTrack::parse(const std::string& path, ToneTrack* out) {
  if (path.rfind("tone:", 0) != 0) return false;
  std::string what = path.substr(5);
  ToneTrack t;
  const size_t at = what.find('@');
  if (at != std::string::npos) {
    t.rate = number(what.substr(at + 1));
    if (t.rate == 0) return false;
    what.resize(at);
  }
  if (ClickGen::parse(what, &t.click)) {
    if (at != std::string::npos) return false;  // clicks are 44.1 kHz only
    t.kind = Kind::Clicks;
    t.seconds = kClickSeconds;
  } else if (what == "silence") {
    t.kind = Kind::Silence;
    t.seconds = kSilenceSeconds;
  } else {
    const uint32_t hz = what == "left" ? 440 : number(what);
    if (hz == 0 || 2 * hz >= t.rate) return false;
    t.kind = Kind::Sine;
    t.hz = static_cast<float>(hz);
    t.leftOnly = what == "left";
    t.seconds = kSineSeconds;
  }
  if (out) *out = t;
  return true;
}
