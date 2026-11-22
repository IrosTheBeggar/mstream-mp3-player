// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ListLayout.h"

void ListLayout::set(uint32_t rows, bool topBar, bool topRow) {
  rows_ = rows;
  topBar_ = topBar;
  topRow_ = topRow;
  if (expanded_ >= 0 && static_cast<uint32_t>(expanded_) >= rows_) expanded_ = -1;
}

ListLayout::Item ListLayout::item(uint32_t index) const {
  Item it;
  if (index >= itemCount()) return it;
  if (topRow_) {
    if (index == 0) {
      it.kind = Kind::TopRow;
      return it;
    }
    --index;
  }
  if (topBar_) {
    if (index == 0) {
      it.kind = Kind::TopBar;
      return it;
    }
    --index;
  }
  if (expanded_ >= 0) {
    const auto e = static_cast<uint32_t>(expanded_);
    if (index == e + 1) {
      it.kind = Kind::InlineBar;
      it.row = expanded_;
      return it;
    }
    if (index > e + 1) --index;
  }
  it.kind = Kind::Row;
  it.row = static_cast<int32_t>(index);
  return it;
}

uint32_t ListLayout::itemOfRow(uint32_t row) const {
  uint32_t i = row + leads();
  if (expanded_ >= 0 && row > static_cast<uint32_t>(expanded_)) ++i;
  return i;
}

int32_t ListLayout::itemAt(int32_t contentY) const {
  if (contentY < 0) return -1;
  const int32_t i = contentY / kPitch;
  return i < static_cast<int32_t>(itemCount()) ? i : -1;
}

int32_t ListLayout::setExpanded(int32_t row, int32_t offset, int32_t viewportPx) {
  if (row >= static_cast<int32_t>(rows_)) row = -1;
  if (row == expanded_) return clamp(offset, maxOffset(viewportPx));
  // Keep a row where it is on screen, shifting by how far its item moves:
  // the tapped row when expanding; collapsing, the row at the top of the
  // viewport (the expanded one if its bar is at the top).
  int32_t anchor = row;
  if (anchor < 0) {
    const int32_t at = itemAt(offset < 0 ? 0 : offset);
    anchor = at >= 0 ? item(static_cast<uint32_t>(at)).row : -1;
  }
  int32_t o = offset;
  if (anchor >= 0) {
    const int32_t before = static_cast<int32_t>(itemOfRow(static_cast<uint32_t>(anchor))) * kPitch;
    expanded_ = row;
    const int32_t after = static_cast<int32_t>(itemOfRow(static_cast<uint32_t>(anchor))) * kPitch;
    o += after - before;
  } else {
    expanded_ = row;  // only lead items above: nothing on screen moves
  }
  o = clamp(o, maxOffset(viewportPx));
  if (row >= 0) o = reveal(itemOfRow(static_cast<uint32_t>(row)) + 1, o, viewportPx);
  return o;
}

int32_t ListLayout::reveal(uint32_t index, int32_t offset, int32_t viewportPx) const {
  const int32_t top = static_cast<int32_t>(index) * kPitch;
  const int32_t bottom = top + kPitch;
  int32_t o = offset;
  if (bottom > o + viewportPx) o = bottom - viewportPx;
  if (top < o) o = top;
  return clamp(o, maxOffset(viewportPx));
}

int32_t ListLayout::topOf(uint32_t index, int32_t viewportPx) const {
  return clamp(static_cast<int32_t>(index) * kPitch, maxOffset(viewportPx));
}

int ListLayout::thumbTop(int32_t offset, int32_t maxOffset, int trackPx, int thumbPx) {
  const int travel = trackPx - thumbPx;
  if (maxOffset <= 0 || travel <= 0) return 0;
  const int32_t o = clamp(offset, maxOffset);
  return static_cast<int>((static_cast<int64_t>(o) * travel + maxOffset / 2) / maxOffset);
}

int32_t ListLayout::scrubOffset(int y, int32_t maxOffset, int trackPx, int thumbPx) {
  const int travel = trackPx - thumbPx;
  if (maxOffset <= 0 || travel <= 0) return 0;
  int t = y - thumbPx / 2;
  if (t < 0) t = 0;
  if (t > travel) t = travel;
  const int64_t raw = static_cast<int64_t>(t) * maxOffset / travel;
  // Whole rows, rounded to the nearest; the end stays reachable.
  int32_t o = static_cast<int32_t>((raw + kPitch / 2) / kPitch * kPitch);
  if (t == travel) o = maxOffset;
  return clamp(o, maxOffset);
}

namespace listlayout {

Place arrival(int32_t refScroll, int32_t refExpanded, uint32_t rows, uint32_t leads, bool sameVisit,
              int32_t heightPx) {
  Place p;
  p.expanded = sameVisit && refExpanded >= 0 && static_cast<uint32_t>(refExpanded) < rows ? refExpanded : -1;
  if (refScroll < 0) {
    p.offset = refScroll;
    return p;
  }
  const uint32_t items = leads + rows + (p.expanded >= 0 ? 1u : 0u);
  const int32_t m = static_cast<int32_t>(items) * ListLayout::kPitch - heightPx;
  p.offset = ListLayout::clamp(refScroll, m > 0 ? m : 0);
  return p;
}

namespace {

bool isLead(ListLayout::Kind k) { return k == ListLayout::Kind::TopRow || k == ListLayout::Kind::TopBar; }

// Position `p` (no edge change, not the caller's lead item): stale?
bool stale(const ListLayout& from, const ListLayout& to, uint32_t p, uint32_t changedRow) {
  using Kind = ListLayout::Kind;
  const ListLayout::Item was = from.item(p), is = to.item(p);
  if ((is.kind == Kind::Row || is.kind == Kind::InlineBar) && static_cast<uint32_t>(is.row) >= changedRow) {
    return true;
  }
  if (was.kind != is.kind || was.row != is.row) return true;
  if (is.kind != Kind::None) return false;
  // Past both lists' items: the ground. The old empty line (with no rows,
  // drawn somewhere under the lead items: ListView::drawEmpty()) goes
  // unless the new one is drawn in its place; a blank ground needs nothing.
  return from.rows() == 0 && (to.rows() != 0 || from.leads() != to.leads());
}

}  // namespace

Staging staging(const ListLayout& from, Edge fromEdge, const ListLayout& to, Edge toEdge, int32_t offset,
                int32_t heightPx, uint32_t changedRow, bool leadChanged) {
  Staging s;
  s.edge = fromEdge != toEdge;
  s.rail = s.edge || (toEdge != Edge::None && from.contentPx() != to.contentPx());
  const int32_t off = offset > 0 ? offset : 0;
  s.moves = off > to.maxOffset(heightPx);
  if (heightPx <= 0) return s;
  constexpr int32_t kPitch = ListLayout::kPitch;
  // The positions on screen: each one whose 42 lines meet the viewport's.
  uint32_t p = static_cast<uint32_t>(off / kPitch);
  const auto past = static_cast<uint32_t>((off + heightPx + kPitch - 1) / kPitch);
  if (p == 0 && isLead(from.item(0).kind) && from.item(0).kind == to.item(0).kind) {
    s.lead = leadChanged || s.edge;
    p = 1;
  }
  for (; p < past; ++p) {
    if (!s.edge && !stale(from, to, p, changedRow)) continue;
    if (s.first == s.end) s.first = p;
    s.end = p + 1;
  }
  return s;
}

}  // namespace listlayout
