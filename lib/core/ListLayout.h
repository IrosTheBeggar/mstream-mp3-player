// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The items of a list screen and where they are (the tab bar spec's list,
// §6.2-6.3). Every item is one row pitch tall (42 px), so the content is
// uniform and the hardware scroll (VScrollMap) and the kinetic scroll
// (KineticScroll) work in plain pixels:
//
//   [top row]     optional: a row of the source's own ahead of the top bar
//                 (Most played's tray: LISTENING-STATS.md 7.2 item 1)
//   [top bar]     optional: a container's Play / Play next / + Queue
//   row 0
//   row 1         <- expanded: a tap opened its inline action bar
//   [inline bar]  right under the expanded row (at most one)
//   row 2
//   ...
//
// The top row and the top bar are the lead items (0-2 of them). Offsets are
// content pixels from the first item (0: item 0 at the top of the
// viewport). Also the A-Z rail's geometry: where its thumb sits for an
// offset, and the offset a scrub to a point asks for. Portable, host-tested.
class ListLayout {
public:
  static constexpr int kPitch = 42;

  // (TopRow last: the others keep their values.)
  enum class Kind : uint8_t { None, TopBar, Row, InlineBar, TopRow };
  struct Item {
    Kind kind = Kind::None;
    int32_t row = -1;  // the data row (Row, InlineBar); -1 otherwise
  };

  // The list's data rows, whether it starts with a top bar, and whether a
  // top row comes ahead of that. The expanded row is kept if it still exists.
  void set(uint32_t rows, bool topBar, bool topRow);
  // Today's form, every list but the stats shelves': no top row.
  void set(uint32_t rows, bool topBar) { set(rows, topBar, false); }
  uint32_t rows() const { return rows_; }
  bool topBar() const { return topBar_; }
  bool topRow() const { return topRow_; }
  // The items ahead of row 0.
  uint32_t leads() const { return (topRow_ ? 1u : 0u) + (topBar_ ? 1u : 0u); }

  uint32_t itemCount() const { return rows_ + leads() + (expanded_ >= 0 ? 1u : 0u); }
  int32_t contentPx() const { return static_cast<int32_t>(itemCount()) * kPitch; }
  int32_t maxOffset(int32_t viewportPx) const {
    const int32_t m = contentPx() - viewportPx;
    return m > 0 ? m : 0;
  }
  Item item(uint32_t index) const;
  // The item index of data row `row` (and of its inline bar: + 1).
  uint32_t itemOfRow(uint32_t row) const;
  // The item at content line `contentY`; -1 past the end (or before 0).
  int32_t itemAt(int32_t contentY) const;

  int32_t expanded() const { return expanded_; }
  // Expands `row` (-1: collapses) and returns the offset that keeps the
  // list steady: the tapped row stays where it was on screen, then moves
  // just enough for its bar to be fully visible, all within the list.
  int32_t setExpanded(int32_t row, int32_t offset, int32_t viewportPx);
  // The offset that shows item `index` fully, moving as little as possible
  // from `offset`.
  int32_t reveal(uint32_t index, int32_t offset, int32_t viewportPx) const;
  // The offset that puts item `index` at the top (clamped to the list).
  int32_t topOf(uint32_t index, int32_t viewportPx) const;
  static int32_t clamp(int32_t offset, int32_t maxOffset) {
    return offset < 0 ? 0 : offset > maxOffset ? maxOffset : offset;
  }
  // The offset to draw a moving list at: `target`, or at most `maxStep`
  // from `drawn` towards it (the hardware scroll's one-hold step: a bigger
  // move would be a full redraw).
  static int32_t stepToward(int32_t drawn, int32_t target, int32_t maxStep) {
    if (target > drawn + maxStep) return drawn + maxStep;
    if (target < drawn - maxStep) return drawn - maxStep;
    return target;
  }
  // The same while the list moves in whole rows (the audio is short of
  // time): `target` to a whole row, then at most `maxStep` from `drawn`,
  // and a capped step ends on the whole row nearest `drawn` within it, so
  // the rounding never makes the step longer (from an unaligned `drawn`,
  // rounding after the cap could add most of a row: a full redraw).
  // `maxStep` must be at least a pitch.
  static int32_t stepTowardRows(int32_t drawn, int32_t target, int32_t maxStep, int32_t pitch) {
    const int32_t t = target / pitch * pitch;
    const int32_t s = stepToward(drawn, t, maxStep);
    if (s == t) return s;
    return s > drawn ? s / pitch * pitch : (s + pitch - 1) / pitch * pitch;
  }

