// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for gapless playback's decode side (docs/GAPLESS.md):
// GaplessJoin (the boundary book) and GaplessEngine (the decode task's
// state machine at a source's end), through the real PcmRing, RingFeed and
// TrimFeed, with synthetic tracks, a reader that takes random amounts, and
// a stand-in for the player that names what follows each track once it is
// heard. Run: pio test -e native -f test_gapless
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "GaplessEngine.h"
#include "GaplessJoin.h"
#include "PcmRing.h"
#include "RateConverter.h"
#include "RingFeed.h"
#include "TrimFeed.h"

// A read under way, as the consumer marks it.
class PcmRingProbe {
public:
  static void setReading(PcmRing& r, bool on) { r.reading_.store(on); }
};

namespace {
using Frames = std::vector<int16_t>;
using Engine = GaplessEngine;

constexpr uint32_t kPass = 1024;
constexpr uint8_t kReader = 1;

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

// The converter alone, one stream.
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

// A synthetic track: its frames at its rate, the trim it arms (an MP3's
// lead + delay + 529 and padding - 529), and how it fails.
struct Track {
  uint32_t rate = 44100;
  Frames frames;
  uint32_t skip = 0, hold = 0;
  bool failProbe = false;
  bool failStart = false;
  bool unknownRate = false;  // the probe can't say: a join after the tail
  bool mp3Order = false;     // the rate said after its 2nd frame (MP3's generator)
  size_t endEarlyAt = 0;     // a decode error after this many frames (0: none)

  size_t end() const { return endEarlyAt ? std::min(endEarlyAt, frames.size() / 2) : frames.size() / 2; }
  // What of it reaches the ring (before the converter).
  Frames kept() const {
    const size_t n = end();
    const size_t to = endEarlyAt ? n : (n > hold ? n - hold : 0);
    const size_t from = std::min<size_t>(skip, to);
    return slice(frames, from, to);
  }
};

Track track(uint32_t rate, size_t frames, uint32_t seed, uint32_t skip = 0, uint32_t hold = 0) {
  Track t;
  t.rate = rate;
  t.frames = noise(frames, seed);
  t.skip = skip;
  t.hold = hold;
  return t;
}

// The decode task, its ring and outputs, and a stand-in for the player.
struct Rig : Engine::Tracks {
  explicit Rig(uint32_t ringFrames = 65536, uint32_t initialIndex = 0, uint32_t seed = 1)
      : ringBuf(ringFrames * 2), ring(ringBuf.data(), ringFrames, initialIndex), rng(seed) {
    ring.setConsumer(kReader);
    holdBuf.resize(TrimFeed::kMaxHold * 2);
    trim.setHoldBuffer(holdBuf.data(), TrimFeed::kMaxHold);
    marks[0].reset(new RingFeed::Mark);
    marks[1].reset(new RingFeed::Mark);
    engine.setMarks(marks[0].get(), marks[1].get());
  }

  std::vector<int16_t> ringBuf;
  PcmRing ring;
  RingFeed feed{ring};
  TrimFeed trim{feed};
  GaplessJoin book;
  Engine engine{ring, feed, trim, book, *this};
  std::unique_ptr<RingFeed::Mark> marks[2];
  std::vector<int16_t> holdBuf;
  std::map<std::string, Track> tracks;
  std::mt19937 rng;

  // ---- Engine::Tracks: the decoder side ----
  const Track* probed = nullptr;
  const Track* cur = nullptr;
  size_t pos = 0;
  bool rateSaid = false;
  std::vector<Engine::Note> notes;
  bool probe(const GaplessJoin::Offer& o, uint32_t* rate) override {
    auto it = tracks.find(o.path);
    if (it == tracks.end() || it->second.failProbe) return false;
    probed = &it->second;
    *rate = probed->unknownRate ? 0 : probed->rate;
    return true;
  }
  bool start() override {
    if (!probed || probed->failStart) return false;
    cur = probed;
    pos = 0;
    rateSaid = false;
    trim.arm(cur->skip, cur->hold);
    trim.setChannels(2);
    if (!cur->mp3Order) {
      trim.setRate(static_cast<int>(cur->rate));
      rateSaid = true;
    }
    return true;
  }
  void close() override {
    cur = nullptr;
    probed = nullptr;
  }
  void note(const Engine::Note& n) override { notes.push_back(n); }
  uint32_t count(Engine::Event e) const {
    return static_cast<uint32_t>(std::count_if(notes.begin(), notes.end(), [e](const Engine::Note& n) { return n.event == e; }));
  }

  // ---- the request (Core2AudioBackend::start()) ----
  uint32_t gen = 0;
  bool requestFailed = false;
  void request(const std::string& path) {
    ++gen;
    ring.discardAll();
    feed.reset(240, true);
    trim.disarm();
    engine.begin(gen, ring.writePos(), 0, true);
    GaplessJoin::Offer o;
    o.path = path;
    uint32_t hz = 0;
    requestFailed = !probe(o, &hz) || !start();
    if (requestFailed) engine.idle(gen);
    // The player's state for this request.
    heardToken = 0;
    at = 0;
    tokenIndex.clear();
  }

