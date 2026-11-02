// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the waveform index (lib/core/CardWaves, MPWF v1,
// docs/WAVEFORM-INDEX.md), written from that document and the shared fixtures
// (test/fixtures/card): the writer makes the goldens byte for byte from their
// descriptions and the reader reads them back; every mpwf-* and msmf-waves-*
// hardening file reads for its listed reason, and its lookup as listed; every
// vector of the "mpwf" group (the names and sizes, the bars, the slot CRCs,
// each step of the search and each lookup's rows, the row limit, the root's
// row); the search against a walk from row 0 on tables of every shape, and
// what it costs in slot reads (the 20,000 rows of the vectors, and paths of
// the synthetic card's shape); a failed read, a damaged slot and a broken
// writer each in its place; the writer's edge cases; no heap and little stack.
// The byte loops over the goldens (truncation, flipped bits, the resealed
// fuzz) are test_card_files'; device.txt is test_card_contract's and the
// root's entry test_card_jobs'. When to write a new file (the `commit`
// vectors) binds the writer: tools/test_mpwf.py's.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardManifest.h"
#include "CardWaves.h"

namespace cc = cardcontract;
namespace mpwf = cardcontract::mpwf;
namespace msmf = cardcontract::msmf;
using minijson::Value;
using Bytes = std::vector<uint8_t>;

// Every allocation counted: the reader is to make none.
namespace {
size_t gAllocs = 0;
}
void* operator new(size_t n) {
  ++gAllocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > b.size()) b.resize(offset + n);
    if (n) std::memcpy(b.data() + offset, data, n);
    return true;
  }
  Bytes b;
};

Bytes fixture(const std::string& rel) {
  const std::string s = cardfixtures::bytes(rel);
  TEST_ASSERT_FALSE_MESSAGE(s.empty(), rel.c_str());
  return Bytes(s.begin(), s.end());
}
Bytes golden(const std::string& name) { return fixture("golden/" + name); }

Value load(const std::string& rel) {
  Value v;
  TEST_ASSERT_TRUE_MESSAGE(cardfixtures::json(rel, &v), rel.c_str());
  return v;
}

const Value& vectors() {
  static const Value v = load("vectors.json");
  return v["mpwf"];
}

Bytes hexBytes(const std::string& s) {
  Bytes out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

uint32_t u32(const Value& v) { return static_cast<uint32_t>(v.u64()); }

const char* lookupName(mpwf::Lookup l) {
  switch (l) {
    case mpwf::Lookup::Hit: return "hit";
    case mpwf::Lookup::Miss: return "miss";
    case mpwf::Lookup::Damaged: return "damaged";
    case mpwf::Lookup::Broken: return "broken";
    case mpwf::Lookup::Io: return "io";
  }
  return "?";
}

const uint8_t* slotAt(const Bytes& file, uint32_t row) {
  return file.data() + mpwf::kSlotsOffset + row * mpwf::kSlotBytes;
}

uint64_t hashAt(const Bytes& file, uint32_t row) { return cc::get64(slotAt(file, row)); }

// The reads of a file, as rows, with every read of a slot's whole bytes at
// its place; anything past the first 256 bytes that isn't one is counted.
class RowLog : public cc::Source {
public:
  explicit RowLog(const Bytes& b) : m_(b.data(), static_cast<uint32_t>(b.size())) {}
  uint32_t size() const override { return m_.size(); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (offset >= mpwf::kSlotsOffset) {
      if (n == mpwf::kSlotBytes && (offset - mpwf::kSlotsOffset) % mpwf::kSlotBytes == 0)
        rows.push_back((offset - mpwf::kSlotsOffset) / mpwf::kSlotBytes);
      else
        ++odd;
    }
    return m_.read(offset, out, n);
  }
  std::vector<uint32_t> rows;
  uint32_t odd = 0;

private:
  cc::MemSource m_;
};

// A source whose reads fail on demand: `failAt` counts them down, the one that
// reaches 0 and every read while `fail` is set failing.
class FlakySource : public cc::Source {
public:
  explicit FlakySource(const Bytes& b) : m_(b.data(), static_cast<uint32_t>(b.size())) {}
  uint32_t size() const override { return m_.size(); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (fail || failAt == 1) {
      failAt = 0;
      return false;
    }
    if (failAt) --failAt;
    return m_.read(offset, out, n);
  }
  bool fail = false;
  uint32_t failAt = 0;

private:
  cc::MemSource m_;
};

// A description (13.2) as the writer's input: each track's bars from its peaks.
struct Described {
  std::vector<std::array<uint8_t, mpwf::kBarCount>> bars;
  std::vector<mpwf::RowIn> rows;
  mpwf::TableIn in;

  explicit Described(const Value& d) {
    const Value& ts = d["tracks"];
    bars.resize(ts.size());
    rows.resize(ts.size());
    for (size_t i = 0; i < ts.size(); ++i) {
      const Value& t = ts[i];
      Bytes peaks;
      for (size_t k = 0; k < t["peaks"].size(); ++k) peaks.push_back(static_cast<uint8_t>(t["peaks"][k].u64()));
      mpwf::barsFrom(peaks.data(), static_cast<uint32_t>(peaks.size()), bars[i].data());
      rows[i].pathHash = t.has("hash") ? t["hash"].u64() : cc::pathHash(t["path"].c_str());
      rows[i].lengthMs = u32(t["lengthMs"]);
      rows[i].bars = bars[i].data();
    }
    in.generation = u32(d["generation"]);
    in.cardId = d["cardId"].u64();
    in.minor = static_cast<uint16_t>(d["minor"].u64());
    in.rows = rows.data();
    in.rowCount = static_cast<uint32_t>(rows.size());
  }
};

// A file of these hashes (the bars don't matter: all zero).
Bytes tableOf(const std::vector<uint64_t>& hashes) {
  static const uint8_t kBars[mpwf::kBarCount] = {};
  std::vector<mpwf::RowIn> rows(hashes.size());
  for (size_t i = 0; i < hashes.size(); ++i) {
    rows[i].pathHash = hashes[i];
    rows[i].bars = kBars;
  }
  mpwf::TableIn in;
  in.generation = 9;
  in.rows = rows.data();
  in.rowCount = static_cast<uint32_t>(rows.size());
  VecSink out;
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mpwf::write(out, in, nullptr, nullptr, &error), error ? error : "");
  return out.b;
}

// Reads a file as a tool does (uses "all": every slot, then WVSL's CRC) or as
// the device does (the frame alone).
cc::Why readAs(const Bytes& file, bool device) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mpwf::Table t;
  const cc::Why w = t.open(src);
  if (w != cc::Why::Ok || device) return w;
  uint8_t scratch[1024];
  return t.check(scratch, sizeof(scratch));
}

// Two files read the same (the hardening's `same`): every slot's hash, length
// and bars, found by its hash.
void assertSameSlots(const Bytes& want, const Bytes& got, const char* what) {
  cc::MemSource ws(want.data(), static_cast<uint32_t>(want.size())), gs(got.data(), static_cast<uint32_t>(got.size()));
  mpwf::Table w, g;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(w.open(ws)), what);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(g.open(gs)), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(w.rows(), g.rows(), what);
  for (uint32_t r = 0; r < w.rows(); ++r) {
    mpwf::Slot s;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("hit", lookupName(g.find(hashAt(want, r), &s)), what);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(slotAt(want, r), s.bytes, mpwf::kCrcAt, what);
  }
}

// What a lookup costs, in slot reads.
struct Reads {
  uint64_t sum = 0;
  uint32_t n = 0, most = 0, atMost5 = 0;
  void add(uint32_t r) {
    sum += r;
    ++n;
    most = std::max(most, r);
    atMost5 += r <= 5;
  }
  double mean() const { return n ? static_cast<double>(sum) / n : 0; }
};

// Each hash looked up in `file`; each must read `want`.
Reads lookUp(const Bytes& file, const std::vector<uint64_t>& hashes, mpwf::Lookup want) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mpwf::Table t;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
  Reads out;
  mpwf::Slot s;
  for (uint64_t h : hashes) {
    TEST_ASSERT_EQUAL_STRING(lookupName(want), lookupName(t.find(h, &s)));
    if (want == mpwf::Lookup::Hit) TEST_ASSERT_TRUE(s.pathHash() == h);
    out.add(t.reads());
  }
  return out;
}

void report(const char* what, const Reads& hits, const Reads& misses) {
  std::printf("[waves] %s: %u rows: a hit reads %.2f slots on average, %u at most, %.0f %% at most 5; "
              "%u misses: %.2f on average, %u at most\n",
              what, static_cast<unsigned>(hits.n), hits.mean(), static_cast<unsigned>(hits.most),
              100.0 * hits.atMost5 / hits.n, static_cast<unsigned>(misses.n), misses.mean(),
              static_cast<unsigned>(misses.most));
  std::fflush(stdout);
}

