// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the mood filter (lib/core/MoodFilter;
// docs/MOOD-FILTERS.md 4.1, 5.3, 5.4, 5.7): q's pack and unpack over all
// 65,536 values (1,152 valid; any bit 11-15, voice 3 or level 3 reads as no
// filter); 5.7's worked example end to end, the server's row to the tail to
// the word, then filter 0x127 and its clause table; the value rule's float
// traps (0.29, 0.57, 0.58), its clamp, and every score to 4 and 5 places;
// the scale bytes and order bits; every byte value at every tail position
// against 5.3's validity rule; the word's buckets and NULLs; matches() on
// the tail's word against 4.1's predicate on the raw scores (the test's own
// port of mood-filter.js's SQL, with NaN for SQL's NULL) over a grid with
// ties, NULLs, an all-NULL row, no row and the bars exactly, for every
// valid q; a level alone, which filters nothing, word 0 included; count(),
// tally() and the album majority, under an inactive filter too; every group
// of test/fixtures/card/moods.json (tools/moodfilter.py's vectors), and the
// spec's literals it can only repeat. Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "MoodFilter.h"

using namespace moodfilter;
using minijson::Value;

namespace {

// MOOD_COLUMNS (mStream src/db/mood-sync.js): danceable, happy, sad,
// relaxed, aggressive, party, acoustic, instrumental.
enum { DANCEABLE, HAPPY, SAD, RELAXED, AGGRESSIVE, PARTY, ACOUSTIC, INSTR };
static_assert(kHeadDanceable == DANCEABLE && kHeadHappy == HAPPY && kHeadSad == SAD && kHeadRelaxed == RELAXED &&
                  kHeadAggressive == AGGRESSIVE && kHeadParty == PARTY && kHeadAcoustic == ACOUSTIC &&
                  kHeadInstrumental == INSTR && kHeads == 8,
              "the heads are in MOOD_COLUMNS order");

const double kNaN = std::nan("");

// ---------------------------------------------------------------------------
// 4.1's predicate on the raw scores, as mStream's SQL (mood-filter.js
// scale(), moodPredicate()), with q decoded here rather than by unpack().
// NaN is SQL's NULL: every comparison with it is false, and the predicate
// has no NOT, so a NULL head fails what tests it.
// ---------------------------------------------------------------------------
struct Bar {
  double on, mirror;
};

Bar barOf(unsigned level) {
  if (level == 1) return {0.7, 0.3};  // Strict
  if (level == 2) return {0.3, 0.7};  // Loose
  return {0.5, 0.5};                  // Standard
}

bool refScale(double lo, double hi, unsigned choice, Bar b) {
  switch (choice) {
    case 1: return lo >= b.on && lo >= hi;
    case 2: return lo < b.mirror && hi < b.mirror;
    case 3: return hi >= b.on && hi > lo;
    default: return true;
  }
}

// A valid q against a row (null: no row). A q that sets no group is no
// clause at all, so every song passes, one with no row too; any other
// passes no song without a row (its EXISTS).
bool refHolds(const double* s, uint16_t q) {
  if (!(q & 0x1FF)) return true;
  if (!s) return false;
  const Bar b = barOf(q >> 9 & 3);
  if (!refScale(s[HAPPY], s[SAD], q & 3, b)) return false;
  if (!refScale(s[RELAXED], s[AGGRESSIVE], q >> 2 & 3, b)) return false;
  const unsigned voice = q >> 4 & 3, tags = q >> 6 & 7;
  if (voice == 1 && !(s[INSTR] >= b.on)) return false;
  if (voice == 2 && !(s[INSTR] < b.mirror)) return false;
  if ((tags & 1) && !(s[ACOUSTIC] >= b.on)) return false;
  if ((tags & 2) && !(s[PARTY] >= b.on)) return false;
  if ((tags & 4) && !(s[DANCEABLE] >= b.on)) return false;
  return true;
}

// 5.4's rule, written out for the test.
bool expectValid(uint32_t q) { return q < 0x800 && (q >> 4 & 3) != 3 && (q >> 9 & 3) != 3; }

std::vector<uint16_t> validQs() {
  std::vector<uint16_t> out;
  for (uint32_t q = 0; q < 0x10000; ++q)
    if (expectValid(q)) out.push_back(static_cast<uint16_t>(q));
  return out;
}

uint16_t wordOf(const double* scores) {
  uint8_t t[8];
  tailOf(scores, t);
  return wordOfTail(t);
}

// One row through the writer and the reader: its tail, and the word the
// tail makes. moods.json's rows (S1's fixture) can go through it as they
// are, with their `holds` checked by refHolds and matches().
void expectRow(const char* note, const double* scores, const uint8_t want[8], uint16_t word) {
  uint8_t t[8];
  tailOf(scores, t);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, t, 8, note);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(word, wordOfTail(t), note);
}

// The tail byte a lone score makes (it sits at instrumental).
uint8_t byteOf(double score) {
  double s[8] = {0, 0, 0, 0, 0, 0, 0, score};
  uint8_t t[8];
  tailOf(s, t);
  return t[3];
}

