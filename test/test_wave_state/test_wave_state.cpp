// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for what Now Playing wants of the waveform index (lib/core/
// WaveState, docs/WAVEFORM-INDEX.md 11): a want and the same hash again, a hash
// of 0; each answer in its place (a shape, none, the file off, a read failed
// twice, a file that didn't open); a burst of wants while a job runs; a remount
// before, during and after a job; the worker refusing; the retry's wait across
// millis()'s wrap; version() against shape(); the test hook; the counters; with
// the real scheduler; and a random session against a card whose answers are
// known. Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "CardContainer.h"
#include "ScanScheduler.h"
#include "WaveShape.h"
#include "WaveState.h"

namespace cc = cardcontract;
using F = WaveState::Finish;
using wave::Read;
using wave::ReadResult;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint64_t kA = 0xAAAAAAAAAAAAAAAAull, kB = 0xBBBBBBBBBBBBBBBBull, kC = 0xCCCCCCCCCCCCCCCCull;

ReadResult hit(uint8_t level = 5, uint32_t lengthMs = 180000, uint32_t jobMs = 20, uint32_t slotReads = 5) {
  ReadResult r;
  r.read = Read::Hit;
  r.jobMs = jobMs;
  r.slotReads = slotReads;
  r.shape.lengthMs = lengthMs;
  std::memset(r.shape.heights, level, sizeof(r.shape.heights));
  return r;
}

ReadResult answer(Read read, cc::Why why = cc::Why::Ok, uint32_t jobMs = 20, uint32_t slotReads = 5) {
  ReadResult r;
  r.read = read;
  r.why = why;
  r.jobMs = jobMs;
  r.slotReads = slotReads;
  return r;
}

// The file's on, nothing wanted yet.
WaveState named() {
  WaveState s;
  s.begin(true);
  return s;
}

// The file's on, `hash` wanted, its shape read.
WaveState holding(uint64_t hash, uint8_t level = 5) {
  WaveState s = named();
  s.want(hash);
  TEST_ASSERT_TRUE(s.wantsRead(0));
  s.started();
  TEST_ASSERT_EQUAL(F::Recorded, s.finished(hit(level), 0));
  return s;
}

}  // namespace

void test_nothing_until_a_file_and_a_want() {
  WaveState s;
  TEST_ASSERT_EQUAL(WaveState::File::None, s.file());
  TEST_ASSERT_FALSE(s.wantsRead(0));
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(0, s.version());
  // A want and no file: nothing to read, ever.
  s.want(kA);
  TEST_ASSERT_EQUAL_UINT64(kA, s.wanted());
  TEST_ASSERT_FALSE(s.wantsRead(0));
  TEST_ASSERT_FALSE(s.wantsRead(1000000));
  // The file and no want: nothing to read either.
  WaveState t = named();
  TEST_ASSERT_EQUAL(WaveState::File::Named, t.file());
  TEST_ASSERT_FALSE(t.wantsRead(0));
  // Both.
  t.want(kA);
  TEST_ASSERT_TRUE(t.wantsRead(0));
  TEST_ASSERT_TRUE(t.wantsRead(5));  // until it starts
  TEST_ASSERT_NULL(t.shape());
  TEST_ASSERT_FALSE(t.reading());
  // 0: nothing for this track.
  t.want(0);
  TEST_ASSERT_EQUAL_UINT64(0, t.wanted());
  TEST_ASSERT_FALSE(t.wantsRead(0));
  TEST_ASSERT_NULL(t.shape());
}

void test_a_hit_is_the_record() {
  WaveState s = named();
  s.want(kA);
  const uint32_t v = s.version();
  s.started();
  TEST_ASSERT_TRUE(s.reading());
  TEST_ASSERT_FALSE(s.wantsRead(0));  // one job at a time
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(v, s.version());
  TEST_ASSERT_EQUAL(F::Recorded, s.finished(hit(7, 181000), 100));
  TEST_ASSERT_FALSE(s.reading());
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(181000, s.shape()->lengthMs);
  for (uint8_t h : s.shape()->heights) TEST_ASSERT_EQUAL_UINT8(7, h);
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_EQUAL_UINT64(kA, s.heldHash());
  TEST_ASSERT_TRUE(s.heldShape());
  // Held: no read again, whatever the time, and nothing changes for the same
  // hash (a library update's renumbering; a repaint).
  const uint32_t held = s.version();
  TEST_ASSERT_FALSE(s.wantsRead(1000));
  s.want(kA);
  TEST_ASSERT_FALSE(s.wantsRead(2000));
  TEST_ASSERT_EQUAL_UINT32(held, s.version());
  TEST_ASSERT_NOT_NULL(s.shape());
}

