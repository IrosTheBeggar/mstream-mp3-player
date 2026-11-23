// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the listening stats' screens as kinds and numbers
// (lib/core/StatsView: docs/LISTENING-STATS.md 4.4, 7.1-7.7, 7.11; S3b's
// C17a), against the made-up snapshots statsvisit::writeMadeUp() writes:
// - 4.4's states from the lifetime, in their order (Folding before Empty:
//   HEAD is partial while the boot's fold runs), and the day a first-month
//   row opens;
// - notPlayedOpen()'s three inputs (O5: dated, the counting days; with no
//   clock, DAYS' span), against statsvisit's rule for the visit's grade
//   (one rule: 29, 30 and 31 days);
// - a page's look by phase: the snapshot's figures, the phase's line, Try
//   again with its reason, the No card line alone while the card is out and
//   Try again with stats.card_back once it is back (the owner's call 3), Off
//   with the stats off and Working with one ask due when Idle with them on
//   (D9 as replaced: "Couldn't read the stats" only for a read fault);
// - the hub's rows in each state (open, disabled with its reason and toast,
//   the status row with no clock, Not played yet by its bits and O5), the
//   phase lines on the hub, the shelves and Week;
// - the Try again row's place: after row 6 on the hub (with the status row
//   in row 1 too), first under Most played's tray with no top bar, row 0 on
//   a shelf without one, Not played yet's lean ask;
// - the card back on all three page shapes (the shelves with no tray while
//   the card is out), and through a remount's re-read (Lifetime::on is
//   StatsGlue's on() or rereadPending(): Try again, not Off);
// - the rising edge's asks (D3: none for Off, the card out, Empty, a
//   made-up view) and onStatsPages() over stacks (an album above a shelf
//   counts; another tab, the dark, suspended and the synthetic library
//   don't);
// - the entry row: when it is there (D18 among it), its line and its
//   fallback (the lifetime line in 204 px, 204 fitting and 205 not, else
//   the time alone; no plays), the time and day texts, kEntryBars;
// - the month's edge (29 and 30 counting days) for the hub's rows 3-5, the
//   shelves and the state alike; Folding's rows from RAM;
// - Off and NoPlays drawing nothing on a shelf (no line, no tray);
// - the tray by counting days and the rank's RCNT mark (a gap at the
//   window's start stays 30), its cells' boxes, its hits (each cell's
//   centre at least 16 px inside its splits on the lab's panel and a true
//   one) and the kept window's fallback;
// - Recently played's sections: a dated snapshot's Today, Yesterday, a
//   heading a day and one Earlier for its undated rows; an undated one's
//   This session then Earlier, with no times, and its dated rows under
//   their days;
// - Week's figures from a made-up snapshot: the total, the plays, the bars
//   (none before sinceDay, sinceDay's own, none for an empty day), the big
//   day (a tie to the later day), Auto DJ's rate from 5 tracks (D11);
// - the made-up views 0-9, 7-9's looks on every page, none with an ask due
//   (a view whose snapshot wasn't written among them).
// The texts' widths in every language are test_ui_library's (S6).
// Run: pio test -e native -f test_ui_stats
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "FilterGrid.h"
#include "NavModel.h"
#include "StatsFold.h"
#include "StatsRank.h"
#include "StatsView.h"
#include "StatsVisit.h"
#include "TouchCalibration.h"
#include "UiStrings.h"

namespace sf = statsfold;
namespace sr = statsrank;
namespace sv = statsvisit;
using namespace statsview;

void setUp() { uistr::setLang(uistr::Lang::En); }
void tearDown() { uistr::setLang(uistr::Lang::En); }

namespace {

constexpr uint64_t kStamp = 0x5eed0123456789abull;
constexpr uint32_t kTracks = 19417;
constexpr uint32_t kAlbums = 1706;
constexpr uint16_t kToday = 313;  // Tue 10 Nov 2026
constexpr uint16_t kSince = 243;  // Tue 1 Sep 2026

// A block as the Dance tab's: kBound bytes, aligned as malloc's.
struct Snap {
  std::vector<uint64_t> m = std::vector<uint64_t>(sv::kBound / 8, 0);
  sv::Snapshot* s() { return sv::Snapshot::at(m.data()); }
  const sv::Snapshot* operator->() { return s(); }
};

// The lifetime of a card counting `days` days, dated (today kToday).
Lifetime dated(uint32_t days = 71) {
  Lifetime l;
  l.on = true;
  l.cardIn = true;
  l.dated = true;
  l.today = kToday;
  l.sinceDay = static_cast<uint16_t>(kToday - days + 1);
  l.countingDays = sv::countingDaysOf(true, l.today, l.sinceDay);
  l.plays = 1204;
  l.listenedMs = (74ull * 60 + 10) * 60000;
  l.nowMin = kToday * 1440u + 21 * 60 + 40;
  return l;
}
// ... with the board's clock unset (the first day kept from a past save).
Lifetime noClock() {
  Lifetime l = dated();
  l.dated = false;
  l.today = sf::kUnsetDay;
  l.countingDays = 0;
  l.nowMin = 0;
  return l;
}

PhaseInfo phase(Phase p, Outcome o = Outcome::None, uint8_t why = 0) {
  PhaseInfo i;
  i.phase = p;
  i.outcome = o;
  i.why = why;
  i.version = 7;
  return i;
}
const PhaseInfo kReady = phase(Phase::Ready, Outcome::Ok);

// A made-up snapshot: view 1's (a full card) with changes.
sv::MadeUp fullCard() {
  MadeUpView v;
  TEST_ASSERT_TRUE(madeUpView(1, kStamp, kTracks, kAlbums, &v));
  return v.spec;
}
void put(Snap& b, const sv::MadeUp& m) { TEST_ASSERT_TRUE(sv::writeMadeUp(m, b.s(), sv::kBound)); }


// A monospace width: ctx is the px a byte.
int monoWidth(const char* t, void* ctx) { return static_cast<int>(std::strlen(t)) * *static_cast<int*>(ctx); }
// Any text the same width: ctx is it.
int fixedWidth(const char*, void* ctx) { return *static_cast<int*>(ctx); }

void lineIs(Line want, S text, const HubItem& it, const char* msg) {
  TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(want), static_cast<int>(it.line), msg);
  if (want == Line::Text || want == Line::Warming)
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(text), static_cast<int>(it.text), msg);
}

}  // namespace

// ---------------------------------------------------------------------------
// 4.4's states
// ---------------------------------------------------------------------------
void test_the_states_of_the_lifetime() {
  Lifetime l = dated();
  TEST_ASSERT_EQUAL(State::Ready, stateOf(l));
  // Under 30 counting days: the first month; 30 on: Ready.
  TEST_ASSERT_EQUAL(State::FirstMonth, stateOf(dated(1)));
  TEST_ASSERT_EQUAL(State::FirstMonth, stateOf(dated(29)));
  TEST_ASSERT_EQUAL(State::Ready, stateOf(dated(30)));
  TEST_ASSERT_EQUAL(State::NoDates, stateOf(noClock()));
  // Nothing played: Empty, clock or not; but not while the boot's fold runs
  // (HEAD partial: Folding first).
  l.plays = 0;
  TEST_ASSERT_EQUAL(State::Empty, stateOf(l));
  Lifetime n = noClock();
  n.plays = 0;
  TEST_ASSERT_EQUAL(State::Empty, stateOf(n));
  l.folding = true;
  TEST_ASSERT_EQUAL(State::Folding, stateOf(l));
  // The card out comes before the fold, the stats off before all.
  l.cardIn = false;
  TEST_ASSERT_EQUAL(State::NoCard, stateOf(l));
  l.on = false;
  TEST_ASSERT_EQUAL(State::Off, stateOf(l));
  TEST_ASSERT_EQUAL_STRING("first month", stateName(State::FirstMonth));
  // Opens: sinceDay + 29 (6 Dec for day 5 of counting on 11 Nov).
  Lifetime f = dated(5);
  f.today = 314;
  f.sinceDay = 310;
  TEST_ASSERT_EQUAL_UINT16(339, opensDay(f));
  f.sinceDay = sf::kUnsetDay;
  TEST_ASSERT_EQUAL_UINT16(343, opensDay(f));
  f.today = sf::kUnsetDay;
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, opensDay(f));
}

// ---------------------------------------------------------------------------
// notPlayedOpen()'s three inputs (O5), the visit's grade's rule beside it
// ---------------------------------------------------------------------------
void test_not_played_open_s_three_inputs() {
  // Dated: the counting days (29 closed, 30 and 31 open); the span unread.
  TEST_ASSERT_FALSE(notPlayedOpen(29, true, 400));
  TEST_ASSERT_TRUE(notPlayedOpen(30, true, 0));
  TEST_ASSERT_TRUE(notPlayedOpen(31, true, 0));
  TEST_ASSERT_FALSE(notPlayedOpen(0, true, 400));
  // No clock: DAYS' span (0 before the ask lands, or no dated day).
  TEST_ASSERT_FALSE(notPlayedOpen(400, false, 0));
  TEST_ASSERT_FALSE(notPlayedOpen(400, false, 29));
  TEST_ASSERT_TRUE(notPlayedOpen(0, false, 30));
  TEST_ASSERT_TRUE(notPlayedOpen(0, false, 31));
  static_assert(notPlayedOpen(30, true, 0) && !notPlayedOpen(30, false, 0), "notPlayedOpen() at compile time");
  // Dated, the shelf and the visit's grade agree at every first day around
  // the month (statsvisit's one rule): 29, 30 and 31 counting days, a first
  // day after today, none.
  for (int d = -3; d <= 3; ++d) {
    const uint16_t since = static_cast<uint16_t>(kToday - 29 + d);
    const uint32_t days = sv::countingDaysOf(true, kToday, since);
    TEST_ASSERT_EQUAL(sv::notPlayedMayOpen(true, kToday, since), notPlayedOpen(days, true, 0));
  }
  TEST_ASSERT_EQUAL_UINT32(29, sv::countingDaysOf(true, kToday, kToday - 28));
  TEST_ASSERT_EQUAL_UINT32(30, sv::countingDaysOf(true, kToday, kToday - 29));
  TEST_ASSERT_EQUAL_UINT32(31, sv::countingDaysOf(true, kToday, kToday - 30));
  TEST_ASSERT_FALSE(notPlayedOpen(sv::countingDaysOf(true, kToday, kToday - 28), true, 0));
  TEST_ASSERT_TRUE(notPlayedOpen(sv::countingDaysOf(true, kToday, kToday - 29), true, 0));
  TEST_ASSERT_TRUE(notPlayedOpen(sv::countingDaysOf(true, kToday, kToday - 30), true, 0));
  TEST_ASSERT_FALSE(sv::notPlayedMayOpen(true, kToday, sf::kUnsetDay));
  TEST_ASSERT_FALSE(notPlayedOpen(sv::countingDaysOf(true, kToday, sf::kUnsetDay), true, 0));
  // No clock: the visit asks Full (perhaps), the shelf waits for the span.
  TEST_ASSERT_TRUE(sv::notPlayedMayOpen(false, 0, kSince));
}

