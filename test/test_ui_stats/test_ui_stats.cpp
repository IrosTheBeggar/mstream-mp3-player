// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the listening stats' screens as kinds and numbers
// (lib/core/StatsView: docs/LISTENING-STATS.md 4.4, 7.1-7.7, 7.11; S3b's
// C17a), against the made-up snapshots statsvisit::writeMadeUp() writes:
// - 4.4's states from the lifetime, in their order (Folding before Empty:
//   HEAD is partial while the boot's fold runs), and the day a first-month
//   row opens;
// - the lifetime from RAM as the host fills it (lifetimeOf(): S3b's S7):
//   HEAD's figures, the clock's day and minute (the minute a row's `when`
//   gives), the counting days (a first day after today, none), the clock
//   unset or not believed, and the host's flags into the states;
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
// And the words (lib/core/StatsText: S3b's C17b), in English with a made-up
// font (each character the same px):
// - each line's form and its short form where the room says so: the hub's
//   header line, Last 7 days' time and plays, Most played's top three cut
//   with "…", New finds' long or short count, Old favourites', Not played
//   yet's albums since the first day (or the day alone), Recently played's
//   title and how long ago, "Opens 6 Dec" long or short, the toasts, the
//   empty state's rule; the shelves' header lines and tray labels;
// - a ranked row's line on each list, "artist cut, count kept": the parts
//   measured first and kept whole, the artist cut to what they leave, left
//   out when that is under kMinNameW;
// - relative times at 0 s, 59 s, 59 min, 23 h 59 and 24 h (D10);
// - day labels: weekdays from day 0's Thursday, a section's date, Last 7
//   days' range in one month, across a month's end and a year's, a
//   minute's day with a time zone's offset either way;
// - Last 7 days' big day and Auto DJ's line, where its header line goes,
//   and the stat value's segments in English, Polish, Japanese and Chinese
//   (numbers apart from words), with their width.
// The texts' widths in every language are test_ui_library's (S6).
// And the hub as ui's LibraryPage draws and touches it (S3b's S9):
// - each row kind's touch (pressable, inert, held: ListView::Source's
//   hooks, a table), the rows' titles, a shelf's title;
// - the header's lifetime line by state (none while folding or with
//   nothing played), uiSh's reach;
// - the shape and texts a page polls: they move with the rows' kinds and
//   count (the card back, the clock, the month) and with the phase's line,
//   a reason or a new ask's snapshot, never with an unchanged look;
// - each row's line from a made-up snapshot through a title table (the
//   top three, the uncapped counts, the newest title and how long ago);
// - a new minute: when "25 min ago" moves, and the minute from its low
//   16 bits;
// - the latched entry row at the boot fold's end (A20: "Couldn't read the
//   stats" with the stats off since), and its tap's reach (that row opens
//   nothing);
// - S9's review: the card back after the session's first visit was made
//   with the card out (the rise's mark through phaseSeen(): Try again with
//   "The card is back", nothing due; a phase the host moved is its own);
//   the shape's bit 7 left for the header's line; the empty state to No
//   card's line drawn whole (hubDrawsWhole()).
// And the shelves as the page draws, touches and acts on them (S3b's S10):
// - each shelf's rows from a made-up snapshot: ranked rows at their
//   snapshot rows (the act's view reads the same ids), Recently played's
//   headings and rows, Not played yet's albums A-Z, the Try again row (Not
//   played yet's its lean ask), none pending; the row that holds a track
//   (kShowPlaying's); an id the index hasn't never drawn or acted on (D6);
// - each row kind's touch (a table: ranked and album rows hold), when the
//   rows are pending (D5, Not played yet's lean ask among it) and when they
//   arrive; the shape (look, the tray's cells, the window, "16 days"): when
//   the tray and the header are drawn again; the console's uiS<shelf>[w];
// - Recently played at a new minute: the first row on screen whose "25 min
//   ago" moved.
// And S3b's cross-cutting review: the hub's header line and the entry row's
// with the card out (nothing played, folding: the card hides neither), an
// Off hub's rows refused (hubTap(): no blank shelf or Week pushed), Most
// played's header at a capped RCNT naming what the picked cell names; uiS's
// checksum over a shelf, Not played yet's albums in one walk of the bits
// (shelfSum(): the values albumAt() gives).
// And Last 7 days as the page draws it (S3b's S11): each look through the
// list's body hook (the card out's No card line alone there, as Pending's)
// or as the empty state (Try again's button with Retry and the card back),
// its header's days and when the
// header is drawn again, its texts (the count, the big day, Auto DJ's rate;
// none for a week with nothing or an undated one), its columns, and the
// status band off on it alone. S11's review: a real ask's undated snapshot
// once the clock is set (undatedStale(): never with no clock, a dated or
// made-up snapshot, or a made-up view's lifetime), released, then Week and
// the hub's Working line with one ask due, and the dated landing not stale.
// And the made-up views as the Ui takes them (S3b's S12, uiSx<n>): its
// accessors' lifetime and phase (no recipe), the views that write, the
// console's argument and names, the rise writing a view again (never an
// ask), the snapshot the pages read under a view and with none (a real
// ask's write clears the mark), and every act refused under a view but the
// tray's window.
// And 7.10's "A new UTC day": the counting days a page latches at its
// enter() and the wake (the hub's rows 3-5, the tray and the window it
// shows), held across a new day (6 to 7, 29 to 30), the clock set back (30
// to 29, 7 to 6) and a fold, kept through a re-read and taken again at a
// lifetime's first whole reading (its end, the clock set).
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
#include "IdView.h"
#include "ListLayout.h"
#include "NavModel.h"
#include "StatsFold.h"
#include "StatsRank.h"
#include "StatsText.h"
#include "StatsView.h"
#include "StatsVisit.h"
#include "TextFit.h"
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
// The lifetime from RAM (lifetimeOf(): the host's statsLifetime(), S3b's S7)
// ---------------------------------------------------------------------------
void test_the_lifetime_from_ram() {
  sf::Head h;
  h.plays = 1204;
  h.skips = 99;  // (not the lifetime's)
  h.listenedMs = (uint64_t{74} * 60 + 10) * 60000;
  h.sinceDay = kSince;
  // Tue 10 Nov 2026, 21:40:59.999 UTC: day 313, minute 21:40.
  const uint64_t minute = sf::kMinMs + uint64_t{kToday} * sf::kDayMs + (uint64_t{21} * 60 + 40) * 60000;
  const uint64_t now = minute + 59999;
  Lifetime l = lifetimeOf(true, false, true, h, kSince, now);
  TEST_ASSERT_TRUE(l.on);
  TEST_ASSERT_FALSE(l.folding);
  TEST_ASSERT_TRUE(l.cardIn);
  TEST_ASSERT_TRUE(l.dated);
  TEST_ASSERT_EQUAL_UINT16(kToday, l.today);
  TEST_ASSERT_EQUAL_UINT16(kSince, l.sinceDay);
  TEST_ASSERT_EQUAL_UINT32(71, l.countingDays);
  TEST_ASSERT_TRUE(l.plays == 1204);
  TEST_ASSERT_TRUE(l.listenedMs == h.listenedMs);
  TEST_ASSERT_EQUAL_UINT32(kToday * 1440u + 21 * 60 + 40, l.nowMin);
  TEST_ASSERT_FALSE(l.madeUp);  // (the host's: an ask may be due)
  TEST_ASSERT_EQUAL(State::Ready, stateOf(l));
  // As the fixture dated() has it, field for field that a page reads.
  const Lifetime d = dated();
  TEST_ASSERT_EQUAL_UINT32(d.countingDays, l.countingDays);
  TEST_ASSERT_EQUAL_UINT32(d.nowMin, l.nowMin);
  TEST_ASSERT_TRUE(d.listenedMs == l.listenedMs);
  // nowMin is the minute a snapshot row's `when` gives a play started now
  // (write(): (timeOf() - 1) / 60), the minute's first ms and its last alike,
  // so "1 min ago" and the day's sections read the same clock.
  TEST_ASSERT_EQUAL_UINT32((sf::timeOf(now) - 1) / 60, l.nowMin);
  TEST_ASSERT_EQUAL_UINT32(l.nowMin, lifetimeOf(true, false, true, h, kSince, minute).nowMin);
  TEST_ASSERT_EQUAL_UINT32(l.nowMin + 1, lifetimeOf(true, false, true, h, kSince, now + 1).nowMin);
  // A first day after today (a clock set back since): one counting day.
  TEST_ASSERT_EQUAL_UINT32(1, lifetimeOf(true, false, true, h, kToday + 3, now).countingDays);
  TEST_ASSERT_EQUAL_UINT32(1, lifetimeOf(true, false, true, h, kToday, now).countingDays);
  // No first day yet (nothing dated folded, no save): none known; the row
  // opens from today (opensDay()).
  l = lifetimeOf(true, false, true, h, sf::kUnsetDay, now);
  TEST_ASSERT_TRUE(l.dated);
  TEST_ASSERT_EQUAL_UINT32(0, l.countingDays);
  TEST_ASSERT_EQUAL_UINT16(kToday + 29, opensDay(l));
  // The clock's first ms (2026-01-01T00:00Z): day 0, minute 0.
  l = lifetimeOf(true, false, true, h, 0, sf::kMinMs);
  TEST_ASSERT_TRUE(l.dated);
  TEST_ASSERT_EQUAL_UINT16(0, l.today);
  TEST_ASSERT_EQUAL_UINT32(0, l.nowMin);
  TEST_ASSERT_EQUAL_UINT32(1, l.countingDays);
  // The clock unset (boardclock::now()'s 0), or not believed (before 2026,
  // past 2099): undated, no today, no counting days, no minute; the first
  // day kept (the entry line's "since 1 Sep" needs no clock).
  for (uint64_t t : {uint64_t{0}, sf::kMinMs - 1, wallclock::kMaxMs + 1}) {
    l = lifetimeOf(true, false, true, h, kSince, t);
    TEST_ASSERT_FALSE(l.dated);
    TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, l.today);
    TEST_ASSERT_EQUAL_UINT32(0, l.countingDays);
    TEST_ASSERT_EQUAL_UINT32(0, l.nowMin);
    TEST_ASSERT_EQUAL_UINT16(kSince, l.sinceDay);
    TEST_ASSERT_EQUAL(State::NoDates, stateOf(l));
  }
  // The last believed ms is dated.
  TEST_ASSERT_TRUE(lifetimeOf(true, false, true, h, kSince, wallclock::kMaxMs).dated);
  // The flags as the host gives them, into 4.4's states: a remount's
  // re-read waiting is on (StatsGlue::on() || rereadPending()); the boot's
  // fold before Empty (HEAD partial); the card out; the stats off.
  h.plays = 0;
  TEST_ASSERT_EQUAL(State::Folding, stateOf(lifetimeOf(true, true, true, h, kSince, now)));
  TEST_ASSERT_EQUAL(State::Empty, stateOf(lifetimeOf(true, false, true, h, kSince, now)));
  TEST_ASSERT_EQUAL(State::NoCard, stateOf(lifetimeOf(true, true, false, h, kSince, now)));
  TEST_ASSERT_EQUAL(State::Off, stateOf(lifetimeOf(false, true, false, h, kSince, now)));
  // HEAD's figures whole, past 32 bits.
  h.plays = uint64_t{0x100000000} + 5;
  h.listenedMs = uint64_t{0x123456789AB};
  l = lifetimeOf(true, false, true, h, kSince, now);
  TEST_ASSERT_TRUE(l.plays == uint64_t{0x100000005});
  TEST_ASSERT_TRUE(l.listenedMs == uint64_t{0x123456789AB});
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

// The session's first visit made with the card out (S9's review): the rise
// asks nothing (D3), StatsLists records nothing (no ask, no snapshot), and
// its phase stays Idle. Read through the rise's mark (phaseSeen()), the card
// back offers Try again with "The card is back", nothing due, on every page.
void test_the_card_back_after_a_rise_with_the_card_out() {
  Lifetime out = dated();
  out.cardIn = false;
  const Lifetime back = dated();
  // The mark: the card out at the rise; not for the stats off (Off first),
  // the card in, nothing played, or a made-up view.
  TEST_ASSERT_TRUE(risesCardOut(out));
  TEST_ASSERT_FALSE(asksOnEdge(out));
  TEST_ASSERT_FALSE(risesCardOut(back));
  Lifetime offOut = out;
  offOut.on = false;
  TEST_ASSERT_FALSE(risesCardOut(offOut));
  Lifetime emptyIn = dated();
  emptyIn.plays = 0;
  TEST_ASSERT_FALSE(risesCardOut(emptyIn));
  Lifetime madeUpOut = out;
  madeUpOut.madeUp = true;
  TEST_ASSERT_FALSE(risesCardOut(madeUpOut));

  const PhaseInfo idle = phase(Phase::Idle);
  const PhaseInfo seen = phaseSeen(idle, true);
  TEST_ASSERT_EQUAL(Phase::NoCard, seen.phase);
  TEST_ASSERT_EQUAL(Outcome::CardAway, seen.outcome);
  TEST_ASSERT_EQUAL_UINT16(idle.version, seen.version);
  // Without the mark, Idle as ever (D9 as replaced: a later rise with the
  // card in has asked; a Cancelled ask's one due).
  TEST_ASSERT_EQUAL(Phase::Idle, phaseSeen(idle, false).phase);
  HubLook h = hubLook(back, idle, nullptr);
  TEST_ASSERT_TRUE(h.page.askDue);  // (what the page did before: an ask by itself)

  // The card still out: the No card line alone, no ask.
  h = hubLook(out, seen, nullptr);
  TEST_ASSERT_EQUAL_STRING("no card", lookName(h.page.look));
  TEST_ASSERT_EQUAL_UINT32(0, h.rows);
  TEST_ASSERT_FALSE(h.page.askDue);
  // Back: Try again with stats.card_back, nothing due; the shape moved, so
  // the page restages with no news.
  const HubLook was = h;
  h = hubLook(back, seen, nullptr);
  TEST_ASSERT_EQUAL_STRING("retry", lookName(h.page.look));
  TEST_ASSERT_EQUAL_UINT32(7, h.rows);
  TEST_ASSERT_EQUAL(RowKind::TryAgain, h.item[6].kind);
  lineIs(Line::Text, S::stats_card_back, h.item[6], "hub, the first visit's card back");
  TEST_ASSERT_FALSE(h.page.askDue);
  TEST_ASSERT_NOT_EQUAL(hubShape(was), hubShape(h));
  for (uint32_t s = 0; s < kShelves; ++s) {
    const ShelfLook k = shelfLook(shelfId(static_cast<Shelf>(s)), back, seen, nullptr);
    TEST_ASSERT_EQUAL_STRING("retry", lookName(k.page.look));
    TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(k.page.line));
    TEST_ASSERT_FALSE(k.page.askDue);
  }
  const WeekLook w = weekLook(back, seen, nullptr);
  TEST_ASSERT_TRUE(w.button);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(w.page.line));
  TEST_ASSERT_FALSE(w.page.askDue);
  // Nothing played on the card back: the empty state, as ever.
  Lifetime emptyBack = dated();
  emptyBack.plays = 0;
  TEST_ASSERT_EQUAL_STRING("no plays", lookName(hubLook(emptyBack, seen, nullptr).page.look));

  // A phase the host has moved is the host's: an ask under way, a console
  // ask's outcome, a landing.
  for (const PhaseInfo& p : {phase(Phase::Working), phase(Phase::Waiting), phase(Phase::NoRoom, Outcome::NoRoom),
                             phase(Phase::Unread, Outcome::Io), kReady,
                             phase(Phase::NoCard, Outcome::CardAway, sv::kWhyCard)}) {
    const PhaseInfo q = phaseSeen(p, true);
    TEST_ASSERT_EQUAL(p.phase, q.phase);
    TEST_ASSERT_EQUAL(p.outcome, q.outcome);
    TEST_ASSERT_EQUAL_UINT8(p.why, q.why);
  }
  Snap full;
  put(full, fullCard());
  TEST_ASSERT_EQUAL_STRING("served", lookName(hubLook(back, phaseSeen(kReady, true), full.s()).page.look));
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
  // An undated snapshot has these two sections alone (the owner's call,
  // 2026-11-24, "Yes, fix the order"): a `when` that is neither code (none
  // a write() makes) reads as Earlier, never a day.
  putWhen(b, 3, 300u * 1440 + 90);
  putWhen(b, 4, 299u * 1440 + 1300);
  sameRecent(*b.s(), wantU, 7, "undated, a stray dated when");
  // Past the end: no row.
  TEST_ASSERT_FALSE(recentAt(*b.s(), 7).heading);
  TEST_ASSERT_EQUAL_UINT32(b->rows(sr::kRecent), recentAt(*b.s(), 7).row);

  // The whole way: a rank with no clock (300 dated tracks of earlier
  // rounds; this session's undated plays: two tracks never dated and one
  // dated long ago played again, its lastAt kept; one never dated of the
  // round before), copied out and resolved. This session's three first,
  // then Earlier, the only two headings, no time. Before the call the 216
  // dated rows came first: This session was empty and the track played
  // again sat under its old day.
  const uint32_t live = 12;
  std::vector<uint64_t> km(sr::Ranker::kBytes / 8 + 1, 0);
  sr::Ranker k;
  k.begin(km.data(), false, 0, live);
  auto track = [&k](uint64_t h, uint32_t at, uint32_t ord) {
    sf::Track t;
    t.hash = h;
    t.plays = 1;
    t.firstAt = t.lastAt = at;
    t.lastOrd = ord;
    k.track(t);
  };
  // (Each hash's whole row once: the oldest dated one, 0x1000, as the
  // merge leaves it played again with no clock, lastAt kept.)
  for (uint32_t i = 0; i < 300; ++i) track(0x1000 + i, (200 + i) * 86400u + 1, i == 0 ? live : 1 + i % 10);
  track(0x2001, 0, live);
  track(0x2002, 0, live);
  track(0x2003, 0, live - 1);
  k.finish();
  std::vector<uint64_t> wm(statsmap::Wanted::bytesFor(sr::Ranker::kEntries) / 8 + 1, 0);
  statsmap::Wanted w;
  TEST_ASSERT_TRUE(w.region(reinterpret_cast<uint8_t*>(wm.data()), sr::Ranker::kEntries));
  for (int l = 0; l < sr::kLists; ++l)
    for (uint32_t i = 0; i < k.size(static_cast<sr::List>(l)); ++i) w.add(k.at(static_cast<sr::List>(l), i).hash);
  w.seal();
  for (uint32_t i = 0; i < w.n; ++i) w.setId(i, 100 + i);
  sv::Context ctx;
  ctx.buildStamp = kStamp;
  ctx.trackCount = kTracks;
  ctx.albums = kAlbums;
  ctx.liveRound = live;
  ctx.sinceDay = kSince;
  Snap c;
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, c.s(), sv::kBound));
  TEST_ASSERT_TRUE(sv::resolve(w, c.s()));
  TEST_ASSERT_FALSE(c->dated());
  TEST_ASSERT_EQUAL_UINT32(sr::kCaps[sr::kRecent], c->rows(sr::kRecent));
  TEST_ASSERT_EQUAL_UINT32(sr::kCaps[sr::kRecent] + 2, recentCount(*c.s()));
  uint32_t headings = 0;
  for (uint32_t i = 0; i < recentCount(*c.s()); ++i) {
    const RecentItem it = recentAt(*c.s(), i);
    TEST_ASSERT_FALSE(it.timed);
    TEST_ASSERT_TRUE(it.section == Section::Session || it.section == Section::Earlier);
    TEST_ASSERT_EQUAL(it.row < 3 ? static_cast<int>(Section::Session) : static_cast<int>(Section::Earlier),
                      static_cast<int>(it.section));
    headings += it.heading ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_UINT32(2, headings);
  TEST_ASSERT_TRUE(recentAt(*c.s(), 0).heading && recentAt(*c.s(), 4).heading);
  // This session's: the one played again (its dated lastAt), then by the
  // hash; then Earlier by round (the review of the session order): the
  // never-dated one of the round before first, though 300 rows are dated,
  // then round 10's newest dated one.
  TEST_ASSERT_TRUE(k.at(sr::kRecent, 0).hash == 0x1000 && k.at(sr::kRecent, 1).hash == 0x2001 &&
                   k.at(sr::kRecent, 2).hash == 0x2002 && k.at(sr::kRecent, 3).hash == 0x2003 &&
                   k.at(sr::kRecent, 4).hash == 0x1000 + 299);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenEarlier, c->rowAt(sr::kRecent, 3).when);
  // The hub's row 6: This session's first row, no time.
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenSession, c->rowAt(sr::kRecent, 0).when);
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

