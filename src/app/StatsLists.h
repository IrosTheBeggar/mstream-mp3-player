// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <atomic>

#include "AutoDjJoin.h"
#include "LibraryIndex.h"
#include "ScanScheduler.h"
#include "StatsGlue.h"
#include "StatsMap.h"
#include "StatsRank.h"
#include "StatsVisit.h"
#include "TagStore.h"
#include "app/CardWorker.h"

// The listening stats' lists and map on the console (docs/LISTENING-STATS.md
// 6, 9.3, 9.6; S2): Hr, Hl<shelf>[w], Hw and Hp. The owner's "A: nothing
// held" (2026-11-20): nothing of them is held between asks, and S2 ranks
// only when asked (S3 decides how long a page holds its lists). Each ask, one
// at a time:
//   1. asked (command()); it waits for the stats on and the boot's scan done
//      (no hold of its own), no library update asked or under way, Auto DJ's
//      join neither Working nor due (6.8: a rank starts only when the join
//      is Ready or off), the card not away or held; then for a pass the
//      play log's write gate lets through, after a stats save due and the
//      plays waiting for their slots (PlayHistory's turn, as Hs and the
//      looks: its PSRAM carved only where theirs is);
//   2. at that turn, on the loop: stats.bin's directory read (its TRKD's
//      rows: the played set's room), ONE block carved at the top of PSRAM's
//      largest free block with the free block above it closed by caps (the
//      boot hold's way: autodj::TopHold, 5.14's fix A; a decoder's room
//      asked, as for the ranges' block: no room, the ask refused), holding
//      the ask's state (the ranker, the map, the walk's path buffer) and
//      every block its steps fill: the rank's work memory, the lists, the
//      30 days' RCNT rows, the records' copy's room, and for Hl or Hw the
//      walk's: the played set (Hw, Hl3), the album bits, the folders'
//      states, the wanted set. A list that needs the clock, with it unset,
//      is said at once and carves nothing;
//   3. at the hand-off (start(), the card worker free, on the loop): the
//      rank's input as the session stands then, the records' copy
//      (Fold::Cursor's rows, TRKD's and RCNT's: 5.14's "The screens"),
//      stats.bin and the base opened (a FatFile each, PSRAM: the worker
//      allocates nothing, 6.8); then the card worker's step
//      (ScanScheduler's Job::Stats): the rank in the
//      Rank lane (background work after a compaction: never under the
//      update step or a yield, priority 0 lit, 1 dark, never cut); Hp's read
//      in the Track lane beside the waveform read (a binary search of the
//      base: no sparse index is kept between asks). While it runs (from the
//      hand-off until it is taken in) no stats save and no slot (a slot's
//      rotation saves) is made: none replaces stats.bin under its reads;
//   4. Hr, Hp: the result printed, the block given back. Hl, Hw: the walk
//      (StatsMap over pathhash::Walk) on the loop after Auto DJ's step, up
//      to 8 ms a pass (the loop's idle wait short meanwhile, as for the
//      join's), never in a pass the join works, with a list moving or the
//      card held (not held by an update asked: the update waits for the
//      block); an index that changes under it begins it again (three times,
//      then the ask ends); then printed, given back.
// While the block is held (holds()): a library update asked waits for it
// (LibraryUpdate::In::statsWait: it is never in the update's memory check or
// across a build), and Auto DJ's join doesn't begin (its Env::statsHold).
// The fence can't come while it is held; should it all the same
// (enterFence(): drop()), the block is given back (its bytes in the fence's
// line, not the memory check's alsoFreed, made before), and the ask begins
// again after the Live. A step the card worker hasn't taken in 30 s is given
// up.
//
// S3a (docs/LISTENING-STATS.md 9.7, under "A: nothing held" and section
// 1's four calls): the stats pages' asks, typed (ask(): a visit, Not played
// yet's lean ask), in the same one slot, and their console twins (Hv, Hv1,
// Hn, Hn1, Hu; Hk, Hc). A visit's ask leaves its lists in the SNAPSHOT
// (lib/core StatsVisit), which lives in the Dance tab's 4,096 B tracker
// scratch, lent while that tab is down (the owner's "Borrow Dance's block",
// 2026-11-21: the stats own no memory, PSRAM stays as without them;
// DanceMode::lendScratch() through statshold::borrow()), held after the
// ask until release() (the pages leave, the dark, the card, the fence, the
// orderly end, the Dance tab coming up: dancelend::reclaim()...):
// snapshot() serves it once the visit has landed. Each ask:
//   1. asked; before its turn it waits for S2's gates, the tag scan while
//      it holds its memory (CardTasks' word each pass, restAhead(): the
//      owner's "Only while held"; kWhyRest; not at the battery floor, the
//      owner's "Don't wait at the floor") and the Rank lane's own waits
//      (a lit list moving, input in the last 0.5 s, the ring, an underrun,
//      a long decode pass, a track change, a seek, Bluetooth:
//      ScanScheduler::yieldNow(), CardTasks' word each pass, rankWait();
//      and this pass's input, Gates::input, which CardTasks' word only has
//      after the turn: a tap's or a console line's own pass never takes),
//      holding nothing: phaseOf()'s Working, then Waiting from 3 s. With
//      the card away or held it is dropped (NoCard) and the snapshot
//      released;
//   2. at the turn: a visit's snapshot lent first (the Dance tab's block,
//      if none is held: refused while that tab is up, or past 18,432
//      albums, No room; a re-ask rewrites the held one whole), then the
//      grades tried in order through the hold (statsvisit::layout(), the
//      real sizeof(Ask)): Full then Lists (Lists alone while Not played yet
//      can't open, or Hv1), Not played yet's Lean then No folders (Hn1: No
//      folders). Every grade refused: the snapshot released if this ask
//      borrowed it, No room. A refused take leaves no trace;
//   3. the rank on the card worker (the played-set-only rank for the lean
//      ask), then finishRank() on the loop: the wanted set in the block's
//      tail, sealed (a heap sort), statsvisit::write() into the snapshot,
//      the folders and bits at the block's head, the walk over the u40 set.
//      After the walk resolve() and notPlayed() (the lean ask's only then,
//      into the held snapshot: its lists stay served meanwhile), and the
//      block given back in that pass. A u40 set that lost a hash to its
//      room counts no Not played yet (a short set would show played albums
//      as not played);
//   4. its hold bounded: a wait between the take and the hand-off, the walk
//      skipping passes for 1 s and the 30 s worker give-up each give the
//      block back in the pass that sees it, the ask back in Asked, no count
//      (after the give-up held there 30 s more, holding nothing: Working,
//      then Waiting; not taken again at once for a worker that never comes).
//      A take that ends without landing says its figures (sayTake()).
//      The walk while lit: 100 tracks a slice, 4 ms a pass, none in a pass
//      that handled input; dark as S2's. The lit cap (kLitCap, on: the
//      owner's "Cap on"; Hc0/Hc1): the rank at priority 1 and the walk at
//      the dark's passes, lit or not, and a visit's block given back once
//      its holds reach kLitCapMs (Capped, no re-ask).
// listsHold() stays the ask's block alone (holds()): a held snapshot holds
// neither Auto DJ's join nor a library update. The fence releases it
// (drop(): no heap bytes, so none in the fence's line or the memory check's
// alsoFreed). An ask whose snapshot is released under it (the Dance tab,
// the fence) ends without writing (finishVisit(), landVisit(): Cancelled).
// The releases with no ask under way give the pages their phase (9.7's
// "Phase from why"): the card's and a remount's key No card (Try again once
// it is back), the stats off Idle; and every release moves the version
// (news()).
// Each phase change is said on the console (a [listen] line: the device
// runs read it).
// Loop task, but for the worker's step (its own ask's fields, published by
// state_).
// The top of PSRAM's hold for an ask's block (app/ListeningStats.cpp:
// autodj::TopHold over the heap, the boot hold's): take a block (the
// decoder's room asked; null: no room, `r` says why), give it back. And
// the snapshot's loan (main.cpp: DanceMode::lendScratch(), the Dance tab's
// 4,096 B block; null while that tab is up, for more than
// statsvisit::kBound, or with no block), given back by dropping the
// pointer; danceUp() says whether the tab is up (a refused loan's line).
namespace statshold {
void* take(size_t bytes, autodj::CloseReport* r);
void give(void* p);
void* borrow(size_t bytes);
bool danceUp();
}  // namespace statshold

