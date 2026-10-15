// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "AutoDjCommand.h"
#include "AutoDjDriver.h"
#include "AutoDjEngine.h"
#include "AutoDjJoin.h"
#include "AutoDjView.h"
#include "CardAutoDj.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "PlaybackController.h"
#include "TrackCatalog.h"
#include "app/AutoDjContract.h"  // autoDjReadMpdj()
#include "app/Library.h"
#include "storage/CardFat.h"

// Auto DJ on the device (the engine design's 3-4.2; its M4): the glue
// between the card's AutoDJ index, the library and the player. It owns the
// open index (a raw FatFs file in PSRAM, its mpdj::Table, its own
// cardroot::Root for remounts), the join (autodj::Join: the index's rows
// to this library's tracks, in slices), and the loop step
// (autodj::Driver: the sessions, the trigger, the picks, the DJ marks),
// and keeps the settings in NVS (app/AutoDjStore). The console's D drives
// it (AutoDjCommand.h has the list), and the UI (M5: lib/core/AutoDjView
// has what it shows) through main.cpp's UiHost: view() each pass, the
// settings, the empty states' button (start()), "Auto DJ from here"
// (fromHere()), the Queue's DJ marks (marked()) and the toasts
// (takeNews()).
//
//   setup(), after the library and the queue: begin(): the settings (on
//     after a boot if it was: the restored queue waits for Play), the
//     engine's memory, and the file the boot's root names (Library::root()),
//     opened and checked against it (Table::open(), companionMatches(): 1-2
//     small reads), on or off: usable() says the card has an index (the
//     empty states' button, the Output page). Any failure: no index, picks
//     at random (logged why).
//   every loop pass, after the card's guard and before the card's step
//     (main.cpp's loop()): step(): a remount seen first (sectordisk's
//     counter: the root read again into its own Root; the same index: the
//     file reopened, the maps and the session kept; another: closed,
//     opened, joined again, a new session); then the join's slices while
//     Auto DJ is on (after the UI's first frame, never while a list moves
//     or the card is held: Join::stepPass(), kJoinPassUs and one card read
//     a pass; main.cpp shortens the loop's idle wait meanwhile:
//     joinWorking()); then the Driver's pass (a pick appended when due,
//     waiting while the decoder refills the ring from empty: Env::
//     refilling; else a slot read ahead, or a slot's run made again for
//     new maps); then a long console command's step (Dn, Db: one a pass).
//   the join only while on (2026-10-14): switched on (the "..." menu, the
//     page, D1, "Auto DJ from here", the empty states' button), it joins;
//     switched off, the join is given back (155 KB at 19,410 tracks), so
//     a listener who never uses Auto DJ keeps all of PSRAM. The empty
//     states' button doesn't wait for a join: a random library track whose
//     path's hash the index's DJPH has (Table::rowOf(), a binary search)
//     plays at once, and the join follows. Its blocks take as little of
//     PSRAM's largest free block as they can (Join::setPlacement()): each
//     in a smaller free block that holds it, the rest at that block's top
//     (spacers, never leaving less than kJoinMarginBytes free), so what
//     comes after is carved below them.
//   the update step's memory check (main.cpp's hook, LibraryUpdate::
//     setGiveBack()): a new track table that doesn't fit the largest free
//     block while the join is held next to it: giveBack() releases it there
//     (Block::Update), so the step sees the block Auto DJ off would have
//     left; joined again after the step's Live (or at once when it
//     deferred or failed).
//   the update step's fence (main.cpp): beforeFence() notes the marks'
//     positions while the queue is there; fence() (before the old index
//     goes) releases the join's maps (fenceBytes(): the step's memory
//     check counts them, UpdateEnv::alsoFreed); live() after the read-back
//     carries the marks and joins again (new track ids).
//   owed(): a pick is due but the index joins (UpdateEnv::autoDjOwed: no
//     fence until it lands).
// Auto DJ never starts playback by itself, never changes the output or the
// volume: it appends to the queue, and at a finished queue cues its pick,
// paused (PlaybackController::addFromAutoDj()). The console's Dh and De
// play a track because the listener asked, as qp does.
//
// In PSRAM (psramNew: about 4 KB here, the engine's 19 KB, the maps about
// 160 KB at 20,000 tracks while on, a FIL's 4.1 KB); nothing in internal
// RAM but main.cpp's pointer. Loop task only.
class AutoDj {
public:
  // What main.cpp knows this pass.
  struct Env {
    uint32_t nowMs = 0;
    bool audioPlaying = false;   // the backend: isPlaying()
    bool positionKnown = true;   // ... positionKnown()
    uint32_t trackSeq = 0;       // ... trackSeq()
    uint32_t positionMs = 0;     // ... positionMs()
    bool cardHeld = false;       // the card's guard holds the card (a restart comes)
    bool restartSet = false;     // a restart is set (restartAtMs, a CPU speed's)
    bool sleepEndOfQueue = false;  // the sleep timer's End of queue runs
    bool listMoving = false;     // Ui::listMoving()
    bool uiStarted = true;       // the UI's first frame is up (or there is no UI)
    bool refilling = false;      // the decoder fills the ring from empty (autodj::Env::refilling)
  };

