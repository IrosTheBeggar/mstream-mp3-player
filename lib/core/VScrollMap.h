#pragma once
#include <cstdint>

// The bookkeeping of an LCD controller's hardware vertical scroll (MIPI DCS
// VSCRDEF 33h / VSCRSADD 37h, as on the Core2's ILI9342C), for a list that
// scrolls through a band of the screen. Portable: no display here, the
// caller sends the commands and draws what plan() asks for.
//
// The controller shows its frame memory (GRAM) lines [0, top) and
// [top + height, 240) where they are (the fixed areas), and fills screen
// lines [top, top + height) from GRAM line `vsp` onwards, wrapping within
// the scroll area:
//
//   screen line top + k  shows  GRAM line top + (vsp - top + k) mod height
//
// Each content line c (0 = the list's first pixel line) is kept at GRAM line
// top + ((c - base) mod height), and vsp = top + ((offset - base) mod
// height). Then screen line top + k shows content line offset + k, whatever
// the offset. Moving the list by d lines (|d| <= maxStep) only needs the d
// newly exposed content lines written, into the GRAM lines of the d lines
// that just left the other edge, and vsp changed; nothing else is copied.
//
// A full redraw (the first plan, after invalidate(), or a move of more than
// maxStep lines) re-picks `base` so that vsp stays what the panel already
// has: the new content is then written straight into the screen lines where
// it will show, top to bottom, like a plain redraw, and never shows wrapped
// or shifted while it is being drawn (no address change is needed).
class VScrollMap {
public:
  // A band of content lines to draw into GRAM lines [gramY, gramY + h),
  // contentY first. Never wraps (plan() splits at the wrap).
  struct Span {
    int32_t contentY = 0;
    int32_t gramY = 0;
    int32_t h = 0;
  };
  static constexpr int kMaxSpans = 2;

  // The scroll area: `top` fixed lines above it, `height` lines of it.
  // `maxStep`: the largest move drawn incrementally (the rest are full
  // redraws in place); <= 0 or >= height means height - 1. Forgets what
  // was drawn (the next plan() draws the whole area), and assumes the panel
  // is at the identity (vsp = top), as after VSCRDEF + VSCRSADD = top.
  void configure(int top, int height, int maxStep = 0);
  int top() const { return top_; }
  int height() const { return height_; }
  int maxStep() const { return maxStep_; }

  // The next plan() redraws the whole area (after anything else drew over
  // it). The panel's start address is unchanged, and so is vsp().
  void invalidate() { valid_ = false; }
  bool valid() const { return valid_; }

  // The content lines to draw for the list to show content [offset, offset +
  // height), into `out` (up to kMaxSpans); returns how many. Afterwards
  // vsp() is the start address to send (unchanged after a full redraw), and
  // the map assumes the caller drew every span.
  int plan(int32_t offset, Span out[kMaxSpans]);
  // Whether the last plan() was a full redraw (in place, vsp unchanged).
  bool lastPlanFull() const { return lastFull_; }

  // The content offset the GRAM holds (valid() only).
  int32_t offset() const { return offset_; }
  // The start address (VSP) to send / in use: top + shift.
  uint16_t vsp() const { return static_cast<uint16_t>(top_ + shift_); }
  // The GRAM line that screen line `y` shows at vsp(), and the screen line
  // that shows GRAM line `gramY` (identity outside the scroll area).
  int gramLineForScreen(int y) const;
  int screenLineForGram(int gramY) const;
  // The GRAM line where content line `c` lives.
  int gramLineForContent(int32_t c) const;

private:
  // Adds [c, c + n) to out, split where it wraps; returns spans used.
  int emit(int32_t c, int32_t n, Span* out) const;

  int top_ = 0;
  int height_ = 1;
  int maxStep_ = 0;
  int32_t offset_ = 0;
  int32_t base_ = 0;  // content line kept at GRAM line top
  int shift_ = 0;     // vsp - top, in [0, height): what the panel has (or gets next)
  bool valid_ = false;
  bool lastFull_ = false;
};