class StatsLists {
public:
  // (The object is internal RAM's .bss, in app/ListeningStats: its state
  // alone, a few bytes; everything else is the ask's block.)
  void begin(statsglue::Glue* glue, tagstore::Fs* fs);

  // What the loop knows this pass.
  struct Gates {
    uint32_t nowMs = 0;
    const LibraryIndex* index = nullptr;  // Library::index() (nullptr behind the fence)
    bool updateAsked = false;             // a library update asked and not over (LibraryUpdate::asked())
    bool joinPending = false;             // Auto DJ's join Working or due (AutoDj::joinPending())
    bool joinWorking = false;             // ... Working this pass (AutoDj::joinWorking())
    bool cardHeld = false;                // the card's guard holds the card
    bool cardAway = false;                // out of the slot, refusing writes, another card
    bool uiStarted = true;
    bool listMoving = false;
    uint64_t playingHash = 0;             // the open play's path hash (Hp; 0: none)
    bool lit = false;                     // the UI up and lit (CardTasks' covers): a visit's walk's pace, its lines
    // Input this pass (main.cpp's anyInput: a touch, a console line): a
    // visit's walk skips it while lit (or capped), and an S3a ask takes no
    // turn in it (the Rank lane's Input wait, which CardTasks hands over
    // only after this pass's turn: rankWait()).
    bool input = false;
  };

