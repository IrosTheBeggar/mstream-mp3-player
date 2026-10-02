// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for gapless playback end to end (docs/GAPLESS.md): the real
// PlaybackController and QueueModel over a backend made of the firmware's
// portable pieces (PcmRing, RingFeed, TrimFeed, GaplessJoin, GaplessEngine)
// with synthetic tracks. Each edit, the sleep timer, skips, pauses and
// failures while the next track is decoded ahead: what is heard, frame for
// frame, and where the player is. Run: pio test -e native -f test_gapless_player
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "GaplessEngine.h"
#include "GaplessJoin.h"
#include "LibraryIndex.h"
#include "PcmRing.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "RateConverter.h"
#include "RingFeed.h"
#include "SleepTimer.h"
#include "TrackCatalog.h"
#include "TrimFeed.h"
#include "hal/IAudioBackend.h"

namespace {
using Frames = std::vector<int16_t>;
using Engine = GaplessEngine;

constexpr uint32_t kPass = 1024;
constexpr uint8_t kReader = 1;
constexpr uint32_t kRingFrames = 65536;

Frames noise(size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  Frames v(frames * 2);
  for (auto& s : v) s = static_cast<int16_t>(static_cast<int>(rng() % 60001) - 30000);
  return v;
}
Frames slice(const Frames& v, size_t from, size_t to) { return Frames(v.begin() + 2 * from, v.begin() + 2 * to); }
Frames concat(std::initializer_list<Frames> parts) {
  Frames v;
  for (const Frames& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}
Frames reference(uint32_t hz, const Frames& in) {
  static RateConverter c;
  c.reset();
  TEST_ASSERT_TRUE(c.setRate(hz, 240, true));
  Frames out;
  int16_t buf[RateConverter::kMaxOut * 2];
  for (size_t i = 0; i + 1 < in.size(); i += 2) {
    const uint32_t n = c.push(&in[i], buf);
    out.insert(out.end(), buf, buf + 2 * n);
  }
  while (!c.finished()) {
    const uint32_t n = c.finishPush(buf);
    out.insert(out.end(), buf, buf + 2 * n);
  }
  return out;
}
void assertSame(const Frames& want, const Frames& got) {
  TEST_ASSERT_EQUAL_UINT32(want.size() / 2, got.size() / 2);
  for (size_t i = 0; i < want.size(); ++i) {
    if (want[i] != got[i]) {
      char msg[80];
      snprintf(msg, sizeof(msg), "first difference at frame %u", static_cast<unsigned>(i / 2));
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

struct Track {
  uint32_t rate = 44100;
  Frames frames;
  uint32_t skip = 0, hold = 0;
  Frames kept() const {
    const size_t n = frames.size() / 2;
    return slice(frames, std::min<size_t>(skip, n), n > hold ? n - hold : 0);
  }
};
Track track(uint32_t rate, size_t frames, uint32_t seed, uint32_t skip = 1106, uint32_t hold = 779) {
  Track t;
  t.rate = rate;
  t.frames = noise(frames, seed);
  t.skip = skip;
  t.hold = hold;
  return t;
}

// The Core2's backend as far as gapless playback goes: a request starts a
// generation (discardAll(), the feed reset, the first track opened), the
// engine joins what setNext() names, the outputs read the ring.
class HostBackend : public IAudioBackend, public Engine::Tracks {
public:
  HostBackend() : ringBuf_(kRingFrames * 2), ring(ringBuf_.data(), kRingFrames), rng_(3) {
    ring.setConsumer(kReader);
    hold_.resize(TrimFeed::kMaxHold * 2);
    trim.setHoldBuffer(hold_.data(), TrimFeed::kMaxHold);
    marks_[0].reset(new RingFeed::Mark);
    marks_[1].reset(new RingFeed::Mark);
    engine.setMarks(marks_[0].get(), marks_[1].get());
  }

  std::map<std::string, Track> tracks;
  int plays = 0, stops = 0;
  uint32_t requestAt = 0;  // the ring's read index at the last play()
  uint32_t trackSeq = 0;   // starts and advances (Core2AudioBackend::trackSeq())
  std::vector<std::string> probes;  // tracks opened for a join
  struct Advance {
    uint32_t token, readPos, heardAt;
  };
  std::vector<Advance> advances;
  Frames heard;
  bool consumerPaused = false;  // (the test holding the reader)

  // ---- IAudioBackend ----
  bool play(const std::string& path, uint32_t hintMs, uint32_t startMs) override {
    (void)hintMs;
    (void)startMs;
    ++plays;
    requestAt = ring.readPos();
    paused_ = false;
    begin();
    engine.begin(gen_, ring.writePos(), 0, true);
    GaplessJoin::Offer o;
    o.path = path;
    uint32_t hz = 0;
    failed_ = !open(o, &hz) || !start();
    if (failed_) engine.idle(gen_);
    ++trackSeq;
    return true;
  }
  void pause() override { paused_ = true; }
  void resume() override { paused_ = false; }
  void stop() override {
    ++stops;
    begin();
    failed_ = false;
    engine.idle(gen_);
  }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return !paused_ && !failed_ && engine.phase() != Engine::Phase::Idle; }
  uint32_t positionMs() const override { return book.positionMs(gen_, ring.readPos()); }
  uint32_t durationMs() const override {
    uint32_t ms = 0;
    return book.frozenLength(&ms) ? ms : 0;
  }
  bool finished() const override { return !failed_ && engine.phase() == Engine::Phase::Ended; }
  bool failed() const override { return failed_; }
  void setNext(const Next& n) override { book.setOffer(gen_, n.after, n.token, n.path, n.hintMs); }
  bool takeAdvance(uint32_t* token) override {
    const GaplessJoin::Status st = book.status();
    if (!book.takeAdvance(gen_, ring.readPos(), token)) return false;
    advances.push_back({*token, ring.readPos(), st.b.heardAt});
    ++trackSeq;
    return true;
  }

  // ---- Engine::Tracks ----
  bool probe(const GaplessJoin::Offer& o, uint32_t* rate) override {
    probes.push_back(o.path);
    return open(o, rate);
  }
  bool start() override {
    if (!probed_) return false;
    cur_ = probed_;
    pos_ = 0;
    trim.arm(cur_->skip, cur_->hold);
    trim.setChannels(2);
    trim.setRate(static_cast<int>(cur_->rate));
    return true;
  }
  void close() override {
    cur_ = nullptr;
    probed_ = nullptr;
  }

  // ---- the tasks ----
  uint32_t bufferedMs() const { return static_cast<uint32_t>(static_cast<uint64_t>(ring.size()) * 1000 / 44100); }
  void decodeOnce() {
    const Engine::Phase p = engine.phase();
    if (p == Engine::Phase::Idle || p == Engine::Phase::Ended) return;
    if (engine.cutDue() || p != Engine::Phase::Producing) {
      engine.step(bufferedMs());
      return;
    }
    bool more = false;
    if (cur_) {
      feed.setBudget(kPass);
      const size_t end = cur_->frames.size() / 2;
      while (pos_ < end && trim.consume(&cur_->frames[2 * pos_])) ++pos_;
      feed.commit();
      more = pos_ < end;
    }
    if (!more) engine.sourceEnded(false);
  }
  void consumeOnce() {
    if (paused_ || consumerPaused) return;
    read(rng_() % 3 == 0 ? 0 : rng_() % 1500);
  }
  void read(uint32_t n) {
    int16_t buf[512 * 2];
    while (n > 0) {
      const uint32_t got = ring.read(kReader, buf, std::min<uint32_t>(n, 512));
      if (got == 0) return;
      heard.insert(heard.end(), buf, buf + 2 * got);
      n -= got;
    }
  }
  uint32_t decodingPos() const { return static_cast<uint32_t>(pos_); }

private:
  void begin() {
    ++gen_;
    ring.discardAll();
    feed.reset(240, true);
    trim.disarm();
    close();
  }
  bool open(const GaplessJoin::Offer& o, uint32_t* rate) {
    auto it = tracks.find(o.path);
    if (it == tracks.end()) return false;
    probed_ = &it->second;
    *rate = probed_->rate;
    return true;
  }

  std::vector<int16_t> ringBuf_;

public:
  PcmRing ring;
  RingFeed feed{ring};
  TrimFeed trim{feed};
  GaplessJoin book;
  Engine engine{ring, feed, trim, book, *this};

private:
  std::unique_ptr<RingFeed::Mark> marks_[2];
  std::vector<int16_t> hold_;
  std::mt19937 rng_;
  uint32_t gen_ = 0;
  bool paused_ = false;
  bool failed_ = false;
  const Track* probed_ = nullptr;
  const Track* cur_ = nullptr;
  size_t pos_ = 0;
};

struct Gate : PlaybackController::NextGate {
  std::function<bool()> ends = [] { return false; };
  bool endsHere() const override { return ends(); }
};

// The player over that backend, with a library of named tracks
// ("/music/<name>.mp3") and a queue of some of them.
struct World {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  QueueModel queue;
  HostBackend audio;
  PlaybackController player{audio, queue, catalog};
  Gate gate;
  std::vector<std::string> names;
  // The current entry's changes: (index, the ring's read index then).
  std::vector<std::pair<int, uint32_t>> changes;

  World(std::initializer_list<const char*> library, std::initializer_list<const char*> queued) {
    index.begin("/music");
    for (const char* n : library) {
      names.push_back(n);
      index.addFile(path(n).c_str());
    }
    index.finish();
    std::vector<uint32_t> ids;
    for (const char* n : queued) ids.push_back(id(n));
    queue.assign(ids.data(), static_cast<uint32_t>(ids.size()), 0);
    player.setNextGate(&gate);
  }
  static std::string path(const std::string& name) { return "/music/" + name + ".mp3"; }
  uint32_t id(const std::string& name) const {
    return static_cast<uint32_t>(std::find(names.begin(), names.end(), name) - names.begin());
  }
  Track& put(const std::string& name, Track t) { return audio.tracks[path(name)] = std::move(t); }
  const Track& get(const std::string& name) { return audio.tracks[path(name)]; }
  std::function<void()> beforeUpdate = [] {};

  void tick() {
    audio.consumeOnce();
    audio.decodeOnce();
    const int before = player.currentIndex();
    beforeUpdate();
    player.update(0);
    if (player.currentIndex() != before) changes.push_back({player.currentIndex(), audio.ring.readPos()});
  }
  template <class F>
  void runUntil(F until, uint32_t maxTicks = 3000000) {
    for (uint32_t i = 0; i < maxTicks; ++i) {
      if (until()) return;
      tick();
    }
    TEST_FAIL_MESSAGE("never got there");
  }
  void runToStop() {
    runUntil([this] { return player.state() == PlayState::Stopped; });
  }
  // The next track joined and decoding, the reader held before J.
  void untilDecodedAhead(uint32_t frames = 3000) {
    audio.consumerPaused = true;
    runUntil([this, frames] { return audio.engine.boundaryUp() && audio.decodingPos() > frames; });
    TEST_ASSERT_TRUE(static_cast<int32_t>(audio.book.status().b.cutAt - audio.ring.readPos()) > 0);
  }
  uint32_t cutAt() const { return audio.book.status().b.cutAt; }
  uint32_t heardAt() const { return audio.book.status().b.heardAt; }
};

}  // namespace

void setUp() {}
void tearDown() {}

// An album plays as one stream, with one play(): each join is taken when
// the reader passes it, never before, and the player's entry follows.
void test_an_album_plays_as_one_stream() {
  World w({"a", "b", "c"}, {"a", "b", "c"});
  w.put("a", track(44100, 80000, 1));
  w.put("b", track(44100, 30000, 2, 1106, 1300));
  w.put("c", track(44100, 60000, 3, 1, 0));
  w.player.setRepeat(false);
  w.player.play(0);
  w.runToStop();
  assertSame(concat({w.get("a").kept(), w.get("b").kept(), w.get("c").kept()}), w.audio.heard);
  TEST_ASSERT_EQUAL_INT(1, w.audio.plays);
  TEST_ASSERT_EQUAL_UINT32(2, w.player.gaplessStats().adopted);
  TEST_ASSERT_EQUAL_UINT32(2, w.audio.advances.size());
  for (const auto& a : w.audio.advances) TEST_ASSERT_TRUE(static_cast<int32_t>(a.readPos - a.heardAt) > 0);
  // The entry changed to b, then c, each in the pass that took its advance.
  TEST_ASSERT_TRUE(w.changes.size() >= 2);
  TEST_ASSERT_EQUAL_INT(1, w.changes[0].first);
  TEST_ASSERT_EQUAL_UINT32(w.audio.advances[0].readPos, w.changes[0].second);
  TEST_ASSERT_EQUAL_INT(2, w.changes[1].first);
  TEST_ASSERT_EQUAL_UINT32(w.audio.advances[1].readPos, w.changes[1].second);
}

// Play next while b is decoded ahead: b is cut back out, y joined in its
// place, b and c after it. Nothing of b before y.
void test_play_next_while_the_next_is_decoded_ahead() {
  World w({"a", "b", "c", "y"}, {"a", "b", "c"});
  w.put("a", track(44100, 60000, 4));
  w.put("b", track(44100, 30000, 5));
  w.put("c", track(44100, 30000, 6));
  w.put("y", track(44100, 30000, 7));
  w.player.setRepeat(false);
  w.player.play(0);
  w.untilDecodedAhead();
  const uint32_t y = w.id("y");
  w.player.playNext(&y, 1);
  w.audio.consumerPaused = false;
  w.runToStop();
  assertSame(concat({w.get("a").kept(), w.get("y").kept(), w.get("b").kept(), w.get("c").kept()}), w.audio.heard);
  TEST_ASSERT_EQUAL_INT(1, w.audio.plays);
  TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().cuts);
}

// Too late to cut (the reader past J, before B: a converting rate): b's
// start is heard; at its advance the player sees that y comes next, and
// starts it (a request): y, then b again from its start, then c.
void test_play_next_too_late_to_cut_still_plays_y_next() {
  World w({"a", "b", "c", "y"}, {"a", "b", "c"});
  w.put("a", track(48000, 60000, 8));
  w.put("b", track(48000, 30000, 9));
  w.put("c", track(48000, 30000, 10));
  w.put("y", track(48000, 30000, 11));
  w.player.setRepeat(false);
  w.player.play(0);
  w.untilDecodedAhead();
  TEST_ASSERT_TRUE(static_cast<int32_t>(w.heardAt() - w.cutAt()) > 1);  // the filter's delay: J < B
  w.audio.read(w.cutAt() + 1 - w.audio.ring.readPos());
  const uint32_t y = w.id("y");
  w.player.playNext(&y, 1);  // after a: the heard one
  w.audio.decodeOnce();      // the cut: too late
  TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().tooLate);
  w.audio.consumerPaused = false;
  w.runToStop();
  TEST_ASSERT_EQUAL_INT(2, w.audio.plays);  // a's, then y's
  TEST_ASSERT_EQUAL_UINT32(1, w.player.gaplessStats().restarted);
  // a, a little of b's start, then y and what follows it as one stream.
  const Frames ab = reference(48000, concat({w.get("a").kept(), w.get("b").kept()}));
  const uint32_t k = w.audio.requestAt;
  const Frames heardBefore = Frames(ab.begin(), ab.begin() + 2 * k);
  assertSame(concat({heardBefore, reference(48000, concat({w.get("y").kept(), w.get("b").kept(), w.get("c").kept()}))}),
             w.audio.heard);
  const uint32_t aRing = static_cast<uint32_t>(RateConverter::plan(48000, 240, true).ringFrames(w.get("a").kept().size() / 2));
  TEST_ASSERT_TRUE(k > aRing && k - aRing < 1600);  // at most a read of b before y
}

// Removing the next entry, moving another before it, clearing what's up
// next (repeat off: nothing follows), an undo: each cut and joined again.
void test_queue_edits_while_decoded_ahead() {
  for (int edit = 0; edit < 4; ++edit) {
    World w({"a", "b", "c"}, {"a", "b", "c"});
    w.put("a", track(44100, 60000, 12));
    w.put("b", track(44100, 30000, 13));
    w.put("c", track(44100, 30000, 14));
    w.player.setRepeat(false);
    w.player.play(0);
    w.untilDecodedAhead();
    const uint32_t one = 1, two = 2;
    Frames want;
    if (edit == 0) {
      w.player.remove(&one, 1);
      want = concat({w.get("a").kept(), w.get("c").kept()});
    } else if (edit == 1) {
      w.player.moveNext(&two, 1);  // a, c, b
      want = concat({w.get("a").kept(), w.get("c").kept(), w.get("b").kept()});
    } else if (edit == 2) {
      w.player.clearUpNext();
      want = w.get("a").kept();
    } else {
      w.player.remove(&one, 1);  // c joined in b's place...
      w.runUntil([&w] { return w.audio.engine.counters().cuts == 1 && w.audio.engine.boundaryUp() && w.audio.decodingPos() > 3000; });
      TEST_ASSERT_TRUE(w.player.undo());  // ...and b back
      want = concat({w.get("a").kept(), w.get("b").kept(), w.get("c").kept()});
    }
    w.audio.consumerPaused = false;
    w.runToStop();
    assertSame(want, w.audio.heard);
    TEST_ASSERT_EQUAL_INT(1, w.audio.plays);
    TEST_ASSERT_EQUAL_UINT32(edit == 3 ? 2 : 1, w.audio.engine.counters().cuts);
  }
}

// A queue of one with repeat: the track joined to itself; repeat turned
// off: cut, and it stops at its end.
void test_repeat_turned_off_on_the_last_entry() {
  World w({"a"}, {"a"});
  w.put("a", track(44100, 60000, 15));
  w.player.play(0);
  w.untilDecodedAhead();
  w.player.setRepeat(false);  // (no action: the next update sees it)
  w.audio.consumerPaused = false;
  w.runToStop();
  assertSame(w.get("a").kept(), w.audio.heard);
  TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().cuts);
}