// One record, for whichever hash it answers: another track hides it, the track
// again shows it with no read, a third's answer replaces it.
void test_the_record_outlives_a_want_until_replaced() {
  WaveState s = holding(kA);
  uint32_t v = s.version();
  s.want(kB);
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_TRUE(s.wantsRead(0));
  v = s.version();
  // Back before B's read came: no read, and the shape again.
  s.want(kA);
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_FALSE(s.wantsRead(0));
  // No track, then back.
  v = s.version();
  s.want(0);
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.version() != v);
  v = s.version();
  s.want(kA);
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_FALSE(s.wantsRead(0));
  // B's answer replaces it: A reads again, even on a Miss.
  s.want(kB);
  s.started();
  TEST_ASSERT_EQUAL(F::None, s.finished(answer(Read::Miss), 0));
  TEST_ASSERT_NULL(s.shape());
  s.want(kA);
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.wantsRead(0));
  // A want with no shape either side moves nothing: no needless full strip.
  WaveState n = named();
  v = n.version();
  n.want(kA);
  n.want(kB);
  n.want(0);
  TEST_ASSERT_EQUAL_UINT32(v, n.version());
}

// Miss and Damaged: none for this hash, known, and the file stays on.
void test_a_miss_and_a_damaged_slot() {
  for (Read read : {Read::Miss, Read::Damaged}) {
    WaveState s = named();
    s.want(kA);
    s.started();
    const uint32_t v = s.version();
    TEST_ASSERT_EQUAL(F::None, s.finished(answer(read), 0));
    TEST_ASSERT_NULL(s.shape());
    TEST_ASSERT_EQUAL_UINT32(v, s.version());  // (it was none already)
    TEST_ASSERT_FALSE(s.wantsRead(0));
    TEST_ASSERT_FALSE(s.wantsRead(100000));
    TEST_ASSERT_EQUAL_UINT64(kA, s.heldHash());
    TEST_ASSERT_FALSE(s.heldShape());
    s.want(kA);  // a repaint, a renumbering
    TEST_ASSERT_FALSE(s.wantsRead(100000));
    s.want(0);  // no track and back: known
    s.want(kA);
    TEST_ASSERT_FALSE(s.wantsRead(100000));
    TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());
    s.want(kB);  // the file goes on
    TEST_ASSERT_TRUE(s.wantsRead(100000));
  }
  WaveState m = named();
  m.want(kA);
  m.started();
  m.finished(answer(Read::Miss), 0);
  m.want(kB);
  m.started();
  m.finished(answer(Read::Damaged, cc::Why::WvSlotCrc), 0);
  TEST_ASSERT_EQUAL_UINT32(1, m.misses());
  TEST_ASSERT_EQUAL_UINT32(1, m.damaged());
}

// Absent: the file's off, whichever track wanted it, until a remount.
void test_absent_is_off_until_a_remount() {
  WaveState s = holding(kA);
  s.want(kB);
  s.started();
  uint32_t v = s.version();
  TEST_ASSERT_EQUAL(F::Absent, s.finished(answer(Read::Absent, cc::Why::WvSlotOrder), 0));
  TEST_ASSERT_EQUAL(WaveState::File::Absent, s.file());
  TEST_ASSERT_EQUAL_STRING("wvSlotOrder", cc::whyName(s.why()));
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(v, s.version());
  TEST_ASSERT_FALSE(s.wantsRead(0));
  TEST_ASSERT_FALSE(s.wantsRead(100000));
  // Any other track, and the one whose shape it read before: plain lines.
  for (uint64_t h : {kC, kA, kB}) {
    s.want(h);
    TEST_ASSERT_FALSE(s.wantsRead(100000));
    TEST_ASSERT_NULL(s.shape());
  }
  // A remount: the file again.
  s.remounted(true);
  TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(s.why()));
  TEST_ASSERT_TRUE(s.wantsRead(0));
  // The file level's own failure is the same.
  s.started();
  TEST_ASSERT_EQUAL(F::Absent, s.finished(answer(Read::Absent, cc::Why::Magic, 5, 0), 0));
  TEST_ASSERT_EQUAL_STRING("magic", cc::whyName(s.why()));
  TEST_ASSERT_EQUAL_UINT32(2, s.absents());
}

