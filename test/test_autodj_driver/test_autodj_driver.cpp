// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for Auto DJ's loop step (lib/core/AutoDjDriver: the
// firmware glue's portable part, the engine design's M4) and the console's
// D (lib/core/AutoDjCommand):
// - the D command's every form, and what it refuses;
// - the DJ marks across a library rebuild's read-back (MarkCarry);
// - sessions: switched on (no seed track, no pin), the boot's, every new
//   queue (a Play, Shuffle all, a Clear, the undo of either) seeded by its
//   current track, the other edits keeping it; Ds's seed, once;
// - the current entry: a track steers once it has started (0.5 s played,
//   the backend on it), a quick skip leaves the ring, a natural end or a
//   later skip doesn't; a pick taken out before it played leaves it too;
// - the pick: appended at the last entry, DJ-marked, the listener's undo
//   kept, and said when it went (a snapshot at the cap); cued at a finished
//   queue (nothing starts); the holds; a refused pick tried again later;
// - the failure pause fed: three of Auto DJ's picks failing in a row, a
//   listener's track that plays ends it;
// - the fence: nothing behind it, the marks, the current entry and the
//   pick up next carried by position after the read-back;
// - the index arriving after a session began; "Auto DJ from here" and the
//   empty states' button (joined, and before a join: tracks looked up by
//   their path's hash in DJPH, which agrees with the join); Dp; Dn's dry
//   run on a copy (the live session untouched);
// - a due pick waiting while the decoder refills the ring from empty; a
//   quick skip's row (none for a file the index doesn't have);
// - a slot whose CRC fails again at a pick: the pass names its row, once.
// An AutoDJ index written with mpdj::write for a made-up library, joined
// (AutoDjJoin), its slots read through mpdj::Table::slot(), and a real
// PlaybackController over a small backend.
// Run: pio test -e native -f test_autodj_driver
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "AutoDjCommand.h"
#include "AutoDjDriver.h"
#include "AutoDjEngine.h"
#include "AutoDjJoin.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using autodj::Began;
using autodj::Command;
using autodj::Driver;
using autodj::Pass;

namespace {

// ---------------------------------------------------------------------------
// The rig
// ---------------------------------------------------------------------------

// A backend whose starts, position and ends the test drives.
class Backend : public IAudioBackend {
public:
  std::string lastPath;
  int plays = 0;
  uint32_t seq = 0;
  bool playing = false, paused = false, ended = false, failing = false;
  bool failEverything = false;
  uint32_t position = 0;
  bool play(const std::string& p, uint32_t, uint32_t startMs) override {
    lastPath = p;
    ++plays;
    ++seq;
    playing = true;
    paused = false;
    ended = false;
    failing = failEverything;
    position = startMs;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override { playing = paused = false; }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused && !failing; }
  uint32_t positionMs() const override { return position; }
  bool finished() const override { return ended; }
  bool failed() const override { return failing; }
};

class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > bytes.size()) bytes.resize(offset + n);
    if (n) std::memcpy(bytes.data() + offset, data, n);
    return true;
  }
  std::vector<uint8_t> bytes;
};

class TableReader : public autodj::SlotReader {
public:
  explicit TableReader(const mpdj::Table& t) : t_(t) {}
  mpdj::SlotRead read(uint16_t row, uint8_t* bytes, uint32_t* count, cc::Why* why) override {
    ++reads;
    if (fail) {  // (the card pulled: the read fails)
      if (why) *why = cc::Why::Io;
      return mpdj::SlotRead::Damaged;
    }
    if (crc.count(row)) {  // (the slot's bytes changed: its CRC fails)
      if (why) *why = cc::Why::DjSlotCrc;
      return mpdj::SlotRead::Damaged;
    }
    return t_.slot(row, bytes, nullptr, count, why);
  }
  int reads = 0;
  bool fail = false;
  std::set<uint16_t> crc;

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

// Sixteen made-up tracks around a circle (row i at 0.3 i radians), four
// artists, a BPM and a key each, one card file each, and a file the index
// doesn't have.
struct Card {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  std::vector<uint8_t> file;
  std::unique_ptr<cc::MemSource> mem;
  mpdj::Table table;
  autodj::Join join;

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
      rows[i].hash[0] = static_cast<uint8_t>(i + 1);
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
    TEST_ASSERT_TRUE(join.begin(table, &index) == autodj::Join::Status::Working);
    for (int s = 0; s < 1000 && join.status() == autodj::Join::Status::Working; ++s) join.step(&index);
    TEST_ASSERT_TRUE(join.status() == autodj::Join::Status::Ready);
  }
  uint32_t trackOf(int i) const {
    const uint32_t t = index.findTrack(("/music/" + relOf(i)).c_str());
    TEST_ASSERT_TRUE(t != LibraryIndex::kNone);
    return t;
  }
  uint32_t loose() const { return index.findTrack("/music/Loose Sample.flac"); }
  uint16_t rowOf(uint32_t track) const { return join.maps()->trackToRow[track]; }
};

uint32_t s_random = 0x5EED0001u;
uint32_t fixedRandom() { return s_random++; }
uint64_t s_us = 0;
uint64_t clockUs() { return s_us += 7; }

Driver::Config config() {
  Driver::Config c;
  c.random = fixedRandom;
  c.nowUs = clockUs;
  return c;
}

struct Rig {
  Card card;
  TableReader reader{card.table};
  QueueModel queue;
  Backend audio;
  PlaybackController player{audio, queue, card.catalog};
  Driver dj{config()};
  autodj::Index ix;
  autodj::Env env;
  bool indexed = true;  // the index joined (else: picks by id, or `joining`)
  bool joining = false;