  // The console's H's letters r, l, w, p, and S3a's v, n, u, k, c (the text
  // from that letter on).
  void command(const char* arg);

  // ---- S3a: the stats pages' asks (docs/LISTENING-STATS.md 9.7's API) ----
  enum class UiAsk : uint8_t { Visit, NotPlayed };
  using Phase = statsvisit::Phase;
  using Outcome = statsvisit::Outcome;
  // Queued (Asked): true. Behind a console ask under way, it waits for it;
  // another UI ask under way is cancelled first. Dropped at once (false)
  // with the stats off, with the card away or held (`cardGone`: NoCard, the
  // snapshot released), and Not played yet's with no landed snapshot to
  // write.
  bool ask(UiAsk a, bool cardGone);
  // Asked: dropped; queued with its block: given back; on the worker: its
  // result dropped at its end (a step is never cut: about 1 s at most);
  // walking: map.release(), given back that pass. A console ask goes on.
  void cancel();
  // cancel(), and the snapshot released (`why`: its [listen] line's words;
  // the Dance tab's block given back to it). A console ask under way goes
  // on, and ends without writing (Cancelled) if it had borrowed it.
  void release(const char* why);
  // The pages' phase (statsvisit::phaseOf(): Ready only while snapshot()
  // serves one; a release with no ask under way gives No card for the card
  // or a remount's key, Idle for the stats off).
  Phase phase() const;
  uint8_t why() const { return why_; }  // S2's gate bits holding the ask (statsvisit::kWhy...)
  Outcome outcome() const { return outcome_; }
  // +1 at each phase change, each landing and each release of the snapshot,
  // and when a visit asked again over a served snapshot rewrites it
  // (finishVisit()'s write(): snapshot() null from there to the landing).
  uint16_t version() const { return version_; }
  bool news();  // the version moved since the UI last asked
  // The held snapshot once the visit's ask has landed (resolved); else null.
  // (A visit asked again over it serves it until its rank's write(), which
  // moves the version: the pages read it at each use, not a pointer kept.)
  const statsvisit::Snapshot* snapshot() const {
    return snap_ && snap_->resolved() ? snap_ : nullptr;
  }
  // Most played's window into the held snapshot's header (no ask).
  void setWindow(uint8_t w) {
    if (snap_ && snap_->written()) snap_->setWindow(w);
  }
  // The held snapshot's room (0: none): the Dance tab's block, lent whole,
  // statsvisit::kBound (4,096 B): write()'s bytes, the albums' check, the
  // lines.
  size_t snapshotBytes() const { return snap_ ? statsvisit::kBound : 0; }
  // ... and the heap's bytes it holds: none, held or not (the block is the
  // Dance tab's, carved at the boot and kept). The fence releases it:
  // drop()'s count and main.cpp's memory check (alsoFreed) take this.
  size_t snapshotHeapBytes() const { return 0; }

