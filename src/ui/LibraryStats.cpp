// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Library's stats half (docs/LISTENING-STATS.md 7.1-7.5, 7.10, 7.11;
// S3b's C22): the Artists root's entry row, the hub and its empty state,
// and what a stats page does each pass (its shape, its minute, the stats'
// news). LibraryPage's shape is in Pages.h. What a page shows and its words
// are lib/core's (statsview, statstext), host-tested in test_ui_stats; this
// reads the host through Ui's accessors (the lifetime, the phase, the
// snapshot) at each draw and act, and keeps nothing of them but the shape
// it drew and the minute. The shelves' rows and Last 7 days' body come with
// S10 and S11: until then they draw their header and their look's line.
#include <cstdio>
#include <cstring>

#include "StatsRank.h"
#include "StatsText.h"
#include "StatsView.h"
#include "UiStrings.h"
#include "UiText.h"
#include "ui/Fonts.h"
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
    case PageKind::StatsShelf:
      k = sv::shelfLook(id_, l, p, snap).page;
      shape = static_cast<uint8_t>(k.look);
      break;
    default:
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
  // every row new (0), no lead item (the hub has none; a shelf's tray is
  // S10's). Under a modal or in the dark the page is drawn whole when it
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
  // gone). Week's own empty state is S11's.
  if (kind_ == PageKind::StatsHub &&
      sv::hubDrawsWhole(static_cast<sv::Look>((drawn >> 16) & 7u), look.look)) {
    list.invalidate();
  }
}

bool LibraryPage::statsNews(const char* why) {
  // A page that drew the snapshot's rows (a shelf's: S10) has none now:
  // attached again at 0 rows, by the Ui (what reads the snapshot closed
  // first). The hub and Week read the snapshot at each draw: drawn again.
  if (kind_ == PageKind::StatsShelf && ui_.list().source() == this && ui_.list().layout().rows() > 0 &&
      !ui_.statsSnapshot()) {
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
  // text moved, at a still pass.
  const uint16_t now = static_cast<uint16_t>(l.nowMin);
  if (kind_ != PageKind::StatsHub || now == minute_ || ui_.list().animating()) return;
  const uint32_t was = sv::minuteFrom(minute_, l.nowMin);
  minute_ = now;
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
  Serial.printf("[ui] stats: %s: item %u staged in place\n", name, static_cast<unsigned>(c.item));
}

// ---- the list ----

uint32_t LibraryPage::statsRows() {
  if (kind_ != PageKind::StatsHub) return 0;  // (a shelf's rows: S10; Week has none, its body: S11)
  sv::Lifetime l;
  ui_.statsLifetime(l);
  return hubNow(l).rows;
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
  if (kind_ != PageKind::StatsHub) return;  // (a shelf's rows: S10)
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
    case PageKind::StatsShelf: h.title = tr(sv::shelfTitle(sv::shelfOf(id_))); break;  // (its window's line: S10)
    default: h.title = tr(S::stats_last7); break;                                      // (its days: S11)
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
  if (kind_ != PageKind::StatsHub) return ListView::Tap::Handled;  // (a shelf's rows: S10)
  sv::Lifetime l;
  ui_.statsLifetime(l);
  const sv::HubLook h = hubNow(l);
  if (row >= h.rows) return ListView::Tap::Handled;
  const sv::HubItem& it = h.item[row];
  if (it.kind == sv::RowKind::TryAgain) {
    // The visit asked again (after No room, Unread, Capped, the card back):
    // queued, its phase on the rows from the next pass (the news).
    Serial.printf("[ui] stats: Try again (\"%s\")\n", tr(it.text));
    ui_.statsAsk(sv::UiAsk::Visit);
    return ListView::Tap::Handled;
  }
  if (it.kind != sv::RowKind::Open) return ListView::Tap::Handled;  // (pressable() kept the others off)
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
  if (kind_ != PageKind::StatsHub) return sv::Control{};  // (a shelf's rows: S10)
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
// headerPath_, which no stats page's header uses); Week's (S11 draws it).
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
// all (onEmptyAction()).
bool LibraryPage::statsEmptyState(EmptyState& e) {
  if (kind_ != PageKind::StatsHub) return false;  // (Week's Try again: S11)
  sv::Lifetime l;
  ui_.statsLifetime(l);
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
  if (kind_ != PageKind::StatsHub) {
    Serial.printf("[ui] stats:   its line: \"%s\"\n", statsEmptyText());
    return;
  }
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

}  // namespace ui