// A queue of one with repeat (the firmware's default) loops without a gap
// every time round, not only the first: each loop's word has a new token
// (the heard one was taken already). Then repeat off: it stops.
void test_a_queue_of_one_on_repeat_loops_as_one_stream() {
  World w({"a"}, {"a"});
  w.put("a", track(44100, 30000, 60));
  w.player.setRepeat(true);
  w.player.play(0);
  w.runUntil([&w] { return w.audio.advances.size() == 3; });
  w.player.setRepeat(false);
  w.runToStop();
  const Frames a = w.get("a").kept();
  assertSame(concat({a, a, a, a}), w.audio.heard);
  TEST_ASSERT_EQUAL_INT(1, w.audio.plays);
  TEST_ASSERT_EQUAL_UINT32(3, w.player.gaplessStats().adopted);
  TEST_ASSERT_EQUAL_UINT32(0, w.player.gaplessStats().restarted);
  TEST_ASSERT_EQUAL_UINT32(3, w.audio.advances.size());
  TEST_ASSERT_TRUE(w.audio.advances[0].token != w.audio.advances[1].token);
  TEST_ASSERT_TRUE(w.audio.advances[1].token != w.audio.advances[2].token);
}

// The sleep timer's End of track, chosen early: the next track is never
// decoded ahead; the player pauses at the boundary with nothing of it
// heard, the next entry cued.
void test_end_of_track_never_decodes_the_next() {
  World w({"a", "b"}, {"a", "b"});
  w.put("a", track(44100, 60000, 16));
  w.put("b", track(44100, 30000, 17));
  w.gate.ends = [] { return true; };
  w.player.setPauseAfterTrack(true);
  w.player.play(0);
  w.runUntil([&w] { return w.player.state() == PlayState::Paused; });
  assertSame(w.get("a").kept(), w.audio.heard);
  TEST_ASSERT_TRUE(w.audio.probes.empty());
  TEST_ASSERT_EQUAL_INT(1, w.player.currentIndex());
  TEST_ASSERT_TRUE(w.player.pausedByTimer());
  TEST_ASSERT_EQUAL_UINT32(1, w.player.timerStops());
}

