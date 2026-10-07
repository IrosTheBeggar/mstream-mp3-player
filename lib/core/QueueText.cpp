// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueText.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace queuetext {

namespace {

constexpr char kMagic[] = "mstream-queue ";
// A version 2 line's rank and its space, at the longest ("4294967295 ").
constexpr size_t kRankRoom = 11;

// Version 2 when shuffled (the header's version is the file's mode).
bool writeHeader(const QueueModel& q, uint32_t generation, ByteSink& out) {
  char line[64];
  const int n = std::snprintf(line, sizeof(line), "%s%d %lu %ld %lu\n", kMagic, q.shuffled() ? 2 : 1,
                              static_cast<unsigned long>(q.size()), static_cast<long>(q.current()),
                              static_cast<unsigned long>(generation));
  return n > 0 && static_cast<size_t>(n) < sizeof(line) && out.write(line, static_cast<size_t>(n));
}

// One entry: its path (shuffled: after its rank and a space), or nothing
// for a track the catalog doesn't know (the line count stays the entry
// count, so `current` still points at the right line). `shuffled`: the
// header's version.
bool writeEntry(const QueueModel& q, const TrackCatalog& catalog, uint32_t pos, bool shuffled, ByteSink& out) {
  char line[kRankRoom + TrackCatalog::kMaxPath + 1];
  size_t at = 0;
  if (shuffled) {
    const int r = std::snprintf(line, kRankRoom + 1, "%lu ", static_cast<unsigned long>(q.rankAt(pos)));
    if (r <= 0 || static_cast<size_t>(r) > kRankRoom) return false;
    at = static_cast<size_t>(r);
  }
  size_t n = at + catalog.path(q.trackAt(pos), line + at, TrackCatalog::kMaxPath);
  line[n++] = '\n';
  return out.write(line, n);
}

// A version 2 line's "<rank> ": the rank, and s moved past its space;
// false if it isn't one (no digits, no space, or past 4294967295).
bool parseRank(const char*& s, uint32_t* rank) {
  if (*s < '0' || *s > '9') return false;
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + static_cast<uint64_t>(*s++ - '0');
    if (v > 0xFFFFFFFFull) return false;
  }
  if (*s++ != ' ') return false;
  *rank = static_cast<uint32_t>(v);
  return true;
}

// Parses "123" at s, moving s past it; false if there's no number.
bool parseNumber(const char*& s, long* value) {
  bool negative = false;
  if (*s == '-') {
    negative = true;
    ++s;
  }
  if (*s < '0' || *s > '9') return false;
  long v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  *value = negative ? -v : v;
  return true;
}

bool parseHeader(const char* line, Header* h) {
  const size_t magicLen = sizeof(kMagic) - 1;
  if (std::strncmp(line, kMagic, magicLen) != 0) return false;
  const char* s = line + magicLen;
  // Version 1, or 2 (shuffled); nothing else.
  if ((s[0] != '1' && s[0] != '2') || s[1] != ' ') return false;
  const bool shuffled = s[0] == '2';
  s += 2;
  long entries = 0, current = 0, generation = 0;
  if (!parseNumber(s, &entries) || *s++ != ' ' || !parseNumber(s, &current) || *s++ != ' ' ||
      !parseNumber(s, &generation) || *s != 0 || entries < 0 || generation < 0) {
    return false;
  }
  h->entries = static_cast<uint32_t>(entries);
  h->current = static_cast<int32_t>(current);
  h->generation = static_cast<uint32_t>(generation);
  h->shuffled = shuffled;
  return true;
}

// Lines from a source, through a small buffer. A line longer than the
// buffer comes back as `overflow` (its text cut), so it matches nothing.
class LineReader {
public:
  explicit LineReader(ByteSource& in) : in_(in) {}

  // False at the end. `line` is NUL-terminated, without "\r\n".
  bool next(char** line, bool* overflow) {
    size_t n = 0;
    bool any = false;
    *overflow = false;
    for (;;) {
      if (at_ == have_) {
        have_ = in_.read(chunk_, sizeof(chunk_));
        at_ = 0;
        if (have_ == 0) break;
      }
      any = true;
      const char c = chunk_[at_++];
      if (c == '\n') break;
      if (n + 1 < sizeof(line_)) {
        line_[n++] = c;
      } else {
        *overflow = true;
      }
    }
    if (!any) return false;
    if (n && line_[n - 1] == '\r') --n;
    line_[n] = 0;
    *line = line_;
    return true;
  }

private:
  ByteSource& in_;
  char chunk_[256];
  char line_[kRankRoom + TrackCatalog::kMaxPath + 2];  // (version 2: the rank first)
  size_t at_ = 0, have_ = 0;
};