// ---------------------------------------------------------------------------
// The look by phase
// ---------------------------------------------------------------------------
void test_the_page_look_by_phase() {
  const Lifetime l = dated();
  struct Case {
    PhaseInfo p;
    Look look;
    S line;
    bool askDue;
  } cases[] = {
      {phase(Phase::Working), Look::Pending, S::stats_counting, false},
      {phase(Phase::Waiting, Outcome::None, sv::kWhyJoin), Look::Pending, S::stats_waiting, false},
      {phase(Phase::NoRoom, Outcome::NoRoom), Look::Retry, S::stats_no_room, false},
      {phase(Phase::NoRoom, Outcome::NoBlock), Look::Retry, S::stats_no_room, false},
      {phase(Phase::Unread, Outcome::Io), Look::Retry, S::stats_unread, false},
      {phase(Phase::Capped), Look::Retry, S::stats_waiting, false},
      // The card back (No card, the card in): Try again, "The card is back".
      {phase(Phase::NoCard, Outcome::CardAway), Look::Retry, S::stats_card_back, false},
      // Idle with the stats on (a Cancelled ask, a landing released): the
      // Working line and one ask; never "Couldn't read the stats".
      {phase(Phase::Idle, Outcome::Cancelled), Look::Pending, S::stats_counting, true},
      {phase(Phase::Idle, Outcome::Ok), Look::Pending, S::stats_counting, true},
      {phase(Phase::Idle), Look::Pending, S::stats_counting, true},
      // Ready needs a snapshot served; without one, as Idle.
      {kReady, Look::Pending, S::stats_counting, true},
  };
  for (const Case& c : cases) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s / %s", sv::phaseName(c.p.phase), sv::outcomeName(c.p.outcome));
    const PageLook k = pageLook(l, c.p, false);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(lookName(c.look), lookName(k.look), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.line), static_cast<int>(k.line), msg);
    TEST_ASSERT_EQUAL_MESSAGE(c.askDue, k.askDue, msg);
    TEST_ASSERT_TRUE_MESSAGE(k.line != S::stats_unread || c.p.phase == Phase::Unread, msg);
    // Served: the snapshot's, whatever the phase (a lean ask, or a re-ask,
    // under way over it).
    TEST_ASSERT_EQUAL_STRING_MESSAGE("served", lookName(pageLook(l, c.p, true).look), msg);
  }
  // The stats off: Off, whatever the phase, no ask.
  Lifetime off = l;
  off.on = false;
  for (Phase p : {Phase::Idle, Phase::Working, Phase::NoRoom, Phase::Unread, Phase::NoCard}) {
    const PageLook k = pageLook(off, phase(p), false);
    TEST_ASSERT_EQUAL_STRING("off", lookName(k.look));
    TEST_ASSERT_FALSE(k.askDue);
    TEST_ASSERT_EQUAL_STRING("off", lookName(pageLook(off, phase(p), true).look));
  }
  // The card out: the No card line alone, whatever the phase; no ask.
  Lifetime out = l;
  out.cardIn = false;
  for (Phase p : {Phase::Idle, Phase::Working, Phase::NoRoom, Phase::NoCard}) {
    const PageLook k = pageLook(out, phase(p, Outcome::CardAway, sv::kWhyCard), false);
    TEST_ASSERT_EQUAL_STRING("no card", lookName(k.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(k.line));
    TEST_ASSERT_FALSE(k.askDue);
  }
  // Nothing played: the empty state, nothing asked.
  Lifetime empty = l;
  empty.plays = 0;
  TEST_ASSERT_EQUAL_STRING("no plays", lookName(pageLook(empty, phase(Phase::Idle), false).look));
  TEST_ASSERT_FALSE(pageLook(empty, phase(Phase::Idle), false).askDue);
  // A made-up view (look only, D16): the same looks, never an ask due.
  Lifetime madeUp = l;
  madeUp.madeUp = true;
  for (const Case& c : cases) {
    const PageLook k = pageLook(madeUp, c.p, false);
    TEST_ASSERT_EQUAL_STRING(lookName(c.look), lookName(k.look));
    TEST_ASSERT_EQUAL(static_cast<int>(c.line), static_cast<int>(k.line));
    TEST_ASSERT_FALSE(k.askDue);
  }
  // The phases' lines (7.11).
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_counting), static_cast<int>(phaseLine(Phase::Working)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_waiting), static_cast<int>(phaseLine(Phase::Waiting)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_waiting), static_cast<int>(phaseLine(Phase::Capped)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_room), static_cast<int>(phaseLine(Phase::NoRoom)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_unread), static_cast<int>(phaseLine(Phase::Unread)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(phaseLine(Phase::NoCard)));
}

// The rising edge's asks (D1, D3 with No card: ask() would only drop it).
void test_the_rising_edge_s_asks() {
  TEST_ASSERT_TRUE(asksOnEdge(dated()));
  TEST_ASSERT_TRUE(asksOnEdge(dated(5)));
  TEST_ASSERT_TRUE(asksOnEdge(noClock()));
  Lifetime l = dated();
  l.folding = true;  // the ask waits for the boot's scan (Working)
  TEST_ASSERT_TRUE(asksOnEdge(l));
  l = dated();
  l.madeUp = true;  // a made-up view: look only
  TEST_ASSERT_FALSE(asksOnEdge(l));
  l = dated();
  l.plays = 0;
  TEST_ASSERT_FALSE(asksOnEdge(l));  // Empty
  l = dated();
  l.cardIn = false;
  TEST_ASSERT_FALSE(asksOnEdge(l));  // the card out
  l = dated();
  l.on = false;
  TEST_ASSERT_FALSE(asksOnEdge(l));  // Off
  // A remount's re-read waiting (StatsGlue on() false, rereadPending():
  // Lifetime::on is both, HEAD cleared, folding): on, so it asks (the ask
  // waits for the scan, Working), as StatsLists' ask() takes it.
  l = dated();
  l.folding = true;
  l.plays = 0;
  l.listenedMs = 0;
  l.sinceDay = sf::kUnsetDay;
  l.countingDays = 0;
  TEST_ASSERT_EQUAL(State::Folding, stateOf(l));
  TEST_ASSERT_TRUE(asksOnEdge(l));
}

// ---------------------------------------------------------------------------
// The held condition over stacks
// ---------------------------------------------------------------------------
void test_on_stats_pages_over_stacks() {
  // ui::PageKind's values: Library 2, Artist 3, Album 4.
  const NavModel::PageRef root{2, 0, -1, -1}, artist{3, 5, -1, -1}, album{4, 9, -1, -1};
  const NavModel::PageRef hub{kKindHub, 0, -1, -1}, shelf{kKindShelf, shelfId(Shelf::MostPlayed), -1, -1},
      week{kKindWeek, 0, -1, -1};
  NavModel nav;
  nav.setRoot(NavModel::Tab::Library, root);
  nav.select(NavModel::Tab::Library);
  TEST_ASSERT_FALSE(onStatsPages(nav, false, false, false));  // Artists
  nav.push(hub);
  TEST_ASSERT_TRUE(onStatsPages(nav, false, false, false));
  nav.push(shelf);
  TEST_ASSERT_TRUE(onStatsPages(nav, false, false, false));
  nav.push(album);  // an album above a shelf counts
  TEST_ASSERT_TRUE(onStatsPages(nav, false, false, false));
  // The dark, suspended, the synthetic library: not.
  TEST_ASSERT_FALSE(onStatsPages(nav, true, false, false));
  TEST_ASSERT_FALSE(onStatsPages(nav, false, true, false));
  TEST_ASSERT_FALSE(onStatsPages(nav, false, false, true));
  // Another tab, the stack kept: not.
  for (NavModel::Tab t : {NavModel::Tab::NowPlaying, NavModel::Tab::Queue, NavModel::Tab::Dance,
                          NavModel::Tab::Output}) {
    nav.select(t);
    TEST_ASSERT_FALSE(onStatsPages(nav, false, false, false));
  }
  nav.select(NavModel::Tab::Library);
  TEST_ASSERT_TRUE(onStatsPages(nav, false, false, false));
  // Back to Artists: not; an artist's album: not; Week: yes.
  nav.popToRoot(NavModel::Tab::Library);
  TEST_ASSERT_FALSE(onStatsPages(nav, false, false, false));
  nav.push(artist);
  nav.push(album);
  TEST_ASSERT_FALSE(onStatsPages(nav, false, false, false));
  nav.popToRoot(NavModel::Tab::Library);
  nav.push(hub);
  nav.push(week);
  TEST_ASSERT_TRUE(onStatsPages(nav, false, false, false));
  // A stats page on another tab's stack doesn't count (none is pushed
  // there, but the rule reads the Library's alone).
  NavModel q;
  q.setRoot(NavModel::Tab::Library, root);
  q.setRoot(NavModel::Tab::Queue, hub);
  q.select(NavModel::Tab::Queue);
  TEST_ASSERT_FALSE(onStatsPages(q, false, false, false));
  TEST_ASSERT_TRUE(statsKind(kKindHub) && statsKind(kKindShelf) && statsKind(kKindWeek));
  TEST_ASSERT_FALSE(statsKind(14) || statsKind(18) || statsKind(2));
}

