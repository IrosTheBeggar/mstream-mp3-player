#pragma once
#include <Arduino.h>

#include "TouchCalibration.h"
#include "TouchGesture.h"
#include "app/Haptics.h"
#include "ui/LcdLock.h"

// UI spike: the input lab (console u, docs/UI-SPIKE.md). Measures how
// people actually press the Core2's three touch buttons (BtnA/B/C: regions
// of the touch panel below the LCD, which M5Unified turns into buttons) and
// tap the glass near them, before any screen is built on assumptions.
//
// While it is open it owns the screen, the glass and the buttons (they are
// logged, not acted on: a B-hold would switch the output). Modes:
//   u0  free: a live readout; every press and touch is logged
//   u1  tab target practice: the tab bar and the bottom-row controls of the
//       tabs design (Queue edit bar, the Undo toast, the last list row);
//       prompts name a target, each tap is logged as hit or miss, its offset
//       from the target's centre, and any BtnA/B/C event within 300 ms
//   u2  button practice: prompts to click, then hold, each of A/B/C; logs
//       the durations, M5Unified's reading, and glass touches at y 220-239
//       around the press: the finger that pressed the button was on the
//       glass first (landed there and rolled down), or left it for the glass
//       (the button released as it rolled up), or another finger touched
//       there during the press or within 300 ms before it
//   u3  haptics: tiles that play tick patterns (15/25/40 ms at two
//       strengths, and the spec's double tick and buzzes)
// Console (Enter after each): u toggles; u0-u3 open a mode; us prints the
// summary of everything logged since boot; ur clears it; uh<ms>[,<level>
// [,<count>,<gap>]] plays a haptic pattern; ut<ms> sets M5Unified's button
// hold threshold (default 500) for this session.
//
// Log lines: [btn] per press (down, M5Unified's hold, up with duration and
// classification, its click-count decision), [touch] per glass touch
// (tap/hold/drag/flick, duration, movement, release velocity), [target] per
// u1 attempt, [haptic] per pattern. Samples are kept in PSRAM (the lab
// itself is psramNew'd), so internal RAM stays untouched.
//
// The lab measures the panel as it is: its coordinates are the raw ones
// (what M5Unified converts), never corrected. Since the input layer
// corrects every touch (TouchCalibration), each u1 attempt also says where
// the correction in use puts the press and whether that hits: "cal=(x,y)
// cal_off=(dx,dy) cal_hit=yes|no" (a raw x of 319 is marked "clamped").
class InputLab {
public:
  explicit InputLab(Haptics& haptics);
  ~InputLab();

  bool ready() const { return buttons_ && targets_ && touches_; }
  void open(int mode);
  void close();
  bool active() const { return active_; }
  int mode() const { return mode_; }
  // A console argument (the text after 'u').
  void command(const char* arg);
  // Every loop pass, after M5.update().
  void loop(uint32_t nowMs);
  void summary();
  // The correction the input layer applies (for the u1 lines); null: none.
  void setCalibration(const TouchCalibration* c) { cal_ = c; }
  void resetStats();

private:
  enum Mode { Free = 0, Targets = 1, Buttons = 2, HapticTest = 3 };
  enum Zone : uint8_t { ZoneTabs, ZoneChip, ZoneLastRow, ZoneEditBar, ZoneUndo, kZones };
  enum Scene : uint8_t { SceneList, SceneEdit, SceneToast, kScenes };
  enum Intent : uint8_t { IntentNone, IntentClick, IntentHold };

