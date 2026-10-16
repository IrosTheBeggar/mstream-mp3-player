// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Queue (the tab bar spec §6.4, mockups 16-18, with the review's grafts
// and the usability fixes; the page's shape is in Pages.h):
//
//   - It opens on the playing track (tinted, on top), and a tap on the
//     Queue tab again goes back to it; a tap on the header's title goes to
//     the playing track, the top, the end, in turn (the review's graft).
//   - The header says what's left: "4 of 16 · 12 up next · 49 min" (the
//     lengths are learned as tracks play: "49+ min" while some aren't),
//     beside the title, or under it where a language's title leaves it
//     too little.
//   - A tap on a row opens its bar: Play now / Play next / Remove (the
//     usability walk: a tap that played at once surprised first-time
//     users, and removing one track took edit mode).
//   - After Play next or + Queue in the Library, the next visit scrolls to
//     what was added and highlights it (the review's graft; the toast's
//     View does the same at once).
//   - A long press (or Edit) is selection mode: checkboxes, All / None,
//     and a bar under the list: Remove, Play next, and Clear (a sheet:
//     "Clear up next" keeps the playing song, "Clear queue" stops it and
//     asks first). The bar sits in the LCD's fixed area under a shorter
//     scroll band, so it stays put while the list scrolls.
//   - Every edit shows a toast with Undo.
//   - A track that failed to play keeps a small amber "!".
//   - Coming back to a queue that changed meanwhile (Play on an album
//     replaced it) starts at the playing track with no row open: the
//     remembered ones were positions in the old queue.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "QueueView.h"
#include "TrackCatalog.h"
#include "UiStrings.h"
#include "UiText.h"
#include "app/Psram.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

using uistr::S;

namespace {
const S kActions[3] = {S::queue_action_play_now, S::queue_action_play_next, S::queue_action_remove};

// The bottom bar's buttons (selection mode): x ranges; the last reaches the
// edge. Remove has the room for "Remove 16" (uitext; test_ui_library).
constexpr int kRemoveX = uitext::kRemoveX, kRemoveW = uitext::kRemoveW;
constexpr int kNextX = uitext::kQueueNextX, kNextW = uitext::kQueueNextW;
constexpr int kClearX = uitext::kQueueClearX, kClearW = uitext::kQueueClearW;
constexpr int kEditBarH = kH - QueuePage::kBarY;  // 42

uint32_t hintMs(void* ctx, uint32_t id) { return static_cast<const TrackCatalog*>(ctx)->durationHintMs(id); }

// "Removed 3 tracks", "Playing next: 1 track", "Cleared 12 tracks".
void plural(char* buf, size_t size, S id, uint32_t n) { trnf(buf, size, id, n, n); }
}  // namespace

QueuePage::QueuePage(Ui& ui) : Page(ui), selection_(psramAlloc, psramFree) {}

bool QueuePage::noCard() const {
  const AppState& s = ui_.state();
  return !s.card && s.libraryTracks == 0;
}

void QueuePage::enter(NavModel::PageRef& ref) {
  ref_ = &ref;
  const AppState& s = ui_.state();
  // What the Library added since the last visit: shown and highlighted.
  showAdded_ = false;
  queueview::AddedMark& added = ui_.added();
  if (added.pending()) {
    const uint32_t first = added.firstPosition(ui_.queue(), ui_.djKeys());  // (never Auto DJ's appends)
    if (first != queueview::AddedMark::kNone) {
      if (showPos_ < 0) showPos_ = static_cast<int32_t>(first);
      showAdded_ = true;
    } else {
      added.clear();  // undone or removed since
    }
  }
  if ((left_ && s.contentVersion != leftContent_) || showPos_ >= 0) {
    ref.scrollPx = -1;
    ref.expanded = -1;
  }
  content_ = s.contentVersion;
  position_ = s.positionVersion;
  emptyDj_ = autodjview::emptyButton(s.autoDj, s.libraryTracks);
  selecting_ = false;
  touchOn_ = TouchOn::None;
  headerPressed_ = 0;
  barPressed_ = -1;
  ask_ = Ask::None;
  jump_ = 0;
  refreshSummary();
  repaintHeader();
  // The first time (or after the queue changed), the playing track on top;
  // an add to show: its first entry, one row down (what's before it shows
  // where it went).
  int32_t top = s.current > 0 ? s.current * kRowH : 0;
  if (showPos_ >= 0) top = showPos_ > 0 ? (showPos_ - 1) * kRowH : 0;
  if (showAdded_) Serial.printf("[ui] queue: showing what was added, from entry %ld\n", (long)showPos_ + 1);
  showPos_ = -1;
  ui_.list().attach(this, &ref, top);
}