  // ---- the loop (app/PlayHistory, main.cpp) ----
  // An ask waits and may carve now (PlayHistory's turn, statsglue::turnOf()'s
  // Hs: after a save due and the slots due).
  bool turnWanted(const Gates& g) const;
  // At that turn: the carve, the records' copy, the opens; or a refusal.
  void turn(const Gates& g);
  // Every pass, before the card's step: a step the worker finished taken in
  // (the files closed on the loop), its result printed or the walk begun.
  void loop(const Gates& g);
  // After Auto DJ's step: the walk's slices.
  void stepMap(const Gates& g);

  // ---- the card worker (app/CardTasks) ----
  // The card worker has background work a rank comes after (a walk, a
  // compaction): a rank's ask waits for it before it takes its block (each
  // pass, CardTasks' word).
  void workerAhead(bool ahead) { setAhead(kAheadWalk, ahead); }
  // S3a: the card worker's tag scan holds its memory (CardJobs::scanHeld():
  // its work memory and any View; the owner's "Only while held",
  // 2026-11-21): a visit's ask (and the lean ask) waits it out before its
  // take, and one queued gives its block back (gateOf()'s kWhyRest); S2's
  // asks don't wait for it. Each pass, CardTasks' word
  // (statsvisit::restHolds()): a fresh card's or a Rescan's whole rest
  // holds them (minutes); not while the memory is one no step gives back
  // (CardJobs::scanStuck()), nor while the battery floor pauses the scan
  // (ScanScheduler::batteryLow(): the memory carved mid-rest stays held
  // there; the owner's "Don't wait at the floor", 2026-11-22). Then a
  // visit goes ahead, and its take still checks there's room for a decoder
  // (statshold::take()). It stops a take only: a scan that starts during a
  // visit's hold (from its rank's end through its walk) carves beside it.
  void restAhead(bool held) { setAhead(kAheadRest, held); }
  // S3a: the Rank lane's own wait this pass (ScanScheduler::yieldNow()): a
  // visit's ask takes no block while it isn't None, and one queued gives its
  // block back (each pass, CardTasks' word, as workerAhead(); after the
  // turn's pass, so the turn reads Gates::input too).
  void rankWait(ScanScheduler::Wait w) { rankWait_ = static_cast<uint8_t>(w); }
  // A rank's priority: `p` (ScanScheduler's), or 1 for a visit's ask under
  // the lit cap (it ranks as dark), at the hand-off and while it runs.
  uint8_t rankPriority(uint8_t p) const;
  bool wantsRank() const { return state_.load() == kRankQueued; }
  bool wantsTrack() const { return state_.load() == kTrackQueued; }
  bool rankRunning() const {
    const uint8_t s = state_.load();
    return (s == kRunning || s == kDone) && rank_;
  }
  // Hands the queued step to the worker (Job::Stats; `rank`: the Rank lane's;
  // `dark`: the screen, for a visit's line).
  bool start(CardWorker& worker, uint8_t priority, bool rank, uint32_t nowMs, bool dark);
  // The worker's step handed and not yet taken in (its files open): no stats
  // save and no slot meanwhile.
  bool workerOwned() const {
    const uint8_t s = state_.load();
    return s == kRunning || s == kDone;
  }

