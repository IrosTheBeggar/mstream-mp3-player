// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for a track's shape (lib/core/WaveShape, docs/WAVEFORM-
// INDEX.md 11): the reduction on made-up shapes (silent, flat, one loud peak, a
// brick with a quiet intro, the 10th percentile's edge, 9 reached, every whole
// rounding), against the rule again in f64 on random shapes; the length's
// edges; the card worker's read over the shared fixtures (test/fixtures/card):
// the goldens' rows against heights worked out apart from this code, each
// lookup of the vectors, every mpwf-* hardening file to its verdict and lookup,
// each root's row, a file that isn't the root's, a failed read at every read, a
// file that didn't open; the job's memory used again; no heap and little stack.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
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
#include "WaveShape.h"

namespace cc = cardcontract;
namespace mpwf = cardcontract::mpwf;
namespace msmf = cardcontract::msmf;
using minijson::Value;
using Bytes = std::vector<uint8_t>;
using wave::kColumns;
using wave::Read;

// Every allocation counted: the read is to make none.
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

void setUp() {}
void tearDown() {}

namespace {

constexpr int kBars = mpwf::kBarCount;

// Column i's bars: 5's rule in full, M = 240 for 99 columns.
void rangeOf(int i, int* start, int* end) {
  *start = i * kBars / kColumns;
  *end = std::min(kBars, std::max(*start + 1, (i + 1) * kBars / kColumns));
}

std::string digits(const uint8_t* heights) {
  std::string s;
  for (int i = 0; i < kColumns; ++i) s += heights[i] <= 9 ? static_cast<char>('0' + heights[i]) : '?';
  return s;
}

std::string reduced(const uint8_t* bars) {
  uint8_t h[kColumns];
  std::memset(h, 0xEE, sizeof(h));
  wave::reduce(bars, h);
  return digits(h);
}

// Bars that make each column's energy exactly its whole value.
void barsOf(const int* columns, uint8_t* bars) {
  for (int i = 0; i < kColumns; ++i) {
    int start, end;
    rangeOf(i, &start, &end);
    for (int b = start; b < end; ++b) bars[b] = static_cast<uint8_t>(columns[i]);
  }
}

std::string reducedColumns(const int* columns) {
  uint8_t bars[kBars];
  barsOf(columns, bars);
  return reduced(bars);
}

std::string times(char c, int n) { return std::string(static_cast<size_t>(n), c); }

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

// The row a root would have for this file: its own header's values.
msmf::Comp rowFor(const Bytes& file) {
  msmf::Comp e;
  e.kind = cc::kMagicMpwf;
  if (file.size() >= 36) {
    e.fileBytes = cc::get32(&file[16]);
    e.generation = cc::get32(&file[20]);
    e.headerCrc = cc::get32(&file[32]);
  }
  return e;
}

// The job's memory, as the device keeps it: one of each, used again.
struct Job {
  mpwf::Table table;
  mpwf::Slot slot;
  wave::ReadResult result;
};

Job& theJob() {
  static Job j;
  return j;
}

const wave::ReadResult& readFile(const Bytes& file, const msmf::Comp& entry, uint64_t hash) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  Job& j = theJob();
  wave::readShape(src, entry, hash, &j.table, &j.slot, &j.result);
  return j.result;
}

const wave::ReadResult& readFile(const Bytes& file, uint64_t hash) { return readFile(file, rowFor(file), hash); }

// A source whose reads fail on demand: the `failAt`th from now, 1 the next.
class FlakySource : public cc::Source {
public:
  explicit FlakySource(const Bytes& b) : m_(b.data(), static_cast<uint32_t>(b.size())) {}
  uint32_t size() const override { return m_.size(); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    ++reads;
    if (failAt == 1) {
      failAt = 0;
      return false;
    }
    if (failAt) --failAt;
    return m_.read(offset, out, n);
  }
  uint32_t failAt = 0;
  uint32_t reads = 0;

private:
  cc::MemSource m_;
};

