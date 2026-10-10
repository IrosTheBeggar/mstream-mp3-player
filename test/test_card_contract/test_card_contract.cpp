// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the card contract's conventions (lib/core/
// CardContract, docs/METADATA.md 2.3, 2.6.5, 2.6.8, 2.14.1, 2.15, 5.3):
// every vector of 2.18, read from the shared fixture
// test/fixtures/card/vectors.json (tools/card_fixtures.py checks the same
// file with its own implementation), and the rules around them: CRC-32 and
// its combination, the path hash, qfp, the FAT time and the skew, the
// number rule, the text decoders and 2.3.6's limits, the string run, the
// canonical order, the album folder, the root election, the selection
// signature and device.txt. The binary formats are test_card_files'.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "ThumbCache.h"

namespace cc = cardcontract;
using minijson::Value;

namespace {

Value gVectors;

uint64_t hexOf(const Value& v) { return v.u64(); }

std::vector<uint8_t> hexBytes(const std::string& s) {
  std::vector<uint8_t> out;
  int nib = -1;
  for (char c : s) {
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    if (d < 0) continue;
    if (nib < 0) {
      nib = d;
    } else {
      out.push_back(static_cast<uint8_t>(nib << 4 | d));
      nib = -1;
    }
  }
  return out;
}

std::vector<uint8_t> pattern(size_t n) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(i & 0xFF);
  return b;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_fixture_loads() {
  TEST_ASSERT_TRUE_MESSAGE(cardfixtures::json("vectors.json", &gVectors), cardfixtures::dir().c_str());
  TEST_ASSERT_TRUE(gVectors["fnv1a64"].size() >= 10);
}

void test_crc32_and_its_combination() {
  const Value& v = gVectors["crc32"][0];
  TEST_ASSERT_EQUAL_HEX32(hexOf(v["crc"]), cc::crc32(v["ascii"].c_str(), v["ascii"].str().size()));
  TEST_ASSERT_EQUAL_HEX32(0, cc::crc32("", 0));
  // Chained like zlib: a then b is the CRC of the two.
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, cc::crc32(s + 4, 5, cc::crc32(s, 4)));
  // Combined from the parts' CRCs (the MPTG walker's two STRS runs).
  std::vector<uint8_t> big = pattern(10000);
  const uint32_t whole = cc::crc32(big.data(), big.size());
  for (size_t cut : {size_t(0), size_t(1), size_t(7), size_t(4096), size_t(9999), size_t(10000)}) {
    const uint32_t a = cc::crc32(big.data(), cut), b = cc::crc32(big.data() + cut, big.size() - cut);
    TEST_ASSERT_EQUAL_HEX32(whole, cc::crc32Combine(a, b, big.size() - cut));
  }
}

void test_fnv_and_the_path_hash() {
  const Value& v = gVectors["fnv1a64"];
  for (size_t i = 0; i < v.size(); ++i) {
    const std::string& s = v[i]["utf8"].str();
    TEST_ASSERT_TRUE_MESSAGE(cc::fnv1a64(s.data(), s.size()) == hexOf(v[i]["hash"]), s.c_str());
    TEST_ASSERT_TRUE(cc::fnv1a64Str(s.c_str()) == hexOf(v[i]["hash"]));
    // The device's thumbnails hash the same way.
    TEST_ASSERT_TRUE(thumbfile::pathHash(s.c_str()) == hexOf(v[i]["hash"]));
  }
  const Value& p = gVectors["pathHash"];
  for (size_t i = 0; i < p.size(); ++i)
    TEST_ASSERT_TRUE_MESSAGE(cc::pathHash(p[i]["rel"].c_str()) == hexOf(p[i]["hash"]), p[i]["rel"].c_str());
  TEST_ASSERT_TRUE(cc::pathHash("") == cc::kMusicHash);
  TEST_ASSERT_TRUE(cc::pathHash(nullptr) == cc::kMusicHash);
  // A path's hash continues from its folder's: "/" and the name.
  const uint64_t album = cc::pathHash("Artist/Album");
  TEST_ASSERT_TRUE(cc::pathHash("Artist/Album/01 - Title.mp3") ==
                   cc::fnv1a64Str("01 - Title.mp3", cc::fnv1a64("/", 1, album)));
}