  AutoDj(Library& library, PlaybackController& player);

  void begin();
  void step(const Env& env);
  // The console's D (the text after it).
  void command(const char* arg, const Env& env);

  // ---- main.cpp's hooks ----
  // Auto DJ's pick took the listener's undo since the last call (main.cpp
  // tells the UI: Ui::queueUndoDropped()).
  bool takeUndoDropped();
  // UpdateEnv::autoDjOwed.
  bool owed() const { return dj_.owed(); }
  // What fence() gives back (UpdateEnv::alsoFreed).
  size_t fenceBytes() const { return join_.heldBytes(); }
  void beforeFence();
  size_t fence();
  void live();
  // The update step's memory check (LibraryUpdate::setGiveBack()): the
  // join released now, joined again after the step (its bytes; 0: none
  // held).
  size_t giveBack();
  // The join works (main.cpp: the loop's idle wait short meanwhile).
  bool joinWorking() const { return join_.status() == autodj::Join::Status::Working; }
  // The join's kept bytes at the top of PSRAM's largest free block when it
  // was placed (AutoDjJoin.h): what giving it back adds to that block.
  size_t joinTopBytes() const { return join_.stats().topKept; }

  // ---- for the UI (M5) and the console ----
  const autodj::Settings& settings() const { return dj_.settings(); }
  bool setOn(bool on);
  bool setMode(autodj::Mode m);
  bool setFloor(uint8_t pct);
  bool setMatch(bool tempo, bool key);
  // "Auto DJ from here" on `track`; the empty states' button. False /
  // not Played: nothing played.
  bool fromHere(uint32_t track);
  autodjview::Started start();
  // The card has an index this build reads, open and checked (joined or
  // not: it joins while Auto DJ is on), for a library it can join (not
  // over 65,534 tracks) and not found broken.
  bool usable() const;
  const autodj::Marks& marks() const { return dj_.marks(); }
  // ---- the UI's (M5) ----
  // What the screen shows of it (the UI's snapshot, every pass).
  void view(autodjview::State* v) const;
  // Queue entry `key` is one Auto DJ added (the Queue's DJ mark).
  bool marked(uint32_t key) const { return dj_.marks().has(key); }
  // Library track `track` has neighbours in a usable index (the Library's
  // "Auto DJ from here"): the maps' when joined, else its path's hash
  // looked up in DJPH (a binary search: a few small reads).
  bool hasNeighbours(uint32_t track) { return usable() && hasRow(track); }
  // What the loop's passes (and a switch on) had to say since the last
  // call: autodjview::News bits (main.cpp: Ui::autoDjNews(), a toast).
  uint8_t takeNews();

private:
  enum class File : uint8_t {
    None,     // the root names no index (or there is no root: no transfer data)
    NotRead,  // ... one of a major this build doesn't read (read.mpdj)
    Absent,   // ... one that failed its checks (why_)
    Open,     // open and checked: the join's, the picks'
  };
  // What keeps a join from starting: nothing; a failed read's wait
  // (retryAtMs_); the next library build (no memory, a library too big);
  // the update step it was given back to (giveBack()), until its Live or
  // its end.
  enum class Block : uint8_t { None, Retry, Build, Update };
  enum class Job : uint8_t { None, DryRun, Bench };