// The rows of waves-0000002a.bin (WAVEFORM-INDEX.md 13.4), each with its heights
// worked out in Python from the golden's bytes by the rule in f64, apart from
// this code.
struct GoldenRow {
  uint64_t hash;
  uint32_t lengthMs;
  const char* heights;
};
const GoldenRow kRows[] = {
    {0x57017C0CC011BB1Bull, 193000,  // 80 bars each of 30, 40, 50
     "000000000000000000000000000000000555555555555555555555555555555555999999999999999999999999999999999"},
    {0x6137B0E2A6715752ull, 183000,  // a made-up song
     "000012456834433434434433444344344333344479889888888989889997443344333443344433368766554332210000000"},
    {0x954A7DE3A079A42Eull, 186000,  // silent, and it has a slot
     "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
    {0xC48674AC937F2ADAull, 0,  // a brick, at 255: nothing to stretch, at its own level
     "999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"},
    {0xCADB3EFDEF4B2DD9ull, 181000,  // bar 3k + 2 is k + 1, the rest 0
     "000000111101111012120222220323202333034430343404445405454045650565605657057676076860688707879079797"},
    {0xDA70B312760188D8ull, 180000,  // a ramp to 254
     "000000000000001111111111222222222233333333334444444444555555555566666666667777777777888888888899999"},
    {0xF1B24FC757494F2Bull, 192000,  // 120 bars each of 10, 20
     "000000000000000000000000000000000000000000000000049999999999999999999999999999999999999999999999999"},
};

// The stack under a call: painted below this frame, then looked at.
constexpr size_t kPaintBytes = 16384;
__attribute__((noinline)) size_t stackPaint(bool paint) {
  volatile uint8_t pad[kPaintBytes];
  if (paint) {
    for (size_t i = 0; i < kPaintBytes; ++i) pad[i] = 0xA5;
    return 0;
  }
  size_t i = 0;
  while (i < kPaintBytes && pad[i] == 0xA5) ++i;
  return kPaintBytes - i;
}

}  // namespace

// ---------------------------------------------------------------------------
// The reduction.
// ---------------------------------------------------------------------------
// Each bar is in one column by 5's rule, 2 or 3 bars a column: one bar lit lights
// one column.
void test_every_bar_is_in_one_column() {
  int next = 0;
  for (int i = 0; i < kColumns; ++i) {
    int start, end;
    rangeOf(i, &start, &end);
    TEST_ASSERT_EQUAL_INT(next, start);
    TEST_ASSERT_TRUE(end - start == 2 || end - start == 3);
    TEST_ASSERT_EQUAL_INT((i + 1) * kBars / kColumns, end);  // (the min and the max don't bind)
    next = end;
    for (int b = start; b < end; ++b) {
      uint8_t bars[kBars] = {};
      bars[b] = 255;
      TEST_ASSERT_EQUAL_STRING((times('0', i) + "9" + times('0', kColumns - 1 - i)).c_str(), reduced(bars).c_str());
    }
  }
  TEST_ASSERT_EQUAL_INT(kBars, next);
}

void test_a_silent_track_is_all_zero() {
  const uint8_t bars[kBars] = {};
  TEST_ASSERT_EQUAL_STRING(times('0', kColumns).c_str(), reduced(bars).c_str());
}

// Nothing to stretch: at its own level, the whole height, whatever that level.
void test_a_flat_track_is_at_its_own_level() {
  for (int level : {1, 2, 3, 64, 128, 254, 255}) {
    uint8_t bars[kBars];
    std::memset(bars, level, sizeof(bars));
    TEST_ASSERT_EQUAL_STRING(times('9', kColumns).c_str(), reduced(bars).c_str());
  }
}

void test_one_loud_peak() {
  // On silence: its column alone, and not the bars beside it.
  uint8_t bars[kBars] = {};
  bars[120] = 255;
  int start, end, column = -1;
  for (int i = 0; i < kColumns; ++i) {
    rangeOf(i, &start, &end);
    if (start <= 120 && 120 < end) column = i;
  }
  const std::string want = times('0', column) + "9" + times('0', kColumns - 1 - column);
  TEST_ASSERT_EQUAL_STRING(want.c_str(), reduced(bars).c_str());
  // A quiet peak on silence is the same: the stretch makes it the whole height.
  bars[120] = 3;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), reduced(bars).c_str());
  // On a low level: the level is the floor. RMS, not the peak: the same column.
  std::memset(bars, 4, sizeof(bars));
  bars[120] = 255;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), reduced(bars).c_str());
  // Across two columns, each at its RMS against the louder: column 0 takes bars
  // 0 and 1 (e 200), column 1 bars 2 and 3, one of them lit (e 141.4: x 6.36).
  std::memset(bars, 0, sizeof(bars));
  for (int i = 0; i < 3; ++i) bars[i] = 200;
  TEST_ASSERT_EQUAL_STRING(("96" + times('0', kColumns - 2)).c_str(), reduced(bars).c_str());
}

