// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the Queue screen's portable pieces (QueueView): the
// "12 up next · 49 min" summary from learned track lengths, the mark on
// what a Library add put in the queue, the failed-track ring, when an
// Undo toast goes because the undo went from under it (UndoWatch), and the
// texts of the queue's cap. (The shuffle's loop moved to
// lib/core/Shuffle.h: test_queue.)
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "QueueModel.h"
#include "QueueView.h"

using namespace queueview;

void setUp() {}
void tearDown() {}

namespace {

void fill(QueueModel& q, uint32_t n, int32_t current) {
  std::vector<uint32_t> ids(n);
  for (uint32_t i = 0; i < n; ++i) ids[i] = i;
  q.assign(ids.data(), n, current);
}

uint32_t builtinHint(void*, uint32_t id) { return id >= 0x80000000u ? 60000 : 0; }

}  // namespace

void test_durations_are_learned_per_track() {
  DurationBook b;
  TEST_ASSERT_TRUE(b.reset(10));
  TEST_ASSERT_EQUAL_UINT32(0, b.seconds(3));
  const uint32_t v = b.version();
  b.note(3, 200400);  // 3:20.4
  TEST_ASSERT_EQUAL_UINT32(200, b.seconds(3));
  TEST_ASSERT_NOT_EQUAL(v, b.version());
  const uint32_t v2 = b.version();
  b.note(3, 200100);  // the same second: nothing changed
  TEST_ASSERT_EQUAL_UINT32(v2, b.version());
  b.note(99, 1000);   // outside the book: ignored
  TEST_ASSERT_EQUAL_UINT32(0, b.seconds(99));
  b.note(4, 100 * 3600 * 1000u);  // capped
  TEST_ASSERT_EQUAL_UINT32(65535, b.seconds(4));
  TEST_ASSERT_TRUE(b.reset(5));   // a rebuilt library: all unknown again
  TEST_ASSERT_EQUAL_UINT32(0, b.seconds(3));
}

void test_up_next_time_adds_what_is_known() {
  QueueModel q;
  fill(q, 6, 1);  // entries 2..5 are up next
  DurationBook b;
  b.reset(6);
  b.note(1, 999000);  // the current one: not counted
  b.note(2, 180000);
  b.note(4, 240000);
  Time t = upNextTime(q, b, nullptr, nullptr);
  TEST_ASSERT_EQUAL_UINT32(420, t.knownS);
  TEST_ASSERT_EQUAL_UINT32(2, t.unknown);
  // The built-in tracks' hints count as known.
  QueueModel r;
  const uint32_t ids[] = {0, 0x80000000u, 0x80000001u};
  r.assign(ids, 3, 0);
  t = upNextTime(r, b, builtinHint, nullptr);
  TEST_ASSERT_EQUAL_UINT32(120, t.knownS);
  TEST_ASSERT_EQUAL_UINT32(0, t.unknown);
}

