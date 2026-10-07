// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardTags.h"
#include "LibraryIndex.h"

// The device's one builder (docs/METADATA.md 2.9, 3.4.1): library.idx from
// the tag records on the card. Portable, host-tested.
//
// Its inputs are two MPTG files read as streams in the card's canonical
// order (2.6.8): T, the transfer's tags file of the valid root (2.5), and D,
// /.player/tags.bin, the device's own, always compacted before a build. D
// is also the listing (which files exist): it has a row for every audio
// file the last walk saw, with the size and FAT time the walk read, and its
// status (DSTA, 3.3.2: a full record the device read, a row for a file T
// covers, a file waiting for the scan, a file the scan couldn't read). The
// build is a 2-way merge by path with two walkers and no map of paths, so
// its own memory is fixed whatever the card holds (about 40 KB, plus 8
// bytes per T folder for the THUMB set; LibraryIndex's blocks are the
// rest).
//
// Per file, one record, whole (2.9):
//   1. T's record, when its size is the file's and its FAT time matches
//      (equal, or equal after T's skew, 2.3.4) or its qfp was confirmed
//      against the file (D's row says so: the walk checks a doubtful stamp
//      once, N5). A T record flagged UNREADABLE without FROM_API only
//      settles that the file is the software's: rule 2 names it.
//   2. Else D's record, when it is a full record of a parser the builder
//      still accepts.
//   3. Else no record: the file is named from its path, as today, and is
//      marked Pending (the scan should read it), unless D says the scan
//      already failed on it at this size and time.
// A field the chosen record lacks comes from the path, never from the other
// producer (LibraryIndex::addRecord()). Before the first walk after a new
// commit (D's walk identity older than the root's: the caller knows), T's
// paths count as present too (the software listed the card moments ago),
// its records are taken as they are, and D's rows for T's files that the
// new T no longer lists are dropped (no ghost tracks before the walk).
// Paths match by their exact bytes: "Acme/x.mp3" on the card misses T's
// "ACME/x.mp3" until the software's next run takes the new spelling.
//
// T is checked as it streams (its CRCs and 2.4.3's order rules); a failure,
// known at the latest at its end, restarts the build from D alone (3.4.1),
// before anything is shown. A bad D restarts it from T's listing alone (the
// walk and the scan rebuild D). With neither, the caller walks the card
// (today's path build, LibraryIndex::addFile()).
//
// The folders' other files (their covers and counts) come from the caller
// (D's DFLD, N4), asked once per folder as the merge enters it. The album
// folders T flags THUMB give their albums the transfer thumbnail (2.14.3).
class LibraryBuilder {
public:
  using AllocFn = LibraryIndex::AllocFn;
  using FreeFn = LibraryIndex::FreeFn;

  // D's status of one record (3.3.2's DSTA).
  enum class Status : uint8_t {
    Scanned = 0,  // a full record: the device read the file
    Software,     // a row for a file T's record covered at the walk (size and time only)
    Pending,      // waiting for the scan
    Unreadable,   // the scan couldn't read it at this size and time
  };
  struct Row {
    Status status = Status::Scanned;
    bool confirmed = false;  // T's qfp was confirmed against the file at this size (2.9, rule 1)
  };

  // D's statuses, asked once per D record, in record order.
  class DeviceRows {
  public:
    virtual ~DeviceRows() = default;
    virtual Row row(uint32_t record) = 0;
  };

  // A folder's other files (`rel` relative to /music, "" for /music itself):
  // false when the caller knows of none.
  class FolderFactsSource {
  public:
    virtual ~FolderFactsSource() = default;
    virtual bool facts(const char* rel, size_t len, LibraryIndex::FolderFacts* out) = 0;
  };

