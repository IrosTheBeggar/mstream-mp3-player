// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardJobs.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace cardjobs {

namespace cc = cardcontract;
namespace cw = cardwalk;
namespace ts = tagstore;

// walk.jnl's sink, and a count of the added files new to the index.
class CountingSink : public cw::Sink {
public:
  ts::WalkSink inner;
  bool (*indexed)(const char*, size_t, void*) = nullptr;
  void* ctx = nullptr;
  uint32_t newToIndex = 0;
  bool folder(const char* rel, size_t len, const cw::FolderRow& row) override { return inner.folder(rel, len, row); }
  bool folderGone(const char* rel, size_t len) override { return inner.folderGone(rel, len); }
  bool file(const char* rel, size_t len, cw::Change change, const cw::FileRow& row) override {
    if (change == cw::Change::Added && !(indexed && indexed(rel, len, ctx))) ++newToIndex;
    return inner.file(rel, len, change, row);
  }
  bool fileGone(const char* rel, size_t len) override { return inner.fileGone(rel, len); }
  bool doubt(const char* rel, size_t len, const cw::Doubt& d) override { return inner.doubt(rel, len, d); }
  bool rewindDoubts() override { return inner.rewindDoubts(); }
  Read nextDoubt(char* rel, size_t* len, cw::Doubt* d) override { return inner.nextDoubt(rel, len, d); }
  bool finish(const cw::Summary& s) override { return inner.finish(s); }
  void abort() override { inner.abort(); }
};

// The walk's memory (about 98 KB): PSRAM, never the worker's stack.
struct Jobs::WalkWork {
  cw::CardWalk walk;
  ts::KnownD known;
  CountingSink sink;
  cw::StreamedTransfer streamed;
  cw::IndexedTransfer indexed;
  ts::File* t = nullptr;  // T, open while the walk reads it
  bool begun = false;
  bool again = false;  // askWalk() during it: another after it
  uint8_t scratch[cw::CardWalk::kDeviceScratch];
  uint8_t knownBuf[4096];
  uint8_t journalBuf[4096];
  uint8_t doubtBuf[512];
  uint8_t transferBuf[8192];
};

// The scan's memory (about 85 KB with its read set and chunk).
struct Jobs::ScanWork {
  tagscan::Scanner scanner;
  ts::ChunkBuilder chunk;
  ts::TagStore::View view;
  bool viewOpen = false;
  bool chunkTimed = false;
  uint32_t chunkFirstMs = 0;
  // Verify (gv): T through its HIDX.
  cw::IndexedTransfer verifyT;
  ts::File* verifyFile = nullptr;
  bool verifyOpen = false;
  bool verifyMissing = false;  // no T to verify against
  uint8_t buf[4096];
  uint8_t* chunkBuf = nullptr;
  uint64_t* readSet = nullptr;  // open addressing; 0 empty
  uint32_t readCount = 0;
};

Jobs::Jobs(AllocFn alloc, FreeFn release) : alloc_(alloc), free_(release) {}

Jobs::~Jobs() {
  if (walk_) {
    if (walk_->t && c_.fs) c_.fs->close(walk_->t);
    walk_->~WalkWork();
    release(walk_);
  }
  if (scan_) {
    closeView();
    if (scan_->verifyFile && c_.fs) c_.fs->close(scan_->verifyFile);
    release(scan_->chunkBuf);
    release(scan_->readSet);
    scan_->~ScanWork();
    release(scan_);
  }
}

void* Jobs::alloc(size_t n) { return alloc_ ? alloc_(n) : std::malloc(n); }
void Jobs::release(void* p) {
  if (!p) return;
  if (free_) {
    free_(p);
  } else {
    std::free(p);
  }
}

