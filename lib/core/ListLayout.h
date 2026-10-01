// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The items of a list screen and where they are (the tab bar spec's list,
// §6.2-6.3). Every item is one row pitch tall (42 px), so the content is
// uniform and the hardware scroll (VScrollMap) and the kinetic scroll
// (KineticScroll) work in plain pixels:
//
//   [top bar]     optional item 0: a container's Play / Play next / + Queue
//   row 0
//   row 1         <- expanded: a tap opened its inline action bar
//   [inline bar]  right under the expanded row (at most one)
//   row 2
//   ...
//
// Offsets are content pixels from the first item (0: item 0 at the top of
// the viewport). Also the A-Z rail's geometry: where its thumb sits for an
// offset, and the offset a scrub to a point asks for. Portable, host-tested.
class ListLayout {
public:
  static constexpr int kPitch = 42;

  enum class Kind : uint8_t { None, TopBar, Row, InlineBar };
  struct Item {
    Kind kind = Kind::None;
    int32_t row = -1;  // the data row (Row, InlineBar); -1 otherwise
  };

  // The list's data rows and whether it starts with a top bar. The
  // expanded row is kept if it still exists.
  void set(uint32_t rows, bool topBar);
  uint32_t rows() const { return rows_; }
  bool topBar() const { return topBar_; }

  uint32_t itemCount() const { return rows_ + (topBar_ ? 1u : 0u) + (expanded_ >= 0 ? 1u : 0u); }
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
  int32_t expanded_ = -1;
};
