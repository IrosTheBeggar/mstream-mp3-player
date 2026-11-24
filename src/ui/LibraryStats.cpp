// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Library's stats half (docs/LISTENING-STATS.md 7.1-7.5, 7.10, 7.11;
// S3b's C22): the Artists root's entry row, the hub and its empty state,
// and what a stats page does each pass (its shape, its minute, the stats'
// news). LibraryPage's shape is in Pages.h. What a page shows and its words
// are lib/core's (statsview, statstext), host-tested in test_ui_stats; this
// reads the host through Ui's accessors (the lifetime, the phase, the
// snapshot) at each draw and act, and keeps nothing of them but the shape
// it drew and the minute. The shelves (7.6, S10): their rows from the
// snapshot (ranked tracks, Recently played's headings, Not played yet's
// albums), Most played's tray, their acts through the queue's id view, and
// their rows' arrival at the place kept for them. Last 7 days (7.7, S11):
// its picture through the list's body hook, strip by strip, from the
// snapshot's week, or the empty state with its reason and Try again.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "IdView.h"
#include "LibraryText.h"
#include "ListLayout.h"
#include "StatsRank.h"
#include "StatsText.h"
#include "StatsView.h"
#include "UiStrings.h"
#include "UiText.h"
#include "ui/EmptyState.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

using uistr::S;
namespace sv = statsview;
namespace st = statstext;

namespace {

// The console's names for the hub's rows (it speaks English, whatever the
// UI's language).
const char* const kHubRowNames[sv::kHubRows + 1] = {"Last 7 days",    "Most played",     "New finds", "Old favourites",
                                                    "Not played yet", "Recently played", "Try again"};

const char* rowKindName(sv::RowKind k) {
  switch (k) {
    case sv::RowKind::Open: return "open";
    case sv::RowKind::Disabled: return "disabled";
    case sv::RowKind::Status: return "the status row";
    default: return "Try again";
  }
}

NavModel::PageRef page(PageKind kind, uint32_t id) {
  NavModel::PageRef p;
  p.kind = static_cast<uint8_t>(kind);
  p.id = id;
  return p;
}

// A text's width in Small (statsview's entry line measures by it).
int smallWidth(const char* text, void*) { return Fonts::instance().width(Font::Small, text); }

// The row recipe (7.4): the icon centred at (26, 21), drawn there directly
// (not ListView::icon()'s x + 22); the title and line from x 54.
constexpr int kIconX = 26, kIconY = 21, kTextX = 54;

// The entry row's fixed bars (7.3, call 2: an icon, not the week), also the
// hub's Last 7 days icon: past DIM, the last violet, standing on y 32.
void drawBars(M5Canvas& c) {
  for (int k = 0; k < sv::kEntryBarCount; ++k) {
    const int h = sv::kEntryBars[k];
    c.fillRect(sv::kEntryBarX + k * sv::kEntryBarStep, sv::kEntryBarBase - h, sv::kEntryBarW, h,
               k == sv::kEntryBarCount - 1 ? accent::Library : col::DIM);
  }
}

}  // namespace

// ---- the entry row (7.3) ----

void LibraryPage::latchEntry() {
  entry_ = EntryRow::None;
  if (!root() || segment() != LibrarySegment::Artists) return;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const LibraryIndex* i = index();
  // Absent with the stats off, the card out, behind the update's fence, on
  // uil's synthetic library, and with no artists (D18: under a row the
  // empty state's buttons couldn't be pressed).
  if (!sv::entryDue(l, i != nullptr, !real(), i ? i->artistCount() : 0)) return;
  // Its line, RAM, as of now (call 1: the lifetime one in 204 px, else the
  // time alone; no plays).
  const sv::EntryLine k = sv::entryLine(l, smallWidth, nullptr, headerSub_, sizeof(headerSub_));
  entry_ = k == sv::EntryLine::Counting ? EntryRow::Counting : EntryRow::Shown;
}

void LibraryPage::drawEntryRow(ListView::Row& r) {
  drawBars(r.c);
  // (FAINT, at right - 14.) None on "Couldn't read the stats": the row
  // opens nothing then (onTopRowTap()).
  if (entry_ != EntryRow::Unread) ListView::chevron(r);
  Fonts& f = Fonts::instance();
  f.draw(r.c, Font::Body, tr(S::stats_title), kTextX, 13, sv::kEntryRoom, col::TXT, r.bg);
  f.draw(r.c, Font::Small, headerSub_, kTextX, 31, sv::kEntryRoom, col::DIM, r.bg);
}

// The row's tap (after ListView's tick): the hub, when it can be shown now
// (statsview::hubReachable(), uiSh's rule: 4.4's Off has no hub). A20's
// "Couldn't read the stats", or a row whose stats went off since its latch,
// opens nothing: the double buzz, no toast (its line says why). The ask is
// the held condition's rise (Ui::showTop()), not the tap's.
void LibraryPage::entryTap() {
  if (entry_ == EntryRow::None) return;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const LibraryIndex* i = index();
  if (entry_ == EntryRow::Unread || !sv::hubReachable(l, i != nullptr, !real())) {
    ui_.input().missBuzz();
    Serial.printf("[ui] stats: the listening stats' row opens nothing now (%s)\n",
                  entry_ == EntryRow::Unread ? "couldn't read the stats: off this session"
                                             : "the hub isn't reachable: the stats are off");
    return;
  }
  ui_.push(page(PageKind::StatsHub, 0));
}

// ---- a stats page's life ----

sv::HubLook LibraryPage::hubNow(const sv::Lifetime& l) const {
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  return sv::hubLook(l, p, ui_.statsSnapshot());
}

uint32_t LibraryPage::statsSigNow(const sv::Lifetime& l, sv::PageLook* look) const {
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  sv::PageLook k;
  uint8_t shape = 0;
  switch (kind_) {
    case PageKind::StatsHub: {
      const sv::HubLook h = sv::hubLook(l, p, snap);
      k = h.page;
      // (Its header's lifetime line too: it comes and goes with the rows'
      // looks, A19, 7.5, and the header follows: statsFollow().)
      shape = static_cast<uint8_t>(sv::hubShape(h) | (sv::hubSubDue(l) ? sv::kShapeSub : 0u));
      break;
    }
    case PageKind::StatsShelf: {
      // (Its tray too: its cells, the window picked, a "16 days" label; and
      // the header's line with the window.)
      const sv::ShelfLook s = sv::shelfLook(id_, l, p, snap);
      k = s.page;
      shape = sv::shelfShape(s, sv::trayOf(l, snap));
      break;
    }
    default:
      // (Week: its look; its header's days move with its texts, the
      // snapshot's version: statsview::weekHeaderMoved().)
      k = sv::weekLook(l, p, snap).page;
      shape = static_cast<uint8_t>(k.look);
      break;
  }
  if (look) *look = k;
  return static_cast<uint32_t>(shape) << 16 | sv::lookText(k, snap);
}

void LibraryPage::statsEntering() {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  minute_ = static_cast<uint16_t>(l.nowMin);
  sv::PageLook look;
  keepSig(statsSigNow(l, &look));
  // Idle with nothing served and the stats on (a Cancelled ask, a landing
  // released since): its Working line, and the one ask it is due (D9 as
  // replaced), once the held condition is up. The edge's own ask, just
  // made, reads Working already: none due then.
  if (look.askDue && ui_.statsHeld()) ui_.statsAsk(sv::UiAsk::Visit);
  // Most played served: the window it shows is the snapshot's from here
  // (D7, as at a landing: the hub's next push opens on it; a shelf pushed
  // with a window of its own, from the console or Now Playing, keeps it).
  if (kind_ == PageKind::StatsShelf && sv::shelfOf(id_) == sv::Shelf::MostPlayed) {
    const sv::ShelfLook k = shelfNow(l);
    if (sv::windowOwed(k, ui_.statsSnapshot())) ui_.statsSetWindow(k.window);
  }
}

