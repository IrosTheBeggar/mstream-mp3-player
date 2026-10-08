// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the play queue: QueueModel (edits, positions, keys, undo,
// shuffle and its ranks: docs/QUEUE-MODES.md; its memory: release() and
// the exact trim), TrackCatalog (library and built-in ids), QueueText (the
// queue saved as paths, and read back after a library rebuild), QueueSaver
// and QueueRemap (the queue carried across a rebuild through queue.txt:
// docs/METADATA.md 3.4.2, milestone N3). Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ByteStream.h"
#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueRemap.h"
#include "QueueSaver.h"
#include "QueueText.h"
#include "Shuffle.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

namespace {
// The global heap, counted while `counting` (test_a_toggle_allocates_nothing):
// QueueModel's hooks can't see an operator new or a std::stable_sort's
// buffer (get_temporary_buffer: a nothrow new), and on the device that
// would land on the loop task's heap. The replacements below serve the whole
// test program; they only count while asked to.
struct GlobalNew {
  static bool counting;
  static long count;
  static void* take(std::size_t n) {
    if (counting) ++count;
    return std::malloc(n ? n : 1);
  }
};
bool GlobalNew::counting = false;
long GlobalNew::count = 0;
}  // namespace

void* operator new(std::size_t n) {
  if (void* p = GlobalNew::take(n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return GlobalNew::take(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return GlobalNew::take(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

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

// An allocator that meters bytes, the firmware's PSRAM in miniature: what
// is live, the most at once since mark(), and a ceiling (what PSRAM has
// left) past which it fails; none by default.
struct Meter {
  static size_t live, peak, ceiling;
  static constexpr size_t kHead = 16;  // the size, ahead of the block (keeps malloc's alignment)
  static void* alloc(size_t n) {
    if (n > ceiling || live > ceiling - n) return nullptr;
    auto* p = static_cast<unsigned char*>(std::malloc(n + kHead));
    if (!p) return nullptr;
    std::memcpy(p, &n, sizeof(n));
    live += n;
    peak = std::max(peak, live);
    return p + kHead;
  }
  static void release(void* block) {
    if (!block) return;
    unsigned char* p = static_cast<unsigned char*>(block) - kHead;
    size_t n = 0;
    std::memcpy(&n, p, sizeof(n));
    live -= n;
    std::free(p);
  }
  static void mark() { peak = live; }
  static void reset() {
    live = peak = 0;
    ceiling = SIZE_MAX;
  }
};
size_t Meter::live = 0;
size_t Meter::peak = 0;
size_t Meter::ceiling = SIZE_MAX;

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
  Meter::reset();
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

// Shuffle all: a Play that turns the mode on, one edit; its undo puts back
// the queue and the mode it found (docs/QUEUE-MODES.md section 2.6).
void test_a_play_that_sets_the_mode_undoes_it_too() {
  const uint32_t lib[] = {50, 51, 52, 53, 54, 55, 56, 57, 58, 59};
  {
    // From an empty queue, shuffle off (the device's case): undo leaves it
    // empty and off, not "shuffle on" over nothing.
    QueueModel q;
    TEST_ASSERT_TRUE(q.replace(lib, 10, QueueModel::kAnyStart, true));
    TEST_ASSERT_TRUE(q.shuffled());
    TEST_ASSERT_EQUAL_INT(0, q.current());
    TEST_ASSERT_TRUE(ownOrder(q) == std::vector<uint32_t>(std::begin(lib), std::end(lib)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Replace), static_cast<int>(q.undoable()));
    TEST_ASSERT_FALSE(q.undoShuffled());  // what the undo puts back
    const uint32_t v = q.contentVersion();
    TEST_ASSERT_TRUE(q.undo());
    TEST_ASSERT_FALSE(q.shuffled());
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_EQUAL_INT(-1, q.current());
    TEST_ASSERT_TRUE(q.contentVersion() != v);  // the saver writes the mode back
    TEST_ASSERT_FALSE(q.undoShuffled());        // (no undo: the mode itself)
  }
  {
    // Over a queue, off, mid-way: its own order and its current entry come
    // back, unshuffled; an add after the undo is no shuffled add.
    QueueModel q;
    fill(q, 6, 3);
    const std::vector<uint32_t> was = keys(q);
    TEST_ASSERT_TRUE(q.replace(lib, 10, QueueModel::kAnyStart, true));
    TEST_ASSERT_TRUE(q.undo());
    TEST_ASSERT_FALSE(q.shuffled());
    TEST_ASSERT_TRUE(keys(q) == was);
    expectTracks(q, {10, 11, 12, 13, 14, 15});
    TEST_ASSERT_EQUAL_INT(3, q.current());
    for (uint32_t i = 0; i < q.size(); ++i) TEST_ASSERT_EQUAL_UINT32(i, q.rankAt(i));
    const uint32_t two[] = {7, 8};
    TEST_ASSERT_TRUE(q.append(two, 2));
    expectTracks(q, {10, 11, 12, 13, 14, 15, 7, 8});
  }
  {
    // Already on: the undo keeps it on, with the old order and its ranks.
    QueueModel q;
    fill(q, 10, 2);
    q.setShuffled(true);
    const std::vector<uint32_t> was = keys(q), own = ownOrder(q);
    TEST_ASSERT_TRUE(q.replace(lib, 10, QueueModel::kAnyStart, true));
    TEST_ASSERT_TRUE(q.undoShuffled());
    TEST_ASSERT_TRUE(q.undo());
    TEST_ASSERT_TRUE(q.shuffled());
    TEST_ASSERT_TRUE(keys(q) == was);
    TEST_ASSERT_TRUE(ownOrder(q) == own);
  }
  {
    // The other way (a Play that turns it off): the same rule.
    QueueModel q;
    fill(q, 10, 2);
    q.setShuffled(true);
    const std::vector<uint32_t> was = keys(q);
    TEST_ASSERT_TRUE(q.replace(lib, 10, 4, false));
    TEST_ASSERT_FALSE(q.shuffled());
    TEST_ASSERT_EQUAL_INT(4, q.current());
    expectTracks(q, {std::begin(lib), std::end(lib)});
    TEST_ASSERT_TRUE(q.undo());
    TEST_ASSERT_TRUE(q.shuffled());
    TEST_ASSERT_TRUE(keys(q) == was);
  }
  {
    // A toggle after it is no edit: it drops that undo, as any.
    QueueModel q;
    fill(q, 6, 3);
    q.replace(lib, 10, QueueModel::kAnyStart, true);
    q.setShuffled(false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
    TEST_ASSERT_FALSE(q.undo());
    TEST_ASSERT_EQUAL_UINT32(10, q.size());
  }
  {
    // No memory for the Play: false, and neither the queue nor the mode
    // changed.
    QueueModel q(Heap::alloc, Heap::release);
    fill(q, 6, 3);  // (room for 6: assign() is exact)
    const std::vector<uint32_t> was = keys(q);
    const uint32_t v = q.contentVersion();
    std::vector<uint32_t> big(40, 50);
    Heap::failing = true;
    TEST_ASSERT_FALSE(q.replace(big.data(), 40, QueueModel::kAnyStart, true));
    Heap::failing = false;
    TEST_ASSERT_FALSE(q.shuffled());
    TEST_ASSERT_TRUE(keys(q) == was);
    TEST_ASSERT_EQUAL_UINT32(v, q.contentVersion());
  }
  {
    // Nothing to play: a Clear, in that mode, undone with it. In the same
    // mode, clear() itself (an empty queue: no edit, the undo kept).
    QueueModel q;
    fill(q, 6, 3);
    TEST_ASSERT_TRUE(q.replace(nullptr, 0, 0, true));
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_TRUE(q.shuffled());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Clear), static_cast<int>(q.undoable()));
    TEST_ASSERT_TRUE(q.undo());
    TEST_ASSERT_FALSE(q.shuffled());
    expectTracks(q, {10, 11, 12, 13, 14, 15});
    TEST_ASSERT_EQUAL_INT(3, q.current());
    QueueModel e;
    const uint32_t one[] = {9};
    e.append(one, 1);
    e.clear();
    TEST_ASSERT_TRUE(e.replace(nullptr, 0, 0, false));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Clear), static_cast<int>(e.undoable()));
    TEST_ASSERT_TRUE(e.undo());
    expectTracks(e, {9});
  }
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
  // The global count sees what it is for: a stable sort's buffer.
  {
    uint32_t probe[64] = {};
    const long before = GlobalNew::count;
    GlobalNew::counting = true;
    std::stable_sort(probe, probe + 64);
    GlobalNew::counting = false;
    TEST_ASSERT_TRUE(GlobalNew::count > before);
  }
  QueueModel q(Heap::alloc, Heap::release);
  fill(q, QueueModel::kMaxEntries - 1, 2500);  // (and the add below fills it: the cap)
  const uint32_t add[] = {1};
  TEST_ASSERT_TRUE(q.append(add, 1));  // (the snapshot's memory exists)
  const long allocs = Heap::allocs;
  const long news = GlobalNew::count;
  Heap::failing = true;  // and none could be had
  GlobalNew::counting = true;  // nor taken from the global heap
  const bool on = q.setShuffled(true);
  const bool off = q.setShuffled(false);
  GlobalNew::counting = false;
  Heap::failing = false;
  TEST_ASSERT_TRUE(on);
  TEST_ASSERT_TRUE(off);
  TEST_ASSERT_EQUAL_INT(allocs, Heap::allocs);
  TEST_ASSERT_EQUAL_INT(news, GlobalNew::count);
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kMaxEntries, q.size());
  TEST_ASSERT_EQUAL_UINT32(2510, q.currentTrack());
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

void settle(QueueSaver& saver, uint32_t from);
QueueSaver::Transport pausedAt(uint32_t ms, uint32_t dur = 0);

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

// A write dropped for a library rebuild (remap), and the queue then kept
// as less than the file (a rebuild that left no library): nothing is
// written, and its moves don't touch the file's line or the resume point
// (they aren't the file's lines); an edit makes the queue the file's again.
void test_saver_abort_and_kept_file() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  saver.loaded(1, false, 0);
  TEST_ASSERT_TRUE(saver.fileIsQueue());
  fillLong(q);
  saver.loop(10);
  saver.loop(2100);
  saver.abort();
  TEST_ASSERT_FALSE(saver.writing());
  TEST_ASSERT_EQUAL_INT(1, st.discards);
  saver.keptFile(1, 37);
  TEST_ASSERT_FALSE(saver.fileIsQueue());
  TEST_ASSERT_EQUAL_INT(37, saver.fileLine());
  TEST_ASSERT_FALSE(saver.busy());
  TEST_ASSERT_TRUE(saver.flushNow(3000));
  TEST_ASSERT_EQUAL_INT(0, st.commits);
  // A move, a pause: nothing saved, and nothing left waiting.
  const int positions = st.positions, resumes = st.resumes;
  q.step(1, true);
  saver.noteTransport(pausedAt(42000));
  for (uint32_t t = 3000; t < 8000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(positions, st.positions);
  TEST_ASSERT_EQUAL_INT(resumes, st.resumes);
  TEST_ASSERT_FALSE(saver.busy());
  TEST_ASSERT_TRUE(saver.flushNow(8000));
  TEST_ASSERT_EQUAL_INT(37, saver.fileLine());
  // An edit: written (the listener's queue now), its position with it.
  const uint32_t first = 0;
  q.remove(&first, 1);
  settle(saver, 9000);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_TRUE(saver.fileIsQueue());
  TEST_ASSERT_EQUAL_INT(q.current(), st.pos);
  TEST_ASSERT_EQUAL_INT(q.current(), saver.fileLine());
  TEST_ASSERT_TRUE(st.resume.valid);  // and the resume point, paired with the new file
  TEST_ASSERT_EQUAL_UINT32(2, st.resume.generation);
}


// ---- the resume point (QueueSaver, the second an entry picks up at) ----

QueueSaver::Transport pausedAt(uint32_t ms, uint32_t dur) {
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

// Shuffle all, then its Undo: the file is version 2 after the Play and
// version 1 again after the undo, the old queue whole (the mode is saved
// with the queue, so the undo's mode reaches the card as any edit does).
void test_shuffle_alls_undo_writes_version_1_again() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  QueueSaver saver(st, q, c);
  fillLong(q);
  saver.loaded(3, false, 0);
  const uint32_t all[] = {0, 1, 2, 3, 4, 5};
  TEST_ASSERT_TRUE(q.replace(all, 6, QueueModel::kAnyStart, true));
  for (uint32_t t = 0; t < 4000; t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_INT(0, st.file.compare(0, 16, "mstream-queue 2 "));
  TEST_ASSERT_TRUE(q.undo());
  for (uint32_t t = 5000; t < 9000; t += 20) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(2, st.commits);
  TEST_ASSERT_EQUAL_INT(0, st.file.compare(0, 16, "mstream-queue 1 "));
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, st.posGeneration).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_UINT32(200, q.size());
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

// ---- QueueModel's memory: release() and the exact trim (docs/METADATA.md 3.5) ----

void test_assign_is_exact_and_growth_is_bounded() {
  QueueModel q(Meter::alloc, Meter::release);
  std::vector<uint32_t> ids(20000);
  for (uint32_t i = 0; i < 20000; ++i) ids[i] = i;
  // The boot's default queue on a library of 20,000: its first 5,000 (the
  // cap), 12 bytes an entry, no snapshot.
  TEST_ASSERT_TRUE(q.assign(ids.data(), 20000, 0));
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kMaxEntries, q.size());
  TEST_ASSERT_EQUAL_size_t(60000, Meter::live);
  TEST_ASSERT_EQUAL_size_t(60000, q.memoryBytes());
  // Full: an add is refused, and asks nothing of the hooks.
  const uint32_t one[] = {7};
  TEST_ASSERT_FALSE(q.append(one, 1));
  TEST_ASSERT_EQUAL_size_t(60000, Meter::live);
  // 4,000, then a track more: the first edit takes its snapshot, exact,
  // and the entries grow by doubling, but never past the cap (5,000, not
  // 8,000).
  TEST_ASSERT_TRUE(q.assign(ids.data(), 4000, 0));
  TEST_ASSERT_EQUAL_size_t(48000, Meter::live);
  TEST_ASSERT_TRUE(q.append(one, 1));
  TEST_ASSERT_EQUAL_size_t((5000 + 4000) * 12, Meter::live);
  TEST_ASSERT_EQUAL_size_t(Meter::live, q.memoryBytes());
  // Another assign: exact again (a new block), the snapshot given back.
  TEST_ASSERT_TRUE(q.assign(ids.data(), 500, 3));
  TEST_ASSERT_EQUAL_size_t(500 * 12, Meter::live);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  // The same size: the same block, nothing asked of the hooks.
  Meter::ceiling = Meter::live;
  TEST_ASSERT_TRUE(q.assign(ids.data() + 100, 500, 3));
  TEST_ASSERT_EQUAL_UINT32(103, q.currentTrack());
  // Smaller with no memory for a new block: the bigger one kept, still filled.
  TEST_ASSERT_TRUE(q.assign(ids.data(), 30, 29));
  TEST_ASSERT_EQUAL_UINT32(30, q.size());
  TEST_ASSERT_EQUAL_UINT32(29, q.currentTrack());
  TEST_ASSERT_EQUAL_size_t(500 * 12, Meter::live);
  // Bigger with no memory: false, the queue as it was.
  TEST_ASSERT_FALSE(q.assign(ids.data(), 600, 0));
  TEST_ASSERT_EQUAL_UINT32(30, q.size());
  TEST_ASSERT_EQUAL_INT(29, q.current());
  Meter::ceiling = SIZE_MAX;
  // Nothing: the block given back, the mode as asked.
  TEST_ASSERT_TRUE(q.assign(nullptr, 0, -1, true));
  TEST_ASSERT_EQUAL_size_t(0, Meter::live);
  TEST_ASSERT_TRUE(q.shuffled());
  // Small queues grow as before: 16 at first, then doubling. (An add to
  // an empty queue: its snapshot is of nothing, and holds nothing.)
  QueueModel s(Meter::alloc, Meter::release);
  TEST_ASSERT_TRUE(s.append(ids.data(), 3));
  TEST_ASSERT_EQUAL_size_t(16 * 12, s.memoryBytes());
  TEST_ASSERT_TRUE(s.append(ids.data(), 14));
  TEST_ASSERT_EQUAL_size_t((32 + 16) * 12, s.memoryBytes());
}

void test_release_gives_everything_back() {
  QueueModel q(Meter::alloc, Meter::release);
  fill(q, 1000, 400);
  const uint32_t at[] = {10};
  q.remove(at, 1);  // a snapshot
  q.setShuffled(true);
  const uint32_t more[] = {1, 2};
  q.append(more, 2);  // another snapshot, shuffled
  TEST_ASSERT_TRUE(Meter::live > 1000 * 12);
  std::set<uint32_t> keys;
  for (uint32_t i = 0; i < q.size(); ++i) keys.insert(q.keyAt(i));
  const uint32_t content = q.contentVersion(), position = q.positionVersion();
  q.release();
  TEST_ASSERT_EQUAL_size_t(0, Meter::live);
  TEST_ASSERT_EQUAL_size_t(0, q.memoryBytes());
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(-1, q.current());
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kNone, q.currentTrack());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  TEST_ASSERT_FALSE(q.undo());
  TEST_ASSERT_TRUE(q.shuffled());  // the listener's mode
  TEST_ASSERT_TRUE(q.contentVersion() != content);
  TEST_ASSERT_TRUE(q.positionVersion() != position);
  q.release();  // twice: nothing more
  TEST_ASSERT_EQUAL_size_t(0, Meter::live);
  // Read back after: fresh keys, never one from before.
  fill(q, 50, 0);
  for (uint32_t i = 0; i < q.size(); ++i) TEST_ASSERT_TRUE(keys.count(q.keyAt(i)) == 0);
  TEST_ASSERT_EQUAL_size_t(50 * 12, Meter::live);
}

// ---- the cap: 5,000 entries (docs/QUEUE-MODES.md section 15) ----

std::vector<uint32_t> range(uint32_t n, uint32_t from = 10) {
  std::vector<uint32_t> ids(n);
  for (uint32_t i = 0; i < n; ++i) ids[i] = from + i;
  return ids;
}

void expectWindow(uint32_t n, int32_t current, uint32_t first, uint32_t count) {
  const QueueModel::Window w = QueueModel::window(n, current);
  char msg[64];
  std::snprintf(msg, sizeof(msg), "window(%lu, %ld)", static_cast<unsigned long>(n), static_cast<long>(current));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(first, w.first, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, w.count, msg);
}

void test_window_holds_the_current_entry() {
  TEST_ASSERT_EQUAL_UINT32(5000, QueueModel::kMaxEntries);
  // What fits: all of it, wherever the current entry is.
  expectWindow(0, -1, 0, 0);
  expectWindow(40, 39, 0, 40);
  expectWindow(5000, 4999, 0, 5000);
  // Past the cap: the first 5,000 while the current entry is among them.
  expectWindow(20000, -1, 0, 5000);
  expectWindow(20000, 0, 0, 5000);
  expectWindow(20000, 4999, 0, 5000);
  // Else from the current entry on (what played before it goes)...
  expectWindow(20000, 5000, 5000, 5000);
  expectWindow(20000, 7342, 7342, 5000);
  expectWindow(5001, 5000, 1, 5000);
  // ... moved back when fewer than 5,000 follow it: still full.
  expectWindow(20000, 16000, 15000, 5000);
  expectWindow(20000, 19999, 15000, 5000);
  expectWindow(20000, 40000, 15000, 5000);  // (out of range: the last 5,000)
}

// The boot's default queue on a big library, an older firmware's queue
// read back: the window that holds the current entry, exact, no undo.
void test_assign_past_the_cap_keeps_the_window() {
  QueueModel q(Meter::alloc, Meter::release);
  const std::vector<uint32_t> ids = range(20000);
  TEST_ASSERT_TRUE(q.assign(ids.data(), 20000, 7342));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_EQUAL_UINT32(10 + 7342, q.trackAt(0));
  TEST_ASSERT_EQUAL_UINT32(10 + 12341, q.trackAt(4999));
  TEST_ASSERT_EQUAL_size_t(5000 * 12, Meter::live);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  TEST_ASSERT_EQUAL_UINT32(0, q.room());
  // The first 5,000 when it is among them.
  TEST_ASSERT_TRUE(q.assign(ids.data(), 20000, 123));
  TEST_ASSERT_EQUAL_INT(123, q.current());
  TEST_ASSERT_EQUAL_UINT32(10, q.trackAt(0));
  // Shuffled, the window's ranks come with it (gaps are fine) and off
  // lays it out by them.
  std::vector<uint32_t> ranks(20000);
  for (uint32_t i = 0; i < 20000; ++i) ranks[i] = 20000 - i;  // the play order is the own order backwards
  TEST_ASSERT_TRUE(q.assign(ids.data(), 20000, 16000, true, ranks.data()));
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(1000, q.current());
  TEST_ASSERT_EQUAL_UINT32(10 + 15000, q.trackAt(0));
  TEST_ASSERT_EQUAL_UINT32(5000, q.rankAt(0));
  TEST_ASSERT_EQUAL_UINT32(10 + 16000, q.currentTrack());
  q.setShuffled(false);
  TEST_ASSERT_EQUAL_UINT32(10 + 19999, q.trackAt(0));
  TEST_ASSERT_EQUAL_UINT32(10 + 16000, q.currentTrack());
  TEST_ASSERT_EQUAL_INT(3999, q.current());
}

// Play all and a big container's Play, shuffle off: the first 5,000 (or
// the window that holds the tapped track), undoable like any Play.
void test_play_past_the_cap_in_order() {
  QueueModel q(Meter::alloc, Meter::release);
  const std::vector<uint32_t> ids = range(20000);
  fill(q, 30, 4);
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, QueueModel::kAnyStart));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(0, q.current());
  for (uint32_t i = 0; i < 5000; ++i) TEST_ASSERT_EQUAL_UINT32(10 + i, q.trackAt(i));
  // Never more than the cap's blocks: the entries and the snapshot.
  TEST_ASSERT_TRUE(Meter::live <= 2 * 5000 * 12);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Replace), static_cast<int>(q.undoable()));
  TEST_ASSERT_TRUE(q.undo());  // the undo is kept, whatever the size
  TEST_ASSERT_EQUAL_UINT32(30, q.size());
  TEST_ASSERT_EQUAL_INT(4, q.current());
  // A start among the first 5,000: those; past them: from it on.
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, 3000));
  TEST_ASSERT_EQUAL_INT(3000, q.current());
  TEST_ASSERT_EQUAL_UINT32(10, q.trackAt(0));
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, 7342));
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_EQUAL_UINT32(10 + 7342, q.currentTrack());
  TEST_ASSERT_EQUAL_UINT32(10 + 12341, q.trackAt(4999));
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, 19000));
  TEST_ASSERT_EQUAL_INT(4000, q.current());
  TEST_ASSERT_EQUAL_UINT32(10 + 19000, q.currentTrack());
  TEST_ASSERT_EQUAL_UINT32(10 + 15000, q.trackAt(0));
  // Exactly the cap: all of it.
  TEST_ASSERT_TRUE(q.replace(ids.data(), 5000, 4999));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(4999, q.current());
  // Out of memory for the cap's block: refused whole, the queue as it was.
  fill(q, 30, 4);
  const uint32_t before = q.contentVersion();
  Meter::ceiling = Meter::live;
  TEST_ASSERT_FALSE(q.replace(ids.data(), 20000, 0));
  TEST_ASSERT_FALSE(q.replace(ids.data(), 20000, QueueModel::kAnyStart, true));
  Meter::ceiling = SIZE_MAX;
  TEST_ASSERT_EQUAL_UINT32(before, q.contentVersion());
  TEST_ASSERT_EQUAL_UINT32(30, q.size());
  TEST_ASSERT_FALSE(q.shuffled());
}

