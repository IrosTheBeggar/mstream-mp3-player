// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "FilterGrid.h"

#include <cstring>

#include "UiStrings.h"
#include "UiText.h"

namespace filtergrid {

using moodfilter::Filter;
using moodfilter::Level;
using moodfilter::Scale;
using moodfilter::Voice;
using uistr::S;
using uistr::tr;

namespace {

static_assert(textRoom(kRightRail, false) == uitext::kFilterRowTitleW, "mood.filters' room beside the rail");
static_assert(textRoom(kRightRail, true) == uitext::kFilterRowCountW, "the first row's count beside the rail");
static_assert(textRoom(kRightBar, true) == 212 && textRoom(kRightNone, true) == 220, "7.2's rooms");
static_assert(crossHitX(kRightRail) == 224 && crossHitX(kRightBar) == 276 && crossHitX(kRightNone) == 284,
              "7.4's cross zones");

// " · " between the summary's words (librarytext::kDot).
constexpr const char* kDot = " \xC2\xB7 ";

// The rows' cells, left to right (row 3's last is the level cell).
constexpr Cell kRowCells[kRows][3] = {
    {Cell::Happy, Cell::NeutralV, Cell::Sad},
    {Cell::Relaxed, Cell::NeutralE, Cell::Aggressive},
    {Cell::Instrumental, Cell::Vocals, Cell::Level},
    {Cell::Acoustic, Cell::Party, Cell::Danceable},
};
// How many of a row's cells share it by their words (the voice tray's two).
constexpr int kShared[kRows] = {3, 3, 2, 3};

// a / b to the nearest whole number, a half down (b > 0; a may be
// negative: a chip row's overflow).
int nearest(long a, long b) {
  const long n = 2 * a + b - 1, d = 2 * b;
  long q = n / d;
  if (n % d != 0 && n < 0) --q;  // floor
  return static_cast<int>(q);
}

int widthOf(const textfit::Font& f, const char* s) { return s && *s ? textfit::drawnWidth(f, s) : 0; }

// A word's need in its cell: its width and 12. A tag in Small wider than
// kWrapW with a space wraps at the space that leaves its wider line the
// narrowest; the need is that line's, and `*wrapAt` the byte the second
// line starts at (0: one line).
int needOf(const textfit::Font& f, const char* word, bool wraps, uint8_t* wrapAt) {
  *wrapAt = 0;
  const int whole = widthOf(f, word);
  if (!wraps || whole <= kWrapW || !word) return whole + kWordPad;
  char line[128];
  int best = whole;
  const size_t len = strlen(word);
  for (size_t sp = 1; sp + 1 < len && sp < sizeof(line); ++sp) {
    if (word[sp] != ' ') continue;
    memcpy(line, word, sp);
    line[sp] = 0;
    const int first = widthOf(f, line);
    const int second = widthOf(f, word + sp + 1);
    const int wider = first > second ? first : second;
    if (wider < best && sp + 1 < 256) {
      best = wider;
      *wrapAt = static_cast<uint8_t>(sp + 1);
    }
  }
  return best + kWordPad;
}

// The widths of `n` cells sharing `total` px, from their needs (7.3):
// equal while each need fits its share, else proportional (each its need
// and an even share of the spare); a row still too narrow scales to it
// when `scale` (a tray), else gives up the overflow evenly (the chips).
Layout::Share share(const int* need, int n, int total, bool scale, int* out) {
  bool equal = true;
  for (int k = 0; k < n; ++k) {
    out[k] = nearest(static_cast<long>(total) * (k + 1), n) - nearest(static_cast<long>(total) * k, n);
    if (need[k] > out[k]) equal = false;
  }
  if (equal) return Layout::Share::Equal;
  int sum = 0;
  for (int k = 0; k < n; ++k) sum += need[k];
  const int spare = total - sum;
  const bool scaled = spare < 0 && scale;
  int cum = 0, prev = 0;
  for (int k = 0; k < n; ++k) {
    cum += need[k];
    const int edge = scaled ? nearest(static_cast<long>(cum) * total, sum)
                            : cum + nearest(static_cast<long>(spare) * (k + 1), n);
    out[k] = edge - prev;
    prev = edge;
  }
  return scaled ? Layout::Share::Scaled : Layout::Share::Proportional;
}

}  // namespace

Words words() {
  Words w;
  auto set = [&](Cell c, S id) { w.cell[index(c)] = tr(id); };
  set(Cell::Happy, S::mood_happy);
  set(Cell::NeutralV, S::mood_neutral);
  set(Cell::Sad, S::mood_sad);
  set(Cell::Relaxed, S::mood_relaxed);
  set(Cell::NeutralE, S::mood_neutral);
  set(Cell::Aggressive, S::mood_aggressive);
  set(Cell::Instrumental, S::mood_instrumental);
  set(Cell::Vocals, S::mood_vocals);
  set(Cell::Acoustic, S::mood_acoustic);
  set(Cell::Party, S::mood_party);
  set(Cell::Danceable, S::mood_danceable);
  set(Cell::Clear, S::mood_clear);
  for (int k = 0; k < 3; ++k) w.level[k] = levelWord(static_cast<Level>(k));
  return w;
}

const char* levelWord(Level l) {
  return tr(l == Level::Strict ? S::mood_strict : l == Level::Loose ? S::mood_loose : S::mood_standard);
}

Layout layout(const Words& words, const textfit::Font& body, const textfit::Font& small) {
  Layout l;
  // The level cell: its widest word in Small, whatever the grid's font.
  int widest = 0;
  for (const char* w : words.level) {
    const int lw = widthOf(small, w);
    if (lw > widest) widest = lw;
  }
  const int levelW = widest + kLevelPad > kLevelMinW ? widest + kLevelPad : kLevelMinW;
  l.trayW[0] = l.trayW[1] = kRowW;
  l.trayW[2] = kRowW - kLevelGap - levelW;
  const int total[kRows] = {kRowW - 2 * kTrayInset, kRowW - 2 * kTrayInset, l.trayW[2] - 2 * kTrayInset,
                            kRowW - 2 * kChipGap};
  // The grid's font: Body when every row's words fit it.
  auto needs = [&](int row, const textfit::Font& f, bool wraps, int* out) {
    for (int k = 0; k < kShared[row]; ++k) {
      const int c = index(kRowCells[row][k]);
      out[k] = needOf(f, words.cell[c], wraps && row == 3, &l.wrap[c]);
    }
  };
  bool bodyFits = true;
  for (int row = 0; row < kRows && bodyFits; ++row) {
    int need[3], sum = 0;
    needs(row, body, false, need);
    for (int k = 0; k < kShared[row]; ++k) sum += need[k];
    bodyFits = sum <= total[row];
  }
  l.small = !bodyFits;
  const textfit::Font& f = l.small ? small : body;
  for (int row = 0; row < kRows; ++row) {
    int need[3], w[3];
    needs(row, f, l.small, need);
    l.share[row] = share(need, kShared[row], total[row], row < 3, w);
    int x = row < 3 ? kRowX + kTrayInset : kRowX;
    for (int k = 0; k < kShared[row]; ++k) {
      const int c = index(kRowCells[row][k]);
      l.x[c] = static_cast<int16_t>(x);
      l.w[c] = static_cast<int16_t>(w[k]);
      x += w[k] + (row < 3 ? 0 : kChipGap);
    }
  }
  l.x[index(Cell::Level)] = static_cast<int16_t>(kRowX + l.trayW[2] + kLevelGap);
  l.w[index(Cell::Level)] = static_cast<int16_t>(levelW);
  // The title row.
  const int pillW = widthOf(small, words.cell[index(Cell::Clear)]) + kClearPad;
  l.x[index(Cell::Clear)] = kClearX;
  l.w[index(Cell::Clear)] = static_cast<int16_t>(pillW);
  l.x[index(Cell::Cross)] = kCrossX;
  l.w[index(Cell::Cross)] = kCrossW;
  l.countX = static_cast<int16_t>((kClearX + pillW + kCountRight) / 2);
  l.countW = static_cast<int16_t>(kCountRight - (kClearX + pillW) - 2 * kCountGap);
  return l;
}

int rowOf(Cell c) {
  if (c == Cell::Clear || c == Cell::Cross || c == Cell::None) return -1;
  const int i = index(c);
  return i < index(Cell::Relaxed) ? 0 : i < index(Cell::Instrumental) ? 1 : i < index(Cell::Acoustic) ? 2 : 3;
}

Box box(const Layout& l, Cell c) {
  Box b;
  if (c == Cell::None) return b;
  const int row = rowOf(c);
  b.x = l.x[index(c)];
  b.w = l.w[index(c)];
  b.y = row < 0 ? kPillY : kRowY[row];
  b.h = row < 0 ? kPillH : kRowH;
  return b;
}

int split(const Layout& l, int row, int k) {
  const Cell next = kRowCells[row][k + 1];
  int edge = l.x[index(next)];
  // A gap splits at its middle: between chips, and the voice tray's
  // before the level cell.
  if (row == 3) edge -= kChipGap / 2;
  if (next == Cell::Level) edge -= kLevelGap / 2;
  return edge + skew(edge);
}

Cell hit(const Layout& l, int x, int y, bool rightEdge) {
  if (y < kTitleBand) {
    if (rightEdge || x >= kCrossHitX) return Cell::Cross;
    const int clearEnd = l.x[index(Cell::Clear)] + l.w[index(Cell::Clear)];
    return x < clearEnd + skew(clearEnd) ? Cell::Clear : Cell::None;
  }
  int row = (y - kTitleBand) / kRowPitch;
  if (row >= kRows) row = kRows - 1;
  const Cell* cells = kRowCells[row];
  if (rightEdge) return cells[2];
  for (int k = 0; k < 2; ++k) {
    if (x < split(l, row, k)) return cells[k];
  }
  return cells[2];
}

Filter tap(const Filter& f, Cell c) {
  Filter g = f;
  auto scale = [](Scale now, Scale v) { return now == v ? Scale::Any : v; };
  auto voice = [](Voice now, Voice v) { return now == v ? Voice::Any : v; };
  switch (c) {
    case Cell::Happy: g.valence = scale(f.valence, Scale::First); break;
    case Cell::NeutralV: g.valence = scale(f.valence, Scale::Neutral); break;
    case Cell::Sad: g.valence = scale(f.valence, Scale::Second); break;
    case Cell::Relaxed: g.energy = scale(f.energy, Scale::First); break;
    case Cell::NeutralE: g.energy = scale(f.energy, Scale::Neutral); break;
    case Cell::Aggressive: g.energy = scale(f.energy, Scale::Second); break;
    case Cell::Instrumental: g.voice = voice(f.voice, Voice::Instrumental); break;
    case Cell::Vocals: g.voice = voice(f.voice, Voice::Vocals); break;
    case Cell::Acoustic: g.tags ^= moodfilter::kAcoustic; break;
    case Cell::Party: g.tags ^= moodfilter::kParty; break;
    case Cell::Danceable: g.tags ^= moodfilter::kDanceable; break;
    // Strict > Standard > Loose > Strict (3.2).
    case Cell::Level:
      g.level = f.level == Level::Strict ? Level::Standard : f.level == Level::Standard ? Level::Loose : Level::Strict;
      break;
    case Cell::Clear: g = Filter(); break;
    default: break;
  }
  return g;
}

bool picked(const Filter& f, Cell c) {
  switch (c) {
    case Cell::Happy: return f.valence == Scale::First;
    case Cell::NeutralV: return f.valence == Scale::Neutral;
    case Cell::Sad: return f.valence == Scale::Second;
    case Cell::Relaxed: return f.energy == Scale::First;
    case Cell::NeutralE: return f.energy == Scale::Neutral;
    case Cell::Aggressive: return f.energy == Scale::Second;
    case Cell::Instrumental: return f.voice == Voice::Instrumental;
    case Cell::Vocals: return f.voice == Voice::Vocals;
    case Cell::Acoustic: return (f.tags & moodfilter::kAcoustic) != 0;
    case Cell::Party: return (f.tags & moodfilter::kParty) != 0;
    case Cell::Danceable: return (f.tags & moodfilter::kDanceable) != 0;
    default: return false;
  }
}

bool anythingSet(const Filter& f) { return f.active() || f.level != Level::Standard; }

int levelDot(Level l) { return l == Level::Strict ? 0 : l == Level::Loose ? 2 : 1; }

size_t summary(const Filter& f, char* out, size_t size) {
  if (!out || !size) return 0;
  const char* parts[8];
  int n = 0;
  auto scale = [&](Scale s, S first, S second) {
    if (s != Scale::Any) parts[n++] = tr(s == Scale::First ? first : s == Scale::Second ? second : S::mood_neutral);
  };
  scale(f.valence, S::mood_happy, S::mood_sad);
  scale(f.energy, S::mood_relaxed, S::mood_aggressive);
  if (f.voice != Voice::Any) parts[n++] = tr(f.voice == Voice::Instrumental ? S::mood_instrumental : S::mood_vocals);
  if (f.tags & moodfilter::kAcoustic) parts[n++] = tr(S::mood_acoustic);
  if (f.tags & moodfilter::kParty) parts[n++] = tr(S::mood_party);
  if (f.tags & moodfilter::kDanceable) parts[n++] = tr(S::mood_danceable);
  if (f.level != Level::Standard) parts[n++] = levelWord(f.level);
  size_t len = 0;
  out[0] = 0;
  for (int i = 0; i < n; ++i) {
    // Whole words only: a buffer too short ends after the last that fits.
    const size_t sep = i ? strlen(kDot) : 0;
    if (len + sep + strlen(parts[i]) >= size) break;
    if (i) len += uistr::copy(out + len, size - len, kDot);
    len += uistr::copy(out + len, size - len, parts[i]);
  }
  return len;
}

size_t countText(const textfit::Font& small, int room, Scope scope, bool active, uint32_t fits, uint32_t total,
                 char* out, size_t size) {
  const bool albums = scope == Scope::Albums;
  if (!active) return uistr::trnf(out, size, albums ? S::library_albums_n : S::library_tracks_n, total,
                                  uistr::grouped(total));
  const size_t n = uistr::trnf(out, size, albums ? S::mood_albums_of : S::mood_tracks_of, total,
                               uistr::grouped(fits), uistr::grouped(total));
  if (textfit::drawnWidth(small, out) <= room) return n;
  return uistr::trf(out, size, S::queue_summary_position, uistr::grouped(fits), uistr::grouped(total));
}

Top topHit(int x, bool rightEdge, int right, bool on) {
  if (right <= kRightRail && (rightEdge || x >= kRailHitX)) return Top::Rail;
  if (on && (rightEdge || x >= crossHitX(right))) return Top::Cross;
  return Top::Editor;
}

}  // namespace filtergrid
