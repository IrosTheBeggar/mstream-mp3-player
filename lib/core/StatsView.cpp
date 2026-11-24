// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsView.h"

#include "FilterGrid.h"
#include "WallClock.h"

namespace statsview {

namespace sf = statsfold;
namespace sr = statsrank;
namespace sv = statsvisit;

namespace {

constexpr uint32_t kDayMin = 1440;  // a `when`'s minutes a UTC day
constexpr uint32_t kTrayDays = 7;   // the tray from a week of counting (7.6)

bool isCode(uint32_t when) { return when == sv::kWhenSession || when == sv::kWhenEarlier; }

// Dated and a month of counting (30 days, as Not played yet: call 4): the
// first month's rows open (New finds, Old favourites) and the state leaves
// First month. One test for all three places, from RAM in every state.
bool pastMonth(const Lifetime& l) { return l.dated && sv::notPlayedDays(l.countingDays); }

}  // namespace

// ---------------------------------------------------------------------------
// The host's lifetime
// ---------------------------------------------------------------------------
Lifetime lifetimeOf(bool on, bool folding, bool cardIn, const sf::Head& head, uint16_t sinceDay, uint64_t nowMs) {
  constexpr uint64_t kMinuteMs = 60000;
  Lifetime l;
  l.on = on;
  l.folding = folding;
  l.cardIn = cardIn;
  l.dated = sf::todayOf(nowMs, &l.today);  // (today left kUnsetDay undated)
  l.sinceDay = sinceDay;
  l.countingDays = sv::countingDaysOf(l.dated, l.today, sinceDay);
  l.plays = head.plays;
  l.listenedMs = head.listenedMs;
  l.nowMin = l.dated ? static_cast<uint32_t>((nowMs - sf::kMinMs) / kMinuteMs) : 0;
  return l;
}

// ---------------------------------------------------------------------------
// The states
// ---------------------------------------------------------------------------
State stateOf(const Lifetime& l) {
  if (!l.on) return State::Off;
  if (!l.cardIn) return State::NoCard;
  if (l.folding) return State::Folding;
  if (l.plays == 0) return State::Empty;
  if (!l.dated) return State::NoDates;
  if (!pastMonth(l)) return State::FirstMonth;
  return State::Ready;
}

const char* stateName(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::NoCard: return "no card";
    case State::Folding: return "folding";
    case State::Empty: return "empty";
    case State::NoDates: return "no dates";
    case State::FirstMonth: return "first month";
    case State::Ready: return "ready";
  }
  return "?";
}

uint16_t opensDay(const Lifetime& l) {
  const uint16_t from = l.sinceDay != sf::kUnsetDay ? l.sinceDay : l.dated ? l.today : sf::kUnsetDay;
  const uint32_t day = static_cast<uint32_t>(from) + sv::kNotPlayedDays - 1;
  return from == sf::kUnsetDay || day >= sf::kUnsetDay ? sf::kUnsetDay : static_cast<uint16_t>(day);
}

// ---------------------------------------------------------------------------
// The counting days a page shows
// ---------------------------------------------------------------------------
static_assert(kDaysMask >= sv::kNotPlayedDays && kDaysMask >= kTrayDays,
              "the days held keep the month's and the tray's edges apart");

bool daysWhole(const Lifetime& l) { return l.on && l.cardIn && !l.folding && l.countingDays > 0; }

uint8_t latchDays(uint8_t held, const Lifetime& l, bool entering) {
  const uint8_t days = static_cast<uint8_t>(l.countingDays < kDaysMask ? l.countingDays : kDaysMask);
  if (!daysWhole(l)) {
    // (Kept, to be taken at the first whole reading; none held: its own.)
    return static_cast<uint8_t>((held & kDaysHeld) ? held | kDaysRetake : kDaysHeld | kDaysRetake | days);
  }
  if (entering || !(held & kDaysHeld) || (held & kDaysRetake)) return static_cast<uint8_t>(kDaysHeld | days);
  return held;
}

void holdDays(Lifetime& l, uint8_t held) {
  if (held & kDaysHeld) l.countingDays = held & kDaysMask;
}

// ---------------------------------------------------------------------------
// The look
// ---------------------------------------------------------------------------
const char* lookName(Look k) {
  switch (k) {
    case Look::Served: return "served";
    case Look::Pending: return "pending";
    case Look::Retry: return "retry";
    case Look::NoCard: return "no card";
    case Look::Off: return "off";
    case Look::NoPlays: return "no plays";
    case Look::Closed: return "closed";
  }
  return "?";
}

S phaseLine(Phase p) {
  switch (p) {
    case Phase::Waiting:
    case Phase::Capped: return S::stats_waiting;
    case Phase::NoRoom: return S::stats_no_room;
    case Phase::Unread: return S::stats_unread;
    case Phase::NoCard: return S::stats_no_card;
    default: return S::stats_counting;
  }
}

PageLook pageLook(const Lifetime& l, const PhaseInfo& p, bool served) {
  PageLook k;
  const State st = stateOf(l);
  if (st == State::Off) {
    k.look = Look::Off;
    return k;
  }
  if (st == State::NoCard) {
    k.look = Look::NoCard;
    k.line = S::stats_no_card;
    return k;
  }
  if (st == State::Empty) {
    k.look = Look::NoPlays;
    return k;
  }
  if (served) {
    k.look = Look::Served;
    return k;
  }
  switch (p.phase) {
    case Phase::Working:
    case Phase::Waiting:
      k.look = Look::Pending;
      k.line = phaseLine(p.phase);
      break;
    case Phase::NoRoom:
    case Phase::Unread:
    case Phase::Capped:
      k.look = Look::Retry;
      k.line = phaseLine(p.phase);
      break;
    case Phase::NoCard:
      // The card in again (stateOf() read it in), the phase No card until
      // the next ask: Try again, "The card is back" (the owner's call 3).
      k.look = Look::Retry;
      k.line = S::stats_card_back;
      break;
    default:
      // Idle (a Cancelled ask, a landing released since; the stats off is
      // Off above) and Ready over nothing served: the Working line and one
      // ask (D9 as replaced: never "Couldn't read the stats", a read
      // fault's alone); none under a made-up view (D16).
      k.look = Look::Pending;
      k.line = S::stats_counting;
      k.askDue = !l.madeUp;
      break;
  }
  return k;
}

