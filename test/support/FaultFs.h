// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Cards that fail some of their calls, over CutFs (test/support/CutFs.h): the
// play log's tests made them (test_play_log), and the listening stats' proof
// drives the journal over them too (test_stats_store). Host only.
//   - FlakyFs: a card that fails some of its questions and opens, as the
//     firmware's FatFs says them (storage/CardFat), numbered from 0;
//   - FaultFs: a card that fails chosen calls (a write, a sync, a close, an
//     open to create or to update, a rename, half a rename, a remove, a
//     question; a read of a file's bad stretch), each by a Knob.
#include <cstdint>
#include <string>

#include "CutFs.h"
#include "TagStore.h"

namespace faultfs {

using cutfs::CutFs;

// A card that fails some of its questions and opens, as the firmware's FatFs
// says them (storage/CardFat): open() says no file, exists() no,
// firstCluster() 0, and only entry() and openRead() say it was the card.
// They are numbered from 0 in `events`; those from failFrom up to failTo
// fail (failFrom -1: none; failTo -1: for ever).
struct FlakyFs : CutFs {
  long failFrom = -1, failTo = -1, events = 0;
  void failing(long from, long to) {
    events = 0;
    failFrom = from;
    failTo = to;
  }
  tagstore::File* open(const char* p, Mode m) override {
    // (Create is no question: it makes the file, and empties one that is there.)
    if (!inside_ && m != Mode::Create && fails()) return nullptr;
    return CutFs::open(p, m);
  }
  tagstore::File* openRead(const char* p, bool* cardFailed) override {
    if (fails()) {
      *cardFailed = true;
      return nullptr;
    }
    Inside in(*this);
    return CutFs::openRead(p, cardFailed);
  }
  Entry entry(const char* p) override {
    if (fails()) {
      Entry e;
      e.cardFailed = true;
      return e;
    }
    Inside in(*this);
    return CutFs::entry(p);
  }
  bool exists(const char* p) override { return !(!inside_ && fails()) && CutFs::exists(p); }
  uint32_t firstCluster(const char* p) override { return !inside_ && fails() ? 0 : CutFs::firstCluster(p); }

private:
  struct Inside {
    explicit Inside(FlakyFs& f) : f_(f), was_(f.inside_) { f_.inside_ = true; }
    ~Inside() { f_.inside_ = was_; }
    FlakyFs& f_;
    bool was_;
  };
  bool fails() {
    if (failFrom < 0) return false;
    const long k = events++;
    return k >= failFrom && (failTo < 0 || k < failTo);
  }
  bool inside_ = false;
};

// So many calls pass, then so many fail (-1: every one from there).
struct Knob {
  long skip = 0, fail = 0;
  bool hit() {
    if (skip > 0) {
      --skip;
      return false;
    }
    if (fail == 0) return false;
    if (fail > 0) --fail;
    return true;
  }
};

// A card that fails chosen calls, over CutFs (the journal's files through
// it): a write, a sync, a close, an open to write, a rename, a remove, a
// question; a read of a file's bad stretch. A write that fails lands or not
// (`lands`); a sync that fails leaves its file unsynced, and a close that
// fails (a written file's: the knob counts those) doesn't sync it either
// (the card refused both).
struct FaultFs : tagstore::Fs {
  CutFs& cut;
  Knob noWrite, noSync, noClose, noCreate, noUpdate, noRename, halfRename, noRemove, noEntry;
  bool lands = false;
  std::string badPath;  // reads of it that touch [badFrom, badTo) fail, by `bad`
  uint32_t badFrom = 0, badTo = 0;
  Knob bad;
  uint32_t writes = 0;  // writes that reached the card
  explicit FaultFs(CutFs& c) : cut(c) {}

  struct F : tagstore::File {
    FaultFs& fs;
    tagstore::File* in;
    std::string path;
    bool wrote = false;
    F(FaultFs& f, tagstore::File* i, const char* p) : fs(f), in(i), path(p) {}
    uint32_t size() const override { return in->size(); }
    bool damaged() const override { return in->damaged(); }
    bool read(uint32_t off, void* out, uint32_t n) override {
      if (path == fs.badPath && off < fs.badTo && off + n > fs.badFrom && fs.bad.hit()) return false;
      return in->read(off, out, n);
    }
    bool write(uint32_t off, const void* d, uint32_t n) override {
      if (fs.noWrite.hit()) {
        if (fs.lands && in->write(off, d, n)) {
          ++fs.writes;
          wrote = true;
        }
        return false;
      }
      if (!in->write(off, d, n)) return false;
      ++fs.writes;
      wrote = true;
      return true;
    }
    bool sync() override { return !fs.noSync.hit() && in->sync(); }
    bool truncate(uint32_t s) override { return in->truncate(s); }
  };
  tagstore::File* wrap(tagstore::File* in, const char* p) { return in ? new F(*this, in, p) : nullptr; }
  tagstore::File* open(const char* p, Mode m) override {
    if (m == Mode::Create && noCreate.hit()) return nullptr;
    if (m == Mode::Update && noUpdate.hit()) return nullptr;
    return wrap(cut.open(p, m), p);
  }
  tagstore::File* openRead(const char* p, bool* cardFailed) override { return wrap(cut.openRead(p, cardFailed), p); }
  Entry entry(const char* p) override {
    if (noEntry.hit()) {
      Entry e;
      e.cardFailed = true;
      return e;
    }
    return cut.entry(p);
  }
  bool close(tagstore::File* file) override {
    F* f = static_cast<F*>(file);
    const bool refused = f->wrote && noClose.hit();
    const bool was = cut.refuse;
    if (refused) cut.refuse = true;  // (its own sync of a written file refused too)
    const bool ok = cut.close(f->in);
    cut.refuse = was;
    delete f;
    return ok && !refused;
  }
  bool exists(const char* p) override { return cut.exists(p); }
  bool remove(const char* p) override { return !noRemove.hit() && cut.remove(p); }
  bool rename(const char* from, const char* to) override {
    if (noRename.hit()) return false;
    if (halfRename.hit()) {  // the new entry written, the old one not removed
      cut.link(from, to);
      return false;
    }
    return cut.rename(from, to);
  }
  uint32_t firstCluster(const char* p) override { return cut.firstCluster(p); }
};

}  // namespace faultfs