void Jobs::begin(const Config& c) {
  c_ = c;
  if (c_.readSlots < 16) c_.readSlots = 16;
  if (c_.readSlots & (c_.readSlots - 1)) {
    uint32_t p = 16;
    while (p < c_.readSlots) p <<= 1;
    c_.readSlots = p;
  }
  if (c_.chunkFiles == 0) c_.chunkFiles = 1;
  if (c_.rowsPerStep == 0) c_.rowsPerStep = 1;
  transferBad_ = false;
  appendBlocked_ = false;
  compactFailed_ = false;
  appendFailed_ = false;
  recordedMark_ = counts_.recorded;
  mode_ = Mode::Normal;
  // Nothing for the scan when D has only Software rows (or none) and no
  // journal: every file is the transfer's. Else one pass of the View says.
  const ts::TagStore* st = c_.store;
  restDone_ = !st || (!st->hasJournals() && (!st->device().present || st->device().ownRecords == 0));
  if (scan_) {
    closeView();
    readSetClear();
  }
}

// ---- what there is to do ----

void Jobs::askWalk() {
  if (walk_) {
    walk_->again = true;
  } else {
    walkAsked_ = true;
  }
}

void Jobs::askCompact(bool rescan) {
  compactAsked_ = true;
  compactFailed_ = false;
  if (rescan) rescanAsked_ = true;
}

void Jobs::askRest(Mode mode) {
  mode_ = mode;
  restDone_ = false;
  restartRest_ = true;  // the View starts over at the next scan step (a step may be under way now)
}

bool Jobs::walkWork() const {
  return c_.store && c_.card && (walk_ != nullptr || (walkAsked_ && !c_.store->hasWalk()));
}

bool Jobs::compactWork() const {
  if (!c_.store) return false;
  if (compactAsked_ || appendBlocked_) return !compactFailed_ || compactAsked_;
  // The last walk merged before the next walks (a failed merge isn't handed
  // again: on a full or pulled card it would be every pass, each holding
  // the idle power-off; the walk then waits for the next boot).
  if (walkAsked_ && !walk_ && c_.store->hasWalk()) return !compactFailed_;
  return !compactFailed_ && c_.store->wantsCompaction();
}

bool Jobs::restWork() const {
  if (!c_.store || !c_.card || restDone_) return false;
  // A full journal that can't be compacted: nothing can be recorded.
  return !(appendBlocked_ && compactFailed_);
}

bool Jobs::chunkPending() const { return scan_ && scan_->chunk.count() > 0; }

const tagscan::Record* Jobs::record() const { return scan_ ? &scan_->scanner.record() : nullptr; }

// ---- one step ----

bool Jobs::ensureScan() {
  if (scan_) return true;
  void* mem = alloc(sizeof(ScanWork));
  if (!mem) return false;
  scan_ = new (mem) ScanWork();
  scan_->chunkBuf = static_cast<uint8_t*>(alloc(c_.chunkBytes));
  scan_->readSet = static_cast<uint64_t*>(alloc(static_cast<size_t>(c_.readSlots) * sizeof(uint64_t)));
  if (!scan_->chunkBuf || !scan_->readSet) {
    release(scan_->chunkBuf);
    release(scan_->readSet);
    scan_->~ScanWork();
    release(scan_);
    scan_ = nullptr;
    return false;
  }
  scan_->chunk.begin(scan_->chunkBuf, c_.chunkBytes);
  readSetClear();
  return true;
}

void Jobs::closeView() {
  if (!scan_ || !scan_->viewOpen) return;
  scan_->view.~View();
  new (&scan_->view) ts::TagStore::View();
  scan_->viewOpen = false;
}

bool Jobs::prepare(Job job, Source source, const char* rel, size_t len, uint32_t nowMs) {
  done_ = Done();
  done_.job = job;
  done_.source = source;
  job_ = Job::None;
  source_ = source;
  nowMs_ = nowMs;
  if (!c_.store || !c_.card) return false;
  switch (job) {
    case Job::Walk:
      if (!walk_) {
        void* mem = alloc(sizeof(WalkWork));
        if (!mem) return false;
        walk_ = new (mem) WalkWork();
        walkAsked_ = false;
      }
      closeView();  // the walk writes walk.jnl, which a View may be reading
      break;
    case Job::Compact:
      closeView();  // the compaction replaces tags.bin and the journals
      break;
    case Job::Scan:
      if (!ensureScan()) return false;
      if (source == Source::Rest) {
        if (restartRest_) {
          closeView();
          restartRest_ = false;
        }
      } else {
        if (!rel || len == 0 || len > cc::kMaxRelPath) return false;
        std::memcpy(rel_, rel, len);
        rel_[len] = 0;
        relLength_ = len;
      }
      break;
    default:
      return false;
  }
  job_ = job;
  return true;
}

