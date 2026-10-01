// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "spike/FontProbe.h"

#include <M5Unified.h>
#include <esp_timer.h>

#include <cstring>

#include "TextFold.h"
#include "spike/SpikeUi.h"

using namespace spike;

namespace {

struct Sample {
  const char* title;
  const char* artist;
};
// The spike's sample titles (from the SD card's library), with their artists.
const Sample kSamples[] = {
    {"06 Can’T Tell Me Nothing", "Kanye West"},
    {"05 Good Life Feat. T‐Pain", "Kanye West"},
    {"10 Le voyage de Pénélope", "Émilie Simon, Végétal"},
    {"01 La demme d'argent", "Air"},
    {"Selected Ambient Works 85-92", "Aphex Twin"},
};
constexpr int kSamples_ = sizeof(kSamples) / sizeof(kSamples[0]);
constexpr int kReps = 20;
constexpr int kPageTop = 30;  // a 30 px caption, then the five rows

const char* const kOptionNames[] = {
    "",
    "FreeSans 9 + Font2, folded",
    "FreeSans 12 + Font2, folded",
    "efont 16 + 12, UTF-8",
    "VLW DejaVu 16 + 13, UTF-8",
    "spec: fold punct., efont if needed",
};

}  // namespace

FontProbe::FontProbe() = default;

FontProbe::~FontProbe() {
  row_.deleteSprite();
  psramFree(evict_);
}

// Reads kEvictBytes of PSRAM a cache line (32 B) at a time: twice the cache,
// so the font's glyph data (flash) and the row sprite (PSRAM) are out of it.
void FontProbe::evictCache() {
  if (!evict_) return;
  const volatile uint8_t* p = evict_;
  uint32_t sum = 0;
  for (size_t i = 0; i < kEvictBytes; i += 32) sum += p[i];
  asm volatile("" : : "r"(sum));
}

