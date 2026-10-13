// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the AutoDJ index's own rules (lib/core/CardAutoDj,
// MPDJ v2, docs/AUTODJ-INDEX.md), past what the shared fixtures pin
// (test_card_files: the goldens, the hardening files, truncation and
// flipped bits; test_card_contract: the vectors): the slot and header
// geometry for every k and section count; the strided ranks' properties,
// and the counting-sort selection against a plain sort over random levels;
// one slot's checks, each in its turn (8.4); the writer's edge cases (one
// row, one song, path-hash collisions down to an empty DJPH, both slot
// sizes, both kinds, m below, at and just past k) and a refusal no fixture
// can hold; rows with no song
// key; the level's rounding and its summation order; the selection
// signature's text, byte for byte, and every input it covers.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using minijson::Value;
using Bytes = std::vector<uint8_t>;
using List = std::vector<std::pair<int, int>>;  // (row, level), best first

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

Bytes hexBytes(const std::string& s) {
  Bytes out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

// ---------------------------------------------------------------------------
// Slots by hand.
// ---------------------------------------------------------------------------
void resealSlot(Bytes& s, uint32_t row) {
  cc::put32(&s[s.size() - 4], mpdj::slotCrc(s.data(), static_cast<uint32_t>(s.size()), row));
}

// Row `row`'s slot at k holding `entries` (a row of -1: an unused entry,
// written as 0xFFFF with its level), its CRC made right.
Bytes makeSlot(uint32_t k, uint32_t row, const List& entries) {
  Bytes s(mpdj::slotBytesFor(k), 0);
  for (uint32_t j = 0; j < k; ++j) {
    const bool given = j < entries.size();
    cc::put16(&s[2 * j], given && entries[j].first >= 0 ? static_cast<uint16_t>(entries[j].first) : mpdj::kUnused);
    s[2 * k + j] = given ? static_cast<uint8_t>(entries[j].second) : 0;
  }
  resealSlot(s, row);
  return s;
}

std::string slotWhy(const Bytes& s, uint32_t k, uint32_t row, uint32_t rowCount, mpdj::SlotRead* read = nullptr,
                    List* got = nullptr) {
  mpdj::Entry e[mpdj::kMaxK];
  uint32_t c = 99;
  cc::Why why = cc::Why::Io;
  const mpdj::SlotRead r = mpdj::checkSlot(s.data(), static_cast<uint32_t>(s.size()), k, row, rowCount, e, &c, &why);
  if (read) *read = r;
  if (r != mpdj::SlotRead::Ok) TEST_ASSERT_EQUAL_UINT32(0, c);
  if (got) {
    got->clear();
    for (uint32_t j = 0; j < c; ++j) got->emplace_back(e[j].row, e[j].level);
  }
  return cc::whyName(why);
}

// ---------------------------------------------------------------------------
// Tables by hand: rows with their paths, embeddings and song keys.
// ---------------------------------------------------------------------------
struct Library {
  struct Row {
    std::vector<float> e;
    std::vector<uint64_t> paths;
    std::string song;
    bool hasSong = false;
  };
  std::vector<Row> rows;
  std::vector<mpdj::RowIn> in;

  // A row whose hash's first byte is `id` (so rows are in the order added
  // when the ids rise).
  Row& add(uint8_t id, std::vector<float> e, std::vector<std::string> files, const char* song = nullptr) {
    Row r;
    r.e = std::move(e);
    for (const auto& f : files) r.paths.push_back(cc::pathHash(f.c_str()));
    if (song) {
      r.song = song;
      r.hasSong = true;
    }
    rows.push_back(r);
    ids.push_back(id);
    return rows.back();
  }
  mpdj::TableIn table(uint32_t k, uint8_t kind = mpdj::kSelTop, uint32_t head = 0, uint32_t floor = 0) {
    in.clear();
    for (size_t i = 0; i < rows.size(); ++i) {
      mpdj::RowIn x;
      x.hash[0] = ids[i];
      x.hash[15] = static_cast<uint8_t>(i);
      x.paths = rows[i].paths.data();
      x.pathCount = static_cast<uint32_t>(rows[i].paths.size());
      x.songKey = rows[i].hasSong ? rows[i].song.c_str() : nullptr;
      x.embedding = rows[i].e.data();
      in.push_back(x);
    }
    mpdj::TableIn t;
    t.generation = 3;
    t.k = k;
    t.selKind = kind;
    t.selHead = head;
    t.selFloor = floor;
    t.modelId = "synthetic-embedding";
    t.modelVersion = "1";
    t.license = "CC0-1.0";
    t.dim = rows.empty() ? 0 : static_cast<uint32_t>(rows[0].e.size());
    t.rows = in.data();
    t.rowCount = static_cast<uint32_t>(in.size());
    return t;
  }
  std::vector<uint8_t> ids;
};

Bytes build(const mpdj::TableIn& in, uint8_t sig[16] = nullptr) {
  VecSink out;
  uint8_t s[16];
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mpdj::write(out, in, sig ? sig : s, nullptr, nullptr, &error), error ? error : "");
  return out.b;
}

