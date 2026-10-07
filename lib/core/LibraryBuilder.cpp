// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryBuilder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

// Each walker's buffers: 8 KB, 2 KB a stream for FOLD, RECS and STRS's two
// runs (3.4.1; the device reads no HIDX nor ledger).
constexpr uint32_t kScratch = 8192;
// The root, '/' and a path within the contract's limit (2.4.3), with room.
constexpr size_t kAbsBytes = 512;

}  // namespace

// The builder's own memory: one block, never on the caller's stack.
struct LibraryBuilder::Work {
  mptg::Walker t, d;
  cc::RunFields tRun, dRun;
  uint8_t tScratch[kScratch];
  uint8_t dScratch[kScratch];
  char abs[kAbsBytes];               // the root + "/" + a path, for the index
  char last[cc::kMaxRelPath + 1];    // the folder the merge last entered (relative)
  size_t lastLen = 0;
  bool entered = false;              // the root's facts asked
  size_t rootLen = 0;
  uint64_t* thumbs = nullptr;        // T's THUMB folders' path hashes
  uint32_t thumbN = 0, thumbCap = 0;
};

LibraryBuilder::LibraryBuilder(AllocFn alloc, FreeFn release)
    : alloc_(alloc ? alloc : defaultAlloc), free_(release ? release : defaultFree) {}

LibraryBuilder::Choice LibraryBuilder::choose(const Seen& t, const Seen& d, const Row& row, bool parserOk,
                                              int32_t skew, bool transferLists) {
  // U8 answered the other way: the device's own full record first.
  if (!kTransferBeatsRescan && d.present && row.status == Status::Scanned && parserOk) return {Pick::Device, false};
  if (t.present) {
    // Rule 1: fresh against the walk's sight of the file, or the listing is
    // T's own (no walk since its commit).
    const bool fresh = transferLists || !d.present ||
                       (t.size == d.size && (cc::timeMatches(t.fatTime, d.fatTime, skew) || row.confirmed));
    // An UNREADABLE record the software couldn't fill from the API settles
    // only that the file is its own: rule 2 names it.
    const bool namesNothing = (t.flags & mptg::kUnreadable) && !(t.flags & mptg::kFromApi);
    if (fresh && !namesNothing) return {Pick::Transfer, false};
  }
  if (d.present) {
    // Rule 2: a full record of an accepted parser.
    if (row.status == Status::Scanned && parserOk) return {Pick::Device, false};
    // The scan failed on it at this size and time: not retried.
    if (row.status == Status::Unreadable) return {Pick::Path, false};
  }
  // Rule 3: the path, and the scan should read it.
  return {Pick::Path, true};
}

LibraryIndex::TagView LibraryBuilder::viewOf(const mptg::Record& r, const cc::RunFields* run, uint8_t source) {
  LibraryIndex::TagView v;
  v.source = source;
  const uint32_t k = r.known;
  auto field = [&](uint32_t bit, uint32_t f, const char** s, size_t* n) {
    if ((k & bit) && run && run->has(f)) {
      *s = run->get(f);
      *n = run->len(f);
    }
  };
  field(mptg::kKnownTitle, cc::kTitle, &v.title, &v.titleLen);
  field(mptg::kKnownArtist, cc::kArtist, &v.artist, &v.artistLen);
  field(mptg::kKnownAlbum, cc::kAlbum, &v.album, &v.albumLen);
  field(mptg::kKnownAlbumArtist, cc::kAlbumArtist, &v.albumArtist, &v.albumArtistLen);
  field(mptg::kKnownSortNames, cc::kArtistSort, &v.artistSort, &v.artistSortLen);
  field(mptg::kKnownSortNames, cc::kAlbumSort, &v.albumSort, &v.albumSortLen);
  field(mptg::kKnownSortNames, cc::kAlbumArtistSort, &v.albumArtistSort, &v.albumArtistSortLen);
  if (k & mptg::kKnownYear) v.year = r.year;
  if (k & mptg::kKnownTrack) v.track = r.track;
  if (k & mptg::kKnownDisc) v.disc = r.disc;
  if (k & mptg::kKnownDuration) v.durationMs = r.durationMs;
  if (k & mptg::kKnownCompilation) v.compilation = mptg::compilationOf(r);
  v.jpeg = (k & mptg::kKnownPicture) && mptg::hasPicture(r) && mptg::picMimeOf(r) == mptg::kMimeJpeg;
  return v;
}

