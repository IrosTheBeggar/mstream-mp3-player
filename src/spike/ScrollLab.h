#pragma once
#include <Arduino.h>
#include <M5GFX.h>

#include "KineticScroll.h"
#include "LibraryIndex.h"
#include "ScrollGovernor.h"
#include "app/Haptics.h"
#include "audio/Core2AudioBackend.h"
#include "ui/LcdLock.h"
#include "ui/ListScroller.h"

// UI spike: the scroll lab (console w, docs/UI-SPIKE.md). One tab-bar-styled
// list screen (36 px bar, a segmented header, 42 px two-line rows, the A-Z
// rail) over the LibraryIndex, virtualised: only the rows on screen are
// drawn. It answers the design's third risk: can a list flick-scroll while
// audio streams from the SD card that shares the LCD's SPI bus?
//
// Rendering, per the tabs spec's sketch: each row is drawn into an RGB565
// row sprite in PSRAM, then pushed to the LCD in slices of `sliceH` rows,
// each slice under its own LcdLock (the bus, and the SD card's mutex, held
// for that slice only), with a yield between slices. `cacheRows` row sprites
// (1 = the spec's single sprite, every row redrawn every frame; up to 6 keeps
// rows that are still on screen, so a scroll only pushes them). The frame
// rate is capped by the ScrollGovernor from the ring fill: 15 fps normally,
// fewer frames, whole-row steps, or none while the decoder's buffer is low.
//
// Modes: w0 interactive (drag, flick, rail scrub, header taps switch the
// view); w1 a 60 s stress of full-speed flicks up and down plus A-Z jumps,
// governor on; w2 the same with the governor off; w3 governor off and no
// frame cap (as fast as the bus goes). The stress keeps the list moving the
// whole time: a flick is reversed the moment it reaches an end, and an A-Z
// jump is followed by a flick at once. It needs a list of at least
// kMinStressPx of scrolling (about 50 rows; the card's 77 tracks, or g2000's
// 120 artists): on a shorter one it refuses. Start a track first (i<n>) so the SD card is being
// read. A [scroll] line per second: fps, the share of the second the list
// was moving and the fps over that time only, frame/draw/push ms, the
// longest SPI hold, ring fill minimum, underruns, decode load, internal
// heap minimum, governor level; a summary at the end of a stress.
//
// Scroll round 2 (docs/UI-SPIKE.md) adds three options, to A/B against the
// path above:
//   wm0  full redraw, as above (spike 1)
//   wm1  hardware vertical scroll (ui/ListScroller): the tab bar and header
//        are the fixed top area; a move of up to kHwMaxStep lines renders
//        first, then pushes only the newly exposed lines, sends the new
//        start address and puts the A-Z rail (a 30 x 168 PSRAM sprite) back
//        where it belongs, in one bus hold; bigger moves redraw in place
//   wm2  wm1 plus the interaction boost (wb1)
//   wb0/wb1  the interaction boost off/on, on either path: while the list
//        is touched or moving (and 300 ms after) and the ring holds >= 900
//        ms, the decode task yields core 1 to the loop
//        (Core2AudioBackend::setUiBoost); the loop then rests at least
//        kBoostRestMs after each frame so the decoder always gets time
//   wp0 / wp1 / wp<x10>  gentle refill at track starts off / on / on at
//        x10/10 times realtime, 15-40 (Core2AudioBackend::setRefillPacing;
//        it stays set after the lab closes)
// For each track start while the lab is open it logs the longest frame and
// the longest loop gap in the 3 s after it ("[scroll] track start: ...");
// the backend logs how the ring filled ("[audio] refill: ...").
//
// Console (Enter after each): w toggles; w0-w3 modes; wv<0-2> view
// (artists, albums, tracks); wf<fps> the normal frame cap (0: none); wh<px>
// slice height (1-42); wc<n> cached row sprites (1-6; the hardware path
// uses all 6); wd<s> stress length; wg0/wg1 governor
// off/on; wm<0-2> render path; wb0/wb1 boost; wp<n> refill pacing; ws the
// settings and last summary; wq closes.
class ScrollLab : private ListScroller::Painter {
public:
  ScrollLab(Core2AudioBackend& audio, Haptics& haptics);
  ~ScrollLab();

