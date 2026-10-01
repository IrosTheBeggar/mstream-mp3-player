// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Dance tab (spec §6.5): the dancer in its box (DanceMode draws it, 24-30
// frames a second, 10 while it idles, only the rectangle that moved), with the beat on the
// left, the dancer's name on the right and the track at the bottom, redrawn
// only when a value changes, at most twice a second. A tap on the dancer
// switches it (crab, stick figure). There is no tap zone at the bottom any
// more (the usability walk: it sat right above BtnC, and a press meant as
// "next" left the page).
//
//   left 0-99      BPM, locked / listening / no beat, confidence
//   box 100-219    x y 44-193, the dancer
//   right 220-319  "dancer", its name, "tap the dancer to switch"
//   196-239        the title and artist; a 2 px progress line at the bottom
//                  (while a computer drives the dancer, the USB visualizer:
//                  "Dancing to your computer" over how to stop it, no line)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "DanceSkin.h"
#include "UiText.h"
#include "ui/DanceView.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

namespace {
constexpr uint32_t kPanelMs = 500;
constexpr int kBottomY = 196;
}  // namespace

void DancePage::enter(NavModel::PageRef& ref) {
  (void)ref;
  DanceMode& d = ui_.dance();
  d.setActive(true);  // follows the output's audio from now, and draws the box
  repaint();
}

void DancePage::leave() { ui_.dance().setActive(false); }

void DancePage::repaint() {
  gfx::fill(0, kContentY, kW, kH - kContentY, col::BG);
  DanceMode& d = ui_.dance();
  d.view().enter();  // the box was cleared: its next frame is whole
  if (!d.ready()) {
    M5Canvas& s = gfx::strip();
    s.fillRect(0, 0, 120, 40, col::BG);
    Fonts::instance().draw(s, Font::Small, "No PSRAM for", 60, 12, 116, col::DIM, col::BG, Fonts::Align::Centre);
    Fonts::instance().draw(s, Font::Small, "the dancer", 60, 28, 116, col::DIM, col::BG, Fonts::Align::Centre);
    gfx::push(s, DanceView::kBoxX, 100, 120, 40);
  }
  drawPanels(true);
}

