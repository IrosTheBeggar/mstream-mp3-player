// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryUpdate.h"

#include <algorithm>
#include <cstdlib>
#include <new>

const tagstore::Names LibraryUpdate::kIndexNames{"/.player/library.idx", "/.player/library.tmp", "/.player/library"};

namespace {

void* heapAlloc(size_t n) { return std::malloc(n); }
void heapFree(void* p) { std::free(p); }

// A file of the card as LibraryIndex's streams.
class FileIn : public ByteSource {
public:
  explicit FileIn(tagstore::File& f) : f_(f) {}
  size_t read(void* data, size_t n) override {
    const uint32_t left = f_.size() > at_ ? f_.size() - at_ : 0;
    const uint32_t k = static_cast<uint32_t>(std::min<size_t>(n, left));
    if (k == 0 || !f_.read(at_, data, k)) return 0;
    at_ += k;
    return k;
  }

private:
  tagstore::File& f_;
  uint32_t at_ = 0;
};

class FileOut : public ByteSink {
public:
  explicit FileOut(tagstore::File& f) : f_(f) {}
  bool write(const void* data, size_t n) override {
    if (!f_.write(at_, data, static_cast<uint32_t>(n))) return false;
    at_ += static_cast<uint32_t>(n);
    return true;
  }

private:
  tagstore::File& f_;
  uint32_t at_ = 0;
};

// library.tmp left alone by a cut (2.12.6's settle()): whole when its
// header reads and its sum holds (an older version's is taken as it is: it
// is rebuilt anyway). It is loaded once to know (the boot's index is empty
// then), and cleared. No memory to load it counts as torn: library.idx is
// a cache the records rebuild (tags.tmp's check keeps its tmp instead).
class IdxCheck : public tagstore::TmpCheck {
public:
  explicit IdxCheck(LibraryIndex& idx) : idx_(idx) {}
  Verdict check(tagstore::Fs& fs, const char* tmp) override {
    tagstore::File* f = fs.open(tmp, tagstore::Fs::Mode::Read);
    if (!f) return Verdict::Torn;
    FileIn head(*f);
    LibraryIndex::Inputs got;
    const LibraryIndex::Load r = LibraryIndex::peek(head, &got);
    bool ok = r == LibraryIndex::Load::Outdated;
    if (r == LibraryIndex::Load::Loaded) {
      FileIn all(*f);
      ok = idx_.load(all, got) == LibraryIndex::Load::Loaded;
      idx_.clear();
    }
    fs.close(f);
    return ok ? Verdict::Whole : Verdict::Torn;
  }

private:
  LibraryIndex& idx_;
};

// One of the build's files, read only, its failed reads noted: a read that
// fails is the card's trouble (pulled, failing), whoever asked. The walkers
// say Why::Io, but D's rows and its folders' facts read on without a word
// (a row Pending, no facts), and a builder that restarts without both
// files says "no records". Its size is the one at the open: FatFs keeps it
// in the FIL, so a pulled card's reads are tried, and fail.
class Watched : public tagstore::File {
public:
  void watch(tagstore::File* f) {
    f_ = f;
    size_ = f ? f->size() : 0;
  }
  bool failed() const { return failed_; }
  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (f_ && f_->read(offset, out, n)) return true;
    failed_ = true;
    return false;
  }
  bool write(uint32_t, const void*, uint32_t) override { return false; }
  bool sync() override { return true; }
  bool truncate(uint32_t) override { return false; }

private:
  tagstore::File* f_ = nullptr;
  uint32_t size_ = 0;
  bool failed_ = false;
};

// The build's three opens of D (the merge, DSTA's rows, DFLD's facts: each
// read front to back) and T's, watched, closed together.
struct Opened {
  tagstore::Fs& fs;
  tagstore::File* t = nullptr;
  tagstore::File* d = nullptr;
  tagstore::File* rows = nullptr;
  tagstore::File* facts = nullptr;
  Watched wt, wd, wrows, wfacts;
  explicit Opened(tagstore::Fs& f) : fs(f) {}
  ~Opened() {
    for (tagstore::File* x : {t, d, rows, facts})
      if (x) fs.close(x);
  }
  void watch() {
    wt.watch(t);
    wd.watch(d);
    wrows.watch(rows);
    wfacts.watch(facts);
  }
  bool transferFailed() const { return wt.failed(); }
  bool deviceFailed() const { return wd.failed() || wrows.failed() || wfacts.failed(); }
};

}  // namespace

