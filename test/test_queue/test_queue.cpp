// Host tests for the play queue: QueueModel (edits, positions, keys, undo),
// TrackCatalog (library and built-in ids) and QueueText (the queue saved as
// paths, and read back after a library rebuild). Run: pio test -e native
#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ByteStream.h"
#include "LibraryIndex.h"
#include "QueueModel.h"
#include "QueueText.h"
#include "TrackCatalog.h"

namespace {

// An allocator that can be told to fail, and counts what's live.
struct Heap {
  static long live;
  static bool failing;
  static void* alloc(size_t n) {
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
  TEST_ASSERT_FALSE(c.valid(TrackCatalog::kBuiltin + 9));
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
  RUN_TEST(test_memory_from_the_hooks_and_out_of_memory);
  RUN_TEST(test_an_edit_without_memory_for_its_snapshot_is_not_undoable);
  RUN_TEST(test_random_edits_match_a_simple_model);
  RUN_TEST(test_catalog_library_and_builtin_ids);
  RUN_TEST(test_text_round_trip);
  RUN_TEST(test_text_remaps_after_a_rebuild);
  RUN_TEST(test_text_current_override_and_bad_files);
  RUN_TEST(test_text_writer_in_steps);
  return UNITY_END();
}
