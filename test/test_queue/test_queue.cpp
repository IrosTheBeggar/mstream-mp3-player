// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the play queue: QueueModel (edits, positions, keys, undo,
// shuffle and its ranks: docs/QUEUE-MODES.md),
// TrackCatalog (library and built-in ids) and QueueText (the queue saved as
// paths, and read back after a library rebuild). Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ByteStream.h"
#include "LibraryIndex.h"
#include "QueueModel.h"
#include "QueueSaver.h"
#include "QueueText.h"
#include "Shuffle.h"
#include "TrackCatalog.h"

namespace {

// An allocator that can be told to fail, and counts what's live.
struct Heap {
  static long live;
  static long allocs;  // every alloc asked for
  static bool failing;
  static void* alloc(size_t n) {
    ++allocs;
    if (failing) return nullptr;
    ++live;
    return std::malloc(n ? n : 1);
  }
  static void release(void* p) {
    if (!p) return;
    --live;
    std::free(p);
  }
};
long Heap::live = 0;
long Heap::allocs = 0;
bool Heap::failing = false;

// The queue's tracks, in order.
std::vector<uint32_t> tracks(const QueueModel& q) {
  std::vector<uint32_t> t;
  for (uint32_t i = 0; i < q.size(); ++i) t.push_back(q.trackAt(i));
  return t;
}

void expectTracks(const QueueModel& q, std::vector<uint32_t> want) {
  const std::vector<uint32_t> got = tracks(q);
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_UINT32(want[i], got[i]);
}

// 10, 11, 12, ... n tracks: made-up ids (QueueModel doesn't look at them).
void fill(QueueModel& q, uint32_t n, int32_t current = 0) {
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < n; ++i) ids.push_back(10 + i);
  TEST_ASSERT_TRUE(q.assign(ids.data(), n, current));
}

const char* const kFiles[] = {
    "/music/Daft Punk/Discovery/01 - One More Time.mp3",
    "/music/Daft Punk/Discovery/02 - Aerodynamic.mp3",
    "/music/Daft Punk/Discovery/03 - Digital Love.mp3",
    "/music/Kavinsky/OutRun/08 - Nightcall.mp3",
    "/music/Émilie Simon/Végétal/10 - Le voyage de Pénélope.mp3",
    "/music/Root Track.flac",
};

void build(LibraryIndex& idx, const std::vector<const char*>& files) {
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (const char* f : files) TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addFile(f)));
  TEST_ASSERT_TRUE(idx.finish());
}

std::string pathOf(const TrackCatalog& c, uint32_t id) {
  char buf[TrackCatalog::kMaxPath];
  c.path(id, buf, sizeof(buf));
  return buf;
}

}  // namespace

void setUp() {
  Heap::live = 0;
  Heap::failing = false;
}
void tearDown() {}

// ---- QueueModel ----

void test_empty_queue() {
  QueueModel q;
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(-1, q.current());
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.currentTrack());
  TEST_ASSERT_EQUAL_UINT32(0, q.upNext());
  TEST_ASSERT_FALSE(q.step(1, true));
  TEST_ASSERT_FALSE(q.setCurrent(0));
  TEST_ASSERT_FALSE(q.undo());
}

void test_replace_sets_current_at_start() {
  QueueModel q;
  const uint32_t album[] = {5, 6, 7, 8};
  TEST_ASSERT_TRUE(q.replace(album, 4, 2));
  expectTracks(q, {5, 6, 7, 8});
  TEST_ASSERT_EQUAL_INT(2, q.current());
  TEST_ASSERT_EQUAL_UINT32(7, q.currentTrack());
  TEST_ASSERT_EQUAL_UINT32(1, q.upNext());
  TEST_ASSERT_TRUE(q.replace(album, 4, 99));  // past the end: the last one
  TEST_ASSERT_EQUAL_INT(3, q.current());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Replace), static_cast<int>(q.undoable()));
}

void test_insert_next_goes_after_current() {
  QueueModel q;
  fill(q, 4, 1);  // 10 [11] 12 13
  const uint32_t add[] = {1, 2};
  TEST_ASSERT_TRUE(q.insertNext(add, 2));
  expectTracks(q, {10, 11, 1, 2, 12, 13});
  TEST_ASSERT_EQUAL_INT(1, q.current());
  TEST_ASSERT_EQUAL_UINT32(4, q.upNext());
}

void test_append_and_adding_to_an_empty_queue() {
  QueueModel q;
  const uint32_t a[] = {1};
  const uint32_t b[] = {2, 3};
  TEST_ASSERT_TRUE(q.append(a, 1));
  TEST_ASSERT_EQUAL_INT(0, q.current());  // the first new entry is current
  TEST_ASSERT_TRUE(q.append(b, 2));
  expectTracks(q, {1, 2, 3});
  TEST_ASSERT_EQUAL_INT(0, q.current());
  QueueModel q2;
  TEST_ASSERT_TRUE(q2.insertNext(b, 2));
  expectTracks(q2, {2, 3});
  TEST_ASSERT_EQUAL_INT(0, q2.current());
  TEST_ASSERT_TRUE(q2.append(b, 0));  // nothing: no change, no snapshot
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::InsertNext), static_cast<int>(q2.undoable()));
}

void test_keys_are_stable_across_edits() {
  QueueModel q;
  fill(q, 5, 2);  // 10 11 [12] 13 14
  const uint32_t k13 = q.keyAt(3);
  const uint32_t k10 = q.keyAt(0);
  TEST_ASSERT_TRUE(k13 != k10);
  const uint32_t add[] = {1, 2, 3};
  q.insertNext(add, 3);  // 13 moves to 6
  TEST_ASSERT_EQUAL_UINT32(6, q.positionOf(k13));
  const uint32_t rm[] = {0};
  q.remove(rm, 1);  // 10 goes: 13 at 5
  TEST_ASSERT_EQUAL_UINT32(5, q.positionOf(k13));
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.positionOf(k10));
  // New entries never reuse a key.
  const uint32_t again[] = {10};
  q.append(again, 1);
  TEST_ASSERT_TRUE(q.keyAt(q.size() - 1) != k10);
}

void test_remove_before_and_after_current() {
  QueueModel q;
  fill(q, 6, 3);  // 10 11 12 [13] 14 15
  const uint32_t rm[] = {5, 0, 1, 0, 99};  // any order, repeats and junk ignored
  const QueueModel::Removed r = q.remove(rm, 5);
  TEST_ASSERT_EQUAL_UINT32(3, r.count);
  TEST_ASSERT_FALSE(r.current);
  expectTracks(q, {12, 13, 14});
  TEST_ASSERT_EQUAL_INT(1, q.current());
  TEST_ASSERT_EQUAL_UINT32(13, q.currentTrack());
}

void test_remove_current_moves_to_the_next_survivor() {
  QueueModel q;
  fill(q, 5, 1);  // 10 [11] 12 13 14
  const uint32_t rm[] = {1, 2};
  const QueueModel::Removed r = q.remove(rm, 2);
  TEST_ASSERT_TRUE(r.current);
  TEST_ASSERT_FALSE(r.pastEnd);
  expectTracks(q, {10, 13, 14});
  TEST_ASSERT_EQUAL_UINT32(13, q.currentTrack());
}

void test_remove_current_with_nothing_after_it() {
  QueueModel q;
  fill(q, 4, 2);  // 10 11 [12] 13
  const uint32_t rm[] = {2, 3};
  const QueueModel::Removed r = q.remove(rm, 2);
  TEST_ASSERT_TRUE(r.current && r.pastEnd);
  TEST_ASSERT_EQUAL_UINT32(11, q.currentTrack());  // the last one left
  const uint32_t all[] = {0, 1};
  const QueueModel::Removed r2 = q.remove(all, 2);
  TEST_ASSERT_TRUE(r2.current && r2.pastEnd);
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(-1, q.current());
}

void test_remove_nothing_keeps_the_undo() {
  QueueModel q;
  fill(q, 3);
  const uint32_t add[] = {1};
  q.append(add, 1);
  const uint32_t junk[] = {50};
  TEST_ASSERT_EQUAL_UINT32(0, q.remove(junk, 1).count);
  TEST_ASSERT_EQUAL_UINT32(0, q.remove(nullptr, 0).count);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Append), static_cast<int>(q.undoable()));
}

void test_move_next_in_queue_order() {
  QueueModel q;
  fill(q, 7, 1);  // 10 [11] 12 13 14 15 16
  const uint32_t sel[] = {5, 1, 3, 0};  // the current one (1) can't move after itself
  TEST_ASSERT_TRUE(q.moveNext(sel, 4));
  expectTracks(q, {11, 10, 13, 15, 12, 14, 16});
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_EQUAL_UINT32(11, q.currentTrack());
  TEST_ASSERT_TRUE(q.undo());
  expectTracks(q, {10, 11, 12, 13, 14, 15, 16});
  TEST_ASSERT_EQUAL_INT(1, q.current());
}

// "Play next" on the playing entry (the Queue page's inline bar): nothing to
// move, so nothing changes and the undo held is still the older edit's. The
// page tells by the content version (and offers no Undo for it).
void test_move_next_of_the_current_entry_changes_nothing() {
  QueueModel q;
  fill(q, 4, 1);
  const uint32_t add[] = {7};
  q.append(add, 1);
  const uint32_t version = q.contentVersion();
  const uint32_t cur = 1;
  TEST_ASSERT_TRUE(q.moveNext(&cur, 1));
  TEST_ASSERT_EQUAL_UINT32(version, q.contentVersion());
  expectTracks(q, {10, 11, 12, 13, 7});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Append), static_cast<int>(q.undoable()));
}

void test_clear_up_next_keeps_the_current_and_history() {
  QueueModel q;
  fill(q, 5, 2);
  TEST_ASSERT_TRUE(q.clearUpNext());
  expectTracks(q, {10, 11, 12});
  TEST_ASSERT_EQUAL_INT(2, q.current());
  TEST_ASSERT_EQUAL_UINT32(0, q.upNext());
  TEST_ASSERT_TRUE(q.undo());
  TEST_ASSERT_EQUAL_UINT32(5, q.size());
}

void test_clear_and_undo() {
  QueueModel q;
  fill(q, 3, 1);
  const uint32_t key = q.currentKey();
  TEST_ASSERT_TRUE(q.clear());
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(-1, q.current());
  TEST_ASSERT_TRUE(q.undo());
  expectTracks(q, {10, 11, 12});
  TEST_ASSERT_EQUAL_INT(1, q.current());
  TEST_ASSERT_EQUAL_UINT32(key, q.currentKey());  // the same entries, keys and all
  TEST_ASSERT_FALSE(q.undo());                     // one level
}

void test_undo_keeps_what_plays_current() {
  QueueModel q;
  fill(q, 4, 0);  // [10] 11 12 13
  const uint32_t sel[] = {1};
  q.remove(sel, 1);
  q.step(+1, false);  // playing moved on to 12
  TEST_ASSERT_TRUE(q.undo());
  expectTracks(q, {10, 11, 12, 13});
  TEST_ASSERT_EQUAL_UINT32(12, q.currentTrack());  // not back to 10
  // After a replace, what plays isn't in the old queue: its old current.
  const uint32_t one[] = {99};
  q.replace(one, 1, 0);
  TEST_ASSERT_TRUE(q.undo());
  TEST_ASSERT_EQUAL_UINT32(12, q.currentTrack());
}

