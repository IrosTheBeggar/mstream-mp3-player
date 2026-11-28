// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Output > About, and About > Device info (the pages' shapes are in
// Pages.h): rows of a label over its value, read from the host on the way
// in and every 3 s while the page is open.
#include <cstdio>
#include <cstring>

#include "DeviceInfo.h"
#include "LibraryIndex.h"
#include "UiStrings.h"
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/Icons.h"
#include "ui/OutputParts.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

using uistr::S;
using namespace outputparts;

namespace {
constexpr uint32_t kAboutMs = 3000;
}  // namespace

// ---- About ----

void AboutPage::enter(NavModel::PageRef& ref) {
  ui_.host().about(about_);
  nextAboutMs_ = millis() + kAboutMs;
  show(ref);
}

Header AboutPage::header() const {
  Header h;
  h.subWhole = true;  // (a phrase: whole beside the title, or not at all)
  h.back = true;
  h.title = tr(S::about_title);
  h.sub = tr(S::about_subtitle);
  return h;
}

bool AboutPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  ListView& list = ui_.list();
  const bool still = !list.animating();
  if (static_cast<int32_t>(nowMs - nextAboutMs_) >= 0 && still) {
    nextAboutMs_ = nowMs + kAboutMs;
    ui_.host().about(about_);
    list.refreshRow(Battery);
    list.refreshRow(Memory);
  }
  return list.update(nowMs, frameDue, wholeRows);
}

uint32_t AboutPage::rows() { return kAboutRows; }

void AboutPage::drawRow(ListView::Row& r) {
  const AppState& s = ui_.state();
  char value[128] = "";
  char labelText[96];
  // The library's counts: the line, or its short form where it doesn't fit.
  char tracks[40] = "", albums[40] = "";
  bool library = false;
  const char* label = "";
  const icons::Icon* icon = &icons::kInfo;
  switch (static_cast<AboutItem>(r.row)) {
    case Battery:
      label = tr(S::about_battery);
      if (s.charging) {
        char pct[16];
        trf(pct, sizeof(pct), S::format_percent, s.battery);
        trf(value, sizeof(value), S::about_battery_charging, pct);
      } else {
        trf(value, sizeof(value), S::format_percent, s.battery);
      }
      break;
    case Storage:
      label = tr(S::about_storage);
      snprintf(value, sizeof(value), "%s", about_.storage);
      icon = &icons::kSdCard;
      break;
    case LibraryInfo: {
      label = tr(S::about_library);
      const LibraryIndex* i = ui_.library().index();
      if (i && i->ready()) {
        char artists[40];
        trnf(tracks, sizeof(tracks), S::library_tracks_n, i->trackCount(), i->trackCount());
        trnf(artists, sizeof(artists), S::library_artists_n, i->artistCount(), i->artistCount());
        trnf(albums, sizeof(albums), S::library_albums_n, i->albumCount(), i->albumCount());
        trf(value, sizeof(value), S::about_library_value, tracks, artists, albums);
        library = true;
      } else {
        trf(value, sizeof(value), S::about_no_music);
      }
      icon = &icons::kLibrary;
      break;
    }
    case Headphones:
      label = tr(S::about_headphones);
      snprintf(value, sizeof(value), "%s", about_.bluetooth);
      icon = &icons::kHeadphones;
      break;
    case PowerInfo:
      label = tr(S::about_power);
      snprintf(value, sizeof(value), "%s", about_.power);
      break;
    case Memory:
      label = tr(S::about_memory);
      trf(value, sizeof(value), S::about_memory_value, about_.ramFree / 1024, about_.ramMin / 1024,
          uistr::decimal(about_.psramFree / (1024.0f * 1024.0f), 1));
      break;
    case Version:
      trf(labelText, sizeof(labelText), S::about_version_label, about_.built, about_.elf);
      label = labelText;
      snprintf(value, sizeof(value), "%s", about_.version);
      break;
    case LicenceInfo:
      label = tr(S::about_licence_label);
      trf(value, sizeof(value), S::about_licence);
      break;
    case SourceInfo:
      label = tr(S::about_source_label);
      snprintf(value, sizeof(value), "%s", uitext::kAboutSourceRepo);
      break;
    case DeviceInfoRow: {
      // A page of its own (what the boot screen used to list).
      const int x = ListView::icon(r, icons::kInfo, col::SOFT);
      const int right = ListView::chevron(r);
      const char* t = tr(S::about_device_info);
      const char* sub = tr(S::about_device_info_sub);
      ListView::lines(r, x, right, t, strlen(t), sub, strlen(sub), col::TXT);
      return;
    }
    case Tips: {
      const int x = ListView::icon(r, icons::kInfo, accent::Output);
      const char* t = tr(S::about_tips);
      const char* sub = tr(S::about_tips_sub);
      ListView::lines(r, x, r.right - 8, t, strlen(t), sub, strlen(sub), accent::Output);
      return;
    }
    default: return;
  }
  // The label small over its value (small too when it's long: "10000
  // tracks, 600 artists, 1500 albums"; longer still, the library's tracks
  // and albums: "99,999 tracks, 1500 albums").
  Fonts& f = Fonts::instance();
  const int x = ListView::icon(r, *icon, col::DIM);
  const int room = r.right - 8 - x;
  if (library && f.width(Font::Small, value) > room) {
    trf(value, sizeof(value), S::about_library_value_short, tracks, albums);
  }
  f.draw(r.c, Font::Small, label, x, 12, room, col::DIM, r.bg);
  f.draw(r.c, f.fitting(Font::Body, value, room), value, x, 30, room, col::TXT, r.bg);
}