void LibraryPage::statsFollow(const sv::Lifetime& l, const char* why) {
  sv::PageLook look;
  const uint32_t sig = statsSigNow(l, &look);
  // Idle with nothing served and the stats on (D9 as replaced: a Cancelled
  // ask, a landing released since): its one ask, while the held condition
  // is up. It reads Working at once (a queued ask), so it is made once; an
  // ask the host would drop (the stats off, the card gone) is never due
  // (their looks are Off and No card).
  if (look.askDue && ui_.statsHeld()) ui_.statsAsk(sv::UiAsk::Visit);
  const uint32_t drawn = statsSigDrawn();
  if (sig == drawn) return;  // (nothing it draws moved: a landing's second version move, Idle to Working)
  keepSig(sig);
  if (kind_ == PageKind::StatsShelf) {
    shelfFollow(drawn, sig, l, why);  // (its shape's bits are its own: shelfShape())
    return;
  }
  if (kind_ == PageKind::StatsWeek) {
    weekFollow(drawn, sig, look.look, why);  // (no rows: its body or its empty state, whole)
    return;
  }
  const uint32_t moved = sig ^ drawn;
  constexpr uint32_t kSub = static_cast<uint32_t>(sv::kShapeSub) << 16;
  // The hub's header line came or went (the boot fold's end, a play counted
  // on the empty hub, the stats off): the header again, about 5 ms, as 7.4
  // has it ("At once: the header and its sub"). Under a modal or in the
  // dark it is drawn whole when the page shows again (repaintUnder(),
  // redrawAll()); a toast over it keeps its row (gfx's cover).
  if ((moved & kSub) && ui_.pageShowing()) repaintHeader();
  if (!(moved & ~kSub)) return;  // (the header alone)
  minute_ = static_cast<uint16_t>(l.nowMin);  // (row 6 drawn again with the rest)
  Serial.printf("[ui] stats: %s drawn again in place (%s): %s\n", pageKindName(static_cast<uint8_t>(kind_)), why,
                sv::lookName(look.look));
  // One stale row a pass at a still pass (7.2, 7.10): the place kept (-1:
  // where it stands), no bar put back (-1: across a new ask none is, 7.2),
  // every row new (0), no lead item (the hub has none; a shelf's tray:
  // shelfFollow()). Under a modal or in the dark the page is drawn whole when it
  // shows again. A text alone moved (the shape the same: each row still the
  // kind it was, the Try again row there or not as before): a finger on a
  // row keeps its press, its tap the row it was (a tap across a landing's
  // news, or Working's line to Waiting's).
  const bool sameRows = !((moved >> 16) & ~static_cast<uint32_t>(sv::kShapeSub) & 0xFFu);
  ListView& list = ui_.list();
  list.recountStaged(-1, -1, 0, false, sameRows);
  // The empty state to No card's line (the card pulled with nothing
  // played): no rows either way, so the recount owes nothing but the line's
  // strip; the band is drawn whole once (its disc, lines and Shuffle all
  // gone). (Week's body and empty state each cover the band: weekFollow().)
  if (kind_ == PageKind::StatsHub &&
      sv::hubDrawsWhole(static_cast<sv::Look>((drawn >> 16) & 7u), look.look)) {
    list.invalidate();
  }
}

bool LibraryPage::statsNews(const char* why) {
  // A page that drew the snapshot's rows (a shelf served: its tracks, its
  // albums) has none now: attached again at 0 rows, by the Ui (what reads
  // the snapshot closed first: its sheet, the jump grid). (A Try again row
  // isn't the snapshot's: restaged as any.) The hub and Week read the
  // snapshot at each draw: drawn again.
  if (kind_ == PageKind::StatsShelf && ui_.list().source() == this && ui_.list().layout().rows() > 0 &&
      static_cast<sv::Look>(statsShape_ & sv::kShelfLook) == sv::Look::Served && !ui_.statsSnapshot()) {
    return true;
  }
  sv::Lifetime l;
  ui_.statsLifetime(l);
  statsFollow(l, why);
  return false;
}

void LibraryPage::statsUpdate() {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  if (entry_ == EntryRow::Counting) {
    // The Artists root's row at the boot fold's end (7.3, 7.10, A20): its
    // line drawn again in place, once.
    if (!sv::entryFoldEnded(sv::EntryLine::Counting, l.folding)) return;
    const sv::EntryLine k = sv::entryLine(l, smallWidth, nullptr, headerSub_, sizeof(headerSub_));
    entry_ = k == sv::EntryLine::Unread ? EntryRow::Unread : EntryRow::Shown;  // (Unread: no chevron, opens nothing)
    ui_.list().refreshTopRow();
    Serial.printf("[ui] stats: Artists' first row at the boot fold's end: \"%s\"\n", headerSub_);
    return;
  }
  if (!statsShown()) return;
  // What no news brings (the card back, a play counted on the empty hub,
  // the clock set, a day): drawn again in place. (What the news brought
  // under a modal or in the dark was drawn whole as the page came back.)
  const uint32_t drawn = statsSigDrawn();
  statsFollow(l, "its look moved");
  if (statsSigDrawn() != drawn) return;
  // A new minute (7.10): Recently played's "25 min ago" on row 6, when its
  // text moved, at a still pass; on its own shelf, its rows on screen.
  const uint16_t now = static_cast<uint16_t>(l.nowMin);
  if (now == minute_ || ui_.list().animating()) return;
  const uint32_t was = sv::minuteFrom(minute_, l.nowMin);
  minute_ = now;
  if (kind_ == PageKind::StatsShelf) {
    recentMinute(l, was);
    return;
  }
  if (kind_ != PageKind::StatsHub) return;
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  const sv::HubLook h = hubNow(l);
  const uint32_t row = static_cast<uint32_t>(sv::HubRow::Recent);
  if (!snap || h.item[row].line != sv::Line::Data || snap->rows(statsrank::kRecent) == 0) return;
  if (!st::agoMoved(was, l.nowMin, snap->rowAt(statsrank::kRecent, 0).when)) return;
  ui_.list().refreshRow(row);
}

void LibraryPage::statsFrame() {
  const ListView::FrameCost& c = ui_.list().lastFrame();
  if (c.kind != ListView::FrameCost::Kind::Stage) return;
  const char* name = pageKindName(static_cast<uint8_t>(kind_));
  if (c.item == ListView::FrameCost::kEmptyLine) {
    if (kind_ == PageKind::StatsWeek) {
      // (Its picture whole, three strips: what was drawn of it is no more.)
      sv::Lifetime l;
      ui_.statsLifetime(l);
      Serial.printf("[ui] stats: %s: its %s drawn whole in place\n", name,
                    sv::weekBody(weekNow(l).page.look) ? "body" : "empty state");
      return;
    }
    Serial.printf("[ui] stats: %s: its line staged in place\n", name);
    return;
  }
  // A position past the items is the ground filled where an old row was
  // (Try again gone, the card pulled), not a row of the hub's.
  if (c.item >= ui_.list().layout().itemCount()) {
    Serial.printf("[ui] stats: %s: position %u, past the rows, filled in place\n", name,
                  static_cast<unsigned>(c.item + 1));
    return;
  }
  if (kind_ == PageKind::StatsHub) {
    // (The hub has no lead items: its item is its row, named as hubLook()
    // has it now: the status row with no clock, Try again at index 6.)
    sv::Lifetime l;
    ui_.statsLifetime(l);
    const sv::HubLook h = hubNow(l);
    if (c.item < h.rows) {
      const sv::HubItem& it = h.item[c.item];
      Serial.printf("[ui] stats: %s: row %u (%s) staged in place\n", name, static_cast<unsigned>(c.item + 1),
                    it.kind == sv::RowKind::Status     ? "the status row"
                    : it.kind == sv::RowKind::TryAgain ? kHubRowNames[sv::kHubRows]
                                                       : kHubRowNames[static_cast<uint32_t>(it.row)]);
      return;
    }
  }
  if (kind_ == PageKind::StatsShelf) {
    // A shelf's item: the tray, the top bar, a bar, or a row by its kind
    // as the snapshot has it now (a rank, a heading, an album, Try again).
    const ListLayout::Item it = ui_.list().layout().item(c.item);
    if (it.kind == ListLayout::Kind::Row) {
      Serial.printf("[ui] stats: %s: row %u (%s) staged in place\n", name, static_cast<unsigned>(it.row + 1),
                    sv::shelfRowName(shelfItemAt(static_cast<uint32_t>(it.row)).kind));
    } else {
      Serial.printf("[ui] stats: %s: %s staged in place\n", name,
                    it.kind == ListLayout::Kind::TopRow    ? "the tray"
                    : it.kind == ListLayout::Kind::TopBar  ? "the top bar"
                                                           : "a row's bar");
    }
    return;
  }
  Serial.printf("[ui] stats: %s: item %u staged in place\n", name, static_cast<unsigned>(c.item));
}