bool asksOnEdge(const Lifetime& l) {
  if (l.madeUp) return false;
  const State st = stateOf(l);
  return st != State::Off && st != State::NoCard && st != State::Empty;
}

bool risesCardOut(const Lifetime& l) { return !l.madeUp && stateOf(l) == State::NoCard; }

PhaseInfo phaseSeen(const PhaseInfo& host, bool roseCardOut) {
  PhaseInfo p = host;
  if (roseCardOut && host.phase == Phase::Idle) {
    p.phase = Phase::NoCard;
    p.outcome = Outcome::CardAway;
  }
  return p;
}

// ---------------------------------------------------------------------------
// The held condition
// ---------------------------------------------------------------------------
bool onStatsPages(const NavModel& nav, bool dark, bool suspended, bool synthetic) {
  if (dark || suspended || synthetic || nav.tab() != NavModel::Tab::Library) return false;
  const int depth = nav.depth(NavModel::Tab::Library);
  for (int level = 0; level < depth; ++level)
    if (statsKind(nav.at(NavModel::Tab::Library, level).kind)) return true;
  return false;
}

bool undatedStale(const Lifetime& l, const Snapshot* snap) {
  return snap && l.dated && !l.madeUp && !snap->dated() && !snap->madeUp();
}

// ---------------------------------------------------------------------------
// The entry row
// ---------------------------------------------------------------------------
bool entryDue(const Lifetime& l, bool indexReady, bool synthetic, uint32_t artists) {
  const State st = stateOf(l);
  return st != State::Off && st != State::NoCard && indexReady && !synthetic && artists > 0;
}

size_t timeText(uint64_t ms, char* out, size_t size) {
  const uint64_t min = ms / 60000;
  if (min < 60) return uistr::trf(out, size, S::format_minutes, uistr::Arg(static_cast<unsigned long long>(min)));
  return uistr::trf(out, size, S::format_hours_minutes, uistr::grouped(min / 60),
                    uistr::Arg(static_cast<unsigned>(min % 60)));
}

size_t dayMonthText(uint16_t day, char* out, size_t size) {
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs);
  const int month = c.month >= 1 && c.month <= 12 ? c.month : 1;
  const S m = static_cast<S>(static_cast<int>(S::month_jan) + month - 1);
  return uistr::trf(out, size, S::format_day_month, uistr::Arg(c.day), uistr::tr(m));
}

EntryLine entryLine(const Lifetime& l, WidthFn width, void* ctx, char* out, size_t size) {
  // The lifetime's own fields, not stateOf(): its No card comes before
  // Folding and Empty, and the card out hides neither here.
  if (!l.on) {
    // (A row latched while folding, the fold ended with the stats off: A20.)
    uistr::copy(out, size, uistr::tr(S::stats_unread));
    return EntryLine::Unread;
  }
  if (l.folding) {
    uistr::copy(out, size, uistr::tr(S::stats_counting));
    return EntryLine::Counting;
  }
  if (l.plays == 0) {
    uistr::copy(out, size, uistr::tr(S::stats_entry_empty));
    return EntryLine::Empty;
  }
  char time[48];
  timeText(l.listenedMs, time, sizeof(time));
  if (l.sinceDay != sf::kUnsetDay) {
    char day[32];
    dayMonthText(l.sinceDay, day, sizeof(day));
    uistr::trf(out, size, S::stats_since, time, day);
    if (!width || width(out, ctx) <= kEntryRoom) return EntryLine::Since;
  }
  uistr::copy(out, size, time);
  return EntryLine::Time;
}

// ---------------------------------------------------------------------------
// The hub
// ---------------------------------------------------------------------------
namespace {

// An open row's line by the look: the snapshot's, the phase's, else none.
void openLine(const PageLook& k, HubItem* it) {
  it->kind = RowKind::Open;
  if (k.look == Look::Served) {
    it->line = Line::Data;
  } else if (k.look == Look::Pending) {
    it->line = Line::Text;
    it->text = k.line;
  } else {
    it->line = Line::None;
  }
}
void disabled(Line line, S text, S toast, HubItem* it) {
  it->kind = RowKind::Disabled;
  it->line = line;
  it->text = text;
  it->toast = toast;
}
// A disabled row's reason (no clock: Needs dates; the first month: Opens 6
// Dec), and its toast.
void closedRow(const Lifetime& l, HubItem* it) {
  if (!l.dated) {
    disabled(Line::Text, S::stats_needs_dates, S::toast_stats_needs_dates, it);
  } else {
    disabled(Line::Warming, S::stats_warming, S::toast_stats_needs_month, it);
  }
}

}  // namespace