// What the stretch is for. A brick (at face value 230-250 is rows 8 and 9) with
// a short quiet intro: the intro's columns are below the 10th percentile and
// don't anchor the floor, so the body gets every height.
void test_a_brick_with_a_quiet_intro() {
  int columns[kColumns];
  // 9 silent columns, then a body that rises from 230 to 250.
  for (int i = 0; i < kColumns; ++i) columns[i] = i < 9 ? 0 : 230 + (i - 9) * 20 / (kColumns - 10);
  uint8_t bars[kBars];
  barsOf(columns, bars);
  uint8_t h[kColumns];
  wave::reduce(bars, h);
  std::set<int> used;
  for (int i = 0; i < 9; ++i) TEST_ASSERT_EQUAL_UINT8(0, h[i]);
  for (int i = 9; i < kColumns; ++i) {
    used.insert(h[i]);
    if (i > 9) TEST_ASSERT_TRUE(h[i] >= h[i - 1]);
  }
  TEST_ASSERT_EQUAL_UINT8(0, h[9]);
  TEST_ASSERT_EQUAL_UINT8(9, h[kColumns - 1]);
  TEST_ASSERT_EQUAL_size_t(10, used.size());  // every height
  // The 10th percentile's edge: the 10th column from the smallest is the floor.
  // 10 silent columns are a quiet part, not an intro, and the floor is 0: the
  // body is the brick it was.
  columns[9] = 0;
  barsOf(columns, bars);
  wave::reduce(bars, h);
  for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL_UINT8(0, h[i]);
  for (int i = 10; i < kColumns; ++i) TEST_ASSERT_TRUE(h[i] == 8 || h[i] == 9);
  // Wherever the quiet columns stand: an outro as well as an intro.
  for (int i = 0; i < kColumns; ++i) columns[i] = i >= kColumns - 9 ? 0 : 240;
  TEST_ASSERT_EQUAL_STRING((times('9', kColumns - 9) + times('0', 9)).c_str(), reducedColumns(columns).c_str());
  for (int i = 0; i < kColumns; ++i) columns[i] = i >= 40 && i < 49 ? 0 : 240;
  TEST_ASSERT_EQUAL_STRING((times('9', 40) + times('0', 9) + times('9', 50)).c_str(), reducedColumns(columns).c_str());
  // Columns below the floor are 0, not less: 8 at 0, 1 at 100, the 10th smallest
  // at 200.
  for (int i = 0; i < kColumns; ++i) columns[i] = i < 8 ? 0 : i == 8 ? 100 : i == kColumns - 1 ? 250 : 200;
  TEST_ASSERT_EQUAL_STRING((times('0', kColumns - 1) + "9").c_str(), reducedColumns(columns).c_str());
}

// Whole energies: the exact rule, floor((18 (e - f) + s) / 2s), on every pair of
// a loudest and an energy, at floors of 0 and others. Half rounds up: 0.5 is 1,
// 2.5 is 3 and 4.5 is 5, where half to even gives 0, 2 and 4.
void test_the_rounding() {
  struct Case {
    int e, h;
  };
  const Case cases[] = {{9, 0},   {10, 1},  {11, 1},  {29, 1},  {30, 2},  {49, 2},  {50, 3},  {70, 4},
                        {89, 4},  {90, 5},  {110, 6}, {130, 7}, {150, 8}, {169, 8}, {170, 9}, {179, 9}};
  for (const Case& c : cases) {
    int columns[kColumns] = {};
    columns[10] = 180;
    columns[20] = c.e;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<char>('0' + c.h), reducedColumns(columns)[20], "a rounding");
  }
  uint32_t checked = 0;
  for (int quiet : {0, 1, 7, 100, 200, 254}) {
    for (int loud = quiet + 1; loud <= 255; ++loud) {
      for (int e = quiet; e <= loud;) {
        int columns[kColumns];
        for (int i = 0; i < 10; ++i) columns[i] = quiet;
        columns[10] = loud;
        for (int i = 11; i < kColumns; ++i) columns[i] = e <= loud ? e++ : quiet;
        const std::string got = reducedColumns(columns);
        for (int i = 0; i < kColumns; ++i) {
          const int span = loud - quiet;
          const int want = (18 * (columns[i] - quiet) + span) / (2 * span);
          TEST_ASSERT_EQUAL_INT_MESSAGE(want, got[i] - '0', "the exact rule");
          ++checked;
        }
      }
    }
  }
  TEST_ASSERT_TRUE(checked > 100000);
}

namespace {

// The rule again, in f64, as the terminal's resample_bars has it.
struct Model {
  double e[kColumns];
  double x[kColumns];
  double quiet = 0, loudest = 0;
  bool stretched = false;

