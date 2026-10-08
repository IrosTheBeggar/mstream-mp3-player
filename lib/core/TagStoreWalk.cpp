// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagStoreWalk.h"

#include <cstring>

namespace tagstore {

namespace cc = cardcontract;
namespace cw = cardwalk;

FolderFacts folderFacts(const cw::FolderRow& row) {
  FolderFacts f;
  f.digest = row.digest;
  f.imageSize = row.imageSize;
  f.imageTime = row.imageTime;
  f.audio = row.audio;
  f.images = row.images;
  f.others = row.others;
  f.imageRank = row.imageRank;
  f.imageOwned = row.imageOwned;
  const size_t n = row.image && row.imageLength <= 255 ? row.imageLength : 0;
  f.imageLength = static_cast<uint8_t>(n);
  if (n) std::memcpy(f.image, row.image, n);
  f.image[n] = 0;
  return f;
}

// ---------------------------------------------------------------------------
// KnownD
// ---------------------------------------------------------------------------
bool KnownD::begin(TagStore& store, uint8_t* scratch, uint32_t scratchBytes) {
  close();
  fs_ = &store.fs();
  have_ = held_ = done_ = failed_ = false;
  if (!store.device().present) return true;  // no D: the walk adds every file
  f_ = fs_->open(store.devicePath(), Fs::Mode::Read);
  if (!f_ || r_.begin(*f_, scratch, scratchBytes, nullptr, true) != Why::Ok) {
    failed_ = true;
    return false;
  }
  have_ = true;
  return true;
}

void KnownD::close() {
  if (f_ && fs_) fs_->close(f_);
  f_ = nullptr;
  have_ = false;
}

bool KnownD::nextFolder(cw::KnownFolder* out) {
  if (!have_ || done_ || failed_) return false;
  for (;;) {
    DeviceReader::Step s;
    if (held_) {
      s = heldStep_;
      held_ = false;
    } else {
      s = r_.next();
    }
    if (s == DeviceReader::Step::Record) continue;  // rows not asked for
    if (s == DeviceReader::Step::End) {
      done_ = true;
      return false;
    }
    if (s == DeviceReader::Step::Bad) {
      failed_ = true;
      return false;
    }
    const cc::mptg::Walker& w = r_.walker();
    const size_t n = w.pathLength();
    std::memcpy(folder_, w.path(), n);
    folder_[n] = 0;
    out->path = folder_;
    out->pathLength = n;
    out->digest = r_.folderFacts().digest;
    out->imageOwned = r_.folderFacts().imageLength && r_.folderFacts().imageOwned;
    return true;
  }
}

bool KnownD::nextFile(cw::KnownFile* out) {
  if (!have_ || done_ || failed_ || held_) return false;
  const DeviceReader::Step s = r_.next();
  if (s != DeviceReader::Step::Record) {
    // The next folder, or the end: nextFolder() takes it.
    held_ = true;
    heldStep_ = s;
    if (s == DeviceReader::Step::Bad) failed_ = true;
    return false;
  }
  const cc::mptg::Walker& w = r_.walker();
  const size_t n = w.nameLength();
  std::memcpy(name_, w.name(), n);
  name_[n] = 0;
  out->name = name_;
  out->nameLength = n;
  const cc::mptg::Record& rec = w.record();
  out->row.size = rec.size;
  out->row.fatTime = rec.fatTime;
  out->row.qfp = rec.qfp;
  out->row.status = r_.row().status;
  out->row.confirmed = r_.row().confirmed;
  return true;
}

// ---------------------------------------------------------------------------
// WalkSink
// ---------------------------------------------------------------------------
bool WalkSink::begin(TagStore& store, const Identity& walk, uint8_t* buf, uint32_t bytes, uint8_t* readBuf,
                     uint32_t readBytes) {
  store_ = &store;
  id_ = walk;
  readBuf_ = readBuf;
  readBytes_ = readBytes;
  doubts_ = 0;
  reading_ = false;
  r_.close();
  return readBuf && readBytes >= 64 && store.beginWalk(walk, buf, bytes, &w_);
}

bool WalkSink::folder(const char* rel, size_t len, const cw::FolderRow& row) {
  return w_.folder(rel, len, folderFacts(row));
}

bool WalkSink::folderGone(const char* rel, size_t len) { return w_.folderGone(rel, len); }

bool WalkSink::file(const char* rel, size_t len, cw::Change change, const cw::FileRow& row) {
  (void)change;  // the row says it all; run 2 is the settled rows
  return w_.file(rel, len, row.size, row.fatTime, row.qfp, row.status, row.confirmed);
}

bool WalkSink::fileGone(const char* rel, size_t len) { return w_.gone(rel, len); }

bool WalkSink::doubt(const char* rel, size_t len, const cw::Doubt& d) {
  Doubt x;
  x.settle = d.settle;
  x.hasDelta = d.hasDelta;
  x.delta = d.delta;
  x.size = d.size;
  x.fatTime = d.fatTime;
  x.transferQfp = d.transferQfp;
  x.deviceQfp = d.deviceQfp;
  x.fallback = d.fallback;
  if (!w_.doubt(rel, len, x)) return false;
  ++doubts_;
  return true;
}

bool WalkSink::rewindDoubts() {
  if (!store_) return false;
  if (w_.run() == 1) {
    // The walk is over: run 1 closes, with the skew D has for this commit
    // (none on a new one) until the walk's own comes with its summary.
    const DeviceInfo& d = store_->device();
    const int32_t skew = d.present && d.header.walked && d.header.walk == id_ ? d.header.skew : 0;
    if (!w_.endRun(skew)) return false;
  }
  reading_ = true;
  r_.close();
  if (doubts_ == 0) return true;  // nothing to read back (run 1 may not even be on the card)
  return store_->readWalk(1, readBuf_, readBytes_, &r_);
}

cw::Sink::Read WalkSink::nextDoubt(char* rel, size_t* len, cw::Doubt* d) {
  if (!reading_) return Read::Error;
  if (doubts_ == 0) return Read::End;
  WalkEntry e;
  while (r_.next(&e)) {
    if (e.kind != EntryKind::Doubt) continue;
    std::memcpy(rel, e.path, e.pathLen);
    rel[e.pathLen] = 0;
    *len = e.pathLen;
    d->settle = e.doubt.settle;
    d->hasDelta = e.doubt.hasDelta;
    d->delta = e.doubt.delta;
    d->size = e.doubt.size;
    d->fatTime = e.doubt.fatTime;
    d->transferQfp = e.doubt.transferQfp;
    d->deviceQfp = e.doubt.deviceQfp;
    d->fallback = e.doubt.fallback;
    return Read::Item;
  }
  return r_.failed() ? Read::Error : Read::End;
}

bool WalkSink::finish(const cw::Summary& s) {
  r_.close();
  // A whole walk closes both runs (run 1 here when no doubt was read back),
  // so its commit and skew reach DHDR.
  bool ok = w_.run() != 1 || w_.endRun(s.skew);
  ok = ok && w_.endRun(s.skew);
  return w_.finish() && ok;
}

void WalkSink::abort() {
  r_.close();
  w_.abort();
}

}  // namespace tagstore