// Shuffle all on a big library: the chosen track (a random one) and a
// random 4,999 of the rest, each once; the ranks their places in the
// list, so Off gives them in the library's order.
void test_shuffled_play_past_the_cap_takes_a_random_5000() {
  QueueModel q;
  const std::vector<uint32_t> ids = range(20000);
  fill(q, 30, 4);  // not shuffled
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, QueueModel::kAnyStart, true));
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(0, q.current());
  std::set<uint32_t> seen;
  uint32_t inOrder = 0;
  for (uint32_t i = 0; i < 5000; ++i) {
    const uint32_t t = q.trackAt(i);
    TEST_ASSERT_TRUE(t >= 10 && t < 20010);
    TEST_ASSERT_EQUAL_UINT32(t - 10, q.rankAt(i));  // the rank: its place in the list
    seen.insert(t);
    if (i > 0 && q.trackAt(i) > q.trackAt(i - 1)) ++inOrder;
  }
  TEST_ASSERT_EQUAL_size_t(5000, seen.size());  // each once
  TEST_ASSERT_TRUE(*seen.rbegin() > 15000 && *seen.begin() < 5000);  // from the whole list, not the first 5,000
  TEST_ASSERT_TRUE(inOrder > 2000 && inOrder < 3000);  // shuffled (sorted would be 4,999)
  const uint32_t first = q.currentTrack();
  // Off: the 5,000 in the list's order, the first still current.
  q.setShuffled(false);
  for (uint32_t i = 1; i < 5000; ++i) TEST_ASSERT_TRUE(q.trackAt(i) > q.trackAt(i - 1));
  TEST_ASSERT_EQUAL_UINT32(first, q.currentTrack());
  // Its undo: the queue before, whole (the undo is kept at the cap).
  q.setShuffled(true);
  const std::vector<uint32_t> was = tracks(q);
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, QueueModel::kAnyStart, true));
  TEST_ASSERT_TRUE(q.undo());
  TEST_ASSERT_TRUE(tracks(q) == was);
  // Another Shuffle all: another 5,000.
  std::set<uint32_t> again;
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, QueueModel::kAnyStart, true));
  for (uint32_t i = 0; i < 5000; ++i) again.insert(q.trackAt(i));
  TEST_ASSERT_TRUE(again != seen);
  // A chosen track (a tapped one, shuffled): first, and never twice.
  TEST_ASSERT_TRUE(q.replace(ids.data(), 20000, 12345));
  TEST_ASSERT_EQUAL_UINT32(10 + 12345, q.currentTrack());
  TEST_ASSERT_EQUAL_INT(0, q.current());
  TEST_ASSERT_EQUAL_UINT32(12345, q.rankAt(0));
  for (uint32_t i = 1; i < 5000; ++i) TEST_ASSERT_TRUE(q.trackAt(i) != 10 + 12345);
  // With the hook: the pick and the shuffle are one draw each, plus the
  // random first's.
  hookDraws = 0;
  QueueModel h(nullptr, nullptr, countingRandom);
  TEST_ASSERT_TRUE(h.replace(ids.data(), 20000, QueueModel::kAnyStart, true));
  TEST_ASSERT_EQUAL_UINT32(3, hookDraws);
}