namespace {

// "<root>/<rel>" (`rel` of `len` bytes; "" is the root) into `out`; false
// when it doesn't fit.
bool absPath(const char* root, size_t rootLen, const char* rel, size_t len, char* out, size_t cap) {
  if (rootLen + 1 + len + 1 > cap) return false;
  std::memcpy(out, root, rootLen);
  size_t n = rootLen;
  if (len) {
    out[n++] = '/';
    std::memcpy(out + n, rel, len);
    n += len;
  }
  out[n] = 0;
  return true;
}

}  // namespace

LibraryBuilder::Result LibraryBuilder::build(LibraryIndex& index, const Config& c) {
  Result r;
  index.clear();
  void* mem = alloc_(sizeof(Work));
  if (!mem) {
    r.noMemory = true;
    return r;
  }
  Work* w = new (mem) Work();
  r.workBytes = sizeof(Work);
  bool useT = c.transfer != nullptr, useD = c.device != nullptr, lists = c.transferLists;
  for (;;) {
    const Outcome o = attempt(index, c, *w, useT, useD, lists, &r);
    if (o == Outcome::TransferBad) {
      useT = false;  // 3.4.1: from D alone
      continue;
    }
    if (o == Outcome::DeviceBad) {
      useD = false;  // T's listing alone; the walk and the scan rebuild D
      lists = true;
      continue;
    }
    if (o == Outcome::NoMemory) {
      index.clear();
      r.noMemory = true;
    }
    break;
  }
  w->~Work();
  free_(mem);
  return r;
}