// Io: read again once, 2 s on, then none until the track changes.
void test_a_failed_read_is_read_again_once() {
  WaveState s = named();
  s.want(kA);
  s.started();
  TEST_ASSERT_EQUAL(F::Retry, s.finished(answer(Read::Io, cc::Why::Io), 1000));
  TEST_ASSERT_EQUAL_UINT32(1, s.failedReads());
  TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());  // the file isn't to blame
  TEST_ASSERT_FALSE(s.wantsRead(1000));
  TEST_ASSERT_FALSE(s.wantsRead(2999));
  TEST_ASSERT_TRUE(s.wantsRead(3000));
  TEST_ASSERT_TRUE(s.wantsRead(9000));
  s.started();
  TEST_ASSERT_EQUAL(F::GaveUp, s.finished(answer(Read::Io, cc::Why::Io), 9100));
  TEST_ASSERT_EQUAL_UINT32(2, s.failedReads());
  TEST_ASSERT_FALSE(s.wantsRead(9100));
  TEST_ASSERT_FALSE(s.wantsRead(11100));
  TEST_ASSERT_FALSE(s.wantsRead(100000000));
  TEST_ASSERT_NULL(s.shape());
  // The same hash again changes nothing (a repaint).
  s.want(kA);
  TEST_ASSERT_FALSE(s.wantsRead(100000000));
  // Another track: read at once, and its own two tries.
  s.want(kB);
  TEST_ASSERT_EQUAL_UINT32(0, s.failedReads());
  TEST_ASSERT_TRUE(s.wantsRead(9100));
  s.started();
  TEST_ASSERT_EQUAL(F::Retry, s.finished(answer(Read::Io, cc::Why::Io), 9200));
  TEST_ASSERT_FALSE(s.wantsRead(9200));
  // The track changes in the wait: no wait for the new one.
  s.want(kC);
  TEST_ASSERT_TRUE(s.wantsRead(9200));
  // And back to the first: it has two tries again.
  s.want(kA);
  TEST_ASSERT_TRUE(s.wantsRead(9200));
  s.started();
  TEST_ASSERT_EQUAL(F::Retry, s.finished(answer(Read::Io, cc::Why::Io), 9300));
  TEST_ASSERT_TRUE(s.wantsRead(11300));
  // The retry's answer stands: a shape.
  s.started();
  TEST_ASSERT_EQUAL(F::Recorded, s.finished(hit(), 11400));
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_FALSE(s.wantsRead(11400));
  TEST_ASSERT_EQUAL_UINT32(4, s.ioErrors());
}

// A file that didn't open. A fault on the way to it is a failed read like any: a
// wait, one more try, and then only this track goes without; the file stays on, a
// shape read before with it. FatFs's word that it isn't on the card is the file
// off until a remount.
void test_a_file_that_didnt_open() {
  ReadResult failed, gone;
  wave::openFailed(false, &failed);
  wave::openFailed(true, &gone);
  WaveState s = holding(kA);
  s.want(kB);
  s.started();
  TEST_ASSERT_EQUAL(F::Retry, s.finished(failed, 1000));
  TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());
  TEST_ASSERT_FALSE(s.wantsRead(2999));
  TEST_ASSERT_TRUE(s.wantsRead(3000));
  s.started();
  TEST_ASSERT_EQUAL(F::GaveUp, s.finished(failed, 3000));
  TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());
  TEST_ASSERT_FALSE(s.wantsRead(100000));
  // The next tracks read, and the shape read before is still there.
  s.want(kA);
  TEST_ASSERT_NOT_NULL(s.shape());
  s.want(kC);
  TEST_ASSERT_TRUE(s.wantsRead(100000));
  // And then the card says it isn't there.
  s.started();
  TEST_ASSERT_EQUAL(F::Absent, s.finished(gone, 100000));
  TEST_ASSERT_EQUAL(WaveState::File::Absent, s.file());
  TEST_ASSERT_EQUAL_STRING("io", cc::whyName(s.why()));
  TEST_ASSERT_FALSE(s.wantsRead(200000));
  s.want(kA);
  TEST_ASSERT_FALSE(s.wantsRead(200000));
  TEST_ASSERT_NULL(s.shape());
  s.remounted(true);
  TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());
  TEST_ASSERT_TRUE(s.wantsRead(200000));
  TEST_ASSERT_EQUAL_UINT32(1, s.absents());
  TEST_ASSERT_EQUAL_UINT32(2, s.ioErrors());
}

// Across millis()'s wrap the wait is still 2 s; a wait that ran out long ago
// doesn't come back.
void test_the_retrys_wait_across_the_wrap() {
  WaveState s = named();
  s.want(kA);
  s.started();
  const uint32_t at = 0xFFFFFF00u;
  TEST_ASSERT_EQUAL(F::Retry, s.finished(answer(Read::Io, cc::Why::Io), at));
  TEST_ASSERT_FALSE(s.wantsRead(at));
  TEST_ASSERT_FALSE(s.wantsRead(0xFFFFFFFFu));
  TEST_ASSERT_FALSE(s.wantsRead(0));
  TEST_ASSERT_FALSE(s.wantsRead(at + WaveState::kIoRetryMs - 1));
  TEST_ASSERT_TRUE(s.wantsRead(at + WaveState::kIoRetryMs));
  TEST_ASSERT_TRUE(s.wantsRead(0x7FFFFFFFu));  // 24 days on: still due
  TEST_ASSERT_TRUE(s.wantsRead(0xF0000000u));
}

