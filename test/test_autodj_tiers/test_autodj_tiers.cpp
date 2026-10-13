// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the Auto DJ engine's preferences and its stream
// (lib/core/AutoDjTiers, AutoDjRng; the engine design's 1.4): PCG32
// against its published outputs and tools/autodj_ref.py's draws
// (test/fixtures/autodj: rng); below(n) as Lemire's method; the tempo
// windows at 1x, half (rounded up, as Dart's round()) and double; the 6
// Camelot codes with the 12/1 wrap; the rank tables for the four on/off
// combinations against a port of mStream's waterfall (random.js: the steps
// for one pick, classifyRow's tiers) cell by cell, and the one cell where
// the design's "any known key is good before an anchor" differs; D3's
// integer odds and its "no known value" case; the gate's level and its
// clamp at selFloor. Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "AutoDjJoin.h"
#include "AutoDjRng.h"
#include "AutoDjTiers.h"

using namespace autodj;
using minijson::Value;

namespace {

bool fixture(Value* out) {
  std::string text;
  return minijson::readFile(cardfixtures::dir() + "/../autodj/sessions.json", &text) && minijson::parse(text, out);
}

// ---------------------------------------------------------------------------
// mStream's waterfall for one pick, as data (the test's port of random.js
// 1005-1066 and classifyRow, 422-450).
// ---------------------------------------------------------------------------
struct Step {
  int bpm;  // 0 no BPM constraint, 1 tight, 2 wide
  bool key;
};

// The first step that admits a row of statuses (t, k), then its tier in
// that step. Before a key anchor the request carries requireMusicalKey and
// no keys: steps with a key admit any known key, and classifyRow sees "na".
std::pair<int, int> waterfall(Tempo t, Key k, bool tempoOn, bool keyOn, bool anchored) {
  std::vector<Step> steps;
  if (tempoOn || keyOn) steps.push_back({tempoOn ? 1 : 0, keyOn});
  if (tempoOn && keyOn) steps.push_back({2, true});
  if (tempoOn && keyOn) steps.push_back({1, false});
  if (tempoOn) steps.push_back({2, false});
  steps.push_back({0, false});
  auto admits = [&](const Step& s) {
    if (s.bpm == 1 && t != Tempo::Tight) return false;
    if (s.bpm == 2 && t != Tempo::Tight && t != Tempo::Wide) return false;
    if (s.key && !(k == Key::Good || (!anchored && k == Key::Wrong))) return false;
    return true;
  };
  // classifyRow: "good" in range, "wrong" out of it, "unknown" null, "na".
  enum S { Na, Good, Unknown, Wrong };
  const S bs = !tempoOn ? Na : t == Tempo::Tight ? Good : t == Tempo::Unknown ? Unknown : Wrong;
  const S ks = !keyOn || !anchored ? Na : k == Key::Good ? Good : k == Key::Unknown ? Unknown : Wrong;
  int tier = 2;
  if ((bs == Good && ks != Wrong) || (ks == Good && bs != Wrong))
    tier = 0;
  else if (bs != Wrong && ks != Wrong)
    tier = 1;
  for (size_t i = 0; i < steps.size(); ++i)
    if (admits(steps[i])) return {static_cast<int>(i), tier};
  return {99, tier};
}

std::vector<std::pair<Tempo, Key>> cells(bool tempoOn, bool keyOn) {
  std::vector<Tempo> ts = tempoOn ? std::vector<Tempo>{Tempo::Tight, Tempo::Wide, Tempo::Wrong, Tempo::Unknown}
                                  : std::vector<Tempo>{Tempo::Na};
  std::vector<Key> ks =
      keyOn ? std::vector<Key>{Key::Good, Key::Wrong, Key::Unknown} : std::vector<Key>{Key::Na};
  std::vector<std::pair<Tempo, Key>> out;
  for (Tempo t : ts)
    for (Key k : ks) out.push_back({t, k});
  return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The stream
// ---------------------------------------------------------------------------
void test_pcg32_reference_outputs() {
  // pcg32-demo's first outputs for pcg32_srandom_r(42, 54).
  Rng r(42, 54);
  const uint32_t want[6] = {0xA15C02B7, 0x7B47F409, 0xBA1D3330, 0x83D2F293, 0xBFA4784B, 0xCBED606E};
  for (uint32_t w : want) TEST_ASSERT_EQUAL_HEX32(w, r.next());
}

void test_the_stream_against_the_reference() {
  Value fx;
  TEST_ASSERT_TRUE_MESSAGE(fixture(&fx), "test/fixtures/autodj/sessions.json");
  const Value& v = fx["rng"];
  Rng r(v["seed"].u64(), v["stream"].u64());
  for (size_t i = 0; i < v["next"].size(); ++i) TEST_ASSERT_EQUAL_UINT32(v["next"][i].u64(), r.next());
  TEST_ASSERT_EQUAL_UINT64(Rng::kStream, v["sessionStream"].u64());
  Rng q(v["belowSeed"].u64());
  size_t cases = 0;
  for (const Value& c : v["below"].items) {
    const uint32_t n = static_cast<uint32_t>(c["n"].u64());
    for (const Value& d : c["draws"].items) TEST_ASSERT_EQUAL_UINT32(d.u64(), q.below(n));
    ++cases;
  }
  TEST_ASSERT_EQUAL(13, cases);
}

void test_below_is_lemire() {
  // The high word of next() x n, unless the low word is under 2^32 mod n.
  Rng a(99), b(99);
  const uint32_t ns[] = {3, 1000, 0x80000001u, 0xFFFFFFFFu};
  for (uint32_t n : ns) {
    for (int i = 0; i < 500; ++i) {
      uint64_t m;
      do {
        m = static_cast<uint64_t>(b.next()) * n;
      } while (static_cast<uint32_t>(m) < (0u - n) % n);
      TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(m >> 32), a.below(n));
    }
  }
}

void test_below_is_in_range_and_takes_every_value() {
  Rng r(7);
  const uint32_t ns[] = {1, 2, 3, 7, 50, 169};
  for (uint32_t n : ns) {
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < 40 * n; ++i) {
      const uint32_t x = r.below(n);
      TEST_ASSERT_LESS_THAN_UINT32(n, x);
      seen.insert(x);
    }
    TEST_ASSERT_EQUAL(n, seen.size());
  }
  // 0 draws nothing.
  Rng c(7), d(7);
  TEST_ASSERT_EQUAL_UINT32(0, c.below(0));
  TEST_ASSERT_EQUAL_UINT32(d.next(), c.next());
}

