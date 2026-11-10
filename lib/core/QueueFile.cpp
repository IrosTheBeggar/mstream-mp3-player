// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueFile.h"

#include <cstring>
#include <initializer_list>
#include <new>

namespace queuefile {

namespace {
constexpr char kMusic[] = "/music/";
constexpr size_t kMusicLen = sizeof(kMusic) - 1;
}  // namespace

void Lookup::begin() {
  tracks_.clear();
  settled_ = full_ = failed_ = false;
  next_ = 0;
  kept_ = unasked_ = gone_ = held_ = 0;
}

uint32_t Lookup::track(const char* path) {
  // (A built-in the firmware no longer has, a line no firmware wrote: no file to ask about.)
  if (std::strncmp(path, kMusic, kMusicLen) != 0) return TrackCatalog::kNone;
  if (settled_) {
    // The same lines in the same order. (Past what it holds: those it had no room for.)
    if (next_ >= tracks_.count()) return TrackCatalog::kNone;
    const uint32_t id = TrackCatalog::kUnlisted + next_++;
    return tracks_.path(id)[0] ? id : TrackCatalog::kNone;
  }
  // No room for one: nor for any after it, so the order holds for the read after settle().
  const uint32_t id = full_ ? TrackCatalog::kNone : tracks_.add(path);
  if (id == TrackCatalog::kNone) {
    full_ = true;
    ++held_;
  }
  return id;
}

bool Lookup::settle() {
  settled_ = true;
  next_ = 0;
  const uint32_t startMs = nowMs_ ? nowMs_() : 0;
  bool asking = true;
  for (uint32_t n = 0; n < tracks_.count(); ++n) {
    if (asking && nowMs_ && nowMs_() - startMs >= budgetMs_) asking = false;
    if (!asking) {
      ++unasked_;
      continue;
    }
    const uint32_t id = TrackCatalog::kUnlisted + n;
    switch (fs_.look(tracks_.path(id))) {
      case tagstore::Fs::Seen::There:
        ++kept_;
        break;
      case tagstore::Fs::Seen::Gone:
        ++gone_;
        tracks_.drop(id);
        break;
      case tagstore::Fs::Seen::Unknown:
        ++unasked_;
        asking = false;
        failed_ = true;
        break;
    }
  }
  return gone_ > 0;
}

ByteSource* FsFiles::open(const char* path, bool* cardFailed) {
  file_ = fs_.openRead(path, cardFailed);
  return file_ ? new (in_) tagstore::FileIn(*file_) : nullptr;
}

bool FsFiles::close() {
  if (!file_) return false;
  const bool failed = reinterpret_cast<tagstore::FileIn*>(in_)->failed();
  fs_.close(file_);
  file_ = nullptr;
  return failed;
}

Read readAtBoot(Files& files, const char* file, const char* temp, const TrackCatalog& catalog, QueueModel& q,
                int32_t (*pickCurrent)(const queuetext::Header& h, void* ctx), void* ctx, MemorySink::AllocFn alloc,
                MemorySink::FreeFn release, void (*wait)(uint32_t ms), Lookup* lookup) {
  Read out;
  const auto once = [&]() {
    bool unread = false;
    out.got = Got::None;
    out.notWhole = nullptr;
    ++out.reads;
    for (const char* path : {file, temp}) {
      bool cardFailed = false, failed = false, opened = false;
      if (lookup) lookup->begin();
      const queuetext::Restored r = readSettled(
          [&]() {
            ByteSource* in = files.open(path, &cardFailed);
            opened = opened || in;
            return in;
          },
          [&]() { failed = files.close(); },
          [&](ByteSource& in) { return queuetext::read(in, catalog, q, pickCurrent, ctx, alloc, release, lookup); },
          lookup);
      out.faulted = out.faulted || cardFailed || failed;
      out.noMemory = out.noMemory || r.noMemory;
      if (r.ok) {
        out.got = Got::Whole;
        out.path = path;
        out.read = r;
        return;
      }
      // (No memory says nothing of the file either: taken for a file that isn't whole,
      // the boot's default queue would go over it. Read again too: memory may come back.)
      if (cardFailed || failed || r.noMemory) {
        unread = true;
      } else if (opened) {
        out.notWhole = path;
      }
    }
    if (unread) out.got = Got::Unread;
  };
  once();
  for (const uint32_t ms : tagstore::kBootWaitsMs) {
    if (out.got != Got::Unread || !wait) break;
    wait(ms);
    once();
  }
  return out;
}

}  // namespace queuefile