void test_server_url_key() {
  const Value& v = gVectors["serverUrlKey"];
  for (size_t i = 0; i < v.size(); ++i) {
    char buf[256];
    TEST_ASSERT_TRUE(cc::normaliseServerUrl(v[i]["url"].c_str(), buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_STRING(v[i]["normalised"].c_str(), buf);
    TEST_ASSERT_TRUE(cc::serverUrlKey(v[i]["url"].c_str()) == hexOf(v[i]["key"]));
  }
  char buf[64];
  TEST_ASSERT_EQUAL_size_t(0, cc::normaliseServerUrl("no scheme", buf, sizeof(buf)));
  TEST_ASSERT_TRUE(cc::serverUrlKey("music.example") == 0);  // unknown
  TEST_ASSERT_TRUE(cc::normaliseServerUrl("http://[::1]:8080/", buf, sizeof(buf)) > 0);
  TEST_ASSERT_EQUAL_STRING("http://[::1]:8080", buf);
  TEST_ASSERT_TRUE(cc::normaliseServerUrl("https://h:0443", buf, sizeof(buf)) > 0);
  TEST_ASSERT_EQUAL_STRING("https://h", buf);
  TEST_ASSERT_TRUE(cc::normaliseServerUrl("http://h:", buf, sizeof(buf)) > 0);
  TEST_ASSERT_EQUAL_STRING("http://h", buf);
  TEST_ASSERT_EQUAL_size_t(0, cc::normaliseServerUrl("http://h:8x/", buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_size_t(0, cc::normaliseServerUrl("http://music.example:3000/", buf, 8));  // too small
}

void test_qfp() {
  const Value& v = gVectors["qfp"];
  for (size_t i = 0; i < v.size(); ++i) {
    const uint32_t size = static_cast<uint32_t>(v[i]["size"].u64());
    const std::vector<uint8_t> file = pattern(size);
    const cc::QfpRanges r = cc::qfpRanges(size);
    TEST_ASSERT_TRUE(r.headBytes <= 4096 && r.tailBytes <= 4096);
    TEST_ASSERT_TRUE(r.tailBytes == 0 || r.tailOffset >= r.headBytes);  // never overlap
    TEST_ASSERT_EQUAL_UINT32(size, r.tailOffset + r.tailBytes);
    const uint64_t q = cc::qfp(size, file.data(), file.data() + r.tailOffset);
    TEST_ASSERT_TRUE_MESSAGE(q == hexOf(v[i]["qfp"]), std::to_string(size).c_str());
  }
  const cc::QfpRanges r = cc::qfpRanges(8193);
  TEST_ASSERT_EQUAL_UINT32(4097, r.tailOffset);  // byte 4096 is between them, unread
}

void test_fat_time() {
  const Value& v = gVectors["fatTime"];
  for (size_t i = 0; i < v.size(); ++i) {
    int y, mo, d, h, mi, s;
    TEST_ASSERT_EQUAL_INT(6, std::sscanf(v[i]["time"].c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s));
    TEST_ASSERT_EQUAL_HEX32(hexOf(v[i]["fatTime"]), cc::fatTime(y, mo, d, h, mi, s));
  }
  const Value& bad = gVectors["fatTimeInvalid"];
  for (size_t i = 0; i < bad.size(); ++i) {
    TEST_ASSERT_FALSE(cc::fatTimeValid(static_cast<uint32_t>(hexOf(bad[i]))));
    TEST_ASSERT_EQUAL_INT64(-1, cc::fatWallSeconds(static_cast<uint32_t>(hexOf(bad[i]))));
  }
  const Value& w = gVectors["wallDelta"][0];
  TEST_ASSERT_EQUAL_INT64(w["delta"].i64(), cc::fatWallSeconds(static_cast<uint32_t>(hexOf(w["b"]))) -
                                                cc::fatWallSeconds(static_cast<uint32_t>(hexOf(w["a"]))));
  // The invalid fields one by one, and the dates FAT can hold.
  TEST_ASSERT_EQUAL_HEX32(0, cc::fatTime(1979, 12, 31, 0, 0, 0));
  TEST_ASSERT_EQUAL_HEX32(0, cc::fatTime(2108, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_HEX32(0, cc::fatTime(2023, 2, 29, 0, 0, 0));
  TEST_ASSERT_TRUE(cc::fatTime(2024, 2, 29, 0, 0, 0) != 0);
  TEST_ASSERT_EQUAL_HEX32(0, cc::fatTime(2100, 2, 29, 0, 0, 0));  // not a leap year
  const uint32_t t = cc::fatTime(2026, 10, 7, 14, 30, 42);
  TEST_ASSERT_FALSE(cc::fatTimeValid((t & 0xFFFFFFE0u) | 30));                 // sec2 30
  TEST_ASSERT_FALSE(cc::fatTimeValid((t & ~(63u << 5)) | (60u << 5)));         // minute 60
  TEST_ASSERT_FALSE(cc::fatTimeValid((t & ~(31u << 11)) | (24u << 11)));       // hour 24
  TEST_ASSERT_FALSE(cc::fatTimeValid((t & ~(31u << 16)) | (0u << 16)));        // day 0
  TEST_ASSERT_FALSE(cc::fatTimeValid((t & ~(15u << 21)) | (13u << 21)));       // month 13
  // W round-trips, and is even.
  for (uint32_t s : {cc::fatTime(1980, 1, 1, 0, 0, 0), t, cc::fatTime(2107, 12, 31, 23, 59, 58),
                     cc::fatTime(2000, 2, 29, 12, 0, 1)}) {
    const int64_t wall = cc::fatWallSeconds(s);
    TEST_ASSERT_TRUE(wall >= 0 && wall % 2 == 0);
    TEST_ASSERT_EQUAL_HEX32(s, cc::fatTimeFromWall(wall));
  }
  TEST_ASSERT_EQUAL_INT64(0, cc::fatWallSeconds(cc::fatTime(1980, 1, 1, 0, 0, 0)));
}

void test_skew_rule() {
  const Value& v = gVectors["skew"];
  const int64_t base = cc::fatWallSeconds(cc::fatTime(2026, 10, 7, 14, 30, 42));
  for (size_t i = 0; i < v.size(); ++i) {
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    const Value& groups = v[i]["groups"];
    for (size_t g = 0; g < groups.size(); ++g)
      for (uint64_t k = 0; k < groups[g]["count"].u64(); ++k) {
        uint32_t rec = cc::fatTimeFromWall(base + 2 * 86400 * static_cast<int64_t>(pairs.size()));
        const int64_t delta = groups[g].has("delta") ? groups[g]["delta"].i64() : 0;
        uint32_t obs = cc::fatTimeFromWall(cc::fatWallSeconds(rec) + delta);
        // A group may pin either stamp: a 0, or an invalid one.
        if (groups[g].has("recorded")) rec = static_cast<uint32_t>(hexOf(groups[g]["recorded"]));
        if (groups[g].has("observed")) obs = static_cast<uint32_t>(hexOf(groups[g]["observed"]));
        pairs.emplace_back(rec, obs);
      }
    cc::SkewHistogram h;
    uint32_t counted = 0;
    for (auto& p : pairs) {
      // Left out (false) exactly when a stamp is 0 or invalid.
      const bool valid = cc::fatTimeValid(p.first) && cc::fatTimeValid(p.second);
      TEST_ASSERT_EQUAL(valid, h.add(p.first, p.second));
      counted += valid ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(counted, h.pairs());
    TEST_ASSERT_FALSE(h.needsRecount());
    TEST_ASSERT_EQUAL_INT32(v[i]["skew"].i64(), h.skew());
    int matching = 0;
    for (auto& p : pairs) matching += cc::timeMatches(p.first, p.second, h.skew()) ? 1 : 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(v[i]["matching"].i64()), matching);
  }
  // A 0 or an invalid stamp is left out, and never matches.
  cc::SkewHistogram h;
  TEST_ASSERT_FALSE(h.add(0, 0x5D4773D5u));
  TEST_ASSERT_FALSE(h.add(0x5C0773D5u, 0x5D4773D5u));
  TEST_ASSERT_EQUAL_UINT32(0, h.pairs());
  TEST_ASSERT_FALSE(cc::timeMatches(0, 0, 0));
  TEST_ASSERT_FALSE(cc::timeMatches(0x5C0773D5u, 0x5C0773D5u, 0));
  TEST_ASSERT_TRUE(cc::timeMatches(0x5D4773D5u, 0x5D4773D5u, 0));
  // Three shifted files don't make a skew (2.17, item 5); 8 of 8 at a
  // quarter hour do; 9 at +86,400 do, 9 at +87,300 don't (over a day).
  cc::SkewHistogram three, eight, day, over;
  const uint32_t t = cc::fatTime(2026, 3, 29, 1, 15, 0);
  for (int k = 0; k < 3; ++k) three.add(t, cc::fatTimeFromWall(cc::fatWallSeconds(t) + 3600));
  for (int k = 0; k < 20; ++k) three.add(t, t);
  TEST_ASSERT_EQUAL_INT32(0, three.skew());
  for (int k = 0; k < 8; ++k) eight.add(t, cc::fatTimeFromWall(cc::fatWallSeconds(t) - 900));
  TEST_ASSERT_EQUAL_INT32(-900, eight.skew());
  for (int k = 0; k < 9; ++k) day.add(t, cc::fatTimeFromWall(cc::fatWallSeconds(t) + 86400));
  TEST_ASSERT_EQUAL_INT32(86400, day.skew());
  for (int k = 0; k < 9; ++k) over.add(t, cc::fatTimeFromWall(cc::fatWallSeconds(t) + 87300));
  TEST_ASSERT_EQUAL_INT32(0, over.skew());
  // More distinct deltas than the slots: 300 distinct and 10 at +3,600 has
  // no skew (10 < half), recounted or not.
  cc::SkewHistogram full;
  for (int k = 0; k < 300; ++k) full.add(t, cc::fatTimeFromWall(cc::fatWallSeconds(t) + 2 * (k + 1)));
  for (int k = 0; k < 10; ++k) full.add(t, cc::fatTimeFromWall(cc::fatWallSeconds(t) + 3600));
  TEST_ASSERT_EQUAL_UINT32(310, full.pairs());
  TEST_ASSERT_TRUE(full.needsRecount());
  TEST_ASSERT_EQUAL_INT32(0, full.skew());
  // A delta over FAT's whole range fits (delta / 2 in an i32).
  cc::SkewHistogram wide;
  TEST_ASSERT_TRUE(wide.add(cc::fatTime(1980, 1, 1, 0, 0, 0), cc::fatTime(2107, 12, 31, 23, 59, 58)));
  TEST_ASSERT_EQUAL_INT32(0, wide.skew());
}

namespace {

// 2.3.4 as written, with no cap: the reference for the summary.
int32_t skewReference(const std::vector<int64_t>& deltas, uint32_t pairs) {
  std::map<int64_t, uint32_t> counts;
  for (int64_t d : deltas)
    if (d) ++counts[d];
  int64_t best = 0;
  uint32_t c = 0;
  for (const auto& kv : counts) {
    const int64_t d = kv.first, ad = d < 0 ? -d : d, ab = best < 0 ? -best : best;
    if (kv.second > c || (kv.second == c && (ad < ab || (ad == ab && d < best)))) {
      best = d;
      c = kv.second;
    }
  }
  if (c < 8 || static_cast<uint64_t>(c) * 2 < pairs || best > 86400 || best < -86400 || best % 900) return 0;
  return static_cast<int32_t>(best);
}

// The deltas through a histogram: its skew with no second pass, and with one.
struct SkewRun {
  int32_t first = 0, recounted = 0;
  bool needed = false;
};
SkewRun runSkew(const std::vector<int64_t>& deltas) {
  const uint32_t t = cc::fatTime(2026, 3, 29, 1, 15, 0);
  const int64_t w = cc::fatWallSeconds(t) + 200000;  // room either way
  cc::SkewHistogram h;
  for (int64_t d : deltas) TEST_ASSERT_TRUE(h.add(cc::fatTimeFromWall(w), cc::fatTimeFromWall(w + d)));
  SkewRun r;
  r.first = h.skew();
  r.needed = h.needsRecount();
  h.beginRecount();
  for (int64_t d : deltas) h.recount(d);
  TEST_ASSERT_FALSE(h.needsRecount());
  r.recounted = h.skew();
  return r;
}

}  // namespace

// The histogram's 256 slots against 2.3.4 with no cap (review of N1): 256
// retouched files with 256 different deltas, then 300 under a +3,600 shift,
// give +3,600 in either order (a capped table that kept the first 256 deltas
// it saw lost it); at exactly half the pairs, the order can hide it until the
// recount; random cards agree with the rule after the recount, and without
// one never give another skew.
void test_skew_summary_is_order_free() {
  std::vector<int64_t> retouched, shifted(300, 3600);
  for (int k = 0; k < 256; ++k) retouched.push_back(2 * (k + 1) + 7200);
  std::vector<int64_t> a = retouched, b = shifted;
  a.insert(a.end(), shifted.begin(), shifted.end());
  b.insert(b.end(), retouched.begin(), retouched.end());
  for (const auto* d : {&a, &b}) {
    TEST_ASSERT_EQUAL_INT32(3600, skewReference(*d, static_cast<uint32_t>(d->size())));
    const SkewRun r = runSkew(*d);
    TEST_ASSERT_EQUAL_INT32(3600, r.first);
    TEST_ASSERT_EQUAL_INT32(3600, r.recounted);
  }
  // 300 at +3,600 and 300 distinct: exactly half. Shifted first, the 256th
  // distinct delta takes one from its count (299 < 300): no skew until the
  // recount counts it again.
  std::vector<int64_t> c(300, 3600), d;
  for (int k = 0; k < 300; ++k) d.push_back(2 * (k + 1) + 7200);
  std::vector<int64_t> half = c;
  half.insert(half.end(), d.begin(), d.end());
  TEST_ASSERT_EQUAL_INT32(3600, skewReference(half, 600));
  const SkewRun h = runSkew(half);
  TEST_ASSERT_TRUE(h.needed);
  TEST_ASSERT_EQUAL_INT32(0, h.first);
  TEST_ASSERT_EQUAL_INT32(3600, h.recounted);
  // Random cards: a shift (or none), retouched files, matches; every other
  // one with the shift at about half the pairs and more than 256 distinct
  // retouches, where the order matters to the summary.
  std::mt19937 rng(20261007);
  int found = 0, hidden = 0;
  for (int iter = 0; iter < 400; ++iter) {
    std::vector<int64_t> ds;
    const int64_t shift = (static_cast<int64_t>(rng() % 9) - 4) * 900;
    const bool edge = iter % 2 != 0;
    const uint32_t nRetouch = edge ? 260 + rng() % 500 : rng() % 700, nSame = rng() % (edge ? 40 : 200);
    const uint32_t spread = edge ? 5000 : 1 + rng() % 2000;
    const uint32_t nShift = edge ? nRetouch + nSame + rng() % 7 - 3 : rng() % 700;
    for (uint32_t k = 0; k < nShift; ++k) ds.push_back(shift);
    for (uint32_t k = 0; k < nRetouch; ++k) ds.push_back(2 * (1 + static_cast<int64_t>(rng() % spread)) + 90000);
    for (uint32_t k = 0; k < nSame; ++k) ds.push_back(0);
    std::shuffle(ds.begin(), ds.end(), rng);
    const int32_t want = skewReference(ds, static_cast<uint32_t>(ds.size()));
    const SkewRun r = runSkew(ds);
    TEST_ASSERT_EQUAL_INT32(want, r.recounted);
    TEST_ASSERT_TRUE(r.first == want || r.first == 0);
    if (!r.needed) TEST_ASSERT_EQUAL_INT32(want, r.first);
    found += want != 0 ? 1 : 0;
    hidden += want != 0 && r.first == 0 ? 1 : 0;
  }
  TEST_ASSERT_TRUE(found > 50);  // the cases are worth something
  printf("[skew] 400 random cards: %d with a skew, %d of them settled only by the recount\n", found, hidden);
}

void test_number_rule() {
  const Value& v = gVectors["numbers"];
  for (size_t i = 0; i < v.size(); ++i) {
    const std::string& kind = v[i]["kind"].str();
    const Value& want = v[i]["value"];
    const std::string& text = v[i]["text"].str();
    bool ok = false;
    int64_t got = 0;
    if (kind == "gain") {
      int16_t g;
      ok = cc::gainFromText(text.data(), text.size(), &g);
      got = g;
    } else if (kind == "peak") {
      uint16_t p;
      ok = cc::peakFromText(text.data(), text.size(), &p);
      got = p;
    } else if (kind == "bpm") {
      uint16_t b;
      ok = cc::bpm10FromText(text.data(), text.size(), &b);
      got = b;
    } else if (kind == "r128") {
      ok = true;
      got = cc::r128ToGain(static_cast<int16_t>(v[i]["q"].i64()));
      const std::string q = std::to_string(v[i]["q"].i64());
      int16_t viaText;
      TEST_ASSERT_TRUE(cc::r128FromText(q.data(), q.size(), &viaText));
      TEST_ASSERT_EQUAL_INT16(got, viaText);
    }
    const std::string what = kind + " " + text;
    if (want.isNull()) {
      TEST_ASSERT_FALSE_MESSAGE(ok, what.c_str());
    } else {
      TEST_ASSERT_TRUE_MESSAGE(ok, what.c_str());
      TEST_ASSERT_EQUAL_INT64_MESSAGE(want.i64(), got, what.c_str());
    }
  }
  // Around the edges: whitespace (VT isn't), signs, a bare point, a long
  // digit string, the ranges.
  int16_t g;
  uint16_t u;
  TEST_ASSERT_TRUE(cc::gainFromText(" \t+1.5dB\r\n", 10, &g));
  TEST_ASSERT_EQUAL_INT16(150, g);
  TEST_ASSERT_FALSE(cc::gainFromText("\v1.5", 4, &g));
  TEST_ASSERT_TRUE(cc::gainFromText(".005", 4, &g));
  TEST_ASSERT_EQUAL_INT16(1, g);
  TEST_ASSERT_TRUE(cc::gainFromText("-.005", 5, &g));
  TEST_ASSERT_EQUAL_INT16(-1, g);
  TEST_ASSERT_TRUE(cc::gainFromText("5.", 2, &g));
  TEST_ASSERT_EQUAL_INT16(500, g);
  TEST_ASSERT_FALSE(cc::gainFromText(".", 1, &g));
  TEST_ASSERT_FALSE(cc::gainFromText("", 0, &g));
  TEST_ASSERT_FALSE(cc::gainFromText("dB", 2, &g));
  TEST_ASSERT_FALSE(cc::gainFromText("1.0 dBdB", 8, &g));
  TEST_ASSERT_TRUE(cc::gainFromText("327.67", 6, &g));
  TEST_ASSERT_EQUAL_INT16(32767, g);
  TEST_ASSERT_FALSE(cc::gainFromText("327.68", 6, &g));
  TEST_ASSERT_TRUE(cc::gainFromText("-327.68", 7, &g));
  TEST_ASSERT_EQUAL_INT16(-32768, g);
  const std::string longDigits(400, '9');
  TEST_ASSERT_FALSE(cc::gainFromText(longDigits.data(), longDigits.size(), &g));
  TEST_ASSERT_TRUE(cc::peakFromText(longDigits.data(), longDigits.size(), &u));
  TEST_ASSERT_EQUAL_UINT16(65535, u);
  TEST_ASSERT_FALSE(cc::peakFromText("-0.5", 4, &u));
  TEST_ASSERT_TRUE(cc::peakFromText("0.00004", 7, &u));
  TEST_ASSERT_EQUAL_UINT16(0, u);
  TEST_ASSERT_TRUE(cc::peakFromText("0.00005", 7, &u));
  TEST_ASSERT_EQUAL_UINT16(1, u);
  TEST_ASSERT_TRUE(cc::bpm10FromText("20", 2, &u));
  TEST_ASSERT_EQUAL_UINT16(200, u);
  TEST_ASSERT_FALSE(cc::bpm10FromText("19.4", 4, &u));
  TEST_ASSERT_TRUE(cc::bpm10FromText("300.4", 5, &u));
  TEST_ASSERT_EQUAL_UINT16(3000, u);
  TEST_ASSERT_FALSE(cc::bpm10FromText("-120", 4, &u));
  TEST_ASSERT_FALSE(cc::r128FromText("1.5", 3, &g));
  TEST_ASSERT_FALSE(cc::r128FromText("32768", 5, &g));
  TEST_ASSERT_TRUE(cc::r128FromText("-32768", 6, &g));
  TEST_ASSERT_TRUE(cc::r128FromText(" +7 ", 4, &g));
  TEST_ASSERT_EQUAL_INT16(cc::r128ToGain(7), g);
}

void test_hash_sampled() {
  const Value& v = gVectors["hashSampled"];
  for (size_t i = 0; i < v.size(); ++i)
    TEST_ASSERT_EQUAL(v[i]["sampled"].boolean(),
                      cc::hashSampled(static_cast<uint16_t>(v[i]["hashV"].u64()), v[i]["fileSize"].u64()));
}

void test_text_decoders() {
  char out[64];
  // Latin-1: 0x80-0x9F stay C1 code points; trailing NULs go.
  const uint8_t l1[] = {'A', 0x80, 0x9F, 0xE9, 0, 0};
  cc::DecodeResult r = cc::latin1ToUtf8(l1, sizeof(l1), out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(7, r.length);
  TEST_ASSERT_EQUAL_MEMORY("A\xC2\x80\xC2\x9F\xC3\xA9", out, 8);
  // Cut at a code point, never inside one.
  r = cc::latin1ToUtf8(l1, 4, out, 3);
  TEST_ASSERT_TRUE(r.cut);
  TEST_ASSERT_EQUAL_size_t(1, r.length);
  // UTF-8, lossy: maximal subparts (as Rust's from_utf8_lossy).
  struct Case {
    const char* in;
    size_t n;
    const char* want;
  } cases[] = {
      {"ok \xC3\xA9", 5, "ok \xC3\xA9"},
      {"\xC3", 1, "\xEF\xBF\xBD"},                                   // a cut sequence
      {"\xE0\x80", 2, "\xEF\xBF\xBD\xEF\xBF\xBD"},                    // E0 wants A0-BF next
      {"\xF0\x9F\x98", 3, "\xEF\xBF\xBD"},                           // one subpart of three
      {"\xF0\x9F\x98x", 4, "\xEF\xBF\xBDx"},
      {"\xED\xA0\x80", 3, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},    // a surrogate: three
      {"\xC0\xAF", 2, "\xEF\xBF\xBD\xEF\xBF\xBD"},                    // overlong
      {"\xF4\x90\x80\x80", 4, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},  // past U+10FFFF
      {"a\xFF" "b", 3, "a\xEF\xBF\xBD" "b"},
      {"\xF0\x9F\x98\x80", 4, "\xF0\x9F\x98\x80"},
  };
  for (const Case& c : cases) {
    r = cc::utf8Lossy(reinterpret_cast<const uint8_t*>(c.in), c.n, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(c.want, out);
  }
  // UTF-16: pairs, unpaired surrogates, an odd length's stray byte.
  const uint8_t le[] = {'h', 0, 0x3D, 0xD8, 0x00, 0xDE, 0x00, 0xDC, 'x'};
  r = cc::utf16ToUtf8(le, sizeof(le), false, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("h\xF0\x9F\x98\x80\xEF\xBF\xBD\xEF\xBF\xBD", out);
  const uint8_t be[] = {0, 'h', 0xD8, 0x3D};
  r = cc::utf16ToUtf8(be, sizeof(be), true, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("h\xEF\xBF\xBD", out);
  // The cut length never splits a code point.
  TEST_ASSERT_EQUAL_size_t(2, cc::utf8CutLength("ab\xC3\xA9", 4, 3));
  TEST_ASSERT_EQUAL_size_t(2, cc::utf8CutLength("ab\xC3\xA9", 4, 2));
  TEST_ASSERT_EQUAL_size_t(4, cc::utf8CutLength("ab\xC3\xA9", 4, 9));
}

void test_field_rules() {
  const Value& v = gVectors["fieldRules"];
  for (size_t i = 0; i < v.size(); ++i) {
    const std::string& field = v[i]["field"].str();
    const bool list = field == "artist" || field == "albumArtist" || field == "genre" || field == "composer";
    cc::FieldBuilder b(list);
    for (size_t k = 0; k < v[i]["values"].size(); ++k) b.add(v[i]["values"][k].str().data(), v[i]["values"][k].str().size());
    TEST_ASSERT_EQUAL_STRING(v[i]["stored"].c_str(), b.data());
    TEST_ASSERT_EQUAL(v[i]["truncated"].boolean(), b.truncated());
    if (v[i].has("bytes")) TEST_ASSERT_EQUAL_size_t(v[i]["bytes"].u64(), b.size());
  }
  // Control characters and DEL to spaces, NUL included; a value of only a
  // control character isn't empty.
  cc::FieldBuilder b(true);
  b.add("a\0b", 3);
  b.add("\x7F", 1);
  b.add("", 0);
  TEST_ASSERT_EQUAL_STRING("a b\x1F ", b.data());
  TEST_ASSERT_EQUAL_UINT32(2, b.values());
  // Sixteen values, and the seventeenth ends the list.
  cc::FieldBuilder many(true);
  for (int k = 0; k < 17; ++k) {
    char s[8];
    std::snprintf(s, sizeof(s), "v%d", k);
    many.add(s);
  }
  TEST_ASSERT_EQUAL_UINT32(16, many.values());
  TEST_ASSERT_TRUE(many.truncated());
  // A repeat after the cut: two values equal in their first 255 bytes.
  cc::FieldBuilder cut(true);
  std::string x(300, 'x'), y(300, 'x');
  y[299] = 'y';
  cut.add(x.data(), x.size());
  cut.add(y.data(), y.size());
  TEST_ASSERT_EQUAL_UINT32(1, cut.values());
  TEST_ASSERT_TRUE(cut.truncated());
  // A single field: later values are ignored, a long one doesn't flag.
  cc::FieldBuilder one(false);
  one.add("first");
  one.add(x.data(), x.size());
  TEST_ASSERT_EQUAL_STRING("first", one.data());
  TEST_ASSERT_FALSE(one.truncated());
}

void test_string_runs() {
  static const char* kNames[cc::kRunFields] = {"title",     "artist",     "album",      "albumArtist",
                                               "genre",     "composer",   "titleSort",  "artistSort",
                                               "albumSort", "albumArtistSort", "mbAlbumId", "mbRecordingId"};
  const Value& v = gVectors["stringRuns"];
  for (size_t i = 0; i < v.size(); ++i) {
    std::string built[cc::kRunFields];
    const char* fields[cc::kRunFields] = {};
    for (int f = 0; f < cc::kRunFields; ++f) {
      const Value& vals = v[i]["fields"][kNames[f]];
      cc::FieldBuilder b(cc::isListField(f));
      for (size_t k = 0; k < vals.size(); ++k) b.add(vals[k].c_str());
      built[f] = b.data();
      fields[f] = built[f].c_str();
    }
    uint8_t run[cc::kRunMax];
    const size_t n = cc::encodeRun(fields, run, sizeof(run));
    const std::vector<uint8_t> want = hexBytes(v[i]["run"].str());
    TEST_ASSERT_EQUAL_size_t(want.size(), n);
    if (n) TEST_ASSERT_EQUAL_MEMORY(want.data(), run, n);
    // And back.
    if (n) {
      cc::RunFields parsed;
      size_t used = 0;
      TEST_ASSERT_TRUE(cc::parseRun(run, n, &parsed, &used));
      TEST_ASSERT_EQUAL_size_t(n, used);
      for (int f = 0; f < cc::kRunFields; ++f) TEST_ASSERT_EQUAL_STRING(fields[f], parsed.get(f));
    }
  }
  // A run that doesn't fit, a run cut short, a field past the limits (a
  // reader cuts it at a code point), a field past the twelve.
  const char* fields[cc::kRunFields] = {"T"};
  uint8_t small[2];
  TEST_ASSERT_EQUAL_size_t(0, cc::encodeRun(fields, small, sizeof(small)));
  const uint8_t shortRun[] = {2, 'a', 0, 'b'};
  cc::RunFields rf;
  TEST_ASSERT_FALSE(cc::parseRun(shortRun, sizeof(shortRun), &rf));
  std::vector<uint8_t> longRun = {14};
  for (int k = 0; k < 254; ++k) longRun.push_back('t');
  longRun.push_back(0xC3);
  longRun.push_back(0xA9);
  longRun.push_back(0);
  for (int f = 1; f < 14; ++f) {
    longRun.push_back(static_cast<uint8_t>('a' + f));
    longRun.push_back(0);
  }
  TEST_ASSERT_TRUE(cc::parseRun(longRun.data(), longRun.size(), &rf));
  TEST_ASSERT_EQUAL_size_t(254, rf.len(cc::kTitle));
  TEST_ASSERT_TRUE(rf.cut);
  TEST_ASSERT_EQUAL_STRING("b", rf.get(cc::kArtist));
  TEST_ASSERT_EQUAL_STRING("l", rf.get(cc::kMbRecordingId));
  TEST_ASSERT_EQUAL_UINT8(14, rf.n);
}

void test_names_and_the_canonical_order() {
  const Value& sib = gVectors["canonicalOrder"]["siblings"];
  for (size_t i = 0; i + 1 < sib.size(); ++i) {
    TEST_ASSERT_TRUE(cc::compareNames(sib[i].c_str(), sib[i].str().size(), sib[i + 1].c_str(),
                                      sib[i + 1].str().size()) < 0);
    TEST_ASSERT_TRUE(cc::compareFolderPaths(sib[i].c_str(), sib[i + 1].c_str()) < 0);
  }
  const Value& files = gVectors["canonicalOrder"]["files"];
  for (size_t i = 0; i + 1 < files.size(); ++i) {
    TEST_ASSERT_TRUE(cc::compareFilePaths(files[i].c_str(), files[i + 1].c_str()) < 0);
    TEST_ASSERT_TRUE(cc::compareFilePaths(files[i + 1].c_str(), files[i].c_str()) > 0);
  }
  TEST_ASSERT_EQUAL_INT(0, cc::compareFilePaths("A/x", "A/x"));
  // Folders: an ancestor first; "A" and its subfolders before "A B".
  TEST_ASSERT_TRUE(cc::compareFolderPaths("", "A") < 0);
  TEST_ASSERT_TRUE(cc::compareFolderPaths("A", "A/B") < 0);
  TEST_ASSERT_TRUE(cc::compareFolderPaths("A/B/C", "A B") < 0);
  TEST_ASSERT_EQUAL_INT(0, cc::compareFolderPaths("A/B", "A/B"));
  // Names and paths.
  TEST_ASSERT_TRUE(cc::validName("x", 1));
  TEST_ASSERT_TRUE(cc::validName("...", 3));
  TEST_ASSERT_FALSE(cc::validName("", 0));
  TEST_ASSERT_FALSE(cc::validName(".", 1));
  TEST_ASSERT_FALSE(cc::validName("..", 2));
  TEST_ASSERT_FALSE(cc::validName("a/b", 3));
  TEST_ASSERT_FALSE(cc::validName("a\0b", 3));
  TEST_ASSERT_TRUE(cc::validRelPath("A/B/c.mp3", 9));
  TEST_ASSERT_FALSE(cc::validRelPath("/A", 2));
  TEST_ASSERT_FALSE(cc::validRelPath("A/", 2));
  TEST_ASSERT_FALSE(cc::validRelPath("A//b", 4));
  TEST_ASSERT_FALSE(cc::validRelPath("A/../b", 6));
  const std::string longest(cc::kMaxRelPath, 'x'), over(cc::kMaxRelPath + 1, 'x');
  TEST_ASSERT_TRUE(cc::validRelPath(longest.data(), longest.size()));
  TEST_ASSERT_FALSE(cc::validRelPath(over.data(), over.size()));
  TEST_ASSERT_EQUAL_size_t(255, std::strlen("/music/") + cc::kMaxRelPath);
}

void test_album_folder() {
  const Value& v = gVectors["albumFolder"];
  for (size_t i = 0; i < v.size(); ++i) {
    const std::string& rel = v[i]["rel"].str();
    std::vector<std::string> roots;
    for (size_t k = 0; k < v[i]["roots"].size(); ++k) roots.push_back(v[i]["roots"][k].str());
    std::vector<const char*> rp;
    for (auto& r : roots) rp.push_back(r.c_str());
    const size_t n = cc::albumFolderLength(rel.data(), rel.size(), rp.data(), rp.size());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(v[i]["album"].c_str(), rel.substr(0, n).c_str(), rel.c_str());
    TEST_ASSERT_TRUE(cc::pathHash(rel.data(), n) == hexOf(v[i]["hash"]));
  }
  // The longest root wins; a root isn't a prefix of a longer name.
  const char* roots[] = {"Lib", "Lib/Sub"};
  const char* rel = "Lib/Sub/Artist/Album/CD1/01.flac";
  TEST_ASSERT_EQUAL_size_t(std::strlen("Lib/Sub/Artist/Album"), cc::albumFolderLength(rel, std::strlen(rel), roots, 2));
  const char* other = "Library/Artist/Album/x.mp3";
  TEST_ASSERT_EQUAL_size_t(std::strlen("Library/Artist"), cc::albumFolderLength(other, std::strlen(other), roots, 2));
  const char* loose = "Lib/x.mp3";
  TEST_ASSERT_EQUAL_size_t(3, cc::albumFolderLength(loose, std::strlen(loose), roots, 2));
}

void test_bytes_and_the_thumbnail() {
  const Value& b = gVectors["bytes"];
  const std::vector<uint8_t> mptg = hexBytes(b["MPTG"].str());
  TEST_ASSERT_EQUAL_HEX32(cc::kMagicMptg, cc::get32(mptg.data()));
  TEST_ASSERT_EQUAL_HEX32(hexOf(b["MPTGu32"]), cc::kMagicMptg);
  TEST_ASSERT_EQUAL_HEX32(hexOf(b["MPTHu32"]), cc::kMagicMpth);
  TEST_ASSERT_EQUAL_HEX32(thumbfile::kMagic, cc::kMagicMpth);
  // The transfer thumbnail: the device's MPTH v1, keyed by the album folder.
  const Value& t = b["thumbPath"];
  const uint64_t h = cc::pathHash(t["folder"].c_str());
  char path[64];
  TEST_ASSERT_TRUE(cc::transferThumbPath("/.mstream/thumbs", h, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING(t["path"].c_str(), path);
  TEST_ASSERT_EQUAL_size_t(t["bytes"].u64(), thumbfile::kFileBytes);
  thumbfile::Header hd;
  hd.pathHash = h;
  uint8_t head[thumbfile::kHeaderBytes];
  thumbfile::write(hd, head);
  const std::vector<uint8_t> want0 = hexBytes(t["head0to15"].str()), want20 = hexBytes(t["head20to23"].str());
  TEST_ASSERT_EQUAL_MEMORY(want0.data(), head, 16);
  TEST_ASSERT_EQUAL_MEMORY(want20.data(), head + 20, 4);
  // Stored byte strings: a UUID in RFC 4122 order, a hash prefix as its hex.
  const std::vector<uint8_t> uuid = hexBytes(b["serverInstance"]["uuid"].str());
  TEST_ASSERT_EQUAL_MEMORY(hexBytes(b["serverInstance"]["stored"].str()).data(), uuid.data(), 16);
  const std::vector<uint8_t> audio = hexBytes(b["hashPrefix"]["audioHash"].str());
  TEST_ASSERT_EQUAL_MEMORY(hexBytes(b["hashPrefix"]["stored"].str()).data(), audio.data(), 8);
  // An integer id is little-endian: B1F7E69FBD466B59 is 59 6B 46 BD 9F E6 F7 B1.
  uint8_t le[8];
  cc::put64(le, 0xB1F7E69FBD466B59ull);
  const uint8_t wantLe[8] = {0x59, 0x6B, 0x46, 0xBD, 0x9F, 0xE6, 0xF7, 0xB1};
  TEST_ASSERT_EQUAL_MEMORY(wantLe, le, 8);
}

void test_selection_signature() {
  // SHA-256 itself: FIPS 180-2's "abc" and the two-block message.
  uint8_t d[32];
  cc::mpdj::Sha256 a;
  a.update("abc", 3);
  a.final(d);
  TEST_ASSERT_EQUAL_MEMORY(hexBytes("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").data(), d, 32);
  cc::mpdj::Sha256 two;
  const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  two.update(m, std::strlen(m));
  two.final(d);
  TEST_ASSERT_EQUAL_MEMORY(hexBytes("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1").data(), d, 32);
  const Value& v = gVectors["selectionSignature"];
  for (size_t i = 0; i < v.size(); ++i) {
    std::vector<std::vector<uint8_t>> hs;
    for (size_t k = 0; k < v[i]["hashes"].size(); ++k) hs.push_back(hexBytes(v[i]["hashes"][k].str()));
    std::vector<uint8_t> flat;
    for (auto& h : hs) flat.insert(flat.end(), h.begin(), h.end());
    uint8_t sig[16];
    cc::mpdj::selectionSignature(v[i]["modelId"].c_str(), v[i]["modelVersion"].c_str(), v[i]["metric"].c_str(),
                                 static_cast<uint32_t>(v[i]["k"].u64()),
                                 reinterpret_cast<const uint8_t(*)[16]>(flat.data()), hs.size(), sig);
    TEST_ASSERT_EQUAL_MEMORY(hexBytes(v[i]["sig"].str()).data(), sig, 16);
  }
  TEST_ASSERT_EQUAL_MEMORY(hexBytes(gVectors["bytes"]["selectionSigStored"].str()).data(),
                           hexBytes(v[0]["sig"].str()).data(), 16);
  // The score: half away from zero, clamped.
  const float e[2] = {0.5f, 0.5f}, f[2] = {0.5f, 0.5f}, g[2] = {-1.0f, 0.0f}, h[2] = {1.0f, 1.0f};
  TEST_ASSERT_EQUAL_UINT8(128, cc::mpdj::score(e, f, 2));  // 127.5
  TEST_ASSERT_EQUAL_UINT8(0, cc::mpdj::score(e, g, 2));
  TEST_ASSERT_EQUAL_UINT8(255, cc::mpdj::score(h, h, 2));
}

void test_root_election() {
  const Value& v = gVectors["rootElection"];
  for (size_t i = 0; i < v.size(); ++i) {
    auto cand = [](const Value& c) {
      cc::RootCandidate r;
      r.valid = c["valid"].boolean();
      r.generation = static_cast<uint32_t>(c["generation"].u64());
      r.commitId = c["commitId"].u64();
      r.headerCrc = static_cast<uint32_t>(c["headerCrc"].u64());
      return r;
    };
    const cc::Election e = cc::elect(cand(v[i]["bin"]), cand(v[i]["tmp"]));
    const std::string& pick = v[i]["pick"].str();
    TEST_ASSERT_EQUAL_INT(pick == "bin" ? 1 : pick == "tmp" ? 2 : 0, static_cast<int>(e.pick));
    TEST_ASSERT_EQUAL(v[i]["sameCommit"].boolean(), e.sameCommit);
    if (v[i]["softwareRefuses"].boolean()) {
      // The tmp of major 2: the software's guard, on the magic and major alone.
      uint8_t head[8] = {'M', 'S', 'M', 'F', 2, 0, 0, 0};
      TEST_ASSERT_TRUE(cc::unknownMajor(head, sizeof(head)));
    }
  }
  uint8_t head[6] = {'M', 'P', 'T', 'G', 1, 0};
  TEST_ASSERT_FALSE(cc::unknownMajor(head, 6));
  head[4] = 0;
  TEST_ASSERT_TRUE(cc::unknownMajor(head, 6));  // 0 isn't a major either
  head[0] = 'X';
  TEST_ASSERT_FALSE(cc::unknownMajor(head, 6));  // not a contract file
  TEST_ASSERT_FALSE(cc::unknownMajor(head, 5));
  const uint8_t pend[6] = {'M', 'S', 'P', 'D', 3, 0}, dj[6] = {'M', 'P', 'D', 'J', 9, 9};
  TEST_ASSERT_TRUE(cc::unknownMajor(pend, 6));
  TEST_ASSERT_TRUE(cc::unknownMajor(dj, 6));
}

void test_device_txt() {
  namespace dt = cc::devicetxt;
  const std::string want = gVectors["deviceTxt"]["text"].str();
  TEST_ASSERT_EQUAL_STRING(cardfixtures::bytes("golden/device.txt").c_str(), want.c_str());
  char buf[512];
  const size_t n = dt::format(dt::current("0.8.0"), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_size_t(want.size(), n);
  TEST_ASSERT_EQUAL_STRING(want.c_str(), buf);
  // Read back.
  dt::Info i;
  TEST_ASSERT_TRUE(dt::parse(buf, n, &i));
  TEST_ASSERT_EQUAL_STRING("0.8.0", i.firmware);
  TEST_ASSERT_TRUE(dt::readsMajor(i.readMptg, 1));
  TEST_ASSERT_FALSE(dt::readsMajor(i.readMptg, 2));
  TEST_ASSERT_TRUE(dt::listHas(i.codecs, "opus"));
  TEST_ASSERT_TRUE(dt::listHas(i.extensions, "FLAC"));
  TEST_ASSERT_EQUAL_UINT32(48000, i.maxRate);
  // No file: majors 1, mp3 and flac.
  const dt::Info none = dt::defaults();
  TEST_ASSERT_EQUAL_STRING(gVectors["deviceTxt"]["noFile"]["codecs"].c_str(), none.codecs);
  TEST_ASSERT_EQUAL_STRING(gVectors["deviceTxt"]["noFile"]["extensions"].c_str(), none.extensions);
  TEST_ASSERT_FALSE(dt::listHas(none.codecs, "opus"));
  TEST_ASSERT_EQUAL_UINT32(48000, none.maxRate);
  TEST_ASSERT_EQUAL_UINT32(2, none.maxChannels);
  TEST_ASSERT_FALSE(dt::parse("hello\n", 6, &i));
  TEST_ASSERT_EQUAL_STRING("mp3,flac", i.codecs);
  // Unknown keys ignored, CRLF tolerated, a missing key its default, an
  // empty value none, several majors, a newer contract still read.
  const char* text = "contract=2\r\nfuture.key=x\r\nread.mptg=1,2\r\nread.mpdj=\r\ncodecs=mp3\r\n";
  TEST_ASSERT_TRUE(dt::parse(text, std::strlen(text), &i));
  TEST_ASSERT_EQUAL_UINT16(2, i.contract);
  TEST_ASSERT_TRUE(dt::readsMajor(i.readMptg, 2));
  TEST_ASSERT_EQUAL_UINT32(0, i.readMpdj);
  TEST_ASSERT_TRUE(dt::readsMajor(i.readMsmf, 1));
  TEST_ASSERT_EQUAL_STRING("mp3", i.codecs);
  TEST_ASSERT_EQUAL_STRING("mp3,flac", i.extensions);
  TEST_ASSERT_FALSE(dt::listHas("mp3,flac", "mp"));
  TEST_ASSERT_FALSE(dt::listHas("", "mp3"));
  TEST_ASSERT_EQUAL_size_t(0, dt::format(dt::current("0.8.0"), buf, 40));  // doesn't fit
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fixture_loads);
  RUN_TEST(test_crc32_and_its_combination);
  RUN_TEST(test_fnv_and_the_path_hash);
  RUN_TEST(test_server_url_key);
  RUN_TEST(test_qfp);
  RUN_TEST(test_fat_time);
  RUN_TEST(test_skew_rule);
  RUN_TEST(test_skew_summary_is_order_free);
  RUN_TEST(test_number_rule);
  RUN_TEST(test_hash_sampled);
  RUN_TEST(test_text_decoders);
  RUN_TEST(test_field_rules);
  RUN_TEST(test_string_runs);
  RUN_TEST(test_names_and_the_canonical_order);
  RUN_TEST(test_album_folder);
  RUN_TEST(test_bytes_and_the_thumbnail);
  RUN_TEST(test_selection_signature);
  RUN_TEST(test_root_election);
  RUN_TEST(test_device_txt);
  return UNITY_END();
}