HubLook hubLook(const Lifetime& l, const PhaseInfo& p, const Snapshot* snap) {
  HubLook h;
  h.page = pageLook(l, p, snap != nullptr);
  if (h.page.look == Look::NoCard || h.page.look == Look::NoPlays) return h;
  const bool served = h.page.look == Look::Served;
  for (uint32_t i = 0; i < kHubRows; ++i) {
    HubItem& it = h.item[i];
    it.row = static_cast<HubRow>(i);
    openLine(h.page, &it);
  }
  // Row 1: the status row with no clock (Week isn't reached then).
  if (!l.dated) {
    HubItem& it = h.item[static_cast<int>(HubRow::Week)];
    it.kind = RowKind::Status;
    it.line = Line::Status;
  }
  // Rows 3-4: from 30 counting days, dated.
  if (!pastMonth(l)) {
    closedRow(l, &h.item[static_cast<int>(HubRow::NewFinds)]);
    closedRow(l, &h.item[static_cast<int>(HubRow::OldFavourites)]);
  }
  // Row 5: O5. Dated, the counting days; with no clock, pending until the
  // snapshot gives DAYS' span (D14). Served and open, its count only after
  // a Full or lean landing (hasNotPlayed(), not the grade: 9.7).
  HubItem& np = h.item[static_cast<int>(HubRow::NotPlayed)];
  if (l.dated ? !notPlayedOpen(l.countingDays, true, 0) : served && !notPlayedOpen(0, false, snap->spanDays())) {
    closedRow(l, &np);
  } else if (served && !snap->hasNotPlayed()) {
    np.line = Line::SinceOnly;
  }
  h.rows = kHubRows;
  if (h.page.look == Look::Retry) {
    HubItem& it = h.item[kHubRows];
    it.row = HubRow::TryAgain;
    it.kind = RowKind::TryAgain;
    it.line = Line::Text;
    it.text = h.page.line;
    h.rows = kHubRows + 1;
  }
  return h;
}

S hubTitle(const HubItem& it) {
  if (it.kind == RowKind::Status) return S::stats_no_dates;
  switch (it.row) {
    case HubRow::Week: return S::stats_last7;
    case HubRow::MostPlayed: return S::stats_most_played;
    case HubRow::NewFinds: return S::stats_new_finds;
    case HubRow::OldFavourites: return S::stats_old_favourites;
    case HubRow::NotPlayed: return S::stats_not_played;
    case HubRow::Recent: return S::stats_recent;
    default: return S::empty_try_again;
  }
}

Control controlOf(RowKind k) {
  Control c;
  switch (k) {
    case RowKind::Open:
    case RowKind::TryAgain: c.pressable = true; break;
    case RowKind::Disabled: c.inert = true; break;
    case RowKind::Status: break;
  }
  return c;
}

HubTap hubTap(const HubLook& h, uint32_t row, const Lifetime& l, bool indexReady, bool synthetic) {
  if (row >= h.rows) return HubTap::None;
  switch (h.item[row].kind) {
    case RowKind::TryAgain: return HubTap::Ask;
    case RowKind::Open: return hubReachable(l, indexReady, synthetic) ? HubTap::Push : HubTap::Refused;
    default: return HubTap::None;
  }
}

bool hubSubDue(const Lifetime& l) {
  // (Not stateOf(): the card out would hide Folding and Empty.)
  return l.on && !l.folding && l.plays > 0;
}

bool hubReachable(const Lifetime& l, bool indexReady, bool synthetic) {
  return stateOf(l) != State::Off && indexReady && !synthetic;
}

uint8_t hubShape(const HubLook& h) {
  // Bits 0-2 the look (it gives the row count and the open rows' line
  // kind); 3 the status row (no clock: rows 3-5's reason needs_dates, not
  // Opens 6 Dec); 4 rows 3-4 disabled; 5-6 row 5 open with the look's line,
  // since-only, or disabled. (NoCard and NoPlays have no rows: their items
  // are the defaults, so the bits above 2 are 0.)
  const HubItem& np = h.item[static_cast<int>(HubRow::NotPlayed)];
  const uint8_t row5 = np.kind == RowKind::Disabled ? 2 : np.line == Line::SinceOnly ? 1 : 0;
  return static_cast<uint8_t>(static_cast<uint8_t>(h.page.look) |
                              (h.item[static_cast<int>(HubRow::Week)].kind == RowKind::Status ? 8u : 0u) |
                              (h.item[static_cast<int>(HubRow::NewFinds)].kind == RowKind::Disabled ? 16u : 0u) |
                              static_cast<uint8_t>(row5 << 5));
}

uint16_t lookText(const PageLook& k, const Snapshot* snap) {
  uint32_t text = 0;
  switch (k.look) {
    case Look::Served: text = snap ? snap->version() : 0; break;
    case Look::Off:
    case Look::NoPlays: break;
    default: text = static_cast<uint32_t>(k.line); break;
  }
  // (The catalogue's ids are under 8,192; a version 8,192 asks on is the
  // same text again, never between two passes.)
  static_assert(uistr::kStrings <= 0x2000, "a line's id in lookText()'s 13 bits");
  return static_cast<uint16_t>(static_cast<uint32_t>(k.look) | (text & 0x1FFFu) << 3);
}

uint32_t minuteFrom(uint16_t low, uint32_t nowMin) {
  uint32_t m = (nowMin & ~0xFFFFu) | low;
  if (m > nowMin) m = m >= 0x10000u ? m - 0x10000u : 0;
  return m;
}

// ---------------------------------------------------------------------------
// The shelves
// ---------------------------------------------------------------------------
Shelf shelfOf(uint32_t id) {
  const uint32_t s = id / 4;
  return s < kShelves ? static_cast<Shelf>(s) : Shelf::Recent;
}

uint8_t windowOf(uint32_t id) {
  if (shelfOf(id) != Shelf::MostPlayed) return sv::kAllTime;
  const uint32_t w = id % 4;
  return w <= sv::kAllTime ? static_cast<uint8_t>(w) : sv::kAllTime;
}

