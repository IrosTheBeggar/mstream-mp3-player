// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for Auto DJ's trigger (lib/core/AutoDjTrigger; the
// engine design's 2.3, 2.3.1 and 2.3.2), like the mobile app's
// auto_dj_top_up_test.dart:
// - every condition of 2.3 on its own, and their order (the console's
//   reason is the first that fails);
// - the finished queue: picked and cued, under the same conditions;
// - the holds (Repeat One, the sleep timer's End of queue, the failure
//   pause) and what the "..." row shows, wherever the queue is;
// - autoDjOwed: true only while the index joins or reopens and a pick is
//   otherwise due; never while held, never with tracks up next or nothing
//   playing;
// - the failure pause: three of Auto DJ's picks in a row, the listener's
//   failures neither counting nor ending the row, cleared by a track that
//   plays and by off and on;
// - the pieces together: an AutoDJ index written for a made-up library,
//   joined (AutoDjJoin), its Maps and its slots (mpdj::Table::slot()) feeding
//   the engine (AutoDjEngine), the trigger read off a real player, each pick
//   added through PlaybackController::addFromAutoDj(): a session of picks
//   as the firmware's step will make them, a removed pick picked again, the
//   fence, and a finished queue's cued pick.
// Run: pio test -e native -f test_autodj_trigger
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "AutoDjEngine.h"
#include "AutoDjJoin.h"
#include "AutoDjTrigger.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "HeadsetKeys.h"
#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using autodj::Hold;
using autodj::TriggerIn;
using autodj::TriggerOut;
using autodj::Wait;