void test_step_and_set_current() {
  QueueModel q;
  fill(q, 3, 0);
  const uint32_t v = q.positionVersion();
  const uint32_t c = q.contentVersion();
  TEST_ASSERT_FALSE(q.step(-1, false));
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_TRUE(q.step(-1, true));
  TEST_ASSERT_EQUAL_INT(2, q.current());
  TEST_ASSERT_TRUE(q.step(+1, true));
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_TRUE(q.setCurrent(1));
  TEST_ASSERT_FALSE(q.setCurrent(3));
  TEST_ASSERT_TRUE(q.positionVersion() != v);
  TEST_ASSERT_EQUAL_UINT32(c, q.contentVersion());  // moving isn't an edit
}

// peek(): where step() would go, without going (the player's word on what
// follows: docs/GAPLESS.md section 3.1).
void test_peek_is_step_without_the_step() {
  QueueModel q;
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.peek(+1, true));  // empty
  fill(q, 3, 0);
  const uint32_t v = q.positionVersion();
  TEST_ASSERT_EQUAL_UINT32(1, q.peek(+1, false));
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.peek(-1, false));
  TEST_ASSERT_EQUAL_UINT32(2, q.peek(-1, true));
  q.setCurrent(2);
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.peek(+1, false));  // the end, no repeat
  TEST_ASSERT_EQUAL_UINT32(0, q.peek(+1, true));
  TEST_ASSERT_EQUAL_UINT32(1, q.peek(-1, false));
  TEST_ASSERT_EQUAL_INT(2, q.current());
  // A queue of one with repeat: itself.
  QueueModel one;
  fill(one, 1, 0);
  TEST_ASSERT_EQUAL_UINT32(0, one.peek(+1, true));
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, one.peek(+1, false));
  (void)v;
}

void test_memory_from_the_hooks_and_out_of_memory() {
  {
    QueueModel q(Heap::alloc, Heap::release);
    fill(q, 100, 50);
    const uint32_t add[] = {1, 2, 3};
    TEST_ASSERT_TRUE(q.insertNext(add, 3));
    TEST_ASSERT_TRUE(Heap::live > 0);
    // Out of memory: an edit that needs to grow fails and changes nothing.
    std::vector<uint32_t> many(1000, 7);
    Heap::failing = true;
    const uint32_t before = q.contentVersion();
    TEST_ASSERT_FALSE(q.append(many.data(), 1000));
    TEST_ASSERT_FALSE(q.replace(many.data(), 1000, 0));
    TEST_ASSERT_EQUAL_UINT32(103, q.size());
    TEST_ASSERT_EQUAL_UINT32(before, q.contentVersion());
    // No memory for a remove's selection: nothing removed.
    const uint32_t rm[] = {0};
    TEST_ASSERT_EQUAL_UINT32(0, q.remove(rm, 1).count);
    TEST_ASSERT_EQUAL_UINT32(103, q.size());
    Heap::failing = false;
  }
  TEST_ASSERT_EQUAL_INT(0, Heap::live);  // everything went back
}

void test_an_edit_without_memory_for_its_snapshot_is_not_undoable() {
  QueueModel q(Heap::alloc, Heap::release);
  fill(q, 20, 0);
  q.dropUndo();
  Heap::failing = true;  // clear() needs no memory, its snapshot does
  TEST_ASSERT_TRUE(q.clear());
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  TEST_ASSERT_FALSE(q.undo());
  Heap::failing = false;
}

// A long random run against a plain vector model: sizes, order and the
// current entry agree after every edit and undo.
void test_random_edits_match_a_simple_model() {
  QueueModel q;
  struct E {
    uint32_t track, key;
  };
  std::vector<E> m, undoM;
  int32_t cur = -1, undoCur = -1;
  bool canUndo = false;
  auto sync = [&]() {
    TEST_ASSERT_EQUAL_UINT32(m.size(), q.size());
    for (uint32_t i = 0; i < m.size(); ++i) {
      TEST_ASSERT_EQUAL_UINT32(m[i].track, q.trackAt(i));
      TEST_ASSERT_EQUAL_UINT32(m[i].key, q.keyAt(i));
    }
    TEST_ASSERT_EQUAL_INT(cur, q.current());
  };
  auto keysOfNew = [&](uint32_t at, uint32_t n) {
    std::vector<E> added;
    for (uint32_t i = 0; i < n; ++i) added.push_back({q.trackAt(at + i), q.keyAt(at + i)});
    return added;
  };
  uint32_t x = 99;
  auto rnd = [&](uint32_t n) {
    x = x * 1664525u + 1013904223u;
    return n ? (x >> 8) % n : 0;
  };
  for (int it = 0; it < 3000; ++it) {
    const uint32_t op = rnd(9);
    std::vector<uint32_t> ids;
    for (uint32_t i = rnd(4); i > 0; --i) ids.push_back(rnd(50));
    const auto save = [&] {
      undoM = m;
      undoCur = cur;
      canUndo = true;
    };
    if (op == 0) {
      const uint32_t start = rnd(5);
      if (ids.empty()) {
        if (!m.empty()) save();
        q.replace(ids.data(), 0, 0);
        m.clear();
        cur = -1;
      } else {
        save();
        q.replace(ids.data(), static_cast<uint32_t>(ids.size()), start);
        m = keysOfNew(0, static_cast<uint32_t>(ids.size()));
        cur = static_cast<int32_t>(start < ids.size() ? start : ids.size() - 1);
      }
    } else if (op == 1 || op == 2) {
      if (ids.empty()) continue;
      save();
      const uint32_t at = op == 1 ? (cur < 0 ? static_cast<uint32_t>(m.size()) : static_cast<uint32_t>(cur) + 1)
                                  : static_cast<uint32_t>(m.size());
      op == 1 ? q.insertNext(ids.data(), static_cast<uint32_t>(ids.size()))
              : q.append(ids.data(), static_cast<uint32_t>(ids.size()));
      const std::vector<E> added = keysOfNew(at, static_cast<uint32_t>(ids.size()));
      m.insert(m.begin() + at, added.begin(), added.end());
      if (cur < 0) cur = static_cast<int32_t>(at);
    } else if (op == 3 && !m.empty()) {
      std::vector<uint32_t> pos;
      for (uint32_t i = rnd(3) + 1; i > 0; --i) pos.push_back(rnd(static_cast<uint32_t>(m.size()) + 1));
      std::vector<bool> gone(m.size(), false);
      bool any = false;
      for (uint32_t p : pos) {
        if (p < m.size()) gone[p] = any = true;
      }
      if (!any) continue;
      save();
      q.remove(pos.data(), static_cast<uint32_t>(pos.size()));
      std::vector<E> kept;
      int32_t newCur = -1, firstAfter = -1, lastBefore = -1;
      for (uint32_t i = 0; i < m.size(); ++i) {
        if (gone[i]) continue;
        const auto w = static_cast<int32_t>(kept.size());
        if (static_cast<int32_t>(i) < cur) lastBefore = w;
        if (static_cast<int32_t>(i) == cur) newCur = w;
        if (static_cast<int32_t>(i) > cur && firstAfter < 0) firstAfter = w;
        kept.push_back(m[i]);
      }
      m = kept;
      cur = newCur >= 0 ? newCur : (firstAfter >= 0 ? firstAfter : lastBefore);
    } else if (op == 4 && cur >= 0 && static_cast<size_t>(cur) + 1 < m.size()) {
      save();
      q.clearUpNext();
      m.resize(static_cast<size_t>(cur) + 1);
    } else if (op == 5 && !m.empty() && rnd(4) == 0) {
      save();
      q.clear();
      m.clear();
      cur = -1;
    } else if (op == 6 && !m.empty()) {
      const int d = static_cast<int>(rnd(5)) - 2;
      q.step(d, true);
      const auto n = static_cast<int32_t>(m.size());
      cur = ((cur + d) % n + n) % n;
    } else if (op == 7) {
      const bool did = q.undo();
      TEST_ASSERT_EQUAL(canUndo, did);
      if (did) {
        const uint32_t key = cur >= 0 ? m[cur].key : QueueModel::kNone;
        m = undoM;
        int32_t found = -1;
        for (uint32_t i = 0; i < m.size(); ++i) {
          if (m[i].key == key) found = static_cast<int32_t>(i);
        }
        cur = found >= 0 ? found : undoCur;
        canUndo = false;
      }
    } else if (op == 8 && cur >= 0) {
      std::vector<uint32_t> pos;
      for (uint32_t i = rnd(3) + 1; i > 0; --i) pos.push_back(rnd(static_cast<uint32_t>(m.size())));
      std::vector<bool> sel(m.size(), false);
      bool any = false;
      for (uint32_t p : pos) {
        if (static_cast<int32_t>(p) != cur) sel[p] = any = true;
      }
      if (!any) continue;
      save();
      q.moveNext(pos.data(), static_cast<uint32_t>(pos.size()));
      std::vector<E> out;
      int32_t newCur = -1;
      for (uint32_t i = 0; i < m.size(); ++i) {
        if (sel[i]) continue;
        out.push_back(m[i]);
        if (static_cast<int32_t>(i) == cur) {
          newCur = static_cast<int32_t>(out.size()) - 1;
          for (uint32_t j = 0; j < m.size(); ++j) {
            if (sel[j]) out.push_back(m[j]);
          }
        }
      }
      m = out;
      cur = newCur;
    }
    sync();
  }
}

// ---- shuffle (docs/QUEUE-MODES.md section 2) ----

namespace {

std::vector<uint32_t> keys(const QueueModel& q) {
  std::vector<uint32_t> k;
  for (uint32_t i = 0; i < q.size(); ++i) k.push_back(q.keyAt(i));
  return k;
}

// The tracks in the queue's own order (by rank; as setShuffled(false)
// would lay them out).
std::vector<uint32_t> ownOrder(const QueueModel& q) {
  std::vector<std::pair<uint64_t, uint32_t>> r;
  for (uint32_t i = 0; i < q.size(); ++i) {
    r.push_back({(static_cast<uint64_t>(q.rankAt(i)) << 32) | q.keyAt(i), q.trackAt(i)});
  }
  std::sort(r.begin(), r.end());
  std::vector<uint32_t> t;
  for (const auto& e : r) t.push_back(e.second);
  return t;
}

bool samePermutation(std::vector<uint32_t> a, std::vector<uint32_t> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

// A hook that counts its draws (the firmware's esp_random()).
uint32_t hookState = 1;
uint32_t hookDraws = 0;
uint32_t countingRandom() {
  ++hookDraws;
  hookState = hookState * 1664525u + 1013904223u;
  return hookState;
}

}  // namespace

void test_shuffle_on_keeps_what_played_and_what_plays() {
  QueueModel q;
  fill(q, 20, 5);  // 10 .. 29, at 15
  const uint32_t add[] = {7};
  q.append(add, 1);  // (an undo to drop)
  const std::vector<uint32_t> before = keys(q), was = tracks(q);
  const uint32_t cv = q.contentVersion(), pv = q.positionVersion();
  TEST_ASSERT_TRUE(q.setShuffled(true));
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_FALSE(q.setShuffled(true));  // already
  TEST_ASSERT_EQUAL_INT(5, q.current());
  TEST_ASSERT_EQUAL_UINT32(15, q.currentTrack());
  const std::vector<uint32_t> after = keys(q);
  for (uint32_t i = 0; i <= 5; ++i) {
    TEST_ASSERT_EQUAL_UINT32(before[i], after[i]);  // what played, and what plays, stay
    TEST_ASSERT_EQUAL_UINT32(i, q.rankAt(i));
  }
  const std::vector<uint32_t> upBefore(before.begin() + 6, before.end()), upAfter(after.begin() + 6, after.end());
  TEST_ASSERT_TRUE(samePermutation(upBefore, upAfter));
  TEST_ASSERT_FALSE(upBefore == upAfter);  // really shuffled
  // Each entry's rank is where it was.
  for (uint32_t i = 6; i < q.size(); ++i) {
    const auto it = std::find(before.begin(), before.end(), q.keyAt(i));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(it - before.begin()), q.rankAt(i));
  }
  TEST_ASSERT_TRUE(ownOrder(q) == was);
  TEST_ASSERT_TRUE(q.contentVersion() != cv);
  TEST_ASSERT_TRUE(q.positionVersion() != pv);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
}

