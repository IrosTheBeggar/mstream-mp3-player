// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "AutoDjJoin.h"
#include "AutoDjRng.h"
#include "AutoDjTiers.h"
#include "CardAutoDj.h"
#include "CardContainer.h"

// The Auto DJ engine: one session's state and its pick (the engine design's
// 1.4 and 2.5-2.9, with the user's decisions of 2026-10-13: D1 every track
// that plays steers, a quick skip excepted; D2 BPM continuity and Harmonic
// mixing off by default; D3 a missing BPM or key at the odds of the rest;
// D5 no two-hop). When the queue's last entry starts, the firmware asks for
// one pick and appends it; the pick reads at most kReadsPerPick slots of
// the card's AutoDJ index (docs/AUTODJ-INDEX.md) through a SlotReader, and
// maps rows to this library's tracks through the join's Maps (AutoDjJoin).
// Portable and seedable, host-tested (test_autodj_engine) pick for pick
// against tools/autodj_ref.py, the same rule in Python
// (test/fixtures/autodj/sessions.json).
//
// The session (newSession(): a fresh stream, empty rings, no pin, no key
// anchor; never saved):
//   the ring     Follow the vibe's anchors: the session's last kAnchors rows,
//                newest last; a row that comes back moves to the end. The
//                seed is its first entry; Auto DJ's own picks join when
//                picked (the pick made as track n starts steers the one made
//                as n + 1 starts, as the mobile app), the listener's tracks
//                when they start (trackStarted(), D1); so does a pick made by
//                track id while the index was away, if it has come since. A
//                track skipped in its first kQuickSkipMs (trackLeft(): not
//                its natural end) leaves the ring and stays in both
//                cooldowns; so may a pick the listener removed before it
//                played (trackLeft(.., 0, false)). A track with no row never
//                anchors.
//   the pin      Stay on seed's one anchor: newSession()'s seed track when it
//                has a row; else (no seed track, a seed without a row, or the
//                Anchor changed: modeChanged()) the playing track's row at
//                the next Stay on seed pick, else that pick's own row (the
//                design's 2.2: the seed song, else the playing track at the
//                first pick, as the mobile app and the terminal). A pin
//                whose slot is damaged gives way to the random pick it
//                causes, as a seed without a row does. Its slot is the only
//                one read.
//   the seed     newSession()'s track; without one, the first track to start
//                (it anchors the ring, but is no pin) or the playing track at
//                the first pick.
//   cooldowns    the last kTrackCooldown rows played or picked (the seed
//                included; one that comes back moves to the end, as
//                mStream's ignoreList, random.js 1150-1157), and the last
//                kArtistCooldown artists (the filter word's 16-bit artist; 0
//                never cooled). Both soft.
//   key anchor   Harmonic mixing's Camelot reference, taken lazily while it
//                is on: at a pick with none, the playing track's key; still
//                none after the pick, the pick's. Kept when it is turned off.
//   by id        a cooldown of the last kTrackCooldown track ids, for picks
//                without an index (cleared by tracksRenumbered()).
//
// One pick (pick()):
//   1. The anchors: the ring (Follow the vibe) or the pin (Stay on seed).
//      Their slots come from a cache of kSlotCache (the ring's rows and the
//      pin are never evicted; the least recently used other slot is).
//      Missing ones are read now, newest first, at most kReadsPerPick reads
//      a pick (re-reads included); the rest are left out of this pick
//      (Stats::leftOut). A loop pass reads one ahead (prefetchDue(),
//      prefetch()), so a pick usually reads nothing.
//   2. The candidates: the union of the anchors' used slot entries, minus
//      the anchors and the playing track's row, minus rows that map to no
//      track; each with its best level and the sum of its levels, in row
//      order (the same order on the host and the device: the draws match).
//   3. The gate: best level >= floorLevel(floorPct, selFloor). Nothing
//      passes (dry): the kDry with the highest sum, ties by row.
//   4. The class of each: cooldown level x 8 + music rank (AutoDjTiers.h),
//      after D3's draws; the cooldown level is 2 for a cooled row, 1 for a
//      cooled artist, else 0. One uniform draw among the lowest class.
//   5. The pick joins the ring and the cooldowns; the key anchor, the pin.
// Random picks (no usable anchor, no candidate, a broken file):
// kRandomDraws uniform draws over the rows, each kept once unless it maps
// to no track or is an anchor or the playing track's; then the classes and
// the draw as above, without a gate (every draw is made: D3's odds need
// them all). Nothing usable, or no index (maps or slots null): a library
// track by id, not the playing one, up to kNoIndexTries draws for one
// outside the by-id cooldown.
//
// Slots (docs/AUTODJ-INDEX.md 8.5): one whose CRC fails twice in a row (in
// this pick if the budget allows, else at its next read) goes into a ring
// of kDamagedRing rows never read again until fileReopened(); a failed
// read (Why::Io: the card pulled, a FIL a remount invalidated) is skipped
// and never goes there; a Broken one (a good CRC over a bad list: the
// writer's bug) makes the file absent: every later pick is random until
// fileReopened() (Pick::broken, broken()).
//
// Memory: begin() takes about 15 KB from the hooks (the firmware's PSRAM):
// the slot cache (7 slots of 512 bytes: one to read into) and the union's
// scratch (kMaxUnion keys and candidates). pick() allocates nothing; its
// frames stay small (no slot or list on the stack). The object itself is
// about 700 bytes (rings, counters).
//
// Not thread-safe: the firmware's loop task owns it.
namespace autodj {

enum class Mode : uint8_t { Rolling, Locked };  // "Follow the vibe", "Stay on seed"

constexpr uint8_t kAnchors = 5;
constexpr uint8_t kTrackCooldown = 50;
constexpr uint8_t kArtistCooldown = 3;
constexpr uint32_t kQuickSkipMs = 30000;
constexpr uint8_t kReadsPerPick = 2;
constexpr uint8_t kSlotCache = 6;
constexpr uint8_t kDamagedRing = 8;
constexpr uint8_t kDry = 50;
constexpr uint8_t kRandomDraws = 64;
constexpr uint8_t kNoIndexTries = 16;
constexpr uint8_t kSurpriseDraws = 64;
constexpr uint32_t kNoTrackId = 0xFFFFFFFFu;  // a track id (u32): none
constexpr uint32_t kMaxUnion = kAnchors * cardcontract::mpdj::kMaxK;
constexpr uint8_t kClasses = 3 * kRanks;
// The strictness steps (the design's 2.6): 55 to 75 % by 5.
constexpr uint8_t kFloorMin = 55;
constexpr uint8_t kFloorMax = 75;
constexpr uint8_t kFloorStep = 5;

struct Settings {
  bool on = false;
  Mode mode = Mode::Rolling;
  uint8_t floorPct = kFloorMin;  // Match strictness, 55..75 (any percent works; the clamp is selFloor)
  bool matchTempo = false;       // "BPM continuity" (D2: off by default)
  bool matchKey = false;         // "Harmonic mixing" (D2: off by default)
};

// The firmware's: mpdj::Table::slot() on the open file. Fills `bytes` (the
// table's slotBytes, at most 512) for `row` and `count` with its used
// entries, the slot checked (8.4: S1-S4); decodes nothing. Damaged with
// Why::Io for a read that failed, any other Why for a CRC that failed;
// Broken for a good CRC over a bad list.
class SlotReader {
public:
  virtual ~SlotReader() = default;
  virtual cardcontract::mpdj::SlotRead read(uint16_t row, uint8_t* bytes, uint32_t* count, cardcontract::Why* why) = 0;
};

struct PickIn {
  const Maps* maps = nullptr;  // null: no index (picks by track id)
  SlotReader* slots = nullptr;  // null with no index
  uint16_t k = 0;               // the table's k: where a slot's levels start (2k)
  uint16_t selHead = 0;         // not used by v1 (two-hop, M7)
  uint8_t selFloor = 0;         // the gate's clamp
  uint32_t playingTrack = kNoTrackId;  // the current entry's track id
  uint32_t libraryTracks = 0;          // picks by id draw below it
};

struct Pick {
  bool ok = false;
  uint32_t track = kNoTrackId;
  uint16_t row = kNoRow;    // kNoRow: a pick by id of a track with no row
  uint8_t level = 0;        // its best level to an anchor (random: 0)
  uint8_t cooldownLevel = 0, musicRank = 0;
  bool dry = false;         // the gate passed nothing: the kDry best sums
  bool random = false;      // no anchor's list: drawn over the rows (or by id)
  bool noIndex = false;     // by track id
  bool imputedTempo = false, imputedKey = false;  // D3 drew its status
  bool tempoBreak = false;  // a known BPM outside the tight windows of a known reference
  bool keyBreak = false;    // a known key outside the anchor's 6 codes
  uint8_t reads = 0;        // slot reads this pick (0-kReadsPerPick)
  bool broken = false;      // the file is broken: absent from now (until fileReopened())
  uint16_t candidates = 0, gated = 0, inClass = 0;
};

// What prefetch() (and a pick's read) made of one read.
enum class SlotResult : uint8_t {
  Ok,       // cached
  Suspect,  // its CRC failed: one more failure in a row and it is damaged
  Damaged,  // failed twice in a row: in the damaged ring
  Io,       // the read failed (Why::Io): skipped, never in the ring
  Broken,   // the file is broken
};

// The console's D counters (since begin(); a session doesn't reset them).
struct Stats {
  uint32_t picks = 0;    // pick()s that gave a track
  uint32_t failed = 0;   // ... that gave none (no library track to draw)
  uint32_t readsAtPick[kReadsPerPick + 1] = {};
  uint32_t prefetchReads = 0;
  uint32_t leftOut = 0;  // anchors left out of a pick by the read budget
  uint32_t damaged = 0;  // rows put in the damaged ring
  uint32_t ioSkips = 0;
  uint32_t broken = 0;
  uint32_t dry = 0, random = 0, noIndex = 0;
  uint32_t imputedTempo = 0, imputedKey = 0;
  uint32_t classes[kClasses] = {};  // picks with an index, by class
  uint32_t modePicks[2] = {}, tempoBreaks[2] = {}, keyBreaks[2] = {};  // by Mode
  uint32_t quickSkips = 0;
};

// A fixed ring of the last N values, oldest first; add() moves a value
// already in it to the end.
template <typename T, uint8_t N>
class Ring {
public:
  uint8_t size() const { return n_; }
  T at(uint8_t i) const { return v_[i]; }
  bool has(T x) const {
    for (uint8_t i = 0; i < n_; ++i)
      if (v_[i] == x) return true;
    return false;
  }
  bool remove(T x) {
    for (uint8_t i = 0; i < n_; ++i) {
      if (v_[i] != x) continue;
      for (uint8_t j = i + 1; j < n_; ++j) v_[j - 1] = v_[j];
      --n_;
      return true;
    }
    return false;
  }
  void add(T x) {
    remove(x);
    if (n_ == N) remove(v_[0]);
    v_[n_++] = x;
  }
  void clear() { n_ = 0; }

private:
  T v_[N] = {};
  uint8_t n_ = 0;
};

class Engine {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  Engine() = default;
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // The slot cache and the scratch from the hooks (nullptr: malloc/free);
  // false when an allocation failed (nothing held: picks by id only).
  bool begin(AllocFn alloc = nullptr, FreeFn release = nullptr);
  void end();
  size_t heldBytes() const;