// The pick (shuffle::sample()): exactly k, ascending, every index as
// likely; k >= n takes every one; repeatable from its seed.
void test_sample_is_exact_and_uniform() {
  std::vector<uint32_t> got;
  shuffle::sample(10, 3, 77, [&](uint32_t i) { got.push_back(i); });
  TEST_ASSERT_EQUAL_size_t(3, got.size());
  TEST_ASSERT_TRUE(std::is_sorted(got.begin(), got.end()));
  std::vector<uint32_t> again;
  shuffle::sample(10, 3, 77, [&](uint32_t i) { again.push_back(i); });
  TEST_ASSERT_TRUE(got == again);
  std::vector<uint32_t> all;
  shuffle::sample(5, 9, 1, [&](uint32_t i) { all.push_back(i); });
  TEST_ASSERT_TRUE(all == std::vector<uint32_t>({0, 1, 2, 3, 4}));
  int none = 0;
  shuffle::sample(5, 0, 1, [&](uint32_t) { ++none; });
  shuffle::sample(0, 3, 1, [&](uint32_t) { ++none; });
  TEST_ASSERT_EQUAL_INT(0, none);
  // 3 of 10, 30,000 seeds: each index taken within 3 % of 30 % of them.
  constexpr int kRuns = 30000;
  int count[10] = {};
  uint32_t seed = 12345;
  for (int r = 0; r < kRuns; ++r) {
    seed = seed * 1664525u + 1013904223u;
    int taken = 0;
    shuffle::sample(10, 3, seed, [&](uint32_t i) {
      ++count[i];
      ++taken;
    });
    TEST_ASSERT_EQUAL_INT(3, taken);
  }
  for (int c : count) TEST_ASSERT_INT_WITHIN(kRuns * 3 / 10 * 3 / 100, kRuns * 3 / 10, c);
}