namespace {

// A pick is due: Auto DJ on, the last of three entries playing.
TriggerIn due() {
  TriggerIn in;
  in.on = true;
  in.queueSize = 3;
  in.upNext = 0;
  in.play = PlayState::Playing;
  return in;
}

void expectWait(Wait want, const TriggerOut& o, const char* what) {
  char msg[96];
  std::snprintf(msg, sizeof(msg), "%s: %s, not %s", what, autodj::waitName(o.wait), autodj::waitName(want));
  TEST_ASSERT_TRUE_MESSAGE(o.wait == want, msg);
  TEST_ASSERT_EQUAL_MESSAGE(want == Wait::Due, o.pick, what);
}

// The smallest backend a player needs: plays, ends, and the words.
class Backend : public IAudioBackend {
public:
  std::string lastPath;
  int plays = 0;
  bool playing = false, paused = false, ended = false;
  std::vector<Next> nexts;
  bool play(const std::string& p, uint32_t, uint32_t) override {
    lastPath = p;
    ++plays;
    playing = true;
    paused = false;
    ended = false;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override { playing = paused = false; }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return ended ? 1000 : 0; }
  bool finished() const override { return ended; }
  void setNext(const Next& n) override { nexts.push_back(n); }
};

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The rule.
// ---------------------------------------------------------------------------

// Each condition of 2.3 failing on its own, the rest as due.
void test_every_condition() {
  expectWait(Wait::Due, decide(due()), "due");
  TEST_ASSERT_FALSE(decide(due()).cue);
  TEST_ASSERT_FALSE(decide(due()).owed);
  TEST_ASSERT_TRUE(decide(due()).hold == Hold::None);

  TriggerIn in = due();
  in.on = false;
  const TriggerOut off = decide(in);
  expectWait(Wait::Off, off, "1: off");
  TEST_ASSERT_TRUE(off.hold == Hold::None);
  TEST_ASSERT_FALSE(off.owed);

  in = due();
  in.queueSize = 0;
  expectWait(Wait::Empty, decide(in), "2: an empty queue");
  in = due();
  in.upNext = 1;
  expectWait(Wait::UpNext, decide(in), "2: one up next");

  // 3: a playback session (Playing, or Waiting for the headphones), never
  // a paused or a just-restored (stopped) queue.
  in = due();
  in.play = PlayState::Waiting;
  expectWait(Wait::Due, decide(in), "3: waiting for the headphones");
  TEST_ASSERT_FALSE(decide(in).cue);
  in.play = PlayState::Paused;
  expectWait(Wait::Idle, decide(in), "3: paused");
  in.play = PlayState::Stopped;
  expectWait(Wait::Idle, decide(in), "3: stopped (restored)");

  in = due();
  in.fenced = true;
  expectWait(Wait::Fenced, decide(in), "4: the fence");
  in = due();
  in.cardHeld = true;
  expectWait(Wait::Card, decide(in), "5: the card held");
  in = due();
  in.restartSet = true;
  expectWait(Wait::Card, decide(in), "5: a restart set");
  in = due();
  in.joining = true;
  expectWait(Wait::Joining, decide(in), "1, 5: the join runs");
  TEST_ASSERT_TRUE(decide(in).owed);

  in = due();
  in.repeatOne = true;
  expectWait(Wait::RepeatOne, decide(in), "6: Repeat One");
  TEST_ASSERT_TRUE(decide(in).hold == Hold::RepeatOne);
  in = due();
  in.sleepEndOfQueue = true;
  expectWait(Wait::SleepTimer, decide(in), "7: the sleep timer's End of queue");
  TEST_ASSERT_TRUE(decide(in).hold == Hold::SleepTimer);
  in = due();
  in.failurePaused = true;
  expectWait(Wait::Failures, decide(in), "8: the failure pause");
  TEST_ASSERT_TRUE(decide(in).hold == Hold::Failures);
}

// The reason is the first condition that fails, in 2.3's order.
void test_the_first_condition_that_fails() {
  TriggerIn in = due();
  in.failurePaused = true;
  in.sleepEndOfQueue = true;
  in.repeatOne = true;
  in.joining = true;
  in.cardHeld = true;
  in.fenced = true;
  in.play = PlayState::Paused;
  in.upNext = 2;
  expectWait(Wait::UpNext, decide(in), "all");
  in.upNext = 0;
  expectWait(Wait::Idle, decide(in), "from 3");
  in.play = PlayState::Playing;
  expectWait(Wait::Fenced, decide(in), "from 4");
  in.fenced = false;
  expectWait(Wait::Card, decide(in), "from 5");
  in.cardHeld = false;
  expectWait(Wait::Joining, decide(in), "from 5's join");
  in.joining = false;
  expectWait(Wait::RepeatOne, decide(in), "from 6");
  in.repeatOne = false;
  expectWait(Wait::SleepTimer, decide(in), "from 7");
  in.sleepEndOfQueue = false;
  expectWait(Wait::Failures, decide(in), "from 8");
  in.failurePaused = false;
  expectWait(Wait::Due, decide(in), "none");
  // Off before everything, and no hold shown.
  in.on = false;
  in.repeatOne = true;
  expectWait(Wait::Off, decide(in), "off");
  TEST_ASSERT_TRUE(decide(in).hold == Hold::None);
  // Every reason has a name.
  for (int w = 0; w <= static_cast<int>(Wait::Failures); ++w)
    TEST_ASSERT_TRUE(std::strcmp("?", autodj::waitName(static_cast<Wait>(w))) != 0);
}

// 2.3.1: a finished queue (stopped on its last entry at its end) is picked
// for and cued, under conditions 1, 2 and 4-8; a stopped queue that didn't
// finish there isn't.
void test_a_finished_queue() {
  TriggerIn in = due();
  in.play = PlayState::Stopped;
  in.finished = true;
  TriggerOut o = decide(in);
  expectWait(Wait::Due, o, "finished");
  TEST_ASSERT_TRUE(o.cue);
  // (The player says finished only while stopped: Playing is a session, no cue.)
  in.play = PlayState::Playing;
  TEST_ASSERT_FALSE(decide(in).cue);
  TEST_ASSERT_TRUE(decide(in).pick);
  // Paused (the cued pick itself, at the end): no second pick.
  in.play = PlayState::Paused;
  in.finished = false;
  expectWait(Wait::Idle, decide(in), "the cued pick");
  // Finished with tracks added after: not last.
  in.play = PlayState::Stopped;
  in.finished = true;
  in.upNext = 1;
  expectWait(Wait::UpNext, decide(in), "finished, then + Queue");
  // Each of 1, 2 and 4-8 holds it.
  const auto blocked = [](void (*set)(TriggerIn&), Wait want, const char* what) {
    TriggerIn b = due();
    b.play = PlayState::Stopped;
    b.finished = true;
    set(b);
    const TriggerOut r = decide(b);
    expectWait(want, r, what);
    TEST_ASSERT_FALSE_MESSAGE(r.cue, what);
  };
  blocked([](TriggerIn& b) { b.on = false; }, Wait::Off, "off");
  blocked([](TriggerIn& b) { b.queueSize = 0; }, Wait::Empty, "empty");
  blocked([](TriggerIn& b) { b.fenced = true; }, Wait::Fenced, "fenced");
  blocked([](TriggerIn& b) { b.cardHeld = true; }, Wait::Card, "card");
  blocked([](TriggerIn& b) { b.restartSet = true; }, Wait::Card, "restart");
  blocked([](TriggerIn& b) { b.joining = true; }, Wait::Joining, "joining");
  blocked([](TriggerIn& b) { b.repeatOne = true; }, Wait::RepeatOne, "Repeat One");
  blocked([](TriggerIn& b) { b.sleepEndOfQueue = true; }, Wait::SleepTimer, "End of queue");
  blocked([](TriggerIn& b) { b.failurePaused = true; }, Wait::Failures, "failures");
}

// The holds are shown wherever the queue is (the "..." row: Waiting or
// Paused), Paused first; none while off.
void test_the_holds_shown() {
  TriggerIn in = due();
  in.upNext = 4;
  in.play = PlayState::Paused;
  in.repeatOne = true;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::RepeatOne);
  in.sleepEndOfQueue = true;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::RepeatOne);
  in.repeatOne = false;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::SleepTimer);
  in.failurePaused = true;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::Failures);
  in.repeatOne = true;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::Failures);
  in.queueSize = 0;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::Failures);
  in.on = false;
  TEST_ASSERT_TRUE(decide(in).hold == Hold::None);
}