void test_seeds_and_streams() {
  Rng a(5), b(5), c(6), d(5, 1);
  const uint32_t x = a.next();
  TEST_ASSERT_EQUAL_UINT32(x, b.next());
  TEST_ASSERT_NOT_EQUAL(x, c.next());
  TEST_ASSERT_NOT_EQUAL(x, d.next());
  a.reseed(5);
  TEST_ASSERT_EQUAL_UINT32(x, a.next());
}

// ---------------------------------------------------------------------------
// Tempo
// ---------------------------------------------------------------------------
void test_windows_round_half_up() {
  Window w[3];
  windows(121, 8, w);  // half of 121 is 61 (60.5 up)
  TEST_ASSERT_EQUAL(113, w[0].lo);
  TEST_ASSERT_EQUAL(129, w[0].hi);
  TEST_ASSERT_EQUAL(53, w[1].lo);
  TEST_ASSERT_EQUAL(69, w[1].hi);
  TEST_ASSERT_EQUAL(234, w[2].lo);
  TEST_ASSERT_EQUAL(250, w[2].hi);
  windows(120, 10, w);
  TEST_ASSERT_EQUAL(50, w[1].lo);
  TEST_ASSERT_EQUAL(70, w[1].hi);
  windows(1, 8, w);
  TEST_ASSERT_EQUAL(-7, w[1].lo);
  TEST_ASSERT_EQUAL(9, w[1].hi);
}