  // ---- the session ----
  // A new session: `seed` the stream's (the firmware's esp_random(), or the
  // console's Ds), `seedTrack` the seed (kNoTrackId: the first track to
  // start, or the playing one at the first pick).
  void newSession(uint32_t seed, uint32_t seedTrack = kNoTrackId, const Maps* maps = nullptr);
  // A track started: the listener's (`byAutoDj` false) joins the ring and
  // the cooldowns; Auto DJ's joined when picked (one picked by track id
  // without the index joins now, if `maps` knows its row and it isn't in
  // the cooldown).
  void trackStarted(uint32_t track, const Maps* maps, bool byAutoDj);
  // A track stopped being the current entry after `playedMs`: not a natural
  // end and under kQuickSkipMs, it leaves the ring (a quick skip).
  void trackLeft(uint32_t track, const Maps* maps, uint32_t playedMs, bool naturalEnd);
  // The same by its row (kNoRow: none leaves): the firmware's loop step
  // remembers a track's row while the maps are away (the join again after
  // a library update's fence; rows don't change there).
  void rowLeft(uint16_t row, uint32_t playedMs, bool naturalEnd);
  // A pick the queue refused (full of tracks not played yet, or no
  // memory): out of the ring, as it never played; no quick skip; it stays
  // in the cooldowns.
  void pickRefused(uint16_t row);
  // Anchor changed: the pin goes; the next Stay on seed pick takes it.
  void modeChanged();
  // The index file was (re)opened: the slot cache, the damaged ring and
  // the broken mark cleared (rows may mean other slots now).
  void fileReopened();
  // The library was rebuilt (track ids renumbered): the by-id cooldown
  // cleared. The rings hold rows and stay.
  void tracksRenumbered();