  explicit Model(const uint8_t* bars) {
    double sorted[kColumns];
    for (int i = 0; i < kColumns; ++i) {
      int start, end;
      rangeOf(i, &start, &end);
      double sum = 0;
      for (int b = start; b < end; ++b) sum += static_cast<double>(bars[b]) * bars[b];
      e[i] = std::sqrt(sum / (end - start));
      sorted[i] = e[i];
    }
    std::sort(sorted, sorted + kColumns);
    quiet = sorted[kColumns / 10];
    loudest = sorted[kColumns - 1];
    stretched = loudest - quiet > DBL_EPSILON;
    const double floorE = stretched ? quiet : 0;
    const double span = stretched ? loudest - quiet : loudest;
    for (int i = 0; i < kColumns; ++i)
      x[i] = loudest <= 0 ? 0 : std::min(1.0, std::max(0.0, (e[i] - floorE) / span)) * 9;
  }
};

void randomShape(std::mt19937& rng, int kind, uint8_t* bars) {
  auto within = [&](int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<uint32_t>(hi - lo + 1)); };
  switch (kind) {
    case 0:  // anything
      for (int b = 0; b < kBars; ++b) bars[b] = static_cast<uint8_t>(within(0, 255));
      break;
    case 1:  // a loud master
      for (int b = 0; b < kBars; ++b) bars[b] = static_cast<uint8_t>(within(225, 255));
      break;
    case 2: {  // fades in and out of a loud body
      const int fade = within(1, 40);
      for (int b = 0; b < kBars; ++b) {
        const int edge = std::min(b, kBars - 1 - b);
        const int top = edge < fade ? 255 * edge / fade : 255;
        bars[b] = static_cast<uint8_t>(within(top * 8 / 10, top));
      }
      break;
    }
    case 3: {  // a few peaks on silence
      std::memset(bars, 0, kBars);
      for (int n = within(1, 12); n > 0; --n) bars[within(0, kBars - 1)] = static_cast<uint8_t>(within(1, 255));
      break;
    }
    case 4:  // a quiet recording
      for (int b = 0; b < kBars; ++b) bars[b] = static_cast<uint8_t>(within(0, 12));
      break;
    case 5: {  // verses and choruses
      int level = within(40, 200);
      for (int b = 0; b < kBars; ++b) {
        if (b % within(20, 60) == 0) level = within(40, 240);
        bars[b] = static_cast<uint8_t>(std::min(255, level + within(0, 15)));
      }
      break;
    }
    default:  // flat, but for one bar
      std::memset(bars, within(0, 255), kBars);
      bars[within(0, kBars - 1)] = static_cast<uint8_t>(within(0, 255));
      break;
  }
}

}  // namespace

// The loudest column is 9, whatever its energy (no rounding leaves it a row
// short), and a column at the floor is 0: on many a column's RMS, the rest of the
// track silent, then at a low level.
void test_nine_is_reached() {
  for (int a = 0; a <= 255; a += 7) {
    for (int b = 1; b <= 255; b += 11) {
      for (int rest : {0, 1}) {
        uint8_t bars[kBars], h[kColumns];
        std::memset(bars, rest, sizeof(bars));
        bars[0] = static_cast<uint8_t>(a);
        bars[1] = static_cast<uint8_t>(b);
        wave::reduce(bars, h);
        if (Model(bars).e[0] <= rest) continue;  // (no louder than the rest: another shape)
        TEST_ASSERT_EQUAL_UINT8(9, h[0]);
        TEST_ASSERT_EQUAL_UINT8(0, h[1]);
        TEST_ASSERT_EQUAL_UINT8(0, h[kColumns - 1]);
      }
    }
  }
}

// The model on random shapes of every kind (a value within 0.0001 of a tie may go
// either way between the two precisions). The same runs: the loudest column is 9,
// the 10 smallest 0 when there was a stretch, and a louder column is never lower.
void test_against_the_rule_in_f64() {
  std::mt19937 rng(20261102);
  uint32_t ties = 0, stretched = 0, flat = 0;
  for (int iter = 0; iter < 3500; ++iter) {
    uint8_t bars[kBars], h[kColumns];
    randomShape(rng, iter % 7, bars);
    wave::reduce(bars, h);
    const Model m(bars);
    int zeros = 0, nines = 0;
    for (int i = 0; i < kColumns; ++i) {
      const double up = m.x[i] + 0.5;
      const int want = static_cast<int>(std::floor(up));
      if (std::fabs(up - std::round(up)) < 0.0001) {
        TEST_ASSERT_TRUE_MESSAGE(h[i] == std::lround(up) || h[i] == std::lround(up) - 1, "near a tie");
        ++ties;
      } else {
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, h[i], "the model");
      }
      zeros += h[i] == 0;
      nines += h[i] == 9;
      for (int k = 0; k < i; ++k) {
        if (m.e[k] > m.e[i]) TEST_ASSERT_TRUE(h[k] >= h[i]);
        if (m.e[i] > m.e[k]) TEST_ASSERT_TRUE(h[i] >= h[k]);
      }
    }
    if (m.loudest > 0) TEST_ASSERT_TRUE(nines >= 1);
    if (m.stretched) {
      TEST_ASSERT_TRUE(zeros >= 10);
      ++stretched;
    } else {
      ++flat;
    }
  }
  TEST_ASSERT_TRUE(stretched > 3000);
  TEST_ASSERT_TRUE(flat > 100);
  std::printf("[wave] against f64: %u shapes stretched, %u not, %u columns near a tie\n",
              static_cast<unsigned>(stretched), static_cast<unsigned>(flat), static_cast<unsigned>(ties));
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// The length.
// ---------------------------------------------------------------------------
static_assert(wave::lengthAgrees(0, 0), "a constexpr: the page may ask in a constant");