uint64_t splitMix64(uint64_t* state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// ---------------------------------------------------------------------------
// Paths in the synthetic card's shape (tools/synthcard.py: the library it
// measured, aggregates only). Its rates, with its words, on a deterministic
// generator of this test's own: artist folders from made-up syllables, the
// measured albums per artist and tracks per album, its album folder forms and
// file name shapes, most tracks at depth 2, disc folders, loose tracks. Every
// name is made up.
// ---------------------------------------------------------------------------
struct Weight {
  int value;
  int weight;
};
// tools/synthcard.py's TRACK_WEIGHTS and ALBUMS_PER_ARTIST (x 10, to integers)
// and NAME_SHAPES (x 10), in CardPaths::fileName()'s order.
const Weight kTracks[] = {
    {1, 680},   {2, 140},   {3, 160},  {4, 200},  {5, 240},  {6, 300},  {7, 420},  {8, 640}, {9, 900},
    {10, 1180}, {11, 1180}, {12, 1120}, {13, 860}, {14, 620}, {15, 450}, {16, 330}, {17, 240}, {18, 180},
    {19, 130},  {20, 90},   {21, 70},  {22, 60},  {23, 50},  {24, 40},  {25, 30},  {26, 30}, {27, 20},
    {28, 20},   {29, 15},   {30, 15},  {32, 10},  {34, 10},  {36, 7},   {38, 5},   {40, 4},  {43, 3}};
const Weight kAlbumsPerArtist[] = {{1, 500}, {2, 180}, {3, 100}, {4, 60}, {5, 40}, {6, 30}, {7, 20}, {8, 20}, {9, 12},
                                   {10, 10}, {12, 8},  {14, 6},  {16, 5}, {20, 4}, {25, 3}, {32, 2}, {40, 1}};
const Weight kShapes[] = {{0, 400}, {1, 150}, {2, 50}, {3, 20}, {4, 10}, {5, 100},
                          {6, 35},  {7, 40},  {8, 25}, {9, 4},  {10, 75}, {11, 20}};
const char* const kSyllables[] = {
    "ba",  "bel", "bri", "cal", "cor", "dal", "del", "dra", "el",  "en",  "fa",  "fen", "gal", "gor", "ha",  "hel",
    "ir",  "is",  "ja",  "ka",  "kel", "kor", "la",  "lin", "lo",  "lu",  "ma",  "mer", "mir", "mo",  "na",  "nel",
    "nor", "ol",  "os",  "pa",  "pel", "ra",  "rel", "ro",  "ru",  "sa",  "sel", "sil", "so",  "ta",  "tel", "tor",
    "tu",  "va",  "vel", "vin", "vor", "wa",  "wel", "ya",  "yel", "za",  "zel", "zor", "rin", "mon", "ver", "lis"};
const char* const kNouns[] = {
    "river",  "morning", "house",   "window", "door",    "room",   "bridge",  "tower",   "station", "field",
    "forest", "valley",  "ocean",   "island", "shore",   "desert", "lake",    "rain",    "snow",    "storm",
    "thunder", "wind",   "cloud",   "sky",    "star",    "moon",   "sun",     "fire",    "stone",   "iron",
    "gold",   "letter",  "song",    "story",  "dream",   "memory", "shadow",  "mirror",  "circle",  "signal",
    "echo",   "voice",   "heart",   "hand",   "ghost",   "stranger", "friend", "machine", "engine",  "radio",
    "camera", "planet",  "comet",   "atlas",  "compass", "anchor", "arrow",   "feather", "flower",  "rose",
    "tiger",  "wolf",    "fox",     "raven",  "crow",    "swan",   "heron",   "falcon",  "whale",   "holiday",
    "harbour", "garden", "street",  "road",   "city",    "mountain"};
const char* const kAdjectives[] = {
    "glass",  "silver", "paper",  "empty",  "golden", "quiet",  "broken", "northern", "electric", "hollow", "velvet",
    "distant", "wild",  "open",   "slow",   "little", "bright", "cold",   "lonely",   "secret",   "burning", "falling",
    "hidden", "last",   "early",  "late",   "blue",   "green",  "red",    "black",    "white",    "grey",   "amber",
    "violet", "crimson", "heavy", "soft",   "long",   "deep",   "high",   "new",      "old",      "young"};
const char* const kBands[] = {"Trio", "Quartet", "Ensemble", "Collective", "Orchestra",
                              "Band", "Project", "Sound",    "Society",    "Club"};
const char* const kSuffixes[] = {"(Live)", "(Remix)", "(Acoustic)", "(Demo)",           "[FLAC]",       "[MP3]",
                                 "(320)",  "[V0]",    "(CD)",       "(Deluxe Edition)", "(Remastered)", "(EP)"};

class CardPaths {
public:
  explicit CardPaths(uint32_t seed) : rng_(seed) {}

  std::vector<std::string> make(size_t count) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& rel) {
      if (out.size() < count && seen.insert(rel).second) out.push_back(rel);
    };
    while (out.size() < count) {
      const std::string artist = artistName();
      if (pct(1.7))  // loose tracks: a dozen artist folders in 705
        for (int i = pick(2, 4); i > 0; --i) add(artist + "/" + title() + ".mp3");
      for (int albums = weighted(kAlbumsPerArtist); albums > 0 && out.size() < count; --albums) {
        const std::string tagAlbum = phrase();
        const std::string album = albumFolder(artist, tagAlbum);
        const char* ext = albumExtension();
        const int shape = weighted(kShapes);
        const int tracks = weighted(kTracks);
        // 30 + 8 albums in 1,791 sit in disc folders, and as many hold two discs in one folder.
        const int discs = pct(2.1) ? pick(2, 4) : 1;
        const bool discNames = discs == 1 && pct(2.0);
        for (int d = 1; d <= discs; ++d) {
          const std::string dir = artist + "/" + album + (discs > 1 ? "/" + discFolder(d) : "");
          for (int n = 1; n <= tracks; ++n) {
            std::string name = fileName(shape, n, artist, tagAlbum, title());
            if (discNames) name = std::to_string(1 + n % 2) + "-" + two(n) + " " + title();
            add(dir + "/" + name + ext);
          }
        }
      }
    }
    return out;
  }