bool shelfOfRow(HubRow r, Shelf* out) {
  switch (r) {
    case HubRow::MostPlayed: *out = Shelf::MostPlayed; return true;
    case HubRow::NewFinds: *out = Shelf::NewFinds; return true;
    case HubRow::OldFavourites: *out = Shelf::OldFavourites; return true;
    case HubRow::NotPlayed: *out = Shelf::NotPlayed; return true;
    case HubRow::Recent: *out = Shelf::Recent; return true;
    default: return false;
  }
}

S shelfTitle(Shelf s) {
  switch (s) {
    case Shelf::MostPlayed: return S::stats_most_played;
    case Shelf::NewFinds: return S::stats_new_finds;
    case Shelf::OldFavourites: return S::stats_old_favourites;
    case Shelf::NotPlayed: return S::stats_not_played;
    default: return S::stats_recent;
  }
}

sr::List listOf(Shelf s, uint8_t window) {
  switch (s) {
    case Shelf::MostPlayed:
      return window == sv::k7Days ? sr::kMost7 : window == sv::k30Days ? sr::kMost30 : sr::kMostAll;
    case Shelf::NewFinds: return sr::kNewFinds;
    case Shelf::OldFavourites: return sr::kOldFavourites;
    case Shelf::Recent: return sr::kRecent;
    default: return sr::kLists;
  }
}

Tray trayOf(const Lifetime& l, const Snapshot* snap) {
  Tray t;
  if (!l.dated || l.countingDays < kTrayDays) return t;
  if (!pastMonth(l)) {
    t.cells = 2;
    t.window[0] = sv::k7Days;
    t.window[1] = sv::kAllTime;
    return t;
  }
  t.cells = 3;
  t.window[0] = sv::k7Days;
  t.window[1] = sv::k30Days;
  t.window[2] = sv::kAllTime;
  // RCNT short (5.2's cap: whole days go past 4,096 rows; the rank's mark,
  // DAYS played on a day RCNT no longer holds): the cell says the days it
  // holds. An oldest day inside the window alone says nothing (RCNT holds
  // days with plays only).
  if (snap && snap->dated() && snap->rcntShort()) {
    const uint16_t today = snap->today(), first = snap->rcntFirstDay();
    if (first != sf::kUnsetDay && first <= today && today - first + 1u < 30u)
      t.days30 = static_cast<uint8_t>(today - first + 1u);
  }
  return t;
}

int trayCellX(const Tray& t, int cell) {
  if (cell < 0 || cell >= t.cells) return 0;
  return t.cells == 3 ? kTrayCellX3[cell] : kTrayCellX2[cell];
}

int trayCellW(const Tray& t) { return t.cells == 3 ? kTrayCellW3 : t.cells == 2 ? kTrayCellW2 : 0; }

int trayCellAt(const Tray& t, int x, bool rightEdge) {
  if (t.cells == 0) return -1;
  if (rightEdge) return t.cells - 1;
  const int w = trayCellW(t);
  // Each split at its gap's middle, right by the panel's skew there (7.9).
  for (int k = 0; k + 1 < t.cells; ++k) {
    const int mid = (trayCellX(t, k) + w + trayCellX(t, k + 1)) / 2;
    if (x < mid + filtergrid::skew(mid)) return k;
  }
  return t.cells - 1;
}

uint8_t windowShown(uint8_t kept, const Tray& t) {
  for (int k = 0; k < t.cells; ++k)
    if (t.window[k] == kept) return kept;
  return sv::kAllTime;
}

namespace {

// A shelf the clock or the month keeps shut, from RAM (its hub row is
// disabled): New finds and Old favourites with no clock or under 30 days;
// Not played yet dated under 30 days (with no clock, the snapshot's span
// decides: below).
bool closedByRam(Shelf s, const Lifetime& l) {
  switch (s) {
    case Shelf::NewFinds:
    case Shelf::OldFavourites: return !pastMonth(l);
    case Shelf::NotPlayed: return l.dated && !notPlayedOpen(l.countingDays, true, 0);
    default: return false;
  }
}

}  // namespace

ShelfLook shelfLook(uint32_t id, const Lifetime& l, const PhaseInfo& p, const Snapshot* snap) {
  ShelfLook k;
  k.shelf = shelfOf(id);
  const Tray tray = trayOf(l, snap);
  if (k.shelf == Shelf::MostPlayed) {
    k.tray = tray.cells > 0;
    k.window = windowShown(windowOf(id), tray);
  }
  k.list = listOf(k.shelf, k.window);
  k.page = pageLook(l, p, snap != nullptr);
  switch (k.page.look) {
    case Look::NoCard:
      // The No card line alone (the owner's call 3): no tray over it.
      k.drawLine = true;
      k.tray = false;
      return k;
    case Look::Off:
    case Look::NoPlays:
      // No data, no line (D9 as replaced), and no tray offering windows of
      // nothing.
      k.tray = false;
      return k;
    default: break;
  }
  if (closedByRam(k.shelf, l)) {
    k.page.look = Look::Closed;
    k.page.line = l.dated ? S::stats_warming : S::stats_needs_dates;
    k.page.askDue = false;
    k.drawLine = true;
    return k;
  }
  if (k.page.look == Look::Served) {
    if (k.shelf == Shelf::NotPlayed) {
      // With no clock, DAYS' span decides (O5); landed shut: "Needs dates"
      // as its one line (D14).
      if (!l.dated && !notPlayedOpen(0, false, snap->spanDays())) {
        k.page.look = Look::Closed;
        k.page.line = S::stats_needs_dates;
        k.drawLine = true;
        return k;
      }
      if (!snap->hasNotPlayed()) {
        // Its own state (7.6): the visit took Lists (Full refused, or not
        // asked: a snapshot held across the midnight that opens the shelf)
        // or its notPlayed() was refused. Try again makes the lean ask;
        // while that runs the snapshot stays served, the phase its.
        k.ask = UiAsk::NotPlayed;
        switch (p.phase) {
          case Phase::Working:
          case Phase::Waiting:
            k.page.look = Look::Pending;
            k.page.line = phaseLine(p.phase);
            k.drawLine = true;
            return k;
          case Phase::Unread:
          case Phase::Capped:
            k.page.line = phaseLine(p.phase);
            break;
          default:
            k.page.line = S::stats_no_room;
            break;
        }
        k.page.look = Look::Retry;
        k.tryAgain = true;
        k.rows = 1;
        return k;
      }
      k.rows = snap->notPlayedCount();
    } else if (k.shelf == Shelf::Recent) {
      k.rows = recentCount(*snap);
    } else {
      k.rows = snap->rows(k.list);
    }
    k.topBar = k.rows > 0 && k.shelf != Shelf::NotPlayed;
    if (k.rows == 0) {
      k.page.line = S::stats_shelf_empty;
      k.drawLine = true;
    }
    return k;
  }
  if (k.page.look == Look::Retry) {
    k.tryAgain = true;
    k.rows = 1;
  } else {
    k.drawLine = true;  // Pending: the phase's line
  }
  return k;
}