// ---------------------------------------------------------------------------
// The entry row (7.3)
// ---------------------------------------------------------------------------
void test_the_entry_row_and_its_line() {
  // There: Folding, Empty, No dates, the first month, Ready; with an index,
  // not the synthetic library, with artists.
  Lifetime l = dated();
  TEST_ASSERT_TRUE(entryDue(l, true, false, 12));
  TEST_ASSERT_FALSE(entryDue(l, false, false, 12));  // behind the update's fence
  TEST_ASSERT_FALSE(entryDue(l, true, true, 12));    // uil<n>
  TEST_ASSERT_FALSE(entryDue(l, true, false, 0));    // no artists (D18)
  TEST_ASSERT_TRUE(entryDue(dated(5), true, false, 1));
  TEST_ASSERT_TRUE(entryDue(noClock(), true, false, 1));
  Lifetime f = l;
  f.folding = true;
  TEST_ASSERT_TRUE(entryDue(f, true, false, 1));
  Lifetime e = l;
  e.plays = 0;
  TEST_ASSERT_TRUE(entryDue(e, true, false, 1));
  Lifetime out = l;
  out.cardIn = false;
  TEST_ASSERT_FALSE(entryDue(out, true, false, 1));
  Lifetime off = l;
  off.on = false;
  TEST_ASSERT_FALSE(entryDue(off, true, false, 1));

  char b[96];
  int px = 6;  // "74 h 10 min since 1 Sep": 23 B, 138 px
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min since 1 Sep", b);
  // Wider than 204 px: the time alone.
  px = 9;  // 207 px
  TEST_ASSERT_EQUAL(EntryLine::Time, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min", b);
  px = 8;  // 184 px: fits again
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(l, monoWidth, &px, b, sizeof(b)));
  // The room's edge (the owner's call 1: 204 px): 204 fits, 205 doesn't.
  int fixed = kEntryRoom;
  TEST_ASSERT_EQUAL_INT(204, kEntryRoom);
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(l, fixedWidth, &fixed, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min since 1 Sep", b);
  fixed = kEntryRoom + 1;
  TEST_ASSERT_EQUAL(EntryLine::Time, entryLine(l, fixedWidth, &fixed, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min", b);
  // No first day: the time alone, whatever the width.
  Lifetime u = l;
  u.sinceDay = sf::kUnsetDay;
  px = 1;
  TEST_ASSERT_EQUAL(EntryLine::Time, entryLine(u, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min", b);
  // No clock: the first day kept, so the lifetime line (4.4's No dates).
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(noClock(), monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min since 1 Sep", b);
  // Under an hour: minutes; no plays in any form.
  u = l;
  u.listenedMs = 45 * 60000 + 59999;
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(u, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("45 min since 1 Sep", b);
  TEST_ASSERT_NULL(std::strstr(b, "play"));
  // Folding: Counting plays…; nothing played: Plays show up as you listen.
  TEST_ASSERT_EQUAL(EntryLine::Counting, entryLine(f, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_counting), b);
  TEST_ASSERT_EQUAL(EntryLine::Empty, entryLine(e, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_entry_empty), b);

  // The times (4.1) and the days.
  const struct {
    uint64_t ms;
    const char* text;
  } times[] = {{0, "0 min"},
               {59999, "0 min"},
               {45 * 60000ull, "45 min"},
               {60 * 60000ull - 1, "59 min"},
               {60 * 60000ull, "1 h 0 min"},
               {(74ull * 60 + 10) * 60000, "74 h 10 min"},
               {(1204ull * 60 + 5) * 60000, "1,204 h 5 min"},
               {(99999ull * 60 + 59) * 60000, "99,999 h 59 min"}};
  for (const auto& t : times) {
    timeText(t.ms, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(t.text, b);
  }
  const struct {
    uint16_t day;
    const char* text;
  } days[] = {{0, "1 Jan"}, {58, "28 Feb"}, {243, "1 Sep"}, {313, "10 Nov"}, {339, "6 Dec"}, {364, "31 Dec"},
              {365, "1 Jan"}};
  for (const auto& d : days) {
    dayMonthText(d.day, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(d.text, b);
  }
  // The fixed bars: the Entry frame's (its last bar's pixels at x 39-42),
  // 24 px the top, standing on y 32 inside the 42 px row, clear of the
  // title at x 54.
  TEST_ASSERT_EQUAL_UINT8(24, *std::max_element(kEntryBars, kEntryBars + kEntryBarCount));
  TEST_ASSERT_EQUAL_INT(42, kEntryBarX + (kEntryBarCount - 1) * kEntryBarStep + kEntryBarW - 1);
  TEST_ASSERT_TRUE(kEntryBarBase - 24 >= 0 && kEntryBarBase <= 42);
}

// ---------------------------------------------------------------------------
// The hub (7.4)
// ---------------------------------------------------------------------------
void test_hub_rows_in_each_state() {
  Snap full;
  put(full, fullCard());
  // Ready, served: every row open, its data line; no Try again.
  HubLook h = hubLook(dated(), kReady, full.s());
  TEST_ASSERT_EQUAL_STRING("served", lookName(h.page.look));
  TEST_ASSERT_EQUAL_UINT32(6, h.rows);
  for (uint32_t i = 0; i < 6; ++i) {
    TEST_ASSERT_EQUAL(static_cast<int>(i), static_cast<int>(h.item[i].row));
    TEST_ASSERT_EQUAL(RowKind::Open, h.item[i].kind);
    TEST_ASSERT_EQUAL(Line::Data, h.item[i].line);
  }
  // Not played yet's count only after a Full or lean landing: a Lists
  // snapshot reads "since 1 Sep".
  sv::MadeUp lists = fullCard();
  lists.grade = sv::Grade::Lists;
  lists.notPlayedEvery = 0;
  Snap ls;
  put(ls, lists);
  h = hubLook(dated(), kReady, ls.s());
  TEST_ASSERT_EQUAL(RowKind::Open, h.item[4].kind);
  TEST_ASSERT_EQUAL(Line::SinceOnly, h.item[4].line);

  // Ready, pending: every row open, the phase's line.
  h = hubLook(dated(), phase(Phase::Working), nullptr);
  TEST_ASSERT_EQUAL_UINT32(6, h.rows);
  for (uint32_t i = 0; i < 6; ++i) {
    TEST_ASSERT_EQUAL(RowKind::Open, h.item[i].kind);
    lineIs(Line::Text, S::stats_counting, h.item[i], "working");
  }

  // The first month (day 5): New finds, Old favourites and Not played yet
  // disabled, "Opens 6 Dec", the month's toast; served or not.
  for (const sv::Snapshot* snap : {static_cast<const sv::Snapshot*>(full.s()), static_cast<const sv::Snapshot*>(nullptr)}) {
    h = hubLook(dated(5), snap ? kReady : phase(Phase::Working), snap);
    TEST_ASSERT_EQUAL_UINT32(6, h.rows);
    for (HubRow r : {HubRow::NewFinds, HubRow::OldFavourites, HubRow::NotPlayed}) {
      const HubItem& it = h.item[static_cast<int>(r)];
      TEST_ASSERT_EQUAL(RowKind::Disabled, it.kind);
      lineIs(Line::Warming, S::stats_warming, it, "first month");
      TEST_ASSERT_EQUAL(static_cast<int>(S::toast_stats_needs_month), static_cast<int>(it.toast));
    }
    for (HubRow r : {HubRow::Week, HubRow::MostPlayed, HubRow::Recent})
      TEST_ASSERT_EQUAL(RowKind::Open, h.item[static_cast<int>(r)].kind);
  }
  // At 29 days still disabled; at 30 open: rows 3-5 at the same edge as the
  // state's (First month to Ready), served or not.
  for (const sv::Snapshot* snap : {static_cast<const sv::Snapshot*>(full.s()), static_cast<const sv::Snapshot*>(nullptr)}) {
    const PhaseInfo p = snap ? kReady : phase(Phase::Working);
    const HubLook h29 = hubLook(dated(29), p, snap), h30 = hubLook(dated(30), p, snap);
    TEST_ASSERT_EQUAL(State::FirstMonth, stateOf(dated(29)));
    TEST_ASSERT_EQUAL(State::Ready, stateOf(dated(30)));
    for (HubRow r : {HubRow::NewFinds, HubRow::OldFavourites, HubRow::NotPlayed}) {
      TEST_ASSERT_EQUAL(RowKind::Disabled, h29.item[static_cast<int>(r)].kind);
      lineIs(Line::Warming, S::stats_warming, h29.item[static_cast<int>(r)], "29 days");
      TEST_ASSERT_EQUAL(RowKind::Open, h30.item[static_cast<int>(r)].kind);
    }
  }

  // No dates, pending: row 1 the status row; New finds and Old favourites
  // disabled, "Needs dates" and its toast; Not played yet pending (open,
  // the phase's line: it pushes as any pending row, D14).
  h = hubLook(noClock(), phase(Phase::Working), nullptr);
  TEST_ASSERT_EQUAL_UINT32(6, h.rows);
  TEST_ASSERT_EQUAL(RowKind::Status, h.item[0].kind);
  TEST_ASSERT_EQUAL(Line::Status, h.item[0].line);
  for (HubRow r : {HubRow::NewFinds, HubRow::OldFavourites}) {
    const HubItem& it = h.item[static_cast<int>(r)];
    TEST_ASSERT_EQUAL(RowKind::Disabled, it.kind);
    lineIs(Line::Text, S::stats_needs_dates, it, "no dates");
    TEST_ASSERT_EQUAL(static_cast<int>(S::toast_stats_needs_dates), static_cast<int>(it.toast));
  }
  TEST_ASSERT_EQUAL(RowKind::Open, h.item[4].kind);
  lineIs(Line::Text, S::stats_counting, h.item[4], "no dates, pending");
  // ... landed: Not played yet by DAYS' span (O5): 30 days or more open
  // (the NoClock frame), else disabled with "Needs dates".
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  Snap nc;
  put(nc, v3.spec);
  TEST_ASSERT_TRUE(nc->spanDays() >= 30);
  h = hubLook(noClock(), kReady, nc.s());
  TEST_ASSERT_EQUAL(RowKind::Status, h.item[0].kind);
  TEST_ASSERT_EQUAL(RowKind::Open, h.item[4].kind);
  TEST_ASSERT_EQUAL(Line::Data, h.item[4].line);
  sv::MadeUp shortSpan = v3.spec;
  shortSpan.firstDay = 290;
  shortSpan.newestDay = 318;  // 29 days
  Snap ss;
  put(ss, shortSpan);
  TEST_ASSERT_EQUAL_UINT32(29, ss->spanDays());
  h = hubLook(noClock(), kReady, ss.s());
  TEST_ASSERT_EQUAL(RowKind::Disabled, h.item[4].kind);
  lineIs(Line::Text, S::stats_needs_dates, h.item[4], "span 29");

  // Folding: the rows from RAM, every open row's line "Counting plays…".
  const PhaseInfo boot = phase(Phase::Working, Outcome::None, sv::kWhyBoot);
  Lifetime f = dated();
  f.folding = true;
  h = hubLook(f, boot, nullptr);
  TEST_ASSERT_EQUAL_UINT32(6, h.rows);
  for (uint32_t i = 0; i < 6; ++i) lineIs(Line::Text, S::stats_counting, h.item[i], "folding");
  // ... in the first month the disabled rows keep "Opens 6 Dec"; with no
  // clock the status row and "Needs dates" stay (7.4 as decided: RAM's).
  f = dated(5);
  f.folding = true;
  h = hubLook(f, boot, nullptr);
  for (HubRow r : {HubRow::Week, HubRow::MostPlayed, HubRow::Recent})
    lineIs(Line::Text, S::stats_counting, h.item[static_cast<int>(r)], "folding, first month");
  for (HubRow r : {HubRow::NewFinds, HubRow::OldFavourites, HubRow::NotPlayed}) {
    TEST_ASSERT_EQUAL(RowKind::Disabled, h.item[static_cast<int>(r)].kind);
    lineIs(Line::Warming, S::stats_warming, h.item[static_cast<int>(r)], "folding, first month");
  }
  f = noClock();
  f.folding = true;
  h = hubLook(f, boot, nullptr);
  TEST_ASSERT_EQUAL(RowKind::Status, h.item[0].kind);
  TEST_ASSERT_EQUAL(Line::Status, h.item[0].line);
  for (HubRow r : {HubRow::NewFinds, HubRow::OldFavourites}) {
    TEST_ASSERT_EQUAL(RowKind::Disabled, h.item[static_cast<int>(r)].kind);
    lineIs(Line::Text, S::stats_needs_dates, h.item[static_cast<int>(r)], "folding, no clock");
  }
  for (HubRow r : {HubRow::MostPlayed, HubRow::NotPlayed, HubRow::Recent})
    lineIs(Line::Text, S::stats_counting, h.item[static_cast<int>(r)], "folding, no clock");

  // Empty: no rows, the empty state (7.5).
  Lifetime e = dated();
  e.plays = 0;
  h = hubLook(e, phase(Phase::Idle), nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, h.rows);
  TEST_ASSERT_EQUAL_STRING("no plays", lookName(h.page.look));

  // No card (the card out): no rows, the one line.
  Lifetime out = dated();
  out.cardIn = false;
  h = hubLook(out, phase(Phase::NoCard, Outcome::CardAway, sv::kWhyCard), nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, h.rows);
  TEST_ASSERT_EQUAL_STRING("no card", lookName(h.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(h.page.line));

  // Off (the stats gone off on a shown hub): the rows, no data lines, no
  // Try again, no ask.
  Lifetime off = dated();
  off.on = false;
  h = hubLook(off, phase(Phase::Idle, Outcome::None, sv::kWhyOff), nullptr);
  TEST_ASSERT_EQUAL_UINT32(6, h.rows);
  TEST_ASSERT_FALSE(h.page.askDue);
  for (uint32_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL(Line::None, h.item[i].line);
}

// The phases' lines on the hub, the shelves and Week.
void test_the_phase_lines_on_every_page() {
  const Lifetime l = dated();
  for (Phase p : {Phase::Working, Phase::Waiting}) {
    const S want = phaseLine(p);
    const HubLook h = hubLook(l, phase(p), nullptr);
    for (uint32_t i = 0; i < 6; ++i) lineIs(Line::Text, want, h.item[i], sv::phaseName(p));
    TEST_ASSERT_EQUAL_UINT32(6, h.rows);
    for (uint32_t s = 0; s < kShelves; ++s) {
      const ShelfLook k = shelfLook(shelfId(static_cast<Shelf>(s)), l, phase(p), nullptr);
      TEST_ASSERT_EQUAL_STRING("pending", lookName(k.page.look));
      TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(k.page.line));
      TEST_ASSERT_EQUAL_UINT32(0, k.rows);
      TEST_ASSERT_FALSE(k.topBar);
      TEST_ASSERT_FALSE(k.tryAgain);
    }
    const WeekLook w = weekLook(l, phase(p), nullptr);
    TEST_ASSERT_EQUAL_STRING("pending", lookName(w.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(w.page.line));
    TEST_ASSERT_FALSE(w.button);
  }
  for (Phase p : {Phase::NoRoom, Phase::Unread, Phase::Capped}) {
    const S want = phaseLine(p);
    const HubLook h = hubLook(l, phase(p), nullptr);
    TEST_ASSERT_EQUAL_UINT32(7, h.rows);
    for (uint32_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL(Line::None, h.item[i].line);
    lineIs(Line::Text, want, h.item[6], sv::phaseName(p));
    for (uint32_t s = 0; s < kShelves; ++s) {
      const ShelfLook k = shelfLook(shelfId(static_cast<Shelf>(s)), l, phase(p), nullptr);
      TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
      TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(k.page.line));
      TEST_ASSERT_TRUE(k.tryAgain);
      TEST_ASSERT_EQUAL_UINT32(1, k.rows);
    }
    const WeekLook w = weekLook(l, phase(p), nullptr);
    TEST_ASSERT_EQUAL_STRING("retry", lookName(w.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(w.page.line));
    TEST_ASSERT_TRUE(w.button);
  }
}

// ---------------------------------------------------------------------------
// The Try again row's place
// ---------------------------------------------------------------------------
void test_the_try_again_row_s_place() {
  // The hub: after row 6, at index 6, with the status row in row 1 too.
  for (const Lifetime& l : {dated(), noClock()}) {
    const HubLook h = hubLook(l, phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
    TEST_ASSERT_EQUAL_UINT32(7, h.rows);
    TEST_ASSERT_EQUAL(HubRow::TryAgain, h.item[6].row);
    TEST_ASSERT_EQUAL(RowKind::TryAgain, h.item[6].kind);
    TEST_ASSERT_EQUAL(l.dated ? RowKind::Open : RowKind::Status, h.item[0].kind);
  }
  // Most played in No room, Unread or Capped: the tray (the lead TopRow),
  // then row 0 Try again, no top bar.
  for (Phase p : {Phase::NoRoom, Phase::Unread, Phase::Capped}) {
    const ShelfLook k = shelfLook(shelfId(Shelf::MostPlayed), dated(), phase(p), nullptr);
    TEST_ASSERT_TRUE(k.tray);
    TEST_ASSERT_TRUE(k.tryAgain);
    TEST_ASSERT_FALSE(k.topBar);
    TEST_ASSERT_EQUAL_UINT32(1, k.rows);
    TEST_ASSERT_EQUAL(UiAsk::Visit, k.ask);
  }
  // ... without a tray (under 7 days, or no clock): Try again is row 0.
  for (const Lifetime& l : {dated(5), noClock()}) {
    const ShelfLook k = shelfLook(shelfId(Shelf::MostPlayed), l, phase(Phase::Unread, Outcome::Io), nullptr);
    TEST_ASSERT_FALSE(k.tray);
    TEST_ASSERT_TRUE(k.tryAgain);
    TEST_ASSERT_EQUAL_UINT32(1, k.rows);
  }
  // A shelf without a tray: row 0.
  for (Shelf s : {Shelf::NewFinds, Shelf::OldFavourites, Shelf::NotPlayed, Shelf::Recent}) {
    const ShelfLook k = shelfLook(shelfId(s), dated(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
    TEST_ASSERT_FALSE(k.tray);
    TEST_ASSERT_TRUE(k.tryAgain);
    TEST_ASSERT_FALSE(k.topBar);
    TEST_ASSERT_EQUAL_UINT32(1, k.rows);
    TEST_ASSERT_EQUAL(UiAsk::Visit, k.ask);
  }
  // Not played yet over a snapshot with no bits (the visit took Lists):
  // its own No room, Try again its lean ask; Working while that runs;
  // Unread if it read a fault; its rows once it lands.
  sv::MadeUp lists = fullCard();
  lists.grade = sv::Grade::Lists;
  lists.notPlayedEvery = 0;
  Snap ls;
  put(ls, lists);
  const uint32_t np = shelfId(Shelf::NotPlayed);
  ShelfLook k = shelfLook(np, dated(), kReady, ls.s());
  TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
  TEST_ASSERT_TRUE(k.tryAgain);
  TEST_ASSERT_EQUAL(UiAsk::NotPlayed, k.ask);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_room), static_cast<int>(k.page.line));
  TEST_ASSERT_EQUAL_UINT32(1, k.rows);
  k = shelfLook(np, dated(), phase(Phase::NoRoom, Outcome::Room), ls.s());
  TEST_ASSERT_EQUAL(UiAsk::NotPlayed, k.ask);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_room), static_cast<int>(k.page.line));
  k = shelfLook(np, dated(), phase(Phase::Working), ls.s());
  TEST_ASSERT_EQUAL_STRING("pending", lookName(k.page.look));
  TEST_ASSERT_EQUAL_UINT32(0, k.rows);
  k = shelfLook(np, dated(), phase(Phase::Unread, Outcome::Io), ls.s());
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_unread), static_cast<int>(k.page.line));
  TEST_ASSERT_EQUAL(UiAsk::NotPlayed, k.ask);
  // Capped (the lit cap's 3 s): "Waiting for the card…" with its Try again,
  // the lean ask.
  k = shelfLook(np, dated(), phase(Phase::Capped), ls.s());
  TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_waiting), static_cast<int>(k.page.line));
  TEST_ASSERT_TRUE(k.tryAgain);
  TEST_ASSERT_EQUAL(UiAsk::NotPlayed, k.ask);
  TEST_ASSERT_EQUAL_UINT32(1, k.rows);
  TEST_ASSERT_FALSE(k.drawLine);  // the reason is the row's
  // A Lists snapshot asked on counting day 29 and held across the UTC
  // midnight that opens the shelf (RAM's 30 days): opening it asks nothing
  // by itself (7.6), so it reads its own No room with the lean ask's Try
  // again (a departure kept: S3b's review of S5).
  sv::MadeUp early = lists;
  early.sinceDay = static_cast<uint16_t>(kToday - 28);
  Snap es;
  put(es, early);
  TEST_ASSERT_FALSE(sv::notPlayedMayOpen(es->dated(), es->today(), es->sinceDay()));
  Lifetime month = dated();
  month.today = static_cast<uint16_t>(kToday + 1);  // the next UTC day
  month.sinceDay = early.sinceDay;
  month.countingDays = sv::countingDaysOf(true, month.today, month.sinceDay);
  TEST_ASSERT_EQUAL_UINT32(30, month.countingDays);
  TEST_ASSERT_EQUAL(RowKind::Open, hubLook(month, kReady, es.s()).item[4].kind);
  k = shelfLook(np, month, kReady, es.s());
  TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_room), static_cast<int>(k.page.line));
  TEST_ASSERT_EQUAL(UiAsk::NotPlayed, k.ask);
  TEST_ASSERT_FALSE(k.page.askDue);
  Snap full;
  put(full, fullCard());
  k = shelfLook(np, dated(), kReady, full.s());
  TEST_ASSERT_EQUAL_STRING("served", lookName(k.page.look));
  TEST_ASSERT_EQUAL_UINT32(full->notPlayedCount(), k.rows);
  TEST_ASSERT_TRUE(k.rows > 0);
  TEST_ASSERT_FALSE(k.topBar);  // Not played yet: no items before its rows
  TEST_ASSERT_FALSE(k.tryAgain);
}

// The card back (the owner's call 3) on all three page shapes: the card
// out, the No card line alone, no Try again, no ask; the card in again
// (the phase still No card until the next ask), Try again with "The card is
// back".
void test_the_card_back_on_every_page() {
  const PhaseInfo noCard = phase(Phase::NoCard, Outcome::CardAway);
  Lifetime out = dated();
  out.cardIn = false;
  const Lifetime back = dated();
  // The hub.
  HubLook h = hubLook(out, noCard, nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, h.rows);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(h.page.line));
  TEST_ASSERT_FALSE(h.page.askDue);
  h = hubLook(back, noCard, nullptr);
  TEST_ASSERT_EQUAL_UINT32(7, h.rows);
  TEST_ASSERT_EQUAL(RowKind::TryAgain, h.item[6].kind);
  lineIs(Line::Text, S::stats_card_back, h.item[6], "hub, card back");
  TEST_ASSERT_FALSE(h.page.askDue);
  // The shelves: while the card is out the No card line alone, Most played
  // with no tray over it (the owner's call 3); back, its tray and Try again.
  for (uint32_t s = 0; s < kShelves; ++s) {
    const uint32_t id = shelfId(static_cast<Shelf>(s));
    ShelfLook k = shelfLook(id, out, noCard, nullptr);
    TEST_ASSERT_EQUAL_STRING("no card", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(k.page.line));
    TEST_ASSERT_FALSE(k.tryAgain);
    TEST_ASSERT_EQUAL_UINT32(0, k.rows);
    TEST_ASSERT_FALSE(k.tray);
    TEST_ASSERT_TRUE(k.drawLine);
    k = shelfLook(id, back, noCard, nullptr);
    TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(k.page.line));
    TEST_ASSERT_TRUE(k.tryAgain);
    TEST_ASSERT_EQUAL(UiAsk::Visit, k.ask);
    TEST_ASSERT_EQUAL_UINT32(1, k.rows);
    TEST_ASSERT_EQUAL(s == 0, k.tray);
    TEST_ASSERT_FALSE(k.drawLine);
  }
  // Week's empty state: its line alone, then with its button.
  WeekLook w = weekLook(out, noCard, nullptr);
  TEST_ASSERT_EQUAL_STRING("no card", lookName(w.page.look));
  TEST_ASSERT_FALSE(w.button);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(w.page.line));
  w = weekLook(back, noCard, nullptr);
  TEST_ASSERT_TRUE(w.button);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(w.page.line));

  // The remount's re-read waiting (run 3b-9's window): StatsGlue's on() is
  // false and HEAD cleared until the scan's next reserve(), but Lifetime::on
  // is on() || rereadPending() (StatsLists' own off test) and folding is
  // booting(). So the card back reads Try again with "The card is back" on
  // every page, not Off, and Artists keeps its entry row, "Counting plays…".
  Lifetime reread = dated();
  reread.folding = true;
  reread.plays = 0;
  reread.listenedMs = 0;
  reread.sinceDay = sf::kUnsetDay;
  reread.countingDays = 0;
  TEST_ASSERT_EQUAL(State::Folding, stateOf(reread));  // before Empty: HEAD cleared, not empty
  h = hubLook(reread, noCard, nullptr);
  TEST_ASSERT_EQUAL_STRING("retry", lookName(h.page.look));
  TEST_ASSERT_EQUAL_UINT32(7, h.rows);
  TEST_ASSERT_EQUAL(RowKind::TryAgain, h.item[6].kind);
  lineIs(Line::Text, S::stats_card_back, h.item[6], "hub, re-read");
  for (Shelf s : {Shelf::MostPlayed, Shelf::Recent}) {
    const ShelfLook k = shelfLook(shelfId(s), reread, noCard, nullptr);
    TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(k.page.line));
    TEST_ASSERT_TRUE(k.tryAgain);
  }
  // (The month's shelves read RAM's reason meanwhile, as their hub rows: the
  // re-read's first day not known yet, 0 counting days.)
  for (Shelf s : {Shelf::NewFinds, Shelf::OldFavourites, Shelf::NotPlayed}) {
    const ShelfLook k = shelfLook(shelfId(s), reread, noCard, nullptr);
    TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_warming), static_cast<int>(k.page.line));
    TEST_ASSERT_EQUAL(RowKind::Disabled, h.item[static_cast<int>(s) + 1].kind);
  }
  w = weekLook(reread, noCard, nullptr);
  TEST_ASSERT_TRUE(w.button);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(w.page.line));
  TEST_ASSERT_TRUE(entryDue(reread, true, false, 12));
  char b[96];
  int px = 6;
  TEST_ASSERT_EQUAL(EntryLine::Counting, entryLine(reread, monoWidth, &px, b, sizeof(b)));
  // (Read as StatsGlue::on() alone it would be Off: no rows' lines, no Try
  // again, no entry row.)
  Lifetime glueOn = reread;
  glueOn.on = false;
  TEST_ASSERT_EQUAL_STRING("off", lookName(hubLook(glueOn, noCard, nullptr).page.look));
  TEST_ASSERT_FALSE(entryDue(glueOn, true, false, 12));
}