private:
  int pick(int lo, int hi) { return lo + static_cast<int>(rng_() % static_cast<uint32_t>(hi - lo + 1)); }
  bool pct(double p) { return rng_() % 100000 < static_cast<uint32_t>(p * 1000); }
  template <size_t N>
  const char* choice(const char* const (&list)[N]) {
    return list[rng_() % N];
  }
  template <size_t N>
  int weighted(const Weight (&w)[N]) {
    int total = 0;
    for (const Weight& e : w) total += e.weight;
    int x = pick(0, total - 1);
    for (const Weight& e : w) {
      x -= e.weight;
      if (x < 0) return e.value;
    }
    return w[N - 1].value;
  }
  static std::string capital(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
  }
  static std::string capitals(const std::string& s) {
    std::string out;
    size_t at = 0;
    while (at <= s.size()) {
      size_t sp = s.find(' ', at);
      if (sp == std::string::npos) sp = s.size();
      if (at) out += ' ';
      out += capital(s.substr(at, sp - at));
      at = sp + 1;
    }
    return out;
  }
  static std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }
  std::string word(int syllables = 0) {
    static const int kLengths[] = {2, 2, 2, 3, 3, 1};
    int n = syllables ? syllables : kLengths[rng_() % 6];
    std::string w;
    while (n-- > 0) w += choice(kSyllables);
    return capital(w);
  }
  std::string artistName() {
    const int k = pick(0, 99);
    std::string a;
    if (k < 45) a = word() + " " + word();
    else if (k < 64) a = word(pick(2, 3));
    else if (k < 72) a = "The " + word() + "s";
    else if (k < 75) a = word() + " & the " + word() + "s";
    else if (k < 79) a = word() + " " + word() + " & " + word() + " " + word();
    else if (k < 87) a = word() + " " + choice(kBands);
    else if (k < 89) a = std::string(pct(50) ? "DJ " : "MC ") + word();
    else if (k < 95) a = word() + " " + word() + " " + word(1);
    else if (k < 97) a = word() + " " + std::to_string(pick(2, 99));
    else if (k < 98) a = "the " + word();
    else a = word() + " \xC3\x89" + word();  // an accent, UTF-8 as the card stores it
    return a;
  }
  std::string phrase() {
    const int k = pick(0, 99);
    std::string p;
    if (k < 26) p = std::string(choice(kAdjectives)) + " " + choice(kNouns);
    else if (k < 40) p = std::string(choice(kNouns)) + " of the " + choice(kAdjectives) + " " + choice(kNouns);
    else if (k < 52) p = std::string("the ") + choice(kAdjectives) + " " + choice(kNouns);
    else if (k < 60) p = choice(kNouns);
    else if (k < 78) {
      for (int n = pick(2, 5); n > 0; --n) p += std::string(p.empty() ? "" : " ") + (pct(50) ? choice(kNouns) : choice(kAdjectives));
    } else if (k < 89) p = std::string(choice(kNouns)) + " in the " + choice(kNouns);
    else if (k < 95) p = std::string(choice(kNouns)) + ", " + choice(kNouns) + " & " + choice(kNouns);
    else p = std::string(choice(kAdjectives)) + " " + choice(kNouns) + " Vol. " + std::to_string(pick(1, 9));
    return capitals(p);
  }
  std::string title() {
    std::string t = phrase();
    if (pct(2.3)) t = std::to_string(pick(2, 99)) + " " + t;
    if (pct(6)) t += " (feat. " + word() + " " + word() + ")";
    if (pct(6)) t += std::string(" ") + choice(kSuffixes);
    return t;
  }
  std::string albumFolder(const std::string& artist, const std::string& tag) {
    const int k = pick(0, 99);
    const std::string prefix = pct(40) ? artist + " - " : "";
    const std::string year = std::to_string(pick(1958, 2024));
    if (k < 38) return tag;
    if (k < 73) {
      const int form = pick(0, 99);
      if (form < 55) return prefix + tag + " (" + year + ")";
      if (form < 70) return prefix + year + " - " + tag;
      if (form < 85) return prefix + tag + " [" + year + "]";
      if (form < 90) return prefix + "[" + year + "] " + tag;
      return prefix + tag + " - " + year;
    }
    if (k < 89) return prefix + tag + " " + choice(kSuffixes);
    if (k < 93) return pct(50) ? "Untitled" : tag + " Sessions";
    std::string lower = tag;
    for (char& c : lower)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return lower;
  }
  std::string discFolder(int d) {
    const std::string n = std::to_string(d);
    const int k = pick(0, 3);
    return k == 0 ? "CD" + n : k == 1 ? "CD " + n : k == 2 ? "Disc " + n : "Disc " + n + " - " + phrase();
  }
  const char* albumExtension() {
    if (pct(5.9)) return ".flac";
    if (pct(1)) return ".opus";
    if (pct(0.6)) return ".m4a";
    return pct(2) ? ".MP3" : ".mp3";
  }
  std::string fileName(int shape, int n, const std::string& artist, const std::string& album, const std::string& t) {
    const std::string nn = two(n);
    switch (shape) {
      case 0: return nn + " - " + t;
      case 1: return nn + " " + t;
      case 2: return nn + ". " + t;
      case 3: return nn + "-" + t;
      case 4: return nn + "_" + t;
      case 5: return nn + " - " + artist + " - " + t;
      case 6: return nn + " " + artist + " - " + t;
      case 7: return artist + " - " + nn + " - " + t;
      case 8: return artist + " - " + album + " - " + nn + " - " + t;
      case 9: return "(" + nn + ") " + t;
      case 10: return t;
      default: return artist + " - " + t;
    }
  }

  std::mt19937 rng_;
};

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The constants, the names and the sizes (3, 4, 7.1, 10).
// ---------------------------------------------------------------------------
void test_the_constants() {
  TEST_ASSERT_EQUAL_HEX32(cc::fourcc("MPWF"), cc::kMagicMpwf);
  TEST_ASSERT_EQUAL_HEX32(cc::kMagicMpwf, mpwf::kSpec.magic);
  TEST_ASSERT_EQUAL_UINT16(1, mpwf::kSpec.major);
  TEST_ASSERT_EQUAL_UINT32(224, mpwf::kSpec.headerBytes);
  TEST_ASSERT_EQUAL_UINT32(1, mpwf::kSpec.count);
  TEST_ASSERT_EQUAL_HEX32(cc::fourcc("WVSL"), mpwf::kSpec.sections[0].type);
  TEST_ASSERT_EQUAL_UINT32(256, mpwf::kSpec.sections[0].stride);
  TEST_ASSERT_TRUE(mpwf::kSpec.sections[0].required);
  TEST_ASSERT_EQUAL_UINT32(256, mpwf::kSlotsOffset);
  TEST_ASSERT_EQUAL_size_t(256, sizeof(mpwf::Slot));
  // No slot crosses a 512-byte sector: slots on the card start at multiples of 256.
  TEST_ASSERT_EQUAL_UINT32(0, mpwf::kSlotsOffset % mpwf::kSlotBytes);
  // The largest file is under the transfer's 64 MiB, and 6's product in 51 bits.
  TEST_ASSERT_TRUE(mpwf::fileBytesFor(mpwf::kMaxRows) < 64u * 1024 * 1024);
  TEST_ASSERT_TRUE(mpwf::kMaxRows < 1u << 18);
  for (size_t i = 0; i < vectors()["sizes"].size(); ++i) {
    const Value& e = vectors()["sizes"][i];
    TEST_ASSERT_EQUAL_UINT32(e["fileBytes"].u64(), mpwf::fileBytesFor(u32(e["rows"])));
  }
  char name[msmf::Manifest::kNameBytes];
  for (size_t i = 0; i < vectors()["names"].size(); ++i) {
    const Value& e = vectors()["names"][i];
    TEST_ASSERT_TRUE(msmf::companionName(cc::kMagicMpwf, u32(e["generation"]), name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING(e["name"].c_str(), name);
  }
  TEST_ASSERT_FALSE(msmf::companionName(cc::kMagicMpwf, 42, name, 18));  // a short buffer: 18 bytes and the NUL
  TEST_ASSERT_TRUE(msmf::companionName(cc::kMagicMpwf, 42, name, 19));
}

// ---------------------------------------------------------------------------
// The bars (5).
// ---------------------------------------------------------------------------
void test_the_bars() {
  const Value& ranges = vectors()["barRanges"];
  TEST_ASSERT_TRUE(ranges.size() >= 5);
  for (size_t i = 0; i < ranges.size(); ++i)
    for (size_t c = 0; c < ranges[i]["cases"].size(); ++c) {
      const Value& e = ranges[i]["cases"][c];
      uint32_t start = 99, end = 99;
      mpwf::barRange(u32(e["i"]), u32(ranges[i]["m"]), &start, &end);
      TEST_ASSERT_EQUAL_UINT32(e["start"].u64(), start);
      TEST_ASSERT_EQUAL_UINT32(e["end"].u64(), end);
    }
  // M at least 240: the ranges part M, no gap, no overlap. Below: one peak a bar.
  for (uint32_t m = 1; m <= 2400; ++m) {
    uint32_t next = 0;
    for (uint32_t i = 0; i < mpwf::kBarCount; ++i) {
      uint32_t start, end;
      mpwf::barRange(i, m, &start, &end);
      TEST_ASSERT_TRUE(start < end && end <= m);
      if (m >= mpwf::kBarCount) {
        TEST_ASSERT_EQUAL_UINT32(next, start);
        next = end;
      } else {
        TEST_ASSERT_EQUAL_UINT32(1, end - start);
      }
    }
    if (m >= mpwf::kBarCount) TEST_ASSERT_EQUAL_UINT32(m, next);
  }
  // The vectors: the peaks and bars in hex, or a map of the ones not 0.
  const Value& cases = vectors()["bars"];
  TEST_ASSERT_TRUE(cases.size() >= 10);
  for (size_t i = 0; i < cases.size(); ++i) {
    const Value& e = cases[i];
    const uint32_t m = u32(e["m"]);
    Bytes peaks(m, 0), want(mpwf::kBarCount, 0);
    if (e.has("peaks")) peaks = hexBytes(e["peaks"].str());
    if (e.has("bars")) want = hexBytes(e["bars"].str());
    for (const auto& kv : e["peaksAt"].members) peaks.at(std::stoul(kv.first)) = static_cast<uint8_t>(kv.second.u64());
    for (const auto& kv : e["barsAt"].members) want.at(std::stoul(kv.first)) = static_cast<uint8_t>(kv.second.u64());
    TEST_ASSERT_EQUAL_size_t_MESSAGE(m, peaks.size(), e["note"].c_str());
    uint8_t got[mpwf::kBarCount];
    mpwf::barsFrom(peaks.data(), m, got);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), got, mpwf::kBarCount, e["note"].c_str());
  }
  // The integer reading (5) agrees: the largest v with w (2v - 1)^2 <= 4n, or
  // 0, on random peaks of many widths.
  std::mt19937 rng(20261101);
  const uint32_t widths[] = {1, 2, 3, 7, 239, 240, 241, 480, 800, 1000, 1600};
  for (int iter = 0; iter < 300; ++iter)
    for (uint32_t m : widths) {
      Bytes peaks(m);
      const uint32_t top = 1 + rng() % 255;
      for (uint8_t& p : peaks) p = static_cast<uint8_t>(rng() % (top + 1));
      uint8_t got[mpwf::kBarCount];
      mpwf::barsFrom(peaks.data(), m, got);
      for (uint32_t i = 0; i < mpwf::kBarCount; ++i) {
        uint32_t start, end;
        mpwf::barRange(i, m, &start, &end);
        uint64_t n = 0;
        for (uint32_t j = start; j < end; ++j) n += static_cast<uint32_t>(peaks[j]) * peaks[j];
        const uint64_t w = end - start;
        uint32_t v = 255;
        while (v > 0 && w * (2 * v - 1) * (2 * v - 1) > 4 * n) --v;
        TEST_ASSERT_EQUAL_UINT8(v, got[i]);
      }
    }
  // M 0 has no peaks: zeros, not a division by 0.
  uint8_t none[mpwf::kBarCount];
  std::memset(none, 7, sizeof(none));
  mpwf::barsFrom(nullptr, 0, none);
  for (uint8_t b : none) TEST_ASSERT_EQUAL_UINT8(0, b);
}

// ---------------------------------------------------------------------------
// The slot's CRC (4.3) and the section's (4.2).
// ---------------------------------------------------------------------------
void test_the_slot_crcs() {
  const Value& v = vectors()["slot"];
  const Bytes slot = hexBytes(v["bytes"].str());
  TEST_ASSERT_EQUAL_size_t(256, slot.size());
  TEST_ASSERT_EQUAL_HEX32(v["crc"].u64(), mpwf::slotCrc(slot.data(), u32(v["row"])));
  TEST_ASSERT_EQUAL_HEX32(v["crc"].u64(), cc::get32(slot.data() + 252));
  TEST_ASSERT_EQUAL_HEX32(v["unseeded"].u64(), cc::crc32(slot.data(), 252));
  TEST_ASSERT_TRUE(mpwf::slotCrc(slot.data(), u32(v["row"]) + 1) != v["crc"].u64());  // another row's
  const Bytes one = golden(v["file"].str());
  TEST_ASSERT_EQUAL_MEMORY(slot.data(), slotAt(one, 0), 256);
  mpwf::Slot s;
  std::memcpy(s.bytes, slot.data(), 256);
  TEST_ASSERT_TRUE(s.pathHash() == v["pathHash"].u64());
  TEST_ASSERT_TRUE(s.pathHash() == cc::pathHash(v["path"].c_str()));
  TEST_ASSERT_EQUAL_UINT32(v["lengthMs"].u64(), s.lengthMs());
  for (uint32_t b = 0; b < mpwf::kBarCount; ++b) TEST_ASSERT_EQUAL_UINT8(v["barsAll"].u64(), s.bars()[b]);
  TEST_ASSERT_EQUAL_HEX32(v["crc"].u64(), s.crc());

  const Value& rowCrc = vectors()["slotCrc"];
  const Bytes file = golden(rowCrc["file"].str());
  const uint32_t row = u32(rowCrc["row"]);
  TEST_ASSERT_EQUAL_HEX32(rowCrc["crc"].u64(), mpwf::slotCrc(slotAt(file, row), row));
  TEST_ASSERT_EQUAL_HEX32(rowCrc["crc"].u64(), cc::get32(slotAt(file, row) + 252));
  TEST_ASSERT_EQUAL_HEX32(rowCrc["unseeded"].u64(), cc::crc32(slotAt(file, row), 252));

  // WVSL's CRC depends only on the row count when every slot passes: two files
  // that share nothing but that have one, so one header CRC (4.2).
  const Value& sec = vectors()["sectionCrc"];
  TEST_ASSERT_EQUAL_size_t(2, sec.size());
  for (size_t i = 0; i < sec.size(); ++i) {
    const Bytes f = golden(sec[i]["file"].str());
    TEST_ASSERT_EQUAL_UINT32(sec[i]["rows"].u64(), cc::get32(&f[224 + 16]));
    TEST_ASSERT_EQUAL_HEX32(sec[i]["crc"].u64(), cc::get32(&f[224 + 24]));
    TEST_ASSERT_EQUAL_HEX32(sec[i]["crc"].u64(), cc::crc32(f.data() + 256, f.size() - 256));
    std::vector<uint64_t> others;
    for (uint32_t r = 0; r < sec[i]["rows"].u64(); ++r) others.push_back((r + 1) * 0x0123456789ABCDEull);
    const Bytes o = tableOf(others);
    TEST_ASSERT_EQUAL_HEX32(sec[i]["crc"].u64(), cc::get32(&o[224 + 24]));
  }
}