  uint32_t bufferedMs() const { return static_cast<uint32_t>(static_cast<uint64_t>(ring.size()) * 1000 / 44100); }

  // ---- the decode task: one iteration ----
  uint32_t lastRest = 0;
  void decodeOnce() {
    const Engine::Phase p = engine.phase();
    if (p == Engine::Phase::Idle || p == Engine::Phase::Ended) return;
    if (engine.cutDue() || p != Engine::Phase::Producing) {
      lastRest = engine.step(bufferedMs());
      return;
    }
    bool more = false;
    if (cur) {
      feed.setBudget(kPass);
      const size_t end = cur->end();
      while (pos < end) {
        if (cur->mp3Order && !rateSaid && pos == 2) {
          trim.setRate(static_cast<int>(cur->rate));
          rateSaid = true;
        }
        if (!trim.consume(&cur->frames[2 * pos])) break;
        ++pos;
      }
      feed.commit();
      more = pos < end;
    }
    if (!more) engine.sourceEnded(cur && cur->endEarlyAt != 0);
  }

  // ---- the outputs ----
  Frames heard;
  uint32_t maxRead = 1500;
  bool paused = false;
  uint32_t readLimit = 0;  // nonzero: never read past this ring index
  bool limitReads = false;
  void consumeOnce() {
    if (paused) return;
    uint32_t n = rng() % 3 == 0 ? 0 : rng() % maxRead;
    if (limitReads) n = std::min<uint32_t>(n, readLimit - ring.readPos());
    read(n);
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

  // ---- the player's stand-in: a queue of paths played in order ----
  std::vector<std::string> queue;
  size_t at = 0;              // the heard entry
  uint32_t heardToken = 0;
  uint32_t nextToken = 0;
  std::map<uint32_t, size_t> tokenIndex;
  bool autoOffer = true;
  struct Advance {
    uint32_t token;
    uint32_t readPos;
  };
  std::vector<Advance> advances;
  // The word on what follows the heard entry (a new token each time).
  void offerNext() {
    if (at + 1 < queue.size()) {
      const uint32_t t = ++nextToken;
      tokenIndex[t] = at + 1;
      book.setOffer(gen, heardToken, t, queue[at + 1], 0);
    } else {
      book.setOffer(gen, heardToken, 0, "", 0);
    }
  }
  void offer(const std::string& path) {
    const uint32_t t = ++nextToken;
    tokenIndex[t] = queue.size();
    queue.push_back(path);
    book.setOffer(gen, heardToken, t, path, 0);
  }
  void offerNothing() { book.setOffer(gen, heardToken, 0, "", 0); }
  void loopTask() {
    uint32_t t = 0;
    while (book.takeAdvance(gen, ring.readPos(), &t)) {
      advances.push_back({t, ring.readPos()});
      heardToken = t;
      at = tokenIndex[t];
      if (autoOffer) offerNext();
    }
  }
  void play(const std::vector<std::string>& paths) {
    queue = paths;
    request(paths[0]);
    if (autoOffer) offerNext();
  }

  void tick() {
    consumeOnce();
    decodeOnce();
    loopTask();
  }
  bool done() const { return (engine.phase() == Engine::Phase::Ended || engine.phase() == Engine::Phase::Idle) && ring.size() == 0; }
  // Until played out (or `until` says so).
  template <class F>
  void runUntil(F until, uint32_t maxTicks = 2000000) {
    for (uint32_t i = 0; i < maxTicks; ++i) {
      if (until()) return;
      tick();
    }
    TEST_FAIL_MESSAGE("never got there");
  }
  void runToEnd() {
    runUntil([this] { return done(); });
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// ---- sample-exact joins ----

// Tracks at one rate play as one stream: the converter runs on across
// each join (no reset, no tail), so what is heard is exactly the trimmed
// tracks converted as one file. At 44.1 kHz that is the tracks themselves,
// back to back.
void test_same_rate_joins_are_one_stream() {
  const uint32_t rates[] = {44100, 48000, 22050, 32000, 8000};
  for (const uint32_t hz : rates) {
    Rig r(65536, 0, hz);
    r.tracks["/a"] = track(hz, hz * 2 + 17, hz + 1, 1106, 779);
    r.tracks["/b"] = track(hz, hz / 2 + 3, hz + 2, 1, 0);
    r.tracks["/c"] = track(hz, hz * 3 / 2, hz + 3, 529, 4095);
    r.play({"/a", "/b", "/c"});
    r.runToEnd();
    assertSame(reference(hz, concat({r.tracks["/a"].kept(), r.tracks["/b"].kept(), r.tracks["/c"].kept()})), r.heard);
    TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().joins);
    TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().resets);
    TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().cuts);
    TEST_ASSERT_EQUAL_UINT32(2, r.advances.size());
  }
}

// An Opus track's shape (docs/OPUS.md): always 48 kHz, and it trims itself
// (the pre-skip and the EOS trim are the generator's: TrimFeed armed {0,0}
// gets kept samples only). Two of them join as one stream, sample-exact
// (what is heard is the two files' kept samples converted as one), and
// one against a 44.1 kHz MP3 is a rate change: the tail, then a new stream.
void test_self_trimming_48k_tracks_join_as_opus_does() {
  {
    Rig r;
    r.tracks["/a"] = track(48000, 2519040 / 24, 61);  // the card set's album parts, scaled down
    r.tracks["/b"] = track(48000, 2519040 / 24, 62);
    r.tracks["/c"] = track(48000, 60000, 63);
    r.play({"/a", "/b", "/c"});
    r.runToEnd();
    assertSame(reference(48000, concat({r.tracks["/a"].kept(), r.tracks["/b"].kept(), r.tracks["/c"].kept()})), r.heard);
    TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().joins);
    TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().resets);
  }
  {
    Rig r;
    r.tracks["/opus"] = track(48000, 70000, 64);
    r.tracks["/mp3"] = track(44100, 50000, 65, 1106, 700);
    r.tracks["/opus2"] = track(48000, 40000, 66);
    r.play({"/opus", "/mp3", "/opus2"});
    r.runToEnd();
    assertSame(concat({reference(48000, r.tracks["/opus"].kept()), r.tracks["/mp3"].kept(),
                       reference(48000, r.tracks["/opus2"].kept())}),
               r.heard);
    TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().joins);
    TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().resets);
  }
}