// ---------------------------------------------------------------------------
// The shelves
// ---------------------------------------------------------------------------
void test_the_shelves() {
  // Ids: shelf * 4 + window (Most played's alone).
  TEST_ASSERT_EQUAL_UINT32(0, shelfId(Shelf::MostPlayed, sv::k7Days));
  TEST_ASSERT_EQUAL_UINT32(2, shelfId(Shelf::MostPlayed));
  TEST_ASSERT_EQUAL_UINT32(4, shelfId(Shelf::NewFinds));
  TEST_ASSERT_EQUAL_UINT32(16, shelfId(Shelf::Recent, sv::k30Days));
  for (uint32_t s = 0; s < kShelves; ++s)
    for (uint8_t w = 0; w <= sv::kAllTime; ++w) {
      const uint32_t id = shelfId(static_cast<Shelf>(s), w);
      TEST_ASSERT_EQUAL(static_cast<int>(s), static_cast<int>(shelfOf(id)));
      TEST_ASSERT_EQUAL_UINT8(s == 0 ? w : sv::kAllTime, windowOf(id));
    }
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, windowOf(3));  // past All time
  TEST_ASSERT_EQUAL(sr::kMost7, listOf(Shelf::MostPlayed, sv::k7Days));
  TEST_ASSERT_EQUAL(sr::kMost30, listOf(Shelf::MostPlayed, sv::k30Days));
  TEST_ASSERT_EQUAL(sr::kMostAll, listOf(Shelf::MostPlayed, sv::kAllTime));
  TEST_ASSERT_EQUAL(sr::kNewFinds, listOf(Shelf::NewFinds, 0));
  TEST_ASSERT_EQUAL(sr::kOldFavourites, listOf(Shelf::OldFavourites, 0));
  TEST_ASSERT_EQUAL(sr::kRecent, listOf(Shelf::Recent, 0));
  TEST_ASSERT_EQUAL(sr::kLists, listOf(Shelf::NotPlayed, 0));
  Shelf s = Shelf::Recent;
  TEST_ASSERT_FALSE(shelfOfRow(HubRow::Week, &s));
  TEST_ASSERT_FALSE(shelfOfRow(HubRow::TryAgain, &s));
  TEST_ASSERT_TRUE(shelfOfRow(HubRow::MostPlayed, &s));
  TEST_ASSERT_EQUAL(Shelf::MostPlayed, s);
  TEST_ASSERT_TRUE(shelfOfRow(HubRow::Recent, &s));
  TEST_ASSERT_EQUAL(Shelf::Recent, s);

  Snap full;
  put(full, fullCard());
  // Served: the window's rows, the tray, the top bar.
  for (uint8_t w = 0; w <= sv::kAllTime; ++w) {
    const ShelfLook k = shelfLook(shelfId(Shelf::MostPlayed, w), dated(), kReady, full.s());
    TEST_ASSERT_TRUE(k.tray);
    TEST_ASSERT_TRUE(k.topBar);
    TEST_ASSERT_EQUAL_UINT8(w, k.window);
    TEST_ASSERT_EQUAL(listOf(Shelf::MostPlayed, w), k.list);
    TEST_ASSERT_EQUAL_UINT32(full->rows(k.list), k.rows);
  }
  // A kept 30 days the tray no longer offers (7-29 days): All time.
  ShelfLook k = shelfLook(shelfId(Shelf::MostPlayed, sv::k30Days), dated(12), kReady, full.s());
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, k.window);
  TEST_ASSERT_EQUAL(sr::kMostAll, k.list);
  // Recently played: its rows with their sections.
  k = shelfLook(shelfId(Shelf::Recent), dated(), kReady, full.s());
  TEST_ASSERT_EQUAL_UINT32(recentCount(*full.s()), k.rows);
  TEST_ASSERT_TRUE(k.rows > full->rows(sr::kRecent));
  TEST_ASSERT_TRUE(k.topBar);
  // None landed in a window: "Nothing in this period." under the tray, no
  // top bar (D12).
  sv::MadeUp none = fullCard();
  none.rows[sr::kMost7] = 0;
  Snap ns;
  put(ns, none);
  k = shelfLook(shelfId(Shelf::MostPlayed, sv::k7Days), dated(), kReady, ns.s());
  TEST_ASSERT_EQUAL_UINT32(0, k.rows);
  TEST_ASSERT_FALSE(k.topBar);
  TEST_ASSERT_TRUE(k.tray);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_shelf_empty), static_cast<int>(k.page.line));
  TEST_ASSERT_TRUE(k.drawLine);
  // Closed from RAM: New finds and Old favourites with no clock and in the
  // first month; Not played yet dated in the first month.
  for (Shelf c : {Shelf::NewFinds, Shelf::OldFavourites}) {
    k = shelfLook(shelfId(c), noClock(), phase(Phase::Working), nullptr);
    TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_needs_dates), static_cast<int>(k.page.line));
    k = shelfLook(shelfId(c), dated(5), kReady, full.s());
    TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_warming), static_cast<int>(k.page.line));
    TEST_ASSERT_EQUAL_UINT32(0, k.rows);
  }
  k = shelfLook(shelfId(Shelf::NotPlayed), dated(29), kReady, full.s());
  TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_warming), static_cast<int>(k.page.line));
  TEST_ASSERT_TRUE(k.drawLine);
  // The month's edge, as the hub's rows and the state: closed at 29
  // counting days, served at 30.
  for (Shelf c : {Shelf::NewFinds, Shelf::OldFavourites, Shelf::NotPlayed}) {
    k = shelfLook(shelfId(c), dated(29), kReady, full.s());
    TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
    k = shelfLook(shelfId(c), dated(30), kReady, full.s());
    TEST_ASSERT_EQUAL_STRING("served", lookName(k.page.look));
    TEST_ASSERT_TRUE(k.rows > 0);
    TEST_ASSERT_FALSE(k.drawLine);
  }
  // Not played yet with no clock: pending before the landing (D14); landed,
  // by DAYS' span.
  k = shelfLook(shelfId(Shelf::NotPlayed), noClock(), phase(Phase::Working), nullptr);
  TEST_ASSERT_EQUAL_STRING("pending", lookName(k.page.look));
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  sv::MadeUp shortSpan = v3.spec;
  shortSpan.newestDay = static_cast<uint16_t>(shortSpan.firstDay + 28);
  Snap ss;
  put(ss, shortSpan);
  k = shelfLook(shelfId(Shelf::NotPlayed), noClock(), kReady, ss.s());
  TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_needs_dates), static_cast<int>(k.page.line));
  Snap nc;
  put(nc, v3.spec);
  k = shelfLook(shelfId(Shelf::NotPlayed), noClock(), kReady, nc.s());
  TEST_ASSERT_EQUAL_STRING("served", lookName(k.page.look));
  TEST_ASSERT_EQUAL_UINT32(nc->notPlayedCount(), k.rows);
  // Off and Empty: no rows, no Try again, no line drawn (Off's `line` is
  // the default and means nothing), no tray (D9 as replaced), served or not.
  Lifetime off = dated();
  off.on = false;
  Lifetime empty = dated();
  empty.plays = 0;
  for (const Lifetime& l : {off, empty}) {
    for (const sv::Snapshot* snap : {static_cast<const sv::Snapshot*>(full.s()), static_cast<const sv::Snapshot*>(nullptr)}) {
      for (uint32_t si = 0; si < kShelves; ++si) {
        k = shelfLook(shelfId(static_cast<Shelf>(si)), l, phase(Phase::Idle, Outcome::None, sv::kWhyOff), snap);
        TEST_ASSERT_EQUAL_STRING(l.on ? "no plays" : "off", lookName(k.page.look));
        TEST_ASSERT_EQUAL_UINT32(0, k.rows);
        TEST_ASSERT_FALSE(k.tryAgain);
        TEST_ASSERT_FALSE(k.page.askDue);
        TEST_ASSERT_FALSE(k.drawLine);
        TEST_ASSERT_FALSE(k.tray);
        TEST_ASSERT_FALSE(k.topBar);
      }
    }
  }
  // Idle with the stats on: the Working line, one ask due.
  k = shelfLook(shelfId(Shelf::Recent), dated(), phase(Phase::Idle, Outcome::Cancelled), nullptr);
  TEST_ASSERT_EQUAL_STRING("pending", lookName(k.page.look));
  TEST_ASSERT_TRUE(k.page.askDue);
  TEST_ASSERT_TRUE(k.drawLine);
  // Served with rows: no line.
  k = shelfLook(shelfId(Shelf::Recent), dated(), kReady, full.s());
  TEST_ASSERT_FALSE(k.drawLine);
}

