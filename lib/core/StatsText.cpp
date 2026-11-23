// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsText.h"

#include <cstring>

#include "LibraryText.h"
#include "WallClock.h"

namespace statstext {

namespace sf = statsfold;
namespace sr = statsrank;
namespace sv = statsvisit;
using statsview::Section;
using statsview::Shelf;

namespace {

// " · " between a line's parts (the Library's: librarytext::kDot).
const char* const kDot = librarytext::kDot;

// `s` after `out`'s first `at` bytes, cut at a character boundary as uistr
// cuts; the new length.
size_t append(char* out, size_t size, size_t at, const char* s) {
  if (at + 1 >= size) return at;
  return at + uistr::copy(out + at, size - at, s);
}

// A UTC day's date (day 0: 2026-01-01).
wallclock::Civil civilOf(uint16_t day) {
  return wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs);
}

S monthOf(const wallclock::Civil& c) {
  const int month = c.month >= 1 && c.month <= 12 ? c.month : 1;
  return static_cast<S>(static_cast<int>(S::month_jan) + month - 1);
}

// The plays, "12 plays" (grouped: "9,999 plays").
size_t playsText(uint32_t n, char* out, size_t size) {
  return uistr::trnf(out, size, S::stats_plays_n, n, uistr::grouped(n));
}

// `text` (`len` bytes) cut with "…" to `maxW` px of `f` as the draw cuts a
// name (textfit::fit: composed, folded), into `out`. The fit's own buffer
// is 159 bytes, past any room here (the narrowest glyph is 2 px a byte or
// more: 318 px), so it never cuts a text that would fit. When `out` is
// shorter than the fit, the text is cut there too and ends with the
// ellipsis (a buffer's cut alone would leave a name with no mark).
size_t fitInto(const textfit::Font& f, int maxW, const char* text, size_t len, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char fitted[160];
  textfit::fit(f, text, len, fitted, sizeof(fitted), maxW);
  const size_t n = std::strlen(fitted);
  if (n < size) {
    std::memcpy(out, fitted, n + 1);
    return n;
  }
  // TextFit's mark: "…" when the font has it, else "...".
  const char* mark = !f.has || f.has(f.ctx, 0x2026) ? "\xE2\x80\xA6" : "...";
  const size_t m = std::strlen(mark);
  if (size <= m + 1) return uistr::copy(out, size, fitted);
  size_t keep = size - 1 - m;
  while (keep > 0 && (static_cast<unsigned char>(fitted[keep]) & 0xC0) == 0x80) --keep;
  while (keep > 0 && fitted[keep - 1] == ' ') --keep;  // as TextFit drops them before its mark
  std::memcpy(out, fitted, keep);
  std::memcpy(out + keep, mark, m);
  out[keep + m] = 0;
  return keep + m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Relative times
// ---------------------------------------------------------------------------
size_t agoText(uint32_t nowMin, uint32_t whenMin, char* out, size_t size) {
  if (!out || size == 0) return 0;
  out[0] = 0;
  if (whenMin == sv::kWhenSession || whenMin == sv::kWhenEarlier || whenMin > nowMin) return 0;
  const uint32_t ago = nowMin - whenMin;
  if (ago >= kDayMin) return 0;
  if (ago < 60) return uistr::trf(out, size, S::stats_ago_min, ago == 0 ? 1u : ago);
  return uistr::trf(out, size, S::stats_ago_h, ago / 60);
}

bool agoMoved(uint32_t wasMin, uint32_t nowMin, uint32_t whenMin) {
  if (wasMin == nowMin) return false;
  char was[48], now[48];
  agoText(wasMin, whenMin, was, sizeof(was));
  agoText(nowMin, whenMin, now, sizeof(now));
  return std::strcmp(was, now) != 0;
}

// ---------------------------------------------------------------------------
// Days
// ---------------------------------------------------------------------------
uint16_t localDay(uint32_t whenMin, int32_t tzOffsetMin) {
  const int64_t local = static_cast<int64_t>(whenMin) + tzOffsetMin;
  if (local <= 0) return 0;
  const int64_t day = local / kDayMin;
  return day >= sf::kUnsetDay ? static_cast<uint16_t>(sf::kUnsetDay - 1) : static_cast<uint16_t>(day);
}

uint8_t weekdayOf(uint16_t day) {
  // Day 0 was a Thursday (3, Monday being 0).
  return static_cast<uint8_t>((static_cast<uint32_t>(day) + 3u) % 7u);
}

S dayShort(uint16_t day) { return static_cast<S>(static_cast<int>(S::day_mon) + weekdayOf(day)); }
S dayLong(uint16_t day) { return static_cast<S>(static_cast<int>(S::day_monday) + weekdayOf(day)); }

size_t weekdayDate(uint16_t day, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char date[32];
  dayMonthText(day, date, sizeof(date));
  return uistr::trf(out, size, S::format_weekday_date, uistr::tr(dayShort(day)), date);
}

size_t dayRange(uint16_t first, uint16_t last, char* out, size_t size) {
  if (!out || size == 0) return 0;
  const wallclock::Civil a = civilOf(first), b = civilOf(last);
  if (a.year == b.year && a.month == b.month)
    return uistr::trf(out, size, S::format_day_range_month, uistr::Arg(a.day), uistr::Arg(b.day),
                      uistr::tr(monthOf(a)));
  char from[32], to[32];
  dayMonthText(first, from, sizeof(from));
  dayMonthText(last, to, sizeof(to));
  return uistr::trf(out, size, S::format_day_range, from, to);
}

size_t sectionText(const statsview::RecentItem& it, char* out, size_t size) {
  if (!out || size == 0) return 0;
  switch (it.section) {
    case Section::Today: return uistr::copy(out, size, uistr::tr(S::stats_today));
    case Section::Yesterday: return uistr::copy(out, size, uistr::tr(S::stats_yesterday));
    case Section::Session: return uistr::copy(out, size, uistr::tr(S::stats_this_session));
    case Section::Earlier: return uistr::copy(out, size, uistr::tr(S::stats_earlier));
    case Section::Day: break;
  }
  return weekdayDate(it.day, out, size);
}

// ---------------------------------------------------------------------------
// The hub
// ---------------------------------------------------------------------------
size_t hubSub(uint64_t listenedMs, uint16_t sinceDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char time[48];
  timeText(listenedMs, time, sizeof(time));
  if (sinceDay == sf::kUnsetDay) return uistr::copy(out, size, time);
  char day[32];
  dayMonthText(sinceDay, day, sizeof(day));
  return uistr::trf(out, size, S::stats_since, time, day);
}

size_t weekLine(uint64_t listenedMs, uint32_t plays, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char time[48], count[64];
  timeText(listenedMs, time, sizeof(time));
  playsText(plays, count, sizeof(count));
  return uistr::trf(out, size, S::stats_time_plays, time, count);
}

size_t topThreeLine(const textfit::Font& f, int maxW, const char* const* titles, uint32_t n, char* out,
                    size_t size) {
  if (!out || size == 0) return 0;
  out[0] = 0;
  if (!titles) return 0;
  // The join, then one cut at its end (4.2: "cut with … like any names").
  // A join the buffer cuts is 159 bytes, wider than any room (fitInto()),
  // so the fit cuts it with "…" anyway.
  char join[160];
  size_t at = 0;
  join[0] = 0;
  for (uint32_t i = 0; i < n && i < 3; ++i) {
    const char* t = titles[i] ? titles[i] : "";
    if (i) at = append(join, sizeof(join), at, kDot);
    at = append(join, sizeof(join), at, t);
  }
  return fitInto(f, maxW, join, at, out, size);
}

uint32_t hubCount(const sv::Snapshot& s, sr::List l) {
  // 4.2 as decided: the ranker's count, not the shelf's cap.
  return l < sr::kLists ? s.total(l) : 0;
}

size_t newFindsLine(const textfit::Font& f, int maxW, uint32_t n, char* out, size_t size) {
  if (!out || size == 0) return 0;
  const size_t len = uistr::trnf(out, size, S::stats_new_finds_sub, n, uistr::grouped(n));
  if (textfit::drawnWidth(f, out) <= maxW) return len;
  return uistr::trnf(out, size, S::stats_new_finds_sub_short, n, uistr::grouped(n));
}

size_t oldFavouritesLine(const textfit::Font& f, int maxW, uint32_t n, char* out, size_t size) {
  if (!out || size == 0) return 0;
  const size_t len = uistr::trnf(out, size, S::stats_old_favourites_sub, n, uistr::grouped(n));
  if (textfit::drawnWidth(f, out) <= maxW) return len;
  return uistr::trnf(out, size, S::library_tracks_n, n, uistr::grouped(n));
}

size_t notPlayedLine(uint32_t albums, uint16_t sinceDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char count[48];
  uistr::trnf(count, sizeof(count), S::library_albums_n, albums, albums);
  if (sinceDay == sf::kUnsetDay) return uistr::copy(out, size, count);
  char day[32];
  dayMonthText(sinceDay, day, sizeof(day));
  return uistr::trf(out, size, S::stats_since, count, day);
}

size_t sinceOnlyLine(uint16_t sinceDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  out[0] = 0;
  if (sinceDay == sf::kUnsetDay) return 0;
  char day[32];
  dayMonthText(sinceDay, day, sizeof(day));
  return uistr::trf(out, size, S::stats_since_only, day);
}

size_t recentLine(const textfit::Font& f, int maxW, const char* title, bool timed, uint32_t nowMin,
                  uint32_t whenMin, char* out, size_t size) {
  char ago[48] = "";
  if (timed) agoText(nowMin, whenMin, ago, sizeof(ago));
  return nameAndParts(f, maxW, title, ago, out, size);
}

size_t warmingLine(const textfit::Font& f, int maxW, uint16_t opensDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char day[32];
  dayMonthText(opensDay, day, sizeof(day));
  const size_t len = uistr::trf(out, size, S::stats_warming, day);
  if (textfit::drawnWidth(f, out) <= maxW) return len;
  return uistr::trf(out, size, S::stats_warming_short, day);
}

size_t toastText(S toast, uint16_t opensDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  if (toast == S::toast_stats_needs_month) {
    char day[32], when[64];
    dayMonthText(opensDay, day, sizeof(day));
    uistr::trf(when, sizeof(when), S::stats_warming_short, day);
    return uistr::trf(out, size, S::toast_stats_needs_month, when);
  }
  return uistr::trf(out, size, S::toast_stats_needs_dates, uistr::tr(S::stats_clock_fix));
}

S emptyRule(const textfit::Font& f, int maxW) {
  return textfit::drawnWidth(f, uistr::tr(S::stats_empty_rule)) <= maxW ? S::stats_empty_rule
                                                                         : S::stats_empty_rule_short;
}

namespace {

// The snapshot's data for a hub row (Line::Data), in kHubLineW.
size_t hubData(statsview::HubRow row, const statsview::Lifetime& l, const sv::Snapshot& s, const textfit::Font& f,
               TitleFn title, void* ctx, char* out, size_t size) {
  using statsview::HubRow;
  auto titleOf = [&](uint16_t id, char* buf, size_t n) {
    buf[0] = 0;
    if (title && id != sv::kNoRow) title(id, buf, n, ctx);
  };
  switch (row) {
    case HubRow::Week: {
      const statsview::WeekFigures w = statsview::weekFigures(s);
      return weekLine(w.listenedMs, w.plays, out, size);
    }
    case HubRow::MostPlayed: {
      char t[3][kTitleBytes];
      const char* titles[3] = {t[0], t[1], t[2]};
      const uint32_t n = s.rows(sr::kMostAll) < 3 ? s.rows(sr::kMostAll) : 3;
      for (uint32_t i = 0; i < n; ++i) titleOf(s.rowAt(sr::kMostAll, i).id, t[i], sizeof(t[i]));
      return topThreeLine(f, kHubLineW, titles, n, out, size);
    }
    case HubRow::NewFinds: return newFindsLine(f, kHubLineW, hubCount(s, sr::kNewFinds), out, size);
    case HubRow::OldFavourites: return oldFavouritesLine(f, kHubLineW, hubCount(s, sr::kOldFavourites), out, size);
    case HubRow::NotPlayed: return notPlayedLine(s.notPlayedCount(), s.sinceDay(), out, size);
    case HubRow::Recent: {
      if (s.rows(sr::kRecent) == 0) return 0;
      const sv::Row r = s.rowAt(sr::kRecent, 0);
      char t[kTitleBytes];
      titleOf(r.id, t, sizeof(t));
      // (A row's "25 min ago" from the snapshot's `when` and now: D10. A
      // dated row of an undated snapshot has no time: no today to say it.)
      const bool timed = s.dated() && l.dated && r.when != sv::kWhenSession && r.when != sv::kWhenEarlier;
      return recentLine(f, kHubLineW, t, timed, l.nowMin, r.when, out, size);
    }
    default: return 0;
  }
}

}  // namespace

size_t hubLine(const statsview::HubItem& it, const statsview::Lifetime& l, const sv::Snapshot* snap,
               const textfit::Font& f, TitleFn title, void* ctx, char* out, size_t size) {
  using statsview::Line;
  if (!out || size == 0) return 0;
  out[0] = 0;
  switch (it.line) {
    case Line::Text: return uistr::copy(out, size, uistr::tr(it.text));
    case Line::Warming: return warmingLine(f, kReasonW, statsview::opensDay(l), out, size);
    case Line::Status: return uistr::copy(out, size, uistr::tr(S::stats_clock_fix));
    case Line::SinceOnly: return sinceOnlyLine(snap ? snap->sinceDay() : l.sinceDay, out, size);
    case Line::Data: return snap ? hubData(it.row, l, *snap, f, title, ctx, out, size) : 0;
    case Line::None: break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// The shelves
// ---------------------------------------------------------------------------
size_t shelfSub(Shelf shelf, uint8_t window, uint16_t sinceDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  out[0] = 0;
  switch (shelf) {
    case Shelf::MostPlayed:
      if (window == sv::k7Days) return uistr::copy(out, size, uistr::tr(S::stats_sub_last7));
      if (window == sv::k30Days) return uistr::copy(out, size, uistr::tr(S::stats_sub_last30));
      return sinceOnlyLine(sinceDay, out, size);
    case Shelf::NewFinds: return uistr::copy(out, size, uistr::tr(S::stats_first_heard));
    case Shelf::OldFavourites: return uistr::copy(out, size, uistr::tr(S::stats_not_in_30));
    case Shelf::NotPlayed: return sinceOnlyLine(sinceDay, out, size);
    case Shelf::Recent: break;
  }
  return 0;
}

size_t trayLabel(uint8_t window, uint8_t days30, char* out, size_t size) {
  if (!out || size == 0) return 0;
  if (window == sv::k7Days) return uistr::trnf(out, size, S::stats_days_n, 7, 7);
  if (window == sv::k30Days) return uistr::trnf(out, size, S::stats_days_n, days30, static_cast<unsigned>(days30));
  return uistr::copy(out, size, uistr::tr(S::stats_window_all));
}

size_t nameAndParts(const textfit::Font& f, int maxW, const char* name, const char* parts, char* out, size_t size) {
  if (!out || size == 0) return 0;
  out[0] = 0;
  if (!name) name = "";
  if (!parts) parts = "";
  if (!parts[0]) return fitInto(f, maxW, name, std::strlen(name), out, size);
  // The parts are measured first and kept whole (the count is never cut);
  // the name gets what they leave, in pixels and in the buffer's bytes (a
  // character and the ellipsis at least, else the parts alone).
  const size_t tail = std::strlen(kDot) + std::strlen(parts);
  const int room = maxW - textfit::drawnWidth(f, kDot) - textfit::drawnWidth(f, parts);
  if (!name[0] || room < kMinNameW || tail + 5 > size) return uistr::copy(out, size, parts);
  size_t at = fitInto(f, room, name, std::strlen(name), out, size - tail);
  at = append(out, size, at, kDot);
  return append(out, size, at, parts);
}

size_t rowLine(const textfit::Font& f, int maxW, sr::List list, const sv::Row& row, const char* artist, bool timed,
               uint32_t nowMin, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char parts[128] = "", plays[64];
  // The extra part (who found it, the last day) beside the count, when the
  // two leave the artist kMinNameW px; else the count alone (the artist is
  // the row's other name, the count its figure: both stay). German's and
  // Spanish's plays beside "found by shuffle" leave it 28 and 15 px at 22
  // plays (test_ui_library). With no artist the pair has the whole line,
  // and gives way when it is wider: nameAndParts() keeps the parts uncut,
  // and the draw's cut would take the count (7.6: never cut).
  auto withExtra = [&](const char* first, const char* second) {
    size_t at = append(parts, sizeof(parts), 0, first);
    at = append(parts, sizeof(parts), at, kDot);
    append(parts, sizeof(parts), at, second);
    const bool named = artist && artist[0];
    const int w = textfit::drawnWidth(f, parts);
    if (named ? maxW - textfit::drawnWidth(f, kDot) - w < kMinNameW : w > maxW)
      uistr::copy(parts, sizeof(parts), plays);
  };
  switch (list) {
    case sr::kMost7:
    case sr::kMost30:
    case sr::kMostAll: playsText(row.count, parts, sizeof(parts)); break;
    case sr::kNewFinds:
      playsText(row.count, plays, sizeof(plays));
      if (row.src == sf::kAutoDj) {
        withExtra(uistr::tr(S::stats_found_autodj), plays);
      } else if (row.src == sf::kShuffle) {
        withExtra(uistr::tr(S::stats_found_shuffle), plays);
      } else {
        uistr::copy(parts, sizeof(parts), plays);
      }
      break;
    case sr::kOldFavourites: {
      char day[32], last[64];
      playsText(row.count, plays, sizeof(plays));
      dayMonthText(row.lastDay, day, sizeof(day));
      uistr::trf(last, sizeof(last), S::stats_last_on, day);
      withExtra(plays, last);
      break;
    }
    case sr::kRecent:
      if (timed) agoText(nowMin, row.when, parts, sizeof(parts));
      break;
    default: break;
  }
  return nameAndParts(f, maxW, artist, parts, out, size);
}

// ---------------------------------------------------------------------------
// Last 7 days
// ---------------------------------------------------------------------------
size_t weekSub(uint16_t firstDay, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char range[48];
  const uint32_t last = static_cast<uint32_t>(firstDay) + sr::kWeekDays - 1;
  dayRange(firstDay, static_cast<uint16_t>(last < sf::kUnsetDay ? last : sf::kUnsetDay - 1), range, sizeof(range));
  return uistr::trf(out, size, S::stats_utc, range);
}

size_t bigDayLine(uint16_t day, uint64_t ms, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char time[48];
  timeText(ms, time, sizeof(time));
  return uistr::trf(out, size, S::stats_big_day, uistr::tr(dayLong(day)), time);
}

size_t autoDjLine(uint8_t pct, char* out, size_t size) {
  if (!out || size == 0) return 0;
  char share[16];
  uistr::trf(share, sizeof(share), S::format_percent, static_cast<unsigned>(pct));
  return uistr::trf(out, size, S::stats_autodj_right, share);
}

void statValue(uint64_t ms, StatValue* out) {
  if (!out) return;
  *out = StatValue();
  // timeText()'s numbers and pattern: the hours grouped, the minutes
  // unpadded; under an hour the minutes alone.
  const uint64_t min = ms / 60000;
  const bool hours = min >= 60;
  const uistr::Arg args[2] = {hours ? uistr::grouped(min / 60) : uistr::Arg(static_cast<unsigned long long>(min)),
                              uistr::Arg(static_cast<unsigned>(min % 60))};
  const int argc = hours ? 2 : 1;
  const char* p = uistr::tr(hours ? S::format_hours_minutes : S::format_minutes);
  size_t at = 0;
  // A segment: its text, NUL-terminated after the last one's (dropped when
  // the text or the segments are full: the value is short, 64 B and 6
  // segments hold any language's).
  auto add = [&](const char* s, size_t n, bool value) {
    if (n == 0 || out->count >= kStatSegments || at + n + 1 > sizeof(out->text)) return;
    std::memcpy(out->text + at, s, n);
    out->text[at + n] = 0;
    out->seg[out->count].at = static_cast<uint8_t>(at);
    out->seg[out->count].value = value;
    ++out->count;
    at += n + 1;
  };
  // The pattern walked as uistr::format() fills it: {0}..{N-1} the numbers,
  // every run between them (any other brace among it) one word segment.
  const char* words = p;
  while (*p) {
    if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}' && p[1] - '0' < argc) {
      add(words, static_cast<size_t>(p - words), false);
      const char* s = args[p[1] - '0'].str();
      add(s, std::strlen(s), true);
      p += 3;
      words = p;
      continue;
    }
    ++p;
  }
  add(words, static_cast<size_t>(p - words), false);
}

int statWidth(const StatValue& v, const textfit::Font& value, const textfit::Font& words) {
  int w = 0;
  for (int k = 0; k < v.count; ++k) w += textfit::drawnWidth(v.seg[k].value ? value : words, v.at(k));
  return w;
}

}  // namespace statstext