// Chosen while b is decoded ahead: cut, the same exact pause. Chosen too
// late (the reader past J): the pause comes at the advance, with at most a
// read of b heard.
void test_end_of_track_chosen_late() {
  for (int tooLate = 0; tooLate < 2; ++tooLate) {
    World w({"a", "b"}, {"a", "b"});
    w.put("a", track(48000, 60000, 18));
    w.put("b", track(48000, 30000, 19));
    w.player.play(0);
    w.untilDecodedAhead();
    if (tooLate) w.audio.read(w.cutAt() + 1 - w.audio.ring.readPos());
    w.gate.ends = [] { return true; };
    w.player.setPauseAfterTrack(true);  // (main.cpp's stepSleep, then the player's update)
    w.player.update(0);
    w.audio.decodeOnce();  // the cut: Done, or too late
    w.audio.consumerPaused = false;
    w.runUntil([&w] { return w.player.state() == PlayState::Paused; });
    const Frames a = reference(48000, w.get("a").kept());
    if (!tooLate) {
      assertSame(a, w.audio.heard);
      TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().cuts);
    } else {
      TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().tooLate);
      TEST_ASSERT_TRUE(w.audio.heard.size() > a.size());
      TEST_ASSERT_TRUE(w.audio.heard.size() - a.size() < 2 * 1600);
      TEST_ASSERT_EQUAL_UINT32(1, w.player.gaplessStats().paused);
    }
    TEST_ASSERT_EQUAL_INT(1, w.player.currentIndex());  // b cued at 0:00
    TEST_ASSERT_TRUE(w.player.pausedByTimer());
    const size_t heard = w.audio.heard.size();
    for (int i = 0; i < 2000; ++i) w.tick();
    TEST_ASSERT_EQUAL_UINT32(heard, w.audio.heard.size());  // nothing plays by itself
  }
}

