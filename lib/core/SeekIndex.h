// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>
#include <mutex>

#include "FrameCursor.h"
#include "ResumeAnchor.h"

class TrimFeed;

// The run index (docs/SEEK.md section 4.3): while a track decodes, where
// every 4th MP3 frame begins in the file and in time. A run is one
// decoder's pass through one file, from its start (the top, or a plan) to
// its end. From it:
// - a pause's resume anchor (anchorAt(): the sample the outputs read next,
//   as the bytes a later start needs to pick up on it exactly);
// - a seek back into what the run decoded, exact (find(): the console's qs,
//   before the request's own start resets the slots).
//
// Two slots, the gapless player's two tracks: the heard one and the one
// decoded ahead. A request resets both (after its own lookup) and decodes
// into one; a join decodes into the other (beginJoin()); the advance makes
// it the heard one (advance(), with the boundary book's); a cut clears it
// (cut()). A FLAC or Opus run has no entries (libFLAC seeks by sample
// itself; the Opus reader plans a start by a bisection on the pages'
// granule positions, lib/core/OggOpus): its header makes its anchors, the
// sample with the file's size and its length (docs/SEEK.md section 4.4,
// docs/OPUS.md).
//
// Times are samples on the run's timeline at the file's rate: the trimmed
// timeline (docs/GAPLESS.md section 4.6) for a run from the top or by an
// exact plan or anchor; a TOC start's shown time otherwise (exact false).
//
// The decode task writes (begin(), notePass(), ...), any task reads, under
// the index's mutex. The entries are the caller's (the firmware's: 2 x 24 KB
// of PSRAM). Portable, host-tested (test_seek_index).
class SeekIndex {
public:
  struct Entry {
    uint32_t byte;  // a frame's file offset
    int32_t t0;     // its first sample on the run's timeline (< 0 inside the start trim)
    uint32_t hash;  // resumeanchor::frameHash() of its first bytes
  };
  static constexpr uint32_t kCapacity = 2048;  // the firmware's, per slot: the last 8,192 frames, 3.6 min at 44.1 kHz
  static constexpr uint32_t kEveryFrames = 4;
  // A lookup's landing entry is at most this many frames before the time
  // asked (entries are 4-5 frames apart): beyond, the run doesn't cover it.
  static constexpr uint32_t kMaxSkipFrames = 16;
  // A preroll further back than this isn't used (trackseek's check).
  static constexpr uint32_t kMaxPrerollSpan = 65536;

  enum class Kind : uint8_t { None, Mp3, Flac, Opus };
  struct Run {
    Kind kind = Kind::None;
    uint32_t gen = 0;         // the request's generation (TransportSync)
    uint32_t pathHash = 0;
    uint32_t fileSize = 0;
    uint32_t rate = 0;
    uint32_t spf = 0;         // MP3
    uint64_t base = 0;        // the timeline's sample of the run's first kept sample
    bool exact = false;       // the timeline is the file's own (see the class)
    uint64_t totalSamples = 0;  // FLAC: STREAMINFO's; Opus: the exact trimmed length (the tail scan's; 0: not known)
    // MP3: the frame the run started on, as an entry the ring can't
    // overwrite: the first audio frame from the top (t0 = -(delay + 529)
    // with LAME's tag, its preroll itself), or a plan's landing frame (its
    // preroll the plan's). None: the start's timeline was lost (a landing
    // elsewhere).
    bool origin = false;
    uint32_t originByte = 0;
    int64_t originT0 = 0;
    uint32_t originPreroll = 0;
    uint32_t originHash = 0;
  };

  // The two slots' entries, `capacity` each (none: no MP3 index, FLAC only).
  void setStorage(Entry* a, Entry* b, uint32_t capacity);
  bool ready() const { return slots_[0].e != nullptr && slots_[1].e != nullptr; }