// uiSx<n> from a host with no play log (UiHost::statsMadeUp()'s default and
// app/PlayHistory's with none read the one rule): nothing served, so the
// views with a snapshot (1, 3, 4) aren't written; the end and the views
// with none are the pages' alone; past 9 no view at all.
void test_the_made_up_views_with_no_play_log() {
  // 0 the end; 1, 3 and 4 not written; 2 and 5-9 none to write.
  const bool want[kMadeUpViews] = {true, false, true, false, false, true, true, true, true, true};
  for (uint8_t n = 0; n < kMadeUpViews; ++n) {
    char msg[24];
    snprintf(msg, sizeof(msg), "view %u", static_cast<unsigned>(n));
    TEST_ASSERT_EQUAL_MESSAGE(want[n], madeUpNoLog(n), msg);
    // ... the views' own word for it: true exactly where none is written.
    if (n == 0) continue;
    MadeUpView v;
    TEST_ASSERT_TRUE_MESSAGE(madeUpView(n, 0, 0, 0, &v), msg);
    TEST_ASSERT_EQUAL_MESSAGE(!v.snapshot, madeUpNoLog(n), msg);
  }
  for (unsigned n = kMadeUpViews; n <= 0xFF; ++n) TEST_ASSERT_FALSE(madeUpNoLog(static_cast<uint8_t>(n)));
}

namespace {

void putReal(Snap& b, bool dated);  // (a real ask's snapshot: below, S11's review)

void sameLife(const Lifetime& a, const Lifetime& b, const char* msg) {
  TEST_ASSERT_EQUAL_MESSAGE(a.on, b.on, msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.folding, b.folding, msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.cardIn, b.cardIn, msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.dated, b.dated, msg);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(a.today, b.today, msg);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(a.sinceDay, b.sinceDay, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.countingDays, b.countingDays, msg);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(a.plays, b.plays, msg);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(a.listenedMs, b.listenedMs, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.nowMin, b.nowMin, msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.madeUp, b.madeUp, msg);
}

}  // namespace

// S3b's S12: the made-up views as the Ui takes them (uiSx<n>, look only).
// - Its accessors' lifetime and phase (madeUpState(): no recipe) are the
//   view's, for every n; the views that write a snapshot (1, 3, 4: the
//   Dance tab's block, so uiSx says so on that tab), the console's names
//   and argument.
// - The rise with a view on writes it again, never asks, whatever the
//   lifetime says (the view's own: madeUp; a host's off, card out, empty);
//   with none, D3's asks.
// - The snapshot the pages read: the view's own under a view, a real one
//   with none; a real ask's write over the view's block clears the mark (a
//   real ask replaces it), so it is no view's.
// - Every act refused under a view (Play, Play next, + Queue, Auto DJ from
//   here, Shuffle all, Try again), the tray's window never; none with no
//   view. The Try again rows views 8 and 9 draw are refused by the same
//   rule, and no look has an ask due under any view.
void test_the_made_up_views_as_the_ui_takes_them() {
  // The accessors' lifetime and phase: the view's, without its recipe.
  for (unsigned u = 0; u <= 0xFF; ++u) {
    const uint8_t n = static_cast<uint8_t>(u);
    char msg[24];
    snprintf(msg, sizeof(msg), "view %u", u);
    MadeUpView v;
    const bool there = madeUpView(n, kStamp, kTracks, kAlbums, &v);
    Lifetime l;
    PhaseInfo p;
    TEST_ASSERT_EQUAL_MESSAGE(there, madeUpState(n, &l, &p), msg);
    TEST_ASSERT_EQUAL_MESSAGE(n >= 1 && n <= 9, there, msg);
    TEST_ASSERT_EQUAL_MESSAGE(n == 1 || n == 3 || n == 4, madeUpWrites(n), msg);
    if (!there) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(n == 0 ? "the card's own" : "?", madeUpName(n), msg);
      continue;
    }
    sameLife(v.life, l, msg);
    TEST_ASSERT_TRUE_MESSAGE(l.madeUp, msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(v.phase.phase), static_cast<int>(p.phase), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(v.phase.outcome), static_cast<int>(p.outcome), msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(v.phase.why, p.why, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(v.phase.version, p.version, msg);
    TEST_ASSERT_EQUAL_MESSAGE(v.snapshot, madeUpWrites(n), msg);
    // Either out alone.
    Lifetime l2;
    PhaseInfo p2;
    TEST_ASSERT_TRUE_MESSAGE(madeUpState(n, &l2, nullptr), msg);
    sameLife(l, l2, msg);
    TEST_ASSERT_TRUE_MESSAGE(madeUpState(n, nullptr, &p2), msg);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(p.phase), static_cast<int>(p2.phase), msg);
  }
  // Out untouched for no view.
  Lifetime untouched = dated();
  TEST_ASSERT_FALSE(madeUpState(0, &untouched, nullptr));
  sameLife(dated(), untouched, "view 0 leaves the out alone");
  const char* const names[kMadeUpViews] = {"the card's own", "a full card", "nothing played", "no clock",
                                           "day 5 of counting", "folding", "no card", "waiting", "no room",
                                           "unread"};
  for (uint8_t n = 0; n < kMadeUpViews; ++n) TEST_ASSERT_EQUAL_STRING(names[n], madeUpName(n));

  // uiSx<n>'s argument: "x" and one digit.
  uint8_t n = 0xEE;
  for (uint8_t d = 0; d <= 9; ++d) {
    char a[4] = {'x', static_cast<char>('0' + d), 0, 0};
    TEST_ASSERT_TRUE(madeUpArg(a, &n));
    TEST_ASSERT_EQUAL_UINT8(d, n);
  }
  n = 0xEE;
  for (const char* bad : {"x", "x10", "xa", "x-1", "x1 ", "X1", "", "1", "x/", "x:"}) {
    TEST_ASSERT_FALSE_MESSAGE(madeUpArg(bad, &n), bad);
  }
  TEST_ASSERT_FALSE(madeUpArg(nullptr, &n));
  TEST_ASSERT_EQUAL_UINT8(0xEE, n);  // (nothing written on a refusal)
  TEST_ASSERT_TRUE(madeUpArg("x7", nullptr));

  // The rise: a view on writes it again, never asks; none, D3's asks.
  Lifetime off = dated();
  off.on = false;
  Lifetime out = dated();
  out.cardIn = false;
  Lifetime empty = dated();
  empty.plays = 0;
  empty.listenedMs = 0;
  for (uint8_t v = 1; v < kMadeUpViews; ++v) {
    Lifetime own;
    TEST_ASSERT_TRUE(madeUpState(v, &own, nullptr));
    for (const Lifetime* l : {&own, &off, &out, &empty}) {
      TEST_ASSERT_EQUAL(Rise::MadeUp, riseOf(*l, v));
    }
    TEST_ASSERT_EQUAL(Rise::MadeUp, riseOf(dated(), v));
    // ... and its own lifetime never asks by itself (D16).
    TEST_ASSERT_FALSE(asksOnEdge(own));
  }
  TEST_ASSERT_EQUAL(Rise::Ask, riseOf(dated(), 0));
  TEST_ASSERT_EQUAL(Rise::Ask, riseOf(noClock(), 0));
  TEST_ASSERT_EQUAL(Rise::Ask, riseOf(dated(5), 0));
  TEST_ASSERT_EQUAL(Rise::None, riseOf(off, 0));
  TEST_ASSERT_EQUAL(Rise::None, riseOf(out, 0));
  TEST_ASSERT_EQUAL(Rise::None, riseOf(empty, 0));
  // (No such view: as none.)
  TEST_ASSERT_EQUAL(Rise::Ask, riseOf(dated(), kMadeUpViews));
  TEST_ASSERT_EQUAL(Rise::None, riseOf(off, 0xFF));

  // The snapshot the pages read.
  Snap made;
  put(made, fullCard());
  TEST_ASSERT_TRUE(made->madeUp());
  TEST_ASSERT_TRUE(viewServes(*made.s(), true));
  TEST_ASSERT_FALSE(viewServes(*made.s(), false));  // (never for the card's)
  Snap real;
  putReal(real, true);
  TEST_ASSERT_FALSE(real->madeUp());
  TEST_ASSERT_TRUE(viewServes(*real.s(), false));
  TEST_ASSERT_FALSE(viewServes(*real.s(), true));  // (never under a view's lifetime)
  // A real ask over the view's block (a console Hv or Hu under a view: the
  // held snapshot rewritten whole) clears the mark: a real one from then.
  Snap over;
  put(over, fullCard());
  TEST_ASSERT_TRUE(over->madeUp());
  putReal(over, true);
  TEST_ASSERT_TRUE(over->resolved());
  TEST_ASSERT_FALSE(over->madeUp());
  TEST_ASSERT_TRUE(viewServes(*over.s(), false));
  TEST_ASSERT_FALSE(viewServes(*over.s(), true));
  // ... and the view's again over a real one (the next rise's write).
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  put(over, v3.spec);
  TEST_ASSERT_TRUE(over->madeUp() && !over->dated());
  TEST_ASSERT_TRUE(viewServes(*over.s(), true));

  // The acts: every one refused under a view, the tray's window never;
  // none refused with no view.
  for (const Act a : {Act::Queue, Act::AutoDj, Act::ShuffleAll, Act::Ask}) {
    TEST_ASSERT_TRUE(actRefused(a, true));
    TEST_ASSERT_FALSE(actRefused(a, false));
  }
  TEST_ASSERT_FALSE(actRefused(Act::Window, true));
  TEST_ASSERT_FALSE(actRefused(Act::Window, false));
  // The Try again rows views 8 and 9 draw (look only): their taps the Ask
  // refused, and no look under any view has an ask due, written or not.
  for (uint8_t v = 1; v < kMadeUpViews; ++v) {
    Lifetime l;
    PhaseInfo p;
    TEST_ASSERT_TRUE(madeUpState(v, &l, &p));
    const statsvisit::Snapshot* snap = nullptr;
    Snap b;
    if (madeUpWrites(v)) {
      MadeUpView mv;
      TEST_ASSERT_TRUE(madeUpView(v, kStamp, kTracks, kAlbums, &mv));
      put(b, mv.spec);
      snap = b.s();
    }
    for (const statsvisit::Snapshot* s : {snap, static_cast<const statsvisit::Snapshot*>(nullptr)}) {
      const HubLook h = hubLook(l, p, s);
      TEST_ASSERT_FALSE(h.page.askDue);
      TEST_ASSERT_FALSE(weekLook(l, p, s).page.askDue);
      for (uint32_t k = 0; k < kShelves; ++k) {
        TEST_ASSERT_FALSE(shelfLook(shelfId(static_cast<Shelf>(k)), l, p, s).page.askDue);
      }
      if (h.rows == kHubRows + 1) {
        TEST_ASSERT_EQUAL(RowKind::TryAgain, h.item[kHubRows].kind);
        TEST_ASSERT_TRUE(actRefused(Act::Ask, l.madeUp));
      }
    }
  }
  MadeUpView v9;
  TEST_ASSERT_TRUE(madeUpView(9, kStamp, kTracks, kAlbums, &v9));
  TEST_ASSERT_EQUAL_UINT32(kHubRows + 1, hubLook(v9.life, v9.phase, nullptr).rows);
}

// 7.2's "An open bar across a new ask" where a made-up view's snapshot is
// written before the page's enter() (the rise's MadeUp, uiSx<n> with the
// held condition up: S12's review). ListView::attach() puts the ref's bar
// back whenever its row is under the rows there, as arrival() does within a
// visit; so the fold first makes the attach land as a new ask's landing
// does: the place kept, no bar.
void test_a_view_s_write_folds_the_shelves_bars() {
  // ui::PageKind's values: Library 2, Album 4.
  const NavModel::PageRef root{2, 0, 40, 1}, album{4, 9, 80, 2}, hub{kKindHub, 0, 0, -1};
  const NavModel::PageRef most{kKindShelf, shelfId(Shelf::MostPlayed), 120, 3};
  const NavModel::PageRef notPlayed{kKindShelf, shelfId(Shelf::NotPlayed), 64, 5};
  const NavModel::PageRef otherTab{kKindShelf, shelfId(Shelf::Recent), 10, 4};
  NavModel nav;
  nav.setRoot(NavModel::Tab::Library, root);
  nav.setRoot(NavModel::Tab::Queue, otherTab);  // (a shelf's kind on another tab: not the Library's)
  nav.select(NavModel::Tab::Library);
  nav.push(hub);
  nav.push(most);
  // The harm: the ref as attach() reads it, its bar under another
  // snapshot's rows, comes back on row 3.
  constexpr uint32_t kRows = 40, kLeads = 1;
  constexpr int32_t kH = 176;
  TEST_ASSERT_EQUAL_INT32(3, listlayout::arrival(nav.top().scrollPx, nav.top().expanded, kRows, kLeads, true, kH)
                                 .expanded);
  TEST_ASSERT_EQUAL(1, foldShelfBars(nav));
  const NavModel::PageRef& t = nav.top();
  TEST_ASSERT_EQUAL_INT32(-1, t.expanded);
  TEST_ASSERT_EQUAL_INT32(120, t.scrollPx);  // (the place kept)
  TEST_ASSERT_EQUAL_UINT32(most.id, t.id);
  // The folded ref read as attach() reads it lands as a new ask's landing
  // from the ref as it was: the same place, no bar, over any rows.
  for (const uint32_t rows : {0u, 2u, 4u, 40u, 400u}) {
    const listlayout::Place landing = listlayout::arrival(120, 3, rows, kLeads, false, kH);
    const listlayout::Place attached = listlayout::arrival(t.scrollPx, t.expanded, rows, kLeads, true, kH);
    TEST_ASSERT_EQUAL_INT32(landing.offset, attached.offset);
    TEST_ASSERT_EQUAL_INT32(-1, attached.expanded);
    TEST_ASSERT_EQUAL_INT32(landing.expanded, attached.expanded);
  }
  // Again: nothing open, nothing folded.
  TEST_ASSERT_EQUAL(0, foldShelfBars(nav));
  // The root's and the hub's refs, and another tab's, are their own.
  TEST_ASSERT_EQUAL_INT32(1, nav.at(NavModel::Tab::Library, 0).expanded);
  TEST_ASSERT_EQUAL_INT32(40, nav.at(NavModel::Tab::Library, 0).scrollPx);
  TEST_ASSERT_EQUAL_INT32(4, nav.top(NavModel::Tab::Queue).expanded);
  // Every shelf in the stack, under another page too (an album pushed from
  // Not played yet: its own bar kept, the card's album's).
  nav.popToRoot(NavModel::Tab::Library);
  nav.push(hub);
  nav.push(notPlayed);
  nav.push(album);
  TEST_ASSERT_EQUAL(1, foldShelfBars(nav));
  TEST_ASSERT_EQUAL_INT32(-1, nav.at(NavModel::Tab::Library, 2).expanded);
  TEST_ASSERT_EQUAL_INT32(64, nav.at(NavModel::Tab::Library, 2).scrollPx);
  TEST_ASSERT_EQUAL_INT32(2, nav.top().expanded);
  TEST_ASSERT_EQUAL_INT32(80, nav.top().scrollPx);
  // Two shelves, both folded (no page pushes a shelf over a shelf, but the
  // rule reads the whole stack); a scroll below 0 (kShowPlaying, -2: Now
  // Playing's jump) left as it is, for enter() to resolve.
  nav.popToRoot(NavModel::Tab::Library);
  nav.push(hub);
  NavModel::PageRef showPlaying = most;
  showPlaying.scrollPx = -2;
  nav.push(showPlaying);
  nav.push(notPlayed);
  TEST_ASSERT_EQUAL(2, foldShelfBars(nav));
  TEST_ASSERT_EQUAL_INT32(-1, nav.at(NavModel::Tab::Library, 2).expanded);
  TEST_ASSERT_EQUAL_INT32(-2, nav.at(NavModel::Tab::Library, 2).scrollPx);
  TEST_ASSERT_EQUAL_INT32(-1, nav.top().expanded);
  // On another tab the Library's stack is folded all the same (the rise's
  // write comes as the tab returns; uiSx set elsewhere writes then).
  NavModel away;
  away.setRoot(NavModel::Tab::Library, root);
  away.select(NavModel::Tab::Library);
  away.push(hub);
  away.push(most);
  away.select(NavModel::Tab::Queue);
  TEST_ASSERT_EQUAL(1, foldShelfBars(away));
  TEST_ASSERT_EQUAL_INT32(-1, away.top(NavModel::Tab::Library).expanded);
}

// ---------------------------------------------------------------------------
// The words (StatsText)
// ---------------------------------------------------------------------------
namespace {

#define DOT " \xC2\xB7 "         // librarytext::kDot
#define ELLIPSIS "\xE2\x80\xA6"  // TextFit's cut

// A made-up font: every character (a code point) *ctx px wide.
int cpWidth(void* ctx, const char* s) {
  int n = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) n += (*p & 0xC0) != 0x80;
  return n * *static_cast<int*>(ctx);
}
textfit::Font mono(int* px) {
  textfit::Font f;
  f.ctx = px;
  f.width = cpWidth;
  return f;
}
bool endsWith(const char* s, const char* tail) {
  const size_t n = std::strlen(s), t = std::strlen(tail);
  return n >= t && std::strcmp(s + n - t, tail) == 0;
}
constexpr uint64_t hm(uint64_t h, uint64_t m) { return (h * 60 + m) * 60000; }

}  // namespace

