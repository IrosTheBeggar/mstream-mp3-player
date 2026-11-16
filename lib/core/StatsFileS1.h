// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "StatsFile.h"
#include "StatsFoldS1.h"

// S1 as built (414488c): the listening stats' store over statsfold::s1::Fold
// (the whole aggregate in RAM, written whole at each save), the store the
// S1 rework replaces (docs/LISTENING-STATS.md 5.14). Its R4 made
// statsfile::Store the rework's (a save streams the card's sections and
// merges them with the records); this one is the one before it, moved here
// unchanged, so lib/core StatsGlue and app/ListeningStats keep building
// until R5 moves them onto statsfile::Store. Deleted then. The names, the
// frame, StatsCheck, the read's verdicts (Read, Opened) and SaveAsk are
// statsfile's (StatsFile.h); the save's outcomes, the work memory and the
// Store are this one's.
namespace statsfile {
namespace s1 {

// statsfile's, as this store's callers name them (sx::).
using statsfile::baseFrame;
using statsfile::baseName;
using statsfile::cardFault;
using statsfile::checkWhole;
using statsfile::kBaseRowsAt;
using statsfile::kBaseSpec;
using statsfile::kBufBytes;
using statsfile::kHeaderBytes;
using statsfile::kMagic;
using statsfile::kMajor;
using statsfile::kMinor;
using statsfile::kNameCap;
using statsfile::kStatsBad;
using statsfile::kStatsBin;
using statsfile::kStatsNames;
using statsfile::kStatsSpec;
using statsfile::kStatsTmp;
using statsfile::Opened;
using statsfile::Read;
using statsfile::readName;
using statsfile::SaveAsk;
using statsfile::StatsCheck;
using statsfile::statsOn;

// ---------------------------------------------------------------------------
// The caller's memory: what would be the loop's stack (the writer, the
// save's plan and section table, a frame's Container and a Stream), then
// two 4 KB buffers. Every call keeps its own frame small: a save's
// (a rotation's, the loop's deepest path) is a few hundred bytes.
// ---------------------------------------------------------------------------
struct Scratch {
  cardcontract::ContainerWriter writer;
  statsfold::s1::Fold::Plan plan;
  cardcontract::SectionOut sections[statsfold::kSecs];
  cardcontract::Container frame;  // stats.bin's, a base's, the old base's
  cardcontract::Stream stream;    // the old base's rows (a compaction's)
};
constexpr size_t kScratchBytes = (sizeof(Scratch) + 15) & ~static_cast<size_t>(15);
constexpr size_t kWorkBytes = kScratchBytes + 2 * static_cast<size_t>(kBufBytes);

// ---------------------------------------------------------------------------
// The save (5.5).
// ---------------------------------------------------------------------------
enum class Saved : uint8_t {
  Wrote,     // stats.bin is this RAM's now
  Nothing,   // nothing new since the card's copy: nothing written (true)
  Off,       // stats off this session, a partial scan, or the fold out of memory
  Changed,   // the card's stats.bin isn't the copy RAM stands on: this and every later save refused
  NoMemory,  // no work memory
  Failed,    // a step failed (or the card failed a question): the card's copy as it was
  Held,      // the tmp written whole, but replace() failed: held, promoted first by the next save
};
const char* savedName(Saved s);
constexpr bool savedOk(Saved s) { return s == Saved::Wrote || s == Saved::Nothing; }

struct SaveReport {
  Saved what = Saved::Off;
  const char* step = "";         // where a Failed save stopped ("settle", "card copy", "tmp", ...)
  bool heldPromoted = false;     // a held tmp promoted first
  bool compacted = false;        // a new base written and named
  bool compactFailed = false;    // a compaction was due and failed (the save went on)
  bool compactSkipped = false;   // due, but its name is the one the card's stats.bin names (5.3)
  uint32_t generation = 0;       // the file written
  uint32_t bytes = 0;            // stats.bin's (stats.tmp's)
  uint32_t baseBytes = 0;        // the new base's
  uint32_t baseRows = 0;
  bool oldBaseRemoved = false;
};

// One card's stats files, for a session: what the card's copy is, a held
// tmp, a change seen.
class Store {
public:
  using WaitFn = void (*)(uint32_t ms);
  explicit Store(tagstore::Fs& fs) : fs_(fs) {}

  // The read into `fold` (cleared first; begin()'d by the caller). `work`:
  // kWorkBytes. `wait`: the boot's (tagstore::kBootWaitsMs between reads the
  // card failed, 100 ms before a corrupt file's second read); nullptr: a
  // remount's, one read. A read starts the session again: the card's copy
  // is the one read, and a change seen before is forgotten.
  Opened read(statsfold::s1::Fold& fold, uint8_t* work, WaitFn wait);

  // The card's copy this RAM stands on: none, or stats.bin's generation and
  // header CRC.
  struct Copy {
    bool present = false;
    uint32_t generation = 0;
    uint32_t headerCrc = 0;
    uint32_t baseGen = 0;  // the base it names (0: none), whole or lost: a compaction never writes that name
  };

  // The save of `fold` (5.5). The glue's own guards come first (the card
  // away or another card, the end's flush for the boot's save). Its step 1
  // in two calls: promote() (a tmp held by an earlier save takes stats.bin's
  // place, when the save would go that far), then save() with what it came
  // to: the glue calls them one after the other, so the promotion's frames
  // (replace()'s and a twin's rename's) are never under the save's (the
  // loop's stack: 11). save(fold, ask, work) makes both calls. (One byte
  // between the two: the held copy itself stays the store's.)
  enum class Held : uint8_t {
    None,      // no tmp held, or the save wouldn't come to its step 1
    Promoted,  // it took stats.bin's place
    Gone,      // it was gone: stats.bin is the held copy, or the one before it
    Fault,     // the card failed a question of it: the save fails ("held tmp")
  };
  Held promote(statsfold::s1::Fold& fold);
  SaveReport save(statsfold::s1::Fold& fold, const SaveAsk& ask, uint8_t* work, Held held);
  SaveReport save(statsfold::s1::Fold& fold, const SaveAsk& ask, uint8_t* work) {
    const Held held = promote(fold);
    return save(fold, ask, work, held);
  }
  const Copy& cardCopy() const { return expect_; }
  bool on() const { return on_; }
  bool changed() const { return changed_; }
  bool held() const { return held_.present; }
  // The tmp held (written whole, its replace() failed): its generation and
  // header CRC (present false: none held).
  const Copy& heldCopy() const { return held_; }

private:
  // What a read found: final; a card fault (read again at the boot); a
  // file that failed its own checks (read once more at the boot).
  enum class Again : uint8_t { Final, CardFault, Corrupt };
  Again readStats(statsfold::s1::Fold& fold, Scratch* sc, uint8_t* a, uint8_t* b, Opened* o);
  Again checkBase(const statsfold::Head& h, Scratch* sc, uint8_t* a, Opened* o, bool* lost);
  bool quarantine();
  enum class Promote : uint8_t { Promoted, Gone, NotOurs, Fault };
  Promote promoteHeld();
  enum class Hdr : uint8_t { None, Whole, Short, Fault };  // a file's header: none there, read, too short, the card failed
  Hdr headerOf(const char* path, Copy* out);
  bool compact(statsfold::s1::Fold& fold, Scratch* sc, uint8_t* a, uint8_t* b, SaveReport* r, uint32_t* oldGen);

  tagstore::Fs& fs_;
  Copy expect_;
  Copy held_;          // a tmp written whole whose replace() failed
  bool on_ = false;
  bool changed_ = false;
};

}  // namespace s1
}  // namespace statsfile
