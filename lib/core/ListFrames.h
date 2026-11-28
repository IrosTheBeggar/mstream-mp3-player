// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "KineticScroll.h"
#include "ListLayout.h"
#include "RowSlots.h"
#include "ScanSync.h"

// What a list does with a loop pass, and when a move's frame goes on the
// bus: the decisions ui/ListView's update(), scanFrame() and renderAhead()
// make, apart from the sprites, the bus and the clock, so a host test can
// take a list through a gesture pass by pass, with a clock of its own and
// something else on the core (test_list_frames: the rows a frame has to
// render itself, the frames that miss their refresh).
//
// The list keeps its state and hands it in (Pass, Look); what comes back is
// a decision it carries out. Which sprite a row takes and which row is
// rendered ahead are RowSlots'; where the panel's scan lets a hold begin is
// ScanSync's.
//
// Portable, host-tested.
namespace listframes {

constexpr int kPitch = ListLayout::kPitch;
// A move of up to this many lines is one bus hold of new lines (2 rows);
// more is a redraw in place.
constexpr int kMaxStep = 2 * kPitch;

// ---- the frames' timing on the panel's scan (ScanSync) ----
// A frame under a finger is taken up this long before its window opens (the
// loop's passes come a millisecond or two apart): it sleeps to the window,
// then holds the bus.
constexpr int32_t kNearUs = 1500;
// A list moving by itself sleeps up to this long for its frame's window (a
// finger's frames poll instead: its latest place counts).
constexpr int32_t kSleepMaxUs = 13000;
// A hold begun this long after its window closed still counts as in it (the
// window ends 8 scan lines before the band: half of that).
constexpr int32_t kLateOkUs = 230;
// A hold's time besides its lines (the window's commands, the clip).
constexpr uint32_t kHoldExtraUs = 60;

// ---- the scroll ----

// The list moves: a fling, a snap, or a finger that has dragged it (or
// caught it moving: `dragged`). A finger down that hasn't moved it isn't.
constexpr bool moving(KineticScroll::Phase p, bool dragged) {
  return p == KineticScroll::Phase::Flinging || p == KineticScroll::Phase::Snapping ||
         (p == KineticScroll::Phase::Dragging && dragged);
}

// The offset a frame draws now: where the scroll is (`at`), or a step
// towards it. `stepped`: the band is drawn and nothing is owed whole (no
// jump, no redraw), so a frame is a step of at most kMaxStep lines (one bus
// hold of new lines): a finger faster than that is followed a frame or two
// late instead of costing a full redraw. A jump or a redraw in place goes
// straight there. In whole rows (`rows`: the governor, when the audio is
// short of time) the rounding is part of the step, never on top of it.
inline int32_t frameOffset(int32_t drawn, int32_t at, bool stepped, bool rows) {
  if (stepped) {
    return rows ? ListLayout::stepTowardRows(drawn, at, kMaxStep, kPitch) : ListLayout::stepToward(drawn, at, kMaxStep);
  }
  return rows ? at / kPitch * kPitch : at;
}

// ---- a pass ----

// Where the finger is.
enum class Touch : uint8_t { None, List, Rail };

// What a pass finds (after the scroll's own update).
struct Pass {
  // The scroll.
  int32_t at = 0;      // where the scroll is
  int32_t drawn = -1;  // the offset on the glass; -1: nothing drawn yet
  KineticScroll::Phase phase = KineticScroll::Phase::Idle;
  bool dragged = false;  // this touch moved the list, or took hold of it while it moved
  // What is owed whole.
  bool force = false;       // the band is owed whole (a reload, an overlay gone)
  bool jumped = false;      // the scroll jumped (a scroll to a row, a scrub): no step towards it
  bool bandActive = false;  // a band is being drawn a row a pass
  bool staged = false;      // a recount's stale items are owed
  bool stageBand = false;   // ... and its jump is drawn as a band
  // The clock.
  bool frameDue = false;   // a frame may be drawn (the frame clock)
  bool wholeRows = false;  // the governor: move in whole rows
  // The edge (the A-Z rail, or the scroll bar).
  bool edge = false;        // the list has one
  Touch touch = Touch::None;
  bool pacing = false;      // a rail scrub is paced (ScrubPacer)
  bool pacerOwed = false;   // ... a position is owed
  bool pacerDue = false;    // ... and its band is due
  bool railSprite = false;  // the rail's sprite exists
  bool railUp = false;      // it is on the glass
  int32_t railOffset = -1;  // ... drawn for this offset (-1: owed again)
  // The panel.
  bool scan = false;      // the panel's scan can be followed (the rail aside)
  bool hwScroll = false;  // the hardware scroll is on and its map holds
  bool dark = false;      // the screen is off
  int32_t mapOffset = 0;  // the offset the hardware scroll shows
  int32_t maxStep = 0;    // the most it moves in one go
};

// What the pass draws of the band.
enum class Frame : uint8_t {
  None,     // nothing
  BandRow,  // a band under way: its next row, whatever the frame clock says
  Rail,     // the rail alone follows the finger (a paced scrub's position waits)
  OnScan,   // a move, timed to the panel's scan
  Band,     // a jump drawn a row a pass: its band begins (should it have none to draw, a Whole)
  Whole,    // a frame drawn now: a move off the scan, a jump, a redraw in place
  Stage,    // a recount's next stale item, at a still pass
};

struct Choice {
  Frame frame = Frame::None;
  int32_t off = 0;          // the offset the frame is for
  bool rows = false;        // ... moving in whole rows
  bool railWanted = false;  // the edge belongs on the glass now
  bool hideRail = false;    // first of all: the rail taken off it
  bool owed = false;        // a frame is owed (due or not)
};

// A jump to `off` is drawn a row a pass: under a finger on the rail (paced),
// or a recount's whose rows landed more than a move away. A jump the hardware
// scroll takes as a move is one hold already.
constexpr bool bandInRows(const Pass& p, int32_t off) {
  if (!(p.touch == Touch::Rail && p.pacing) && !p.stageBand) return false;
  if (p.force || p.drawn < 0 || p.dark || !p.hwScroll) return false;
  const int32_t d = off - p.mapOffset;
  return d > p.maxStep || -d > p.maxStep;
}

// The pass's choice. In its order:
//   - the rail sits in the scrolled band, so it is off the glass while the
//     list moves (still catching up with the finger, a step a frame, is
//     moving too) and back once it rests, or at once under a finger on it;
//   - a band under way goes on, a row a pass;
//   - a frame owed and due: under a paced scrub the rail alone until the
//     band is due; a step of a list that moves on the panel's scan when it
//     can be followed; a jump under the rail (or a recount's) as a band;
//     else drawn now;
//   - nothing owed: a recount's stale item, one a frame, at a still pass
//     (no finger, the scroll at rest).
inline Choice choose(const Pass& p) {
  Choice c;
  const bool mov = moving(p.phase, p.dragged);
  c.rows = p.wholeRows && mov;
  const bool stepped = !p.force && !p.jumped && p.drawn >= 0;
  c.off = frameOffset(p.drawn, p.at, stepped, c.rows);
  const bool catchingUp = stepped && c.off != p.at;
  c.railWanted = p.edge && (p.touch == Touch::Rail || (!mov && !catchingUp));
  c.hideRail = !c.railWanted && p.railUp;
  if (p.bandActive) {
    c.frame = Frame::BandRow;
    return c;
  }
  c.owed = c.off != p.drawn || p.force;
  if (c.owed && p.frameDue) {
    if (p.pacing && p.touch == Touch::Rail && !p.force && p.drawn >= 0 && p.pacerOwed && !p.pacerDue) {
      // (The rail there already, or none to draw: nothing.)
      c.frame = p.railOffset == c.off || !p.railSprite ? Frame::None : Frame::Rail;
    } else if (stepped && p.scan && !c.railWanted) {
      c.frame = Frame::OnScan;
    } else {
      c.frame = bandInRows(p, c.off) ? Frame::Band : Frame::Whole;
    }
    return c;
  }
  if (!c.owed && p.frameDue && p.staged && p.phase == KineticScroll::Phase::Idle && p.touch == Touch::None) {
    c.frame = Frame::Stage;
  }
  return c;
}

// After a pass that drew nothing of the band (or whose Stage pushed
// nothing): the edge at rest put up, or drawn again for a recount once its
// items are. Under a finger resting on the rail it comes up at once,
// whatever is owed of the items.
constexpr bool showRail(bool owed, Touch touch, bool staged, bool railWanted, bool railUp, int32_t railOffset) {
  return !owed && (touch == Touch::Rail || !staged) && railWanted && (!railUp || railOffset < 0);
}

// ---- a move on the panel's scan ----
//
// A move's frame is one bus hold, begun in the window ScanSync gives so the
// scan never shows it half done. In its order: the window for the step
// (window(), and whether the list is behind()); for a list moving by itself,
// where it will be when the frame shows (shownInUs()), which is where it is
// drawn, and the window for that step; the step's rows rendered; then
// start(): now, at the window, or not in this pass; the hold; and whether it
// was late().

// A hold's length up to its address: its lines on the bus (`lineUs` each) and
// what a hold takes besides.
constexpr uint32_t holdFor(int d, uint32_t lineUs) { return static_cast<uint32_t>(d < 0 ? -d : d) * lineUs + kHoldExtraUs; }

// The window for a move of `d` lines, the scan read at `line` at `readUs`.
// The refresh under way's: ahead, open, or just over (never the next one's:
// a window the loop comes too late for is not waited out, the frame is drawn
// at once, late, and the list keeps its pace). But one frame a refresh: once
// this refresh has its frame (`lastShowUs`: when the last one showed; 0:
// none), the next one's window.
inline ScanSync::Window window(const ScanSync& sync, int line, int64_t readUs, int64_t lastShowUs, int d,
                               uint32_t lineUs) {
  const int32_t refreshUs = static_cast<int32_t>(sync.panel().frameUs);
  const uint32_t hold = holdFor(d, lineUs);
  ScanSync::Window win = sync.plan(line, d, hold, static_cast<uint32_t>(refreshUs));
  if (lastShowUs && readUs + win.showsUs < lastShowUs + refreshUs / 2) {
    win = sync.plan(line, d, hold);
    if (readUs + win.showsUs < lastShowUs + refreshUs / 2) {  // (its window is still open)
      win.earliestUs += refreshUs;
      win.latestUs += refreshUs;
      win.addressUs += refreshUs;
      win.showsUs += refreshUs;
    }
  }
  return win;
}

// A refresh went by without a frame (the loop was held: a decoder's pass, a
// row rendered in the frame's way; or the list stood still): the frame isn't
// made to wait for its window on top of that.
constexpr bool behind(int64_t readUs, int32_t showsUs, int64_t lastShowUs, int32_t refreshUs) {
  return !lastShowUs || readUs + showsUs - lastShowUs > refreshUs + refreshUs / 2;
}

// How long from the scan's reading until the frame shows: where a list
// moving by itself is drawn, so frames a refresh apart move it a refresh's
// worth whenever in the refresh the loop got to them. (One drawn at once
// shows as its address goes out.)
constexpr int32_t shownInUs(const ScanSync::Window& w, bool isBehind, uint32_t holdUs) {
  return w.late || isBehind ? static_cast<int32_t>(holdUs) : w.showsUs;
}

// When the hold begins, its rows rendered (`nowUs`).
struct Start {
  bool atOnce = false;     // not kept for a window: its own is over, or the list is behind
  int64_t earliestUs = 0;  // the hold not before this
  int64_t addressUs = 0;   // the address not before this (0: as its lines are out)
  bool comeBack = false;   // the window opens later than a pass waits: nothing now, the loop comes back
};
// `coasting`: the list moves by itself, and sleeps to its window; under a
// finger the loop comes back for it (its passes read the finger meanwhile:
// the frame is drawn where it is then).
constexpr Start start(const ScanSync::Window& w, bool isBehind, int64_t readUs, int64_t nowUs, bool coasting) {
  Start s;
  s.atOnce = w.late || isBehind;
  s.earliestUs = s.atOnce ? nowUs : readUs + w.earliestUs;
  s.addressUs = s.atOnce ? 0 : readUs + w.addressUs;
  s.comeBack = s.earliestUs - nowUs > (coasting ? kSleepMaxUs : kNearUs);
  return s;
}

// The hold began at `beganUs`: late, when it was drawn at once or began
// after its window closed.
constexpr bool late(const ScanSync::Window& w, bool atOnce, int64_t readUs, int64_t beganUs) {
  return atOnce || beganUs > readUs + w.latestUs + kLateOkUs;
}

// What a line costs on the bus after a hold of `n` lines whose lines took
// `linesUs` (a short hold says little, and one a higher task cut into
// doesn't count).
constexpr uint32_t lineTime(uint32_t lineUs, int n, uint32_t linesUs) {
  if (n < 8 || linesUs <= kHoldExtraUs) return lineUs;
  const uint32_t per = (linesUs - kHoldExtraUs) / static_cast<uint32_t>(n);
  return per < lineUs * 2 ? (lineUs * 3 + per) / 4 : lineUs;
}

// ---- between two frames ----

// What the time between two frames is for.
enum class Look : uint8_t {
  None,    // nothing: no time, or nothing to be ahead of
  Unheld,  // a caught list: the row on screen that lost its sprite (rowslots::unheld())
  Ahead,   // a moving list: the row past the edge it moves toward (rowslots::ahead())
};

struct LookIn {
  int32_t at = 0;      // where the scroll is
  int32_t drawn = -1;  // the offset on the glass
  bool force = false, jumped = false;
  bool moving = false;   // moving()
  bool grabbed = false;  // a finger caught it moving, and hasn't moved it yet
  int lastDir = 0;       // the last frame's way (+1 down the list, -1 up; 0: none yet)
  int32_t items = 0;
  // The time there is.
  bool onScan = false;      // its frames follow the panel's scan
  int64_t sinceHoldUs = 0;  // ... the last hold began this long ago (-1: none yet)
  int64_t frameUs = 0;      // ... a refresh
  uint32_t untilDueMs = 0;  // else: to the frame clock's next deadline
  uint32_t rowUs = 0;       // what a row is taken to cost
};

// A row rendered between two frames, so the frame that brings it on only
// pushes. A caught list standing still under its finger gets the row its
// fling's look-ahead took (which way it goes next is the finger's; caught
// between two of its frames it owes the last one first). A list that moves
// renders a row ahead when there is time for one before the next frame: on
// the panel's scan a refresh after the last hold began, else by the frame
// clock's deadline.
constexpr Look look(const LookIn& l) {
  if (l.drawn < 0 || l.force || l.jumped || !l.moving || l.items == 0) return Look::None;
  if (l.grabbed) return l.at != l.drawn ? Look::None : Look::Unheld;
  if (l.lastDir == 0) return Look::None;
  if (l.onScan && l.sinceHoldUs >= 0) {
    if (!rowslots::roomOnScan(l.sinceHoldUs, l.frameUs, l.rowUs)) return Look::None;
  } else if (!rowslots::roomOnClock(l.untilDueMs, l.rowUs)) {
    return Look::None;
  }
  return Look::Ahead;
}

// The list rests, drawn where it is, nothing owed and no finger on it: the
// rows just outside the band are rendered then (rowslots::neighbour()), for
// the next motion's first frame.
struct RestIn {
  int32_t at = 0, drawn = -1;
  bool force = false, jumped = false, bandActive = false, dark = false;
  KineticScroll::Phase phase = KineticScroll::Phase::Idle;
  Touch touch = Touch::None;
  int32_t items = 0;
};
constexpr bool rests(const RestIn& r) {
  if (r.force || r.jumped || r.drawn < 0 || r.bandActive || r.dark) return false;
  if (r.phase != KineticScroll::Phase::Idle || r.touch != Touch::None || r.at != r.drawn) return false;
  return r.items > 0;
}

}  // namespace listframes
