#pragma once
#include <cstdint>

// The tab bar's layout, what it shows, and what must be redrawn when that
// changes (the tab bar spec §3.2, with the review's grafts and the user's
// measurements). Portable, host-tested; ui/TabBar draws it.
//
// Layout (x, inclusive), 36 px tall at the top of the screen:
//
//   Now Playing 0-53 | Library 54-107 | Queue 108-161 | Dance 162-215 | Output 216-319
//
// The Output cell reaches the screen's edge and holds the output icon, the
// VOLUME beside it and the battery: the user's panel reads the right side
// up to 45 px too far right (and stops at 319), and a separate volume chip
// in the corner took every tap meant for the Output tab next to it. So
// there is no other target right of 216, and a reading clamped at the edge
// is the Output tab whatever it is corrected to.
//
// Status in the bar: the Now Playing icon is live (EQ bars moving 4 times a
// second while playing, flat when paused, a note when nothing is loaded)
// with a progress hairline under it; the Queue icon has an up-next badge,
// which flashes for 1.5 s when tracks are added; the Output icon's colour
// is the Bluetooth state; the active tab has a plate, an underline and a
// text label in its section's colour.
namespace tabbar {

constexpr int kTabs = 5;
constexpr int kHeight = 36;
constexpr int kTabW = 54;
constexpr int kOutputX = 4 * kTabW;  // 216

inline int cellX0(int tab) { return tab * kTabW; }
inline int cellX1(int tab) { return tab < kTabs - 1 ? cellX0(tab) + kTabW - 1 : 319; }  // inclusive
// The tab under a (corrected) x; a reading clamped at the right edge is the
// Output tab.
inline int tabAt(int x, bool atRightEdge) {
  if (atRightEdge || x >= kOutputX) return kTabs - 1;
  if (x < 0) return 0;
  return x / kTabW;
}

enum class Output : uint8_t { Speaker, BtConnected, BtConnecting, BtLost };
enum class Play : uint8_t { Nothing, Stopped, Paused, Playing };

// Everything the bar shows. Two states compare cell by cell (dirty()).
struct State {
  uint8_t active = 0;              // the current tab
  Play play = Play::Nothing;
  uint8_t eqStep = 0;              // the EQ animation's step (4 a second while playing)
  uint8_t progressPx = 0;          // the hairline's fill, 0-kProgressW
  bool progressKnown = false;      // false: dotted (unknown duration)
  uint16_t upNext = 0;             // the Queue badge
  bool badgeFlash = false;         // tracks were just added
  Output output = Output::Speaker;
  uint8_t volume = 0;              // the active output's, 0-100
  uint8_t battery = 0;             // 0-100
  bool charging = false;
  bool lowBlink = false;           // battery at 10 % or less: its blink phase
};
constexpr int kProgressW = 32;

// Bit t: tab cell t must be redrawn to go from `drawn` to `now`.
uint8_t dirty(const State& drawn, const State& now);
constexpr uint8_t kAll = (1u << kTabs) - 1u;

// The four EQ bars' heights (1-kEqMaxH px) at an animation step: a
// fixed-looking pseudo-random dance, the same for a step every time.
constexpr int kEqMaxH = 16;
void eqBars(uint8_t step, bool playing, uint8_t out[4]);

// The badge's text: "7", "99", "99+" ("" for 0).
void badgeText(uint32_t n, char out[4]);

// The hairline's fill for a position in a track of `durationMs` (0: unknown).
uint8_t progressPx(uint32_t positionMs, uint32_t durationMs);

}  // namespace tabbar
