// The queue (a first page for the framework; the spec's §6.4 is the full
// screen, with the usability fixes): every entry, the playing one tinted
// with the section's colour, the ones already played dimmed. It opens on the
// playing track, and a tap on the Queue tab again goes back to it.
//
//   - A tap on a row opens its inline bar: Play / Play next / Remove (the
//     usability walk: a tap that played at once surprised first-time users,
//     and removing one track took edit mode).
//   - A long press (or "Select") is selection mode: checkboxes, taps
//     toggle, "Remove" in the header (away from the touch buttons), "✕"
//     leaves it.
//   - A tap on the header's title goes to the playing track, the top, the
//     end, in turn (the review's graft).
//   - Every edit shows a toast with Undo.
//   - Coming back to a queue that changed meanwhile (Play on an album
//     replaced it, + Queue added to it) starts at the playing track with no
//     row open: the remembered ones were positions in the old queue.
#include <cstdio>
#include <cstring>

#include "TrackCatalog.h"
#include "app/Psram.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

namespace {
const char* const kActions[3] = {"Play", "Play next", "Remove"};
}  // namespace

QueuePage::QueuePage(Ui& ui) : Page(ui), selection_(psramAlloc, psramFree) {}

void QueuePage::enter(NavModel::PageRef& ref) {
  ref_ = &ref;
  const AppState& s = ui_.state();
  if ((left_ && s.contentVersion != leftContent_) || showPos_ >= 0) {
    ref.scrollPx = -1;
    ref.expanded = -1;
  }
  content_ = s.contentVersion;
  position_ = s.positionVersion;
  selecting_ = false;
  touchInList_ = false;
  headerPressed_ = 0;
  headerSig_ = 0;
  jump_ = 0;
  repaintHeader();
  // The first time (or after "Show in the queue"), the playing track on
  // top; "View" from a toast: the added entry, one row down (what's before
  // it shows where it went).
  int32_t top = s.current > 0 ? s.current * kRowH : 0;
  if (showPos_ >= 0) top = showPos_ > 0 ? (showPos_ - 1) * kRowH : 0;
  showPos_ = -1;
  ui_.list().attach(this, &ref, top);
}

void QueuePage::leave() {
  if (selecting_) {
    selecting_ = false;  // leaving the Queue ends selection mode
    selection_.resize(0);
  }
  ui_.list().detach();
  left_ = true;
  leftContent_ = ui_.state().contentVersion;
}

void QueuePage::repaint() {
  repaintHeader();
  ui_.list().invalidate();
}

Header QueuePage::header() const {
  static char title[32];
  static char sub[48];
  Header h;
  h.title = title;
  h.sub = sub;
  sub[0] = 0;
  const AppState& s = ui_.state();
  if (selecting_) {
    h.cross = true;
    snprintf(title, sizeof(title), "%lu selected", static_cast<unsigned long>(selection_.count()));
    snprintf(sub, sizeof(sub), "of %lu", static_cast<unsigned long>(s.queueSize));
    if (selection_.count() > 0) {
      h.right = "Remove";
      h.rightDanger = true;
    }
  } else {
    snprintf(title, sizeof(title), "Queue");
    if (s.queueSize == 0) {
      snprintf(sub, sizeof(sub), "empty");
    } else if (s.current >= 0) {
      snprintf(sub, sizeof(sub), "%ld of %lu, %lu next", static_cast<long>(s.current + 1),
               static_cast<unsigned long>(s.queueSize), static_cast<unsigned long>(s.upNext));
    }
    if (s.queueSize > 0) h.right = "Select";
  }
  h.pressed = headerPressed_ == 2;
  return h;
}

void QueuePage::repaintHeader() {
  ui_.drawHeader(header());
  const AppState& s = ui_.state();
  headerSig_ = (static_cast<uint32_t>(s.current + 1) * 2654435761u) ^ (s.queueSize * 40503u) ^
               (selecting_ ? selection_.count() * 97u + 1u : 0u) ^ (static_cast<uint32_t>(headerPressed_) << 29);
}

bool QueuePage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  const AppState& s = ui_.state();
  ListView& list = ui_.list();
  if (s.contentVersion != content_) {
    // Entries came or went: positions moved, so the selection starts over,
    // and an open row closes (it would be under another track now).
    content_ = s.contentVersion;
    position_ = s.positionVersion;
    if (selecting_) selection_.resize(s.queueSize);
    if (selecting_ && s.queueSize == 0) selecting_ = false;
    list.collapse();
    list.reload();
  } else if (s.positionVersion != position_) {
    position_ = s.positionVersion;  // another track plays: its row and the last one change
    list.refreshAll();
  }
  const uint32_t sig = (static_cast<uint32_t>(s.current + 1) * 2654435761u) ^ (s.queueSize * 40503u) ^
                       (selecting_ ? selection_.count() * 97u + 1u : 0u) ^ (static_cast<uint32_t>(headerPressed_) << 29);
  if (sig != headerSig_) repaintHeader();
  return list.update(nowMs, frameDue, wholeRows);
}

bool QueuePage::animating() const { return ui_.list().animating(); }

void QueuePage::home() {
  ListView& list = ui_.list();
  list.collapse();
  const int32_t c = ui_.state().current;
  list.scrollToRow(c > 0 ? static_cast<uint32_t>(c) : 0);
  jump_ = 1;  // a title tap next goes to the top
}

void QueuePage::jump() {
  ListView& list = ui_.list();
  const uint32_t n = ui_.queue().size();
  if (n == 0) return;
  list.collapse();
  const int32_t c = ui_.state().current;
  switch (jump_) {
    case 0: list.scrollToRow(c > 0 ? static_cast<uint32_t>(c) : 0); break;
    case 1: list.scrollToRow(0); break;
    default: list.scrollToRow(n - 1); break;  // clamped: the last screenful
  }
  static const char* const kWhere[3] = {"the playing track", "the top", "the end"};
  Serial.printf("[ui] queue: to %s\n", kWhere[jump_]);
  jump_ = static_cast<uint8_t>((jump_ + 1) % 3);
}