  Rig() {
    TEST_ASSERT_TRUE(dj.begin());
    player.setRepeat(PlaybackController::Repeat::Off);
    env.libraryTracks = card.index.trackCount();
  }
  autodj::Index index() {
    autodj::Index i;
    if (indexed) {
      i.maps = card.join.maps();
      i.slots = &reader;
      i.k = static_cast<uint16_t>(card.table.k());
      i.selHead = static_cast<uint16_t>(card.table.selHead());
      i.selFloor = static_cast<uint8_t>(card.table.selFloor());
    }
    i.joining = joining;
    return i;
  }
  // One loop pass: the player's update, then Auto DJ's step.
  Pass pass() {
    env.nowMs += 20;
    player.update(env.nowMs);
    env.audioPlaying = audio.isPlaying();
    env.trackSeq = audio.seq;
    env.positionMs = audio.position;
    return dj.step(player, index(), env);
  }
  // The track plays on to `ms` (its position), a pass.
  Pass playTo(uint32_t ms) {
    audio.position = ms;
    return pass();
  }
  // The current track ends; the passes after it.
  Pass end() {
    audio.ended = true;
    return pass();
  }
  void playOne(uint32_t track) { TEST_ASSERT_TRUE(player.playNow(&track, 1, 0)); }
  autodj::Settings on() {
    autodj::Settings s;
    s.on = true;
    return s;
  }
  bool inRing(uint16_t row) const {
    for (uint8_t i = 0; i < dj.engine().anchors(); ++i)
      if (dj.engine().anchor(i) == row) return true;
    return false;
  }
  // The library update's fence and its read-back, as main.cpp's
  // enterFence() and leaveFence() do them (queueremap::Carry::finish()'s
  // order): the same entries with fresh keys; a pass behind the fence.
  void fenceAndReadBack() {
    dj.fenceUp(queue);
    std::vector<uint32_t> tracks;
    for (uint32_t i = 0; i < queue.size(); ++i) tracks.push_back(queue.trackAt(i));
    const int32_t cur = queue.current();
    player.setFenced(true);
    queue.release();
    pass();
    player.setFenced(false);
    TEST_ASSERT_TRUE(queue.assign(tracks.data(), static_cast<uint32_t>(tracks.size()), cur));
    player.queueReplaced(true);
    dj.remapped(queue);
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The command
// ---------------------------------------------------------------------------
void test_the_command_parses_every_form() {
  using K = Command::Kind;
  struct Case {
    const char* text;
    K kind;
    uint32_t n;
  };
  const Case cases[] = {
      {"", K::Status, 0},          {"  ", K::Status, 0},        {"0", K::Off, 0},          {"1", K::On, 0},
      {" 1 ", K::On, 0},           {"m0", K::Mode, 0},          {"m1", K::Mode, 1},        {"f55", K::Floor, 55},
      {"f60", K::Floor, 60},       {"f75", K::Floor, 75},       {"t0", K::Tempo, 0},       {"t1", K::Tempo, 1},
      {"k0", K::Key, 0},           {"k1", K::Key, 1},           {"s1", K::Seed, 1},
      {"sDEADbeef", K::Seed, 0xDEADBEEF},
      {"s0", K::Seed, 0},          {"r", K::Renew, 0},          {"p", K::Pick, 0},         {"n1", K::DryRun, 1},
      {"n200", K::DryRun, 200},    {"n2000", K::DryRun, 2000},  {"j", K::Join, 0},         {"c", K::Reopen, 0},
      {"b1", K::Bench, 1},         {"b200", K::Bench, 200},     {"b5000", K::Bench, 5000},
      {"h", K::FromHere, Command::kCurrent},
      {"h0", K::FromHere, 0},      {"h12", K::FromHere, 12},    {"e", K::Surprise, 0},     {"x", K::Stop, 0},
      {"?", K::Help, 0},
  };
  for (const Case& c : cases) {
    const Command got = autodj::parseCommand(c.text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(c.kind), static_cast<int>(got.kind), c.text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.n, got.n, c.text);
  }
  TEST_ASSERT_TRUE(autodj::parseCommand(nullptr).kind == K::Status);
  // Refused: a value out of range or not a step, a letter it doesn't take,
  // junk after a command.
  const char* bad[] = {"2",    "01",   "m",    "m2",  "m01", "f",    "f50",   "f56",   "f80",  "f055x", "t",
                       "t2",   "k",    "k10",  "s",   "sxyz", "s123456789", "n", "n0",    "n2001", "nx",  "b",
                       "b0",   "b5001", "rr",  "p1",  "j1",  "c0",   "e1",    "hx",    "h-1",  "x1",    "??",
                       "z",    "D",    "q",    "1 1",
                       // Ten digits and more: never wrapped round to a small number.
                       "h4294967296", "n4294967297", "b10000000000", "f0000000055", "h0000000012"};
  for (const char* b : bad) TEST_ASSERT_TRUE_MESSAGE(autodj::parseCommand(b).kind == K::Bad, b);
  TEST_ASSERT_TRUE(std::strlen(autodj::commandHelp()) > 100);
}

// ---------------------------------------------------------------------------
// The marks across a rebuild
// ---------------------------------------------------------------------------
void test_the_marks_follow_their_positions_across_a_read_back() {
  QueueModel q;
  const uint32_t tracks[] = {10, 11, 12, 13, 14, 15};
  TEST_ASSERT_TRUE(q.assign(tracks, 6, 2));
  autodj::Marks marks;
  marks.add(q.keyAt(3));
  marks.add(q.keyAt(5));
  autodj::MarkCarry carry;
  carry.fenceUp(q, marks);
  TEST_ASSERT_EQUAL_UINT32(2, carry.noted());
  // The read-back: the same entries with fresh keys.
  q.release();
  TEST_ASSERT_TRUE(q.assign(tracks, 6, 2));
  TEST_ASSERT_FALSE(marks.has(q.keyAt(3)));  // (the keys are new)
  carry.remapped(q, &marks);
  TEST_ASSERT_EQUAL_INT(2, marks.count());
  TEST_ASSERT_TRUE(marks.has(q.keyAt(3)));
  TEST_ASSERT_TRUE(marks.has(q.keyAt(5)));
  TEST_ASSERT_FALSE(marks.has(q.keyAt(4)));
  // A track gone from the card: the positions mean nothing, none marked.
  carry.fenceUp(q, marks);
  q.release();
  TEST_ASSERT_TRUE(q.assign(tracks, 5, 2));
  carry.remapped(q, &marks);
  TEST_ASSERT_EQUAL_INT(0, marks.count());
  // remapped() without a fence before it changes nothing.
  marks.add(q.keyAt(1));
  carry.remapped(q, &marks);
  TEST_ASSERT_EQUAL_INT(1, marks.count());
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

// Off: nothing picked, no session for a Play. On: a session with no seed
// track (2.1: no pin); a new queue (a Play) starts one seeded by its
// current track, which steers and is Stay on seed's pin.
void test_sessions_begin_at_on_and_at_every_new_queue() {
  Rig r;
  r.playOne(r.card.trackOf(5));
  Pass p = r.playTo(1000);
  TEST_ASSERT_FALSE(p.picked);
  TEST_ASSERT_TRUE(p.trigger.wait == autodj::Wait::Off);
  TEST_ASSERT_EQUAL_UINT32(0, r.dj.sessions());
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  TEST_ASSERT_FALSE(r.dj.setOn(true));  // (no change)
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.sessions());
  TEST_ASSERT_TRUE(r.dj.sessionBegan() == Began::On);
  TEST_ASSERT_FALSE(r.dj.engine().seeded());
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, r.dj.engine().pin());
  // The playing track is the queue's last: the first pick in this pass,
  // seeded by it (the engine's first pick).
  p = r.pass();
  TEST_ASSERT_TRUE(p.picked && p.added);
  TEST_ASSERT_TRUE(r.dj.engine().seeded());
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(5));
  TEST_ASSERT_EQUAL_UINT32(2, r.queue.size());
  // A Play: a new session, seeded by its current track (in the ring, and
  // Stay on seed's pin).
  r.dj.setMode(autodj::Mode::Locked);
  const uint32_t album[] = {r.card.trackOf(9), r.card.trackOf(10)};
  const uint32_t skips = r.dj.engine().stats().quickSkips;
  TEST_ASSERT_TRUE(r.player.playNow(album, 2, 0));
  p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::NewQueue);
  // (The track the Play replaced 1 s in is no quick skip: a new queue.)
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(skips, r.dj.engine().stats().quickSkips);
  TEST_ASSERT_EQUAL_UINT32(2, r.dj.sessions());
  TEST_ASSERT_EQUAL_UINT16(9, r.dj.engine().pin());
  TEST_ASSERT_EQUAL_UINT8(1, r.dj.engine().anchors());
  TEST_ASSERT_EQUAL_UINT16(9, r.dj.engine().anchor(0));
  TEST_ASSERT_FALSE(r.dj.engine().rowCooled(5));  // (the old session's)
  TEST_ASSERT_FALSE(p.picked);                     // (one up next)
  // Kept by the other edits: Play next, + Queue, a remove, their undo.
  const uint32_t one = r.card.trackOf(3);
  TEST_ASSERT_TRUE(r.player.playNext(&one, 1));
  TEST_ASSERT_TRUE(r.player.addToQueue(&one, 1));
  const uint32_t at = 1;
  TEST_ASSERT_EQUAL_UINT32(1, r.player.remove(&at, 1).count);
  TEST_ASSERT_TRUE(r.player.undo());
  p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::None);
  TEST_ASSERT_EQUAL_UINT32(2, r.dj.sessions());
  // A Clear: a new session with nothing to seed it.
  r.player.clearQueue();
  p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::NewQueue);
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_FALSE(r.dj.engine().seeded());
  TEST_ASSERT_TRUE(p.trigger.wait == autodj::Wait::Empty);
  // Its undo: the queue back, a new session seeded by its current track.
  TEST_ASSERT_TRUE(r.player.undo());
  p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::NewQueue);
  TEST_ASSERT_TRUE(r.dj.engine().seeded());
  TEST_ASSERT_EQUAL_UINT16(r.card.rowOf(r.queue.currentTrack()), r.dj.engine().pin());
  // Shuffle all's Play and its undo: each a new queue.
  const uint32_t sessions = r.dj.sessions();
  const LibraryIndex::Span all = r.card.index.allTracks();
  TEST_ASSERT_TRUE(r.player.playNow(all.ids, all.count, PlaybackController::kAnyStart, true));
  r.pass();
  TEST_ASSERT_TRUE(r.player.undo());
  r.pass();
  TEST_ASSERT_EQUAL_UINT32(sessions + 2, r.dj.sessions());
  // Off: a Play starts none.
  TEST_ASSERT_TRUE(r.dj.setOn(false));
  r.playOne(r.card.trackOf(1));
  p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::None);
  TEST_ASSERT_EQUAL_UINT32(sessions + 2, r.dj.sessions());
}

// Ds's seed is the next session's, once; the others are the hook's. The
// boot's settings: on starts a session (no seed track).
void test_the_seed_and_the_boot() {
  Rig r;
  r.dj.setSeed(0xABCD1234u);
  TEST_ASSERT_TRUE(r.dj.seedSet());
  r.dj.newSession(Began::Console, autodj::kNoTrackId, nullptr);
  TEST_ASSERT_EQUAL_HEX32(0xABCD1234u, r.dj.sessionSeed());
  TEST_ASSERT_FALSE(r.dj.seedSet());
  r.dj.newSession(Began::Console, autodj::kNoTrackId, nullptr);
  TEST_ASSERT_TRUE(r.dj.sessionSeed() != 0xABCD1234u);
  Rig b;
  autodj::Settings s;
  s.on = true;
  s.mode = autodj::Mode::Locked;
  s.floorPct = 65;
  s.matchKey = true;
  b.dj.load(s);
  TEST_ASSERT_TRUE(b.dj.settings().on);
  TEST_ASSERT_TRUE(b.dj.sessionBegan() == Began::Boot);
  TEST_ASSERT_FALSE(b.dj.engine().seeded());
  TEST_ASSERT_EQUAL_UINT8(65, b.dj.settings().floorPct);
  s.floorPct = 62;  // (not a step: the default)
  Rig c;
  c.dj.load(s);
  TEST_ASSERT_EQUAL_UINT8(55, c.dj.settings().floorPct);
  TEST_ASSERT_FALSE(c.dj.setFloor(80));
  TEST_ASSERT_TRUE(c.dj.setFloor(70));
  TEST_ASSERT_FALSE(c.dj.setFloor(70));
  TEST_ASSERT_TRUE(c.dj.setMatch(true, false));
  TEST_ASSERT_FALSE(c.dj.setMatch(true, false));
  TEST_ASSERT_TRUE(c.dj.setMode(autodj::Mode::Rolling));
  TEST_ASSERT_FALSE(c.dj.setMode(autodj::Mode::Rolling));
}

// ---------------------------------------------------------------------------
// The current entry
// ---------------------------------------------------------------------------