// A block of exactly `n` words from the hooks, given back at the end of
// the read. The header says how many lines follow, so the read's ids (and,
// shuffled, ranks) never grow by doubling: a 20,000-line file takes 80 KB
// (160 KB shuffled) next to the queue's own 240 KB, not twice that
// (docs/METADATA.md section 3.4.2, step 5). A header that claims more
// than memory holds fails here, as its file would at the line count.
class Words {
public:
  Words(MemorySink::AllocFn alloc, MemorySink::FreeFn release)
      : alloc_(alloc ? alloc : heapAlloc), free_(release ? release : heapFree) {}
  ~Words() {
    if (data_) free_(data_);
  }
  Words(const Words&) = delete;
  Words& operator=(const Words&) = delete;

  bool reserve(uint32_t n) {
    if (n == 0) return true;
    if (n > SIZE_MAX / sizeof(uint32_t)) return false;
    data_ = static_cast<uint32_t*>(alloc_(static_cast<size_t>(n) * sizeof(uint32_t)));
    cap_ = data_ ? n : 0;
    return data_ != nullptr;
  }
  // False past the block's end (more lines than the header said).
  bool push(uint32_t w) {
    if (size_ >= cap_) return false;
    data_[size_++] = w;
    return true;
  }
  const uint32_t* data() const { return data_; }

private:
  static void* heapAlloc(size_t n) { return std::malloc(n); }
  static void heapFree(void* p) { std::free(p); }

  MemorySink::AllocFn alloc_;
  MemorySink::FreeFn free_;
  uint32_t* data_ = nullptr;
  uint32_t cap_ = 0, size_ = 0;
};

}  // namespace

void Writer::begin(const QueueModel& q, uint32_t generation) {
  version_ = q.contentVersion();
  generation_ = generation;
  next_ = 0;
  headerDone_ = false;
}

Writer::Step Writer::step(const QueueModel& q, const TrackCatalog& catalog, ByteSink& out, uint32_t maxLines) {
  // (A toggle is a content change: the version the header said can't
  // change under the lines.)
  if (q.contentVersion() != version_) return Step::Changed;
  if (!headerDone_ && maxLines) {
    if (!writeHeader(q, generation_, out)) return Step::Failed;
    headerDone_ = true;
    --maxLines;
  }
  for (; maxLines && next_ < q.size(); --maxLines, ++next_) {
    if (!writeEntry(q, catalog, next_, q.shuffled(), out)) return Step::Failed;
  }
  return headerDone_ && next_ >= q.size() ? Step::Done : Step::More;
}

bool write(const QueueModel& q, const TrackCatalog& catalog, uint32_t generation, ByteSink& out) {
  if (!writeHeader(q, generation, out)) return false;
  for (uint32_t i = 0; i < q.size(); ++i) {
    if (!writeEntry(q, catalog, i, q.shuffled(), out)) return false;
  }
  return true;
}

Restored read(ByteSource& in, const TrackCatalog& catalog, QueueModel& q, int32_t (*pickCurrent)(const Header&, void*),
              void* ctx, MemorySink::AllocFn alloc, MemorySink::FreeFn release) {
  Restored r;
  LineReader lines(in);
  char* line = nullptr;
  bool overflow = false;
  if (!lines.next(&line, &overflow) || overflow || !parseHeader(line, &r.header)) return r;
  const int32_t target = pickCurrent ? pickCurrent(r.header, ctx) : r.header.current;

  const bool shuffled = r.header.shuffled;
  Words ids(alloc, release);    // the tracks that survive
  Words ranks(alloc, release);  // version 2: their ranks
  if (!ids.reserve(r.header.entries) || (shuffled && !ranks.reserve(r.header.entries))) return r;
  uint32_t count = 0;
  int32_t current = -1;
  while (lines.next(&line, &overflow)) {
    const char* path = line;
    uint32_t rank = 0;
    // A version 2 line that isn't "<rank> ...": not a whole file (a long
    // path's line, cut, still starts with its rank).
    if (shuffled && !parseRank(path, &rank)) return r;
    // More lines than the header said: not what it says (the queue left
    // alone, as for fewer).
    if (r.lines == r.header.entries) return r;
    const uint32_t id = overflow || !*path ? TrackCatalog::kNone : catalog.find(path);
    const bool isTarget = static_cast<int32_t>(r.lines) == target;
    ++r.lines;
    if (id == TrackCatalog::kNone) {
      ++r.dropped;
      if (isTarget) current = static_cast<int32_t>(count);  // the next survivor, if any
      continue;
    }
    if (isTarget) {
      current = static_cast<int32_t>(count);
      r.currentKept = true;
    }
    // (Room for every line the header counts, so these can't fail.)
    if (!ids.push(id)) return r;
    if (shuffled && !ranks.push(rank)) return r;  // (dropped tracks leave gaps: fine)
    ++count;
  }
  if (r.lines != r.header.entries) return r;  // cut short: leave the queue alone
  if (current >= static_cast<int32_t>(count)) current = static_cast<int32_t>(count) - 1;
  if (!q.assign(ids.data(), count, current, shuffled, shuffled ? ranks.data() : nullptr)) return r;
  r.entries = count;
  r.ok = true;
  return r;
}

}  // namespace queuetext