  // ---- reads ----
  // The anchor slot one loop pass should read ahead (the newest missing in
  // the ring, or the pin in Stay on seed), or false.
  bool prefetchDue(const Settings& s, uint16_t* row) const;
  // One read of it.
  SlotResult prefetch(SlotReader& slots, uint16_t row);

  // ---- picks ----
  Pick pick(const Settings& s, const PickIn& in);
  // The empty states' Auto DJ button: a random row that maps to a track
  // (up to kSurpriseDraws draws); the session is untouched but for its
  // stream. Not ok without an index.
  Pick surprise(const Settings& s, const PickIn& in);

  // The console's Dn: this engine takes `from`'s session, slot cache and
  // file state (it needs its own begin()), so dry-run picks leave the live
  // session untouched.
  void copySession(const Engine& from);

  // ---- what it holds (the console's D, the tests) ----
  const Stats& stats() const { return stats_; }
  uint8_t anchors() const { return ses_.ring.size(); }
  uint16_t anchor(uint8_t i) const { return ses_.ring.at(i); }  // oldest first
  uint16_t pin() const { return ses_.pin; }
  uint8_t keyAnchor() const { return ses_.keyAnchor; }
  bool seeded() const { return ses_.seeded; }
  uint8_t cooledRows() const { return ses_.cool.size(); }
  bool rowCooled(uint16_t row) const { return ses_.cool.has(row); }
  uint8_t cooledArtists() const { return ses_.arts.size(); }
  uint16_t cooledArtist(uint8_t i) const { return ses_.arts.at(i); }
  uint8_t cooledIds() const { return ses_.ids.size(); }
  bool idCooled(uint32_t track) const { return ses_.ids.has(track); }
  uint8_t damagedRows() const { return damaged_.size(); }
  bool rowDamaged(uint16_t row) const { return damaged_.has(row); }
  bool cached(uint16_t row) const { return find(row) >= 0; }
  bool broken() const { return broken_; }

private:
  struct Session {
    Rng rng;
    Ring<uint16_t, kAnchors> ring;
    Ring<uint16_t, kTrackCooldown> cool;
    Ring<uint16_t, kArtistCooldown> arts;
    Ring<uint32_t, kTrackCooldown> ids;
    uint16_t pin = kNoRow;
    uint8_t keyAnchor = 0;
    bool seeded = false;
  };
  struct CacheSlot {
    uint16_t row = kNoRow;  // kNoRow: empty
    uint16_t count = 0;
    uint32_t stamp = 0;
  };
  // A candidate: its row, best level and level sum; its statuses as known
  // (t, k) and after D3 (ti, ki); its class.
  struct Cand {
    uint16_t row;
    uint16_t sum;
    uint8_t best;
    uint8_t cls;
    Tempo t, ti;
    Key k, ki;
  };

