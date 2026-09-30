#pragma once
#include <cstddef>
#include <cstdint>

#include "TouchCalibration.h"

// The rules of the touch check and the calibration flow (ui/CalibrationScreen
// draws them): portable, host-tested (test_touch_input).
//
// The default is no correction (TouchCalibration::defaults()), and panels
// differ: the user's reads x up to ~45 px too far right, another may read
// true. So a Core2 with no saved table asks once, after its first boot,
// before the tips: tap three dots, one at a time, where the lab's panel was
// 0, +20 and +40 px off (x 50, 190, 280). If the taps say the panel is off,
// it offers the calibration (9 crosses); if not, "Touch is accurate".
namespace touchcheck {

// ---- the first-boot check ----

struct Dot {
  int16_t x, y;
};
inline constexpr int kDots = 3;
// Away from the header and the rows the result page puts under them (y 104
// to 136), one at a time.
inline constexpr Dot kDot[kDots] = {{50, 112}, {190, 128}, {280, 104}};

// A tap on a dot: where the Core2 read it (the table in use: none, while
// the check is due) and whether the panel's x was at its clamp (0 or 319).
struct Tap {
  int16_t x = 0, y = 0;
  bool clamped = false;
};

// Off by this much the tap is asked again, once (a finger that missed, not
// the panel); the second one counts wherever it lands.
inline constexpr int kAskAgainPx = 90;
// The verdict: calibrate when a dot is off by more than kFarPx, or when
// two of the three are off by more than kOffPx (one careless 16 px tap on
// an accurate panel isn't enough), or when a dot away from the edges read
// the clamp (only a panel far off does that).
inline constexpr int kFarPx = 30;
inline constexpr int kOffPx = 15;
inline constexpr int kEdgeMarginPx = 30;

// How far a tap is from its dot (px, rounded).
int offBy(const Dot& d, const Tap& t);
bool askAgain(const Dot& d, const Tap& t);

enum class Dir : uint8_t { None, Right, Left, Below, Above };
struct Verdict {
  bool calibrate = false;
  int px = 0;             // about how far off (rounded to 5), in `dir`
  Dir dir = Dir::None;    // the way the taps land from the finger
};
// `taps`: one per kDot, in order.
Verdict verdict(const Tap* taps, int n = kDots);
// "Taps land about 40 px to the right of your finger." (the result page
// wraps it over two lines: UiText's room).
void verdictText(const Verdict& v, char* buf, size_t size);

// Whether the check shows after this boot: no table saved, and it hasn't
// been answered (NVS "input"/"cal_ask": done, skipped, or walked away from).
bool due(bool calibrated, bool answered);

// ---- the calibration (9 crosses) ----

// Spread over the screen, with distinct x and y each, so that any first
// 5-9 of them give each axis as many points; the right side (where the
// lab's panel is off the most) gets its share. Clear of the two ways out on
// the glass (below): no cross's sample window reaches the header's Cancel,
// and none is drawn in the A hint's corner (x < 190, y > 210).
inline constexpr int kCrosses = 9;
inline constexpr Dot kCross[kCrosses] = {{160, 120}, {20, 80},   {300, 200}, {90, 170}, {230, 60},
                                         {55, 195},  {265, 140}, {125, 95},  {195, 185}};

// A tap is a sample for the cross when the panel read it within this
// (raw px) of the cross: the lab's panel is up to ~45 px off.
inline constexpr int kAcceptDx = 70;
inline constexpr int kAcceptDy = 50;
// After two misses on the same cross, a third tap within kAgreePx of the
// second is taken, up to kAgreeMaxPx from the cross: two taps that agree
// are the panel's reading, not the finger's (a panel off by more than
// kAcceptDx could never pass a cross otherwise).
inline constexpr int kAgreePx = 20;
inline constexpr int kAgreeMaxPx = 120;
// The two ways out on the glass, judged before the cross (the table's
// reading): the header's Cancel (x < 110, y < 26: top left, where a panel
// that reads right carries a tap away from it, not onto it), which no
// cross's sample window reaches; and the "A: Cancel" label over the A dot
// (x < 200, y >= 222: UiText's kCalHintY), at least 22 px below every
// cross (y reads true: a tap aimed at a cross doesn't read that low).
inline constexpr int kCancelW = 110;
inline constexpr int kCancelH = 26;
inline constexpr int kAHintW = 200;
inline constexpr int kAHintY = 222;

struct CrossTries {
  int misses = 0;
  int16_t lastX = 0, lastY = 0;  // the last miss's reading (raw)
};
enum class Take : uint8_t { Sample, Miss, Cancel };
// A settled tap (the raw reading; `x`, `y` the table's) while cross (tx, ty)
// is up: the Cancel (either way out), a sample, or a miss (asked again:
// `tries` counts it).
Take judgeCross(CrossTries& tries, int tx, int ty, int rawX, int rawY, int x, int y);

// How far the taps are from their crosses with a table (px, 2D).
struct Error {
  float max = 0, mean = 0;  // mean: of the distances
};
Error measure(const TouchCalibration& table, const TouchCalibration::Sample* sx, const TouchCalibration::Sample* sy,
              int n);
// How far each tap is from its cross with a table fitted to the other taps
// (leave-one-out, the x and y knots of fitAxis()): what a table fitted to
// these taps does on a tap it didn't see. Measured on the taps it was
// fitted to, a new table always looks better: 9 x knots follow 9 taps'
// finger scatter, and a panel that reads true would be told to save a
// table fitted to noise (worse than none on the next taps). 2n small fits,
// once, on the loop task (the log says how long they took).
Error measureUnseen(const TouchCalibration::Sample* sx, const TouchCalibration::Sample* sy, int n);

// What the result page says, from the table in use (`now`) and the new one
// on taps it didn't see (`unseen`: measureUnseen()). The fit is kept only
// if its rms is within kMaxRmsAfter on both axes (else Disagree: "the taps
// didn't agree"). Better only when the new table is at least kMinGainPx
// closer on average; else Accurate when the taps land within
// kAccurateMeanPx on average with the table in use (a panel that reads
// true: ordinary finger scatter), or NoBetter. Only Better puts Save first.
inline constexpr float kMaxRmsAfter = 15.0f;
inline constexpr float kMinGainPx = 3.0f;
inline constexpr float kAccurateMeanPx = 8.0f;
enum class Outcome : uint8_t { Disagree, Accurate, NoBetter, Better };
Outcome outcome(bool fitOk, const Error& now, const Error& unseen);

// "Now: up to 42 px off, average 21" / "Calibrated: up to 5 px off, average
// 3" (UiText's rooms).
void errorText(const char* what, const Error& e, char* buf, size_t size);

}  // namespace touchcheck