// autoDjOwed: a pick due by 1-3 and 6-8 while the index joins or reopens.
// Every combination of the inputs that matter, against that rule.
void test_owed_only_while_joining() {
  int owed = 0, cases = 0;
  for (int bits = 0; bits < (1 << 11); ++bits) {
    TriggerIn in;
    in.on = bits & 1;
    in.joining = bits & 2;
    in.queueSize = (bits & 4) ? 2 : 0;
    in.upNext = (bits & 8) ? 1 : 0;
    in.play = (bits & 16) ? PlayState::Playing : PlayState::Stopped;
    in.finished = bits & 32;
    in.repeatOne = bits & 64;
    in.sleepEndOfQueue = bits & 128;
    in.failurePaused = bits & 256;
    in.fenced = bits & 512;
    in.cardHeld = bits & 1024;
    const bool session = in.play == PlayState::Playing || in.finished;
    const bool want = in.on && in.joining && in.queueSize && !in.upNext && session && !in.repeatOne &&
                      !in.sleepEndOfQueue && !in.failurePaused;
    const TriggerOut o = decide(in);
    char msg[32];
    std::snprintf(msg, sizeof(msg), "inputs %#x", bits);
    TEST_ASSERT_EQUAL_MESSAGE(want, o.owed, msg);
    // Never both: a pick owed is a pick that can't be made yet.
    TEST_ASSERT_FALSE_MESSAGE(o.owed && o.pick, msg);
    // A hold never owes one.
    if (o.hold != Hold::None) TEST_ASSERT_FALSE_MESSAGE(o.owed, msg);
    owed += o.owed;
    ++cases;
  }
  TEST_ASSERT_EQUAL_INT(2048, cases);
  // Playing (finished or not: the player never says both) or stopped and
  // finished; fenced and the card held each way (they don't matter: the
  // update step has its own fence, and a held card holds it too).
  TEST_ASSERT_EQUAL_INT(3 * 2 * 2, owed);
}

// The failure pause (2.3.2).
void test_the_failure_pause() {
  autodj::FailurePause f;
  TEST_ASSERT_FALSE(f.paused());
  TEST_ASSERT_FALSE(f.failed(true));
  TEST_ASSERT_FALSE(f.failed(true));
  TEST_ASSERT_FALSE(f.failed(false));  // the listener's: neither counted nor ending the row
  TEST_ASSERT_EQUAL_UINT8(2, f.inARow());
  TEST_ASSERT_TRUE(f.failed(true));  // the third: paused (the toast, once)
  TEST_ASSERT_TRUE(f.paused());
  TEST_ASSERT_FALSE(f.failed(true));  // (no second toast)
  TEST_ASSERT_TRUE(f.paused());
  f.played();  // any track that plays
  TEST_ASSERT_FALSE(f.paused());
  TEST_ASSERT_EQUAL_UINT8(0, f.inARow());
  f.failed(true);
  f.failed(true);
  f.played();  // not in a row any more
  TEST_ASSERT_FALSE(f.failed(true));
  TEST_ASSERT_FALSE(f.failed(true));
  TEST_ASSERT_TRUE(f.failed(true));
  f.clear();  // off and on
  TEST_ASSERT_FALSE(f.paused());
}

