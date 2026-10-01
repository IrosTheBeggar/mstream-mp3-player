// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/TabBar.h"

#include <cstdio>

#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace ui {

namespace {

uint16_t outputColour(tabbar::Output o, uint16_t idle) {
  switch (o) {
    case tabbar::Output::BtConnected: return col::CYAN;
    case tabbar::Output::BtConnecting: return col::AMBER;
    case tabbar::Output::BtLost: return col::RED;
    default: return idle;
  }
}

// The Now Playing icon: live EQ bars (or a note with nothing loaded), and
// the progress hairline under them.
void drawNowPlaying(M5Canvas& c, int cx, int iconTop, const tabbar::State& s, bool active) {
  if (s.play == tabbar::Play::Nothing) {
    icons::drawCentred(c, icons::kNote, cx, iconTop + 8, active ? accent::NowPlaying : col::ICON);
    return;
  }
  uint8_t h[4];
  tabbar::eqBars(s.eqStep, s.play == tabbar::Play::Playing, h);
  const uint16_t colour = s.play == tabbar::Play::Waiting                 ? col::AMBER
                          : s.play == tabbar::Play::Playing || active ? accent::NowPlaying
                                                                      : col::ICON;
  const int bottom = iconTop + tabbar::kEqMaxH;
  for (int i = 0; i < 4; ++i) c.fillRect(cx - 9 + i * 5, bottom - h[i], 3, h[i], colour);
  // The hairline: filled to the position, dotted when the length isn't known.
  const int x0 = cx - tabbar::kProgressW / 2, y = bottom + 2;
  if (s.progressKnown) {
    c.fillRect(x0, y, tabbar::kProgressW, 2, col::DIV);
    c.fillRect(x0, y, s.progressPx, 2, colour);
  } else {
    for (int x = x0; x < x0 + tabbar::kProgressW; x += 4) c.fillRect(x, y, 2, 2, col::DIV);
  }
}

void drawBattery(M5Canvas& c, int x, int y, const tabbar::State& s, uint16_t bg) {
  Fonts& f = Fonts::instance();
  const bool low = s.battery <= 20;
  const uint16_t fill = s.charging ? col::GREEN : low ? col::RED : col::GREEN;
  if (!(s.battery <= 10 && s.lowBlink && !s.charging)) {
    c.drawRoundRect(x, y, 22, 11, 2, col::SOFT);
    c.fillRect(x + 22, y + 3, 2, 5, col::SOFT);
    c.fillRect(x + 2, y + 2, 18 * s.battery / 100, 7, fill);
  }
  char t[6];
  snprintf(t, sizeof(t), "%u%%", static_cast<unsigned>(s.battery));
  f.draw(c, Font::Small, t, x + 12, 27, tabbar::kBatteryTextW, s.charging ? col::GREEN : col::DIM, bg,
         Fonts::Align::Centre);
}

}  // namespace