  // ---- the rest of the firmware ----
  // The ask holds its block (a library update waits; Auto DJ's join doesn't
  // begin).
  bool holds() const { return block_ != nullptr; }
  // The walk works (main.cpp: the loop's idle wait short meanwhile).
  bool walking() const { return state_.load() == kWalk; }
  // The update's fence went up: an ask's block (it shouldn't be held) given
  // back and the stats pages' snapshot released (the heap's bytes given
  // back, for the fence's line: the block's alone, the snapshot's none);
  // S2's and the console's asks kept to begin again after the Live (Hu's
  // visit too, its snapshot lent again), a UI ask cancelled (the fence
  // takes the tab back to Artists).
  size_t drop();
  // The orderly end: a worker step under way finished first (at most
  // `maxMs`), then the block given back and the snapshot released.
  void ending(uint32_t maxMs);

private:
  enum Kind : uint8_t { kNone, kRank, kList, kWalkAsk, kTrack, kVisit, kLean };
  enum State : uint8_t { kIdle, kAsked, kRankQueued, kTrackQueued, kRunning, kDone, kWalk };
  // flags_: the ask is the UI's; forced (Hv1: Lists; Hn1: No folders); the
  // snapshot is Hu's (the console's held fake visit, lent to Hu's ask:
  // kept as a page keeps it, lent again after a fence); the lit cap on
  // (Hc1); the visit capped; this ask borrowed the snapshot (kCarved: the
  // Dance tab's block lent to it); stalled (an S3a
  // ask the card worker took no step of in 30 s: in Asked, holding nothing,
  // statsvisit::kStallMs from that give-up before its next take: gateOf()'s
  // kWhyWorker, the wait's start kept from it until the take); the ask is
  // Hu's.
  static constexpr uint8_t kFromUi = 1, kForced = 2, kFake = 4, kCapOn = 8, kCapped = 16, kCarved = 32,
                           kStalled = 64, kHu = 128;
  // Hk's pace (stepHk()): a part, or a row, only while the loop's TX buffer
  // has room for its worst case (Serial.availableForWrite(): at most 1,016
  // B, half the 2 KB ring less an item's header, reported empty), so no
  // pass waits on the UART; the rest at later passes.
  static constexpr int kHkPieceRoom = 768;  // a header's piece (its lines at most about 700 B)
  static constexpr int kHkRowRoom = 320;    // a list's row (a 255 B title: 319 B), its list's line or an album's
  struct Ask;  // the block's head (StatsLists.cpp)
  static void entry(void* self);
  bool prepare();
  void giveBack();
  void finishRank(const Gates& g);
  void finishTrack();
  void printLists(const Gates& g);
  void printWalk(const Gates& g);
  void refuse(const char* why);
  bool waitWorker(uint32_t maxMs);  // the worker's step under way ended (at most maxMs): true
  const char* name() const;
  // S3a's.
  bool visiting() const { return kind_ == kVisit || kind_ == kLean; }
  uint8_t gateOf(const Gates& g) const;  // S2's gate bits holding an ask in Asked (0: none)
  // The Rank lane's wait this pass: CardTasks' word, or Input for this
  // pass's input (Gates::input), which its word has only after the turn.
  ScanScheduler::Wait laneWait(const Gates& g) const;
  Phase phaseAt(uint32_t nowMs, ScanScheduler::Wait lane) const;
  // The pages' phase changed (`p`): its line (`heldMs`: an ask in Asked,
  // how long its waits held it to this pass).
  void sayPhase(Phase p, uint32_t nowMs, ScanScheduler::Wait lane, uint32_t heldMs) const;
  const char* whyName(uint8_t why) const;  // a gate bit's words (kWhyWorker while stalled: the give-up's)
  void beginAsk(Kind k, uint8_t flags, uint32_t nowMs);
  void turnVisit(const Gates& g);
  void finishVisit(const Gates& g);
  void stepVisit(const Gates& g);
  void landVisit(const Gates& g);
  uint32_t holdMs() const;  // the block's hold so far (0: none held)
  bool capReached() const;  // the lit cap on, and the visit's holds at kLitCapMs
  void addHold(uint32_t ms);
  // A take that ends without landing: its figures (9.7's lit hold: the
  // screen at the take, the rank's start and the walk's; the priority; the
  // queued, rank and walk ms, the passes and the longest; the loop's
  // pieces), said while block_ is held (landVisit()'s line has a landing's).
  void sayTake() const;
  void putBack(const char* why, uint32_t nowMs);   // the block given back, the ask in Asked again (no count)
  void endVisit(Outcome o, const char* why);        // the ask ended (its block back), `why` said
  void capVisit();                                  // ... at the lit cap: Capped, no ask by itself
  void stop(Outcome o, const char* why);            // the ask under way stopped (cancel(), the card, the stats off)
  // The snapshot released (the pointer dropped: the Dance tab's block is
  // the tab's again, nothing freed), `why` said: its line's us
  // (turnVisit()'s leaves it out of the take's turn, as its other lines;
  // 0: none held).
  uint32_t freeSnap(const char* why);
  // kAheadWalk: the card worker's walk or a compaction comes before a rank
  // (workerAhead()); kAheadRest: the tag scan holds its memory (restAhead()).
  static constexpr uint8_t kAheadWalk = 1, kAheadRest = 2;
  void setAhead(uint8_t bit, bool on) { ahead_ = static_cast<uint8_t>(on ? ahead_ | bit : ahead_ & ~bit); }
  // The held snapshot's header as a page reads it (Hk's first parts, a
  // console visit's landing): `piece` 0 all of it; 1 the key and the rows'
  // counts, 2 the 7 days, 3 DAYS' span and Not played yet's count (Hk's,
  // a part each). True: written (its rows may follow).
  bool printSnapshot(const Gates& g, const char* who, uint8_t piece = 0);
  void stepHk(const Gates& g);  // Hk's next part (hkPart_), at loop()