// End of album: decided in the same update that makes the album's last
// track current, so the next album's first track is never decoded ahead.
void test_end_of_album_is_decided_in_the_advance_s_update() {
  World w({"a1", "a2", "b1"}, {"a1", "a2", "b1"});
  w.put("a1", track(44100, 50000, 20));
  w.put("a2", track(44100, 40000, 21));
  w.put("b1", track(44100, 30000, 22));
  PlaybackController& p = w.player;
  w.gate.ends = [&p] { return p.currentIndex() == 1; };
  w.beforeUpdate = [&p] { p.setPauseAfterTrack(p.currentIndex() == 1); };  // (main.cpp's stepSleep)
  p.play(0);
  w.runUntil([&p] { return p.state() == PlayState::Paused; });
  assertSame(concat({w.get("a1").kept(), w.get("a2").kept()}), w.audio.heard);
  TEST_ASSERT_EQUAL_UINT32(1, w.audio.probes.size());
  TEST_ASSERT_EQUAL_STRING(World::path("a2").c_str(), w.audio.probes[0].c_str());
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
}

// A next pressed just after the reader passed the join (before the
// player's update): the joined track is the one heard, so it is the one
// skipped.
void test_next_right_after_a_join_skips_the_joined_track() {
  World w({"a", "b", "c"}, {"a", "b", "c"});
  w.put("a", track(44100, 60000, 23));
  w.put("b", track(44100, 30000, 24));
  w.put("c", track(44100, 30000, 25));
  w.player.setRepeat(false);
  w.player.play(0);
  w.untilDecodedAhead();
  w.audio.read(w.heardAt() + 100 - w.audio.ring.readPos());
  w.player.next();
  TEST_ASSERT_EQUAL_INT(2, w.player.currentIndex());
  TEST_ASSERT_EQUAL_INT(2, w.audio.plays);
  w.audio.consumerPaused = false;
  w.runToStop();
  assertSame(concat({w.get("a").kept(), slice(w.get("b").kept(), 0, 100), w.get("c").kept()}), w.audio.heard);
}

