// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for idsort (lib/core/IdSort.h; docs/METADATA.md 3.9, the
// 2026-10-09 device run): the index views' sort.
// - Equal to std::sort, id for id, on every shape (random with ties,
//   sorted, reversed, organ pipe, sorted with a tail of small keys, all
//   equal, interleaved runs) at sizes from 0 to 65,535, with a comparator
//   that breaks ties by id (as every view's does).
// - Its recursion: at most lg(n / 16) + 1 frames on every shape (std::sort's
//   libstdc++ introsort goes to 2 lg n on the sorted-with-a-tail shape).
// - Its compares near n lg n, the killer shape included.
// - The device's own shape: names in the records' byte order (the
//   canonical order, 2.6.8) sorted by textfold's (case and accents folded),
//   lowercase and accented first letters after 'Z' in bytes: the result is
//   std::sort's and the recursion bounded.
// Run: pio test -e native -f test_id_sort
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "IdSort.h"
#include "TextFold.h"

namespace {

int boundFor(size_t n) {
  // lg(n / 16) + 1, rounded up: the parts halve at least once a frame.
  int b = 1;
  while (n > 16 && (static_cast<size_t>(16) << (b - 1)) < n) ++b;
  return b;
}

std::vector<uint32_t> keysOf(int shape, uint32_t n, std::mt19937& rng) {
  std::vector<uint32_t> key(n);
  for (uint32_t i = 0; i < n; ++i) {
    switch (shape) {
      case 0: key[i] = rng() % (n + 1); break;                              // random, with ties
      case 1: key[i] = i; break;                                            // sorted
      case 2: key[i] = n - i; break;                                        // reversed
      case 3: key[i] = i < n / 2 ? i : n - i; break;                        // organ pipe
      case 4: key[i] = i < n - n / 20 ? i + n : i - (n - n / 20); break;   // sorted, then a tail of small keys
      case 5: key[i] = 7; break;                                            // all equal (the ids break ties)
      default: key[i] = (i % 23) * 1000 + i / 23; break;                   // interleaved runs
    }
  }
  return key;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_equal_to_std_sort_on_every_shape() {
  std::mt19937 rng(7);
  const uint32_t sizes[] = {0, 1, 2, 3, 15, 16, 17, 100, 128, 129, 1000, 4096, 20000, 65535};
  for (uint32_t n : sizes) {
    for (int shape = 0; shape < 7; ++shape) {
      const std::vector<uint32_t> key = keysOf(shape, n, rng);
      std::vector<uint32_t> a(n), b(n);
      for (uint32_t i = 0; i < n; ++i) a[i] = b[i] = i;
      uint64_t compares = 0;
      auto less = [&](uint32_t x, uint32_t y) {
        ++compares;
        return key[x] != key[y] ? key[x] < key[y] : x < y;
      };
      idsort::Probe probe;
      idsort::sort(a.data(), a.data() + n, less, &probe);
      const uint64_t mine = compares;
      std::sort(b.data(), b.data() + n, less);
      char what[64];
      snprintf(what, sizeof(what), "n %u, shape %d", n, shape);
      TEST_ASSERT_TRUE_MESSAGE(a == b, what);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, probe.frames, what);  // every frame left
      TEST_ASSERT_TRUE_MESSAGE(probe.most <= boundFor(n), what);
      // Near n lg n (the insertion pass and the pivots on top), every shape.
      if (n >= 1000) {
        const double nlgn = n * std::log2(static_cast<double>(n));
        TEST_ASSERT_TRUE_MESSAGE(static_cast<double>(mine) <= 1.5 * nlgn, what);
        if (shape == 4 || n == 20000)
          printf("[idsort] %s: %d frames (bound %d), %llu compares (n lg n %.0f)\n", what, probe.most, boundFor(n),
                 (unsigned long long)mine, nlgn);
      }
    }
  }
}

// The views' killer, as the device met it: the canonical order's bytes
// against textfold's order.
void test_names_in_byte_order_sorted_by_textfold() {
  // 700 artist folders, A-Z and then the ones a byte order puts after 'Z':
  // a lowercase first letter ("dZihan"), an accented one ("Émilie").
  std::vector<std::string> artists;
  for (int i = 0; i < 665; ++i) {
    std::string s(1, static_cast<char>('A' + i % 26));
    s += "rtist " + std::to_string(i);
    artists.push_back(s);
  }
  for (int i = 0; i < 33; ++i) artists.push_back(std::string(1, static_cast<char>('a' + i % 26)) + "rtist low " +
                                                std::to_string(i));
  artists.push_back("\xC3\x89milie");     // É
  artists.push_back("\xC3\x96stra Lag");  // Ö
  // About 28 tracks an artist, each its path "Artist/Album/NN - Title".
  std::vector<std::string> paths;
  for (size_t a = 0; a < artists.size(); ++a)
    for (int t = 0; t < 28; ++t) paths.push_back(artists[a] + "/Album " + std::to_string(t / 12) + "/" +
                                                 std::to_string(10 + t) + " - Song");
  // The records' order: the bytes.
  std::sort(paths.begin(), paths.end());
  const uint32_t n = static_cast<uint32_t>(paths.size());
  std::vector<uint32_t> a(n), b(n);
  for (uint32_t i = 0; i < n; ++i) a[i] = b[i] = i;
  auto less = [&](uint32_t x, uint32_t y) {
    const int c = textfold::compare(paths[x].c_str(), paths[y].c_str());
    return c != 0 ? c < 0 : x < y;
  };
  idsort::Probe probe;
  idsort::sort(a.data(), a.data() + n, less, &probe);
  std::sort(b.data(), b.data() + n, less);
  TEST_ASSERT_TRUE(a == b);
  TEST_ASSERT_TRUE(probe.most <= boundFor(n));
  printf("[idsort] %u paths in byte order by textfold: %d frames (bound %d)\n", n, probe.most, boundFor(n));
  // And it is the textfold order: "dZihan"-like names among the D's.
  for (uint32_t i = 1; i < n; ++i) TEST_ASSERT_TRUE(textfold::compare(paths[a[i - 1]].c_str(), paths[a[i]].c_str()) <= 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_equal_to_std_sort_on_every_shape);
  RUN_TEST(test_names_in_byte_order_sorted_by_textfold);
  return UNITY_END();
}