  statsglue::Glue* glue_ = nullptr;
  tagstore::Fs* fs_ = nullptr;
  Ask* block_ = nullptr;  // the ask's block (its head: Ask, which keeps its bytes)
  statsvisit::Snapshot* snap_ = nullptr;  // the stats pages' snapshot (the Dance tab's block, lent), held
  std::atomic<uint8_t> state_{kIdle};
  Kind kind_ = kNone;
  bool rank_ = false;         // the queued (or running) step is a rank
  uint8_t shelf_ = 0;         // Hl's: 0 Most played, 1 New finds, 2 Old favourites, 3 Not played yet, 4 Recently played
  uint8_t window_ = 2;        // Most played's: 0 7 days, 1 30 days, 2 All time
  uint8_t restarts_ = 0;      // the walk begun again (an index that changed)
  uint8_t ahead_ = 0;         // what the card worker has ahead of an ask (kAheadWalk, kAheadRest: CardTasks' word)
  uint8_t waitLogged_ = 0;    // the gate that held the ask, said once (a bit each)
  // In Asked, the wait's start (the last pass nothing held it: phaseOf()'s
  // waitedMs); queued, the step queued (Gates::nowMs).
  uint32_t queuedMs_ = 0;
  // S3a's (docs/LISTENING-STATS.md 9.7's ".bss": at most 32 B over S2's).
  uint16_t version_ = 0;
  uint16_t rankWaits_ = 0;    // the Rank lane's waits met before the take (a bit a Wait), each said once
  uint16_t holdSumMs_ = 0;    // the visit's holds over its takes (saturating): the lit cap's
  uint8_t newsSeen_ = 0;      // version_'s low byte at the last news()
  uint8_t lastPhase_ = 0;     // phase() at the last loop()
  uint8_t why_ = 0;           // S2's gate bits at the last loop() (an ask in Asked)
  Outcome outcome_ = Outcome::None;  // the last visit's (or lean ask's) end
  uint8_t uiWant_ = 0;        // a UI ask waiting behind a console ask: 1 a visit, 2 Not played yet
  uint8_t flags_ = 0;         // k... above
  uint8_t rankWait_ = 0;      // ScanScheduler::Wait, CardTasks' word (rankWait())
  uint8_t pastCap_ = 0;       // visits whose holds passed kLitCapMs this session (saturating)
  // Hk owed, as the TX buffer's room allows (stepHk()): 0 none; 1-3 the
  // header's pieces; 4-9 a list's rows (from hkRow_); 10 Not played yet's
  // first albums (from hkRow_). (Both in the object's tail padding: no
  // .bss.)
  uint8_t hkPart_ = 0;
  uint8_t hkRow_ = 0;
};