// ---------------------------------------------------------------------------
// The pieces together.
// ---------------------------------------------------------------------------
namespace {

class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > bytes.size()) bytes.resize(offset + n);
    if (n) std::memcpy(bytes.data() + offset, data, n);
    return true;
  }
  std::vector<uint8_t> bytes;
};

// The firmware's SlotReader: the open table's slot().
class TableReader : public autodj::SlotReader {
public:
  explicit TableReader(const mpdj::Table& t) : t_(t) {}
  mpdj::SlotRead read(uint16_t row, uint8_t* bytes, uint32_t* count, cc::Why* why) override {
    ++reads;
    return t_.slot(row, bytes, nullptr, count, why);
  }
  int reads = 0;

private:
  const mpdj::Table& t_;
};

constexpr int kRows = 16;

std::string relOf(int i) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "Generated Artist %02d/Sample Album %02d/%02d - Sample Title %02d.mp3", i / 4, i % 4,
                i, i);
  return buf;
}

// Sixteen made-up tracks around a circle (row i at 0.3 i radians: its
// neighbours are the rows beside it), four artists, a BPM and a key each,
// one card file each; a file for a track the index doesn't have.
struct Card {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  std::vector<uint8_t> file;
  std::unique_ptr<cc::MemSource> mem;
  mpdj::Table table;

  Card() {
    TEST_ASSERT_TRUE(index.begin("/music"));
    for (int i = 0; i < kRows; ++i)
      TEST_ASSERT_EQUAL_INT(0, static_cast<int>(index.addFile(("/music/" + relOf(i)).c_str())));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(index.addFile("/music/Loose Sample.flac")));
    TEST_ASSERT_TRUE(index.finish());
    std::vector<uint64_t> paths(kRows);
    std::vector<float> e(2 * kRows);
    std::vector<mpdj::RowIn> rows(kRows);
    for (int i = 0; i < kRows; ++i) {
      paths[i] = cc::pathHash(relOf(i).c_str());
      e[2 * i] = static_cast<float>(std::cos(0.3 * i));
      e[2 * i + 1] = static_cast<float>(std::sin(0.3 * i));
      rows[i].hash[0] = static_cast<uint8_t>(i + 1);  // (row i is rows[i]: canonical-hash order)
      rows[i].paths = &paths[i];
      rows[i].pathCount = 1;
      rows[i].bpm10 = static_cast<uint16_t>(1200 + 10 * i);
      rows[i].camelot = static_cast<uint8_t>(1 + i % 12);
      rows[i].artistKey = 0x1000u + static_cast<uint32_t>(i / 4);
      rows[i].embedding = &e[2 * i];
    }
    mpdj::TableIn t;
    t.generation = 3;
    t.k = 4;
    t.selKind = mpdj::kSelTop;
    t.modelId = "synthetic-embedding";
    t.modelVersion = "1";
    t.license = "CC0-1.0";
    t.dim = 2;
    t.rows = rows.data();
    t.rowCount = kRows;
    VecSink out;
    uint8_t sig[16];
    const char* error = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(mpdj::write(out, t, sig, nullptr, nullptr, &error), error ? error : "");
    file = out.bytes;
    mem.reset(new cc::MemSource(file.data(), static_cast<uint32_t>(file.size())));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(table.open(*mem)));
  }
  uint32_t trackOf(int i) const {
    const uint32_t t = index.findTrack(("/music/" + relOf(i)).c_str());
    TEST_ASSERT_TRUE(t != LibraryIndex::kNone);
    return t;
  }
  std::string pathOf(uint32_t track) const {
    char p[TrackCatalog::kMaxPath];
    catalog.path(track, p, sizeof(p));
    return p;
  }
};

// The rows a slot lists (its used entries).
std::set<uint16_t> listed(const mpdj::Table& t, uint16_t row) {
  std::vector<uint8_t> buf(t.slotBytes());
  std::vector<mpdj::Entry> e(t.k());
  uint32_t n = 0;
  TEST_ASSERT_TRUE(t.slot(row, buf.data(), e.data(), &n) == mpdj::SlotRead::Ok);
  std::set<uint16_t> out;
  for (uint32_t i = 0; i < n; ++i) out.insert(e[i].row);
  return out;
}

}  // namespace