// Every row's list, the file checked whole first.
std::vector<List> listsOf(const Bytes& file, mpdj::Table* table = nullptr) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mpdj::Table t;
  uint8_t scratch[1024], buf[mpdj::kLargeSlot];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.check(mpdj::Table::kUseAll, scratch, sizeof(scratch))));
  std::vector<List> out(t.rowCount());
  mpdj::Entry e[mpdj::kMaxK];
  for (uint32_t r = 0; r < t.rowCount(); ++r) {
    uint32_t c = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Ok), static_cast<int>(t.slot(r, buf, e, &c)));
    for (uint32_t j = 0; j < c; ++j) out[r].emplace_back(e[j].row, e[j].level);
  }
  if (table) *table = t;
  return out;
}

void assertList(const List& want, const List& got) {
  TEST_ASSERT_EQUAL_size_t(want.size(), got.size());
  for (size_t j = 0; j < want.size(); ++j) {
    TEST_ASSERT_EQUAL_INT(want[j].first, got[j].first);
    TEST_ASSERT_EQUAL_INT(want[j].second, got[j].second);
  }
}

// A unit vector in the plane whose cosine to (1, 0) is level / 255: its
// level to (1, 0) is `level` (cos x 255 lands within 1e-5 of it).
std::vector<float> atLevel(int level) {
  const double c = level / 255.0;
  return {static_cast<float>(c), static_cast<float>(std::sqrt(1 - c * c))};
}

// The selection signature's text (6), written out plainly.
std::string sigText(const mpdj::SigIn& in) {
  char buf[96];
  std::string t = "MPDJ-SEL 2\nmodel " + std::string(in.modelId) + " " + in.modelVersion + "\nmetric " + in.metric;
  std::snprintf(buf, sizeof(buf), "\nk %u\n", static_cast<unsigned>(in.k));
  t += buf;
  if (in.selKind == mpdj::kSelTop) {
    t += "sel top\n";
  } else {
    std::snprintf(buf, sizeof(buf), "sel strided %u %u\n", static_cast<unsigned>(in.selHead),
                  static_cast<unsigned>(in.selFloor));
    t += buf;
  }
  std::snprintf(buf, sizeof(buf), "vec %u ", static_cast<unsigned>(in.dim));
  t += buf;
  for (int i = 0; i < 32; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x", in.vecSha256[i]);
    t += buf;
  }
  t += "\n";
  for (uint32_t r = 0; r < in.rowCount; ++r) {
    for (int i = 0; i < 16; ++i) {
      std::snprintf(buf, sizeof(buf), "%02x", in.hashes[r][i]);
      t += buf;
    }
    t += "\n";
  }
  uint32_t at = 0;
  for (uint32_t g = 0; g < in.groupCount; ++g) {
    t += "same";
    for (; at < in.groupEnds[g]; ++at) t += " " + std::to_string(in.groupRows[at]);
    t += "\n";
  }
  return t;
}

Bytes sigOf(const mpdj::SigIn& in) {
  uint8_t s[16];
  mpdj::selectionSignature(in, s);
  return Bytes(s, s + 16);
}

