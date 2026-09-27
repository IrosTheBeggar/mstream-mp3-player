#pragma once
#include <Arduino.h>
#include <M5GFX.h>

#include "ui/LcdLock.h"
#include "ui/VlwFonts.h"

// efont (options 3 and 5) is out of the default build since the UI draws in
// the VLW fonts: it cost 533 KB of flash and misses glyphs (tofu). Build
// with -DUI_SPIKE_EFONT=1 to measure it again. The VLW option (4) uses the
// UI's own fonts (ui/VlwFonts.h), which are always in the build.
#ifndef UI_SPIKE_EFONT
#define UI_SPIKE_EFONT 0
#endif
#define UI_SPIKE_VLW 1

// UI spike: the font probe (console e, docs/UI-SPIKE.md). Draws the same
// list rows (title + artist, 42 px, into the PSRAM row sprite the lists use)
// with each way of handling names that aren't plain ASCII, times the draw and
// the push per row, and leaves the page on screen for a screenshot (X):
//   e1  FreeSans 9 pt (GFX, 7-bit) + Font2, names folded to ASCII (textfold Full)
//   e2  FreeSans 12 pt + Font2, folded
//   e3  efontCN_16 + efontCN_12 (M5GFX's Unicode bitmap fonts), UTF-8 as is
//       (not in the default build)
//   e4  VLW smooth DejaVu Sans 16 + 13 px from flash (Latin-1 + punctuation),
//       UTF-8 as is: what the UI now uses
//   e5  the spec's rule: punctuation folded; FreeSans 9 when the row is then
//       ASCII, efont 16/12 for that row only when it isn't (not in the
//       default build)
// Every option does the same work a list row will: the text fitted to the
// column with its own font (a width pass, and "..." when it is too long),
// then drawn. Per row: the first draw after the CPU cache is flushed (by
// reading kEvictBytes of PSRAM: the ESP32's 32 KB cache holds flash and PSRAM
// alike, and a scrolling list draws each row once, with the font's glyphs
// likely evicted), then the mean and best of kReps warm draws.
// e (or e0) measures every option compiled in and shows e1; e<n> measures
// and shows one; e again (or eq) closes. Options 3 and 5 need
// UI_SPIKE_EFONT (off by default, above).
class FontProbe {
public:
  FontProbe();
  ~FontProbe();
  void command(const char* arg);
  bool active() const { return active_; }
  void close();

private:
  static constexpr int kOptions = 5;
  static constexpr size_t kEvictBytes = 64 * 1024;
  bool ensure();
  void evictCache();
  bool available(int option) const;
  void drawRow(int option, int sample);
  float measure(int option, bool log);
  void showPage(int option);

  M5Canvas row_;
  bool ready_ = false;
  bool active_ = false;
  float drawMs_[kOptions + 1] = {};   // warm mean
  float coldMs_[kOptions + 1] = {};   // first draw after a cache flush, mean over the samples
  float pushMs_[kOptions + 1] = {};
  uint8_t* evict_ = nullptr;          // PSRAM, kEvictBytes
#if UI_SPIKE_VLW
  lgfx::PointerWrapper vlwData16_, vlwData13_;
  lgfx::VLWfont vlw16_, vlw13_;
  bool vlwLoaded_ = false;
#endif
  SpiHoldStats lock_;
};