// Play next and + Queue past the cap: the first ones that fit, in their
// order; the Undo takes the whole add back.
void test_an_add_takes_what_fits() {
  QueueModel q;
  fill(q, 4990, 100);
  TEST_ASSERT_EQUAL_UINT32(10, q.room());
  const std::vector<uint32_t> twelve = range(12, 900000);
  TEST_ASSERT_TRUE(q.append(twelve.data(), 12));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_UINT32(0, q.room());
  for (uint32_t i = 0; i < 10; ++i) TEST_ASSERT_EQUAL_UINT32(900000 + i, q.trackAt(4990 + i));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Append), static_cast<int>(q.undoable()));
  TEST_ASSERT_TRUE(q.undo());
  TEST_ASSERT_EQUAL_UINT32(4990, q.size());
  // Play next: right after the current entry, the first ten.
  TEST_ASSERT_TRUE(q.insertNext(twelve.data(), 12));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(100, q.current());
  for (uint32_t i = 0; i < 10; ++i) TEST_ASSERT_EQUAL_UINT32(900000 + i, q.trackAt(101 + i));
  TEST_ASSERT_EQUAL_UINT32(10 + 101, q.trackAt(111));  // what was next follows them
  // An add to an empty queue past the cap: its first 5,000, the first current.
  QueueModel e;
  const std::vector<uint32_t> many = range(6000);
  TEST_ASSERT_TRUE(e.append(many.data(), 6000));
  TEST_ASSERT_EQUAL_UINT32(5000, e.size());
  TEST_ASSERT_EQUAL_INT(0, e.current());
  TEST_ASSERT_EQUAL_UINT32(10 + 4999, e.trackAt(4999));
}

// Full: Play next and + Queue are refused (false), nothing changes, and the
// last edit's undo stays; a remove makes room again.
void test_a_full_queue_refuses_an_add() {
  QueueModel q(Heap::alloc, Heap::release);
  fill(q, 5000, 10);
  const uint32_t at[] = {4000};
  q.remove(at, 1);
  const uint32_t one[] = {7};
  TEST_ASSERT_TRUE(q.append(one, 1));  // the last one that fits
  TEST_ASSERT_EQUAL_UINT32(0, q.room());
  const uint32_t content = q.contentVersion(), position = q.positionVersion();
  const long allocs = Heap::allocs;
  TEST_ASSERT_FALSE(q.append(one, 1));
  TEST_ASSERT_FALSE(q.insertNext(one, 1));
  TEST_ASSERT_TRUE(q.append(one, 0));  // (nothing asked: nothing refused)
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_UINT32(content, q.contentVersion());
  TEST_ASSERT_EQUAL_UINT32(position, q.positionVersion());
  TEST_ASSERT_EQUAL_INT(allocs, Heap::allocs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Append), static_cast<int>(q.undoable()));
  // Moves, a toggle and a Play still work at the cap.
  const uint32_t sel[] = {4998, 4999};
  TEST_ASSERT_TRUE(q.moveNext(sel, 2));
  TEST_ASSERT_TRUE(q.setShuffled(true));
  TEST_ASSERT_TRUE(q.setShuffled(false));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  // Room again after a remove.
  q.remove(at, 1);
  TEST_ASSERT_EQUAL_UINT32(1, q.room());
  const uint32_t two[] = {1, 2};
  TEST_ASSERT_TRUE(q.insertNext(two, 2));
  TEST_ASSERT_EQUAL_UINT32(1, q.trackAt(11));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
}

// While shuffled, what fits keeps its place as any add does: Play next's
// right after the current entry's rank, + Queue's after the highest.
void test_shuffled_adds_at_the_cap() {
  QueueModel q;
  fill(q, 4995, 10);
  q.setShuffled(true);
  const std::vector<uint32_t> eight = range(8, 900000);
  TEST_ASSERT_TRUE(q.append(eight.data(), 8));
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  for (uint32_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_UINT32(900000 + i, q.trackAt(4995 + i));
  q.setShuffled(false);
  for (uint32_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_UINT32(900000 + i, q.trackAt(4995 + i));  // at the end
  // An add to an empty shuffled queue past the cap: its first 5,000, laid
  // out as a Play from its first.
  QueueModel e;
  e.setShuffled(true);
  const std::vector<uint32_t> many = range(6000);
  TEST_ASSERT_TRUE(e.insertNext(many.data(), 6000));
  TEST_ASSERT_EQUAL_UINT32(5000, e.size());
  TEST_ASSERT_EQUAL_UINT32(10, e.currentTrack());
  e.setShuffled(false);
  for (uint32_t i = 0; i < 5000; ++i) TEST_ASSERT_EQUAL_UINT32(10 + i, e.trackAt(i));
}

// A random run near the cap: no edit ever takes the queue past it, an add
// takes exactly min(n, room), and a refused one changes nothing.
void test_random_edits_never_pass_the_cap() {
  QueueModel q;
  fill(q, 4980, 2000);
  uint32_t x = 7;
  auto rnd = [&](uint32_t n) {
    x = x * 1664525u + 1013904223u;
    return n ? (x >> 8) % n : 0;
  };
  std::vector<uint32_t> ids;
  for (int it = 0; it < 4000; ++it) {
    const uint32_t op = rnd(6);
    ids.assign(rnd(30) + 1, 7);
    const uint32_t size = q.size(), room = q.room(), v = q.contentVersion();
    const uint32_t n = static_cast<uint32_t>(ids.size());
    if (op == 0 || op == 1) {
      const bool ok = op == 0 ? q.append(ids.data(), n) : q.insertNext(ids.data(), n);
      TEST_ASSERT_EQUAL(room > 0, ok);
      TEST_ASSERT_EQUAL_UINT32(size + std::min(n, room), q.size());
      if (!ok) TEST_ASSERT_EQUAL_UINT32(v, q.contentVersion());
    } else if (op == 2 && q.size() > 0) {
      std::vector<uint32_t> pos;
      for (uint32_t i = rnd(20) + 1; i > 0; --i) pos.push_back(rnd(q.size()));
      q.remove(pos.data(), static_cast<uint32_t>(pos.size()));
    } else if (op == 3) {
      q.undo();
    } else if (op == 4) {
      q.setShuffled(!q.shuffled());
    } else if (op == 5 && rnd(50) == 0) {
      const std::vector<uint32_t> big = range(5000 + rnd(3000));
      TEST_ASSERT_TRUE(q.replace(big.data(), static_cast<uint32_t>(big.size()), rnd(8000)));
      TEST_ASSERT_EQUAL_UINT32(5000, q.size());
    }
    TEST_ASSERT_TRUE(q.size() <= QueueModel::kMaxEntries);
    TEST_ASSERT_EQUAL_UINT32(QueueModel::kMaxEntries - q.size(), q.room());
  }
}

// ---- QueueText: the read's blocks, sized from the header ----

void test_text_read_is_sized_by_its_header() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  const uint32_t ids[] = {0, 1, 2, 3, 4, 5, TrackCatalog::kBuiltin + 1, 2};
  q.assign(ids, 8, 6);
  q.setShuffled(true);
  const std::string text = textOf(q, c, 4);
  // The read's two blocks (ids and ranks, 4 bytes a line) next to the
  // queue's own (12 an entry), and then only the queue's.
  QueueModel back(Meter::alloc, Meter::release);
  Meter::mark();
  MemorySource in(text.data(), text.size(), 5);
  TEST_ASSERT_TRUE(queuetext::read(in, c, back, nullptr, nullptr, Meter::alloc, Meter::release).ok);
  TEST_ASSERT_EQUAL_size_t(8 * 12 + (8 * 12 + 8 * 4 + 8 * 4), Meter::peak);
  TEST_ASSERT_EQUAL_size_t(8 * 12 * 2, Meter::live);
  // More lines than the header says: not a whole file, the queue left alone.
  const char* more = "mstream-queue 1 1 0 1\n/music/Root Track.flac\ntone:440\n";
  MemorySource m(more, std::strlen(more));
  TEST_ASSERT_FALSE(queuetext::read(m, c, back, nullptr, nullptr, Meter::alloc, Meter::release).ok);
  TEST_ASSERT_EQUAL_UINT32(8, back.size());
  // A header that claims two billion lines: the read asks for no more than
  // the cap's window (20 KB, not 8 GB), and the line count fails it: the
  // same, nothing kept.
  const char* huge = "mstream-queue 1 2000000000 0 1\n/music/Root Track.flac\n";
  MemorySource h(huge, std::strlen(huge));
  Meter::ceiling = 1 << 20;
  TEST_ASSERT_FALSE(queuetext::read(h, c, back, nullptr, nullptr, Meter::alloc, Meter::release).ok);
  TEST_ASSERT_EQUAL_UINT32(8, back.size());
  TEST_ASSERT_EQUAL_size_t(8 * 12 * 2, Meter::live);
}

// ---- the remap through queue.txt (QueueRemap: docs/METADATA.md 3.4.2, N3) ----

// The audio backend, for a player that stops, plays and pauses (the remap
// tells it what became of its entry).
class QuietBackend : public IAudioBackend {
public:
  std::string lastPath;
  int plays = 0;
  bool playing = false, paused = false;
  bool play(const std::string& p, uint32_t, uint32_t) override {
    lastPath = p;
    ++plays;
    playing = true;
    paused = false;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override { playing = paused = false; }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return 0; }
  bool finished() const override { return false; }
};

// A made-up library: 10 songs to an album, 10 albums to an artist; paths
// about as long as a real library's (75 bytes: 1.5 MB of text at 20,000).
std::string synthPath(uint32_t i) {
  char b[128];
  std::snprintf(b, sizeof(b), "/music/Made-up Artist %03u/A Made-up Album %02u/%02u - A Made-up Song %05u.mp3",
                static_cast<unsigned>(i / 100), static_cast<unsigned>(i / 10 % 10), static_cast<unsigned>(i % 10 + 1),
                static_cast<unsigned>(i));
  return b;
}

// Builds `idx` again in place, as Library::rebuild() does: the first `n`
// made-up paths but those in `gone`, and `extra` (which renumbers).
bool buildSynth(LibraryIndex& idx, uint32_t n, const std::set<std::string>& gone = {},
                const std::vector<std::string>& extra = {}) {
  idx.clear();
  if (!idx.begin("/music", n + static_cast<uint32_t>(extra.size()))) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const std::string p = synthPath(i);
    if (gone.count(p)) continue;
    if (idx.addFile(p.c_str()) != LibraryIndex::Add::Added) return false;
  }
  for (const std::string& p : extra) {
    if (idx.addFile(p.c_str()) != LibraryIndex::Add::Added) return false;
  }
  return idx.finish();
}