Bytes digestOf(const std::string& text) {
  mpdj::Sha256 sha;
  sha.update(text.data(), text.size());
  uint8_t d[32];
  sha.final(d);
  return Bytes(d, d + 16);
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The geometry (3.3, 4.3).
// ---------------------------------------------------------------------------
void test_slot_and_header_bytes() {
  for (uint32_t k = 0; k <= 300; ++k) {
    const uint32_t sb = mpdj::slotBytesFor(k);
    if (k < mpdj::kMinK || k > mpdj::kMaxK) {
      TEST_ASSERT_EQUAL_UINT32(0, sb);
      continue;
    }
    TEST_ASSERT_EQUAL_UINT32(k <= mpdj::kMaxSmallK ? 256 : 512, sb);
    TEST_ASSERT_TRUE(3 * k + 4 <= sb);  // the entries and the CRC fit
  }
  TEST_ASSERT_EQUAL_UINT32(256, 3 * mpdj::kMaxSmallK + 4);  // no padding at 84
  TEST_ASSERT_TRUE(3 * (mpdj::kMaxSmallK + 1) + 4 > 256);   // 85 needs 512
  TEST_ASSERT_EQUAL_UINT32(511, 3 * mpdj::kMaxK + 4);       // 1 byte of padding at 169 (byte 507)
  TEST_ASSERT_TRUE(3 * (mpdj::kMaxK + 1) + 4 > 512);
  // The alignment rule: the smallest multiple of 8, at least the header,
  // that ends the directory on a slot boundary.
  for (uint32_t n = 0; n <= cc::kMaxSections; ++n)
    for (uint32_t sb : {256u, 512u})
      for (uint32_t minimum : {104u, 112u, 200u, 260u}) {
        const uint32_t hb = mpdj::headerBytesFor(n, sb, minimum);
        TEST_ASSERT_EQUAL_UINT32(0, hb % 8);
        TEST_ASSERT_TRUE(hb >= minimum);
        TEST_ASSERT_EQUAL_UINT32(0, (hb + 32 * n) % sb);
        TEST_ASSERT_TRUE(hb - 8 < minimum || (hb - 8 + 32 * n) % sb != 0);
      }
  TEST_ASSERT_EQUAL_UINT32(128, mpdj::headerBytesFor(4, 256));
  TEST_ASSERT_EQUAL_UINT32(384, mpdj::headerBytesFor(4, 512));
  TEST_ASSERT_EQUAL_UINT32(352, mpdj::headerBytesFor(5, 256));
  TEST_ASSERT_EQUAL_UINT32(0, mpdj::headerBytesFor(4, 0));
  TEST_ASSERT_EQUAL_UINT32(0, mpdj::headerBytesFor(4, 300));
  // The type header, 40-103, and the reader's spec.
  TEST_ASSERT_EQUAL_UINT32(104, mpdj::kMinHeaderBytes);
  TEST_ASSERT_EQUAL_UINT32(104, mpdj::kSpec.headerBytes);
  TEST_ASSERT_EQUAL_UINT16(2, mpdj::kSpec.major);
  TEST_ASSERT_EQUAL_UINT32(4, mpdj::kSpec.count);
}

// ---------------------------------------------------------------------------
// The selection (5.3, 5.4).
// ---------------------------------------------------------------------------
void test_strided_ranks() {
  for (uint32_t k = 3; k <= 40; ++k)
    for (uint32_t h = 1; h + 2 <= k; ++h)
      for (uint32_t m = k + 1; m <= k + 60; ++m) {
        for (uint32_t j = 0; j < h; ++j) TEST_ASSERT_EQUAL_UINT32(j, mpdj::strideRank(j, k, h, m));
        TEST_ASSERT_EQUAL_UINT32(h, mpdj::strideRank(h, k, h, m));
        TEST_ASSERT_EQUAL_UINT32(m - 1, mpdj::strideRank(k - 1, k, h, m));
        for (uint32_t j = 1; j < k; ++j)
          TEST_ASSERT_TRUE(mpdj::strideRank(j, k, h, m) > mpdj::strideRank(j - 1, k, h, m));
      }
  // Not strided: the top k.
  for (uint32_t j = 0; j < 169; ++j) TEST_ASSERT_EQUAL_UINT32(j, mpdj::strideRank(j, 169, 84, 169));
  // The largest library: 32 bits hold every product.
  TEST_ASSERT_EQUAL_UINT32(65533, mpdj::strideRank(168, 169, 84, 65534));
  TEST_ASSERT_EQUAL_UINT32(84 + 83 * 65449 / 84, mpdj::strideRank(167, 169, 84, 65534));
  // The vectors' cases (AUTODJ-INDEX.md 5.4).
  const uint32_t r4[] = {0, 1, 2, 4};
  for (uint32_t j = 0; j < 4; ++j) TEST_ASSERT_EQUAL_UINT32(r4[j], mpdj::strideRank(j, 4, 1, 5));
  TEST_ASSERT_EQUAL_UINT32(109, mpdj::strideRank(85, 169, 84, 2258));
  TEST_ASSERT_EQUAL_UINT32(1170, mpdj::strideRank(126, 169, 84, 2258));
  TEST_ASSERT_EQUAL_UINT32(169, mpdj::strideRank(168, 169, 84, 170));  // rank 168 skipped
  TEST_ASSERT_EQUAL_UINT32(167, mpdj::strideRank(167, 169, 84, 170));

  // The counting sort against a plain sort, over random levels with many
  // ties, random exclusions and every kind of k, head and floor.
  std::mt19937 rng(20261013);
  std::vector<uint16_t> levels, order;
  mpdj::Entry out[mpdj::kMaxK];
  for (int iter = 0; iter < 3000; ++iter) {
    const uint32_t n = 1 + rng() % 400;
    const uint32_t k = mpdj::kMinK + rng() % (mpdj::kMaxK - 1);
    const uint8_t kind = rng() % 3 == 0 ? mpdj::kSelTop : mpdj::kSelStrided;
    const uint32_t head = 1 + rng() % (k - 2 + (k == 2));
    const uint32_t floor = 1 + rng() % 255;
    const uint32_t spread = 1 + rng() % 256;  // narrow: many ties
    const uint32_t base = rng() % 256;
    levels.assign(n, 0);
    order.assign(n, 0);
    std::vector<std::pair<int, uint32_t>> plain;  // (-level, row)
    for (uint32_t b = 0; b < n; ++b) {
      if (rng() % 10 == 0) {
        levels[b] = mpdj::kNotCandidate;
        continue;
      }
      levels[b] = static_cast<uint16_t>(std::min<uint32_t>(255, base + rng() % spread));
      plain.emplace_back(-static_cast<int>(levels[b]), b);
    }
    std::sort(plain.begin(), plain.end());
    uint32_t m = 0;
    while (m < plain.size() && -plain[m].first >= static_cast<int>(floor)) ++m;
    const bool strided = kind == mpdj::kSelStrided && k >= 3 && m > k;
    const uint32_t c = mpdj::selectEntries(levels.data(), n, kind, k, head, floor, out, order.data());
    TEST_ASSERT_EQUAL_UINT32(std::min<size_t>(k, plain.size()), c);
    for (uint32_t j = 0; j < c; ++j) {
      const uint32_t r = strided && j >= head ? head + (j - head) * (m - 1 - head) / (k - 1 - head) : j;
      TEST_ASSERT_EQUAL_UINT16(plain[r].second, out[j].row);
      TEST_ASSERT_EQUAL_UINT8(-plain[r].first, out[j].level);
    }
  }
}

// ---------------------------------------------------------------------------
// One slot (8.4).
// ---------------------------------------------------------------------------
void test_check_slot() {
  const List good = {{0, 200}, {5, 171}, {4, 150}, {3, 128}};
  mpdj::SlotRead read;
  List got;
  // Ok: the entries, best first.
  Bytes s = makeSlot(4, 2, good);
  TEST_ASSERT_EQUAL_size_t(256, s.size());
  TEST_ASSERT_EQUAL_STRING("ok", slotWhy(s, 4, 2, 6, &read, &got).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Ok), static_cast<int>(read));
  assertList(good, got);
  // S1, Damaged: the CRC itself, a byte under it, a slot read for another row.
  s[252] ^= 1;
  TEST_ASSERT_EQUAL_STRING("djSlotCrc", slotWhy(s, 4, 2, 6, &read).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Damaged), static_cast<int>(read));
  s = makeSlot(4, 2, good);
  s[100] = 1;  // padding, under the CRC
  TEST_ASSERT_EQUAL_STRING("djSlotCrc", slotWhy(s, 4, 2, 6, &read).c_str());
  s = makeSlot(4, 2, good);
  TEST_ASSERT_EQUAL_STRING("djSlotCrc", slotWhy(s, 4, 3, 6, &read).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Damaged), static_cast<int>(read));
  // A bad CRC over a bad list is Damaged: the CRC comes first.
  s = makeSlot(4, 2, {{2, 200}});
  s[253] ^= 1;
  TEST_ASSERT_EQUAL_STRING("djSlotCrc", slotWhy(s, 4, 2, 6, &read).c_str());
  // S2, Broken: a used entry after an unused one.
  TEST_ASSERT_EQUAL_STRING("djSlotUnused",
                           slotWhy(makeSlot(4, 2, {{0, 200}, {-1, 0}, {4, 150}}), 4, 2, 6, &read).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Broken), static_cast<int>(read));
  // S3: a row out of range, the slot's own row (0xFFFE is a row too).
  TEST_ASSERT_EQUAL_STRING("djSlotIndex", slotWhy(makeSlot(4, 2, {{0, 200}, {6, 100}}), 4, 2, 6, &read).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Broken), static_cast<int>(read));
  TEST_ASSERT_EQUAL_STRING("djSlotIndex", slotWhy(makeSlot(4, 2, {{0, 200}, {2, 100}}), 4, 2, 6).c_str());
  TEST_ASSERT_EQUAL_STRING("djSlotIndex", slotWhy(makeSlot(4, 2, {{0xFFFE, 200}}), 4, 2, 6).c_str());
  // S4: a level rising; a tie whose rows fall, or repeat.
  TEST_ASSERT_EQUAL_STRING("djSlotOrder", slotWhy(makeSlot(4, 2, {{0, 200}, {5, 201}}), 4, 2, 6, &read).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Broken), static_cast<int>(read));
  TEST_ASSERT_EQUAL_STRING("djSlotOrder", slotWhy(makeSlot(4, 2, {{0, 200}, {5, 171}, {4, 171}}), 4, 2, 6).c_str());
  TEST_ASSERT_EQUAL_STRING("djSlotOrder", slotWhy(makeSlot(4, 2, {{4, 171}, {4, 171}}), 4, 2, 6).c_str());
  TEST_ASSERT_EQUAL_STRING("ok", slotWhy(makeSlot(4, 2, {{4, 171}, {5, 171}}), 4, 2, 6).c_str());
  // Each check its own pass: S2 before S3 before S4, whatever the entry.
  TEST_ASSERT_EQUAL_STRING("djSlotUnused", slotWhy(makeSlot(4, 2, {{9, 1}, {-1, 0}, {0, 200}}), 4, 2, 6).c_str());
  TEST_ASSERT_EQUAL_STRING("djSlotIndex", slotWhy(makeSlot(4, 2, {{0, 1}, {5, 200}, {9, 100}}), 4, 2, 6).c_str());
  // Not checked: an unused entry's level, the padding (the CRC covers both),
  // a row listed twice (a reader MAY check it).
  TEST_ASSERT_EQUAL_STRING("ok", slotWhy(makeSlot(4, 2, {{0, 200}, {-1, 7}}), 4, 2, 6, nullptr, &got).c_str());
  assertList({{0, 200}}, got);
  s = makeSlot(4, 2, good);
  s[200] = 0x5A;
  resealSlot(s, 2);
  TEST_ASSERT_EQUAL_STRING("ok", slotWhy(s, 4, 2, 6).c_str());
  TEST_ASSERT_EQUAL_STRING("ok", slotWhy(makeSlot(4, 2, {{3, 200}, {3, 100}}), 4, 2, 6).c_str());
  // No entry at all.
  TEST_ASSERT_EQUAL_STRING("ok", slotWhy(makeSlot(4, 2, {}), 4, 2, 6, nullptr, &got).c_str());
  TEST_ASSERT_EQUAL_size_t(0, got.size());
  // Full slots at both sizes: 84 (no padding) and 169 (byte 507, unchecked).
  for (uint32_t k : {mpdj::kMaxSmallK, mpdj::kMaxK}) {
    List full;
    for (uint32_t j = 0; j < k; ++j) full.emplace_back(j + 1, 255 - static_cast<int>(j));
    s = makeSlot(k, 0, full);
    TEST_ASSERT_EQUAL_STRING("ok", slotWhy(s, k, 0, 300, nullptr, &got).c_str());
    assertList(full, got);
    if (k == mpdj::kMaxK) {
      s[507] = 0xA5;
      resealSlot(s, 0);
      TEST_ASSERT_EQUAL_STRING("ok", slotWhy(s, k, 0, 300).c_str());
    }
  }
  // A k the slot can't hold: refused, never read past it.
  s = makeSlot(4, 2, good);
  TEST_ASSERT_EQUAL_STRING("shape", slotWhy(s, 85, 2, 6, &read).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Broken), static_cast<int>(read));
}

