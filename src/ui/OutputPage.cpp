// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Output tab (the tab bar spec §6.6, mockups 19-21 and 23, with the
// review's grafts; the page's shape is in Pages.h): where the music plays,
// each output's volume, the Pair screen, the settings, the language, About
// and its Device info. One page object for the tab's six kinds of page
// (Output, Pair, Language, About, Device info, Auto DJ), each a list on
// the hardware scroll.
//
// What the Bluetooth card shows and offers is OutputModel's (host-tested);
// what its buttons do goes through the UiHost to BtSink. The rules it keeps:
// the audio stays where it is until the headphones are up (a tap on the
// card, Connect, only connects; the Connected event moves the audio),
// nothing moves to the speaker unpaused, and Forget takes a second tap.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DeviceInfo.h"
#include "IdlePolicy.h"
#include "LibraryIndex.h"
#include "PowerChoices.h"
#include "ScreenPower.h"
#include "UiStrings.h"
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

using uistr::S;

namespace {

constexpr uint32_t kSpinMs = 125;   // the spinner: 8 steps a second
constexpr uint32_t kScanPollMs = 250;
constexpr uint32_t kAboutMs = 3000;
// The speaker card's volume chip: its hit area around the chip (UiText's,
// host-tested against a panel that reads right).
constexpr int kSpeakerChipHitX = uitext::kSpeakerChipHitX;
constexpr int kSpeakerChipHitEnd = uitext::kSpeakerChipHitEnd;

void radio(M5Canvas& c, int cx, int cy, bool on, uint16_t acc, uint16_t bg) {
  c.fillCircle(cx, cy, 10, bg);
  c.drawCircle(cx, cy, 10, on ? acc : col::FAINT);
  c.drawCircle(cx, cy, 9, on ? acc : col::FAINT);
  if (on) c.fillCircle(cx, cy, 5, acc);
}

// A ring of 8 dots, one bright (the step): something is under way.
void spinner(M5Canvas& c, int cx, int cy, uint8_t step, uint16_t colour) {
  static const int8_t kDx[8] = {0, 6, 8, 6, 0, -6, -8, -6};
  static const int8_t kDy[8] = {-8, -6, 0, 6, 8, 6, 0, -6};
  for (int i = 0; i < 8; ++i) {
    const int age = (step - i + 8) % 8;
    const uint16_t ink = age == 0 ? colour : age < 3 ? col::SOFT : col::FAINT;
    c.fillCircle(cx + kDx[i], cy + kDy[i], age == 0 ? 2 : 1, ink);
  }
}

// A pairing under way (as the idle power-off's blocker): a restart for the
// CPU speed would drop it.
bool pairingUnderWay(const AppState& s) {
  return s.btLink.phase == BtLink::Phase::PairScan || s.btLink.phase == BtLink::Phase::Pairing ||
         s.btSession.pairingUnderWay();
}

// Four signal bars, `lit` of them in the ink.
void bars(M5Canvas& c, int x, int bottom, int lit, uint16_t ink) {
  for (int i = 0; i < 4; ++i) {
    const int h = 4 + i * 3;
    c.fillRect(x + i * 5, bottom - h, 3, h, i < lit ? ink : col::DIV);
  }
}

uint16_t toneColour(BtTone t) {
  switch (t) {
    case BtTone::Cyan: return col::CYAN;
    case BtTone::Amber: return col::AMBER;
    case BtTone::Red: return col::RED;
    case BtTone::Dim: break;
  }
  return col::DIM;
}

// A card's background across one or two list rows: `part` 0 its only row,
// 1 its top row, 2 its bottom row (the rounded corners only at its ends).
// `right`: the row's (the scrollbar's column after it).
void card(M5Canvas& c, int part, int right, uint16_t bg, bool outline, uint16_t acc) {
  c.fillSprite(col::BG);
  const int top = part == 2 ? -20 : 3;
  const int bottom = part == 1 ? kRowH + 20 : kRowH - 2;
  const int w = right - 6 - 6;
  c.fillRoundRect(6, top, w, bottom - top, 10, bg);
  if (outline) {
    c.drawRoundRect(6, top, w, bottom - top, 10, acc);
    c.drawRoundRect(7, top + 1, w - 2, bottom - top - 2, 9, acc);
  }
}

NavModel::PageRef page(PageKind kind) {
  NavModel::PageRef p;
  p.kind = static_cast<uint8_t>(kind);
  return p;
}

// A setting's value (and the Library row's Rescan) in its pill at the row's
// right, centred: Body, or Small when it doesn't fit (Russian's "Средняя").
void valuePill(ListView::Row& r, int pillX, int pillW, const char* value, uint16_t ink, uint16_t pill) {
  Fonts& f = Fonts::instance();
  r.c.fillRoundRect(pillX, 9, pillW, 24, 8, pill);
  const int room = pillW - uitext::kSettingPillPad;
  f.draw(r.c, f.fitting(Font::Body, value, room), value, pillX + pillW / 2, 21, room, ink, pill, Fonts::Align::Centre);
}

}  // namespace

const char* OutputPage::btName() const {
  const AppState& s = ui_.state();
  return s.btName[0] ? s.btName : "";
}

// ---- the page ----

void OutputPage::enter(NavModel::PageRef& ref) {
  kind_ = static_cast<PageKind>(ref.kind);
  ref_ = &ref;
  firstBoot_ = kind_ == PageKind::Language && ref.id == kFirstBootPicker;
  touchInList_ = false;
  headerPressed_ = 0;
  forget_.reset();
  picked_ = -1;
  searchEnded_ = false;
  ask_ = Ask::None;
  drawnBt_ = btSig();
  drawnSpeaker_ = speakerSig();
  drawnHaptics_ = ui_.input().hapticsOn();
  drawnCalibrated_ = ui_.input().calibrated();
  drawnScreen_ = screenSig(ui_.state());
  drawnAutoDj_ = autoDjSig(ui_.state().autoDj);
  if (kind_ == PageKind::Pair) {
    scan_.clear();
    scanVersion_ = 0;
    nextScanMs_ = 0;
    ui_.host().btPairScan(true);
    search_.start(millis());
    Serial.println("[ui] output: pair screen (scanning)");
  }
  if (kind_ == PageKind::About) {
    ui_.host().about(about_);
    nextAboutMs_ = millis() + kAboutMs;
  }
  if (kind_ == PageKind::DeviceInfo) {
    ui_.host().deviceInfo(info_);
    nextAboutMs_ = millis() + kAboutMs;
  }
  repaintHeader();
  // The languages open at the one in use (with the one before it above).
  const int lang = static_cast<int>(uistr::lang());
  ui_.list().attach(this, &ref, kind_ == PageKind::Language && lang > 0 ? (lang - 1) * ListLayout::kPitch : 0);
}

void OutputPage::leave() {
  if (kind_ == PageKind::Pair) ui_.host().btPairScan(false);
  search_.stop();
  ui_.list().detach();
}

void OutputPage::repaint() {
  repaintHeader();
  ui_.repaintList();
}

// Its tab tapped again: Pair and About go back to the Output list (the Ui
// pops them); at the root, to the top.
void OutputPage::home() { ui_.list().scrollTo(0); }

// The screen went off: the Pair screen's scan (an inquiry, back to back)
// stops, as after its 2 minutes; the row offers "Search again" on the wake.
// A pairing under way keeps the screen lit, so it isn't one, and the
// search itself holds a lit screen lit (pairSearching(): main.cpp's
// ScreenControl::Hold): what turns a held screen off still does (the
// sleep timer's pause, the console's Ps0). Before, it dimmed and went off
// as ever: the dim screen swallowed the listener's taps as wakes, and the
// screen going off stopped the search (2026-10-04: three tries to pair).
void OutputPage::screenOff() {
  if (kind_ != PageKind::Pair || !search_.searching()) return;
  if (ui_.state().btLink.phase == BtLink::Phase::Pairing) return;
  search_.stop();
  ui_.host().btPairScanPause();  // (the page stays: nothing else is tried until it closes)
  Serial.println("[ui] output: pair screen: the search stopped (the screen went off; tap Search again)");
}