// Another rate: the tail goes in first, then a new stream (a fresh
// filter): each track converted alone, with its exact count.
void test_rate_change_joins_are_streams_back_to_back() {
  Rig r;
  r.tracks["/a"] = track(48000, 70000, 1, 1106, 700);
  r.tracks["/b"] = track(44100, 50000, 2, 1, 0);
  r.tracks["/c"] = track(22050, 30000, 3);
  r.tracks["/d"] = track(22050, 20000, 4);
  r.play({"/a", "/b", "/c", "/d"});
  r.runToEnd();
  assertSame(concat({reference(48000, r.tracks["/a"].kept()), r.tracks["/b"].kept(),
                     reference(22050, concat({r.tracks["/c"].kept(), r.tracks["/d"].kept()}))}),
             r.heard);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().joins);   // c -> d
  TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().resets);  // a -> b, b -> c
}

// A file that doesn't say its rate before its first frame: a join after
// the tail (two streams), even at the same rate; MP3's order (the rate
// said after its 2nd frame) at a continuous join changes nothing.
void test_an_unknown_rate_resets_and_the_mp3_order_continues() {
  Rig r;
  r.tracks["/a"] = track(48000, 40000, 5);
  r.tracks["/b"] = track(48000, 40000, 6);
  r.tracks["/b"].unknownRate = true;
  r.tracks["/c"] = track(48000, 30000, 7, 1106, 900);
  r.tracks["/c"].mp3Order = true;
  r.play({"/a", "/b", "/c"});
  r.runToEnd();
  assertSame(concat({reference(48000, r.tracks["/a"].kept()),
                     reference(48000, concat({r.tracks["/b"].kept(), r.tracks["/c"].kept()}))}),
             r.heard);
}

// A queue of one with repeat: the same track joined to itself.
void test_the_same_track_again() {
  Rig r;
  r.tracks["/a"] = track(44100, 50000, 8, 1106, 779);
  r.play({"/a", "/a", "/a"});
  r.runToEnd();
  const Frames k = r.tracks["/a"].kept();
  assertSame(concat({k, k, k}), r.heard);
}

// A track shorter than the ring: the decoder reaches its end before it is
// heard, and waits (the player names what follows a track only once it is
// heard) while the ring holds more than 250 ms: still one stream.
void test_a_track_shorter_than_the_ring() {
  Rig r;
  r.tracks["/a"] = track(48000, 90000, 9);
  r.tracks["/s"] = track(48000, 19000, 10);  // ~0.4 s
  r.tracks["/b"] = track(48000, 60000, 11);
  r.play({"/a", "/s", "/b"});
  r.runToEnd();
  assertSame(reference(48000, concat({r.tracks["/a"].kept(), r.tracks["/s"].kept(), r.tracks["/b"].kept()})), r.heard);
  TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().joins);
}

// Under 250 ms it can't wait for the word: it ends as before gapless
// playback (its tail, Draining, Ended); the player then starts what
// follows as a request.
void test_a_track_under_250_ms_ends_as_before() {
  Rig r;
  r.tracks["/a"] = track(48000, 90000, 12);
  r.tracks["/s"] = track(48000, 4000, 13);  // ~83 ms
  r.tracks["/b"] = track(48000, 60000, 14);
  r.play({"/a", "/s", "/b"});
  r.runToEnd();
  assertSame(reference(48000, concat({r.tracks["/a"].kept(), r.tracks["/s"].kept()})), r.heard);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().joins);
  TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().late);
}

// ---- the boundary is heard, not decoded ----