// ---- the list ----

uint32_t LibraryPage::statsRows() {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  switch (kind_) {
    case PageKind::StatsHub: return hubNow(l).rows;
    case PageKind::StatsShelf: return shelfNow(l).rows;  // (its tracks, its headings, its albums; Try again)
    default: return 0;                                   // (Week has none: its body, drawBody())
  }
}

size_t LibraryPage::statsTitle(uint16_t id, char* out, size_t size, void* ctx) {
  const LibraryPage* p = static_cast<const LibraryPage*>(ctx);
  const LibraryIndex* i = p->index();
  // (D6: an id of the live index only; the snapshot's key matched it.)
  if (!i || id >= i->trackCount()) {
    if (size) out[0] = 0;
    return 0;
  }
  return p->titleOf(id, out, size);
}

size_t LibraryPage::hubLineOf(const sv::HubItem& it, const sv::Lifetime& l, const statsvisit::Snapshot* snap,
                              char* out, size_t size) {
  return st::hubLine(it, l, snap, Fonts::instance().fit(Font::Small), statsTitle, this, out, size);
}

// The hub's rows (7.4): the row recipe, a disabled row (icon FAINT, title
// and reason DIM, no chevron), the status row (the clock AMBER, its two
// Small lines), Try again (no icon, no chevron: it opens no page).
void LibraryPage::drawStatsRow(ListView::Row& r) {
  if (kind_ != PageKind::StatsHub) return;  // (a shelf's rows: drawShelfRow())
  sv::Lifetime l;
  ui_.statsLifetime(l);
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  const sv::HubLook h = sv::hubLook(l, p, snap);
  if (r.row >= h.rows) return;
  const sv::HubItem& it = h.item[r.row];
  const uint16_t ink = it.kind == sv::RowKind::Disabled ? col::FAINT : col::SOFT;
  switch (it.kind == sv::RowKind::TryAgain ? sv::HubRow::TryAgain : it.row) {
    case sv::HubRow::Week:
      if (it.kind == sv::RowKind::Status) {
        icons::drawCentred(r.c, icons::kClock, kIconX, kIconY, col::AMBER);
      } else {
        drawBars(r.c);  // (the entry row's fixed bars, not the week: 7.4)
      }
      break;
    case sv::HubRow::MostPlayed: icons::drawCentred(r.c, icons::kTrophy, kIconX, kIconY, ink); break;
    case sv::HubRow::NewFinds: icons::drawCentred(r.c, icons::kSpark, kIconX, kIconY, ink); break;
    // (15 rows: centred on y 21 its top would be y 14; the Main frame has
    // it at y 13.)
    case sv::HubRow::OldFavourites: icons::drawCentred(r.c, icons::kHeart, kIconX, kIconY - 1, ink); break;
    case sv::HubRow::NotPlayed: icons::drawCentred(r.c, icons::kRingPlay, kIconX, kIconY, ink); break;
    case sv::HubRow::Recent: icons::drawCentred(r.c, icons::kClock, kIconX, kIconY, ink); break;
    default: break;
  }
  char line[160];
  hubLineOf(it, l, snap, line, sizeof(line));
  if (it.kind == sv::RowKind::Status) {
    // "No dates yet: the clock isn't set" AMBER over "Plug in to the MP3
    // Player tab" DIM, both Small in 250 (D13).
    Fonts& f = Fonts::instance();
    f.draw(r.c, Font::Small, tr(S::stats_no_dates), kTextX, 13, st::kReasonW, col::AMBER, r.bg);
    f.draw(r.c, Font::Small, line, kTextX, 31, st::kReasonW, col::DIM, r.bg);
    return;
  }
  // Open: the chevron, the line in 232. Try again: the same room, no
  // chevron. Disabled: none, its reason in 250.
  const int right = it.kind == sv::RowKind::Open       ? ListView::chevron(r)
                    : it.kind == sv::RowKind::TryAgain ? r.right - 26
                                                       : r.right - 8;
  const char* title = tr(sv::hubTitle(it));
  ListView::lines(r, kTextX, right, title, strlen(title), line, strlen(line),
                  it.kind == sv::RowKind::Disabled ? col::DIM : col::TXT);
}

Header LibraryPage::statsHeader() const {
  Header h;
  h.back = true;
  h.subBelow = true;  // (beside the title when it fits whole there, else under it: 7.4's 155 px in 139)
  headerSub_[0] = 0;
  h.sub = headerSub_;
  switch (kind_) {
    case PageKind::StatsHub: {
      h.title = tr(S::stats_title);
      // The lifetime line (RAM), none while the boot's fold runs (A19) or
      // with nothing played (7.5: the title alone).
      sv::Lifetime l;
      ui_.statsLifetime(l);
      if (sv::hubSubDue(l)) st::hubSub(l.listenedMs, l.sinceDay, headerSub_, sizeof(headerSub_));
      break;
    }
    case PageKind::StatsShelf: {
      // Its line under the title (subBelow, 7.6: it stays once the tray
      // scrolls away): Most played's window ("last 7 days", "since 1
      // Sep"; at the 30 days what the picked cell says, "16 days" with
      // RCNT capped), the others' own; none on Recently played.
      const sv::Shelf s = sv::shelfOf(id_);
      h.title = tr(sv::shelfTitle(s));
      sv::Lifetime l;
      ui_.statsLifetime(l);
      st::shelfSub(s, shelfNow(l).window, sv::trayOf(l, ui_.statsSnapshot()).days30, l.sinceDay, headerSub_,
                   sizeof(headerSub_));
      break;
    }
    default: {
      // "Last 7 days" and its figures' days, beside it when they fit whole
      // there, else under it (7.7: "4–10 Nov (UTC)", 179 px in English: under
      // it); the snapshot's days, so the title alone with no figures shown
      // (statsview::weekHeaderDay()).
      h.title = tr(S::stats_last7);
      sv::Lifetime l;
      ui_.statsLifetime(l);
      const uint16_t first = sv::weekHeaderDay(weekNow(l).page.look, ui_.statsSnapshot());
      if (first != statsfold::kUnsetDay) st::weekSub(first, headerSub_, sizeof(headerSub_));
      break;
    }
  }
  if (crumb()) {
    h.right = tr(S::library_crumb);
    h.rightMaxW = uitext::kCrumbTextW;
    h.rightBack = true;
    h.pressed = headerPressed_ == 2;
  }
  return h;
}

ListView::Tap LibraryPage::statsTap(uint32_t row) {
  if (kind_ == PageKind::StatsShelf) {
    // A shelf's row (7.6): a track's bar (Play plays the shelf from it), an
    // album (it asks nothing), Try again's ask (Not played yet's: its lean
    // ask); a heading is no control (pressable()).
    const RowRef rr = statsRowAt(row);
    switch (rr.kind) {
      case RowKind::Ranked: return ListView::Tap::Expand;
      case RowKind::Album: ui_.push(page(PageKind::Album, rr.id)); return ListView::Tap::Handled;
      case RowKind::StatsTryAgain: {
        sv::Lifetime l;
        ui_.statsLifetime(l);
        const sv::ShelfLook k = shelfNow(l);
        Serial.printf("[ui] stats: Try again (\"%s\"): %s\n", tr(k.page.line),
                      k.ask == sv::UiAsk::NotPlayed ? "Not played yet's lean ask" : "the visit's ask");
        ui_.statsAsk(k.ask);
        return ListView::Tap::Handled;
      }
      default: return ListView::Tap::Handled;
    }
  }
  if (kind_ != PageKind::StatsHub) return ListView::Tap::Handled;  // (Week has no rows)
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::HubLook h = hubNow(l);
  const LibraryIndex* i = index();
  switch (sv::hubTap(h, row, l, i != nullptr, !real())) {
    case sv::HubTap::Ask:
      // The visit asked again (after No room, Unread, Capped, the card
      // back): queued, its phase on the rows from the next pass (the news).
      Serial.printf("[ui] stats: Try again (\"%s\")\n", tr(h.item[row].text));
      ui_.statsAsk(sv::UiAsk::Visit);
      return ListView::Tap::Handled;
    case sv::HubTap::Refused:
      // The stats gone off on the shown hub (4.4's Off reaches no shelf
      // and no Week): nothing pushed, the double buzz, as the entry row's.
      ui_.input().missBuzz();
      Serial.printf("[ui] stats: %s opens nothing now (the hub isn't reachable: the stats are off)\n",
                    kHubRowNames[static_cast<uint32_t>(h.item[row].row)]);
      return ListView::Tap::Handled;
    case sv::HubTap::Push: break;
    default: return ListView::Tap::Handled;  // (pressable() kept the others off)
  }
  const sv::HubItem& it = h.item[row];
  if (it.row == sv::HubRow::Week) {
    ui_.push(page(PageKind::StatsWeek, 0));
    return ListView::Tap::Handled;
  }
  sv::Shelf s;
  if (!sv::shelfOfRow(it.row, &s)) return ListView::Tap::Handled;
  // Most played opens on the snapshot's window (D7: a new visit's is All
  // time; none landed, All time). A push inside the visit asks nothing.
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  ui_.push(page(PageKind::StatsShelf, sv::shelfId(s, snap ? snap->window() : statsvisit::kAllTime)));
  return ListView::Tap::Handled;
}