// ---------------------------------------------------------------------------
// The tray (7.6, 7.9)
// ---------------------------------------------------------------------------
void test_the_tray_by_days_and_its_hits() {
  // By the counting days: none under 7 or with no clock; 7-29 two cells;
  // 30 on three.
  TEST_ASSERT_EQUAL_UINT8(0, trayOf(dated(6), nullptr).cells);
  TEST_ASSERT_EQUAL_UINT8(0, trayOf(noClock(), nullptr).cells);
  Tray t = trayOf(dated(7), nullptr);
  TEST_ASSERT_EQUAL_UINT8(2, t.cells);
  TEST_ASSERT_EQUAL_UINT8(sv::k7Days, t.window[0]);
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, t.window[1]);
  TEST_ASSERT_EQUAL_UINT8(2, trayOf(dated(29), nullptr).cells);
  t = trayOf(dated(30), nullptr);
  TEST_ASSERT_EQUAL_UINT8(3, t.cells);
  TEST_ASSERT_EQUAL_UINT8(sv::k7Days, t.window[0]);
  TEST_ASSERT_EQUAL_UINT8(sv::k30Days, t.window[1]);
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, t.window[2]);
  TEST_ASSERT_EQUAL_UINT8(30, t.days30);  // before the landing: 30
  // RCNT short (the rank's mark, rcntShort(): capped, DAYS played on days it
  // lost): its oldest day inside the window gives the days held.
  sv::MadeUp m = fullCard();
  Snap a;
  put(a, m);  // its RCNT reaches past the window: 30
  TEST_ASSERT_FALSE(a->rcntShort());
  TEST_ASSERT_EQUAL_UINT8(30, trayOf(dated(), a.s()).days30);
  m.rcntFirstDay = static_cast<uint16_t>(kToday - 15);
  m.rcntShort = true;
  Snap b;
  put(b, m);
  TEST_ASSERT_TRUE(b->rcntShort());
  TEST_ASSERT_EQUAL_UINT8(16, trayOf(dated(), b.s()).days30);
  // A gap at the window's start, not capped (the listener away until 3 days
  // ago: RCNT holds only days with plays, so its oldest is 3 days back): no
  // mark, 30, never fewer than the 7-day cell.
  for (const uint16_t back : {uint16_t{15}, uint16_t{2}}) {
    m.rcntFirstDay = static_cast<uint16_t>(kToday - back);
    m.rcntShort = false;
    Snap g;
    put(g, m);
    TEST_ASSERT_FALSE(g->rcntShort());
    TEST_ASSERT_EQUAL_UINT8(30, trayOf(dated(), g.s()).days30);
  }
  m.rcntShort = true;
  m.rcntFirstDay = static_cast<uint16_t>(kToday - 29);  // the window's first day: all 30 held
  Snap c;
  put(c, m);
  TEST_ASSERT_EQUAL_UINT8(30, trayOf(dated(), c.s()).days30);
  m.rcntFirstDay = sf::kUnsetDay;  // nothing in RCNT
  Snap d;
  put(d, m);
  TEST_ASSERT_EQUAL_UINT8(30, trayOf(dated(), d.s()).days30);
  // The mark comes from a real rank's report through write() (the rank's
  // own test: test_stats_rank), and undated snapshots never carry it.
  sr::Report rep;
  rep.rcntFirstDay = static_cast<uint16_t>(kToday - 15);
  rep.days30FirstDay = static_cast<uint16_t>(kToday - 29);
  TEST_ASSERT_TRUE(sr::rcntShort(rep));
  rep.days30FirstDay = rep.rcntFirstDay;  // the gap: DAYS' first played day is RCNT's
  TEST_ASSERT_FALSE(sr::rcntShort(rep));
  rep.days30FirstDay = sf::kUnsetDay;
  TEST_ASSERT_FALSE(sr::rcntShort(rep));
  m.dated = false;
  m.rcntFirstDay = static_cast<uint16_t>(kToday - 15);
  Snap u;
  put(u, m);
  TEST_ASSERT_FALSE(u->rcntShort());

  // The cells' boxes: three of 96 at 8, 108, 208; two of 146 at 8, 158 (7.6's 148
  // at 160 would end at 307); inside the tray (6-305), 2 px in from its ends.
  const Tray three = trayOf(dated(30), nullptr), two = trayOf(dated(7), nullptr);
  TEST_ASSERT_EQUAL_INT(96, trayCellW(three));
  TEST_ASSERT_EQUAL_INT(146, trayCellW(two));
  for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_INT(kTrayCellX3[i], trayCellX(three, i));
  for (int i = 0; i < 2; ++i) TEST_ASSERT_EQUAL_INT(kTrayCellX2[i], trayCellX(two, i));
  for (const Tray& tr : {three, two}) {
    TEST_ASSERT_EQUAL_INT(kTrayX + 2, trayCellX(tr, 0));
    TEST_ASSERT_EQUAL_INT(kTrayX + kTrayW - 2, trayCellX(tr, tr.cells - 1) + trayCellW(tr));
    for (int i = 1; i < tr.cells; ++i)
      TEST_ASSERT_EQUAL_INT(4, trayCellX(tr, i) - (trayCellX(tr, i - 1) + trayCellW(tr)));
    TEST_ASSERT_EQUAL_INT(tr.cells == 3 ? 92 : 142, trayCellW(tr) - 4);  // the label's room
  }
  // The splits: 106 and 219; 162 (156 + skew 6).
  TEST_ASSERT_EQUAL_INT(0, trayCellAt(three, 0, false));
  TEST_ASSERT_EQUAL_INT(0, trayCellAt(three, 105, false));
  TEST_ASSERT_EQUAL_INT(1, trayCellAt(three, 106, false));
  TEST_ASSERT_EQUAL_INT(1, trayCellAt(three, 218, false));
  TEST_ASSERT_EQUAL_INT(2, trayCellAt(three, 219, false));
  TEST_ASSERT_EQUAL_INT(2, trayCellAt(three, 319, false));
  TEST_ASSERT_EQUAL_INT(0, trayCellAt(two, 161, false));
  TEST_ASSERT_EQUAL_INT(1, trayCellAt(two, 162, false));
  // A reading clamped at the edge: the last cell.
  TEST_ASSERT_EQUAL_INT(2, trayCellAt(three, 10, true));
  TEST_ASSERT_EQUAL_INT(1, trayCellAt(two, 10, true));
  TEST_ASSERT_EQUAL_INT(-1, trayCellAt(trayOf(dated(5), nullptr), 100, false));
  // Each cell's centre hits its cell on the lab's panel (x read too far
  // right: TouchCalibration::labFitX()) and a true one, 16 px inside its
  // splits at least (as test_ui_library's mood grid).
  const TouchCalibration::Axis lab = TouchCalibration::labFitX();
  // Where cell k + 1 starts: the first reading that is it.
  auto split = [](const Tray& tr, int k) {
    for (int x = 0; x < 320; ++x)
      if (trayCellAt(tr, x, false) == k + 1) return x;
    return 320;
  };
  TEST_ASSERT_EQUAL_INT(106, split(three, 0));
  TEST_ASSERT_EQUAL_INT(219, split(three, 1));
  TEST_ASSERT_EQUAL_INT(162, split(two, 0));
  int least = 1000;
  for (const Tray& tr : {three, two}) {
    for (int panel = 0; panel < 2; ++panel) {
      for (int k = 0; k < tr.cells; ++k) {
        const int centre = trayCellX(tr, k) + trayCellW(tr) / 2;
        const long r = panel ? centre : std::lround(lab.unmap(static_cast<float>(centre)));
        const bool edge = r >= 319;
        const int x = edge ? 319 : static_cast<int>(r);
        char msg[80];
        snprintf(msg, sizeof(msg), "%d cells, %s, cell %d's centre %d reads %d", tr.cells, panel ? "true" : "lab", k,
                 centre, x);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k, trayCellAt(tr, x, edge), msg);
        // Its room either side: to the split before it, and the one after.
        const int left = k ? x - split(tr, k - 1) : 1000;
        const int right = k < tr.cells - 1 && !edge ? split(tr, k) - 1 - x : 1000;
        const int margin = std::min(left, right);
        TEST_ASSERT_TRUE_MESSAGE(margin >= 16, msg);
        least = std::min(least, margin);
      }
    }
  }
  printf("[stats] the tray's least margin, lab and true panels: %d px\n", least);
  // The kept window: offered, kept; else All time.
  TEST_ASSERT_EQUAL_UINT8(sv::k30Days, windowShown(sv::k30Days, three));
  TEST_ASSERT_EQUAL_UINT8(sv::k7Days, windowShown(sv::k7Days, two));
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, windowShown(sv::k30Days, two));
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, windowShown(sv::k7Days, trayOf(dated(3), nullptr)));
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, windowShown(9, three));
}