std::vector<std::string> paths(const QueueModel& q, const TrackCatalog& c) {
  std::vector<std::string> p;
  for (uint32_t i = 0; i < q.size(); ++i) p.push_back(pathOf(c, q.trackAt(i)));
  return p;
}

// The card, in memory: queue.txt is the MemStore's file. flush() is
// QueueStore's (the player's resume point to the saver, then its
// flushNow()), or false as a card that can't take the file; the rebuild
// is the test's. It notes what the queue held when the rebuild began and
// marks the meter when it ends, so Meter::peak is the re-read's.
struct MemCard : queueremap::Card {
  MemCard(QueueSaver& s, MemStore& m, PlaybackController& p, std::function<bool()> r)
      : saver(s), st(m), player(p), rebuildFn(std::move(r)) {}
  QueueSaver& saver;
  MemStore& st;
  PlaybackController& player;
  std::function<bool()> rebuildFn;
  bool cantTake = false;  // the card can't take the file (none, full)
  bool fileGone = false;  // ... nor give it back after the rebuild
  uint32_t now = 0;
  size_t liveAtRebuild = SIZE_MAX;
  std::unique_ptr<MemorySource> src;

  bool flush() override {
    if (cantTake) return false;
    QueueSaver::Transport t;
    t.have = player.resumePoint(&t.positionMs, &t.durationMs, &t.anchor);
    saver.noteTransport(t);
    return saver.flushNow(now);
  }
  ByteSource* openFile() override {
    if (fileGone) return nullptr;
    src.reset(new MemorySource(st.file.data(), st.file.size(), 61));  // (reads split mid-line)
    return src.get();
  }
  void closeFile() override { src.reset(); }
  bool rebuild() override {
    liveAtRebuild = Meter::live;
    const bool ok = rebuildFn();
    Meter::mark();
    return ok;
  }
};

// Runs the saver's passes until what it has to write is written (a pass
// first: it notices an edit only in one).
void settle(QueueSaver& saver, uint32_t from) {
  uint32_t t = from;
  do {
    saver.loop(t);
    t += 20;
  } while (t < from + 60000 && (saver.contentDirty() || saver.writing() || saver.busy()));
}

// A full queue (the cap, 5,000) of a library of 20,000, with its undo
// snapshot, across a rebuild, under a ceiling of what it already holds:
// the rebuild has all of its memory, and the re-read's peak is its two
// blocks, 16 bytes a line. The same queue through memory (a card that
// can't take the file) needs its text, about 0.38 MB: under that ceiling
// it can't come across, which is how the old remap ran out (at about
// 15,000 entries, before the cap).
void test_remap_of_a_full_queue_within_its_budget() {
  constexpr uint32_t kN = 20000, kQ = QueueModel::kMaxEntries;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  // The boot's default queue (queueEverything(): the library's first
  // 5,000), then an edit: 0.12 MB.
  const LibraryIndex::Span all = idx.allTracks();
  const std::vector<uint32_t> ids(all.ids, all.ids + all.count);
  TEST_ASSERT_TRUE(q.assign(ids.data(), kN, 3500));
  TEST_ASSERT_EQUAL_UINT32(kQ, q.size());
  saver.loaded(3, true, 0);
  const uint32_t first = 100;
  q.remove(&first, 1);
  TEST_ASSERT_EQUAL_size_t(2 * kQ * 12, Meter::live);
  const std::vector<std::string> before = paths(q, c);
  std::vector<uint32_t> beforeIds;
  for (uint32_t i = 0; i < q.size(); ++i) beforeIds.push_back(q.trackAt(i));
  const std::string playing = before[3499];
  const std::string gone = before[2000];

  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN, {gone}, {"/music/A Made-up Opener/01 - Intro.mp3"}); });
  card.now = 5000;
  Meter::ceiling = Meter::live + 4096;  // what PSRAM has left: the queue's own, and a little
  const queueremap::Result r = queueremap::run(q, saver, player, c, card, 5000, Meter::alloc, Meter::release);
  TEST_ASSERT_TRUE(r.rebuilt);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queueremap::Via::File), static_cast<int>(r.via));
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_FALSE(r.noLibrary);
  const size_t lines = kQ - 1, entries = kQ - 2;
  TEST_ASSERT_EQUAL_UINT32(lines, r.read.lines);
  TEST_ASSERT_EQUAL_UINT32(1, r.read.dropped);
  TEST_ASSERT_EQUAL_UINT32(0, r.read.capped);
  // The budget: everything to the rebuild; the re-read 16 bytes a line at
  // its peak (80 KB), then the queue exact (12 an entry, no snapshot).
  TEST_ASSERT_EQUAL_size_t(2 * kQ * 12, r.freedBytes);
  TEST_ASSERT_EQUAL_size_t(0, card.liveAtRebuild);
  TEST_ASSERT_EQUAL_size_t(lines * 4 + entries * 12, Meter::peak);
  TEST_ASSERT_EQUAL_size_t(entries * 12, Meter::live);
  TEST_ASSERT_EQUAL_size_t(entries * 12, q.memoryBytes());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  // What the old remap held across the rebuild: the text (and twice that
  // as it doubled). Several times the new peak.
  TEST_ASSERT_TRUE(st.file.size() > 350000);
  TEST_ASSERT_TRUE(Meter::peak * 4 < st.file.size());
  // The queue: the same paths in the same order, the one gone left out,
  // the same track current; the ids are the new library's.
  std::vector<std::string> want = before;
  want.erase(want.begin() + 2000);
  TEST_ASSERT_TRUE(paths(q, c) == want);
  TEST_ASSERT_TRUE(r.read.currentKept);
  TEST_ASSERT_EQUAL_STRING(playing.c_str(), pathOf(c, q.currentTrack()).c_str());
  uint32_t renumbered = 0;
  for (uint32_t i = 0; i < q.size(); ++i) renumbered += q.trackAt(i) != beforeIds[i < 2000 ? i : i + 1] ? 1 : 0;
  TEST_ASSERT_TRUE(renumbered > 0);
  // A track dropped: the file is written again 2 s later, a generation on.
  TEST_ASSERT_TRUE(saver.contentDirty());
  settle(saver, 5000);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 5).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_INT(q.current(), st.pos);
  TEST_ASSERT_EQUAL_UINT32(5, st.posGeneration);

  // The same through memory: its text doesn't fit. Cleared, and queue.txt
  // keeps the last queue saved (the next boot restores it).
  const uint32_t two = 2;
  q.remove(&two, 1);
  Meter::ceiling = Meter::live + 4096;
  const std::string file = st.file;
  const int commits = st.commits;
  card.cantTake = true;
  const queueremap::Result m = queueremap::run(q, saver, player, c, card, 70000, Meter::alloc, Meter::release);
  TEST_ASSERT_TRUE(m.rebuilt);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queueremap::Via::None), static_cast<int>(m.via));
  TEST_ASSERT_FALSE(m.read.ok);
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_size_t(0, Meter::live);
  TEST_ASSERT_FALSE(saver.contentDirty());
  for (uint32_t t = 70000; t < 90000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(commits, st.commits);
  TEST_ASSERT_EQUAL_STRING(file.c_str(), st.file.c_str());
}

// Shuffled: the play order and every rank come back (a gap where a track
// went), the mode too; off then lays out the own order without them.
void test_remap_keeps_a_shuffled_queue_and_its_ranks() {
  constexpr uint32_t kN = 300;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 40));
  q.setShuffled(true);
  saver.loaded(8, true, 0);
  const std::vector<std::string> order = paths(q, c);
  std::vector<uint32_t> ranks;
  for (uint32_t i = 0; i < q.size(); ++i) ranks.push_back(q.rankAt(i));
  const std::string current = pathOf(c, q.currentTrack());
  // One before the current entry and one after it go; one is new.
  const std::set<std::string> gone = {order[12], order[200]};

  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN, gone, {"/music/A Made-up Opener/01 - Intro.mp3"}); });
  Meter::ceiling = Meter::live + 4096;
  const queueremap::Result r = queueremap::run(q, saver, player, c, card, 100, Meter::alloc, Meter::release);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_EQUAL_UINT32(2, r.read.dropped);
  TEST_ASSERT_TRUE(r.read.header.shuffled);
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_size_t(0, card.liveAtRebuild);
  // Version 2's peak: the ids and the ranks, 8 bytes a line, then the queue.
  TEST_ASSERT_EQUAL_size_t(kN * 8 + (kN - 2) * 12, Meter::peak);
  TEST_ASSERT_EQUAL_size_t((kN - 2) * 12, Meter::live);
  // The play order and the ranks, less the two.
  std::vector<std::string> wantOrder;
  std::vector<uint32_t> wantRanks;
  for (uint32_t i = 0; i < order.size(); ++i) {
    if (gone.count(order[i])) continue;
    wantOrder.push_back(order[i]);
    wantRanks.push_back(ranks[i]);
  }
  TEST_ASSERT_TRUE(paths(q, c) == wantOrder);
  for (uint32_t i = 0; i < q.size(); ++i) TEST_ASSERT_EQUAL_UINT32(wantRanks[i], q.rankAt(i));
  TEST_ASSERT_TRUE(r.read.currentKept);
  TEST_ASSERT_EQUAL_STRING(current.c_str(), pathOf(c, q.currentTrack()).c_str());
  TEST_ASSERT_EQUAL_INT(39, q.current());  // one before it went
  // Off: the own order (the library's), the two left out, the same entry current.
  TEST_ASSERT_TRUE(q.setShuffled(false));
  std::vector<std::string> own;
  for (uint32_t i = 0; i < kN; ++i) {
    const std::string p = synthPath(i);
    if (!gone.count(p)) own.push_back(p);
  }
  TEST_ASSERT_TRUE(paths(q, c) == own);
  TEST_ASSERT_EQUAL_STRING(current.c_str(), pathOf(c, q.currentTrack()).c_str());
  // The file follows: written again (version 2 while it was shuffled).
  settle(saver, 100);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, saver.generation()).c_str(), st.file.c_str());
}