void Jobs::step() {
  switch (job_) {
    case Job::Walk: stepWalk(); break;
    case Job::Compact: stepCompact(); break;
    case Job::Scan: stepScan(); break;
    default: break;
  }
}

const Done& Jobs::finish() {
  if (done_.walkEnded && walk_) {
    const bool again = walk_->again;
    walk_->~WalkWork();
    release(walk_);
    walk_ = nullptr;
    if (again || done_.walkRetried) walkAsked_ = true;
  }
  job_ = Job::None;
  return done_;
}

// ---- the walk ----

void Jobs::endWalk() {
  WalkWork& w = *walk_;
  if (w.t) {
    c_.fs->close(w.t);
    w.t = nullptr;
  }
  w.known.close();
  done_.walkEnded = true;
  done_.walk = w.walk.result();
  done_.newToIndex = w.sink.newToIndex;
}

void Jobs::stepWalk() {
  if (!walk_) return;
  WalkWork& w = *walk_;
  if (!w.begun) {
    w.begun = true;
    ++counts_.walks;
    appendChunk();  // N7's rule: the scan's chunk out before another job (and append() refuses during a walk)
    ts::TagStore& st = *c_.store;
    // T first: the walk's identity is the root's only when T can be read.
    ts::Identity id;
    const bool useT = c_.transferPath && c_.fs && c_.root.present && !transferBad_;
    const ts::DeviceInfo& d = st.device();
    bool first = true;
    cw::CardWalk::Config cfg;
    if (useT) {
      id = c_.root;
      first = !(d.present && d.header.walked && d.header.walk == id);
      w.t = c_.fs->open(c_.transferPath, ts::Fs::Mode::Read);
      cc::Why why = cc::Why::Missing;
      if (w.t) why = first ? w.streamed.begin(*w.t, w.transferBuf, sizeof(w.transferBuf)) : w.indexed.begin(*w.t);
      if (why == cc::Why::Ok) {
        cfg.transfer = first ? static_cast<cw::Transfer*>(&w.streamed) : static_cast<cw::Transfer*>(&w.indexed);
      } else {
        // A T that won't even open as one is absent from now on.
        transferBad_ = true;
        if (w.t) c_.fs->close(w.t);
        w.t = nullptr;
        id = ts::Identity();
      }
    }
    if (!cfg.transfer) first = !(d.present && d.header.walked && d.header.walk == id);
    if (!w.known.begin(st, w.knownBuf, sizeof(w.knownBuf))) {
      endWalk();
      done_.walk.state = cw::CardWalk::State::Failed;
      done_.walk.error = cw::CardWalk::Error::Known;
      ++counts_.walksFailed;
      return;
    }
    w.sink.indexed = c_.indexed;
    w.sink.ctx = c_.indexedCtx;
    if (!w.sink.inner.begin(st, id, w.journalBuf, sizeof(w.journalBuf), w.doubtBuf, sizeof(w.doubtBuf))) {
      endWalk();
      done_.walk.state = cw::CardWalk::State::Failed;
      done_.walk.error = cw::CardWalk::Error::Sink;
      ++counts_.walksFailed;
      return;
    }
    cfg.lister = c_.card;
    cfg.known = &w.known;
    cfg.sink = &w.sink;
    cfg.firstAfterCommit = first;
    cfg.skew = first ? 0 : d.header.skew;
    cfg.scratch = w.scratch;
    cfg.scratchBytes = sizeof(w.scratch);
    if (!w.walk.begin(cfg)) {
      w.sink.abort();
      endWalk();
      ++counts_.walksFailed;
      return;
    }
  }
  const cw::CardWalk::State s = w.walk.step();
  ++counts_.walkSteps;
  if (s != cw::CardWalk::State::Done && s != cw::CardWalk::State::Failed) return;
  endWalk();
  if (s == cw::CardWalk::State::Failed) {
    ++counts_.walksFailed;
    if (done_.walk.error == cw::CardWalk::Error::Transfer) {
      // T failed a check as it streamed: nothing of the walk counts; walked
      // again without T, which is absent for the rest of the session.
      transferBad_ = true;
      done_.walkRetried = true;
    }
    return;
  }
  // New rows (an added or changed file, a merge against T) may be Pending:
  // the scan's rest looks again.
  if (done_.walk.summary.changed) {
    restDone_ = false;
    restartRest_ = true;
  }
}

