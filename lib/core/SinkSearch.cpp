// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "SinkSearch.h"

#include <cctype>
#include <cstring>

namespace sinksearch {

namespace {
bool named(const Setup& s) { return s.name && s.name[0]; }
}  // namespace

bool mayScan(bool remembered, const Setup& s) {
  if (remembered) return false;
  return (named(s) && !s.forgotForGood) || s.bySignal;
}

Verdict judge(const Setup& s, const char* found, int rssi) {
  if (!found) found = "";
  if (named(s) && !s.forgotForGood && containsIgnoreCase(found, s.name)) return Verdict::ByName;
  if (s.bySignal) return rssi >= kMinRssi ? Verdict::BySignal : Verdict::TooFar;
  if (!named(s)) return Verdict::NoName;
  return s.forgotForGood && containsIgnoreCase(found, s.name) ? Verdict::Forgotten : Verdict::OtherName;
}

const char* whyNot(Verdict v) {
  switch (v) {
    case Verdict::ByName:
    case Verdict::BySignal: return "";
    case Verdict::NoName: return "no name to look for: only Output > Pair new headphones pairs";
    case Verdict::OtherName: return "the name doesn't match BT_SINK_NAME";
    case Verdict::Forgotten: return "forgotten on the Output tab: pair them again there";
    case Verdict::TooFar: return "not close enough for Bs (auto-pair by signal)";
  }
  return "?";
}

bool containsIgnoreCase(const char* haystack, const char* needle) {
  if (!haystack || !needle || !needle[0]) return false;
  const size_t n = std::strlen(needle);
  for (const char* h = haystack; *h; ++h) {
    size_t i = 0;
    while (i < n && h[i] &&
           std::tolower(static_cast<unsigned char>(h[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) {
      ++i;
    }
    if (i == n) return true;
  }
  return false;
}

}  // namespace sinksearch