// The current track gone: the next entry that stayed is current (the last
// one when none after it did), it plays if the player was playing, and the
// file and the position follow. A rebuild that leaves no library keeps
// the file as it is and stops.
void test_remap_when_the_current_track_is_gone() {
  constexpr uint32_t kN = 60;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q;
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 0));
  saver.loaded(2, true, 0);
  player.play(20);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PlayState::Playing), static_cast<int>(player.state()));
  const std::vector<std::string> before = paths(q, c);
  TEST_ASSERT_EQUAL_STRING(before[20].c_str(), audio.lastPath.c_str());
  const int plays = audio.plays;

  std::set<std::string> gone = {before[20]};
  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN, gone); });
  queueremap::Result r = queueremap::run(q, saver, player, c, card, 100);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_FALSE(r.read.currentKept);
  TEST_ASSERT_EQUAL_INT(20, q.current());
  TEST_ASSERT_EQUAL_STRING(before[21].c_str(), pathOf(c, q.currentTrack()).c_str());
  // It was playing: the new current one starts.
  TEST_ASSERT_EQUAL_INT(plays + 1, audio.plays);
  TEST_ASSERT_EQUAL_STRING(before[21].c_str(), audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PlayState::Playing), static_cast<int>(player.state()));
  settle(saver, 100);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, saver.generation()).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_INT(20, st.pos);
  TEST_ASSERT_EQUAL_UINT32(saver.generation(), st.posGeneration);

  // The last ones gone, the current among them: the last one that stayed.
  player.play(57);
  gone.insert(before[57]);
  gone.insert(before[58]);
  gone.insert(before[59]);
  r = queueremap::run(q, saver, player, c, card, 70000);
  TEST_ASSERT_FALSE(r.read.currentKept);
  TEST_ASSERT_EQUAL_UINT32(56, q.size());
  TEST_ASSERT_EQUAL_INT(55, q.current());
  TEST_ASSERT_EQUAL_STRING(before[56].c_str(), pathOf(c, q.currentTrack()).c_str());
  settle(saver, 70000);

  // No library after it (the card went away): the built-in tracks stay,
  // the file keeps the library's for the next boot, and it stops.
  const uint32_t tones[] = {TrackCatalog::kBuiltin + 2, TrackCatalog::kBuiltin + 4};
  q.append(tones, 2);  // (the queue: 56 library tracks, then two tones)
  player.play(10);
  settle(saver, 140000);
  const std::string file = st.file;
  MemCard gone2(saver, st, player, [&] {
    idx.clear();
    return false;
  });
  r = queueremap::run(q, saver, player, c, gone2, 200000);
  TEST_ASSERT_FALSE(r.rebuilt);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_TRUE(r.noLibrary);
  TEST_ASSERT_EQUAL_UINT32(56, r.read.dropped);
  expectTracks(q, {TrackCatalog::kBuiltin + 2, TrackCatalog::kBuiltin + 4});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PlayState::Stopped), static_cast<int>(player.state()));
  TEST_ASSERT_FALSE(saver.contentDirty());
  for (uint32_t t = 200000; t < 220000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_STRING(file.c_str(), st.file.c_str());
}

// The resume point after a boot (the player's start point, NVS's resume
// point) goes across with its entry: kept as it was when nothing before it
// went; at its new line, a generation on, when something did; gone with
// the entry's track.
void test_remap_carries_the_resume_point() {
  constexpr uint32_t kN = 40;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q;
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  // As QueueStore::restore() leaves it: the file of generation 6, line 12
  // current, stopped 1:23 into it with an anchor.
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 12));
  st.file = wholeText(q, c, 6);
  const std::string path = pathOf(c, q.currentTrack());
  ResumeAnchor anchor;
  anchor.kind = ResumeAnchor::Kind::Mp3;
  anchor.exact = true;
  anchor.rate = 44100;
  anchor.sample = 83000ull * 441 / 10;
  anchor.fileSize = 4000000;
  anchor.frameByte = 1300000;
  anchor.prerollByte = 1299000;
  anchor.frameHash = 0x1234u;
  QueueResume saved;
  saved.valid = true;
  saved.generation = 6;
  saved.entry = 12;
  saved.pathHash = QueueSaver::pathHash(path.c_str());
  saved.positionMs = 83000;
  saved.durationMs = 240000;
  saved.anchor = anchor;
  st.resume = saved;
  saver.setGeneration(6);
  saver.loadedResume(saved);
  saver.loaded(6, false, 0);
  player.setStartPoint(83000, 240000, &anchor);

  // A rebuild that renumbers and drops nothing: the start point kept, and
  // nothing written (the file and the resume point are still this queue's).
  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN, {}, {"/music/A Made-up Opener/01 - Intro.mp3"}); });
  queueremap::Result r = queueremap::run(q, saver, player, c, card, 100);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_TRUE(r.startCarried);
  uint32_t ms = 0, dur = 0;
  ResumeAnchor a;
  TEST_ASSERT_TRUE(player.startPoint(&ms, &dur, &a));
  TEST_ASSERT_EQUAL_UINT32(83000, ms);
  TEST_ASSERT_EQUAL_UINT32(240000, dur);
  TEST_ASSERT_TRUE(a == anchor);
  TEST_ASSERT_EQUAL_STRING(path.c_str(), pathOf(c, q.currentTrack()).c_str());
  const int resumes = st.resumes, commits = st.commits;
  QueueSaver::Transport t;
  t.have = player.resumePoint(&t.positionMs, &t.durationMs, &t.anchor);
  saver.noteTransport(t);
  for (uint32_t now = 100; now < 20000; now += 100) saver.loop(now);
  TEST_ASSERT_EQUAL_INT(commits, st.commits);
  TEST_ASSERT_EQUAL_INT(resumes, st.resumes);
  TEST_ASSERT_EQUAL_UINT32(6, st.resume.generation);

  // Two tracks before it go: the same second on the same file, now line 10
  // of the next generation's file.
  const std::vector<std::string> before = paths(q, c);
  MemCard card2(saver, st, player, [&] { return buildSynth(idx, kN, {before[0], before[5]}); });
  r = queueremap::run(q, saver, player, c, card2, 30000);
  TEST_ASSERT_TRUE(r.startCarried);
  TEST_ASSERT_EQUAL_INT(10, q.current());
  TEST_ASSERT_TRUE(player.startPoint(&ms, &dur, &a));
  TEST_ASSERT_EQUAL_UINT32(83000, ms);
  t.have = player.resumePoint(&t.positionMs, &t.durationMs, &t.anchor);
  saver.noteTransport(t);
  settle(saver, 30000);
  TEST_ASSERT_EQUAL_UINT32(7, saver.generation());
  TEST_ASSERT_TRUE(st.resume.valid);
  TEST_ASSERT_EQUAL_UINT32(7, st.resume.generation);
  TEST_ASSERT_EQUAL_INT(10, st.resume.entry);
  TEST_ASSERT_EQUAL_UINT32(QueueSaver::pathHash(path.c_str()), st.resume.pathHash);
  TEST_ASSERT_EQUAL_UINT32(83000, st.resume.positionMs);
  TEST_ASSERT_TRUE(st.resume.anchor == anchor);
  // ... which is what the next boot pairs with its file.
  TEST_ASSERT_TRUE(QueueSaver::resumeApplies(st.resume, 7, 10, true, path.c_str()));

  // Its own track gone: no start point any more, and the resume point is
  // cleared at the saver's next pass.
  MemCard card3(saver, st, player, [&] { return buildSynth(idx, kN, {before[0], before[5], path}); });
  r = queueremap::run(q, saver, player, c, card3, 60000);
  TEST_ASSERT_FALSE(r.startCarried);
  TEST_ASSERT_FALSE(player.startPoint(&ms, &dur, &a));
  t.have = player.resumePoint(&t.positionMs, &t.durationMs, &t.anchor);
  TEST_ASSERT_FALSE(t.have);
  saver.noteTransport(t);
  saver.loop(60100);
  TEST_ASSERT_FALSE(st.resume.valid);
}

// The undo snapshot: given to the rebuild with the entries, never brought
// back (its keys are gone), and the first edit after takes one of the
// queue's exact size.
void test_remap_and_the_undo_snapshot() {
  constexpr uint32_t kN = 1000;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 500));
  saver.loaded(1, true, 0);
  const uint32_t at[] = {3, 4};
  q.remove(at, 2);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::Remove), static_cast<int>(q.undoable()));
  TEST_ASSERT_EQUAL_size_t(2 * kN * 12, Meter::live);
  // A write under way when the rebuild is asked for: the flush finishes it
  // (the file whole, nothing dropped) before anything is given back.
  saver.loop(10);
  saver.loop(2100);
  TEST_ASSERT_TRUE(saver.writing());

  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN, {}, {"/music/A Made-up Opener/01 - Intro.mp3"}); });
  card.now = 2120;
  const queueremap::Result r = queueremap::run(q, saver, player, c, card, 2120, Meter::alloc, Meter::release);
  TEST_ASSERT_EQUAL_INT(1, st.commits);
  TEST_ASSERT_EQUAL_INT(0, st.discards);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queueremap::Via::File), static_cast<int>(r.via));
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_EQUAL_size_t(2 * kN * 12, r.freedBytes);
  TEST_ASSERT_EQUAL_size_t(0, card.liveAtRebuild);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueModel::Edit::None), static_cast<int>(q.undoable()));
  TEST_ASSERT_FALSE(q.undo());
  TEST_ASSERT_EQUAL_UINT32(kN - 2, q.size());
  TEST_ASSERT_EQUAL_size_t((kN - 2) * 12, Meter::live);
  // The first edit after: a snapshot exactly the queue's size.
  const uint32_t first = 0;
  q.remove(&first, 1);
  TEST_ASSERT_EQUAL_size_t((kN - 2) * 12 * 2, Meter::live);
  TEST_ASSERT_TRUE(q.undo());
  TEST_ASSERT_EQUAL_UINT32(kN - 2, q.size());
}