// ---------------------------------------------------------------------------
// Recently played's sections
// ---------------------------------------------------------------------------
namespace {

struct Key {
  Section section = Section::Today;
  uint16_t day = sf::kUnsetDay;
  bool operator==(const Key& o) const { return section == o.section && day == o.day; }
};

Key keyOf(const Snapshot& s, uint32_t when) {
  Key k;
  if (!s.dated()) {
    // Undated: this session's, then earlier (4.3; the owner's call,
    // 2026-11-24): the copy-out codes every row one or the other, so a
    // when that is neither (none a write() makes) reads as Earlier too,
    // never a day.
    k.section = when == sv::kWhenSession ? Section::Session : Section::Earlier;
    return k;
  }
  if (isCode(when)) {
    k.section = Section::Earlier;  // dated: every undated row under one Earlier, last
    return k;
  }
  k.day = static_cast<uint16_t>(when / kDayMin);
  if (k.day == s.today()) {
    k.section = Section::Today;
  } else if (k.day + 1u == s.today()) {
    k.section = Section::Yesterday;
  } else {
    k.section = Section::Day;
  }
  return k;
}

// Walks the list: `fn(listRow, item)` for each, until it returns true.
template <class Fn>
bool walkRecent(const Snapshot& s, Fn fn) {
  const uint32_t n = s.rows(sr::kRecent);
  uint32_t at = 0;
  Key prev;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t when = s.rowAt(sr::kRecent, i).when;
    const Key k = keyOf(s, when);
    if (i == 0 || !(k == prev)) {
      RecentItem head;
      head.heading = true;
      head.section = k.section;
      head.day = k.day;
      head.row = i;
      if (fn(at++, head)) return true;
    }
    prev = k;
    RecentItem row;
    row.section = k.section;
    row.day = k.day;
    row.row = i;
    row.timed = s.dated() && !isCode(when);
    if (fn(at++, row)) return true;
  }
  return false;
}

}  // namespace

uint32_t recentCount(const Snapshot& s) {
  uint32_t n = 0;
  walkRecent(s, [&n](uint32_t, const RecentItem&) {
    ++n;
    return false;
  });
  return n;
}

RecentItem recentAt(const Snapshot& s, uint32_t listRow) {
  RecentItem out;
  out.row = s.rows(sr::kRecent);
  walkRecent(s, [&](uint32_t at, const RecentItem& it) {
    if (at != listRow) return false;
    out = it;
    return true;
  });
  return out;
}

uint32_t recentListRow(const Snapshot& s, uint32_t snapRow) {
  uint32_t found = 0;
  walkRecent(s, [&](uint32_t at, const RecentItem& it) {
    if (it.heading || it.row != snapRow) return false;
    found = at;
    return true;
  });
  return found;
}

// ---------------------------------------------------------------------------
// A shelf as LibraryPage draws, touches and acts on it
// ---------------------------------------------------------------------------
const char* shelfRowName(ShelfRow k) {
  switch (k) {
    case ShelfRow::Ranked: return "ranked";
    case ShelfRow::Section: return "section";
    case ShelfRow::Album: return "album";
    case ShelfRow::TryAgain: return "Try again";
    default: return "none";
  }
}

ShelfItem shelfItem(const ShelfLook& k, const Snapshot* snap, uint32_t row) {
  ShelfItem it;
  if (row >= k.rows) return it;
  if (k.tryAgain) {
    it.kind = ShelfRow::TryAgain;
    it.text = k.page.line;
    return it;
  }
  if (k.page.look != Look::Served || !snap) return it;
  if (k.shelf == Shelf::NotPlayed) {
    // The k-th album not played, its place in albumsAZ() (the bits' order:
    // A-Z, so the rail and the jump grid follow).
    const uint32_t place = snap->albumAt(row);
    if (place == sv::kNoAlbum || place >= snap->albums()) return it;
    it.kind = ShelfRow::Album;
    it.at = row;
    it.id = place;
    return it;
  }
  uint32_t at = row;
  if (k.shelf == Shelf::Recent) {
    it.recent = recentAt(*snap, row);
    if (it.recent.heading) {
      it.kind = ShelfRow::Section;
      return it;
    }
    at = it.recent.row;
  }
  if (at >= snap->rows(k.list)) return it;
  const uint32_t id = snap->rowAt(k.list, at).id;
  if (id >= snap->trackCount()) return it;  // (D6: never a resolved row's)
  it.kind = ShelfRow::Ranked;
  it.list = k.list;
  it.at = at;
  it.id = id;
  return it;
}

