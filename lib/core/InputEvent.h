#pragma once
#include <cstdint>

// What the input layer (src/ui/Input) hands the screens: one clean event per
// thing that happened, on the glass or on a touch button. Nothing
// downstream reads the touch panel or BtnA/B/C itself.
//
// Glass coordinates are screen pixels, already corrected (TouchCalibration)
// before any hit test; rawX/rawY are what the panel read (the calibration
// screen wants those). Portable: the recognisers in lib/core fill it in.
struct InputEvent {
  enum class Type : uint8_t {
    None,
    // ---- the glass ----
    Down,       // a finger landed at (x, y): highlight what's under it
    Tap,        // lifted within the slop and before the hold: act; (x, y) is where it landed
    LongPress,  // still down at the hold time, within the slop; (x, y) where it landed
    Release,    // lifted after a LongPress (no tap)
    DragStart,  // moved beyond the slop: (x, y) now, (dx, dy) from where it landed
    DragMove,   // (x, y) now, (dx, dy) since the last Drag event
    DragEnd,    // lifted after a drag: (x, y) the last point, (vx, vy) the release velocity, capped
    Fling,      // right after a DragEnd whose speed is a flick: the same (vx, vy)
    Cancel,     // the touch was dropped (Input::cancelTouch): no more events from it
    // ---- the touch buttons (button 0 A, 1 B, 2 C) ----
    Click,      // released before the hold time
    Hold,       // the hold time reached, still down
    Repeat,     // every repeat period after the Hold, still down (A and C)
    HoldEnd,    // released after a Hold
  };
  // The screen edge a clamped reading sits at: the panel stops at 0 and 319,
  // so the finger may have been further out (see TouchCalibration).
  enum Edge : uint8_t { kEdgeNone = 0, kEdgeLeft = 1, kEdgeRight = 2 };

  Type type = Type::None;
  uint8_t button = 0;  // buttons: 0 A, 1 B, 2 C
  uint8_t edges = 0;   // glass: Edge bits of the point
  uint8_t repeat = 0;  // Repeat: how many so far (1, 2, ...; saturates at 255)
  int16_t x = 0, y = 0;
  int16_t dx = 0, dy = 0;
  int16_t rawX = 0, rawY = 0;
  float vx = 0, vy = 0;  // px/s
  uint32_t ms = 0;

  bool isButton() const { return type >= Type::Click; }
  bool isGlass() const { return type != Type::None && type < Type::Click; }
  bool atRightEdge() const { return (edges & kEdgeRight) != 0; }
  // A hit test for a control at the right edge of the screen (x from `left`
  // to the edge): a clamped reading counts wherever it was corrected to.
  bool inRightEdgeZone(int left) const { return x >= left || atRightEdge(); }

  static const char* name(Type t);
};