void test_tempo_statuses_at_the_edges() {
  struct Case {
    uint8_t bpm;
    Tempo want;
  };
  const Case cases[] = {
      {113, Tempo::Tight}, {129, Tempo::Tight}, {112, Tempo::Wide},  {111, Tempo::Wide},  {110, Tempo::Wrong},
      {131, Tempo::Wide},  {132, Tempo::Wrong}, {61, Tempo::Tight},  {69, Tempo::Tight},  {70, Tempo::Wide},
      {72, Tempo::Wrong},  {242, Tempo::Tight}, {252, Tempo::Wide},  {253, Tempo::Wrong}, {0, Tempo::Unknown},
      {255, Tempo::Wrong}, {51, Tempo::Wide},   {50, Tempo::Wrong},  {53, Tempo::Tight},  {234, Tempo::Tight},
  };
  for (const Case& c : cases)
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.want), static_cast<int>(tempoStatus(c.bpm, 121)),
                              std::to_string(c.bpm).c_str());
  TEST_ASSERT_EQUAL(static_cast<int>(Tempo::Na), static_cast<int>(tempoStatus(120, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Tempo::Na), static_cast<int>(tempoStatus(0, 0)));
  // A saturated reference: double is 510, out of a u8's reach; half still works.
  TEST_ASSERT_EQUAL(static_cast<int>(Tempo::Tight), static_cast<int>(tempoStatus(128, 255)));
  TEST_ASSERT_EQUAL(static_cast<int>(Tempo::Tight), static_cast<int>(tempoStatus(247, 255)));
}

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------
void test_camelot_six_codes_with_the_wrap() {
  auto near = [](uint8_t a) {
    std::vector<int> out;
    for (uint8_t c = 1; c <= 24; ++c)
      if (camelotNear(a, c)) out.push_back(c);
    return out;
  };
  TEST_ASSERT_TRUE((near(1) == std::vector<int>{1, 2, 12, 13, 14, 24}));    // 1A: 12A 1A 2A, 12B 1B 2B
  TEST_ASSERT_TRUE((near(12) == std::vector<int>{1, 11, 12, 13, 23, 24}));  // 12A
  TEST_ASSERT_TRUE((near(13) == near(1)));                                  // 1B
  TEST_ASSERT_TRUE((near(20) == std::vector<int>{7, 8, 9, 19, 20, 21}));    // 8B
  for (uint8_t a = 1; a <= 24; ++a) {
    TEST_ASSERT_EQUAL(6, near(a).size());
    for (uint8_t b = 1; b <= 24; ++b) TEST_ASSERT_EQUAL(camelotNear(a, b), camelotNear(b, a));
  }
}