// The worker wouldn't take the job: asked again a second on, for covers' reason.
void test_the_worker_refuses() {
  WaveState s = named();
  s.want(kA);
  TEST_ASSERT_TRUE(s.wantsRead(100));
  s.refused(100);
  TEST_ASSERT_FALSE(s.reading());
  TEST_ASSERT_FALSE(s.wantsRead(100));
  TEST_ASSERT_FALSE(s.wantsRead(1099));
  TEST_ASSERT_TRUE(s.wantsRead(1100));
  // It costs the hash no try.
  TEST_ASSERT_EQUAL_UINT32(0, s.failedReads());
  s.started();
  TEST_ASSERT_TRUE(s.reading());
  TEST_ASSERT_FALSE(s.wantsRead(5000));
  TEST_ASSERT_EQUAL(F::Recorded, s.finished(hit(), 5000));
  // The track changes in the wait: asked at once (the worker's own wait stands
  // if it is the worker that can't).
  WaveState t = named();
  t.want(kA);
  t.refused(100);
  t.want(kB);
  TEST_ASSERT_TRUE(t.wantsRead(100));
}

// A burst of wants while a job runs: every answer but the wanted hash's goes.
void test_a_burst_of_wants_while_a_job_runs() {
  WaveState s = named();
  s.want(kA);
  s.started();
  uint32_t v = s.version();
  s.want(kB);
  s.want(kC);
  TEST_ASSERT_FALSE(s.wantsRead(0));  // one job at a time
  TEST_ASSERT_EQUAL_UINT32(v, s.version());
  // A's late answer: dropped, whatever it is, and C read next.
  TEST_ASSERT_EQUAL(F::Dropped, s.finished(hit(), 10));
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(v, s.version());
  TEST_ASSERT_EQUAL_UINT64(0, s.heldHash());
  TEST_ASSERT_TRUE(s.wantsRead(10));
  s.started();
  TEST_ASSERT_EQUAL(F::Recorded, s.finished(hit(3), 30));
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT8(3, s.shape()->heights[0]);
  TEST_ASSERT_EQUAL_UINT64(kC, s.heldHash());
  TEST_ASSERT_EQUAL_UINT32(1, s.dropped());
  TEST_ASSERT_EQUAL_UINT32(2, s.reads());

  // Each kind of late answer costs the file and the track nothing: a Hit no
  // shape, a Miss no mark, an Absent no off, an Io no try or wait.
  const ReadResult late[] = {hit(), answer(Read::Miss), answer(Read::Damaged, cc::Why::WvSlotCrc),
                             answer(Read::Absent, cc::Why::Magic), answer(Read::Io, cc::Why::Io)};
  for (const ReadResult& r : late) {
    WaveState t = named();
    t.want(kA);
    t.started();
    t.want(kB);
    TEST_ASSERT_EQUAL(F::Dropped, t.finished(r, 0));
    TEST_ASSERT_EQUAL(WaveState::File::Named, t.file());
    TEST_ASSERT_EQUAL_UINT32(0, t.failedReads());
    TEST_ASSERT_TRUE(t.wantsRead(0));
    TEST_ASSERT_NULL(t.shape());
  }

  // Back to the job's own hash before it ended: its answer is wanted after all.
  WaveState b = named();
  b.want(kA);
  b.started();
  b.want(kB);
  b.want(kA);
  TEST_ASSERT_EQUAL(F::Recorded, b.finished(hit(), 0));
  TEST_ASSERT_NOT_NULL(b.shape());
  TEST_ASSERT_FALSE(b.wantsRead(0));

  // The track gone to no track: dropped.
  WaveState z = named();
  z.want(kA);
  z.started();
  z.want(0);
  TEST_ASSERT_EQUAL(F::Dropped, z.finished(hit(), 0));
  TEST_ASSERT_FALSE(z.wantsRead(0));
}