void test_shuffle_off_brings_the_own_order_back() {
  QueueModel q;
  fill(q, 30, 3);
  const std::vector<uint32_t> was = tracks(q), wasKeys = keys(q);
  // On, then straight off: the identity.
  q.setShuffled(true);
  TEST_ASSERT_TRUE(q.setShuffled(false));
  TEST_ASSERT_FALSE(q.shuffled());
  TEST_ASSERT_FALSE(q.setShuffled(false));  // already
  TEST_ASSERT_TRUE(keys(q) == wasKeys);
  TEST_ASSERT_EQUAL_INT(3, q.current());
  TEST_ASSERT_EQUAL_UINT32(5, q.rankAt(5));  // not shuffled: the position
  // On, two steps, off: the current entry at its own place, keys kept.
  q.setShuffled(true);
  q.step(+1, false);
  q.step(+1, false);
  const uint32_t key = q.currentKey(), track = q.currentTrack();
  q.setShuffled(false);
  TEST_ASSERT_TRUE(tracks(q) == was);
  TEST_ASSERT_TRUE(keys(q) == wasKeys);
  TEST_ASSERT_EQUAL_UINT32(key, q.currentKey());
  TEST_ASSERT_EQUAL_UINT32(track, q.currentTrack());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(track - 10), q.current());
}

void test_shuffle_with_nothing_up_next() {
  // Empty, 0 up next, 1 up next: the mode flips and the content version
  // bumps (the saver writes the mode), nothing moves.
  for (uint32_t n : {0u, 1u, 3u, 4u}) {
    QueueModel q;
    if (n) fill(q, n, n == 4 ? 2 : static_cast<int32_t>(n) - 1);
    const std::vector<uint32_t> was = keys(q);
    const int32_t cur = q.current();
    uint32_t v = q.contentVersion();
    TEST_ASSERT_TRUE(q.setShuffled(true));
    TEST_ASSERT_TRUE(q.contentVersion() != v);
    TEST_ASSERT_TRUE(keys(q) == was);
    TEST_ASSERT_EQUAL_INT(cur, q.current());
    v = q.contentVersion();
    TEST_ASSERT_TRUE(q.setShuffled(false));
    TEST_ASSERT_TRUE(q.contentVersion() != v);
    TEST_ASSERT_TRUE(keys(q) == was);
  }
}

void test_shuffled_adds_keep_their_place() {
  QueueModel q;
  fill(q, 10, 2);  // 10 11 [12] 13 .. 19
  q.setShuffled(true);
  const uint32_t pn[] = {100, 101};
  TEST_ASSERT_TRUE(q.insertNext(pn, 2));
  TEST_ASSERT_EQUAL_UINT32(100, q.trackAt(3));  // right after the current entry, in the given order
  TEST_ASSERT_EQUAL_UINT32(101, q.trackAt(4));
  const uint32_t aq[] = {200, 201, 202};
  TEST_ASSERT_TRUE(q.append(aq, 3));
  TEST_ASSERT_EQUAL_UINT32(200, q.trackAt(12));  // at the end, in the given order
  TEST_ASSERT_EQUAL_UINT32(202, q.trackAt(14));
  TEST_ASSERT_TRUE(q.setShuffled(false));
  expectTracks(q, {10, 11, 12, 100, 101, 13, 14, 15, 16, 17, 18, 19, 200, 201, 202});
  TEST_ASSERT_EQUAL_INT(2, q.current());
  // Play next after a few steps: right after what plays, in its own place.
  q.setShuffled(true);
  q.step(+1, false);
  q.step(+1, false);
  const uint32_t playing = q.currentTrack();
  const uint32_t one[] = {300};
  TEST_ASSERT_TRUE(q.insertNext(one, 1));
  q.setShuffled(false);
  const std::vector<uint32_t> t = tracks(q);
  const auto at = std::find(t.begin(), t.end(), playing);
  TEST_ASSERT_TRUE(at + 1 < t.end());
  TEST_ASSERT_EQUAL_UINT32(300, *(at + 1));
  TEST_ASSERT_EQUAL_UINT32(playing, q.currentTrack());
}

void test_shuffled_move_remove_and_clear_up_next() {
  {
    // moveNext: right after the current entry, in play order, then too.
    QueueModel q;
    fill(q, 10, 0);
    q.setShuffled(true);
    const uint32_t a = q.trackAt(4), b = q.trackAt(7);
    const uint32_t sel[] = {7, 4};
    TEST_ASSERT_TRUE(q.moveNext(sel, 2));
    TEST_ASSERT_EQUAL_UINT32(a, q.trackAt(1));
    TEST_ASSERT_EQUAL_UINT32(b, q.trackAt(2));
    q.setShuffled(false);
    std::vector<uint32_t> want = {10, a, b};
    for (uint32_t t = 11; t < 20; ++t) {
      if (t != a && t != b) want.push_back(t);
    }
    expectTracks(q, want);
    TEST_ASSERT_EQUAL_INT(0, q.current());
  }
  {
    // remove: the others keep their places; a removed current entry gives
    // way to the next in play order; off: what is left, in its own order.
    QueueModel q;
    fill(q, 10, 3);
    q.setShuffled(true);
    const uint32_t gone = q.trackAt(6), next = q.trackAt(4);
    const uint32_t rm[] = {3, 6};
    const QueueModel::Removed r = q.remove(rm, 2);
    TEST_ASSERT_TRUE(r.current);
    TEST_ASSERT_EQUAL_UINT32(next, q.currentTrack());
    q.setShuffled(false);
    std::vector<uint32_t> want;
    for (uint32_t t = 10; t < 20; ++t) {
      if (t != 13 && t != gone) want.push_back(t);
    }
    expectTracks(q, want);
    TEST_ASSERT_EQUAL_UINT32(next, q.currentTrack());
  }
  {
    // Clear up next: the play order's up next goes.
    QueueModel q;
    fill(q, 10, 4);
    q.setShuffled(true);
    q.step(+1, false);
    const uint32_t playing = q.currentTrack();
    TEST_ASSERT_TRUE(q.clearUpNext());
    TEST_ASSERT_EQUAL_UINT32(6, q.size());
    q.setShuffled(false);
    std::vector<uint32_t> want = {10, 11, 12, 13, 14, playing};
    std::sort(want.begin(), want.end());
    expectTracks(q, want);
    TEST_ASSERT_EQUAL_UINT32(playing, q.currentTrack());
  }
  {
    // Clear: empty, and shuffle stays on.
    QueueModel q;
    fill(q, 4, 0);
    q.setShuffled(true);
    TEST_ASSERT_TRUE(q.clear());
    TEST_ASSERT_TRUE(q.shuffled());
  }
}

void test_shuffled_play_puts_the_chosen_track_first() {
  QueueModel q;
  TEST_ASSERT_TRUE(q.setShuffled(true));  // (an empty queue: only the mode)
  const uint32_t album[] = {50, 51, 52, 53, 54, 55, 56, 57, 58, 59};
  TEST_ASSERT_TRUE(q.replace(album, 10, 4));
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_EQUAL_UINT32(54, q.currentTrack());
  TEST_ASSERT_TRUE(samePermutation(tracks(q), {std::begin(album), std::end(album)}));
  TEST_ASSERT_TRUE(ownOrder(q) == std::vector<uint32_t>(std::begin(album), std::end(album)));
  q.setShuffled(false);
  expectTracks(q, {std::begin(album), std::end(album)});
  TEST_ASSERT_EQUAL_INT(4, q.current());
  // kAnyStart: shuffled, a random first; off, the given order around it.
  q.setShuffled(true);
  TEST_ASSERT_TRUE(q.replace(album, 10, QueueModel::kAnyStart));
  TEST_ASSERT_EQUAL_INT(0, q.current());
  const uint32_t first = q.currentTrack();
  q.setShuffled(false);
  expectTracks(q, {std::begin(album), std::end(album)});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(first - 50), q.current());
  // Not shuffled: the first (never the clamp's last).
  TEST_ASSERT_TRUE(q.replace(album, 10, QueueModel::kAnyStart));
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_TRUE(q.replace(album, 10, 99));
  TEST_ASSERT_EQUAL_INT(9, q.current());
  // A random first is random: over a few Plays more than one track starts.
  q.setShuffled(true);
  std::vector<bool> seen(10, false);
  int distinct = 0;
  for (int i = 0; i < 40; ++i) {
    q.replace(album, 10, QueueModel::kAnyStart);
    const uint32_t t = q.currentTrack() - 50;
    if (!seen[t]) ++distinct;
    seen[t] = true;
  }
  TEST_ASSERT_TRUE(distinct >= 5);
}

void test_an_add_to_an_empty_shuffled_queue() {
  QueueModel q;
  q.setShuffled(true);
  const uint32_t album[] = {50, 51, 52, 53, 54, 55, 56, 57};
  TEST_ASSERT_TRUE(q.append(album, 8));
  // As a Play from its first: the first current, the rest shuffled.
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_EQUAL_UINT32(50, q.currentTrack());
  TEST_ASSERT_TRUE(samePermutation(tracks(q), {std::begin(album), std::end(album)}));
  TEST_ASSERT_FALSE(tracks(q) == std::vector<uint32_t>(std::begin(album), std::end(album)));
  q.setShuffled(false);
  expectTracks(q, {std::begin(album), std::end(album)});
  TEST_ASSERT_EQUAL_INT(0, q.current());
  // Play next into an empty shuffled queue: the same.
  QueueModel p;
  p.setShuffled(true);
  TEST_ASSERT_TRUE(p.insertNext(album, 8));
  TEST_ASSERT_EQUAL_UINT32(50, p.currentTrack());
  TEST_ASSERT_TRUE(ownOrder(p) == std::vector<uint32_t>(std::begin(album), std::end(album)));
}

void test_undo_while_shuffled_restores_the_ranks() {
  QueueModel q;
  fill(q, 10, 2);
  q.setShuffled(true);
  const std::vector<uint32_t> was = keys(q), own = ownOrder(q);
  std::vector<uint32_t> ranks;
  for (uint32_t i = 0; i < q.size(); ++i) ranks.push_back(q.rankAt(i));
  const uint32_t pn[] = {100, 101};
  q.insertNext(pn, 2);
  TEST_ASSERT_TRUE(q.undo());
  TEST_ASSERT_TRUE(keys(q) == was);
  for (uint32_t i = 0; i < q.size(); ++i) TEST_ASSERT_EQUAL_UINT32(ranks[i], q.rankAt(i));
  TEST_ASSERT_TRUE(ownOrder(q) == own);
  q.setShuffled(false);
  expectTracks(q, {10, 11, 12, 13, 14, 15, 16, 17, 18, 19});
}

void test_a_toggle_drops_the_undo() {
  QueueModel q;
  fill(q, 6, 1);
  const uint32_t add[] = {7};
  q.append(add, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Append), static_cast<int>(q.undoable()));
  q.setShuffled(true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  TEST_ASSERT_FALSE(q.undo());
  q.append(add, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Append), static_cast<int>(q.undoable()));
  q.setShuffled(false);
  TEST_ASSERT_FALSE(q.undo());
}