Control shelfControlOf(ShelfRow k) {
  Control c;
  switch (k) {
    case ShelfRow::Ranked:
    case ShelfRow::Album:
      c.pressable = true;
      c.holds = true;
      break;
    case ShelfRow::TryAgain: c.pressable = true; break;
    default: break;  // (Section, None: nothing, and not inert: no buzz, no toast; a stale row never acted on)
  }
  return c;
}

int32_t shelfRowOf(const ShelfLook& k, const Snapshot* snap, uint32_t track, uint32_t albumPlace) {
  if (k.page.look != Look::Served || k.tryAgain || !snap || k.rows == 0) return -1;
  if (k.shelf == Shelf::NotPlayed) {
    // Its album's row (at most the albums not played, once a push).
    for (uint32_t r = 0; r < k.rows; ++r)
      if (snap->albumAt(r) == albumPlace) return static_cast<int32_t>(r);
    return -1;
  }
  const uint32_t n = snap->rows(k.list);
  for (uint32_t i = 0; i < n; ++i) {
    if (snap->rowAt(k.list, i).id != track) continue;
    return static_cast<int32_t>(k.shelf == Shelf::Recent ? recentListRow(*snap, i) : i);
  }
  return -1;
}

bool idsServed(const Snapshot& s, sr::List l, uint32_t trackCount) {
  const uint32_t n = s.rows(l);
  if (n == 0 || !s.ids(l)) return false;
  for (uint32_t i = 0; i < n; ++i)
    if (s.rowAt(l, i).id >= trackCount) return false;
  return true;
}

bool shelfPending(Shelf s, const Snapshot* snap) { return !snap || (s == Shelf::NotPlayed && !snap->hasNotPlayed()); }

uint8_t shelfShape(const ShelfLook& k, const Tray& t) {
  const uint8_t cells = k.tray ? (t.cells == 3 ? 2 : t.cells == 2 ? 1 : 0) : 0;
  const bool shortCell = cells == 2 && t.days30 < 30;
  return static_cast<uint8_t>((static_cast<uint8_t>(k.page.look) & kShelfLook) | cells << 3 |
                              (k.window & 3u) << 5 | (shortCell ? kShelfShort : 0u));
}

bool trayMoved(uint8_t drawnShape, uint8_t nowShape, bool textsMoved) {
  const bool before = (drawnShape & kShelfCells) != 0, after = (nowShape & kShelfCells) != 0;
  if (!before || !after) return false;  // (coming or going: the item itself changes, staged as any)
  if ((drawnShape ^ nowShape) & (kShelfCells | kShelfWindow | kShelfShort)) return true;
  return (nowShape & kShelfShort) && textsMoved;  // ("16 days" to "15 days": a new ask's)
}

bool windowOwed(const ShelfLook& k, const Snapshot* snap) {
  return snap && k.shelf == Shelf::MostPlayed && k.page.look == Look::Served && snap->window() != k.window;
}

uint32_t shelfSum(const ShelfLook& k, const Snapshot& s) {
  uint32_t hash = 2166136261u;
  auto mix = [&hash](uint32_t v) { hash = (hash ^ v) * 16777619u; };
  if (k.shelf == Shelf::NotPlayed) {
    // Each clear bit's place, in order, until k.rows of them: one walk.
    const uint8_t* bits = reinterpret_cast<const uint8_t*>(&s) + s.bitsOffset();
    const uint32_t n = s.albums();
    uint32_t seen = 0;
    for (uint32_t pos = 0; pos < n && seen < k.rows; ++pos) {
      if (bits[pos / 8] >> (pos % 8) & 1) continue;
      mix(pos);
      ++seen;
    }
    for (; seen < k.rows; ++seen) mix(sv::kNoAlbum);  // (past the bits: albumAt()'s none)
    return hash;
  }
  for (uint32_t r = 0; r < s.rows(k.list); ++r) {
    const sv::Row row = s.rowAt(k.list, r);
    mix(row.id);
    mix(row.count);
    mix(row.lastDay);
    mix(row.when);
  }
  return hash;
}

bool shelfArg(const char* a, Shelf* shelf, uint8_t* window, bool* windowGiven) {
  if (!a || a[0] < '0' || a[0] >= static_cast<char>('0' + kShelves)) return false;
  const Shelf s = static_cast<Shelf>(a[0] - '0');
  uint8_t w = sv::kAllTime;
  bool given = false;
  if (a[1]) {
    if (s != Shelf::MostPlayed || a[1] < '0' || a[1] > static_cast<char>('0' + sv::kAllTime) || a[2]) return false;
    w = static_cast<uint8_t>(a[1] - '0');
    given = true;
  }
  *shelf = s;
  *window = w;
  *windowGiven = given;
  return true;
}

// ---------------------------------------------------------------------------
// Last 7 days
// ---------------------------------------------------------------------------
WeekLook weekLook(const Lifetime& l, const PhaseInfo& p, const Snapshot* snap) {
  WeekLook w;
  w.page = pageLook(l, p, snap != nullptr);
  if (w.page.look == Look::Off || w.page.look == Look::NoCard || w.page.look == Look::NoPlays) return w;
  if (!l.dated) {
    w.page.look = Look::Closed;
    w.page.line = S::stats_needs_dates;
    w.page.askDue = false;
    return w;
  }
  w.button = w.page.look == Look::Retry;
  return w;
}