// ---------------------------------------------------------------------------
// The goldens (13.4).
// ---------------------------------------------------------------------------
void test_the_goldens_written() {
  for (const char* name : {"waves-one", "waves-0000002a", "waves-skew"}) {
    const Value d = load(std::string("libraries/") + name + ".json");
    Described x(d);
    const Bytes want = golden(d["file"].str());
    VecSink out;
    uint32_t fileBytes = 0, headerCrc = 0;
    const char* error = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(mpwf::write(out, x.in, &fileBytes, &headerCrc, &error), error ? error : name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), out.b.size(), name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), out.b.data(), want.size(), name);
    TEST_ASSERT_EQUAL_UINT32(want.size(), fileBytes);
    TEST_ASSERT_EQUAL_HEX32(cc::get32(&want[32]), headerCrc);
    // Tracks may come in any order.
    std::mt19937 rng(7);
    std::shuffle(x.rows.begin(), x.rows.end(), rng);
    VecSink again;
    TEST_ASSERT_TRUE(mpwf::write(again, x.in, nullptr, nullptr, nullptr));
    TEST_ASSERT_TRUE_MESSAGE(again.b == want, name);
  }
}

void test_the_goldens_read_back() {
  // 13.4's table.
  struct Row {
    uint64_t hash;
    uint32_t lengthMs;
  };
  const Row rows[7] = {{0x57017C0CC011BB1Bull, 193000}, {0x6137B0E2A6715752ull, 183000}, {0x954A7DE3A079A42Eull, 186000},
                       {0xC48674AC937F2ADAull, 0},      {0xCADB3EFDEF4B2DD9ull, 181000}, {0xDA70B312760188D8ull, 180000},
                       {0xF1B24FC757494F2Bull, 192000}};
  const Bytes file = golden("waves-0000002a.bin");
  TEST_ASSERT_EQUAL_size_t(2048, file.size());
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mpwf::Table t;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
  TEST_ASSERT_EQUAL_UINT32(7, t.rows());
  TEST_ASSERT_EQUAL_HEX32(cc::kMagicMpwf, t.frame().magic);
  TEST_ASSERT_EQUAL_UINT16(1, t.frame().major);
  TEST_ASSERT_EQUAL_UINT16(0, t.frame().minor);
  TEST_ASSERT_EQUAL_UINT32(224, t.frame().headerBytes);
  TEST_ASSERT_EQUAL_UINT32(1, t.frame().sectionCount);
  TEST_ASSERT_EQUAL_UINT32(2048, t.frame().fileBytes);
  TEST_ASSERT_EQUAL_UINT32(42, t.frame().generation);
  TEST_ASSERT_TRUE(t.frame().cardId == 0x5EEDC0DE0A1B2C3Dull);
  const cc::Section& wvsl = t.container().section(mpwf::kSecSlots);
  TEST_ASSERT_EQUAL_HEX32(mpwf::kWvsl, wvsl.type);
  TEST_ASSERT_EQUAL_UINT32(cc::kSectionRequired, wvsl.flags);
  TEST_ASSERT_EQUAL_UINT32(256, wvsl.offset);
  TEST_ASSERT_EQUAL_UINT32(7 * 256, wvsl.bytes);
  TEST_ASSERT_EQUAL_UINT32(7, wvsl.count);
  TEST_ASSERT_EQUAL_UINT32(256, wvsl.stride);
  uint8_t scratch[1024];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.check(scratch, sizeof(scratch))));
  mpwf::Slot s;
  uint32_t r = 0;
  for (const Row& row : rows) {
    TEST_ASSERT_TRUE(hashAt(file, r) == row.hash);
    TEST_ASSERT_EQUAL_STRING("hit", lookupName(t.find(row.hash, &s)));
    TEST_ASSERT_TRUE(s.pathHash() == row.hash);
    TEST_ASSERT_EQUAL_UINT32(row.lengthMs, s.lengthMs());
    TEST_ASSERT_EQUAL_MEMORY(slotAt(file, r), s.bytes, 256);
    ++r;
  }
  // The paths' hashes are the rows' (NFC and NFD apart).
  TEST_ASSERT_TRUE(cc::pathHash("Artist/Album/CD1/01 - Disc One.flac") == rows[1].hash);
  TEST_ASSERT_TRUE(cc::pathHash("A/x.mp3") == rows[2].hash);
  TEST_ASSERT_TRUE(cc::pathHash("Artist/x.mp3") == rows[3].hash);
  TEST_ASSERT_TRUE(cc::pathHash("Artist/Album/02 - Other.mp3") == rows[4].hash);
  TEST_ASSERT_TRUE(cc::pathHash("Artist/Album/01 - Title.mp3") == rows[5].hash);
  TEST_ASSERT_TRUE(cc::pathHash("Caf\xC3\xA9/Album/01 - Title.mp3") == rows[6].hash);
  TEST_ASSERT_TRUE(cc::pathHash("Cafe\xCC\x81/Album/01 - Title.mp3") == rows[0].hash);
  // Their bars.
  const uint8_t* b0 = slotAt(file, 0) + mpwf::kBarsAt;  // 80 each of 30, 40, 50
  TEST_ASSERT_EQUAL_UINT8(30, b0[79]);
  TEST_ASSERT_EQUAL_UINT8(40, b0[80]);
  TEST_ASSERT_EQUAL_UINT8(40, b0[159]);
  TEST_ASSERT_EQUAL_UINT8(50, b0[160]);
  TEST_ASSERT_EQUAL_UINT8(50, b0[239]);
  const uint8_t* b2 = slotAt(file, 2) + mpwf::kBarsAt;  // silent, and it has a slot
  const uint8_t* b3 = slotAt(file, 3) + mpwf::kBarsAt;  // a brick
  const uint8_t* b4 = slotAt(file, 4) + mpwf::kBarsAt;  // bar 3k + 2 is k + 1, every one a tie
  const uint8_t* b5 = slotAt(file, 5) + mpwf::kBarsAt;  // a ramp to 254
  const uint8_t* b6 = slotAt(file, 6) + mpwf::kBarsAt;  // 120 each of 10, 20
  for (uint32_t i = 0; i < mpwf::kBarCount; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0, b2[i]);
    TEST_ASSERT_EQUAL_UINT8(255, b3[i]);
    TEST_ASSERT_EQUAL_UINT8(i % 3 == 2 ? i / 3 + 1 : 0, b4[i]);
    if (i) TEST_ASSERT_TRUE(b5[i] >= b5[i - 1]);
    TEST_ASSERT_EQUAL_UINT8(i < 120 ? 10 : 20, b6[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(254, b5[239]);
  uint8_t top = 0;
  for (uint32_t i = 0; i < mpwf::kBarCount; ++i) top = std::max(top, slotAt(file, 1)[mpwf::kBarsAt + i]);
  TEST_ASSERT_TRUE(top > 0 && top <= 239);  // a made-up song whose peak is 239

  // waves-one.bin, the smallest file: 13.4's dump, the header's fields.
  const Bytes one = golden("waves-one.bin");
  TEST_ASSERT_EQUAL_size_t(512, one.size());
  const uint8_t head[48] = {0x4d, 0x50, 0x57, 0x46, 0x01, 0x00, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                            0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3d, 0x2c, 0x1b, 0x0a, 0xde, 0xc0, 0xed, 0x5e,
                            0x26, 0xac, 0xd6, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_MEMORY(head, one.data(), sizeof(head));
  const uint8_t entry[32] = {0x57, 0x56, 0x53, 0x4c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                             0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x5f, 0x2a, 0x7b, 0xbd, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_MEMORY(entry, &one[224], sizeof(entry));
  for (size_t i = sizeof(head); i < 224; ++i) TEST_ASSERT_EQUAL_UINT8(0, one[i]);

  // waves-skew.bin: 49 rows, its two extremes, and the two tracks of one hash
  // left out.
  const Bytes skew = golden("waves-skew.bin");
  cc::MemSource ss(skew.data(), static_cast<uint32_t>(skew.size()));
  mpwf::Table st;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.open(ss)));
  TEST_ASSERT_EQUAL_UINT32(49, st.rows());
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.check(scratch, sizeof(scratch))));
  TEST_ASSERT_TRUE(hashAt(skew, 0) == 0);
  TEST_ASSERT_TRUE(hashAt(skew, 40) == 0x4000000000000000ull);
  TEST_ASSERT_TRUE(hashAt(skew, 48) == ~0ull);
  TEST_ASSERT_EQUAL_STRING("miss", lookupName(st.find(0x8000000000000000ull, &s)));
  // A whole-file check, whatever the scratch: a small one, one that streams
  // whole sectors, too small a one refused.
  std::vector<uint8_t> big(8192);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.check(scratch, 320)));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.check(big.data(), static_cast<uint32_t>(big.size()))));
  TEST_ASSERT_EQUAL_STRING("io", cc::whyName(st.check(scratch, 319)));
  TEST_ASSERT_EQUAL_STRING("io", cc::whyName(st.check(nullptr, 1024)));
}

// ---------------------------------------------------------------------------
// The search (6): one step, then each lookup's rows.
// ---------------------------------------------------------------------------
void test_one_step_of_the_search() {
  const Value& v = vectors()["pick"];
  TEST_ASSERT_TRUE(v.size() >= 10);
  for (size_t i = 0; i < v.size(); ++i) {
    const Value& e = v[i];
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["row"].u64(),
                                     mpwf::pick(e["h"].u64(), u32(e["lo"]), u32(e["hi"]), e["loH"].u64(), e["hiH"].u64(),
                                                u32(e["probes"])),
                                     e["note"].c_str());
  }
  // In 64 bits at the largest product: every answer inside lo..hi, whatever the
  // hash and the bounds.
  std::mt19937_64 rng(20261101);
  for (int iter = 0; iter < 200000; ++iter) {
    uint64_t a = rng(), b = rng();
    if (iter % 3 == 0) b = a + (rng() >> (rng() % 64));  // spans of every size
    if (b < a) std::swap(a, b);
    const uint64_t span = b - a;
    const uint64_t h = a + (span == ~0ull ? rng() : rng() % (span + 1));
    const uint32_t lo = static_cast<uint32_t>(rng() % mpwf::kMaxRows);
    const uint32_t hi = lo + static_cast<uint32_t>(rng() % (mpwf::kMaxRows - lo));
    const uint32_t probes = static_cast<uint32_t>(rng() % 6);
    const uint32_t r = mpwf::pick(h, lo, hi, a, b, probes);
    TEST_ASSERT_TRUE(r >= lo && r <= hi);
    if (probes >= mpwf::kInterpolated) TEST_ASSERT_EQUAL_UINT32(lo + (hi - lo) / 2, r);
  }
}

