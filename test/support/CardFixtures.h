// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Where the card contract's shared fixtures are (test/fixtures/card), and
// reading them: `pio test` runs a test from the project's root; a test run
// from elsewhere finds the folder from this file's own path.
#include <cstdio>
#include <string>

#include "MiniJson.h"

namespace cardfixtures {

inline bool exists(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

inline const std::string& dir() {
  static const std::string d = [] {
    const char* tries[] = {"test/fixtures/card", "../test/fixtures/card", "../../test/fixtures/card"};
    for (const char* t : tries)
      if (exists(std::string(t) + "/vectors.json")) return std::string(t);
    std::string self = __FILE__;  // .../test/support/CardFixtures.h
    for (int up = 0; up < 2; ++up) {
      const size_t k = self.find_last_of("/\\");
      if (k == std::string::npos) break;
      self = self.substr(0, k);
    }
    return self + "/fixtures/card";
  }();
  return d;
}

// A fixture's bytes ("golden/manifest.bin"); empty when it can't be read.
inline std::string bytes(const std::string& rel) {
  std::string out;
  minijson::readFile(dir() + "/" + rel, &out);
  return out;
}

inline bool json(const std::string& rel, minijson::Value* out) {
  std::string text;
  return minijson::readFile(dir() + "/" + rel, &text) && minijson::parse(text, out);
}

}  // namespace cardfixtures
