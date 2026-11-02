// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <algorithm>
#include <cstdint>

#include "SeekBar.h"
#include "TrackSeek.h"
#include "WaveShape.h"

// Where Now Playing's waveform is drawn, in what inks, and which pass draws what
// (docs/SEEK-BAR.md): the numbers and the rules the page draws by, apart from the
// drawing, so a host test can hold them. A strip of 21 rows, y 137-157, and in it
// 99 bars of 2 px with a gap of 1 px, along the seek bar's line (x 12-307),
// mirrored about the strip's row 10, y 147: a bar is that row and h rows either
// side, so a silent one is 2 x 1 px. A playhead of 2 px, x - 1 and x, the strip's
// whole height; under a finger a knob (r 4, a dot of r 2) on the centre row.
// Under the strip the text row, y 158-173; above it, while a finger scrubs, the
// readout row, y 110-136.
//
// The page maps each Ink to its colour and draws. A track with no shape
// (showsBars()) draws the plain line on the same centre row. The strip and the
// text row are separate draws on separate triggers, regionToDraw() and textDraw(),
// and go as one push when both fall due with the strip whole (pushedAsOne()).
// Portable, host-tested (test_wave_bar; the rows against the fonts, test_ui_library).
namespace wave {

constexpr int kScreenW = 320;
constexpr int kStripY = 137, kStripH = 21;  // y 137-157: pushed as one, or a part of its width
constexpr int kCentreY = 147;               // the strip's row 10
constexpr int kBarsY = kCentreY - kMaxHeight, kBarsH = 2 * kMaxHeight + 1;  // y 138-156
constexpr int kFirstColumnX = 12, kColumnStep = 3, kColumnW = 2;
constexpr int kPlayheadW = 2;  // x - 1 and x, as a scrub's marker
constexpr int kKnobR = 4, kKnobDotR = 2;
constexpr int kMoveMargin = kKnobR;  // how far past its old and new x a playhead's move redraws
// The strip's sprite has its row 0 on y 137.
constexpr int kCentreRow = kCentreY - kStripY;
// The plain line, about the same centre row c, in the sprite's rows: 4 px thin,
// c - 2 to c + 1 (y 145-148), with a knob of r 3 once it can be moved; under a
// finger 6 px, c - 3 to c + 2 (y 144-149). With no length known, and past a scrub's
// reach, dots of 3 x 2 every 6 px, c - 1 and c. A scrub's marker, 9 rows, c - 4 to
// c + 4. They are the rows the line had about its knob before the bars.
constexpr int kThinRow = kCentreRow - 2, kThinH = 4, kThinR = 2, kRestKnobR = 3;
constexpr int kThickRow = kCentreRow - 3, kThickH = 6, kThickR = 3;
constexpr int kDotsRow = kCentreRow - 1, kDotsH = 2, kDotW = 3, kDotStep = 6;
constexpr int kLineMarkerRow = kCentreRow - 4, kLineMarkerH = 9;
// The text row, the elapsed time, the status and the length: a Small line centred
// on y 165. Its 17 rows start on y 157, the strip's last, and it is pushed as y
// 158-173. Latin, Greek and Cyrillic have no ink on that first row (but for 8 rare
// accented letters); the glyph store's do, so a line with one of its glyphs is
// centred on y 166 (the page; test_ui_library). Then y 174-175 are background.
constexpr int kTextRowY = kStripY + kStripH, kTextRowH = 16, kTextCentreY = 165;
constexpr int kStripAndTextH = kStripH + kTextRowH;  // y 137-173: the two in one push
// The readout row while a finger scrubs: the 27 rows that end where the strip
// starts. Its text in its rows: Title and Bold centred on row 13, Small on row 16.
constexpr int kReadoutH = 27, kReadoutY = kStripY - kReadoutH;
constexpr int kReadoutBigRow = 13, kReadoutSmallRow = 16;

// Column i's first px (i 0-98); the second is one right of it.
constexpr int columnX(int i) { return kFirstColumnX + kColumnStep * i; }
// A bar of height h: its top row, and how many rows it has.
constexpr int barTop(int h) { return kCentreY - h; }
constexpr int barRows(int h) { return 2 * h + 1; }

static_assert(kFirstColumnX == SeekBar::kLineX, "the first bar starts where the line does");
static_assert(columnX(kColumns - 1) + kColumnW == SeekBar::kLineX + SeekBar::kLineW,
              "the last bar ends on the line's last px, x 307");
static_assert(kCentreY == kStripY + kStripH / 2, "the centre row is the strip's middle");
static_assert(kBarsY == kStripY + 1 && kBarsY + kBarsH == kStripY + kStripH - 1,
              "the playhead stands a row past the tallest bar at each end");
static_assert(SeekBar::kOffAboveY == kReadoutY - 7 && SeekBar::kBackAboveY == kReadoutY + 1,
              "off 7 rows above the readout (onto the artist row), back from its second row");

// The playhead's x at `posMs` of `lengthMs` (where the plain line's fill ends);
// under a finger it is SeekBar::knobX().
inline int playheadX(uint32_t posMs, uint32_t lengthMs) { return SeekBar::kLineX + SeekBar::xOf(posMs, lengthMs); }
// At rest it stands on the whole second: it moves in the pass the text row's time
// does, once a second at most, not at each px of a short track (30 a second at
// 10 s, each a bus hold of its own). The plain line's fill with it, as the band's.
inline int restPlayheadX(uint32_t posMs, uint32_t lengthMs) { return playheadX(posMs - posMs % 1000, lengthMs); }
// How far a scrub goes: the x of the seek's limit (where the plain line's thick
// part ends).
inline int reachX(uint32_t lengthMs) { return playheadX(trackseek::seekLimitMs(lengthMs), lengthMs); }

// The columns that overlap x rx..rx + rw - 1: first..last, none when first >
// last (a gap between two bars, or nothing of the line).
struct ColumnRange {
  int first;
  int last;
  constexpr bool empty() const { return first > last; }
};
constexpr ColumnRange columnsIn(int rx, int rw) {
  const int end = rx + rw - 1;  // the region's last px
  if (rw <= 0 || end < kFirstColumnX) return ColumnRange{0, -1};
  const int endOf0 = kFirstColumnX + kColumnW - 1;  // column 0's last px
  const int first = rx <= endOf0 ? 0 : (rx - endOf0 + kColumnStep - 1) / kColumnStep;
  const int last = std::min((end - kFirstColumnX) / kColumnStep, kColumns - 1);
  return ColumnRange{first, last};
}

// A part of the strip's width: x..x + w - 1.
struct Region {
  int x;
  int w;
};
// What a move of the playhead from `oldX` to `newX` redraws, clamped to the
// screen: the old place and the new, and every bar between that changes side.
constexpr Region movedRegion(int oldX, int newX) {
  const int lo = std::max(std::min(oldX, newX) - kMoveMargin, 0);
  const int hi = std::min(std::max(oldX, newX) + kMoveMargin, kScreenW - 1);
  return Region{lo, hi - lo + 1};
}

// The bar's looks. Inert (too short, or no length to seek in) draws the line,
// never bars. RestPaused is anything but Playing: paused, stopped, waiting.
enum class Look : uint8_t { Inert, RestPlaying, RestPaused, Pressed, Scrubbing, Off };

// The colours by Theme.h's names: accent::NowPlaying, col::TXT, col::SOFT,
// col::DIM, col::FAINT, col::BTN_HI, col::BTN. None: not drawn.
enum class Ink : uint8_t { None, Accent, Txt, Soft, Dim, Faint, BtnHi, Btn };

// A look's inks. The knob and the marker are under a finger only. The knob is a
// dot of `knobDot` on a disc of `knob`. The page hides the marker while the touch
// is staying (SeekBar::staying()). A look with no `text` blanks the text row: the
// readout above says it all.
struct Inks {
  Ink played;     // a bar left of the playhead
  Ink unplayed;   // the others
  Ink pastReach;  // the others a seek can't reach
  Ink playhead;
  Ink knob;
  Ink knobDot;
  Ink marker;
  bool text;
};
constexpr Inks inksOf(Look look) {
  switch (look) {
    case Look::RestPlaying:
      return {Ink::Accent, Ink::BtnHi, Ink::BtnHi, Ink::Txt, Ink::None, Ink::None, Ink::None, true};
    case Look::RestPaused:
      return {Ink::Dim, Ink::BtnHi, Ink::BtnHi, Ink::Soft, Ink::None, Ink::None, Ink::None, true};
    case Look::Pressed:
      return {Ink::Accent, Ink::Faint, Ink::Faint, Ink::Txt, Ink::Txt, Ink::Accent, Ink::None, true};
    case Look::Scrubbing:
      return {Ink::Accent, Ink::Faint, Ink::Btn, Ink::Txt, Ink::Txt, Ink::Accent, Ink::Dim, false};
    case Look::Off:
      return {Ink::Dim, Ink::Btn, Ink::Btn, Ink::Dim, Ink::Dim, Ink::None, Ink::None, false};
    case Look::Inert:
      break;
  }
  return {Ink::None, Ink::None, Ink::None, Ink::None, Ink::None, Ink::None, Ink::None, true};
}

// What a bar is to the playhead at x `playheadAt` (the knob's, under a finger).
// Played: its first px is left of that, as the plain line's fill. Past: while
// scrubbing, not played and its first px not left of `reach` (reachX()). Under
// any other look nothing is past the reach.
enum class BarClass : uint8_t { Played, Unplayed, Past };
constexpr BarClass barClass(Look look, int i, int playheadAt, int reach) {
  return columnX(i) < playheadAt                          ? BarClass::Played
         : look == Look::Scrubbing && columnX(i) >= reach ? BarClass::Past
                                                          : BarClass::Unplayed;
}
constexpr Ink barInk(Look look, int i, int playheadAt, int reach) {
  const BarClass k = barClass(look, i, playheadAt, reach);
  const Inks in = inksOf(look);
  return k == BarClass::Played ? in.played : k == BarClass::Past ? in.pastReach : in.unplayed;
}

// Whether the bar is drawn as bars: a shape, a look that takes one, and a length
// that agrees with `lengthMs` (the bar's frozen one under a finger, else the
// player's). Anything else is the plain line.
constexpr bool showsBars(Look look, const Shape* shape, uint32_t lengthMs) {
  return shape != nullptr && look != Look::Inert && lengthAgrees(shape->lengthMs, lengthMs);
}

// The look of a touch's phase: at rest, inert unless the track can be sought.
constexpr Look lookOf(SeekBar::Phase phase, bool seekable, bool playing) {
  return phase == SeekBar::Phase::Pressed     ? Look::Pressed
         : phase == SeekBar::Phase::Scrubbing ? Look::Scrubbing
         : phase == SeekBar::Phase::Off       ? Look::Off
         : !seekable                          ? Look::Inert
         : playing                            ? Look::RestPlaying
                                              : Look::RestPaused;
}

// What the strip has about its centre row: the bars; or the plain line; or, with
// no length known, the line's dots.
enum class Fill : uint8_t { Dots, Line, Bars };
constexpr Fill fillOf(Look look, const Shape* shape, uint32_t lengthMs) {
  return showsBars(look, shape, lengthMs) ? Fill::Bars : lengthMs > 0 ? Fill::Line : Fill::Dots;
}

// What the strip shows: all a pass compares. The page keeps the one it drew; a
// default one has no playhead, so anything is new to it. playheadX is the screen
// x. markerX is -1 unless a finger scrubs or is off the bar. version is
// WaveState::version(), counted only while the bars show: a read that came to
// nothing draws nothing.
struct Strip {
  Look look = Look::Inert;
  Fill fill = Fill::Dots;
  bool playing = false;  // the plain line's ink, and at rest the bars'
  uint32_t track = 0;
  uint32_t version = 0;
  int playheadX = -1;
  int markerX = -1;
};
// From the touch, the player's snapshot and the wave record. Under a finger they
// are the bar's: its length frozen at the Down, and its knob, on the live x. At rest
// the playhead is on the whole second (restPlayheadX()).
inline Strip stripOf(const SeekBar& bar, bool seekable, bool playing, uint32_t posMs, uint32_t durationMs,
                     const Shape* shape, uint32_t track, uint32_t version) {
  Strip s;
  s.look = lookOf(bar.phase(), seekable, playing);
  s.fill = fillOf(s.look, shape, bar.active() ? bar.lengthMs() : durationMs);
  s.playing = playing;
  s.track = track;
  s.version = s.fill == Fill::Bars ? version : 0;
  s.playheadX = bar.active() ? bar.knobX() : restPlayheadX(posMs, durationMs);
  s.markerX = bar.scrubbing() ? bar.markerX() : -1;
  return s;
}

// What of the strip a pass redraws, from what was drawn and what it shows now. A
// look, a shape, a track, a play state or a scrub's marker that changed: all of it.
// Only the playhead moved: the part movedRegion() says. Nothing: w 0. Neither the
// second nor the length is here: they are the text row's, and they move the strip
// only by moving the playhead. (While a finger scrubs, the page draws any of this
// as a whole frame, on the frame deadlines.)
constexpr Region regionToDraw(const Strip& drawn, const Strip& now) {
  return drawn.playheadX < 0 || drawn.look != now.look || drawn.fill != now.fill || drawn.playing != now.playing ||
                 drawn.track != now.track || drawn.version != now.version || drawn.markerX != now.markerX
             ? Region{0, kScreenW}
         : drawn.playheadX != now.playheadX ? movedRegion(drawn.playheadX, now.playheadX)
                                            : Region{0, 0};
}

// The text row as last drawn (Unknown: not since a repaint), and what a pass does
// to it. A look with no text blanks it once and draws nothing more, whatever
// changes meanwhile. A look with text draws it when it isn't up, or when what it
// says changed (`changed`).
enum class TextRow : uint8_t { Unknown, Shown, Blank };
enum class TextDraw : uint8_t { None, Draw, Blank };
constexpr TextDraw textDraw(Look look, TextRow row, bool changed) {
  return inksOf(look).text ? (row != TextRow::Shown || changed ? TextDraw::Draw : TextDraw::None)
                           : (row != TextRow::Blank ? TextDraw::Blank : TextDraw::None);
}

// Whether what a pass draws of the strip and of the text row goes to the screen as
// one push: the strip whole, and the text row due, to draw or to blank (a play
// state, a track, a repaint, a lift, a scrub's first frame). They are neighbours,
// in the sprite and on the screen: y 137-173, kStripAndTextH rows, the same px in
// one bus hold where two pushes take two. A part of the strip and the text row stay
// two: as one, the push would take the rest of the strip's width with it.
constexpr bool pushedAsOne(const Region& region, TextDraw text) {
  return region.w == kScreenW && text != TextDraw::None;
}

}  // namespace wave