void QueuePage::leave() {
  if (selecting_) {
    selecting_ = false;  // leaving the Queue ends selection mode
    selection_.resize(0);
    ui_.setListBand(ListView::kHeight);
  }
  ui_.list().detach();
  left_ = true;
  leftContent_ = ui_.state().contentVersion;
  // Seen: the next visit highlights only what is added after this one.
  if (showAdded_) ui_.added().clear();
  showAdded_ = false;
}

void QueuePage::repaint() {
  repaintHeader();
  if (selecting_) drawBar();
  ui_.list().invalidate();
}

// ---- the header ----

void QueuePage::refreshSummary() {
  const AppState& s = ui_.state();
  const queueview::DurationBook& book = ui_.host().durations();
  const auto lang = static_cast<uint8_t>(uistr::lang());
  if (s.contentVersion == sumContent_ && s.positionVersion == sumPosition_ && book.version() == sumBook_ &&
      lang == sumLang_) {
    return;
  }
  sumLang_ = lang;
  sumContent_ = s.contentVersion;
  sumPosition_ = s.positionVersion;
  sumBook_ = book.version();
  const TrackCatalog& cat = ui_.player().catalog();
  const queueview::Time t = queueview::upNextTime(ui_.queue(), book, hintMs, const_cast<TrackCatalog*>(&cat));
  const uint32_t position = s.current >= 0 ? static_cast<uint32_t>(s.current + 1) : 0;
  queueview::summary(s.upNext, t, position, s.queueSize, summaryLong_, sizeof(summaryLong_));
  queueview::summary(s.upNext, t, position, s.queueSize, summaryMid_, sizeof(summaryMid_), /*withUpNext=*/false);
  queueview::summary(s.upNext, t, 0, s.queueSize, summaryShort_, sizeof(summaryShort_));
}

Header QueuePage::header() const {
  char* title = headerTitle_;
  char* sub = headerSub_;
  Header h;
  h.title = title;
  h.sub = sub;
  sub[0] = 0;
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  // "of 16", "empty", the summary: whole, or not at all (a cut "noch 9 · 3…"
  // said nothing).
  h.subWhole = true;
  if (selecting_) {
    h.cross = true;
    trnf(title, sizeof(headerTitle_), S::queue_selected, selection_.count(), selection_.count());
    trf(sub, sizeof(headerSub_), S::queue_selected_of, s.queueSize);
    h.right = tr(selection_.count() == s.queueSize && s.queueSize > 0 ? S::queue_none : S::queue_all);
    h.rightMaxW = uitext::kQueuePillTextW;
  } else {
    trf(title, sizeof(headerTitle_), S::queue_title);
    if (s.queueSize == 0) {
      trf(sub, sizeof(headerSub_), S::queue_empty_sub);
    } else {
      // Behind the library update's fence the size is the queue's as it
      // was (AppState's Frozen), its entries the build's: nothing to edit.
      if (!s.libraryFenced) {
        h.right = tr(S::queue_edit);
        h.rightMaxW = uitext::kQueuePillTextW;  // Small past it ("Wählen"): the summary keeps its room
      }
      // The long form if it fits beside the title and the pill (the room
      // drawHeader() leaves: their words in the UI's language); else the
      // position and the time (where the queue is is what the header is
      // for: spec §6.4); else the position alone; the count and the time
      // only with nothing current (no position: the owner's call). When
      // the position alone doesn't fit there (German's "Warteschlange"
      // once it takes six digits), the first that fits on the header's
      // second line, under the title, in the same order (the Folder page's
      // two lines: the header keeps its height, the list its rows); else
      // none (queueview::placeSummary()).
      const int pill = Header::pillW(tr(S::queue_edit), uitext::kQueuePillTextW, false);
      const int right = uitext::headerRight(pill);
      const int room = uitext::headerSubRoom(12, f.width(Font::Bold, tr(S::queue_title)), right);
      // The position alone, made in the header's buffer.
      if (s.current >= 0) {
        queueview::summary(0, queueview::Time{}, static_cast<uint32_t>(s.current + 1), s.queueSize, sub,
                           sizeof(headerSub_), /*withUpNext=*/false);
      }
      // Each form at its SummaryForm (the order they are tried is the
      // enum's, which the host tests check: QueuePage isn't built there).
      const char* forms[queueview::kSummaryForms];
      forms[queueview::kSummaryLong] = summaryLong_;
      forms[queueview::kSummaryMid] = summaryMid_;
      forms[queueview::kSummaryPosition] = sub;
      forms[queueview::kSummaryShort] = summaryShort_;
      int widths[queueview::kSummaryForms];
      for (int k = 0; k < queueview::kSummaryForms; ++k) {
        widths[k] = forms[k][0] ? f.width(Font::Small, forms[k]) : -1;
      }
      const queueview::SummaryPick pick = queueview::placeSummary(widths, room, right - 12);
      // (drawHeader() puts it under the title: it is wider than the room
      // beside it.)
      h.subBelow = pick.below;
      if (pick.form < 0) {
        sub[0] = 0;
      } else if (pick.form != queueview::kSummaryPosition) {
        snprintf(sub, sizeof(headerSub_), "%s", forms[pick.form]);
      }
    }
  }
  h.pressed = headerPressed_ == 2;
  return h;
}