bool FontProbe::ensure() {
  if (ready_) return true;
  row_.setPsram(true);  // before createSprite(): otherwise internal RAM
  row_.setColorDepth(16);
  if (!row_.createSprite(kW, kRowH)) {
    Serial.println("[font] no PSRAM for the row sprite");
    return false;
  }
  evict_ = static_cast<uint8_t*>(psramAlloc(kEvictBytes));
  if (evict_) memset(evict_, 0x5A, kEvictBytes);
  if (!evict_) Serial.println("[font] no PSRAM for the cache flush: the cold numbers are warm");
#if UI_SPIKE_VLW
  // Load both VLW fonts once (their glyph tables go to PSRAM), from flash.
  const uint32_t before = internalFree();
  const uint32_t psBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const int64_t t0 = esp_timer_get_time();
  vlwData16_.set(kVlwSans16, kVlwSans16Size);
  vlwData13_.set(kVlwSans13, kVlwSans13Size);
  vlwLoaded_ = vlw16_.loadFont(&vlwData16_) && vlw13_.loadFont(&vlwData13_);
  const int64_t t1 = esp_timer_get_time();
  Serial.printf("[font] VLW fonts %s in %.2f ms: %u + %u bytes of flash; internal RAM %+ld B, PSRAM %+ld B\n",
                vlwLoaded_ ? "loaded" : "FAILED to load", (t1 - t0) / 1000.0f, (unsigned)kVlwSans16Size,
                (unsigned)kVlwSans13Size, (long)internalFree() - (long)before,
                (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (long)psBefore);
#endif
  ready_ = true;
  return true;
}

bool FontProbe::available(int option) const {
  switch (option) {
    case 1:
    case 2: return true;
    case 3:
    case 5: return UI_SPIKE_EFONT != 0;
    case 4:
#if UI_SPIKE_VLW
      return vlwLoaded_;
#else
      return false;
#endif
    default: return false;
  }
}

void FontProbe::drawRow(int option, int sample) {
  M5Canvas& s = row_;
  const Sample& smp = kSamples[sample];
  s.fillSprite(col::BG);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextPadding(0);
  const int x = 12, maxW = kW - x - 12;
  char buf[112];
  switch (option) {
    case 1:
    case 2:
      s.setFont(option == 1 ? &fonts::FreeSans9pt7b : &fonts::FreeSans12pt7b);
      s.setTextColor(col::TXT, col::BG);
      fitText(s, smp.title, strlen(smp.title), buf, sizeof(buf), maxW);
      s.drawString(buf, x, option == 1 ? 14 : 13);
      s.setFont(&fonts::Font2);
      s.setTextColor(col::DIM, col::BG);
      fitText(s, smp.artist, strlen(smp.artist), buf, sizeof(buf), maxW);
      s.drawString(buf, x, 33);
      break;
#if UI_SPIKE_EFONT
    case 3:
      // UTF-8 as it is, fitted with the font's own widths.
      s.setFont(&fonts::efontCN_16);
      s.setTextColor(col::TXT, col::BG);
      fitRaw(s, smp.title, strlen(smp.title), buf, sizeof(buf), maxW);
      s.drawString(buf, x, 13);
      s.setFont(&fonts::efontCN_12);
      s.setTextColor(col::DIM, col::BG);
      fitRaw(s, smp.artist, strlen(smp.artist), buf, sizeof(buf), maxW);
      s.drawString(buf, x, 32);
      break;
    case 5: {
      // The spec's normaliser, then a font per row, then the fit in that font.
      const textfold::Result r = textfold::fold(smp.title, buf, sizeof(buf), textfold::Mode::Punctuation);
      s.setTextColor(col::TXT, col::BG);
      s.setFont(r.ascii ? static_cast<const lgfx::IFont*>(&fonts::FreeSans9pt7b) : &fonts::efontCN_16);
      ellipsize(s, buf, sizeof(buf), maxW);
      s.drawString(buf, x, r.ascii ? 14 : 13);
      const textfold::Result a = textfold::fold(smp.artist, buf, sizeof(buf), textfold::Mode::Punctuation);
      s.setTextColor(col::DIM, col::BG);
      s.setFont(a.ascii ? static_cast<const lgfx::IFont*>(&fonts::Font2) : &fonts::efontCN_12);
      ellipsize(s, buf, sizeof(buf), maxW);
      s.drawString(buf, x, a.ascii ? 33 : 32);
      break;
    }
#endif
#if UI_SPIKE_VLW
    case 4:
      s.setFont(&vlw16_);
      s.setTextColor(col::TXT, col::BG);
      fitRaw(s, smp.title, strlen(smp.title), buf, sizeof(buf), maxW);
      s.drawString(buf, x, 13);
      s.setFont(&vlw13_);
      s.setTextColor(col::DIM, col::BG);
      fitRaw(s, smp.artist, strlen(smp.artist), buf, sizeof(buf), maxW);
      s.drawString(buf, x, 32);
      break;
#endif
    default:
      s.setFont(&fonts::Font2);
      s.setTextColor(col::RED, col::BG);
      s.drawString("not in this build", x, 21);
      break;
  }
  s.drawFastHLine(x, kRowH - 1, kW - x, col::ROW_DIV);
}

// Per sample: one draw right after a cache flush (cold), then kReps warm
// draws into the row sprite (mean and best), and one push.
float FontProbe::measure(int option, bool log) {
  int64_t drawUs = 0, coldUs = 0, pushUs = 0;
  lock_.reset();
  for (int i = 0; i < kSamples_; ++i) {
    evictCache();
    const int64_t c0 = esp_timer_get_time();
    drawRow(option, i);
    const int64_t cold = esp_timer_get_time() - c0;
    coldUs += cold;
    int64_t best = INT64_MAX, sum = 0;
    for (int r = 0; r < kReps; ++r) {
      const int64_t t0 = esp_timer_get_time();
      drawRow(option, i);
      const int64_t dt = esp_timer_get_time() - t0;
      sum += dt;
      if (dt < best) best = dt;
    }
    drawUs += sum / kReps;
    const int64_t t0 = esp_timer_get_time();
    {
      LcdLock lock(&lock_);
      row_.pushSprite(&M5.Display, 0, kPageTop + i * kRowH);
    }
    const int64_t pu = esp_timer_get_time() - t0;
    pushUs += pu;
    if (log) {
      Serial.printf("[font] e%d row %d \"%s\": cold %.3f ms, warm %.3f ms (best %.3f), push %.2f ms\n", option,
                    i + 1, kSamples[i].title, cold / 1000.0f, sum / kReps / 1000.0f, best / 1000.0f, pu / 1000.0f);
    }
  }
  drawMs_[option] = drawUs / 1000.0f / kSamples_;
  coldMs_[option] = coldUs / 1000.0f / kSamples_;
  pushMs_[option] = pushUs / 1000.0f / kSamples_;
  return drawMs_[option];
}

void FontProbe::showPage(int option) {
  auto& d = M5.Display;
  fillLcd(0, kH, col::BG);
  char cap[72];
  snprintf(cap, sizeof(cap), "e%d  %s", option, kOptionNames[option]);
  {
    LcdLock lock;
    d.fillRect(0, 0, kW, kPageTop, col::HEAD);
    d.setFont(&fonts::Font2);
    d.setTextColor(col::TXT, col::HEAD);
    d.setTextDatum(textdatum_t::middle_left);
    d.setTextPadding(0);
    d.drawString(cap, 6, 9);
    if (!available(option)) {
      d.setTextColor(col::AMBER, col::HEAD);
      d.drawString("not in this build", 6, 23);
      return;
    }
  }
  measure(option, true);  // its pushes take their own (timed) locks
  snprintf(cap, sizeof(cap), "draw %.2f cold / %.2f warm ms/row, push %.2f", coldMs_[option], drawMs_[option],
           pushMs_[option]);
  LcdLock lock;
  d.setFont(&fonts::Font2);
  d.setTextColor(col::DIM, col::HEAD);
  d.setTextDatum(textdatum_t::middle_left);
  d.drawString(cap, 6, 23);
  d.setTextDatum(textdatum_t::top_left);
}

void FontProbe::close() {
  if (!active_) return;
  active_ = false;
  Serial.println("[font] closed");
}

void FontProbe::command(const char* arg) {
  if (arg && (arg[0] == 'q' || (!arg[0] && active_))) {
    close();
    return;
  }
  if (!ensure()) return;
  active_ = true;
  const int option = arg ? atoi(arg) : 0;
  if (option >= 1 && option <= kOptions) {
    showPage(option);
    return;
  }
  // All options, then leave e1 on screen.
  Serial.printf("[font] build: UI_SPIKE_EFONT=%d UI_SPIKE_VLW=%d; %d samples: 1 cold draw (after a %u KB cache "
                "flush) + %d warm draws each, every option fitted to the column in its own font\n",
                UI_SPIKE_EFONT, UI_SPIKE_VLW, kSamples_, (unsigned)(kEvictBytes / 1024), kReps);
  for (int o = kOptions; o >= 1; --o) {
    if (!available(o)) {
      Serial.printf("[font] e%d %s: not in this build\n", o, kOptionNames[o]);
      continue;
    }
    showPage(o);
  }
  for (int o = 1; o <= kOptions; ++o) {
    if (available(o)) {
      Serial.printf("[font] summary e%d %-36s draw cold %.3f / warm %.3f ms/row  push %.2f ms/row\n", o,
                    kOptionNames[o], coldMs_[o], drawMs_[o], pushMs_[o]);
    }
  }
  Serial.println("[font] e1-e5 shows one page (X for a screenshot); e or eq closes");
}