// The advance comes at the first frame read past B, once; until it is
// taken the position stops at the track's exact end; then it counts from
// the joined track's start. Near the counters' 2^32 wrap too.
void test_the_advance_comes_when_the_reader_passes_b() {
  const uint32_t starts[] = {0, 0xFFFFFFFFu - 50000};
  for (const uint32_t start : starts) {
    Rig r(262144, start);
    r.tracks["/a"] = track(44100, 88200 + 441, 15);  // 2.01 s
    r.tracks["/b"] = track(44100, 60000, 16);
    r.autoOffer = false;
    r.play({"/a", "/b"});
    r.offerNext();
    r.paused = true;  // the decoder runs ahead, into b
    r.runUntil([&r] { return r.engine.boundaryUp() && r.pos > 1000; });
    const GaplessJoin::Status st = r.book.status();
    TEST_ASSERT_TRUE(st.boundary);
    const uint32_t b = st.b.heardAt;
    TEST_ASSERT_EQUAL_UINT32(st.b.cutAt, b);  // the passthrough: B = J
    TEST_ASSERT_EQUAL_UINT32(88200 + 441, b - r.ring.readPos());
    uint32_t len = 0;
    TEST_ASSERT_TRUE(r.book.frozenLength(&len));
    TEST_ASSERT_EQUAL_UINT32(2010, len);  // a's exact length, at its end of file
    // 1 s in.
    r.read(44100);
    TEST_ASSERT_EQUAL_UINT32(1000, r.book.positionMs(r.gen, r.ring.readPos()));
    uint32_t token = 0;
    // Up to B exactly: a read to its end, nothing of b: no advance.
    r.read(b - r.ring.readPos());
    TEST_ASSERT_EQUAL_UINT32(b, r.ring.readPos());
    TEST_ASSERT_FALSE(r.book.takeAdvance(r.gen, r.ring.readPos(), &token));
    TEST_ASSERT_EQUAL_UINT32(2010, r.book.positionMs(r.gen, r.ring.readPos()));
    // Past it: held at a's end until the advance is taken...
    r.read(500);
    TEST_ASSERT_EQUAL_UINT32(2010, r.book.positionMs(r.gen, r.ring.readPos()));
    // ...then b's, from its start, once.
    TEST_ASSERT_TRUE(r.book.takeAdvance(r.gen, r.ring.readPos(), &token));
    TEST_ASSERT_EQUAL_UINT32(1, token);
    TEST_ASSERT_EQUAL_UINT32(11, r.book.positionMs(r.gen, r.ring.readPos()));  // 500 frames
    TEST_ASSERT_EQUAL_UINT32(0, r.book.startMs());
    TEST_ASSERT_FALSE(r.book.takeAdvance(r.gen, r.ring.readPos(), &token));
    TEST_ASSERT_FALSE(r.book.frozenLength(&len));  // b still decodes
    // Another generation's never comes.
    TEST_ASSERT_FALSE(r.book.takeAdvance(r.gen + 1, r.ring.readPos(), &token));
  }
}

// At a converting rate B is the converter's frame for b's first source
// frame: ceil(len(a) x num / den) from the stream's start, after J; and
// a's length frozen at its end is exactly that.
void test_b_at_a_converting_rate() {
  Rig r(262144);
  r.tracks["/a"] = track(48000, 96000 + 7, 17);  // 2 s and 7 frames
  r.tracks["/b"] = track(48000, 30000, 18);
  r.autoOffer = false;
  r.play({"/a", "/b"});
  const uint32_t start = r.ring.writePos();
  r.offerNext();
  r.paused = true;
  r.runUntil([&r] { return r.engine.boundaryUp(); });
  const GaplessJoin::Status st = r.book.status();
  const uint64_t want = RateConverter::plan(48000, 240, true).ringFrames(96007);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(want), st.b.heardAt - start);
  TEST_ASSERT_TRUE(static_cast<int32_t>(st.b.heardAt - st.b.cutAt) > 0);  // the filter's delay: J < B
  uint32_t len = 0;
  TEST_ASSERT_TRUE(r.book.frozenLength(&len));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(want * 1000 / 44100), len);
}

// ---- changes while the next track is decoded ahead ----

// Before the end of a: the new word is simply what is taken. No cut.
void test_a_change_before_the_end_is_just_taken() {
  Rig r;
  r.tracks["/a"] = track(48000, 120000, 19);
  r.tracks["/x"] = track(48000, 30000, 20);
  r.tracks["/y"] = track(48000, 30000, 21);
  r.play({"/a", "/x"});
  for (int i = 0; i < 20; ++i) r.tick();
  TEST_ASSERT_FALSE(r.engine.boundaryUp());
  r.queue.resize(1);
  r.offer("/y");
  r.runToEnd();
  assertSame(reference(48000, concat({r.tracks["/a"].kept(), r.tracks["/y"].kept()})), r.heard);
  TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().cuts);
}