  // D's records of an older parser than this are read again (2.9, rule 2;
  // the scan's parser, N6, sets it).
  static constexpr uint16_t kMinDeviceParser = 0;
  // docs/METADATA.md 7.1, U8 (open; the proposed default): a T record that
  // matches the file wins over the device's own reading of it, so "Rescan
  // tags" rewrites D's records only, and a retag that kept a software file's
  // size and time stays unseen until the next transfer. false: a full D
  // record wins over a matching T record.
  static constexpr bool kTransferBeatsRescan = true;

  struct Config {
    const char* root = "/music";
    // T: the valid root's MPTG companion (2.5.3), or nullptr.
    cardcontract::Source* transfer = nullptr;
    int32_t skew = 0;            // T's skew (2.3.4) as the last walk found it; 0: none
    bool transferLists = false;  // no walk since T's commit: T's paths count as present
    // D, compacted, or nullptr.
    cardcontract::Source* device = nullptr;
    DeviceRows* rows = nullptr;  // nullptr: every D record full, an UNREADABLE one Unreadable
    uint16_t minParser = kMinDeviceParser;
    // nullptr: no facts; T's own non-audio records then give its folders'
    // images, when T's paths count as present.
    FolderFactsSource* facts = nullptr;
    // The library roots (the root's LIBR, 2.8.6), relative to /music.
    const char* const* libraryRoots = nullptr;
    uint32_t libraryRootCount = 0;
  };

  struct Result {
    bool built = false;      // the index is ready
    bool noRecords = false;  // neither T nor D could be read: the caller walks
    bool noMemory = false;
    bool transferUsed = false;  // T was read whole and used (the inputs' `transfer`)
    bool deviceUsed = false;
    bool restarted = false;     // T or D failed partway: built again without it
    cardcontract::Why transferWhy = cardcontract::Why::Ok;  // why T is absent (Ok: it isn't)
    cardcontract::Why deviceWhy = cardcontract::Why::Ok;
    uint32_t deviceCrc = 0;     // D's headerCrc (the inputs' soft part)
    uint32_t fromTransfer = 0;  // tracks named by T's records
    uint32_t fromDevice = 0;    // by D's
    uint32_t fromPath = 0;      // by their paths
    uint32_t pending = 0;       // of those, the files the scan should read
    uint32_t dropped = 0;       // D's rows for files the new T no longer lists
    uint32_t ignored = 0;       // T's records for files the walk didn't see
    size_t workBytes = 0;       // the builder's own memory at its most
  };

  // Rules 1-3 for one path (2.9; 2.18's builder vectors). `t` is T's
  // record, `d` D's row (the walk's sight of the file), `present` false for
  // none; `row` D's status of it.
  enum class Pick : uint8_t { Transfer, Device, Path };
  struct Seen {
    bool present = false;
    uint32_t size = 0;
    uint32_t fatTime = 0;
    uint16_t flags = 0;  // the record's flags (UNREADABLE, FROM_API)
  };
  struct Choice {
    Pick pick = Pick::Path;
    bool pending = false;
  };
  static Choice choose(const Seen& t, const Seen& d, const Row& row, bool parserOk, int32_t skew,
                       bool transferLists);

  // What LibraryIndex::addRecord() takes of a record: the fields its `known`
  // bits say its producer looked for, from the record and its run.
  static LibraryIndex::TagView viewOf(const cardcontract::mptg::Record& r, const cardcontract::RunFields* run,
                                      uint8_t source);

  // The builder's own memory (its walkers, their run fields and buffers,
  // the THUMB set) comes from these: the firmware's PSRAM, never the card
  // worker's stack. nullptr: malloc/free.
  explicit LibraryBuilder(AllocFn alloc = nullptr, FreeFn release = nullptr);
  // Builds `index` (its begin() to its finish()). `index` is left empty
  // when the result isn't built.
  Result build(LibraryIndex& index, const Config& config);

private:
  struct Work;
  enum class Outcome : uint8_t { Done, TransferBad, DeviceBad, NoMemory };
  Outcome attempt(LibraryIndex& index, const Config& c, Work& w, bool useT, bool useD, bool lists, Result* r);

  AllocFn alloc_;
  FreeFn free_;
};
