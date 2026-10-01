// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

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
// second while playing, flat when paused, flat in amber while play waits for
// the headphones, a note when nothing is loaded)
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

// BtIdle: Bluetooth is the output, not connected, and nothing is looking
// for them (the background search rests; they come back by themselves):
// the headphones in the plain icon colour, not amber.
enum class Output : uint8_t { Speaker, BtConnected, BtConnecting, BtLost, BtIdle };

// The Output icon's state. `lost`: dropped while the output (until back or
// the output moves); `failed`: a connection the listener asked for failed;
// `asked`: one is on its way (the audio waits on the speaker meanwhile);
// `looking`: the link is trying (paging, scanning, backing off), not
// resting or off.
Output outputFor(bool onBluetooth, bool connected, bool lost, bool asked, bool failed, bool looking);
enum class Play : uint8_t { Nothing, Stopped, Paused, Playing, Waiting };
// The sleep timer's badge on the Now Playing cell (a 7 x 7 moon in its
// top-right corner): none, running (counting or armed), fading (amber).
enum class Sleep : uint8_t { None, Running, Fading };

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
  Sleep sleep = Sleep::None;       // the sleep timer's moon (no minutes: no redraw as they pass)
};
constexpr int kMoonPx = 7;         // the sleep timer's badge
constexpr int kMoonRight = 25;     // its right edge, from the Now Playing icon's centre
constexpr int kProgressW = 32;

// Where the bar's texts go in a cell (x from the cell's left) and how
// wide each may be. The host test measures every text any state can show
// with the real fonts (DejaVu Sans 13) against these, so none is cut on
// the device ("Playi…" and the battery's "10…" were, before).
constexpr int kLabelW = kTabW - 2;           // the active tab's label, on its plate
constexpr int kOutputPlateX = 3;             // the Output tab's plate: its icon and the volume
constexpr int kOutputPlateW = 64;
constexpr int kVolumeX = 31;                 // "100%" beside the output icon
constexpr int kVolumeW = 36;
constexpr int kBatteryX = 288 - kOutputX;    // the battery icon (288 on screen), 22 x 11
constexpr int kBatteryTextW = 38;            // its "100%", centred under it (kBatteryX + 12)
constexpr int kBadgeRight = 23;              // the Queue badge's right edge, from the icon's centre
constexpr int kBadgePad = 8;                 // the badge pill: its text + this
inline int labelWidth(int tab) { return tab == kTabs - 1 ? kOutputPlateW : kLabelW; }
extern const char* const kLabels[kTabs];     // "Playing", "Library", "Queue", "Dance", "Output"

// Bit t: tab cell t must be redrawn to go from `drawn` to `now`.
uint8_t dirty(const State& drawn, const State& now);
constexpr uint8_t kAll = (1u << kTabs) - 1u;

// The four EQ bars' heights (1-kEqMaxH px) at an animation step: a
// fixed-looking pseudo-random dance, the same for a step every time.
constexpr int kEqMaxH = 16;
void eqBars(uint8_t step, bool playing, uint8_t out[4]);

// The badge's text: "7", "99", "99+" ("" for 0).
void badgeText(uint32_t n, char out[4]);

// (outputFor(): TabBarModel.cpp.)

// The hairline's fill for a position in a track of `durationMs` (0: unknown).
uint8_t progressPx(uint32_t positionMs, uint32_t durationMs);

}  // namespace tabbar
