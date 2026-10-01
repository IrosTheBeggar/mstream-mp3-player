// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "spike/SpikeUi.h"

#include <M5Unified.h>

#include <algorithm>
#include <cstring>

namespace spike {

const char* tabName(int tab) {
  static const char* const kNames[] = {"Now Playing", "Library", "Queue", "Dance", "Output"};
  return tab >= 0 && tab < 5 ? kNames[tab] : "";
}

namespace {

// Icons, ~22x18, centred on (cx, cy).
void iconNowPlaying(lgfx::LovyanGFX& g, int cx, int cy, uint16_t c) {
  const int h[4] = {8, 14, 10, 17};
  for (int i = 0; i < 4; ++i) g.fillRect(cx - 10 + i * 6, cy + 9 - h[i], 3, h[i], c);
}

void iconLibrary(lgfx::LovyanGFX& g, int cx, int cy, uint16_t c) {
  // A disc and a sleeve: not bars, so it can't be read as Now Playing's EQ.
  g.drawRect(cx - 11, cy - 8, 16, 17, c);
  g.drawCircle(cx + 3, cy, 8, c);
  g.fillCircle(cx + 3, cy, 2, c);
}

void iconQueue(lgfx::LovyanGFX& g, int cx, int cy, uint16_t c) {
  for (int i = 0; i < 3; ++i) g.fillRect(cx - 4, cy - 7 + i * 6, 15, 2, c);
  g.fillTriangle(cx - 11, cy - 8, cx - 11, cy - 2, cx - 6, cy - 5, c);
}

void iconDance(lgfx::LovyanGFX& g, int cx, int cy, uint16_t c) {
  g.fillEllipse(cx, cy + 2, 7, 5, c);                 // body
  g.fillCircle(cx - 10, cy - 5, 3, c);                // claws
  g.fillCircle(cx + 10, cy - 5, 3, c);
  g.drawLine(cx - 6, cy, cx - 10, cy - 3, c);
  g.drawLine(cx + 6, cy, cx + 10, cy - 3, c);
  for (int i = -1; i <= 1; i += 2) {                  // legs
    g.drawLine(cx + i * 4, cy + 6, cx + i * 7, cy + 9, c);
    g.drawLine(cx + i * 2, cy + 6, cx + i * 3, cy + 9, c);
  }
}

void iconOutput(lgfx::LovyanGFX& g, int cx, int cy, uint16_t c) {
  g.drawArc(cx, cy + 1, 9, 8, 180, 360, c);           // the band
  g.fillRoundRect(cx - 10, cy + 1, 5, 8, 2, c);       // cups
  g.fillRoundRect(cx + 6, cy + 1, 5, 8, 2, c);
}

}  // namespace

void drawTabBar(lgfx::LovyanGFX& g, int active, int volumePercent, int batteryPercent) {
  g.fillRect(0, 0, kW, kBarH - 1, col::SURF);
  g.drawFastHLine(0, kBarH - 1, kW, col::DIV);
  for (int t = 0; t < 5; ++t) {
    const int x0 = t * kTabW;
    const int cx = x0 + kTabW / 2;
    const bool on = t == active;
    if (on) g.fillRoundRect(x0 + 3, 3, 50, 30, 6, col::ACT_BG);
    const uint16_t c = on ? col::CORAL : col::ICON;
    const int cy = on ? 13 : 17;  // the active icon moves up for its label
    switch (t) {
      case 0: iconNowPlaying(g, cx, cy, c); break;
      case 1: iconLibrary(g, cx, cy, c); break;
      case 2: iconQueue(g, cx, cy, c); break;
      case 3: iconDance(g, cx, cy, c); break;
      default: iconOutput(g, cx, cy, c); break;
    }
    if (on) {
      g.setFont(&fonts::Font0);
      g.setTextDatum(textdatum_t::middle_center);
      g.setTextColor(col::CORAL, col::ACT_BG);
      g.setTextPadding(0);
      const char* labels[] = {"Playing", "Library", "Queue", "Dance", "Output"};
      g.drawString(labels[t], cx, 28);
    }
  }
  // Volume chip: the value over a thin battery gauge.
  g.fillRoundRect(kChipX + 2, 4, 36, 28, 5, col::BTN);
  g.setFont(&fonts::Font2);
  g.setTextDatum(textdatum_t::middle_center);
  g.setTextColor(col::TXT, col::BTN);
  char v[8];
  snprintf(v, sizeof(v), "%d%%", volumePercent);
  g.drawString(v, kChipX + 20, 14);
  const int gw = 26;
  g.drawRect(kChipX + 7, 25, gw + 2, 4, col::FAINT);
  g.fillRect(kChipX + 8, 26, gw * batteryPercent / 100, 2, batteryPercent <= 20 ? col::RED : col::GREEN);
  g.setTextDatum(textdatum_t::top_left);
}

void fillLcd(int y, int h, uint16_t colour, SpiHoldStats* stats) {
  for (int b = y; b < y + h; b += kFillBand) {
    LcdLock lock(stats);
    M5.Display.fillRect(0, b, kW, std::min(kFillBand, y + h - b), colour);
  }
}

const char* fitText(lgfx::LovyanGFX& g, const char* in, size_t inLen, char* out, size_t outSize, int maxW,
                    textfold::Mode mode) {
  textfold::fold(in, inLen, out, outSize, mode);
  ellipsize(g, out, outSize, maxW);
  return out;
}

const char* fitRaw(lgfx::LovyanGFX& g, const char* in, size_t inLen, char* out, size_t outSize, int maxW) {
  if (outSize == 0) return out;
  size_t n = std::min(inLen, outSize - 1);
  while (n > 0 && n < inLen && (static_cast<unsigned char>(in[n]) & 0xC0) == 0x80) --n;
  std::memcpy(out, in, n);
  out[n] = 0;
  ellipsize(g, out, outSize, maxW);
  return out;
}

void ellipsize(lgfx::LovyanGFX& g, char* out, size_t outSize, int maxW) {
  if (g.textWidth(out) <= maxW) return;
  // Longest prefix that fits with "...", cut at a code point boundary.
  const size_t n = std::strlen(out);
  char tmp[160];
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = (lo + hi + 1) / 2;
    size_t cut = mid;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    if (cut + 4 > sizeof(tmp)) {
      hi = mid - 1;
      continue;
    }
    std::memcpy(tmp, out, cut);
    std::memcpy(tmp + cut, "...", 4);
    if (g.textWidth(tmp) <= maxW) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  size_t cut = lo;
  while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
  while (cut > 0 && out[cut - 1] == ' ') --cut;
  if (cut + 4 <= outSize) {
    std::memcpy(out + cut, "...", 4);
  }
}

uint32_t internalFree() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t internalMinEver() { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)); }

}  // namespace spike
