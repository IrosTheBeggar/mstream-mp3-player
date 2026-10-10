// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueView.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "UiText.h"

namespace queueview {

// ---- DurationBook ----

DurationBook::DurationBook(AllocFn alloc, FreeFn release) : alloc_(alloc), free_(release) {}

DurationBook::~DurationBook() {
  if (s_) (free_ ? free_ : ::free)(s_);
}

bool DurationBook::reset(uint32_t tracks) {
  if (s_) (free_ ? free_ : ::free)(s_);
  s_ = nullptr;
  n_ = 0;
  ++version_;
  if (tracks == 0) return true;
  const size_t bytes = static_cast<size_t>(tracks) * sizeof(uint16_t);
  s_ = static_cast<uint16_t*>(alloc_ ? alloc_(bytes) : ::malloc(bytes));
  if (!s_) return false;
  std::memset(s_, 0, bytes);
  n_ = tracks;
  return true;
}

void DurationBook::note(uint32_t id, uint32_t ms) {
  if (id >= n_ || ms == 0) return;
  uint32_t s = (ms + 500) / 1000;
  if (s == 0) s = 1;
  if (s > 65535) s = 65535;
  if (s_[id] == s) return;
  s_[id] = static_cast<uint16_t>(s);
  ++version_;
}

Time upNextTime(const QueueModel& q, const DurationBook& book, uint32_t (*hintMs)(void* ctx, uint32_t id),
                void* ctx) {
  Time t;
  const int32_t c = q.current();
  if (c < 0) return t;
  for (uint32_t p = static_cast<uint32_t>(c) + 1; p < q.size(); ++p) {
    const uint32_t id = q.trackAt(p);
    uint32_t s = book.seconds(id);
    if (!s && hintMs) s = (hintMs(ctx, id) + 500) / 1000;
    if (s) {
      t.knownS += s;
    } else {
      ++t.unknown;
    }
  }
  return t;
}

namespace {
// " · 49 min" (or " · 2 h 5 min", "+" when only part is known) at buf + n.
void appendTime(const Time& t, char* buf, size_t bufSize, int n) {
  if (t.knownS == 0 || n < 0 || static_cast<size_t>(n) >= bufSize) return;
  const uint32_t mins = (t.knownS + 59) / 60;
  const char* more = t.unknown ? "+" : "";
  if (mins > 99) {
    snprintf(buf + n, bufSize - n, " \xC2\xB7 %lu h %lu%s min", static_cast<unsigned long>(mins / 60),
             static_cast<unsigned long>(mins % 60), more);
  } else {
    snprintf(buf + n, bufSize - n, " \xC2\xB7 %lu%s min", static_cast<unsigned long>(mins), more);
  }
}
}  // namespace

char* summary(uint32_t upNext, const Time& t, uint32_t position, uint32_t size, char* buf, size_t bufSize,
              bool withUpNext) {
  if (!buf || bufSize == 0) return buf;
  if (!withUpNext && position > 0) {
    const int n = snprintf(buf, bufSize, "%lu of %lu", static_cast<unsigned long>(position),
                           static_cast<unsigned long>(size));
    if (upNext > 0) appendTime(t, buf, bufSize, n);
    return buf;
  }
  int n = 0;
  if (position > 0) {
    n = snprintf(buf, bufSize, "%lu of %lu \xC2\xB7 ", static_cast<unsigned long>(position),
                 static_cast<unsigned long>(size));
    if (n < 0 || static_cast<size_t>(n) >= bufSize) return buf;
  }
  if (upNext == 0) {
    snprintf(buf + n, bufSize - n, "Nothing up next");
    return buf;
  }
  n += snprintf(buf + n, bufSize - n, "%lu up next", static_cast<unsigned long>(upNext));
  appendTime(t, buf, bufSize, n);
  return buf;
}

// ---- AddedMark ----

uint32_t AddedMark::firstPosition(const QueueModel& q) const {
  if (since_ == kNone) return kNone;
  for (uint32_t p = 0; p < q.size(); ++p) {
    if (marks(q.keyAt(p))) return p;
  }
  return kNone;
}

// ---- the queue's cap ----

static_assert(QueueModel::kMaxEntries == 5000, "uitext's cap texts say 5,000");

char* grouped(uint32_t n, char* buf, size_t size) {
  if (!buf || size == 0) return buf;
  char digits[12];
  const int len = snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(n));
  size_t w = 0;
  for (int i = 0; i < len && w + 1 < size; ++i) {
    if (i > 0 && (len - i) % 3 == 0) {
      buf[w++] = ',';
      if (w + 1 >= size) break;
    }
    buf[w++] = digits[i];
  }
  buf[w] = 0;
  return buf;
}