void test_each_lookups_rows() {
  const Value& files = vectors()["search"]["files"];
  TEST_ASSERT_EQUAL_size_t(3, files.size());
  uint32_t lookups = 0;
  for (const auto& kv : files.members) {
    const Bytes file = golden(kv.first);
    for (size_t i = 0; i < kv.second.size(); ++i) {
      const Value& e = kv.second[i];
      RowLog src(file);
      mpwf::Table t;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
      TEST_ASSERT_EQUAL_size_t(0, src.rows.size());  // open() reads no slot
      mpwf::Slot s;
      cc::Why why = cc::Why::Io;
      const std::string what = kv.first + " " + e["hash"].str();
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e["read"].c_str(), lookupName(t.find(e["hash"].u64(), &s, &why)), what.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(why), what.c_str());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(e["probes"].size(), src.rows.size(), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["probes"].size(), t.reads(), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, src.odd, what.c_str());
      for (size_t p = 0; p < src.rows.size(); ++p)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["probes"][p].u64(), src.rows[p], what.c_str());
      if (e["read"].str() == "hit") {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["row"].u64(), src.rows.back(), what.c_str());
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(slotAt(file, u32(e["row"])), s.bytes, 256, what.c_str());
      }
      ++lookups;
    }
  }
  TEST_ASSERT_TRUE(lookups >= 20);
}

// Every row found at its row and nothing else found, against a walk from row 0.
void assertSearches(const std::vector<uint64_t>& hashes, const char* what) {
  std::vector<uint64_t> sorted = hashes;
  std::sort(sorted.begin(), sorted.end());
  const Bytes file = tableOf(hashes);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(mpwf::fileBytesFor(static_cast<uint32_t>(sorted.size())), file.size(), what);
  RowLog src(file);
  mpwf::Table t;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.open(src)), what);
  mpwf::Slot s;
  for (uint32_t r = 0; r < sorted.size(); ++r) {
    src.rows.clear();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("hit", lookupName(t.find(sorted[r], &s)), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r, src.rows.back(), what);
    TEST_ASSERT_TRUE_MESSAGE(t.reads() <= 22, what);
    // Its neighbours, when they aren't rows.
    for (uint64_t h : {sorted[r] - 1, sorted[r] + 1}) {
      if (std::binary_search(sorted.begin(), sorted.end(), h)) continue;
      TEST_ASSERT_EQUAL_STRING_MESSAGE("miss", lookupName(t.find(h, &s)), what);
      TEST_ASSERT_TRUE_MESSAGE(t.reads() <= 22, what);
    }
  }
}

void test_the_search_on_tables_of_every_shape() {
  std::mt19937_64 rng(20261102);
  // A few rows: every small count, the extremes in place.
  for (uint32_t n = 1; n <= 70; ++n) {
    std::set<uint64_t> h;
    while (h.size() < n) h.insert(rng());
    assertSearches(std::vector<uint64_t>(h.begin(), h.end()), "uniform");
    h.erase(h.begin());
    h.insert(0);
    assertSearches(std::vector<uint64_t>(h.begin(), h.end()), "with hash 0");
    h.erase(std::prev(h.end()));
    h.insert(~0ull);
    assertSearches(std::vector<uint64_t>(h.begin(), h.end()), "with hash 0 and 2^64 - 1");
  }
  // Skewed: everything low, everything high, a tight clump, powers of two,
  // clumps at both ends. The search never goes wrong on them, only slower.
  std::vector<uint64_t> low, high, clump, powers, both;
  for (uint64_t i = 0; i < 3000; ++i) {
    low.push_back(i);
    high.push_back(~0ull - i);
    clump.push_back(0x7000000000000000ull + i * 3);
    both.push_back(i % 2 ? i : ~0ull - i);
  }
  for (int bit = 0; bit < 64; ++bit) powers.push_back(1ull << bit);
  assertSearches(low, "consecutive from 0");
  assertSearches(high, "consecutive up to 2^64 - 1");
  assertSearches(clump, "a clump");
  assertSearches(powers, "powers of two");
  assertSearches(both, "clumps at both ends");
  // Spans around the 2^32 where the interpolation gives way (den 0).
  std::vector<uint64_t> spans;
  for (uint64_t i = 0; i < 2000; ++i) spans.push_back(0x123456789ull + i * 0x1000000ull);
  assertSearches(spans, "under and over 2^32");
}

// 250,000 consecutive hashes, the worst shape: 22 reads (6). No 64 MB here: the
// row limit vector's first 256 bytes, then slots made as they are asked for.
class MadeSlots : public cc::Source {
public:
  MadeSlots(const Bytes& head, uint64_t base) : head_(head), base_(base) {}
  uint32_t size() const override { return cc::get32(&head_[16]); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (offset < head_.size()) {
      if (offset + n > head_.size()) return false;
      std::memcpy(out, &head_[offset], n);
      return true;
    }
    if (n != mpwf::kSlotBytes) return false;
    const uint32_t r = (offset - mpwf::kSlotsOffset) / mpwf::kSlotBytes;
    uint8_t* slot = static_cast<uint8_t*>(out);
    std::memset(slot, 0, mpwf::kSlotBytes);
    cc::put64(slot, base_ + r);
    cc::put32(slot + mpwf::kCrcAt, mpwf::slotCrc(slot, r));
    return true;
  }

private:
  const Bytes& head_;
  uint64_t base_;
};

void test_the_row_limit() {
  const Value& v = vectors()["rowLimit"];
  TEST_ASSERT_EQUAL_size_t(2, v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    const Value& e = v[i];
    const Bytes head = hexBytes(e["head"].str());
    TEST_ASSERT_EQUAL_size_t(256, head.size());
    TEST_ASSERT_EQUAL_UINT32(e["fileBytes"].u64(), cc::get32(&head[16]));
    MadeSlots src(head, 0);
    mpwf::Table t;
    TEST_ASSERT_EQUAL_STRING(e["why"].c_str(), cc::whyName(t.open(src)));
    if (e["why"].str() == "ok") {
      TEST_ASSERT_EQUAL_UINT32(e["rows"].u64(), t.rows());
    } else {
      TEST_ASSERT_EQUAL_UINT32(0, t.rows());
    }
  }
  // The most a lookup can take, at the most rows, at either end of the hashes.
  const Bytes head = hexBytes(v[0]["head"].str());
  for (uint64_t base : {0ull, ~0ull - (mpwf::kMaxRows - 1)}) {
    MadeSlots src(head, base);
    mpwf::Table t;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    mpwf::Slot s;
    Reads hits;
    for (uint32_t r = 0; r < mpwf::kMaxRows; r += r < 64 || r >= mpwf::kMaxRows - 64 ? 1 : 251) {
      TEST_ASSERT_EQUAL_STRING("hit", lookupName(t.find(base + r, &s)));
      TEST_ASSERT_TRUE(s.pathHash() == base + r);
      hits.add(t.reads());
    }
    TEST_ASSERT_EQUAL_UINT32(22, hits.most);
    TEST_ASSERT_TRUE(mpwf::kMaxProbes > hits.most);  // the guard can't fire on a valid file
  }
}

// ---------------------------------------------------------------------------
// The hardening files (13.5) and the root's row (7.2).
// ---------------------------------------------------------------------------
void test_the_hardening_files() {
  const Value index = load("hardening/index.json");
  std::set<std::string> reasons;
  uint32_t files = 0, lookups = 0, roots = 0;
  for (size_t i = 0; i < index.size(); ++i) {
    const Value& e = index[i];
    const std::string file = e["file"].str();
    const std::string what = file + ": " + e["check"].str();
    // The roots: each row's verdict is test_the_roots_row's.
    if (file.compare(0, 11, "msmf-waves-") == 0) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("MSMF", e["format"].c_str(), what.c_str());
      const Bytes b = fixture("hardening/" + file);
      cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
      uint8_t scratch[256];
      msmf::Manifest m;
      const cc::Why why = m.open(src, scratch, sizeof(scratch));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(why), what.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(e["expect"].str() == "present", why == cc::Why::Ok, what.c_str());
      ++roots;
      continue;
    }
    if (e["format"].str() != "MPWF") continue;
    ++files;
    const Bytes b = fixture("hardening/" + file);
    TEST_ASSERT_TRUE_MESSAGE(file.compare(0, 5, "mpwf-") == 0, what.c_str());
    const cc::Why why = readAs(b, e["uses"].str() == "device");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(why), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(e["expect"].str() == "present", why == cc::Why::Ok, what.c_str());
    reasons.insert(cc::whyName(why));
    if (e.has("same")) assertSameSlots(golden(e["same"].str()), b, what.c_str());
    if (e.has("slot")) {
      RowLog src(b);
      mpwf::Table t;
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.open(src)), what.c_str());
      mpwf::Slot s;
      cc::Why slotWhy = cc::Why::Io;
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e["slot"]["read"].c_str(), lookupName(t.find(e["slot"]["hash"].u64(), &s, &slotWhy)),
                                       what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["slot"]["reads"].u64(), t.reads(), what.c_str());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(e["slot"]["reads"].u64(), src.rows.size(), what.c_str());
      if (e["why"].str() != "ok") {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(slotWhy), what.c_str());
      } else if (file == "mpwf-uses-device-slot-crc.bin") {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("wvSlotCrc", cc::whyName(slotWhy), what.c_str());
      } else if (file == "mpwf-uses-device-unsorted.bin") {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("wvSlotOrder", cc::whyName(slotWhy), what.c_str());
      } else {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(slotWhy), what.c_str());
      }
      ++lookups;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(50, files);
  TEST_ASSERT_EQUAL_UINT32(12, lookups);
  TEST_ASSERT_EQUAL_UINT32(13, roots);
  // Every reason of 13.5's table, and the present ones.
  for (const char* r : {"ok", "short", "magic", "major", "headerBytes", "sectionCount", "fileBytes", "headerCrc", "layout",
                        "unknownRequired", "shape", "missing", "enum", "wvLayout", "wvSlotCrc", "wvSlotOrder", "sectionCrc"})
    TEST_ASSERT_TRUE_MESSAGE(reasons.count(r) == 1, r);
}