void test_the_forms_of_the_lines() {
  namespace st = statstext;
  char b[160];
  int px = 6;
  const textfit::Font f = mono(&px);
  // The hub's header line: lifetime, the time alone with no first day.
  st::hubSub(hm(74, 10), kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min since 1 Sep", b);
  st::hubSub(hm(0, 30), kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("30 min since 1 Sep", b);
  st::hubSub(hm(74, 10), sf::kUnsetDay, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min", b);
  // Last 7 days: the time and the plays (one; grouped).
  st::weekLine(hm(7, 37), 118, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("7 h 37 min" DOT "118 plays", b);
  st::weekLine(hm(0, 45), 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("45 min" DOT "1 play", b);
  st::weekLine(hm(168, 59), 9999, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("168 h 59 min" DOT "9,999 plays", b);
  // Not played yet: the albums since the first day, the albums alone with
  // none; before its count, the day alone ("" with none).
  st::notPlayedLine(612, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("612 albums since 1 Sep", b);
  st::notPlayedLine(1, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 album since 1 Sep", b);
  st::notPlayedLine(612, sf::kUnsetDay, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("612 albums", b);
  st::sinceOnlyLine(kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("since 1 Sep", b);
  TEST_ASSERT_EQUAL_UINT32(0, st::sinceOnlyLine(sf::kUnsetDay, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
  // Old favourites: the long form where it fits 232 (35 characters: 210 px
  // at 6, 280 at 8), else the count alone; one track; a count grouped.
  st::oldFavouritesLine(f, st::kHubLineW, 38, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("38 tracks, played often, not lately", b);
  st::oldFavouritesLine(f, st::kHubLineW, 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 track, played often, not lately", b);
  px = 8;
  st::oldFavouritesLine(f, st::kHubLineW, 38, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("38 tracks", b);
  px = 6;
  st::oldFavouritesLine(f, 210, 38, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("38 tracks, played often, not lately", b);
  st::oldFavouritesLine(f, 209, 38, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("38 tracks", b);
  st::oldFavouritesLine(f, 400, 1204, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1,204 tracks, played often, not lately", b);
  st::oldFavouritesLine(f, st::kHubLineW, 1204, b, sizeof(b));  // 38 characters: 228 px
  TEST_ASSERT_EQUAL_STRING("1,204 tracks, played often, not lately", b);
  st::oldFavouritesLine(f, st::kHubLineW, 12040, b, sizeof(b));  // 39: 234
  TEST_ASSERT_EQUAL_STRING("12,040 tracks", b);
  // Their counts: the snapshot's total (4.2 as decided: every row offered,
  // less the orphans dropped), past the shelf's cap too (50, 200): the
  // hub says how many there are, the shelf shows its cap.
  {
    sv::MadeUp m = fullCard();
    Snap a;
    put(a, m);
    TEST_ASSERT_EQUAL_UINT32(a->total(sr::kNewFinds), st::hubCount(*a.s(), sr::kNewFinds));
    TEST_ASSERT_EQUAL_UINT32(a->total(sr::kOldFavourites), st::hubCount(*a.s(), sr::kOldFavourites));
    TEST_ASSERT_TRUE(a->total(sr::kNewFinds) > 0 && a->total(sr::kOldFavourites) > 0);
    m.totals[sr::kNewFinds] = 50;
    m.totals[sr::kOldFavourites] = 200;
    Snap c;
    put(c, m);
    TEST_ASSERT_EQUAL_UINT32(50, st::hubCount(*c.s(), sr::kNewFinds));
    TEST_ASSERT_EQUAL_UINT32(200, st::hubCount(*c.s(), sr::kOldFavourites));
    m.totals[sr::kNewFinds] = 80;
    m.totals[sr::kOldFavourites] = 1000;
    Snap d;
    put(d, m);
    TEST_ASSERT_EQUAL_UINT32(80, st::hubCount(*d.s(), sr::kNewFinds));
    TEST_ASSERT_EQUAL_UINT32(1000, st::hubCount(*d.s(), sr::kOldFavourites));
    TEST_ASSERT_EQUAL_UINT32(80, d->total(sr::kNewFinds));
    TEST_ASSERT_EQUAL_UINT32(1000, d->total(sr::kOldFavourites));
    TEST_ASSERT_EQUAL_UINT32(0, st::hubCount(*d.s(), sr::kLists));
  }
  // New finds: the long form where it fits 232 (32 characters: 192 px at 6,
  // 256 at 8), else the short one; one track; a count grouped.
  st::newFindsLine(f, st::kHubLineW, 41, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("41 tracks first heard in 30 days", b);
  px = 8;
  st::newFindsLine(f, st::kHubLineW, 41, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("41 new in 30 days", b);
  px = 6;
  st::newFindsLine(f, st::kHubLineW, 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 track first heard in 30 days", b);
  st::newFindsLine(f, st::kHubLineW, 1204, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1,204 tracks first heard in 30 days", b);
  px = 232 / 32;  // 7: 224 px, fits; one more character doesn't
  st::newFindsLine(f, 224, 41, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("41 tracks first heard in 30 days", b);
  st::newFindsLine(f, 223, 41, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("41 new in 30 days", b);
  st::newFindsLine(f, 223, 1204, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1,204 new in 30 days", b);
  // A first-month row: "Opens 6 Dec, after a month of plays" (35
  // characters) in 250, else its short form; the toasts.
  px = 7;  // 245 px
  st::warmingLine(f, st::kReasonW, 339, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Opens 6 Dec, after a month of plays", b);
  px = 8;  // 280 px
  st::warmingLine(f, st::kReasonW, 339, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Opens on 6 Dec", b);
  st::toastText(S::toast_stats_needs_month, 339, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Needs a month of plays: Opens on 6 Dec", b);
  st::toastText(S::toast_stats_needs_dates, 339, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Needs dates: Plug in to the MP3 Player tab", b);
  // The empty state's rule (37 characters) in 304, else its short form.
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_empty_rule), static_cast<int>(st::emptyRule(f, 304)));  // 296
  px = 9;  // 333
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_empty_rule_short), static_cast<int>(st::emptyRule(f, 304)));
  px = 6;

  // Most played: the top three, " · " between, cut at the end with "…".
  const char* titles[] = {"Night Ticket", "Glass Harbour", "Gulls Over Ninth", "Fourth"};
  st::topThreeLine(f, 400, titles, 3, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Night Ticket" DOT "Glass Harbour" DOT "Gulls Over Ninth", b);
  st::topThreeLine(f, 400, titles, 4, b, sizeof(b));  // three at most
  TEST_ASSERT_EQUAL_STRING("Night Ticket" DOT "Glass Harbour" DOT "Gulls Over Ninth", b);
  st::topThreeLine(f, st::kHubLineW, titles, 3, b, sizeof(b));  // 47 characters: 282 px
  TEST_ASSERT_TRUE(cpWidth(&px, b) <= st::kHubLineW);  // 38 characters at most: "Gulls " cut, its space dropped
  TEST_ASSERT_EQUAL_STRING("Night Ticket" DOT "Glass Harbour" DOT "Gulls" ELLIPSIS, b);
  st::topThreeLine(f, st::kHubLineW, titles, 2, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Night Ticket" DOT "Glass Harbour", b);
  st::topThreeLine(f, st::kHubLineW, titles, 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Night Ticket", b);
  TEST_ASSERT_EQUAL_UINT32(0, st::topThreeLine(f, st::kHubLineW, titles, 0, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
  const char* odd[] = {nullptr, "Glass Harbour"};
  st::topThreeLine(f, st::kHubLineW, odd, 2, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING(DOT "Glass Harbour", b);
  // Long titles past the join's 255 bytes: still cut to the room.
  std::string longTitle(200, 'x');
  const char* longs[] = {longTitle.c_str(), longTitle.c_str(), longTitle.c_str()};
  st::topThreeLine(f, st::kHubLineW, longs, 3, b, sizeof(b));
  TEST_ASSERT_TRUE(cpWidth(&px, b) <= st::kHubLineW && endsWith(b, ELLIPSIS));

  // Recently played: the title and how long ago; the title cut, the time
  // kept; the title alone from 24 h, or undated.
  const uint32_t now = kToday * 1440u + 600;
  st::recentLine(f, st::kHubLineW, "Slow Lantern", true, now, now - 25, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Slow Lantern" DOT "25 min ago", b);
  st::recentLine(f, st::kHubLineW, "Slow Lantern", true, now, now - 1440, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Slow Lantern", b);
  st::recentLine(f, st::kHubLineW, "Slow Lantern", false, now, now - 25, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Slow Lantern", b);
  st::recentLine(f, st::kHubLineW, "A Very Long Title That Goes On And On For Ever", true, now, now - 25, b,
                 sizeof(b));
  TEST_ASSERT_TRUE(cpWidth(&px, b) <= st::kHubLineW);
  TEST_ASSERT_TRUE(endsWith(b, ELLIPSIS DOT "25 min ago"));
  TEST_ASSERT_EQUAL_INT(0, std::strncmp(b, "A Very Long Title", 17));

  // The shelves' header lines and the tray's labels.
  st::shelfSub(Shelf::MostPlayed, sv::k7Days, 30, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("last 7 days", b);
  st::shelfSub(Shelf::MostPlayed, sv::k30Days, 30, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("last 30 days", b);
  st::shelfSub(Shelf::MostPlayed, sv::kAllTime, 30, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("since 1 Sep", b);
  TEST_ASSERT_EQUAL_UINT32(0, st::shelfSub(Shelf::MostPlayed, sv::kAllTime, 30, sf::kUnsetDay, b, sizeof(b)));
  // RCNT capped (the tray's 30-day cell "16 days"): the header at the 30
  // days names what the cell names (S3b's review, finding 8); the other
  // windows and shelves don't read it.
  st::shelfSub(Shelf::MostPlayed, sv::k30Days, 16, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("16 days", b);
  st::shelfSub(Shelf::MostPlayed, sv::k30Days, 1, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 day", b);
  st::shelfSub(Shelf::MostPlayed, sv::k30Days, 29, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("29 days", b);
  st::shelfSub(Shelf::MostPlayed, sv::k7Days, 16, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("last 7 days", b);
  st::shelfSub(Shelf::MostPlayed, sv::kAllTime, 16, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("since 1 Sep", b);
  st::shelfSub(Shelf::NewFinds, sv::kAllTime, 16, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("first heard in 30 days", b);
  st::shelfSub(Shelf::NewFinds, sv::kAllTime, 30, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("first heard in 30 days", b);
  st::shelfSub(Shelf::OldFavourites, sv::kAllTime, 30, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("not in 30 days", b);
  st::shelfSub(Shelf::NotPlayed, sv::kAllTime, 30, kSince, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("since 1 Sep", b);
  TEST_ASSERT_EQUAL_UINT32(0, st::shelfSub(Shelf::Recent, sv::kAllTime, 30, kSince, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
  st::trayLabel(sv::k7Days, 30, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("7 days", b);
  st::trayLabel(sv::k30Days, 30, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("30 days", b);
  st::trayLabel(sv::k30Days, 16, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("16 days", b);
  st::trayLabel(sv::k30Days, 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 day", b);
  st::trayLabel(sv::kAllTime, 30, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("All time", b);

  // A short buffer cuts at a character boundary, as uistr does.
  char small[12];
  st::weekLine(hm(7, 37), 118, small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("7 h 37 min ", small);
  char tiny[13];
  st::weekLine(hm(7, 37), 118, tiny, sizeof(tiny));  // the dot's two bytes don't fit: dropped whole
  TEST_ASSERT_EQUAL_STRING("7 h 37 min ", tiny);
  // Most played's line in a buffer shorter than its cut: cut there too,
  // still ending with the ellipsis.
  char line[20];
  st::topThreeLine(f, 400, titles, 3, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("Night Ticket \xC2\xB7" ELLIPSIS, line);  // 16 bytes kept, the space before the mark dropped
}

void test_a_ranked_row_keeps_its_count() {
  namespace st = statstext;
  char b[160];
  int px = 6;
  const textfit::Font f = mono(&px);
  sv::Row r;
  r.count = 12;
  // Most played, each window: the plays.
  for (sr::List l : {sr::kMost7, sr::kMost30, sr::kMostAll}) {
    st::rowLine(f, st::kRowLineW, l, r, "Lumen Ferry", false, 0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("Lumen Ferry" DOT "12 plays", b);
  }
  r.count = 1;
  st::rowLine(f, st::kRowLineW, sr::kMostAll, r, "Lumen Ferry", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry" DOT "1 play", b);
  r.count = 9999;
  st::rowLine(f, st::kRowLineW, sr::kMostAll, r, "Lumen Ferry", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry" DOT "9,999 plays", b);
  // New finds: who found it (nothing when the listener picked it), then
  // the plays.
  r.count = 9;
  r.src = sf::kAutoDj;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "Sefa Ilmar", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sefa Ilmar" DOT "found by Auto DJ" DOT "9 plays", b);
  r.src = sf::kShuffle;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "Sefa Ilmar", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sefa Ilmar" DOT "found by shuffle" DOT "9 plays", b);
  r.src = sf::kYou;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "Sefa Ilmar", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sefa Ilmar" DOT "9 plays", b);
  // Old favourites: the plays and the last day (14 Sep).
  r.lastDay = 256;
  st::rowLine(f, st::kRowLineW, sr::kOldFavourites, r, "Halvard Moss", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Halvard Moss" DOT "9 plays" DOT "last 14 Sep", b);
  // Recently played: how long ago when timed and under a day, else the
  // artist alone (no count: the list has none).
  const uint32_t now = kToday * 1440u + 600;
  r.when = now - 25;
  st::rowLine(f, st::kRowLineW, sr::kRecent, r, "Lumen Ferry", true, now, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry" DOT "25 min ago", b);
  st::rowLine(f, st::kRowLineW, sr::kRecent, r, "Lumen Ferry", false, now, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry", b);
  r.when = now - 24 * 60;
  st::rowLine(f, st::kRowLineW, sr::kRecent, r, "Lumen Ferry", true, now, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry", b);
  r.when = sv::kWhenSession;
  st::rowLine(f, st::kRowLineW, sr::kRecent, r, "Lumen Ferry", true, now, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry", b);

  // A long artist: cut to what the parts leave, the parts whole at the end.
  const char* longArtist = "The Lantern Choir and the Longest Name of All the Artists";
  r.count = 9999;
  r.src = sf::kAutoDj;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, longArtist, false, 0, b, sizeof(b));
  // (The parts and the dot: 33 characters, 198 px; the artist's 66: "The
  // Lanter…".)
  TEST_ASSERT_TRUE(cpWidth(&px, b) <= st::kRowLineW);
  TEST_ASSERT_EQUAL_STRING("The Lanter" ELLIPSIS DOT "found by Auto DJ" DOT "9,999 plays", b);
  // The artist cut alone (Recently played with no time).
  st::rowLine(f, st::kRowLineW, sr::kRecent, r, longArtist, false, now, b, sizeof(b));
  TEST_ASSERT_TRUE(cpWidth(&px, b) <= st::kRowLineW && endsWith(b, ELLIPSIS));
  // Who found it, and the last day, give way to the count when the pair
  // would leave the artist under kMinNameW: at 8 px "found by Auto DJ ·
  // 9,999 plays" and the dot are 264 px (none left), the plays alone 112;
  // at 9 px "9,999 plays · last 14 Sep" and the dot 252 (12 left). With no
  // artist, the pair whole while it fits the line, else the count alone
  // (the parts are never cut, so a pair wider than the line would lose the
  // count to the draw's cut): "found by Auto DJ · 9,999 plays" is 30
  // characters, 240 px at 8, 270 at 9; "9,999 plays · last 14 Sep" 25, 225
  // at 9, 275 at 11.
  px = 8;
  r.src = sf::kAutoDj;
  r.count = 9;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "Sefa Ilmar", false, 0, b, sizeof(b));  // 32 px left: kept
  TEST_ASSERT_EQUAL_STRING("Sef" ELLIPSIS DOT "found by Auto DJ" DOT "9 plays", b);
  r.count = 9999;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "Sefa Ilmar", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sefa Ilmar" DOT "9,999 plays", b);
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("found by Auto DJ" DOT "9,999 plays", b);
  st::rowLine(f, 240, sr::kNewFinds, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("found by Auto DJ" DOT "9,999 plays", b);
  st::rowLine(f, 239, sr::kNewFinds, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("9,999 plays", b);
  st::rowLine(f, 239, sr::kNewFinds, r, nullptr, false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("9,999 plays", b);
  px = 9;
  st::rowLine(f, st::kRowLineW, sr::kNewFinds, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("9,999 plays", b);
  r.count = 9;
  st::rowLine(f, st::kRowLineW, sr::kOldFavourites, r, "Halvard Moss", false, 0, b, sizeof(b));
  TEST_ASSERT_TRUE(endsWith(b, DOT "9 plays" DOT "last 14 Sep"));
  r.count = 9999;
  st::rowLine(f, st::kRowLineW, sr::kOldFavourites, r, "Halvard Moss", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Halvard Moss" DOT "9,999 plays", b);
  st::rowLine(f, st::kRowLineW, sr::kOldFavourites, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("9,999 plays" DOT "last 14 Sep", b);
  px = 11;
  st::rowLine(f, st::kRowLineW, sr::kOldFavourites, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("9,999 plays", b);
  px = 6;
  // No artist (the files right under /music): the parts alone.
  r.count = 12;
  st::rowLine(f, st::kRowLineW, sr::kMostAll, r, "", false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("12 plays", b);
  st::rowLine(f, st::kRowLineW, sr::kMostAll, r, nullptr, false, 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("12 plays", b);

  // nameAndParts()'s edge: the name keeps kMinNameW px or is left out.
  // " · " and "12 plays": 11 characters, 66 px; at 98 px 32 are left (5
  // characters, "Lume…"), at 97 none.
  st::nameAndParts(f, 66 + st::kMinNameW, "Lumen Ferry", "12 plays", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lume" ELLIPSIS DOT "12 plays", b);
  st::nameAndParts(f, 66 + st::kMinNameW - 1, "Lumen Ferry", "12 plays", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("12 plays", b);
  // A whole name that fits: as it is.
  st::nameAndParts(f, 66 + 66, "Lumen Ferry", "12 plays", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Lumen Ferry" DOT "12 plays", b);
  // A buffer short of the parts' bytes and five: the parts alone.
  char shortBuf[16];
  st::nameAndParts(f, 400, "Lumen Ferry", "12 plays", shortBuf, sizeof(shortBuf));
  TEST_ASSERT_EQUAL_STRING("12 plays", shortBuf);
  // A buffer that cuts the name (its pixels fit): the name cut there with
  // "…", the parts whole after it.
  char mid[24];
  st::nameAndParts(f, 400, longArtist, "12 plays", mid, sizeof(mid));
  TEST_ASSERT_EQUAL_STRING("The Lant" ELLIPSIS DOT "12 plays", mid);
}

void test_relative_times() {
  namespace st = statstext;
  char b[48];
  const uint32_t now = kToday * 1440u + 600;
  // By the minute: 0 s and 59 s ago are the same minute, "1 min ago" (a
  // minute at least: D10).
  TEST_ASSERT_TRUE(st::agoText(now, now, b, sizeof(b)) > 0);
  TEST_ASSERT_EQUAL_STRING("1 min ago", b);
  st::agoText(now, now - 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 min ago", b);
  st::agoText(now, now - 59, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("59 min ago", b);
  st::agoText(now, now - 60, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("1 h ago", b);
  st::agoText(now, now - (23 * 60 + 59), b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("23 h ago", b);
  // From 24 h: none (the name alone), as for an undated row or a time
  // after now (a clock set back).
  for (uint32_t when : {now - 24 * 60, now - 24 * 60 - 1, now - 5000, sv::kWhenSession, sv::kWhenEarlier, now + 1}) {
    std::strcpy(b, "x");
    TEST_ASSERT_EQUAL_UINT32(0, st::agoText(now, when, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("", b);
  }
  // A made-up snapshot's row 0 (25 min before its time) against its now.
  Snap a;
  put(a, fullCard());
  MadeUpView v;
  TEST_ASSERT_TRUE(madeUpView(1, kStamp, kTracks, kAlbums, &v));
  st::agoText(v.life.nowMin, a->rowAt(sr::kRecent, 0).when, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("25 min ago", b);
  st::agoText(v.life.nowMin, a->rowAt(sr::kRecent, 1).when, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("2 h ago", b);  // 25 + 97 = 122 min
}

void test_day_labels() {
  namespace st = statstext;
  char b[64];
  // Day 0, 2026-01-01, was a Thursday; 313 (10 Nov) a Tuesday.
  TEST_ASSERT_EQUAL_UINT8(3, st::weekdayOf(0));
  TEST_ASSERT_EQUAL_UINT8(1, st::weekdayOf(kToday));
  TEST_ASSERT_EQUAL_UINT8(6, st::weekdayOf(311));
  TEST_ASSERT_EQUAL(static_cast<int>(S::day_tue), static_cast<int>(st::dayShort(kToday)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::day_saturday), static_cast<int>(st::dayLong(310)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::day_monday), static_cast<int>(st::dayLong(312)));
  TEST_ASSERT_EQUAL(static_cast<int>(S::day_fri), static_cast<int>(st::dayShort(365)));  // 2027-01-01, a Friday
  // A section's older day.
  st::weekdayDate(311, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sun 8 Nov", b);
  st::weekdayDate(0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Thu 1 Jan", b);
  // Last 7 days' range: one month, across a month's end, across a year's.
  st::dayRange(307, 313, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("4\xE2\x80\x93" "10 Nov", b);
  st::dayRange(270, 276, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("28 Sep\xE2\x80\x93" "4 Oct", b);
  st::dayRange(361, 367, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("28 Dec\xE2\x80\x93" "3 Jan", b);
  st::dayRange(334, 340, b, sizeof(b));  // 1-7 Dec
  TEST_ASSERT_EQUAL_STRING("1\xE2\x80\x93" "7 Dec", b);
  st::weekSub(307, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("4\xE2\x80\x93" "10 Nov (UTC)", b);
  st::weekSub(270, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("28 Sep\xE2\x80\x93" "4 Oct (UTC)", b);
  // A minute's day with a time zone's offset (0 in S3): 23:30 UTC on 10
  // Nov is 11 Nov an hour east; 00:30 UTC is 9 Nov an hour west; 31 Oct
  // 23:30 is 1 Nov an hour east (a month's end); before day 0, day 0.
  TEST_ASSERT_EQUAL_UINT16(kToday, st::localDay(kToday * 1440u + 23 * 60 + 30, 0));
  TEST_ASSERT_EQUAL_UINT16(kToday + 1, st::localDay(kToday * 1440u + 23 * 60 + 30, 60));
  TEST_ASSERT_EQUAL_UINT16(kToday - 1, st::localDay(kToday * 1440u + 30, -60));
  TEST_ASSERT_EQUAL_UINT16(kToday, st::localDay(kToday * 1440u + 30, 0));
  const uint16_t nov1 = st::localDay(303u * 1440 + 23 * 60 + 30, 60);
  TEST_ASSERT_EQUAL_UINT16(304, nov1);
  st::weekdayDate(nov1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sun 1 Nov", b);
  st::weekdayDate(st::localDay(303u * 1440 + 23 * 60 + 30, 0), b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sat 31 Oct", b);
  TEST_ASSERT_EQUAL_UINT16(0, st::localDay(30, -60));
  TEST_ASSERT_EQUAL_UINT16(0, st::localDay(0, 0));
  // Recently played's headings.
  RecentItem it;
  it.heading = true;
  it.section = Section::Today;
  st::sectionText(it, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Today", b);
  it.section = Section::Yesterday;
  st::sectionText(it, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Yesterday", b);
  it.section = Section::Day;
  it.day = 311;
  st::sectionText(it, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Sun 8 Nov", b);
  it.section = Section::Session;
  st::sectionText(it, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("This session", b);
  it.section = Section::Earlier;
  st::sectionText(it, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Earlier", b);
  // A dated snapshot's sections, worded.
  sv::MadeUp m = fullCard();
  m.time = (kToday * 1440u + 600) * 60 + 1;
  m.rows[sr::kRecent] = 4;
  m.recentFirstAgoMin = 30;
  m.recentStepMin = 720;
  m.sessionRows = 0;
  m.earlierRows = 1;
  Snap a;
  put(a, m);
  const char* want[] = {"Today", nullptr, "Yesterday", nullptr, nullptr, "Sun 8 Nov", nullptr, "Earlier", nullptr};
  TEST_ASSERT_EQUAL_UINT32(9, recentCount(*a.s()));
  for (uint32_t i = 0; i < 9; ++i) {
    const RecentItem r = recentAt(*a.s(), i);
    TEST_ASSERT_EQUAL_MESSAGE(want[i] != nullptr, r.heading, want[i] ? want[i] : "a row");
    if (!r.heading) continue;
    st::sectionText(r, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(want[i], b);
  }
}

void test_the_week_texts() {
  namespace st = statstext;
  char b[96];
  // The big day: its weekday's full name and its time; under an hour, the
  // minutes.
  st::bigDayLine(310, hm(2, 1), b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Saturday was your big day: 2 h 1 min", b);
  st::bigDayLine(312, hm(0, 50), b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Monday was your big day: 50 min", b);
  // Auto DJ's rate.
  st::autoDjLine(87, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Auto DJ got it right 87% of the time", b);
  st::autoDjLine(100, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Auto DJ got it right 100% of the time", b);
  // From a made-up snapshot: the frames' week (4-10 Nov, Saturday 2 h 1
  // min, 87%).
  Snap a;
  put(a, fullCard());
  const WeekFigures w = weekFigures(*a.s());
  st::weekSub(w.firstDay, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("4\xE2\x80\x93" "10 Nov (UTC)", b);
  st::bigDayLine(static_cast<uint16_t>(w.firstDay + w.bigDay), a->day(static_cast<uint32_t>(w.bigDay)).listenedMs, b,
                 sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Saturday was your big day: 2 h 1 min", b);
  st::autoDjLine(w.autoDjPct, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Auto DJ got it right 87% of the time", b);
  st::weekLine(w.listenedMs, w.plays, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("7 h 37 min" DOT "118 plays", b);
  // The columns' names, oldest first: Wed ... Tue.
  const char* names[7] = {"Wed", "Thu", "Fri", "Sat", "Sun", "Mon", "Tue"};
  for (uint32_t i = 0; i < 7; ++i)
    TEST_ASSERT_EQUAL_STRING(names[i], uistr::tr(st::dayShort(static_cast<uint16_t>(w.firstDay + i))));
  // Its header line: beside a 104 px title (from x 140) when it fits to
  // 207, else under it.
  TEST_ASSERT_TRUE(st::weekSubPlace(104, 67, 207) == uitext::HeaderSub::Beside);
  TEST_ASSERT_TRUE(st::weekSubPlace(104, 68, 207) == uitext::HeaderSub::Below);
  TEST_ASSERT_EQUAL_INT(140, 28 + 104 + uitext::kHeaderSubGap);
  // A's two columns, 8 px apart at least.
  TEST_ASSERT_TRUE(st::fitsBeside(100, 60));
  TEST_ASSERT_TRUE(st::fitsBeside(304 - 16 - 8 - 60, 60));
  TEST_ASSERT_FALSE(st::fitsBeside(304 - 16 - 8 - 60 + 1, 60));
}

// Last 7 days as ui's LibraryPage draws it (S3b's S11): each look through
// the body hook or as the empty state (Try again's button with Retry, the
// card back among it, never with the card out), the band covered whole by
// one or the other; its header's days (the snapshot's, while its figures
// show) and when the header is drawn again; its texts from a made-up
// snapshot (the count grouped, the big day, Auto DJ's rate, none for a week
// with nothing or an undated one); its columns' geometry; and the status
// band off on it alone.
void test_week_as_the_page_draws_it() {
  namespace st = statstext;
  // The body or the empty state, every look one of the two.
  const Look looks[] = {Look::Served, Look::Pending, Look::Retry,  Look::NoCard,
                        Look::Off,    Look::NoPlays, Look::Closed};
  int bodies = 0, lines = 0;
  for (const Look k : looks) {
    bodies += weekBody(k);
    lines += weekBodyLine(k);
    if (weekBodyLine(k)) TEST_ASSERT_TRUE(weekBody(k));  // (a body's line: never the empty state's)
  }
  TEST_ASSERT_EQUAL_INT(5, bodies);
  TEST_ASSERT_EQUAL_INT(2, lines);
  TEST_ASSERT_TRUE(weekBody(Look::Served));
  TEST_ASSERT_TRUE(weekBody(Look::Pending));
  TEST_ASSERT_TRUE(weekBody(Look::Off));
  TEST_ASSERT_TRUE(weekBody(Look::NoPlays));
  TEST_ASSERT_FALSE(weekBody(Look::Retry));
  TEST_ASSERT_FALSE(weekBody(Look::Closed));
  // The card out (S3b's review, finding 7; the owner's call 3: only the No
  // card line): the list's lone Body line in A, as Pending's and as the hub
  // and the shelves draw it; not the empty state's disc and Small line.
  TEST_ASSERT_TRUE(weekBody(Look::NoCard));
  TEST_ASSERT_TRUE(weekBodyLine(Look::NoCard));
  TEST_ASSERT_TRUE(weekBodyLine(Look::Pending));
  for (const Look k : {Look::Served, Look::Off, Look::NoPlays, Look::Retry, Look::Closed})
    TEST_ASSERT_FALSE(weekBodyLine(k));

  // Each phase's look on Week, and its empty state's line and button.
  Snap a;
  put(a, fullCard());
  struct Case {
    Lifetime l;
    PhaseInfo p;
    bool served;
    Look look;
    S line;
    bool button;
  };
  Lifetime out = dated();
  out.cardIn = false;
  Lifetime off = dated();
  off.on = false;
  Lifetime none = dated();
  none.plays = 0;
  const Case cases[] = {
      {dated(), kReady, true, Look::Served, S::stats_counting, false},
      {dated(), phase(Phase::Working), false, Look::Pending, S::stats_counting, false},
      {dated(), phase(Phase::Waiting), false, Look::Pending, S::stats_waiting, false},
      {dated(), phase(Phase::NoRoom, Outcome::NoRoom), false, Look::Retry, S::stats_no_room, true},
      {dated(), phase(Phase::Unread, Outcome::Io), false, Look::Retry, S::stats_unread, true},
      {dated(), phase(Phase::Capped), false, Look::Retry, S::stats_waiting, true},
      // The card back (the owner's call 3): Try again, "The card is back".
      {dated(), phase(Phase::NoCard, Outcome::CardAway), false, Look::Retry, S::stats_card_back, true},
      // The card out: its line alone, no button, nothing due.
      {out, phase(Phase::NoCard, Outcome::CardAway), false, Look::NoCard, S::stats_no_card, false},
      {noClock(), kReady, true, Look::Closed, S::stats_needs_dates, false},
      {off, phase(Phase::Idle), false, Look::Off, S::stats_counting, false},
      {none, phase(Phase::Idle), false, Look::NoPlays, S::stats_counting, false},
  };
  char msg[96];
  for (const Case& c : cases) {
    const WeekLook w = weekLook(c.l, c.p, c.served ? a.s() : nullptr);
    snprintf(msg, sizeof(msg), "%s from %s", lookName(c.look), sv::phaseName(c.p.phase));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(lookName(c.look), lookName(w.page.look), msg);
    TEST_ASSERT_EQUAL_MESSAGE(c.button, w.button, msg);
    if (!weekBody(w.page.look) || weekBodyLine(w.page.look))
      TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.line), static_cast<int>(w.page.line), msg);
    // The card out: its line alone in the body, the empty state's never
    // (no disc, no button: the hub's and the shelves' one line).
    if (c.look == Look::NoCard) {
      TEST_ASSERT_TRUE_MESSAGE(weekBody(w.page.look) && weekBodyLine(w.page.look), msg);
      TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(S::stats_no_card), static_cast<int>(w.page.line), msg);
    }
    // The button only on the empty state.
    if (w.button) TEST_ASSERT_FALSE_MESSAGE(weekBody(w.page.look), msg);
    // Its header's days: the snapshot's, with the figures alone.
    const uint16_t day = weekHeaderDay(w.page.look, c.served ? a.s() : nullptr);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(w.page.look == Look::Served && c.l.dated ? kToday - 6 : sf::kUnsetDay, day, msg);
  }
  // Idle with the stats on (D9 as replaced): the Working line, one ask due.
  const WeekLook idle = weekLook(dated(), phase(Phase::Idle, Outcome::Cancelled), nullptr);
  TEST_ASSERT_EQUAL_STRING("pending", lookName(idle.page.look));
  TEST_ASSERT_TRUE(idle.page.askDue);
  TEST_ASSERT_FALSE(idle.button);

  // The first day: the snapshot's today and the six before (weekFigures()'s);
  // an undated snapshot has none, nor its header. (No page draws one: with
  // no clock Week is Closed, and the clock set since its ask releases it
  // first, undatedStale(): test_an_undated_snapshot_once_the_clock_is_set.)
  TEST_ASSERT_EQUAL_UINT16(kToday - 6, weekFirstDay(*a.s()));
  TEST_ASSERT_EQUAL_UINT16(weekFigures(*a.s()).firstDay, weekFirstDay(*a.s()));
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  Snap u;
  put(u, v3.spec);
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, weekFirstDay(*u.s()));
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, weekHeaderDay(Look::Served, u.s()));
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, weekHeaderDay(Look::Served, nullptr));
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, weekHeaderDay(Look::Pending, a.s()));

  // The header drawn again: the figures coming (a landing) or going (a
  // release, the card pulled), a new ask's snapshot; not a phase's line to
  // another, nor a reason to another, nor the same snapshot.
  PageLook served;
  served.look = Look::Served;
  PageLook working;
  working.look = Look::Pending;
  working.line = S::stats_counting;
  PageLook waiting = working;
  waiting.line = S::stats_waiting;
  PageLook noRoom;
  noRoom.look = Look::Retry;
  noRoom.line = S::stats_no_room;
  PageLook noCard;
  noCard.look = Look::NoCard;
  noCard.line = S::stats_no_card;
  Snap b;
  sv::MadeUp m = fullCard();
  put(b, m);
  TEST_ASSERT_TRUE(weekHeaderMoved(lookText(working, nullptr), lookText(served, a.s())));
  TEST_ASSERT_TRUE(weekHeaderMoved(lookText(served, a.s()), lookText(noCard, nullptr)));
  TEST_ASSERT_TRUE(weekHeaderMoved(lookText(served, a.s()), lookText(noRoom, nullptr)));
  TEST_ASSERT_FALSE(weekHeaderMoved(lookText(served, a.s()), lookText(served, a.s())));
  TEST_ASSERT_FALSE(weekHeaderMoved(lookText(working, nullptr), lookText(waiting, nullptr)));
  TEST_ASSERT_FALSE(weekHeaderMoved(lookText(noRoom, nullptr), lookText(noCard, nullptr)));
  TEST_ASSERT_FALSE(weekHeaderMoved(lookText(noCard, nullptr), lookText(working, nullptr)));
  // (A new ask's snapshot is a new version: StatsLists' write.)
  const uint16_t was = lookText(served, a.s());
  TEST_ASSERT_TRUE(weekHeaderMoved(static_cast<uint16_t>(was ^ 8u), was));

  // The texts from the frames' week: "118", Saturday's 2 h 1 min, 87%.
  char t[160];
  const WeekFigures w = weekFigures(*a.s());
  st::weekPlays(w.plays, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("118", t);
  st::weekPlays(1204, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("1,204", t);
  st::weekPlays(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("0", t);
  st::weekBigDay(*a.s(), w, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Saturday was your big day: 2 h 1 min", t);
  st::weekAutoDj(w, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Auto DJ got it right 87% of the time", t);
  // A week with nothing: no big day, no Auto DJ line (a dated week).
  for (int i = 0; i < 7; ++i) m.days[i] = sr::Day();
  Snap e;
  put(e, m);
  const WeekFigures nothing = weekFigures(*e.s());
  TEST_ASSERT_EQUAL_UINT16(kToday - 6, nothing.firstDay);
  TEST_ASSERT_EQUAL_UINT32(0, st::weekBigDay(*e.s(), nothing, t, sizeof(t)));
  TEST_ASSERT_EQUAL_STRING("", t);
  TEST_ASSERT_EQUAL_UINT32(0, st::weekAutoDj(nothing, t, sizeof(t)));
  TEST_ASSERT_EQUAL_STRING("", t);
  // Its last day the big one: Tuesday, today's column.
  m.days[6].listenedMs = 45 * 60000u;
  m.days[6].plays = 9;
  Snap f;
  put(f, m);
  const WeekFigures last = weekFigures(*f.s());
  TEST_ASSERT_EQUAL_INT8(6, last.bigDay);
  st::weekBigDay(*f.s(), last, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Tuesday was your big day: 45 min", t);
  // An undated snapshot: a big day by time but no weekday to name.
  WeekFigures undated = weekFigures(*u.s());
  undated.bigDay = 3;
  TEST_ASSERT_EQUAL_UINT32(0, st::weekBigDay(*u.s(), undated, t, sizeof(t)));
  TEST_ASSERT_EQUAL_STRING("", t);

  // The columns: seven of 41 px from x 16 to 303, inside A's right edge
  // (x 304) as the rule is; each bar 24 px, 9 in (the frame's x 25, 66 ...
  // 271), its day's name centred on it.
  const int barX[7] = {25, 66, 107, 148, 189, 230, 271};
  for (int i = 0; i < 7; ++i) {
    TEST_ASSERT_EQUAL_INT(barX[i], st::weekBarX(i));
    TEST_ASSERT_EQUAL_INT(barX[i] + 12, st::weekDayX(i));
    TEST_ASSERT_TRUE(st::weekBarX(i) + st::kWeekBarW <= st::weekColumnX(i) + st::kWeekPitch);
  }
  TEST_ASSERT_EQUAL_INT(303, st::weekColumnX(6) + st::kWeekPitch);
  TEST_ASSERT_TRUE(st::weekColumnX(6) + st::kWeekPitch <= st::kWeekPlaysRight);
  TEST_ASSERT_EQUAL_INT(st::kWeekPlaysRight, st::kWeekValueX + st::kWeekLineW);
  // B's tallest bar inside B (y 122-189), standing on the baseline; the
  // rule in C (y 190-239).
  TEST_ASSERT_TRUE(st::kWeekBaseY - kWeekBarPx >= 122);
  TEST_ASSERT_TRUE(st::kWeekBaseY < 190 && st::kWeekRuleY >= 190);
  // The count's room: what the value and the gap leave, so the two fit
  // beside each other exactly when fitsBeside() says.
  for (const int valueW : {60, 120, 200}) {
    const int room = st::weekCountRoom(valueW);
    TEST_ASSERT_TRUE(st::fitsBeside(valueW, room));
    TEST_ASSERT_FALSE(st::fitsBeside(valueW, room + 1));
  }

  // The status band: on every Library list but Week.
  TEST_ASSERT_FALSE(statusLineOn(kKindWeek));
  TEST_ASSERT_TRUE(statusLineOn(kKindHub));
  TEST_ASSERT_TRUE(statusLineOn(kKindShelf));
  for (uint8_t k = 0; k < kKindHub; ++k) TEST_ASSERT_TRUE(statusLineOn(k));
}

namespace {

// A real ask's snapshot (write(), not writeMadeUp(): no made-up mark): two
// tracks played this session, none dated. Made with the clock unset (dated
// false) it is ranked undated, so its 7 days are all 0 and the lists that
// need the clock have no rows; else as an ask made today.
void putReal(Snap& b, bool dated) {
  std::vector<uint64_t> mem(sr::Ranker::kBytes / 8 + 1, 0);
  sr::Ranker k;
  k.begin(mem.data(), dated, kToday, 1);
  for (const uint64_t h : {uint64_t{0x51}, uint64_t{0x52}}) {
    sf::Track t;
    t.hash = h;
    t.plays = 3;
    t.firstAt = 0;  // (played, none dated)
    t.lastOrd = 1;
    k.track(t);
  }
  k.finish();
  std::vector<uint64_t> wm(statsmap::Wanted::bytesFor(2) / 8 + 1, 0);
  statsmap::Wanted w;
  TEST_ASSERT_TRUE(w.region(reinterpret_cast<uint8_t*>(wm.data()), 2));
  TEST_ASSERT_TRUE(w.add(0x51));
  TEST_ASSERT_TRUE(w.add(0x52));
  w.seal();
  w.setId(0, 10);
  w.setId(1, 11);
  sv::Context ctx;
  ctx.grade = sv::Grade::Lists;
  ctx.buildStamp = kStamp;
  ctx.trackCount = kTracks;
  ctx.albums = kAlbums;
  ctx.time = dated ? kToday * 86400u + 1 : 0;  // (a row's time: seconds since 2026, plus 1)
  ctx.liveRound = 1;
  ctx.sinceDay = kSince;
  ctx.version = 9;
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, b.s(), sv::kBound));
  TEST_ASSERT_TRUE(sv::resolve(w, b.s()));
}

}  // namespace

// S3b's review of S11: a snapshot asked with no clock, the clock set while
// the pages are up (the status row's "Plug in to the MP3 Player tab" done).
// Its week is 0 and its dated lists unoffered, so the Ui releases it
// (undatedStale()) and the page asks once, dated.
void test_an_undated_snapshot_once_the_clock_is_set() {
  Snap u;
  putReal(u, false);
  TEST_ASSERT_TRUE(u->resolved());
  TEST_ASSERT_FALSE(u->dated());
  TEST_ASSERT_FALSE(u->madeUp());
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    TEST_ASSERT_EQUAL_UINT32(0, u->day(i).listenedMs);
    TEST_ASSERT_EQUAL_UINT32(0, u->day(i).plays);
  }
  TEST_ASSERT_EQUAL_UINT32(2, u->rows(sr::kMostAll));
  for (sr::List l : {sr::kMost7, sr::kMost30, sr::kNewFinds, sr::kOldFavourites})
    TEST_ASSERT_EQUAL_UINT32(0, u->rows(l));

  // What the pages would draw over it with the clock set (why it goes):
  // Week served with no week (0 min, 0 plays, no bars, no day), the hub's
  // row 1 open on the snapshot's line, New finds open on no rows.
  const Lifetime now = dated();
  const WeekLook w = weekLook(now, kReady, u.s());
  TEST_ASSERT_EQUAL_STRING("served", lookName(w.page.look));
  const WeekFigures f = weekFigures(*u.s());
  TEST_ASSERT_EQUAL_UINT64(0, f.listenedMs);
  TEST_ASSERT_EQUAL_UINT32(0, f.plays);
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, f.firstDay);
  const HubLook h = hubLook(now, kReady, u.s());
  TEST_ASSERT_EQUAL(RowKind::Open, h.item[static_cast<int>(HubRow::Week)].kind);
  TEST_ASSERT_EQUAL(Line::Data, h.item[static_cast<int>(HubRow::Week)].line);
  TEST_ASSERT_EQUAL(Line::Data, h.item[static_cast<int>(HubRow::NewFinds)].line);

  // So it is stale: the clock set now, the snapshot undated, neither made up.
  TEST_ASSERT_TRUE(undatedStale(now, u.s()));
  TEST_ASSERT_TRUE(undatedStale(dated(5), u.s()));  // (the first month too: Week and row 1 open)
  // Not with no clock: the pages' own (Week Closed, row 1 the status row).
  TEST_ASSERT_FALSE(undatedStale(noClock(), u.s()));
  TEST_ASSERT_EQUAL_STRING("closed", lookName(weekLook(noClock(), kReady, u.s()).page.look));
  TEST_ASSERT_EQUAL(RowKind::Status, hubLook(noClock(), kReady, u.s()).item[0].kind);
  // Nor with nothing served, a dated snapshot (either clock: one way only,
  // the board's clock never unset within a boot), a made-up snapshot (view
  // 3, no clock on purpose) or under a made-up view's lifetime.
  TEST_ASSERT_FALSE(undatedStale(now, nullptr));
  Snap d;
  put(d, fullCard());
  TEST_ASSERT_TRUE(d->dated());
  TEST_ASSERT_FALSE(undatedStale(now, d.s()));
  TEST_ASSERT_FALSE(undatedStale(noClock(), d.s()));
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  Snap m;
  put(m, v3.spec);
  TEST_ASSERT_TRUE(m->madeUp() && !m->dated());
  TEST_ASSERT_FALSE(undatedStale(now, m.s()));
  Lifetime view = now;
  view.madeUp = true;
  TEST_ASSERT_FALSE(undatedStale(view, u.s()));

  // Released (StatsLists::release(): its landing's Ok kept, nothing
  // served): the host's phase Idle, so Week and the hub read the Working
  // line with one ask due (D9 as replaced), made with nothing served (D6).
  const Phase idle = sv::phaseOf(0, Outcome::Ok, ScanScheduler::Wait::None, 0, false, false);
  TEST_ASSERT_EQUAL_STRING("idle", sv::phaseName(idle));
  const PhaseInfo after = phase(idle, Outcome::Ok);
  const WeekLook wa = weekLook(now, after, nullptr);
  TEST_ASSERT_EQUAL_STRING("pending", lookName(wa.page.look));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_counting), static_cast<int>(wa.page.line));
  TEST_ASSERT_TRUE(wa.page.askDue);
  TEST_ASSERT_FALSE(wa.button);
  const HubLook ha = hubLook(now, after, nullptr);
  TEST_ASSERT_TRUE(ha.page.askDue);
  lineIs(Line::Text, S::stats_counting, ha.item[static_cast<int>(HubRow::Week)], "row 1 after the release");
  // ... and the page's shape moved, so it is drawn again (the news).
  TEST_ASSERT_NOT_EQUAL(lookText(w.page, u.s()), lookText(wa.page, nullptr));
  // The new ask's landing, dated (its turn's clock): not stale, so once.
  Snap r;
  putReal(r, true);
  TEST_ASSERT_TRUE(r->dated() && !r->madeUp());
  TEST_ASSERT_EQUAL_UINT16(kToday, r->today());
  TEST_ASSERT_FALSE(undatedStale(now, r.s()));
  TEST_ASSERT_EQUAL_STRING("served", lookName(weekLook(now, kReady, r.s()).page.look));
  TEST_ASSERT_EQUAL_UINT16(kToday - 6, weekFigures(*r.s()).firstDay);
}

void test_the_stat_value_s_segments() {
  namespace st = statstext;
  struct Want {
    const char* text;
    bool value;
  };
  auto same = [](const st::StatValue& v, const Want* want, int n, const char* what) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(n, v.count, what);
    for (int k = 0; k < n && k < v.count; ++k) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want[k].text, v.at(k), what);
      TEST_ASSERT_EQUAL_MESSAGE(want[k].value, v.seg[k].value, what);
    }
  };
  st::StatValue v;
  st::statValue(hm(7, 37), &v);
  const Want en[] = {{"7", true}, {" h ", false}, {"37", true}, {" min", false}};
  same(v, en, 4, "en 7 h 37 min");
  // Its width: the numbers in one font, the words in another.
  int big = 10, smallPx = 5;
  TEST_ASSERT_EQUAL_INT(10 + 15 + 20 + 20, st::statWidth(v, mono(&big), mono(&smallPx)));
  st::statValue(hm(0, 45), &v);
  const Want enMin[] = {{"45", true}, {" min", false}};
  same(v, enMin, 2, "en 45 min");
  st::statValue(hm(0, 0), &v);
  const Want enZero[] = {{"0", true}, {" min", false}};
  same(v, enZero, 2, "en 0 min");
  st::statValue(hm(1204, 5), &v);
  const Want enGrouped[] = {{"1,204", true}, {" h ", false}, {"5", true}, {" min", false}};
  same(v, enGrouped, 4, "en 1,204 h 5 min");
  st::statValue(hm(99999, 59), &v);
  TEST_ASSERT_EQUAL_STRING("99,999", v.at(0));
  // In the UI's language: Polish's words between the numbers, Japanese's
  // and Chinese's.
  uistr::setLang(uistr::Lang::Pl);
  st::statValue(hm(7, 37), &v);
  const Want pl[] = {{"7", true}, {" godz. ", false}, {"37", true}, {" min", false}};
  same(v, pl, 4, "pl");
  uistr::setLang(uistr::Lang::Ja);
  st::statValue(hm(7, 37), &v);
  const Want ja[] = {{"7", true}, {" \xE6\x99\x82\xE9\x96\x93 ", false}, {"37", true}, {" \xE5\x88\x86", false}};
  same(v, ja, 4, "ja");
  uistr::setLang(uistr::Lang::Zh);
  st::statValue(hm(0, 45), &v);
  const Want zh[] = {{"45", true}, {" \xE5\x88\x86\xE9\x92\x9F", false}};
  same(v, zh, 2, "zh");
  uistr::setLang(uistr::Lang::En);
  // The segments joined are timeText()'s.
  char whole[64], joined[64] = "";
  for (uint64_t ms : {hm(7, 37), hm(0, 45), hm(1204, 5), hm(168, 59)}) {
    st::statValue(ms, &v);
    joined[0] = 0;
    for (int k = 0; k < v.count; ++k) std::strcat(joined, v.at(k));
    timeText(ms, whole, sizeof(whole));
    TEST_ASSERT_EQUAL_STRING(whole, joined);
  }
}

// ---------------------------------------------------------------------------
// S3b's S9: the hub as the page draws and touches it, the entry row's fold
// ---------------------------------------------------------------------------
namespace {

// The host's titles: "T<id>" (the card's catalog on the device).
size_t tableTitle(uint16_t id, char* out, size_t size, void* ctx) {
  ++*static_cast<int*>(ctx);
  const int n = std::snprintf(out, size, "T%u", static_cast<unsigned>(id));
  return n < 0 ? 0 : static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

}  // namespace

// The hub's rows by kind: pressable, inert, held (ListView::Source's three
// hooks), and the titles each row draws.
void test_the_hub_rows_take_a_touch() {
  const Control open = controlOf(RowKind::Open), off = controlOf(RowKind::Disabled),
                status = controlOf(RowKind::Status), retry = controlOf(RowKind::TryAgain);
  // Open: a tick and its page; Try again: a tick and the ask. No holds.
  TEST_ASSERT_TRUE(open.pressable && !open.inert && !open.holds);
  TEST_ASSERT_TRUE(retry.pressable && !retry.inert && !retry.holds);
  // Disabled: inert (the buzz and the toast, no press look, no tick).
  TEST_ASSERT_TRUE(!off.pressable && off.inert && !off.holds);
  // The status row: nothing at all (D13: no press, no buzz).
  TEST_ASSERT_TRUE(!status.pressable && !status.inert && !status.holds);
  // As the looks give them: no clock (the status row, rows 3-4 disabled), a
  // Retry's row 7.
  const HubLook nc = hubLook(noClock(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
  TEST_ASSERT_EQUAL_UINT32(7, nc.rows);
  TEST_ASSERT_FALSE(controlOf(nc.item[0].kind).pressable || controlOf(nc.item[0].kind).inert);
  TEST_ASSERT_TRUE(controlOf(nc.item[2].kind).inert && controlOf(nc.item[3].kind).inert);
  TEST_ASSERT_TRUE(controlOf(nc.item[1].kind).pressable && controlOf(nc.item[5].kind).pressable);
  TEST_ASSERT_TRUE(controlOf(nc.item[6].kind).pressable);
  for (uint32_t i = 0; i < nc.rows; ++i) TEST_ASSERT_FALSE(controlOf(nc.item[i].kind).holds);

  // The titles: each row's name; the status row's "No dates yet: ...";
  // Try again's.
  const HubLook h = hubLook(dated(), phase(Phase::Unread, Outcome::Io), nullptr);
  const S names[] = {S::stats_last7, S::stats_most_played, S::stats_new_finds, S::stats_old_favourites,
                     S::stats_not_played, S::stats_recent, S::empty_try_again};
  TEST_ASSERT_EQUAL_UINT32(7, h.rows);
  for (uint32_t i = 0; i < h.rows; ++i)
    TEST_ASSERT_EQUAL(static_cast<int>(names[i]), static_cast<int>(hubTitle(h.item[i])));
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_dates), static_cast<int>(hubTitle(nc.item[0])));
  // A shelf's title is its hub row's.
  for (uint32_t s = 0; s < kShelves; ++s) {
    Shelf from;
    TEST_ASSERT_TRUE(shelfOfRow(static_cast<HubRow>(s + 1), &from));
    TEST_ASSERT_EQUAL(static_cast<int>(names[s + 1]), static_cast<int>(shelfTitle(from)));
  }
}

// A hub row's tap (statsTap(): S3b's review, finding 16): Try again asks,
// an open row pushes its page while the hub is reachable, else it is
// refused (the stats gone off on a shown hub: no blank shelf or Week); the
// disabled and status rows and past the rows nothing.
void test_a_hub_row_s_tap() {
  const HubLook nc = hubLook(noClock(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
  TEST_ASSERT_EQUAL_UINT32(7, nc.rows);
  // (pressable() keeps the disabled and status rows off; a disabled row's
  // buzz is onInertTap()'s.)
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(nc, 0, noClock(), true, false));  // the status row
  TEST_ASSERT_EQUAL(HubTap::Push, hubTap(nc, 1, noClock(), true, false));
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(nc, 2, noClock(), true, false));  // disabled
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(nc, 3, noClock(), true, false));
  TEST_ASSERT_EQUAL(HubTap::Ask, hubTap(nc, 6, noClock(), true, false));
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(nc, 7, noClock(), true, false));
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(nc, 99, noClock(), true, false));
  const HubLook working = hubLook(dated(), phase(Phase::Working), nullptr);
  for (uint32_t i = 0; i < working.rows; ++i)
    TEST_ASSERT_EQUAL(HubTap::Push, hubTap(working, i, dated(), true, false));
  // The stats gone off on a shown hub (a remount's re-read failed: S3b's
  // review, finding 16): its six rows still drawn open (D9 as replaced: no
  // data lines) and pressed with their tick, but a tap pushes nothing (4.4:
  // Off reaches no shelf and no Week): refused, the double buzz, as the
  // entry row's whose hub went off (S9). None asks.
  Lifetime stopped = dated();
  stopped.on = false;
  const HubLook gone = hubLook(stopped, phase(Phase::Idle, Outcome::None, sv::kWhyOff), nullptr);
  TEST_ASSERT_EQUAL(Look::Off, gone.page.look);
  TEST_ASSERT_EQUAL_UINT32(6, gone.rows);
  for (uint32_t i = 0; i < gone.rows; ++i) {
    TEST_ASSERT_EQUAL(RowKind::Open, gone.item[i].kind);
    TEST_ASSERT_TRUE(controlOf(gone.item[i].kind).pressable);
    TEST_ASSERT_EQUAL(HubTap::Refused, hubTap(gone, i, stopped, true, false));
  }
  // ... no clock as well (the status row, rows 3-4 disabled: still none).
  Lifetime offNoClock = noClock();
  offNoClock.on = false;
  const HubLook goneNc = hubLook(offNoClock, phase(Phase::Idle, Outcome::None, sv::kWhyOff), nullptr);
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(goneNc, 0, offNoClock, true, false));
  TEST_ASSERT_EQUAL(HubTap::Refused, hubTap(goneNc, 1, offNoClock, true, false));
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(goneNc, 2, offNoClock, true, false));
  // The same rule as uiSh's and the entry row's (hubReachable()): behind the
  // update's fence or over uil's library, refused too.
  TEST_ASSERT_EQUAL(HubTap::Refused, hubTap(working, 1, dated(), false, false));
  TEST_ASSERT_EQUAL(HubTap::Refused, hubTap(working, 1, dated(), true, true));
  // The card out: no rows, nothing to tap.
  Lifetime out = dated();
  out.cardIn = false;
  const HubLook pulled = hubLook(out, phase(Phase::NoCard, Outcome::CardAway, sv::kWhyCard), nullptr);
  TEST_ASSERT_EQUAL(HubTap::None, hubTap(pulled, 0, out, true, false));
}

// The header line, the hub's reach (uiSh's refusal), and the shape a page
// polls: it moves with the rows' kinds and count, never with a text alone.
void test_the_hub_s_sub_reach_and_shape() {
  // The sub: from RAM, for a whole HEAD with plays (not Off, Folding or
  // Empty), whatever the card.
  Lifetime l = dated();
  TEST_ASSERT_TRUE(hubSubDue(l));
  TEST_ASSERT_TRUE(hubSubDue(dated(5)));
  TEST_ASSERT_TRUE(hubSubDue(noClock()));
  l.cardIn = false;
  TEST_ASSERT_TRUE(hubSubDue(l));  // (No card: the lifetime is RAM's)
  l = dated();
  l.folding = true;
  TEST_ASSERT_FALSE(hubSubDue(l));  // A19
  l = dated();
  l.plays = 0;
  TEST_ASSERT_FALSE(hubSubDue(l));  // 7.5: the title alone
  l = dated();
  l.on = false;
  TEST_ASSERT_FALSE(hubSubDue(l));
  // ... and the card out hides neither (S3b's review, finding 6): an empty
  // card pulled on the empty hub keeps the title alone (not "0 min since
  // 23 Nov"), and a card held during the boot's fold no part-folded total.
  l = dated();
  l.cardIn = false;
  l.plays = 0;
  l.listenedMs = 0;
  TEST_ASSERT_EQUAL(State::NoCard, stateOf(l));
  TEST_ASSERT_FALSE(hubSubDue(l));
  l = dated();
  l.cardIn = false;
  l.folding = true;
  TEST_ASSERT_EQUAL(State::NoCard, stateOf(l));
  TEST_ASSERT_FALSE(hubSubDue(l));
  // (The sub's bit in the page's shape follows: pulled on the empty hub, the
  // look moves (the empty state to No card's line), the header doesn't.)
  Lifetime emptyIn = dated(), emptyOut = dated();
  emptyIn.plays = emptyOut.plays = 0;
  emptyOut.cardIn = false;
  TEST_ASSERT_EQUAL(hubSubDue(emptyIn), hubSubDue(emptyOut));
  l = dated();
  l.on = false;
  // Reachable: not Off, an index, not the synthetic library; the card out
  // and nothing played still show it.
  TEST_ASSERT_TRUE(hubReachable(dated(), true, false));
  TEST_ASSERT_FALSE(hubReachable(dated(), false, false));
  TEST_ASSERT_FALSE(hubReachable(dated(), true, true));
  TEST_ASSERT_FALSE(hubReachable(l, true, false));
  l = dated();
  l.cardIn = false;
  TEST_ASSERT_TRUE(hubReachable(l, true, false));
  l = dated();
  l.plays = 0;
  TEST_ASSERT_TRUE(hubReachable(l, true, false));
  // The latched entry row's tap opens what uiSh would: never with the stats
  // off, so A20's "Couldn't read the stats" row (entryLine()'s Unread is
  // exactly the stats off) opens nothing.
  char entry[96];
  int epx = 6;
  for (int k = 0; k < 7; ++k) {
    Lifetime e = k == 1 ? dated(5) : k == 2 ? noClock() : dated();
    if (k == 3) e.cardIn = false;
    if (k == 4) e.plays = 0;
    if (k == 5) e.on = false;
    if (k == 6) e.folding = true;
    const bool unread = entryLine(e, monoWidth, &epx, entry, sizeof(entry)) == EntryLine::Unread;
    TEST_ASSERT_EQUAL(!e.on, unread);
    if (unread) TEST_ASSERT_FALSE(hubReachable(e, true, false));
  }

  // The shape: one byte per structure.
  Snap full, lists, nc, ss;
  put(full, fullCard());
  sv::MadeUp lm = fullCard();
  lm.grade = sv::Grade::Lists;
  lm.notPlayedEvery = 0;
  put(lists, lm);
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  put(nc, v3.spec);
  sv::MadeUp shortSpan = v3.spec;
  shortSpan.firstDay = 290;
  shortSpan.newestDay = 318;
  put(ss, shortSpan);
  Lifetime out = dated();
  out.cardIn = false;
  Lifetime empty = dated();
  empty.plays = 0;
  Lifetime off = dated();
  off.on = false;
  const PhaseInfo away = phase(Phase::NoCard, Outcome::CardAway, sv::kWhyCard);
  const HubLook looks[] = {
      hubLook(dated(), kReady, full.s()),                               // served
      hubLook(dated(), kReady, lists.s()),                              // row 5 since-only
      hubLook(dated(), phase(Phase::Working), nullptr),                 // pending
      hubLook(dated(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr), // Try again
      hubLook(out, away, nullptr),                                      // the card out
      hubLook(dated(), away, nullptr),                                  // ... back: Try again, card_back
      hubLook(empty, phase(Phase::Idle), nullptr),                      // nothing played
      hubLook(off, phase(Phase::Idle), nullptr),                        // off
      hubLook(dated(5), kReady, full.s()),                              // the first month, served
      hubLook(dated(5), phase(Phase::Working), nullptr),                // ... pending
      hubLook(noClock(), phase(Phase::Working), nullptr),               // no clock, pending
      hubLook(noClock(), kReady, nc.s()),                               // ... DAYS 30 on: row 5 open
      hubLook(noClock(), kReady, ss.s()),                               // ... DAYS 29: row 5 disabled
  };
  const size_t n = sizeof(looks) / sizeof(looks[0]);
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) {
      // The card coming back moves it (No card's line to Try again), as a
      // Retry's other reasons don't (index 3 against 5 differ by look:
      // Retry against NoCard's; 3 and 5 are both Retry, the same shape).
      if (i == 3 && j == 5) {
        TEST_ASSERT_EQUAL_UINT8(hubShape(looks[i]), hubShape(looks[j]));
        continue;
      }
      char msg[32];
      std::snprintf(msg, sizeof(msg), "looks %u and %u", static_cast<unsigned>(i), static_cast<unsigned>(j));
      TEST_ASSERT_NOT_EQUAL_MESSAGE(hubShape(looks[i]), hubShape(looks[j]), msg);
    }
  TEST_ASSERT_NOT_EQUAL(hubShape(looks[4]), hubShape(looks[5]));  // the card back restages
  // Bit 7 is the page's (its header's line): no look sets it.
  for (size_t i = 0; i < n; ++i) TEST_ASSERT_EQUAL_UINT8(0, hubShape(looks[i]) & kShapeSub);
  // The empty state to No card's line (no rows either way: nothing staged)
  // draws the band whole; every other pair stages (or covers it: the empty
  // state's own draw).
  for (int a = 0; a <= static_cast<int>(Look::Closed); ++a)
    for (int b = 0; b <= static_cast<int>(Look::Closed); ++b)
      TEST_ASSERT_EQUAL(a == static_cast<int>(Look::NoPlays) && b == static_cast<int>(Look::NoCard),
                        hubDrawsWhole(static_cast<Look>(a), static_cast<Look>(b)));
  TEST_ASSERT_EQUAL_UINT32(0, looks[6].rows);  // (nothing played: no rows)
  TEST_ASSERT_EQUAL_UINT32(0, looks[4].rows);  // (the card out: no rows)
  // A text alone doesn't move it: Working against Waiting, one Retry
  // reason against another, a served snapshot against another's data.
  TEST_ASSERT_EQUAL_UINT8(hubShape(hubLook(dated(), phase(Phase::Working), nullptr)),
                          hubShape(hubLook(dated(), phase(Phase::Waiting), nullptr)));
  TEST_ASSERT_EQUAL_UINT8(hubShape(hubLook(dated(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr)),
                          hubShape(hubLook(dated(), phase(Phase::Unread, Outcome::Io), nullptr)));
  TEST_ASSERT_EQUAL_UINT8(hubShape(hubLook(dated(), kReady, full.s())),
                          hubShape(hubLook(dated(), kReady, nc.s())));
  // The day that ends the first month moves it (rows 3-5 open).
  TEST_ASSERT_NOT_EQUAL(hubShape(hubLook(dated(29), kReady, full.s())),
                        hubShape(hubLook(dated(30), kReady, full.s())));

  // The texts: the phase's line, a reason, the No card line, a served
  // snapshot's version; the same look and line, the same.
  auto text = [&](const Lifetime& lt, const PhaseInfo& p, const sv::Snapshot* snap) {
    return lookText(hubLook(lt, p, snap).page, snap);
  };
  const uint16_t working = text(dated(), phase(Phase::Working), nullptr);
  TEST_ASSERT_EQUAL_UINT16(working, text(dated(), phase(Phase::Idle), nullptr));  // (Idle reads Working's line)
  TEST_ASSERT_EQUAL_UINT16(working, text(dated(5), phase(Phase::Working), nullptr));  // (rows' kinds: the shape's)
  TEST_ASSERT_NOT_EQUAL(working, text(dated(), phase(Phase::Waiting), nullptr));
  const uint16_t noRoom = text(dated(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
  TEST_ASSERT_NOT_EQUAL(noRoom, text(dated(), phase(Phase::Unread, Outcome::Io), nullptr));
  TEST_ASSERT_NOT_EQUAL(noRoom, text(dated(), away, nullptr));  // card_back
  TEST_ASSERT_NOT_EQUAL(noRoom, working);
  TEST_ASSERT_NOT_EQUAL(text(out, away, nullptr), text(dated(), away, nullptr));  // the card out, back
  // Served: by the snapshot's version (a new ask's rows), not the phase.
  sv::MadeUp v2 = fullCard();
  v2.version = 2;
  Snap next;
  put(next, v2);
  TEST_ASSERT_EQUAL_UINT32(1, full->version());
  TEST_ASSERT_EQUAL_UINT32(2, next->version());
  const uint16_t served = text(dated(), kReady, full.s());
  TEST_ASSERT_EQUAL_UINT16(served, text(dated(), phase(Phase::Working), full.s()));  // (a lean ask under way: served)
  TEST_ASSERT_NOT_EQUAL(served, text(dated(), kReady, next.s()));
  TEST_ASSERT_NOT_EQUAL(served, working);
  // Off and nothing played: their look alone, whatever the line says.
  PageLook k;
  k.look = Look::Off;
  k.line = S::stats_waiting;
  const uint16_t offText = lookText(k, nullptr);
  k.line = S::stats_counting;
  TEST_ASSERT_EQUAL_UINT16(offText, lookText(k, full.s()));
  k.look = Look::NoPlays;
  TEST_ASSERT_NOT_EQUAL(offText, lookText(k, nullptr));
}

// Each hub row's line from a made-up snapshot, the phase's line, a reason,
// the status row's second line; nothing without a snapshot.
void test_the_hub_s_lines() {
  namespace st = statstext;
  int px = 6;
  const textfit::Font f = mono(&px);
  Snap full;
  put(full, fullCard());
  const sv::Snapshot& s = *full.s();
  const Lifetime l = dated();
  int asked = 0;
  char b[160], want[160];
  HubLook h = hubLook(l, kReady, &s);
  auto line = [&](uint32_t i) { return st::hubLine(h.item[i], l, &s, f, tableTitle, &asked, b, sizeof(b)); };
  // Last 7 days: the frames' week.
  line(0);
  TEST_ASSERT_EQUAL_STRING("7 h 37 min" DOT "118 plays", b);
  // Most played: the All time top three's titles, by the snapshot's ids.
  asked = 0;
  line(1);
  std::snprintf(want, sizeof(want), "T%u" DOT "T%u" DOT "T%u", static_cast<unsigned>(s.rowAt(sr::kMostAll, 0).id),
                static_cast<unsigned>(s.rowAt(sr::kMostAll, 1).id), static_cast<unsigned>(s.rowAt(sr::kMostAll, 2).id));
  TEST_ASSERT_EQUAL_STRING(want, b);
  TEST_ASSERT_EQUAL_INT(3, asked);
  // New finds and Old favourites: the totals (the shelves' rows are 23 and
  // 41 there, their totals the same: fullCard()'s).
  line(2);
  std::snprintf(want, sizeof(want), "%u tracks first heard in 30 days", static_cast<unsigned>(s.total(sr::kNewFinds)));
  TEST_ASSERT_EQUAL_STRING(want, b);
  line(3);
  std::snprintf(want, sizeof(want), "%u tracks, played often, not lately",
                static_cast<unsigned>(s.total(sr::kOldFavourites)));
  TEST_ASSERT_EQUAL_STRING(want, b);
  // Not played yet: its albums since the snapshot's first day.
  line(4);
  std::snprintf(want, sizeof(want), "%u albums since 1 Sep", static_cast<unsigned>(s.notPlayedCount()));
  TEST_ASSERT_EQUAL_STRING(want, b);
  // Recently played: the newest title, 25 min ago at the lifetime's now.
  asked = 0;
  line(5);
  std::snprintf(want, sizeof(want), "T%u" DOT "25 min ago", static_cast<unsigned>(s.rowAt(sr::kRecent, 0).id));
  TEST_ASSERT_EQUAL_STRING(want, b);
  TEST_ASSERT_EQUAL_INT(1, asked);
  // ... an hour on, "1 h ago"; the clock unset since, the title alone.
  Lifetime later = l;
  later.nowMin += 60;
  st::hubLine(h.item[5], later, &s, f, tableTitle, &asked, b, sizeof(b));
  std::snprintf(want, sizeof(want), "T%u" DOT "1 h ago", static_cast<unsigned>(s.rowAt(sr::kRecent, 0).id));
  TEST_ASSERT_EQUAL_STRING(want, b);
  st::hubLine(h.item[5], noClock(), &s, f, tableTitle, &asked, b, sizeof(b));
  std::snprintf(want, sizeof(want), "T%u", static_cast<unsigned>(s.rowAt(sr::kRecent, 0).id));
  TEST_ASSERT_EQUAL_STRING(want, b);
  // Data with no snapshot: nothing (a draw before the landing never reads
  // one: the look is Pending then, but the line guards it too).
  TEST_ASSERT_EQUAL_UINT32(0, st::hubLine(h.item[0], l, nullptr, f, tableTitle, &asked, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
  // No title function: the join of nothing.
  st::hubLine(h.item[5], l, &s, f, nullptr, nullptr, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("25 min ago", b);

  // Not played yet before its count: "since 1 Sep".
  Snap ls;
  sv::MadeUp lm = fullCard();
  lm.grade = sv::Grade::Lists;
  lm.notPlayedEvery = 0;
  put(ls, lm);
  h = hubLook(l, kReady, ls.s());
  st::hubLine(h.item[4], l, ls.s(), f, tableTitle, &asked, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("since 1 Sep", b);
  // Pending: the phase's line on every open row.
  h = hubLook(l, phase(Phase::Waiting), nullptr);
  for (uint32_t i = 0; i < h.rows; ++i) {
    st::hubLine(h.item[i], l, nullptr, f, tableTitle, &asked, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_waiting), b);
  }
  // Try again's reason, and a Retry's open rows: no line.
  h = hubLook(l, phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
  st::hubLine(h.item[6], l, nullptr, f, tableTitle, &asked, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_no_room), b);
  TEST_ASSERT_EQUAL_UINT32(0, st::hubLine(h.item[1], l, nullptr, f, tableTitle, &asked, b, sizeof(b)));
  // The first month: "Opens 6 Dec, after a month of plays" in 250 (35
  // characters: 210 px at 6), its short form at 8 px.
  const Lifetime early = dated(5);
  h = hubLook(early, phase(Phase::Working), nullptr);
  st::hubLine(h.item[2], early, nullptr, f, tableTitle, &asked, b, sizeof(b));
  char day[32];
  dayMonthText(opensDay(early), day, sizeof(day));
  std::snprintf(want, sizeof(want), "Opens %s, after a month of plays", day);
  TEST_ASSERT_EQUAL_STRING(want, b);
  px = 8;
  st::hubLine(h.item[2], early, nullptr, f, tableTitle, &asked, b, sizeof(b));
  std::snprintf(want, sizeof(want), "Opens on %s", day);
  TEST_ASSERT_EQUAL_STRING(want, b);
  px = 6;
  // No clock: the status row's second line; the disabled rows' "Needs
  // dates".
  h = hubLook(noClock(), phase(Phase::Working), nullptr);
  st::hubLine(h.item[0], noClock(), nullptr, f, tableTitle, &asked, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_clock_fix), b);
  st::hubLine(h.item[2], noClock(), nullptr, f, tableTitle, &asked, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_needs_dates), b);
  // A snapshot with no Recently played rows: none.
  sv::MadeUp none = fullCard();
  none.rows[sr::kRecent] = 0;
  none.totals[sr::kRecent] = 0;
  none.earlierRows = 0;
  Snap e;
  put(e, none);
  h = hubLook(l, kReady, e.s());
  TEST_ASSERT_EQUAL_UINT32(0, e->rows(sr::kRecent));
  TEST_ASSERT_EQUAL_UINT32(0, st::hubLine(h.item[5], l, e.s(), f, tableTitle, &asked, b, sizeof(b)));
}

// A new minute: when a row's "25 min ago" moves, and the minute a page
// drew at from its low 16 bits.
void test_a_new_minute() {
  namespace st = statstext;
  const uint32_t when = kToday * 1440u + 600;
  TEST_ASSERT_FALSE(st::agoMoved(when + 25, when + 25, when));
  TEST_ASSERT_TRUE(st::agoMoved(when + 25, when + 26, when));   // 25 to 26 min
  TEST_ASSERT_FALSE(st::agoMoved(when, when + 1, when));        // "1 min ago" both (0 s reads a minute)
  TEST_ASSERT_TRUE(st::agoMoved(when + 59, when + 60, when));   // 59 min to 1 h
  TEST_ASSERT_FALSE(st::agoMoved(when + 120, when + 179, when));  // 2 h all that hour
  TEST_ASSERT_TRUE(st::agoMoved(when + 179, when + 180, when));
  TEST_ASSERT_TRUE(st::agoMoved(when + 1439, when + 1440, when));  // 23 h to none
  TEST_ASSERT_FALSE(st::agoMoved(when + 1440, when + 5000, when));
  TEST_ASSERT_TRUE(st::agoMoved(when + 30, when + 300, when));   // minutes skipped (the dark): moved
  TEST_ASSERT_FALSE(st::agoMoved(when + 1, when + 2, sv::kWhenSession));
  TEST_ASSERT_FALSE(st::agoMoved(when + 1, when + 2, sv::kWhenEarlier));
  TEST_ASSERT_FALSE(st::agoMoved(when - 5, when - 4, when));  // after now (a clock set back): none either way
  // The minute from its low bits, at or before now.
  for (uint32_t now : {0u, 5u, 65535u, 65536u, 65540u, 1000000u, kToday * 1440u + 1300u, 0xFFFFFFF0u}) {
    for (uint32_t back : {0u, 1u, 59u, 1440u, 65535u}) {
      if (back > now) continue;
      const uint32_t was = now - back;
      TEST_ASSERT_EQUAL_UINT32(was, minuteFrom(static_cast<uint16_t>(was), now));
    }
  }
  TEST_ASSERT_EQUAL_UINT32(0, minuteFrom(10, 5));  // (low bits past a now under 65,536: none earlier)
}

// The latched entry row at the boot fold's end (A20): the lifetime line,
// nothing played, or "Couldn't read the stats" with the stats off since.
void test_the_entry_row_at_the_fold_s_end() {
  char b[96];
  int px = 6;
  Lifetime l = dated();
  l.folding = true;
  TEST_ASSERT_EQUAL(EntryLine::Counting, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_FALSE(entryFoldEnded(EntryLine::Counting, true));
  TEST_ASSERT_TRUE(entryFoldEnded(EntryLine::Counting, false));
  for (EntryLine e : {EntryLine::Empty, EntryLine::Since, EntryLine::Time, EntryLine::Unread}) {
    TEST_ASSERT_FALSE(entryFoldEnded(e, false));  // a later fold doesn't move it
    TEST_ASSERT_FALSE(entryFoldEnded(e, true));
  }
  l.folding = false;
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(l, monoWidth, &px, b, sizeof(b)));
  l.plays = 0;
  TEST_ASSERT_EQUAL(EntryLine::Empty, entryLine(l, monoWidth, &px, b, sizeof(b)));
  l = dated();
  l.on = false;
  TEST_ASSERT_EQUAL(EntryLine::Unread, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_unread), b);
  TEST_ASSERT_FALSE(entryDue(l, true, false, 1));  // (never at an enter(): absent then)
  // The fold ended with the card out (scanAway(): a row latched Counting):
  // RAM's HEAD, whole, read as it is (S3b's review, finding 6). Nothing
  // played: "Plays show up as you listen", never "0 min since 23 Nov"; with
  // plays, the lifetime line; still folding (the card held), Counting.
  l = dated();
  l.cardIn = false;
  l.plays = 0;
  l.listenedMs = 0;
  TEST_ASSERT_EQUAL(EntryLine::Empty, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_entry_empty), b);
  l.folding = true;
  TEST_ASSERT_EQUAL(EntryLine::Counting, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::stats_counting), b);
  l = dated();
  l.cardIn = false;
  l.folding = true;
  TEST_ASSERT_EQUAL(EntryLine::Counting, entryLine(l, monoWidth, &px, b, sizeof(b)));
  l.folding = false;
  TEST_ASSERT_EQUAL(EntryLine::Since, entryLine(l, monoWidth, &px, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("74 h 10 min since 1 Sep", b);
}

// ---------------------------------------------------------------------------
// A shelf as ui's LibraryPage draws, touches and acts on it (S3b's S10)
// ---------------------------------------------------------------------------
namespace {

// A list's row's id written over (a test's own: an id the index hasn't).
void putId(Snap& b, sr::List l, uint32_t row, uint16_t id) {
  uint8_t* p = reinterpret_cast<uint8_t*>(b.m.data()) + sv::offsetOf(l, kAlbums) +
               static_cast<size_t>(row) * sv::kStrides[l];
  p[0] = static_cast<uint8_t>(id);
  p[1] = static_cast<uint8_t>(id >> 8);
}

}  // namespace

// Each shelf's rows as the page reads them at a draw and an act: ranked
// rows by the snapshot's list (their ids the queue's view reads), Recently
// played's headings and rows, Not played yet's albums, the Try again row;
// an id the live index hasn't is never one (D6).
void test_a_shelf_s_rows() {
  Snap full;
  put(full, fullCard());
  const sv::Snapshot& s = *full.s();
  // Most played's three windows, New finds, Old favourites: rank order.
  const uint32_t ranked[] = {shelfId(Shelf::MostPlayed, sv::k7Days), shelfId(Shelf::MostPlayed, sv::k30Days),
                             shelfId(Shelf::MostPlayed, sv::kAllTime), shelfId(Shelf::NewFinds),
                             shelfId(Shelf::OldFavourites)};
  for (uint32_t id : ranked) {
    const ShelfLook k = shelfLook(id, dated(), kReady, &s);
    TEST_ASSERT_TRUE(k.rows > 0);
    const IdView v = IdView::u16(s.ids(k.list), s.rows(k.list), sv::kStrides[k.list]);
    TEST_ASSERT_EQUAL_UINT32(k.rows, v.count);
    for (uint32_t r = 0; r < k.rows; ++r) {
      const ShelfItem it = shelfItem(k, &s, r);
      TEST_ASSERT_EQUAL(ShelfRow::Ranked, it.kind);
      TEST_ASSERT_EQUAL(k.list, it.list);
      TEST_ASSERT_EQUAL_UINT32(r, it.at);
      TEST_ASSERT_EQUAL_UINT32(s.rowAt(k.list, r).id, it.id);
      TEST_ASSERT_EQUAL_UINT32(v[r], it.id);  // the act's view reads the row's id
      TEST_ASSERT_TRUE(it.id < kTracks);
      TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(r), shelfRowOf(k, &s, it.id, 0));
    }
    TEST_ASSERT_EQUAL(ShelfRow::None, shelfItem(k, &s, k.rows).kind);  // past the rows
    TEST_ASSERT_TRUE(idsServed(s, k.list, kTracks));
    TEST_ASSERT_FALSE(idsServed(s, k.list, s.rowAt(k.list, 0).id));  // (an index too small for its first)
  }
  // Recently played: its headings (no control) and its rows, each at its
  // snapshot row (Play plays the list from there).
  ShelfLook k = shelfLook(shelfId(Shelf::Recent), dated(), kReady, &s);
  uint32_t headings = 0, rows = 0;
  for (uint32_t r = 0; r < k.rows; ++r) {
    const ShelfItem it = shelfItem(k, &s, r);
    const RecentItem want = recentAt(s, r);
    if (want.heading) {
      TEST_ASSERT_EQUAL(ShelfRow::Section, it.kind);
      ++headings;
      continue;
    }
    TEST_ASSERT_EQUAL(ShelfRow::Ranked, it.kind);
    TEST_ASSERT_EQUAL(sr::kRecent, it.list);
    TEST_ASSERT_EQUAL_UINT32(want.row, it.at);
    TEST_ASSERT_EQUAL(want.timed, it.recent.timed);
    TEST_ASSERT_EQUAL_UINT32(s.rowAt(sr::kRecent, it.at).id, it.id);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(r), shelfRowOf(k, &s, it.id, 0));
    ++rows;
  }
  TEST_ASSERT_EQUAL_UINT32(s.rows(sr::kRecent), rows);
  TEST_ASSERT_TRUE(headings >= 2);  // (Today, a day or more, Earlier)
  // Not played yet: the k-th album not played, A-Z.
  k = shelfLook(shelfId(Shelf::NotPlayed), dated(), kReady, &s);
  TEST_ASSERT_EQUAL_UINT32(s.notPlayedCount(), k.rows);
  uint32_t last = 0;
  for (uint32_t r = 0; r < k.rows; ++r) {
    const ShelfItem it = shelfItem(k, &s, r);
    TEST_ASSERT_EQUAL(ShelfRow::Album, it.kind);
    TEST_ASSERT_EQUAL_UINT32(r, it.at);
    TEST_ASSERT_EQUAL_UINT32(s.albumAt(r), it.id);
    TEST_ASSERT_TRUE(it.id < kAlbums);
    TEST_ASSERT_TRUE(r == 0 || it.id > last);  // (albumsAZ()'s order: the rail's)
    last = it.id;
  }
  TEST_ASSERT_EQUAL_INT32(4, shelfRowOf(k, &s, 0, s.albumAt(4)));
  TEST_ASSERT_EQUAL_INT32(-1, shelfRowOf(k, &s, 0, 1));  // (view 1 leaves every third: 1 was played)
  // A track no list holds: no row.
  k = shelfLook(shelfId(Shelf::MostPlayed, sv::k7Days), dated(), kReady, &s);
  bool held = true;
  uint32_t outside = 0;
  for (; outside < kTracks && held; ++outside) {
    held = false;
    for (uint32_t r = 0; r < k.rows; ++r) held = held || s.rowAt(k.list, r).id == outside;
    if (!held) break;
  }
  TEST_ASSERT_EQUAL_INT32(-1, shelfRowOf(k, &s, outside, 0));

  // The Try again row (No room: first under the tray), and Not played yet's
  // own (its lean ask); nothing else, no row for what plays.
  k = shelfLook(shelfId(Shelf::MostPlayed), dated(), phase(Phase::NoRoom, Outcome::NoRoom), nullptr);
  TEST_ASSERT_TRUE(k.tray);
  TEST_ASSERT_EQUAL_UINT32(1, k.rows);
  TEST_ASSERT_EQUAL(ShelfRow::TryAgain, shelfItem(k, nullptr, 0).kind);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_room), static_cast<int>(shelfItem(k, nullptr, 0).text));
  TEST_ASSERT_EQUAL(ShelfRow::None, shelfItem(k, nullptr, 1).kind);
  TEST_ASSERT_EQUAL_INT32(-1, shelfRowOf(k, nullptr, 5, 0));
  sv::MadeUp lists = fullCard();  // (the Lists grade: no bits)
  lists.grade = sv::Grade::Lists;
  lists.notPlayedEvery = 0;
  Snap ls;
  put(ls, lists);
  k = shelfLook(shelfId(Shelf::NotPlayed), dated(), kReady, ls.s());
  TEST_ASSERT_EQUAL(UiAsk::NotPlayed, k.ask);
  TEST_ASSERT_EQUAL(ShelfRow::TryAgain, shelfItem(k, ls.s(), 0).kind);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_no_room), static_cast<int>(shelfItem(k, ls.s(), 0).text));
  // The card back (the owner's call 3): its reason "The card is back".
  k = shelfLook(shelfId(Shelf::OldFavourites), dated(), phase(Phase::NoCard, Outcome::CardAway), nullptr);
  TEST_ASSERT_EQUAL(ShelfRow::TryAgain, shelfItem(k, nullptr, 0).kind);
  TEST_ASSERT_EQUAL(static_cast<int>(S::stats_card_back), static_cast<int>(shelfItem(k, nullptr, 0).text));
  // Pending, the No card line: no rows at all.
  for (const PhaseInfo& p : {phase(Phase::Working), phase(Phase::Waiting)}) {
    k = shelfLook(shelfId(Shelf::Recent), dated(), p, nullptr);
    TEST_ASSERT_EQUAL_UINT32(0, k.rows);
    TEST_ASSERT_EQUAL(ShelfRow::None, shelfItem(k, nullptr, 0).kind);
  }

  // D6: a row whose id the index hasn't reads None, and the list acts on
  // nothing (Most played All time's row 3 made 65,000, past 19,417 tracks).
  Snap bad;
  put(bad, fullCard());
  putId(bad, sr::kMostAll, 3, 65000);
  k = shelfLook(shelfId(Shelf::MostPlayed), dated(), kReady, bad.s());
  TEST_ASSERT_EQUAL(ShelfRow::Ranked, shelfItem(k, bad.s(), 2).kind);
  TEST_ASSERT_EQUAL(ShelfRow::None, shelfItem(k, bad.s(), 3).kind);
  TEST_ASSERT_FALSE(idsServed(*bad.s(), sr::kMostAll, kTracks));
  TEST_ASSERT_TRUE(idsServed(*bad.s(), sr::kMost7, kTracks));
  // An empty list acts on nothing either (its top bar isn't there).
  sv::MadeUp none = fullCard();
  none.rows[sr::kMost7] = 0;
  Snap ns;
  put(ns, none);
  TEST_ASSERT_FALSE(idsServed(*ns.s(), sr::kMost7, kTracks));
}

// How each shelf row takes a touch (crit's per-kind table: 7.6), and when
// its rows are pending, arrive, and the tray or header is drawn again.
void test_a_shelf_s_touch_place_and_shape() {
  struct Want {
    ShelfRow k;
    bool pressable, inert, holds;
  };
  const Want table[] = {{ShelfRow::Ranked, true, false, true},
                        {ShelfRow::Album, true, false, true},
                        {ShelfRow::TryAgain, true, false, false},
                        {ShelfRow::Section, false, false, false},
                        {ShelfRow::None, false, false, false}};
  for (const Want& w : table) {
    const Control c = shelfControlOf(w.k);
    TEST_ASSERT_EQUAL_MESSAGE(w.pressable, c.pressable, shelfRowName(w.k));
    TEST_ASSERT_EQUAL_MESSAGE(w.inert, c.inert, shelfRowName(w.k));
    TEST_ASSERT_EQUAL_MESSAGE(w.holds, c.holds, shelfRowName(w.k));
  }
  TEST_ASSERT_EQUAL_STRING("Try again", shelfRowName(ShelfRow::TryAgain));

  // Pending (D5): nothing served, or Not played yet with no bits.
  Snap full;
  put(full, fullCard());
  sv::MadeUp lists = fullCard();
  lists.grade = sv::Grade::Lists;
  lists.notPlayedEvery = 0;
  Snap ls;
  put(ls, lists);
  for (uint32_t si = 0; si < kShelves; ++si) {
    const Shelf sh = static_cast<Shelf>(si);
    TEST_ASSERT_TRUE(shelfPending(sh, nullptr));
    TEST_ASSERT_FALSE(shelfPending(sh, full.s()));
    TEST_ASSERT_EQUAL(sh == Shelf::NotPlayed, shelfPending(sh, ls.s()));
  }
  // The arrival: drawn without the snapshot's rows, served with rows now.
  TEST_ASSERT_TRUE(shelfArrives(Look::Pending, Look::Served, 12));
  TEST_ASSERT_TRUE(shelfArrives(Look::Retry, Look::Served, 1));  // (Try again, then the landing)
  TEST_ASSERT_TRUE(shelfArrives(Look::NoCard, Look::Served, 3));
  TEST_ASSERT_FALSE(shelfArrives(Look::Pending, Look::Served, 0));  // "Nothing in this period."
  TEST_ASSERT_FALSE(shelfArrives(Look::Served, Look::Served, 12));  // a new ask's rows: restaged in place
  TEST_ASSERT_FALSE(shelfArrives(Look::Pending, Look::Retry, 1));

  // The shape: look, cells, window, the short 30-day label.
  const Lifetime l = dated();
  const uint32_t most = shelfId(Shelf::MostPlayed);
  auto shapeOf = [](uint32_t id, const Lifetime& life, const PhaseInfo& p, const sv::Snapshot* snap) {
    return shelfShape(shelfLook(id, life, p, snap), trayOf(life, snap));
  };
  const uint8_t pending = shapeOf(most, l, phase(Phase::Working), nullptr);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Look::Pending), pending & kShelfLook);
  TEST_ASSERT_EQUAL_UINT8(2u << 3, pending & kShelfCells);
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime << 5, pending & kShelfWindow);
  TEST_ASSERT_EQUAL_UINT8(0, pending & kShelfShort);
  const uint8_t served = shapeOf(most, l, kReady, full.s());
  TEST_ASSERT_FALSE(trayMoved(pending, served, true));  // a landing, RCNT whole: the tray as drawn
  TEST_ASSERT_FALSE(shelfHeaderMoved(pending, served, true));
  // A landing whose RCNT is capped: "16 days" on the 30-day cell.
  sv::MadeUp capped = fullCard();
  capped.rcntFirstDay = static_cast<uint16_t>(kToday - 15);
  capped.rcntShort = true;
  Snap cs;
  put(cs, capped);
  const uint8_t shortServed = shapeOf(most, l, kReady, cs.s());
  TEST_ASSERT_EQUAL_UINT8(kShelfShort, shortServed & kShelfShort);
  TEST_ASSERT_TRUE(trayMoved(pending, shortServed, true));
  TEST_ASSERT_TRUE(trayMoved(shortServed, shortServed, true));    // "16 days" to a new ask's "15 days"
  TEST_ASSERT_FALSE(trayMoved(shortServed, shortServed, false));  // the same
  // ... at All time the header ("since 1 Sep") doesn't read the days.
  TEST_ASSERT_FALSE(shelfHeaderMoved(pending, shortServed, true));
  TEST_ASSERT_FALSE(shelfHeaderMoved(shortServed, shortServed, true));
  // At the 30 days (S3b's review, finding 8) the header names the window
  // the picked cell names: "16 days" both, over the 16 days' rows, never
  // "last 30 days" over them; a whole RCNT "last 30 days" and "30 days".
  {
    namespace st = statstext;
    const uint32_t month = shelfId(Shelf::MostPlayed, sv::k30Days);
    char sub[64], cell[48];
    for (const sv::Snapshot* snap : {static_cast<const sv::Snapshot*>(cs.s()), static_cast<const sv::Snapshot*>(full.s())}) {
      const ShelfLook k = shelfLook(month, l, kReady, snap);
      const Tray t = trayOf(l, snap);
      TEST_ASSERT_EQUAL_UINT8(sv::k30Days, k.window);
      TEST_ASSERT_EQUAL_UINT8(3, t.cells);
      TEST_ASSERT_EQUAL_UINT8(sv::k30Days, t.window[1]);
      st::shelfSub(k.shelf, k.window, t.days30, l.sinceDay, sub, sizeof(sub));
      st::trayLabel(t.window[1], t.days30, cell, sizeof(cell));
      if (snap == cs.s()) {
        TEST_ASSERT_EQUAL_UINT8(16, t.days30);
        TEST_ASSERT_EQUAL_STRING("16 days", cell);
        TEST_ASSERT_EQUAL_STRING(cell, sub);
      } else {
        TEST_ASSERT_EQUAL_STRING("30 days", cell);
        TEST_ASSERT_EQUAL_STRING("last 30 days", sub);
      }
    }
    // ... and the header is drawn again as the cell is: a capped landing,
    // a new ask's days ("16 days" to "15 days": its texts), not the same.
    const uint8_t monthPending = shapeOf(month, l, phase(Phase::Working), nullptr);
    const uint8_t monthWhole = shapeOf(month, l, kReady, full.s());
    const uint8_t monthShort = shapeOf(month, l, kReady, cs.s());
    TEST_ASSERT_EQUAL_UINT8(kShelfShort, monthShort & kShelfShort);
    TEST_ASSERT_TRUE(shelfHeaderMoved(monthPending, monthShort, true));
    TEST_ASSERT_TRUE(shelfHeaderMoved(monthPending, monthShort, false));
    TEST_ASSERT_TRUE(shelfHeaderMoved(monthShort, monthWhole, true));  // a new ask, RCNT whole again
    TEST_ASSERT_TRUE(shelfHeaderMoved(monthShort, monthShort, true));
    TEST_ASSERT_FALSE(shelfHeaderMoved(monthShort, monthShort, false));
    TEST_ASSERT_FALSE(shelfHeaderMoved(monthPending, monthWhole, true));  // "last 30 days" both
    TEST_ASSERT_FALSE(shelfHeaderMoved(monthWhole, monthWhole, true));
  }
  // A tray tap: the window, so the tray and the header.
  const uint8_t week = shapeOf(shelfId(Shelf::MostPlayed, sv::k7Days), l, kReady, full.s());
  TEST_ASSERT_TRUE(trayMoved(served, week, false));
  TEST_ASSERT_TRUE(shelfHeaderMoved(served, week, false));
  // A day that makes two cells three; none under 7 days (no tray: the item
  // comes or goes, staged as any).
  TEST_ASSERT_TRUE(trayMoved(shapeOf(most, dated(29), kReady, full.s()), shapeOf(most, dated(30), kReady, full.s()),
                             false));
  TEST_ASSERT_EQUAL_UINT8(0, shapeOf(most, dated(6), kReady, full.s()) & kShelfCells);
  TEST_ASSERT_FALSE(
      trayMoved(shapeOf(most, dated(6), kReady, full.s()), shapeOf(most, dated(7), kReady, full.s()), true));
  // The card out: no tray over the No card line.
  Lifetime out = dated();
  out.cardIn = false;
  const uint8_t noCard = shapeOf(most, out, phase(Phase::NoCard, Outcome::CardAway), nullptr);
  TEST_ASSERT_EQUAL_UINT8(0, noCard & kShelfCells);
  TEST_ASSERT_FALSE(trayMoved(served, noCard, true));
  // Other shelves: no tray, All time, their look alone.
  for (uint32_t si = 1; si < kShelves; ++si) {
    const uint8_t sh = shapeOf(shelfId(static_cast<Shelf>(si)), l, kReady, full.s());
    TEST_ASSERT_EQUAL_UINT8(0, sh & (kShelfCells | kShelfShort));
    TEST_ASSERT_EQUAL_UINT8(sv::kAllTime << 5, sh & kShelfWindow);
  }
  // Every look, cells, window and label its own shape.
  std::vector<uint8_t> seen;
  for (uint32_t w = 0; w <= sv::kAllTime; ++w)
    for (uint32_t c = 0; c < 3; ++c)
      for (uint32_t lk = 0; lk < 7; ++lk)
        for (uint32_t sh = 0; sh < 2; ++sh) {
          if (sh && c != 2) continue;
          ShelfLook k;
          k.page.look = static_cast<Look>(lk);
          k.tray = c > 0;
          k.window = static_cast<uint8_t>(w);
          Tray t;
          t.cells = c == 2 ? 3 : c == 1 ? 2 : 0;
          t.days30 = sh ? 16 : 30;
          const uint8_t got = shelfShape(k, t);
          TEST_ASSERT_TRUE(std::find(seen.begin(), seen.end(), got) == seen.end());
          seen.push_back(got);
        }

  // The console's uiS<shelf>[w].
  Shelf sh = Shelf::Recent;
  uint8_t w = 9;
  bool given = true;
  TEST_ASSERT_TRUE(shelfArg("0", &sh, &w, &given));
  TEST_ASSERT_EQUAL(Shelf::MostPlayed, sh);
  TEST_ASSERT_FALSE(given);
  TEST_ASSERT_TRUE(shelfArg("01", &sh, &w, &given));
  TEST_ASSERT_EQUAL_UINT8(sv::k30Days, w);
  TEST_ASSERT_TRUE(given);
  TEST_ASSERT_TRUE(shelfArg("4", &sh, &w, &given));
  TEST_ASSERT_EQUAL(Shelf::Recent, sh);
  TEST_ASSERT_FALSE(given);
  TEST_ASSERT_TRUE(shelfArg("3", &sh, &w, &given));
  TEST_ASSERT_EQUAL(Shelf::NotPlayed, sh);
  for (const char* bad : {"", "5", "9", "03", "12", "021", "0x", "h", "w"})
    TEST_ASSERT_FALSE_MESSAGE(shelfArg(bad, &sh, &w, &given), bad);
}

// D7's window written back whenever Most played is served showing another
// (a landing with no rows in its window too: S10's review), and a restage
// for the tray alone (the page's latch taking the counting days again:
// its rows kept, its bar open).
void test_most_played_s_window_and_the_tray_alone() {
  Snap full;
  put(full, fullCard());
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, full->window());  // (a new loan's: All time)
  const Lifetime l = dated();
  const uint32_t week = shelfId(Shelf::MostPlayed, sv::k7Days), all = shelfId(Shelf::MostPlayed, sv::kAllTime);
  ShelfLook k = shelfLook(week, l, kReady, full.s());
  TEST_ASSERT_EQUAL(Look::Served, k.page.look);
  TEST_ASSERT_TRUE(k.rows > 0);
  TEST_ASSERT_TRUE(windowOwed(k, full.s()));
  TEST_ASSERT_FALSE(windowOwed(shelfLook(all, l, kReady, full.s()), full.s()));  // (the snapshot's own)
  TEST_ASSERT_FALSE(windowOwed(k, nullptr));                                    // (nothing served)
  // The window with no plays: served, no rows ("Nothing in this period."),
  // so no arrival; the write is owed all the same (the hub's next push).
  sv::MadeUp quiet = fullCard();
  quiet.rows[sr::kMost7] = 0;
  Snap qs;
  put(qs, quiet);
  k = shelfLook(week, l, kReady, qs.s());
  TEST_ASSERT_EQUAL(Look::Served, k.page.look);
  TEST_ASSERT_EQUAL_UINT32(0, k.rows);
  TEST_ASSERT_TRUE(k.drawLine);
  TEST_ASSERT_FALSE(shelfArrives(Look::Pending, k.page.look, k.rows));
  TEST_ASSERT_TRUE(windowOwed(k, qs.s()));
  qs.s()->setWindow(sv::k7Days);  // (written: none owed after)
  TEST_ASSERT_FALSE(windowOwed(k, qs.s()));
  // Pending, the card out, the stats off: no window shown, none owed.
  TEST_ASSERT_FALSE(windowOwed(shelfLook(week, l, phase(Phase::Working), nullptr), nullptr));
  Lifetime out = dated();
  out.cardIn = false;
  TEST_ASSERT_FALSE(windowOwed(shelfLook(week, out, phase(Phase::NoCard, Outcome::CardAway), full.s()), full.s()));
  Lifetime off = dated();
  off.on = false;
  TEST_ASSERT_FALSE(windowOwed(shelfLook(week, off, kReady, full.s()), full.s()));
  // Another shelf never writes it (its window reads All time).
  full.s()->setWindow(sv::k7Days);
  for (uint32_t si = 1; si < kShelves; ++si)
    TEST_ASSERT_FALSE(windowOwed(shelfLook(shelfId(static_cast<Shelf>(si)), l, kReady, full.s()), full.s()));
  // A kept 30 days the tray doesn't offer (day 20: two cells) shows All
  // time, written back.
  TEST_ASSERT_TRUE(windowOwed(shelfLook(shelfId(Shelf::MostPlayed, sv::k30Days), dated(20), kReady, full.s()),
                              full.s()));
  full.s()->setWindow(sv::kAllTime);

  // The tray alone: two cells three, or the tray coming (day 7), with the
  // rows' texts as drawn (the page's latch taking the counting days again
  // at a lifetime's first whole reading: never a new day while it shows,
  // test_the_counting_days_a_page_holds).
  auto shapeOf = [](uint32_t id, const Lifetime& life, const sv::Snapshot* snap) {
    return shelfShape(shelfLook(id, life, kReady, snap), trayOf(life, snap));
  };
  auto textOf = [](uint32_t id, const Lifetime& life, const sv::Snapshot* snap) {
    return lookText(shelfLook(id, life, kReady, snap).page, snap);
  };
  TEST_ASSERT_EQUAL_UINT16(textOf(all, dated(29), full.s()), textOf(all, dated(30), full.s()));
  TEST_ASSERT_TRUE(trayAlone(shapeOf(all, dated(29), full.s()), shapeOf(all, dated(30), full.s()), false));
  TEST_ASSERT_TRUE(trayAlone(shapeOf(all, dated(6), full.s()), shapeOf(all, dated(7), full.s()), false));
  TEST_ASSERT_FALSE(trayAlone(shapeOf(all, dated(29), full.s()), shapeOf(all, dated(30), full.s()), true));
  TEST_ASSERT_FALSE(trayAlone(shapeOf(all, dated(30), full.s()), shapeOf(all, dated(30), full.s()), false));
  // RCNT capped: the 30-day cell's "16 days" comes with the third cell.
  sv::MadeUp capped = fullCard();
  capped.rcntFirstDay = static_cast<uint16_t>(kToday - 15);
  capped.rcntShort = true;
  Snap cs;
  put(cs, capped);
  const uint8_t two = shapeOf(all, dated(29), cs.s()), three = shapeOf(all, dated(30), cs.s());
  TEST_ASSERT_EQUAL_UINT8(0, two & kShelfShort);
  TEST_ASSERT_EQUAL_UINT8(kShelfShort, three & kShelfShort);
  TEST_ASSERT_TRUE(trayAlone(two, three, false));
  // Not alone: a tray tap (the window), a landing (the look), a kept 7
  // days pushed on day 6 that day 7's tray now offers (its window moves:
  // other rows).
  TEST_ASSERT_FALSE(trayAlone(shapeOf(all, l, full.s()), shapeOf(week, l, full.s()), false));
  TEST_ASSERT_FALSE(trayAlone(shelfShape(shelfLook(all, l, phase(Phase::Working), nullptr), trayOf(l, nullptr)),
                              shapeOf(all, l, full.s()), false));
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime << 5, shapeOf(week, dated(6), full.s()) & kShelfWindow);
  TEST_ASSERT_FALSE(trayAlone(shapeOf(week, dated(6), full.s()), shapeOf(week, dated(7), full.s()), false));
}

// uiS's checksum over a served shelf (run 3b-6): the ranked rows' ids and
// figures, and Not played yet's albums in the bits' order, the values
// albumAt(r) a row gives (the old walk, O(rows x groups) in one pass), in
// one walk of the bits (S3b's review, finding 12): every shelf, albums at
// and past a group's end, every album or few of them not played.
void test_the_uis_checksum_in_one_walk() {
  auto reference = [](const ShelfLook& k, const sv::Snapshot& s) {
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint32_t v) { hash = (hash ^ v) * 16777619u; };
    if (k.shelf == Shelf::NotPlayed) {
      for (uint32_t r = 0; r < k.rows; ++r) mix(s.albumAt(r));
    } else {
      for (uint32_t r = 0; r < s.rows(k.list); ++r) {
        const sv::Row row = s.rowAt(k.list, r);
        mix(row.id);
        mix(row.count);
        mix(row.lastDay);
        mix(row.when);
      }
    }
    return hash;
  };
  const uint32_t ids[] = {shelfId(Shelf::MostPlayed, sv::k7Days), shelfId(Shelf::MostPlayed, sv::k30Days),
                          shelfId(Shelf::MostPlayed, sv::kAllTime), shelfId(Shelf::NewFinds),
                          shelfId(Shelf::OldFavourites), shelfId(Shelf::NotPlayed), shelfId(Shelf::Recent)};
  const uint32_t albums[] = {kAlbums, 64, 65, 127, 128, 1};
  const uint32_t every[] = {1, 3, 7, 64, 1000};
  uint32_t notPlayed = 0;
  for (uint32_t a : albums) {
    for (uint32_t e : every) {
      sv::MadeUp m = fullCard();
      m.albums = a;
      m.notPlayedEvery = e;
      Snap b;
      put(b, m);
      for (uint32_t id : ids) {
        const ShelfLook k = shelfLook(id, dated(), kReady, b.s());
        TEST_ASSERT_EQUAL(Look::Served, k.page.look);
        TEST_ASSERT_FALSE(k.tryAgain);
        char msg[64];
        snprintf(msg, sizeof(msg), "shelf %u, %u albums, every %u", static_cast<unsigned>(id),
                 static_cast<unsigned>(a), static_cast<unsigned>(e));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(reference(k, *b.s()), shelfSum(k, *b.s()), msg);
        if (k.shelf != Shelf::NotPlayed) continue;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((a + e - 1) / e, k.rows, msg);
        ++notPlayed;
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT32(30, notPlayed);
  // (A checksum: another set of albums not played reads another sum.)
  uint32_t sums[2] = {};
  for (int i = 0; i < 2; ++i) {
    sv::MadeUp m = fullCard();
    m.notPlayedEvery = i ? 7 : 3;
    Snap b;
    put(b, m);
    sums[i] = shelfSum(shelfLook(shelfId(Shelf::NotPlayed), dated(), kReady, b.s()), *b.s());
  }
  TEST_ASSERT_NOT_EQUAL(sums[0], sums[1]);
}

// 7.10's "A new UTC day": the hub's disabled rows and the tray (its cells,
// the window shown) follow the counting days at the next enter(). A page
// latches them at its enter() (and the wake), holds them across a new day,
// the clock set back and a fold, and takes them again at the first whole
// reading after one that wasn't (a re-read's end, the clock set).
void test_the_counting_days_a_page_holds() {
  Snap full;
  put(full, fullCard());
  const uint32_t all = shelfId(Shelf::MostPlayed, sv::kAllTime), month = shelfId(Shelf::MostPlayed, sv::k30Days);
  const uint32_t finds = shelfId(Shelf::NewFinds);
  const int nf = static_cast<int>(HubRow::NewFinds), of = static_cast<int>(HubRow::OldFavourites),
            np = static_cast<int>(HubRow::NotPlayed);
  // The page's reading of the lifetime `now` under its latch.
  auto seen = [](Lifetime now, uint8_t held) {
    holdDays(now, held);
    return now;
  };
  auto cells = [&](const Lifetime& now, uint8_t held) { return trayOf(seen(now, held), full.s()).cells; };
  auto shelf = [&](uint32_t id, const Lifetime& now, uint8_t held) {
    return shelfLook(id, seen(now, held), kReady, full.s());
  };
  auto shape = [&](uint32_t id, const Lifetime& now, uint8_t held) {
    return shelfShape(shelf(id, now, held), trayOf(seen(now, held), full.s()));
  };
  auto hub = [&](const Lifetime& now, uint8_t held) { return hubLook(seen(now, held), kReady, full.s()); };
  auto monthRows = [&](const HubLook& h, RowKind want, const char* msg) {
    TEST_ASSERT_EQUAL_MESSAGE(want, h.item[nf].kind, msg);
    TEST_ASSERT_EQUAL_MESSAGE(want, h.item[of].kind, msg);
    TEST_ASSERT_EQUAL_MESSAGE(want, h.item[np].kind, msg);
  };

  // Day 6 to 7 while Most played shows: no tray until the next enter(),
  // nothing for a pass to restage.
  uint8_t held = latchDays(0, dated(6), true);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 6, held);
  held = latchDays(held, dated(7), false);  // (a pass past UTC midnight)
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 6, held);
  TEST_ASSERT_EQUAL(0, cells(dated(7), held));
  TEST_ASSERT_FALSE(shelf(all, dated(7), held).tray);
  TEST_ASSERT_EQUAL_UINT8(shape(all, dated(6), held), shape(all, dated(7), held));
  held = latchDays(held, dated(7), true);  // the next enter()
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 7, held);
  TEST_ASSERT_EQUAL(2, cells(dated(7), held));
  TEST_ASSERT_TRUE(shelf(all, dated(7), held).tray);

  // Day 29 to 30 on the hub and the shelves: rows 3-5 disabled, New finds
  // shut, two cells (a kept 30 days shown as All time); at the next
  // enter(), open, three cells, the 30 days shown.
  held = latchDays(latchDays(0, dated(29), true), dated(30), false);
  monthRows(hub(dated(30), held), RowKind::Disabled, "29 to 30, held");
  TEST_ASSERT_EQUAL_UINT8(hubShape(hub(dated(29), held)), hubShape(hub(dated(30), held)));
  TEST_ASSERT_EQUAL(Look::Closed, shelf(finds, dated(30), held).page.look);
  TEST_ASSERT_EQUAL(2, cells(dated(30), held));
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, shelf(month, dated(30), held).window);
  TEST_ASSERT_EQUAL_UINT8(shape(month, dated(29), held), shape(month, dated(30), held));
  held = latchDays(held, dated(30), true);
  monthRows(hub(dated(30), held), RowKind::Open, "30, entered");
  TEST_ASSERT_EQUAL(Look::Served, shelf(finds, dated(30), held).page.look);
  TEST_ASSERT_EQUAL(3, cells(dated(30), held));
  TEST_ASSERT_EQUAL_UINT8(sv::k30Days, shelf(month, dated(30), held).window);

  // The clock set back (a computer's @time), 30 to 29 and 7 to 6: as
  // entered until the next enter().
  held = latchDays(latchDays(0, dated(30), true), dated(29), false);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 30, held);
  monthRows(hub(dated(29), held), RowKind::Open, "30 to 29, held");
  TEST_ASSERT_EQUAL(3, cells(dated(29), held));
  TEST_ASSERT_EQUAL_UINT8(sv::k30Days, shelf(month, dated(29), held).window);
  held = latchDays(held, dated(29), true);
  monthRows(hub(dated(29), held), RowKind::Disabled, "29, entered");
  TEST_ASSERT_EQUAL(2, cells(dated(29), held));
  held = latchDays(latchDays(0, dated(7), true), dated(6), false);
  TEST_ASSERT_EQUAL(2, cells(dated(6), held));
  TEST_ASSERT_EQUAL(0, cells(dated(6), latchDays(held, dated(6), true)));

  // A fold while a page shows (plays and time; the first day as it was):
  // held.
  Lifetime folded = dated(29);
  folded.plays += 1;
  folded.listenedMs += 180000;
  held = latchDays(0, dated(29), true);
  TEST_ASSERT_EQUAL_UINT8(held, latchDays(held, folded, false));

  // Not whole: a remount's re-read (HEAD cleared until it is read: its first
  // day today, one counting day, folding). The days held stay, an enter()
  // meanwhile among it, marked; so the card back's Try again keeps rows 3-5
  // open. Its whole end takes the days (another card's 5: shut, no tray).
  Lifetime reread = dated(71);
  reread.folding = true;
  reread.sinceDay = kToday;
  reread.countingDays = 1;
  TEST_ASSERT_FALSE(daysWhole(reread));
  held = latchDays(latchDays(0, dated(71), true), reread, false);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | kDaysMask, held);
  TEST_ASSERT_EQUAL_UINT8(held, latchDays(held, reread, true));
  const HubLook back = hubLook(seen(reread, held), phase(Phase::NoCard, Outcome::CardAway), nullptr);
  TEST_ASSERT_EQUAL(Look::Retry, back.page.look);
  monthRows(back, RowKind::Open, "a re-read under way");
  held = latchDays(held, dated(5), false);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 5, held);
  monthRows(hub(dated(5), held), RowKind::Disabled, "another card's 5 days");
  TEST_ASSERT_EQUAL(0, cells(dated(5), held));
  // ... under 30 days: a re-read of a card the page held at 5 reads 5 (the
  // mark is no day): rows 3-5 disabled, no tray, New finds shut (S3b's
  // review of the cross-cutting fixes: the marked days read back).
  const PhaseInfo away = phase(Phase::NoCard, Outcome::CardAway);
  held = latchDays(latchDays(0, dated(5), true), reread, false);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | 5, held);
  TEST_ASSERT_EQUAL_UINT32(5, seen(reread, held).countingDays);
  monthRows(hubLook(seen(reread, held), away, nullptr), RowKind::Disabled, "a re-read, 5 days held");
  TEST_ASSERT_EQUAL(0, cells(reread, held));
  TEST_ASSERT_EQUAL(Look::Closed, shelfLook(finds, seen(reread, held), away, nullptr).page.look);
  // ... the same marks with the card out, the stats off, HEAD's first day
  // unknown; taken at the next whole reading.
  Lifetime out = dated(40), off = dated(40), noFirst = dated(40);
  out.cardIn = false;
  off.on = false;
  noFirst.sinceDay = sf::kUnsetDay;
  noFirst.countingDays = 0;
  for (const Lifetime& l : {out, off, noFirst}) {
    TEST_ASSERT_FALSE(daysWhole(l));
    TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | 20, latchDays(kDaysHeld | 20, l, false));
  }
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 40, latchDays(kDaysHeld | kDaysRetake | 20, dated(40), false));
  // Entered with no clock: the clock set takes the days (the status row
  // goes with it, a look the page draws anew anyway).
  held = latchDays(0, noClock(), true);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake, held);
  TEST_ASSERT_EQUAL(0, cells(noClock(), held));
  held = latchDays(held, dated(40), false);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | 40, held);
  TEST_ASSERT_EQUAL(3, cells(dated(40), held));
  // Days held over an undated lifetime read as undated: no tray, the status
  // row, rows 3-4 "Needs dates".
  const HubLook undated = hub(noClock(), kDaysHeld | 40);
  TEST_ASSERT_EQUAL(0, cells(noClock(), kDaysHeld | 40));
  TEST_ASSERT_EQUAL(RowKind::Status, undated.item[static_cast<int>(HubRow::Week)].kind);
  lineIs(Line::Text, S::stats_needs_dates, undated.item[nf], "held days, undated");
  // None held: the lifetime's own. Past kDaysMask, clamped: the edges kept.
  TEST_ASSERT_EQUAL_UINT32(71, seen(dated(71), 0).countingDays);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysMask, latchDays(0, dated(200), true));
  TEST_ASSERT_EQUAL(State::Ready, stateOf(seen(dated(200), kDaysHeld | kDaysMask)));
  TEST_ASSERT_EQUAL(3, cells(dated(200), kDaysHeld | kDaysMask));
  // ... and not whole (a page first entered during the boot's fold): the
  // lifetime's own, marked. Under 30, shut with no tray; past kDaysMask,
  // clamped, three cells.
  Lifetime fold5 = dated(5), fold71 = dated(71);
  fold5.folding = true;
  fold71.folding = true;
  TEST_ASSERT_FALSE(daysWhole(fold5));
  held = latchDays(0, fold5, true);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | 5, held);
  TEST_ASSERT_EQUAL_UINT32(5, seen(fold5, held).countingDays);
  TEST_ASSERT_EQUAL(0, cells(fold5, held));
  held = latchDays(0, fold71, true);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | kDaysMask, held);
  TEST_ASSERT_EQUAL(3, cells(fold71, held));
  // The made-up views: uiSx<n> enters the page again, so it takes the
  // view's (view 4's day 5: no tray; view 1's full card: three cells).
  Lifetime v1, v4;
  TEST_ASSERT_TRUE(madeUpState(1, &v1, nullptr));
  TEST_ASSERT_TRUE(madeUpState(4, &v4, nullptr));
  held = latchDays(kDaysHeld | 40, v4, true);
  TEST_ASSERT_EQUAL(0, cells(v4, held));
  held = latchDays(held, v1, true);
  TEST_ASSERT_EQUAL(3, cells(v1, held));
  // View 5 (the boot's fold over a full card's lifetime, not whole) after
  // view 4's pages, as 3b-1 runs them: day 5's held, marked. The hub's rows
  // 3-5 disabled, opening 30 Sep (view 5's first day, 1 Sep, + 29); Most
  // played with no tray; New finds, Old favourites and Not played yet shut
  // on that line; Recently played the phase's (Counting plays…). After view
  // 1's pages instead: the full card folding (rows 3-5 open, three cells).
  Lifetime v5;
  PhaseInfo p5;
  TEST_ASSERT_TRUE(madeUpState(5, &v5, &p5));
  TEST_ASSERT_FALSE(daysWhole(v5));
  held = latchDays(latchDays(kDaysHeld | 40, v4, true), v5, true);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | 5, held);
  const Lifetime f5 = seen(v5, held);
  TEST_ASSERT_EQUAL_UINT32(5, f5.countingDays);
  const HubLook h5 = hubLook(f5, p5, nullptr);
  TEST_ASSERT_EQUAL(Look::Pending, h5.page.look);
  monthRows(h5, RowKind::Disabled, "view 5 after view 4");
  TEST_ASSERT_EQUAL_UINT16(kSince + 29, opensDay(f5));  // (30 Sep)
  TEST_ASSERT_EQUAL(0, trayOf(f5, nullptr).cells);
  TEST_ASSERT_FALSE(shelfLook(all, f5, p5, nullptr).tray);
  for (Shelf s : {Shelf::NewFinds, Shelf::OldFavourites, Shelf::NotPlayed}) {
    const ShelfLook k = shelfLook(shelfId(s), f5, p5, nullptr);
    TEST_ASSERT_EQUAL(Look::Closed, k.page.look);
    TEST_ASSERT_EQUAL(S::stats_warming, k.page.line);
  }
  const ShelfLook recent5 = shelfLook(shelfId(Shelf::Recent), f5, p5, nullptr);
  TEST_ASSERT_EQUAL(Look::Pending, recent5.page.look);
  TEST_ASSERT_EQUAL(S::stats_counting, recent5.page.line);
  held = latchDays(latchDays(kDaysHeld | 5, v1, true), v5, true);
  TEST_ASSERT_EQUAL_UINT8(kDaysHeld | kDaysRetake | kDaysMask, held);
  monthRows(hubLook(seen(v5, held), p5, nullptr), RowKind::Open, "view 5 after view 1");
  TEST_ASSERT_EQUAL(3, trayOf(seen(v5, held), nullptr).cells);
  TEST_ASSERT_EQUAL(Look::Pending, shelfLook(finds, seen(v5, held), p5, nullptr).page.look);
}

// Recently played's shelf at a new minute (7.10): the first row on screen
// whose "25 min ago" moved.
void test_recently_played_at_a_new_minute() {
  namespace st = statstext;
  Snap full;
  put(full, fullCard());  // rows 25 min, then every 97 min before now (21:40 on day 313)
  const sv::Snapshot& s = *full.s();
  const uint32_t now = dated().nowMin;
  TEST_ASSERT_TRUE(recentAt(s, 0).heading);
  // A minute later: row 0's "25 min ago" moves (list row 1, under Today).
  TEST_ASSERT_EQUAL_UINT32(1, st::recentMoved(s, 0, 4, now - 1, now));
  // Below it, hours: "2 h ago" holds for a minute; 3 minutes back crosses
  // 2 h (122 min ago now, 119 then).
  TEST_ASSERT_EQUAL_UINT32(st::kNoneMoved, st::recentMoved(s, 2, 5, now - 1, now));
  TEST_ASSERT_EQUAL_UINT32(2, st::recentMoved(s, 2, 5, now - 3, now));
  // Nothing moved: the same minute; an undated snapshot (no times).
  TEST_ASSERT_EQUAL_UINT32(st::kNoneMoved, st::recentMoved(s, 0, 4, now, now));
  MadeUpView v3;
  TEST_ASSERT_TRUE(madeUpView(3, kStamp, kTracks, kAlbums, &v3));
  Snap u;
  put(u, v3.spec);
  TEST_ASSERT_EQUAL_UINT32(st::kNoneMoved, st::recentMoved(*u.s(), 0, 5, now - 30, now));
  // Past the list: none.
  TEST_ASSERT_EQUAL_UINT32(st::kNoneMoved, st::recentMoved(s, 500, 504, now - 1, now));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_states_of_the_lifetime);
  RUN_TEST(test_the_lifetime_from_ram);
  RUN_TEST(test_not_played_open_s_three_inputs);
  RUN_TEST(test_the_page_look_by_phase);
  RUN_TEST(test_the_rising_edge_s_asks);
  RUN_TEST(test_on_stats_pages_over_stacks);
  RUN_TEST(test_the_entry_row_and_its_line);
  RUN_TEST(test_hub_rows_in_each_state);
  RUN_TEST(test_the_phase_lines_on_every_page);
  RUN_TEST(test_the_try_again_row_s_place);
  RUN_TEST(test_the_card_back_on_every_page);
  RUN_TEST(test_the_card_back_after_a_rise_with_the_card_out);
  RUN_TEST(test_the_shelves);
  RUN_TEST(test_the_tray_by_days_and_its_hits);
  RUN_TEST(test_recently_played_s_sections);
  RUN_TEST(test_week_from_a_made_up_snapshot);
  RUN_TEST(test_the_made_up_views);
  RUN_TEST(test_the_made_up_views_with_no_play_log);
  RUN_TEST(test_the_made_up_views_as_the_ui_takes_them);
  RUN_TEST(test_a_view_s_write_folds_the_shelves_bars);
  RUN_TEST(test_the_forms_of_the_lines);
  RUN_TEST(test_a_ranked_row_keeps_its_count);
  RUN_TEST(test_relative_times);
  RUN_TEST(test_day_labels);
  RUN_TEST(test_the_week_texts);
  RUN_TEST(test_week_as_the_page_draws_it);
  RUN_TEST(test_an_undated_snapshot_once_the_clock_is_set);
  RUN_TEST(test_the_stat_value_s_segments);
  RUN_TEST(test_the_hub_rows_take_a_touch);
  RUN_TEST(test_a_hub_row_s_tap);
  RUN_TEST(test_the_hub_s_sub_reach_and_shape);
  RUN_TEST(test_the_hub_s_lines);
  RUN_TEST(test_a_new_minute);
  RUN_TEST(test_the_entry_row_at_the_fold_s_end);
  RUN_TEST(test_a_shelf_s_rows);
  RUN_TEST(test_a_shelf_s_touch_place_and_shape);
  RUN_TEST(test_most_played_s_window_and_the_tray_alone);
  RUN_TEST(test_the_uis_checksum_in_one_walk);
  RUN_TEST(test_the_counting_days_a_page_holds);
  RUN_TEST(test_recently_played_at_a_new_minute);
  return UNITY_END();
}