void DancePage::drawPanels(bool all) {
  DanceMode& d = ui_.dance();
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  M5Canvas& c = gfx::strip();
  const float bpm = d.bpm();
  const int bpmX10 = bpm > 0.0f ? static_cast<int>(std::lround(bpm * 10.0f)) : 0;
  const int lock = d.locked() ? 2 : bpm > 0.0f ? 1 : 0;
  const int conf = static_cast<int>(std::lround(d.confidence() * 100.0f));
  const int skin = static_cast<int>(d.skin());
  const int host = d.host() ? 1 : 0;
  // The left panel: the beat.
  if (all || bpmX10 / 10 != bpmX10_ / 10) {
    c.fillRect(0, 0, 100, 56, col::BG);
    f.draw(c, Font::Small, "BPM", 50, 14, 96, col::DIM, col::BG, Fonts::Align::Centre);
    char v[8];
    if (bpmX10) {
      snprintf(v, sizeof(v), "%d", (bpmX10 + 5) / 10);
    } else {
      snprintf(v, sizeof(v), "--");
    }
    f.draw(c, Font::Title, v, 50, 40, 96, col::TXT, col::BG, Fonts::Align::Centre);
    gfx::push(c, 0, 44, 100, 56);
  }
  if (all || lock != lock_) {
    c.fillRect(0, 0, 100, 32, col::BG);
    const uint16_t pill = lock == 2 ? col::GREEN : lock == 1 ? col::AMBER : col::BTN;
    c.fillRoundRect(14, 4, 72, 24, 12, pill);
    f.draw(c, Font::Small, lock == 2 ? "locked" : lock == 1 ? "listening" : "no beat", 50, 16, 68,
           lock ? col::DARK : col::DIM, pill, Fonts::Align::Centre);
    gfx::push(c, 0, 102, 100, 32);
  }
  if (all || conf != conf_) {
    c.fillRect(0, 0, 100, 56, col::BG);
    f.draw(c, Font::Small, "confidence", 50, 10, 96, col::DIM, col::BG, Fonts::Align::Centre);
    c.fillRoundRect(14, 22, 72, 6, 3, col::DIV);
    const int fill = 72 * std::max(0, std::min(100, conf)) / 100;
    if (fill > 0) c.fillRoundRect(14, 22, fill, 6, 3, conf >= 60 ? col::GREEN : col::AMBER);
    char v[8];
    snprintf(v, sizeof(v), "%d%%", conf);
    f.draw(c, Font::Small, v, 50, 42, 96, col::SOFT, col::BG, Fonts::Align::Centre);
    gfx::push(c, 0, 138, 100, 56);
  }
  // The right panel: the dancer.
  if (all || skin != skin_) {
    c.fillRect(0, 0, 100, 56, col::BG);
    f.draw(c, Font::Small, "dancer", 50, 14, 96, col::DIM, col::BG, Fonts::Align::Centre);
    const char* name = d.skin() == dance::Skin::Crab ? "Crab" : "Stick";
    f.draw(c, Font::Title, name, 50, 40, 96, col::TXT, col::BG, Fonts::Align::Centre);
    gfx::push(c, 220, 44, 100, 56);
    c.fillRect(0, 0, 100, 56, col::BG);
    for (int i = 0; i < 2; ++i) {  // which of the two dancers
      if (i == skin) {
        c.fillCircle(42 + i * 16, 8, 4, accent::Dance);
      } else {
        c.drawCircle(42 + i * 16, 8, 4, col::FAINT);
      }
    }
    f.draw(c, Font::Small, "tap the dancer", 50, 30, 98, col::FAINT, col::BG, Fonts::Align::Centre);
    f.draw(c, Font::Small, "to switch", 50, 46, 98, col::FAINT, col::BG, Fonts::Align::Centre);
    gfx::push(c, 220, 102, 100, 56);
  }
  // The bottom: the track, or the computer that drives the dancer.
  if (all || s.trackId != track_ || host != host_) {
    c.fillRect(0, 0, kW, 40, col::BG);
    if (host) {
      f.draw(c, Font::Bold, uitext::kVizTitle, kW / 2, 12, uitext::kDanceBottomW, col::TXT, col::BG,
             Fonts::Align::Centre);
      f.draw(c, Font::Small, uitext::kVizHint, kW / 2, 31, uitext::kDanceBottomW, col::DIM, col::BG,
             Fonts::Align::Centre);
    } else {
      char title[96] = "Nothing playing";
      if (s.current >= 0) ui_.player().catalog().title(s.trackId, title, sizeof(title));
      f.draw(c, Font::Bold, title, kW / 2, 12, kW - 16, col::TXT, col::BG, Fonts::Align::Centre);
      const char* artist = s.current >= 0 ? ui_.player().catalog().artist(s.trackId) : "";
      f.draw(c, Font::Small, artist, kW / 2, 31, kW - 16, col::DIM, col::BG, Fonts::Align::Centre);
    }
    gfx::push(c, 0, kBottomY, kW, 40);
  }
  const int progress = host || !s.durationMs
                           ? 0
                           : static_cast<int>(static_cast<uint64_t>(std::min(s.positionMs, s.durationMs)) * kW /
                                              s.durationMs);
  if (all || progress != progress_ || host != host_) {
    c.fillRect(0, 0, kW, 2, host ? col::BG : col::DIV);  // (the computer's track: no line)
    if (progress > 0) c.fillRect(0, 0, progress, 2, accent::Dance);
    gfx::push(c, 0, kH - 2, kW, 2);
  }
  bpmX10_ = bpmX10;
  lock_ = lock;
  conf_ = conf;
  skin_ = skin;
  track_ = s.trackId;
  progress_ = progress;
  host_ = host;
}

bool DancePage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  (void)frameDue;
  (void)wholeRows;
  if (static_cast<int32_t>(nowMs - nextPanelMs_) < 0) return false;
  nextPanelMs_ = nowMs + kPanelMs;
  drawPanels(false);
  return false;
}

void DancePage::onEvent(const InputEvent& e) {
  if (e.type != InputEvent::Type::Tap || !DanceView::inBox(e.x, e.y)) return;
  ui_.tick();
  ui_.dance().cycleSkin();
  drawPanels(false);
}

void DancePage::describe(char* buf, size_t size) const {
  DanceMode& d = const_cast<Ui&>(ui_).dance();
  snprintf(buf, size, "Dance: %s, %.1f BPM, confidence %.2f%s, %.0f fps (of %lu)%s", dance::skinName(d.skin()),
           d.bpm(), d.confidence(), d.locked() ? ", locked" : "", d.fps(), static_cast<unsigned long>(d.targetFps()),
           d.host() ? ", the computer's music (USB visualizer)" : "");
}

}  // namespace ui
