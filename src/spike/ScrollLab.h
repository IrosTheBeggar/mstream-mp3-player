#pragma once
#include <Arduino.h>
#include <M5GFX.h>

#include "KineticScroll.h"
#include "LibraryIndex.h"
#include "ScrollGovernor.h"
#include "app/Haptics.h"
#include "audio/Core2AudioBackend.h"
#include "ui/LcdLock.h"

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
// Console (Enter after each): w toggles; w0-w3 modes; wv<0-2> view
// (artists, albums, tracks); wf<fps> the normal frame cap (0: none); wh<px>
// slice height (1-42); wc<n> cached row sprites (1-6); wd<s> stress
// length; wg0/wg1 governor off/on; ws the settings and last summary; wq
// closes.
class ScrollLab {
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
  bool governed_ = true;

  KineticScroll scroll_;
  ScrollGovernor gov_;
  ScrollGovernor::Budget budget_;
  int drawnOffset_ = -1;
  bool forceFrame_ = true;
  uint32_t lastFrameMs_ = 0;
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
  uint32_t underruns0_ = 0;
  uint64_t busy0_ = 0;
  // The stress run.
  Second* seconds_ = nullptr;  // PSRAM
  uint32_t nSeconds_ = 0;
  uint32_t runUnderruns0_ = 0;
  char lastSummary_[320] = "";
};