// A track steers once it starts (current, the player Playing: before the
// pick it may be due in the same pass), not while paused, waiting or
// stopped; it plays (the failure pause's "starts successfully") once 0.5 s
// of it was heard. Skipped in its first 30 s (by the backend's position),
// not by its end, it leaves the ring and stays in the cooldown; played to
// its end, or skipped later, it stays.
void test_a_track_steers_once_started_and_a_quick_skip_leaves_the_ring() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const int rows[] = {2, 6, 10, 14, 1, 13};
  uint32_t tracks[6];
  for (int i = 0; i < 6; ++i) tracks[i] = r.card.trackOf(rows[i]);
  TEST_ASSERT_TRUE(r.queue.assign(tracks, 6, 0));  // (a restored queue: no new session)
  r.player.queueReplaced(false);
  Pass p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::None);
  TEST_ASSERT_FALSE(p.started);  // stopped
  r.player.togglePlayPause();
  p = r.playTo(200);
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_FALSE(p.playing);  // (under 0.5 s)
  TEST_ASSERT_EQUAL_UINT32(r.card.trackOf(2), p.startedTrack);
  TEST_ASSERT_FALSE(p.startedByAutoDj);
  TEST_ASSERT_EQUAL_UINT8(1, r.dj.engine().anchors());
  TEST_ASSERT_EQUAL_UINT16(2, r.dj.engine().anchor(0));
  p = r.playTo(600);
  TEST_ASSERT_FALSE(p.started);  // (once)
  TEST_ASSERT_TRUE(p.playing);
  // Its natural end at 5 s: no quick skip; the next starts and steers.
  r.playTo(5000);
  p = r.end();
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_EQUAL_UINT8(2, r.dj.engine().anchors());
  // Skipped at 10 s: a quick skip, out of the ring, still cooled; the
  // next one steers.
  r.playTo(10000);
  r.player.next();
  p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(r.card.trackOf(6), p.leftTrack);
  TEST_ASSERT_EQUAL_UINT32(10000, p.leftMs);
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(6));
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.engine().stats().quickSkips);
  bool six = false, ten = false;
  for (uint8_t i = 0; i < r.dj.engine().anchors(); ++i) {
    six = six || r.dj.engine().anchor(i) == 6;
    ten = ten || r.dj.engine().anchor(i) == 10;
  }
  TEST_ASSERT_FALSE(six);
  TEST_ASSERT_TRUE(ten);
  // Paused 0.3 s in, then skipped: a quick skip too (it started).
  r.playTo(300);
  r.player.togglePlayPause();
  r.pass();
  r.player.next();
  p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(300, p.leftMs);
  // Stopped, a new entry doesn't start: nothing steers until Play.
  r.player.stop();
  r.player.cueNext();  // (stopped: stays stopped)
  p = r.pass();
  TEST_ASSERT_FALSE(p.started);
  r.player.togglePlayPause();
  p = r.pass();
  TEST_ASSERT_TRUE(p.started);
  // Skipped at 45 s: it stays in the ring.
  r.playTo(45000);
  const uint16_t row = r.card.rowOf(r.queue.currentTrack());
  r.player.next();
  p = r.pass();
  TEST_ASSERT_FALSE(p.quickSkip);
  bool kept = false;
  for (uint8_t i = 0; i < r.dj.engine().anchors(); ++i) kept = kept || r.dj.engine().anchor(i) == row;
  TEST_ASSERT_TRUE(kept);
}

// The queue's end: the last track played to its end stays the current
// entry; Auto DJ switched on cues a pick there, and the track that ended
// isn't taken for a quick skip when the cue moves on from it.
void test_a_finished_queue_gets_a_cued_pick_and_no_quick_skip() {
  Rig r;
  r.playOne(r.card.trackOf(4));
  r.playTo(800);
  r.playTo(9000);
  r.end();
  TEST_ASSERT_TRUE(r.player.endedAtEnd());
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  // (its session seeds from the playing track at the first pick)
  const int plays = r.audio.plays;
  Pass p = r.pass();
  TEST_ASSERT_TRUE(p.picked && p.added && p.cued);
  TEST_ASSERT_EQUAL_INT(PlayState::Paused, r.player.state());
  TEST_ASSERT_TRUE(r.player.cuedByAutoDj());
  TEST_ASSERT_EQUAL_INT(plays, r.audio.plays);  // nothing started
  TEST_ASSERT_EQUAL_UINT32(p.key, r.queue.currentKey());
  TEST_ASSERT_TRUE(r.dj.marks().has(p.key));
  p = r.pass();
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_FALSE(p.picked);  // (the cued pick is last, paused: Idle)
  TEST_ASSERT_TRUE(p.trigger.wait == autodj::Wait::Idle);
}

// ---------------------------------------------------------------------------
// The pick
// ---------------------------------------------------------------------------

// The pick lands as the last entry plays: appended, DJ-marked, the
// listener's last edit still undoable; a session of picks; reads ahead one
// a pass, none while a list moves.
void test_picks_are_appended_marked_and_keep_the_undo() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const uint32_t two[] = {r.card.trackOf(7), r.card.trackOf(8)};
  TEST_ASSERT_TRUE(r.player.playNow(two, 2, 0));
  r.playTo(600);
  const uint32_t extra = r.card.trackOf(12);
  TEST_ASSERT_TRUE(r.player.addToQueue(&extra, 1));
  const uint32_t at = 2;
  TEST_ASSERT_EQUAL_UINT32(1, r.player.remove(&at, 1).count);  // the listener's Remove
  Pass p = r.end();  // the last entry starts: the pick in the same pass
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_TRUE(p.picked && p.added);
  TEST_ASSERT_FALSE(p.cued);
  TEST_ASSERT_FALSE(p.undoDropped);
  TEST_ASSERT_TRUE(r.queue.undoable() == QueueModel::Edit::Remove);  // still the listener's
  TEST_ASSERT_EQUAL_UINT32(3, r.queue.size());
  TEST_ASSERT_EQUAL_UINT32(p.pick.track, r.queue.trackAt(2));
  TEST_ASSERT_EQUAL_UINT32(r.queue.keyAt(2), p.key);
  TEST_ASSERT_TRUE(r.dj.marks().has(p.key));
  TEST_ASSERT_FALSE(r.dj.marks().has(r.queue.keyAt(1)));
  TEST_ASSERT_TRUE(p.pickUs > 0);
  TEST_ASSERT_TRUE(p.readUs <= p.pickUs);
  Pass last;
  TEST_ASSERT_TRUE(r.dj.lastPick(&last));
  TEST_ASSERT_EQUAL_UINT32(p.key, last.key);
  // A session of picks: each lands as the last starts; the slot read ahead
  // between them; the pick itself then reads nothing.
  for (int i = 0; i < 6; ++i) {
    Pass a = r.playTo(700);
    TEST_ASSERT_FALSE(a.picked);
    for (int j = 0; j < 3; ++j) {
      a = r.pass();
      if (a.prefetched) TEST_ASSERT_TRUE(a.prefetch == autodj::SlotResult::Ok);
    }
    a = r.end();
    TEST_ASSERT_TRUE(a.picked && a.added);
    TEST_ASSERT_TRUE(a.startedByAutoDj);
    TEST_ASSERT_TRUE(a.pick.reads <= autodj::kReadsPerPick);
  }
  TEST_ASSERT_TRUE(r.dj.engine().stats().prefetchReads > 0);
  TEST_ASSERT_TRUE(r.dj.engine().stats().readsAtPick[0] > 0);
  // No read ahead while a list moves, or the card is held (the pick just
  // made is due one).
  r.env.listMoving = true;
  for (int i = 0; i < 5; ++i) TEST_ASSERT_FALSE(r.pass().prefetched);
  r.env.listMoving = false;
  r.env.cardHeld = true;
  for (int i = 0; i < 5; ++i) TEST_ASSERT_FALSE(r.pass().prefetched);
  r.env.cardHeld = false;
  Pass a = r.pass();
  TEST_ASSERT_TRUE(a.prefetched);
  TEST_ASSERT_TRUE(a.prefetch == autodj::SlotResult::Ok);
}

// A read ahead that failed (the card pulled): none again for
// kReadAgainMs, and never a damaged row for it.
void test_a_failed_read_ahead_waits() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  TEST_ASSERT_TRUE(r.playTo(700).added);  // (its row joined the ring, unread)
  r.reader.fail = true;
  Pass a = r.pass();
  TEST_ASSERT_TRUE(a.prefetched);
  TEST_ASSERT_TRUE(a.prefetch == autodj::SlotResult::Io);
  const int reads = r.reader.reads;
  for (int i = 0; i < 50; ++i) TEST_ASSERT_FALSE(r.pass().prefetched);  // (1 s)
  TEST_ASSERT_EQUAL_INT(reads, r.reader.reads);
  r.reader.fail = false;
  r.env.nowMs += autodj::kReadAgainMs;
  a = r.pass();
  TEST_ASSERT_TRUE(a.prefetched);
  TEST_ASSERT_TRUE(a.prefetch == autodj::SlotResult::Ok);
  TEST_ASSERT_EQUAL_UINT8(0, r.dj.engine().damagedRows());
  TEST_ASSERT_TRUE(r.dj.engine().stats().ioSkips >= 1);
}