  // ---- the decode task ----
  // A request: both slots empty, the first one heard and decoding.
  void reset();
  // A joined track decodes into the other slot (cleared).
  void beginJoin();
  // The joined track taken back out (a cut, or its start failed): its slot
  // cleared, the heard track's decodes again.
  void cut();
  // The decoding slot's run begins (its entries cleared).
  void begin(const Run& run);
  // The decoding run's start landed: its base and whether it is exact
  // (a landing a frame late: base + lateBy, still exact; elsewhere: not,
  // and without its origin).
  void rebase(uint64_t base, bool exact, bool origin);
  // A decoding pass ended with the sample the cursor describes refused:
  // the next to go in, at `nextSample` on the run's timeline. Records its
  // frame when it is kEveryFrames after the last entry (or the first).
  // True: an entry was written.
  bool notePass(const FrameCursor& cursor, int64_t nextSample);

  // ---- the loop task ----
  // The joined track is heard (with GaplessJoin::takeAdvance()).
  void advance();

  // ---- any task ----
  // The heard slot's run (false: none).
  bool heardRun(Run* out) const;
  // The anchor for a pause of the heard run of request `gen`, the outputs
  // `ringFrames` (at `ringRate`) past its first frame. False: none (another
  // request's run, no run, nothing covers it).
  bool anchorAt(uint32_t gen, uint32_t ringFrames, uint32_t ringRate, ResumeAnchor* out) const;
  // An MP3 start at `t` in the file of `pathHash` and `fileSize` from
  // either slot's run of that file (the anchor a seek would start by;
  // its fileSize filled in). False: no run of it covers `t`.
  bool find(uint32_t pathHash, uint32_t fileSize, uint64_t t, ResumeAnchor* out) const;

  // The run's sample `ringFrames` (at `ringRate`, the ring's 44.1 kHz)
  // after its first: exact at the ring's rate, else within one source
  // sample (ring frame n is n / ringRate s of the source: RESAMPLER.md 5).
  static uint64_t sampleAt(uint64_t base, uint32_t ringFrames, uint32_t rate, uint32_t ringRate);

  // For the console and the tests.
  uint32_t entries(int slot) const;
  int heardSlot() const;
  int decodingSlot() const;

private:
  struct Slot {
    Run run;
    Entry* e = nullptr;
    uint32_t cap = 0;
    uint32_t head = 0;   // the oldest entry
    uint32_t count = 0;
    Entry last{0, 0, 0};
  };
  static void clear(Slot& s);
  static const Entry& at(const Slot& s, uint32_t i) { return s.e[(s.head + i) % s.cap]; }
  static bool lookup(const Slot& s, uint64_t t, ResumeAnchor* out);

  mutable std::mutex lock_;
  Slot slots_[2];
  uint8_t heard_ = 0;
  uint8_t decoding_ = 0;
};

// The decode task's side of a run (docs/SEEK.md section 4.3, "Recording"):
// after each decoding pass that ended on a refused sample, the landing of a
// planned start settled once (a frame late: the base moves by lateBy, still
// exact; elsewhere: not exact, no origin), then, once past the start's
// skip, the cursor's frame noted at base + TrimFeed::kept(). The backend's
// and the tests' model decoder's, the same code.
class SeekRecorder {
public:
  enum class Settled : uint8_t { None, Late, Elsewhere };

  // A run begins in `index` (SeekIndex::begin() done), its first kept
  // sample at `base`; `landingDue`: a planned start (TrimFeed::armAt()).
  void begin(SeekIndex* index, uint64_t base, bool exact, bool landingDue) {
    index_ = index;
    base_ = static_cast<int64_t>(base);
    exact_ = exact;
    landingDue_ = landingDue;
  }
  void stop() { index_ = nullptr; }
  bool active() const { return index_ != nullptr; }
  // After a pass (see the class). Late or Elsewhere: once, when the landing
  // settled so.
  Settled afterPass(const TrimFeed& trim, const FrameCursor& cursor);
  // The run's base now (a late landing moved it).
  uint64_t base() const { return static_cast<uint64_t>(base_); }

private:
  SeekIndex* index_ = nullptr;
  int64_t base_ = 0;
  bool exact_ = false;
  bool landingDue_ = false;
};