// matches() against refHolds() for one row under each of `qs`; returns the
// mismatches, naming the first in `msg` while it is still empty.
uint32_t checkRow(const double* s, const std::vector<uint16_t>& qs, const std::vector<Filter>& fs, char* msg,
                  size_t msgSize) {
  const uint16_t word = wordOf(s);
  uint32_t wrong = 0;
  for (size_t k = 0; k < qs.size(); ++k) {
    if (matches(word, fs[k]) == refHolds(s, qs[k])) continue;
    if (!wrong++ && !msg[0]) {
      if (s) {
        std::snprintf(msg, msgSize, "q %03x word %04x: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", qs[k], word, s[0],
                      s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
      } else {
        std::snprintf(msg, msgSize, "q %03x: no row", qs[k]);
      }
    }
  }
  return wrong;
}

std::vector<Filter> filtersOf(const std::vector<uint16_t>& qs) {
  std::vector<Filter> out;
  for (uint16_t q : qs) out.push_back(Filter::unpack(q));
  return out;
}

// Scores to 4 places, as the server stores them: NULL, out of range both
// ways, the float traps, and each bar exactly with its neighbours.
const double kGrid[] = {kNaN, -0.25, 0,      0.1,  0.29,   0.2999, 0.3, 0.3001, 0.4999,
                        0.5,  0.5001, 0.57,  0.58, 0.6999, 0.7,    0.7001, 1,   1.25};
constexpr int kGridN = sizeof(kGrid) / sizeof(kGrid[0]);

// ---------------------------------------------------------------------------
// moods.json (test/fixtures/card, made with tools/moodfilter.py): hex
// strings are integers, byte strings space-separated hex, null a NULL.
// ---------------------------------------------------------------------------
Value moodsJson() {
  Value v;
  TEST_ASSERT_TRUE_MESSAGE(cardfixtures::json("moods.json", &v), "moods.json");
  return v;
}

void tailBytes(const Value& v, uint8_t out[8]) {
  const std::string& s = v.str();
  TEST_ASSERT_EQUAL_size_t_MESSAGE(23, s.size(), s.c_str());
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(std::strtoul(s.substr(3 * i, 2).c_str(), nullptr, 16));
}

// A row's scores in Head order, NaN for a NULL head; false for no row.
bool scoresOf(const Value& v, double out[8]) {
  if (v.isNull()) return false;
  TEST_ASSERT_EQUAL_size_t(8, v.size());
  for (int i = 0; i < 8; ++i) out[i] = v[i].isNull() ? kNaN : v[i].num();
  return true;
}

// The fixture's names (mStream's) for a filter's fields; "" for none.
std::string nameOf(const Value& v) { return v.isNull() ? std::string() : v.str(); }

Level levelNamed(const std::string& s) {
  if (s == "strict") return Level::Strict;
  if (s == "loose") return Level::Loose;
  TEST_ASSERT_EQUAL_STRING("standard", s.c_str());
  return Level::Standard;
}

Scale scaleNamed(const std::string& s, const char* first, const char* second) {
  if (s.empty()) return Scale::Any;
  if (s == first) return Scale::First;
  if (s == second) return Scale::Second;
  TEST_ASSERT_EQUAL_STRING("neutral", s.c_str());
  return Scale::Neutral;
}

Voice voiceNamed(const std::string& s) {
  if (s.empty()) return Voice::Any;
  if (s == "instrumental") return Voice::Instrumental;
  TEST_ASSERT_EQUAL_STRING("vocals", s.c_str());
  return Voice::Vocals;
}

uint8_t tagNamed(const std::string& s) {
  const uint8_t t = s == "acoustic" ? kAcoustic : s == "party" ? kParty : s == "danceable" ? kDanceable : 0;
  TEST_ASSERT_TRUE_MESSAGE(t != 0, s.c_str());
  return t;
}

// A predicate's one-group filter at Standard, from its name ("valence.sad").
Filter predicateNamed(const std::string& name) {
  const size_t dot = name.find('.');
  const std::string group = name.substr(0, dot), value = name.substr(dot + 1);
  Filter f;
  if (group == "valence") {
    f.valence = scaleNamed(value, "happy", "sad");
  } else if (group == "energy") {
    f.energy = scaleNamed(value, "relaxed", "aggressive");
  } else if (group == "voice") {
    f.voice = voiceNamed(value);
  } else {
    TEST_ASSERT_EQUAL_STRING("tags", group.c_str());
    f.tags = tagNamed(value);
  }
  return f;
}

bool listed(const Value& names, const std::string& name) {
  for (size_t i = 0; i < names.size(); ++i)
    if (names[i].str() == name) return true;
  return false;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// q
// ---------------------------------------------------------------------------

void test_pack_and_unpack_every_q() {
  uint32_t valids = 0;
  for (uint32_t q = 0; q < 0x10000; ++q) {
    char msg[32];
    std::snprintf(msg, sizeof msg, "q %04x", q);
    const bool want = expectValid(q);
    TEST_ASSERT_EQUAL_MESSAGE(want, valid(static_cast<uint16_t>(q)), msg);
    const Filter f = Filter::unpack(static_cast<uint16_t>(q));
    if (want) {
      ++valids;
      TEST_ASSERT_EQUAL_HEX16_MESSAGE(q, f.pack(), msg);
      TEST_ASSERT_EQUAL_MESSAGE((q & 0x1FF) != 0, f.active(), msg);
    } else {
      // The whole of q reads as 0: no filter, at Standard.
      TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, f.pack(), msg);
      TEST_ASSERT_FALSE_MESSAGE(f.active(), msg);
    }
  }
  TEST_ASSERT_EQUAL_UINT32(4 * 4 * 3 * 8 * 3, valids);

  // The console's refusals (9.3): q800 (bit 11), q30 (voice 3), q600 (level 3).
  TEST_ASSERT_FALSE(valid(0x800));
  TEST_ASSERT_FALSE(valid(0x30));
  TEST_ASSERT_FALSE(valid(0x600));
  TEST_ASSERT_FALSE(valid(0x8000));
  TEST_ASSERT_TRUE(valid(0x5EF));  // the largest valid q

  // The fields, both ways.
  const Filter f = Filter::unpack(0x127);
  TEST_ASSERT_EQUAL(static_cast<int>(Scale::Second), static_cast<int>(f.valence));
  TEST_ASSERT_EQUAL(static_cast<int>(Scale::First), static_cast<int>(f.energy));
  TEST_ASSERT_EQUAL(static_cast<int>(Voice::Vocals), static_cast<int>(f.voice));
  TEST_ASSERT_EQUAL_UINT8(kDanceable, f.tags);
  TEST_ASSERT_EQUAL(static_cast<int>(Level::Standard), static_cast<int>(f.level));
  Filter g;
  TEST_ASSERT_EQUAL_HEX16(0, g.pack());
  g.valence = Scale::Neutral;
  g.energy = Scale::Second;
  g.voice = Voice::Instrumental;
  g.tags = kAcoustic | kParty;
  g.level = Level::Loose;
  TEST_ASSERT_EQUAL_HEX16(2 | 3 << 2 | 1 << 4 | 3 << 6 | 2 << 9, g.pack());
}

void test_a_level_alone_is_not_active() {
  const Level levels[] = {Level::Standard, Level::Strict, Level::Loose};
  for (Level l : levels) {
    Filter f;
    f.level = l;
    TEST_ASSERT_FALSE(f.active());
  }
  TEST_ASSERT_FALSE(Filter::unpack(0x200).active());
  TEST_ASSERT_FALSE(Filter::unpack(0x400).active());
  // Any one group is.
  const uint16_t one[] = {0x001, 0x002, 0x003, 0x004, 0x008, 0x00C, 0x010, 0x020, 0x040, 0x080, 0x100};
  for (uint16_t q : one) TEST_ASSERT_TRUE(Filter::unpack(q).active());
}

// ---------------------------------------------------------------------------
// The tail and the word
// ---------------------------------------------------------------------------

void test_the_worked_example() {
  // 5.7: music/Artist A/song.flac.
  const double row[8] = {0.7012, 0.3104, 0.5803, 0.7, 0.2999, kNaN, 0.4999, 0.29};
  const uint8_t tail[8] = {0x03, 0x3A, 0x46, 0x1D, 0x31, 0xFF, 0x46, 0x00};
  expectRow("5.7", row, tail, 0xB313);
  TEST_ASSERT_TRUE(tailValid(tail));

  // Filter 0x127: Sad, Relaxed, Vocals, Danceable at Standard. Word true, SQL 1.
  const Filter f = Filter::unpack(0x127);
  TEST_ASSERT_TRUE(f.active());
  TEST_ASSERT_TRUE(matches(0xB313, f));
  TEST_ASSERT_TRUE(refHolds(row, 0x127));

  // The clause table: each clause alone holds, on the word and on the SQL.
  const uint16_t holds[] = {
      0x003,  // Sad: bucket 2 >= 2, side 1
      0x004,  // Relaxed: bucket 3 >= 2, side 0
      0x020,  // Vocals: bucket 0 < 2
      0x100,  // Danceable: bucket 3 >= 2
      0x440,  // Acoustic at Loose: .4999 >= .3
      0x300,  // Danceable at Strict: .7012 >= .7
      0x220,  // Vocals at Strict: .29 < .3
  };
  for (uint16_t q : holds) {
    char msg[16];
    std::snprintf(msg, sizeof msg, "q %03x", q);
    TEST_ASSERT_TRUE_MESSAGE(matches(0xB313, Filter::unpack(q)), msg);
    TEST_ASSERT_TRUE_MESSAGE(refHolds(row, q), msg);
  }
  // And the ones it fails.
  const uint16_t fails[] = {
      0x001,  // Happy: sad beat it
      0x002,  // Neutral valence: .5803 is not < .5
      0x00C,  // Aggressive
      0x008,  // Neutral energy
      0x010,  // Instrumental
      0x040,  // Acoustic: .4999 < .5
      0x080,  // Party: NULL
      0x480,  // Party at Loose: still NULL
      0x203,  // Sad at Strict: .5803 < .7
  };
  for (uint16_t q : fails) {
    char msg[16];
    std::snprintf(msg, sizeof msg, "q %03x", q);
    TEST_ASSERT_FALSE_MESSAGE(matches(0xB313, Filter::unpack(q)), msg);
    TEST_ASSERT_FALSE_MESSAGE(refHolds(row, q), msg);
  }
}

void test_the_float_traps_and_the_clamp() {
  // Plain floor is wrong at these on this host too.
  volatile double trap = 0.29;
  TEST_ASSERT_EQUAL_INT(28, static_cast<int>(std::floor(trap * 100.0)));
  TEST_ASSERT_EQUAL_UINT8(29, byteOf(0.29));
  TEST_ASSERT_EQUAL_UINT8(57, byteOf(0.57));
  TEST_ASSERT_EQUAL_UINT8(58, byteOf(0.58));
  // At every value byte, scales included.
  const double traps[] = {0.29, 0.57, 0.58};
  for (double v : traps) {
    const uint8_t want = static_cast<uint8_t>(std::lround(v * 100));
    const double s[8] = {v, v, 0, v, 0, v, v, v};
    uint8_t t[8];
    tailOf(s, t);
    for (int i = 1; i <= 6; ++i) TEST_ASSERT_EQUAL_UINT8(want, t[i]);
  }
  // The clamp (3.2), and the ends.
  TEST_ASSERT_EQUAL_UINT8(0, byteOf(-0.25));
  TEST_ASSERT_EQUAL_UINT8(0, byteOf(-INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, byteOf(0));
  TEST_ASSERT_EQUAL_UINT8(0, byteOf(0.0099));
  TEST_ASSERT_EQUAL_UINT8(1, byteOf(0.01));
  TEST_ASSERT_EQUAL_UINT8(99, byteOf(0.9999));
  TEST_ASSERT_EQUAL_UINT8(100, byteOf(1));
  TEST_ASSERT_EQUAL_UINT8(100, byteOf(1.25));
  TEST_ASSERT_EQUAL_UINT8(100, byteOf(INFINITY));
  TEST_ASSERT_EQUAL_UINT8(kNull, byteOf(kNaN));
}

void test_every_score_to_4_and_5_places() {
  // 5.3: the slack form is right for every score the server can store (to
  // 4 places), and on a 5-place grid.
  for (uint32_t k = 0; k <= 10000; ++k) {
    char msg[24];
    std::snprintf(msg, sizeof msg, "%u / 10000", k);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(k / 100, byteOf(k / 10000.0), msg);
  }
  for (uint32_t k = 0; k <= 100000; ++k) {
    char msg[24];
    std::snprintf(msg, sizeof msg, "%u / 100000", k);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(k / 1000, byteOf(k / 100000.0), msg);
  }
}

void test_the_scale_bytes_and_the_order_bits() {
  const double n = kNaN;
  struct Case {
    const char* note;
    double happy, sad, relaxed, aggressive;
    uint8_t flags, valence, energy;
  };
  const Case cases[] = {
      {"plain", 0.3104, 0.5803, 0.7, 0.2999, 0x03, 58, 70},
      {"ties go to happy, relaxed", 0.5, 0.5, 0.3, 0.3, 0x01, 50, 30},
      {"raw order, one byte", 0.5801, 0.5803, 0.7002, 0.7001, 0x03, 58, 70},
      {"aggressive beat relaxed", 0.9, 0.1, 0.2, 0.6, 0x05, 90, 60},
      {"happy NULL", n, 0.9, 0.4, 0.4, 0x01, kNull, 40},
      {"sad NULL", 0.9, n, 0.4, 0.4, 0x01, kNull, 40},
      {"relaxed NULL", 0.1, 0.2, n, 0.9, 0x03, 20, kNull},
      {"aggressive NULL", 0.1, 0.2, 0.9, n, 0x03, 20, kNull},
      {"both under 30, sad over", 0.1, 0.2, 0.1, 0.2, 0x07, 20, 20},
      {"clamped, order from the raw scores", 1.2, 1.5, -0.1, -0.2, 0x03, 100, 0},
  };
  for (const Case& c : cases) {
    const double s[8] = {0.5, c.happy, c.sad, c.relaxed, c.aggressive, 0.5, 0.5, 0.5};
    uint8_t t[8];
    tailOf(s, t);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(c.flags, t[0], c.note);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(c.valence, t[1], c.note);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(c.energy, t[2], c.note);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, t[7], c.note);
    TEST_ASSERT_TRUE_MESSAGE(tailValid(t), c.note);
  }

  // All NULL differs from no row: it is analysed, and counts in an
  // album's denominator.
  const double allNull[8] = {n, n, n, n, n, n, n, n};
  const uint8_t nullTail[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  expectRow("all NULL", allNull, nullTail, kAnalysed | kVoiceNull | 4 << 11 | 4 << 8);
  const uint8_t zeros[8] = {};
  expectRow("no row", nullptr, zeros, 0);
  TEST_ASSERT_FALSE(tailValid(zeros));
}

void test_every_byte_at_every_tail_position() {
  const uint8_t bases[][8] = {
      {0x03, 0x3A, 0x46, 0x1D, 0x31, 0xFF, 0x46, 0x00},  // 5.7
      {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00},  // all NULL
      {0x07, 100, 100, 100, 100, 100, 100, 0x00},
      {0x01, 0, 0, 0, 0, 0, 0, 0x00},
  };
  for (const auto& base : bases) {
    TEST_ASSERT_TRUE(tailValid(base));
    for (int pos = 0; pos < 8; ++pos) {
      for (int v = 0; v < 256; ++v) {
        uint8_t t[8];
        for (int i = 0; i < 8; ++i) t[i] = base[i];
        t[pos] = static_cast<uint8_t>(v);
        bool want;
        if (pos == 0) {
          want = (v & 1) && !(v & 0xF8);
        } else if (pos == 7) {
          want = v == 0;
        } else {
          want = v <= 100 || v == 255;
        }
        char msg[32];
        std::snprintf(msg, sizeof msg, "byte %d = %d", 72 + pos, v);
        TEST_ASSERT_EQUAL_MESSAGE(want, tailValid(t), msg);
        const uint16_t w = wordOfTail(t);
        if (want) {
          TEST_ASSERT_TRUE_MESSAGE(w & kAnalysed, msg);
        } else {
          TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, w, msg);
        }
        // No filter passes either; an active one (Neutral at Loose) passes
        // no refused tail.
        TEST_ASSERT_TRUE_MESSAGE(matches(w, Filter()), msg);
        if (!want) TEST_ASSERT_FALSE_MESSAGE(matches(w, Filter::unpack(0x402)), msg);
      }
    }
  }
  // mptg-minor-1's 0xAB fill (flag bits 0xA8) reads as no scores.
  uint8_t ab[8];
  for (uint8_t& b : ab) b = 0xAB;
  TEST_ASSERT_FALSE(tailValid(ab));
  TEST_ASSERT_EQUAL_HEX16(0, wordOfTail(ab));
}

void test_the_word_buckets_and_nulls() {
  // Each value byte at the bucket edges: 0 under 30, 1 for 30-49, 2 for
  // 50-69, 3 for 70 and up.
  const uint8_t edges[][2] = {{0, 0}, {29, 0}, {30, 1}, {49, 1}, {50, 2}, {69, 2}, {70, 3}, {100, 3}};
  const int shifts[] = {6, 4, 2, 0};  // instrumental, acoustic, party, danceable at bytes 75-78
  for (const auto& e : edges) {
    for (int k = 0; k < 4; ++k) {
      uint8_t t[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
      t[3 + k] = e[0];
      TEST_ASSERT_EQUAL_HEX16(kAnalysed | e[1] << shifts[k], wordOfTail(t));
    }
    // The scales, at side 0 and side 1 (bucket 0 stores side 0).
    for (int side = 0; side < 2; ++side) {
      uint8_t t[8] = {static_cast<uint8_t>(side ? 0x07 : 0x01), e[0], e[0], 0, 0, 0, 0, 0};
      const unsigned field = e[1] ? (side << 2 | e[1]) : 0;
      TEST_ASSERT_EQUAL_HEX16(kAnalysed | field << 11 | field << 8, wordOfTail(t));
    }
  }
  // NULLs: a scale stores 4, an order bit beside it ignored; the voice
  // sets bit 14 and stores 0; a tag stores 0.
  const uint8_t nullScales[8] = {0x07, 0xFF, 0xFF, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_HEX16(kAnalysed | 4 << 11 | 4 << 8, wordOfTail(nullScales));
  const uint8_t nullVoice[8] = {0x01, 0, 0, 0xFF, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_HEX16(kAnalysed | kVoiceNull, wordOfTail(nullVoice));
  const uint8_t nullTags[8] = {0x01, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0};
  TEST_ASSERT_EQUAL_HEX16(kAnalysed, wordOfTail(nullTags));
}

// ---------------------------------------------------------------------------
// The predicate
// ---------------------------------------------------------------------------

void test_matches_the_raw_predicate() {
  const std::vector<uint16_t> all = validQs();
  std::vector<uint16_t> scales, voiceTags;
  for (uint16_t q : all) {
    if ((q & 0x1F0) == 0) scales.push_back(q);
    if ((q & 0x00F) == 0) voiceTags.push_back(q);
  }
  const std::vector<Filter> allF = filtersOf(all), scalesF = filtersOf(scales), voiceTagsF = filtersOf(voiceTags);
  TEST_ASSERT_EQUAL_UINT32(1152, all.size());
  TEST_ASSERT_EQUAL_UINT32(48, scales.size());
  TEST_ASSERT_EQUAL_UINT32(72, voiceTags.size());

  char msg[160] = "";
  uint32_t wrong = 0, rows = 0;
  // Every pair of happy, sad and of relaxed, aggressive on the grid (ties,
  // NULLs, the bars) under every scale filter at every level.
  for (int a = 0; a < kGridN; ++a)
    for (int b = 0; b < kGridN; ++b)
      for (int c = 0; c < kGridN; ++c)
        for (int d = 0; d < kGridN; ++d) {
          const double s[8] = {kGrid[(a + c) % kGridN], kGrid[a], kGrid[b], kGrid[c], kGrid[d],
                               kGrid[(b + d) % kGridN], kGrid[(a + d) % kGridN], kGrid[(b + c) % kGridN]};
          wrong += checkRow(s, scales, scalesF, msg, sizeof msg);
          ++rows;
        }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, wrong, msg);
  // Every instrumental, acoustic, party, danceable on the grid under every
  // voice and tag filter.
  for (int a = 0; a < kGridN; ++a)
    for (int b = 0; b < kGridN; ++b)
      for (int c = 0; c < kGridN; ++c)
        for (int d = 0; d < kGridN; ++d) {
          const double s[8] = {kGrid[d], kGrid[(a + b) % kGridN], kGrid[(c + d) % kGridN], kGrid[(a + c) % kGridN],
                               kGrid[(b + d) % kGridN], kGrid[c], kGrid[b], kGrid[a]};
          wrong += checkRow(s, voiceTags, voiceTagsF, msg, sizeof msg);
          ++rows;
        }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, wrong, msg);

  // Every valid q over rows of the grid drawn at random, and the rows a
  // grid misses: all NULL, no row, every head at one bar.
  const double n = kNaN;
  const double special[][8] = {
      {n, n, n, n, n, n, n, n},
      {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5},
      {0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3},
      {0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7},
      {0.7012, 0.3104, 0.5803, 0.7, 0.2999, n, 0.4999, 0.29},
  };
  for (const auto& s : special) wrong += checkRow(s, all, allF, msg, sizeof msg);
  wrong += checkRow(nullptr, all, allF, msg, sizeof msg);
  uint32_t x = 0x9E3779B9u;  // xorshift32
  for (int r = 0; r < 4096; ++r) {
    double s[8];
    for (double& v : s) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      v = kGrid[x % kGridN];
    }
    wrong += checkRow(s, all, allF, msg, sizeof msg);
    ++rows;
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, wrong, msg);
  TEST_ASSERT_EQUAL_UINT32(2 * 104976 + 4096, rows);
}

void test_a_level_alone_filters_nothing() {
  // Every word: a level alone is no filter, which holds for every word,
  // word 0 and those without kAnalysed included (4.1, moodfilter.py's
  // matches); so does an invalid q, which reads as 0.
  const Level levels[] = {Level::Standard, Level::Strict, Level::Loose};
  for (Level l : levels) {
    Filter f;
    f.level = l;
    uint32_t wrong = 0;
    for (uint32_t w = 0; w < 0x10000; ++w)
      if (!matches(static_cast<uint16_t>(w), f) || !matches(static_cast<uint16_t>(w), Filter())) ++wrong;
    TEST_ASSERT_EQUAL_UINT32(0, wrong);
  }
  TEST_ASSERT_TRUE(matches(0, Filter::unpack(0x627)));
  TEST_ASSERT_TRUE(matches(0x3313, Filter::unpack(0x800)));
  // Under an active q, word 0 and every word without kAnalysed match
  // nothing.
  const std::vector<uint16_t> all = validQs();
  uint32_t active = 0;
  for (uint16_t q : all) {
    const Filter f = Filter::unpack(q);
    if (!f.active()) continue;
    ++active;
    TEST_ASSERT_FALSE(matches(0, f));
    TEST_ASSERT_FALSE(matches(0x7FFF, f));
    TEST_ASSERT_FALSE(matches(0x3313, f));
  }
  TEST_ASSERT_EQUAL_UINT32(1152 - 3, active);
}

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

void test_count() {
  const uint16_t words[] = {
      0xB313,  // 5.7: holds 0x127
      0,       // no row
      0xE400,  // all NULL: analysed, holds nothing set
      0x8000,  // analysed, every head under 30
      0xB313,
      0x3313,  // 5.7's fields without kAnalysed: nothing
  };
  const Filter f = Filter::unpack(0x127);
  uint32_t analysed = 99;
  TEST_ASSERT_EQUAL_UINT32(2, count(words, 6, f, &analysed));
  TEST_ASSERT_EQUAL_UINT32(4, analysed);
  TEST_ASSERT_EQUAL_UINT32(2, count(words, 6, f, nullptr));
  // No filter, or a level alone: every word, analysed or not.
  TEST_ASSERT_EQUAL_UINT32(6, count(words, 6, Filter(), &analysed));
  TEST_ASSERT_EQUAL_UINT32(4, analysed);
  TEST_ASSERT_EQUAL_UINT32(6, count(words, 6, Filter::unpack(0x200), &analysed));
  TEST_ASSERT_EQUAL_UINT32(4, analysed);
  // Neutral valence: 0x8000 alone (all NULL fails its own scale).
  TEST_ASSERT_EQUAL_UINT32(1, count(words, 6, Filter::unpack(0x002), &analysed));
  // Vocals: 5.7 twice and 0x8000; all NULL's voice is NULL.
  TEST_ASSERT_EQUAL_UINT32(3, count(words, 6, Filter::unpack(0x020), &analysed));
  TEST_ASSERT_EQUAL_UINT32(0, count(nullptr, 0, f, &analysed));
  TEST_ASSERT_EQUAL_UINT32(0, analysed);
}

void test_the_album_majority() {
  // 4.1 and 5.7: 5 passing of 9 analysed matches, since 10 > 9. 2 of 4
  // does not. 0 of 0 does not.
  TEST_ASSERT_TRUE(majority(Tally{5, 9}));
  TEST_ASSERT_FALSE(majority(Tally{2, 4}));
  TEST_ASSERT_FALSE(majority(Tally{0, 0}));
  TEST_ASSERT_FALSE(majority(Tally{4, 9}));
  TEST_ASSERT_TRUE(majority(Tally{1, 1}));
  TEST_ASSERT_TRUE(majority(Tally{0x80000000u, 0xFFFFFFFFu}));  // no overflow
  TEST_ASSERT_FALSE(majority(Tally{0x7FFFFFFFu, 0xFFFFFFFEu}));

  // tally() over an album's ids: songs with no row stay out of the
  // denominator; an all-NULL one stays in; 5.7's fields without kAnalysed
  // are no row.
  const uint16_t pass = 0xB313, fail = 0x8000, allNull = 0xE400, unanalysed = 0x3313;
  const uint16_t words[] = {0,    pass, fail, pass, 0,    pass, allNull,   fail,
                            pass, 0,    fail, pass, fail, allNull, unanalysed};
  const Filter f = Filter::unpack(0x127);
  const uint32_t nine[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};  // 5 pass of 9 analysed, 3 with no row
  Tally t = tally(words, nine, 12, f);
  TEST_ASSERT_EQUAL_UINT32(5, t.pass);
  TEST_ASSERT_EQUAL_UINT32(9, t.analysed);
  TEST_ASSERT_FALSE(t.unfiltered);
  TEST_ASSERT_TRUE(majority(t));
  const uint32_t nineAndOne[] = {14, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  t = tally(words, nineAndOne, 13, f);
  TEST_ASSERT_EQUAL_UINT32(5, t.pass);
  TEST_ASSERT_EQUAL_UINT32(9, t.analysed);
  const uint32_t four[] = {13, 1, 12, 9, 3};  // 2 of 4, and one with no row
  t = tally(words, four, 5, f);
  TEST_ASSERT_EQUAL_UINT32(2, t.pass);
  TEST_ASSERT_EQUAL_UINT32(4, t.analysed);
  TEST_ASSERT_FALSE(majority(t));
  const uint32_t none[] = {0, 4, 9, 14};  // no analysed song
  t = tally(words, none, 4, f);
  TEST_ASSERT_EQUAL_UINT32(0, t.pass);
  TEST_ASSERT_EQUAL_UINT32(0, t.analysed);
  TEST_ASSERT_FALSE(majority(t));
  t = tally(words, nullptr, 0, f);
  TEST_ASSERT_FALSE(majority(t));

  // An inactive filter (none, a level alone, an invalid q), as
  // moodfilter.py's album_counts and album_matches: pass counts the
  // analysed songs, and every album matches, one with none analysed too.
  for (uint16_t q : {0x000, 0x200, 0x400, 0x627}) {
    const Filter g = Filter::unpack(q);
    t = tally(words, nineAndOne, 13, g);
    TEST_ASSERT_EQUAL_UINT32(9, t.pass);
    TEST_ASSERT_EQUAL_UINT32(9, t.analysed);
    TEST_ASSERT_TRUE(t.unfiltered);
    TEST_ASSERT_TRUE(majority(t));
    t = tally(words, none, 4, g);
    TEST_ASSERT_EQUAL_UINT32(0, t.pass);
    TEST_ASSERT_EQUAL_UINT32(0, t.analysed);
    TEST_ASSERT_TRUE(majority(t));
    TEST_ASSERT_TRUE(majority(tally(words, nullptr, 0, g)));
  }
}

// ---------------------------------------------------------------------------
// moods.json, every group (9.6 C3; METADATA 2.17, item 9)
// ---------------------------------------------------------------------------

void test_the_spec_literals() {
  // What moods.json can only repeat, pinned from 5.4: the 11 predicates'
  // q in its order, and the level codes, strict 1, standard 0, loose 2.
  struct Pin {
    const char* name;
    uint16_t q;
  };
  const Pin pins[11] = {
      {"valence.happy", 0x001},      {"valence.neutral", 0x002}, {"valence.sad", 0x003},
      {"energy.relaxed", 0x004},     {"energy.neutral", 0x008},  {"energy.aggressive", 0x00C},
      {"voice.instrumental", 0x010}, {"voice.vocals", 0x020},    {"tags.acoustic", 0x040},
      {"tags.party", 0x080},         {"tags.danceable", 0x100},
  };
  const Value m = moodsJson();
  const Value& ps = m["predicates"];
  TEST_ASSERT_EQUAL_size_t(11, ps.size());
  for (size_t i = 0; i < 11; ++i) {
    TEST_ASSERT_EQUAL_STRING(pins[i].name, ps[i]["name"].c_str());
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(pins[i].q, ps[i]["q"].u64(), pins[i].name);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(pins[i].q, predicateNamed(pins[i].name).pack(), pins[i].name);
  }
  const struct {
    const char* name;
    uint16_t code;
    Level level;
  } codes[3] = {{"strict", 1, Level::Strict}, {"standard", 0, Level::Standard}, {"loose", 2, Level::Loose}};
  const Value& ls = m["levels"];
  TEST_ASSERT_EQUAL_size_t(3, ls.size());
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL_STRING(codes[i].name, ls[i]["level"].c_str());
    TEST_ASSERT_EQUAL_UINT16(codes[i].code, ls[i]["code"].u64());
    Filter f;
    f.level = codes[i].level;
    TEST_ASSERT_EQUAL_HEX16(codes[i].code << 9, f.pack());
    TEST_ASSERT_EQUAL(static_cast<int>(codes[i].level), static_cast<int>(Filter::unpack(codes[i].code << 9).level));
  }
  // Happy, Relaxed, Instrumental at Strict; 5.7's filter.
  Filter f;
  f.valence = Scale::First;
  f.energy = Scale::First;
  f.voice = Voice::Instrumental;
  f.level = Level::Strict;
  TEST_ASSERT_EQUAL_HEX16(0x215, f.pack());
  Filter g;
  g.valence = Scale::Second;
  g.energy = Scale::First;
  g.voice = Voice::Vocals;
  g.tags = kDanceable;
  TEST_ASSERT_EQUAL_HEX16(0x127, g.pack());
}

void test_the_moods_json_values_levels_and_bars() {
  const Value m = moodsJson();
  char msg[64];
  // The value rule, and each value's bucket.
  const Value& vs = m["values"];
  TEST_ASSERT_TRUE(vs.size() > 0);
  for (size_t i = 0; i < vs.size(); ++i) {
    const Value& v = vs[i];
    std::snprintf(msg, sizeof msg, "values[%u] %s", static_cast<unsigned>(i), v["score"].c_str());
    const uint8_t b = byteOf(v["score"].isNull() ? kNaN : v["score"].num());
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(v["value"].u64(), b, msg);
    if (!v.has("bucket")) continue;
    const uint8_t t[8] = {0x01, 0, 0, b, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(v["bucket"].u64(), wordOfTail(t) >> 6 & 3, msg);
  }
  // need / under: the buckets each level's predicates take, on a scale (side
  // 0) and on the voice.
  const Value& ls = m["levels"];
  for (size_t i = 0; i < ls.size(); ++i) {
    const uint16_t code = static_cast<uint16_t>(ls[i]["code"].u64() << 9);
    const unsigned need = static_cast<unsigned>(ls[i]["need"].u64());
    const unsigned under = static_cast<unsigned>(ls[i]["under"].u64());
    const Filter happy = Filter::unpack(0x001 | code), neutral = Filter::unpack(0x002 | code);
    const Filter instrumental = Filter::unpack(0x010 | code), vocals = Filter::unpack(0x020 | code);
    for (unsigned b = 0; b < 4; ++b) {
      std::snprintf(msg, sizeof msg, "%s bucket %u", ls[i]["level"].c_str(), b);
      const uint16_t voice = static_cast<uint16_t>(kAnalysed | b << 6);
      const uint16_t scale = static_cast<uint16_t>(kAnalysed | b << 11);
      TEST_ASSERT_EQUAL_MESSAGE(b >= need, matches(voice, instrumental), msg);
      TEST_ASSERT_EQUAL_MESSAGE(b < under, matches(voice, vocals), msg);
      TEST_ASSERT_EQUAL_MESSAGE(b >= need, matches(scale, happy), msg);
      TEST_ASSERT_EQUAL_MESSAGE(b < under, matches(scale, neutral), msg);
    }
  }
  // The bars, on / mirror, through the writer: Instrumental holds at on and
  // not a step under it; Vocals a step under mirror and not at it.
  const Value& bs = m["bars"];
  TEST_ASSERT_EQUAL_size_t(3, bs.size());
  for (size_t i = 0; i < bs.size(); ++i) {
    const uint16_t code = static_cast<uint16_t>(static_cast<unsigned>(levelNamed(bs[i]["level"].str())) << 9);
    const double on = bs[i]["on"].num(), mirror = bs[i]["mirror"].num();
    const Filter instrumental = Filter::unpack(0x010 | code), vocals = Filter::unpack(0x020 | code);
    const double at[4] = {on, on - 0.0001, mirror - 0.0001, mirror};
    uint16_t w[4];
    for (int k = 0; k < 4; ++k) {
      const double s[8] = {0, 0, 0, 0, 0, 0, 0, at[k]};
      w[k] = wordOf(s);
    }
    const char* level = bs[i]["level"].c_str();
    TEST_ASSERT_TRUE_MESSAGE(matches(w[0], instrumental), level);
    TEST_ASSERT_FALSE_MESSAGE(matches(w[1], instrumental), level);
    TEST_ASSERT_TRUE_MESSAGE(matches(w[2], vocals), level);
    TEST_ASSERT_FALSE_MESSAGE(matches(w[3], vocals), level);
  }
}

void test_the_moods_json_rows_tails_filters_and_albums() {
  const Value m = moodsJson();
  const Value& ps = m["predicates"];
  const Value& ls = m["levels"];
  char msg[200];

  // Every row: the writer's tail of its scores, the reader's word of the
  // tail, and its 33 checks, on the word and on the test's SQL port.
  const Value& rows = m["rows"];
  TEST_ASSERT_TRUE(rows.size() > 0);
  std::vector<uint16_t> words;
  uint32_t checks = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Value& r = rows[i];
    std::snprintf(msg, sizeof msg, "rows[%u] %s", static_cast<unsigned>(i), r["note"].c_str());
    double s[8];
    const double* scores = scoresOf(r["scores"], s) ? s : nullptr;
    uint8_t want[8], t[8];
    tailBytes(r["tail"], want);
    tailOf(scores, t);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, t, 8, msg);
    const uint16_t word = static_cast<uint16_t>(r["word"].u64());
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(word, wordOfTail(t), msg);
    words.push_back(word);
    for (size_t l = 0; l < ls.size(); ++l) {
      const Value& holds = r["holds"][ls[l]["level"].c_str()];
      const uint16_t code = static_cast<uint16_t>(ls[l]["code"].u64() << 9);
      for (size_t p = 0; p < ps.size(); ++p) {
        const std::string& name = ps[p]["name"].str();
        const uint16_t q = static_cast<uint16_t>(ps[p]["q"].u64() | code);
        const bool expect = listed(holds, name);
        std::snprintf(msg, sizeof msg, "rows[%u] %s at %s", static_cast<unsigned>(i), name.c_str(),
                      ls[l]["level"].c_str());
        TEST_ASSERT_EQUAL_MESSAGE(expect, matches(word, Filter::unpack(q)), msg);
        TEST_ASSERT_EQUAL_MESSAGE(expect, refHolds(scores, q), msg);
        ++checks;
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT32(rows.size() * 33, checks);

  // The tails a reader takes or refuses.
  const Value& tails = m["tails"];
  TEST_ASSERT_TRUE(tails.size() > 0);
  for (size_t i = 0; i < tails.size(); ++i) {
    std::snprintf(msg, sizeof msg, "tails[%u] %s", static_cast<unsigned>(i), tails[i]["note"].c_str());
    uint8_t t[8];
    tailBytes(tails[i]["tail"], t);
    TEST_ASSERT_EQUAL_MESSAGE(tails[i]["valid"].boolean(), tailValid(t), msg);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(tails[i]["word"].u64(), wordOfTail(t), msg);
  }

  // The filters: q's validity, as read (unpack, then pack), its fields, and
  // the rows it lets through, every row's word tried.
  const Value& fs = m["filters"];
  TEST_ASSERT_TRUE(fs.size() > 0);
  for (size_t i = 0; i < fs.size(); ++i) {
    const Value& e = fs[i];
    std::snprintf(msg, sizeof msg, "filters[%u] %s %s", static_cast<unsigned>(i), e["q"].c_str(), e["note"].c_str());
    const uint16_t q = static_cast<uint16_t>(e["q"].u64());
    TEST_ASSERT_EQUAL_MESSAGE(e["valid"].boolean(), valid(q), msg);
    const Filter f = Filter::unpack(q);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(e["reads"].u64(), f.pack(), msg);
    TEST_ASSERT_EQUAL_MESSAGE(e["active"].boolean(), f.active(), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(levelNamed(e["level"].str())), static_cast<int>(f.level), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(scaleNamed(nameOf(e["valence"]), "happy", "sad")),
                              static_cast<int>(f.valence), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(scaleNamed(nameOf(e["energy"]), "relaxed", "aggressive")),
                              static_cast<int>(f.energy), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(voiceNamed(nameOf(e["voice"]))), static_cast<int>(f.voice), msg);
    uint8_t tags = 0;
    for (size_t k = 0; k < e["tags"].size(); ++k) tags |= tagNamed(e["tags"][k].str());
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(tags, f.tags, msg);
    std::vector<uint32_t> want, got;
    for (size_t k = 0; k < e["matches"].size(); ++k) want.push_back(static_cast<uint32_t>(e["matches"][k].u64()));
    for (uint32_t k = 0; k < words.size(); ++k)
      if (matches(words[k], f)) got.push_back(k);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), got.size(), msg);
    if (!want.empty()) TEST_ASSERT_EQUAL_UINT32_ARRAY_MESSAGE(want.data(), got.data(), want.size(), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), count(words.data(), static_cast<uint32_t>(words.size()), f, nullptr),
                                     msg);
  }

  // The albums: tally() over their rows' words, and the majority.
  const Value& as = m["albums"];
  TEST_ASSERT_TRUE(as.size() > 0);
  for (size_t i = 0; i < as.size(); ++i) {
    const Value& a = as[i];
    std::snprintf(msg, sizeof msg, "albums[%u] %s", static_cast<unsigned>(i), a["note"].c_str());
    std::vector<uint32_t> ids;
    for (size_t k = 0; k < a["rows"].size(); ++k) ids.push_back(static_cast<uint32_t>(a["rows"][k].u64()));
    for (uint32_t id : ids) TEST_ASSERT_TRUE_MESSAGE(id < words.size(), msg);
    const Tally t = tally(words.data(), ids.data(), static_cast<uint32_t>(ids.size()),
                          Filter::unpack(static_cast<uint16_t>(a["q"].u64())));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a["pass"].u64(), t.pass, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a["analysed"].u64(), t.analysed, msg);
    TEST_ASSERT_EQUAL_MESSAGE(a["matches"].boolean(), majority(t), msg);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pack_and_unpack_every_q);
  RUN_TEST(test_a_level_alone_is_not_active);
  RUN_TEST(test_the_worked_example);
  RUN_TEST(test_the_float_traps_and_the_clamp);
  RUN_TEST(test_every_score_to_4_and_5_places);
  RUN_TEST(test_the_scale_bytes_and_the_order_bits);
  RUN_TEST(test_every_byte_at_every_tail_position);
  RUN_TEST(test_the_word_buckets_and_nulls);
  RUN_TEST(test_matches_the_raw_predicate);
  RUN_TEST(test_a_level_alone_filters_nothing);
  RUN_TEST(test_count);
  RUN_TEST(test_the_album_majority);
  RUN_TEST(test_the_spec_literals);
  RUN_TEST(test_the_moods_json_values_levels_and_bars);
  RUN_TEST(test_the_moods_json_rows_tails_filters_and_albums);
  return UNITY_END();
}