sv::Control LibraryPage::statsControl(uint32_t row) {
  // A shelf's rows by their kind (statsview::shelfControlOf(): its tracks
  // and albums hold, its headings are no control).
  if (kind_ == PageKind::StatsShelf) return sv::shelfControlOf(shelfItemAt(row).kind);
  if (kind_ != PageKind::StatsHub) return sv::Control{};
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::HubLook h = hubNow(l);
  return row < h.rows ? sv::controlOf(h.item[row].kind) : sv::Control{};
}

bool LibraryPage::inert(uint32_t row) { return statsShown() && statsControl(row).inert; }

// A disabled hub row (4.5): the double buzz and its toast in amber.
void LibraryPage::onInertTap(uint32_t row) {
  if (kind_ != PageKind::StatsHub) return;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::HubLook h = hubNow(l);
  if (row >= h.rows || h.item[row].kind != sv::RowKind::Disabled) return;
  ui_.input().missBuzz();
  char text[128];
  st::toastText(h.item[row].toast, sv::opensDay(l), text, sizeof(text));
  Serial.printf("[ui] stats: %s is disabled: \"%s\"\n", kHubRowNames[row < sv::kHubRows ? row : sv::kHubRows], text);
  ui_.warn(text);
}

// The line under no rows: the hub's No card line (the card out: no Try
// again, no ask); a shelf's look's line (the phase's, its reason; a closed
// shelf's "Opens 6 Dec" from statstext::warmingLine() in Body 300, into
// headerPath_, which no stats page's header uses); Week's empty state's
// (uiS's: the list draws Week's body or its empty state, never this line).
const char* LibraryPage::statsEmptyText() {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  switch (kind_) {
    case PageKind::StatsHub: {
      const sv::PageLook k = sv::hubLook(l, p, snap).page;
      return k.look == sv::Look::NoCard ? tr(k.line) : "";
    }
    case PageKind::StatsShelf: {
      const sv::ShelfLook k = sv::shelfLook(id_, l, p, snap);
      if (!k.drawLine) return "";
      if (k.page.look == sv::Look::Closed && k.page.line == S::stats_warming) {
        st::warmingLine(Fonts::instance().fit(Font::Body), uitext::kListLineW, sv::opensDay(l), headerPath_,
                        sizeof(headerPath_));
        return headerPath_;
      }
      return tr(k.page.line);
    }
    default: {
      const sv::PageLook k = sv::weekLook(l, p, snap).page;
      return k.look == sv::Look::Served || k.look == sv::Look::Off || k.look == sv::Look::NoPlays ? "" : tr(k.line);
    }
  }
}

// The hub with nothing played (7.5, frame NoPlays): the chart on its disc,
// "No plays yet", the rule and "Your stats stay on this card", and Shuffle
// all (onEmptyAction()). Last 7 days when its ask came to nothing or the
// clock is unset (7.7 as decided): the chart on its disc, no title, the
// reason as its line (Small, 304 px), and Try again (Bold, 168 px) with
// Retry (the card back among it: the owner's call 3), none with no clock.
// (The card out is its body's lone No card line: weekBody().)
bool LibraryPage::statsEmptyState(EmptyState& e) {
  if (kind_ == PageKind::StatsShelf) return false;  // (its line, or its Try again row)
  sv::Lifetime l;
  ui_.statsLifetime(l);
  if (kind_ == PageKind::StatsWeek) {
    const sv::WeekLook w = weekNow(l);
    if (sv::weekBody(w.page.look)) return false;  // (its picture: drawBody())
    e.icon = &icons::kChart;
    e.iconColour = col::SOFT;
    e.title = "";
    e.lines = tr(w.page.line);
    if (w.button) e.buttons[0] = tr(S::empty_try_again);
    return true;
  }
  if (hubNow(l).page.look != sv::Look::NoPlays) return false;
  e.icon = &icons::kChart;
  e.iconColour = col::SOFT;
  e.title = tr(S::stats_empty_title);
  // Its two lines (the rule's short form where the long one is wider than
  // the line's room), into headerPath_: the hub's header has no path.
  snprintf(headerPath_, sizeof(headerPath_), "%s\n%s",
           tr(st::emptyRule(Fonts::instance().fit(Font::Small), uitext::kEmptyLineW)), tr(S::stats_empty_card));
  e.lines = headerPath_;
  e.buttons[0] = tr(S::empty_shuffle_all);
  e.buttonIcons[0] = &icons::kShuffle;
  return true;
}

// ---- uiS ----

void LibraryPage::printStats() {
  ListView& list = ui_.list();
  if (!statsShown()) {
    if (root() && segment() == LibrarySegment::Artists) {
      Serial.printf("[ui] stats: Artists' first row: %s%s%s\n",
                    entry_ == EntryRow::None ? "none (at its enter(): the stats off, the card out, behind the "
                                               "fence, uil's library, or no artists)"
                    : entry_ == EntryRow::Counting ? "the listening stats' row, folding: \""
                    : entry_ == EntryRow::Unread   ? "the listening stats' row, opening nothing (the fold ended with "
                                                     "the stats off): \""
                                                   : "the listening stats' row: \"",
                    entry_ == EntryRow::None ? "" : headerSub_, entry_ == EntryRow::None ? "" : "\"");
    } else {
      Serial.printf("[ui] stats: not a stats page (%s)\n", pageKindName(static_cast<uint8_t>(kind_)));
    }
    return;
  }
  sv::Lifetime l;
  ui_.statsLifetime(l);
  sv::PageLook look;
  const uint32_t sig = statsSigNow(l, &look);
  const bool attached = list.source() == this;
  Serial.printf("[ui] stats: %s %lu, look %s%s, drawn as 0x%06lX (now 0x%06lX); the list: %s%lu rows drawn (%lu "
                "now), %lu items, offset %ld px, open row %ld%s\n",
                pageKindName(static_cast<uint8_t>(kind_)), static_cast<unsigned long>(id_), sv::lookName(look.look),
                look.askDue ? " (an ask due)" : "", static_cast<unsigned long>(statsSigDrawn()),
                static_cast<unsigned long>(sig), attached ? "" : "NOT ATTACHED, ",
                static_cast<unsigned long>(attached ? list.layout().rows() : 0),
                static_cast<unsigned long>(statsRows()), static_cast<unsigned long>(attached ? list.layout().itemCount() : 0),
                static_cast<long>(attached ? list.offset() : 0), static_cast<long>(attached ? list.expanded() : -1),
                attached && list.stagingOnly() ? ", rows still staged" : "");
  // (Each page's part in a frame of its own, not stacked on this one: a
  // shelf's lines go through the same text chain as its rows.)
  if (kind_ == PageKind::StatsShelf) {
    printShelf(l);
  } else if (kind_ == PageKind::StatsHub) {
    printHub(l);
  } else {
    printWeek(l);
  }
}