// ---------------------------------------------------------------------------
// Recently played's sections (D10)
// ---------------------------------------------------------------------------
namespace {

struct WantRecent {
  bool heading;
  Section section;
  uint32_t row;
  bool timed;
  uint16_t day;  // Day's (kUnsetDay: not checked)
};

void sameRecent(const sv::Snapshot& s, const WantRecent* want, uint32_t n, const char* what) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(n, recentCount(s), what);
  for (uint32_t i = 0; i < n; ++i) {
    const RecentItem it = recentAt(s, i);
    char msg[48];
    snprintf(msg, sizeof(msg), "%s, list row %u", what, static_cast<unsigned>(i));
    TEST_ASSERT_EQUAL_MESSAGE(want[i].heading, it.heading, msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(want[i].section), static_cast<int>(it.section), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].row, it.row, msg);
    TEST_ASSERT_EQUAL_MESSAGE(want[i].timed, it.timed, msg);
    if (want[i].day != sf::kUnsetDay) TEST_ASSERT_EQUAL_UINT16_MESSAGE(want[i].day, it.day, msg);
    if (!it.heading) TEST_ASSERT_EQUAL_UINT32_MESSAGE(i, recentListRow(s, it.row), msg);
  }
}

// A Recently played row's `when` written over (a test's own row).
void putWhen(Snap& b, uint32_t row, uint32_t when) {
  uint8_t* p = reinterpret_cast<uint8_t*>(b.m.data()) + sv::offsetOf(sr::kRecent, kAlbums) +
               static_cast<size_t>(row) * sv::kStrides[sr::kRecent];
  p[2] = static_cast<uint8_t>(when);
  p[3] = static_cast<uint8_t>(when >> 8);
  p[4] = static_cast<uint8_t>(when >> 16);
}

}  // namespace