void test_summary_texts() {
  char buf[64];
  Time t;
  t.knownS = 49 * 60 - 30;
  TEST_ASSERT_EQUAL_STRING("12 up next \xC2\xB7 49 min", summary(12, t, 0, 16, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("4 of 16 \xC2\xB7 12 up next \xC2\xB7 49 min", summary(12, t, 4, 16, buf, sizeof(buf)));
  t.unknown = 3;
  TEST_ASSERT_EQUAL_STRING("12 up next \xC2\xB7 49+ min", summary(12, t, 0, 16, buf, sizeof(buf)));
  t = Time{};
  t.unknown = 12;
  TEST_ASSERT_EQUAL_STRING("12 up next", summary(12, t, 0, 16, buf, sizeof(buf)));
  t.knownS = 20;
  t.unknown = 0;
  TEST_ASSERT_EQUAL_STRING("1 up next \xC2\xB7 1 min", summary(1, t, 0, 2, buf, sizeof(buf)));
  t.knownS = 125 * 60;
  TEST_ASSERT_EQUAL_STRING("30 up next \xC2\xB7 2 h 5 min", summary(30, t, 0, 31, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("Nothing up next", summary(0, t, 0, 1, buf, sizeof(buf)));
  // Without the count (the header's form when the long one is too wide):
  // the position stays.
  t = Time{};
  t.knownS = 49 * 60 - 30;
  TEST_ASSERT_EQUAL_STRING("4 of 16 \xC2\xB7 49 min", summary(12, t, 4, 16, buf, sizeof(buf), false));
  t.unknown = 2;
  TEST_ASSERT_EQUAL_STRING("4 of 16 \xC2\xB7 49+ min", summary(12, t, 4, 16, buf, sizeof(buf), false));
  t = Time{};
  TEST_ASSERT_EQUAL_STRING("4 of 16", summary(12, t, 4, 16, buf, sizeof(buf), false));
  t.knownS = 600;
  TEST_ASSERT_EQUAL_STRING("16 of 16", summary(0, t, 16, 16, buf, sizeof(buf), false));
  // A small buffer is cut, never overrun.
  char tiny[8];
  summary(12, t, 4, 16, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_size_t(7, strlen(tiny));
  summary(12, t, 1234, 5678, tiny, sizeof(tiny), false);
  TEST_ASSERT_EQUAL_size_t(7, strlen(tiny));
}

// Play next and + Queue from the Library: the next visit finds and
// highlights everything added since the last one.
void test_added_mark_finds_what_was_added() {
  QueueModel q;
  fill(q, 5, 1);
  AddedMark m;
  TEST_ASSERT_FALSE(m.pending());
  TEST_ASSERT_EQUAL_UINT32(AddedMark::kNone, m.firstPosition(q));
  // + Queue: two at the end.
  const uint32_t two[] = {10, 11};
  const uint32_t endAt = q.size();
  q.append(two, 2);
  m.noteAdded(q.keyAt(endAt));
  // Play next: one right after the current entry.
  const uint32_t one[] = {12};
  q.insertNext(one, 1);
  m.noteAdded(q.keyAt(2));
  TEST_ASSERT_TRUE(m.pending());
  TEST_ASSERT_EQUAL_UINT32(2, m.firstPosition(q));  // the Play next one comes first now
  int marked = 0;
  for (uint32_t p = 0; p < q.size(); ++p) marked += m.marks(q.keyAt(p)) ? 1 : 0;
  TEST_ASSERT_EQUAL_INT(3, marked);
  TEST_ASSERT_FALSE(m.marks(q.keyAt(0)));
  // Undone and removed: nothing to show.
  const uint32_t at[] = {2, 6, 7};
  q.remove(at, 3);
  TEST_ASSERT_EQUAL_UINT32(AddedMark::kNone, m.firstPosition(q));
  m.clear();
  TEST_ASSERT_FALSE(m.pending());
}

void test_failed_keys_ring() {
  KeyRing r;
  r.add(5);
  r.add(5);
  TEST_ASSERT_EQUAL_INT(1, r.count());
  TEST_ASSERT_TRUE(r.has(5));
  for (uint32_t k = 100; k < 100 + KeyRing::kSize; ++k) r.add(k);
  TEST_ASSERT_EQUAL_INT(KeyRing::kSize, r.count());
  TEST_ASSERT_FALSE(r.has(5));  // the oldest went
  TEST_ASSERT_TRUE(r.has(100));
  TEST_ASSERT_TRUE(r.has(100 + KeyRing::kSize - 1));
  r.add(QueueModel::kNone);
  TEST_ASSERT_FALSE(r.has(QueueModel::kNone));
}

// Ui's pass, as Ui::loop makes it: the mode, an Undo toast up, the undo.
static UndoWatch::Gone pass(UndoWatch& w, const QueueModel& q, bool undoToast) {
  return w.pass(q.shuffled(), undoToast, q.undoable());
}

// A shuffle toggle drops the undo a toast offers (docs/QUEUE-MODES.md
// 2.6): the toast goes, "a shuffle toggle took the undo".
void test_undo_watch_a_toggle_takes_the_undo() {
  QueueModel q;
  fill(q, 5, 1);
  UndoWatch w;
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, false));
  const uint32_t one[] = {12};
  TEST_ASSERT_TRUE(q.insertNext(one, 1));  // Play next, its toast with Undo
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, true));
  TEST_ASSERT_FALSE(w.modeChanged());
  TEST_ASSERT_TRUE(q.setShuffled(true));
  TEST_ASSERT_EQUAL(QueueModel::Edit::None, q.undoable());
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Toggle, pass(w, q, true));
  TEST_ASSERT_TRUE(w.modeChanged());  // (no badge flash)
  // The toast gone, the next pass sees no change.
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, false));
  TEST_ASSERT_FALSE(w.modeChanged());
  // A toggle with no Undo toast up: nothing to take away.
  TEST_ASSERT_TRUE(q.setShuffled(false));
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, false));
  TEST_ASSERT_TRUE(w.modeChanged());
}