// Paused just before the join: the reader stops, so the player never
// advances; resumed, the join plays gaplessly.
void test_a_paused_player_never_advances() {
  World w({"a", "b"}, {"a", "b"});
  w.put("a", track(44100, 60000, 26));
  w.put("b", track(44100, 30000, 27));
  w.player.setRepeat(false);
  w.player.play(0);
  w.untilDecodedAhead();
  w.audio.read(w.heardAt() - 10 - w.audio.ring.readPos());
  w.player.togglePlayPause();
  w.audio.consumerPaused = false;
  const uint32_t at = w.player.positionMs();
  for (int i = 0; i < 5000; ++i) w.tick();
  TEST_ASSERT_EQUAL_INT(0, w.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(at, w.player.positionMs());
  w.player.togglePlayPause();
  w.runToStop();
  assertSame(concat({w.get("a").kept(), w.get("b").kept()}), w.audio.heard);
  TEST_ASSERT_EQUAL_INT(1, w.audio.plays);
}

// Gapless turned off while b is decoded ahead: cut, a ends as before, and
// b starts as a request (as in v0.5.0).
void test_gapless_off_ends_tracks_as_before() {
  World w({"a", "b", "c"}, {"a", "b", "c"});
  w.put("a", track(44100, 60000, 28));
  w.put("b", track(44100, 30000, 29));
  w.put("c", track(44100, 30000, 30));
  w.player.setRepeat(false);
  w.player.play(0);
  w.untilDecodedAhead();
  w.player.setGapless(false);
  w.audio.consumerPaused = false;
  w.runToStop();
  assertSame(concat({w.get("a").kept(), w.get("b").kept(), w.get("c").kept()}), w.audio.heard);
  TEST_ASSERT_EQUAL_INT(3, w.audio.plays);
  TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().cuts);
  TEST_ASSERT_EQUAL_UINT32(0, w.player.gaplessStats().adopted);
}

