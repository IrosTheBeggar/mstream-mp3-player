// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueView.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "LibraryText.h"
#include "UiStrings.h"

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
using uistr::S;

// " · 49 min" (or " · 2 h 5 min", "+" when only part is known) at buf + n.
void appendTime(const Time& t, char* buf, size_t bufSize, int n) {
  if (t.knownS == 0 || n < 0 || static_cast<size_t>(n) >= bufSize) return;
  const uint32_t mins = (t.knownS + 59) / 60;
  const char* more = t.unknown ? "+" : "";
  char m[16], time[48];
  if (mins > 99) {
    snprintf(m, sizeof(m), "%lu%s", static_cast<unsigned long>(mins % 60), more);
    trf(time, sizeof(time), S::format_hours_minutes, mins / 60, m);
  } else {
    snprintf(m, sizeof(m), "%lu%s", static_cast<unsigned long>(mins), more);
    trf(time, sizeof(time), S::format_minutes, m);
  }
  snprintf(buf + n, bufSize - n, "%s%s", librarytext::kDot, time);
}
}  // namespace

char* summary(uint32_t upNext, const Time& t, uint32_t position, uint32_t size, char* buf, size_t bufSize,
              bool withUpNext) {
  if (!buf || bufSize == 0) return buf;
  if (!withUpNext && position > 0) {
    const int n = static_cast<int>(trf(buf, bufSize, S::queue_summary_position, position, size));
    if (upNext > 0) appendTime(t, buf, bufSize, n);
    return buf;
  }
  size_t n = 0;
  if (position > 0) {
    n = trf(buf, bufSize, S::queue_summary_position, position, size);
    n += uistr::format(buf + n, bufSize - n, librarytext::kDot);
    if (n + 1 >= bufSize) return buf;
  }
  if (upNext == 0) {
    trf(buf + n, bufSize - n, S::queue_summary_nothing_next);
    return buf;
  }
  n += trnf(buf + n, bufSize - n, S::queue_summary_up_next, upNext, upNext);
  appendTime(t, buf, bufSize, static_cast<int>(n));
  return buf;
}

int pickSummary(const int (&widths)[kSummaryForms], int room) {
  // The count and the time (the last form) only with no position: where
  // the queue is comes first.
  const int forms = widths[kSummaryPosition] >= 0 ? kSummaryShort : kSummaryForms;
  for (int k = 0; k < forms; ++k) {
    if (widths[k] >= 0 && widths[k] <= room) return k;
  }
  return -1;
}

SummaryPick placeSummary(const int (&widths)[kSummaryForms], int beside, int below) {
  SummaryPick p;
  // Beside the title while where the queue is fits there, else under it
  // (a count alone that fits beside, "noch 20", would hide it; pickSummary()
  // doesn't try it while there is a position).
  const int position = widths[kSummaryPosition];
  if (position < 0 || position <= beside) p.form = pickSummary(widths, beside);
  if (p.form < 0) {
    p.form = pickSummary(widths, below);
    p.below = p.form >= 0;
  }
  return p;
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

namespace {
// "<what>: <why>" (toast.what_why): the toast splits there onto two lines.
char* whatWhy(const char* what, const char* why, char* buf, size_t size) {
  trf(buf, size, S::toast_what_why, what, why);
  return buf;
}
}  // namespace

char* cappedText(Capped what, uint32_t took, uint32_t asked, char* buf, size_t size) {
  if (!buf || size == 0) return buf;
  const S id = what == Capped::Shuffle ? S::queue_cap_shuffling
               : what == Capped::Play  ? S::queue_cap_playing
               : what == Capped::Add   ? S::queue_cap_added
                                       : S::queue_cap_next;
  char head[64], why[64];
  trf(head, sizeof(head), id, uistr::grouped(took), uistr::grouped(asked));
  trnf(why, sizeof(why), S::queue_cap_why, QueueModel::kMaxEntries, uistr::grouped(QueueModel::kMaxEntries));
  return whatWhy(head, why, buf, size);
}

char* pushedText(Capped what, uint32_t took, uint32_t asked, uint32_t pushed, char* buf, size_t size) {
  if (!buf || size == 0) return buf;
  const bool next = what == Capped::Next;
  char head[64], line[64];
  if (took < asked) {
    trf(head, sizeof(head), next ? S::queue_cap_next : S::queue_cap_added, uistr::grouped(took),
        uistr::grouped(asked));
  } else {
    trnf(head, sizeof(head), next ? S::queue_cap_push_next : S::queue_cap_push_added, took, uistr::grouped(took));
  }
  trnf(line, sizeof(line), S::queue_cap_pushed, pushed, uistr::grouped(pushed));
  return whatWhy(head, line, buf, size);
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
  const bool undone = undone_, dropped = dropped_;
  undone_ = dropped_ = false;  // (told once: a later pass's toast is another's)
  if (!undoToast || undoable != QueueModel::Edit::None) return Gone::Stays;
  if (undone) return Gone::Undone;
  if (dropped) return Gone::Dropped;
  return changed_ ? Gone::Toggle : Gone::Stays;
}

}  // namespace queueview