// A card that can't take the file: the queue goes across as its text in
// memory, sized exactly, and the saver keeps at writing it.
void test_remap_through_memory_when_the_card_cant_take_the_file() {
  LibraryIndex idx;
  build(idx, {std::begin(kFiles), std::end(kFiles)});
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  st.file = "the last one";
  QueueSaver saver(st, q, c);
  const uint32_t ids[] = {3, 0, TrackCatalog::kBuiltin + 4, 5, 1};
  q.assign(ids, 5, 3);
  saver.loaded(4, true, 0);
  const std::vector<std::string> before = paths(q, c);
  const size_t text = wholeText(q, c, 4).size();  // (the saver's generation: the file it last had)
  // Another order of adding (other ids), and one file fewer that the queue
  // doesn't hold.
  MemCard card(saver, st, player, [&] {
    build(idx, {kFiles[5], kFiles[4], kFiles[3], kFiles[1], kFiles[0]});
    return true;
  });
  card.cantTake = true;
  const queueremap::Result r = queueremap::run(q, saver, player, c, card, 100, Meter::alloc, Meter::release);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queueremap::Via::Memory), static_cast<int>(r.via));
  TEST_ASSERT_EQUAL_size_t(text, r.textBytes);  // exactly its size
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_EQUAL_UINT32(5, r.read.lines);
  TEST_ASSERT_EQUAL_UINT32(0, r.read.dropped);
  TEST_ASSERT_TRUE(paths(q, c) == before);
  TEST_ASSERT_EQUAL_INT(3, q.current());
  TEST_ASSERT_EQUAL_size_t(5 * 12, Meter::live);  // the text given back
  // The file doesn't hold it: still to write.
  TEST_ASSERT_TRUE(saver.contentDirty());
  TEST_ASSERT_EQUAL_STRING("the last one", st.file.c_str());
  settle(saver, 100);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 5).c_str(), st.file.c_str());
}

// ---- the file holding more than the queue (review of N3) ----

// After a rebuild that left no library, the queue is the built-in tracks
// and queue.txt keeps the whole queue. The listener plays the second tone:
// a move of that queue, not of the file's lines, so the file's line (NVS)
// stays at the library track that played. The next rebuild brings the
// library back and reads the whole file from that line, as the next boot
// would: the queue is back where it was, what plays is its current entry,
// and the start point and NVS agree with it.
void test_remap_after_a_rebuild_with_no_library() {
  constexpr uint32_t kN = 60;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q;
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 0));
  const uint32_t tones[] = {TrackCatalog::kBuiltin + 2, TrackCatalog::kBuiltin + 4};
  q.append(tones, 2);  // lines 60 and 61
  saver.loaded(2, true, 0);
  settle(saver, 0);
  player.play(10);
  settle(saver, 10000);
  const std::string file = st.file;
  const std::vector<std::string> whole = paths(q, c);
  TEST_ASSERT_EQUAL_INT(10, st.pos);

  MemCard none(saver, st, player, [&] {
    idx.clear();
    return false;
  });
  queueremap::Result r = queueremap::run(q, saver, player, c, none, 20000);
  TEST_ASSERT_TRUE(r.noLibrary);
  TEST_ASSERT_EQUAL_UINT32(2, q.size());
  TEST_ASSERT_FALSE(saver.fileIsQueue());
  TEST_ASSERT_EQUAL_INT(10, saver.fileLine());
  // The second tone plays (the queue's entry 1, the file's line 61).
  player.play(1);
  TEST_ASSERT_EQUAL_STRING(pathOf(c, q.currentTrack()).c_str(), audio.lastPath.c_str());
  for (uint32_t t = 20000; t < 30000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(10, st.pos);  // not 1: that's library track 1's line
  TEST_ASSERT_EQUAL_STRING(file.c_str(), st.file.c_str());

  MemCard back(saver, st, player, [&] { return buildSynth(idx, kN); });
  r = queueremap::run(q, saver, player, c, back, 40000);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_FALSE(r.noLibrary);
  TEST_ASSERT_TRUE(paths(q, c) == whole);
  TEST_ASSERT_EQUAL_INT(10, q.current());
  // Not the tone any more: playing, the queue's current entry starts.
  TEST_ASSERT_FALSE(r.read.currentKept);
  TEST_ASSERT_EQUAL_STRING(whole[10].c_str(), pathOf(c, q.currentTrack()).c_str());
  TEST_ASSERT_EQUAL_STRING(pathOf(c, q.currentTrack()).c_str(), audio.lastPath.c_str());
  TEST_ASSERT_TRUE(saver.fileIsQueue());
  settle(saver, 40000);
  TEST_ASSERT_EQUAL_STRING(file.c_str(), st.file.c_str());  // the same queue: not written again
  TEST_ASSERT_EQUAL_INT(10, st.pos);
  TEST_ASSERT_EQUAL_UINT32(saver.generation(), st.posGeneration);
}

// "Try again" after a boot with no library: restore() left the queue empty
// and the file whole, with its line from NVS (25, not the header's 0).
// The rebuild reads the whole queue back at that line, as the boot would
// have, and NVS still agrees.
void test_remap_after_a_boot_with_no_library() {
  constexpr uint32_t kN = 40;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q;
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 0));
  st.file = wholeText(q, c, 6);  // its header: line 0
  const std::vector<std::string> whole = paths(q, c);
  st.posGeneration = 6;  // a move saved since: line 25
  st.pos = 25;
  // The boot: no library this time. QueueStore::restore() reads the file
  // with the position from NVS, and keeps the file.
  idx.clear();
  struct Nvs {
    uint32_t generation;
    int32_t position;
  } nvs{6, 25};
  MemorySource in(st.file.data(), st.file.size());
  const queuetext::Restored b = queuetext::read(
      in, c, q,
      [](const queuetext::Header& h, void* ctx) {
        const Nvs& n = *static_cast<const Nvs*>(ctx);
        return n.generation == h.generation ? n.position : h.current;
      },
      &nvs);
  TEST_ASSERT_TRUE(b.ok);
  TEST_ASSERT_EQUAL_UINT32(0, q.size());
  saver.setGeneration(6);
  saver.keptFile(6, 25);
  player.queueReplaced(b.currentKept);
  for (uint32_t t = 0; t < 5000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(25, st.pos);

  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN); });
  const queueremap::Result r = queueremap::run(q, saver, player, c, card, 10000);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_TRUE(paths(q, c) == whole);
  TEST_ASSERT_EQUAL_INT(25, q.current());
  TEST_ASSERT_EQUAL_STRING(whole[25].c_str(), pathOf(c, q.currentTrack()).c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PlayState::Stopped), static_cast<int>(player.state()));
  for (uint32_t t = 10000; t < 20000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(25, st.pos);
  TEST_ASSERT_EQUAL_UINT32(6, st.posGeneration);
  TEST_ASSERT_EQUAL_INT(0, st.commits);  // the file was already this queue
}

// A queue that couldn't come back (the file gone during the rebuild) is
// cleared, and the file keeps the last queue saved with its line (30). The
// next rebuild that finds the file brings it back at that line, which is
// where NVS says the next boot would start.
void test_remap_after_a_cleared_queue() {
  constexpr uint32_t kN = 40;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q;
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  const LibraryIndex::Span all = idx.allTracks();
  TEST_ASSERT_TRUE(q.assign(all.ids, all.count, 0));
  saver.loaded(2, true, 0);
  settle(saver, 0);
  q.setCurrent(30);
  for (uint32_t t = 10000; t < 15000; t += 100) saver.loop(t);
  TEST_ASSERT_EQUAL_INT(30, st.pos);
  const std::vector<std::string> whole = paths(q, c);

  MemCard gone(saver, st, player, [&] { return buildSynth(idx, kN); });
  gone.fileGone = true;
  queueremap::Result r = queueremap::run(q, saver, player, c, gone, 20000);
  TEST_ASSERT_FALSE(r.read.ok);
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_FALSE(saver.fileIsQueue());
  TEST_ASSERT_EQUAL_INT(30, saver.fileLine());

  MemCard back(saver, st, player, [&] { return buildSynth(idx, kN); });
  r = queueremap::run(q, saver, player, c, back, 30000);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_TRUE(paths(q, c) == whole);
  TEST_ASSERT_EQUAL_INT(30, q.current());
  TEST_ASSERT_EQUAL_INT(st.pos, q.current());
}

// ---- the cap and the queue file: a file longer than the queue holds ----

// A queue file of the made-up library's first `n` paths, as a firmware
// from before the cap wrote it (version 2 with `ranks`).
std::string longFile(uint32_t n, int32_t current, uint32_t generation, const std::vector<uint32_t>* ranks = nullptr) {
  std::string s = "mstream-queue " + std::string(ranks ? "2 " : "1 ") + std::to_string(n) + " " +
                  std::to_string(current) + " " + std::to_string(generation) + "\n";
  for (uint32_t i = 0; i < n; ++i) {
    if (ranks) s += std::to_string((*ranks)[i]) + " ";
    s += synthPath(i) + "\n";
  }
  return s;
}