ListView::Tap AboutPage::onTapAt(uint32_t row, int x, bool rightEdge) {
  (void)x;
  (void)rightEdge;
  if (row == DeviceInfoRow) {
    ui_.push(page(PageKind::DeviceInfo));
  } else if (row == Tips) {
    ui_.showCoach();
  }
  return ListView::Tap::Handled;
}

// ---- Device info ----

void DeviceInfoPage::enter(NavModel::PageRef& ref) {
  ui_.host().deviceInfo(info_);
  nextMs_ = millis() + kAboutMs;
  show(ref);
}

Header DeviceInfoPage::header() const {
  Header h;
  // A phrase: whole beside the title, or not at all (a title of 243 px, as
  // Spanish's "Información del dispositivo" was, leaves "este Core2" 31 px).
  h.subWhole = true;
  h.back = true;
  h.title = tr(S::about_device_info);
  h.sub = tr(S::device_info_subtitle);
  return h;
}

bool DeviceInfoPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  ListView& list = ui_.list();
  const bool still = !list.animating();
  // The rows that can change (the battery, memory, the uptime...), in
  // place; refreshRow() draws only the ones on screen.
  if (static_cast<int32_t>(nowMs - nextMs_) >= 0 && still) {
    nextMs_ = nowMs + kAboutMs;
    ui_.host().deviceInfo(info_);
    for (int i = 0; i < deviceinfo::kItems; ++i) {
      if (deviceinfo::changes(static_cast<deviceinfo::Item>(i))) list.refreshRow(static_cast<uint32_t>(i));
    }
  }
  return list.update(nowMs, frameDue, wholeRows);
}

uint32_t DeviceInfoPage::rows() { return deviceinfo::kItems; }

// A Device info row: its label (Small) over its value (Body, or Small when
// it's long: "cc13597, 2026-10-09, ELF 1a2b3c4d"), no icon: the values get
// the width (UiText's kDeviceInfoX and kDeviceInfoW, measured by
// test_ui_library).
void DeviceInfoPage::drawRow(ListView::Row& r) {
  if (r.row >= static_cast<uint32_t>(deviceinfo::kItems)) return;
  const auto item = static_cast<deviceinfo::Item>(r.row);
  char value[128];
  deviceinfo::value(item, info_, value, sizeof(value));
  Fonts& f = Fonts::instance();
  const int x = r.x + uitext::kDeviceInfoX;
  const int room = r.right - 8 - x;
  f.draw(r.c, Font::Small, deviceinfo::label(item), x, 12, room, col::DIM, r.bg);
  f.draw(r.c, f.width(Font::Body, value) <= room ? Font::Body : Font::Small, value, x, 30, room, col::TXT, r.bg);
}

ListView::Tap DeviceInfoPage::onTapAt(uint32_t row, int x, bool rightEdge) {
  (void)row;
  (void)x;
  (void)rightEdge;
  return ListView::Tap::Handled;  // nothing to tap
}

}  // namespace ui