// A slot whose CRC fails on the read ahead and again when the next pick
// reads it (the device check of 2026-10-15: an album's slots changed on
// the card): the read ahead's pass says its first failure (Suspect), the
// pick's pass names the row it put in the damaged ring (the glue's "read
// at the pick, row R: DAMAGED" line), and no later pass names it again:
// it isn't read again.
void test_a_slot_damaged_at_the_pick_is_named_once() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  Pass a = r.playTo(700);
  TEST_ASSERT_TRUE(a.added);  // (its row joined the ring, unread)
  const uint16_t row = a.pick.row;
  TEST_ASSERT_TRUE(row != autodj::kNoRow);
  TEST_ASSERT_EQUAL_UINT8(0, a.pick.damaged);
  r.reader.crc.insert(row);
  a = r.pass();
  TEST_ASSERT_TRUE(a.prefetched);
  TEST_ASSERT_EQUAL_UINT16(row, a.prefetchRow);
  TEST_ASSERT_TRUE(a.prefetch == autodj::SlotResult::Suspect);
  TEST_ASSERT_FALSE(r.dj.engine().rowDamaged(row));
  a = r.end();  // the pick starts, the last entry: the next pick reads its slot
  TEST_ASSERT_TRUE(a.picked && a.added);
  TEST_ASSERT_EQUAL_UINT8(1, a.pick.reads);
  TEST_ASSERT_EQUAL_UINT8(1, a.pick.damaged);
  TEST_ASSERT_EQUAL_UINT16(row, a.pick.damagedRow[0]);
  TEST_ASSERT_FALSE(a.pick.random);  // (the seed's slot, cached, still anchors)
  TEST_ASSERT_TRUE(r.dj.engine().rowDamaged(row));
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.engine().stats().damaged);
  const int reads = r.reader.reads;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 5; ++j) {
      a = r.playTo(700);  // (it plays: no failure pause)
      TEST_ASSERT_FALSE(a.prefetched && a.prefetchRow == row);
    }
    a = r.end();
    TEST_ASSERT_TRUE(a.picked && a.added);
    TEST_ASSERT_EQUAL_UINT8(0, a.pick.damaged);
  }
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.engine().stats().damaged);
  TEST_ASSERT_TRUE(r.reader.reads > reads);  // (the new picks' slots: read ahead or at the pick)
  // Read twice at one pick (no read ahead between): named the same way.
  const uint16_t next = a.pick.row;
  TEST_ASSERT_TRUE(next != row);
  r.reader.crc.insert(next);
  a = r.end();
  TEST_ASSERT_TRUE(a.picked);
  TEST_ASSERT_EQUAL_UINT8(2, a.pick.reads);
  TEST_ASSERT_EQUAL_UINT8(1, a.pick.damaged);
  TEST_ASSERT_EQUAL_UINT16(next, a.pick.damagedRow[0]);
  TEST_ASSERT_EQUAL_UINT32(2, r.dj.engine().stats().damaged);
}

// A snapshot at the cap: Auto DJ's append drops the listener's undo, and
// the pass says so (Ui::queueUndoDropped(): the toast's Undo goes).
void test_the_undo_it_drops_is_said() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  std::vector<uint32_t> tracks(QueueModel::kMaxEntries);
  for (uint32_t i = 0; i < tracks.size(); ++i) tracks[i] = r.card.trackOf(static_cast<int>(i % kRows));
  TEST_ASSERT_TRUE(r.queue.assign(tracks.data(), QueueModel::kMaxEntries, QueueModel::kMaxEntries - 2));
  r.player.queueReplaced(false);
  r.player.play(QueueModel::kMaxEntries - 2);
  r.playTo(700);
  const uint32_t last = QueueModel::kMaxEntries - 1;
  TEST_ASSERT_EQUAL_UINT32(1, r.player.remove(&last, 1).count);  // its snapshot: 5,000 entries
  TEST_ASSERT_TRUE(r.queue.undoable() == QueueModel::Edit::Remove);
  const Pass p = r.pass();
  TEST_ASSERT_TRUE(p.added);
  TEST_ASSERT_TRUE(p.undoDropped);
  TEST_ASSERT_TRUE(r.queue.undoable() == QueueModel::Edit::None);
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.undoDrops());
}

// Auto DJ's pick taken out before it played leaves the ring (a skip at
// 0:00), stays cooled; another is picked.
void test_a_pick_removed_before_it_played_leaves_the_ring() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(3));
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.added);
  const uint16_t row = r.card.rowOf(p.pick.track);
  bool inRing = false;
  for (uint8_t i = 0; i < r.dj.engine().anchors(); ++i) inRing = inRing || r.dj.engine().anchor(i) == row;
  TEST_ASSERT_TRUE(inRing);  // (joined when picked)
  const uint32_t at = 1;
  TEST_ASSERT_EQUAL_UINT32(1, r.player.remove(&at, 1).count);
  p = r.pass();
  TEST_ASSERT_TRUE(p.removedPick);
  TEST_ASSERT_EQUAL_UINT32(r.card.rowOf(p.removedTrack), row);
  inRing = false;
  for (uint8_t i = 0; i < r.dj.engine().anchors(); ++i) inRing = inRing || r.dj.engine().anchor(i) == row;
  TEST_ASSERT_FALSE(inRing);
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(row));
  TEST_ASSERT_TRUE(p.added);  // the playing entry is last again: another pick, this pass
  TEST_ASSERT_TRUE(r.card.rowOf(p.pick.track) != row);
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.removedPicks());
}

// The holds: Repeat One and the sleep timer's End of queue add nothing
// (counted once each); behind the fence, nothing; a card held, nothing.
void test_the_holds_add_nothing() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  r.player.setRepeat(PlaybackController::Repeat::One);
  TEST_ASSERT_FALSE(r.playTo(700).picked);
  for (int i = 0; i < 3; ++i) {
    const Pass p = r.pass();
    TEST_ASSERT_FALSE(p.picked);
    TEST_ASSERT_TRUE(p.trigger.hold == autodj::Hold::RepeatOne);
  }
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.holds());
  r.player.setRepeat(PlaybackController::Repeat::Off);
  r.env.sleepEndOfQueue = true;
  TEST_ASSERT_FALSE(r.pass().picked);
  TEST_ASSERT_EQUAL_UINT32(2, r.dj.holds());
  r.env.sleepEndOfQueue = false;
  r.env.cardHeld = true;
  TEST_ASSERT_FALSE(r.pass().picked);
  r.env.cardHeld = false;
  r.joining = true;
  Pass p = r.pass();
  TEST_ASSERT_FALSE(p.picked);
  TEST_ASSERT_TRUE(p.trigger.owed);
  TEST_ASSERT_TRUE(r.dj.owed());
  r.joining = false;
  p = r.pass();
  TEST_ASSERT_TRUE(p.added);
  TEST_ASSERT_FALSE(r.dj.owed());
}

// A pick that gives no track (no index, no library) is tried again after
// kRetryMs, not every pass.
void test_a_refused_pick_is_tried_again_later() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.indexed = false;
  r.env.libraryTracks = 0;
  r.playOne(r.card.trackOf(5));
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.picked);
  TEST_ASSERT_TRUE(p.refused);
  TEST_ASSERT_FALSE(p.pick.ok);
  for (int i = 0; i < 100; ++i) TEST_ASSERT_FALSE(r.pass().picked);  // 2 s
  TEST_ASSERT_TRUE(r.dj.retryWaits(r.env.nowMs));
  r.env.libraryTracks = r.card.index.trackCount();
  r.env.nowMs += autodj::kRetryMs;
  p = r.pass();
  TEST_ASSERT_TRUE(p.added);
  TEST_ASSERT_TRUE(p.pick.noIndex);  // (by id: no index)
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.refusals());
}

// ---------------------------------------------------------------------------
// The failure pause
// ---------------------------------------------------------------------------
void test_failing_picks_pause_auto_dj_and_a_track_that_plays_ends_it() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  TEST_ASSERT_TRUE(r.playTo(700).added);  // the first pick
  TEST_ASSERT_EQUAL_UINT32(2, r.queue.size());
  // From here every track fails: each pick plays, fails, and the next is
  // picked as it becomes last; the third failure pauses Auto DJ.
  r.audio.failEverything = true;
  r.end();
  bool paused = false;
  int failures = 0;
  for (int i = 0; i < 20 && !paused; ++i) {
    const Pass p = r.pass();
    if (p.failed) {
      ++failures;
      TEST_ASSERT_TRUE(p.failedPick);
    }
    paused = p.paused;
  }
  TEST_ASSERT_TRUE(paused);
  TEST_ASSERT_EQUAL_INT(3, failures);
  TEST_ASSERT_TRUE(r.dj.failurePause().paused());
  const uint32_t size = r.queue.size();
  for (int i = 0; i < 10; ++i) {
    const Pass p = r.pass();
    TEST_ASSERT_FALSE(p.added);
  }
  TEST_ASSERT_TRUE(r.queue.size() == size);
  // The listener plays a track that plays: the pause is over.
  r.audio.failEverything = false;
  r.playOne(r.card.trackOf(11));
  r.playTo(700);
  TEST_ASSERT_FALSE(r.dj.failurePause().paused());
}

// Off and on clears the failure pause.
void test_off_and_on_clears_the_failure_pause() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  TEST_ASSERT_TRUE(r.playTo(700).added);
  r.audio.failEverything = true;
  r.end();
  for (int i = 0; i < 20 && !r.dj.failurePause().paused(); ++i) r.pass();
  TEST_ASSERT_TRUE(r.dj.failurePause().paused());
  TEST_ASSERT_TRUE(r.dj.setOn(false));
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  TEST_ASSERT_FALSE(r.dj.failurePause().paused());
}

// ---------------------------------------------------------------------------
// The fence
// ---------------------------------------------------------------------------