  bool open(LibraryIndex* index, int mode);
  void close();
  bool active() const { return active_; }
  void command(const char* arg, LibraryIndex* index);
  void loop(uint32_t nowMs);
  // The index is about to change under us (a rebuild): close first.
  void indexChanging() { close(); }

private:
  enum View : uint8_t { Artists, Albums, Tracks };
  static constexpr int kMaxSlots = 6;
  static constexpr int kMaxSeconds = 300;
  // A stress needs a list that scrolls: below kMinStressPx it refuses (the
  // card's artists and albums); below kShortStressPx (a 4,000 px/s flick
  // runs ~1 s before an end) it runs with a note (the card's 77 tracks).
  static constexpr int kMinStressPx = 2000;
  static constexpr int kShortStressPx = 4000;
  // While boosted, the loop rests at least this long after each frame
  // (from the frame's end, whatever the cap; w3 has none), so the lowered
  // decoder gets a defined share of core 1. At priority 0 it shares that
  // rest with IDLE1 (time slicing), so it gets about half of it.
  static constexpr uint32_t kBoostRestMs = 20;
  // The hardware path draws moves of up to this many lines incrementally, in
  // one bus hold (2 rows: ~10.5 ms idle, plus ~2 ms for the rail); bigger
  // moves are full redraws in place (see ListScroller).
  static constexpr int kHwMaxStep = 2 * 42;
  // A track start is watched this long for the longest frame and loop gap.
  static constexpr uint32_t kStartWatchMs = 3000;
  enum class Path : uint8_t { Redraw, Hardware };

  struct Slot {
    M5Canvas sprite;
    int32_t row = -1;  // the list row drawn in it
  };
  struct Second {
    float fps;
    float movingFps;  // frames over the time the list was moving
    float moving;     // share of the second the list was moving (0-1)
    float frameMaxMs;
    float lockMaxMs;
    uint16_t ringMinMs;
    uint16_t underruns;
    float decode;
    uint32_t heapMin;
    float boosted;    // share of the second the boost was on (0-1)
    float gapMaxMs;   // longest time between two passes of the lab's loop
  };

  bool createSlots();
  void dropSlots();
  uint32_t count() const;
  bool railShown() const { return view_ != Tracks && count() * 42 > 168; }
  int rowWidth() const { return railShown() ? 290 : 320; }
  void enterScreen();
  void drawHeader();
  void drawRail(int offset, bool force);
  void drawRow(M5Canvas& s, uint32_t i);
  Slot* slotFor(uint32_t row, uint32_t firstVisible, uint32_t lastVisible);
  void renderFrame(int offset);
  // The hardware-scroll path (wm1/wm2).
  bool hw() const { return path_ == Path::Hardware; }
  bool startHw();
  void stopHw(bool clearBand);
  void renderFrameHw(int offset);
  // ListScroller::Painter, for the hardware path.
  void prepare(const VScrollMap::Span* spans, int n) override;
  void push(const VScrollMap::Span& span, bool committing) override;
  void prepareFixed(int32_t offset) override;
  void pushFixed() override;
  // The row slot for `row`, rendered.
  Slot* renderedSlot(int32_t row);
  bool createRailSprite();
  void drawRailSprite(int offset);
  void setPath(int m);
  void pollBoost();
  void pollTrackStart(uint32_t nowMs, uint32_t dtMs);
  void setView(View v);
  void handleTouch(uint32_t nowMs);
  void stressStep(uint32_t nowMs);
  void perSecond(uint32_t nowMs, bool force);
  void finishStress();
  void printSettings();