void test_shuffle_is_repeatable_and_uniform() {
  // The same hook sequence, the same order (no hook: the fixed one).
  {
    QueueModel a, b;
    fill(a, 50, 0);
    fill(b, 50, 0);
    a.setShuffled(true);
    b.setShuffled(true);
    TEST_ASSERT_TRUE(tracks(a) == tracks(b));
    hookState = 1;
    hookDraws = 0;
    QueueModel c(nullptr, nullptr, countingRandom);
    fill(c, 50, 0);
    c.setShuffled(true);
    TEST_ASSERT_EQUAL_UINT32(1, hookDraws);  // one draw seeds a shuffle
    const std::vector<uint32_t> first = tracks(c);
    c.setShuffled(false);
    hookState = 1;
    c.setShuffled(true);
    TEST_ASSERT_TRUE(tracks(c) == first);
    c.replace(first.data(), 50, QueueModel::kAnyStart);
    TEST_ASSERT_EQUAL_UINT32(4, hookDraws);  // a random first is one more
  }
  // 5 up next, 20,000 shuffles: each entry in each slot within 3 % of 1/5;
  // the current entry never moves.
  QueueModel q;
  fill(q, 6, 0);
  int count[5][5] = {};
  constexpr int kRuns = 20000;
  for (int r = 0; r < kRuns; ++r) {
    q.setShuffled(true);
    TEST_ASSERT_EQUAL_UINT32(10, q.trackAt(0));
    for (uint32_t slot = 1; slot < 6; ++slot) ++count[slot - 1][q.trackAt(slot) - 11];
    q.setShuffled(false);
  }
  for (auto& slot : count) {
    for (int c : slot) {
      TEST_ASSERT_INT_WITHIN(kRuns / 5 * 3 / 100, kRuns / 5, c);
    }
  }
}

void test_a_toggle_allocates_nothing() {
  QueueModel q(Heap::alloc, Heap::release);
  fill(q, 10000, 5000);
  const uint32_t add[] = {1};
  q.append(add, 1);  // (the snapshot's memory exists)
  const long allocs = Heap::allocs;
  Heap::failing = true;  // and none could be had
  TEST_ASSERT_TRUE(q.setShuffled(true));
  TEST_ASSERT_TRUE(q.setShuffled(false));
  Heap::failing = false;
  TEST_ASSERT_EQUAL_INT(allocs, Heap::allocs);
  TEST_ASSERT_EQUAL_UINT32(10001, q.size());
  TEST_ASSERT_EQUAL_UINT32(5010, q.currentTrack());
}

void test_assign_with_ranks() {
  QueueModel q;
  const uint32_t ids[] = {1, 2, 3, 4, 5};
  const uint32_t ranks[] = {40, 10, 30, 0, 20};
  TEST_ASSERT_TRUE(q.assign(ids, 5, 2, true, ranks));
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_UINT32(30, q.rankAt(2));
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.rankAt(5));
  q.setShuffled(false);
  expectTracks(q, {4, 2, 5, 3, 1});
  TEST_ASSERT_EQUAL_UINT32(3, q.currentTrack());  // its key followed
  // Not shuffled, ranks mean nothing: the positions.
  TEST_ASSERT_TRUE(q.assign(ids, 5, 0, false, ranks));
  TEST_ASSERT_FALSE(q.shuffled());
  TEST_ASSERT_EQUAL_UINT32(2, q.rankAt(2));
  // An empty shuffled queue keeps its mode.
  TEST_ASSERT_TRUE(q.assign(nullptr, 0, -1, true));
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_INT(-1, q.current());
}

void test_a_rank_overflow_is_refused() {
  QueueModel q;
  const uint32_t ids[] = {1, 2};
  const uint32_t ranks[] = {0, 0xFFFFFFFEu};
  q.assign(ids, 2, 0, true, ranks);
  const uint32_t v = q.contentVersion();
  const uint32_t two[] = {7, 8};
  TEST_ASSERT_FALSE(q.append(two, 2));  // past 0xFFFFFFFF: refused, as out of memory is
  TEST_ASSERT_FALSE(q.insertNext(two, 2));
  TEST_ASSERT_EQUAL_UINT32(2, q.size());
  TEST_ASSERT_EQUAL_UINT32(v, q.contentVersion());
  TEST_ASSERT_TRUE(q.append(two, 1));  // to 0xFFFFFFFF exactly
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, q.rankAt(2));
  TEST_ASSERT_FALSE(q.insertNext(two, 1));
  const uint32_t pos[] = {2};
  TEST_ASSERT_FALSE(q.moveNext(pos, 1));
  TEST_ASSERT_EQUAL_UINT32(3, q.size());
  // A Play renumbers.
  TEST_ASSERT_TRUE(q.replace(two, 2, 0));
  TEST_ASSERT_TRUE(q.append(two, 2));
}

// A long random run while shuffled: after every edit, undo and toggle, the
// ranks sort into the own order a plain model keeps (every edit's rule of
// section 2.4), and off lays the queue out in it.
void test_random_shuffled_edits_keep_the_own_order() {
  QueueModel q;
  std::vector<uint32_t> own, undoOwn;  // keys in the own order
  bool canUndo = false;
  uint32_t x = 7;
  auto rnd = [&](uint32_t n) {
    x = x * 1664525u + 1013904223u;
    return n ? (x >> 8) % n : 0;
  };
  auto ownKeys = [&]() {
    std::vector<std::pair<uint64_t, uint32_t>> r;
    for (uint32_t i = 0; i < q.size(); ++i) r.push_back({(static_cast<uint64_t>(q.rankAt(i)) << 32) | q.keyAt(i), q.keyAt(i)});
    std::sort(r.begin(), r.end());
    std::vector<uint32_t> k;
    for (const auto& e : r) k.push_back(e.second);
    return k;
  };
  auto newKeys = [&](uint32_t from) {  // keys >= from, in key order (the given order)
    std::vector<uint32_t> k;
    for (uint32_t i = 0; i < q.size(); ++i) {
      if (q.keyAt(i) >= from) k.push_back(q.keyAt(i));
    }
    std::sort(k.begin(), k.end());
    return k;
  };
  auto maxKey = [&]() {
    uint32_t m = 0;
    for (uint32_t i = 0; i < q.size(); ++i) m = std::max(m, q.keyAt(i) + 1);
    return m;
  };
  uint32_t nextKey = 0;  // at least every key given so far
  q.setShuffled(true);
  for (int it = 0; it < 3000; ++it) {
    const uint32_t op = rnd(10);
    std::vector<uint32_t> ids;
    for (uint32_t i = rnd(4) + 1; i > 0; --i) ids.push_back(rnd(50));
    const auto n = static_cast<uint32_t>(ids.size());
    nextKey = std::max(nextKey, maxKey());
    const auto save = [&] {
      undoOwn = own;
      canUndo = true;
    };
    if (op == 0) {
      save();
      q.replace(ids.data(), n, rnd(2) ? QueueModel::kAnyStart : rnd(n));
      own = newKeys(nextKey);
    } else if (op == 1 || op == 2) {
      save();
      const uint32_t cur = q.current() >= 0 ? q.currentKey() : QueueModel::kNone;
      op == 1 ? q.insertNext(ids.data(), n) : q.append(ids.data(), n);
      const std::vector<uint32_t> added = newKeys(nextKey);
      if (cur == QueueModel::kNone) {
        own = added;
      } else if (op == 1) {
        own.insert(std::find(own.begin(), own.end(), cur) + 1, added.begin(), added.end());
      } else {
        own.insert(own.end(), added.begin(), added.end());
      }
    } else if (op == 3 && !q.empty()) {
      const uint32_t pos[] = {rnd(q.size()), rnd(q.size())};
      std::vector<uint32_t> gone = {q.keyAt(pos[0]), q.keyAt(pos[1])};
      save();
      q.remove(pos, 2);
      for (uint32_t k : gone) own.erase(std::remove(own.begin(), own.end(), k), own.end());
    } else if (op == 4 && q.upNext() > 0) {
      save();
      std::vector<uint32_t> gone;
      for (uint32_t i = static_cast<uint32_t>(q.current()) + 1; i < q.size(); ++i) gone.push_back(q.keyAt(i));
      q.clearUpNext();
      for (uint32_t k : gone) own.erase(std::remove(own.begin(), own.end(), k), own.end());
    } else if (op == 5 && !q.empty()) {
      q.step(static_cast<int>(rnd(5)) - 2, true);
    } else if (op == 6) {
      const bool did = q.undo();
      TEST_ASSERT_EQUAL(canUndo, did);
      if (did) own = undoOwn;
      canUndo = false;
    } else if (op == 7 && q.current() >= 0 && q.size() > 2) {
      std::vector<uint32_t> pos = {rnd(q.size()), rnd(q.size()), rnd(q.size())};
      std::sort(pos.begin(), pos.end());
      pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
      std::vector<uint32_t> moved;
      for (uint32_t p : pos) {
        if (static_cast<int32_t>(p) != q.current()) moved.push_back(q.keyAt(p));
      }
      if (moved.empty()) continue;
      const uint32_t cur = q.currentKey();
      save();
      q.moveNext(pos.data(), static_cast<uint32_t>(pos.size()));
      for (uint32_t k : moved) own.erase(std::remove(own.begin(), own.end(), k), own.end());
      own.insert(std::find(own.begin(), own.end(), cur) + 1, moved.begin(), moved.end());
    } else if (op == 8 && !q.empty() && rnd(8) == 0) {
      save();
      q.clear();
      own.clear();
    } else if (op == 9 && rnd(4) == 0) {
      // A toggle: off lays the queue out in the own order, on keeps it.
      const uint32_t cur = q.currentKey();
      q.setShuffled(false);
      TEST_ASSERT_TRUE(keys(q) == own);
      TEST_ASSERT_EQUAL_UINT32(cur, q.currentKey());
      if (rnd(2)) q.step(+1, true);
      own = keys(q);  // on: up next shuffled, the own order what it is now
      q.setShuffled(true);
      canUndo = false;
    }
    TEST_ASSERT_TRUE(ownKeys() == own);
  }
}