// Behind the fence nothing is read or added; after the read-back (fresh
// keys) the marks, the current entry and the pick up next are the same
// entries: no quick skip, no removed pick, no new session.
void test_the_fence_and_the_read_back() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.added);
  const uint32_t sessions = r.dj.sessions();
  const uint32_t pickPos = 1;
  // The fence (main.cpp's enterFence(): noted, then the queue given away).
  r.dj.fenceUp(r.queue);
  std::vector<uint32_t> tracks;
  for (uint32_t i = 0; i < r.queue.size(); ++i) tracks.push_back(r.queue.trackAt(i));
  const int32_t cur = r.queue.current();
  r.player.setFenced(true);
  r.queue.release();
  for (int i = 0; i < 5; ++i) {
    p = r.pass();
    TEST_ASSERT_FALSE(p.picked || p.prefetched || p.quickSkip || p.removedPick);
    // (The fence's wait, not the released queue's emptiness: D's status.)
    TEST_ASSERT_TRUE(p.trigger.wait == autodj::Wait::Fenced);
    TEST_ASSERT_TRUE(r.dj.lastTrigger().wait == autodj::Wait::Fenced);
    TEST_ASSERT_FALSE(p.trigger.owed);
  }
  // The read-back (queueremap::Carry::finish()'s order), the same queue.
  r.player.setFenced(false);
  TEST_ASSERT_TRUE(r.queue.assign(tracks.data(), static_cast<uint32_t>(tracks.size()), cur));
  r.player.queueReplaced(true);
  r.dj.remapped(r.queue);
  TEST_ASSERT_TRUE(r.dj.marks().has(r.queue.keyAt(pickPos)));
  for (int i = 0; i < 3; ++i) {
    p = r.playTo(1500 + 100 * static_cast<uint32_t>(i));
    TEST_ASSERT_FALSE(p.quickSkip);
    TEST_ASSERT_FALSE(p.removedPick);
    TEST_ASSERT_TRUE(p.began == Began::None);
    TEST_ASSERT_FALSE(p.picked);  // (the pick is still up next)
  }
  TEST_ASSERT_EQUAL_UINT32(sessions, r.dj.sessions());
  // The pick plays: it is Auto DJ's (its mark carried).
  p = r.end();
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_TRUE(p.startedByAutoDj);
}

// ---------------------------------------------------------------------------
// The index arriving, the starts, Dp
// ---------------------------------------------------------------------------

// A session that began while the index joined: the track that started
// meanwhile steers once it is joined, and the pick waits for it (owed).
void test_the_index_arriving_after_the_session_began() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.indexed = false;
  r.joining = true;
  r.playOne(r.card.trackOf(6));
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_TRUE(p.trigger.owed);
  TEST_ASSERT_FALSE(p.picked);
  TEST_ASSERT_EQUAL_UINT8(0, r.dj.engine().anchors());
  r.indexed = true;
  r.joining = false;
  p = r.pass();
  TEST_ASSERT_TRUE(p.mapsArrived);
  TEST_ASSERT_TRUE(p.added);
  TEST_ASSERT_FALSE(p.pick.random);  // (steered by the playing track)
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(6));
}

// "Auto DJ from here" and the empty states' button: a one-track Play,
// Auto DJ on, one session seeded by the track (the pin), not a second one
// at the next pass.
void test_auto_dj_from_here_and_the_button() {
  Rig r;
  r.dj.setMode(autodj::Mode::Locked);
  bool switched = false;
  TEST_ASSERT_TRUE(r.dj.playFrom(r.player, r.card.trackOf(8), Began::FromHere, r.card.join.maps(), &switched));
  TEST_ASSERT_TRUE(switched);
  TEST_ASSERT_TRUE(r.dj.settings().on);
  TEST_ASSERT_EQUAL_UINT32(1, r.queue.size());
  TEST_ASSERT_EQUAL_INT(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_UINT16(8, r.dj.engine().pin());
  const uint32_t sessions = r.dj.sessions();
  Pass p = r.pass();
  TEST_ASSERT_TRUE(p.began == Began::None);
  TEST_ASSERT_EQUAL_UINT32(sessions, r.dj.sessions());
  TEST_ASSERT_TRUE(p.added);  // (the one track is last)
  // The button: a random track with a row.
  const autodj::Pick s = r.dj.surprise(r.index());
  TEST_ASSERT_TRUE(s.ok);
  TEST_ASSERT_TRUE(s.row != autodj::kNoRow);
  TEST_ASSERT_TRUE(r.dj.playFrom(r.player, s.track, Began::Surprise, r.card.join.maps(), &switched));
  TEST_ASSERT_FALSE(switched);  // (on already)
  TEST_ASSERT_TRUE(r.dj.sessionBegan() == Began::Surprise);
  TEST_ASSERT_EQUAL_UINT16(s.row, r.dj.engine().pin());
  // Without an index the button has no track.
  r.indexed = false;
  TEST_ASSERT_FALSE(r.dj.surprise(r.index()).ok);
  TEST_ASSERT_FALSE(r.dj.playFrom(r.player, autodj::kNoTrackId, Began::Surprise, nullptr, &switched));
  // Behind the fence the Play is refused.
  r.player.setFenced(true);
  TEST_ASSERT_FALSE(r.dj.playFrom(r.player, r.card.trackOf(1), Began::FromHere, nullptr, &switched));
  r.player.setFenced(false);
}

// The Undo of "Auto DJ from here" and of the empty states' button, as the
// toast's does it (PlaybackController::undo(), then Auto DJ off again when
// the tap switched it on): the queue as it was, in the state it was in.
// Nothing that was silent starts (the device check of 2026-10-14: the
// Undo of a Play over a queue paused on Auto DJ's cued pick played it, and
// Auto DJ picked on), and no pick comes at the restored paused or stopped
// queue; one that played plays its entry again. With Auto DJ off before
// the tap and on before it.
void test_the_undo_of_from_here_and_the_button_starts_nothing() {
  enum class Was { Playing, Paused, Stopped, CuedPick, Empty };
  const char* names[] = {"playing", "paused", "stopped", "paused on Auto DJ's cued pick", "empty (the button)"};
  for (int w = 0; w <= static_cast<int>(Was::Empty); ++w) {
    for (int djOn = 0; djOn < 2; ++djOn) {
      const Was was = static_cast<Was>(w);
      if (was == Was::CuedPick && !djOn) continue;  // (only Auto DJ cues a pick)
      char what[96];
      std::snprintf(what, sizeof(what), "%s, Auto DJ %s", names[w], djOn ? "on" : "off");
      Rig r;
      if (djOn) TEST_ASSERT_TRUE(r.dj.setOn(true));
      const uint32_t three[] = {r.card.trackOf(1), r.card.trackOf(2), r.card.trackOf(3)};
      switch (was) {
        case Was::Playing:
        case Was::Paused:
        case Was::Stopped:
          TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
          r.playTo(1500);
          if (was == Was::Paused) r.player.togglePlayPause();
          if (was == Was::Stopped) r.player.stop();
          break;
        case Was::CuedPick:
          TEST_ASSERT_TRUE(r.player.playNow(three, 3, 2));
          r.audio.ended = true;  // (no pick yet: the pass sees the end first, the queue finished)
          r.player.update(r.env.nowMs);
          TEST_ASSERT_TRUE_MESSAGE(r.player.endedAtEnd(), what);
          r.audio.ended = false;
          TEST_ASSERT_TRUE_MESSAGE(r.pass().cued, what);
          TEST_ASSERT_TRUE_MESSAGE(r.player.cuedByAutoDj(), what);
          break;
        case Was::Empty: r.player.clearQueue(); break;
      }
      const PlayState then = r.player.state();
      const uint32_t size = r.queue.size();
      const int index = r.player.currentIndex();
      r.pass();
      // The tap: the button's draw, or the long-pressed track.
      bool switched = false;
      const Began why = was == Was::Empty ? Began::Surprise : Began::FromHere;
      TEST_ASSERT_TRUE_MESSAGE(r.dj.playFrom(r.player, r.card.trackOf(8), why, r.card.join.maps(), &switched), what);
      TEST_ASSERT_EQUAL_MESSAGE(!djOn, switched, what);
      TEST_ASSERT_EQUAL_INT_MESSAGE(PlayState::Playing, r.player.state(), what);
      r.playTo(600);
      r.playTo(2000);  // (its pick appended: the one track is last)
      const int plays = r.audio.plays;
      // The toast's Undo.
      TEST_ASSERT_TRUE_MESSAGE(r.player.undo(), what);
      if (switched) TEST_ASSERT_TRUE(r.dj.setOn(false));
      TEST_ASSERT_EQUAL_MESSAGE(djOn != 0, r.dj.settings().on, what);
      for (int k = 0; k < 10; ++k) r.pass();
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(size, r.queue.size(), what);  // (no pick at the restored queue)
      TEST_ASSERT_EQUAL_INT_MESSAGE(index, r.player.currentIndex(), what);
      if (then == PlayState::Playing) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(PlayState::Playing, r.player.state(), what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(plays + 1, r.audio.plays, what);  // its entry again, from 0:00
        continue;
      }
      TEST_ASSERT_EQUAL_INT_MESSAGE(then, r.player.state(), what);
      TEST_ASSERT_EQUAL_INT_MESSAGE(plays, r.audio.plays, what);
      TEST_ASSERT_FALSE_MESSAGE(r.audio.isPlaying(), what);
      TEST_ASSERT_EQUAL_MESSAGE(was == Was::CuedPick, r.player.cuedByAutoDj(), what);
    }
  }
}

// Dp: a pick now, whatever the trigger says; refused behind the fence.
void test_a_pick_now() {
  Rig r;
  const uint32_t three[] = {r.card.trackOf(1), r.card.trackOf(2), r.card.trackOf(3)};
  TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
  r.playTo(700);
  Pass p = r.dj.pickNow(r.player, r.index(), r.env);  // (off, two up next)
  TEST_ASSERT_TRUE(p.added);
  TEST_ASSERT_EQUAL_UINT32(4, r.queue.size());
  TEST_ASSERT_TRUE(r.dj.marks().has(r.queue.keyAt(3)));
  r.player.setFenced(true);
  p = r.dj.pickNow(r.player, r.index(), r.env);
  TEST_ASSERT_FALSE(p.picked);
  TEST_ASSERT_TRUE(p.refused);
  r.player.setFenced(false);
}

// Dn: dry-run picks on a copy of the session, the live one untouched;
// one pass each, a read ahead first when one is due; the tallies.
void test_the_dry_run_leaves_the_live_session_alone() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  TEST_ASSERT_TRUE(r.playTo(700).added);
  r.pass();
  const autodj::Stats before = r.dj.engine().stats();
  const uint8_t anchors = r.dj.engine().anchors();
  const uint8_t cooled = r.dj.engine().cooledRows();
  autodj::DryRun run;
  TEST_ASSERT_FALSE(run.running());
  TEST_ASSERT_TRUE(run.begin(r.dj.engine(), 40, r.queue.currentTrack()));
  TEST_ASSERT_TRUE(run.running());
  int passes = 0;
  while (run.step(r.dj.settings(), r.index(), r.env.libraryTracks, clockUs)) ++passes;
  TEST_ASSERT_FALSE(run.running());
  TEST_ASSERT_EQUAL_UINT32(40, run.done());
  const autodj::DryRun::Result& res = run.result();
  TEST_ASSERT_EQUAL_UINT32(40, res.picks + res.failed);
  TEST_ASSERT_EQUAL_UINT32(40, res.picks);
  uint32_t classes = 0;
  for (uint32_t c = 0; c < autodj::kClasses; ++c) classes += res.classes[c];
  TEST_ASSERT_EQUAL_UINT32(40 - res.noIndex, classes);
  TEST_ASSERT_EQUAL_UINT32(40, res.readsAtPick[0] + res.readsAtPick[1] + res.readsAtPick[2]);
  TEST_ASSERT_TRUE(res.repeats > 0);  // (16 rows, 40 picks)
  TEST_ASSERT_TRUE(res.artists >= 1 && res.artists <= 4);
  TEST_ASSERT_TRUE(res.prefetchReads > 0);
  TEST_ASSERT_TRUE(passes >= 40);
  for (uint32_t i = 1; i < run.done(); ++i) TEST_ASSERT_TRUE(run.times()[i - 1] <= run.times()[i]);
  // The live session as it was.
  TEST_ASSERT_EQUAL_UINT32(before.picks, r.dj.engine().stats().picks);
  TEST_ASSERT_EQUAL_UINT32(before.prefetchReads, r.dj.engine().stats().prefetchReads);
  TEST_ASSERT_EQUAL_UINT8(anchors, r.dj.engine().anchors());
  TEST_ASSERT_EQUAL_UINT8(cooled, r.dj.engine().cooledRows());
  run.end();
  TEST_ASSERT_FALSE(run.begun());
  TEST_ASSERT_FALSE(run.begin(r.dj.engine(), 0, autodj::kNoTrackId));
  // Each dry pick is treated as played: the next follows it (its tempo
  // reference, its playing row left out), not the track playing live.
  autodj::DryRun one;
  const uint32_t playing = r.queue.currentTrack();
  TEST_ASSERT_TRUE(one.begin(r.dj.engine(), 1, playing));
  TEST_ASSERT_EQUAL_UINT32(playing, one.following());
  while (one.step(r.dj.settings(), r.index(), r.env.libraryTracks, clockUs)) {
  }
  TEST_ASSERT_EQUAL_UINT32(1, one.result().picks);
  TEST_ASSERT_TRUE(one.following() != playing);
  TEST_ASSERT_TRUE(r.card.rowOf(one.following()) != autodj::kNoRow);
}

