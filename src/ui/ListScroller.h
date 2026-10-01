// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <M5GFX.h>

#include <cstdint>

#include "VScrollMap.h"
#include "ui/LcdLock.h"

// Hardware vertical scrolling of one band of the Core2's LCD, for lists
// (docs/UI-SPIKE.md, "Scroll round 2"). The ILI9342C (and the later
// ILI9342E) can show its frame memory rotated by a start address (VSCRDEF
// 33h, VSCRSADD 37h). A list that keeps its content lines at fixed GRAM
// lines, rotated (VScrollMap), then scrolls by sending two bytes and drawing
// only the lines that became visible, instead of pushing the whole band.
//
// Why it works on the Core2 in our orientation: the panel is 320 x 240
// natively (M5GFX: memory and panel 320 x 240) and M5GFX's rotation 1 (the
// default, which the firmware keeps) maps to internal rotation 0 on this
// board (offset_rotation 3): MADCTL = BGR only, no MV/MX/MY/ML. So GRAM
// lines are the screen's rows, top first, and the controller's scroll axis
// (its 240 frame-memory lines, TFA + VSA + BFA = 240) is the screen's
// vertical. begin() refuses any other rotation.
//
// What a frame looks like (scrollTo()):
//
//   - A move of up to maxStep lines: the painter renders everything first
//     (the rows the new lines come from, and what must not move, like the
//     A-Z rail), off the bus. Then, in ONE bus hold: the new lines are
//     pushed into the GRAM lines of the lines leaving at the far edge (which
//     are still on screen until the address changes), the new start address
//     is sent, and what must not move is pushed back to its place. So the
//     wrong strip and the displaced rail are on screen for that bus time
//     only (plus any preemption by a higher-priority task: the decoder).
//   - A full redraw (the first frame, after invalidate(), or a move of more
//     than maxStep lines): the start address is kept and the content is
//     written in place, top to bottom, exactly like a plain redraw (in
//     slices, with yields); nothing on screen shifts or wraps.
//
// What the rest of the firmware has to know while it is active (between
// begin() and end()):
//
//   - Drawing outside the band (the tab bar, the header) is unaffected.
//   - Anything drawn into the band with plain M5GFX calls lands at GRAM
//     lines, which the panel shows elsewhere: draw a band-sized thing with
//     pushAtScreen(), or map lines with gramLineForScreen().
//   - readRect() reads GRAM: screenshots must map rows (app/Screenshot does,
//     with gramLineForScreen()).
//   - end() puts the start address back and leaves scroll mode, but the
//     band's GRAM is still in the rotated order: clear the band BEFORE
//     end(), or the rotated picture shows until the next redraw (the scroll
//     lab clears it first, then whoever draws next redraws its screen).
//
// Loop task only (the one task that draws). One at a time.
class ListScroller {
public:
  // What scrollTo() calls back. Spans never wrap.
  class Painter {
  public:
    // Before a move's commit: render everything the spans will push (row
    // sprites), without the bus. `spans` are consecutive content lines.
    virtual void prepare(const VScrollMap::Span* spans, int n) = 0;
    // Push content lines [span.contentY, + span.h) into GRAM lines
    // [span.gramY, + span.h). `committing`: inside the move's single bus
    // hold, so no yields (nested LcdLocks are fine: only the outer one takes
    // the bus). Otherwise (a full redraw in place): slices, each under its
    // own lock, with yields, rendering whatever isn't rendered yet.
    virtual void push(const VScrollMap::Span& span, bool committing) = 0;
    // What must not move with the list (the A-Z rail, an overlay): render
    // it for `offset` (off the bus), then push it with pushAtScreen(). Both
    // are called whenever the list moved; on a move, pushFixed() runs right
    // after the address, under the same hold. Default: nothing.
    virtual void prepareFixed(int32_t offset) { (void)offset; }
    virtual void pushFixed() {}

  protected:
    ~Painter() = default;
  };

  ~ListScroller() { end(); }

  // Scrolls screen lines [top, top + height) from now on (the rest fixed).
  // Sends VSCRDEF, and VSCRSADD = top, so nothing on screen moves yet. The
  // first scrollTo() draws the whole band. Moves of up to `maxStep` lines
  // (0: height - 1) are drawn incrementally in one bus hold; bigger ones are
  // full redraws in place (so maxStep bounds that hold). False (and nothing
  // sent) if the display isn't the Core2's in rotation 1, or another
  // scroller is active.
  bool begin(int top, int height, SpiHoldStats* stats = nullptr, int maxStep = 0);
  // VSCRSADD back to the top, then Normal Display Mode On (13h), which
  // leaves the scroll mode (datasheet 8.2.30 / 9.2.2). See the note above.
  void end();
  bool active() const { return active_ == this; }

  // Shows content lines [offset, offset + height) (see "What a frame looks
  // like" above). Returns the number of content lines pushed. `sendUs`
  // (optional) gets the time the address took on the bus (0 if unsent).
  int scrollTo(int32_t offset, Painter& painter, uint32_t* sendUs = nullptr);
  // The scroll area and the start address sent again, as they are (the
  // screen woke from the panel's sleep-in, which should keep them: this
  // makes sure). Nothing moves.
  void resend();
  // Something else drew over the band: the next scrollTo() redraws it all.
  void invalidate() { map_.invalidate(); }
  const VScrollMap& map() const { return map_; }

  // Pushes `sprite` so it shows at screen (x, screenY) with the start
  // address last sent: its lines inside the band go to the GRAM lines shown
  // there (split at the wrap), each piece under its own LcdLock (nested,
  // and so free, inside a Painter::pushFixed() on a move). For what must
  // not move with the list: the A-Z rail, overlays, toasts.
  // `h`: only its top h lines (0: all of it).
  void pushAtScreen(M5Canvas& sprite, int x, int screenY, SpiHoldStats* stats = nullptr, int h = 0);

  // The GRAM line the panel shows at screen line `y` now (identity when no
  // scroller is active), and the start address in use (0 when none).
  static int gramLineForScreen(int y);
  static bool anyActive() { return active_ != nullptr; }
  static uint16_t activeVsp();

private:
  void sendStartAddress(uint16_t vsp);

  static ListScroller* active_;
  VScrollMap map_;
  SpiHoldStats* stats_ = nullptr;
  uint16_t sentVsp_ = 0;
};