void test_the_roots_row() {
  const Value& v = vectors()["rootRow"];
  std::vector<std::pair<std::string, const Value*>> files = {{"golden/" + v["golden"]["file"].str(), &v["golden"]}};
  for (size_t i = 0; i < v["hardening"].size(); ++i)
    files.emplace_back("hardening/" + v["hardening"][i]["file"].str(), &v["hardening"][i]);
  TEST_ASSERT_EQUAL_size_t(12, files.size());
  for (const auto& f : files) {
    const char* what = f.first.c_str();
    const Value& e = *f.second;
    const Bytes b = fixture(f.first);
    cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
    uint8_t scratch[256];
    msmf::Manifest m;
    // Whatever its MPWF row holds, the root reads, and names its T.
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(m.open(src, scratch, sizeof(scratch))), what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("tags-0000002a.bin", m.tagsName(), what);
    if (f.first.find("no-mpdj") == std::string::npos)
      TEST_ASSERT_EQUAL_STRING_MESSAGE("autodj-00000029.bin", m.autoDjName(), what);
    if (e["waves"].isNull()) {
      TEST_ASSERT_FALSE_MESSAGE(m.hasWaves(), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.wavesName(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, m.waves().kind, what);
      continue;
    }
    TEST_ASSERT_TRUE_MESSAGE(m.hasWaves(), what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(e["waves"].c_str(), m.wavesName(), what);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(cc::kMagicMpwf, m.waves().kind, what);
    // The row against the golden file of that name.
    const Bytes file = golden(m.wavesName());
    cc::MemSource fs(file.data(), static_cast<uint32_t>(file.size()));
    mpwf::Table t;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.open(fs)), what);
    TEST_ASSERT_EQUAL_MESSAGE(e["matches"].boolean(), msmf::companionMatches(m.waves(), t.frame(), 0, nullptr), what);
    // The signature an MPDJ row compares isn't looked at.
    const uint8_t other[16] = {1, 2, 3};
    TEST_ASSERT_EQUAL_MESSAGE(e["matches"].boolean(), msmf::companionMatches(m.waves(), t.frame(), 0, other), what);
  }
  // A Manifest opened again over a root with no row forgets the last one's.
  const Bytes withRow = golden("manifest-waves.bin"), without = golden("manifest.bin");
  cc::MemSource a(withRow.data(), static_cast<uint32_t>(withRow.size())), c(without.data(), static_cast<uint32_t>(without.size()));
  uint8_t scratch[256];
  msmf::Manifest m;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(a, scratch, sizeof(scratch))));
  TEST_ASSERT_TRUE(m.hasWaves());
  TEST_ASSERT_EQUAL_UINT32(42, m.waves().generation);
  TEST_ASSERT_EQUAL_UINT32(43, m.frame().generation);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(c, scratch, sizeof(scratch))));
  TEST_ASSERT_FALSE(m.hasWaves());
  TEST_ASSERT_EQUAL_STRING("", m.wavesName());
  // Unknown kinds stay ignored, and don't count as a second row.
  const Bytes unknown = fixture("hardening/msmf-comp-unknown-kind.bin");
  cc::MemSource u(unknown.data(), static_cast<uint32_t>(unknown.size()));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(u, scratch, sizeof(scratch))));
  TEST_ASSERT_FALSE(m.hasWaves());
}

void test_the_root_written() {
  msmf::CompIn tags, dj, waves;
  tags.kind = cc::kMagicMptg;
  tags.generation = 7;
  dj.kind = cc::kMagicMpdj;
  dj.generation = 6;
  dj.key[0] = 9;
  waves.kind = cc::kMagicMpwf;
  waves.generation = 5;
  waves.fileBytes = 512;
  waves.headerCrc = 0xABCD1234;
  waves.key[3] = 0xFF;  // a writer writes zeros: only an MPDJ row has a key
  auto written = [&](std::vector<msmf::CompIn> comps, Bytes* out) {
    msmf::ManifestIn in;
    in.generation = 7;
    in.comps = comps.data();
    in.compCount = static_cast<uint32_t>(comps.size());
    VecSink sink;
    const char* error = nullptr;
    const bool ok = msmf::write(sink, in, nullptr, nullptr, &error);
    if (out) *out = sink.b;
    if (!ok) TEST_ASSERT_NOT_NULL(error);
    return ok;
  };
  // After the MPTG and the MPDJ, or the MPTG alone.
  for (bool withDj : {false, true}) {
    Bytes root;
    TEST_ASSERT_TRUE(written(withDj ? std::vector<msmf::CompIn>{tags, dj, waves} : std::vector<msmf::CompIn>{tags, waves}, &root));
    cc::MemSource src(root.data(), static_cast<uint32_t>(root.size()));
    uint8_t scratch[256];
    msmf::Manifest m;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(src, scratch, sizeof(scratch))));
    TEST_ASSERT_EQUAL(withDj, m.hasAutoDj());
    TEST_ASSERT_TRUE(m.hasWaves());
    TEST_ASSERT_EQUAL_STRING("waves-00000005.bin", m.wavesName());
    TEST_ASSERT_EQUAL_UINT32(5, m.waves().generation);
    TEST_ASSERT_EQUAL_UINT32(512, m.waves().fileBytes);
    TEST_ASSERT_EQUAL_HEX32(0xABCD1234, m.waves().headerCrc);
    const uint8_t zeros[16] = {};
    TEST_ASSERT_EQUAL_MEMORY(zeros, m.waves().key, 16);
    // The entry against a header: the file named, and any other not.
    cc::Header h;
    h.magic = cc::kMagicMpwf;
    h.generation = 5;
    h.fileBytes = 512;
    h.headerCrc = 0xABCD1234;
    TEST_ASSERT_TRUE(msmf::companionMatches(m.waves(), h, 0, nullptr));
    for (int field = 0; field < 4; ++field) {
      cc::Header x = h;
      if (field == 0) x.magic = cc::kMagicMpdj;
      if (field == 1) x.generation ^= 1;
      if (field == 2) x.fileBytes ^= 1;
      if (field == 3) x.headerCrc ^= 1;
      TEST_ASSERT_FALSE(msmf::companionMatches(m.waves(), x, 0, nullptr));
    }
    msmf::Comp keyed = m.waves();
    keyed.key[15] = 1;
    TEST_ASSERT_FALSE(msmf::companionMatches(keyed, h, 0, nullptr));
  }
  // COMP's order, a writer's rule (7.2): readers don't check the row's place.
  TEST_ASSERT_FALSE(written({waves}, nullptr));
  TEST_ASSERT_FALSE(written({waves, tags}, nullptr));
  TEST_ASSERT_FALSE(written({tags, waves, dj}, nullptr));
  TEST_ASSERT_FALSE(written({tags, waves, waves}, nullptr));
  TEST_ASSERT_FALSE(written({tags, dj, dj}, nullptr));
  TEST_ASSERT_FALSE(written({tags, dj, waves, waves}, nullptr));
  TEST_ASSERT_TRUE(written({tags}, nullptr));
  TEST_ASSERT_TRUE(written({tags, dj}, nullptr));
}