// The review's case: shuffle off, an empty queue, Shuffle all (the mode
// set inside the Play: its toast stays), then the console's qu within the
// toast's 4 s. The undo puts the mode back, a change of mode that is no
// toggle: the toast goes, "undone from the console".
void test_undo_watch_qu_of_shuffle_all_is_no_toggle() {
  QueueModel q;
  UndoWatch w;
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, false));
  const uint32_t ids[] = {0, 1, 2, 3, 4, 5, 6, 7};
  TEST_ASSERT_TRUE(q.replace(ids, 8, QueueModel::kAnyStart, /*shuffled=*/true));
  TEST_ASSERT_TRUE(q.shuffled());
  // "Shuffling 8 tracks" with Undo: the mode changed, the undo is there.
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, true));
  TEST_ASSERT_TRUE(w.modeChanged());
  // qu.
  TEST_ASSERT_TRUE(q.undo());
  w.undone();
  TEST_ASSERT_FALSE(q.shuffled());
  TEST_ASSERT_EQUAL(QueueModel::Edit::None, q.undoable());
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Undone, pass(w, q, true));
  TEST_ASSERT_TRUE(w.modeChanged());
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, false));
}

// qu of an edit that kept the mode (Play next) takes its toast away too;
// the toast's own Undo replaces it ("Undone", no Undo), and qu's word is
// told once: a later toast's undo taken by a toggle reads as a toggle.
void test_undo_watch_qu_of_any_edit() {
  QueueModel q;
  fill(q, 5, 1);
  UndoWatch w;
  const uint32_t one[] = {12};
  TEST_ASSERT_TRUE(q.insertNext(one, 1));
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, true));
  TEST_ASSERT_TRUE(q.undo());
  w.undone();
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Undone, pass(w, q, true));
  TEST_ASSERT_FALSE(w.modeChanged());
  // qu with no Undo toast up (it had gone): nothing to do, and the word goes.
  TEST_ASSERT_TRUE(q.insertNext(one, 1));
  TEST_ASSERT_TRUE(q.undo());
  w.undone();
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, false));
  TEST_ASSERT_TRUE(q.insertNext(one, 1));
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, true));
  TEST_ASSERT_TRUE(q.setShuffled(true));
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Toggle, pass(w, q, true));
  // An undo that is still there keeps its toast, whatever was said.
  TEST_ASSERT_TRUE(q.insertNext(one, 1));
  w.undone();
  TEST_ASSERT_EQUAL(UndoWatch::Gone::Stays, pass(w, q, true));
}

// The queue's cap (QueueModel::kMaxEntries, docs/QUEUE-MODES.md 15): its
// counts grouped by thousands, and the toast of what it cut short.
void test_capped_texts() {
  char b[96];
  TEST_ASSERT_EQUAL_STRING("0", grouped(0, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("999", grouped(999, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("1,000", grouped(1000, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("19,412", grouped(19412, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("100,000", grouped(100000, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("4,294,967,295", grouped(4294967295u, b, sizeof(b)));
  char small[4];
  TEST_ASSERT_EQUAL_STRING("1,0", grouped(1000, small, sizeof(small)));  // cut, never past the buffer
  TEST_ASSERT_EQUAL_STRING("Shuffling 5,000 of 19,412: the queue holds 5,000 tracks",
                           cappedText(Capped::Shuffle, 5000, 19412, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Playing 5,000 of 6,021: the queue holds 5,000 tracks",
                           cappedText(Capped::Play, 5000, 6021, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Added 37 of 300: the queue holds 5,000 tracks",
                           cappedText(Capped::Add, 37, 300, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("10 of 12 play next: the queue holds 5,000 tracks",
                           cappedText(Capped::Next, 10, 12, b, sizeof(b)));
  char tiny[12];
  TEST_ASSERT_EQUAL_STRING("Added 37 of", cappedText(Capped::Add, 37, 300, tiny, sizeof(tiny)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_capped_texts);
  RUN_TEST(test_durations_are_learned_per_track);
  RUN_TEST(test_up_next_time_adds_what_is_known);
  RUN_TEST(test_summary_texts);
  RUN_TEST(test_added_mark_finds_what_was_added);
  RUN_TEST(test_failed_keys_ring);
  RUN_TEST(test_undo_watch_a_toggle_takes_the_undo);
  RUN_TEST(test_undo_watch_qu_of_shuffle_all_is_no_toggle);
  RUN_TEST(test_undo_watch_qu_of_any_edit);
  return UNITY_END();
}