// The Pair screen's 2 minutes, checked every pass: a dialog over the page
// (the headphones lost, "Couldn't reach") stops its update(), not the
// inquiry, which costs power while it runs. Stopped, and the status row
// offers to search again (drawn by update() once the page shows). The
// list stays, and nothing else is tried until the page closes.
void OutputPage::tick(uint32_t nowMs) {
  if (kind_ != PageKind::Pair || !search_.due(nowMs)) return;
  ui_.host().btPairScanPause();
  searchEnded_ = true;
  Serial.println("[ui] output: pair screen: the search stopped after 2 min (tap Search again)");
}

Header OutputPage::header() const {
  Header h;
  // Each line is a phrase or a count: whole beside the title, or not at all
  // (a title of 243 px, as Spanish's "Información del dispositivo" was,
  // leaves "este Core2" 31 px).
  h.subWhole = true;
  switch (kind_) {
    case PageKind::Pair:
      h.back = true;
      h.title = tr(S::pair_title);
      trnf(headerSub_, sizeof(headerSub_), S::pair_found, scan_.count(), scan_.count());
      h.sub = scan_.count() ? headerSub_ : "";
      break;
    case PageKind::About:
      h.back = true;
      h.title = tr(S::about_title);
      h.sub = tr(S::about_subtitle);
      break;
    case PageKind::DeviceInfo:
      h.back = true;
      h.title = tr(S::about_device_info);
      h.sub = tr(S::device_info_subtitle);
      break;
    case PageKind::AutoDj:
      h.back = true;
      h.title = tr(S::output_autodj);
      h.sub = tr(S::autodj_subtitle);
      break;
    case PageKind::Language:
      // The first boot's: nothing behind it, and where it is found later.
      h.back = !firstBoot_;
      h.title = tr(S::language_title);
      h.sub = tr(firstBoot_ ? S::language_first_boot_sub : S::language_subtitle);
      break;
    default:
      h.title = tr(S::output_title);
      h.sub = tr(S::output_subtitle);
      break;
  }
  return h;
}

void OutputPage::repaintHeader() { ui_.drawHeader(header()); }

uint16_t OutputPage::accent() { return accent::Output; }

uint32_t OutputPage::btSig() const {
  const AppState& s = ui_.state();
  const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
  uint32_t h = static_cast<uint32_t>(v.card) * 131u + s.btLink.attempt * 7u + s.btLink.attempts;
  h = h * 31u + (s.onBluetooth ? 1u : 0u) + (s.btConnected ? 2u : 0u) + (s.btSession.wanted() ? 4u : 0u);
  h = h * 31u + s.btVolume;
  h = h * 31u + (forget_.armed(millis()) ? 1u : 0u);
  for (const char* p = s.btName; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  for (const char* p = s.btDetail; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  return h;
}

uint32_t OutputPage::speakerSig() const {
  const AppState& s = ui_.state();
  uint32_t h = (s.onBluetooth ? 1u : 0u) | (s.silent ? 2u : 0u) | (s.btSession.wanted() ? 4u : 0u);
  h = h * 131u + s.speakerVolume;
  for (const char* p = s.btName; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  return h;
}

bool OutputPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  ListView& list = ui_.list();
  const bool still = !list.animating();
  if (kind_ == PageKind::Output) {
    // A card changed: its rows again (in place: nothing moves).
    if (forget_.expired(nowMs)) forget_.reset();  // its "Tap again" goes
    const uint32_t bt = btSig();
    if (bt != drawnBt_ && still) {
      drawnBt_ = bt;
      list.refreshRow(BtTop);
      list.refreshRow(BtButtons);
    }
    const uint32_t sp = speakerSig();
    if (sp != drawnSpeaker_ && still) {
      drawnSpeaker_ = sp;
      list.refreshRow(SpeakerRow);
    }
    if (ui_.input().hapticsOn() != drawnHaptics_ && still) {
      drawnHaptics_ = ui_.input().hapticsOn();
      list.refreshRow(Haptics);
    }
    // (Removed here, or from the console: ad.)
    if (ui_.input().calibrated() != drawnCalibrated_ && still) {
      drawnCalibrated_ = ui_.input().calibrated();
      list.refreshRow(Calibrate);
    }
    const uint32_t lib = librarySig(ui_.state());
    if (lib != drawnLibrary_ && still) {
      drawnLibrary_ = lib;
      list.refreshRow(LibraryRow);
    }
    const uint32_t dj = autoDjSig(ui_.state().autoDj);
    if (dj != drawnAutoDj_ && still) {
      drawnAutoDj_ = dj;
      list.refreshRow(AutoDjRow);
    }
    const uint32_t sc = screenSig(ui_.state());
    if (sc != drawnScreen_ && still) {
      drawnScreen_ = sc;
      list.refreshRow(ScreenOff);
      list.refreshRow(Brightness);
      list.refreshRow(IdleOff);
      list.refreshRow(CpuSpeed);
      list.refreshRow(BtPower);
    }
    // The spinner, while something is under way.
    const AppState& s = ui_.state();
    const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
    if (v.spinner && still && static_cast<int32_t>(nowMs - nextSpinMs_) >= 0) {
      nextSpinMs_ = nowMs + kSpinMs;
      spin_ = static_cast<uint8_t>((spin_ + 1) % 8);
      // (drawBtTop() draws it at (290, 21) on the card's own colour.)
      if (!spinPatch(BtTop, 290, 21, toneColour(v.tone), s.onBluetooth ? col::ROW_SEL : col::CARD)) {
        list.refreshRow(BtTop);
      }
    }
  } else if (kind_ == PageKind::Pair) {
    if (searchEnded_) {
      searchEnded_ = false;
      list.refreshRow(0);  // "Search again" (tick() stopped it)
    }
    if (static_cast<int32_t>(nowMs - nextScanMs_) >= 0) {
      nextScanMs_ = nowMs + kScanPollMs;
      // Devices came (or their signal changed): the list again, once it's
      // still (a later poll catches up).
      if (still) {
        const uint32_t v = ui_.host().btScan(scan_);
        if (v != scanVersion_) {
          scanVersion_ = v;
          list.reload();
          repaintHeader();
        }
      }
    }
    if (still && search_.searching() && static_cast<int32_t>(nowMs - nextSpinMs_) >= 0) {
      nextSpinMs_ = nowMs + kSpinMs;
      spin_ = static_cast<uint8_t>((spin_ + 1) % 8);
      if (!spinPatch(0, 24, 21, col::CYAN, col::BG)) list.refreshRow(0);  // (its row draws it at (24, 21))
    }
  } else if (kind_ == PageKind::AutoDj) {
    // A setting, the index's state, a hold: the rows in place (a tap's
    // change shows here, from the next pass's snapshot).
    const uint32_t dj = autoDjSig(ui_.state().autoDj);
    if (dj != drawnAutoDj_ && still) {
      drawnAutoDj_ = dj;
      for (uint32_t i = 0; i < autodjview::kPageRows; ++i) list.refreshRow(i);
    }
  } else if (kind_ == PageKind::About) {
    if (static_cast<int32_t>(nowMs - nextAboutMs_) >= 0 && still) {
      nextAboutMs_ = nowMs + kAboutMs;
      ui_.host().about(about_);
      list.refreshRow(Battery);
      list.refreshRow(Memory);
    }
  } else if (kind_ == PageKind::DeviceInfo) {
    // The rows that can change (the battery, memory, the uptime...), in
    // place; refreshRow() draws only the ones on screen.
    if (static_cast<int32_t>(nowMs - nextAboutMs_) >= 0 && still) {
      nextAboutMs_ = nowMs + kAboutMs;
      ui_.host().deviceInfo(info_);
      for (int i = 0; i < deviceinfo::kItems; ++i) {
        if (deviceinfo::changes(static_cast<deviceinfo::Item>(i))) list.refreshRow(static_cast<uint32_t>(i));
      }
    }
  }
  return list.update(nowMs, frameDue, wholeRows);
}

bool OutputPage::animating() const { return ui_.list().animating(); }

void OutputPage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    touchInList_ = e.y >= ListView::kTop;
    if (!touchInList_) {
      headerPressed_ = header().hit(e);
    }
  } else if (e.type == T::DragStart && e.fromStrip) {
    touchInList_ = true;  // a swipe up from the strip: it scrolls the list
  }
  if (touchInList_) {
    ui_.list().onEvent(e);
    return;
  }
  if (e.type == T::Tap) {
    const int hit = headerPressed_;
    headerPressed_ = 0;
    if (hit == 1 && kind_ != PageKind::Output) {
      ui_.tick();
      ui_.back();
    }
  } else if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
    headerPressed_ = 0;
  }
}