LibraryBuilder::Outcome LibraryBuilder::attempt(LibraryIndex& index, const Config& c, Work& w, bool useT, bool useD,
                                                bool lists, Result* r) {
  r->built = r->transferUsed = r->deviceUsed = false;
  r->fromTransfer = r->fromDevice = r->fromPath = r->pending = r->dropped = r->ignored = 0;
  r->deviceCrc = 0;
  if (useT) {
    r->transferWhy = w.t.begin(*c.transfer, 0, w.tScratch, kScratch, &w.tRun);
    if (r->transferWhy != cc::Why::Ok) return Outcome::TransferBad;
  }
  if (useD) {
    r->deviceWhy = w.d.begin(*c.device, 0, w.dScratch, kScratch, &w.dRun);
    if (r->deviceWhy != cc::Why::Ok) return Outcome::DeviceBad;
  }
  if (!useT && !useD) {
    index.clear();
    r->noRecords = true;
    return Outcome::Done;
  }
  const bool tLists = useT && (lists || !useD);  // T's paths count as present
  const mptg::Info* ti = useT ? &w.t.info() : nullptr;
  const mptg::Info* di = useD ? &w.d.info() : nullptr;
  const bool parserOk = di && di->parserVersion >= c.minParser && di->readRules >= mptg::kReadRules;

  // The blocks from the headers' counts (upper bounds: T's records include
  // its non-audio files). When both list, the files are T's and D's rows
  // that aren't Software rows (those are T's files, or dropped): D's own
  // counts when its statuses give them, else all of D. The strings take
  // 64 KB chunks as they come.
  uint32_t dRecords = di ? di->recordCount : 0, dFolders = di ? di->folderCount : 0;
  if (tLists && di && c.rows) {
    uint32_t ownRecords = 0, ownFolders = 0;
    if (c.rows->ownCounts(&ownRecords, &ownFolders)) {
      dRecords = std::min(dRecords, ownRecords);
      dFolders = std::min(dFolders, ownFolders);
    }
  }
  LibraryIndex::Sizing s;
  s.tracks = dRecords + (tLists ? ti->recordCount : 0);
  s.folders = dFolders + (tLists ? ti->folderCount : 0);
  s.firstChunk = s.tracks * 40 + 1024;
  if (!index.begin(s, c.root, c.libraryRoots, c.libraryRootCount)) return Outcome::NoMemory;
  w.rootLen = std::strlen(c.root);
  while (w.rootLen > 1 && c.root[w.rootLen - 1] == '/') --w.rootLen;
  w.lastLen = 0;
  w.entered = false;

  // T's THUMB folders, one hash each at most.
  w.thumbN = 0;
  w.thumbCap = ti ? ti->folderCount : 0;
  w.thumbs = nullptr;
  if (w.thumbCap) {
    w.thumbs = static_cast<uint64_t*>(alloc_(w.thumbCap * sizeof(uint64_t)));
    if (!w.thumbs) return Outcome::NoMemory;
    if (sizeof(Work) + w.thumbCap * sizeof(uint64_t) > r->workBytes)
      r->workBytes = sizeof(Work) + w.thumbCap * sizeof(uint64_t);
  }
  auto dropThumbs = [&]() {
    if (w.thumbs) free_(w.thumbs);
    w.thumbs = nullptr;
    w.thumbN = w.thumbCap = 0;
  };

  // The next record of a walker (its folders passed, T's THUMBs noted).
  auto next = [&](mptg::Walker& wk, bool isT, bool* has, bool* bad) {
    for (;;) {
      const mptg::Walker::Step st = wk.next();
      if (st == mptg::Walker::Step::Folder) {
        if (isT && (wk.folder().flags & mptg::kFolderThumb) && w.thumbN < w.thumbCap) w.thumbs[w.thumbN++] = wk.pathHash();
        continue;
      }
      *has = st == mptg::Walker::Step::Record;
      *bad = st == mptg::Walker::Step::Bad;
      return;
    }
  };

  // The folders a file's path enters, from the one before (canonical order
  // enters each folder once, its ancestors first): their facts asked.
  auto enterFolders = [&](const char* rel, size_t relLen) -> bool {
    if (!c.facts) return true;
    auto apply = [&](size_t len) -> bool {
      LibraryIndex::FolderFacts f;
      if (!c.facts->facts(rel, len, &f)) return true;
      return absPath(c.root, w.rootLen, rel, len, w.abs, sizeof(w.abs)) && index.setFolderFacts(w.abs, f);
    };
    size_t folderLen = relLen;
    while (folderLen > 0 && rel[folderLen - 1] != '/') --folderLen;
    folderLen = folderLen ? folderLen - 1 : 0;  // the file's folder: rel[0, folderLen)
    if (!w.entered) {
      w.entered = true;
      w.lastLen = 0;
      if (!apply(0)) return false;
    }
    // The longest common prefix in whole names.
    size_t i = 0;
    while (i < w.lastLen && i < folderLen && w.last[i] == rel[i]) ++i;
    size_t common;
    if (i == w.lastLen && (i == folderLen || rel[i] == '/')) {
      common = i;
    } else if (i == folderLen && w.last[i] == '/') {
      common = i;
    } else {
      while (i > 0 && rel[i - 1] != '/') --i;
      common = i ? i - 1 : 0;
    }
    for (size_t b = common ? common + 1 : 0; b <= folderLen; ++b) {
      if (b == folderLen || rel[b] == '/') {
        if (b > 0 && !apply(b)) return false;
      }
    }
    std::memcpy(w.last, rel, folderLen);
    w.lastLen = folderLen;
    return true;
  };

  // One file into the index; false: out of memory.
  auto add = [&](const mptg::Walker& wk, Choice ch, uint8_t source) -> bool {
    if (!enterFolders(wk.path(), wk.pathLength())) return false;
    if (!absPath(c.root, w.rootLen, wk.path(), wk.pathLength(), w.abs, sizeof(w.abs))) return true;
    LibraryIndex::Add a;
    if (ch.pick == Pick::Path) {
      a = index.addFile(w.abs, ch.pending ? LibraryIndex::kAddPending : 0);
      if (a == LibraryIndex::Add::Added) {
        ++r->fromPath;
        if (ch.pending) ++r->pending;
      }
    } else {
      a = index.addRecord(w.abs, viewOf(wk.record(), wk.run(), source));
      if (a == LibraryIndex::Add::Added) ++(source == LibraryIndex::kFromTransfer ? r->fromTransfer : r->fromDevice);
    }
    return a != LibraryIndex::Add::NoMemory;
  };
  auto seen = [](const mptg::Walker& wk) {
    Seen s;
    s.present = true;
    s.size = wk.record().size;
    s.fatTime = wk.record().fatTime;
    s.flags = wk.record().flags;
    return s;
  };
  auto rowOf = [&](const mptg::Walker& wk) {
    if (c.rows) return c.rows->row(wk.recordIndex());
    Row row;
    row.status = (wk.record().flags & mptg::kUnreadable) ? Status::Unreadable : Status::Scanned;
    return row;
  };
  auto audio = [](const mptg::Walker& wk) {
    return LibraryIndex::formatOf(wk.name(), wk.nameLength()) != LibraryIndex::Format::Unknown;
  };

  bool tHas = false, tBad = false, dHas = false, dBad = false;
  if (useT) next(w.t, true, &tHas, &tBad);
  if (useD) next(w.d, false, &dHas, &dBad);
  for (;;) {
    if (tBad) {
      r->transferWhy = w.t.why();
      r->restarted = true;
      dropThumbs();
      return Outcome::TransferBad;
    }
    if (dBad) {
      r->deviceWhy = w.d.why();
      r->restarted = true;
      dropThumbs();
      return Outcome::DeviceBad;
    }
    if (!tHas && !dHas) break;
    const int cmp = !tHas ? 1
                    : !dHas ? -1
                            : cc::compareFilePaths(w.t.path(), w.t.pathLength(), w.d.path(), w.d.pathLength());
    bool ok = true;
    if (cmp < 0) {
      // T alone: a file of T's listing, or one the walk didn't see.
      if (!audio(w.t)) {
        if (tLists && !c.facts) {
          ok = enterFolders(w.t.path(), w.t.pathLength());
          if (ok && absPath(c.root, w.rootLen, w.t.path(), w.t.pathLength(), w.abs, sizeof(w.abs)))
            ok = index.addFile(w.abs, LibraryIndex::kAddOwned) != LibraryIndex::Add::NoMemory;
        }
      } else if (tLists) {
        ok = add(w.t, choose(seen(w.t), Seen{}, Row{}, parserOk, c.skew, true), LibraryIndex::kFromTransfer);
      } else {
        ++r->ignored;
      }
      next(w.t, true, &tHas, &tBad);
    } else if (cmp > 0) {
      // D alone: no T record for it.
      const Row row = rowOf(w.d);
      if (!audio(w.d)) {
        // D holds audio files only; anything else isn't a track.
      } else if (useT && tLists && row.status == Status::Software) {
        ++r->dropped;  // the new commit deleted it or gave it up: the walk re-adds it if it's there
      } else {
        ok = add(w.d, choose(Seen{}, seen(w.d), row, parserOk, c.skew, tLists), LibraryIndex::kFromDevice);
      }
      next(w.d, false, &dHas, &dBad);
    } else {
      const Row row = rowOf(w.d);
      if (audio(w.d)) {
        const Choice ch = choose(seen(w.t), seen(w.d), row, parserOk, c.skew, tLists);
        ok = ch.pick == Pick::Transfer ? add(w.t, ch, LibraryIndex::kFromTransfer)
                                       : add(w.d, ch, LibraryIndex::kFromDevice);
      }
      next(w.t, true, &tHas, &tBad);
      next(w.d, false, &dHas, &dBad);
    }
    if (!ok) {
      dropThumbs();
      return Outcome::NoMemory;
    }
  }

  std::sort(w.thumbs, w.thumbs + w.thumbN);
  index.setThumbFolders(w.thumbs, w.thumbN);
  const bool finished = index.finish();
  dropThumbs();
  if (!finished) return Outcome::NoMemory;
  r->built = true;
  r->transferUsed = useT;
  r->deviceUsed = useD;
  r->deviceCrc = di ? di->frame.headerCrc : 0;
  return Outcome::Done;
}