__attribute__((noinline)) void LibraryPage::printHub(const sv::Lifetime& l) {
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  const sv::HubLook h = hubNow(l);
  if (h.rows == 0) {
    Serial.printf("[ui] stats:   no rows: %s\n",
                  h.page.look == sv::Look::NoPlays ? "the empty state (nothing played), Shuffle all" : statsEmptyText());
    return;
  }
  char line[160];
  for (uint32_t i = 0; i < h.rows; ++i) {
    const sv::HubItem& it = h.item[i];
    hubLineOf(it, l, snap, line, sizeof(line));
    Serial.printf("[ui] stats:   %lu %s, %s: \"%s\"\n", static_cast<unsigned long>(i + 1),
                  kHubRowNames[it.kind == sv::RowKind::TryAgain ? sv::kHubRows : static_cast<uint32_t>(it.row)],
                  rowKindName(it.kind), line);
  }
}

// ---- a shelf (7.6) ----

namespace {

// The console's names for the windows (in English, whatever the UI's
// language).
const char* const kWindowNames[3] = {"7 days", "30 days", "All time"};

const char* windowName(uint8_t w) { return w <= statsvisit::kAllTime ? kWindowNames[w] : "?"; }

}  // namespace

sv::ShelfLook LibraryPage::shelfNow(const sv::Lifetime& l) const {
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  return sv::shelfLook(id_, l, p, ui_.statsSnapshot());
}

sv::ShelfItem LibraryPage::shelfItemAt(uint32_t row, uint32_t* nowMin) const {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  if (nowMin) *nowMin = l.nowMin;
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  return sv::shelfItem(sv::shelfLook(id_, l, p, snap), snap, row);
}

LibraryPage::RowRef LibraryPage::statsRowAt(uint32_t row) const {
  RowRef r;
  const sv::ShelfItem it = shelfItemAt(row);
  switch (it.kind) {
    case sv::ShelfRow::Ranked:
      r.kind = RowKind::Ranked;
      r.id = it.id;     // (a track of the live index: the snapshot's key is its, D6)
      r.index = it.at;  // (its row in the snapshot's list: where a Play starts)
      break;
    case sv::ShelfRow::Section:
      r.kind = RowKind::Section;
      r.index = row;
      break;
    case sv::ShelfRow::TryAgain: r.kind = RowKind::StatsTryAgain; break;
    case sv::ShelfRow::Album: {
      // Its place in albumsAZ(): an album of the live index (the snapshot's
      // albums are its).
      const LibraryIndex* i = index();
      const LibraryIndex::Span az = i ? i->albumsAZ() : LibraryIndex::Span{};
      if (it.id >= az.count) break;
      r.kind = RowKind::Album;
      r.id = az[it.id];
      r.index = it.at;
      break;
    }
    default: break;
  }
  return r;
}

// A shelf's own rows (7.6): a ranked track (its rank "01" Small DIM at x 30,
// none on Recently played; the playing one's EQ glyph there, its title Bold
// violet; its line "artist cut, count kept" in what the edge leaves, 264 px
// beside the scrollbar), Recently played's heading as a "Disc 2" divider,
// Try again as the hub's. Not played yet's album is the Library's own row:
// `r` handed back for drawLibraryRow() (else nullptr).
ListView::Row* LibraryPage::drawShelfRow(ListView::Row& r) {
  uint32_t nowMin = 0;
  const sv::ShelfItem it = shelfItemAt(r.row, &nowMin);
  switch (it.kind) {
    case sv::ShelfRow::Album: return &r;  // (drawLibraryRow(): its cover, its artist line and year)
    case sv::ShelfRow::Section: {
      // "Today", "Sun 8 Nov", "This session": Bold violet at x 12, y 25,
      // its line under it (a divider, not a control).
      char t[64];
      st::sectionText(it.recent, t, sizeof(t));
      const int x = r.x + uitext::kDiscTextX;
      Fonts::instance().draw(r.c, Font::Bold, t, x, 25, uitext::kDiscTextW, accent::Library, r.bg);
      r.c.fillRect(x, 39, r.right - 8 - x, 1, col::DIV);
      return nullptr;
    }
    case sv::ShelfRow::TryAgain: {
      // empty.try_again over its reason, from x 54 as the hub's (7.4): no
      // icon, no chevron (it opens no page).
      const char* title = tr(S::empty_try_again);
      const char* reason = tr(it.text);
      ListView::lines(r, kTextX, r.right - 26, title, strlen(title), reason, strlen(reason), col::TXT);
      return nullptr;
    }
    case sv::ShelfRow::Ranked: break;
    default: return nullptr;  // (none: past the rows, or an id the index hasn't, D6)
  }
  const LibraryIndex* i = index();
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  if (!i || !snap) return nullptr;
  const bool now = playTrack_ != LibraryIndex::kNone && it.id == playTrack_;
  const int x = now                               ? ListView::playing(r, accent::Library)
                : it.list == statsrank::kRecent ? r.x + 40
                                                  : ListView::number(r, it.at + 1, col::DIM);
  char title[96];
  const size_t len = titleOf(it.id, title, sizeof(title));
  char line[160];
  st::rowLine(Fonts::instance().fit(Font::Small), r.right - 8 - x, it.list, snap->rowAt(it.list, it.at),
              librarytext::trackArtist(*i, it.id), it.recent.timed, nowMin, line, sizeof(line));
  ListView::lines(r, x, r.right - 8, title, len, line, strlen(line), now ? accent::Library : col::TXT,
                  now ? Font::Bold : Font::Body);
  return nullptr;
}

// The container's bar (7.6): with rows landed, but on Not played yet (its
// rows are albums) and over Try again.
bool LibraryPage::statsTopBar() {
  if (kind_ != PageKind::StatsShelf) return false;  // (the hub, Week: none)
  sv::Lifetime l;
  ui_.statsLifetime(l);
  return shelfNow(l).topBar;
}

// Not played yet's albums are A-Z (the bits' order, albumsAZ()'s): the rail
// over 30 of them, and the jump grid (its letters from railName(): the
// albums' sort names, as the Albums root's).
uint32_t LibraryPage::statsRailRows() {
  if (sv::shelfOf(id_) != sv::Shelf::NotPlayed) return 0;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::ShelfLook k = shelfNow(l);
  return k.page.look == sv::Look::Served && !k.tryAgain ? k.rows : 0;
}

int32_t LibraryPage::statsPlayingRow() const {
  if (kind_ != PageKind::StatsShelf || playTrack_ == LibraryIndex::kNone) return -1;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::ShelfLook k = shelfNow(l);
  // Not played yet: its album's place in albumsAZ() (once a push, at most
  // the index's albums).
  uint32_t place = statsvisit::kNoAlbum;
  const LibraryIndex* i = index();
  if (k.shelf == sv::Shelf::NotPlayed && i) {
    const LibraryIndex::Span az = i->albumsAZ();
    for (uint32_t p = 0; p < az.count; ++p) {
      if (az[p] == playAlbum_) {
        place = p;
        break;
      }
    }
  }
  return sv::shelfRowOf(k, ui_.statsSnapshot(), playTrack_, place);
}

bool LibraryPage::pending() {
  return kind_ == PageKind::StatsShelf && sv::shelfPending(sv::shelfOf(id_), ui_.statsSnapshot());
}

__attribute__((noinline)) bool LibraryPage::statsActIds(const RowRef& r, IdView* ids, const char** name) {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::ShelfLook k = shelfNow(l);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  const LibraryIndex* i = index();
  // Read now (D6): a snapshot served over the live index, the shelf's
  // ranked list, every id a track of it; a ranked row still the track it
  // was (a bar or a sheet never stays across a new ask: 7.2, so this is a
  // guard, said if it ever trips).
  const bool listed = snap && i && k.page.look == sv::Look::Served && !k.tryAgain && k.shelf != sv::Shelf::NotPlayed &&
                      sv::idsServed(*snap, k.list, i->trackCount());
  const bool same = r.kind != RowKind::Ranked ||
                    (listed && r.index < snap->rows(k.list) && snap->rowAt(k.list, r.index).id == r.id);
  if (!listed || !same) {
    Serial.printf("[ui] stats: %s: nothing acted on (%s)\n", tr(sv::shelfTitle(k.shelf)),
                  !snap || !i ? "no snapshot served over this index" : !listed ? "no ranked rows served" :
                                                                                "the row isn't the track it was");
    ui_.input().missBuzz();
    return false;
  }
  // The queue's view over the list's ids in place (7.6: nothing copied or
  // carved; the queue copies them, and only the Dance tab coming up takes
  // the block back, which no act does).
  *ids = IdView::u16(snap->ids(k.list), snap->rows(k.list), statsvisit::kStrides[k.list]);
  *name = tr(sv::shelfTitle(k.shelf));
  return true;
}