// After the join, the reader still before J: the frames after J are cut
// back out, the feed rewound, and the new track joined in their place:
// bit for bit a then y, nothing of x. At the passthrough and at a
// converting rate, with the trim.
void test_a_change_after_the_join_cuts_the_decoded_ahead_track() {
  const uint32_t rates[] = {44100, 48000};
  for (const uint32_t hz : rates) {
    Rig r(65536, 0, hz + 5);
    r.tracks["/a"] = track(hz, hz * 2, 22, 1106, 779);
    r.tracks["/x"] = track(hz, hz, 23, 1106, 500);
    r.tracks["/y"] = track(hz, hz, 24, 1106, 600);
    r.play({"/a", "/x"});
    r.runUntil([&r] { return r.engine.boundaryUp() && r.pos > 5000; });  // x well under way
    TEST_ASSERT_TRUE(static_cast<int32_t>(r.book.status().b.cutAt - r.ring.readPos()) > 0);
    r.queue.resize(1);
    r.offer("/y");
    r.runToEnd();
    assertSame(reference(hz, concat({r.tracks["/a"].kept(), r.tracks["/y"].kept()})), r.heard);
    TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().cuts);
    TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().tooLate);
    TEST_ASSERT_EQUAL_UINT32(1, r.count(Engine::Event::Cut));
    TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().joins);
    TEST_ASSERT_EQUAL_UINT32(1, r.advances.size());  // y's, never x's
  }
}

// The reader already past J: too late, x stays (its start is heard) and
// its advance comes; the player sorts the change out after it.
void test_a_change_after_the_reader_passed_j_is_too_late() {
  const uint32_t rates[] = {44100, 48000};
  for (const uint32_t hz : rates) {
    Rig r(65536, 0, hz + 9);
    r.tracks["/a"] = track(hz, hz, 25);
    r.tracks["/x"] = track(hz, hz, 26);
    r.tracks["/y"] = track(hz, hz, 27);
    r.autoOffer = false;
    r.play({"/a", "/x"});
    r.offerNext();
    r.paused = true;
    r.runUntil([&r] { return r.engine.boundaryUp() && r.pos > 2000; });
    const GaplessJoin::Status st = r.book.status();
    // The reader one frame past J (before the loop task takes anything).
    r.read(st.b.cutAt + 1 - r.ring.readPos());
    TEST_ASSERT_EQUAL_UINT32(st.b.cutAt + 1, r.ring.readPos());
    r.queue.resize(1);
    r.offer("/y");  // (after a: the change)
    r.decodeOnce();
    TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().tooLate);
    TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().cuts);
    r.paused = false;
    r.runUntil([&r] { return !r.advances.empty(); });
    TEST_ASSERT_EQUAL_UINT32(1, r.advances[0].token);  // x's
    TEST_ASSERT_TRUE(static_cast<int32_t>(r.advances[0].readPos - st.b.heardAt) > 0);
    r.offerNothing();
    r.runToEnd();
    assertSame(reference(hz, concat({r.tracks["/a"].kept(), r.tracks["/x"].kept()})), r.heard);
  }
}

// Nothing follows any more (the end of the queue, "pause after this
// track", the sleep timer), or gapless turned off: cut, and a ends as
// before.
void test_nothing_or_gapless_off_after_the_join_cuts_and_ends() {
  for (int kind = 0; kind < 2; ++kind) {
    Rig r;
    r.tracks["/a"] = track(48000, 90000, 28);
    r.tracks["/x"] = track(48000, 60000, 29);
    r.play({"/a", "/x"});
    r.runUntil([&r] { return r.engine.boundaryUp(); });
    if (kind == 0) {
      r.queue.resize(1);
      r.offerNothing();
    } else {
      r.engine.setEnabled(false);
    }
    r.runToEnd();
    assertSame(reference(48000, r.tracks["/a"].kept()), r.heard);
    TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().cuts);
    TEST_ASSERT_TRUE(r.advances.empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Engine::Phase::Ended), static_cast<int>(r.engine.phase()));
  }
}

// Changed twice: each cut goes back to a's end.
void test_two_changes_in_a_row() {
  Rig r;
  r.tracks["/a"] = track(44100, 100000, 30, 1106, 779);
  r.tracks["/x"] = track(44100, 40000, 31);
  r.tracks["/y"] = track(44100, 40000, 32);
  r.tracks["/z"] = track(44100, 40000, 33, 1, 300);
  r.play({"/a", "/x"});
  r.runUntil([&r] { return r.engine.boundaryUp() && r.pos > 3000; });
  r.queue.resize(1);
  r.offer("/y");
  r.runUntil([&r] { return r.engine.counters().cuts == 1 && r.engine.boundaryUp() && r.pos > 3000; });
  r.queue.resize(1);
  r.offer("/z");
  r.runToEnd();
  assertSame(concat({r.tracks["/a"].kept(), r.tracks["/z"].kept()}), r.heard);
  TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().cuts);
}