// ---------------------------------------------------------------------------
// The review's cases (M4's fix step)
// ---------------------------------------------------------------------------

// After the fence the maps are away until the join is ready again: a pick
// taken out before it played, and a quick skip, still leave the ring (by
// the rows they had: an index's rows don't change with the library's ids).
void test_leaving_the_ring_while_the_maps_are_away() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.added);
  const uint16_t pickRow = p.pick.row;
  TEST_ASSERT_TRUE(r.inRing(5));
  TEST_ASSERT_TRUE(r.inRing(pickRow));
  r.fenceAndReadBack();
  r.indexed = false;  // (the join again: no maps until it is ready)
  r.joining = true;
  // The pick taken out before it played.
  const uint32_t at = 1;
  TEST_ASSERT_EQUAL_UINT32(1, r.player.remove(&at, 1).count);
  p = r.pass();
  TEST_ASSERT_TRUE(p.removedPick);
  TEST_ASSERT_FALSE(r.inRing(pickRow));
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(pickRow));
  TEST_ASSERT_FALSE(p.picked);  // (owed: the index joins)
  TEST_ASSERT_TRUE(p.trigger.owed);
  // A track of the listener's after it; the playing one skipped at 5 s.
  const uint32_t nine = r.card.trackOf(9);
  TEST_ASSERT_TRUE(r.player.addToQueue(&nine, 1));
  r.playTo(5000);
  r.player.next();
  p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(5000, p.leftMs);
  TEST_ASSERT_FALSE(r.inRing(5));
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(5));
  // The join ready: the track playing now steers (its row from the maps).
  r.indexed = true;
  r.joining = false;
  p = r.pass();
  TEST_ASSERT_TRUE(p.mapsArrived);
  TEST_ASSERT_TRUE(r.inRing(9));
}

// After the read-back the playing entry's time goes on from where it was
// (EntryStart rekeyed): a track 20 s in at the fence, skipped at 60 s, is
// no quick skip and keeps steering.
void test_the_read_back_keeps_the_playing_time() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(5));
  TEST_ASSERT_TRUE(r.playTo(700).added);
  Pass p = r.end();  // Auto DJ's pick starts (and the next is picked)
  TEST_ASSERT_TRUE(p.started && p.startedByAutoDj);
  const uint16_t row = r.card.rowOf(r.queue.currentTrack());
  r.playTo(20000);
  r.fenceAndReadBack();
  r.playTo(60000);
  r.player.next();
  p = r.pass();
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_TRUE(r.inRing(row));
}

// A pick that started is no longer "a pick not played yet": taken out
// while it plays, it is the current entry leaving (45 s in: it stays).
void test_a_pick_that_started_and_is_taken_out_is_no_removed_pick() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playOne(r.card.trackOf(3));
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.added);
  const uint16_t row = p.pick.row;
  r.env.sleepEndOfQueue = true;  // (a hold: no pick as it starts, so it stays the one noted)
  p = r.end();
  TEST_ASSERT_TRUE(p.started && p.startedByAutoDj);
  TEST_ASSERT_FALSE(p.picked);
  r.playTo(45000);
  const uint32_t at = 1;
  TEST_ASSERT_EQUAL_UINT32(1, r.player.remove(&at, 1).count);
  p = r.pass();
  TEST_ASSERT_FALSE(p.removedPick);
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(0, r.dj.removedPicks());
  TEST_ASSERT_TRUE(r.inRing(row));
}

// A rebuild renumbers the library's tracks: the by-id cooldown (picks
// without an index) is cleared at the read-back, or it would cool other
// tracks.
void test_the_by_id_cooldown_is_cleared_at_the_read_back() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.indexed = false;
  r.playOne(r.card.trackOf(5));
  const Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.added && p.pick.noIndex);
  TEST_ASSERT_TRUE(r.dj.engine().idCooled(p.pick.track));
  TEST_ASSERT_TRUE(r.dj.engine().cooledIds() > 0);
  r.fenceAndReadBack();
  TEST_ASSERT_EQUAL_UINT8(0, r.dj.engine().cooledIds());
}

// A pick the queue refused (5,000 entries, none played: nothing to push
// out) never played, so it doesn't steer the retry; it stays cooled, and
// it is no quick skip.
void test_a_refused_pick_leaves_the_ring() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  std::vector<uint32_t> tracks(QueueModel::kMaxEntries);
  for (uint32_t i = 0; i < tracks.size(); ++i) tracks[i] = r.card.trackOf(static_cast<int>(i % kRows));
  // (A Play from the last: nothing before it heard.)
  TEST_ASSERT_TRUE(r.player.playNow(tracks.data(), QueueModel::kMaxEntries, QueueModel::kMaxEntries - 1));
  TEST_ASSERT_EQUAL_UINT32(0, r.queue.played());
  const Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.picked && p.pick.ok);
  TEST_ASSERT_TRUE(p.refused);
  TEST_ASSERT_FALSE(p.added);
  TEST_ASSERT_EQUAL_UINT32(QueueModel::kMaxEntries, r.queue.size());
  TEST_ASSERT_TRUE(p.pick.row != autodj::kNoRow);
  TEST_ASSERT_FALSE(r.inRing(p.pick.row));
  TEST_ASSERT_TRUE(r.inRing(r.card.rowOf(r.queue.currentTrack())));
  TEST_ASSERT_TRUE(r.dj.engine().rowCooled(p.pick.row));
  TEST_ASSERT_EQUAL_UINT32(0, r.dj.engine().stats().quickSkips);
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.refusals());
}

// An entry that never started (cued while stopped, then skipped from)
// never steered: its leaving is no quick skip.
void test_an_entry_that_never_started_is_no_quick_skip() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const uint32_t three[] = {r.card.trackOf(2), r.card.trackOf(6), r.card.trackOf(10)};
  TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
  r.playTo(40000);
  r.player.stop();
  r.player.cueNext();  // (stopped: stays stopped)
  Pass p = r.pass();
  TEST_ASSERT_FALSE(p.started);
  const uint32_t skips = r.dj.engine().stats().quickSkips;
  r.player.next();  // (from the cued entry: the one after starts)
  p = r.pass();
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_FALSE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(skips, r.dj.engine().stats().quickSkips);
}