void LibraryPage::statsAct(const RowRef& r, int action) {
  // The view and the shelf's name only, in this frame across act() and
  // playNow()'s start: what was read to make them (the lifetime, the look)
  // is in statsActIds()'s, gone by then (main's album Play is onRowAction
  // then act(); a shelf's adds this frame alone).
  IdView ids;
  const char* name = "";
  if (!statsActIds(r, &ids, &name)) return;
  if (r.kind == RowKind::None) {
    act(ids, -1, action, name);  // the top bar: the whole shelf, named by its title
  } else if (action == 0) {
    act(ids, static_cast<int32_t>(r.index), 0, "");  // Play: the shelf from it
  } else {
    uint32_t one = r.id;
    act(IdView::of(&one, 1), -1, action, "");
  }
}

// ---- Most played's tray (7.6, 7.9) ----

bool LibraryPage::leadRow() {
  if (kind_ != PageKind::StatsShelf) return false;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  return shelfNow(l).tray;
}

// A CARD tray at (6, 7), 300 x 28; its cells' labels Bold (Small where Bold
// doesn't fit) centred on y 21, the picked one on a BTN_HI plate in TXT, the
// others DIM. A finger on a cell lights that cell alone (ROW_SEL), the
// tray's ground refilled, as the Filters row lights its cross.
void LibraryPage::drawLeadRow(ListView::Row& r) {
  if (kind_ != PageKind::StatsShelf) return;
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::ShelfLook k = shelfNow(l);
  const sv::Tray t = sv::trayOf(l, ui_.statsSnapshot());
  if (!k.tray || t.cells == 0) return;
  const int down = r.pressed ? sv::trayCellAt(t, r.pressX, r.pressX >= kW - 1) : -1;
  if (r.pressed) {
    r.bg = col::BG;
    r.c.fillSprite(r.bg);
  }
  r.c.fillRoundRect(sv::kTrayX, sv::kTrayY, sv::kTrayW, sv::kTrayH, sv::kTrayR, col::CARD);
  Fonts& f = Fonts::instance();
  const int w = sv::trayCellW(t), room = w - 4;
  for (int c = 0; c < t.cells; ++c) {
    const int x = sv::trayCellX(t, c);
    const bool picked = t.window[c] == k.window;
    uint16_t bg = col::CARD;
    if (picked || c == down) {
      bg = c == down ? col::ROW_SEL : col::BTN_HI;
      r.c.fillRoundRect(x, sv::kTrayPlateY, w, sv::kTrayPlateH, sv::kTrayPlateR, bg);
    }
    char label[48];
    st::trayLabel(t.window[c], t.days30, label, sizeof(label));
    f.draw(r.c, f.fitting(Font::Bold, label, room), label, x + w / 2, sv::kTrayLabelY, room,
           picked ? col::TXT : col::DIM, bg, Fonts::Align::Centre);
  }
}

// A cell's tap (after the tick): its window shown (7.6, 7.10). The window
// into the snapshot's header (D7: the hub's next push opens on it), the
// page's id and its ref (kept across the dark and a Back); the tray and the
// header drawn again in its pass; the window's rows staged in one a pass
// where the list stands, a bar folded (S2's rule for a tray tap). No ask.
// The lift pushed nothing (ListView's Tap on a lead row: this push is the
// tray's one, 7.10), so the [ui] line's ms is the pass's pushes.
void LibraryPage::onLeadRowTap(int x, bool rightEdge) {
  if (kind_ != PageKind::StatsShelf) return;
  const uint32_t t0 = micros();
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::ShelfLook k = shelfNow(l);
  const sv::Tray t = sv::trayOf(l, ui_.statsSnapshot());
  const int cell = k.tray ? sv::trayCellAt(t, x, rightEdge) : -1;
  if (cell < 0) return;
  const uint8_t w = t.window[cell];
  if (w == k.window) return;  // (the window shown: its tick said the tap was heard)
  ui_.statsSetWindow(w);
  id_ = sv::shelfId(sv::Shelf::MostPlayed, w);
  if (ref_) ref_->id = id_;
  ListView& list = ui_.list();
  list.refreshTopRow();
  repaintHeader();
  const uint32_t t1 = micros();
  keepSig(statsSigNow(l));  // (this tap's picture: not the next pass's to restage)
  list.recountStaged(-1, -1, 0, false);
  Serial.printf("[ui] stats: Most played, the tray: %s (was %s): the tray and the header in %.1f ms, %lu rows to "
                "stage one a pass\n",
                windowName(w), windowName(k.window), (t1 - t0) / 1000.0f,
                static_cast<unsigned long>(list.layout().rows()));
}

// ---- a shelf following what it reads ----

void LibraryPage::shelfFollow(uint32_t drawn, uint32_t sig, const sv::Lifetime& l, const char* why) {
  ListView& list = ui_.list();
  if (list.source() != this) return;  // (not attached: drawn whole at its next enter())
  const uint8_t was = static_cast<uint8_t>(drawn >> 16), now = static_cast<uint8_t>(sig >> 16);
  const bool textsMoved = ((drawn ^ sig) & 0xFFFFu) != 0;
  // Most played's window line (a landing's D7, a day that drops a kept 30
  // days; at the 30 days a capped landing's "16 days"): the header again
  // while the page shows (else drawn whole when it does).
  if (sv::shelfHeaderMoved(was, now, textsMoved) && ui_.pageShowing()) repaintHeader();
  const sv::ShelfLook k = shelfNow(l);
  // D7: the window shown is the snapshot's from here (a new ask's loan
  // opened on All time), at a landing with rows or with none in the window
  // ("Nothing in this period.": no arrival, but the hub's next push still
  // opens on what the page shows).
  if (sv::windowOwed(k, ui_.statsSnapshot())) ui_.statsSetWindow(k.window);
  const bool lead = sv::trayMoved(was, now, textsMoved);
  minute_ = static_cast<uint16_t>(l.nowMin);  // (Recently played's times drawn with the rest)
  int32_t target = -1;
  const bool arrives = sv::shelfArrives(static_cast<sv::Look>(was & sv::kShelfLook), k.page.look, k.rows);
  if (arrives && ref_) {
    // The place kept in the ref while the rows were pending (7.2), clamped
    // to them; no bar reopened across a new ask; kShowPlaying, the playing
    // row with one of context above it, as enter() puts it.
    ListLayout to;
    to.set(k.rows, k.topBar, k.tray);
    const listlayout::Place place =
        listlayout::arrival(ref_->scrollPx, ref_->expanded, k.rows, to.leads(), false, list.height());
    target = place.offset;
    if (target == kShowPlaying) {
      target = 0;
      const int32_t row = playingRow();
      if (row >= 0) {
        const uint32_t item = to.itemOfRow(static_cast<uint32_t>(row));
        if (to.reveal(item, 0, list.height()) != 0) target = (static_cast<int32_t>(item) - 1) * ListLayout::kPitch;
      }
    }
  }
  // The tray alone moved (a new UTC day: two cells three, or the tray
  // coming on day 7; S10's departure 3, at once rather than 7.10's next
  // enter()): every row is the one it was, so the bar stays open, a press
  // stays on an item still where it was, and no row is owed for itself
  // (changedRow past them). A tray that comes still moves the items under
  // it down by one, staged one a pass (ListView stages each item at the
  // place it had: listlayout::staging()).
  const bool alone = sv::trayAlone(was, now, textsMoved);
  Serial.printf("[ui] stats: %s drawn again in place (%s): %s, %lu rows%s%s\n",
                pageKindName(static_cast<uint8_t>(kind_)), why, sv::lookName(k.page.look),
                static_cast<unsigned long>(k.rows), arrives ? ", arriving at the place kept" : "",
                alone ? ", the tray alone" : lead ? ", the tray too" : "");
  // One stale item a pass at a still pass (7.2, 7.10). Else: every row new
  // (0), a bar folded (-1: across a new ask none comes back), the tray only
  // when its picture moved.
  if (alone) {
    list.recountStaged(-1, list.expanded(), k.rows, lead, true);
  } else {
    list.recountStaged(target, -1, 0, lead);
  }
}