// ---------------------------------------------------------------------------
// The writer (4, 5).
// ---------------------------------------------------------------------------
void test_writer_edge_cases() {
  // One row: no candidate, c 0; the file reads.
  {
    Library lib;
    lib.add(1, {1, 0}, {"A/x.mp3"}, "a|x");
    const Bytes f = build(lib.table(4));
    const auto lists = listsOf(f);
    TEST_ASSERT_EQUAL_size_t(1, lists.size());
    TEST_ASSERT_EQUAL_size_t(0, lists[0].size());
  }
  // Every row one song: every list empty.
  {
    Library lib;
    lib.add(1, {1, 0}, {"A/x.mp3"}, "a|x");
    lib.add(2, {0, 1}, {"A/y.mp3"}, "a|x");
    lib.add(3, {0.6f, 0.8f}, {"B/z.mp3"}, "a|x");
    for (const List& l : listsOf(build(lib.table(4)))) TEST_ASSERT_EQUAL_size_t(0, l.size());
  }
  // A path hash two card files hold (across rows, and twice in one row): it
  // is left out of DJPH; the rows stay in the table, neighbours still.
  {
    Library lib;
    lib.add(1, {1, 0}, {"A/x.mp3", "Shared/s.mp3"});
    lib.add(2, {0.6f, 0.8f}, {"Shared/s.mp3"});
    lib.add(3, {0, 1}, {"C/z.mp3", "C/z.mp3"});
    lib.add(4, {0.8f, 0.6f}, {"D/w.mp3"});
    const Bytes f = build(lib.table(4));
    mpdj::Table t;
    const auto lists = listsOf(f, &t);
    cc::MemSource src(f.data(), static_cast<uint32_t>(f.size()));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    TEST_ASSERT_EQUAL_UINT32(4, t.rowCount());
    TEST_ASSERT_EQUAL_UINT32(2, t.pathCount());  // A/x.mp3 and D/w.mp3
    TEST_ASSERT_EQUAL_UINT32(0, t.rowOf(cc::pathHash("A/x.mp3")));
    TEST_ASSERT_EQUAL_UINT32(3, t.rowOf(cc::pathHash("D/w.mp3")));
    TEST_ASSERT_EQUAL_UINT32(mpdj::kNoRow, t.rowOf(cc::pathHash("Shared/s.mp3")));
    TEST_ASSERT_EQUAL_UINT32(mpdj::kNoRow, t.rowOf(cc::pathHash("C/z.mp3")));
    TEST_ASSERT_EQUAL_size_t(3, lists[0].size());
    TEST_ASSERT_EQUAL_INT(1, lists[3][0].first);  // row 1, with no path, is a neighbour
  }
  // Every card file's hash in a collision: pathCount 0, DJPH empty but
  // still an array of stride 10 (4.5); the rows and their lists stay.
  {
    Library lib;
    lib.add(1, {1, 0}, {"Shared/a.mp3"});
    lib.add(2, {0.6f, 0.8f}, {"Shared/a.mp3", "Shared/b.mp3"});
    lib.add(3, {0, 1}, {"Shared/b.mp3"});
    const Bytes f = build(lib.table(4));
    const auto lists = listsOf(f);
    mpdj::Table t;
    cc::MemSource src(f.data(), static_cast<uint32_t>(f.size()));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    TEST_ASSERT_EQUAL_UINT32(3, t.rowCount());
    TEST_ASSERT_EQUAL_UINT32(0, t.pathCount());
    TEST_ASSERT_EQUAL_UINT32(mpdj::kNoRow, t.rowOf(cc::pathHash("Shared/a.mp3")));
    const uint8_t* e = &f[cc::get32(&f[8]) + 32 * 2];  // DJPH's directory entry
    TEST_ASSERT_EQUAL_UINT32(mpdj::kDjph, cc::get32(e));
    TEST_ASSERT_EQUAL_UINT32(0, cc::get32(e + 12));  // bytes
    TEST_ASSERT_EQUAL_UINT32(0, cc::get32(e + 16));  // count
    TEST_ASSERT_EQUAL_UINT32(mpdj::kPathStride, cc::get32(e + 20));
    assertList({{1, 153}, {2, 0}}, lists[0]);
    assertList({{2, 204}, {0, 153}}, lists[1]);
  }
  // m below, at and just past k (k 4, head 1, floor 100): the top k, down
  // below the floor; the top k; strided, rank 3 skipped.
  for (int above = 3; above <= 5; ++above) {
    Library lib;
    lib.add(0, {1, 0}, {"Seed.mp3"});
    const int lv[] = {250, 240, 230, 220, 210, 90, 80};
    for (int i = 0; i < 7; ++i) {
      const int level = i < above ? lv[i] : lv[5 + (i - above) % 2] - (i - above);
      lib.add(static_cast<uint8_t>(i + 1), atLevel(level), {"R/" + std::to_string(i) + ".mp3"});
    }
    const auto lists = listsOf(build(lib.table(4, mpdj::kSelStrided, 1, 100)));
    const List& l = lists[0];
    TEST_ASSERT_EQUAL_size_t(4, l.size());
    if (above == 3) assertList({{1, 250}, {2, 240}, {3, 230}, {4, 90}}, l);  // below the floor
    if (above == 4) assertList({{1, 250}, {2, 240}, {3, 230}, {4, 220}}, l);
    if (above == 5) assertList({{1, 250}, {2, 240}, {3, 230}, {5, 210}}, l);
  }
  // Both kinds, both slot sizes, against the rule computed plainly: 200
  // random rows of 12 dimensions near one axis (most pairs above the floor).
  std::mt19937 rng(7);
  std::normal_distribution<float> noise(0.0f, 0.15f);
  Library lib;
  for (int i = 0; i < 200; ++i) {
    std::vector<float> e(12);
    double s = 0;
    for (int d = 0; d < 12; ++d) {
      e[d] = (d == 0 ? 1.0f : 0.0f) + noise(rng);
      s += static_cast<double>(e[d]) * e[d];
    }
    for (float& x : e) x = static_cast<float>(x / std::sqrt(s));
    char song[16];
    std::snprintf(song, sizeof(song), "s|%d", i % 150);  // 50 pairs of one song
    lib.add(static_cast<uint8_t>(i), e, {"G/" + std::to_string(i) + ".mp3"}, i % 7 == 0 ? nullptr : song);
  }
  struct Case {
    uint32_t k;
    uint8_t kind;
    uint32_t head, floor;
  };
  const Case cases[] = {{2, mpdj::kSelTop, 0, 0},           {3, mpdj::kSelStrided, 1, 140},
                        {24, mpdj::kSelStrided, 8, 140},    {84, mpdj::kSelTop, 0, 0},
                        {85, mpdj::kSelStrided, 30, 200},   {169, mpdj::kSelStrided, 84, 140},
                        {169, mpdj::kSelTop, 0, 0}};
  for (const Case& c : cases) {
    mpdj::TableIn in = lib.table(c.k, c.kind, c.head, c.floor);
    if (c.kind == mpdj::kSelTop) {
      in.selHead = 7;  // ignored: written as k and 0
      in.selFloor = 9;
    }
    mpdj::Table t;
    const auto lists = listsOf(build(in), &t);
    TEST_ASSERT_EQUAL_UINT32(mpdj::slotBytesFor(c.k), t.slotBytes());
    TEST_ASSERT_EQUAL_UINT32(c.kind == mpdj::kSelTop ? c.k : c.head, t.selHead());
    TEST_ASSERT_EQUAL_UINT32(c.kind == mpdj::kSelTop ? 0 : c.floor, t.selFloor());
    bool sawStrided = false;
    for (uint32_t a = 0; a < 200; ++a) {
      const mpdj::RowIn& ra = in.rows[a];  // ids rise: the order added
      std::vector<std::pair<int, uint32_t>> cand;
      for (uint32_t b = 0; b < 200; ++b) {
        const mpdj::RowIn& rb = in.rows[b];
        if (b == a || (ra.songKey && rb.songKey && std::strcmp(ra.songKey, rb.songKey) == 0)) continue;
        cand.emplace_back(-static_cast<int>(mpdj::level(ra.embedding, rb.embedding, 12)), b);
      }
      std::sort(cand.begin(), cand.end());
      uint32_t m = 0;
      while (m < cand.size() && -cand[m].first >= static_cast<int>(c.floor)) ++m;
      const bool strided = c.kind == mpdj::kSelStrided && m > c.k;
      sawStrided = sawStrided || strided;
      List want;
      for (uint32_t j = 0; j < c.k && j < cand.size(); ++j) {
        const uint32_t r = strided && j >= c.head ? c.head + (j - c.head) * (m - 1 - c.head) / (c.k - 1 - c.head) : j;
        want.emplace_back(cand[r].second, -cand[r].first);
      }
      assertList(want, lists[a]);
      // The first min(selHead, c) entries are the exact top, whatever the kind.
      for (uint32_t j = 0; j < std::min<size_t>(t.selHead(), lists[a].size()); ++j)
        TEST_ASSERT_EQUAL_INT(static_cast<int>(cand[j].second), lists[a][j].first);
    }
    if (c.kind == mpdj::kSelStrided) TEST_ASSERT_TRUE(sawStrided);
  }
  // 65,536 rows: refused before a row is read.
  mpdj::TableIn many = lib.table(4);
  many.rowCount = mpdj::kMaxRows + 1;
  VecSink out;
  uint8_t sig[16];
  const char* error = nullptr;
  TEST_ASSERT_FALSE(mpdj::write(out, many, sig, nullptr, nullptr, &error));
  TEST_ASSERT_NOT_NULL(error);
}