// A remount: everything goes, an answer from before it is dropped, and the wanted
// hash is asked for again.
void test_a_remount() {
  // With a record, and no job.
  WaveState s = holding(kA);
  uint32_t v = s.version(), g = s.generation();
  s.remounted(true);
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_TRUE(s.generation() != g);
  TEST_ASSERT_EQUAL_UINT64(0, s.heldHash());
  TEST_ASSERT_TRUE(s.wantsRead(0));
  s.started();
  TEST_ASSERT_EQUAL(F::Recorded, s.finished(hit(), 0));
  TEST_ASSERT_NOT_NULL(s.shape());

  // In the middle of a job: its answer is the old card's. Nothing starts until
  // it ends, then the same hash is read again.
  WaveState m = named();
  m.want(kA);
  m.started();
  g = m.generation();
  v = m.version();
  m.remounted(true);
  TEST_ASSERT_TRUE(m.generation() != g);
  TEST_ASSERT_EQUAL_UINT32(v, m.version());  // (nothing to take back)
  TEST_ASSERT_TRUE(m.reading());
  TEST_ASSERT_FALSE(m.wantsRead(0));
  TEST_ASSERT_EQUAL(F::Dropped, m.finished(hit(), 0));
  TEST_ASSERT_NULL(m.shape());
  TEST_ASSERT_TRUE(m.wantsRead(0));
  m.started();
  TEST_ASSERT_EQUAL(F::Recorded, m.finished(hit(2), 0));
  TEST_ASSERT_EQUAL_UINT8(2, m.shape()->heights[9]);
  TEST_ASSERT_EQUAL_UINT32(1, m.dropped());

  // A failed read, a known none and a wait: gone too.
  WaveState w = named();
  w.want(kA);
  w.started();
  w.finished(answer(Read::Io, cc::Why::Io), 0);
  w.started();
  w.finished(answer(Read::Io, cc::Why::Io), 0);
  TEST_ASSERT_FALSE(w.wantsRead(5000));
  w.remounted(true);
  TEST_ASSERT_EQUAL_UINT32(0, w.failedReads());
  TEST_ASSERT_TRUE(w.wantsRead(0));
  w.started();
  w.finished(answer(Read::Io, cc::Why::Io), 100);
  TEST_ASSERT_FALSE(w.wantsRead(100));
  w.remounted(true);
  TEST_ASSERT_TRUE(w.wantsRead(100));
  w.started();
  w.finished(answer(Read::Miss), 100);
  TEST_ASSERT_FALSE(w.wantsRead(100));
  w.remounted(true);
  TEST_ASSERT_TRUE(w.wantsRead(100));

  // The root read again names none: nothing more, and no late answer counts.
  WaveState n = holding(kA);
  v = n.version();
  n.remounted(false);
  TEST_ASSERT_EQUAL(WaveState::File::None, n.file());
  TEST_ASSERT_NULL(n.shape());
  TEST_ASSERT_TRUE(n.version() != v);
  TEST_ASSERT_FALSE(n.wantsRead(0));
  n.want(kB);
  TEST_ASSERT_FALSE(n.wantsRead(0));
  // And then a card that names one, the same hash wanted.
  n.remounted(true);
  TEST_ASSERT_TRUE(n.wantsRead(0));
  TEST_ASSERT_EQUAL_UINT64(kB, n.wanted());
  // Two remounts under one job: still the one drop, for the one job.
  WaveState d = named();
  d.want(kA);
  d.started();
  d.remounted(true);
  d.remounted(false);
  d.remounted(true);
  TEST_ASSERT_EQUAL(F::Dropped, d.finished(hit(), 0));
  TEST_ASSERT_TRUE(d.wantsRead(0));
}

void test_a_finish_with_no_job_is_nothing() {
  WaveState s = named();
  s.want(kA);
  TEST_ASSERT_EQUAL(F::Dropped, s.finished(hit(), 0));
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(0, s.reads());
  TEST_ASSERT_EQUAL_UINT32(0, s.dropped());
  TEST_ASSERT_TRUE(s.wantsRead(0));
}