// A short joined track that has already ended and is draining (the ring
// small: under 250 ms, so its word isn't waited for) is cut from there:
// back to a's end, and the new track joined.
void test_a_cut_while_the_joined_track_drains() {
  Rig r(8192, 0, 3);
  r.tracks["/a"] = track(48000, 5000, 34);
  r.tracks["/s"] = track(48000, 2000, 35);
  r.tracks["/y"] = track(48000, 20000, 36);
  r.autoOffer = false;
  r.play({"/a", "/s"});
  r.offerNext();
  r.paused = true;
  r.runUntil([&r] { return r.engine.phase() == Engine::Phase::Draining && r.engine.boundaryUp(); });
  TEST_ASSERT_TRUE(static_cast<int32_t>(r.book.status().b.cutAt - r.ring.readPos()) > 0);
  r.queue.resize(1);
  r.offer("/y");
  r.paused = false;
  r.runUntil([&r] { return !r.advances.empty(); });
  TEST_ASSERT_EQUAL_UINT32(2, r.advances[0].token);  // y's
  r.offerNothing();
  r.runToEnd();
  assertSame(reference(48000, concat({r.tracks["/a"].kept(), r.tracks["/y"].kept()})), r.heard);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().cuts);
}

// The reader at J = B exactly (the passthrough) while the cut runs: the
// advance isn't taken (strictly past B), the cut is Done, and the advance
// that comes is the new track's: what the player shows is what is heard.
void test_the_reader_at_j_equals_b_during_a_cut() {
  Rig r;
  r.tracks["/a"] = track(44100, 50000, 37);
  r.tracks["/x"] = track(44100, 30000, 38);
  r.tracks["/y"] = track(44100, 30000, 39);
  r.autoOffer = false;
  r.play({"/a", "/x"});
  r.offerNext();
  r.paused = true;
  r.runUntil([&r] { return r.engine.boundaryUp(); });
  const uint32_t j = r.book.status().b.cutAt;
  r.read(j - r.ring.readPos());
  r.loopTask();
  TEST_ASSERT_TRUE(r.advances.empty());
  r.queue.resize(1);
  r.offer("/y");
  r.decodeOnce();
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().cuts);
  r.paused = false;
  r.runUntil([&r] { return !r.advances.empty(); });
  TEST_ASSERT_EQUAL_UINT32(2, r.advances[0].token);
  r.offerNothing();
  r.runToEnd();
  assertSame(concat({r.tracks["/a"].kept(), r.tracks["/y"].kept()}), r.heard);
}

// A read under way when the fence goes up: the cut waits (Pending, 1 ms),
// nothing taken out meanwhile, then goes through.
void test_a_pending_cut_is_retried() {
  Rig r;
  r.tracks["/a"] = track(44100, 50000, 40);
  r.tracks["/x"] = track(44100, 30000, 41);
  r.tracks["/y"] = track(44100, 30000, 42);
  r.play({"/a", "/x"});
  r.runUntil([&r] { return r.engine.boundaryUp() && r.pos > 2000; });
  r.paused = true;
  r.queue.resize(1);
  r.offer("/y");
  const uint32_t w = r.ring.writePos();
  PcmRingProbe::setReading(r.ring, true);
  r.decodeOnce();
  TEST_ASSERT_EQUAL_UINT32(Engine::kCutRetryMs, r.lastRest);
  r.decodeOnce();
  TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().retries);
  TEST_ASSERT_EQUAL_UINT32(w, r.ring.writePos());
  PcmRingProbe::setReading(r.ring, false);
  r.decodeOnce();
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().cuts);
  r.paused = false;
  r.runToEnd();
  assertSame(concat({r.tracks["/a"].kept(), r.tracks["/y"].kept()}), r.heard);
}

// ---- a late word ----

// Said while a drains with 250 ms or more in the ring: taken, after a's
// tail (a new stream).
void test_a_late_word_while_draining() {
  Rig r(262144);
  r.tracks["/a"] = track(48000, 90000, 43);
  r.tracks["/b"] = track(48000, 40000, 44);
  r.autoOffer = false;
  r.play({"/a"});
  r.offerNothing();
  r.paused = true;
  r.runUntil([&r] { return r.engine.phase() == Engine::Phase::Draining; });
  r.offer("/b");
  r.paused = false;
  r.runUntil([&r] { return !r.advances.empty(); });
  r.offerNothing();
  r.runToEnd();
  assertSame(concat({reference(48000, r.tracks["/a"].kept()), reference(48000, r.tracks["/b"].kept())}), r.heard);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().late);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().resets);
}

// Under 250 ms left: too late to open anything; a ends.
void test_a_late_word_too_close_to_the_end_is_not_taken() {
  Rig r;
  r.tracks["/a"] = track(44100, 90000, 45);
  r.tracks["/b"] = track(44100, 40000, 46);
  r.autoOffer = false;
  r.play({"/a"});
  r.offerNothing();
  r.runUntil([&r] { return r.engine.phase() == Engine::Phase::Draining && r.bufferedMs() < 200; });
  r.offer("/b");
  r.runToEnd();
  assertSame(r.tracks["/a"].kept(), r.heard);
  TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().late);
  TEST_ASSERT_TRUE(r.advances.empty());
}

