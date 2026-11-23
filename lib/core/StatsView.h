// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "NavModel.h"
#include "StatsFold.h"
#include "StatsRank.h"
#include "StatsVisit.h"
#include "UiStrings.h"

// The listening stats' screens as numbers and kinds (docs/LISTENING-STATS.md
// 4.4, 7.1-7.7, 7.11; S3b's C17a): what each page shows in each state and
// phase, decided from RAM (the lifetime, the phase) and the visit's
// snapshot, never from the card. Portable, host-tested (test_ui_stats);
// src/ui/LibraryStats draws from it, lib/core StatsText (S6) words its
// lines.
//   - The lifetime and the phase, as the host hands them (UiHost's
//     statsLifetime(), statsPhase(); the lifetime filled by lifetimeOf()
//     from HEAD and the clock), and 4.4's state from the lifetime.
//   - The look of a page from its phase (4.4 as decided): the snapshot's
//     figures, the phase's line (Working, Waiting), a Try again row with
//     its reason (No room, Unread, Capped, and the card back: the owner's
//     call 3, stats.card_back), the No card line alone while the card is
//     out, Off (the stats off: no data lines, no Try again), the empty state
//     (nothing played). Idle with nothing served never reads "Couldn't read
//     the stats" (that is for read faults only): with the stats off it is
//     Off, with them on (a Cancelled ask) the Working line, one ask due.
//   - The entry row (Library > Artists, item 0: 7.3): whether it is there,
//     its line (the lifetime one, "74 h 10 min since 1 Sep", when it fits
//     204 px and the first day is set, else the time alone: the owner's call
//     1; no plays), and its fixed bars, kEntryBars.
//   - The hub (7.4): its six rows by state, grade and phase (open, disabled
//     with a reason and a toast, the status row with no clock), and the Try
//     again row after them; the empty state with nothing played; how each
//     row takes a touch, its title, the header's line, and the shape and
//     texts a page polls to know it must draw again (S9).
//   - The shelves (7.6): ids, the tray by counting days and the rank's RCNT
//     mark (rcntShort()), trayCellAt() with filtergrid::skew(), the kept
//     window's fallback, Not played yet's own state (its bits, its lean
//     ask), and Recently played's sections (the map between the list's rows
//     and the snapshot's).
//   - Last 7 days (7.7): its look, and its figures from the snapshot's 7
//     days (the total, the plays, the bars to 44 px, the big day, Auto DJ's
//     rate).
//   - The held condition (onStatsPages(): 7.1, 9.7's "The snapshot's
//     life") and the asks its rising edge makes (none for Off, No card,
//     Empty or a made-up view).
//   - The made-up views 0-9 (9.3's uiSx<n>): each one's lifetime and
//     phase, and the made-up snapshot it writes (statsvisit::writeMadeUp()).
// No allocation, no state: a draw or an act reads it anew.
namespace statsview {

using statsvisit::Phase;
using statsvisit::Outcome;
using statsvisit::Snapshot;
using statsvisit::UiAsk;
using uistr::S;

// ---------------------------------------------------------------------------
// The pages' kinds (ui::PageKind's StatsHub, StatsShelf, StatsWeek, after
// AutoDj = 14: 7.1). lib/core can't name ui::PageKind; src/ui/Ui.cpp
// static_asserts these against src/ui/Page.h once it has them (S9).
// ---------------------------------------------------------------------------
constexpr uint8_t kKindHub = 15, kKindShelf = 16, kKindWeek = 17;
constexpr bool statsKind(uint8_t kind) { return kind == kKindHub || kind == kKindShelf || kind == kKindWeek; }

// ---------------------------------------------------------------------------
// What the host hands the pages (UiHost: RAM, µs)
// ---------------------------------------------------------------------------
// HEAD and the clock (S1's, in RAM: 4.2 as decided). countingDays is
// statsvisit::countingDaysOf(dated, today, sinceDay); nowMin, with dated,
// minutes since 2026-01-01 (Recently played's `when`'s, the relative times').
struct Lifetime {
  // The stats on this session: StatsGlue::on() || rereadPending(), as
  // StatsLists' own off test (its loop()'s release and ask()'s drop read
  // !on() && !rereadPending()). A remount's re-read waiting (on() false, HEAD
  // cleared until the scan's next reserve()) is on, and folding (booting()),
  // so the card back reads Try again, not Off. Else Off.
  bool on = false;
  bool folding = false;  // the boot's fold under way (StatsGlue::booting(): a re-read's among it): HEAD not whole
  bool cardIn = true;    // the card in and not held: else No card
  bool dated = false;    // the board's clock set (statsfold::todayOf())
  uint16_t today = statsfold::kUnsetDay;     // with dated
  uint16_t sinceDay = statsfold::kUnsetDay;  // Fold::sinceDayNow()
  uint32_t countingDays = 0;                 // 0: not known (no clock, no first day)
  uint64_t plays = 0;       // HEAD's (this session's live folds among them)
  uint64_t listenedMs = 0;  // HEAD's
  uint32_t nowMin = 0;      // with dated
  // A made-up view's (madeUpView(): 9.3's uiSx<n>, look only; never the
  // host's): nothing asks (D16), so no look says an ask is due and the
  // rising edge asks none. Its Try again rows still draw; their taps, as
  // every act under a made-up view, are refused by the pages (S9, S12: the
  // look-only toast, as uil<n>'s).
  bool madeUp = false;
};
// StatsLists' phase, why, outcome and version (statsPhase()).
struct PhaseInfo {
  Phase phase = Phase::Idle;
  uint8_t why = 0;
  Outcome outcome = Outcome::None;
  uint16_t version = 0;
};
// The host's lifetime (UiHost::statsLifetime(): app/ListeningStats and
// app/PlayHistory, S3b's S7), from RAM, read at each page's update() for the
// minute's check, so a few loads and a division:
//   - `on`, `folding` and `cardIn` as Lifetime has them (the caller's:
//     StatsGlue::on() || rereadPending(), booting(), and neither main.cpp's
//     cardHeld nor PlayHistory::cardAway());
//   - `head`: Fold::head(), the session's live folds in it (4.2: Empty is
//     its plays 0 once the fold is done); its plays and time;
//   - `sinceDay`: Fold::sinceDayNow(nowMs);
//   - `nowMs`: the board's clock (boardclock::now(): 0 while unset). Dated
//     when believed (statsfold::todayOf(): 2026-01-01 to 2099-12-31), then
//     today, the counting days (statsvisit::countingDaysOf()) and nowMin
//     (its minute since 2026-01-01, the one a snapshot row's `when` gives a
//     play started now); undated, today kUnsetDay and both 0.
// madeUp stays false: the host's is never a made-up view's.
Lifetime lifetimeOf(bool on, bool folding, bool cardIn, const statsfold::Head& head, uint16_t sinceDay,
                    uint64_t nowMs);

// 4.4's states, from RAM, in this order: the stats off; the card away or
// held; the boot's fold under way (HEAD partial, so before Empty); nothing
// played (HEAD's plays 0 once the fold is done); the clock unset; under 30
// counting days; else Ready. (Updating and the synthetic library are the
// pages', below.)
enum class State : uint8_t { Off, NoCard, Folding, Empty, NoDates, FirstMonth, Ready };
State stateOf(const Lifetime& l);
const char* stateName(State s);  // "off", "no card", "folding", "empty", "no dates", "first month", "ready"
// The day a first-month row opens (7.4's "Opens 6 Dec"): sinceDay + 29 (the
// day the counting days reach 30), else today + 29; kUnsetDay with neither.
uint16_t opensDay(const Lifetime& l);

// O5 (3.1, call 4): Not played yet's shelf is open when, dated, the counting
// days reach 30; with no clock, when DAYS' span does (0 before the ask has
// landed). statsvisit's rule, so the visit's grade agrees.
constexpr bool notPlayedOpen(uint32_t countingDays, bool dated, uint32_t datedSpanDays) {
  return statsvisit::notPlayedDays(dated ? countingDays : datedSpanDays);
}

// ---------------------------------------------------------------------------
// The look: what a stats page draws for its data (4.4 as decided)
// ---------------------------------------------------------------------------
enum class Look : uint8_t {
  Served,   // the snapshot's figures (its lists landed and held)
  Pending,  // the phase's line: Working "Counting plays…", Waiting "Waiting for the card…"
  Retry,    // a Try again row (Week: the empty state's button), its line the reason
  NoCard,   // the card out: "No card: plays can't be counted" alone; no Try again, no ask
  Off,      // the stats off: no data lines, no Try again (D9 as replaced)
  NoPlays,  // nothing played (4.4's Empty): the hub's empty state (7.5); nothing asked
  Closed,   // a shelf not open (no clock, the first month): its reason alone
};
const char* lookName(Look k);
struct PageLook {
  Look look = Look::Off;
  // Pending's line, Retry's reason, NoCard's and Closed's line (a served
  // shelf with none landed: stats.shelf_empty). Off and NoPlays draw no line
  // at all: `line` is then the default and means nothing (ShelfLook's
  // drawLine says when a shelf draws it).
  S line = S::stats_counting;
  // Idle with the stats on (a Cancelled ask): one visit's ask due. Never
  // under a made-up view (Lifetime::madeUp): one whose snapshot wasn't
  // written (the Dance tab up, past kBound, refused) reads Pending with
  // nothing due, and uiSx says why (S12).
  bool askDue = false;
};
// The page's look (served: the snapshot landed and held, StatsLists::
// snapshot() non-null), in this order: the stats off: Off; the card out:
// NoCard; nothing played: NoPlays; served: Served; then by the phase:
// Working, Waiting: Pending; No room, Unread, Capped (stats.waiting): Retry;
// No card with the card in again: Retry, stats.card_back (the owner's call
// 3); Idle (and Ready, which needs a snapshot served): Pending's Working
// line, askDue (not under a made-up view).
PageLook pageLook(const Lifetime& l, const PhaseInfo& p, bool served);
// The phases' lines (7.11): Working stats.counting, Waiting and Capped
// stats.waiting, No room stats.no_room, Unread stats.unread, No card
// stats.no_card; Idle and Ready stats.counting.
S phaseLine(Phase p);

// The held condition's rising edge (D1, D3): asks a visit unless the stats
// are off, the card is out (ask() would only drop it: No card), nothing is
// played (Empty asks nothing), or a made-up view is on (l.madeUp: look
// only, D16).
bool asksOnEdge(const Lifetime& l);

// ---------------------------------------------------------------------------
// The held condition (7.1, 9.7): on the stats pages
// ---------------------------------------------------------------------------
// The Library tab shown, its stack holding a hub, shelf or Week (an album,
// artist page, bar or sheet above one counts), the screen lit, the UI not
// suspended (D2), and the library not uil's synthetic one. The snapshot is
// held exactly while it is true: its edges ask and release.
bool onStatsPages(const NavModel& nav, bool dark, bool suspended, bool synthetic);

// ---------------------------------------------------------------------------
// The entry row (7.3)
// ---------------------------------------------------------------------------
constexpr int kEntryRoom = 204;  // its line's room, Small (x 54 to the chevron)
// The fixed bars (the owner's call 2: an icon, not data): 4 px wide on a 5
// px step from x 9, standing on y 32, 24 px the top; past DIM, the last
// violet. The Entry frame's heights.
constexpr int kEntryBarCount = 7;
inline constexpr uint8_t kEntryBars[kEntryBarCount] = {10, 12, 7, 24, 19, 11, 8};
constexpr int kEntryBarX = 9, kEntryBarStep = 5, kEntryBarW = 4, kEntryBarBase = 32;
// Whether the row is there, decided at enter() (adding or removing it moves
// every artist by 42 px): absent for Off and No card, behind the update's
// fence (no index), for the synthetic library (uil<n>), and with no artists
// (D18: under a top row the empty state's buttons can't be pressed).
bool entryDue(const Lifetime& l, bool indexReady, bool synthetic, uint32_t artists);
enum class EntryLine : uint8_t {
  Counting,  // Folding: stats.counting
  Empty,     // nothing played: stats.entry_empty
  Since,     // the lifetime line, stats.since: "74 h 10 min since 1 Sep"
  Time,      // the time alone (wider than kEntryRoom, or no first day)
  Unread,    // the stats off since the row was latched: stats.unread (A20)
};
// A text's width in px (the firmware's Small font; the host's tests' VLW).
using WidthFn = int (*)(const char* text, void* ctx);
// The row's line into `out` and its kind: no plays (call 1); the lifetime
// line when the first day is set and it fits kEntryRoom by `width`, else
// the time alone. With the stats off, stats.unread: never at enter() (the
// row is absent then: entryDue()), only for a row latched while the boot's
// fold ran that ended with the stats off (stats.bin or its base unread, a
// newer major, no memory for the fold: A20). The row stays, since taking
// it away would move every artist by 42 px.
EntryLine entryLine(const Lifetime& l, WidthFn width, void* ctx, char* out, size_t size);
// The latched row's line is drawn again in place at the boot fold's end
// (7.3, 7.10: refreshTopRow), and only then: drawn while folding (Counting)
// and the fold over now. A later fold doesn't move it (its line is as of
// enter()).
constexpr bool entryFoldEnded(EntryLine drawn, bool folding) { return drawn == EntryLine::Counting && !folding; }
// "74 h 10 min" (format.hours_minutes, the hours grouped, the minutes
// unpadded); under an hour "45 min" (format.minutes): 4.1's times.
size_t timeText(uint64_t ms, char* out, size_t size);
// A UTC day (0: 2026-01-01) as format.day_month: "1 Sep".
size_t dayMonthText(uint16_t day, char* out, size_t size);

// ---------------------------------------------------------------------------
// The hub (7.4)
// ---------------------------------------------------------------------------
enum class HubRow : uint8_t { Week, MostPlayed, NewFinds, OldFavourites, NotPlayed, Recent, TryAgain };
constexpr uint32_t kHubRows = 6;    // and Try again's, at index 6
enum class RowKind : uint8_t {
  Open,      // pressable: a tick and its page
  Disabled,  // inert: icon FAINT, its reason; a tap the double buzz and `toast`
  Status,    // no clock, row 1's place: stats.no_dates over stats.clock_fix; no press, no buzz (D13)
  TryAgain,  // empty.try_again over its reason; pressable, the row's tick
};
enum class Line : uint8_t {
  None,       // nothing (the data lines in Retry and Off)
  Text,       // `text` as it is: the phase's line, a reason
  Data,       // the snapshot's (StatsText's forms, S6)
  SinceOnly,  // Not played yet's before a count: stats.since_only ("since 1 Sep")
  Warming,    // a first-month row: stats.warming of opensDay() ("Opens 6 Dec, after a month of plays")
  Status,     // the status row's two lines: stats.no_dates, stats.clock_fix
};
struct HubItem {
  HubRow row = HubRow::Week;
  RowKind kind = RowKind::Open;
  Line line = Line::None;
  S text = S::stats_counting;          // Line::Text's (and Warming's id)
  S toast = S::toast_stats_needs_dates;  // a disabled row's tap (toast.stats_needs_dates, _needs_month)
};
struct HubLook {
  PageLook page;
  uint32_t rows = 0;  // 0 (NoPlays: the empty state; NoCard: its one line), 6, or 7 with Try again
  HubItem item[kHubRows + 1];
};
// Row kinds by state (RAM, drawn at once: 7.4 as decided), lines by the look:
//   - Last 7 days: the status row with no clock, else open;
//   - Most played and Recently played: open;
//   - New finds and Old favourites: disabled with no clock (stats.
//     needs_dates) or under 30 counting days (stats.warming), else open;
//   - Not played yet: by notPlayedOpen(): dated, open from 30 counting days,
//     else disabled with stats.warming; with no clock pending (open) until
//     the snapshot's DAYS span is known, then open or disabled with
//     stats.needs_dates (D14). Its line, served, the count only after a
//     Full or lean landing (hasNotPlayed()), else stats.since_only;
//   - open rows' lines: Served the snapshot's, Pending the phase's line,
//     else none. So in Folding (Pending, the boot's Working) the open rows
//     read Counting plays… while the disabled and status rows keep their
//     RAM reasons (Opens 6 Dec, Needs dates, the status row's two lines);
//   - Retry: a Try again row at index 6 (also with the status row in row 1).
HubLook hubLook(const Lifetime& l, const PhaseInfo& p, const Snapshot* snap);
// A hub row's title (7.4): its name (stats.last7 ... stats.recent); the
// status row's stats.no_dates (Small AMBER, over stats.clock_fix); Try
// again's empty.try_again.
S hubTitle(const HubItem& it);
// How a hub row takes a touch (7.4, 4.5, D13: ListView::Source's
// pressable(), inert() and holds(), a table by its kind):
//   - Open: pressable, a tick and its page; no hold (a long press ends as a
//     tap);
//   - Disabled: inert, no press look and no tick; a tap with the list still
//     at the Down gives the double buzz and the row's toast (a touch that
//     stopped a moving list does nothing: ListView's);
//   - Status: nothing at all, no press, no buzz (D13, as Pages.h's
//     Output > About's rows of text);
//   - TryAgain: pressable, the row's tick and the ask; no hold.
struct Control {
  bool pressable = false;
  bool inert = false;
  bool holds = false;
};
Control controlOf(RowKind k);
// The hub's header line under its title (7.4: the lifetime one, RAM): none
// while the boot's fold runs (A19: HEAD partial), with nothing played (7.5:
// the title alone) or with the stats off.
bool hubSubDue(const Lifetime& l);
// The hub can be shown (4.4: not with the stats off, behind the update's
// fence, nor over uil's synthetic library): console uiSh's refusal. (The
// card out still shows it, its No card line.)
bool hubReachable(const Lifetime& l, bool indexReady, bool synthetic);
// What the hub draws, as a byte: its look (which gives its row count: 0, 6,
// or 7 with Try again; and its open rows' line kind) and each row's kind
// and line kind as the lifetime makes them (the status row, rows 3-4
// disabled, row 5 open, since-only or disabled). Two looks with the same
// shape differ only in their texts (the phase's line, a reason, the data),
// which move with the version (StatsLists' news). A page polls it each pass
// (RAM): a change no news brings (the card back: No card's line to Try
// again; a play counted on the empty hub; the stats off; the clock set; a
// day that opens rows 3-5) restages it, so no row drawn later disagrees
// with those on screen.
uint8_t hubShape(const HubLook& h);
// ... and the texts a look draws, as a u16, exact: the look (3 bits) and
// its line's id (Pending's phase line, a reason, the No card line, a
// closed shelf's), or, served, the snapshot's version (its ask's: a new
// ask's rows and lines). Off and NoPlays draw no line: their look alone.
// With the shape it says whether a page's picture moved: the stats' news
// (a phase, a landing, a release) restages a page only when one of the two
// did, so the landing's two version moves (the landing, then Ready) draw
// it once. (The snapshot's lines read RAM's lifetime too: the minute's own
// rule redraws row 6; the rest moves with the shape or at the next enter().)
uint16_t lookText(const PageLook& k, const Snapshot* snap);
// The full minute a row was drawn at from its low 16 bits (a page keeps
// those: LibraryPage's u16), at or before `nowMin`: exact while under 65,536
// minutes (45 days) have passed, as each pass's look does.
uint32_t minuteFrom(uint16_t low, uint32_t nowMin);

// ---------------------------------------------------------------------------
// The shelves (7.6)
// ---------------------------------------------------------------------------
enum class Shelf : uint8_t { MostPlayed, NewFinds, OldFavourites, NotPlayed, Recent };
constexpr uint32_t kShelves = 5;
// PageRef.id: shelf * 4 + window (windows: statsvisit::k7Days, k30Days,
// kAllTime; Most played's alone).
constexpr uint32_t shelfId(Shelf s, uint8_t window = statsvisit::kAllTime) {
  return static_cast<uint32_t>(s) * 4u + (s == Shelf::MostPlayed ? window : 0u);
}
Shelf shelfOf(uint32_t id);    // past the five: Recently played
uint8_t windowOf(uint32_t id);  // Most played's (past All time: All time); the others' kAllTime
// The shelf a hub row pushes: false for Last 7 days (Week, a page of its
// own) and Try again.
bool shelfOfRow(HubRow r, Shelf* out);
// The snapshot's list for a shelf (Not played yet: none, statsrank::kLists).
statsrank::List listOf(Shelf s, uint8_t window);
// A shelf's title (its header, 7.6): its hub row's.
S shelfTitle(Shelf s);

// The tray (Most played's lead item: 7.6, 7.9). Its cells, in a 300 x 28
// CARD tray at (6, 7), 2 px in from its ends and 4 px apart: three of 96 px
// at x 8, 108, 208, or two of 146 at x 8, 158; labels in Bold, else Small,
// in the cell less 4. (7.6's two of 148 at x 8, 160 end at x 307, 2 px past
// the tray's last pixel, 305: its 2 x 148 + 4 is the tray's whole width, not
// its inside. The label room is 142 there, not 144; every label fits the
// three cells' 92 first, so the catalogue's 144 never binds.)
struct Tray {
  uint8_t cells = 0;              // 0 (no tray), 2 or 3
  uint8_t window[3] = {};         // each cell's window
  uint8_t days30 = 30;            // the 30-day cell's days: fewer when RCNT is short ("16 days")
};
constexpr int kTrayX = 6, kTrayY = 7, kTrayW = 300, kTrayH = 28, kTrayR = 10;
constexpr int kTrayPlateY = 9, kTrayPlateH = 24, kTrayPlateR = 8, kTrayLabelY = 21;
constexpr int kTrayCellW3 = 96, kTrayCellW2 = 146;
inline constexpr int kTrayCellX3[3] = {8, 108, 208};
inline constexpr int kTrayCellX2[2] = {8, 158};
// By the counting days (dated): under 7, or no clock, none; 7-29: 7 days
// and All time; 30 on: 7 days, 30 days and All time. The 30-day cell reads
// the days RCNT holds (today - rcntFirstDay + 1) only when the snapshot
// served says RCNT is short (rcntShort(): 5.2's cap, whole days gone, past
// 4,096 rows), else 30: a window whose first days had no plays (a listener
// away) holds all its plays, so it reads 30.
Tray trayOf(const Lifetime& l, const Snapshot* snap);
// A cell's left edge and width.
int trayCellX(const Tray& t, int cell);
int trayCellW(const Tray& t);
// The cell a reading at x is (-1: no tray): each split at its gap's middle
// plus filtergrid::skew() (7.9: x 0-105, 106-218, 219-319; two cells at
// 156 + 6 = 162, 7.9's 164 being its 158 + 6 of the cells above); a reading
// clamped at the right edge is the last cell.
int trayCellAt(const Tray& t, int x, bool rightEdge);
// The window Most played shows: the kept one (its PageRef.id) when the tray
// offers it, else All time (7.6).
uint8_t windowShown(uint8_t kept, const Tray& t);

struct ShelfLook {
  PageLook page;           // the look (Closed and Not played yet's own state among them)
  Shelf shelf = Shelf::MostPlayed;
  uint8_t window = statsvisit::kAllTime;  // Most played's shown
  statsrank::List list = statsrank::kMostAll;  // the snapshot's (Not played yet: kLists)
  bool tray = false;       // the lead TopRow (Most played with a tray; none in Off, NoCard, NoPlays)
  bool topBar = false;     // the container's bar: rows landed (none on Not played yet, nor on Try again)
  bool tryAgain = false;   // row 0 is Try again (first under the tray: 7.6)
  UiAsk ask = UiAsk::Visit;  // what Try again asks (Not played yet with no bits: its lean ask)
  uint32_t rows = 0;       // the list's rows (Recently played's with its sections; Not played
                           // yet's albums); 1 for Try again; 0 for a line or nothing
  // `page.line` drawn under the lead items (rows 0): Pending, NoCard,
  // Closed, a served shelf with none landed. Off and NoPlays draw none.
  bool drawLine = false;
};
// A shelf's look:
//   - Off, NoPlays: 0 rows, no line, no tray (D9 as replaced: no data);
//   - NoCard: 0 rows, the No card line alone, no tray (the owner's call 3:
//     while the card is out the page shows only that line);
//   - Closed (RAM): New finds and Old favourites with no clock (stats.
//     needs_dates) or under 30 counting days (stats.warming), Not played
//     yet dated under 30 days (stats.warming), or served with no clock and
//     DAYS' span under 30 (stats.needs_dates: D14); stats.warming's line is
//     statstext::warmingLine() of opensDay() in Body kListLineW (its day
//     filled, its short form where the long one is wider);
//   - Served: the snapshot's rows, the top bar with any; none landed:
//     stats.shelf_empty (D12). Not played yet with no bits (the visit took
//     Lists, or its notPlayed() was refused): its own state by the phase,
//     Working and Waiting its lean ask's line, Unread and Capped their
//     reasons, else stats.no_room, Try again its lean ask (7.6). Opening it
//     asks nothing by itself (7.6), so a snapshot asked before the month
//     (Lists, no Full asked) and held across the UTC midnight that opens
//     the shelf reads No room too, its Try again the lean ask;
//   - Pending: 0 rows, the phase's line;
//   - Retry: row 0 Try again (no top bar), its reason.
// Most played's tray (RAM) is there in every other look, Retry and Closed
// among them.
ShelfLook shelfLook(uint32_t id, const Lifetime& l, const PhaseInfo& p, const Snapshot* snap);

// Recently played's list (7.6, D10): section rows, then the snapshot's rows
// under them, in the snapshot's order.
//   - Dated snapshot: a row's UTC day of `when` against the snapshot's
//     today(): Today, Yesterday, else the day (format.weekday_date); its
//     time relative to now. Undated rows (kWhenSession, kWhenEarlier) go
//     last, under one Earlier.
//   - Undated snapshot: This session (kWhenSession), then Earlier
//     (kWhenEarlier), no times. A row with a dated lastAt (the rank puts
//     dated rows first: 4.3) is under its day, with no Today or Yesterday
//     (no today) and no time. That is also a track replayed this session
//     with no clock: an undated play leaves lastAt (5.9), and the copy-out
//     codes kWhenSession only for lastAt 0, so it shows under its old day,
//     not This session (an owner's call: S3b's review of S5).
enum class Section : uint8_t { Today, Yesterday, Day, Session, Earlier };
struct RecentItem {
  bool heading = false;
  Section section = Section::Today;  // its section (a heading's, a row's)
  uint16_t day = statsfold::kUnsetDay;  // Day's UTC day
  uint32_t row = 0;    // a row: its place in the snapshot's list; a heading: the row after it
  bool timed = false;  // a row's "25 min ago" is due (dated snapshot, a dated row)
};
uint32_t recentCount(const Snapshot& s);  // the list's rows: the snapshot's and the headings
RecentItem recentAt(const Snapshot& s, uint32_t listRow);  // past the end: a heading-less default
uint32_t recentListRow(const Snapshot& s, uint32_t snapRow);  // the list row of a snapshot row

// ---------------------------------------------------------------------------
// Last 7 days (7.7)
// ---------------------------------------------------------------------------
constexpr int kWeekBarPx = 44;  // the top day
struct WeekLook {
  PageLook page;        // Served: the figures; Pending: A's value the line, B and C empty; Retry: the empty
                        // state, its line and Try again's button; NoCard, Closed: the empty state, its line
                        // alone; Off, NoPlays: no figures, no line
  bool button = false;  // the empty state's Try again
};
// As the hub's look; with no clock Closed (stats.needs_dates: never reached,
// the hub's row 1 is the status row then).
WeekLook weekLook(const Lifetime& l, const PhaseInfo& p, const Snapshot* snap);
struct WeekFigures {
  uint64_t listenedMs = 0;  // the 7 days'
  uint32_t plays = 0;
  uint16_t firstDay = statsfold::kUnsetDay;  // the window's first (today - 6); kUnsetDay undated
  uint8_t bar[statsrank::kWeekDays] = {};    // px, oldest first (0: no bar: before sinceDay, or nothing)
  int8_t bigDay = -1;       // the top day by time (a tie: the later), -1 none (nothing in the week)
  bool autoDj = false;      // Auto DJ's line: its plays + skips at least 5 (D11)
  uint8_t autoDjPct = 0;    // its plays / (plays + skips), rounded
};
// From the snapshot's 7 days (as of the ask: no fold moves it). Bars to
// `maxPx` (the top day full; any other day with time at least 1 px).
WeekFigures weekFigures(const Snapshot& s, int maxPx = kWeekBarPx);
// Bars to `maxPx` for seven days' times (`firstDay` the first's UTC day,
// none before `sinceDay`; kUnsetDay firstDay: none at all).
void barHeights(const uint32_t ms[statsrank::kWeekDays], uint16_t firstDay, uint16_t sinceDay, int maxPx,
                uint8_t out[statsrank::kWeekDays]);

// ---------------------------------------------------------------------------
// The made-up views (9.3's uiSx<n>, look only; S12 wires them): 1 a full
// card, 2 nothing played, 3 no clock, 4 day 5 of counting, 5 folding, 6 no
// card, 7 waiting, 8 no room, 9 unread; 0 the card's own (none).
// ---------------------------------------------------------------------------
constexpr uint8_t kMadeUpViews = 10;
struct MadeUpView {
  Lifetime life;           // its madeUp true: nothing asks
  PhaseInfo phase;
  bool snapshot = false;     // a made-up snapshot to write (1, 3 and 4): `spec`
  statsvisit::MadeUp spec;   // keyed on the live index's buildStamp, trackCount and albums
};
// View `n` (1-9) over the live index's key; false for 0 and past 9.
bool madeUpView(uint8_t n, uint64_t buildStamp, uint32_t trackCount, uint32_t albums, MadeUpView* out);
// uiSx<n>'s answer from a host with no play log (UiHost::statsMadeUp()'s
// default, app/PlayHistory's with none), which serves nothing and so writes
// no snapshot: true for 0 (the end) and the views with none (2, 5-9); false
// for those with one (1, 3, 4: not written) and past 9 (no such view). One
// rule, so the two hosts read alike.
bool madeUpNoLog(uint8_t n);

}  // namespace statsview