void test_permute_is_a_permutation_and_repeatable() {
  // (queueview::shuffle()'s test, moved with its loop: the same seed gives
  // the same order as before.)
  std::vector<uint32_t> a(500), b;
  for (uint32_t i = 0; i < a.size(); ++i) a[i] = i;
  b = a;
  shuffle::permute(a.data(), static_cast<uint32_t>(a.size()), 1234);
  std::vector<uint32_t> sorted = a;
  std::sort(sorted.begin(), sorted.end());
  TEST_ASSERT_TRUE(sorted == b);  // every id once
  int moved = 0;
  for (uint32_t i = 0; i < a.size(); ++i) moved += a[i] != i;
  TEST_ASSERT_TRUE(moved > 450);  // really shuffled
  const uint32_t head[] = {13, 263, 140, 414, 405, 499};  // Shuffle all's order, as it was
  for (int i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_UINT32(head[i], a[i]);
  TEST_ASSERT_EQUAL_UINT32(38, a[499]);
  std::vector<uint32_t> c = b;
  shuffle::permute(c.data(), static_cast<uint32_t>(c.size()), 1234);
  TEST_ASSERT_TRUE(c == a);  // the same seed, the same order
  std::vector<uint32_t> d = b;
  shuffle::permute(d.data(), static_cast<uint32_t>(d.size()), 99);
  TEST_ASSERT_FALSE(d == a);
  uint32_t one = 7;
  shuffle::permute(&one, 1, 5);
  TEST_ASSERT_EQUAL_UINT32(7, one);
  std::vector<uint32_t> zero = b, seedOne = b;  // 0 is taken as 1
  shuffle::permute(zero.data(), 500, 0);
  shuffle::permute(seedOne.data(), 500, 1);
  TEST_ASSERT_TRUE(zero == seedOne);
}

// ---- TrackCatalog ----

void test_catalog_library_and_builtin_ids() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  for (uint32_t i = 0; i < idx.trackCount(); ++i) {
    TEST_ASSERT_TRUE(c.valid(i));
    const std::string p = pathOf(c, i);
    TEST_ASSERT_EQUAL_UINT32(i, c.find(p.c_str()));
  }
  char buf[64];
  TEST_ASSERT_EQUAL_UINT32(13, c.title(0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("One More Time", buf);
  TEST_ASSERT_EQUAL_STRING("Daft Punk", c.artist(0));
  TEST_ASSERT_EQUAL_STRING("Discovery", c.album(0));
  // Built-ins.
  const LibraryIndex::Span b = TrackCatalog::builtins();
  TEST_ASSERT_EQUAL_UINT32(9, b.count);
  TEST_ASSERT_EQUAL_STRING("tone:440", pathOf(c, b[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("tone:click120off", pathOf(c, b[8]).c_str());
  TEST_ASSERT_EQUAL_UINT32(60000, c.durationHintMs(b[3]));
  TEST_ASSERT_EQUAL_UINT32(b[4], c.find("tone:click120"));
  TEST_ASSERT_EQUAL_STRING("built-in", c.artist(b[0]));
  c.title(b[3], buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Clicks 90 BPM", buf);
  // A title too long for the buffer is cut, never mid-character.
  char small[8];
  TEST_ASSERT_EQUAL_UINT32(7, c.title(0, small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("One Mor", small);
  const uint32_t emilie = c.find("/music/Émilie Simon/Végétal/10 - Le voyage de Pénélope.mp3");
  char cut[20];  // "Le voyage de P" is 14 bytes, then é (2), n, é (2), l
  c.title(emilie, cut, 20);
  TEST_ASSERT_EQUAL_STRING("Le voyage de Péné", cut);
  c.title(emilie, cut, 19);  // 18 bytes would end mid-"é": 17
  TEST_ASSERT_EQUAL_STRING("Le voyage de Pén", cut);
  // Unknown ids: "" and kNone.
  TEST_ASSERT_FALSE(c.valid(idx.trackCount()));
  // The power test's silence: known, but not listed with the others.
  const uint32_t silence = c.find(TrackCatalog::kSilencePath);
  TEST_ASSERT_EQUAL_UINT32(TrackCatalog::kBuiltin + 9, silence);
  TEST_ASSERT_TRUE(c.valid(silence));
  TEST_ASSERT_EQUAL_UINT32(3600000, c.durationHintMs(silence));
  for (uint32_t i = 0; i < b.count; ++i) TEST_ASSERT_NOT_EQUAL(silence, b[i]);
  // The rate converter's test tracks: known, after the silence, never listed.
  const LibraryIndex::Span rt = TrackCatalog::rateTests();
  TEST_ASSERT_EQUAL_UINT32(9, rt.count);
  TEST_ASSERT_EQUAL_STRING("tone:1000@48000", pathOf(c, rt[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("tone:silence@22050", pathOf(c, rt[8]).c_str());
  TEST_ASSERT_EQUAL_UINT32(rt[6], c.find("tone:silence@48000"));
  TEST_ASSERT_EQUAL_UINT32(3600000, c.durationHintMs(rt[6]));
  for (uint32_t i = 0; i < rt.count; ++i) {
    TEST_ASSERT_TRUE(c.valid(rt[i]));
    TEST_ASSERT_TRUE(TrackCatalog::isBuiltin(rt[i]));
    TEST_ASSERT_NOT_EQUAL(silence, rt[i]);
    for (uint32_t j = 0; j < b.count; ++j) TEST_ASSERT_NOT_EQUAL(b[j], rt[i]);
    TEST_ASSERT_TRUE(pathOf(c, rt[i]).find('@') != std::string::npos);
  }
  TEST_ASSERT_EQUAL_UINT32(10 + rt.count, TrackCatalog::builtinCount());
  TEST_ASSERT_FALSE(c.valid(TrackCatalog::kBuiltin + 10 + rt.count));
  TEST_ASSERT_EQUAL_STRING("", pathOf(c, 1234).c_str());
  TEST_ASSERT_EQUAL_STRING("", c.artist(1234));
  TEST_ASSERT_EQUAL_UINT32(TrackCatalog::kNone, c.find("tone:nope"));
  TEST_ASSERT_EQUAL_UINT32(TrackCatalog::kNone, c.find("/music/nope.mp3"));
  // Built-ins need no library at all.
  TrackCatalog bare;
  TEST_ASSERT_EQUAL_STRING("tone:1000", pathOf(bare, b[1]).c_str());
  TEST_ASSERT_FALSE(bare.valid(0));
}

// ---- QueueText ----

void test_text_round_trip() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  const uint32_t ids[] = {3, 0, TrackCatalog::kBuiltin + 4, 5, 3};
  q.assign(ids, 5, 2);
  MemorySink out;
  TEST_ASSERT_TRUE(queuetext::write(q, c, 7, out));
  const std::string text(reinterpret_cast<const char*>(out.data()), out.size());
  TEST_ASSERT_EQUAL_STRING(
      "mstream-queue 1 5 2 7\n"
      "/music/Kavinsky/OutRun/08 - Nightcall.mp3\n"
      "/music/Daft Punk/Discovery/01 - One More Time.mp3\n"
      "tone:click120\n"
      "/music/Root Track.flac\n"
      "/music/Kavinsky/OutRun/08 - Nightcall.mp3\n",
      text.c_str());
  QueueModel back;
  MemorySource in(out.data(), out.size(), 7);  // reads split mid-line
  const queuetext::Restored r = queuetext::read(in, c, back);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(5, r.lines);
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
  TEST_ASSERT_TRUE(r.currentKept);
  TEST_ASSERT_EQUAL_UINT32(7, r.header.generation);
  expectTracks(back, {3, 0, TrackCatalog::kBuiltin + 4, 5, 3});
  TEST_ASSERT_EQUAL_INT(2, back.current());
}

// The library is rebuilt: other files, other ids. The queue follows its
// paths; what's gone is dropped, the current entry moves to the next one
// that stayed.
void test_text_remaps_after_a_rebuild() {
  LibraryIndex before;
  build(before, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&before);
  QueueModel q;
  const uint32_t ids[] = {0, 1, 2, 3};  // Discovery 01-03, Nightcall; current: 02
  q.assign(ids, 4, 1);
  MemorySink saved;
  TEST_ASSERT_TRUE(queuetext::write(q, c, 1, saved));

  LibraryIndex after;  // 02 is gone, a new file sorts first, the order of adding differs
  build(after, {"/music/Kavinsky/OutRun/08 - Nightcall.mp3", "/music/ABBA/Gold/01 - Dancing Queen.mp3",
                "/music/Daft Punk/Discovery/03 - Digital Love.mp3",
                "/music/Daft Punk/Discovery/01 - One More Time.mp3"});
  TrackCatalog c2(&after);
  MemorySource in(saved.data(), saved.size());
  const queuetext::Restored r = queuetext::read(in, c2, q);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(4, r.lines);
  TEST_ASSERT_EQUAL_UINT32(1, r.dropped);
  TEST_ASSERT_FALSE(r.currentKept);
  TEST_ASSERT_EQUAL_UINT32(3, q.size());
  TEST_ASSERT_EQUAL_STRING("/music/Daft Punk/Discovery/01 - One More Time.mp3", pathOf(c2, q.trackAt(0)).c_str());
  TEST_ASSERT_EQUAL_STRING("/music/Daft Punk/Discovery/03 - Digital Love.mp3", pathOf(c2, q.currentTrack()).c_str());
  TEST_ASSERT_EQUAL_STRING("/music/Kavinsky/OutRun/08 - Nightcall.mp3", pathOf(c2, q.trackAt(2)).c_str());
}

int32_t positionFive(const queuetext::Header& h, void* ctx) {
  *static_cast<uint32_t*>(ctx) = h.generation;
  return 3;
}

void test_text_current_override_and_bad_files() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  const char* text = "mstream-queue 1 4 0 42\r\n/music/Root Track.flac\r\n\r\ntone:440\r\n/music/nope.mp3";
  QueueModel q;
  uint32_t gen = 0;
  MemorySource in(text, std::strlen(text));
  const queuetext::Restored r = queuetext::read(in, c, q, positionFive, &gen);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(42, gen);
  TEST_ASSERT_EQUAL_UINT32(2, r.dropped);  // the empty line and nope.mp3
  expectTracks(q, {5, TrackCatalog::kBuiltin + 0});
  // Line 3 (nope.mp3) was current: nothing after it stayed, so the last one.
  TEST_ASSERT_EQUAL_INT(1, q.current());
  TEST_ASSERT_FALSE(r.currentKept);

  // Not a queue file, or cut short: the queue stays as it is.
  const char* bad[] = {"", "hello\n/music/Root Track.flac\n", "mstream-queue 1 3 0 1\n/music/Root Track.flac\n",
                       "mstream-queue 1 x 0 1\n", "mstream-queue 2 1 0 1\n/music/Root Track.flac\n"};
  for (const char* b : bad) {
    MemorySource src(b, std::strlen(b));
    TEST_ASSERT_FALSE(queuetext::read(src, c, q).ok);
    expectTracks(q, {5, TrackCatalog::kBuiltin + 0});
  }
  // An empty queue is a queue.
  const char* empty = "mstream-queue 1 0 -1 3\n";
  MemorySource src(empty, std::strlen(empty));
  TEST_ASSERT_TRUE(queuetext::read(src, c, q).ok);
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(-1, q.current());
}

void test_text_writer_in_steps() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < 50; ++i) ids.push_back(i % 6);
  q.assign(ids.data(), 50, 10);
  MemorySink whole, steps;
  queuetext::write(q, c, 9, whole);
  queuetext::Writer w;
  w.begin(q, 9);
  int calls = 0;
  queuetext::Writer::Step s;
  while ((s = w.step(q, c, steps, 8)) == queuetext::Writer::Step::More) ++calls;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queuetext::Writer::Step::Done), static_cast<int>(s));
  TEST_ASSERT_EQUAL_INT(6, calls);  // 51 lines, 8 a step
  TEST_ASSERT_EQUAL_size_t(whole.size(), steps.size());
  TEST_ASSERT_EQUAL_MEMORY(whole.data(), steps.data(), whole.size());
  // The queue changes mid-write: the writer says so.
  MemorySink partial;
  w.begin(q, 10);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queuetext::Writer::Step::More), static_cast<int>(w.step(q, c, partial, 8)));
  const uint32_t one[] = {1};
  q.append(one, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queuetext::Writer::Step::Changed),
                        static_cast<int>(w.step(q, c, partial, 8)));
  // A move of the current position alone doesn't count.
  w.begin(q, 11);
  w.step(q, c, partial, 8);
  q.step(1, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queuetext::Writer::Step::More), static_cast<int>(w.step(q, c, partial, 8)));
}

// ---- queue.txt version 2: a shuffled queue (docs/QUEUE-MODES.md 2.9) ----

std::string textOf(const QueueModel& q, const TrackCatalog& c, uint32_t generation) {
  MemorySink out;
  TEST_ASSERT_TRUE(queuetext::write(q, c, generation, out));
  return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

void test_v1_unchanged_while_not_shuffled() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  const uint32_t ids[] = {3, 0, TrackCatalog::kBuiltin + 4, 5, 3};
  q.assign(ids, 5, 2);
  const std::string before = textOf(q, c, 7);
  TEST_ASSERT_EQUAL_STRING(
      "mstream-queue 1 5 2 7\n"
      "/music/Kavinsky/OutRun/08 - Nightcall.mp3\n"
      "/music/Daft Punk/Discovery/01 - One More Time.mp3\n"
      "tone:click120\n"
      "/music/Root Track.flac\n"
      "/music/Kavinsky/OutRun/08 - Nightcall.mp3\n",
      before.c_str());
  // On and off again: byte for byte the same file.
  q.setShuffled(true);
  q.setShuffled(false);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), textOf(q, c, 7).c_str());
  QueueModel back;
  MemorySource in(before.data(), before.size());
  const queuetext::Restored r = queuetext::read(in, c, back);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.header.shuffled);
  TEST_ASSERT_FALSE(back.shuffled());
}