// Read in as the cap's window: the first 5,000 lines when the current one
// is among them, else from it on; every line still checked; the blocks
// the window's size, whatever the header says.
void test_a_longer_file_reads_in_its_window() {
  constexpr uint32_t kN = 6000;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN, {synthPath(5200)}));  // one track gone
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  // At line 5,500: lines 1,000-5,999 read in, 5,200 dropped.
  std::string text = longFile(kN, 5500, 8);
  Meter::mark();
  MemorySource in(text.data(), text.size(), 97);
  queuetext::Restored r = queuetext::read(in, c, q, nullptr, nullptr, Meter::alloc, Meter::release);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(kN, r.lines);
  TEST_ASSERT_EQUAL_UINT32(1000, r.first);
  TEST_ASSERT_EQUAL_UINT32(1000, r.capped);
  TEST_ASSERT_EQUAL_UINT32(1, r.dropped);
  TEST_ASSERT_EQUAL_UINT32(4999, r.entries);
  TEST_ASSERT_EQUAL_UINT32(r.lines, r.entries + r.dropped + r.capped);
  TEST_ASSERT_TRUE(r.currentKept);
  TEST_ASSERT_EQUAL_INT(4499, q.current());
  TEST_ASSERT_EQUAL_STRING(synthPath(5500).c_str(), pathOf(c, q.currentTrack()).c_str());
  TEST_ASSERT_EQUAL_STRING(synthPath(1000).c_str(), pathOf(c, q.trackAt(0)).c_str());
  TEST_ASSERT_EQUAL_STRING(synthPath(5999).c_str(), pathOf(c, q.trackAt(4998)).c_str());
  TEST_ASSERT_EQUAL_size_t(5000 * 4 + 4999 * 12, Meter::peak);  // the window's block, not the file's
  TEST_ASSERT_EQUAL_size_t(4999 * 12, Meter::live);
  // At line 10 (a position from NVS overriding the header's): the first 5,000.
  MemorySource early(text.data(), text.size());
  r = queuetext::read(early, c, q, [](const queuetext::Header&, void*) { return int32_t{10}; }, nullptr);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(0, r.first);
  TEST_ASSERT_EQUAL_UINT32(1000, r.capped);
  TEST_ASSERT_EQUAL_UINT32(5000, r.entries);
  TEST_ASSERT_EQUAL_INT(10, q.current());
  // Shuffled (version 2): the window's ranks come with it.
  std::vector<uint32_t> ranks(kN);
  for (uint32_t i = 0; i < kN; ++i) ranks[i] = (i * 7919u) % kN;  // a permutation of 0 .. 5,999
  text = longFile(kN, 5999, 8, &ranks);
  MemorySource v2(text.data(), text.size(), 61);
  r = queuetext::read(v2, c, q);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(q.shuffled());
  TEST_ASSERT_EQUAL_UINT32(1000, r.first);
  TEST_ASSERT_EQUAL_UINT32(4999, q.size());  // (5,200's line dropped)
  TEST_ASSERT_EQUAL_INT(4998, q.current());
  TEST_ASSERT_EQUAL_UINT32(ranks[1000], q.rankAt(0));
  TEST_ASSERT_EQUAL_UINT32(ranks[5999], q.rankAt(4998));
  // A bad line outside the window: not a whole file, the queue left alone.
  const uint32_t before = q.contentVersion();
  const size_t cut = text.find('\n' + std::to_string(ranks[100]) + " ");
  TEST_ASSERT_TRUE(cut != std::string::npos);
  text.replace(cut + 1, std::to_string(ranks[100]).size() + 1, "x");
  MemorySource bad(text.data(), text.size());
  TEST_ASSERT_FALSE(queuetext::read(bad, c, q).ok);
  TEST_ASSERT_EQUAL_UINT32(before, q.contentVersion());
}

// A boot that finds an older firmware's longer queue.txt (QueueStore::
// restore()'s steps): the window, the paused second kept for its entry,
// then the file written again at the queue's size, a generation on, with
// the position and the resume point paired at the entry's new line.
void test_an_older_longer_queue_at_boot_is_written_again() {
  constexpr uint32_t kN = 6000;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  TrackCatalog c(&idx);
  QueueModel q;
  MemStore st;
  st.file = longFile(kN, 5500, 8);
  QueueResume resume;
  resume.valid = true;
  resume.generation = 8;
  resume.entry = 5500;
  resume.pathHash = QueueSaver::pathHash(synthPath(5500).c_str());
  resume.positionMs = 83000;
  QueueSaver saver(st, q, c);
  saver.setGeneration(8);
  saver.loadedResume(resume);
  MemorySource in(st.file.data(), st.file.size());
  const queuetext::Restored r = queuetext::read(in, c, q);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(4500, q.current());
  // The resume point is the file's line's: it applies.
  TEST_ASSERT_TRUE(QueueSaver::resumeApplies(resume, 8, 5500, r.currentKept, pathOf(c, q.currentTrack()).c_str()));
  saver.loaded(8, r.dropped > 0 || r.capped > 0, 0);
  TEST_ASSERT_TRUE(saver.contentDirty());
  saver.noteTransport(pausedAt(83000));
  settle(saver, 0);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 9).c_str(), st.file.c_str());
  TEST_ASSERT_TRUE(st.file.rfind("mstream-queue 1 5000 4500 9\n", 0) == 0);
  TEST_ASSERT_EQUAL_INT(4500, st.pos);
  TEST_ASSERT_EQUAL_UINT32(9, st.posGeneration);
  TEST_ASSERT_TRUE(st.resume.valid);
  TEST_ASSERT_EQUAL_INT(4500, st.resume.entry);
  TEST_ASSERT_EQUAL_UINT32(9, st.resume.generation);
  TEST_ASSERT_EQUAL_UINT32(83000, st.resume.positionMs);
}

// A boot with no library kept an older firmware's longer file whole
// (keptFile(), its line 5,500); the rebuild that brings the library back
// reads it in as its window from that line, and writes it again.
void test_remap_reads_a_kept_longer_file_in_its_window() {
  constexpr uint32_t kN = 6000;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(buildSynth(idx, kN));
  idx.clear();  // the boot found no library
  TrackCatalog c(&idx);
  QueueModel q(Meter::alloc, Meter::release);
  QuietBackend audio;
  PlaybackController player(audio, q, c);
  MemStore st;
  QueueSaver saver(st, q, c);
  st.file = longFile(kN, 5500, 8);
  MemorySource in(st.file.data(), st.file.size());
  const queuetext::Restored b = queuetext::read(in, c, q);  // no library: every line dropped
  TEST_ASSERT_TRUE(b.ok);
  TEST_ASSERT_EQUAL_UINT32(0, q.size());
  saver.setGeneration(8);
  saver.keptFile(8, 5500);
  player.queueReplaced(false);

  MemCard card(saver, st, player, [&] { return buildSynth(idx, kN); });
  Meter::mark();
  const queueremap::Result r = queueremap::run(q, saver, player, c, card, 10000, Meter::alloc, Meter::release);
  TEST_ASSERT_TRUE(r.read.ok);
  TEST_ASSERT_FALSE(r.noLibrary);
  TEST_ASSERT_EQUAL_UINT32(1000, r.read.capped);
  TEST_ASSERT_EQUAL_UINT32(5000, q.size());
  TEST_ASSERT_EQUAL_INT(4500, q.current());
  TEST_ASSERT_EQUAL_STRING(synthPath(5500).c_str(), pathOf(c, q.currentTrack()).c_str());
  TEST_ASSERT_TRUE(Meter::peak <= 5000 * 4 + 5000 * 12);
  TEST_ASSERT_TRUE(saver.fileIsQueue());
  TEST_ASSERT_TRUE(saver.contentDirty());  // written again, at the queue's size
  settle(saver, 10000);
  TEST_ASSERT_EQUAL_STRING(wholeText(q, c, 9).c_str(), st.file.c_str());
  TEST_ASSERT_EQUAL_INT(4500, st.pos);
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
  RUN_TEST(test_a_play_that_sets_the_mode_undoes_it_too);
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
  RUN_TEST(test_saver_abort_and_kept_file);
  RUN_TEST(test_resume_point_saved_at_a_pause_and_cleared_when_it_plays);
  RUN_TEST(test_resume_point_waits_for_the_file_and_follows_its_entry);
  RUN_TEST(test_flush_now_saves_the_resume_point);
  RUN_TEST(test_resume_point_at_boot);
  RUN_TEST(test_resume_anchor_saved_with_the_point);
  RUN_TEST(test_a_toggle_rewrites_the_file);
  RUN_TEST(test_a_toggle_during_a_write_restarts_it);
  RUN_TEST(test_shuffle_alls_undo_writes_version_1_again);
  RUN_TEST(test_off_while_paused_pairs_the_resume_point_again);
  RUN_TEST(test_assign_is_exact_and_growth_is_bounded);
  RUN_TEST(test_release_gives_everything_back);
  RUN_TEST(test_window_holds_the_current_entry);
  RUN_TEST(test_assign_past_the_cap_keeps_the_window);
  RUN_TEST(test_play_past_the_cap_in_order);
  RUN_TEST(test_shuffled_play_past_the_cap_takes_a_random_5000);
  RUN_TEST(test_sample_is_exact_and_uniform);
  RUN_TEST(test_an_add_takes_what_fits);
  RUN_TEST(test_a_full_queue_refuses_an_add);
  RUN_TEST(test_shuffled_adds_at_the_cap);
  RUN_TEST(test_random_edits_never_pass_the_cap);
  RUN_TEST(test_text_read_is_sized_by_its_header);
  RUN_TEST(test_remap_of_a_full_queue_within_its_budget);
  RUN_TEST(test_remap_keeps_a_shuffled_queue_and_its_ranks);
  RUN_TEST(test_remap_when_the_current_track_is_gone);
  RUN_TEST(test_remap_carries_the_resume_point);
  RUN_TEST(test_remap_and_the_undo_snapshot);
  RUN_TEST(test_remap_through_memory_when_the_card_cant_take_the_file);
  RUN_TEST(test_remap_after_a_rebuild_with_no_library);
  RUN_TEST(test_remap_after_a_boot_with_no_library);
  RUN_TEST(test_remap_after_a_cleared_queue);
  RUN_TEST(test_a_longer_file_reads_in_its_window);
  RUN_TEST(test_an_older_longer_queue_at_boot_is_written_again);
  RUN_TEST(test_remap_reads_a_kept_longer_file_in_its_window);
  return UNITY_END();
}