LibraryUpdate::LibraryUpdate(const Config& c) : c_(c) {
  if (!c_.alloc || !c_.release) {
    c_.alloc = heapAlloc;
    c_.release = heapFree;
  }
}

float LibraryUpdate::msSince(uint64_t t0) const {
  return c_.nowUs ? static_cast<float>(c_.nowUs() - t0) / 1000.0f : 0.0f;
}

// ---- the pure parts ----

bool LibraryUpdate::safePoint(bool playing, bool waiting, uint32_t trackLeftMs, uint32_t sinceSeekMs,
                              uint32_t needLeftMs) {
  if (waiting) return false;
  if (!playing) return true;
  return trackLeftMs >= needLeftMs && sinceSeekMs >= kSeekQuietMs;
}

uint32_t LibraryUpdate::safeLeftFor(uint32_t pauseMs) {
  const uint64_t want = static_cast<uint64_t>(pauseMs) + kSafeMarginMs;
  if (want < kSafeLeftMs) return kSafeLeftMs;
  return want > kSafeCapMs ? kSafeCapMs : static_cast<uint32_t>(want);
}

uint32_t LibraryUpdate::pauseFor(uint32_t tracks) {
  const uint64_t ms = static_cast<uint64_t>(tracks) * kPauseUsPerTrack / 1000u;
  return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
}

uint32_t LibraryUpdate::safeLeftMs() const {
  const uint32_t tracks = c_.index && c_.index->ready() ? c_.index->trackCount() : 0;
  const uint32_t estimate = pauseFor(tracks);
  return safeLeftFor(lastPauseMs_ > estimate ? lastPauseMs_ : estimate);
}

LibraryUpdate::Verdict LibraryUpdate::roomToBuild(const Room& r) {
  Verdict v;
  v.peak = r.indexBytes + r.indexBytes / 8 + 96 * 1024;
  v.room = r.psramFree + r.indexBytes + r.alsoFreed;
  // (In 64 bits: 1.1 x a few MB on a 32-bit size_t is fine, but be plain.)
  if (static_cast<uint64_t>(v.room) * 10 < static_cast<uint64_t>(v.peak) * 11) {
    v.shortOf = Short::Room;
    return v;
  }
  if (r.trackBlock < r.tableBytes && r.psramLargest < r.tableBytes + r.tableBytes / 16) v.shortOf = Short::Table;
  return v;
}

size_t LibraryUpdate::spareOf(const Room& r) {
  const Verdict v = roomToBuild(r);
  if (v.shortOf != Short::None) return 0;
  // The room left over 1.1 x the peak (rounded up: the check's own edge).
  const uint64_t need = (static_cast<uint64_t>(v.peak) * 11 + 9) / 10;
  uint64_t spare = v.room > need ? v.room - need : 0;
  // The new table in a free block of its own: what that block can lose.
  if (r.trackBlock < r.tableBytes) {
    const size_t table = r.tableBytes + r.tableBytes / 16;
    spare = std::min<uint64_t>(spare, r.psramLargest > table ? r.psramLargest - table : 0);
  }
  return static_cast<size_t>(spare);
}

bool LibraryUpdate::records() const {
  const bool t = c_.root && c_.root->present && !transferBad_;
  return t || (c_.store && (c_.store->device().present || c_.store->hasJournals()));
}

uint32_t LibraryUpdate::trackSlots() const {
  // As buildIndex() configures the builder: T while this session hasn't
  // found it bad, T listing until the walk at its commit, D's own rows
  // capping D's records while T lists. No records: the walk, the same files
  // as the index (a walk reserves as it goes).
  const uint32_t walked = c_.index && c_.index->ready() ? c_.index->trackCount() : 0;
  if (!c_.store || !c_.root) return walked;
  const bool useT = c_.root->present && !transferBad_;
  const tagstore::DeviceInfo& di = c_.store->device();
  if (!useT && !di.present) return walked;
  const bool walkedHere = di.present && di.header.walked && di.header.walk == c_.root->identity;
  const bool tLists = useT && (!walkedHere || !di.present);
  return LibraryBuilder::trackSlots(di.present ? di.tags.recordCount : 0, di.present ? &di.ownRecords : nullptr,
                                    useT ? c_.root->tagsRecords : 0, tLists);
}