void test_v2_round_trip() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  const uint32_t ids[] = {3, 0, TrackCatalog::kBuiltin + 4, 5, 3};
  const uint32_t ranks[] = {4, 0, 2, 1, 3};
  q.assign(ids, 5, 2, true, ranks);
  const std::string text = textOf(q, c, 7);
  TEST_ASSERT_EQUAL_STRING(
      "mstream-queue 2 5 2 7\n"
      "4 /music/Kavinsky/OutRun/08 - Nightcall.mp3\n"
      "0 /music/Daft Punk/Discovery/01 - One More Time.mp3\n"
      "2 tone:click120\n"
      "1 /music/Root Track.flac\n"
      "3 /music/Kavinsky/OutRun/08 - Nightcall.mp3\n",
      text.c_str());
  QueueModel back;
  MemorySource in(text.data(), text.size(), 7);  // reads split mid-line
  const queuetext::Restored r = queuetext::read(in, c, back);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.header.shuffled);
  TEST_ASSERT_TRUE(back.shuffled());
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
  expectTracks(back, {3, 0, TrackCatalog::kBuiltin + 4, 5, 3});
  for (uint32_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_UINT32(ranks[i], back.rankAt(i));
  TEST_ASSERT_EQUAL_INT(2, back.current());
  // Off after the read: the own order, the current entry with it.
  back.setShuffled(false);
  expectTracks(back, {0, 5, TrackCatalog::kBuiltin + 4, 3, 3});
  TEST_ASSERT_EQUAL_INT(2, back.current());
  // A shuffled queue's own write reads back the same.
  QueueModel s;
  std::vector<uint32_t> many;
  for (uint32_t i = 0; i < 30; ++i) many.push_back(i % 6);
  s.assign(many.data(), 30, 4);
  s.setShuffled(true);
  const std::string st = textOf(s, c, 1);
  QueueModel s2;
  MemorySource in2(st.data(), st.size());
  TEST_ASSERT_TRUE(queuetext::read(in2, c, s2).ok);
  TEST_ASSERT_TRUE(tracks(s2) == tracks(s));
  s.setShuffled(false);
  s2.setShuffled(false);
  TEST_ASSERT_TRUE(tracks(s2) == many);
  TEST_ASSERT_EQUAL_INT(4, s2.current());
}

void test_v2_dropped_tracks_leave_rank_gaps() {
  LibraryIndex before;
  build(before, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&before);
  QueueModel q;
  const uint32_t ids[] = {3, 0, TrackCatalog::kBuiltin + 4, 5, 3};
  const uint32_t ranks[] = {4, 0, 2, 1, 3};
  q.assign(ids, 5, 3, true, ranks);  // current: Root Track, which goes
  const std::string text = textOf(q, c, 2);
  LibraryIndex after;  // no Root Track.flac, other ids
  build(after, {"/music/Kavinsky/OutRun/08 - Nightcall.mp3", "/music/Daft Punk/Discovery/01 - One More Time.mp3"});
  TrackCatalog c2(&after);
  MemorySource in(text.data(), text.size());
  const queuetext::Restored r = queuetext::read(in, c2, q);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(1, r.dropped);
  TEST_ASSERT_FALSE(r.currentKept);
  TEST_ASSERT_EQUAL_UINT32(4, q.size());
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_UINT32(4, q.rankAt(0));
  TEST_ASSERT_EQUAL_UINT32(3, q.rankAt(3));  // rank 1 is a gap now
  // The next survivor is current: the second Nightcall (rank 3).
  TEST_ASSERT_EQUAL_INT(3, q.current());
  q.setShuffled(false);
  TEST_ASSERT_EQUAL_STRING("/music/Daft Punk/Discovery/01 - One More Time.mp3", pathOf(c2, q.trackAt(0)).c_str());
  TEST_ASSERT_EQUAL_STRING("tone:click120", pathOf(c2, q.trackAt(1)).c_str());
  TEST_ASSERT_EQUAL_STRING("/music/Kavinsky/OutRun/08 - Nightcall.mp3", pathOf(c2, q.trackAt(2)).c_str());
  TEST_ASSERT_EQUAL_INT(2, q.current());
}

void test_v2_bad_lines_leave_the_queue_alone() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  const uint32_t ids[] = {5, 0};
  q.assign(ids, 2, 1);
  const char* bad[] = {
      "mstream-queue 2 1 0 1\n/music/Root Track.flac\n",            // no rank
      "mstream-queue 2 1 0 1\n7/music/Root Track.flac\n",           // no space
      "mstream-queue 2 1 0 1\n4294967296 /music/Root Track.flac\n", // past 2^32 - 1
      "mstream-queue 2 1 0 1\n99999999999999999999 tone:440\n",     // far past it
      "mstream-queue 2 1 0 1\n 7 /music/Root Track.flac\n",         // a space first
      "mstream-queue 2 2 0 1\n3 tone:440\n\n",                      // an empty line: no rank either
      "mstream-queue 3 1 0 1\n7 /music/Root Track.flac\n",          // a version this firmware doesn't know
      "mstream-queue 21 1 0 1\n7 /music/Root Track.flac\n",
  };
  for (const char* b : bad) {
    MemorySource src(b, std::strlen(b));
    TEST_ASSERT_FALSE(queuetext::read(src, c, q).ok);
    expectTracks(q, {5, 0});
    TEST_ASSERT_FALSE(q.shuffled());
  }
  // The top rank is a rank; a known rank before an unknown path is a
  // dropped line, as version 1's.
  const char* good = "mstream-queue 2 3 0 1\n4294967295 /music/Root Track.flac\n12 \n0 /music/gone.mp3\n";
  MemorySource src(good, std::strlen(good));
  const queuetext::Restored r = queuetext::read(src, c, q);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(2, r.dropped);
  expectTracks(q, {5});
  TEST_ASSERT_EQUAL_UINT32(4294967295u, q.rankAt(0));
}

void test_v2_long_path_and_ten_digit_rank() {
  // The longest path the catalog passes on (255 bytes) after the longest
  // rank: its line still fits the reader (as version 1's did).
  std::string path = "/music/";
  path += std::string(120, 'a') + "/";
  path += std::string(255 - path.size() - 4, 'b') + ".mp3";
  TEST_ASSERT_EQUAL_size_t(255, path.size());
  LibraryIndex idx;
  build(idx, {path.c_str(), "/music/Root Track.flac"});
  TrackCatalog c(&idx);
  const uint32_t id = c.index()->trackCount() == 2 && pathOf(c, 0) == path ? 0 : 1;
  TEST_ASSERT_EQUAL_STRING(path.c_str(), pathOf(c, id).c_str());
  QueueModel q;
  const uint32_t ids[] = {id, 1 - id};
  const uint32_t ranks[] = {4294967295u, 0};
  q.assign(ids, 2, 0, true, ranks);
  const std::string text = textOf(q, c, 3);
  TEST_ASSERT_TRUE(text.find("4294967295 " + path + "\n") != std::string::npos);
  QueueModel back;
  MemorySource in(text.data(), text.size(), 5);
  const queuetext::Restored r = queuetext::read(in, c, back);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
  expectTracks(back, {id, 1 - id});
  TEST_ASSERT_EQUAL_UINT32(4294967295u, back.rankAt(0));
}

void test_v2_empty_shuffled_queue() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  q.setShuffled(true);
  const std::string text = textOf(q, c, 7);
  TEST_ASSERT_EQUAL_STRING("mstream-queue 2 0 -1 7\n", text.c_str());
  QueueModel back;
  const uint32_t one[] = {1};
  back.assign(one, 1, 0);
  MemorySource in(text.data(), text.size());
  TEST_ASSERT_TRUE(queuetext::read(in, c, back).ok);
  TEST_ASSERT_TRUE(back.empty());
  TEST_ASSERT_TRUE(back.shuffled());
}

// ---- QueueSaver (app/QueueStore's timing, the card replaced by memory) ----

// The card and NVS, in memory: the temporary file, the queue file, the
// saved position; each can be told to fail.
struct MemStore : QueueSaver::Store {
  MemorySink* temp = nullptr;
  std::string file;  // the queue file
  uint32_t posGeneration = 0;
  int32_t pos = -99;
  int opens = 0, commits = 0, discards = 0, positions = 0;
  bool failOpen = false, failCommit = false;

  ~MemStore() { delete temp; }
  ByteSink* openTemp() override {
    ++opens;
    delete temp;
    temp = nullptr;
    if (failOpen) return nullptr;
    temp = new MemorySink();
    return temp;
  }
  bool commitTemp() override {
    ++commits;
    const bool ok = temp && !failCommit;
    if (ok) file.assign(reinterpret_cast<const char*>(temp->data()), temp->size());
    delete temp;
    temp = nullptr;
    return ok;
  }
  void discardTemp() override {
    ++discards;
    delete temp;
    temp = nullptr;
  }
  void savePosition(uint32_t generation, int32_t current) override {
    ++positions;
    posGeneration = generation;
    pos = current;
  }
  QueueResume resume;
  int resumes = 0;
  void saveResume(const QueueResume& r) override {
    ++resumes;
    resume = r;
  }
};

std::string wholeText(const QueueModel& q, const TrackCatalog& c, uint32_t generation) {
  MemorySink out;
  TEST_ASSERT_TRUE(queuetext::write(q, c, generation, out));
  return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

// 200 entries: a write takes 7 passes of 32 lines.
void fillLong(QueueModel& q) {
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < 200; ++i) ids.push_back(i % 6);
  TEST_ASSERT_TRUE(q.assign(ids.data(), 200, 5));
}

// The saver's timing, as QueueStore had it: 2 s after the last edit, a few
// lines a pass, then the position with the new generation.
void test_saver_writes_after_the_edits_settle() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  saver.setGeneration(3);
  saver.loaded(3, false, 0);
  fillLong(q);
  saver.loop(100);
  TEST_ASSERT_TRUE(saver.busy());  // an edit waiting its 2 s
  saver.loop(2000);
  TEST_ASSERT_EQUAL_INT(0, st.opens);
  int passes = 0;
  for (uint32_t t = 2100; saver.writing() || passes == 0; t += 20, ++passes) {
    saver.loop(t);
    if (passes == 0) TEST_ASSERT_TRUE(saver.writing());
  }
  TEST_ASSERT_EQUAL_INT(7, passes);  // 201 lines, 32 a pass
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 4).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_UINT32(4, st.posGeneration);
  TEST_ASSERT_EQUAL_INT(5, st.pos);
  TEST_ASSERT_FALSE(saver.busy());
  // A move alone: the position a second later, no file.
  q.step(1, true);
  saver.loop(5000);
  TEST_ASSERT_TRUE(saver.busy());
  saver.loop(6000);
  TEST_ASSERT_EQUAL_INT(6, st.pos);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_FALSE(saver.busy());
}