// A library rebuild gives every entry a new key (QueueModel::assign()),
// the same tracks: the word keeps its token, nothing is cut, and the
// advance finds the new key. Removing one of two duplicates the same way.
void test_new_keys_for_the_same_track_keep_the_join() {
  for (int kind = 0; kind < 2; ++kind) {
    World w({"a", "b", "c"}, {"a", "b", "b", "c"});
    w.put("a", track(44100, 60000, 31));
    w.put("b", track(44100, 30000, 32));
    w.put("c", track(44100, 30000, 33));
    w.player.setRepeat(false);
    w.player.play(0);
    w.untilDecodedAhead();
    if (kind == 0) {
      const uint32_t ids[] = {w.id("a"), w.id("b"), w.id("b"), w.id("c")};
      w.queue.assign(ids, 4, 0);
      w.player.queueReplaced(true);
    } else {
      const uint32_t one = 1;
      w.player.remove(&one, 1);
    }
    w.audio.consumerPaused = false;
    w.runToStop();
    const Frames b = w.get("b").kept();
    if (kind == 0) {
      assertSame(concat({w.get("a").kept(), b, b, w.get("c").kept()}), w.audio.heard);
    } else {
      assertSame(concat({w.get("a").kept(), b, w.get("c").kept()}), w.audio.heard);
    }
    TEST_ASSERT_EQUAL_UINT32(0, w.audio.engine.counters().cuts);
    TEST_ASSERT_EQUAL_INT(1, w.audio.plays);
  }
}

