// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "MoodFilter.h"

#include <cmath>

namespace moodfilter {

namespace {

// The tail's bytes and flag bits (5.3).
enum : uint8_t { kFlags, kValence, kEnergy, kInstrumental, kAcousticByte, kPartyByte, kDanceableByte, kReserved };
constexpr uint8_t kPresent = 1, kSadOverHappy = 2, kAggressiveOverRelaxed = 4, kFlagsReserved = 0xF8;
constexpr uint32_t kScaleNull = 4;  // a scale field's NULL: side 1 at bucket 0, which never happens

// need / under in buckets, by Level: Standard, Strict, Loose (5.4).
struct Bars {
  uint8_t need, under;
};
constexpr Bars kBars[3] = {{2, 2}, {3, 1}, {1, 3}};

Bars barsOf(Level l) {
  const uint8_t i = static_cast<uint8_t>(l);
  return kBars[i < 3 ? i : 0];
}

uint32_t bucketOf(uint8_t v) { return v < 30 ? 0 : v < 50 ? 1 : v < 70 ? 2 : 3; }

uint32_t scaleField(uint8_t v, bool second) {
  if (v == kNull) return kScaleNull;  // its order bit is ignored
  const uint32_t b = bucketOf(v);
  return b && second ? 4 | b : b;
}

uint32_t tagField(uint8_t v) { return v == kNull ? 0 : bucketOf(v); }

// 5.3's value, the product rounded and then the slack added, as the
// writers compute it. A compiler that fuses the two into one multiply-add
// gives the same bytes on the 4- and 5-place grids
// (test_every_score_to_4_and_5_places).
uint8_t valueOf(double score) {
  if (std::isnan(score)) return kNull;
  const double scaled = score * 100.0;
  const double v = std::floor(scaled + 1e-9);
  return v <= 0 ? 0 : v >= 100 ? 100 : static_cast<uint8_t>(v);
}

// The larger of a scale's two heads, and whether the second beat the first.
void scaleOf(double first, double second, uint8_t* value, bool* secondOver) {
  if (std::isnan(first) || std::isnan(second)) {
    *value = kNull;
    *secondOver = false;
    return;
  }
  *secondOver = second > first;
  *value = valueOf(*secondOver ? second : first);
}

// A filter as the field values each group holds for, a bit each, so one
// word is a handful of shifts (count() runs it over every track). A scale
// field is 0-7 (4 NULL); the voice is NULL<<2 | the instrumental bucket; a
// tag is its bucket. An inactive filter's Test asks for no kAnalysed and
// keeps every mask full, so it holds for every word (4.1).
struct Test {
  uint16_t analysed = kAnalysed;
  uint8_t valence = 0xFF, energy = 0xFF, voice = 0xFF;
  uint8_t acoustic = 0xF, party = 0xF, danceable = 0xF;

