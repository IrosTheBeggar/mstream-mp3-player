// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/BootScreen.h"

#include <M5Unified.h>

#include "BootLayout.h"
#include "LogoArt.h"
#include "RleImage.h"
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/LcdLock.h"
#include "ui/Theme.h"

namespace {

using namespace bootlayout;

// The logo a row at a time: its runs read into pixel codes, each code
// through the palette (the parts' colours blended into the background,
// in the panel's byte order) and the row pushed. No image buffer: a row of
// codes and a row of pixels on the stack (720 B) and the palette's 264 B.
// A few ms for the whole logo (the boot log says how long), before
// anything reads the card.
void drawLogo(lgfx::LovyanGFX& d) {
  rleimage::Palette palette;
  palette.set(kLogoParts, kLogoBg, /*swapped=*/true);
  rleimage::Reader runs(logo::kData, logo::kSize, logo::kW);
  uint8_t codes[logo::kW];
  uint16_t px[logo::kW];
  for (int y = 0; y < logo::kH; ++y) {
    if (!runs.row(codes)) break;  // (never: test_boot_logo reads every row)
    for (int x = 0; x < logo::kW; ++x) px[x] = palette(codes[x]);
    d.pushImage(kLogoX, kLogoY + y, logo::kW, 1, reinterpret_cast<const lgfx::swap565_t*>(px));
  }
}

}  // namespace

void BootScreen::begin(const char* version) {
  auto& d = M5.Display;  // M5.begin() already set the Core2's landscape rotation
  ui::Fonts& fonts = ui::Fonts::instance();
  if (!fonts.load()) Serial.println("[boot] VLW fonts failed to load: the version in the built-in font");
  const uint32_t t0 = micros();
  {
    LcdLock lock;
    d.fillScreen(ui::col::BG);
    drawLogo(d);
    // A long dev build's version in Small ("v0.8.0-12-gabc1234-dirty").
    const ui::Font f = fonts.width(ui::Font::Body, version) <= kTextW ? ui::Font::Body : ui::Font::Small;
    fonts.draw(d, f, version, kW / 2, kVersionY, kTextW, ui::col::SOFT, ui::col::BG, ui::Fonts::Align::Centre);
  }
  Serial.printf("[boot] the boot screen: the logo (%d x %d px, %lu B of runs) and the version in %lu ms\n", logo::kW,
                logo::kH, static_cast<unsigned long>(logo::kSize), static_cast<unsigned long>((micros() - t0) / 1000));
}

void BootScreen::hint(const char* text) {
  LcdLock lock;
  ui::Fonts::instance().draw(M5.Display, ui::Font::Small, text, kW / 2, kHintY, uitext::kCalLineW, ui::col::DIM,
                             ui::col::BG, ui::Fonts::Align::Centre);
}