// ---- a compaction ----

void Jobs::stepCompact() {
  appendChunk();  // its records folded in (a full journal keeps it: appended after)
  ts::TagStore& st = *c_.store;
  done_.compaction = st.compact(rescanAsked_);
  done_.compacted = done_.compaction.ok;
  if (!done_.compaction.ok) {
    ++counts_.compactionsFailed;
    compactAsked_ = false;
    compactFailed_ = true;
    return;
  }
  ++counts_.compactions;
  compactAsked_ = false;
  compactFailed_ = false;
  appendBlocked_ = false;
  if (done_.compaction.walkMerged || done_.compaction.rescanned || rescanAsked_) {
    restDone_ = false;  // new Pending rows: the walk's, or a Rescan's
    restartRest_ = true;
  }
  rescanAsked_ = false;
  // The records the read set guarded are in D now (once the chunk is out).
  if (appendChunk() && scan_) readSetClear();
}

// ---- the scan ----

bool Jobs::appendChunk() {
  if (!scan_ || scan_->chunk.count() == 0) return true;
  const uint32_t n = scan_->chunk.count();
  if (c_.store->append(scan_->chunk)) {
    ++counts_.chunks;
    counts_.recorded += n;
    done_.appended = true;
    scan_->chunkTimed = false;
    appendBlocked_ = false;
    appendFailed_ = false;
    return true;
  }
  ++counts_.appendFailures;
  done_.appendFailed = true;
  appendBlocked_ = true;  // a compaction first (the journal's limits), or the card refused
  appendFailed_ = true;
  appendFailedMs_ = nowMs_;
  return false;
}

bool Jobs::flushChunk() { return appendChunk(); }

bool Jobs::idleFlush(uint32_t nowMs) {
  if (!chunkPending()) return true;
  if (appendFailed_ && nowMs - appendFailedMs_ < c_.retryMs) return false;
  nowMs_ = nowMs;
  return appendChunk();
}

bool Jobs::newRecords() const {
  return counts_.recorded != recordedMark_ || (chunkPending() && !appendBlocked_);
}

void Jobs::trim() {
  if (!scan_ || scan_->chunk.count() > 0 || scan_->viewOpen || scan_->verifyOpen) return;
  release(scan_->chunkBuf);
  release(scan_->readSet);
  scan_->~ScanWork();
  release(scan_);
  scan_ = nullptr;
}

void Jobs::prepareUpdate() {
  closeView();
  appendChunk();
}

void Jobs::libraryRebuilt() {
  if (scan_) {
    closeView();
    if (scan_->chunk.count() == 0) readSetClear();
  }
  restartRest_ = true;
}

void Jobs::readSetClear() {
  if (!scan_ || !scan_->readSet) return;
  std::memset(scan_->readSet, 0, static_cast<size_t>(c_.readSlots) * sizeof(uint64_t));
  scan_->readCount = 0;
}

bool Jobs::readSetHas(uint64_t h) const {
  if (!scan_ || !scan_->readSet) return false;
  if (h == 0) h = 1;
  const uint32_t mask = c_.readSlots - 1;
  for (uint32_t i = static_cast<uint32_t>(h) & mask, n = 0; n < c_.readSlots; i = (i + 1) & mask, ++n) {
    if (scan_->readSet[i] == 0) return false;
    if (scan_->readSet[i] == h) return true;
  }
  return false;
}

