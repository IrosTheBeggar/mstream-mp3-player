// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "AutoDjCommand.h"
#include "AutoDjDriver.h"
#include "AutoDjEngine.h"
#include "AutoDjJoin.h"
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
// it (AutoDjCommand.h has the list); M5 adds the UI.
//
//   setup(), after the library and the queue: begin(): the settings (on
//     after a boot if it was: the restored queue waits for Play), the
//     engine's memory, and the file the boot's root names (Library::root()),
//     opened and checked against it (Table::open(), companionMatches(): 1-2
//     small reads). Any failure: no index, picks at random (logged why).
//   every loop pass, after the card's guard and before the card's step
//     (main.cpp's loop()): step(): a remount seen first (sectordisk's
//     counter: the root read again into its own Root; the same index: the
//     file reopened, the maps and the session kept; another: closed,
//     opened, joined again, a new session); then a slice of the join
//     (after the UI's first frame, never while a list moves or the card is
//     held); then the Driver's pass (a pick appended when due; else a slot
//     read ahead); then a long console command's step (Dn, Db: one a pass).
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
// In PSRAM (psramNew: about 3 KB here, the engine's 15 KB, the maps about
// 160 KB at 20,000 tracks, a FIL's 4.1 KB); nothing in internal RAM but
// main.cpp's pointer. Loop task only.
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

  // ---- for the UI (M5) and the console ----
  const autodj::Settings& settings() const { return dj_.settings(); }
  bool setOn(bool on);
  bool setMode(autodj::Mode m);
  bool setFloor(uint8_t pct);
  bool setMatch(bool tempo, bool key);
  // "Auto DJ from here" on `track`; the empty states' button. False:
  // nothing played.
  bool fromHere(uint32_t track);
  bool start();
  // The card has an index this build reads, joined to the library.
  bool usable() const;
  const autodj::Marks& marks() const { return dj_.marks(); }

private:
  enum class File : uint8_t {
    None,     // the root names no index (or there is no root: no transfer data)
    NotRead,  // ... one of a major this build doesn't read (read.mpdj)
    Absent,   // ... one that failed its checks (why_)
    Open,     // open and checked: the join's, the picks'
  };
  enum class Block : uint8_t { None, Retry, Build };
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
  uint32_t joins_ = 0;
  bool joinLogged_ = false;
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
  char path_[TrackCatalog::kMaxPath] = "";
};
