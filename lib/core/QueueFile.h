// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ByteStream.h"
#include "QueueModel.h"
#include "QueueText.h"
#include "TagStore.h"
#include "TrackCatalog.h"

// The queue's file against a card that may fail a read and a library that may leave
// files out (docs/METADATA.md 3.2.2). Nothing rebuilds queue.txt, so two rules:
//
// - A read the card fails isn't a file that's gone or isn't one (readAtBoot()): Unread,
//   read again by the boot's waits, and nothing goes over a file nobody could read.
// - A line leaves the file only when the card says its file is gone (Lookup). A library
//   can leave files out with no fault known: /music walked through a VFS that can't tell
//   a folder that failed to read from an empty one, or T alone with D absent or failing
//   its checks, and the next boot loads the same library as whole. So the library's word
//   isn't enough, at any boot. A line it doesn't list stays in the queue, by its path
//   (TrackCatalog::Unlisted), unless the card itself says the file isn't there.
//
// That keeps the file the queue: its lines are the queue's positions, a resume point
// pairs with them, an edit or an Auto DJ pick writes them all, and the update step reads
// back from the line that plays. (QueueSaver::keptFile() has none of that, so it stays
// for what can't take this rule: no library at all, the session's walk, no room here.)
//
// Portable, host-tested (test_card_root, test_queue). Loop task only.
namespace queuefile {

// Questions of the card for one read of the file, at most for this long: at the boot
// nothing else is held up; at the update step's read back the loop is.
constexpr uint32_t kBootLookupMs = 5000;
constexpr uint32_t kRemapLookupMs = 500;

// What queuetext::read() asks about a line the catalog doesn't know. The read takes each
// such line of the card's music by its path, and settle(), with the file closed, asks the
// card about them: only if it says some are gone is the file read again, to leave those
// out. (A library that lists every line asks nothing, and reads once.)
class Lookup final : public queuetext::UnknownLines {
public:
  Lookup(tagstore::Fs& fs, TrackCatalog::Unlisted& tracks) : fs_(fs), tracks_(tracks) {}

  // How long settle() may ask: `budgetMs` of `nowMs()` (nullptr: no limit).
  void limit(uint32_t (*nowMs)(), uint32_t budgetMs) {
    nowMs_ = nowMs;
    budgetMs_ = budgetMs;
  }
  // Before a read of the file: the tracks of the last one forgotten, the counts 0.
  void begin();

  uint32_t track(const char* path) override;
  uint32_t held() const override { return held_; }
  // Asks about each line. Once a question fails, or the time is up, the card is asked no
  // more: every line left stays by its path, and the next read asks again. True: some
  // are gone.
  bool settle() override;

  uint32_t kept() const { return kept_; }        // by their paths: the card has them
  uint32_t unasked() const { return unasked_; }  // by their paths: it wasn't asked, or didn't say
  uint32_t gone() const { return gone_; }
  // A question met a fault: such a read with no answer makes FatFs's next call a
  // remount, which is better now than under a card job (FatFs::echoNow()).
  bool failed() const { return failed_; }

private:
  tagstore::Fs& fs_;
  TrackCatalog::Unlisted& tracks_;
  uint32_t (*nowMs_)() = nullptr;
  uint32_t budgetMs_ = 0;
  bool settled_ = false, full_ = false, failed_ = false;
  uint32_t next_ = 0;  // the read after settle(): which line by its path comes next
  uint32_t kept_ = 0, unasked_ = 0, gone_ = 0, held_ = 0;
};

// queuetext::read() of what `open` opens and `close` closes, with `unknown` (optional)
// settled in between and the file read again if it says so. (A read again that fails
// keeps the first: its lines the card says are gone only don't play, and the next read
// asks again.) What `open` gives nullptr for: Restored's not ok.
template <typename Open, typename Close, typename Read>
queuetext::Restored readSettled(Open open, Close close, Read read, queuetext::UnknownLines* unknown) {
  queuetext::Restored r;
  ByteSource* in = open();
  if (!in) return r;
  r = read(*in);
  close();
  if (r.ok && unknown && unknown->settle() && (in = open()) != nullptr) {
    const queuetext::Restored again = read(*in);
    close();
    if (again.ok) r = again;
  }
  return r;
}

// What a read of the queue's files needs of the storage, which may not say much
// (the flash's VFS: no reason for a failed open, a short read for a failed read).
class Files {
public:
  // `path` open for reading; nullptr: it can't be. `cardFailed` true: the card failed
  // (a read, the volume's lock, no memory for the file), and nothing is known of it;
  // false: it isn't there, or the file system's records of it are damaged.
  virtual ByteSource* open(const char* path, bool* cardFailed) = 0;
  // What was open closed. True: its stream ended on a read the card failed.
  virtual bool close() = 0;

protected:
  ~Files() = default;
};

// tagstore::Fs as Files: FatFs's answer on the card.
class FsFiles final : public Files {
public:
  explicit FsFiles(tagstore::Fs& fs) : fs_(fs) {}
  ByteSource* open(const char* path, bool* cardFailed) override;
  bool close() override;

private:
  tagstore::Fs& fs_;
  tagstore::File* file_ = nullptr;
  // (Built in place: a stream per open, with the file's reference.)
  alignas(tagstore::FileIn) unsigned char in_[sizeof(tagstore::FileIn)];
};

enum class Got : uint8_t {
  Whole,   // a whole queue file, read into the queue
  None,    // none: not there, or not a whole queue file (its own fault: cut, damaged)
  Unread,  // the card failed a read of it, and nothing whole took its place
};

struct Read {
  Got got = Got::None;
  const char* path = nullptr;  // Whole: which file
  queuetext::Restored read;    // Whole: what the read found
  uint32_t reads = 0;          // how many times the files were read
  const char* notWhole = nullptr;  // a file there that wasn't a whole queue file, of its own fault (the last read's)
  // A read failed on the way, whatever it came to (one with no answer makes FatFs's next
  // call a remount, which is better now than under a card job: FatFs::echoNow()).
  bool faulted = false;
};

// `file`, else `temp` (only ever whole when power went between removing `file` and the
// rename, or after its flush: the newest, as it is when `file` can't be read), through
// queuetext::read(). Read again after kBootWaitsMs while it comes to Unread and `wait`
// is given (nullptr: one read. A card still failing after an earlier reader's tries
// won't be back for this one). `lookup` (optional) begins again at each read, and asks
// once the file is read whole. Nothing changes in `q` unless it comes to Whole.
Read readAtBoot(Files& files, const char* file, const char* temp, const TrackCatalog& catalog, QueueModel& q,
                int32_t (*pickCurrent)(const queuetext::Header& h, void* ctx), void* ctx, MemorySink::AllocFn alloc,
                MemorySink::FreeFn release, void (*wait)(uint32_t ms), Lookup* lookup);

}  // namespace queuefile