  void seed(uint32_t track, const Maps* maps, bool pin);
  void join(uint16_t row, const Maps& m);
  int find(uint16_t row) const;
  SlotResult read(SlotReader& slots, uint16_t row);
  void insert(uint16_t row, uint16_t count);
  void touch(int i) { cache_[i].stamp = ++clock_; }
  Pick randomPick(const Settings& s, const PickIn& in, uint16_t playingRow, const uint16_t* anchors, uint8_t na,
                  Pick out);
  uint32_t choose(const Settings& s, const Maps& m, uint32_t n, uint16_t playingRow, Pick& out);
  Pick finish(const Settings& s, const Maps& m, uint16_t row, Pick out);
  Pick byId(const Settings& s, const PickIn& in, Pick out);
  Pick counted(const Settings& s, const Pick& out);

  AllocFn alloc_ = nullptr;
  FreeFn free_ = nullptr;
  uint8_t* slotBytes_ = nullptr;  // (kSlotCache + 1) x 512: the cache, then the read buffer
  uint32_t* keys_ = nullptr;      // kMaxUnion
  Cand* cand_ = nullptr;          // kMaxUnion

  Session ses_;
  CacheSlot cache_[kSlotCache];
  uint32_t clock_ = 0;
  Ring<uint16_t, kDamagedRing> damaged_;
  uint16_t suspect_ = kNoRow;  // the row whose last read failed its CRC
  bool broken_ = false;
  Stats stats_;
};

}  // namespace autodj