void test_recently_played_s_sections() {
  // Dated: now 10:00 on day 313; rows 30 min, 12 h 30, 24 h 30, 36 h 30,
  // 48 h 30, 60 h 30 and 72 h 30 ago (313; 312, 312; 311, 311; 310, 310):
  // Today, Yesterday, then a heading a day, two days in a row; then one of
  // this session and one earlier, undated: both under one Earlier, last.
  constexpr uint16_t U = sf::kUnsetDay;
  sv::MadeUp m = fullCard();
  m.time = (kToday * 1440u + 600) * 60 + 1;
  m.rows[sr::kRecent] = 7;
  m.recentFirstAgoMin = 30;
  m.recentStepMin = 720;
  m.sessionRows = 1;
  m.earlierRows = 1;
  Snap a;
  put(a, m);
  TEST_ASSERT_EQUAL_UINT32(9, a->rows(sr::kRecent));
  const WantRecent want[] = {
      {true, Section::Today, 0, false, U},    {false, Section::Today, 0, true, U},
      {true, Section::Yesterday, 1, false, U}, {false, Section::Yesterday, 1, true, U},
      {false, Section::Yesterday, 2, true, U}, {true, Section::Day, 3, false, 311},
      {false, Section::Day, 3, true, 311},     {false, Section::Day, 4, true, 311},
      {true, Section::Day, 5, false, 310},     {false, Section::Day, 5, true, 310},
      {false, Section::Day, 6, true, 310},     {true, Section::Earlier, 7, false, U},
      {false, Section::Earlier, 7, false, U},  {false, Section::Earlier, 8, false, U}};
  sameRecent(*a.s(), want, 14, "dated");
  TEST_ASSERT_EQUAL_UINT32(kToday * 1440u + 600 - 30, a->rowAt(sr::kRecent, 0).when);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenSession, a->rowAt(sr::kRecent, 7).when);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenEarlier, a->rowAt(sr::kRecent, 8).when);

  // Undated: This session, then Earlier, no times.
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  sv::MadeUp u = v3.spec;
  u.sessionRows = 3;
  u.earlierRows = 2;
  Snap b;
  put(b, u);
  TEST_ASSERT_FALSE(b->dated());
  TEST_ASSERT_EQUAL_UINT32(5, b->rows(sr::kRecent));
  const WantRecent wantU[] = {{true, Section::Session, 0, false, U},  {false, Section::Session, 0, false, U},
                              {false, Section::Session, 1, false, U}, {false, Section::Session, 2, false, U},
                              {true, Section::Earlier, 3, false, U},  {false, Section::Earlier, 3, false, U},
                              {false, Section::Earlier, 4, false, U}};
  sameRecent(*b.s(), wantU, 7, "undated");
  // Rows with a dated lastAt (the rank puts them first: 4.3), two days of
  // them: under their days, a heading each, no Today, no time. A track
  // replayed this session with no clock is such a row too (an undated play
  // leaves lastAt: 5.9; the copy-out codes This session only for lastAt 0),
  // so it reads under its old day, not This session: the owner's call.
  putWhen(b, 0, 300u * 1440 + 90);
  putWhen(b, 1, 299u * 1440 + 1300);
  const WantRecent wantD[] = {{true, Section::Day, 0, false, 300},     {false, Section::Day, 0, false, 300},
                              {true, Section::Day, 1, false, 299},     {false, Section::Day, 1, false, 299},
                              {true, Section::Session, 2, false, U},   {false, Section::Session, 2, false, U},
                              {true, Section::Earlier, 3, false, U},   {false, Section::Earlier, 3, false, U},
                              {false, Section::Earlier, 4, false, U}};
  sameRecent(*b.s(), wantD, 9, "undated, two dated days");
  // Past the end: no row.
  TEST_ASSERT_FALSE(recentAt(*b.s(), 9).heading);
  TEST_ASSERT_EQUAL_UINT32(b->rows(sr::kRecent), recentAt(*b.s(), 9).row);
}

// ---------------------------------------------------------------------------
// Last 7 days
// ---------------------------------------------------------------------------
void test_week_from_a_made_up_snapshot() {
  Snap a;
  put(a, fullCard());
  WeekFigures w = weekFigures(*a.s());
  // 7 h 37 min, 118 plays (the frames' week: 4-10 Nov).
  TEST_ASSERT_EQUAL_UINT64((7ull * 60 + 37) * 60000, w.listenedMs);
  TEST_ASSERT_EQUAL_UINT32(118, w.plays);
  TEST_ASSERT_EQUAL_UINT16(kToday - 6, w.firstDay);
  // Saturday's 2 h 1 min the top: 44 px; the others to it, rounded.
  const uint8_t bars[7] = {15, 21, 13, 44, 32, 23, 18};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bars, w.bar, 7);
  TEST_ASSERT_EQUAL_INT8(3, w.bigDay);
  TEST_ASSERT_EQUAL_UINT32(121 * 60000u, a->day(3).listenedMs);
  // Auto DJ: 46 of 53, 87%.
  TEST_ASSERT_TRUE(w.autoDj);
  TEST_ASSERT_EQUAL_UINT8(87, w.autoDjPct);

  // A tie goes to the later day; none before sinceDay; nothing played, no
  // bar; Auto DJ's line from 5 tracks.
  sv::MadeUp m = fullCard();
  const uint32_t mins[7] = {50, 0, 90, 10, 20, 90, 1};
  for (int i = 0; i < 7; ++i) {
    m.days[i] = sr::Day();
    m.days[i].listenedMs = mins[i] * 60000;
    m.days[i].plays = static_cast<uint16_t>(mins[i] ? 1 : 0);
  }
  m.days[2].autoDjPlays = 3;
  m.days[4].autoDjSkips = 1;
  m.sinceDay = static_cast<uint16_t>(kToday - 5);  // day 0 is before it
  Snap b;
  put(b, m);
  w = weekFigures(*b.s());
  TEST_ASSERT_EQUAL_INT8(5, w.bigDay);
  const uint8_t bars2[7] = {0, 0, 44, 5, 10, 44, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bars2, w.bar, 7);
  TEST_ASSERT_FALSE(w.autoDj);  // 4 tracks
  m.days[6].autoDjPlays = 1;
  Snap c;
  put(c, m);
  w = weekFigures(*c.s());
  TEST_ASSERT_TRUE(w.autoDj);
  TEST_ASSERT_EQUAL_UINT8(80, w.autoDjPct);
  // The hub's 24 px from the same days.
  uint32_t ms[7];
  for (int i = 0; i < 7; ++i) ms[i] = mins[i] * 60000;
  uint8_t out[7];
  barHeights(ms, static_cast<uint16_t>(kToday - 6), static_cast<uint16_t>(kToday - 5), 24, out);
  const uint8_t bars24[7] = {0, 0, 24, 3, 5, 24, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bars24, out, 7);
  barHeights(ms, sf::kUnsetDay, 0, 24, out);
  for (uint8_t h : out) TEST_ASSERT_EQUAL_UINT8(0, h);
  // sinceDay's own day has its bar (4.2: none for a day *before* it), the
  // day before it none, though both have time.
  const uint32_t edge[7] = {30 * 60000u, 20 * 60000u, 0, 0, 0, 0, 40 * 60000u};
  barHeights(edge, static_cast<uint16_t>(kToday - 6), static_cast<uint16_t>(kToday - 5), 44, out);
  const uint8_t edgeBars[7] = {0, 22, 0, 0, 0, 0, 44};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(edgeBars, out, 7);
  // ... and every day from it on, the window's first among them.
  barHeights(edge, static_cast<uint16_t>(kToday - 6), static_cast<uint16_t>(kToday - 6), 44, out);
  const uint8_t allBars[7] = {33, 22, 0, 0, 0, 0, 44};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(allBars, out, 7);

  // A week with nothing: no big day, no bars, no Auto DJ.
  for (int i = 0; i < 7; ++i) m.days[i] = sr::Day();
  Snap e;
  put(e, m);
  w = weekFigures(*e.s());
  TEST_ASSERT_EQUAL_INT8(-1, w.bigDay);
  TEST_ASSERT_EQUAL_UINT64(0, w.listenedMs);
  TEST_ASSERT_FALSE(w.autoDj);
  for (uint8_t h : w.bar) TEST_ASSERT_EQUAL_UINT8(0, h);
  // Undated: nothing at all.
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  Snap u;
  put(u, v3.spec);
  w = weekFigures(*u.s());
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, w.firstDay);
  TEST_ASSERT_EQUAL_INT8(-1, w.bigDay);

  // Week's look: served, the figures; with no clock closed ("Needs dates":
  // the hub's row 1 is the status row then); off, nothing.
  TEST_ASSERT_EQUAL_STRING("served", lookName(weekLook(dated(), kReady, a.s()).page.look));
  WeekLook k = weekLook(noClock(), kReady, u.s());
  TEST_ASSERT_EQUAL_STRING("closed", lookName(k.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_needs_dates), static_cast<int>(k.page.line));
  TEST_ASSERT_FALSE(k.button);
  Lifetime off = dated();
  off.on = false;
  k = weekLook(off, phase(Phase::Idle), nullptr);
  TEST_ASSERT_EQUAL_STRING("off", lookName(k.page.look));
  TEST_ASSERT_FALSE(k.button);
}