void OutputPage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  if (kind_ == PageKind::AutoDj) {
    const autodjview::State& d = s.autoDj;
    snprintf(buf, size, "Output (AutoDj): Auto DJ %s, %s, %u %% (shown %u %%), BPM %s, key %s; the index %s%s",
             tr(autodjview::rowText(autodjview::rowState(d))),
             d.mode == autodj::Mode::Locked ? "Stay on seed" : "Follow the vibe", static_cast<unsigned>(d.floorPct),
             static_cast<unsigned>(d.floorShown), d.matchTempo ? "on" : "off", d.matchKey ? "on" : "off",
             autodjview::indexName(d.index), d.usable ? " (usable)" : " (not usable: the settings dim)");
    return;
  }
  const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
  snprintf(buf, size, "Output (%s): %s, the card %s (link %s), volume %u%% (speaker %u%%, bluetooth %u%%)%s",
           pageKindName(static_cast<uint8_t>(kind_)), s.onBluetooth ? "bluetooth" : "speaker", btCardName(v.card),
           btPhaseName(s.btLink.phase), static_cast<unsigned>(s.volume), static_cast<unsigned>(s.speakerVolume),
           static_cast<unsigned>(s.btVolume), kind_ == PageKind::Pair ? (!search_.searching() ? ", search stopped" : scan_.count() ? ", devices found" : ", scanning") : "");
}

// ---- the list ----

uint32_t OutputPage::rows() {
  switch (kind_) {
    case PageKind::Pair: return 1 + static_cast<uint32_t>(scan_.count());
    case PageKind::About: return kAboutRows;
    case PageKind::DeviceInfo: return deviceinfo::kItems;
    case PageKind::Language: return uistr::kLangs;
    case PageKind::AutoDj: return autodjview::kPageRows;
    default: return kRootRows;
  }
}

bool OutputPage::emptyState(EmptyState& e) {
  (void)e;
  return false;  // never empty
}

// Auto DJ's status row is no control: no press, no tick.
bool OutputPage::pressable(uint32_t row) { return !(kind_ == PageKind::AutoDj && row == autodjview::StatusRow); }

void OutputPage::drawRow(ListView::Row& r) {
  if (kind_ == PageKind::Pair) {
    if (r.row == 0) {
      drawPairStatus(r);
    } else if (static_cast<int>(r.row) - 1 < scan_.count()) {
      drawDevice(r, scan_.at(static_cast<int>(r.row) - 1));
    }
    return;
  }
  if (kind_ == PageKind::About) {
    drawAbout(r);
    return;
  }
  if (kind_ == PageKind::DeviceInfo) {
    drawDeviceInfo(r);
    return;
  }
  if (kind_ == PageKind::Language) {
    drawLanguage(r);
    return;
  }
  if (kind_ == PageKind::AutoDj) {
    drawAutoDj(r);
    return;
  }
  const AppState& s = ui_.state();
  switch (static_cast<RootRow>(r.row)) {
    case BtTop: drawBtTop(r); break;
    case BtButtons: drawBtButtons(r); break;
    case SpeakerRow: drawSpeaker(r); break;
    case LineOut:
      drawSetting(r, icons::kJack, tr(S::output_line_out), tr(S::output_line_out_sub), col::DIM, false);
      break;
    case PairNew:
      drawSetting(r, icons::kPlus, tr(S::output_pair_new), "", accent::Output, true);
      break;
    case AutoDjRow: {
      // "Auto DJ", and under it "On · Follow the vibe" or "Off": the state
      // alone when the line with the Anchor doesn't fit whole (the Anchor
      // keeps the mobile app's words), or with no usable index.
      char sub[96];
      autodjview::rootLine(s.autoDj, sub, sizeof(sub));
      if (Fonts::instance().width(Font::Small, sub) > uitext::kAutoDjRowW) {
        autodjview::rootLine(s.autoDj, sub, sizeof(sub), false);
      }
      drawSetting(r, icons::kAutoDj, tr(S::output_autodj), sub, col::TXT, true);
      break;
    }
    case Haptics: {
      const bool on = ui_.input().hapticsOn();
      drawSetting(r, icons::kVibrate, tr(S::output_haptics), tr(on ? S::output_haptics_on : S::output_haptics_off),
                  col::TXT, false);
      // The switch.
      const int x = r.right - 58;
      const uint16_t track = on ? accent::Output : col::BTN;
      r.c.fillRoundRect(x, 11, 44, 20, 10, track);
      r.c.fillCircle(on ? x + 34 : x + 10, 21, 8, on ? col::DARK : col::DIM);
      break;
    }
    case ScreenOff: drawScreenSetting(r, false); break;
    case Brightness: drawScreenSetting(r, true); break;
    case IdleOff: drawIdleSetting(r); break;
    case CpuSpeed: drawPowerSetting(r, false); break;
    case BtPower: drawPowerSetting(r, true); break;
    case Calibrate:
      drawSetting(r, icons::kGear, tr(S::settings_cal),
                  tr(ui_.input().calibrated() ? S::settings_cal_on : S::settings_cal_off), col::TXT, true);
      break;
    case LanguageRow: drawLanguageRow(r); break;
    case LibraryRow: drawLibraryRow(r); break;
    case AboutRow:
      drawSetting(r, icons::kInfo, tr(S::output_about), tr(S::output_about_sub), col::TXT, true);
      break;
    default: break;
  }
  (void)s;
}

// "Language", and under it the one in use in its own name ("Deutsch",
// "日本語": never translated). Its icon, a globe, is drawn here: a ring,
// a meridian, the equator and two parallels.
void OutputPage::drawLanguageRow(ListView::Row& r) {
  const int cx = r.x + 22;
  r.c.drawCircle(cx, 21, 9, col::SOFT);
  r.c.drawEllipse(cx, 21, 4, 9, col::SOFT);
  r.c.drawFastHLine(cx - 9, 21, 19, col::SOFT);
  r.c.drawFastHLine(cx - 7, 16, 15, col::SOFT);
  r.c.drawFastHLine(cx - 7, 26, 15, col::SOFT);
  const int x = r.x + 44;
  const int right = ListView::chevron(r);
  const char* title = tr(S::settings_language);
  const char* sub = uistr::autonym(uistr::lang());
  ListView::lines(r, x, right, title, strlen(title), sub, strlen(sub), col::TXT);
}

// A language on its page: its name in itself, Bold with the radio on for
// the one in use (UiText's kLangRowX and kLangRowW, measured by
// test_ui_strings).
void OutputPage::drawLanguage(ListView::Row& r) {
  if (r.row >= uistr::kLangs) return;
  const auto l = static_cast<uistr::Lang>(r.row);
  const bool on = l == uistr::lang();
  Fonts::instance().draw(r.c, on ? Font::Bold : Font::Body, uistr::autonym(l), r.x + uitext::kLangRowX, 21,
                         uitext::kLangRowW, col::TXT, r.bg);
  radio(r.c, r.right - 22, 21, on, accent::Output, r.bg);
}