void LibraryPage::recentMinute(const sv::Lifetime& l, uint32_t was) {
  if (sv::shelfOf(id_) != sv::Shelf::Recent) return;
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  ListView& list = ui_.list();
  uint32_t first = 0, last = 0;
  if (!snap || list.source() != this || !list.visibleRows(&first, &last)) return;
  const sv::ShelfLook k = shelfNow(l);
  if (k.page.look != sv::Look::Served || k.tryAgain) return;
  const uint32_t row = st::recentMoved(*snap, first, last, was, l.nowMin);
  if (row == st::kNoneMoved) return;
  // From that row down, in place, one a pass: the same rows (a press kept),
  // a bar where it is.
  list.recountStaged(-1, list.expanded(), row, false, true);
}

// uiS on a shelf: its look, the tray, the header's line, its rows (a
// checksum of every one's id and figures, so two uiS can be compared: run
// 3b-6), and the rows on screen as drawn.
__attribute__((noinline)) void LibraryPage::printShelf(const sv::Lifetime& l) {
  const sv::ShelfLook k = shelfNow(l);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  const sv::Tray t = sv::trayOf(l, snap);
  char text[160];
  st::shelfSub(k.shelf, k.window, t.days30, l.sinceDay, text, sizeof(text));
  Serial.printf("[ui] stats:   %s, window %s, the header's line \"%s\"; %lu rows%s%s%s%s\n",
                kHubRowNames[static_cast<uint32_t>(k.shelf) + 1], windowName(k.window), text,
                static_cast<unsigned long>(k.rows), k.tray ? ", the tray" : "", k.topBar ? ", the top bar" : "",
                k.tryAgain ? ", Try again" : "", pending() ? " (pending: its place kept in its ref)" : "");
  if (k.tray) {
    for (int c = 0; c < t.cells; ++c) {
      st::trayLabel(t.window[c], t.days30, text, sizeof(text));
      Serial.printf("[ui] stats:     the tray's cell %d, %s: \"%s\"%s\n", c + 1, windowName(t.window[c]), text,
                    t.window[c] == k.window ? " (picked)" : "");
    }
  }
  if (k.rows == 0 || k.drawLine) {
    Serial.printf("[ui] stats:   its line: \"%s\"\n", statsEmptyText());
    return;
  }
  if (snap && !k.tryAgain) {
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint32_t v) { hash = (hash ^ v) * 16777619u; };
    if (k.shelf == sv::Shelf::NotPlayed) {
      for (uint32_t r = 0; r < k.rows; ++r) mix(snap->albumAt(r));
    } else {
      for (uint32_t r = 0; r < snap->rows(k.list); ++r) {
        const statsvisit::Row row = snap->rowAt(k.list, r);
        mix(row.id);
        mix(row.count);
        mix(row.lastDay);
        mix(row.when);
      }
    }
    Serial.printf("[ui] stats:   the snapshot's version %lu; its %s's ids and figures 0x%08lX\n",
                  static_cast<unsigned long>(snap->version()),
                  k.shelf == sv::Shelf::NotPlayed ? "albums" : "rows", static_cast<unsigned long>(hash));
  }
  ListView& list = ui_.list();
  uint32_t first = 0, last = 0;
  if (list.source() != this || !list.visibleRows(&first, &last)) return;
  const LibraryIndex* i = index();
  for (uint32_t r = first; r <= last; ++r) {
    uint32_t nowMin = 0;
    const sv::ShelfItem it = shelfItemAt(r, &nowMin);
    char title[64] = "";
    text[0] = 0;
    switch (it.kind) {
      case sv::ShelfRow::Ranked:
        titleOf(it.id, title, sizeof(title));
        if (snap && i) {
          st::rowLine(Fonts::instance().fit(Font::Small), st::kRowLineW, it.list, snap->rowAt(it.list, it.at),
                      librarytext::trackArtist(*i, it.id), it.recent.timed, nowMin, text, sizeof(text));
        }
        Serial.printf("[ui] stats:     %lu, track %lu (rank %lu): \"%s\", \"%s\"\n", static_cast<unsigned long>(r + 1),
                      static_cast<unsigned long>(it.id), static_cast<unsigned long>(it.at + 1), title, text);
        break;
      case sv::ShelfRow::Section:
        st::sectionText(it.recent, title, sizeof(title));
        Serial.printf("[ui] stats:     %lu, the heading \"%s\"\n", static_cast<unsigned long>(r + 1), title);
        break;
      case sv::ShelfRow::Album: {
        const LibraryIndex::Span az = i ? i->albumsAZ() : LibraryIndex::Span{};
        Serial.printf("[ui] stats:     %lu, album %lu: \"%s\"\n", static_cast<unsigned long>(r + 1),
                      static_cast<unsigned long>(it.id < az.count ? az[it.id] : 0),
                      it.id < az.count ? librarytext::albumShown(*i, az[it.id]) : "");
        break;
      }
      case sv::ShelfRow::TryAgain:
        Serial.printf("[ui] stats:     %lu, Try again: \"%s\"\n", static_cast<unsigned long>(r + 1), tr(it.text));
        break;
      default:
        Serial.printf("[ui] stats:     %lu, nothing (%s)\n", static_cast<unsigned long>(r + 1),
                      sv::shelfRowName(it.kind));
        break;
    }
  }
}

// ---- Last 7 days (7.7) ----

namespace {

// A line of `f` centred on screen row `y` reaches the strip of `h` rows
// from screen row `top` (the sprite clips the rest; a line not there isn't
// drawn at all).
bool lineIn(Font f, int y, int top, int h) {
  const int half = Fonts::instance().height(f) / 2 + 1;
  return y + half > top && y - half < top + h;
}

// The columns, oldest first; the last is the snapshot's today.
constexpr int kWeekDays = static_cast<int>(statsrank::kWeekDays);

}  // namespace

sv::WeekLook LibraryPage::weekNow(const sv::Lifetime& l) const {
  sv::PhaseInfo p;
  ui_.statsPhase(p);
  return sv::weekLook(l, p, ui_.statsSnapshot());
}

bool LibraryPage::drawBody(M5Canvas& c, int y0, int h) { return kind_ == PageKind::StatsWeek && weekBody(c, y0, h); }

// The page's picture (frame Week; 7.7 as decided), its figures the
// snapshot's week as of the ask (no fold moves it): Served, A, B and C;
// Pending, the phase's line alone in A, as a list's lone line ("Counting
// plays…", "Waiting for the card…"), and the card out its No card line the
// same way (the hub's and the shelves' one line: the owner's call 3); Off
// and nothing played, the band blank.
// Each of the three 56-row strips (the shared strip: not kept between uses)
// is filled, has whatever of the picture falls in it drawn at its offset,
// as drawEmptyState() does, and is pushed: every line of the band, so the
// list fills none under it (about 36 ms with the header, 7.10). The
// snapshot read now (D6), each draw.
bool LibraryPage::weekBody(M5Canvas& c, int y0, int h) {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::WeekLook w = weekNow(l);
  if (!sv::weekBody(w.page.look)) return false;  // (the empty state: statsEmptyState())
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  const bool served = w.page.look == sv::Look::Served && snap;
  sv::WeekFigures f;
  if (served) f = sv::weekFigures(*snap);
  for (int sy = 0; sy < h; sy += gfx::kStripH) {
    const int sh = std::min(gfx::kStripH, h - sy), top = y0 + sy;
    c.fillRect(0, 0, kW, gfx::kStripH, col::BG);
    if (served) {
      weekStrip(c, top, sh, f);
      weekLines(c, top, sh, *snap, f);
    } else if (sv::weekBodyLine(w.page.look) && lineIn(Font::Body, st::kWeekPendingY, top, sh)) {
      Fonts::instance().draw(c, Font::Body, tr(w.page.line), kW / 2, st::kWeekPendingY - top, uitext::kListLineW,
                             col::DIM, col::BG, Fonts::Align::Centre);
    }
    gfx::push(c, 0, top, kW, sh);
  }
  return true;
}

