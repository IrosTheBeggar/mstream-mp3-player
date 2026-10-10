// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <functional>
#include <vector>

#include "InputEvent.h"
#include "LibraryIndex.h"
#include "app/Haptics.h"
#include "app/Library.h"
#include "audio/Core2AudioBackend.h"
#include "storage/LocalStorage.h"

class Input;
class InputLab;
class ScrollLab;
class FontProbe;
class ThumbProbe;
class TagConsole;

// The UI spike's pieces behind one door (docs/UI-SPIKE.md): the input lab
// (u), the scroll lab (w), the library index and its probe (g), the font
// probe (e) and the thumbnail probe (j). Each lab is created in PSRAM the
// first time it's used, so the spike costs no internal RAM until then. At
// most one owns the screen at a time; main.cpp stops drawing the now-playing
// and dance screens while one does, and redraws when it lets go.
//
// The library index is the app's (app/Library, the player's single
// store): g reports it, g0 rebuilds it from the card (through the callback
// main.cpp gives, which carries the queue across). g<n> builds a synthetic
// library of n tracks in an index of the spike's own, which the scroll lab
// and the thumbnail probe then use instead, until g0; the player never sees
// it. The tags' commands (gs, gt, gr, gw, gb, gv: tagtext, docs/METADATA.md
// 3.3.6) go to app/TagConsole, made in PSRAM at its first use.
//
// The screens get the glass from the input layer (ui/Input: corrected,
// as events) through onGlass(), like every other screen. The input lab is
// the exception: it measures the raw panel and M5Unified's own buttons, so
// it reads them itself (ownsInput(): the input layer sends nothing then).
class Spike {
public:
  Spike(Core2AudioBackend& audio, Haptics& haptics, LocalStorage& storage, Library& library, Input& input);

  void loop(uint32_t nowMs);
  // A spike screen is up: the app's own screens must not draw.
  bool ownsScreen() const;
  // The input lab is open: BtnA/B/C and the glass are its alone.
  bool ownsInput() const;
  // A glass event from the input layer: true if a spike screen took it.
  bool onGlass(const InputEvent& e);
  // Closes whatever spike screen is up (another screen takes the display).
  void closeAll() { closeAllBut(Screen::None); }
  // Called when the last spike screen closes (the app redraws its own).
  void onScreenReleased(std::function<void()> fn) { released_ = std::move(fn); }
  // g0: rebuilds the library (main.cpp wraps Library::rebuild() so the
  // queue follows).
  void onRebuild(std::function<bool()> fn) { rebuild_ = std::move(fn); }

  // Console commands (the text after the letter).
  void inputLab(const char* arg);
  void scrollLab(const char* arg);
  void index(const char* arg);
  void fontProbe(const char* arg);
  void thumbProbe(const char* arg);

  // What the labs show: the synthetic library if g<n> made one, else the
  // app's.
  LibraryIndex* libraryIndex();
  // The synthetic library g<n> made (nullptr: none). The UI's Library can
  // browse it (console uil<n>); g0 drops it.
  LibraryIndex* synthetic() { return synth_ && synth_->ready() ? synth_ : nullptr; }
  // The tags' console (nullptr: no PSRAM): the card worker hands it its
  // jobs (TagConsole::setJobs()).
  TagConsole* tags();

private:
  enum class Screen : uint8_t { None, Input, Scroll, Font, Thumb };
  void closeAllBut(Screen keep);
  bool buildSynthetic(uint32_t tracks);
  void report();
  void dropSynthetic();

  Core2AudioBackend& audio_;
  Haptics& haptics_;
  LocalStorage& storage_;
  Library& library_;
  Input& inputLayer_;
  LibraryIndex* synth_ = nullptr;  // PSRAM, g<n> only
  InputLab* input_ = nullptr;      // PSRAM, on first use
  ScrollLab* scroll_ = nullptr;
  FontProbe* font_ = nullptr;
  ThumbProbe* thumb_ = nullptr;
  TagConsole* tags_ = nullptr;     // PSRAM, on first use
  std::function<void()> released_;
  std::function<bool()> rebuild_;
  bool owned_ = false;

  // The synthetic build, for the report.
  struct Build {
    uint32_t files = 0, added = 0;
    float addMs = 0, finishMs = 0;
    int32_t psramUsed = 0;
    int32_t internalDelta = 0;    // free after - free before
    uint32_t internalMinDuring = 0;
    uint32_t internalFreeBefore = 0;
  } build_;
};