// ---------------------------------------------------------------------------
// When a lookup fails (8.3).
// ---------------------------------------------------------------------------
void test_a_failed_read_a_damaged_slot_a_broken_writer() {
  const Bytes file = golden("waves-0000002a.bin");
  const uint64_t row3 = hashAt(file, 3);
  mpwf::Slot s;
  cc::Why why = cc::Why::Ok;
  // A read that fails: Io, the slot it failed on counted; not the file's
  // fault: the next read may work.
  {
    FlakySource src(file);
    mpwf::Table t;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    src.fail = true;
    TEST_ASSERT_EQUAL_STRING("io", lookupName(t.find(row3, &s, &why)));
    TEST_ASSERT_EQUAL_STRING("io", cc::whyName(why));
    TEST_ASSERT_EQUAL_UINT32(1, t.reads());
    src.fail = false;
    TEST_ASSERT_EQUAL_STRING("hit", lookupName(t.find(row3, &s, &why)));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(why));
    const uint32_t reads = t.reads();
    TEST_ASSERT_TRUE(reads >= 2);
    src.failAt = reads;  // its last slot's read
    TEST_ASSERT_EQUAL_STRING("io", lookupName(t.find(row3, &s)));
    TEST_ASSERT_EQUAL_UINT32(reads, t.reads());
  }
  // A read that fails anywhere in open(): Io, and no rows.
  {
    bool opened = false;
    for (uint32_t n = 1; n < 40 && !opened; ++n) {
      FlakySource src(file);
      src.failAt = n;
      mpwf::Table t;
      const cc::Why w = t.open(src);
      opened = src.failAt != 0;  // all its reads done before the n-th
      if (opened) {
        TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(w));
      } else {
        TEST_ASSERT_EQUAL_STRING("io", cc::whyName(w));
        TEST_ASSERT_EQUAL_UINT32(0, t.rows());
      }
    }
    TEST_ASSERT_TRUE(opened);
  }
  // No table: every lookup misses, with no read. Nowhere to read to: Io.
  {
    mpwf::Table t;
    TEST_ASSERT_EQUAL_UINT32(0, t.rows());
    TEST_ASSERT_EQUAL_STRING("miss", lookupName(t.find(row3, &s, &why)));
    TEST_ASSERT_EQUAL_UINT32(0, t.reads());
    cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    TEST_ASSERT_EQUAL_STRING("io", lookupName(t.find(row3, nullptr, &why)));
    TEST_ASSERT_EQUAL_STRING("io", cc::whyName(why));
    TEST_ASSERT_EQUAL_STRING("hit", lookupName(t.find(row3, &s)));
    TEST_ASSERT_TRUE(t.reads() > 0);
    // A table opened again over a bad file holds no rows.
    Bytes bad = file;
    bad[0] ^= 1;
    cc::MemSource bs(bad.data(), static_cast<uint32_t>(bad.size()));
    TEST_ASSERT_EQUAL_STRING("magic", cc::whyName(t.open(bs)));
    TEST_ASSERT_EQUAL_UINT32(0, t.rows());
    TEST_ASSERT_EQUAL_STRING("miss", lookupName(t.find(row3, &s)));
    TEST_ASSERT_EQUAL_UINT32(0, t.reads());
  }
  // A damaged slot costs only the lookups that read it; the file stays.
  {
    for (uint32_t bad = 0; bad < 7; ++bad) {
      Bytes b = file;
      b[mpwf::kSlotsOffset + bad * mpwf::kSlotBytes + 100] ^= 0x10;
      RowLog src(b);
      mpwf::Table t;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
      uint32_t damaged = 0;
      for (uint32_t r = 0; r < 7; ++r) {
        src.rows.clear();
        const mpwf::Lookup l = t.find(hashAt(file, r), &s, &why);
        const bool readBad = std::find(src.rows.begin(), src.rows.end(), bad) != src.rows.end();
        TEST_ASSERT_EQUAL_STRING(readBad ? "damaged" : "hit", lookupName(l));
        TEST_ASSERT_EQUAL_STRING(readBad ? "wvSlotCrc" : "ok", cc::whyName(why));
        damaged += readBad;
      }
      TEST_ASSERT_TRUE(damaged >= 1);  // its own row at least
      uint8_t scratch[1024];
      TEST_ASSERT_EQUAL_STRING("wvSlotCrc", cc::whyName(t.check(scratch, sizeof(scratch))));
    }
  }
  // A slot read for the wrong row fails: the row is in its CRC.
  {
    Bytes b = file;
    std::swap_ranges(b.begin() + 256 + 2 * 256, b.begin() + 256 + 3 * 256, b.begin() + 256 + 4 * 256);
    cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
    mpwf::Table t;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    TEST_ASSERT_EQUAL_STRING("damaged", lookupName(t.find(hashAt(file, 2), &s)));
  }
  // A good CRC over a hash that breaks the sort rule: at the first bound that
  // meets it, whichever bound it is, and a hash equal to a bound too.
  {
    const std::vector<uint64_t> h = {0x1000000000000000ull, 0x3000000000000000ull, 0x5000000000000000ull,
                                     0x7000000000000000ull, 0x9000000000000000ull, 0xB000000000000000ull,
                                     0xD000000000000000ull};
    const Bytes good = tableOf(h);
    auto withHash = [&](uint32_t row, uint64_t hash) {
      Bytes b = good;
      uint8_t* slot = &b[mpwf::kSlotsOffset + row * mpwf::kSlotBytes];
      cc::put64(slot, hash);
      cc::put32(slot + mpwf::kCrcAt, mpwf::slotCrc(slot, row));
      return b;
    };
    struct Case {
      uint32_t row;
      uint64_t hash;
      uint64_t lookFor;
      const char* read;
    };
    const Case cases[] = {
        // 0x4...: row 2 first (0x5...), so hi moves, then row 1 at or above it.
        {1, 0x6000000000000000ull, 0x4000000000000000ull, "broken"},
        {1, 0x5000000000000000ull, 0x4000000000000000ull, "broken"},
        // 0x8...: row 3 first (0x7...), so lo moves, then row 4 at or below it.
        {4, 0x6000000000000000ull, 0x8000000000000000ull, "broken"},
        {4, 0x7000000000000000ull, 0x8000000000000000ull, "broken"},
        // A slot this lookup doesn't read can't stop it: a whole-file check
        // tells. 0xB... reads rows 4 and 5.
        {1, 0x6000000000000000ull, 0xB000000000000000ull, "hit"},
    };
    for (const Case& c : cases) {
      const Bytes b = withHash(c.row, c.hash);
      cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
      mpwf::Table t;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
      TEST_ASSERT_EQUAL_STRING(c.read, lookupName(t.find(c.lookFor, &s, &why)));
      TEST_ASSERT_EQUAL_STRING(std::strcmp(c.read, "broken") == 0 ? "wvSlotOrder" : "ok", cc::whyName(why));
      uint8_t scratch[1024];
      TEST_ASSERT_EQUAL_STRING("wvSlotOrder", cc::whyName(t.check(scratch, sizeof(scratch))));
    }
    // Hash 0 in row 0 and 2^64 - 1 in the last are no fault: no bound was read.
    const Bytes extremes = tableOf({0, 5, ~0ull});
    TEST_ASSERT_EQUAL_UINT32(3, lookUp(extremes, {0, 5, ~0ull}, mpwf::Lookup::Hit).n);
  }
  // What a whole-file check meets first: the CRC before the order, row by row.
  {
    Bytes b = tableOf({1ull << 60, 2ull << 60, 3ull << 60});
    uint8_t* s1 = &b[mpwf::kSlotsOffset + mpwf::kSlotBytes];
    cc::put64(s1, 9ull << 60);  // row 1 out of order, its CRC not made right
    cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
    mpwf::Table t;
    uint8_t scratch[1024];
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    TEST_ASSERT_EQUAL_STRING("wvSlotCrc", cc::whyName(t.check(scratch, sizeof(scratch))));
    cc::put32(s1 + mpwf::kCrcAt, mpwf::slotCrc(s1, 1));
    TEST_ASSERT_EQUAL_STRING("wvSlotOrder", cc::whyName(t.check(scratch, sizeof(scratch))));
    cc::put64(s1, 2ull << 60);
    cc::put32(s1 + mpwf::kCrcAt, mpwf::slotCrc(s1, 1));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.check(scratch, sizeof(scratch))));
    b[256 + 512] ^= 1;
    FlakySource flaky(b);
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(flaky)));
    flaky.fail = true;
    TEST_ASSERT_EQUAL_STRING("io", cc::whyName(t.check(scratch, sizeof(scratch))));
  }
}

// ---------------------------------------------------------------------------
// The writer (4).
// ---------------------------------------------------------------------------
void test_the_writers_edge_cases() {
  const uint8_t bars[mpwf::kBarCount] = {1, 2, 3};
  mpwf::RowIn a, b, c;
  a.pathHash = 3;
  a.lengthMs = 300;
  a.bars = bars;
  b = a;
  b.pathHash = 1;
  c = a;
  c.pathHash = 2;
  auto refuses = [](const mpwf::TableIn& in, const char* what) {
    VecSink out;
    const char* error = nullptr;
    TEST_ASSERT_FALSE_MESSAGE(mpwf::write(out, in, nullptr, nullptr, &error), what);
    TEST_ASSERT_NOT_NULL_MESSAGE(error, what);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, out.b.size(), what);
  };
  mpwf::TableIn in;
  in.generation = 3;
  in.cardId = 0x1122334455667788ull;
  // A writer with no row writes no file.
  refuses(in, "no rows");
  in.rowCount = 1;
  refuses(in, "no row array");
  mpwf::RowIn noBars = a;
  noBars.bars = nullptr;
  in.rows = &noBars;
  refuses(in, "a row with no bars");
  // Every row's hash shared: nothing left to write.
  mpwf::RowIn same[3] = {a, a, a};
  in.rows = same;
  in.rowCount = 3;
  refuses(in, "one hash three times");
  // One shared among others: all of them left out, the rest in order, each
  // CRC made for its place.
  mpwf::RowIn rows[5] = {a, b, c, b, a};
  rows[4].pathHash = 2;  // shared with c
  rows[3].pathHash = 9;
  rows[1].lengthMs = 111;
  in.rows = rows;
  in.rowCount = 5;
  in.minor = 2;
  VecSink out;
  uint32_t fileBytes = 0, headerCrc = 0;
  TEST_ASSERT_TRUE(mpwf::write(out, in, &fileBytes, &headerCrc, nullptr));
  TEST_ASSERT_EQUAL_UINT32(mpwf::fileBytesFor(3), fileBytes);
  cc::MemSource src(out.b.data(), static_cast<uint32_t>(out.b.size()));
  mpwf::Table t;
  uint8_t scratch[1024];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.check(scratch, sizeof(scratch))));
  TEST_ASSERT_EQUAL_UINT32(3, t.rows());
  TEST_ASSERT_EQUAL_UINT16(2, t.frame().minor);
  TEST_ASSERT_EQUAL_UINT32(3, t.frame().generation);
  TEST_ASSERT_TRUE(t.frame().cardId == 0x1122334455667788ull);
  TEST_ASSERT_EQUAL_HEX32(headerCrc, t.frame().headerCrc);
  TEST_ASSERT_TRUE(hashAt(out.b, 0) == 1 && hashAt(out.b, 1) == 3 && hashAt(out.b, 2) == 9);
  mpwf::Slot s;
  TEST_ASSERT_EQUAL_STRING("miss", lookupName(t.find(2, &s)));
  TEST_ASSERT_EQUAL_STRING("hit", lookupName(t.find(1, &s)));
  TEST_ASSERT_EQUAL_UINT32(111, s.lengthMs());
  TEST_ASSERT_EQUAL_MEMORY(bars, s.bars(), mpwf::kBarCount);
  // The header's padding and the reserved fields are zeros, bytes 40-47 its own.
  TEST_ASSERT_EQUAL_UINT32(256, cc::get32(&out.b[40]));
  TEST_ASSERT_EQUAL_UINT32(240, cc::get32(&out.b[44]));
  TEST_ASSERT_EQUAL_UINT32(0, cc::get32(&out.b[36]));
  for (size_t i = 48; i < 224; ++i) TEST_ASSERT_EQUAL_UINT8(0, out.b[i]);
  TEST_ASSERT_EQUAL_UINT32(0, cc::get32(&out.b[224 + 28]));
  // A sink that fails, wherever it fails.
  class Failing : public cc::Sink {
  public:
    explicit Failing(uint32_t n) : left(n) {}
    bool write(uint32_t, const void*, uint32_t) override { return left-- != 0; }
    uint32_t left;
  };
  Failing all(0xFFFFFFFFu);
  TEST_ASSERT_TRUE(mpwf::write(all, in, nullptr, nullptr, nullptr));
  const uint32_t sinkWrites = 0xFFFFFFFFu - all.left;
  TEST_ASSERT_TRUE(sinkWrites >= 3);
  for (uint32_t n = 0; n < sinkWrites; ++n) {
    Failing f(n);
    const char* error = nullptr;
    TEST_ASSERT_FALSE(mpwf::write(f, in, nullptr, nullptr, &error));
    TEST_ASSERT_NOT_NULL(error);
  }
  // The limit: 250,000 rows are written, their frame the vector's but for
  // WVSL's CRC (0 there: no reader of that head reads the slots) and so the
  // header's, and they open; 250,001 aren't.
  class Head : public cc::Sink {
  public:
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      top = std::max(top, offset + n);
      if (offset < sizeof(b)) std::memcpy(b + offset, data, std::min<uint32_t>(n, sizeof(b) - offset));
      return true;
    }
    uint8_t b[256] = {};
    uint32_t top = 0;
  };
  const Value& limit = vectors()["rowLimit"][0];
  const Bytes wantHead = hexBytes(limit["head"].str());
  std::vector<mpwf::RowIn> many(mpwf::kMaxRows + 1);
  uint64_t state = 0;
  for (mpwf::RowIn& r : many) {
    r.pathHash = splitMix64(&state);
    r.bars = bars;
  }
  mpwf::TableIn big;
  big.generation = cc::get32(&wantHead[20]);
  big.cardId = cc::get64(&wantHead[24]);
  big.rows = many.data();
  big.rowCount = mpwf::kMaxRows;
  Head head;
  TEST_ASSERT_TRUE(mpwf::write(head, big, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQUAL_UINT32(limit["fileBytes"].u64(), head.top);
  TEST_ASSERT_EQUAL_MEMORY(wantHead.data(), head.b, 32);
  TEST_ASSERT_EQUAL_MEMORY(&wantHead[36], head.b + 36, 248 - 36);
  TEST_ASSERT_EQUAL_MEMORY(&wantHead[252], head.b + 252, 4);
  const Bytes writtenHead(head.b, head.b + sizeof(head.b));
  MadeSlots made(writtenHead, 0);
  mpwf::Table limitTable;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(limitTable.open(made)));
  TEST_ASSERT_EQUAL_UINT32(mpwf::kMaxRows, limitTable.rows());
  big.rowCount = mpwf::kMaxRows + 1;
  refuses(big, "250,001 rows");
}