  // ---- the rail (a scrollbar thumb, or the A-Z rail's letter thumb) ----
  // The thumb's top in a track of `trackPx` for `offset`, a thumb of `thumbPx`.
  static int thumbTop(int32_t offset, int32_t maxOffset, int trackPx, int thumbPx);
  // The offset a finger at `y` (in the track) asks for, snapped to a whole
  // row pitch: the thumb's centre follows the finger.
  static int32_t scrubOffset(int y, int32_t maxOffset, int trackPx, int thumbPx);

private:
  uint32_t rows_ = 0;
  bool topBar_ = false;
  bool topRow_ = false;  // (in the padding after topBar_)
  int32_t expanded_ = -1;
};
// ListView's layout_, inside ui::Ui: the top row took a padding byte,
// nothing more, so ui::Ui keeps main's carve (LISTENING-STATS.md 7.2,
// "Where S3b's UI bytes live").
static_assert(sizeof(ListLayout) == 12, "ListLayout grew");

// The pure rules a list follows when its rows change under it, beside the
// layout (LISTENING-STATS.md 7.2, as decided): its edge, where a shelf's
// rows land, and what a recount leaves stale on screen. Host-tested.
namespace listlayout {

// The list's right edge: nothing, a thin scrollbar, or the A-Z rail.
enum class Edge : uint8_t { None, Rail, Bar };
// A long alphabetical list gets the A-Z rail (the review: only above about
// 30 entries).
constexpr uint32_t kRailMinRows = 30;

// ListView::setEdge()'s rule: the rail for a long A-Z list over 30 rows, a
// scrollbar for any other list longer than the viewport, else nothing.
// `railRows`: the source's rows sorted by name.
constexpr Edge edgeOf(int32_t contentPx, int32_t heightPx, bool alphabetical, uint32_t railRows) {
  return contentPx <= heightPx                       ? Edge::None
         : alphabetical && railRows > kRailMinRows ? Edge::Rail
                                                     : Edge::Bar;
}
// The rows' width beside the edge: 320, 312 beside the bar, 284 beside the rail.
constexpr int widthOf(Edge e) { return e == Edge::Rail ? 284 : e == Edge::Bar ? 312 : 320; }

// Where a page's rows land (7.2, "An open bar across a new ask"): the
// ref's place put back over the rows that came (0 to N, a pass or two
// after the push; the ref was kept while the rows were pending).
//   - The scroll is kept, clamped to the new items.
//   - Within the visit (`sameVisit`) no row moved, so the bar comes back if
//     its row is there. Across a new ask (a tab back, a wake, Try again)
//     rows can move (a play counted meanwhile), so it never does: a bar
//     reopened by its index could act on a track the listener didn't pick.
//   - A scroll below 0 (-1, the page's own default; kShowPlaying, -2) comes
//     back as it went in: the caller resolves it (playingRow()).
// `leads`: the lead items (0-2).
struct Place {
  int32_t offset = 0;
  int32_t expanded = -1;
};
Place arrival(int32_t refScroll, int32_t refExpanded, uint32_t rows, uint32_t leads, bool sameVisit,
              int32_t heightPx);

// What a recount leaves stale on screen (7.2 item 4, as decided: a recount
// pushes one stale item a pass, never the whole band), at the drawn offset.
// A position is stale when its item changed (kind, row), when its data row
// is at or past `changedRow` (the first row whose picture may differ: 0 for
// all, as at an arrival or a tray tap; from's rows() for only the new
// ones), when the edge changed (every row's width), or when it held the old
// empty line's ground (a list with no rows: everything under its lead
// items) and the new list doesn't draw its own there. A blank ground under
// the new empty line isn't: that line is the caller's to draw, after the
// pushes, when `to` has no rows.
struct Staging {
  // The positions to push one a pass, top first: [first, end), none when
  // first == end. Any between that aren't stale are pushed as well (a
  // superset, never a miss). A position at or past `to`'s items is cleared.
  uint32_t first = 0, end = 0;
  // Item 0 is a lead item (the top row, or a top bar) in both and on
  // screen, and its picture changed (`leadChanged`: the tray's 30-day cell
  // now reading "16 days" at a landing, the entry row's line) or the edge
  // did: the caller redraws it in place (refreshTopRow), before the rest.
  // [first, end) never holds it then.
  bool lead = false;
  bool edge = false;  // the edge changes (320, 312, 284 px)
  // The edge is drawn again: it changed, or the list's height did under a
  // scrollbar or rail (the thumb's size and place follow it).
  bool rail = false;
  // The drawn offset is past `to`'s end: the list has to move as well,
  // which a push in place can't do.
  bool moves = false;
};
Staging staging(const ListLayout& from, Edge fromEdge, const ListLayout& to, Edge toEdge, int32_t offset,
                int32_t heightPx, uint32_t changedRow, bool leadChanged);

}  // namespace listlayout