  bool holds(uint16_t w) const {
    return (w & analysed) == analysed && (valence >> ((w >> 11) & 7) & 1) && (energy >> ((w >> 8) & 7) & 1) &&
           (voice >> (((w >> 12) & 4) | ((w >> 6) & 3)) & 1) && (acoustic >> ((w >> 4) & 3) & 1) &&
           (party >> ((w >> 2) & 3) & 1) && (danceable >> (w & 3) & 1);
  }
};

uint8_t atLeast(uint8_t need) { return static_cast<uint8_t>((0xFu << need) & 0xFu); }  // buckets >= need
uint8_t below(uint8_t under) { return static_cast<uint8_t>((1u << under) - 1u); }       // buckets < under

uint8_t scaleMask(Scale s, Bars b) {
  switch (s) {
    case Scale::First: return atLeast(b.need);
    case Scale::Second: return static_cast<uint8_t>(atLeast(b.need) << 4);
    case Scale::Neutral: return static_cast<uint8_t>((below(b.under) | below(b.under) << 4) & ~(1u << kScaleNull));
    default: return 0xFF;
  }
}

Test testOf(const Filter& f) {
  const Bars b = barsOf(f.level);
  Test t;
  if (!f.active()) {
    t.analysed = 0;
    return t;
  }
  t.valence = scaleMask(f.valence, b);
  t.energy = scaleMask(f.energy, b);
  if (f.voice == Voice::Instrumental) t.voice = atLeast(b.need);
  if (f.voice == Voice::Vocals) t.voice = below(b.under);
  if (f.tags & kAcoustic) t.acoustic = atLeast(b.need);
  if (f.tags & kParty) t.party = atLeast(b.need);
  if (f.tags & kDanceable) t.danceable = atLeast(b.need);
  return t;
}

}  // namespace

bool Filter::active() const {
  return valence != Scale::Any || energy != Scale::Any || voice != Voice::Any || (tags & 7);
}

uint16_t Filter::pack() const {
  return static_cast<uint16_t>((static_cast<uint32_t>(valence) & 3) | (static_cast<uint32_t>(energy) & 3) << 2 |
                               (static_cast<uint32_t>(voice) & 3) << 4 | (tags & 7u) << 6 |
                               (static_cast<uint32_t>(level) & 3) << 9);
}

Filter Filter::unpack(uint16_t q) {
  Filter f;
  if (!valid(q)) return f;
  f.valence = static_cast<Scale>(q & 3);
  f.energy = static_cast<Scale>(q >> 2 & 3);
  f.voice = static_cast<Voice>(q >> 4 & 3);
  f.tags = static_cast<uint8_t>(q >> 6 & 7);
  f.level = static_cast<Level>(q >> 9 & 3);
  return f;
}

bool valid(uint16_t q) { return (q >> 11) == 0 && (q >> 4 & 3) != 3 && (q >> 9 & 3) != 3; }

bool tailValid(const uint8_t t[8]) {
  if (!(t[kFlags] & kPresent) || (t[kFlags] & kFlagsReserved) || t[kReserved] != 0) return false;
  for (int i = kValence; i <= kDanceableByte; ++i)
    if (t[i] > 100 && t[i] != kNull) return false;
  return true;
}

uint16_t wordOfTail(const uint8_t t[8]) {
  if (!tailValid(t)) return 0;
  uint32_t w = kAnalysed;
  w |= scaleField(t[kValence], t[kFlags] & kSadOverHappy) << 11;
  w |= scaleField(t[kEnergy], t[kFlags] & kAggressiveOverRelaxed) << 8;
  if (t[kInstrumental] == kNull) {
    w |= kVoiceNull;
  } else {
    w |= bucketOf(t[kInstrumental]) << 6;
  }
  w |= tagField(t[kAcousticByte]) << 4;
  w |= tagField(t[kPartyByte]) << 2;
  w |= tagField(t[kDanceableByte]);
  return static_cast<uint16_t>(w);
}

void tailOf(const double* scores, uint8_t out[8]) {
  for (int i = 0; i < 8; ++i) out[i] = 0;
  if (!scores) return;
  bool sadOver = false, aggressiveOver = false;
  scaleOf(scores[kHeadHappy], scores[kHeadSad], &out[kValence], &sadOver);
  scaleOf(scores[kHeadRelaxed], scores[kHeadAggressive], &out[kEnergy], &aggressiveOver);
  out[kFlags] = static_cast<uint8_t>(kPresent | (sadOver ? kSadOverHappy : 0) |
                                     (aggressiveOver ? kAggressiveOverRelaxed : 0));
  out[kInstrumental] = valueOf(scores[kHeadInstrumental]);
  out[kAcousticByte] = valueOf(scores[kHeadAcoustic]);
  out[kPartyByte] = valueOf(scores[kHeadParty]);
  out[kDanceableByte] = valueOf(scores[kHeadDanceable]);
}

bool matches(uint16_t word, const Filter& f) { return testOf(f).holds(word); }

Tally tally(const uint16_t* words, const uint32_t* ids, uint32_t n, const Filter& f) {
  const Test t = testOf(f);
  Tally out;
  out.unfiltered = !f.active();
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t w = words[ids[i]];
    const uint32_t analysed = w >> 15;
    out.analysed += analysed;
    out.pass += analysed && t.holds(w);  // only analysed songs count
  }
  return out;
}

uint32_t count(const uint16_t* words, uint32_t n, const Filter& f, uint32_t* analysed) {
  const Test t = testOf(f);
  uint32_t pass = 0, seen = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t w = words[i];
    seen += w >> 15;
    pass += t.holds(w);
  }
  if (analysed) *analysed = seen;
  return pass;
}

}  // namespace moodfilter