// A power-off in the middle of a piece-wise write: flushNow() finishes it,
// and the file is the whole queue (not the lines written so far), with the
// position for its generation.
void test_flush_now_in_the_middle_of_a_write() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  st.file = "old";
  QueueSaver saver(st, q, c);
  saver.loaded(8, false, 0);
  fillLong(q);
  saver.loop(10);
  saver.loop(2100);  // the write begins: 32 lines
  saver.loop(2120);  // 64
  TEST_ASSERT_TRUE(saver.writing());
  TEST_ASSERT_EQUAL_STRING("old", st.file.c_str());
  // (The file's own current line is the one at the write's start: the
  // position saved with its generation is what counts.)
  const std::string whole = wholeText(q, c, 9);
  q.step(2, true);  // and a move not saved yet
  TEST_ASSERT_TRUE(saver.flushNow(2130));
  TEST_ASSERT_FALSE(saver.writing());
  TEST_ASSERT_FALSE(saver.busy());
  TEST_ASSERT_EQUAL_INT(1, st.opens);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_INT(0, st.discards);
  TEST_ASSERT_EQUAL_STRING(whole.c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_UINT32(9, st.posGeneration);
  TEST_ASSERT_EQUAL_INT(7, st.pos);
  // Nothing left: the loop writes nothing more.
  for (uint32_t t = 2200; t < 20000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
}

// The queue changed after the write began (an edit, then the power-off
// before the next pass): the partial write is dropped and the queue as it
// is now written whole.
void test_flush_now_after_an_edit_during_the_write() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  saver.loaded(1, false, 0);
  fillLong(q);
  saver.loop(10);
  saver.loop(2100);
  TEST_ASSERT_TRUE(saver.writing());
  const uint32_t more[] = {1, 2, 3};
  q.append(more, 3);
  TEST_ASSERT_TRUE(saver.flushNow(2110));
  TEST_ASSERT_EQUAL_INT(1, st.discards);  // the partial queue.tmp
  TEST_ASSERT_EQUAL_INT(2, st.opens);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_UINT32(203, q.size());
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 2).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_UINT32(2, st.posGeneration);
  TEST_ASSERT_FALSE(saver.busy());
}

// An edit still inside its 2 s (a power-off right after a queue edit): it
// is written now. With nothing to write, only the position; with nothing
// at all, nothing.
void test_flush_now_writes_an_edit_at_once() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);
  saver.loaded(4, false, 0);  // as restored: saved
  TEST_ASSERT_TRUE(saver.flushNow(10));
  TEST_ASSERT_EQUAL_INT(0, st.opens);
  TEST_ASSERT_EQUAL_INT(0, st.positions);
  q.step(1, true);
  TEST_ASSERT_TRUE(saver.flushNow(20));  // (no loop pass saw the move)
  TEST_ASSERT_EQUAL_INT(0, st.opens);
  TEST_ASSERT_EQUAL_INT(6, st.pos);
  TEST_ASSERT_EQUAL_UINT32(4, st.posGeneration);
  const uint32_t first = 0;
  q.remove(&first, 1);
  saver.loop(30);
  TEST_ASSERT_TRUE(saver.busy());
  TEST_ASSERT_TRUE(saver.flushNow(40));
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 5).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_INT(q.current(), st.pos);
  TEST_ASSERT_EQUAL_UINT32(5, st.posGeneration);
}

// The card fails: flushNow() says so and the last file stays; the saver
// isn't busy with a failed write (no reason to stay on for it), and tries
// again 10 s later.
void test_flush_now_that_fails_keeps_the_last_file() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  st.file = "last good";
  QueueSaver saver(st, q, c);
  saver.loaded(2, false, 0);
  fillLong(q);
  saver.loop(10);
  saver.loop(2100);
  TEST_ASSERT_TRUE(saver.writing());
  st.failCommit = true;
  TEST_ASSERT_FALSE(saver.flushNow(2110));
  TEST_ASSERT_EQUAL_STRING("last good", st.file.c_str());
  TEST_ASSERT_FALSE(saver.writing());
  TEST_ASSERT_TRUE(saver.contentDirty());
  TEST_ASSERT_FALSE(saver.busy());
  TEST_ASSERT_EQUAL_INT(0, st.positions);  // never paired with the old file
  st.failCommit = false;
  st.failOpen = true;
  TEST_ASSERT_FALSE(saver.flushNow(2120));
  st.failOpen = false;
  saver.loop(5000);
  TEST_ASSERT_EQUAL_STRING("last good", st.file.c_str());  // the retry waits its 10 s
  for (uint32_t t = 12200; t < 12400; t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 3).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_UINT32(3, st.posGeneration);
  // An edit after a failure makes it busy again.
  const uint32_t more[] = {2};
  q.append(more, 1);
  saver.loop(12500);
  TEST_ASSERT_TRUE(saver.busy());
}

// A write dropped for a library rebuild (remap), and the queue then marked
// saved: nothing is written.
void test_saver_abort_and_mark_saved() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  saver.loaded(1, false, 0);
  fillLong(q);
  saver.loop(10);
  saver.loop(2100);
  saver.abort();
  TEST_ASSERT_FALSE(saver.writing());
  TEST_ASSERT_EQUAL_INT(1, st.discards);
  saver.markSaved();
  TEST_ASSERT_FALSE(saver.busy());
  TEST_ASSERT_TRUE(saver.flushNow(3000));
  TEST_ASSERT_EQUAL_INT(0, st.commits);
}


// ---- the resume point (QueueSaver, the second an entry picks up at) ----

QueueSaver::Transport pausedAt(uint32_t ms, uint32_t dur = 0) {
  QueueSaver::Transport t;
  t.have = true;
  t.positionMs = ms;
  t.durationMs = dur;
  return t;
}

// Saved at a pause, once (a few ms more as the fade ends don't count),
// cleared as soon as it plays on, once; nothing while it plays.
void test_resume_point_saved_at_a_pause_and_cleared_when_it_plays() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);  // current 5
  saver.loaded(7, false, 0);
  for (uint32_t t = 0; t < 3000; t += 100) saver.loop(t);  // playing: nothing
  TEST_ASSERT_EQUAL_INT(0, st.resumes);
  saver.noteTransport(pausedAt(83000, 240000));
  saver.loop(3100);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);
  TEST_ASSERT_TRUE(st.resume.valid);
  TEST_ASSERT_EQUAL_UINT32(7, st.resume.generation);
  TEST_ASSERT_EQUAL_INT(5, st.resume.entry);
  TEST_ASSERT_EQUAL_UINT32(QueueSaver::pathHash(pathOf(c, q.currentTrack()).c_str()), st.resume.pathHash);
  TEST_ASSERT_EQUAL_UINT32(83000, st.resume.positionMs);
  TEST_ASSERT_EQUAL_UINT32(240000, st.resume.durationMs);
  saver.noteTransport(pausedAt(83012, 240000));
  for (uint32_t t = 3200; t < 6000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);
  TEST_ASSERT_FALSE(saver.busy());  // (nothing on its way: the idle power-off needn't wait)
  // It plays again: cleared at once, and only once.
  saver.noteTransport(QueueSaver::Transport{});
  saver.loop(6100);
  TEST_ASSERT_EQUAL_INT(2, st.resumes);
  TEST_ASSERT_FALSE(st.resume.valid);
  for (uint32_t t = 6200; t < 9000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(2, st.resumes);
  // Paused again later: the new second.
  saver.noteTransport(pausedAt(95000));
  saver.loop(9100);
  TEST_ASSERT_EQUAL_INT(3, st.resumes);
  TEST_ASSERT_EQUAL_UINT32(95000, st.resume.positionMs);
}

// An edit while paused: the point pairs with the file only once the file
// holds the queue; an entry that only moved (one before it removed) is
// saved again at its new line. A clear never waits.
void test_resume_point_waits_for_the_file_and_follows_its_entry() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);
  saver.loaded(2, false, 0);
  const uint32_t first = 0;
  q.remove(&first, 1);  // current 5 -> 4, the same entry
  saver.noteTransport(pausedAt(30000));
  saver.loop(10);
  TEST_ASSERT_EQUAL_INT(0, st.resumes);  // the file still has the old queue
  for (uint32_t t = 2100; saver.contentDirty() || saver.writing(); t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);
  TEST_ASSERT_EQUAL_UINT32(3, st.resume.generation);
  TEST_ASSERT_EQUAL_INT(4, st.resume.entry);
  TEST_ASSERT_EQUAL_UINT32(3, st.posGeneration);
  TEST_ASSERT_EQUAL_INT(4, st.pos);
  // Another edit, then it plays before the file is written: cleared now.
  const uint32_t more[] = {1};
  q.append(more, 1);
  saver.loop(9000);
  TEST_ASSERT_TRUE(saver.contentDirty());
  saver.noteTransport(QueueSaver::Transport{});
  saver.loop(9020);
  TEST_ASSERT_EQUAL_INT(2, st.resumes);
  TEST_ASSERT_FALSE(st.resume.valid);
}

// flushNow() (the CPU speed's restart pauses, then flushes; the idle
// power-off): the point saved with everything else, after the file.
void test_flush_now_saves_the_resume_point() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);
  saver.loaded(4, false, 0);
  const uint32_t more[] = {1, 2};
  q.append(more, 2);
  saver.loop(10);  // an edit inside its 2 s
  saver.noteTransport(pausedAt(61000, 180000));
  TEST_ASSERT_TRUE(saver.flushNow(20));
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);
  TEST_ASSERT_EQUAL_UINT32(5, st.resume.generation);
  TEST_ASSERT_EQUAL_INT(5, st.resume.entry);
  TEST_ASSERT_EQUAL_UINT32(61000, st.resume.positionMs);
  // Stopped (nothing to pick up): a flush clears it.
  saver.noteTransport(QueueSaver::Transport{});
  TEST_ASSERT_TRUE(saver.flushNow(30));
  TEST_ASSERT_FALSE(st.resume.valid);
}

// At boot: a point applies only to the entry it was saved for, in the file
// it was saved with, if that track is still there; one that doesn't apply
// is cleared at the first pass, one that does isn't written again.
void test_resume_point_at_boot() {
  QueueResume r;
  r.valid = true;
  r.generation = 9;
  r.entry = 3;
  r.positionMs = 83000;
  const char* path = "/music/Kavinsky/OutRun/08 - Nightcall.mp3";
  r.pathHash = QueueSaver::pathHash(path);
  TEST_ASSERT_TRUE(QueueSaver::resumeApplies(r, 9, 3, true, path));
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(r, 8, 3, true, path));   // another file
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(r, 9, 4, true, path));   // another entry current
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(r, 9, 3, false, path));  // its track is gone
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(r, 9, 3, true, "/music/Kavinsky/OutRun/09 - Odd Look.mp3"));
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(r, 9, 3, true, ""));
  QueueResume none = r;
  none.valid = false;
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(none, 9, 3, true, path));
  QueueResume zero = r;
  zero.positionMs = 0;
  TEST_ASSERT_FALSE(QueueSaver::resumeApplies(zero, 9, 3, true, path));
  TEST_ASSERT_NOT_EQUAL(QueueSaver::pathHash("a"), QueueSaver::pathHash("b"));

  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  fillLong(q);
  {
    // Applied: the player's start point is what the store has.
    MemStore st;
    QueueSaver saver(st, q, c);
    saver.loaded(9, false, 0);
    QueueResume saved = r;
    saved.entry = 5;
    saved.pathHash = QueueSaver::pathHash(pathOf(c, q.currentTrack()).c_str());
    saver.loadedResume(saved);
    saver.noteTransport(pausedAt(83000));
    for (uint32_t t = 0; t < 3000; t += 100) saver.loop(t);
    TEST_ASSERT_EQUAL_INT(0, st.resumes);
  }
  {
    // Not applied (the player has no start point): cleared.
    MemStore st;
    QueueSaver saver(st, q, c);
    saver.loaded(9, false, 0);
    saver.loadedResume(r);
    saver.loop(0);
    TEST_ASSERT_EQUAL_INT(1, st.resumes);
    TEST_ASSERT_FALSE(st.resume.valid);
  }
}