// ---- failures ----

// The next track can't be opened (missing, refused) or its decoder won't
// begin: a ends whole, as before; the word was taken, so it isn't tried
// again here (the player's play() fails it as before).
void test_a_next_track_that_fails_to_open() {
  for (int kind = 0; kind < 3; ++kind) {
    Rig r;
    r.tracks["/a"] = track(48000, 60000, 47);
    if (kind == 1) {
      r.tracks["/b"] = track(48000, 40000, 48);
      r.tracks["/b"].failProbe = true;
    } else if (kind == 2) {
      r.tracks["/b"] = track(44100, 40000, 48);  // another rate: fails after the tail
      r.tracks["/b"].failStart = true;
    }
    r.play({"/a", "/b"});
    r.runToEnd();
    assertSame(reference(48000, r.tracks["/a"].kept()), r.heard);
    TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().failedOpens);
    TEST_ASSERT_EQUAL_UINT32(1, r.count(Engine::Event::OpenFailed));
    TEST_ASSERT_TRUE(r.advances.empty());
  }
}

// The joined track ends without a frame (it couldn't be decoded): cut back
// out, a ends as before.
void test_a_joined_track_with_no_frames_is_cut_out() {
  Rig r;
  r.tracks["/a"] = track(44100, 60000, 49);
  r.tracks["/b"] = track(44100, 3000, 50, 4000, 0);  // all of it skipped: nothing comes out
  r.play({"/a", "/b"});
  r.runToEnd();
  assertSame(r.tracks["/a"].kept(), r.heard);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().emptyAhead);
  TEST_ASSERT_EQUAL_UINT32(1, r.engine.counters().cuts);
  TEST_ASSERT_TRUE(r.advances.empty());
}

// A decode error in the middle of a joined track: an early end, what the
// trim held goes in (real audio), and the next join follows.
void test_an_early_end_in_a_joined_track() {
  Rig r;
  r.tracks["/a"] = track(44100, 60000, 51);
  r.tracks["/b"] = track(44100, 60000, 52, 1106, 2000);
  r.tracks["/b"].endEarlyAt = 30000;
  r.tracks["/c"] = track(44100, 40000, 53, 1106, 779);
  r.play({"/a", "/b", "/c"});
  r.runToEnd();
  assertSame(concat({r.tracks["/a"].kept(), slice(r.tracks["/b"].frames, 1106, 30000), r.tracks["/c"].kept()}),
             r.heard);
  TEST_ASSERT_EQUAL_UINT32(2, r.engine.counters().joins);
}

// ---- generations ----

// A word given for another request is never taken; a new request drops
// a boundary waiting to be heard, and its advance never comes.
void test_words_and_boundaries_belong_to_their_request() {
  Rig r;
  r.tracks["/a"] = track(44100, 50000, 54);
  r.tracks["/b"] = track(44100, 50000, 55);
  r.autoOffer = false;
  r.play({"/a", "/b"});
  r.book.setOffer(r.gen + 7, 0, 1, "/b", 0);  // not this request's
  r.runToEnd();
  assertSame(r.tracks["/a"].kept(), r.heard);
  TEST_ASSERT_EQUAL_UINT32(0, r.engine.counters().joins);

  Rig q;
  q.tracks["/a"] = track(44100, 50000, 56);
  q.tracks["/b"] = track(44100, 50000, 57);
  q.play({"/a", "/b"});
  q.paused = true;
  q.runUntil([&q] { return q.engine.boundaryUp(); });
  const uint32_t old = q.gen;
  q.request("/b");  // a skip: a new request
  for (int i = 0; i < 100; ++i) q.decodeOnce();
  bool pending = true, cuttable = true, wanted = true;
  q.book.cutCheck(old, &pending, &cuttable, &wanted);
  TEST_ASSERT_FALSE(pending);
  q.read(44100);
  uint32_t token = 0;
  TEST_ASSERT_FALSE(q.book.takeAdvance(old, q.ring.readPos(), &token));
  TEST_ASSERT_FALSE(q.book.takeAdvance(q.gen, q.ring.readPos(), &token));
  TEST_ASSERT_EQUAL_UINT32(1000, q.book.positionMs(q.gen, q.ring.readPos()));  // b's, from its own start
}

// A word already taken (a track that failed) is "nothing follows" from
// then on; a new token is a new word.
void test_a_taken_word_is_taken_once() {
  GaplessJoin book;
  book.restart(1, 0, 0);
  GaplessJoin::Offer o;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::NoWord), static_cast<int>(book.take(1, 0, &o)));
  book.setOffer(1, 0, 5, "/x", 1234);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::NoWord), static_cast<int>(book.take(1, 3, &o)));  // about another track
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::NoWord), static_cast<int>(book.take(2, 0, &o)));  // another request
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Next), static_cast<int>(book.take(1, 0, &o)));
  TEST_ASSERT_EQUAL_STRING("/x", o.path.c_str());
  TEST_ASSERT_EQUAL_UINT32(1234, o.hintMs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Nothing), static_cast<int>(book.take(1, 0, &o)));
  book.setOffer(1, 0, 6, "/x", 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Next), static_cast<int>(book.take(1, 0, &o)));
  book.setOffer(1, 0, 0, "", 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Nothing), static_cast<int>(book.take(1, 0, &o)));
  // A new request drops the old one's word.
  book.setOffer(1, 0, 7, "/y", 0);
  book.restart(2, 0, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::NoWord), static_cast<int>(book.take(2, 0, &o)));
}