// A and B in the strip from screen row `top`: "Listening time" (Small DIM,
// x 16) and "Plays" (right at x 304) over the week's time (ui::drawStat():
// its numbers in Title, its words in Small) and the plays' count (Title,
// right at x 304), standing on one baseline; the bars on their DIV baseline
// (past days DIM, today violet; none before the first day counted, nor for
// a day with nothing) and each day's name under its bar (today's TXT).
void LibraryPage::weekStrip(M5Canvas& c, int top, int h, const sv::WeekFigures& w) {
  Fonts& f = Fonts::instance();
  if (lineIn(Font::Small, st::kWeekLabelY, top, h)) {
    f.draw(c, Font::Small, tr(S::stats_listened), st::kWeekValueX, st::kWeekLabelY - top, st::kListenedW, col::DIM,
           col::BG);
    f.draw(c, Font::Small, tr(S::stats_plays_label), st::kWeekPlaysRight, st::kWeekLabelY - top, st::kPlaysLabelW,
           col::DIM, col::BG, Fonts::Align::Right);
  }
  if (lineIn(Font::Title, st::kWeekCountY, top, h)) {
    st::StatValue v;
    st::statValue(w.listenedMs, &v);
    char plays[24];
    st::weekPlays(w.plays, plays, sizeof(plays));
    // The count in the room the value leaves (it fits beside it in every
    // language: test_ui_library), its line's ground filled; then the value
    // glyphs only over the filled strip (7.7), beside it.
    const int valueW = st::statWidth(v, f.fit(Font::Title), f.fit(Font::Small));
    f.draw(c, Font::Title, plays, st::kWeekPlaysRight, st::kWeekCountY - top, st::weekCountRoom(valueW), col::TXT,
           col::BG, Fonts::Align::Right);
    drawStat(c, st::kWeekValueX, st::kWeekValueBase - top, v);
  }
  if (st::kWeekBaseY - sv::kWeekBarPx < top + h && st::kWeekBaseY >= top) {
    for (int i = 0; i < kWeekDays; ++i) {
      const int bar = w.bar[i];
      if (bar <= 0) continue;
      c.fillRect(st::weekBarX(i), st::kWeekBaseY - bar - top, st::kWeekBarW, bar,
                 i == kWeekDays - 1 ? accent::Library : col::DIM);
    }
    c.fillRect(st::kWeekValueX, st::kWeekBaseY - top, st::kWeekLineW, 1, col::DIV);
  }
  // (No day to name: a snapshot of 2026's first six days, weekFirstDay().
  // An undated one isn't drawn: with no clock Week is Closed, and one whose
  // clock was set since its ask is released first, Ui::statsUndated().)
  if (w.firstDay != statsfold::kUnsetDay && lineIn(Font::Small, st::kWeekDayY, top, h)) {
    for (int i = 0; i < kWeekDays; ++i) {
      const S day = st::dayShort(static_cast<uint16_t>(w.firstDay + i));
      f.draw(c, Font::Small, tr(day), st::weekDayX(i), st::kWeekDayY - top, st::kDayNameW,
             i == kWeekDays - 1 ? col::TXT : col::DIM, col::BG, Fonts::Align::Centre);
    }
  }
}

// C in the strip from screen row `top`: the DIV rule, the big day (Small
// SOFT: "Saturday was your big day: 2 h 1 min"; none with nothing in the
// week; cut with "…" where a language's line is wider than 288 px, as pl,
// ja and zh are until their translations, S13) and Auto DJ's rate (Small
// DIM, only when it started 5 tracks in the week: D11).
void LibraryPage::weekLines(M5Canvas& c, int top, int h, const statsvisit::Snapshot& s,
                            const sv::WeekFigures& w) {
  if (st::kWeekRuleY >= top && st::kWeekRuleY < top + h) {
    c.fillRect(st::kWeekValueX, st::kWeekRuleY - top, st::kWeekLineW, 1, col::DIV);
  }
  Fonts& f = Fonts::instance();
  char line[160];
  if (lineIn(Font::Small, st::kWeekLine1Y, top, h) && st::weekBigDay(s, w, line, sizeof(line))) {
    f.draw(c, Font::Small, line, st::kWeekValueX, st::kWeekLine1Y - top, st::kWeekLineW, col::SOFT, col::BG);
  }
  if (lineIn(Font::Small, st::kWeekLine2Y, top, h) && st::weekAutoDj(w, line, sizeof(line))) {
    f.draw(c, Font::Small, line, st::kWeekValueX, st::kWeekLine2Y - top, st::kWeekLineW, col::DIM, col::BG);
  }
}

// What Week draws moved (a landing, the card pulled or back, a phase's
// line, a new ask's snapshot): its header when its days did (the figures
// coming or going, a new snapshot), while the page shows (else drawn whole
// when it does); the band whole at the next still pass, its body or its
// empty state (a recount of no items to none owes the list's drawEmpty()
// alone: about 31 ms, three strips). Under a modal or in the dark, drawn
// whole when the page shows again.
void LibraryPage::weekFollow(uint32_t drawn, uint32_t sig, sv::Look now, const char* why) {
  ListView& list = ui_.list();
  if (list.source() != this) return;  // (not attached: drawn whole at its next enter())
  const bool header = sv::weekHeaderMoved(static_cast<uint16_t>(drawn), static_cast<uint16_t>(sig));
  if (header && ui_.pageShowing()) repaintHeader();
  Serial.printf("[ui] stats: %s drawn again in place (%s): %s%s\n", pageKindName(static_cast<uint8_t>(kind_)), why,
                sv::lookName(now), header ? ", its header's days too" : "");
  list.recountStaged(-1, -1, 0, false);
}

// The empty state's Try again (after No room, Unread, Capped, the card
// back): the visit asked again, its phase on the page from the next pass
// (the news). A look that moved under the finger (no button now) asks
// nothing.
void LibraryPage::weekTryAgain() {
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::WeekLook w = weekNow(l);
  if (!w.button) return;
  Serial.printf("[ui] stats: Last 7 days, Try again (\"%s\")\n", tr(w.page.line));
  ui_.statsAsk(sv::UiAsk::Visit);
}

// uiS on Week: what it draws (the empty state and its button; Pending's
// line; nothing), or its header's line, A's figures, B's bars and C's
// lines as drawn.
__attribute__((noinline)) void LibraryPage::printWeek(const sv::Lifetime& l) {
  const sv::WeekLook w = weekNow(l);
  const statsvisit::Snapshot* snap = ui_.statsSnapshot();
  if (!sv::weekBody(w.page.look)) {
    Serial.printf("[ui] stats:   the empty state: \"%s\"%s\n", tr(w.page.line),
                  w.button ? ", Try again" : ", no button");
    return;
  }
  if (w.page.look != sv::Look::Served || !snap) {
    const bool line = sv::weekBodyLine(w.page.look);
    Serial.printf("[ui] stats:   %s%s%s\n", line ? "A: \"" : "nothing drawn (no figures, no line)",
                  line ? tr(w.page.line) : "", line ? "\"" : "");
    return;
  }
  const sv::WeekFigures f = sv::weekFigures(*snap);
  char text[160], time[48], plays[24];
  text[0] = 0;
  if (f.firstDay != statsfold::kUnsetDay) st::weekSub(f.firstDay, text, sizeof(text));
  sv::timeText(f.listenedMs, time, sizeof(time));
  st::weekPlays(f.plays, plays, sizeof(plays));
  Serial.printf("[ui] stats:   the header's line \"%s\"; A: \"%s\", %s plays; B (px, oldest first): %u %u %u %u %u %u "
                "%u\n",
                text, time, plays, static_cast<unsigned>(f.bar[0]), static_cast<unsigned>(f.bar[1]),
                static_cast<unsigned>(f.bar[2]), static_cast<unsigned>(f.bar[3]), static_cast<unsigned>(f.bar[4]),
                static_cast<unsigned>(f.bar[5]), static_cast<unsigned>(f.bar[6]));
  st::weekBigDay(*snap, f, text, sizeof(text));
  Serial.printf("[ui] stats:   C: \"%s\"", text);
  st::weekAutoDj(f, text, sizeof(text));
  Serial.printf(", \"%s\"\n", text);
}

}  // namespace ui