// The Library row (docs/METADATA.md 3.3.6): "Library: 19,410 tracks", where
// their names come from (the longest of librarytext::sourcesText()'s forms
// that fits), and Rescan tags in the settings' pill: dim when there are no
// card's records here, or while the library updates.
void OutputPage::drawLibraryRow(ListView::Row& r) {
  using namespace uitext;
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  icons::drawCentred(r.c, icons::kLibrary, r.x + 22, 21, col::SOFT);
  const int x = r.x + 44;
  const int pillX = r.right - 8 - kRescanPillW;
  char title[96], sub[160];
  librarytext::libraryRowTitle(s.libraryTracks, title, sizeof(title));
  if (f.width(Font::Body, title) > pillX - 8 - x) librarytext::libraryRowTitle(s.libraryTracks, title, sizeof(title), 1);
  const bool can = s.libraryRecords && !s.libraryFenced;
  if (!s.libraryRecords) {
    trf(sub, sizeof(sub), S::library_row_no_card);
  } else if (s.libraryFenced) {
    trf(sub, sizeof(sub), S::library_row_updating);
  } else {
    librarytext::Sources src;
    src.transfer = s.libTransfer;
    src.device = s.libDevice;
    src.none = s.libNone;
    for (int form = 0; form < librarytext::kSourceForms; ++form) {
      librarytext::sourcesText(src, form, sub, sizeof(sub));
      if (f.width(Font::Small, sub) <= pillX - 8 - x) break;
    }
  }
  ListView::lines(r, x, pillX - 8, title, strlen(title), sub, strlen(sub), col::TXT);
  valuePill(r, pillX, kRescanPillW, tr(S::library_row_rescan), can ? col::TXT : col::FAINT,
            r.pressed && can ? col::BTN_HI : col::BTN);
}

uint32_t OutputPage::librarySig(const AppState& s) {
  uint32_t h = 2166136261u;
  for (uint32_t v : {s.libraryTracks, s.libTransfer, s.libDevice, s.libNone,
                     static_cast<uint32_t>(s.libraryRecords) | static_cast<uint32_t>(s.libraryFenced) << 1}) {
    h = (h ^ v) * 16777619u;
  }
  return h;
}

// A tap on the Library row: Rescan tags, asked first (minutes of reading on
// a big card).
void OutputPage::onRescan() {
  const AppState& s = ui_.state();
  if (!s.libraryRecords) {
    ui_.warn(tr(S::library_row_rescan_no_card));
    return;
  }
  if (s.libraryFenced) {
    ui_.toast(tr(S::library_updating_wait), false);
    return;
  }
  const char* const buttons[2] = {tr(S::dialog_cancel), tr(S::library_row_rescan)};
  ask_ = Ask::Rescan;
  ui_.openDialog(this, tr(S::library_row_rescan_title), tr(S::library_row_rescan_body), buttons, 2);
}

void OutputPage::drawSetting(ListView::Row& r, const icons::Icon& icon, const char* title, const char* sub,
                             uint16_t ink, bool chevron) {
  const int x = ListView::icon(r, icon, ink == col::TXT ? col::SOFT : ink);
  const int right = chevron ? ListView::chevron(r) : r.right - 64;
  ListView::lines(r, x, right, title, strlen(title), sub, strlen(sub), ink);
}