void test_the_length_agrees() {
  struct Case {
    uint32_t slot, player;
    bool agrees;
    const char* what;
  };
  const Case cases[] = {
      {0, 0, true, "unknown, both"},
      {0, 1, true, "the slot's unknown"},
      {0, 180000, true, "the slot's unknown"},
      {0, 0xFFFFFFFFu, true, "the slot's unknown"},
      // Short tracks: 2 s either way.
      {180000, 180000, true, "equal"},
      {102000, 100000, true, "2 s over"},
      {102001, 100000, false, "2 s and 1 ms over"},
      {98000, 100000, true, "2 s under"},
      {97999, 100000, false, "2 s and 1 ms under"},
      // 2 % from 100 s up: 100,049 ms still has 2,000 (a whole division).
      {102049, 100049, true, "at 100.049 s"},
      {102050, 100049, false, "at 100.049 s"},
      {102051, 100050, true, "at 100.05 s: 2,001"},
      {102052, 100050, false, "at 100.05 s: 2,001"},
      {204000, 200000, true, "2 % of 200 s over"},
      {204001, 200000, false, "just past 2 % of 200 s"},
      {196000, 200000, true, "2 % of 200 s under"},
      {195999, 200000, false, "just past 2 % of 200 s"},
      // 2 % of the player's, not the slot's: the same two times, both ways round.
      {3672000, 3600000, true, "an hour: 72 s over"},
      {3672001, 3600000, false, "an hour"},
      {3528000, 3600000, true, "an hour: 72 s under"},
      {3527999, 3600000, false, "an hour"},
      {3600000, 3672000, true, "the same two, the other way"},
      {3600000, 3528000, false, "the same two: 2 % of the shorter"},
      // The player's unknown: 2 s from nothing.
      {2000, 0, true, "the player's unknown"},
      {2001, 0, false, "the player's unknown"},
      {180000, 0, false, "the player's unknown"},
      // No wrap: an i32 difference would read these as close.
      {0xFFFFFFFFu, 1, false, "the far ends"},
      {1, 0xFFFFFFFFu, false, "the far ends"},
      {0x80000000u, 0, false, "2^31"},
      {0xFFFFFFFFu, 0xFFFFFFFFu, true, "equal, at the top"},
      {0xFFFFFFFFu - 85899345u, 0xFFFFFFFFu, true, "2 % of the top"},
      {0xFFFFFFFFu - 85899346u, 0xFFFFFFFFu, false, "just past 2 % of the top"},
  };
  for (const Case& c : cases) TEST_ASSERT_EQUAL_MESSAGE(c.agrees, wave::lengthAgrees(c.slot, c.player), c.what);
}

// ---------------------------------------------------------------------------
// The read: the goldens, the lookups.
// ---------------------------------------------------------------------------
void test_the_goldens_read() {
  const Bytes file = golden("waves-0000002a.bin");
  for (const GoldenRow& row : kRows) {
    const wave::ReadResult& r = readFile(file, row.hash);
    TEST_ASSERT_EQUAL_STRING("hit", wave::readName(r.read));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(r.why));
    TEST_ASSERT_EQUAL_UINT32(row.lengthMs, r.shape.lengthMs);
    TEST_ASSERT_EQUAL_STRING(row.heights, digits(r.shape.heights).c_str());
    TEST_ASSERT_TRUE(r.slotReads >= 1);
  }
  // A track with no slot.
  const wave::ReadResult& m = readFile(file, cc::pathHash("Artist/Album/03 - Not There.mp3"));
  TEST_ASSERT_EQUAL_STRING("miss", wave::readName(m.read));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.why));
  TEST_ASSERT_TRUE(m.slotReads >= 1);
  // The smallest file: one row at one level, nothing to stretch.
  const Bytes one = golden("waves-one.bin");
  const wave::ReadResult& o = readFile(one, 0xDA70B312760188D8ull);
  TEST_ASSERT_EQUAL_STRING("hit", wave::readName(o.read));
  TEST_ASSERT_EQUAL_UINT32(180000, o.shape.lengthMs);
  TEST_ASSERT_EQUAL_STRING(times('9', kColumns).c_str(), digits(o.shape.heights).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, o.slotReads);
}

// Each lookup of the vectors: its answer, and the slots it read.
void test_each_lookup_of_the_vectors() {
  const Value v = load("vectors.json");
  const Value& files = v["mpwf"]["search"]["files"];
  TEST_ASSERT_EQUAL_size_t(3, files.size());
  uint32_t lookups = 0;
  for (const auto& kv : files.members) {
    const Bytes file = golden(kv.first);
    for (size_t i = 0; i < kv.second.size(); ++i) {
      const Value& e = kv.second[i];
      const std::string what = kv.first + " " + e["hash"].str();
      const wave::ReadResult& r = readFile(file, e["hash"].u64());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e["read"].c_str(), wave::readName(r.read), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["probes"].size(), r.slotReads, what.c_str());
      ++lookups;
    }
  }
  TEST_ASSERT_TRUE(lookups >= 20);
}