void barHeights(const uint32_t ms[sr::kWeekDays], uint16_t firstDay, uint16_t sinceDay, int maxPx,
                uint8_t out[sr::kWeekDays]) {
  // A day before the first day counted has no bar, nor one with nothing.
  bool shown[sr::kWeekDays] = {};
  uint32_t top = 0;
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    out[i] = 0;
    shown[i] = firstDay != sf::kUnsetDay && ms[i] > 0 && (sinceDay == sf::kUnsetDay || firstDay + i >= sinceDay);
    if (shown[i] && ms[i] > top) top = ms[i];
  }
  if (top == 0 || maxPx <= 0) return;
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    if (!shown[i]) continue;
    // To the top day, rounded; any time at least 1 px.
    const uint64_t h = (static_cast<uint64_t>(ms[i]) * static_cast<uint64_t>(maxPx) * 2 + top) / (uint64_t{2} * top);
    out[i] = static_cast<uint8_t>(h == 0 ? 1 : h > static_cast<uint64_t>(maxPx) ? maxPx : h);
  }
}

WeekFigures weekFigures(const Snapshot& s, int maxPx) {
  WeekFigures w;
  uint32_t ms[sr::kWeekDays] = {};
  uint32_t adjPlays = 0, adjSkips = 0, top = 0;
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    const sr::Day d = s.day(i);
    ms[i] = d.listenedMs;
    w.listenedMs += d.listenedMs;
    w.plays += d.plays;
    adjPlays += d.autoDjPlays;
    adjSkips += d.autoDjSkips;
    // The big day by time; a tie goes to the later day.
    if (d.listenedMs > 0 && d.listenedMs >= top) {
      top = d.listenedMs;
      w.bigDay = static_cast<int8_t>(i);
    }
  }
  w.firstDay = weekFirstDay(s);
  barHeights(ms, w.firstDay, s.sinceDay(), maxPx, w.bar);
  // Auto DJ's rate from 5 tracks it started (D11: its plays + skips).
  const uint32_t tracks = adjPlays + adjSkips;
  if (tracks >= 5) {
    w.autoDj = true;
    w.autoDjPct = static_cast<uint8_t>((uint64_t{200} * adjPlays + tracks) / (uint64_t{2} * tracks));
  }
  return w;
}

uint16_t weekFirstDay(const Snapshot& s) {
  const uint16_t today = s.today();
  if (!s.dated() || today == sf::kUnsetDay || today < sr::kWeekDays - 1) return sf::kUnsetDay;
  return static_cast<uint16_t>(today - (sr::kWeekDays - 1));
}

uint16_t weekHeaderDay(Look k, const Snapshot* snap) {
  return k == Look::Served && snap ? weekFirstDay(*snap) : sf::kUnsetDay;
}

// ---------------------------------------------------------------------------
// The made-up views
// ---------------------------------------------------------------------------
namespace {

// The frames' card (docs/img/listening-stats): today Tue 10 Nov 2026 at
// 21:40 UTC, counting since Tue 1 Sep, 1,204 plays and 74 h 10 min in all;
// the week 4-10 Nov, 7 h 37 min and 118 plays, Saturday's 2 h 1 min the
// big day, Auto DJ 46 of 53.
constexpr uint16_t kFullToday = 313, kFullSince = 243;
constexpr uint32_t kFullNowMin = kFullToday * kDayMin + 21 * 60 + 40;
constexpr uint32_t kWeekMin[7] = {41, 58, 35, 121, 88, 64, 50};
constexpr uint16_t kWeekPlays[7] = {10, 15, 9, 31, 23, 17, 13};
constexpr uint16_t kWeekAdjPlays[7] = {4, 6, 3, 12, 9, 7, 5};
constexpr uint16_t kWeekAdjSkips[7] = {1, 1, 0, 2, 1, 1, 1};

Lifetime fullLife() {
  Lifetime l;
  l.on = true;
  l.cardIn = true;
  l.dated = true;
  l.today = kFullToday;
  l.sinceDay = kFullSince;
  l.countingDays = sv::countingDaysOf(true, kFullToday, kFullSince);
  l.plays = 1204;
  l.listenedMs = (uint64_t{74} * 60 + 10) * 60000;
  l.nowMin = kFullNowMin;
  l.madeUp = true;  // every view's: look only
  return l;
}

sv::MadeUp fullSpec(uint64_t stamp, uint32_t tracks, uint32_t albums) {
  sv::MadeUp m;
  m.buildStamp = stamp;
  m.trackCount = tracks;
  m.albums = albums;
  m.grade = sv::Grade::Full;
  m.dated = true;
  m.time = kFullNowMin * 60 + 1;
  m.today = kFullToday;
  m.liveRound = 9;
  m.sinceDay = kFullSince;
  m.firstDay = kFullSince;
  m.newestDay = kFullToday;
  m.rcntFirstDay = static_cast<uint16_t>(kFullToday - 31);
  m.window = sv::kAllTime;
  m.version = 1;
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    m.days[i].listenedMs = kWeekMin[i] * 60000;
    m.days[i].plays = kWeekPlays[i];
    m.days[i].autoDjPlays = kWeekAdjPlays[i];
    m.days[i].autoDjSkips = kWeekAdjSkips[i];
  }
  const uint32_t rows[sr::kLists] = {18, 50, 50, 23, 41, 60};
  const uint32_t totals[sr::kLists] = {18, 64, 212, 23, 41, 60};
  const uint32_t tops[sr::kLists] = {12, 23, 41, 9, 14, 0};
  for (int i = 0; i < sr::kLists; ++i) {
    m.rows[i] = rows[i];
    m.totals[i] = totals[i];
    m.topCount[i] = tops[i];
  }
  m.recentFirstAgoMin = 25;
  m.recentStepMin = 97;
  m.earlierRows = 2;
  m.notPlayedEvery = 3;
  return m;
}

// View 4's day 5 of counting: 11 Nov, counting since 7 Nov.
constexpr uint16_t kDay5Today = 314, kDay5Since = 310;

}  // namespace

