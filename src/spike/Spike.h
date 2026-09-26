#pragma once
#include <Arduino.h>

#include <functional>
#include <vector>

#include "LibraryIndex.h"
#include "Track.h"
#include "app/Haptics.h"
#include "audio/Core2AudioBackend.h"
#include "storage/LocalStorage.h"

class InputLab;
class ScrollLab;
class FontProbe;
class ThumbProbe;

// The UI spike's pieces behind one door (docs/UI-SPIKE.md): the input lab
// (u), the scroll lab (w), the library index and its probe (g), the font
// probe (e) and the thumbnail probe (j). Each lab is created in PSRAM the
// first time it's used, so the spike costs no internal RAM until then. At
// most one owns the screen at a time; main.cpp stops drawing the now-playing
// and dance screens while one does, and redraws when it lets go.
//
// The library index (LibraryIndex in PSRAM) is built from the SD card at
// boot and rebuilt by g0; g<n> replaces it with a synthetic library of n
// tracks; g alone reports what it holds and what today's track list costs.
class Spike {
public:
  Spike(Core2AudioBackend& audio, Haptics& haptics, LocalStorage& storage);

  // Builds the library index from the card (logged as [index]).
  void begin();
  void loop(uint32_t nowMs);
  // A spike screen is up: the app's own screens must not draw.
  bool ownsScreen() const;
  // The input lab is open: BtnA/B/C and the glass are its alone.
  bool ownsInput() const;
  // Called when the last spike screen closes (the app redraws its own).
  void onScreenReleased(std::function<void()> fn) { released_ = std::move(fn); }

  // Console commands (the text after the letter).
  void inputLab(const char* arg);
  void scrollLab(const char* arg);
  void index(const char* arg);
  void fontProbe(const char* arg);
  void thumbProbe(const char* arg);

  // Today's track list (main.cpp's `library` and the playlist copy): what
  // it measured around building them, for g's report.
  void setTrackListCost(uint32_t tracks, int32_t libraryMeasured, int32_t playlistMeasured, uint32_t libraryEstimate,
                        uint32_t playlistEstimate);
  // Internal-RAM bytes a Track list holds (vector + strings past libstdc++'s
  // 15-byte in-place buffer, with ESP-IDF's per-block overhead).
  static uint32_t estimateBytes(const std::vector<Track>& tracks);

  LibraryIndex* libraryIndex() { return index_; }

private:
  enum class Screen : uint8_t { None, Input, Scroll, Font, Thumb };
  void closeAllBut(Screen keep);
  bool buildReal();
  bool buildSynthetic(uint32_t tracks);
  void report();
  bool ensureIndex();

  Core2AudioBackend& audio_;
  Haptics& haptics_;
  LocalStorage& storage_;
  LibraryIndex* index_ = nullptr;  // PSRAM
  InputLab* input_ = nullptr;      // PSRAM, on first use
  ScrollLab* scroll_ = nullptr;
  FontProbe* font_ = nullptr;
  ThumbProbe* thumb_ = nullptr;
  std::function<void()> released_;
  bool owned_ = false;

  // The last build, for the report.
  struct Build {
    bool synthetic = false;
    uint32_t files = 0, added = 0;
    float walkMs = 0, addMs = 0, finishMs = 0;
    int32_t psramUsed = 0;
    int32_t internalDelta = 0;    // free after - free before
    uint32_t internalMinDuring = 0;
    uint32_t internalFreeBefore = 0;
  } build_;
  uint32_t listTracks_ = 0;
  int32_t listMeasured_ = 0, playlistMeasured_ = 0;
  uint32_t listEstimate_ = 0, playlistEstimate_ = 0;
};