// The next track can't be opened (missing): a ends as before, its play()
// fails as before, the player skips it with a note, and c plays.
void test_a_next_track_that_cannot_be_opened_is_skipped_as_before() {
  World w({"a", "b", "c"}, {"a", "b", "c"});
  w.put("a", track(44100, 60000, 34));
  w.put("c", track(44100, 30000, 35));
  w.player.setRepeat(false);
  w.player.play(0);
  w.runToStop();
  assertSame(concat({w.get("a").kept(), w.get("c").kept()}), w.audio.heard);
  TEST_ASSERT_EQUAL_UINT32(1, w.player.lastFailure().count);
  TEST_ASSERT_EQUAL_UINT32(w.id("b"), w.player.lastFailure().track);
  TEST_ASSERT_EQUAL_UINT32(1, w.audio.engine.counters().failedOpens);
}

// The advance taken late (the loop stalled while the reader went 1.13 s
// into b): the sleep timer's EntryStart still sees b start (the backend
// counts the advance), so b's length is known at once.
void test_a_late_advance_still_counts_as_the_entry_s_start() {
  World w({"a", "b"}, {"a", "b"});
  w.put("a", track(44100, 10000, 36));
  w.put("b", track(44100, 80000, 37));
  w.player.setRepeat(false);
  EntryStart e;
  w.player.play(0);
  w.untilDecodedAhead(52000);
  e.update(w.queue.currentKey(), w.audio.trackSeq, w.player.positionMs());
  w.audio.read(w.heardAt() + 50000 - w.audio.ring.readPos());
  w.player.update(0);
  TEST_ASSERT_EQUAL_INT(1, w.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(1133, w.player.positionMs());
  TEST_ASSERT_TRUE(e.update(w.queue.currentKey(), w.audio.trackSeq, w.player.positionMs()));
}

// Each word goes once (refreshed only when something it depends on
// changes), and none while stopped or cued.
void test_the_word_goes_only_when_it_changes() {
  World w({"a", "b", "c"}, {"a", "b", "c"});
  w.put("a", track(44100, 200000, 38));
  w.put("b", track(44100, 30000, 39));
  w.put("c", track(44100, 30000, 40));
  TEST_ASSERT_EQUAL_UINT32(0, w.player.gaplessStats().offers);
  w.player.play(0);
  TEST_ASSERT_EQUAL_UINT32(1, w.player.gaplessStats().offers);
  const uint32_t token = w.player.offeredToken();
  TEST_ASSERT_TRUE(token != 0);
  for (int i = 0; i < 50; ++i) w.tick();
  TEST_ASSERT_EQUAL_UINT32(1, w.player.gaplessStats().offers);
  const uint32_t c = w.id("c");
  w.player.addToQueue(&c, 1);  // not the next: the same word
  TEST_ASSERT_EQUAL_UINT32(1, w.player.gaplessStats().offers);
  w.player.setPauseAfterTrack(true);
  w.tick();
  TEST_ASSERT_EQUAL_UINT32(2, w.player.gaplessStats().offers);
  TEST_ASSERT_EQUAL_UINT32(0, w.player.offeredToken());  // nothing follows
  w.player.setPauseAfterTrack(false);
  w.tick();
  TEST_ASSERT_TRUE(w.player.offeredToken() != 0 && w.player.offeredToken() != token);  // a new token
  w.player.stop();
  const uint32_t sent = w.player.gaplessStats().offers;
  for (int i = 0; i < 50; ++i) w.tick();
  TEST_ASSERT_EQUAL_UINT32(sent, w.player.gaplessStats().offers);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_album_plays_as_one_stream);
  RUN_TEST(test_play_next_while_the_next_is_decoded_ahead);
  RUN_TEST(test_play_next_too_late_to_cut_still_plays_y_next);
  RUN_TEST(test_queue_edits_while_decoded_ahead);
  RUN_TEST(test_repeat_turned_off_on_the_last_entry);
  RUN_TEST(test_a_queue_of_one_on_repeat_loops_as_one_stream);
  RUN_TEST(test_end_of_track_never_decodes_the_next);
  RUN_TEST(test_end_of_track_chosen_late);
  RUN_TEST(test_end_of_album_is_decided_in_the_advance_s_update);
  RUN_TEST(test_next_right_after_a_join_skips_the_joined_track);
  RUN_TEST(test_a_paused_player_never_advances);
  RUN_TEST(test_gapless_off_ends_tracks_as_before);
  RUN_TEST(test_new_keys_for_the_same_track_keep_the_join);
  RUN_TEST(test_a_next_track_that_cannot_be_opened_is_skipped_as_before);
  RUN_TEST(test_a_late_advance_still_counts_as_the_entry_s_start);
  RUN_TEST(test_the_word_goes_only_when_it_changes);
  return UNITY_END();
}
