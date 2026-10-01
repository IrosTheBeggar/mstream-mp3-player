// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "spike/Spike.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>

#include "LibrarySynth.h"
#include "spike/FontProbe.h"
#include "spike/InputLab.h"
#include "spike/ScrollLab.h"
#include "spike/SpikeUi.h"
#include "spike/ThumbProbe.h"
#include "ui/Input.h"

using namespace spike;

namespace {

uint32_t psramFreeNow() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }

// A command that brings a screen up (rather than asking for a report).
bool opens(const char* arg, bool active) {
  if (!arg || !*arg) return !active;
  return (arg[0] >= '0' && arg[0] <= '9') || arg[0] == 'a' || arg[0] == 'w';
}

}  // namespace

Spike::Spike(Core2AudioBackend& audio, Haptics& haptics, LocalStorage& storage, Library& library, Input& input)
    : audio_(audio), haptics_(haptics), storage_(storage), library_(library), inputLayer_(input) {}

LibraryIndex* Spike::libraryIndex() { return synth_ && synth_->ready() ? synth_ : library_.index(); }

void Spike::dropSynthetic() {
  if (!synth_) return;
  if (scroll_) scroll_->indexChanging();
  psramDelete(synth_);
  synth_ = nullptr;
}

bool Spike::ownsScreen() const {
  return (input_ && input_->active()) || (scroll_ && scroll_->active()) || (font_ && font_->active()) ||
         (thumb_ && thumb_->active());
}

bool Spike::ownsInput() const { return input_ && input_->active(); }

bool Spike::onGlass(const InputEvent& e) {
  if (scroll_ && scroll_->active()) {
    scroll_->onTouch(e);
    return true;
  }
  // The probes' pages are static: a tap on the glass closes them.
  if ((font_ && font_->active()) || (thumb_ && thumb_->active())) {
    if (e.type == InputEvent::Type::Tap) {
      if (font_) font_->close();
      if (thumb_) thumb_->close();
    }
    return true;
  }
  return ownsInput();
}

void Spike::closeAllBut(Screen keep) {
  if (keep != Screen::Input && input_) input_->close();
  if (keep != Screen::Scroll && scroll_) scroll_->close();
  if (keep != Screen::Font && font_) font_->close();
  if (keep != Screen::Thumb && thumb_) thumb_->close();
}

void Spike::loop(uint32_t nowMs) {
  if (input_) input_->loop(nowMs);
  if (scroll_) scroll_->loop(nowMs);
  const bool owned = ownsScreen();
  if (owned_ && !owned && released_) released_();
  owned_ = owned;
}

void Spike::inputLab(const char* arg) {
  if (!input_) input_ = psramNew<InputLab>(haptics_);
  if (input_) input_->setCalibration(&inputLayer_.calibration());
  if (!input_ || !input_->ready()) {
    Serial.println("[input] no PSRAM for the input lab");
    return;
  }
  if (opens(arg, input_->active())) closeAllBut(Screen::Input);
  input_->command(arg);
}

void Spike::scrollLab(const char* arg) {
  if (!scroll_) scroll_ = psramNew<ScrollLab>(audio_, inputLayer_);
  if (!scroll_) {
    Serial.println("[scroll] no PSRAM for the scroll lab");
    return;
  }
  if (opens(arg, scroll_->active())) closeAllBut(Screen::Scroll);
  scroll_->command(arg, libraryIndex());
}

void Spike::fontProbe(const char* arg) {
  if (!font_) font_ = psramNew<FontProbe>();
  if (!font_) {
    Serial.println("[font] no PSRAM for the font probe");
    return;
  }
  if (opens(arg, font_->active())) closeAllBut(Screen::Font);
  font_->command(arg);
}