void Jobs::readSetAdd(uint64_t h) {
  if (!scan_ || !scan_->readSet) return;
  if (h == 0) h = 1;
  // Half full at most: past that a file may be read twice (its record twice
  // in the journal, the newest kept), never missed.
  if (scan_->readCount >= c_.readSlots / 2 || readSetHas(h)) return;
  const uint32_t mask = c_.readSlots - 1;
  uint32_t i = static_cast<uint32_t>(h) & mask;
  while (scan_->readSet[i] != 0) i = (i + 1) & mask;
  scan_->readSet[i] = h;
  ++scan_->readCount;
}

bool Jobs::nextRow(char* rel, size_t* len, uint32_t* size, uint32_t* fatTime, uint64_t* qfp, bool* ended) {
  ScanWork& s = *scan_;
  *ended = false;
  if (!s.viewOpen) {
    // A View sees the journal as it is: the chunk out first, so what the
    // scan read before is in it, and the read set can start over.
    if (appendChunk()) readSetClear();
    ++counts_.viewsOpened;
    if (!c_.store->openView(&s.view)) {
      // No memory, or D or a journal can't be read: no rest this session
      // (the walk at the next boot looks again).
      s.view.~View();
      new (&s.view) ts::TagStore::View();
      *ended = true;
      done_.restFailed = true;
      return false;
    }
    s.viewOpen = true;
  }
  for (uint32_t k = 0; k < c_.rowsPerStep; ++k) {
    if (!s.view.next()) {
      *ended = true;
      done_.restFailed = s.view.failed();  // a read failed (the card?), not the end
      return false;
    }
    ++counts_.rowsLooked;
    const ts::Row& row = s.view.row();
    bool want = false;
    switch (mode_) {
      case Mode::Normal: want = row.status == ts::Status::Pending; break;
      case Mode::All: want = true; break;
      case Mode::Verify: want = row.status == ts::Status::Software; break;
    }
    if (!want) continue;
    const size_t n = s.view.pathLength();
    if (n == 0 || n > cc::kMaxRelPath) continue;
    if (mode_ != Mode::Verify && readSetHas(cc::pathHash(s.view.path(), n))) continue;
    std::memcpy(rel, s.view.path(), n);
    rel[n] = 0;
    *len = n;
    const cc::mptg::Record& r = s.view.record();
    *size = r.size;
    *fatTime = r.fatTime;
    *qfp = r.qfp;
    return true;
  }
  return false;  // more next step
}

bool Jobs::scanFile(const char* rel, size_t len, uint32_t size, uint32_t fatTime, uint64_t qfp) {
  ScanWork& s = *scan_;
  done_.handled = true;
  cc::Source* src = c_.card->openFile(rel, len);
  const tagscan::Kind kind = tagscan::kindOf(rel);
  if (!src || src->size() != size || kind == tagscan::Kind::Unknown) {
    // Gone, changed since the walk saw it (the next walk will), or not audio.
    if (src) c_.card->closeFile();
    ++counts_.skipped;
    return false;
  }
  const tagscan::Result r = s.scanner.scan(*src, kind, s.buf, sizeof(s.buf));
  c_.card->closeFile();
  done_.read = true;
  done_.result = r;
  done_.reads = s.scanner.stats().reads;
  if (r == tagscan::Result::ReadError) {
    // Not recorded: read again at the next boot.
    done_.readError = true;
    ++counts_.readErrors;
    return true;
  }
  ++counts_.scanned;
  if (r == tagscan::Result::Unreadable) ++counts_.unreadable;
  if (r == tagscan::Result::Partial) ++counts_.partial;
  cc::mptg::Record rec = s.scanner.record().rec;
  rec.size = size;
  rec.fatTime = fatTime;
  rec.qfp = qfp;
  const ts::Status status = r == tagscan::Result::Unreadable ? ts::Status::Unreadable : ts::Status::Scanned;
  const char* fields[cc::kRunFields];
  s.scanner.record().fields(fields);
  if (!s.chunk.add(rel, len, status, rec, fields)) {
    // The chunk is full: out first. A chunk that can't go keeps the file
    // unrecorded (read again later).
    if (appendChunk()) s.chunk.add(rel, len, status, rec, fields);
  }
  if (s.chunk.count() > 0 && !s.chunkTimed) {
    s.chunkTimed = true;
    s.chunkFirstMs = nowMs_;
  }
  return true;
}

