#pragma once
#include <cstdint>

// The Core2's three touch buttons (A, B, C: the panel's strip below the
// LCD, raw y >= 240), made from the same touch samples as the glass, so a
// finger on the glass can never press one.
//
// M5Unified's own BtnA/B/C press a button for any point in the strip that
// isn't "moving", and once one is down a sliding finger adds its
// neighbours. So a swipe on the Queue that ended in the strip, where the
// panel lost the finger for a moment and found it again as a new touch,
// clicked B and C together (play/pause and next: the "random pause" of the
// device log). Here:
//
//   - only a touch that went DOWN in the strip presses a button, the one
//     under where it went down (raw x 0-106 A, 107-213 B, 214-319 C:
//     M5Unified's split), however the finger drifts after;
//   - moving more than slopPx (Chebyshev) from there cancels it: no click,
//     no hold, no more repeats (`cancelled`: ButtonGesture::cancel()); the
//     rest of that touch is ignored;
//   - a touch that went down on the glass never presses one, wherever it
//     goes (the glass recogniser follows it);
//   - a strip touch starting soon after a touch that wasn't a press lifted
//     (one from the glass, or a strip touch already ignored) is ignored: the
//     panel losing that finger and finding it again. Within bounceMs
//     anywhere; within swipeBounceMs if that touch had moved (a swipe) and
//     this one is within swipeBouncePx of where it was last seen. The
//     captured one came back within ~145 ms; a slower find, sliding less
//     than the slop into the next column, would otherwise click.
//
// Only one touch point is followed (the panel's first, as for the glass):
// a second finger pressing the strip while another touches the glass does
// nothing (Input logs it). When the first point becomes a different finger
// without a lift in between (`newTouch`: the first finger lifted while a
// second stayed, or the panel swapped fingers in one read), the old touch
// lifts and the new one goes down there and then, bounce rules and all.
//
// Each rejected strip touch is reported once (`ignored`), for the log.
// The points are what the panel read (screen pixels, not corrected by
// TouchCalibration): the same as M5Unified's split, and the strip test of
// TouchRecognizer. Portable: fed with timestamps, no clock of its own.
class StripButtons {
public:
  struct Config {
    int stripY = 240;
    int slopPx = 20;
    uint32_t bounceMs = 150;
    uint32_t swipeBounceMs = 400;
    int swipeBouncePx = 60;
  };

  enum class Why : uint8_t {
    None,
    Glass,   // a touch that went down on the glass reached the strip
    Moved,   // a strip press moved beyond the slop (cancelled)
    Bounce,  // a strip touch right after a touch that wasn't a press lifted
  };

  struct Result {
    int8_t pressed = -1;         // the button held down now: 0 A, 1 B, 2 C; -1 none
    bool cancelled = false;      // the press in progress was dropped in this update
    Why ignored = Why::None;     // a strip touch rejected in this update (once per touch)
    int8_t button = -1;          // ignored: the column it was over (cancelled: the press's)
    int16_t moved = 0;           // Moved: how far from where it went down
    uint32_t sinceLiftMs = 0;    // Bounce: since the touch before lifted
    int16_t fromLiftPx = 0;      // Bounce: how far from where that touch was last seen
    bool afterSwipe = false;     // Bounce: that touch had moved (the swipe window)
  };

  StripButtons() = default;
  explicit StripButtons(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  // One sample every loop pass: the finger (if any) at the panel's (x, y).
  // newTouch: it is a different finger from the last pass's (see above).
  Result update(uint32_t ms, bool pressed, int x, int y, bool newTouch = false);
  // The button held down now (-1: none).
  int pressed() const { return state_ == State::Pressing ? button_ : -1; }

  // The button under raw x (0 A, 1 B, 2 C).
  static int column(int x);
  static const char* name(Why w);

private:
  enum class State : uint8_t { Idle, Glass, Pressing, Ignored };

  void lift(uint32_t ms);
  void down(uint32_t ms, int x, int y, Result& r);

  Config config_;
  State state_ = State::Idle;
  bool reported_ = false;  // Glass: its reaching the strip was reported
  int8_t button_ = -1;
  int16_t downX_ = 0, downY_ = 0;
  int16_t lastX_ = 0, lastY_ = 0;
  bool moved_ = false;  // this touch went beyond the slop (or came back from a swipe's)
  // The last touch that lifted without being a press (glass or ignored).
  bool lost_ = false;
  uint32_t lostMs_ = 0;
  int16_t lostX_ = 0, lostY_ = 0;
  bool lostMoved_ = false;
};