// gw=: a shape with no card, no file and no track. It shows until gw-, over any
// track, and no read runs under it.
void test_the_test_hook() {
  uint8_t heights[wave::kColumns];
  for (int i = 0; i < wave::kColumns; ++i) heights[i] = static_cast<uint8_t>(i % 10);
  heights[50] = 200;  // (cut to the most a bar has)
  WaveState s;
  uint32_t v = s.version();
  TEST_ASSERT_FALSE(s.injected());
  s.inject(heights);
  TEST_ASSERT_TRUE(s.injected());
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(0, s.shape()->lengthMs);  // (agrees with any)
  for (int i = 0; i < wave::kColumns; ++i) TEST_ASSERT_EQUAL_UINT8(i == 50 ? 9 : i % 10, s.shape()->heights[i]);
  // With a file and tracks going by: still there, no version, no read.
  s.begin(true);
  v = s.version();
  s.want(kA);
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_FALSE(s.wantsRead(0));
  s.want(kB);
  s.want(0);
  TEST_ASSERT_NOT_NULL(s.shape());
  TEST_ASSERT_EQUAL_UINT32(v, s.version());
  // gw-: gone, and the wanted hash reads.
  s.want(kA);
  s.dropInjected();
  TEST_ASSERT_FALSE(s.injected());
  TEST_ASSERT_NULL(s.shape());
  TEST_ASSERT_TRUE(s.version() != v);
  TEST_ASSERT_TRUE(s.wantsRead(0));
  // Dropping nothing moves nothing.
  v = s.version();
  s.dropInjected();
  TEST_ASSERT_EQUAL_UINT32(v, s.version());
  // Over a record: it goes, and is read again after the hook.
  WaveState r = holding(kA, 8);
  r.inject(heights, 123456);
  TEST_ASSERT_EQUAL_UINT32(123456, r.shape()->lengthMs);
  TEST_ASSERT_EQUAL_UINT8(1, r.shape()->heights[1]);
  r.dropInjected();
  TEST_ASSERT_NULL(r.shape());
  TEST_ASSERT_TRUE(r.wantsRead(0));
  // A job under way when it is set: its answer doesn't write over the hook.
  WaveState j = named();
  j.want(kA);
  j.started();
  j.inject(heights);
  TEST_ASSERT_EQUAL(F::Dropped, j.finished(hit(7), 0));
  TEST_ASSERT_EQUAL_UINT8(1, j.shape()->heights[1]);
  // A remount leaves the hook: nothing to do with the card.
  v = j.version();
  j.remounted(true);
  TEST_ASSERT_NOT_NULL(j.shape());
  TEST_ASSERT_EQUAL_UINT32(v, j.version());
  // Dropped with a job's answer still to come: that answer counts.
  WaveState k = named();
  k.want(kA);
  k.started();
  k.inject(heights);
  k.dropInjected();
  TEST_ASSERT_EQUAL(F::Recorded, k.finished(hit(7), 0));
  TEST_ASSERT_EQUAL_UINT8(7, k.shape()->heights[1]);
}

void test_the_counters() {
  WaveState s = named();
  TEST_ASSERT_EQUAL_UINT32(0, s.meanJobMs());
  TEST_ASSERT_EQUAL_UINT32(0, s.maxJobMs());
  struct Step {
    uint64_t hash;
    ReadResult r;
  };
  const Step steps[] = {
      {kA, hit(1, 1000, 10, 4)},
      {kB, answer(Read::Miss, cc::Why::Ok, 21, 6)},
      {kC, answer(Read::Damaged, cc::Why::WvSlotCrc, 30, 2)},
      {kA, answer(Read::Io, cc::Why::Io, 5, 1)},
      {kB, answer(Read::Absent, cc::Why::Magic, 7, 0)},
  };
  for (const Step& st : steps) {
    s.want(st.hash);
    TEST_ASSERT_TRUE(s.wantsRead(1000000));
    s.started();
    s.finished(st.r, 0);
  }
  s.remounted(true);
  // One dropped, its time counted: the time was spent.
  s.want(kC);
  s.started();
  s.want(kA);
  s.finished(hit(1, 1000, 100, 9), 0);
  TEST_ASSERT_EQUAL_UINT32(6, s.reads());
  TEST_ASSERT_EQUAL_UINT32(s.reads(), s.hits() + s.misses() + s.damaged() + s.absents() + s.ioErrors() + s.dropped());
  TEST_ASSERT_EQUAL_UINT32(1, s.hits());
  TEST_ASSERT_EQUAL_UINT32(1, s.misses());
  TEST_ASSERT_EQUAL_UINT32(1, s.damaged());
  TEST_ASSERT_EQUAL_UINT32(1, s.absents());
  TEST_ASSERT_EQUAL_UINT32(1, s.ioErrors());
  TEST_ASSERT_EQUAL_UINT32(1, s.dropped());
  TEST_ASSERT_EQUAL_UINT32(22, s.slotReads());
  TEST_ASSERT_EQUAL_UINT32(29, s.meanJobMs());  // 173 / 6, rounded
  TEST_ASSERT_EQUAL_UINT32(100, s.maxJobMs());
}