char* cappedText(Capped what, uint32_t took, uint32_t asked, char* buf, size_t size) {
  if (!buf || size == 0) return buf;
  char a[16], b[16];
  grouped(took, a, sizeof(a));
  grouped(asked, b, sizeof(b));
  const char* fmt = what == Capped::Shuffle ? uitext::kCapShuffling
                    : what == Capped::Play  ? uitext::kCapPlaying
                    : what == Capped::Add   ? uitext::kCapAdded
                                            : uitext::kCapNext;
  const int n = snprintf(buf, size, fmt, a, b);
  if (n > 0 && static_cast<size_t>(n) < size) snprintf(buf + n, size - n, ": %s", uitext::kCapWhy);
  return buf;
}

char* pushedText(Capped what, uint32_t took, uint32_t asked, uint32_t pushed, char* buf, size_t size) {
  if (!buf || size == 0) return buf;
  char a[16], b[16];
  grouped(took, a, sizeof(a));
  grouped(asked, b, sizeof(b));
  const bool next = what == Capped::Next;
  int n;
  if (took < asked) {
    n = snprintf(buf, size, next ? uitext::kCapNext : uitext::kCapAdded, a, b);
  } else if (took == 1) {
    n = snprintf(buf, size, "%s", next ? uitext::kPushNextOne : uitext::kPushAddedOne);
  } else {
    n = snprintf(buf, size, next ? uitext::kPushNextMany : uitext::kPushAddedMany, a);
  }
  if (n <= 0 || static_cast<size_t>(n) + 2 >= size) return buf;
  if (pushed == 1) {
    snprintf(buf + n, size - n, ": %s", uitext::kPushedOne);
  } else {
    char c[16], line[48];
    grouped(pushed, c, sizeof(c));
    snprintf(line, sizeof(line), uitext::kPushedMany, c);
    snprintf(buf + n, size - n, ": %s", line);
  }
  return buf;
}

AddOutcome addOutcome(const QueueModel& q, uint32_t sizeBefore, uint32_t asked, bool next, bool ok) {
  AddOutcome o;
  if (asked == 0) return o;  // (nothing asked: nothing done, the last undo as it was)
  if (!ok) {
    // Refused (room() 0: nothing changed), or out of memory with room to
    // spare.
    o.refused = q.room() == 0;
    return o;
  }
  // An add that pushes out always has its snapshot (QueueModel::insertAt()
  // refuses it otherwise), so its undo says how many went; one that pushed
  // nothing out says 0, or has no undo (no memory for it): 0 too.
  o.pushed = q.undoPushed();
  o.took = q.size() + o.pushed - sizeBefore;
  if (o.took == 0) return o;
  // Play next right after the current entry, + Queue at the end; into an
  // empty queue, from the first (it is current).
  o.first = sizeBefore == 0 ? 0 : next ? static_cast<uint32_t>(q.current()) + 1 : q.size() - o.took;
  return o;
}

// ---- KeyRing ----

void KeyRing::add(uint32_t key) {
  if (key == QueueModel::kNone || has(key)) return;
  k_[next_] = key;
  next_ = (next_ + 1) % kSize;
  if (n_ < kSize) ++n_;
}

bool KeyRing::has(uint32_t key) const {
  for (int i = 0; i < n_; ++i) {
    if (k_[i] == key) return true;
  }
  return false;
}

// ---- a refused add's note ----

ToastButtons keptByRefusal(bool up, bool undo, bool view, uint32_t viewKey, QueueModel::Edit undoable) {
  ToastButtons b;
  if (!up) return b;
  b.undo = undo && undoable != QueueModel::Edit::None;
  b.viewKey = view ? viewKey : QueueModel::kNone;
  return b;
}

// ---- UndoWatch ----

UndoWatch::Gone UndoWatch::pass(bool shuffled, bool undoToast, QueueModel::Edit undoable) {
  changed_ = shuffled != last_;
  last_ = shuffled;
  const bool undone = undone_;
  undone_ = false;  // (told once: a later pass's toast is another's)
  if (!undoToast || undoable != QueueModel::Edit::None) return Gone::Stays;
  if (undone) return Gone::Undone;
  return changed_ ? Gone::Toggle : Gone::Stays;
}

}  // namespace queueview