bool LibraryUpdate::writeMarker() {
  if (!c_.fs) return false;
  tagstore::File* f = c_.fs->open(kMarker, tagstore::Fs::Mode::Create);
  const bool ok = f && c_.fs->close(f);
  if (ok) markerSet_ = true;
  return ok;
}

bool LibraryUpdate::cardAnswers() {
  if (!c_.fs) return false;
  // (There is no card-detect: a card pulled while on is found here, before
  // the fence, so the step leaves the index as it is.)
  if (!c_.fs->exists(c_.musicRoot)) return false;
  const bool useT = c_.root && c_.root->present && !transferBad_;
  const bool useD = c_.store && c_.store->device().present;
  for (int k = 0; k < 2; ++k) {
    if (!(k == 0 ? useT : useD)) continue;
    tagstore::File* f = c_.fs->open(k == 0 ? c_.root->tagsPath : c_.store->devicePath(), tagstore::Fs::Mode::Read);
    if (!f) return false;
    c_.fs->close(f);
  }
  return true;
}

bool LibraryUpdate::deviceUnlisted() const {
  if (!c_.store || (c_.root && c_.root->present && !transferBad_)) return false;
  const tagstore::DeviceInfo& di = c_.store->device();
  return di.present && !di.header.walked;
}

bool LibraryUpdate::walkListsCard() const {
  if (!c_.store || (c_.root && c_.root->present && !transferBad_)) return true;
  // A walk waiting in walk.jnl lists the card once merged (its rows; DHDR
  // walked when its doubts are settled: with no T there are none).
  if (c_.store->hasWalk()) return true;
  return c_.store->device().present && c_.store->device().header.walked;
}

bool LibraryUpdate::recordsListCard() const {
  if (walkListsCard()) return true;
  // No D: the build walks /music, unless the journal has the scan's records
  // (the compaction first makes D of them alone).
  return c_.store && !c_.store->device().present && !c_.store->hasJournals();
}

// ---- the build and the save ----

void LibraryUpdate::compactFirst(tagstore::TagStore::Compacted* c, bool* ran) {
  *ran = false;
  if (!c_.store || (!c_.store->hasJournals() && !c_.store->wantsCompaction())) return;
  // (A failure leaves the journals: the build reads tags.bin alone, and
  // saves soft inputs the next boot finds stale.)
  *c = c_.store->compact();
  *ran = true;
}