void test_the_pieces_together() {
  Card card;
  // The join: the library's ids for the file's rows.
  autodj::Join join;
  TEST_ASSERT_TRUE(join.begin(card.table, &card.index) == autodj::Join::Status::Working);
  int slices = 0;
  while (join.status() == autodj::Join::Status::Working && slices < 1000) {
    join.step(&card.index);
    ++slices;
  }
  TEST_ASSERT_TRUE(join.status() == autodj::Join::Status::Ready);
  const autodj::Maps& maps = *join.maps();
  TEST_ASSERT_EQUAL_UINT32(kRows + 1, maps.tracks);
  TEST_ASSERT_EQUAL_UINT32(kRows, maps.rows);
  TEST_ASSERT_EQUAL_UINT32(kRows, maps.mapped);
  for (int i = 0; i < kRows; ++i) {
    TEST_ASSERT_EQUAL_UINT16(i, maps.trackToRow[card.trackOf(i)]);
    TEST_ASSERT_EQUAL_UINT16(card.trackOf(i), maps.rowToTrack[i]);
  }
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, maps.trackToRow[card.index.findTrack("/music/Loose Sample.flac")]);

  // The engine over the table's slots.
  TableReader reader(card.table);
  autodj::Engine engine;
  TEST_ASSERT_TRUE(engine.begin());
  autodj::Settings s;
  s.on = true;
  // The player, repeat Off (the firmware's default).
  QueueModel queue;
  Backend audio;
  PlaybackController player(audio, queue, card.catalog);
  player.setRepeat(PlaybackController::Repeat::Off);
  const uint32_t seed = card.trackOf(5);
  TEST_ASSERT_TRUE(player.playNow(&seed, 1, 0));
  engine.newSession(0x5EED0005u, seed, &maps);

  autodj::PickIn pin;
  pin.maps = &maps;
  pin.slots = &reader;
  pin.k = static_cast<uint16_t>(card.table.k());
  pin.selHead = static_cast<uint16_t>(card.table.selHead());
  pin.selFloor = static_cast<uint8_t>(card.table.selFloor());
  pin.libraryTracks = card.index.trackCount();

  // One loop pass of Auto DJ's step: a slot read ahead (one at most), the
  // trigger read off the player, a pick, its append. True: a track was
  // added.
  auto step = [&](bool on) {
    uint16_t ahead = autodj::kNoRow;
    if (engine.prefetchDue(s, &ahead)) TEST_ASSERT_TRUE(engine.prefetch(reader, ahead) == autodj::SlotResult::Ok);
    TriggerIn in;
    in.on = on;
    const TriggerOut o = decide(fromPlayer(player, in));
    if (!o.pick) return false;
    pin.playingTrack = player.currentTrack();
    const autodj::Pick p = engine.pick(s, pin);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_FALSE(p.random);
    TEST_ASSERT_TRUE(p.reads <= autodj::kReadsPerPick);
    TEST_ASSERT_TRUE(p.row != autodj::kNoRow);
    TEST_ASSERT_EQUAL_UINT16(p.row, maps.trackToRow[p.track]);
    TEST_ASSERT_EQUAL_UINT32(maps.rowToTrack[p.row], p.track);
    TEST_ASSERT_TRUE(p.track != player.currentTrack());
    // A neighbour of an anchor: listed in a slot of a track that played or was picked.
    bool near = false;
    for (uint8_t a = 0; a < engine.anchors(); ++a) near = near || listed(card.table, engine.anchor(a)).count(p.row);
    TEST_ASSERT_TRUE(near || p.dry);
    const uint32_t before = queue.size();
    TEST_ASSERT_TRUE(player.addFromAutoDj(p.track, o.cue));
    TEST_ASSERT_EQUAL_UINT32(before + 1, queue.size());
    TEST_ASSERT_EQUAL_UINT32(p.track, queue.trackAt(queue.size() - 1));
    return true;
  };

  // Ten tracks: each pick lands as the last entry starts, its word sent at
  // once; the track before ends and the pick plays.
  std::vector<uint32_t> played{seed};
  for (int round = 0; round < 10; ++round) {
    TEST_ASSERT_TRUE(step(true));
    TEST_ASSERT_FALSE(step(true));  // (one: the playing entry isn't last any more)
    const uint32_t pick = queue.trackAt(queue.size() - 1);
    TEST_ASSERT_FALSE(audio.nexts.empty());
    TEST_ASSERT_EQUAL_STRING(card.pathOf(pick).c_str(), audio.nexts.back().path.c_str());
    TEST_ASSERT_TRUE(audio.nexts.back().token != 0);
    audio.ended = true;
    player.update(0);
    TEST_ASSERT_EQUAL_INT(PlayState::Playing, player.state());
    TEST_ASSERT_EQUAL_UINT32(pick, player.currentTrack());
    TEST_ASSERT_EQUAL_STRING(card.pathOf(pick).c_str(), audio.lastPath.c_str());
    engine.trackStarted(pick, &maps, true);
    TEST_ASSERT_TRUE(pick != played.back());
    played.push_back(pick);
  }
  TEST_ASSERT_EQUAL_UINT32(11, queue.size());
  TEST_ASSERT_EQUAL_INT(10, player.currentIndex());
  // The Play's undo is still the Play's: no pick took it.
  TEST_ASSERT_TRUE(queue.undoable() == QueueModel::Edit::Replace);
  TEST_ASSERT_EQUAL_UINT32(10, engine.stats().picks);
  TEST_ASSERT_EQUAL_UINT32(0, engine.stats().dry);  // (the neighbours pass the 55 % gate)
  TEST_ASSERT_TRUE(engine.stats().prefetchReads > 0);

  // The pick up next removed: the playing entry is last again, and another
  // is picked (the removed one stays in the cooldown).
  TEST_ASSERT_TRUE(step(true));
  const uint32_t removed = queue.trackAt(queue.size() - 1);
  const uint32_t last = queue.size() - 1;
  TEST_ASSERT_EQUAL_UINT32(1, player.remove(&last, 1).count);
  TEST_ASSERT_TRUE(engine.rowCooled(maps.trackToRow[removed]));
  TEST_ASSERT_TRUE(step(true));
  TEST_ASSERT_TRUE(queue.trackAt(queue.size() - 1) != removed);
  TEST_ASSERT_TRUE(queue.undoable() == QueueModel::Edit::Remove);  // (the listener's remove, still undoable)

  // Behind the fence: nothing.
  TEST_ASSERT_TRUE(player.clearUpNext());
  player.setFenced(true);
  TEST_ASSERT_FALSE(step(true));
  TEST_ASSERT_FALSE(player.addFromAutoDj(card.trackOf(0), false));
  player.setFenced(false);

  // Off through the last track: the queue finishes; switched on, a pick is
  // cued, paused at 0:00; headphone Play leaves it; Play starts it.
  TEST_ASSERT_FALSE(step(false));
  audio.ended = true;
  player.update(0);
  TEST_ASSERT_EQUAL_INT(PlayState::Stopped, player.state());
  TEST_ASSERT_TRUE(player.endedAtEnd());
  const int plays = audio.plays;
  TEST_ASSERT_TRUE(step(true));
  TEST_ASSERT_EQUAL_INT(PlayState::Paused, player.state());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(queue.size()) - 1, player.currentIndex());
  TEST_ASSERT_TRUE(player.cuedByAutoDj());
  TEST_ASSERT_EQUAL_INT(plays, audio.plays);  // nothing started
  TEST_ASSERT_FALSE(step(true));               // (no second pick for the cued one)
  TEST_ASSERT_TRUE(HeadsetKeys::apply(player, HeadsetKeys::Key::Play) == HeadsetKeys::Action::Ignore);
  player.togglePlayPause();
  TEST_ASSERT_EQUAL_INT(PlayState::Playing, player.state());
  TEST_ASSERT_EQUAL_STRING(card.pathOf(queue.currentTrack()).c_str(), audio.lastPath.c_str());
  TEST_ASSERT_FALSE(player.cuedByAutoDj());
  TEST_ASSERT_TRUE(step(true));  // and it goes on
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_condition);
  RUN_TEST(test_the_first_condition_that_fails);
  RUN_TEST(test_a_finished_queue);
  RUN_TEST(test_the_holds_shown);
  RUN_TEST(test_owed_only_while_joining);
  RUN_TEST(test_the_failure_pause);
  RUN_TEST(test_the_pieces_together);
  return UNITY_END();
}