void Spike::thumbProbe(const char* arg) {
  if (!thumb_) thumb_ = psramNew<ThumbProbe>(&audio_);
  if (!thumb_) {
    Serial.println("[thumb] no PSRAM for the thumbnail probe");
    return;
  }
  if (opens(arg, thumb_->active())) closeAllBut(Screen::Thumb);
  thumb_->command(arg, storage_.available() ? &storage_.fs() : nullptr, libraryIndex());
}

void Spike::index(const char* arg) {
  if (!arg || !*arg) {
    report();
    return;
  }
  const long n = atol(arg);
  if (arg[0] == '0' && n == 0) {
    dropSynthetic();
    if (scroll_) scroll_->indexChanging();
    if (rebuild_ ? rebuild_() : library_.rebuild()) report();
    return;
  }
  if (n < 1 || n > 50000) {
    Serial.println("[index] g: report; g0: rebuild from the SD card; g<n>: synthetic library of n tracks (1-50000)");
    return;
  }
  if (buildSynthetic(static_cast<uint32_t>(n))) report();
}

bool Spike::buildSynthetic(uint32_t tracks) {
  if (!synth_) synth_ = psramNew<LibraryIndex>(psramAlloc, psramFree);
  if (!synth_) {
    Serial.println("[index] no PSRAM for a synthetic index");
    return false;
  }
  if (scroll_) scroll_->indexChanging();
  synth_->clear();
  const synth::Spec spec = synth::specFor(tracks);
  Build b;
  b.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  const int64_t t0 = esp_timer_get_time();
  synth_->begin(spec.root);  // no size hint: the same growth as a real scan
  b.added = synth::addTracks(*synth_, spec);
  b.files = spec.tracks;
  const int64_t t1 = esp_timer_get_time();
  const uint32_t minDuring = internalFree();
  const bool ok = synth_->finish();
  const int64_t t2 = esp_timer_get_time();
  b.addMs = (t1 - t0) / 1000.0f;  // path generation included (snprintf of ~70 B a track)
  b.finishMs = (t2 - t1) / 1000.0f;
  b.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  b.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(b.internalFreeBefore);
  b.internalMinDuring = std::min(minDuring, internalFree());
  build_ = b;
  if (!ok) Serial.println("[index] build FAILED (out of PSRAM)");
  Serial.printf("[index] synthetic: %lu tracks, %lu artists, %lu albums asked\n", (unsigned long)spec.tracks,
                (unsigned long)spec.artists, (unsigned long)spec.albums);
  return ok;
}

void Spike::report() {
  library_.report();
  if (!synth_ || !synth_->ready()) {
    Serial.println("[index] no synthetic library (g<n> makes one for the labs)");
    return;
  }
  const LibraryIndex::Memory m = synth_->memory();
  const uint32_t t = synth_->trackCount();
  Serial.printf("[index] synthetic (the labs use it until g0): %lu tracks, %lu artists, %lu albums, %lu folders\n",
                (unsigned long)t, (unsigned long)synth_->artistCount(), (unsigned long)synth_->albumCount(),
                (unsigned long)synth_->folderCount());
  Serial.printf("[index] time: add %.1f ms (making the paths included), finish %.1f ms (sort + views + trim)\n",
                build_.addMs, build_.finishMs);
  Serial.printf("[index] PSRAM: %u B held (%.1f B/track): strings %u, tracks %u, artists %u, albums %u, folders %u, "
                "views %u; build peak %u B; PSRAM free fell %ld B\n",
                (unsigned)m.total, t ? static_cast<float>(m.total) / t : 0.0f, (unsigned)m.strings,
                (unsigned)m.tracks, (unsigned)m.artists, (unsigned)m.albums, (unsigned)m.folders, (unsigned)m.views,
                (unsigned)m.buildPeak, (long)build_.psramUsed);
  Serial.printf("[index] internal RAM: free %lu B before, %+ld B after the build, lowest %lu B during\n",
                (unsigned long)build_.internalFreeBefore, (long)build_.internalDelta,
                (unsigned long)build_.internalMinDuring);
}