void QueuePage::setSelecting(bool on) {
  selecting_ = on;
  if (on) {
    selection_.resize(ui_.state().queueSize);
    ui_.list().collapse();
  } else {
    selection_.resize(0);
  }
  ui_.list().refreshAll();
  repaintHeader();
}

void QueuePage::removeSelected() {
  const uint32_t n = selection_.count();
  if (n == 0) return;
  auto* positions = static_cast<uint32_t*>(psramAlloc(n * sizeof(uint32_t)));
  if (!positions) {
    ui_.toast("Not enough memory for that", false);
    return;
  }
  const uint32_t got = selection_.list(positions, n);
  const QueueModel::Removed r = ui_.player().remove(positions, got);
  psramFree(positions);
  setSelecting(false);
  char text[48];
  snprintf(text, sizeof(text), "Removed %lu track%s", static_cast<unsigned long>(r.count), r.count == 1 ? "" : "s");
  ui_.toast(text, true);
}

void QueuePage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    touchInList_ = e.y >= ListView::kTop;
    if (!touchInList_) {
      headerPressed_ = header().hit(e);
      if (headerPressed_ == 2) repaintHeader();
    }
  }
  if (touchInList_) {
    ui_.list().onEvent(e);
    return;
  }
  if (e.type == T::Tap) {
    const int hit = headerPressed_;
    headerPressed_ = 0;
    if (hit == 1 && selecting_) {
      ui_.tick();
      setSelecting(false);
    } else if (hit == 2) {
      ui_.tick();
      if (selecting_) {
        removeSelected();
      } else {
        setSelecting(true);
      }
    } else if (hit == 3 && !selecting_) {
      ui_.tick();
      jump();
    }
    repaintHeader();
  } else if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
    if (headerPressed_) {
      headerPressed_ = 0;
      repaintHeader();
    }
  }
}

void QueuePage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  snprintf(buf, size, "Queue: %lu entries, current %ld%s", static_cast<unsigned long>(s.queueSize),
           static_cast<long>(s.current), selecting_ ? ", selecting" : "");
}

// ---- the list ----

uint32_t QueuePage::rows() { return ui_.queue().size(); }

uint16_t QueuePage::accent() { return accent::Queue; }

void QueuePage::drawRow(ListView::Row& r) {
  const QueueModel& q = ui_.queue();
  const TrackCatalog& cat = ui_.player().catalog();
  const int32_t current = q.current();
  const bool isCurrent = static_cast<int32_t>(r.row) == current;
  const bool played = current >= 0 && static_cast<int32_t>(r.row) < current;
  if (isCurrent && r.bg == col::BG) {
    r.bg = col::ROW_SEL;
    r.c.fillSprite(r.bg);
  }
  if (isCurrent) r.c.fillRect(0, 0, 3, kRowH, accent::Queue);
  const uint32_t track = q.trackAt(r.row);
  const int x = isCurrent ? ListView::playing(r, accent::Queue)
                          : ListView::number(r, r.row + 1, played ? col::FAINT : col::DIM);
  char title[128];
  if (!cat.title(track, title, sizeof(title))) snprintf(title, sizeof(title), "(not in the library)");
  const char* artist = cat.artist(track);
  const uint16_t colour = isCurrent ? accent::Queue : played ? col::DIM : col::TXT;
  ListView::lines(r, x, r.right - 8, title, strlen(title), artist, strlen(artist), colour,
                  isCurrent ? Font::Bold : Font::Body);
}

int QueuePage::actions(int32_t row, const char* labels[3]) {
  (void)row;
  for (int k = 0; k < 3; ++k) labels[k] = kActions[k];
  return 3;
}

void QueuePage::onAction(int32_t row, int action) {
  if (row < 0 || static_cast<uint32_t>(row) >= ui_.queue().size()) return;
  auto pos = static_cast<uint32_t>(row);
  PlaybackController& p = ui_.player();
  char title[96];
  if (!p.catalog().title(ui_.queue().trackAt(pos), title, sizeof(title))) snprintf(title, sizeof(title), "it");
  char text[128];
  ui_.list().collapse();
  switch (action) {
    case 0:
      p.play(pos);
      break;
    case 1: {
      // The playing entry can't go after itself: moveNext() changes nothing
      // (and saves no undo), so the toast mustn't offer one.
      const uint32_t before = ui_.queue().contentVersion();
      if (!p.moveNext(&pos, 1)) {
        ui_.toast("Not enough memory for that", false);
      } else if (ui_.queue().contentVersion() == before) {
        snprintf(text, sizeof(text), "Playing now: %s", title);
        ui_.toast(text, false);
      } else {
        snprintf(text, sizeof(text), "Plays next: %s", title);
        ui_.toast(text, true);
      }
      break;
    }
    default: {
      const QueueModel::Removed r = p.remove(&pos, 1);
      if (r.count) {
        snprintf(text, sizeof(text), "Removed %s", title);
        ui_.toast(text, true);
      }
      break;
    }
  }
}

ListView::Tap QueuePage::onTap(uint32_t row) {
  if (selecting_) {
    selection_.toggle(row);
    return ListView::Tap::Handled;
  }
  return ListView::Tap::Expand;
}

void QueuePage::onHold(uint32_t row) {
  if (selecting_) return;
  setSelecting(true);
  selection_.set(row, true);
  ui_.list().refreshAll();
  repaintHeader();
}

}  // namespace ui