// ---------------------------------------------------------------------------
// The read: every hardening file (13.5). `uses` "all" is a tool's: the device
// reads each as "device", so a slot's or WVSL's fault is a lookup's, or nothing.
// ---------------------------------------------------------------------------
void test_the_hardening_files() {
  const Value index = load("hardening/index.json");
  uint32_t files = 0, frames = 0, lookups = 0, same = 0;
  std::set<std::string> reasons;
  for (size_t i = 0; i < index.size(); ++i) {
    const Value& e = index[i];
    if (e["format"].str() != "MPWF") continue;
    ++files;
    const std::string name = e["file"].str();
    const std::string what = name + ": " + e["check"].str();
    const std::string why = e["why"].str();
    const Bytes b = fixture("hardening/" + name);
    const msmf::Comp row = rowFor(b);
    // The frame and the header (8.1, 8.2): absent, by that reason, whatever is
    // looked up, and no slot is read.
    if (why != "ok" && why != "wvSlotCrc" && why != "wvSlotOrder" && why != "sectionCrc") {
      for (const GoldenRow& g : kRows) {
        const wave::ReadResult& r = readFile(b, row, g.hash);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("absent", wave::readName(r.read), what.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(why.c_str(), cc::whyName(r.why), what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.slotReads, what.c_str());
      }
      reasons.insert(why);
      ++frames;
      continue;
    }
    // One lookup as the device makes it: `slot`'s answer, in `slot`'s reads. A
    // sort rule broken is the file's end: absent.
    if (e.has("slot")) {
      const std::string read = e["slot"]["read"].str();
      const wave::ReadResult& r = readFile(b, row, e["slot"]["hash"].u64());
      const char* wantRead = read == "broken" ? "absent" : read.c_str();
      TEST_ASSERT_EQUAL_STRING_MESSAGE(wantRead, wave::readName(r.read), what.c_str());
      const char* wantWhy = read == "broken" ? "wvSlotOrder" : read == "damaged" ? "wvSlotCrc" : "ok";
      TEST_ASSERT_EQUAL_STRING_MESSAGE(wantWhy, cc::whyName(r.why), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["slot"]["reads"].u64(), r.slotReads, what.c_str());
      reasons.insert(read);
      ++lookups;
    }
    // Read as the golden: a later minor, reserved bytes set, WVSL's REQUIRED bit
    // clear, and WVSL's own CRC wrong, which the device never looks at.
    if (e.has("same") || why == "sectionCrc") {
      for (const GoldenRow& g : kRows) {
        const wave::ReadResult& r = readFile(b, row, g.hash);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("hit", wave::readName(r.read), what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(g.lengthMs, r.shape.lengthMs, what.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(g.heights, digits(r.shape.heights).c_str(), what.c_str());
      }
      ++same;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(50, files);
  TEST_ASSERT_EQUAL_UINT32(34, frames);
  TEST_ASSERT_EQUAL_UINT32(12, lookups);
  TEST_ASSERT_EQUAL_UINT32(4, same);
  for (const char* r : {"short", "magic", "major", "headerBytes", "sectionCount", "fileBytes", "headerCrc", "layout",
                        "unknownRequired", "shape", "missing", "enum", "wvLayout", "hit", "miss", "damaged", "broken"})
    TEST_ASSERT_TRUE_MESSAGE(reasons.count(r) == 1, r);
}

// ---------------------------------------------------------------------------
// The read: the root's row (7.2).
// ---------------------------------------------------------------------------
void test_the_roots_row_says_whether_the_file_counts() {
  const Value v = load("vectors.json");
  const Value& rootRow = v["mpwf"]["rootRow"];
  std::vector<std::pair<std::string, const Value*>> roots;
  roots.emplace_back("golden/" + rootRow["golden"]["file"].str(), &rootRow["golden"]);
  for (size_t i = 0; i < rootRow["hardening"].size(); ++i)
    roots.emplace_back("hardening/" + rootRow["hardening"][i]["file"].str(), &rootRow["hardening"][i]);
  uint32_t reads = 0, absents = 0;
  for (const auto& f : roots) {
    const char* what = f.first.c_str();
    if ((*f.second)["waves"].isNull()) continue;  // no row: nothing to read
    const Bytes b = fixture(f.first);
    cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
    uint8_t scratch[256];
    msmf::Manifest m;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(m.open(src, scratch, sizeof(scratch))), what);
    TEST_ASSERT_TRUE_MESSAGE(m.hasWaves(), what);
    const Bytes file = golden(m.wavesName());
    const wave::ReadResult& r = readFile(file, m.waves(), kRows[5].hash);
    if ((*f.second)["matches"].boolean()) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("hit", wave::readName(r.read), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(kRows[5].heights, digits(r.shape.heights).c_str(), what);
      ++reads;
    } else {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("absent", wave::readName(r.read), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("comp", cc::whyName(r.why), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.slotReads, what);
      ++absents;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(4, reads);
  TEST_ASSERT_EQUAL_UINT32(3, absents);
}

// Each field of the row against a good file, and no slot read: a file that isn't
// the root's has nothing to say, a good lookup or not.
void test_a_file_that_isnt_the_roots() {
  const Bytes file = golden("waves-0000002a.bin");
  const msmf::Comp row = rowFor(file);
  const wave::ReadResult& ok = readFile(file, row, kRows[0].hash);
  TEST_ASSERT_EQUAL_STRING("hit", wave::readName(ok.read));
  msmf::Comp e = row;
  e.generation = row.generation + 1;
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(readFile(file, e, kRows[0].hash).read));
  e = row;
  e.fileBytes = row.fileBytes + 256;
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(readFile(file, e, kRows[0].hash).read));
  e = row;
  e.headerCrc = row.headerCrc ^ 1;
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(readFile(file, e, kRows[0].hash).read));
  e = row;
  e.key[15] = 1;
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(readFile(file, e, kRows[0].hash).read));
  e = row;
  e.kind = cc::kMagicMpdj;
  const wave::ReadResult& r = readFile(file, e, kRows[0].hash);
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(r.read));
  TEST_ASSERT_EQUAL_STRING("comp", cc::whyName(r.why));
  TEST_ASSERT_EQUAL_UINT32(0, r.slotReads);
  // Another good file under this file's row.
  const Bytes one = golden("waves-one.bin");
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(readFile(one, row, kRows[5].hash).read));
}

// ---------------------------------------------------------------------------
// The read: a failed read, at every read of a lookup. Never the file's fault.
// ---------------------------------------------------------------------------
void test_a_failed_read_at_every_read() {
  const Bytes file = golden("waves-0000002a.bin");
  const msmf::Comp row = rowFor(file);
  for (const GoldenRow& g : kRows) {
    FlakySource count(file);
    Job& j = theJob();
    wave::readShape(count, row, g.hash, &j.table, &j.slot, &j.result);
    TEST_ASSERT_EQUAL_STRING("hit", wave::readName(j.result.read));
    const uint32_t allReads = count.reads, slotReads = j.result.slotReads;
    TEST_ASSERT_TRUE(allReads > slotReads);
    for (uint32_t at = 1; at <= allReads; ++at) {
      FlakySource src(file);
      src.failAt = at;
      wave::readShape(src, row, g.hash, &j.table, &j.slot, &j.result);
      TEST_ASSERT_EQUAL_STRING("io", wave::readName(j.result.read));
      TEST_ASSERT_EQUAL_STRING("io", cc::whyName(j.result.why));
      // The slots it got to, the failed one with them; none in the frame's reads.
      const uint32_t frameReads = allReads - slotReads;
      TEST_ASSERT_EQUAL_UINT32(at <= frameReads ? 0 : at - frameReads, j.result.slotReads);
      // And the same memory reads the file next.
      FlakySource again(file);
      wave::readShape(again, row, g.hash, &j.table, &j.slot, &j.result);
      TEST_ASSERT_EQUAL_STRING("hit", wave::readName(j.result.read));
      TEST_ASSERT_EQUAL_STRING(g.heights, digits(j.result.shape.heights).c_str());
    }
  }
}

// A file that didn't open: no Source and no slot read. FatFs's word that it isn't
// on the card turns the file off; anything else is a failed read like the ones
// above, whatever the last read left in the job's memory.
void test_a_file_that_didnt_open() {
  Job& j = theJob();
  const Bytes file = golden("waves-0000002a.bin");
  j.result.jobMs = 77;
  TEST_ASSERT_TRUE(readFile(file, kRows[1].hash).slotReads >= 1);
  wave::openFailed(true, &j.result);
  TEST_ASSERT_EQUAL_STRING("absent", wave::readName(j.result.read));
  TEST_ASSERT_EQUAL_STRING("io", cc::whyName(j.result.why));
  TEST_ASSERT_EQUAL_UINT32(0, j.result.slotReads);
  TEST_ASSERT_TRUE(readFile(file, kRows[1].hash).slotReads >= 1);
  wave::openFailed(false, &j.result);
  TEST_ASSERT_EQUAL_STRING("io", wave::readName(j.result.read));
  TEST_ASSERT_EQUAL_STRING("io", cc::whyName(j.result.why));
  TEST_ASSERT_EQUAL_UINT32(0, j.result.slotReads);
  TEST_ASSERT_EQUAL_UINT32(77, j.result.jobMs);  // (its caller's)
  // And the same memory reads the file next.
  TEST_ASSERT_EQUAL_STRING("hit", wave::readName(readFile(file, kRows[1].hash).read));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(j.result.why));
}

