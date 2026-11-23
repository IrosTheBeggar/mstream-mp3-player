// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "StatsRank.h"
#include "StatsView.h"
#include "StatsVisit.h"
#include "TextFit.h"
#include "UiStrings.h"
#include "UiText.h"

// The listening stats' words (docs/LISTENING-STATS.md 4.1-4.3, 7.3-7.7,
// 7.11, 8.2; S3b's C17b): each line the stats pages draw, filled from the
// catalogue in the UI's language, with its short form where the catalogue
// has one and the room says so. StatsView decides what a page shows; this
// says it. Portable, host-tested: test_ui_stats for the forms with a made-up
// font, test_ui_library for the rooms with the firmware's fonts in the ten
// languages.
//   - The hub's lines (7.4): the header's lifetime line, Last 7 days' time
//     and plays, Most played's top three titles (cut with "…" as any
//     names), New finds' count (its short form when the long one doesn't
//     fit), Old favourites', Not played yet's albums since the first day
//     (or that day alone), Recently played's newest title and how long ago;
//     a disabled row's "Opens 6 Dec" (its short form) and the toasts.
//   - The shelves (7.6): their header lines, the tray's labels, and a
//     ranked row's line, "artist cut, count kept": the parts after the
//     artist (the plays, who found it, its last day, how long ago) are
//     measured first and kept whole, the artist cut to what they leave.
//   - Relative times (D10): by the minute, "1 min ago" under a minute,
//     "23 h ago" to a day, none from 24 h (the name alone).
//   - Day labels (8.2): a day and its month, a section's weekday and date,
//     Last 7 days' range (one month or across a month's end), the weekday
//     names; a minute's day with a time zone's offset (0 until S6, 9.10:
//     the days are UTC's, and Last 7 days says so).
//   - Last 7 days (7.7): its header line and where it goes (beside the
//     title when it fits, else under it), the big day, Auto DJ's rate, and
//     the stat value's segments ("7 h 37 min": its numbers in Title, its
//     words in Small) walked from format.hours_minutes, with its width.
// The texts go into the caller's buffer, cut at a character boundary as
// uistr's are; no allocation, no state. Its rooms are the catalogue's
// (i18n/en.json's @meta), named here once.
namespace statstext {

using uistr::S;
using statsview::dayMonthText;
using statsview::timeText;

// ---------------------------------------------------------------------------
// The rooms (px)
// ---------------------------------------------------------------------------
constexpr int kHubLineW = 232;    // a hub row's line (Small, x 54 to the chevron: 7.4)
constexpr int kReasonW = 250;     // a disabled row's reason, the status row's lines (no chevron: D13)
constexpr int kRowLineW = 264;    // a ranked row's line (Small, x 40 to the rail: 7.6)
constexpr int kHubSubW = 282;     // the hub's header line under its title (x 28 to 310: 7.4)
constexpr int kWeekLineW = 288;   // Last 7 days' lines under the bars (7.7: C)
constexpr int kTrayLabelW3 = 92;  // a tray label in its cell less 4, of three ...
constexpr int kTrayLabelW2 = 142; // ... and of two (StatsView's Tray)
constexpr int kDayNameW = 41;     // Last 7 days' column (a weekday name under its bar)
constexpr int kListenedW = 150;   // "Listening time" (7.7: A)
constexpr int kPlaysLabelW = 100; // "Plays" (7.7: A)
// A name cut beside parts that stay whole keeps at least this much, else it
// is left out (librarytext's album line's rule: a stump of a name says
// nothing). 264 px leaves it far more in every language (test_ui_library).
constexpr int kMinNameW = 32;

// ---------------------------------------------------------------------------
// Times (4.1) and relative times (D10)
// ---------------------------------------------------------------------------
// timeText(): "74 h 10 min", under an hour "45 min" (statsview's: the entry
// line's).
// How long ago a row's `whenMin` (minutes since 2026-01-01, its `when`) was,
// at `nowMin`: "25 min ago" (stats.ago_min) under an hour, a minute at
// least (0 s and 59 s read "1 min ago"); "23 h ago" (stats.ago_h, whole
// hours) under a day; none from 24 h (the name alone: 7.6, l.406), for an
// undated row (statsvisit::kWhenSession, kWhenEarlier) or one after now (a
// clock set back: no claim). Returns the bytes; 0 and "" for none.
constexpr uint32_t kDayMin = 1440;
size_t agoText(uint32_t nowMin, uint32_t whenMin, char* out, size_t size);

// ---------------------------------------------------------------------------
// Days (8.2): UTC days, 0 being 2026-01-01 (a Thursday)
// ---------------------------------------------------------------------------
// A minute's day where the clock is `tzOffsetMin` from UTC (0 in S3: the
// stats' days are UTC's until S6's time zone, 9.10). A minute before day 0
// there is day 0.
uint16_t localDay(uint32_t whenMin, int32_t tzOffsetMin);
// 0 Monday ... 6 Sunday.
uint8_t weekdayOf(uint16_t day);
// Its weekday's short name (day.mon: a column, a section) and its full one
// (day.monday: the big day).
S dayShort(uint16_t day);
S dayLong(uint16_t day);
// dayMonthText(): format.day_month, "1 Sep" (statsview's).
// A section's older day, format.weekday_date: "Sun 8 Nov".
size_t weekdayDate(uint16_t day, char* out, size_t size);
// Days `first` to `last`: in one month format.day_range_month ("4–10 Nov"),
// across a month's end (a year's too) format.day_range ("28 Sep–4 Oct").
size_t dayRange(uint16_t first, uint16_t last, char* out, size_t size);
// Recently played's heading (StatsView's sections): stats.today,
// stats.yesterday, weekdayDate() of its day, stats.this_session,
// stats.earlier.
size_t sectionText(const statsview::RecentItem& it, char* out, size_t size);

// ---------------------------------------------------------------------------
// The hub (7.4)
// ---------------------------------------------------------------------------
// The header's line, the lifetime one: stats.since of the time and the first
// day ("74 h 10 min since 1 Sep"); with no first day the time alone (the
// header goes under the title when it doesn't fit beside: subBelow, 282 px).
size_t hubSub(uint64_t listenedMs, uint16_t sinceDay, char* out, size_t size);
// Last 7 days: stats.time_plays, "7 h 37 min · 118 plays".
size_t weekLine(uint64_t listenedMs, uint32_t plays, char* out, size_t size);
// Most played: the All time top three's titles (the first `n` of `titles`,
// at most 3), " · " between them, cut with "…" to `maxW` px of `f` as any
// names are (4.2), and to `size` (the ellipsis kept). "" with none.
size_t topThreeLine(const textfit::Font& f, int maxW, const char* const* titles, uint32_t n, char* out,
                    size_t size);
// The count New finds' and Old favourites' lines give (rows 3-4): the
// snapshot's total() (4.2 as decided: every row offered, less the orphans
// dropped), at most the shelf's cap (statsrank::kCaps: 50, 200), as the
// catalogue words and measures them ("the shelf's, up to 50": i18n/en.json's
// worst). Past it the line overflows ("1000 tracks, played often, not
// lately" is 236 px in English's 232), and the shelf shows its cap anyway.
uint32_t hubCount(const statsvisit::Snapshot& s, statsrank::List l);
// New finds: stats.new_finds_sub ("41 tracks first heard in 30 days") when
// it fits `maxW` px of `f`, else stats.new_finds_sub_short ("41 new in 30
// days"). `n`: hubCount().
size_t newFindsLine(const textfit::Font& f, int maxW, uint32_t n, char* out, size_t size);
// Old favourites: stats.old_favourites_sub ("38 tracks, played often, not
// lately").
size_t oldFavouritesLine(uint32_t n, char* out, size_t size);
// Not played yet: stats.since of library.albums_n and the first day ("612
// albums since 1 Sep"); with no first day the albums alone.
size_t notPlayedLine(uint32_t albums, uint16_t sinceDay, char* out, size_t size);
// Not played yet before its count (StatsView's Line::SinceOnly): stats.
// since_only, "since 1 Sep"; "" with no first day.
size_t sinceOnlyLine(uint16_t sinceDay, char* out, size_t size);
// Recently played: the newest track's title and how long ago ("Slow Lantern
// · 25 min ago"), the title cut to what the time leaves; the title alone
// with no time (`timed` false: undated; or agoText() none).
size_t recentLine(const textfit::Font& f, int maxW, const char* title, bool timed, uint32_t nowMin,
                  uint32_t whenMin, char* out, size_t size);
// A row disabled in the first month (StatsView's Line::Warming): stats.
// warming of the day it opens ("Opens 6 Dec, after a month of plays") when
// it fits `maxW` px of `f`, else stats.warming_short ("Opens on 6 Dec").
size_t warmingLine(const textfit::Font& f, int maxW, uint16_t opensDay, char* out, size_t size);
// A disabled row's toast (4.5; StatsView's HubItem::toast):
// toast.stats_needs_dates ("Needs dates: Plug in to the MP3 Player tab"),
// toast.stats_needs_month ("Needs a month of plays: Opens on 6 Dec").
size_t toastText(S toast, uint16_t opensDay, char* out, size_t size);
// The empty state's first line (7.5): stats.empty_rule when it fits `maxW`
// px of `f` (its room, uitext::kEmptyLineW), else stats.empty_rule_short.
S emptyRule(const textfit::Font& f, int maxW);

// ---------------------------------------------------------------------------
// The shelves (7.6)
// ---------------------------------------------------------------------------
// A shelf's header line under its title (subBelow): Most played's window
// (stats.sub_last7, stats.sub_last30, All time stats.since_only of the
// first day), New finds' stats.first_heard, Old favourites' stats.not_in_30,
// Not played yet's stats.since_only; none for Recently played (nor a since
// with no first day).
size_t shelfSub(statsview::Shelf shelf, uint8_t window, uint16_t sinceDay, char* out, size_t size);
// A tray cell's label: stats.days_n of 7, of `days30` for the 30 days (fewer
// when RCNT is short: "16 days"), stats.window_all. Bold in its cell's room
// (kTrayLabelW3, kTrayLabelW2), else Small (the drawing's choice).
size_t trayLabel(uint8_t window, uint8_t days30, char* out, size_t size);
// The parts after the name, whole, and the name cut to what they leave in
// `maxW` px of `f` and in `size`'s bytes, ending with "…" whichever cut it
// ("The Lantern C… · 12 plays"): the parts alone when the name is empty,
// would keep less than kMinNameW px, or `size` hasn't the parts' bytes and
// five more; the name alone, cut, with no parts.
size_t nameAndParts(const textfit::Font& f, int maxW, const char* name, const char* parts, char* out, size_t size);
// A ranked row's line (its title is the track's, drawn above it) for the
// snapshot's `list`: the artist, then
//   - Most played (each window): its plays in the window ("Lumen Ferry · 12
//     plays");
//   - New finds: who found it, then its plays ("Sefa Ilmar · found by Auto
//     DJ · 9 plays"; "found by shuffle"; nothing when the listener picked
//     it);
//   - Old favourites: its plays and its last day ("Halvard Moss · 9 plays ·
//     last 14 Sep");
//     (each of these two: the plays alone when the pair would leave the
//     artist less than kMinNameW px: "Sefa Ilmar · 22 Wiedergaben");
//   - Recently played: how long ago, when `timed` (StatsView's RecentItem:
//     a dated row of a dated snapshot) and under a day ("Lumen Ferry · 25
//     min ago"), else the artist alone;
// cut by nameAndParts() to `maxW` (kRowLineW).
size_t rowLine(const textfit::Font& f, int maxW, statsrank::List list, const statsvisit::Row& row,
               const char* artist, bool timed, uint32_t nowMin, char* out, size_t size);

// ---------------------------------------------------------------------------
// Last 7 days (7.7)
// ---------------------------------------------------------------------------
// The header's line: stats.utc of the seven days from `firstDay`, "4–10
// Nov (UTC)" (the days are UTC's until S6).
size_t weekSub(uint16_t firstDay, char* out, size_t size);
// Where it goes (as ui's headers place a line that may go under the title:
// uitext::headerSub()): beside a title `titleW` px wide (Bold, from x 28),
// kHeaderSubGap after it, when it fits whole up to `right` (the header's,
// before the crumb: x 207 beside English's, so x 140 after "Last 7 days");
// else under the title, from x 28.
constexpr uitext::HeaderSub weekSubPlace(int titleW, int subW, int right) {
  return uitext::headerSub(28, titleW, right, subW, false, true);
}
// The big day (C's first line): stats.big_day of its weekday's full name
// and its time ("Saturday was your big day: 2 h 1 min").
size_t bigDayLine(uint16_t day, uint64_t ms, char* out, size_t size);
// Auto DJ's rate (C's second line): stats.autodj_right of format.percent
// ("Auto DJ got it right 87% of the time").
size_t autoDjLine(uint8_t pct, char* out, size_t size);

// The stat value (A: "7 h 37 min" on its baseline, drawStat()'s): the time
// as timeText() words it, cut into segments by its pattern
// (format.hours_minutes, format.minutes, the UI's language's): each
// placeholder's text a value segment (the numbers, in Title), each run of
// the pattern between them a word segment (in Small: "{0} godz. {1} min"
// gives "7", " godz. ", "37", " min"). Each segment's text is NUL-terminated
// in `text`, in order.
constexpr int kStatSegments = 6;
struct StatValue {
  struct Segment {
    uint8_t at = 0;      // its text: text + at
    bool value = false;  // a placeholder's (Title), else the pattern's words (Small)
  };
  char text[64] = {};
  uint8_t count = 0;
  Segment seg[kStatSegments];
  const char* at(int k) const { return text + seg[k].at; }
};
void statValue(uint64_t ms, StatValue* out);
// Its width: the value segments in `value` (Title), the words in `words`
// (Small), side by side.
int statWidth(const StatValue& v, const textfit::Font& value, const textfit::Font& words);
// A's two columns: the value (and "Listening time" over it) from x 16, the
// plays ("Plays" over its count, both right-aligned at x 304) at the right,
// at least kWeekGap apart on each line.
constexpr int kWeekValueX = 16, kWeekPlaysRight = 304, kWeekGap = 8;
constexpr bool fitsBeside(int leftW, int rightW) {
  return kWeekValueX + leftW + kWeekGap <= kWeekPlaysRight - rightW;
}

}  // namespace statstext