// ---------------------------------------------------------------------------
// What the search costs, in slot reads (6).
// ---------------------------------------------------------------------------
void test_the_search_reads_as_the_vectors_say() {
  const Value& v = vectors()["scale"];
  // SplitMix64 from state 0.
  uint64_t state = 0;
  for (size_t i = 0; i < v["splitmix64"].size(); ++i) TEST_ASSERT_TRUE(splitMix64(&state) == v["splitmix64"][i].u64());
  {
    const Value& e = v["uniform"];
    state = 0;
    std::vector<uint64_t> rows, misses;
    for (uint32_t i = 0; i < e["rows"].u64(); ++i) rows.push_back(splitMix64(&state));
    for (uint32_t i = 0; i < e["misses"].u64(); ++i) misses.push_back(splitMix64(&state));
    const Bytes file = tableOf(rows);
    const Reads hits = lookUp(file, rows, mpwf::Lookup::Hit), miss = lookUp(file, misses, mpwf::Lookup::Miss);
    TEST_ASSERT_TRUE(hits.sum == e["hitReads"].u64());
    TEST_ASSERT_EQUAL_UINT32(e["hitMax"].u64(), hits.most);
    TEST_ASSERT_TRUE(miss.sum == e["missReads"].u64());
    TEST_ASSERT_EQUAL_UINT32(e["missMax"].u64(), miss.most);
    report("uniform hashes", hits, miss);
  }
  {
    const Value& e = v["paths"];
    std::vector<uint64_t> rows, misses;
    char path[128];
    for (int artist = 0; artist < 400; ++artist)
      for (int album = 0; album < 5; ++album)
        for (int track = 0; track < 10; ++track) {
          std::snprintf(path, sizeof(path), v["pattern"].c_str(), artist, album, track);
          rows.push_back(cc::pathHash(path));
        }
    TEST_ASSERT_EQUAL_size_t(e["rows"].u64(), rows.size());
    for (uint32_t i = 0; i < e["misses"].u64(); ++i) {
      std::snprintf(path, sizeof(path), v["missPattern"].c_str(), static_cast<int>(i));
      misses.push_back(cc::pathHash(path));
    }
    const Bytes file = tableOf(rows);
    const Reads hits = lookUp(file, rows, mpwf::Lookup::Hit), miss = lookUp(file, misses, mpwf::Lookup::Miss);
    TEST_ASSERT_TRUE(hits.sum == e["hitReads"].u64());
    TEST_ASSERT_EQUAL_UINT32(e["hitMax"].u64(), hits.most);
    TEST_ASSERT_TRUE(miss.sum == e["missReads"].u64());
    TEST_ASSERT_EQUAL_UINT32(e["missMax"].u64(), miss.most);
    report("paths of one pattern", hits, miss);
  }
}

// Paths of the synthetic card's shape, and uniform hashes of the same count from
// other seeds. Section 6's table: a hit takes 4 or 5 reads, a miss 5 or 6, and the
// most is 18 or 19: the lookups whose 4 probes all fall on one side bisect
// what is left.
void test_the_search_on_paths_of_the_synthetic_cards_shape() {
  constexpr size_t kRows = 20000, kMisses = 5000;
  const std::vector<std::string> paths = CardPaths(20261101).make(kRows);
  TEST_ASSERT_EQUAL_size_t(kRows, paths.size());
  // The shape's folder depths: loose tracks, albums, discs.
  std::set<size_t> depths;
  for (const std::string& p : paths) depths.insert(static_cast<size_t>(std::count(p.begin(), p.end(), '/')));
  TEST_ASSERT_TRUE(depths.count(1) && depths.count(2) && depths.count(3));
  // The misses: files beside the rows, with no slot.
  std::vector<uint64_t> rows, misses;
  for (const std::string& p : paths) rows.push_back(cc::pathHash(p.c_str()));
  for (size_t i = 0; i < kMisses; ++i) misses.push_back(cc::pathHash((paths[i * 4] + ".part").c_str()));
  const Bytes file = tableOf(rows);
  const Reads hits = lookUp(file, rows, mpwf::Lookup::Hit), miss = lookUp(file, misses, mpwf::Lookup::Miss);
  report("paths of the synthetic card's shape", hits, miss);
  TEST_ASSERT_EQUAL_UINT32(kRows, hits.n);
  TEST_ASSERT_TRUE(hits.mean() <= 5.0);
  TEST_ASSERT_TRUE(miss.mean() <= 6.0);
  TEST_ASSERT_TRUE(hits.most <= 22 && miss.most <= 22);
  TEST_ASSERT_TRUE(hits.atMost5 * 100 >= hits.n * 75);
  // Uniform hashes at other seeds, the same bounds.
  std::mt19937_64 seeds(20261103);
  for (int iter = 0; iter < 3; ++iter) {
    std::vector<uint64_t> u, m;
    for (size_t i = 0; i < kRows; ++i) u.push_back(seeds());
    for (size_t i = 0; i < kMisses; ++i) m.push_back(seeds());
    const Bytes f = tableOf(u);
    const Reads h = lookUp(f, u, mpwf::Lookup::Hit), x = lookUp(f, m, mpwf::Lookup::Miss);
    report("uniform hashes, another seed", h, x);
    TEST_ASSERT_TRUE(h.mean() <= 5.0);
    TEST_ASSERT_TRUE(x.mean() <= 6.0);
    TEST_ASSERT_TRUE(h.most <= 22 && x.most <= 22);
  }
}

// ---------------------------------------------------------------------------
// The card worker's 6 KB of stack: no heap, and frames that stay small.
// ---------------------------------------------------------------------------
class StackProbe : public cc::Source {
public:
  explicit StackProbe(const Bytes& b) : m_(b.data(), static_cast<uint32_t>(b.size())) {}
  uint32_t size() const override { return m_.size(); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    volatile char here = 0;
    const uintptr_t at = reinterpret_cast<uintptr_t>(&here);
    if (!lowest || at < lowest) lowest = at;
    return m_.read(offset, out, n);
  }
  uintptr_t lowest = 0;

private:
  cc::MemSource m_;
};

void test_no_heap_and_little_stack() {
  // Static: this frame's own objects aren't what's measured.
  static const Bytes file = golden("waves-0000002a.bin");
  static StackProbe src(file);
  static mpwf::Table t;
  static mpwf::Slot s;
  volatile char top = 0;
  const uintptr_t at = reinterpret_cast<uintptr_t>(&top);
  const size_t before = gAllocs;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::Ok), static_cast<int>(t.open(src)));
  const size_t openBytes = at - src.lowest;
  src.lowest = 0;
  for (uint32_t r = 0; r < 7; ++r) TEST_ASSERT_EQUAL_INT(static_cast<int>(mpwf::Lookup::Hit), static_cast<int>(t.find(hashAt(file, r), &s)));
  const size_t findBytes = at - src.lowest;
  TEST_ASSERT_EQUAL_size_t(before, gAllocs);
  std::printf("[waves] the stack under a read on this host: open() %u bytes, find() %u bytes\n",
              static_cast<unsigned>(openBytes), static_cast<unsigned>(findBytes));
  std::fflush(stdout);
  TEST_ASSERT_TRUE(openBytes < 1024);
  TEST_ASSERT_TRUE(findBytes < 1024);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_constants);
  RUN_TEST(test_the_bars);
  RUN_TEST(test_the_slot_crcs);
  RUN_TEST(test_the_goldens_written);
  RUN_TEST(test_the_goldens_read_back);
  RUN_TEST(test_one_step_of_the_search);
  RUN_TEST(test_each_lookups_rows);
  RUN_TEST(test_the_search_on_tables_of_every_shape);
  RUN_TEST(test_the_row_limit);
  RUN_TEST(test_the_hardening_files);
  RUN_TEST(test_the_roots_row);
  RUN_TEST(test_the_root_written);
  RUN_TEST(test_a_failed_read_a_damaged_slot_a_broken_writer);
  RUN_TEST(test_the_writers_edge_cases);
  RUN_TEST(test_the_search_reads_as_the_vectors_say);
  RUN_TEST(test_the_search_on_paths_of_the_synthetic_cards_shape);
  RUN_TEST(test_no_heap_and_little_stack);
  return UNITY_END();
}