// The job's memory goes from read to read: nothing left by the last one counts,
// and jobMs is its caller's.
void test_the_jobs_memory_is_used_again() {
  const Bytes file = golden("waves-0000002a.bin");
  const Bytes bad = fixture("hardening/mpwf-magic.bin");
  const Bytes damaged = fixture("hardening/mpwf-uses-device-slot-crc.bin");
  const uint64_t damagedHash = 0x954A7DE3A079A42Eull;
  Job& j = theJob();
  j.result.jobMs = 77;
  const char* names[] = {"hit", "miss", "absent", "hit", "damaged", "miss", "hit"};
  const Bytes* files[] = {&file, &file, &bad, &file, &damaged, &file, &file};
  const uint64_t hashes[] = {kRows[1].hash, 12345, kRows[1].hash, kRows[2].hash, damagedHash, 12345, kRows[3].hash};
  const char* whys[] = {"ok", "ok", "magic", "ok", "wvSlotCrc", "ok", "ok"};
  for (int i = 0; i < 7; ++i) {
    const wave::ReadResult& r = readFile(*files[i], hashes[i]);
    TEST_ASSERT_EQUAL_STRING(names[i], wave::readName(r.read));
    TEST_ASSERT_EQUAL_STRING(whys[i], cc::whyName(r.why));
    TEST_ASSERT_EQUAL_UINT32(77, r.jobMs);
    if (i == 2) {
      TEST_ASSERT_EQUAL_UINT32(0, r.slotReads);
    } else {
      TEST_ASSERT_TRUE(r.slotReads >= 1);
    }
  }
  TEST_ASSERT_EQUAL_STRING(kRows[3].heights, digits(j.result.shape.heights).c_str());
  TEST_ASSERT_EQUAL_UINT32(0, j.result.shape.lengthMs);  // row 3's: unknown
  TEST_ASSERT_EQUAL_STRING("?", wave::readName(static_cast<Read>(99)));
}