bool LibraryUpdate::buildIndex(bool update, LibraryBuilder::Result* r, bool* walked, bool* noMemory, bool* cardGone,
                              bool* readErrors) {
  *r = LibraryBuilder::Result{};
  *walked = *noMemory = *cardGone = *readErrors = false;
  LibraryIndex& index = *c_.index;
  const tagstore::DeviceInfo* di = c_.store ? &c_.store->device() : nullptr;
  const bool useT = c_.root && c_.root->present && !transferBad_;
  bool useD = di && di->present;
  // The boot, with D that doesn't list the card (no T, never walked: only
  // what the scan read): /music walked instead, every file Pending, rather
  // than an index of those files alone. (The update step refuses before
  // its fence: update()'s Asked.)
  if (!update && c_.walk && deviceUnlisted()) useD = false;
  Opened o(*c_.fs);
  if (useT) o.t = c_.fs->open(c_.root->tagsPath, tagstore::Fs::Mode::Read);
  if (useD) {
    o.d = c_.fs->open(c_.store->devicePath(), tagstore::Fs::Mode::Read);
    o.rows = c_.fs->open(c_.store->devicePath(), tagstore::Fs::Mode::Read);
    o.facts = c_.fs->open(c_.store->devicePath(), tagstore::Fs::Mode::Read);
  }
  // The update step (an index in use until the fence): T or D there a
  // moment ago and not opening now is the card's trouble (pulled, failing),
  // not "no records": nothing is built (the boot's next load has the last
  // library.idx), rather than walking an absent /music.
  if (update && ((useT && !o.t) || (useD && (!o.d || !o.rows || !o.facts)))) {
    *cardGone = true;
    index.clear();
    return false;
  }
  if (!o.t && !o.d) {
    if (!c_.walk) return false;
    *walked = true;
    const bool ok = c_.walk(index, c_.walkCtx);
    *noMemory = !ok;
    return ok;
  }
  o.watch();
  constexpr uint32_t kRowsBuf = 1024, kFactsBuf = 3072;
  auto* bufs = static_cast<uint8_t*>(c_.alloc(kRowsBuf + kFactsBuf));
  void* rowsMem = c_.alloc(sizeof(tagstore::BuilderRows));
  void* factsMem = c_.alloc(sizeof(tagstore::BuilderFacts));
  auto* rows = rowsMem ? new (rowsMem) tagstore::BuilderRows() : nullptr;
  auto* facts = factsMem ? new (factsMem) tagstore::BuilderFacts() : nullptr;
  LibraryBuilder builder(c_.alloc, c_.release);
  LibraryBuilder::Config bc;
  bc.root = c_.musicRoot;
  bc.transfer = o.t ? &o.wt : nullptr;
  // T's skew as the walk found it at this commit; before the first walk
  // after it T's paths count as present (the software listed the card
  // moments ago) and its records are taken as they are.
  const bool walkedHere = useD && c_.root && di->header.walked && di->header.walk == c_.root->identity;
  bc.skew = walkedHere ? di->header.skew : 0;
  bc.transferLists = o.t && !walkedHere;
  bc.device = o.d ? &o.wd : nullptr;
  if (o.d && bufs && rows && o.rows && rows->begin(o.wrows, bufs, kRowsBuf)) bc.rows = rows;
  if (o.d && bufs && facts && o.facts && facts->begin(o.wfacts, bufs + kRowsBuf, kFactsBuf)) bc.facts = facts;
  if (c_.root) {
    bc.libraryRoots = c_.root->rootList();
    bc.libraryRootCount = c_.root->rootCount;
  }
  *r = builder.build(index, bc);
  if (rows) {
    rows->~BuilderRows();
    c_.release(rowsMem);
  }
  if (facts) {
    facts->~BuilderFacts();
    c_.release(factsMem);
  }
  c_.release(bufs);
  // A read that failed is the card's trouble, not a file's checks. In the
  // update step (the old index in use until the fence, and saved on the
  // card): one of D's, or T's with no whole build from D after it, fails
  // the step rather than walking a card that may be gone (an empty /music:
  // "Library updated", nothing in it) or saving a path-named index over
  // library.idx (a glitch the card came back from).
  const bool tFailed = o.transferFailed(), dFailed = o.deviceFailed();
  *readErrors = tFailed || dFailed;
  if (update && (dFailed || (tFailed && !(r->built && r->deviceUsed)))) {
    *cardGone = true;
    *noMemory = r->noMemory;
    index.clear();
    return false;
  }
  // A T that failed its checks (at its start, or found at its end: the
  // builder restarted from D alone, 3.4.1) is left out for the session; one
  // whose reads failed isn't (the next build tries it again).
  if (useT && o.t && !r->transferUsed && r->transferWhy != cardcontract::Why::Ok && !tFailed) transferBad_ = true;
  if (r->noRecords) {
    if (!c_.walk) return false;
    *walked = true;
    const bool ok = c_.walk(index, c_.walkCtx);
    *noMemory = !ok;
    return ok;
  }
  *noMemory = r->noMemory;
  return r->built;
}

bool LibraryUpdate::save(const LibraryIndex::Inputs& inputs) {
  tagstore::Fs& fs = *c_.fs;
  bool ok = tagstore::prepareTmp(fs, kIndexNames);
  tagstore::File* f = ok ? fs.open(kIndexNames.tmp, tagstore::Fs::Mode::Create) : nullptr;
  ok = f != nullptr;
  if (ok) {
    FileOut out(*f);
    ok = c_.index->save(out, inputs);
    ok = fs.close(f) && ok;
  }
  ok = ok && tagstore::replace(fs, kIndexNames);
  if (!ok) fs.remove(kIndexNames.tmp);
  return ok;
}

// ---- the boot (3.2.2) ----