// ---------------------------------------------------------------------------
// The device checks' fixes (2026-10-14)
// ---------------------------------------------------------------------------

// The decoder refilling the ring from empty (a start, a skip: the device's
// 108 ms pick was the decoder's time): a due pick waits, nothing is read
// ahead, the wait counted once; the refill over, the pick lands. A refill
// owes nothing to the update step, and holds nothing when nothing is due.
void test_a_due_pick_waits_for_the_decoders_refill() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const uint32_t two[] = {r.card.trackOf(3), r.card.trackOf(9)};
  TEST_ASSERT_TRUE(r.player.playNow(two, 2, 0));
  // Not due: a refill reads nothing ahead (the first track's slot is due
  // one once it starts).
  r.env.refilling = true;
  Pass p = r.playTo(600);
  TEST_ASSERT_TRUE(p.started);
  TEST_ASSERT_FALSE(p.picked);
  TEST_ASSERT_FALSE(p.prefetched);
  for (int i = 0; i < 3; ++i) {
    p = r.pass();
    TEST_ASSERT_FALSE(p.prefetched);
    TEST_ASSERT_FALSE(p.refillWait);
  }
  r.env.refilling = false;
  TEST_ASSERT_TRUE(r.pass().prefetched);
  // A skip into the last entry: due, and the decoder refills.
  r.player.next();
  r.env.refilling = true;
  for (int i = 0; i < 4; ++i) {
    p = r.playTo(100);
    TEST_ASSERT_TRUE(p.trigger.pick);
    TEST_ASSERT_FALSE(p.trigger.owed);
    TEST_ASSERT_FALSE(p.picked);
    TEST_ASSERT_FALSE(p.prefetched);
    TEST_ASSERT_TRUE(p.refillWait);
  }
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.refillWaits());
  TEST_ASSERT_EQUAL_UINT32(2, r.queue.size());
  TEST_ASSERT_FALSE(r.dj.owed());
  r.env.refilling = false;
  p = r.playTo(300);
  TEST_ASSERT_TRUE(p.picked && p.added);
  TEST_ASSERT_FALSE(p.refillWait);
  TEST_ASSERT_EQUAL_UINT32(3, r.queue.size());
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.refillWaits());
  // Off: nothing is due, nothing waits.
  TEST_ASSERT_TRUE(r.dj.setOn(false));
  r.env.refilling = true;
  r.player.next();
  p = r.pass();
  TEST_ASSERT_FALSE(p.refillWait);
  TEST_ASSERT_EQUAL_UINT32(1, r.dj.refillWaits());
}

// A quick skip says the row the track had and whether it left the ring:
// none for a file the index doesn't have (it never steered: the log line
// says so, 2026-10-13's "no longer steers" for the bench MP3 was wrong);
// a row that steered, out of the ring now; a row that wasn't steering (the
// track playing when Auto DJ was switched on: it joins at the first pick),
// nothing changes (Engine::rowLeft()'s answer).
void test_a_quick_skip_says_the_row_it_had() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const uint32_t three[] = {r.card.loose(), r.card.trackOf(5), r.card.trackOf(11)};
  TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
  r.playTo(2500);
  r.player.next();
  Pass p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(r.card.loose(), p.leftTrack);
  TEST_ASSERT_EQUAL_UINT16(autodj::kNoRow, p.leftRow);
  TEST_ASSERT_FALSE(p.leftSteered);
  TEST_ASSERT_TRUE(p.leftWhy == autodj::SkipWhy::NoRow);
  TEST_ASSERT_EQUAL_STRING("it has no row in the index: it never steered", autodj::skipWhyText(p.leftWhy));
  r.playTo(2500);
  TEST_ASSERT_TRUE(r.inRing(5));
  r.player.next();
  p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT32(r.card.trackOf(5), p.leftTrack);
  TEST_ASSERT_EQUAL_UINT16(5, p.leftRow);
  TEST_ASSERT_TRUE(p.leftSteered);
  TEST_ASSERT_TRUE(p.leftWhy == autodj::SkipWhy::Steered);
  TEST_ASSERT_EQUAL_STRING("no longer steers", autodj::skipWhyText(p.leftWhy));
  TEST_ASSERT_FALSE(r.inRing(5));
  // Switched on while a track plays: it isn't in the ring yet.
  TEST_ASSERT_TRUE(r.dj.setOn(false));
  const uint32_t two[] = {r.card.trackOf(7), r.card.trackOf(12)};
  TEST_ASSERT_TRUE(r.player.playNow(two, 2, 0));
  r.playTo(700);
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  r.playTo(2500);
  TEST_ASSERT_FALSE(r.inRing(7));
  r.player.next();
  p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_EQUAL_UINT16(7, p.leftRow);
  TEST_ASSERT_FALSE(p.leftSteered);
  TEST_ASSERT_TRUE(p.leftWhy == autodj::SkipWhy::NotAnchor);
  TEST_ASSERT_EQUAL_STRING("it wasn't among the anchors: nothing changes", autodj::skipWhyText(p.leftWhy));
}

// A quick skip before the index is joined (the device check of 2026-10-14:
// D1, then n before "joined", printed "it has no row in the index" for a
// track that has one): its row is unknown, and the line says the index
// isn't joined yet; with no index joined at all (none usable), that. Once
// the maps come while it plays it steers (Engine::trackStarted() at
// mapsArrived), and its skip says "no longer steers". Behind a fence (the
// maps away, the row known from before) the known row's answer.
void test_a_quick_skip_before_the_join_says_the_index_isnt_joined() {
  for (int joining = 0; joining < 2; ++joining) {
    Rig r;
    r.indexed = false;
    r.joining = joining != 0;
    TEST_ASSERT_TRUE(r.dj.setOn(true));
    const uint32_t three[] = {r.card.trackOf(4), r.card.trackOf(5), r.card.trackOf(6)};
    TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
    r.playTo(400);
    r.player.next();
    Pass p = r.pass();
    TEST_ASSERT_TRUE(p.quickSkip);
    TEST_ASSERT_EQUAL_UINT32(r.card.trackOf(4), p.leftTrack);
    TEST_ASSERT_FALSE(p.leftSteered);
    TEST_ASSERT_TRUE(p.leftWhy == (joining ? autodj::SkipWhy::NotJoined : autodj::SkipWhy::NoIndex));
    TEST_ASSERT_EQUAL_STRING(joining ? "the index isn't joined yet: it never steered"
                                     : "no index is joined: it never steered",
                             autodj::skipWhyText(p.leftWhy));
    // The maps come while track 2 plays: it steers from then.
    r.playTo(400);
    r.indexed = true;
    r.joining = false;
    p = r.playTo(600);
    TEST_ASSERT_TRUE(p.mapsArrived);
    TEST_ASSERT_TRUE(r.inRing(5));
    r.player.next();
    p = r.pass();
    TEST_ASSERT_TRUE(p.quickSkip);
    TEST_ASSERT_TRUE(p.leftWhy == autodj::SkipWhy::Steered);
  }
  // The maps away after a fence: the rows known before still answer.
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const uint32_t three[] = {r.card.loose(), r.card.trackOf(5), r.card.trackOf(6)};
  TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
  r.playTo(400);
  r.fenceAndReadBack();
  r.indexed = false;
  r.joining = true;  // (the join again)
  r.playTo(800);
  r.player.next();
  Pass p = r.pass();
  TEST_ASSERT_TRUE(p.quickSkip);
  TEST_ASSERT_TRUE(p.leftWhy == autodj::SkipWhy::NoRow);
}

// A session seeded while the index joins ("Auto DJ from here", the empty
// states' button with Auto DJ off: playback doesn't wait for the join), the
// listener on to another track before the maps come: once they do, the
// seed's row is Stay on seed's pin (Engine::mapsArrived(), the design's
// 2.2), not the playing track's; the playing track steers the ring. An
// Anchor change meanwhile drops it: the playing track's row then.
void test_a_seed_before_the_join_is_the_pin() {
  for (int changed = 0; changed < 2; ++changed) {
    Rig r;
    r.dj.setMode(autodj::Mode::Locked);
    r.indexed = false;
    r.joining = true;
    bool switched = false;
    TEST_ASSERT_TRUE(r.dj.playFrom(r.player, r.card.trackOf(8), Began::FromHere, nullptr, &switched));
    TEST_ASSERT_TRUE(switched);
    TEST_ASSERT_EQUAL_UINT16(autodj::kNoRow, r.dj.engine().pin());
    r.playTo(1500);
    const uint32_t next = r.card.trackOf(3);
    TEST_ASSERT_TRUE(r.player.playNext(&next, 1));
    r.player.next();
    Pass p = r.playTo(400);
    TEST_ASSERT_TRUE(p.quickSkip);  // (the seed: its row unknown, nothing left the ring)
    TEST_ASSERT_TRUE(p.trigger.owed);
    TEST_ASSERT_FALSE(p.picked);
    if (changed) {
      r.dj.setMode(autodj::Mode::Rolling);
      r.dj.setMode(autodj::Mode::Locked);
    }
    r.indexed = true;
    r.joining = false;
    p = r.pass();
    TEST_ASSERT_TRUE(p.mapsArrived);
    TEST_ASSERT_EQUAL_UINT16(changed ? 3 : 8, r.dj.engine().pin());
    TEST_ASSERT_TRUE(p.picked && p.added);
    TEST_ASSERT_FALSE(p.pick.random);
    TEST_ASSERT_EQUAL_UINT8(1, p.pick.reads);  // (the pin's slot: the only one read)
    TEST_ASSERT_TRUE(r.dj.engine().cached(changed ? 3 : 8));
    TEST_ASSERT_TRUE(r.inRing(3));  // (the playing track steers the ring)
  }
}