// Two requests in a row, the second's word given while the decode task
// still starts the first: the first's restart keeps the newer word, and
// the second's request takes it. Older words go, across the 2^32 wrap.
void test_a_newer_request_s_word_survives_the_restart_before_it() {
  GaplessJoin book;
  GaplessJoin::Offer o;
  book.restart(1, 0, 0);
  book.setOffer(3, 0, 8, "/c", 0);  // g+2's word...
  book.restart(2, 0, 0);            // ...before g+1's start
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::NoWord), static_cast<int>(book.take(2, 0, &o)));
  book.restart(3, 0, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Next), static_cast<int>(book.take(3, 0, &o)));
  TEST_ASSERT_EQUAL_STRING("/c", o.path.c_str());

  GaplessJoin w;
  w.restart(0xFFFFFFFEu, 0, 0);
  w.setOffer(0xFFFFFFFFu, 0, 9, "/d", 0);
  w.restart(0xFFFFFFFFu, 0, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Next), static_cast<int>(w.take(0xFFFFFFFFu, 0, &o)));
  w.setOffer(0xFFFFFFFFu, 0, 10, "/e", 0);  // the old request's...
  w.restart(0, 0, 0);                       // ...gone past the wrap
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::NoWord), static_cast<int>(w.take(0, 0, &o)));
  w.setOffer(1, 0, 11, "/f", 0);  // the next one's, before 0's start...
  w.restart(0, 0, 0);
  w.restart(1, 0, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(GaplessJoin::Answer::Next), static_cast<int>(w.take(1, 0, &o)));
}

// A cut under way is never taken as an advance, even with the reader past
// B; once it comes back too late (Committed) it is.
void test_no_advance_while_cutting() {
  GaplessJoin book;
  book.restart(1, 1000, 0);
  GaplessJoin::Boundary b;
  b.gen = 1;
  b.after = 0;
  b.token = 9;
  b.cutAt = 5000;
  b.heardAt = 5024;
  book.joined(b);
  TEST_ASSERT_TRUE(book.beginCut(1));
  uint32_t token = 0;
  TEST_ASSERT_FALSE(book.takeAdvance(1, 6000, &token));
  TEST_ASSERT_FALSE(book.beginCut(1));  // (already)
  book.cutCrossed();
  TEST_ASSERT_FALSE(book.beginCut(1));  // committed: never cut
  TEST_ASSERT_TRUE(book.takeAdvance(1, 6000, &token));
  TEST_ASSERT_EQUAL_UINT32(9, token);
  TEST_ASSERT_EQUAL_UINT32(22, book.positionMs(1, 6000));  // 976 frames of it
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_same_rate_joins_are_one_stream);
  RUN_TEST(test_self_trimming_48k_tracks_join_as_opus_does);
  RUN_TEST(test_rate_change_joins_are_streams_back_to_back);
  RUN_TEST(test_an_unknown_rate_resets_and_the_mp3_order_continues);
  RUN_TEST(test_the_same_track_again);
  RUN_TEST(test_a_track_shorter_than_the_ring);
  RUN_TEST(test_a_track_under_250_ms_ends_as_before);
  RUN_TEST(test_the_advance_comes_when_the_reader_passes_b);
  RUN_TEST(test_b_at_a_converting_rate);
  RUN_TEST(test_a_change_before_the_end_is_just_taken);
  RUN_TEST(test_a_change_after_the_join_cuts_the_decoded_ahead_track);
  RUN_TEST(test_a_change_after_the_reader_passed_j_is_too_late);
  RUN_TEST(test_nothing_or_gapless_off_after_the_join_cuts_and_ends);
  RUN_TEST(test_two_changes_in_a_row);
  RUN_TEST(test_a_cut_while_the_joined_track_drains);
  RUN_TEST(test_the_reader_at_j_equals_b_during_a_cut);
  RUN_TEST(test_a_pending_cut_is_retried);
  RUN_TEST(test_a_late_word_while_draining);
  RUN_TEST(test_a_late_word_too_close_to_the_end_is_not_taken);
  RUN_TEST(test_a_next_track_that_fails_to_open);
  RUN_TEST(test_a_joined_track_with_no_frames_is_cut_out);
  RUN_TEST(test_an_early_end_in_a_joined_track);
  RUN_TEST(test_words_and_boundaries_belong_to_their_request);
  RUN_TEST(test_a_taken_word_is_taken_once);
  RUN_TEST(test_a_newer_request_s_word_survives_the_restart_before_it);
  RUN_TEST(test_no_advance_while_cutting);
  return UNITY_END();
}