// With the real scheduler, as CardTasks has them: in.wave is wantsRead() with the
// worker free, and Job::Wave starts it. A list holds it, and a track change under
// a job reads the next.
void test_with_the_scheduler() {
  using S = ScanScheduler;
  S sched;
  WaveState w = named();
  uint32_t now = 0;
  S::In in;
  S::Out o;
  uint32_t jobs = 0;
  uint64_t jobHash = 0;
  auto pass = [&]() {
    in.nowMs = now;
    if (in.running == S::Job::None) in.wave = w.wantsRead(now);
    o = sched.update(in);
    if (o.job == S::Job::Wave) {
      TEST_ASSERT_TRUE(w.wantsRead(now));
      jobHash = w.wanted();
      w.started();
      in.running = S::Job::Wave;
      in.wave = false;
      ++jobs;
    }
    now += 10;
  };
  // Nothing wanted: nothing starts.
  pass();
  TEST_ASSERT_EQUAL(S::Job::None, o.job);
  // A list moving: it waits, for as long as the list moves.
  in.listMoving = true;
  w.want(kA);
  for (int i = 0; i < 100; ++i) {
    pass();
    TEST_ASSERT_EQUAL(S::Wait::List, o.wait);
  }
  TEST_ASSERT_EQUAL_UINT32(0, jobs);
  // It stops: the next pass starts the job, at the loop's priority.
  in.listMoving = false;
  pass();
  TEST_ASSERT_EQUAL(S::Job::Wave, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  TEST_ASSERT_EQUAL_UINT32(1, jobs);
  // Under way: the next track wanted. Nothing more starts.
  w.want(kB);
  for (int i = 0; i < 5; ++i) {
    pass();
    TEST_ASSERT_EQUAL(S::Wait::Step, o.wait);
  }
  TEST_ASSERT_EQUAL_UINT32(1, jobs);
  // The job ends: the worker's free before the loop has taken the answer in. A
  // pass of it reads nothing yet: wantsRead() is false until finished().
  in.running = S::Job::None;
  pass();
  TEST_ASSERT_EQUAL(S::Job::None, o.job);
  TEST_ASSERT_EQUAL_UINT32(1, jobs);
  TEST_ASSERT_EQUAL_UINT64(kA, jobHash);
  // Taken in: dropped, and B starts.
  TEST_ASSERT_EQUAL(F::Dropped, w.finished(hit(), now));
  pass();
  TEST_ASSERT_EQUAL(S::Job::Wave, o.job);
  TEST_ASSERT_EQUAL_UINT64(kB, jobHash);
  in.running = S::Job::None;
  TEST_ASSERT_EQUAL(F::Recorded, w.finished(hit(), now));
  pass();
  TEST_ASSERT_EQUAL(S::Job::None, o.job);
  TEST_ASSERT_NOT_NULL(w.shape());
  TEST_ASSERT_EQUAL_UINT32(2, jobs);
}

namespace {

// A card's answers, by hash. kFlaky fails its first read after each want; kBad
// breaks the file; kDead always fails. Each with a shape has its own level.
constexpr uint64_t kShaped1 = 1, kShaped2 = 2, kNoSlot = 3, kDamaged = 4, kFlaky = 5, kDead = 6, kBad = 7;

uint8_t levelOf(uint64_t hash) { return hash == kShaped1 ? 1 : hash == kShaped2 ? 2 : hash == kFlaky ? 5 : 0; }

}  // namespace

// A random session: an hour of passes, wants, remounts and a slow worker against
// that card. shape() is only ever the wanted hash's own, version() moves whenever
// what it shows does, jobs never overlap, a known hash isn't read twice, the file
// stays off once off until the next remount, and with the session left alone the
// wanted track's answer always comes.
void test_a_random_session() {
  std::mt19937 rng(20261102);
  auto chance = [&](uint32_t perMille) { return rng() % 1000 < perMille; };
  const uint64_t wants[] = {0, kShaped1, kShaped2, kNoSlot, kDamaged, kFlaky, kDead, kBad};
  uint32_t now = 0xFFF00000u;  // the wrap comes by
  WaveState s = named();
  uint32_t lastVersion = s.version();
  std::string lastShown = "-";
  bool running = false;
  uint32_t doneAt = 0;
  uint64_t jobHash = 0;
  bool flakyFailed = false;
  uint32_t jobs = 0, recorded = 0, offs = 0, remounts = 0, wantBursts = 0, refusals = 0;
  auto shown = [&]() {
    const wave::Shape* sh = s.shape();
    if (!sh) return std::string("-");
    std::string d;
    for (uint8_t h : sh->heights) d += static_cast<char>('0' + h);
    return d;
  };
  auto cardAnswer = [&](uint64_t hash) -> ReadResult {
    switch (hash) {
      case kShaped1:
      case kShaped2: return hit(levelOf(hash));
      case kNoSlot: return answer(Read::Miss);
      case kDamaged: return answer(Read::Damaged, cc::Why::WvSlotCrc);
      case kFlaky:
        if (!flakyFailed) {
          flakyFailed = true;
          return answer(Read::Io, cc::Why::Io);
        }
        return hit(levelOf(hash));
      case kDead: return answer(Read::Io, cc::Why::Io);
      default: return answer(Read::Absent, cc::Why::WvSlotOrder);
    }
  };
  for (int pass = 0; pass < 200000; ++pass) {
    now += 5 + rng() % 36;
    if (chance(20)) {
      const uint64_t next = wants[rng() % 8];
      if (next != s.wanted()) flakyFailed = false;
      s.want(next);
      ++wantBursts;
    }
    if (chance(1)) {
      s.remounted(!chance(100));
      flakyFailed = false;
      ++remounts;
    }
    if (running && static_cast<int32_t>(now - doneAt) >= 0) {
      running = false;
      const F f = s.finished(cardAnswer(jobHash), now);
      if (f == F::Recorded) ++recorded;
      if (f == F::Absent) ++offs;
    }
    if (s.wantsRead(now)) {
      TEST_ASSERT_FALSE(running);
      TEST_ASSERT_TRUE(s.wanted() != 0);
      TEST_ASSERT_EQUAL(WaveState::File::Named, s.file());
      if (chance(30)) {
        s.refused(now);
        ++refusals;
      } else {
        jobHash = s.wanted();
        s.started();
        running = true;
        doneAt = now + 5 + rng() % 80;
        ++jobs;
      }
    }
    // What shows is the wanted hash's, or nothing.
    const std::string sh = shown();
    if (sh != "-") {
      TEST_ASSERT_TRUE(s.wanted() != 0);
      TEST_ASSERT_EQUAL_UINT64(s.wanted(), s.heldHash());
      const std::string want(wave::kColumns, static_cast<char>('0' + levelOf(s.wanted())));
      TEST_ASSERT_EQUAL_STRING(want.c_str(), sh.c_str());
    }
    if (sh != lastShown) TEST_ASSERT_TRUE_MESSAGE(s.version() != lastVersion, "what shows changed, and no version");
    lastShown = sh;
    lastVersion = s.version();
    if (s.file() == WaveState::File::Absent) TEST_ASSERT_NULL(s.shape());
  }
  std::printf("[wave] a session: %u wants, %u jobs (%u shapes, %u files off, %u dropped), %u remounts, %u refusals\n",
              static_cast<unsigned>(wantBursts), static_cast<unsigned>(jobs), static_cast<unsigned>(recorded),
              static_cast<unsigned>(offs), static_cast<unsigned>(s.dropped()), static_cast<unsigned>(remounts),
              static_cast<unsigned>(refusals));
  std::fflush(stdout);
  TEST_ASSERT_TRUE(jobs > 500);
  TEST_ASSERT_TRUE(recorded > 100);
  TEST_ASSERT_TRUE(offs > 10);
  TEST_ASSERT_TRUE(s.dropped() > 10);
  TEST_ASSERT_TRUE(remounts > 50);
  TEST_ASSERT_TRUE(refusals > 2);
  TEST_ASSERT_TRUE(wantBursts > 100);
  // Left alone, on each kind of track: its answer comes, within the retry's wait.
  for (uint64_t hash : {kShaped1, kNoSlot, kDamaged, kFlaky, kDead}) {
    WaveState t = named();
    t.want(hash);
    flakyFailed = false;
    uint32_t reads = 0;
    for (uint32_t ms = 0; ms < 10000; ms += 10) {
      if (!t.wantsRead(ms)) continue;
      t.started();
      ++reads;
      t.finished(cardAnswer(hash), ms);
    }
    const uint32_t wantReads = hash == kFlaky || hash == kDead ? 2 : 1;
    TEST_ASSERT_EQUAL_UINT32(wantReads, reads);
    TEST_ASSERT_EQUAL(levelOf(hash) != 0, t.shape() != nullptr);
  }
}

// Small enough to sit in DRAM, the record in it.
void test_its_size() {
  TEST_ASSERT_TRUE(sizeof(WaveState) < 320);
  TEST_ASSERT_TRUE(sizeof(wave::Shape) <= 104);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_until_a_file_and_a_want);
  RUN_TEST(test_a_hit_is_the_record);
  RUN_TEST(test_the_record_outlives_a_want_until_replaced);
  RUN_TEST(test_a_miss_and_a_damaged_slot);
  RUN_TEST(test_absent_is_off_until_a_remount);
  RUN_TEST(test_a_failed_read_is_read_again_once);
  RUN_TEST(test_a_file_that_didnt_open);
  RUN_TEST(test_the_retrys_wait_across_the_wrap);
  RUN_TEST(test_the_worker_refuses);
  RUN_TEST(test_a_burst_of_wants_while_a_job_runs);
  RUN_TEST(test_a_remount);
  RUN_TEST(test_a_finish_with_no_job_is_nothing);
  RUN_TEST(test_the_test_hook);
  RUN_TEST(test_the_counters);
  RUN_TEST(test_with_the_scheduler);
  RUN_TEST(test_a_random_session);
  RUN_TEST(test_its_size);
  return UNITY_END();
}