// The empty states' button while nothing is joined (Auto DJ off): library
// tracks drawn, the first the index has a row for (the firmware: its
// path's hash in DJPH, mpdj::Table::rowOf()); kSurpriseTries at most; the
// next session's seed (Ds) untouched. The path's hash and DJPH agree with
// the join for every track.
namespace {

struct RowsByPath {
  const LibraryIndex* index;
  const mpdj::Table* table;
  int lookups = 0;
};

bool rowByPath(uint32_t track, void* ctx) {
  RowsByPath* r = static_cast<RowsByPath*>(ctx);
  ++r->lookups;
  char path[autodj::Join::kPathBytes];
  uint64_t h = 0;
  if (!autodj::trackPathHash(*r->index, track, path, sizeof(path), &h)) return false;
  return r->table->rowOf(h) != mpdj::kNoRow;
}

bool never(uint32_t, void* ctx) {
  ++*static_cast<int*>(ctx);
  return false;
}

}  // namespace

void test_the_button_looks_tracks_up_by_their_path() {
  Rig r;
  RowsByPath by{&r.card.index, &r.card.table};
  // The path's hash finds the row the join gave every track (none for the
  // loose file).
  const autodj::Maps& m = *r.card.join.maps();
  for (uint32_t t = 0; t < r.card.index.trackCount(); ++t) {
    TEST_ASSERT_EQUAL(m.trackToRow[t] != autodj::kNoRow, rowByPath(t, &by));
  }
  TEST_ASSERT_FALSE(rowByPath(r.card.loose(), &by));
  // Drawn: a track with a row, in a few lookups (all but one have one).
  r.dj.setSeed(0x1234u);
  for (int i = 0; i < 20; ++i) {
    by.lookups = 0;
    uint8_t tries = 0;
    const uint32_t t = r.dj.surpriseTrack(r.card.index.trackCount(), rowByPath, &by, &tries);
    TEST_ASSERT_TRUE(t < r.card.index.trackCount());
    TEST_ASSERT_TRUE(m.trackToRow[t] != autodj::kNoRow);
    TEST_ASSERT_EQUAL_INT(tries, by.lookups);
    TEST_ASSERT_TRUE(tries >= 1 && tries <= autodj::kSurpriseTries);
  }
  // None has one: kSurpriseTries lookups, no track; no library: none.
  int lookups = 0;
  uint8_t tries = 0;
  TEST_ASSERT_EQUAL_UINT32(autodj::kNoTrackId, r.dj.surpriseTrack(r.card.index.trackCount(), never, &lookups, &tries));
  TEST_ASSERT_EQUAL_INT(autodj::kSurpriseTries, lookups);
  TEST_ASSERT_EQUAL_UINT8(autodj::kSurpriseTries, tries);
  TEST_ASSERT_EQUAL_UINT32(autodj::kNoTrackId, r.dj.surpriseTrack(0, never, &lookups, &tries));
  TEST_ASSERT_EQUAL_UINT8(0, tries);
  // The draws took nothing from the seed Ds set.
  TEST_ASSERT_TRUE(r.dj.seedSet());
  // Played from it before any join (the maps away): Auto DJ on, a session
  // seeded by it; once the maps come, it steers.
  r.indexed = false;
  r.joining = true;
  const uint32_t t = r.dj.surpriseTrack(r.card.index.trackCount(), rowByPath, &by);
  bool switched = false;
  TEST_ASSERT_TRUE(r.dj.playFrom(r.player, t, Began::Surprise, nullptr, &switched));
  TEST_ASSERT_TRUE(switched);
  TEST_ASSERT_TRUE(r.dj.settings().on);
  TEST_ASSERT_EQUAL_UINT32(0x1234u, r.dj.sessionSeed());
  Pass p = r.playTo(700);
  TEST_ASSERT_TRUE(p.trigger.owed);  // (the pick waits for the join, the fence too)
  TEST_ASSERT_FALSE(p.picked);
  r.indexed = true;
  r.joining = false;
  p = r.pass();
  TEST_ASSERT_TRUE(p.mapsArrived);
  TEST_ASSERT_TRUE(p.picked && p.added);
  TEST_ASSERT_TRUE(r.inRing(r.card.rowOf(t)));
}

// The join ready again (a fence, a remount: the glue calls
// Engine::mapsChanged()): each cached slot's run is made again, one a loop
// pass while nothing else is due, without a read; none while a list moves.
// A read ahead makes its slot's run with it.
void test_runs_are_made_again_for_new_maps() {
  Rig r;
  TEST_ASSERT_TRUE(r.dj.setOn(true));
  const uint32_t three[] = {r.card.trackOf(3), r.card.trackOf(9), r.card.trackOf(12)};
  TEST_ASSERT_TRUE(r.player.playNow(three, 3, 0));
  int prefetches = r.playTo(5000).prefetched ? 1 : 0;
  r.end();  // (its natural end: it stays in the ring)
  prefetches += r.playTo(600).prefetched ? 1 : 0;
  for (int i = 0; i < 6; ++i) {
    const Pass p = r.pass();
    prefetches += p.prefetched ? 1 : 0;
    TEST_ASSERT_FALSE(p.runMade);  // (made with each read)
  }
  TEST_ASSERT_TRUE(prefetches >= 1);
  TEST_ASSERT_TRUE(r.dj.engine().cached(3) && r.dj.engine().cached(9));
  const int reads = r.reader.reads;
  r.dj.engine().mapsChanged();
  r.env.listMoving = true;
  for (int i = 0; i < 3; ++i) TEST_ASSERT_FALSE(r.pass().runMade);
  r.env.listMoving = false;
  int made = 0;
  uint16_t rows[4] = {};
  for (int i = 0; i < 6; ++i) {
    const Pass p = r.pass();
    TEST_ASSERT_FALSE(p.prefetched);
    if (p.runMade && made < 4) rows[made++] = p.runRow;
  }
  TEST_ASSERT_EQUAL_INT(2, made);  // the two anchors, newest first, a pass each
  TEST_ASSERT_EQUAL_UINT16(9, rows[0]);
  TEST_ASSERT_EQUAL_UINT16(3, rows[1]);
  TEST_ASSERT_EQUAL_INT(reads, r.reader.reads);
  // The pick as the last entry starts (its own slot read then: it joined
  // the ring as it started); nothing left to make.
  r.player.next();
  const Pass p = r.playTo(100);
  TEST_ASSERT_TRUE(p.picked && p.added);
  TEST_ASSERT_TRUE(p.pick.reads <= 1);
  TEST_ASSERT_FALSE(r.pass().runMade);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_command_parses_every_form);
  RUN_TEST(test_the_marks_follow_their_positions_across_a_read_back);
  RUN_TEST(test_sessions_begin_at_on_and_at_every_new_queue);
  RUN_TEST(test_the_seed_and_the_boot);
  RUN_TEST(test_a_track_steers_once_started_and_a_quick_skip_leaves_the_ring);
  RUN_TEST(test_a_finished_queue_gets_a_cued_pick_and_no_quick_skip);
  RUN_TEST(test_picks_are_appended_marked_and_keep_the_undo);
  RUN_TEST(test_a_failed_read_ahead_waits);
  RUN_TEST(test_a_slot_damaged_at_the_pick_is_named_once);
  RUN_TEST(test_the_undo_it_drops_is_said);
  RUN_TEST(test_a_pick_removed_before_it_played_leaves_the_ring);
  RUN_TEST(test_the_holds_add_nothing);
  RUN_TEST(test_a_refused_pick_is_tried_again_later);
  RUN_TEST(test_failing_picks_pause_auto_dj_and_a_track_that_plays_ends_it);
  RUN_TEST(test_off_and_on_clears_the_failure_pause);
  RUN_TEST(test_the_fence_and_the_read_back);
  RUN_TEST(test_the_index_arriving_after_the_session_began);
  RUN_TEST(test_auto_dj_from_here_and_the_button);
  RUN_TEST(test_the_undo_of_from_here_and_the_button_starts_nothing);
  RUN_TEST(test_a_pick_now);
  RUN_TEST(test_the_dry_run_leaves_the_live_session_alone);
  RUN_TEST(test_leaving_the_ring_while_the_maps_are_away);
  RUN_TEST(test_the_read_back_keeps_the_playing_time);
  RUN_TEST(test_a_pick_that_started_and_is_taken_out_is_no_removed_pick);
  RUN_TEST(test_the_by_id_cooldown_is_cleared_at_the_read_back);
  RUN_TEST(test_a_refused_pick_leaves_the_ring);
  RUN_TEST(test_an_entry_that_never_started_is_no_quick_skip);
  RUN_TEST(test_a_due_pick_waits_for_the_decoders_refill);
  RUN_TEST(test_a_quick_skip_says_the_row_it_had);
  RUN_TEST(test_a_quick_skip_before_the_join_says_the_index_isnt_joined);
  RUN_TEST(test_a_seed_before_the_join_is_the_pin);
  RUN_TEST(test_the_button_looks_tracks_up_by_their_path);
  RUN_TEST(test_runs_are_made_again_for_new_maps);
  return UNITY_END();
}