// ---------------------------------------------------------------------------
// The made-up views (9.3's uiSx<n>)
// ---------------------------------------------------------------------------
void test_the_made_up_views() {
  MadeUpView v;
  TEST_ASSERT_FALSE(madeUpView(0, kStamp, kTracks, kAlbums, &v));  // the card's own
  TEST_ASSERT_FALSE(madeUpView(kMadeUpViews, kStamp, kTracks, kAlbums, &v));
  const State states[kMadeUpViews] = {State::Ready,  State::Ready,   State::Empty,  State::NoDates,
                                      State::FirstMonth, State::Folding, State::NoCard, State::Ready,
                                      State::Ready,  State::Ready};
  const Phase phases[kMadeUpViews] = {Phase::Idle,    Phase::Ready,   Phase::Idle,  Phase::Ready,  Phase::Ready,
                                      Phase::Working, Phase::NoCard,  Phase::Waiting, Phase::NoRoom, Phase::Unread};
  for (uint8_t n = 1; n < kMadeUpViews; ++n) {
    char msg[24];
    snprintf(msg, sizeof(msg), "view %u", static_cast<unsigned>(n));
    TEST_ASSERT_TRUE_MESSAGE(madeUpView(n, kStamp, kTracks, kAlbums, &v), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(stateName(states[n]), stateName(stateOf(v.life)), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(sv::phaseName(phases[n]), sv::phaseName(v.phase.phase), msg);
    TEST_ASSERT_EQUAL_MESSAGE(n == 1 || n == 3 || n == 4, v.snapshot, msg);
    TEST_ASSERT_TRUE_MESSAGE(v.life.madeUp, msg);
    TEST_ASSERT_FALSE_MESSAGE(asksOnEdge(v.life), msg);
    // Nothing due on any page (D16), its snapshot written or not.
    TEST_ASSERT_FALSE_MESSAGE(hubLook(v.life, v.phase, nullptr).page.askDue, msg);
    TEST_ASSERT_FALSE_MESSAGE(weekLook(v.life, v.phase, nullptr).page.askDue, msg);
    for (uint32_t s = 0; s < kShelves; ++s)
      TEST_ASSERT_FALSE_MESSAGE(shelfLook(shelfId(static_cast<Shelf>(s)), v.life, v.phase, nullptr).page.askDue, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sv::countingDaysOf(v.life.dated, v.life.today, v.life.sinceDay),
                                     v.life.countingDays, msg);
    if (!v.snapshot) continue;
    // Its snapshot: the live index's key, made up, served as a landed one.
    Snap b;
    put(b, v.spec);
    TEST_ASSERT_TRUE_MESSAGE(b->resolved() && b->madeUp(), msg);
    TEST_ASSERT_TRUE_MESSAGE(b->matches(kStamp, kTracks), msg);
    TEST_ASSERT_EQUAL_MESSAGE(v.life.dated, b->dated(), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("served", lookName(hubLook(v.life, v.phase, b.s()).page.look), msg);
  }
  // 1: a full card, every row open over the made-up rows.
  TEST_ASSERT_TRUE(madeUpView(1, kStamp, kTracks, kAlbums, &v));
  Snap one;
  put(one, v.spec);
  HubLook h = hubLook(v.life, v.phase, one.s());
  for (uint32_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL(Line::Data, h.item[i].line);
  TEST_ASSERT_EQUAL_UINT8(3, trayOf(v.life, one.s()).cells);
  TEST_ASSERT_TRUE(one->hasNotPlayed());
  // ... its snapshot not written (the Dance tab up, past kBound, refused:
  // S12 says so on the console): Pending, "Counting plays…", nothing due,
  // so no real visit is asked under a look-only view (D16).
  h = hubLook(v.life, v.phase, nullptr);
  TEST_ASSERT_EQUAL_STRING("pending", lookName(h.page.look));
  TEST_ASSERT_FALSE(h.page.askDue);
  for (uint32_t i = 0; i < 6; ++i) lineIs(Line::Text, S::stats_counting, h.item[i], "view 1, nothing written");
  ShelfLook sk = shelfLook(shelfId(Shelf::MostPlayed), v.life, v.phase, nullptr);
  TEST_ASSERT_EQUAL_STRING("pending", lookName(sk.page.look));
  TEST_ASSERT_FALSE(sk.page.askDue);
  TEST_ASSERT_FALSE(weekLook(v.life, v.phase, nullptr).page.askDue);
  // The same view as the host's (madeUp false): one ask due.
  Lifetime host = v.life;
  host.madeUp = false;
  TEST_ASSERT_TRUE(hubLook(host, v.phase, nullptr).page.askDue);
  // 3: no clock, Not played yet open (DAYS' 30 days or more: NoClock's
  // frame), Recently played by session.
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v));
  Snap three;
  put(three, v.spec);
  h = hubLook(v.life, v.phase, three.s());
  TEST_ASSERT_EQUAL(RowKind::Status, h.item[0].kind);
  TEST_ASSERT_EQUAL(RowKind::Open, h.item[4].kind);
  TEST_ASSERT_EQUAL(Section::Session, recentAt(*three.s(), 0).section);
  // 4: day 5 of counting: no tray, the month's rows disabled, opens 6 Dec.
  TEST_ASSERT_TRUE(madeUpView(4, kStamp, kTracks, kAlbums, &v));
  TEST_ASSERT_EQUAL_UINT32(5, v.life.countingDays);
  TEST_ASSERT_EQUAL_UINT16(339, opensDay(v.life));
  Snap four;
  put(four, v.spec);
  TEST_ASSERT_FALSE(four->hasNotPlayed());  // the Lists grade: Not played yet can't open
  TEST_ASSERT_EQUAL_UINT8(0, trayOf(v.life, four.s()).cells);
  h = hubLook(v.life, v.phase, four.s());
  TEST_ASSERT_EQUAL(RowKind::Disabled, h.item[2].kind);
  WeekFigures w = weekFigures(*four.s());
  // 5-11 Nov: no bar before the first day (5 and 6 Nov), the first day's
  // own (7 Nov, 12 min), the top day 44 px (47 min) and the rest to it.
  const uint8_t bars4[7] = {0, 0, 11, 44, 28, 38, 29};
  TEST_ASSERT_EQUAL_UINT16(310, four->sinceDay());
  TEST_ASSERT_EQUAL_UINT16(308, w.firstDay);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bars4, w.bar, 7);
  // 2: nothing played, the empty state; 5: folding, every line counting;
  // 6: no card, the one line.
  TEST_ASSERT_TRUE(madeUpView(2, kStamp, kTracks, kAlbums, &v));
  TEST_ASSERT_EQUAL_UINT32(0, hubLook(v.life, v.phase, nullptr).rows);
  TEST_ASSERT_TRUE(madeUpView(5, kStamp, kTracks, kAlbums, &v));
  h = hubLook(v.life, v.phase, nullptr);
  for (uint32_t i = 0; i < 6; ++i) lineIs(Line::Text, S::stats_counting, h.item[i], "view 5");
  char b[96];
  int px = 6;
  TEST_ASSERT_EQUAL(EntryLine::Counting, entryLine(v.life, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_TRUE(madeUpView(6, kStamp, kTracks, kAlbums, &v));
  h = hubLook(v.life, v.phase, nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, h.rows);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_card), static_cast<int>(h.page.line));
  TEST_ASSERT_FALSE(entryDue(v.life, true, false, 10));

  // 7-9: waiting, no room, unread, on the hub, every shelf and Week.
  const struct {
    uint8_t n;
    Look look;
    S line;
  } views[] = {{7, Look::Pending, S::stats_waiting}, {8, Look::Retry, S::stats_no_room},
               {9, Look::Retry, S::stats_unread}};
  for (const auto& x : views) {
    char msg[24];
    snprintf(msg, sizeof(msg), "view %u", static_cast<unsigned>(x.n));
    TEST_ASSERT_TRUE(madeUpView(x.n, kStamp, kTracks, kAlbums, &v));
    TEST_ASSERT_FALSE(v.snapshot);
    const bool retry = x.look == Look::Retry;
    h = hubLook(v.life, v.phase, nullptr);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(lookName(x.look), lookName(h.page.look), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(retry ? 7 : 6, h.rows, msg);
    if (retry) {
      // The Try again row drawn (look only); its tap, as every act under a
      // made-up view, is the pages' to refuse (S9, S12).
      TEST_ASSERT_EQUAL_MESSAGE(RowKind::TryAgain, h.item[6].kind, msg);
      lineIs(Line::Text, x.line, h.item[6], msg);
    } else {
      for (uint32_t i = 0; i < 6; ++i) lineIs(Line::Text, x.line, h.item[i], msg);
    }
    for (uint32_t s = 0; s < kShelves; ++s) {
      const ShelfLook k = shelfLook(shelfId(static_cast<Shelf>(s)), v.life, v.phase, nullptr);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(lookName(x.look), lookName(k.page.look), msg);
      TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(x.line), static_cast<int>(k.page.line), msg);
      TEST_ASSERT_EQUAL_MESSAGE(retry, k.tryAgain, msg);
      TEST_ASSERT_EQUAL_MESSAGE(s == 0, k.tray, msg);
    }
    const WeekLook wk = weekLook(v.life, v.phase, nullptr);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(lookName(x.look), lookName(wk.page.look), msg);
    TEST_ASSERT_EQUAL_MESSAGE(retry, wk.button, msg);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_states_of_the_lifetime);
  RUN_TEST(test_not_played_open_s_three_inputs);
  RUN_TEST(test_the_page_look_by_phase);
  RUN_TEST(test_the_rising_edge_s_asks);
  RUN_TEST(test_on_stats_pages_over_stacks);
  RUN_TEST(test_the_entry_row_and_its_line);
  RUN_TEST(test_hub_rows_in_each_state);
  RUN_TEST(test_the_phase_lines_on_every_page);
  RUN_TEST(test_the_try_again_row_s_place);
  RUN_TEST(test_the_card_back_on_every_page);
  RUN_TEST(test_the_shelves);
  RUN_TEST(test_the_tray_by_days_and_its_hits);
  RUN_TEST(test_recently_played_s_sections);
  RUN_TEST(test_week_from_a_made_up_snapshot);
  RUN_TEST(test_the_made_up_views);
  return UNITY_END();
}