  class Reader : public autodj::SlotReader {
  public:
    explicit Reader(const cardcontract::mpdj::Table& t) : t_(t) {}
    cardcontract::mpdj::SlotRead read(uint16_t row, uint8_t* bytes, uint32_t* count,
                                      cardcontract::Why* why) override {
      return t_.slot(row, bytes, nullptr, count, why);
    }

  private:
    const cardcontract::mpdj::Table& t_;
  };

  autodj::Index index();  // (not const: its slot reader reads the file)
  autodj::Env driverEnv(const Env& env) const;
  bool joined() const { return file_ == File::Open && join_.status() == autodj::Join::Status::Ready; }
  // The file the root's entry names, opened and checked.
  void open(const cardroot::Root::AutoDj& entry, const char* when);
  void closeFile();
  void absent(cardcontract::Why why, const char* what);
  void remount(const Env& env);
  void stepJoin(const Env& env);
  void beginJoin(const LibraryIndex* index);
  void joinStatus();
  // Switched off (or the index closing): the join given back.
  void releaseJoin(const char* why);
  // Whether a library track has a row: the maps' when joined, else its
  // path's hash looked up in DJPH (Table::rowOf()).
  bool hasRow(uint32_t track);
  static bool hasRowOf(uint32_t track, void* self) { return static_cast<AutoDj*>(self)->hasRow(track); }
  void stepJob(const Env& env);
  void report(const autodj::Pass& p);
  void logSession(autodj::Began why, uint32_t track);
  void logPick(const autodj::Pass& p, const char* how);
  void status(const Env& env);
  void dryRunSummary(bool stopped);
  void stopDryRun(const char* by);
  void benchSummary(bool stopped);
  void saveSettings(const char* what);
  const char* pathOf(uint32_t track);

  Library& lib_;
  PlaybackController& player_;
  autodj::Driver dj_;
  autodj::Join join_;
  cardcontract::mpdj::Table table_;
  Reader reader_{table_};
  cardfat::FatFile* handle_ = nullptr;   // the open file (FatFs::open's, PSRAM)
  File file_ = File::None;
  cardcontract::Why why_ = cardcontract::Why::Missing;
  const char* whyText_ = "no transfer data on the card";
  cardcontract::msmf::Comp entry_;  // the entry the open file is
  bool haveEntry_ = false;
  char filePath_[48] = "";
  cardroot::Root* root_ = nullptr;   // its own (remounts); PSRAM, made at the first
  uint32_t remountsSeen_ = 0;
  uint32_t remounts_ = 0;
  // The join.
  Block block_ = Block::None;
  uint32_t retryAtMs_ = 0;
  int64_t joinT0Us_ = 0;
  float joinMs_ = 0;
  float beginMs_ = 0;         // the join's begin(): its blocks placed
  uint32_t joins_ = 0;
  uint32_t joinPasses_ = 0;   // the loop passes the join took (stepPass())
  bool joinLogged_ = false;
  char hashPath_[autodj::Join::kPathBytes] = "";  // hasRow()'s path
  // The console's long commands.
  Job job_ = Job::None;
  autodj::DryRun* dry_ = nullptr;   // PSRAM while Dn runs
  uint32_t benchWant_ = 0, benchDone_ = 0, benchFails_ = 0;
  float* benchUs_ = nullptr;        // PSRAM while Db runs
  uint64_t benchCheckUs_ = 0;
  uint32_t benchCheckMax_ = 0;
  uint8_t* benchBuf_ = nullptr;
  bool brokenSaid_ = false;  // the index's broken list logged (once a file)
  // main.cpp's.
  bool undoDropped_ = false;
  // The UI's: its news since takeNews() (autodjview::News), from the
  // passes and the switch (NewsWatch: the toasts' rule, host-tested; it
  // counts Auto DJ's appends too: no Queue badge flash for them).
  autodjview::NewsWatch newsWatch_;
  uint8_t news_ = 0;
  char path_[TrackCatalog::kMaxPath] = "";
};