void test_key_statuses() {
  TEST_ASSERT_EQUAL(static_cast<int>(Key::Na), static_cast<int>(keyStatus(5, 1, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(Key::Unknown), static_cast<int>(keyStatus(0, 1, true)));
  TEST_ASSERT_EQUAL(static_cast<int>(Key::Good), static_cast<int>(keyStatus(5, 0, true)));  // no anchor yet
  TEST_ASSERT_EQUAL(static_cast<int>(Key::Good), static_cast<int>(keyStatus(2, 1, true)));
  TEST_ASSERT_EQUAL(static_cast<int>(Key::Good), static_cast<int>(keyStatus(24, 1, true)));
  TEST_ASSERT_EQUAL(static_cast<int>(Key::Wrong), static_cast<int>(keyStatus(3, 1, true)));
}

// ---------------------------------------------------------------------------
// The ranks
// ---------------------------------------------------------------------------
void test_ranks_follow_the_waterfall_cell_by_cell() {
  for (int on = 0; on < 4; ++on) {
    const bool tempoOn = on & 1, keyOn = on & 2;
    std::set<std::pair<int, int>> seen;
    for (const auto& c : cells(tempoOn, keyOn)) seen.insert(waterfall(c.first, c.second, tempoOn, keyOn, true));
    const std::vector<std::pair<int, int>> order(seen.begin(), seen.end());
    for (const auto& c : cells(tempoOn, keyOn)) {
      const auto at = waterfall(c.first, c.second, tempoOn, keyOn, true);
      int want = 0;
      while (order[want] != at) ++want;
      char msg[64];
      std::snprintf(msg, sizeof msg, "tempo %d key %d: t %d k %d", tempoOn, keyOn, static_cast<int>(c.first),
                    static_cast<int>(c.second));
      TEST_ASSERT_EQUAL_MESSAGE(want, musicRank(c.first, c.second), msg);
    }
  }
}

void test_ranks_before_a_key_anchor() {
  // Before an anchor every known key is Good (the design's 1.4.1); against
  // mStream's order only an unknown tempo with a known key differs: 5 here,
  // and in mStream the same as both unknown (6).
  for (const auto& c : cells(true, true)) {
    if (c.second == Key::Wrong) continue;
    const auto seen = waterfall(c.first, c.second, true, true, false);
    if (c.first == Tempo::Unknown && c.second == Key::Good) {
      TEST_ASSERT_TRUE(seen == waterfall(Tempo::Unknown, Key::Unknown, true, true, false));
      TEST_ASSERT_EQUAL(5, musicRank(c.first, c.second));
      TEST_ASSERT_EQUAL(6, musicRank(Tempo::Unknown, Key::Unknown));
    } else {
      TEST_ASSERT_TRUE(seen == waterfall(c.first, c.second, true, true, true));
    }
  }
}

void test_the_design_tables() {
  const Tempo T[] = {Tempo::Tight, Tempo::Wide, Tempo::Wrong, Tempo::Unknown};
  const Key K[] = {Key::Good, Key::Wrong, Key::Unknown};
  const uint8_t both[4][3] = {{0, 3, 2}, {1, 4, 4}, {7, 7, 7}, {5, 7, 6}};
  for (int t = 0; t < 4; ++t)
    for (int k = 0; k < 3; ++k) TEST_ASSERT_EQUAL(both[t][k], musicRank(T[t], K[k]));
  const uint8_t tempo[4] = {0, 1, 3, 2};
  for (int t = 0; t < 4; ++t) TEST_ASSERT_EQUAL(tempo[t], musicRank(T[t], Key::Na));
  const uint8_t key[3] = {0, 2, 1};
  for (int k = 0; k < 3; ++k) TEST_ASSERT_EQUAL(key[k], musicRank(Tempo::Na, K[k]));
  TEST_ASSERT_EQUAL(0, musicRank(Tempo::Na, Key::Na));
}

// ---------------------------------------------------------------------------
// D3
// ---------------------------------------------------------------------------
void test_imputation_odds_in_integers() {
  // nT 2, nW 1, nX 1: below(4) 0-1 Tight, 2 Wide, 3 Wrong.
  TempoOdds to;
  const Tempo known[] = {Tempo::Tight, Tempo::Unknown, Tempo::Wide, Tempo::Wrong, Tempo::Tight, Tempo::Na};
  for (Tempo t : known) to.add(t);
  TEST_ASSERT_EQUAL(2, to.tight);
  TEST_ASSERT_EQUAL(1, to.wide);
  TEST_ASSERT_EQUAL(1, to.wrong);
  TEST_ASSERT_EQUAL(4, to.known());
  KeyOdds ko;
  const Key keys[] = {Key::Good, Key::Good, Key::Good, Key::Wrong, Key::Unknown, Key::Na};
  for (Key k : keys) ko.add(k);
  TEST_ASSERT_EQUAL(4, ko.known());
  Rng a(31), b(31);
  int tally[4] = {};
  for (int i = 0; i < 4000; ++i) {
    const uint32_t u = b.below(4);
    const Tempo want = u < 2 ? Tempo::Tight : u < 3 ? Tempo::Wide : Tempo::Wrong;
    const Tempo got = to.draw(a);
    TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(got));
    ++tally[static_cast<int>(got)];
    const uint32_t v = b.below(4);
    TEST_ASSERT_EQUAL(static_cast<int>(v < 3 ? Key::Good : Key::Wrong), static_cast<int>(ko.draw(a)));
  }
  // About 2 : 1 : 1.
  TEST_ASSERT_INT_WITHIN(150, 2000, tally[static_cast<int>(Tempo::Tight)]);
  TEST_ASSERT_INT_WITHIN(150, 1000, tally[static_cast<int>(Tempo::Wide)]);
  TEST_ASSERT_INT_WITHIN(150, 1000, tally[static_cast<int>(Tempo::Wrong)]);
}

void test_no_known_value() {
  TempoOdds to;
  to.add(Tempo::Unknown);
  to.add(Tempo::Na);
  TEST_ASSERT_EQUAL(0, to.known());
  KeyOdds ko;
  ko.add(Key::Unknown);
  TEST_ASSERT_EQUAL(0, ko.known());
  // Unknown then ranks as mStream ranks it: between the matches and the misses.
  TEST_ASSERT_EQUAL(2, musicRank(Tempo::Unknown, Key::Na));
  TEST_ASSERT_EQUAL(1, musicRank(Tempo::Na, Key::Unknown));
  TEST_ASSERT_EQUAL(6, musicRank(Tempo::Unknown, Key::Unknown));
}

// ---------------------------------------------------------------------------
// The gate's level
// ---------------------------------------------------------------------------
void test_floor_levels_and_the_clamp() {
  const uint8_t steps[5] = {141, 153, 166, 179, 192};
  for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL(steps[i], floorLevel(55 + 5 * i, 0));
  TEST_ASSERT_EQUAL(140, floorLevel(30, 140));
  TEST_ASSERT_EQUAL(160, floorLevel(55, 160));
  TEST_ASSERT_EQUAL(255, floorLevel(150, 0));
  TEST_ASSERT_EQUAL(255, floorLevel(0, 400));
  Value fx;
  TEST_ASSERT_TRUE(fixture(&fx));
  size_t n = 0;
  for (const Value& c : fx["floors"].items) {
    TEST_ASSERT_EQUAL(c["level"].u64(), floorLevel(static_cast<uint32_t>(c["percent"].u64()),
                                                   static_cast<uint32_t>(c["selFloor"].u64())));
    ++n;
  }
  TEST_ASSERT_EQUAL(32, n);
}

void test_the_filter_word() {
  // The join's word (AutoDjJoin.h), as the tiers read it.
  const uint32_t f = filterWord(1205, 7, cardcontract::mpdj::kFilterBpmAnalysed, 0xB460A770);
  TEST_ASSERT_EQUAL(121, filterBpm(f));
  TEST_ASSERT_EQUAL(7, filterCamelot(f));
  TEST_ASSERT_EQUAL_HEX16(0xA770, filterArtist(f));
  TEST_ASSERT_TRUE(filterAnalysed(f));
  TEST_ASSERT_EQUAL(120, filterBpm(filterWord(1204, 0, 0, 0)));
  TEST_ASSERT_EQUAL(255, filterBpm(filterWord(9999, 0, 0, 0)));
  TEST_ASSERT_EQUAL(0, filterCamelot(filterWord(0, 25, 0, 0)));
  TEST_ASSERT_EQUAL(1, filterArtist(filterWord(0, 0, 0, 0x12340000)));
  TEST_ASSERT_EQUAL(0, filterArtist(filterWord(0, 0, 0, 0)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pcg32_reference_outputs);
  RUN_TEST(test_the_stream_against_the_reference);
  RUN_TEST(test_below_is_lemire);
  RUN_TEST(test_below_is_in_range_and_takes_every_value);
  RUN_TEST(test_seeds_and_streams);
  RUN_TEST(test_windows_round_half_up);
  RUN_TEST(test_tempo_statuses_at_the_edges);
  RUN_TEST(test_camelot_six_codes_with_the_wrap);
  RUN_TEST(test_key_statuses);
  RUN_TEST(test_ranks_follow_the_waterfall_cell_by_cell);
  RUN_TEST(test_ranks_before_a_key_anchor);
  RUN_TEST(test_the_design_tables);
  RUN_TEST(test_imputation_odds_in_integers);
  RUN_TEST(test_no_known_value);
  RUN_TEST(test_floor_levels_and_the_clamp);
  RUN_TEST(test_the_filter_word);
  return UNITY_END();
}