uint32_t QueuePage::headerSig() const {
  const AppState& s = ui_.state();
  uint32_t h = (static_cast<uint32_t>(s.current + 1) * 2654435761u) ^ (s.queueSize * 40503u) ^
               (selecting_ ? selection_.count() * 97u + 1u : 0u) ^ (static_cast<uint32_t>(headerPressed_) << 29) ^
               (static_cast<uint32_t>(s.libraryFenced) << 28);
  for (const char* p = summaryLong_; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  for (const char* p = summaryMid_; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  return h;
}

void QueuePage::repaintHeader() {
  ui_.drawHeader(header());
  headerSig_ = headerSig();
}

bool QueuePage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  const AppState& s = ui_.state();
  ListView& list = ui_.list();
  // The library update's fence: the queue is the build's (its version
  // frozen, so the selection would outlive it): selection mode ends.
  if (selecting_ && s.libraryFenced) setSelecting(false);
  if (s.contentVersion != content_) {
    // Entries came or went: positions moved, so the selection starts over,
    // and an open row closes (it would be under another track now).
    content_ = s.contentVersion;
    position_ = s.positionVersion;
    if (selecting_) {
      selection_.resize(s.queueSize);
      if (s.queueSize == 0) {
        setSelecting(false);
      } else {
        drawBar();
      }
    }
    list.collapse();
    list.reload();
  } else if (s.positionVersion != position_) {
    position_ = s.positionVersion;  // another track plays: its row and the last one change
    list.refreshAll();
  }
  // Empty: Auto DJ's button comes or goes with a usable index (a card
  // mounted again): the empty state drawn again.
  const bool dj = autodjview::emptyButton(s.autoDj, s.libraryTracks);
  if (dj != emptyDj_) {
    emptyDj_ = dj;
    if (s.queueSize == 0) list.reload();
  }
  refreshSummary();
  if (headerSig() != headerSig_) repaintHeader();
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

// ---- selection mode ----

void QueuePage::setSelecting(bool on) {
  if (on == selecting_) return;
  selecting_ = on;
  barPressed_ = -1;
  ListView& list = ui_.list();
  if (on) {
    selection_.resize(ui_.state().queueSize);
    list.collapse();
    ui_.setListBand(kEditBand);  // the bar goes under a shorter scroll band
    drawBar();
  } else {
    selection_.resize(0);
    ui_.setListBand(ListView::kHeight);
  }
  list.refreshAll();
  repaintHeader();
  Serial.printf("[ui] queue: selection mode %s\n", on ? "on" : "off");
}

void QueuePage::drawBar() {
  if (!selecting_) return;
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const uint32_t n = selection_.count();
  c.fillRect(0, 0, kW, kEditBarH, col::SURF);
  c.drawFastHLine(0, 0, kW, col::DIV);
  // Remove N: red, or an outline while nothing is selected.
  {
    const bool on = n > 0;
    const uint16_t fill = !on ? col::SURF : barPressed_ == 0 ? col::SOFT : col::RED;
    if (on) {
      c.fillRoundRect(kRemoveX, 5, kRemoveW, 32, 8, fill);
    } else {
      c.drawRoundRect(kRemoveX, 5, kRemoveW, 32, 8, col::FAINT);
    }
    const uint16_t ink = on ? col::DARK : col::FAINT;
    char t[48];
    if (n > 0) {
      trf(t, sizeof(t), S::queue_remove_n, n);
    } else {
      trf(t, sizeof(t), S::queue_remove);
    }
    // The count must show (it's what the finger is about to remove): the
    // icon makes way for a big one ("Remove 120").
    const int textRoom = kRemoveW - uitext::kRemoveTextX - 4;
    if (f.width(Font::Bold, t) <= textRoom) {
      icons::drawCentred(c, icons::kTrash, kRemoveX + 18, 21, ink);
      f.draw(c, Font::Bold, t, kRemoveX + uitext::kRemoveTextX, 21, textRoom, ink, fill);
    } else {
      f.draw(c, Font::Bold, t, kRemoveX + kRemoveW / 2, 21, kRemoveW - 8, ink, fill, Fonts::Align::Centre);
    }
  }
  // Play next.
  {
    const bool on = n > 0;
    const uint16_t fill = barPressed_ == 1 ? col::BTN_HI : col::BTN;
    c.fillRoundRect(kNextX, 5, kNextW, 32, 8, fill);
    const char* label = tr(S::queue_play_next_bar);
    const int room = kNextW - uitext::kQueueNextPad;
    f.draw(c, f.fitting(Font::Body, label, room), label, kNextX + kNextW / 2, 21, room, on ? col::TXT : col::FAINT, fill,
           Fonts::Align::Centre);
  }
  // Clear...: red outline.
  {
    const uint16_t fill = barPressed_ == 2 ? col::ROW_SEL : col::SURF;
    c.fillRoundRect(kClearX, 5, kClearW, 32, 8, fill);
    c.drawRoundRect(kClearX, 5, kClearW, 32, 8, col::RED);
    c.drawRoundRect(kClearX + 1, 6, kClearW - 2, 30, 7, col::RED);
    const char* label = tr(S::queue_clear_bar);
    const int room = kClearW - uitext::kQueueClearPad;
    f.draw(c, f.fitting(Font::Body, label, room), label, kClearX + kClearW / 2, 21, room, col::RED, fill,
           Fonts::Align::Centre);
  }
  gfx::push(c, 0, kBarY, kW, kEditBarH);
}

int QueuePage::barAt(const InputEvent& e) const {
  if (e.y < kBarY) return -1;
  if (e.atRightEdge() || e.x >= kClearX - 3) return 2;
  if (e.x >= kNextX - 3) return 1;
  return 0;
}

void QueuePage::selectAll() {
  const uint32_t n = ui_.state().queueSize;
  if (selection_.size() != n) selection_.resize(n);
  selection_.setAll(selection_.count() != n);  // all, or (all were) none
  ui_.list().refreshAll();
  drawBar();
  repaintHeader();
}

// Behind the library update's fence the queue is the build's: the bar's
// actions wait, as a skip does (PlaybackController refuses them anyway).
bool QueuePage::fenced() {
  if (!ui_.state().libraryFenced) return false;
  setSelecting(false);
  ui_.toast(tr(S::library_updating_wait), false);
  return true;
}

void QueuePage::removeSelected() {
  if (fenced()) return;
  const uint32_t n = selection_.count();
  if (n == 0) return;
  auto* positions = static_cast<uint32_t*>(psramAlloc(n * sizeof(uint32_t)));
  if (!positions) {
    ui_.toast(tr(S::toast_no_memory), false);
    return;
  }
  const uint32_t got = selection_.list(positions, n);
  const QueueModel::Removed r = ui_.player().remove(positions, got);
  psramFree(positions);
  setSelecting(false);
  char text[96];
  plural(text, sizeof(text), S::queue_removed_n, r.count);
  Serial.printf("[ui] queue: removed %lu%s\n", static_cast<unsigned long>(r.count),
                r.current ? " (the playing one too: the next plays)" : "");
  ui_.toast(text, true);
}

void QueuePage::playSelectedNext() {
  if (fenced()) return;
  const uint32_t n = selection_.count();
  if (n == 0) return;
  auto* positions = static_cast<uint32_t*>(psramAlloc(n * sizeof(uint32_t)));
  if (!positions) {
    ui_.toast(tr(S::toast_no_memory), false);
    return;
  }
  const uint32_t got = selection_.list(positions, n);
  const uint32_t before = ui_.queue().contentVersion();
  const bool ok = ui_.player().moveNext(positions, got);
  psramFree(positions);
  setSelecting(false);
  if (!ok) {
    ui_.toast(tr(S::toast_no_memory), false);
  } else if (ui_.queue().contentVersion() == before) {
    ui_.toast(tr(S::queue_thats_playing), false);  // only the playing one was selected
  } else {
    char text[96];
    plural(text, sizeof(text), S::queue_playing_next_n, got);
    Serial.printf("[ui] queue: %lu to play next\n", static_cast<unsigned long>(got));
    ui_.toast(text, true);
  }
}

void QueuePage::clearUpNext() {
  if (fenced()) return;
  const uint32_t n = ui_.state().upNext;
  setSelecting(false);
  if (n == 0) {
    ui_.toast(tr(S::queue_nothing_after), false);
    return;
  }
  const bool ok = ui_.player().clearUpNext();
  char text[96];
  plural(text, sizeof(text), S::queue_cleared_n, n);
  Serial.printf("[ui] queue: clear up next (%lu)\n", static_cast<unsigned long>(n));
  ui_.toast(ok ? text : tr(S::toast_no_memory), ok);
}

void QueuePage::clearQueue() {
  if (fenced()) return;
  const uint32_t n = ui_.state().queueSize;
  setSelecting(false);
  ui_.player().clearQueue();
  char text[96];
  plural(text, sizeof(text), S::queue_cleared_n, n);
  Serial.printf("[ui] queue: cleared (%lu), stopped\n", static_cast<unsigned long>(n));
  ui_.toast(text, true);
}

void QueuePage::onSheet(int choice) {
  if (ask_ != Ask::ClearSheet) return;
  ask_ = Ask::None;
  if (choice == 0) {
    clearUpNext();
  } else if (choice == 1) {
    // The one edit that stops the music: asked once more.
    const char* const buttons[2] = {tr(S::dialog_cancel), tr(S::queue_clear_confirm_button)};
    char body[256];
    const uint32_t n = ui_.state().queueSize;
    trnf(body, sizeof(body), S::queue_clear_confirm_body, n, n);
    ask_ = Ask::ClearConfirm;
    ui_.openDialog(this, tr(S::queue_clear_confirm_title), body, buttons, 2, true);
  }
}

void QueuePage::onDialog(int button) {
  if (ask_ != Ask::ClearConfirm) return;
  ask_ = Ask::None;
  if (button == 1) clearQueue();
}

// ---- touch ----

void QueuePage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    const int bandEnd = ListView::kTop + ui_.list().height();
    if (e.y < ListView::kTop) {
      touchOn_ = TouchOn::Header;
      headerPressed_ = header().hit(e);
      if (headerPressed_ == 2) repaintHeader();
    } else if (selecting_ && e.y >= bandEnd) {
      touchOn_ = TouchOn::Bar;
      const int b = barAt(e);
      barPressed_ = b == 0 || b == 1 ? (selection_.count() > 0 ? b : -1) : b;
      if (barPressed_ >= 0) drawBar();
    } else {
      touchOn_ = TouchOn::List;
    }
  } else if (e.type == T::DragStart && e.fromStrip) {
    // A swipe up from the strip: it scrolls the list, even from under the
    // edit bar (which it doesn't press).
    touchOn_ = TouchOn::List;
  }
  switch (touchOn_) {
    case TouchOn::List:
      ui_.list().onEvent(e);
      if (selecting_ && e.type == T::Tap) {
        // A toggle: the count in the header and the bar.
        drawBar();
        repaintHeader();
      }
      break;
    case TouchOn::Bar:
      if (e.type == T::Tap) {
        const int b = barPressed_;
        barPressed_ = -1;
        drawBar();
        if (b < 0) break;
        ui_.tick();
        if (b == 0) {
          removeSelected();
        } else if (b == 1) {
          playSelectedNext();
        } else {
          const char* const rows[2] = {tr(S::queue_clear_up_next), tr(S::queue_clear_all)};
          const char* const details[2] = {tr(S::queue_clear_up_next_detail), tr(S::queue_clear_all_detail)};
          ask_ = Ask::ClearSheet;
          ui_.openSheet(this, tr(S::queue_clear_sheet_title), rows, 2, details, -1, /*danger=*/1);
        }
      } else if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
        if (barPressed_ >= 0) {
          barPressed_ = -1;
          drawBar();
        }
      }
      break;
    case TouchOn::Header:
      if (e.type == T::Tap) {
        const int hit = headerPressed_;
        headerPressed_ = 0;
        if (hit == 1 && selecting_) {
          ui_.tick();
          setSelecting(false);
        } else if (hit == 2) {
          ui_.tick();
          if (selecting_) {
            selectAll();
          } else if (ui_.state().queueSize > 0 && !fenced()) {
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
      break;
    default:
      break;
  }
  const bool ends = e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel;
  if (ends) touchOn_ = TouchOn::None;
}

void QueuePage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  snprintf(buf, size, "Queue: %lu entries, current %ld%s, \"%s\"%s", static_cast<unsigned long>(s.queueSize),
           static_cast<long>(s.current), selecting_ ? ", selecting" : "", summaryLong_,
           showAdded_ ? ", showing what was added" : "");
}

// ---- the list ----

uint32_t QueuePage::rows() { return ui_.queue().size(); }

uint16_t QueuePage::accent() { return accent::Queue; }

// The playing entry too: ListView fills the row's background before the
// checkbox, so a fill in drawRow() would wipe the ring (seen on the device:
// the playing row had no checkbox in selection mode).
bool QueuePage::tinted(uint32_t row) {
  return static_cast<int32_t>(row) == ui_.queue().current() ||
         (showAdded_ && ui_.added().marks(ui_.queue().keyAt(row), ui_.djKeys()));
}

void QueuePage::drawRow(ListView::Row& r) {
  const QueueModel& q = ui_.queue();
  const TrackCatalog& cat = ui_.player().catalog();
  const int32_t current = q.current();
  const bool isCurrent = static_cast<int32_t>(r.row) == current;
  const bool played = current >= 0 && static_cast<int32_t>(r.row) < current;
  const uint32_t key = q.keyAt(r.row);
  if (isCurrent) r.c.fillRect(0, 0, 3, kRowH, accent::Queue);
  // Added by the Library since the last visit: a dot in the accent.
  const bool added = showAdded_ && ui_.added().marks(key, ui_.djKeys());
  if (added && !r.selected) r.c.fillCircle(r.x + 6, kRowH / 2, 3, accent::Queue);
  const uint32_t track = q.trackAt(r.row);
  const int x = isCurrent ? ListView::playing(r, accent::Queue)
                          : ListView::number(r, r.row + 1, played ? col::FAINT : col::DIM);
  int right = r.right - 8;
  if (ui_.failedKeys().has(key)) {
    // Couldn't be played: a small amber "!" at the right.
    icons::drawCentred(r.c, icons::kWarn, r.right - 18, kRowH / 2, col::AMBER);
    right = r.right - 32;
  } else if (ui_.host().autoDjMarked(key)) {
    // Auto DJ added it: its small mark there, dim (the engine design's
    // 2.14; RAM only, the last 64 of its picks).
    icons::drawCentred(r.c, icons::kAutoDjSmall, r.right - 16, kRowH / 2, col::DIM);
    right = r.right - 28;
  }
  char title[128];
  if (!cat.title(track, title, sizeof(title))) snprintf(title, sizeof(title), "%s", tr(S::queue_not_in_library));
  const char* artist = cat.artist(track);
  const uint16_t colour = isCurrent ? accent::Queue : played ? col::DIM : col::TXT;
  ListView::lines(r, x, right, title, strlen(title), artist, strlen(artist), colour,
                  isCurrent ? Font::Bold : Font::Body);
}

int QueuePage::actions(int32_t row, const char* labels[3]) {
  (void)row;
  for (int k = 0; k < 3; ++k) labels[k] = tr(kActions[k]);
  return 3;
}

void QueuePage::onAction(int32_t row, int action) {
  if (row < 0 || static_cast<uint32_t>(row) >= ui_.queue().size()) return;
  auto pos = static_cast<uint32_t>(row);
  PlaybackController& p = ui_.player();
  char title[96];
  if (!p.catalog().title(ui_.queue().trackAt(pos), title, sizeof(title))) {
    snprintf(title, sizeof(title), "%s", tr(S::queue_it));
  }
  char text[160];
  ui_.list().collapse();
  switch (action) {
    case 0:
      // Logged: the one queue edit with no toast (the review of a jump from
      // entry 24 to 2 found this, unlogged, the only way a touch moves the
      // current entry anywhere without a line in the log).
      Serial.printf("[ui] queue: play entry %lu (its row's bar)\n", static_cast<unsigned long>(pos + 1));
      p.play(pos);
      if (p.state() == PlayState::Waiting) {
        // The headphones aren't connected: it plays once they are (Now
        // Playing shows the wait, and its way out).
        if (ui_.state().btName[0]) {
          trf(text, sizeof(text), S::library_waiting_for, ui_.state().btName, title);
        } else {
          trf(text, sizeof(text), S::library_waiting_for_headphones, title);
        }
        ui_.warn(text);
      }
      break;
    case 1: {
      // The playing entry can't go after itself: moveNext() changes nothing
      // (and saves no undo), so the toast mustn't offer one.
      const uint32_t before = ui_.queue().contentVersion();
      if (!p.moveNext(&pos, 1)) {
        ui_.toast(tr(S::toast_no_memory), false);
      } else if (ui_.queue().contentVersion() == before) {
        trf(text, sizeof(text), S::queue_playing_now, title);
        ui_.toast(text, false);
      } else {
        Serial.printf("[ui] queue: entry %lu to play next\n", static_cast<unsigned long>(pos + 1));
        trf(text, sizeof(text), S::library_plays_next, title);
        ui_.toast(text, true);
      }
      break;
    }
    default: {
      const QueueModel::Removed r = p.remove(&pos, 1);
      if (r.count) {
        Serial.printf("[ui] queue: removed entry %lu%s\n", static_cast<unsigned long>(pos + 1),
                      r.current ? " (it was playing: the next plays)" : "");
        trf(text, sizeof(text), S::queue_removed_one, title);
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
  drawBar();
  repaintHeader();
}

// ---- empty ----

const char* QueuePage::emptyText() {
  return tr(ui_.state().libraryFenced ? S::library_updating_list : S::queue_empty_text);
}

bool QueuePage::emptyState(EmptyState& e) {
  if (ui_.state().libraryFenced) return false;  // the library update's fence: emptyText()'s line
  if (noCard()) {
    noCardState(e, ui_.state().cardKind);
    return true;
  }
  e.icon = &icons::kQueue;
  e.title = tr(S::empty_queue_title);
  e.lines = tr(S::empty_pick_in_library);
  e.buttons[0] = tr(S::empty_open_library);
  e.buttonIcons[0] = &icons::kLibrary;
  const AppState& s = ui_.state();
  if (autodjview::emptyButton(s.autoDj, s.libraryTracks)) {
    // A usable Auto DJ index: Auto DJ in Shuffle all's place (D4).
    e.buttons[1] = tr(S::empty_autodj);
    e.buttonIcons[1] = &icons::kAutoDj;
  } else if (s.libraryTracks > 0) {
    e.buttons[1] = tr(S::empty_shuffle_all);
    e.buttonIcons[1] = &icons::kShuffle;
  }
  return true;
}

void QueuePage::onEmptyAction(int i) {
  if (noCard()) {
    ui_.retryCard();
    return;
  }
  if (i == 0) {
    ui_.showTab(NavModel::Tab::Library);
  } else if (emptyDj_) {
    ui_.autoDjStart();  // (the button drawn: update() draws the state again when it changes)
  } else {
    ui_.shuffleAll();
  }
}

}  // namespace ui