LibraryUpdate::Booted LibraryUpdate::boot() {
  Booted b;
  tagstore::Fs& fs = *c_.fs;
  LibraryIndex& index = *c_.index;
  // library.idx: a cut rename settled (2.12.6), the marker, its header.
  IdxCheck check(index);
  b.tmpSettled = tagstore::settle(fs, kIndexNames, &check);
  b.marker = markerSet_ = fs.exists(kMarker);
  const tagstore::Identity none;
  const tagstore::Identity& root = c_.root ? c_.root->identity : none;
  uint64_t t0 = now();
  LibraryIndex::Inputs saved;
  libraryboot::In in;
  in.saved = libraryboot::Saved::Missing;
  if (tagstore::File* f = fs.open(kIndexNames.path, tagstore::Fs::Mode::Read)) {
    FileIn head(*f);
    in.saved = libraryboot::savedOf(LibraryIndex::peek(head, &saved), saved, root);
    fs.close(f);
  }
  b.peekMs = msSince(t0);
  in.marker = b.marker;
  in.transfer = c_.root && c_.root->present;
  in.device = c_.store && c_.store->device().present;
  libraryboot::Decision d = libraryboot::decide(in);
  auto load = [&]() {
    LibraryIndex::Load r = LibraryIndex::Load::Corrupt;
    if (tagstore::File* f = fs.open(kIndexNames.path, tagstore::Fs::Mode::Read)) {
      FileIn all(*f);
      r = index.load(all, saved);  // its own inputs: the decision compared them
      fs.close(f);
    }
    return r;
  };
  if (d.action == libraryboot::Action::Load) {
    t0 = now();
    const LibraryIndex::Load r = load();
    b.loadMs = msSince(t0);
    if (r == LibraryIndex::Load::Loaded) {
      b.ok = true;
      b.softStale = c_.store && libraryboot::softStale(saved, c_.store->deviceCrc(), c_.store->journalSeq());
    } else if (r == LibraryIndex::Load::NoMemory) {
      b.noMemory = true;
      b.saved = in.saved;
      b.decision = d;
      return b;
    } else {
      in.saved = libraryboot::Saved::Corrupt;  // its sum failed: built or walked instead
      d = libraryboot::decide(in);
    }
  }
  b.saved = in.saved;
  b.decision = d;
  if (!b.ok && d.action != libraryboot::Action::Load) {
    if (d.compactFirst) {
      t0 = now();
      compactFirst(&b.compaction, &b.compacted);
      b.compactMs = msSince(t0);
    }
    t0 = now();
    bool cardGone = false;
    b.ok = buildIndex(false, &b.build, &b.walked, &b.noMemory, &cardGone, &b.readErrors);
    b.built = !b.walked && b.ok;
    b.buildMs = msSince(t0);
    if (b.ok) {
      // Journals the compaction couldn't fold in (a full card, a walk being
      // written) weren't read, or a read failed (the card?): the inputs
      // saved say records were left out, and the next boot finds the index
      // soft-stale (rebuilt at its scan's end).
      b.journalsLeft = c_.store && c_.store->hasJournals();
      const LibraryIndex::Inputs inputs = libraryboot::inputsOf(
          root, !b.walked && b.build.transferUsed, c_.store ? c_.store->deviceCrc() : 0,
          c_.store ? c_.store->journalSeq() : 0, b.journalsLeft || b.readErrors);
      t0 = now();
      b.saveFailed = !save(inputs);
      b.saveMs = msSince(t0);
    } else if (b.noMemory && in.saved == libraryboot::Saved::Matches) {
      // The marker's build (a matching library.idx builds only for it) ran
      // out of PSRAM on this boot's fresh heap: no update step would fit
      // either. library.idx, whose hard inputs are the card's, is loaded
      // instead, as the Load row would (stale, but a library: without this
      // every boot would end here with none), and the marker goes; this
      // session's short memory checks don't write it again.
      t0 = now();
      const LibraryIndex::Load r = load();
      b.loadMs = msSince(t0);
      if (r == LibraryIndex::Load::Loaded) {
        b.ok = true;
        b.noMemory = false;
        b.loadedShort = true;
        bootBuildShort_ = true;
        b.softStale = c_.store && libraryboot::softStale(saved, c_.store->deviceCrc(), c_.store->journalSeq());
        if (b.marker) {
          b.markerRemoved = fs.remove(kMarker);
          if (b.markerRemoved) markerSet_ = false;
        }
        return b;
      }
    }
  }
  // The marker goes once what it asked for is built and saved (a build
  // that didn't save keeps it: the next boot builds again).
  if (b.ok && b.marker && d.removeMarker && !b.saveFailed) {
    b.markerRemoved = fs.remove(kMarker);
    if (b.markerRemoved) markerSet_ = false;
  }
  return b;
}