// A row with no song key (a blank title) is nobody's same song: two such
// rows of one artist list each other; it lists, and is listed by, rows of
// a song. Two rows of one song key don't list each other.
void test_no_song_key() {
  Library lib;
  lib.add(1, {1, 0}, {"Generated Artist 32/01 - Sample Title 32.mp3"});
  lib.add(2, {0.8f, 0.6f}, {"Generated Artist 32/02 - Sample Title 32.mp3"});
  lib.add(3, {0.6f, 0.8f}, {"Generated Artist 33/01.mp3"}, "generated artist 33|sample title 33");
  lib.add(4, {0, 1}, {"Generated Artist 33/02.mp3"}, "generated artist 33|sample title 33");
  uint8_t sig[16];
  const auto lists = listsOf(build(lib.table(4), sig));
  assertList({{1, 204}, {2, 153}, {3, 0}}, lists[0]);
  assertList({{2, 245}, {0, 204}, {3, 153}}, lists[1]);
  assertList({{1, 245}, {0, 153}}, lists[2]);  // not row 3, its song
  assertList({{1, 153}, {0, 0}}, lists[3]);
  // The signature's same line holds rows 2 and 3 alone.
  mpdj::TableIn in = lib.table(4);
  std::vector<const float*> emb;
  Bytes hashes;
  for (const auto& r : lib.in) {
    emb.push_back(r.embedding);
    hashes.insert(hashes.end(), r.hash, r.hash + 16);
  }
  mpdj::SigIn si;
  si.modelId = in.modelId;
  si.modelVersion = in.modelVersion;
  si.metric = in.metric;
  si.k = 4;
  si.selKind = mpdj::kSelTop;
  si.selHead = 4;
  si.dim = 2;
  mpdj::vecDigest(emb.data(), 4, 2, si.vecSha256);
  si.hashes = reinterpret_cast<const uint8_t(*)[16]>(hashes.data());
  si.rowCount = 4;
  const uint32_t groupRows[] = {2, 3}, groupEnds[] = {2};
  si.groupRows = groupRows;
  si.groupEnds = groupEnds;
  si.groupCount = 1;
  TEST_ASSERT_EQUAL_MEMORY(sigOf(si).data(), sig, 16);
  TEST_ASSERT_TRUE(sigText(si).find("\nsame 2 3\n") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The level (5.2).
// ---------------------------------------------------------------------------
void test_level_rounding() {
  TEST_ASSERT_EQUAL_UINT8(255, mpdj::level(1.0));
  TEST_ASSERT_EQUAL_UINT8(255, mpdj::level(1.5));  // clamped
  TEST_ASSERT_EQUAL_UINT8(128, mpdj::level(0.5));  // 127.5: half away from zero
  TEST_ASSERT_EQUAL_UINT8(127, mpdj::level(std::nextafter(0.5, 0.0)));
  TEST_ASSERT_EQUAL_UINT8(100, mpdj::level(0.390625));  // 99.609375
  TEST_ASSERT_EQUAL_UINT8(1, mpdj::level(1 / 255.0));
  TEST_ASSERT_EQUAL_UINT8(0, mpdj::level(0.0));
  TEST_ASSERT_EQUAL_UINT8(0, mpdj::level(-0.0));
  TEST_ASSERT_EQUAL_UINT8(0, mpdj::level(-0.5));
  TEST_ASSERT_EQUAL_UINT8(0, mpdj::level(-1.0));
  TEST_ASSERT_EQUAL_UINT8(0, mpdj::level(std::nan("")));
  // 139.5 is 140: the default floor's own boundary.
  TEST_ASSERT_EQUAL_UINT8(140, mpdj::level(139.5 / 255.0 + 1e-12));
  TEST_ASSERT_EQUAL_UINT8(139, mpdj::level(139.5 / 255.0 - 1e-12));
  // Two rows: an exact 0.5 (autodj-00000029's rows 2 and 3, x 16).
  const float a[8] = {6 / 16.f, 0, 3 / 16.f, 1 / 16.f, 7 / 16.f, 6 / 16.f, 10 / 16.f, -5 / 16.f};
  const float b[8] = {9 / 16.f, -1 / 16.f, 5 / 16.f, 9 / 16.f, -1 / 16.f, 7 / 16.f, 3 / 16.f, 3 / 16.f};
  TEST_ASSERT_EQUAL_UINT8(128, mpdj::level(a, b, 8));
  TEST_ASSERT_EQUAL_UINT8(128, mpdj::level(b, a, 8));
  // The index order is the rule: 2^60 + 1 - 2^60 sums to 0 in it (any
  // other order could give 1, level 255).
  const float big[3] = {1073741824.0f, 1.0f, -1073741824.0f}, pos[3] = {1073741824.0f, 1.0f, 1073741824.0f};
  TEST_ASSERT_EQUAL_UINT8(0, mpdj::level(big, pos, 3));
}

// ---------------------------------------------------------------------------
// The selection signature (6).
// ---------------------------------------------------------------------------
void test_selection_signature() {
  // Its text, written out plainly, is the vectors' byte for byte.
  Value v;
  TEST_ASSERT_TRUE(cardfixtures::json("vectors.json", &v));
  const Value& sig = v["mpdj"]["signature"];
  Value lib;
  TEST_ASSERT_TRUE(cardfixtures::json("libraries/autodj-00000029.json", &lib));
  std::vector<std::pair<std::string, std::vector<float>>> rows;
  for (size_t i = 0; i < lib["rows"].size(); ++i) {
    std::vector<float> e;
    for (size_t k = 0; k < lib["rows"][i]["embedding"].size(); ++k)
      e.push_back(static_cast<float>(lib["rows"][i]["embedding"][k].num()));
    rows.emplace_back(lib["rows"][i]["hash"].str(), e);
  }
  std::sort(rows.begin(), rows.end());
  Bytes hashes;
  std::vector<const float*> emb;
  for (const auto& r : rows) {
    const Bytes h = hexBytes(r.first);
    hashes.insert(hashes.end(), h.begin(), h.end());
    emb.push_back(r.second.data());
  }
  mpdj::SigIn in;
  in.modelId = "synthetic-embedding";
  in.modelVersion = "1";
  in.metric = "cosine";
  in.k = 4;
  in.selKind = mpdj::kSelStrided;
  in.selHead = 1;
  in.selFloor = 100;
  in.dim = 8;
  mpdj::vecDigest(emb.data(), 6, 8, in.vecSha256);
  in.hashes = reinterpret_cast<const uint8_t(*)[16]>(hashes.data());
  in.rowCount = 6;
  const uint32_t groupRows[] = {3, 4, 5}, groupEnds[] = {3};  // rows 3-5: one song
  in.groupRows = groupRows;
  in.groupEnds = groupEnds;
  in.groupCount = 1;
  TEST_ASSERT_EQUAL_STRING(sig["text"].c_str(), sigText(in).c_str());
  TEST_ASSERT_EQUAL_MEMORY(hexBytes(sig["sig"].str()).data(), sigOf(in).data(), 16);
  // Every input it covers changes it; the plain text's digest always
  // agrees with selectionSignature().
  std::vector<Bytes> seen = {sigOf(in)};
  auto differs = [&](const mpdj::SigIn& x, const char* what) {
    const Bytes s = sigOf(x);
    TEST_ASSERT_TRUE_MESSAGE(digestOf(sigText(x)) == s, what);
    for (const Bytes& o : seen) TEST_ASSERT_FALSE_MESSAGE(o == s, what);
    seen.push_back(s);
  };
  mpdj::SigIn x = in;
  x.k = 5;
  differs(x, "k");
  x = in;
  x.selKind = mpdj::kSelTop;
  x.selHead = 4;
  x.selFloor = 0;
  differs(x, "kind");
  x = in;
  x.selHead = 2;
  differs(x, "head");
  x = in;
  x.selFloor = 101;
  differs(x, "floor");
  x = in;
  x.dim = 16;
  differs(x, "dim");
  x = in;
  x.metric = "dot";
  differs(x, "metric");
  x = in;
  x.modelVersion = "2";
  differs(x, "model version");
  // An empty field is written as it is: its separating space stays.
  x = in;
  x.modelVersion = "";
  differs(x, "an empty model version");
  TEST_ASSERT_TRUE(sigText(x).find("\nmodel synthetic-embedding \nmetric cosine\n") != std::string::npos);
  x = in;
  x.vecSha256[31] ^= 1;
  differs(x, "a vector");
  x = in;
  x.groupCount = 0;
  differs(x, "no group");
  const uint32_t twoRows[] = {3, 4}, twoEnds[] = {2};
  x = in;
  x.groupRows = twoRows;
  x.groupEnds = twoEnds;
  differs(x, "a smaller group");
  const uint32_t twoGroups[] = {0, 1, 3, 4, 5}, twoGroupEnds[] = {2, 5};
  x = in;
  x.groupRows = twoGroups;
  x.groupEnds = twoGroupEnds;
  x.groupCount = 2;
  differs(x, "two groups");
  TEST_ASSERT_TRUE(sigText(x).find("\nsame 0 1\nsame 3 4 5\n") != std::string::npos);
  x = in;
  x.rowCount = 5;
  x.groupCount = 0;
  differs(x, "a row fewer");
  // A vector's bytes, not its value: -0 is another vector than +0.
  const float zero[2] = {0.0f, 1.0f}, negZero[2] = {-0.0f, 1.0f};
  const float* z1[1] = {zero};
  const float* z2[1] = {negZero};
  uint8_t d1[32], d2[32];
  mpdj::vecDigest(z1, 1, 2, d1);
  mpdj::vecDigest(z2, 1, 2, d2);
  TEST_ASSERT_FALSE(std::memcmp(d1, d2, 32) == 0);
  // The writer's signature is this one, and the header's.
  Library l;
  l.add(1, {1, 0}, {"A/x.mp3"}, "a|x");
  l.add(2, {0, 1}, {"A/y.mp3"}, "a|y");
  uint8_t ws[16];
  const Bytes f = build(l.table(169, mpdj::kSelStrided, 84, 140), ws);
  TEST_ASSERT_EQUAL_MEMORY(ws, &f[64], 16);
  mpdj::SigIn w;
  w.modelId = "synthetic-embedding";
  w.modelVersion = "1";
  w.k = 169;
  w.selKind = mpdj::kSelStrided;
  w.selHead = 84;
  w.selFloor = 140;
  w.dim = 2;
  const float* we[2] = {l.rows[0].e.data(), l.rows[1].e.data()};
  mpdj::vecDigest(we, 2, 2, w.vecSha256);
  Bytes wh;
  for (const auto& r : l.in) wh.insert(wh.end(), r.hash, r.hash + 16);
  w.hashes = reinterpret_cast<const uint8_t(*)[16]>(wh.data());
  w.rowCount = 2;
  TEST_ASSERT_EQUAL_MEMORY(sigOf(w).data(), ws, 16);
  TEST_ASSERT_TRUE(sigText(w).find("\nsel strided 84 140\nvec 2 ") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_slot_and_header_bytes);
  RUN_TEST(test_strided_ranks);
  RUN_TEST(test_check_slot);
  RUN_TEST(test_writer_edge_cases);
  RUN_TEST(test_no_song_key);
  RUN_TEST(test_level_rounding);
  RUN_TEST(test_selection_signature);
  return UNITY_END();
}