  struct ButtonSample {
    uint8_t button;
    uint8_t intent;          // what u2 asked for (IntentNone in other modes)
    uint8_t prompted;        // the button u2 asked for (0-2), 255: none
    bool m5Click, m5Hold;
    bool glassBottom;        // a glass touch at y 220-239 around the press (see pollTouches)
    bool sameFinger;         // ... by the finger that pressed the button
    int16_t touchDownY;      // where that finger first touched (>= 240: on the strip itself)
    uint16_t leadMs;         // from that touch-down to the button's press
    uint16_t durMs;
    uint32_t atMs;
  };
  struct TargetSample {
    uint8_t target;
    bool hit;
    bool falseButton;        // a BtnA/B/C event within 300 ms of the tap
    bool buttonOnly;         // a button fired and no touch was recorded for it
    bool strip;              // the press landed in the button strip (y >= 240)
    bool hasOffset;          // dx, dy are known (a touch, or the button's raw point)
    uint8_t kind;            // TouchGesture::Kind
    int16_t dx, dy;          // tap (press point) minus the target's centre
    uint16_t durMs;
  };
  struct TouchSample {
    uint8_t kind;
    uint16_t durMs;
    uint16_t movePx;
    uint16_t speed;          // px/s
    int16_t y;
  };
  struct BtnTrack {
    bool down = false;
    uint32_t downMs = 0;
    int16_t x = -1, y = -1;  // the raw touch point that pressed it
    bool sawHold = false;
    uint32_t holdAfterMs = 0;
    bool glassBottom = false;
    bool sameFinger = false;
    int16_t glassY = -1;
    int16_t touchDownY = -1; // the pressing finger's first point
    uint32_t leadMs = 0;     // its touch-down to the button's press
    // After the release: a glass sample in the band within 300 ms (the
    // finger rolled up off the strip) is added to the logged sample.
    uint32_t afterUntilMs = 0;
    int32_t sampleIdx = -1;
  };
  struct TouchTrack {
    bool active = false;
    bool glass = true;       // started on the LCD (y < 240), not the button strip
    uint32_t lastSeenMs = 0;
    uint32_t downMs = 0;
    int16_t downY = -1;
    int16_t bandY = -1;      // its last sample at y 220-239 (-1: none)
    TouchGesture gesture;
  };
  struct Pending {           // a u1 attempt waiting out the 300 ms button window
    bool active = false;
    bool buttonOnly = false;
    uint32_t decideAtMs = 0;
    uint32_t fromMs = 0, toMs = 0;  // the touch (or button press) span
    TouchGesture::Result touch;
    int button = -1;
    int16_t x = -1, y = -1;  // button-only: the raw point that pressed it
  };

  void drawFrame();
  void drawFree();
  void drawScene(Scene scene);
  void drawPrompt();
  void drawButtonPrompt();
  void drawHaptics();
  void nextTarget();
  void nextButtonPrompt();
  void pollButtons(uint32_t nowMs);
  void pollTouches(uint32_t nowMs);
  void onTouchEnd(const TouchGesture::Result& r, bool glass, uint32_t nowMs);
  void finishAttempt(uint32_t nowMs);
  int targetAt(int x, int y) const;
  void noteButtonEvent(uint32_t ms);
  int nearestButtonEventMs(uint32_t from, uint32_t to, int* button) const;
  void playHaptic(int tile);
  void readout(const char* line);

  Haptics& haptics_;
  const TouchCalibration* cal_ = nullptr;
  bool active_ = false;
  int mode_ = Free;

  // Samples since boot (PSRAM).
  static constexpr uint32_t kMaxSamples = 1024;
  ButtonSample* buttons_ = nullptr;
  TargetSample* targets_ = nullptr;
  TouchSample* touches_ = nullptr;
  uint32_t nButtons_ = 0, nTargets_ = 0, nTouches_ = 0;

  BtnTrack btn_[3];
  TouchTrack touch_[5];
  uint32_t btnEvents_[16] = {};  // recent button down/up times
  uint8_t btnEventWho_[16] = {};
  int btnEventHead_ = 0;

  // u1
  uint8_t deck_[64] = {};
  int deckLen_ = 0, deckPos_ = 0;
  int target_ = -1;
  Scene scene_ = SceneList;
  Pending pending_;
  uint32_t promptShownMs_ = 0;
  uint32_t nextPromptAtMs_ = 0;  // after an attempt: the feedback shows until then
  uint32_t lastGlassEndMs_ = 0;
  bool glassDown_ = false;       // a glass touch is on now
  uint32_t lastBandMs_ = 0;      // the last touch sample at y 220-239, any finger
  int16_t lastBandY_ = -1;
  SpiHoldStats drawLock_;        // the lab's own screen draws (bus holds)
  uint32_t attempts_ = 0;
  // u2
  int promptButton_ = 0;
  Intent promptIntent_ = IntentClick;
  uint8_t bdeck_[6] = {};
  int bdeckPos_ = 6;
  bool promptLive_ = false;      // u2: a prompt is showing (not the result pause)
  // u3
  uint32_t hapticReportAtMs_ = 0;
  int lastHaptic_ = -1;
  // u0 readout
  int readoutLine_ = 0;
  int16_t lastDotX_ = -1, lastDotY_ = -1;
};