// ---------------------------------------------------------------------------
// The card worker's 6 KB of stack: no heap, and frames that stay small.
// ---------------------------------------------------------------------------
void test_no_heap_and_little_stack() {
  // Static: this frame's own objects aren't what's measured.
  static const Bytes file = golden("waves-0000002a.bin");
  static cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  static const msmf::Comp row = rowFor(file);
  static uint8_t bars[kBars], heights[kColumns];
  std::mt19937 rng(7);
  randomShape(rng, 0, bars);
  Job& j = theJob();
  const size_t before = gAllocs;
  stackPaint(true);
  wave::reduce(bars, heights);
  const size_t reduceBytes = stackPaint(false);
  stackPaint(true);
  wave::readShape(src, row, kRows[1].hash, &j.table, &j.slot, &j.result);
  const size_t readBytes = stackPaint(false);
  TEST_ASSERT_EQUAL_size_t(before, gAllocs);
  TEST_ASSERT_EQUAL_STRING("hit", wave::readName(j.result.read));
  std::printf("[wave] the stack on this host: reduce() %u bytes, readShape() %u bytes\n",
              static_cast<unsigned>(reduceBytes), static_cast<unsigned>(readBytes));
  std::fflush(stdout);
  TEST_ASSERT_TRUE(reduceBytes > kColumns * sizeof(float));  // (the paint saw it)
  TEST_ASSERT_TRUE(reduceBytes < 1024);
  TEST_ASSERT_TRUE(readBytes < 1024);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_bar_is_in_one_column);
  RUN_TEST(test_a_silent_track_is_all_zero);
  RUN_TEST(test_a_flat_track_is_at_its_own_level);
  RUN_TEST(test_one_loud_peak);
  RUN_TEST(test_a_brick_with_a_quiet_intro);
  RUN_TEST(test_nine_is_reached);
  RUN_TEST(test_the_rounding);
  RUN_TEST(test_against_the_rule_in_f64);
  RUN_TEST(test_the_length_agrees);
  RUN_TEST(test_the_goldens_read);
  RUN_TEST(test_each_lookup_of_the_vectors);
  RUN_TEST(test_the_hardening_files);
  RUN_TEST(test_the_roots_row_says_whether_the_file_counts);
  RUN_TEST(test_a_file_that_isnt_the_roots);
  RUN_TEST(test_a_failed_read_at_every_read);
  RUN_TEST(test_a_file_that_didnt_open);
  RUN_TEST(test_the_jobs_memory_is_used_again);
  RUN_TEST(test_no_heap_and_little_stack);
  return UNITY_END();
}