bool madeUpState(uint8_t n, Lifetime* life, PhaseInfo* phase) {
  if (n == 0 || n >= kMadeUpViews) return false;
  Lifetime l = fullLife();
  PhaseInfo p;
  p.phase = Phase::Ready;
  p.outcome = Outcome::Ok;
  p.version = 1;
  switch (n) {
    case 1: break;  // a full card
    case 2:         // nothing played: the first day today (the first save's)
      l.plays = 0;
      l.listenedMs = 0;
      l.sinceDay = kFullToday;
      l.countingDays = 1;
      p = PhaseInfo();
      break;
    case 3:  // no clock: the first day kept (its snapshot: DAYS over 30 days, Recently played by session)
      l.dated = false;
      l.today = sf::kUnsetDay;
      l.countingDays = 0;
      l.nowMin = 0;
      break;
    case 4:  // day 5 of counting, 11 Nov: opens 6 Dec
      l.today = kDay5Today;
      l.sinceDay = kDay5Since;
      l.countingDays = sv::countingDaysOf(true, kDay5Today, kDay5Since);
      l.plays = 37;
      l.listenedMs = (uint64_t{2} * 60 + 41) * 60000;
      l.nowMin = kDay5Today * kDayMin + 18 * 60 + 5;
      break;
    case 5:  // folding: the boot's fold under way
      l.folding = true;
      p = PhaseInfo();
      p.phase = Phase::Working;
      p.why = sv::kWhyBoot;
      p.version = 1;
      break;
    case 6:  // no card: the card out
      l.cardIn = false;
      p.phase = Phase::NoCard;
      p.why = sv::kWhyCard;
      p.outcome = Outcome::CardAway;
      break;
    case 7:  // waiting: held past 3 s (Auto DJ's join)
      p.phase = Phase::Waiting;
      p.why = sv::kWhyJoin;
      p.outcome = Outcome::None;
      break;
    case 8:  // no room: every grade refused
      p.phase = Phase::NoRoom;
      p.outcome = Outcome::NoRoom;
      break;
    default:  // 9, unread: a read fault
      p.phase = Phase::Unread;
      p.outcome = Outcome::Io;
      break;
  }
  if (life) *life = l;
  if (phase) *phase = p;
  return true;
}

bool madeUpWrites(uint8_t n) { return n == 1 || n == 3 || n == 4; }

bool madeUpView(uint8_t n, uint64_t buildStamp, uint32_t trackCount, uint32_t albums, MadeUpView* out) {
  if (!out) return false;
  MadeUpView v;
  if (!madeUpState(n, &v.life, &v.phase)) return false;
  v.snapshot = madeUpWrites(n);
  switch (n) {
    case 1:  // a full card
      v.spec = fullSpec(buildStamp, trackCount, albums);
      break;
    case 3:  // no clock: DAYS over 30 days (O5 open), Recently played by session
      v.spec = fullSpec(buildStamp, trackCount, albums);
      v.spec.dated = false;
      v.spec.time = 0;
      v.spec.newestDay = 309;
      v.spec.sessionRows = 6;
      v.spec.earlierRows = 40;
      break;
    case 4: {  // day 5 of counting: the Lists grade (Not played yet can't open)
      sv::MadeUp& m = v.spec;
      m = fullSpec(buildStamp, trackCount, albums);
      m.grade = sv::Grade::Lists;
      m.notPlayedEvery = 0;
      m.time = v.life.nowMin * 60 + 1;
      m.today = kDay5Today;
      m.sinceDay = kDay5Since;
      m.firstDay = kDay5Since;
      m.newestDay = kDay5Today;
      m.rcntFirstDay = kDay5Since;
      const uint32_t mins[7] = {0, 0, 12, 47, 30, 41, 31};
      const uint16_t plays[7] = {0, 0, 3, 11, 7, 9, 7};
      for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
        m.days[i] = sr::Day();
        m.days[i].listenedMs = mins[i] * 60000;
        m.days[i].plays = plays[i];
      }
      const uint32_t rows[sr::kLists] = {20, 20, 20, 12, 0, 25};
      const uint32_t tops[sr::kLists] = {4, 4, 4, 3, 0, 0};
      for (int i = 0; i < sr::kLists; ++i) {
        m.rows[i] = rows[i];
        m.totals[i] = rows[i];
        m.topCount[i] = tops[i];
      }
      m.recentStepMin = 61;
      m.earlierRows = 0;
      break;
    }
    default: break;  // (no snapshot: the lifetime and phase alone)
  }
  *out = v;
  return true;
}

bool madeUpNoLog(uint8_t n) { return n == 0 || (n < kMadeUpViews && !madeUpWrites(n)); }

const char* madeUpName(uint8_t n) {
  static const char* const kNames[kMadeUpViews] = {"the card's own", "a full card", "nothing played", "no clock",
                                                   "day 5 of counting", "folding", "no card", "waiting",
                                                   "no room", "unread"};
  return n < kMadeUpViews ? kNames[n] : "?";
}

bool madeUpArg(const char* a, uint8_t* n) {
  if (!a || a[0] != 'x' || a[1] < '0' || a[1] > '9' || a[2] != 0) return false;
  if (n) *n = static_cast<uint8_t>(a[1] - '0');
  return true;
}

Rise riseOf(const Lifetime& l, uint8_t view) {
  if (view != 0 && view < kMadeUpViews) return Rise::MadeUp;
  return asksOnEdge(l) ? Rise::Ask : Rise::None;
}

int foldShelfBars(NavModel& nav) {
  int folded = 0;
  const int depth = nav.depth(NavModel::Tab::Library);
  for (int level = 0; level < depth; ++level) {
    NavModel::PageRef& r = nav.at(NavModel::Tab::Library, level);
    if (r.kind != kKindShelf || r.expanded < 0) continue;
    r.expanded = -1;
    ++folded;
  }
  return folded;
}

}  // namespace statsview