void TabBar::drawCell(int t, const tabbar::State& s) {
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const int x0 = tabbar::cellX0(t);
  const int w = tabbar::cellX1(t) - x0 + 1;
  const bool active = s.active == t;
  const auto tab = static_cast<NavModel::Tab>(t);
  const uint16_t acc = accent::of(tab);
  const bool output = t == tabbar::kTabs - 1;
  // The Output cell: the plate covers the icon and the volume; the battery
  // column right of it (286-319) is inside the tab's hit area but not on it.
  // The plate is nearly the cell's width (only the active tab has one, so
  // plates never meet): the label under the icon needs it ("Playing" is
  // 48 px in DejaVu 13).
  const int plateX = output ? tabbar::kOutputPlateX : 1;
  const int plateW = tabbar::labelWidth(t);
  const int cx = output ? plateX + plateW / 2 : w / 2;  // the plate's centre, in the cell
  c.fillRect(0, 0, w, tabbar::kHeight, col::SURF);
  uint16_t bg = col::SURF;
  if (active) {
    c.fillRoundRect(plateX, 3, plateW, 30, 6, col::ACT_BG);
    bg = col::ACT_BG;
  }
  // Active: the icon moves up for its label, and the accent underlines it.
  const int iconTop = active ? 3 : 7;
  const uint16_t ink = active ? acc : col::ICON;
  switch (tab) {
    case NavModel::Tab::NowPlaying:
      drawNowPlaying(c, cx, iconTop, s, active);
      if (s.sleep != tabbar::Sleep::None) {
        // The sleep timer's moon in the cell's top-right corner (amber
        // while it fades), drawn like the Queue's badge: no animation.
        icons::drawMoon(c, cx + tabbar::kMoonRight - tabbar::kMoonPx, 4, tabbar::kMoonPx,
                        s.sleep == tabbar::Sleep::Fading ? col::AMBER : col::SOFT);
      }
      break;
    case NavModel::Tab::Library:
      icons::drawCentred(c, icons::kLibrary, cx, iconTop + 8, ink);
      break;
    case NavModel::Tab::Queue: {
      icons::drawCentred(c, icons::kQueue, cx - 3, iconTop + 8, ink);
      char n[4];
      tabbar::badgeText(s.upNext, n);
      if (n[0]) {
        // The up-next badge, in digits you can read (the review: Font0's
        // 3x5 were not): on the accent while it flashes after an add.
        const int bw = f.width(Font::Small, n) + tabbar::kBadgePad;
        const int bx = cx + tabbar::kBadgeRight - bw;  // right-aligned over the icon's top right
        const uint16_t pill = s.badgeFlash ? acc : col::BTN_HI;
        c.fillRoundRect(bx, 1, bw, 14, 7, pill);
        f.draw(c, Font::Small, n, bx + bw / 2, 8, bw, s.badgeFlash ? col::DARK : col::TXT, pill, Fonts::Align::Centre);
      }
      break;
    }
    case NavModel::Tab::Dance:
      icons::drawCentred(c, icons::kCrab, cx, iconTop + 8, ink);
      break;
    case NavModel::Tab::Output: {
      const bool bt = s.output != tabbar::Output::Speaker;
      const uint16_t oc = outputColour(s.output, ink);
      const int ix = 6;  // the icon, then the volume beside it, both on the plate
      icons::draw(c, bt ? icons::kHeadphones : icons::kSpeaker, ix, iconTop, oc);
      if (s.output == tabbar::Output::BtLost) c.drawLine(ix, iconTop + 17, ix + 21, iconTop, col::RED);
      if (s.output == tabbar::Output::BtConnecting) {
        for (int i = 0; i < 3; ++i) c.fillRect(ix + 5 + i * 5, iconTop + 12, 2, 2, col::AMBER);
      }
      // The volume, beside the icon: part of the Output tab (a separate
      // chip in the corner stole every tap meant for this tab).
      char v[6];
      snprintf(v, sizeof(v), "%u%%", static_cast<unsigned>(s.volume));
      f.draw(c, Font::Small, v, tabbar::kVolumeX, iconTop + 9, tabbar::kVolumeW, active ? col::TXT : col::SOFT, bg);
      drawBattery(c, tabbar::kBatteryX, 6, s, col::SURF);
      break;
    }
  }
  if (active) {
    f.draw(c, Font::Small, tabbar::kLabels[t], cx, 27, plateW, acc, bg, Fonts::Align::Centre);
    c.fillRect(cx - 12, 33, 24, 2, acc);
  }
  c.drawFastHLine(0, tabbar::kHeight - 1, w, col::DIV);
  gfx::push(c, x0, 0, w, tabbar::kHeight, true);
}

void TabBar::update(const tabbar::State& s) {
  const uint8_t d = valid_ ? tabbar::dirty(drawn_, s) : tabbar::kAll;
  for (int t = 0; t < tabbar::kTabs; ++t) {
    if (d & (1u << t)) drawCell(t, s);
  }
  drawn_ = s;
  valid_ = true;
}

}  // namespace ui