// ---- the update step (3.4.2) ----

void LibraryUpdate::ask(const char* why, bool deferToBoot) {
  if (phase_ == Phase::Idle) {
    phase_ = Phase::Asked;
    why_ = why ? why : "";
    deferAsked_ = false;
  }
  deferAsked_ = deferAsked_ || deferToBoot;
}

LibraryUpdate::Out LibraryUpdate::update(const In& in) {
  Out o;
  // The last seek's time (the safe point's 2 s), kept whatever the phase.
  if (!primed_) {
    primed_ = true;
    lastSeekSeq_ = in.seekSeq;
  } else if (in.seekSeq != lastSeekSeq_) {
    lastSeekSeq_ = in.seekSeq;
    lastSeekMs_ = in.nowMs;
    seekSeen_ = true;
  }
  // Once passed, cleared: an old seek can't come back when millis() wraps.
  if (seekSeen_ && in.nowMs - lastSeekMs_ >= kSeekQuietMs) seekSeen_ = false;
  switch (phase_) {
    case Phase::Idle: break;
    case Phase::Asked: {
      o.holdScan = true;
      if (in.walking) {
        o.wait = Wait::Walk;
        break;
      }
      if (in.journals && !in.compactFailed) {
        o.compact = true;
        o.wait = Wait::Compaction;
        break;
      }
      if (!in.workerFree) {
        o.wait = Wait::Worker;
        break;
      }
      const uint32_t sinceSeek = seekSeen_ ? in.nowMs - lastSeekMs_ : UINT32_MAX;
      if (!safePoint(in.playing, in.waiting, in.trackLeftMs, sinceSeek, safeLeftMs())) {
        o.wait = Wait::SafePoint;
        break;
      }
      // The worker's task, made now if it ended (3 s after its last step):
      // no internal RAM for its stack, and the step waits here, the index
      // as it is, never behind a fence whose build can't start.
      if (!in.workerUp) {
        o.wait = Wait::Worker;
        o.wantWorker = true;
        break;
      }
      step_ = Step{};
      step_.why = why_;
      // The card answers first (it may have been pulled while on): a step
      // on an absent card leaves the index as it is.
      if (!cardAnswers()) {
        step_.cardGone = true;
        phase_ = Phase::Idle;
        o.act = Do::Failed;
        ++steps_;
        break;
      }
      // The records list the card (its compaction done): else the build
      // would replace the index with the files the scan read alone (a
      // failed first walk, 2026-10-09). Before the memory check, so no
      // marker makes the next boot build it either.
      if (deviceUnlisted() && c_.index && c_.index->ready()) {
        step_.unlisted = true;
        phase_ = Phase::Idle;
        deferAsked_ = false;
        o.act = Do::Failed;
        ++steps_;
        break;
      }
      Room r;
      r.psramFree = in.psramFree;
      r.psramLargest = in.psramLargest;
      if (c_.index && c_.index->ready()) {
        const LibraryIndex::Memory m = c_.index->memory();
        r.indexBytes = m.total;
        r.trackBlock = m.tracks;
      }
      r.alsoFreed = in.alsoFreed;
      r.tableBytes = static_cast<size_t>(trackSlots()) * sizeof(LibraryIndex::Track);
      room_ = r;
      verdict_ = roomToBuild(r);
      deferForced_ = deferAsked_;
      if (deferAsked_ || verdict_.shortOf != Short::None) {
        // The next boot builds before the UI, on a fresh heap (3.2.2's
        // first row): without the marker it would load the old index, whose
        // hard inputs still match, and meet the same check again. Not when
        // this boot's own marker build ran short: the next would too.
        if (!deferAsked_ && bootBuildShort_) {
          step_.markerSkipped = true;
        } else {
          step_.markerWritten = writeMarker();
        }
        phase_ = Phase::Idle;
        deferAsked_ = false;
        ++deferrals_;
        o.act = Do::Deferred;
        break;
      }
      phase_ = Phase::Fence;
      fenceAtMs_ = in.nowMs;
      o.act = Do::Fence;
      break;
    }
    case Phase::Fence: o.act = Do::Fence; break;  // until fencedUp(), this pass
    case Phase::Build: o.build = true; break;
    case Phase::Building: break;
    case Phase::Live:
      if (liveDue_) {
        step_.fenceMs = in.nowMs - fenceAtMs_;
        // The pause the next safe point allows for (a whole build's: a
        // failed one's fence says nothing of the next).
        if (step_.built) lastPauseMs_ = step_.fenceMs;
        o.act = Do::Live;
      }
      break;
    case Phase::Save: o.save = true; break;
    case Phase::Saving: break;
  }
  if (savedDue_ && phase_ == Phase::Idle && o.act == Do::None) {
    savedDue_ = false;
    o.act = Do::Saved;
  }
  if (deferredDue_ && phase_ == Phase::Idle && o.act == Do::None) {
    deferredDue_ = false;
    o.act = Do::Deferred;
  }
  const bool held = phase_ != Phase::Idle && phase_ != Phase::Asked;
  o.updating = held;
  o.libraryWrite = held;
  o.fenced = fenced();
  return o;
}