// "Screen off after" and "Brightness" (ScreenPower's choices, saved): the
// title and its line, the value in a pill at the right. A tap takes the
// next choice (a sheet has room for 3 rows, not 6). Their icons are drawn
// here: a screen, and a sun.
void OutputPage::drawScreenSetting(ListView::Row& r, bool brightness) {
  using namespace uitext;
  const AppState& s = ui_.state();
  const int cx = r.x + 22;
  if (brightness) {
    // A sun: a disc and 8 rays (the diagonals start and end nearer, so all
    // eight are about as long, 6.5 to 9.5 px out).
    r.c.fillCircle(cx, 21, 4, col::SOFT);
    static const int8_t kRay[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
    for (const auto& d : kRay) {
      const bool diag = d[0] && d[1];
      const int from = diag ? 5 : 7, to = diag ? 7 : 9;
      r.c.drawLine(cx + d[0] * from, 21 + d[1] * from, cx + d[0] * to, 21 + d[1] * to, col::SOFT);
    }
  } else {
    r.c.drawRoundRect(cx - 9, 13, 18, 13, 2, col::SOFT);
    r.c.fillRect(cx - 7, 15, 14, 9, s.screenTimeout == ScreenPower::kNever ? col::SOFT : col::FAINT);
    r.c.fillRect(cx - 4, 28, 8, 2, col::SOFT);
  }
  const int x = r.x + 44;
  const int pillX = r.right - 8 - kSettingPillW;
  const char* title = tr(brightness ? S::settings_brightness : S::settings_screen_off);
  const char* sub = tr(brightness                                 ? S::settings_brightness_sub
                       : s.screenTimeout == ScreenPower::kNever ? S::settings_screen_never_sub
                                                                  : S::settings_screen_off_sub);
  ListView::lines(r, x, pillX - 8, title, strlen(title), sub, strlen(sub), col::TXT);
  const char* value =
      brightness ? ScreenPower::brightnessLabel(s.brightness) : ScreenPower::timeoutLabel(s.screenTimeout);
  valuePill(r, pillX, kSettingPillW, value, col::TXT, r.pressed ? col::BTN_HI : col::BTN);
}

// "Turn off when idle" (IdlePolicy's choices, saved: ENERGY.md item 4): as
// the screen's rows, with a power symbol drawn here (a ring open at the
// top, and its bar).
void OutputPage::drawIdleSetting(ListView::Row& r) {
  using namespace uitext;
  const AppState& s = ui_.state();
  const int cx = r.x + 22;
  const uint16_t ink = s.idleOff == IdlePolicy::kNever ? col::FAINT : col::SOFT;
  r.c.fillArc(cx, 22, 8, 7, 300, 360, ink);
  r.c.fillArc(cx, 22, 8, 7, 0, 240, ink);
  r.c.fillRect(cx - 1, 11, 2, 10, ink);
  const int x = r.x + 44;
  const int pillX = r.right - 8 - kSettingPillW;
  const char* sub = tr(s.idleOff == IdlePolicy::kNever ? S::settings_idle_never_sub : S::settings_idle_off_sub);
  const char* title = tr(S::settings_idle_off);
  ListView::lines(r, x, pillX - 8, title, strlen(title), sub, strlen(sub), col::TXT);
  valuePill(r, pillX, kSettingPillW, IdlePolicy::choiceLabel(s.idleOff), col::TXT, r.pressed ? col::BTN_HI : col::BTN);
}

// "CPU speed" and "Bluetooth power" (PowerChoices, saved: ENERGY.md items
// 6 and 7): as the screen's rows. Their icons are drawn here: a chip (a
// square with 3 pins a side), and 4 signal bars, as many lit as the
// choice reaches (2, 3, 4).
void OutputPage::drawPowerSetting(ListView::Row& r, bool bluetooth) {
  using namespace uitext;
  namespace pc = powerchoice;
  const AppState& s = ui_.state();
  const int cx = r.x + 22;
  char line[64];
  const char* title;
  const char* sub;
  const char* value;
  if (bluetooth) {
    bars(r.c, cx - 9, 28, s.btPower + 2, col::SOFT);
    title = tr(S::settings_bt_power);
    sub = pc::btSub(s.btPower, s.btPowerPending);
    value = pc::btLabel(s.btPower);
  } else {
    r.c.drawRect(cx - 6, 15, 13, 13, col::SOFT);
    r.c.fillRect(cx - 3, 18, 7, 7, col::FAINT);
    for (int i = 0; i < 3; ++i) {
      const int at = i * 4 - 4;  // -4, 0, +4 from the middle
      r.c.drawFastHLine(cx - 9, 21 + at, 3, col::SOFT);
      r.c.drawFastHLine(cx + 7, 21 + at, 3, col::SOFT);
      r.c.drawFastVLine(cx + at, 12, 3, col::SOFT);
      r.c.drawFastVLine(cx + at, 28, 3, col::SOFT);
    }
    title = tr(S::settings_cpu);
    sub = pc::cpuSub(s.cpuMhz, s.cpuRunMhz, line, sizeof(line));
    value = pc::cpuLabel(s.cpuMhz);
  }
  const int x = r.x + 44;
  const int pillX = r.right - 8 - kSettingPillW;
  ListView::lines(r, x, pillX - 8, title, strlen(title), sub, strlen(sub), col::TXT);
  valuePill(r, pillX, kSettingPillW, value, col::TXT, r.pressed ? col::BTN_HI : col::BTN);
}

uint32_t OutputPage::screenSig(const AppState& s) {
  uint32_t h = static_cast<uint32_t>((s.idleOff & 0x0F) << 8 | (s.screenTimeout & 0x0F) << 4 | (s.brightness & 0x0F));
  h |= static_cast<uint32_t>(s.btPower & 0x03) << 12 | (s.btPowerPending ? 1u : 0u) << 14;
  h |= (s.cpuMhz == 240 ? 1u : 0u) << 15 | (s.cpuRunMhz == 240 ? 1u : 0u) << 16 | (s.cpuRunMhz != s.cpuMhz ? 1u : 0u) << 17;
  return h;
}

// The Bluetooth card's top row: the headphones in the state's colour, the
// name, the status line, and the radio (or the spinner).
void OutputPage::drawBtTop(ListView::Row& r) {
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
  const bool active = s.onBluetooth;
  const uint16_t bg = r.pressed ? col::BTN_HI : active ? col::ROW_SEL : col::CARD;
  card(r.c, 1, r.right, bg, active, accent::Output);
  const uint16_t tone = toneColour(v.tone);
  icons::drawCentred(r.c, icons::kHeadphones, 30, 22, v.card == BtCard::Connected ? col::CYAN : tone);
  if (v.card == BtCard::Lost) r.c.drawLine(19, 31, 41, 13, col::RED);
  const char* name = btName();
  f.draw(r.c, Font::Bold, name[0] ? name : tr(v.card == BtCard::NotPaired ? S::bt_name_not_paired : S::bt_name_default),
         52, 14, 220, col::TXT, bg);
  char line[128];
  btStatusLine(v, s.btLink, name, s.btDetail, line, sizeof(line));
  f.draw(r.c, Font::Small, line, 52, 32, uitext::kBtStatusW, tone, bg);
  if (v.spinner) {
    spinner(r.c, 290, 21, spin_, tone);
  } else {
    radio(r.c, 290, 21, active, accent::Output, bg);
  }
}

bool OutputPage::spinPatch(uint32_t row, int cx, int cy, uint16_t colour, uint16_t bg) {
  ListView& list = ui_.list();
  if (list.pressedRow() == static_cast<int32_t>(row)) return false;  // its press look: the row whole
  constexpr int kR = 11;  // the ring (8 px) and its bright dot (2 px), with a margin
  M5Canvas& c = gfx::strip();
  c.fillRect(0, 0, 2 * kR, 2 * kR, bg);
  spinner(c, kR, kR, spin_, colour);
  return list.pushOnRow(row, c, 0, 0, 2 * kR, 2 * kR, cx - kR, cy - kR);
}

// (uitext has the boxes: test_ui_library checks every label fits its box.)
int OutputPage::buttonBoxes(const BtCardView& v, int* x, int* w) const {
  using namespace uitext;
  const int n = v.buttonCount;
  if (n == 1) {
    x[0] = kBtButtons1X;
    w[0] = v.buttons[0] == BtButton::Pair ? kW - 32 : kBtButtons1W;
  } else if (n == 2) {
    for (int i = 0; i < 2; ++i) {
      x[i] = kBtButtons2X[i];
      w[i] = kBtButtons2W[i];
    }
  } else if (n == 3) {
    // Connected: Disconnect, "..." (the More sheet), the volume chip.
    for (int i = 0; i < 3; ++i) {
      x[i] = kBtButtons3X[i];
      w[i] = kBtButtons3W[i];
    }
  }
  return n;
}

void OutputPage::drawBtButtons(ListView::Row& r) {
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
  const bool active = s.onBluetooth;
  const uint16_t bg = active ? col::ROW_SEL : col::CARD;
  card(r.c, 2, r.right, bg, active, accent::Output);
  int x[3], w[3];
  const int n = buttonBoxes(v, x, w);
  const uint32_t now = millis();
  for (int i = 0; i < n; ++i) {
    const BtButton b = v.buttons[i];
    const bool down = r.pressX >= 0 && r.pressX >= x[i] - 3 && (i == n - 1 || r.pressX < x[i + 1] - 3);
    const bool armed = b == BtButton::Forget && forget_.armed(now);
    uint16_t fill = down ? col::BTN_HI : col::BTN;
    uint16_t ink = col::TXT;
    if (b == BtButton::Pair) {
      fill = down ? col::SOFT : accent::Output;
      ink = col::DARK;
    } else if (armed) {
      fill = down ? col::SOFT : col::RED;
      ink = col::DARK;
    }
    r.c.fillRoundRect(x[i], 6, w[i], 28, 8, fill);
    if (b == BtButton::Volume) {
      icons::drawCentred(r.c, icons::kSpeaker, x[i] + 16, 20, col::SOFT);
      char t[16];
      trf(t, sizeof(t), S::format_percent, s.btVolume);
      f.draw(r.c, Font::Body, t, x[i] + uitext::kBtChipTextX, 20, w[i] - uitext::kBtChipTextX - 4, ink, fill);
    } else if (b == BtButton::More) {
      icons::drawCentred(r.c, icons::kMore, x[i] + w[i] / 2, 20, ink);
    } else {
      // (Pair in a narrow box, beside Try again: "Pair new".) A label too
      // wide for its font in Small (Spanish's "Desconectar"), as the Dialog.
      const char* label = armed ? tr(S::bt_tap_again) : btButtonLabel(b, w[i] < uitext::kBtWideButtonW);
      const int room = w[i] - uitext::kBtButtonPad;
      f.draw(r.c, f.fitting(b == BtButton::Pair || armed ? Font::Bold : Font::Body, label, room), label,
             x[i] + w[i] / 2, 20, room, ink, fill, Fonts::Align::Centre);
    }
  }
  if (v.hint) {
    // Resting: why nothing is under way, beside [Connect] (lost: what to try).
    const char* hint = tr(v.lostHint ? S::bt_lost_hint : S::bt_hint);
    for (int k = 0; k < 2; ++k) {
      char line[96];
      uistr::line(hint, k, line, sizeof(line));
      f.draw(r.c, Font::Small, line, uitext::kBtHintX, 12 + 16 * k, uitext::kBtHintW, col::DIM, bg);
    }
  }
}

void OutputPage::drawSpeaker(ListView::Row& r) {
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  const bool active = !s.onBluetooth;
  const bool onChip = r.pressX >= kSpeakerChipHitX && r.pressX < kSpeakerChipHitEnd;
  const uint16_t bg = r.pressed && !onChip ? col::BTN_HI : active ? col::ROW_SEL : col::CARD;
  card(r.c, 0, r.right, bg, active, accent::Output);
  icons::drawCentred(r.c, icons::kSpeaker, 30, 21, active ? col::SOFT : col::DIM);
  f.draw(r.c, Font::Bold, tr(S::speaker_name), 52, 13, 150, col::TXT, bg);
  // The line: why it plays here while the headphones connect (the
  // spec's "Here until SPYDRONE connects", shorter for a long name).
  char line[96];
  if (s.btSession.wanted() && !s.onBluetooth) {
    const bool named = s.btName[0] != 0;
    if (named) {
      trf(line, sizeof(line), S::speaker_here_until, s.btName);
    } else {
      trf(line, sizeof(line), S::speaker_here_until_headphones);
    }
    if (f.width(Font::Small, line) > uitext::kSpeakerLineW) {
      if (named) {
        trf(line, sizeof(line), S::speaker_waits_for, s.btName);
      } else {
        trf(line, sizeof(line), S::speaker_waits_for_headphones);
      }
    }
  } else if (s.silent) {
    trf(line, sizeof(line), S::speaker_silent_mode);
  } else {
    trf(line, sizeof(line), S::speaker_built_in);
  }
  f.draw(r.c, Font::Small, line, 52, 30, uitext::kSpeakerLineW, col::DIM, bg);
  // Its volume chip: the speaker's sheet.
  const bool chipDown = r.pressed && onChip;
  const uint16_t chip = chipDown ? col::BTN_HI : col::BTN;
  r.c.fillRoundRect(uitext::kSpeakerChipX, 9, uitext::kSpeakerChipW, 24, 8, chip);
  char v[16];
  trf(v, sizeof(v), S::format_percent, s.speakerVolume);
  f.draw(r.c, Font::Body, v, uitext::kSpeakerChipX + uitext::kSpeakerChipW / 2, 21, uitext::kSpeakerChipW - 2,
         col::TXT, chip, Fonts::Align::Centre);
  radio(r.c, 290, 21, active, accent::Output, bg);
}

void OutputPage::drawPairStatus(ListView::Row& r) {
  Fonts& f = Fonts::instance();
  const AppState& s = ui_.state();
  r.c.fillSprite(col::BG);
  const bool pairing = s.btLink.phase == BtLink::Phase::Pairing;
  if (!pairing && !search_.searching()) {
    // Stopped after its 2 minutes (or the screen went off): a tap on this
    // row searches again.
    icons::drawCentred(r.c, icons::kPlus, 24, 21, accent::Output);
    f.draw(r.c, Font::Bold, tr(S::pair_search_again), 48, 13, kW - 60, accent::Output, col::BG);
    f.draw(r.c, Font::Small, tr(S::pair_search_stopped), 48, 31, uitext::kPairHintW, col::DIM, col::BG);
    return;
  }
  spinner(r.c, 24, 21, spin_, col::CYAN);
  f.draw(r.c, Font::Bold, tr(pairing ? S::pair_pairing : S::pair_searching), 48, 13, kW - 60, col::TXT, col::BG);
  f.draw(r.c, Font::Small, tr(S::pair_hint), 48, 31, uitext::kPairHintW, col::DIM, col::BG);
}

void OutputPage::drawDevice(ListView::Row& r, const BtDevice& d) {
  const icons::Icon& icon = d.kind == BtDevice::Kind::Speaker ? icons::kSpeaker : icons::kHeadphones;
  const int x = ListView::icon(r, icon, col::SOFT);
  bars(r.c, r.right - 30, 30, BtScanList::bars(d.rssi), col::SOFT);
  char name[64];
  if (d.name[0]) {
    snprintf(name, sizeof(name), "%s", d.name);
  } else {
    char addr[12];
    snprintf(addr, sizeof(addr), "%02X:%02X:%02X", d.addr[3], d.addr[4], d.addr[5]);
    trf(name, sizeof(name), S::pair_unnamed, addr);
  }
  const char* kind = BtScanList::kindName(d.kind);
  ListView::lines(r, x, r.right - 40, name, strlen(name), kind, strlen(kind), col::TXT);
}

void OutputPage::drawAbout(ListView::Row& r) {
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

// A Device info row: its label (Small) over its value (Body, or Small when
// it's long: "cc13597, 2026-10-09, ELF 1a2b3c4d"), no icon: the values get
// the width (UiText's kDeviceInfoX and kDeviceInfoW, measured by
// test_ui_library).
void OutputPage::drawDeviceInfo(ListView::Row& r) {
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

// ---- Output > Auto DJ ----

uint32_t OutputPage::autoDjSig(const autodjview::State& d) {
  uint32_t h = 2166136261u;
  const uint32_t flags = static_cast<uint32_t>(d.on) | static_cast<uint32_t>(d.matchTempo) << 1 |
                         static_cast<uint32_t>(d.matchKey) << 2 | static_cast<uint32_t>(d.usable) << 3 |
                         static_cast<uint32_t>(d.mode) << 4 | static_cast<uint32_t>(d.hold) << 6 |
                         static_cast<uint32_t>(d.index) << 9 | static_cast<uint32_t>(d.floorPct) << 16 |
                         static_cast<uint32_t>(d.floorShown) << 24;
  for (uint32_t v : {flags, d.rows, d.mapped, d.tracks, static_cast<uint32_t>(d.bpmPct) << 8 | d.keyPct}) {
    h = (h ^ v) * 16777619u;
  }
  // (The language: everything is drawn again when it changes.)
  return h;
}

void OutputPage::drawAutoDj(ListView::Row& r) {
  namespace dv = autodjview;
  using namespace uitext;
  const dv::State& d = ui_.state().autoDj;
  if (r.row >= dv::kPageRows) return;
  const auto row = static_cast<dv::PageRow>(r.row);
  const bool live = dv::rowLive(row, d);
  const char* needs = tr(S::autodj_needs_index);
  switch (row) {
    case dv::OnRow: drawAutoDjSwitch(r, tr(S::output_autodj), tr(S::autodj_on_sub), d.on, true); break;
    case dv::AnchorRow:
      drawAutoDjValue(r, tr(S::autodj_anchor), tr(dv::modeText(d.mode)), live ? tr(dv::anchorHint(d.mode)) : needs,
                      live);
      break;
    case dv::StrictRow: {
      char value[48];
      dv::strictnessText(d.floorShown, value, sizeof(value));
      drawAutoDjValue(r, tr(S::autodj_strictness), value, live ? tr(S::autodj_strictness_sub) : needs, live);
      break;
    }
    case dv::BpmRow: drawAutoDjSwitch(r, tr(S::autodj_bpm), live ? tr(S::autodj_bpm_sub) : needs, d.matchTempo, live); break;
    case dv::KeyRow:
      drawAutoDjSwitch(r, tr(S::autodj_harmonic), live ? tr(S::autodj_harmonic_sub) : needs, d.matchKey, live);
      break;
    case dv::StatusRow: {
      // Two lines of Small, no control: why it waits (amber when paused)
      // over the index, or the index's two.
      Fonts& f = Fonts::instance();
      char a[96], b[96];
      dv::statusLines(d, a, sizeof(a), b, sizeof(b));
      const int x = r.x + kAutoDjX;
      const bool paused = dv::rowState(d) == dv::RowState::Paused;
      if (!b[0]) {
        f.draw(r.c, Font::Small, a, x, 21, kAutoDjLineW, col::SOFT, r.bg);
        break;
      }
      f.draw(r.c, Font::Small, a, x, 12, kAutoDjLineW, paused ? col::AMBER : col::SOFT, r.bg);
      f.draw(r.c, Font::Small, b, x, 30, kAutoDjLineW, col::DIM, r.bg);
      break;
    }
    default: break;
  }
}

// A switch row: the title over its line up to the switch (Haptics'), the
// switch at the right; dim without the index it needs.
void OutputPage::drawAutoDjSwitch(ListView::Row& r, const char* title, const char* sub, bool on, bool live) {
  const int x = r.x + uitext::kAutoDjX;
  ListView::lines(r, x, x + uitext::kAutoDjSwitchW, title, strlen(title), sub, strlen(sub), live ? col::TXT : col::FAINT);
  const int sx = r.right - 58;
  const uint16_t track = !live ? col::DIV : on ? accent::Output : col::BTN;
  r.c.fillRoundRect(sx, 11, 44, 20, 10, track);
  r.c.fillCircle(on ? sx + 34 : sx + 10, 21, 8, !live ? col::FAINT : on ? col::DARK : col::DIM);
}

// A value row: the title and its value (the section's blue) on the first
// line when the value fits whole beside it (uitext::autoDjValueBeside()),
// the hint under them; else the value on the second line, in the hint's
// place (the value is the setting). Dim without the index the same, the
// hint being why ("needs an Auto DJ index"): a value under its title
// takes that line too (uitext::autoDjSecondLine(); UiText.h's Output >
// Auto DJ note says why).
void OutputPage::drawAutoDjValue(ListView::Row& r, const char* title, const char* value, const char* hint,
                                 bool live) {
  using namespace uitext;
  Fonts& f = Fonts::instance();
  const int x = r.x + kAutoDjX;
  const int tw = f.width(Font::Body, title);
  const int vw = f.width(Font::Small, value);
  const uint16_t ink = live ? col::TXT : col::FAINT;
  const uint16_t valueInk = live ? accent::Output : col::FAINT;
  const bool beside = autoDjValueBeside(tw, vw);
  f.draw(r.c, Font::Body, title, x, 13, beside ? kAutoDjLineW - vw - kAutoDjValueGap : kAutoDjLineW, ink, r.bg);
  if (beside) f.draw(r.c, Font::Small, value, x + kAutoDjLineW, 13, vw, valueInk, r.bg, Fonts::Align::Right);
  f.draw(r.c, Font::Small, autoDjSecondLine(tw, vw, value, hint), x, 31, kAutoDjLineW, beside ? col::DIM : valueInk,
         r.bg);
}

// A tap on a row of Output > Auto DJ: saved and applied at once (app/AutoDj,
// NVS); the rows follow from the next snapshot. A dim row: why, as a note.
void OutputPage::onAutoDjTap(uint32_t row) {
  namespace dv = autodjview;
  if (row >= dv::kPageRows || row == dv::StatusRow) return;
  const AppState& s = ui_.state();
  const dv::State& d = s.autoDj;
  const auto r = static_cast<dv::PageRow>(row);
  if (!dv::rowLive(r, d)) {
    ui_.warn(tr(S::toast_autodj_needs_index));
    return;
  }
  UiHost& h = ui_.host();
  switch (r) {
    case dv::OnRow:
      Serial.printf("[ui] output: Auto DJ %s\n", d.on ? "off" : "on");
      // On with nothing queued: nothing starts (B stays inert); it follows
      // the next track the listener plays. (No usable index: its toast,
      // random picks, comes from app/AutoDj.)
      if (h.autoDjSetOn(!d.on) && !d.on && s.queueSize == 0 && d.usable) {
        ui_.toast(tr(S::toast_autodj_on_empty), false);
      }
      break;
    case dv::AnchorRow:
      Serial.printf("[ui] output: Auto DJ's Anchor: %s\n", d.mode == autodj::Mode::Locked ? "Follow the vibe" : "Stay on seed");
      h.autoDjSetMode(d.mode == autodj::Mode::Locked ? autodj::Mode::Rolling : autodj::Mode::Locked);
      break;
    case dv::StrictRow: {
      const uint8_t next = dv::tapFloor(d);  // (the step after the value shown)
      Serial.printf("[ui] output: Auto DJ's Match strictness %u %%\n", static_cast<unsigned>(next));
      h.autoDjSetFloor(next);
      break;
    }
    case dv::BpmRow:
      Serial.printf("[ui] output: BPM continuity %s\n", d.matchTempo ? "off" : "on");
      h.autoDjSetMatch(!d.matchTempo, d.matchKey);
      break;
    case dv::KeyRow:
      Serial.printf("[ui] output: Harmonic mixing %s\n", d.matchKey ? "off" : "on");
      h.autoDjSetMatch(d.matchTempo, !d.matchKey);
      break;
    default: break;
  }
}

// ---- taps ----

ListView::Tap OutputPage::onTapAt(uint32_t row, int x, bool rightEdge) {
  if (kind_ == PageKind::Pair) {
    if (row >= 1) {
      pick(static_cast<int>(row) - 1);
    } else if (!search_.searching() && ui_.state().btLink.phase != BtLink::Phase::Pairing) {
      // Search again: the list starts over, for another 2 minutes.
      ui_.tick();
      scan_.clear();
      scanVersion_ = 0;
      ui_.host().btPairScan(true);
      search_.start(millis());
      Serial.println("[ui] output: pair screen: searching again");
      ui_.list().reload();
      repaintHeader();
    }
    return ListView::Tap::Handled;
  }
  if (kind_ == PageKind::About) {
    if (row == DeviceInfoRow) {
      ui_.push(page(PageKind::DeviceInfo));
    } else if (row == Tips) {
      ui_.showCoach();
    }
    return ListView::Tap::Handled;
  }
  if (kind_ == PageKind::DeviceInfo) return ListView::Tap::Handled;  // nothing to tap
  if (kind_ == PageKind::AutoDj) {
    onAutoDjTap(row);
    return ListView::Tap::Handled;
  }
  if (kind_ == PageKind::Language) {
    // At once: saved, and everything drawn again in it (the first boot's
    // goes on to Now Playing).
    if (row < uistr::kLangs) ui_.chooseLanguage(static_cast<uistr::Lang>(row));
    return ListView::Tap::Handled;
  }
  const AppState& s = ui_.state();
  switch (static_cast<RootRow>(row)) {
    case BtTop: onCard(); break;
    case BtButtons: {
      const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
      int bx[3], bw[3];
      const int n = buttonBoxes(v, bx, bw);
      int hit = -1;
      for (int i = 0; i < n; ++i) {
        if (x >= bx[i] - 3) hit = i;
      }
      if (rightEdge && n > 0) hit = n - 1;  // the last reaches the screen's edge
      if (hit >= 0) onCardButton(v.buttons[hit]);
      break;
    }
    case SpeakerRow:
      if (!rightEdge && x >= kSpeakerChipHitX && x < kSpeakerChipHitEnd) {
        ui_.openVolume(0);  // the speaker's volume, whichever plays
      } else if (s.onBluetooth || s.btSession.wanted()) {
        ui_.host().selectOutput(false);  // paused first when it leaves the headphones
      }
      break;
    case LineOut: ui_.warn(tr(S::output_line_out_note)); break;
    case PairNew: ui_.push(page(PageKind::Pair)); break;
    case AutoDjRow: ui_.push(page(PageKind::AutoDj)); break;
    case Haptics: {
      Input& in = ui_.input();
      in.setHapticsOn(!in.hapticsOn());
      Serial.printf("[ui] haptics %s\n", in.hapticsOn() ? "on" : "off");
      break;
    }
    case ScreenOff:
      // The next choice (after Never, 15 s again); the countdown restarts.
      ui_.host().setScreenTimeout((s.screenTimeout + 1) % ScreenPower::kTimeouts);
      break;
    case Brightness:
      // The next level, at once (Max, then Low again).
      ui_.host().setBrightness((s.brightness + 1) % ScreenPower::kBrightnesses);
      break;
    case IdleOff:
      // The next choice (after Never, 10 min again); the countdown restarts.
      ui_.host().setIdleOff((s.idleOff + 1) % IdlePolicy::kChoices);
      break;
    case CpuSpeed: onCpuSpeed(); break;
    case BtPower:
      // The next choice (after High, Low again), saved and applied at once;
      // with the headphones linked, from the next connection (the line says).
      ui_.host().setBtPower(powerchoice::nextBt(s.btPower));
      break;
    case Calibrate: onTouch(); break;
    case LanguageRow: ui_.push(page(PageKind::Language)); break;
    case LibraryRow: onRescan(); break;
    case AboutRow: ui_.push(page(PageKind::About)); break;
    default: break;
  }
  return ListView::Tap::Handled;
}

// A tap on the Bluetooth card itself: make the headphones the output (the
// audio moves once they're connected).
void OutputPage::onCard() {
  const AppState& s = ui_.state();
  const BtCardView v = btCardView(s.btLink, s.btSession, s.btLost);
  switch (v.card) {
    case BtCard::Connected:
      if (!s.onBluetooth) ui_.host().selectOutput(true);
      break;
    case BtCard::Off:
    case BtCard::Resting:
      ui_.host().selectOutput(true);  // connects; the audio waits
      break;
    case BtCard::Failed:
      onCardButton(BtButton::TryAgain);
      break;
    case BtCard::NotPaired:
      ui_.push(page(PageKind::Pair));
      break;
    default:
      break;  // under way: its Cancel is there
  }
}

void OutputPage::onCardButton(BtButton b) {
  const AppState& s = ui_.state();
  switch (b) {
    case BtButton::Pair: ui_.push(page(PageKind::Pair)); break;
    case BtButton::Connect: ui_.host().selectOutput(true); break;
    case BtButton::TryAgain:
      if (btCardView(s.btLink, s.btSession, s.btLost).pairFailed) {
        ui_.push(page(PageKind::Pair));  // a pairing: pick them again
      } else {
        ui_.host().selectOutput(true);
      }
      break;
    case BtButton::Cancel: ui_.host().btDisconnect(); break;
    case BtButton::Disconnect:
      ui_.host().btDisconnect();
      ui_.toast(tr((s.play == PlayState::Playing || s.play == PlayState::Waiting) && s.onBluetooth
                       ? S::bt_disconnected_paused
                       : S::bt_disconnected),
                false);
      break;
    case BtButton::Forget:
      if (forget_.tap(millis())) forget();
      ui_.list().refreshRow(BtButtons);  // "Tap again", or the new buttons
      drawnBt_ = btSig();
      break;
    case BtButton::Volume: ui_.openVolume(1); break;
    case BtButton::More: {
      // Connected: what is used rarely, away from Disconnect and the volume.
      char forgetRow[96], title[48];
      if (s.btName[0]) {
        trf(forgetRow, sizeof(forgetRow), S::bt_more_forget, s.btName);
      } else {
        trf(forgetRow, sizeof(forgetRow), S::bt_more_forget_these);
      }
      snprintf(title, sizeof(title), "%s", s.btName[0] ? s.btName : tr(S::bt_more_title_default));
      const char* rows[3] = {tr(S::bt_more_disconnect), tr(S::bt_more_pair_new), forgetRow};
      ask_ = Ask::More;
      ui_.openSheet(this, title, rows, 3, nullptr, -1, /*danger=*/2);
      break;
    }
    case BtButton::None: break;
  }
}

void OutputPage::forget() {
  const AppState& s = ui_.state();
  char text[96];
  if (s.btName[0]) {
    trf(text, sizeof(text), S::bt_forgot, s.btName);
  } else {
    trf(text, sizeof(text), S::bt_forgot_headphones);
  }
  ui_.host().btForget();
  ui_.toast(text, false);
}

// "Touch calibration": the choices as full-width rows (y is what the panel
// reads true, so a stack of rows works whatever x does).
void OutputPage::onTouch() {
  const bool saved = ui_.input().calibrated();
  const char* rows[3] = {tr(S::settings_cal_calibrate), tr(S::settings_cal_test_taps), tr(S::settings_cal_remove_row)};
  const char* details[3] = {tr(S::settings_cal_calibrate_detail), "", ""};
  ask_ = Ask::Touch;
  ui_.openSheet(this, tr(S::settings_cal), rows, saved ? 3 : 2, details, /*primary=*/0, /*danger=*/saved ? 2 : -1);
}

void OutputPage::onSheet(int choice) {
  if (ask_ == Ask::Touch) {
    ask_ = Ask::None;
    if (choice == 0 || choice == 1) {
      ui_.host().openCalibration(choice == 1);
    } else if (choice == 2) {
      // Remove, from the sheet: a dialog (as Forget: a second tap on a row
      // that isn't there any more can't confirm it).
      const char* const buttons[2] = {tr(S::dialog_cancel), tr(S::settings_cal_remove)};
      ask_ = Ask::RemoveCal;
      ui_.openDialog(this, tr(S::settings_cal_remove_title), tr(S::settings_cal_remove_body), buttons, 2,
                     /*danger=*/true);
    }
    return;
  }
  if (ask_ != Ask::More) return;
  ask_ = Ask::None;
  const AppState& s = ui_.state();
  switch (choice) {
    case 0: onCardButton(BtButton::Disconnect); break;
    case 1: ui_.push(page(PageKind::Pair)); break;
    case 2: {
      // Forget, from the sheet: a dialog (the sheet is gone; a second tap
      // on a row that isn't there any more can't confirm it).
      const char* const buttons[2] = {tr(S::dialog_cancel), tr(S::bt_forget_button)};
      char title[96];
      if (s.btName[0]) {
        trf(title, sizeof(title), S::bt_forget_title, s.btName);
      } else {
        trf(title, sizeof(title), S::bt_forget_title_headphones);
      }
      ask_ = Ask::Forget;
      ui_.openDialog(this, title, tr(S::bt_forget_body), buttons, 2, /*danger=*/true);
      break;
    }
    default: break;
  }
}

// "CPU speed": the other choice. 240 <-> 160 can't switch while Bluetooth
// runs, so it takes a restart, asked first (Cancel saves nothing). When the
// console's Pcb saved the other one since this boot, the choice asked for
// is the clock that runs: saved, nothing restarts. While a pairing is under
// way the restart isn't offered (it would drop the pairing): a toast.
void OutputPage::onCpuSpeed() {
  namespace pc = powerchoice;
  const AppState& s = ui_.state();
  const uint16_t to = pc::otherCpuMhz(s.cpuMhz);
  switch (pc::cpuTap(s.cpuMhz, s.cpuRunMhz, pairingUnderWay(s))) {
    case pc::CpuTap::SaveOnly: ui_.host().setCpuSpeed(to); return;
    case pc::CpuTap::WaitPairing: ui_.toast(tr(S::settings_cpu_wait_pairing), false); return;
    case pc::CpuTap::AskRestart: break;
  }
  const char* const buttons[2] = {tr(S::dialog_cancel), tr(S::settings_cpu_restart)};
  char title[96];
  pc::cpuDialogTitle(to, title, sizeof(title));
  ask_ = Ask::CpuRestart;
  askCpuMhz_ = to;
  ui_.openDialog(this, title, pc::cpuDialogBody(to), buttons, 2);
}

// A device on the Pair screen: pair (asked first when it replaces the
// headphones remembered now).
void OutputPage::pick(int device) {
  if (device < 0 || device >= scan_.count()) return;
  picked_ = device;
  const AppState& s = ui_.state();
  const BtDevice& d = scan_.at(device);
  ask_ = Ask::Pair;
  if (s.btLink.remembered) {
    const char* const buttons[2] = {tr(S::dialog_cancel), tr(S::pair_confirm_button)};
    char title[96], body[256];
    if (d.name[0]) {
      trf(title, sizeof(title), S::pair_confirm_title, d.name);
    } else {
      trf(title, sizeof(title), S::pair_confirm_title_device);
    }
    if (s.btName[0]) {
      trf(body, sizeof(body), S::pair_confirm_body, s.btName);
    } else {
      trf(body, sizeof(body), S::pair_confirm_body_unnamed);
    }
    ui_.openDialog(this, title, body, buttons, 2);
    return;
  }
  onDialog(1);
}

void OutputPage::onDialog(int button) {
  const Ask ask = ask_;
  ask_ = Ask::None;
  if (ask == Ask::Forget) {
    if (button == 1 && kind_ == PageKind::Output) forget();
    return;
  }
  if (ask == Ask::RemoveCal) {
    if (button != 1 || kind_ != PageKind::Output) return;
    ui_.input().resetCalibration();
    Serial.println("[ui] touch calibration removed: no correction");
    ui_.toast(tr(S::settings_cal_removed), false);  // (its row follows: tick())
    return;
  }
  if (ask == Ask::Rescan) {
    if (button != 1 || kind_ != PageKind::Output) return;
    if (ui_.host().rescanTags()) {
      ui_.toast(tr(S::library_row_rescan_started), false);
    } else {
      ui_.warn(tr(S::library_row_rescan_no_card));
    }
    return;
  }
  if (ask == Ask::CpuRestart) {
    // Cancel saves nothing.
    if (button != 1 || kind_ != PageKind::Output || !askCpuMhz_) return;
    const uint16_t mhz = askCpuMhz_;
    askCpuMhz_ = 0;
    if (ui_.host().setCpuSpeed(mhz)) {
      // Up until the restart (at most 3 s: the headphones let go, the amp
      // off), so the screen never goes dark without it.
      char text[96];
      powerchoice::cpuRestartingText(mhz, text, sizeof(text));
      ui_.note(text, powerchoice::kRestartToastMs);
    } else if (pairingUnderWay(ui_.state())) {
      ui_.toast(tr(S::settings_cpu_wait_pairing), false);  // (one began while the dialog was up: nothing saved)
    }
    return;
  }
  if (ask != Ask::Pair || kind_ != PageKind::Pair || button != 1 || picked_ < 0 || picked_ >= scan_.count()) return;
  const BtDevice d = scan_.at(picked_);
  picked_ = -1;
  searchEnded_ = false;
  char text[96];
  if (ui_.host().btPairWith(d)) {
    if (d.name[0]) {
      trf(text, sizeof(text), S::pair_pairing_with, d.name);
    } else {
      trf(text, sizeof(text), S::pair_pairing_with_it);
    }
  } else if (d.name[0]) {
    // The headphones linked now: nothing to pair (they're the output now).
    trf(text, sizeof(text), S::pair_connected_already, d.name);
  } else {
    trf(text, sizeof(text), S::pair_connected_already_them);
  }
  ui_.back();  // the card shows how it goes
  ui_.toast(text, false);
}

}  // namespace ui
