// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardWalk.h"

#include <algorithm>
#include <cstring>

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

namespace cardwalk {

// ---------------------------------------------------------------------------
// The digest, the facts, the qfp
// ---------------------------------------------------------------------------
void FolderDigest::add(const char* name, size_t len, uint32_t size, uint32_t fatTime) {
  static const uint8_t kNul = 0;
  uint8_t st[8];
  cc::put32(st, size);
  cc::put32(st + 4, fatTime);
  h_ = cc::fnv1a64(name, len, h_);
  h_ = cc::fnv1a64(&kNul, 1, h_);
  h_ = cc::fnv1a64(st, sizeof(st), h_);
}

uint64_t FolderDigest::value() const {
  uint8_t n[4];
  cc::put32(n, others_);
  return cc::fnv1a64(n, sizeof(n), h_);
}

LibraryIndex::FolderFacts factsOf(const FolderRow& row) {
  LibraryIndex::FolderFacts f;
  f.image = row.imageLength ? row.image : nullptr;
  f.imageCount = static_cast<uint8_t>(row.images < 0xFF ? row.images : 0xFF);
  const uint32_t others = row.images + row.others;
  f.otherCount = static_cast<uint16_t>(others < 0xFFFF ? others : 0xFFFF);
  f.imageOwned = row.imageLength && row.imageOwned;
  return f;
}

bool fileQfp(cc::Source& src, uint32_t size, uint8_t* buf, uint32_t bufBytes, uint64_t* out) {
  if (!buf || bufBytes == 0 || src.size() != size) return false;
  const cc::QfpRanges r = cc::qfpRanges(size);
  uint8_t sz[8];
  cc::put64(sz, size);
  uint64_t h = cc::fnv1a64(sz, sizeof(sz));
  auto range = [&](uint32_t offset, uint32_t n) {
    while (n) {
      const uint32_t k = n < bufBytes ? n : bufBytes;
      if (!src.read(offset, buf, k)) return false;
      h = cc::fnv1a64(buf, k, h);
      offset += k;
      n -= k;
    }
    return true;
  };
  if (!range(0, r.headBytes) || !range(r.tailOffset, r.tailBytes)) return false;
  *out = h;
  return true;
}

// ---------------------------------------------------------------------------
// T, streamed and indexed
// ---------------------------------------------------------------------------
namespace {

void fillRecord(const mptg::Record& r, TransferRecord* out) {
  out->size = r.size;
  out->fatTime = r.fatTime;
  out->qfp = r.qfp;
  out->flags = r.flags;
  out->container = r.container;
}

}  // namespace

cc::Why StreamedTransfer::begin(cc::Source& src, uint8_t* scratch, uint32_t scratchBytes) {
  const cc::Why w = walker_.begin(src, 0, scratch, scratchBytes, nullptr);
  begun_ = true;
  have_ = false;
  done_ = w != cc::Why::Ok;
  return w;
}

bool StreamedTransfer::advance() {
  have_ = false;
  while (!done_) {
    switch (walker_.next()) {
      case mptg::Walker::Step::Record:
        have_ = true;
        return true;
      case mptg::Walker::Step::Folder:
        break;
      case mptg::Walker::Step::End:
      case mptg::Walker::Step::Bad:
        done_ = true;
        break;
    }
  }
  return false;
}

bool StreamedTransfer::find(const char* rel, size_t len, TransferRecord* out) {
  if (!begun_) return false;
  for (;;) {
    if (!have_ && !advance()) return false;
    const int c = cc::compareFilePaths(walker_.path(), walker_.pathLength(), rel, len);
    if (c > 0) return false;  // T has none: the next one comes later
    have_ = false;            // passed either way
    if (c == 0) {
      fillRecord(walker_.record(), out);
      return true;
    }
  }
}

bool StreamedTransfer::finish() {
  if (!begun_) return false;
  while (advance()) {
  }
  return walker_.why() == cc::Why::Ok;
}

cc::Why IndexedTransfer::begin(cc::Source& src) {
  const cc::Why w = file_.open(src);
  ok_ = w == cc::Why::Ok;
  return w;
}

bool IndexedTransfer::find(const char* rel, size_t len, TransferRecord* out) {
  if (!ok_) return false;
  const uint32_t i = file_.find(rel, len);
  if (i == mptg::File::kNotFound) return false;
  mptg::Record r;
  if (!file_.record(i, &r)) {
    ok_ = false;  // a read failed: finish() says so
    return false;
  }
  fillRecord(r, out);
  return true;
}

// ---------------------------------------------------------------------------
// The scratch: a pass's entries
// ---------------------------------------------------------------------------
// An entry at `at`: size u32, fatTime u32, its name's length u16, a folder
// byte, its kind, then the name and a NUL, the whole aligned to 4. The pass
// keeps the entries' offsets at [lo, lo + 4n) as a max-heap by key (files
// before folders, then the names' bytes) while it lists, and sorted up once
// it's done; the entries grow down from hi.
namespace {

constexpr uint8_t kAudio = 0, kImage = 1, kOther = 2, kFolder = 3;

uint32_t entryBytes(size_t nameLength) { return static_cast<uint32_t>((12 + nameLength + 1 + 3) & ~size_t{3}); }

uint32_t reserveFor(uint32_t depth) { return (kMaxDepth - depth) * CardWalk::kEntryMax; }

}  // namespace

uint16_t CardWalk::nameLengthAt(uint32_t at) const {
  uint16_t n;
  std::memcpy(&n, base_ + at + 8, sizeof(n));
  return n;
}

int CardWalk::compareAt(uint32_t a, uint32_t b) const {
  const bool fa = base_[a + 10] != 0, fb = base_[b + 10] != 0;
  if (fa != fb) return fa ? 1 : -1;
  return cc::compareNames(nameAt(a), nameLengthAt(a), nameAt(b), nameLengthAt(b));
}

int CardWalk::compareEntryAt(const Entry& e, uint32_t at) const {
  const bool fa = base_[at + 10] != 0;
  if (e.folder != fa) return e.folder ? 1 : -1;
  return cc::compareNames(e.name, e.nameLength, nameAt(at), nameLengthAt(at));
}

int CardWalk::compareEntry(const Entry& e, const Key& k) const {
  if (e.folder != k.folder) return e.folder ? 1 : -1;
  return cc::compareNames(e.name, e.nameLength, k.name, k.length);
}

void CardWalk::keyOf(uint32_t at, Key* out) const {
  out->folder = base_[at + 10] != 0;
  out->length = nameLengthAt(at);
  std::memcpy(out->name, nameAt(at), out->length);
  out->name[out->length] = 0;
}

void CardWalk::place(const Entry& e, uint8_t kind) {
  const uint32_t bytes = entryBytes(e.nameLength);
  bottom_ -= bytes;
  uint8_t* p = base_ + bottom_;
  const uint16_t len = static_cast<uint16_t>(e.nameLength);
  std::memcpy(p, &e.size, 4);
  std::memcpy(p + 4, &e.fatTime, 4);
  std::memcpy(p + 8, &len, 2);
  p[10] = e.folder ? 1 : 0;
  p[11] = kind;
  std::memcpy(p + 12, e.name, len);
  p[12 + len] = 0;
  uint32_t* h = offsets(lo_);
  h[n_++] = bottom_;
  live_ += bytes;
  std::push_heap(h, h + n_, [this](uint32_t a, uint32_t b) { return compareAt(a, b) < 0; });
}

void CardWalk::evictMax() {
  uint32_t* h = offsets(lo_);
  std::pop_heap(h, h + n_, [this](uint32_t a, uint32_t b) { return compareAt(a, b) < 0; });
  const uint32_t at = h[--n_];
  keyOf(at, &ceiling_);  // every key kept is below it, every key dropped at or above it
  truncated_ = true;
  live_ -= entryBytes(nameLengthAt(at));
}

// Slides the entries the offsets name up against hi, dropping the gaps the
// evicted ones left. Highest first, so none is written over before it moves.
void CardWalk::compact() {
  uint32_t* h = offsets(lo_);
  std::sort(h, h + n_, [](uint32_t a, uint32_t b) { return a > b; });
  uint32_t top = hi_;
  for (uint32_t i = 0; i < n_; ++i) {
    const uint32_t bytes = entryBytes(nameLengthAt(h[i]));
    top -= bytes;
    if (top != h[i]) std::memmove(base_ + top, base_ + h[i], bytes);
    h[i] = top;
  }
  bottom_ = top;
}

uint32_t CardWalk::firstFolder() const {
  const uint32_t* h = reinterpret_cast<const uint32_t*>(base_ + lo_);
  uint32_t i = 0;
  while (i < n_ && base_[h[i] + 10] == 0) ++i;
  return i;
}

bool CardWalk::filesComplete() const { return !truncated_ || ceiling_.folder; }

// One pass over the folder at the top of the stack: the smallest keys after
// `after` (all of them when they fit), sorted up at [lo_, lo_ + 4n_). The
// pass may hold at most cap_ bytes, entries and offsets together: what is
// left of the frame's part is the levels below it's (reserveFor()), so each
// of them can list at least one entry, whatever this one holds.
bool CardWalk::fill(Frame& f, const Key* after) {
  lo_ = f.lo;
  hi_ = f.hi;
  cap_ = (hi_ - lo_) - reserveFor(f.depth);
  n_ = 0;
  bottom_ = hi_;
  live_ = 0;
  truncated_ = false;
  ++r_.listings;
  path_[f.pathLength] = 0;
  const Lister::Open open = c_.lister->openDir(path_, f.pathLength);
  if (open == Lister::Open::Missing && f.depth == 0) return true;  // no /music: no library
  if (open != Lister::Open::Ok) {
    fail(Error::Card);
    return false;
  }
  const size_t prefix = f.pathLength ? f.pathLength + 1u : 0u;
  auto less = [this](uint32_t a, uint32_t b) { return compareAt(a, b) < 0; };
  for (;;) {
    Entry e;
    const Lister::Next got = c_.lister->next(&e);
    if (got == Lister::Next::End) break;
    if (got == Lister::Next::Error) {
      c_.lister->closeDir();
      fail(Error::Card);
      return false;
    }
    // What the device sees (2.8.2).
    if (!e.name || e.nameLength == 0 || e.name[0] == '.') continue;
    if (prefix + e.nameLength > cc::kMaxRelPath) continue;
    if (std::memchr(e.name, '/', e.nameLength)) continue;
    if (e.folder && f.depth >= kMaxDepth) continue;
    // This pass's range.
    if (after && compareEntry(e, *after) <= 0) continue;
    if (truncated_ && compareEntry(e, ceiling_) >= 0) continue;
    uint8_t kind = kFolder;
    if (!e.folder) {
      if (LibraryIndex::formatOf(e.name, e.nameLength) != LibraryIndex::Format::Unknown)
        kind = kAudio;
      else if (LibraryIndex::imageRank(e.name, e.nameLength) != LibraryIndex::kNoImage)
        kind = kImage;
      else
        kind = kOther;
    }
    const uint32_t need = entryBytes(e.nameLength) + 4;
    const bool over = live_ + 4 * n_ + need > cap_;
    const bool room = bottom_ - (lo_ + 4 * n_) >= need;
    if (over || !room) {
      // Make room by dropping the largest keys above this one. When the
      // gaps must be closed anyway, down to 3/4 of the budget, so a long
      // pass compacts rarely.
      uint32_t target = cap_ - need;
      if (!room && cap_ / 4 * 3 < target) target = cap_ / 4 * 3;
      while (n_ > 0 && live_ + 4 * n_ > target && compareEntryAt(e, offsets(lo_)[0]) < 0) evictMax();
      if (live_ + 4 * n_ + need > cap_) {
        // Above every key kept: this pass ends below it.
        ceiling_.folder = e.folder;
        ceiling_.length = static_cast<uint16_t>(e.nameLength);
        std::memcpy(ceiling_.name, e.name, e.nameLength);
        ceiling_.name[e.nameLength] = 0;
        truncated_ = true;
        continue;
      }
      if (bottom_ - (lo_ + 4 * n_) < need) {
        compact();
        std::make_heap(offsets(lo_), offsets(lo_) + n_, less);
      }
    }
    place(e, kind);
  }
  c_.lister->closeDir();
  std::sort_heap(offsets(lo_), offsets(lo_) + n_, less);
  return true;
}

// The batch's subfolders become the frame's (the files are done): their
// offsets at lo, their entries against hi, at most half the pass's budget
// (at least one), so the levels below have room. The child's part of the
// scratch is what lies between.
void CardWalk::keepDirs(Frame& f) {
  uint32_t* h = offsets(lo_);
  const uint32_t first = firstFolder();
  uint32_t m = n_ - first;
  if (m == 0 && n_ > 0) keyOf(h[n_ - 1], &after_);  // the next pass starts after the last file
  std::memmove(h, h + first, m * sizeof(uint32_t));
  uint32_t used = 0, keep = 0;
  for (; keep < m; ++keep) {
    const uint32_t bytes = entryBytes(nameLengthAt(h[keep])) + 4;
    if (keep > 0 && used + bytes > cap_ / 2) break;
    used += bytes;
  }
  const bool dropped = keep < m;
  n_ = keep;
  compact();
  std::sort(h, h + n_, [this](uint32_t a, uint32_t b) { return compareAt(a, b) < 0; });
  f.dirs = n_;
  f.nextDir = 0;
  f.dirBottom = bottom_;
  f.moreDirs = truncated_ || dropped;
  f.phase = Phase::Dirs;
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------
bool CardWalk::begin(const Config& config) {
  c_ = config;
  r_ = Result{};
  skew_.clear();
  depth_ = 0;
  dStarted_ = dHave_ = dUsed_ = dFileHave_ = false;
  settleBegun_ = false;
  settle_ = SettlePhase::Recount;
  uintptr_t p = reinterpret_cast<uintptr_t>(config.scratch);
  const uintptr_t aligned = (p + 3) & ~uintptr_t{3};
  const uint32_t lost = static_cast<uint32_t>(aligned - p);
  if (!c_.lister || !c_.sink || !config.scratch || config.scratchBytes < lost ||
      ((config.scratchBytes - lost) & ~3u) < kMinScratch) {
    r_.state = State::Failed;
    r_.error = Error::Config;
    return false;
  }
  base_ = reinterpret_cast<uint8_t*>(aligned);
  bytes_ = (config.scratchBytes - lost) & ~3u;
  r_.summary.firstAfterCommit = c_.firstAfterCommit;
  r_.summary.skew = c_.firstAfterCommit ? 0 : c_.skew;
  r_.summary.changed = c_.firstAfterCommit;  // the header's commit changes
  r_.state = State::Walking;
  Frame& root = frames_[0];
  root = Frame{};
  root.lo = 0;
  root.hi = bytes_;
  path_[0] = 0;
  depth_ = 1;
  return true;
}

CardWalk::State CardWalk::step() {
  if (r_.state != State::Walking && r_.state != State::Settling) return r_.state;
  ++r_.steps;
  if (r_.state == State::Settling) {
    stepSettle();
    return r_.state;
  }
  // A step is one listing: entering a subfolder and leaving a finished one
  // read nothing, so they go on into the next listing (2026-10-09: three
  // steps a folder, two of them no I/O, made a 20k card's walk 7,774 steps,
  // each one a hand-off). At most one folder is listed a step, as before;
  // the walk's end (D's folders left, T's end) is a step of its own.
  const uint32_t listings = r_.listings;
  do {
    stepFolder();
  } while (r_.state == State::Walking && r_.listings == listings);
  return r_.state;
}

const char* CardWalk::errorName(Error e) {
  switch (e) {
    case Error::None: return "none";
    case Error::Config: return "its setup";
    case Error::Card: return "a listing: the card?";
    case Error::Known: return "reading tags.bin";
    case Error::Sink: return "the journal's write";
    case Error::Transfer: return "the transfer's tags file";
  }
  return "?";
}

CardWalk::State CardWalk::run() {
  while (step() == State::Walking || r_.state == State::Settling) {
  }
  return r_.state;
}

void CardWalk::fail(Error e) {
  if (r_.state == State::Failed) return;
  r_.error = e;
  r_.state = State::Failed;
  if (c_.sink) c_.sink->abort();
}

size_t CardWalk::filePath(const Frame& f, const char* name, size_t len) {
  size_t n = f.pathLength;
  if (n) path_[n++] = '/';
  std::memcpy(path_ + n, name, len);
  n += len;
  path_[n] = 0;
  return n;
}

void CardWalk::stepFolder() {
  if (depth_ == 0) {
    finishWalk();
    return;
  }
  Frame& f = frames_[depth_ - 1];
  path_[f.pathLength] = 0;
  switch (f.phase) {
    case Phase::Begin: {
      ++r_.folders;
      if (!syncKnown(f) || !fill(f, nullptr)) return;
      if (filesComplete()) {
        const uint32_t end = firstFolder();
        digestFiles(f, end);
        decideMerge(f);
        if (f.merge && (!beginRows(f) || !mergeFiles(f, end))) return;
        if (!endFiles(f)) return;
        keepDirs(f);
      } else if (c_.firstAfterCommit || !f.known) {
        // A folder bigger than the scratch, merged whatever its digest: the
        // digest and the merge share the passes.
        f.fused = true;
        f.merge = true;
        ++r_.foldersMerged;
        if (!beginRows(f) || !mergeFiles(f, n_)) return;
        keyOf(offsets(lo_)[n_ - 1], &after_);
        f.phase = Phase::Merge;
      } else {
        // A folder bigger than the scratch: its digest first, in passes, then
        // its files again only if it changed.
        digestFiles(f, n_);
        keyOf(offsets(lo_)[n_ - 1], &after_);
        f.phase = Phase::Digest;
      }
      return;
    }
    case Phase::Digest: {
      if (!fill(f, &after_)) return;
      if (!filesComplete()) {
        digestFiles(f, n_);
        keyOf(offsets(lo_)[n_ - 1], &after_);
        return;
      }
      const uint32_t end = firstFolder();
      digestFiles(f, end);
      decideMerge(f);
      if (!f.merge) {
        if (endFiles(f)) keepDirs(f);
        return;
      }
      // Changed: its files again, from the first, against D (and T).
      f.phase = Phase::Merge;
      f.mergeFromStart = true;
      return;
    }
    case Phase::Merge: {
      const bool fromStart = f.mergeFromStart;
      f.mergeFromStart = false;
      if (!fill(f, fromStart ? nullptr : &after_)) return;
      if (fromStart && !beginRows(f)) return;
      const bool last = filesComplete();
      if (!mergeFiles(f, last ? firstFolder() : n_)) return;
      if (!last) {
        keyOf(offsets(lo_)[n_ - 1], &after_);
        return;
      }
      if (endFiles(f)) keepDirs(f);
      return;
    }
    case Phase::Dirs: {
      if (f.nextDir < f.dirs) {
        enterChild(f);
        return;
      }
      if (f.moreDirs) {
        if (f.dirs > 0) keyOf(offsets(f.lo)[f.dirs - 1], &after_);  // its last subfolder
        if (!fill(f, &after_)) return;
        keepDirs(f);
        if (f.dirs > 0) return;
      }
      // Done with it, and its subfolders.
      --depth_;
      if (depth_ > 0) path_[frames_[depth_ - 1].pathLength] = 0;
      return;
    }
  }
}

void CardWalk::enterChild(Frame& parent) {
  const uint32_t at = offsets(parent.lo)[parent.nextDir++];
  const uint16_t len = nameLengthAt(at);
  Frame& child = frames_[depth_];
  child = Frame{};
  child.depth = static_cast<uint8_t>(parent.depth + 1);
  child.lo = parent.lo + 4 * parent.dirs;
  child.hi = parent.dirBottom;
  size_t n = parent.pathLength;
  if (n) path_[n++] = '/';
  std::memcpy(path_ + n, nameAt(at), len);
  n += len;
  path_[n] = 0;
  child.pathLength = static_cast<uint16_t>(n);
  if (child.hi < child.lo || child.hi - child.lo < (kMaxDepth + 1 - child.depth) * kEntryMax) {
    fail(Error::Config);  // can't happen: keepDirs() leaves the levels below their room
    return;
  }
  ++depth_;
}

void CardWalk::decideMerge(Frame& f) {
  f.merge = c_.firstAfterCommit || !f.known || f.digest.value() != f.knownDigest;
  if (f.merge) ++r_.foldersMerged;
  if (!f.merge) f.imageOwned = f.knownOwned;
}

// The batch's files [0, end), counted and digested; the best cover kept.
void CardWalk::digestFiles(Frame& f, uint32_t end) {
  for (uint32_t i = 0; i < end; ++i) digestOne(f, offsets(lo_)[i]);
}

void CardWalk::digestOne(Frame& f, uint32_t at) {
  const uint8_t kind = base_[at + 11];
  const char* name = nameAt(at);
  const uint16_t len = nameLengthAt(at);
  uint32_t size, time;
  std::memcpy(&size, base_ + at, 4);
  std::memcpy(&time, base_ + at + 4, 4);
  if (kind == kOther) {
    f.digest.other();
    ++f.others;
    ++r_.others;
    return;
  }
  f.digest.add(name, len, size, time);
  if (kind == kAudio) {
    ++f.audio;
    ++r_.audio;
    return;
  }
  ++f.images;
  ++r_.images;
  const uint8_t rank = LibraryIndex::imageRank(name, len);
  if (rank < f.imageRank) {  // ties: the first by name, as LibraryIndex elects in this order
    f.imageRank = rank;
    f.imageLength = len;
    std::memcpy(f.image, name, len);
    f.image[len] = 0;
    f.imageSize = size;
    f.imageTime = time;
  }
}

// ---- D ----
void CardWalk::advanceKnown() {
  dFileHave_ = false;
  KnownFolder k;
  dHave_ = c_.known->nextFolder(&k);
  if (c_.known->failed()) {
    dHave_ = false;
    fail(Error::Known);
    return;
  }
  if (!dHave_) return;
  if (k.pathLength > cc::kMaxRelPath) {
    dHave_ = false;
    fail(Error::Known);
    return;
  }
  std::memcpy(dPath_, k.path, k.pathLength);
  dPath_[k.pathLength] = 0;
  dLength_ = k.pathLength;
  dDigest_ = k.digest;
  dOwned_ = k.imageOwned;
}

bool CardWalk::nextKnownFile() {
  KnownFile k;
  dFileHave_ = c_.known->nextFile(&k);
  if (c_.known->failed()) {
    dFileHave_ = false;
    fail(Error::Known);
    return false;
  }
  if (!dFileHave_) return true;
  if (k.nameLength == 0 || k.nameLength > cc::kMaxRelPath) {
    dFileHave_ = false;
    fail(Error::Known);
    return false;
  }
  std::memcpy(dName_, k.name, k.nameLength);
  dName_[k.nameLength] = 0;
  dFile_ = k;
  dFile_.name = dName_;
  return true;
}

// D's folders before this one in pre-order are gone; this one is D's or new.
bool CardWalk::syncKnown(Frame& f) {
  if (!c_.known) return true;
  if (!dStarted_) {
    dStarted_ = true;
    advanceKnown();
    if (r_.state == State::Failed) return false;
  } else if (dUsed_) {
    dUsed_ = false;
    advanceKnown();
    if (r_.state == State::Failed) return false;
  }
  while (dHave_) {
    const int c = cc::compareFolderPaths(dPath_, dLength_, path_, f.pathLength);
    if (c > 0) break;  // D doesn't list this one
    if (c == 0) {
      f.known = true;
      f.knownDigest = dDigest_;
      f.knownOwned = dOwned_;
      dUsed_ = true;
      break;
    }
    if (!goneFolder()) return false;
    advanceKnown();
    if (r_.state == State::Failed) return false;
  }
  path_[f.pathLength] = 0;
  return true;
}

// The folder D gave last isn't on the card: it and its rows are gone.
bool CardWalk::goneFolder() {
  if (!c_.sink->folderGone(dPath_, dLength_)) {
    fail(Error::Sink);
    return false;
  }
  r_.summary.changed = true;
  ++r_.foldersGone;
  for (;;) {
    if (!nextKnownFile()) return false;
    if (!dFileHave_) return true;
    size_t n = dLength_;
    if (n + 1 + dFile_.nameLength > cc::kMaxRelPath) {
      fail(Error::Known);
      return false;
    }
    std::memcpy(other_, dPath_, n);
    if (n) other_[n++] = '/';
    std::memcpy(other_ + n, dName_, dFile_.nameLength);
    n += dFile_.nameLength;
    other_[n] = 0;
    if (!c_.sink->fileGone(other_, n)) {
      fail(Error::Sink);
      return false;
    }
    ++r_.gone;
  }
}

bool CardWalk::beginRows(Frame& f) {
  dFileHave_ = false;
  if (!f.known || !c_.known) return true;
  return nextKnownFile();
}

// D's row `dName_` of the current folder isn't on the card.
bool CardWalk::goneRow(const Frame& f) {
  size_t n = f.pathLength;
  if (n + 1 + dFile_.nameLength > cc::kMaxRelPath) {
    fail(Error::Known);
    return false;
  }
  std::memcpy(other_, path_, n);
  if (n) other_[n++] = '/';
  std::memcpy(other_ + n, dName_, dFile_.nameLength);
  n += dFile_.nameLength;
  other_[n] = 0;
  if (!c_.sink->fileGone(other_, n)) {
    fail(Error::Sink);
    return false;
  }
  r_.summary.changed = true;
  ++r_.gone;
  return nextKnownFile();
}

// ---- the merge ----
bool CardWalk::emitFile(const char* rel, size_t len, Change change, const FileRow& row) {
  if (!c_.sink->file(rel, len, change, row)) {
    fail(Error::Sink);
    return false;
  }
  r_.summary.changed = true;
  if (change == Change::Added) ++r_.added;
  if (change == Change::Changed) ++r_.changed;
  return true;
}

bool CardWalk::emitDoubt(const char* rel, size_t len, const Doubt& d) {
  if (!c_.sink->doubt(rel, len, d)) {
    fail(Error::Sink);
    return false;
  }
  r_.summary.changed = true;
  if (!d.settle) ++r_.countOnly;
  return true;
}

// The batch's files [0, end) against D's rows (and T). Fused, each is
// digested first: a cover that is the best so far is then the one mergeOne()
// looks up, and a better one later looks itself up in its turn.
bool CardWalk::mergeFiles(Frame& f, uint32_t end) {
  for (uint32_t i = 0; i < end; ++i) {
    const uint32_t at = offsets(lo_)[i];
    if (f.fused) digestOne(f, at);
    if (base_[at + 11] == kAudio) {
      // D's rows before this name are gone.
      while (dFileHave_ && cc::compareNames(dName_, dFile_.nameLength, nameAt(at), nameLengthAt(at)) < 0)
        if (!goneRow(f)) return false;
      const bool same =
          dFileHave_ && cc::compareNames(dName_, dFile_.nameLength, nameAt(at), nameLengthAt(at)) == 0;
      if (!mergeOne(f, at, same ? &dFile_ : nullptr)) return false;
      if (same && !nextKnownFile()) return false;
    } else if (!mergeOne(f, at, nullptr)) {
      return false;
    }
  }
  path_[f.pathLength] = 0;
  return true;
}

// One file of a merged folder: its row against D's (`d`), and T's record.
bool CardWalk::mergeOne(Frame& f, uint32_t at, const KnownFile* d) {
  const uint8_t kind = base_[at + 11];
  const char* name = nameAt(at);
  const uint16_t nameLen = nameLengthAt(at);
  uint32_t size, time;
  std::memcpy(&size, base_ + at, 4);
  std::memcpy(&time, base_ + at + 4, 4);
  const bool first = c_.firstAfterCommit;
  const bool best = kind == kImage && f.imageLength == nameLen && std::memcmp(f.image, name, nameLen) == 0;
  const bool unchanged = d && d->row.size == size && d->row.fatTime == time;
  if (kind == kAudio && unchanged && !first) return true;  // its row stands: T is the same commit
  const size_t len = filePath(f, name, nameLen);
  // T's record: every file's on the first walk after a commit (the skew is
  // over every pair), else a changed file's and the best cover's.
  bool ask = false;
  if (c_.transfer) ask = first || kind == kAudio || best;
  TransferRecord t;
  const bool haveT = ask && c_.transfer->find(path_, len, &t);
  const bool sizeMatch = haveT && t.size == size;
  bool hasDelta = false;
  int64_t delta = 0;
  if (sizeMatch) {
    const int64_t wt = cc::fatWallSeconds(t.fatTime), wo = cc::fatWallSeconds(time);
    hasDelta = wt >= 0 && wo >= 0;
    delta = hasDelta ? wo - wt : 0;
    if (first) skew_.add(t.fatTime, time);
  }
  if (best) f.imageOwned = sizeMatch;
  Doubt count;  // a pair the skew counts, for its recount
  count.settle = false;
  count.hasDelta = hasDelta;
  count.delta = delta;
  count.size = size;
  count.fatTime = time;
  const bool countIt = first && hasDelta && delta != 0;
  if (kind != kAudio) return !countIt || emitDoubt(path_, len, count);

  FileRow row;
  row.size = size;
  row.fatTime = time;
  row.qfp = unchanged ? d->row.qfp : 0;
  row.status = unchanged && (d->row.status == Status::Scanned || d->row.status == Status::Unreadable)
                   ? d->row.status
                   : Status::Pending;
  const bool namesNothing = haveT && (t.flags & mptg::kUnreadable) && !(t.flags & mptg::kFromApi);
  if (sizeMatch && !namesNothing) {
    if (cc::timeMatches(t.fatTime, time, first ? 0 : c_.skew)) {
      row.status = Status::Software;
    } else {
      Doubt dd;
      dd.settle = true;
      dd.hasDelta = hasDelta;
      dd.delta = delta;
      dd.size = size;
      dd.fatTime = time;
      dd.transferQfp = t.qfp;
      dd.deviceQfp = row.qfp;
      dd.fallback = row.status;
      ++r_.doubtful;
      return emitFile(path_, len, Change::Doubtful, row) && emitDoubt(path_, len, dd);
    }
  } else if (countIt && !emitDoubt(path_, len, count)) {
    return false;
  }
  if (!d) return emitFile(path_, len, Change::Added, row);
  if (row != d->row) return emitFile(path_, len, Change::Changed, row);
  return true;
}

// The folder's files are done: D's rows left are gone; its row, if it changed.
bool CardWalk::endFiles(Frame& f) {
  if (f.merge) {
    while (dFileHave_)
      if (!goneRow(f)) return false;
  }
  path_[f.pathLength] = 0;
  bool give;
  if (f.known)
    give = f.digest.value() != f.knownDigest || (f.merge && f.imageOwned != f.knownOwned);
  else
    give = f.audio > 0;  // D lists the folders with audio at or below them
  if (!give) {
    f.rowPending = !f.known;
    return true;
  }
  return giveRow(depth_ - 1);
}

// Gives the row of frame `level`, after its ancestors' that wait (a folder
// D doesn't list gets one once audio turns up below it).
bool CardWalk::giveRow(uint32_t level) {
  for (uint32_t i = 0; i <= level; ++i) {
    Frame& g = frames_[i];
    if (i < level && (!g.rowPending || g.rowGiven)) continue;
    FolderRow row;
    row.digest = g.digest.value();
    row.image = g.image;
    row.imageLength = g.imageLength;
    if (!g.imageLength) row.image = "";
    row.imageRank = g.imageRank;
    row.imageSize = g.imageSize;
    row.imageTime = g.imageTime;
    row.imageOwned = g.imageLength && g.imageOwned;
    row.audio = g.audio;
    row.images = g.images;
    row.others = g.others;
    std::memcpy(other_, path_, g.pathLength);
    other_[g.pathLength] = 0;
    if (!c_.sink->folder(other_, g.pathLength, row)) {
      fail(Error::Sink);
      return false;
    }
    r_.summary.changed = true;
    ++r_.folderRows;
    g.rowGiven = true;
    g.rowPending = false;
  }
  return true;
}

void CardWalk::finishWalk() {
  // D's folders the walk didn't reach.
  if (c_.known) {
    if (!dStarted_ || dUsed_) {
      dStarted_ = true;
      dUsed_ = false;
      advanceKnown();
    }
    while (r_.state != State::Failed && dHave_) {
      if (!goneFolder()) return;
      advanceKnown();
    }
    if (r_.state == State::Failed) return;
  }
  // T is checked whole (its CRCs), or nothing it said counts.
  if (c_.transfer && !c_.transfer->finish()) {
    fail(Error::Transfer);
    return;
  }
  r_.state = State::Settling;
  settleBegun_ = false;
  settle_ = c_.firstAfterCommit && skew_.needsRecount() ? SettlePhase::Recount : SettlePhase::Settle;
}

// ---------------------------------------------------------------------------
// After the walk: the skew, then each doubt
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kDoubtsPerStep = 256;
}  // namespace

void CardWalk::stepSettle() {
  if (!settleBegun_) {
    if (!c_.sink->rewindDoubts()) {
      fail(Error::Sink);
      return;
    }
    settleBegun_ = true;
    if (settle_ == SettlePhase::Recount) {
      skew_.beginRecount();
    } else if (c_.firstAfterCommit) {
      r_.summary.skew = skew_.skew();
    }
  }
  for (uint32_t i = 0; i < kDoubtsPerStep; ++i) {
    Doubt d;
    size_t len = 0;
    const Sink::Read got = c_.sink->nextDoubt(other_, &len, &d);
    if (got == Sink::Read::Error) {
      fail(Error::Sink);
      return;
    }
    if (got == Sink::Read::Item) {
      if (len == 0 || len > cc::kMaxRelPath) {
        fail(Error::Sink);
        return;
      }
      other_[len] = 0;
    }
    if (got == Sink::Read::End) {
      if (settle_ == SettlePhase::Recount) {
        r_.recounted = true;
        settle_ = SettlePhase::Settle;
        settleBegun_ = false;
        return;
      }
      if (!c_.sink->finish(r_.summary)) {
        fail(Error::Sink);
        return;
      }
      r_.state = State::Done;
      return;
    }
    if (settle_ == SettlePhase::Recount) {
      if (d.hasDelta) skew_.recount(d.delta);
      continue;
    }
    if (!d.settle) continue;
    bool read = false;
    if (!settleOne(d, len, &read)) return;
    if (read) return;  // one qfp read per step
  }
}

bool CardWalk::settleOne(const Doubt& d, size_t len, bool* read) {
  FileRow row;
  row.size = d.size;
  row.fatTime = d.fatTime;
  row.qfp = d.deviceQfp;
  row.status = d.fallback;
  const int32_t skew = r_.summary.skew;
  if (d.hasDelta && skew != 0 && d.delta == skew) {
    row.status = Status::Software;
    ++r_.bySkew;
  } else if (d.transferQfp == 0) {
    ++r_.notTransfer;  // T has no fingerprint to confirm it by
  } else if (d.deviceQfp != 0) {
    // The device read this file at this size and time already.
    if (d.deviceQfp == d.transferQfp) {
      row.status = Status::Software;
      row.confirmed = true;
      ++r_.byDeviceQfp;
    } else {
      ++r_.notTransfer;
    }
  } else {
    *read = true;
    ++r_.qfpReads;
    uint64_t q = 0;
    bool ok = false;
    if (cc::Source* src = c_.lister->openFile(other_, len)) {
      ok = fileQfp(*src, d.size, base_, bytes_ < kQfpBuffer ? bytes_ : kQfpBuffer, &q);
      c_.lister->closeFile();
    }
    if (!ok) {
      // Not settled: the row without T for now, and the walk's summary says
      // so (the next walk asks T again: Summary::unsettled).
      ++r_.qfpFailed;
      ++r_.notTransfer;
      r_.summary.unsettled = true;
    } else {
      row.qfp = q;
      if (q == d.transferQfp) {
        row.status = Status::Software;
        row.confirmed = true;
        ++r_.byQfp;
      } else {
        ++r_.notTransfer;
      }
    }
  }
  return emitFile(other_, len, Change::Settled, row);
}

}  // namespace cardwalk