void LibraryUpdate::fencedUp() {
  if (phase_ != Phase::Fence) return;
  // Step 3's last part: the old index (two don't fit at 20k). Its track
  // table's block is kept for the build (keepTrackBlock()).
  if (c_.index) c_.index->clear();
  phase_ = Phase::Build;
}

void LibraryUpdate::cantFence() {
  if (phase_ != Phase::Fence) return;
  deferForced_ = false;
  verdict_.shortOf = Short::Carry;
  // (Not after this boot's own marker build ran short: the next would too.)
  if (bootBuildShort_) {
    step_.markerSkipped = true;
  } else {
    step_.markerWritten = writeMarker();
  }
  ++deferrals_;
  deferAsked_ = false;
  phase_ = Phase::Idle;
  deferredDue_ = true;
}

void LibraryUpdate::buildStarted() {
  if (phase_ == Phase::Build) phase_ = Phase::Building;
}

void LibraryUpdate::stepBuild() {
  const uint64_t t0 = now();
  step_.built =
      buildIndex(true, &step_.build, &step_.walked, &step_.noMemory, &step_.cardGone, &step_.readErrors);
  step_.buildMs = msSince(t0);
  // What the save writes (3.4.3): taken now, as the build read them (the
  // scan, which moves the journal on, waits for the save). A read that
  // failed and still built (T's, D read whole): saved as records left out,
  // so the next boot rebuilds with T at its scan's end.
  const tagstore::Identity none;
  const tagstore::Identity& root = c_.root ? c_.root->identity : none;
  step_.journalsLeft = c_.store && c_.store->hasJournals();
  step_.inputs = libraryboot::inputsOf(root, !step_.walked && step_.build.transferUsed,
                                       c_.store ? c_.store->deviceCrc() : 0, c_.store ? c_.store->journalSeq() : 0,
                                       step_.journalsLeft || step_.readErrors);
}

void LibraryUpdate::buildDone() {
  if (phase_ != Phase::Building) return;
  phase_ = Phase::Live;
  liveDue_ = true;
}

void LibraryUpdate::lived() {
  if (phase_ != Phase::Live) return;
  liveDue_ = false;
  ++steps_;
  // Nothing to save (the build failed: no library until the next boot,
  // whose load finds the last library.idx): the step is over.
  if (!step_.built) {
    phase_ = Phase::Idle;
    savedDue_ = true;
    return;
  }
  phase_ = Phase::Save;
}

void LibraryUpdate::saveStarted() {
  if (phase_ == Phase::Save) phase_ = Phase::Saving;
}

void LibraryUpdate::stepSave() {
  const uint64_t t0 = now();
  step_.saved = save(step_.inputs);
  // The marker (a deferral's, or one the boot couldn't remove) asked for a
  // build: this one is it.
  if (step_.saved && markerSet_ && c_.fs->exists(kMarker)) step_.markerRemoved = c_.fs->remove(kMarker);
  if (step_.markerRemoved) markerSet_ = false;
  step_.saveMs = msSince(t0);
}

void LibraryUpdate::saveDone() {
  if (phase_ != Phase::Saving) return;
  phase_ = Phase::Idle;
  savedDue_ = true;
}
