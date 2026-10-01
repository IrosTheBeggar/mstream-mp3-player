// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueText.h"

#include <cstdio>
#include <cstring>

namespace queuetext {

namespace {

constexpr char kMagic[] = "mstream-queue 1 ";

bool writeHeader(const QueueModel& q, uint32_t generation, ByteSink& out) {
  char line[64];
  const int n = std::snprintf(line, sizeof(line), "%s%lu %ld %lu\n", kMagic, static_cast<unsigned long>(q.size()),
                              static_cast<long>(q.current()), static_cast<unsigned long>(generation));
  return n > 0 && static_cast<size_t>(n) < sizeof(line) && out.write(line, static_cast<size_t>(n));
}

// One entry: its path, or an empty line for a track the catalog doesn't
// know (the line count stays the entry count, so `current` still points
// at the right line).
bool writeEntry(const QueueModel& q, const TrackCatalog& catalog, uint32_t pos, ByteSink& out) {
  char path[TrackCatalog::kMaxPath + 1];
  size_t n = catalog.path(q.trackAt(pos), path, TrackCatalog::kMaxPath);
  path[n++] = '\n';
  return out.write(path, n);
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
  long entries = 0, current = 0, generation = 0;
  if (!parseNumber(s, &entries) || *s++ != ' ' || !parseNumber(s, &current) || *s++ != ' ' ||
      !parseNumber(s, &generation) || *s != 0 || entries < 0 || generation < 0) {
    return false;
  }
  h->entries = static_cast<uint32_t>(entries);
  h->current = static_cast<int32_t>(current);
  h->generation = static_cast<uint32_t>(generation);
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
  char line_[TrackCatalog::kMaxPath + 2];
  size_t at_ = 0, have_ = 0;
};

}  // namespace

void Writer::begin(const QueueModel& q, uint32_t generation) {
  version_ = q.contentVersion();
  generation_ = generation;
  next_ = 0;
  headerDone_ = false;
}

Writer::Step Writer::step(const QueueModel& q, const TrackCatalog& catalog, ByteSink& out, uint32_t maxLines) {
  if (q.contentVersion() != version_) return Step::Changed;
  if (!headerDone_ && maxLines) {
    if (!writeHeader(q, generation_, out)) return Step::Failed;
    headerDone_ = true;
    --maxLines;
  }
  for (; maxLines && next_ < q.size(); --maxLines, ++next_) {
    if (!writeEntry(q, catalog, next_, out)) return Step::Failed;
  }
  return headerDone_ && next_ >= q.size() ? Step::Done : Step::More;
}

bool write(const QueueModel& q, const TrackCatalog& catalog, uint32_t generation, ByteSink& out) {
  if (!writeHeader(q, generation, out)) return false;
  for (uint32_t i = 0; i < q.size(); ++i) {
    if (!writeEntry(q, catalog, i, out)) return false;
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

  MemorySink ids(alloc, release);  // the tracks that survive, 4 bytes each
  uint32_t count = 0;
  int32_t current = -1;
  while (lines.next(&line, &overflow)) {
    const uint32_t id = overflow || !*line ? TrackCatalog::kNone : catalog.find(line);
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
    if (!ids.write(&id, sizeof(id))) return r;
    ++count;
  }
  if (r.lines != r.header.entries) return r;  // cut short, or not what it says: leave the queue alone
  if (current >= static_cast<int32_t>(count)) current = static_cast<int32_t>(count) - 1;
  if (!q.assign(reinterpret_cast<const uint32_t*>(ids.data()), count, current)) return r;
  r.entries = count;
  r.ok = true;
  return r;
}

}  // namespace queuetext
