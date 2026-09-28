#pragma once

// Where a sheet sits (ui/Overlays' Sheet: a title with a close pill, then
// rows, from the bottom of the screen): every row kRowH, the 40 px touch
// minimum, however many rows. Up to 3 rows it stays in the list's band
// (from y 72); Now Playing's "..." sheet has 4 (the sleep timer first) and
// rises into the header row (from y 40), as the dialog and the jump grid
// do, never onto the tab bar. A 4 px margin keeps its last row off the
// touch strip. The panel sprite the sheets are drawn in (overlaysBegin())
// is kPanelH tall: the content area, the tallest any overlay needs.
// Portable, host-tested (test_ui_nav).
namespace sheet {

inline constexpr int kScreenH = 240;
inline constexpr int kContentY = 36;  // below the tab bar
inline constexpr int kTitleH = 36;
inline constexpr int kRowH = 40;
inline constexpr int kMaxRows = 4;
inline constexpr int kMargin = 4;
inline constexpr int kPanelH = kScreenH - kContentY;  // 204

// The sheet's height with `rows` rows (title, rows, margin).
constexpr int height(int rows) { return kTitleH + rows * kRowH + kMargin; }
// Its top on the screen.
constexpr int top(int rows) { return kScreenH - height(rows); }

static_assert(top(kMaxRows) >= kContentY, "a sheet never covers the tab bar");
static_assert(height(kMaxRows) <= kPanelH, "the panel sprite holds the tallest sheet");

}  // namespace sheet