// The resume point's anchor (docs/SEEK.md section 5.2): saved with it;
// saved again once when only its sample moves (the pause's fade reads 64
// frames more: 1.5 ms, under the 250 ms slack), not again after; gone with
// the point; flushNow() writes the newest.
QueueSaver::Transport anchoredAt(uint32_t ms, uint64_t sample) {
  QueueSaver::Transport t = pausedAt(ms, 240000);
  t.anchor.kind = ResumeAnchor::Kind::Mp3;
  t.anchor.exact = true;
  t.anchor.rate = 44100;
  t.anchor.sample = sample;
  t.anchor.fileSize = 9000000;
  t.anchor.prerollByte = 1000000;
  t.anchor.frameByte = 1003000;
  t.anchor.skip = static_cast<uint32_t>(sample % 1152);
  t.anchor.frameHash = 0x1234;
  return t;
}

void test_resume_anchor_saved_with_the_point() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);
  saver.loaded(7, false, 0);
  // The pause: saved with its anchor in the same pass.
  saver.noteTransport(anchoredAt(83000, 3660300));
  saver.loop(100);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);
  TEST_ASSERT_TRUE(st.resume.anchor == anchoredAt(83000, 3660300).anchor);
  // The fade: 64 frames more, the same ms: saved again, once.
  saver.noteTransport(anchoredAt(83001, 3660364));
  for (uint32_t t = 200; t < 3000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(2, st.resumes);
  TEST_ASSERT_EQUAL_UINT64(3660364, st.resume.anchor.sample);
  TEST_ASSERT_EQUAL_UINT32(83001, st.resume.positionMs);
  // An anchor that goes (gapless trimming turned off while paused): saved
  // without one.
  saver.noteTransport(pausedAt(83001, 240000));
  saver.loop(3100);
  TEST_ASSERT_EQUAL_INT(3, st.resumes);
  TEST_ASSERT_FALSE(st.resume.anchor.valid());
  // It plays: the point goes, anchor and all.
  saver.noteTransport(QueueSaver::Transport{});
  saver.loop(3200);
  TEST_ASSERT_EQUAL_INT(4, st.resumes);
  TEST_ASSERT_FALSE(st.resume.valid);
  TEST_ASSERT_FALSE(saver.resume().anchor.valid());
  // Paused again, then the power-off's flush: the newest.
  saver.noteTransport(anchoredAt(90000, 3969000));
  saver.loop(3300);
  saver.noteTransport(anchoredAt(90001, 3969064));
  TEST_ASSERT_TRUE(saver.flushNow(3310));
  TEST_ASSERT_EQUAL_INT(6, st.resumes);
  TEST_ASSERT_EQUAL_UINT64(3969064, st.resume.anchor.sample);
  // A boot that restored it: the player's start point carries the same
  // anchor: nothing written; resumeApplies() doesn't look at the anchor.
  MemStore st2;
  QueueSaver boot(st2, q, c);
  boot.loaded(7, false, 0);
  boot.loadedResume(st.resume);
  TEST_ASSERT_TRUE(QueueSaver::resumeApplies(st.resume, 7, 5, true, pathOf(c, q.currentTrack()).c_str()));
  boot.noteTransport(anchoredAt(90001, 3969064));
  for (uint32_t t = 0; t < 2000; t += 100) boot.loop(t);
  TEST_ASSERT_EQUAL_INT(0, st2.resumes);
}

// A toggle is a content change: the file 2 s later, version 2 while
// shuffled and version 1 again after.
void test_a_toggle_rewrites_the_file() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);
  saver.loaded(3, false, 0);
  q.setShuffled(true);
  saver.loop(100);
  TEST_ASSERT_TRUE(saver.busy());
  saver.loop(2000);
  TEST_ASSERT_EQUAL_INT(0, st.opens);  // its 2 s, as any edit's
  for (uint32_t t = 2100; t < 4000; t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 4).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_INT(0, st.file.compare(0, 16, "mstream-queue 2 "));
  q.setShuffled(false);
  for (uint32_t t = 5000; t < 9000; t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(2, st.commits);
  TEST_ASSERT_EQUAL_INT(0, st.file.compare(0, 16, "mstream-queue 1 "));
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 5).c_str(), st.file.c_str());
}

// A toggle while a write is under way: that write (whose header said the
// old version) is dropped, and the queue written again whole.
void test_a_toggle_during_a_write_restarts_it() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  st.file = "old";
  QueueSaver saver(st, q, c);
  saver.loaded(8, false, 0);
  fillLong(q);
  saver.loop(10);
  saver.loop(2100);  // the write begins: version 1's header and 31 lines
  saver.loop(2120);
  TEST_ASSERT_TRUE(saver.writing());
  q.setShuffled(true);
  for (uint32_t t = 2140; t < 8000; t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_TRUE(st.discards >= 1);
  TEST_ASSERT_EQUAL_INT(0, st.file.compare(0, 16, "mstream-queue 2 "));
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, st.posGeneration).c_str(), st.file.c_str());
}

// Off while paused moves the current entry to its own place: the resume
// point is saved again at the new line once the file holds it (the
// moved-only rule), with the new generation.
void test_off_while_paused_pairs_the_resume_point_again() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);  // current 5
  saver.loaded(2, false, 0);
  q.setShuffled(true);
  q.step(+3, false);  // a shuffled entry plays: position 8
  for (uint32_t t = 0; t < 6000; t += 20) saver.loop(t);  // the shuffled file written
  saver.noteTransport(pausedAt(30000));
  saver.loop(6100);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);
  TEST_ASSERT_EQUAL_INT(8, st.resume.entry);
  const uint32_t gen = st.resume.generation;
  const uint32_t key = q.currentKey();
  q.setShuffled(false);
  TEST_ASSERT_EQUAL_UINT32(key, q.currentKey());
  const int32_t own = q.current();
  TEST_ASSERT_TRUE(own != 8);
  saver.loop(6200);
  TEST_ASSERT_EQUAL_INT(1, st.resumes);  // the file doesn't hold the order yet
  for (uint32_t t = 8300; saver.contentDirty() || saver.writing(); t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(2, st.resumes);
  TEST_ASSERT_TRUE(st.resume.valid);
  TEST_ASSERT_EQUAL_INT(own, st.resume.entry);
  TEST_ASSERT_EQUAL_UINT32(gen + 1, st.resume.generation);
  TEST_ASSERT_EQUAL_UINT32(30000, st.resume.positionMs);
  TEST_ASSERT_EQUAL_INT(own, st.pos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_queue);
  RUN_TEST(test_replace_sets_current_at_start);
  RUN_TEST(test_insert_next_goes_after_current);
  RUN_TEST(test_append_and_adding_to_an_empty_queue);
  RUN_TEST(test_keys_are_stable_across_edits);
  RUN_TEST(test_remove_before_and_after_current);
  RUN_TEST(test_remove_current_moves_to_the_next_survivor);
  RUN_TEST(test_remove_current_with_nothing_after_it);
  RUN_TEST(test_remove_nothing_keeps_the_undo);
  RUN_TEST(test_move_next_in_queue_order);
  RUN_TEST(test_move_next_of_the_current_entry_changes_nothing);
  RUN_TEST(test_clear_up_next_keeps_the_current_and_history);
  RUN_TEST(test_clear_and_undo);
  RUN_TEST(test_undo_keeps_what_plays_current);
  RUN_TEST(test_step_and_set_current);
  RUN_TEST(test_peek_is_step_without_the_step);
  RUN_TEST(test_memory_from_the_hooks_and_out_of_memory);
  RUN_TEST(test_an_edit_without_memory_for_its_snapshot_is_not_undoable);
  RUN_TEST(test_random_edits_match_a_simple_model);
  RUN_TEST(test_shuffle_on_keeps_what_played_and_what_plays);
  RUN_TEST(test_shuffle_off_brings_the_own_order_back);
  RUN_TEST(test_shuffle_with_nothing_up_next);
  RUN_TEST(test_shuffled_adds_keep_their_place);
  RUN_TEST(test_shuffled_move_remove_and_clear_up_next);
  RUN_TEST(test_shuffled_play_puts_the_chosen_track_first);
  RUN_TEST(test_an_add_to_an_empty_shuffled_queue);
  RUN_TEST(test_undo_while_shuffled_restores_the_ranks);
  RUN_TEST(test_a_toggle_drops_the_undo);
  RUN_TEST(test_shuffle_is_repeatable_and_uniform);
  RUN_TEST(test_a_toggle_allocates_nothing);
  RUN_TEST(test_assign_with_ranks);
  RUN_TEST(test_a_rank_overflow_is_refused);
  RUN_TEST(test_random_shuffled_edits_keep_the_own_order);
  RUN_TEST(test_permute_is_a_permutation_and_repeatable);
  RUN_TEST(test_catalog_library_and_builtin_ids);
  RUN_TEST(test_text_round_trip);
  RUN_TEST(test_text_remaps_after_a_rebuild);
  RUN_TEST(test_text_current_override_and_bad_files);
  RUN_TEST(test_text_writer_in_steps);
  RUN_TEST(test_v1_unchanged_while_not_shuffled);
  RUN_TEST(test_v2_round_trip);
  RUN_TEST(test_v2_dropped_tracks_leave_rank_gaps);
  RUN_TEST(test_v2_bad_lines_leave_the_queue_alone);
  RUN_TEST(test_v2_long_path_and_ten_digit_rank);
  RUN_TEST(test_v2_empty_shuffled_queue);
  RUN_TEST(test_saver_writes_after_the_edits_settle);
  RUN_TEST(test_flush_now_in_the_middle_of_a_write);
  RUN_TEST(test_flush_now_after_an_edit_during_the_write);
  RUN_TEST(test_flush_now_writes_an_edit_at_once);
  RUN_TEST(test_flush_now_that_fails_keeps_the_last_file);
  RUN_TEST(test_saver_abort_and_mark_saved);
  RUN_TEST(test_resume_point_saved_at_a_pause_and_cleared_when_it_plays);
  RUN_TEST(test_resume_point_waits_for_the_file_and_follows_its_entry);
  RUN_TEST(test_flush_now_saves_the_resume_point);
  RUN_TEST(test_resume_point_at_boot);
  RUN_TEST(test_resume_anchor_saved_with_the_point);
  RUN_TEST(test_a_toggle_rewrites_the_file);
  RUN_TEST(test_a_toggle_during_a_write_restarts_it);
  RUN_TEST(test_off_while_paused_pairs_the_resume_point_again);
  return UNITY_END();
}