  Core2AudioBackend& audio_;
  Haptics& haptics_;
  LibraryIndex* index_ = nullptr;
  bool active_ = false;
  int mode_ = 0;
  View view_ = Artists;

  Slot* slots_ = nullptr;  // PSRAM array of kMaxSlots
  int cacheRows_ = 1;
  int sliceH_ = 14;
  uint32_t normalFrameMs_ = 66;
  uint32_t stressMs_ = 60000;
  // The stress's flick speed (wk<px/s>): 4,000 is spike 1's "full speed";
  // ~1,000 keeps most frames under kHwMaxStep lines, like a drag.
  float flickPxPerS_ = 4000.0f;
  bool governed_ = true;
  Path path_ = Path::Redraw;
  bool boost_ = false;

  ListScroller vscroll_;
  M5Canvas* rail_ = nullptr;  // PSRAM, the hardware path's rail (30 x 168)
  // Rows whose slots the frame being drawn must keep: the rows on screen,
  // or, while a move is prepared, the rows its new lines come from.
  uint32_t keepFirst_ = 0, keepLast_ = 0;
  bool railDue_ = false;  // prepareFixed() rendered the rail: pushFixed() pushes it

  KineticScroll scroll_;
  ScrollGovernor gov_;
  ScrollGovernor::Budget budget_;
  int drawnOffset_ = -1;
  bool forceFrame_ = true;
  uint32_t lastFrameMs_ = 0;
  uint32_t lastFrameEndMs_ = 0;
  int railThumbY_ = -1;
  char railKey_ = 0;
  int railBucket_ = -1;

  // Touch.
  bool touching_ = false;
  enum class TouchOn : uint8_t { None, List, Rail, Header, Bar } touchOn_ = TouchOn::None;
  int touchX_ = 0, touchY_ = 0;
  uint32_t touchDownMs_ = 0;

  // Stress.
  bool stress_ = false;
  uint32_t stressStartMs_ = 0;
  uint32_t nextActionMs_ = 0;
  uint32_t actions_ = 0;
  int flingDir_ = 1;
  bool reflickAfterJump_ = false;  // a jump was drawn: flick on at the next pass

  // Per-second numbers.
  uint32_t secStartMs_ = 0;
  uint32_t frames_ = 0, rowsDrawn_ = 0, slicesPushed_ = 0;
  uint64_t frameUs_ = 0, drawUs_ = 0, pushUs_ = 0;
  uint32_t frameMaxUs_ = 0;
  uint32_t movingMs_ = 0;       // this second: the list moving (or a frame owed)
  uint32_t heldMs_ = 0;         // ... of which the governor held frames back (level above normal)
  uint32_t lastLoopMs_ = 0;
  SpiHoldStats lock_;
  uint32_t ringMin_ = UINT32_MAX;
  uint32_t heapMin_ = UINT32_MAX;
  uint32_t linesPushed_ = 0;  // hardware path: lines drawn this second
  uint32_t boostMs_ = 0;      // this second, while the boost was on
  uint32_t gapMaxMs_ = 0;     // this second, the longest loop gap
  uint32_t underruns0_ = 0;
  uint64_t busy0_ = 0;
  // The stress run.
  Second* seconds_ = nullptr;  // PSRAM
  uint32_t nSeconds_ = 0;
  uint32_t runUnderruns0_ = 0;
  char lastSummary_[400] = "";

  // Boost changes already logged.
  uint32_t boostSeen_ = 0;
  // The track start being watched (kStartWatchMs).
  uint32_t startSeen_ = 0;
  bool watching_ = false;
  uint32_t watchStartMs_ = 0;
  uint32_t watchFrames_ = 0;
  uint32_t watchFrameMaxUs_ = 0;
  uint32_t watchGapMaxMs_ = 0;
  uint32_t lastFrameUs_ = 0;  // the last frame's length: a start's stall is usually in it
};
