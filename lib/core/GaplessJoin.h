// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>
#include <mutex>
#include <string>

// The boundary book (docs/GAPLESS.md section 3.4): what the loop task (the
// player) and the decode task share about gapless joins, under its own
// lock (never the outputs': they only read the ring).
//
// - The offer: the player's word on what follows a track (IAudioBackend::
//   setNext()). `after` names the track it follows by its token (0: the
//   one the request started; a joined track has its offer's token), so a
//   word about the track before is never taken for the track after it.
//   Token 0: nothing follows. Stamped with the request's generation: an
//   offer made before the newest request is never taken.
// - The boundary: where the next track begins in the ring once the decode
//   task has joined it (B: heardAt), and where a cut goes back to (J:
//   cutAt). At most one at a time: the player names the track after a
//   joined one only once it has heard the join.
// - The heard track: its first ring frame and where it started (ms), and
//   once its source has ended its exact length. positionMs() stops at B
//   until the loop takes the advance (takeAdvance(): strictly past B, so
//   at least one frame of the next track has been read).
//
// Portable, host-tested (test_gapless).
class GaplessJoin {
public:
  static constexpr uint32_t kRingRate = 44100;

  struct Offer {
    uint32_t gen = 0;
    uint32_t after = 0;  // the token of the track it follows
    uint32_t token = 0;  // 0: nothing follows
    uint32_t hintMs = 0;
    std::string path;
  };
  // What the decode task finds for the track it is at the end of.
  enum class Answer : uint8_t {
    NoWord,   // the player hasn't said yet (it hasn't heard that track begin, or another request)
    Nothing,  // nothing follows (or what was offered was taken already: it failed)
    Next,     // this follows: taken
  };
  enum class State : uint8_t {
    Pending,    // cuttable
    Cutting,    // the decode task is cutting it: never taken meanwhile
    Committed,  // a cut came too late (the reader was past J): it stays
  };
  struct Boundary {
    uint32_t gen = 0;
    uint32_t after = 0;    // the token of the track before
    uint32_t token = 0;    // the joined track's
    uint32_t cutAt = 0;    // J: the ring's write index at the track before's end
    uint32_t heardAt = 0;  // B: the joined track's first frame
  };

  // ---- the loop task ----
  // The player's word (replaces the last).
  void setOffer(uint32_t gen, uint32_t after, uint32_t token, const std::string& path, uint32_t hintMs);
  // The reader has read past the boundary of this generation, and no cut
  // is under way: its token, and the heard track is the joined one from
  // now (its start B, at 0 ms; its length if its source has ended too).
  // Once per boundary.
  bool takeAdvance(uint32_t gen, uint32_t readPos, uint32_t* token);
  // The heard track's position at `readPos` (held at B while a boundary of
  // `gen` waits to be taken).
  uint32_t positionMs(uint32_t gen, uint32_t readPos) const;
  // The heard track's start in ms (a resume point's landing; 0 after a join).
  uint32_t startMs() const;
  // Its exact length once its source has ended; false while it decodes.
  bool frozenLength(uint32_t* ms) const;

  // ---- the decode task ----
  // A request starts at ring index `startIdx`, `startMs` in: the boundary,
  // the heard record and any older generation's offer go. A newer one
  // stays: the loop can post a second request (and its word) while the
  // decode task is still starting the first, and the word is matched once
  // the task reaches that generation (take() and cutCheck() want it equal).
  void restart(uint32_t gen, uint32_t startIdx, uint32_t startMs);
  // Where the request's track really landed.
  void setStartMs(uint32_t ms);
  // The word on what follows the track `after`; Next takes it (once).
  Answer take(uint32_t gen, uint32_t after, Offer* out);
  // The decoding track's source ended at exactly `lengthMs`: the heard
  // track's, or (a boundary waiting) the joined track's, for the advance.
  void freeze(uint32_t gen, uint32_t lengthMs);
  void joined(const Boundary& b);
  // A cut, as the decode task sees it: `pending` a boundary of `gen` not
  // yet taken; `wanted` it is cuttable (Pending) and the offer for the
  // track before now names another track or nothing.
  void cutCheck(uint32_t gen, bool* pending, bool* cuttable, bool* wanted) const;
  // Pending -> Cutting; false: gone (taken), committed, or another gen.
  bool beginCut(uint32_t gen);
  void cutDone();     // the boundary removed (before any frame of what comes next)
  void cutCrossed();  // Cutting -> Committed

  // ---- for the console (any task) ----
  struct Status {
    bool offer = false;
    Offer offerNow;
    uint32_t lastTaken = 0;
    bool boundary = false;
    Boundary b;
    State state = State::Pending;
    bool frozen = false;
    uint32_t frozenMs = 0;
    uint32_t heardStart = 0;
  };
  Status status() const;

private:
  mutable std::mutex lock_;
  uint32_t gen_ = 0;  // the decode task's request
  bool offerSet_ = false;
  Offer offer_;
  uint32_t lastTaken_ = 0;
  bool hasBoundary_ = false;
  Boundary b_;
  State state_ = State::Pending;
  bool nextFrozen_ = false;
  uint32_t nextLengthMs_ = 0;
  uint32_t heardStart_ = 0;
  uint32_t heardStartMs_ = 0;
  bool frozen_ = false;
  uint32_t frozenMs_ = 0;
};