bool Jobs::verifyFile(const char* rel, size_t len, uint32_t size) {
  ScanWork& s = *scan_;
  done_.handled = true;
  if (!s.verifyOpen && !s.verifyMissing) {
    s.verifyMissing = true;
    if (c_.transferPath && c_.fs && !transferBad_) {
      s.verifyFile = c_.fs->open(c_.transferPath, ts::Fs::Mode::Read);
      if (s.verifyFile && s.verifyT.begin(*s.verifyFile) == cc::Why::Ok) {
        s.verifyOpen = true;
        s.verifyMissing = false;
      } else if (s.verifyFile) {
        c_.fs->close(s.verifyFile);
        s.verifyFile = nullptr;
      }
    }
  }
  ++verified_.checked;
  cw::TransferRecord tr;
  if (!s.verifyOpen || !s.verifyT.find(rel, len, &tr) || tr.qfp == 0) {
    ++verified_.noQfp;
    return false;
  }
  cc::Source* src = c_.card->openFile(rel, len);
  uint64_t q = 0;
  const bool ok = src && cw::fileQfp(*src, size, s.buf, sizeof(s.buf), &q);
  if (src) c_.card->closeFile();
  done_.read = ok;
  if (!ok) {
    ++verified_.failed;
  } else if (q == tr.qfp) {
    ++verified_.equal;
  } else {
    ++verified_.differ;
  }
  return ok;
}

void Jobs::stepScan() {
  if (!scan_) return;
  ScanWork& s = *scan_;
  if (source_ != Source::Rest) {
    // A file the loop names (a Pending track of the index).
    std::memcpy(done_.rel, rel_, relLength_ + 1);
    done_.relLength = relLength_;
    done_.handled = true;
    const uint64_t h = cc::pathHash(rel_, relLength_);
    if (!readSetHas(h)) {
      uint32_t size = 0, fatTime = 0;
      if (c_.card->stat(rel_, relLength_, &size, &fatTime)) {
        scanFile(rel_, relLength_, size, fatTime, 0);
      } else {
        ++counts_.skipped;
      }
      // An open View may not have passed it: not read twice.
      if (s.viewOpen) readSetAdd(h);
    }
  } else {
    bool ended = false;
    uint32_t size = 0, fatTime = 0;
    uint64_t qfp = 0;
    size_t len = 0;
    if (nextRow(done_.rel, &len, &size, &fatTime, &qfp, &ended)) {
      done_.relLength = len;
      if (mode_ == Mode::Verify) {
        verifyFile(done_.rel, len, size);
      } else {
        scanFile(done_.rel, len, size, fatTime, qfp);
      }
    } else if (ended) {
      done_.restEnded = true;
      restDone_ = true;
      closeView();
      if (mode_ == Mode::Verify) {
        done_.verifyEnded = true;
        if (s.verifyFile) c_.fs->close(s.verifyFile);
        s.verifyFile = nullptr;
        s.verifyOpen = false;
        s.verifyMissing = false;
        // The scan's own rest again (it may have had more).
        restDone_ = false;
        restartRest_ = true;
      }
      if (mode_ != Mode::Normal) mode_ = Mode::Normal;
    }
  }
  // The chunk out at its size, or its age.
  const bool full = s.chunk.count() >= c_.chunkFiles || s.chunk.bytes() >= c_.chunkBytes / 2;
  const bool old = s.chunkTimed && nowMs_ - s.chunkFirstMs >= c_.chunkMs;
  if (s.chunk.count() > 0 && (full || old || done_.restEnded)) appendChunk();
}

}  // namespace cardjobs
