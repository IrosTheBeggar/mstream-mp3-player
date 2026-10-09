// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Output tab (the tab bar spec §6.6, mockups 19-21 and 23, with the
// review's grafts; the page's shape is in Pages.h): where the music plays,
// each output's volume, the Pair screen, the settings, About and its Device
// info. One page object for the tab's four kinds of page (Output, Pair,
// About, Device info), each a list on the hardware scroll.
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
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

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

}  // namespace

const char* OutputPage::btName() const {
  const AppState& s = ui_.state();
  return s.btName[0] ? s.btName : "";
}

// ---- the page ----

void OutputPage::enter(NavModel::PageRef& ref) {
  kind_ = static_cast<PageKind>(ref.kind);
  ref_ = &ref;
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
  ui_.list().attach(this, &ref, 0);
}

void OutputPage::leave() {
  if (kind_ == PageKind::Pair) ui_.host().btPairScan(false);
  search_.stop();
  ui_.list().detach();
}

void OutputPage::repaint() {
  repaintHeader();
  ui_.list().invalidate();
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
  switch (kind_) {
    case PageKind::Pair:
      h.back = true;
      h.title = "Pair headphones";
      snprintf(headerSub_, sizeof(headerSub_), "%d found", scan_.count());
      h.sub = scan_.count() ? headerSub_ : "";
      break;
    case PageKind::About:
      h.back = true;
      h.title = "About";
      h.sub = "this player";
      break;
    case PageKind::DeviceInfo:
      h.back = true;
      h.title = uitext::kDeviceInfoTitle;
      h.sub = uitext::kDeviceInfoHeaderSub;
      break;
    default:
      h.title = "Output";
      h.sub = "where the music plays";
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
    if (btCardView(s.btLink, s.btSession, s.btLost).spinner && still &&
        static_cast<int32_t>(nowMs - nextSpinMs_) >= 0) {
      nextSpinMs_ = nowMs + kSpinMs;
      spin_ = static_cast<uint8_t>((spin_ + 1) % 8);
      list.refreshRow(BtTop);
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
      list.refreshRow(0);
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
    default: return kRootRows;
  }
}

bool OutputPage::emptyState(EmptyState& e) {
  (void)e;
  return false;  // never empty
}

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
  const AppState& s = ui_.state();
  switch (static_cast<RootRow>(r.row)) {
    case BtTop: drawBtTop(r); break;
    case BtButtons: drawBtButtons(r); break;
    case SpeakerRow: drawSpeaker(r); break;
    case LineOut:
      drawSetting(r, icons::kJack, "Line out", uitext::kLineOutSub, col::DIM, false);
      break;
    case PairNew:
      drawSetting(r, icons::kPlus, "Pair new headphones", "", accent::Output, true);
      break;
    case Haptics: {
      const bool on = ui_.input().hapticsOn();
      drawSetting(r, icons::kVibrate, "Haptics", on ? "a tick for taps, two for holds" : "off", col::TXT, false);
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
      drawSetting(r, icons::kGear, uitext::kCalRowTitle,
                  ui_.input().calibrated() ? uitext::kCalSubOn : uitext::kCalSubOff, col::TXT, true);
      break;
    case AboutRow:
      drawSetting(r, icons::kInfo, "About", "battery, storage, library, version", col::TXT, true);
      break;
    default: break;
  }
  (void)s;
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
  const char* title = brightness ? kBrightnessTitle : kScreenOffTitle;
  const char* sub = brightness                                 ? kBrightnessSub
                    : s.screenTimeout == ScreenPower::kNever ? kScreenNeverSub
                                                               : kScreenOffSub;
  ListView::lines(r, x, pillX - 8, title, strlen(title), sub, strlen(sub), col::TXT);
  const uint16_t pill = r.pressed ? col::BTN_HI : col::BTN;
  r.c.fillRoundRect(pillX, 9, kSettingPillW, 24, 8, pill);
  const char* value =
      brightness ? ScreenPower::brightnessLabel(s.brightness) : ScreenPower::timeoutLabel(s.screenTimeout);
  Fonts::instance().draw(r.c, Font::Body, value, pillX + kSettingPillW / 2, 21, kSettingPillW - kSettingPillPad,
                         col::TXT, pill, Fonts::Align::Centre);
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
  const char* sub = s.idleOff == IdlePolicy::kNever ? kIdleNeverSub : kIdleOffSub;
  ListView::lines(r, x, pillX - 8, kIdleOffTitle, strlen(kIdleOffTitle), sub, strlen(sub), col::TXT);
  const uint16_t pill = r.pressed ? col::BTN_HI : col::BTN;
  r.c.fillRoundRect(pillX, 9, kSettingPillW, 24, 8, pill);
  Fonts::instance().draw(r.c, Font::Body, IdlePolicy::choiceLabel(s.idleOff), pillX + kSettingPillW / 2, 21,
                         kSettingPillW - kSettingPillPad, col::TXT, pill, Fonts::Align::Centre);
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
  char line[32];
  const char* title;
  const char* sub;
  const char* value;
  if (bluetooth) {
    bars(r.c, cx - 9, 28, s.btPower + 2, col::SOFT);
    title = kBtPowerTitle;
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
    title = kCpuTitle;
    sub = pc::cpuSub(s.cpuMhz, s.cpuRunMhz, line, sizeof(line));
    value = pc::cpuLabel(s.cpuMhz);
  }
  const int x = r.x + 44;
  const int pillX = r.right - 8 - kSettingPillW;
  ListView::lines(r, x, pillX - 8, title, strlen(title), sub, strlen(sub), col::TXT);
  const uint16_t pill = r.pressed ? col::BTN_HI : col::BTN;
  r.c.fillRoundRect(pillX, 9, kSettingPillW, 24, 8, pill);
  Fonts::instance().draw(r.c, Font::Body, value, pillX + kSettingPillW / 2, 21, kSettingPillW - kSettingPillPad,
                         col::TXT, pill, Fonts::Align::Centre);
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
  f.draw(r.c, Font::Bold, name[0] ? name : v.card == BtCard::NotPaired ? "Bluetooth headphones" : "Headphones", 52, 14,
         220, col::TXT, bg);
  char line[80];
  btStatusLine(v, s.btLink, name, s.btDetail, line, sizeof(line));
  f.draw(r.c, Font::Small, line, 52, 32, uitext::kBtStatusW, tone, bg);
  if (v.spinner) {
    spinner(r.c, 290, 21, spin_, tone);
  } else {
    radio(r.c, 290, 21, active, accent::Output, bg);
  }
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
      char t[8];
      snprintf(t, sizeof(t), "%u%%", static_cast<unsigned>(s.btVolume));
      f.draw(r.c, Font::Body, t, x[i] + uitext::kBtChipTextX, 20, w[i] - uitext::kBtChipTextX - 4, ink, fill);
    } else if (b == BtButton::More) {
      icons::drawCentred(r.c, icons::kMore, x[i] + w[i] / 2, 20, ink);
    } else {
      // (Pair in a narrow box, beside Try again: "Pair new".)
      f.draw(r.c, b == BtButton::Pair || armed ? Font::Bold : Font::Body,
             armed ? uitext::kForgetArmed : btButtonLabel(b, w[i] < uitext::kBtWideButtonW), x[i] + w[i] / 2, 20,
             w[i] - uitext::kBtButtonPad, ink, fill, Fonts::Align::Centre);
    }
  }
  if (v.hint) {
    // Resting: why nothing is under way, beside [Connect] (lost: what to try).
    f.draw(r.c, Font::Small, v.lostHint ? uitext::kBtLostHintLine1 : uitext::kBtHintLine1, uitext::kBtHintX, 12,
           uitext::kBtHintW, col::DIM, bg);
    f.draw(r.c, Font::Small, v.lostHint ? uitext::kBtLostHintLine2 : uitext::kBtHintLine2, uitext::kBtHintX, 28,
           uitext::kBtHintW, col::DIM, bg);
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
  f.draw(r.c, Font::Bold, "Speaker", 52, 13, 150, col::TXT, bg);
  // The line: why it plays here while the headphones connect (the
  // spec's "Here until SPYDRONE connects", shorter for a long name).
  char line[64];
  if (s.btSession.wanted() && !s.onBluetooth) {
    const char* them = s.btName[0] ? s.btName : "the headphones";
    snprintf(line, sizeof(line), "Here until %s connects", them);
    if (f.width(Font::Small, line) > uitext::kSpeakerLineW) snprintf(line, sizeof(line), "Waits for %s", them);
  } else if (s.silent) {
    snprintf(line, sizeof(line), "%s", uitext::kSilentMode);
  } else {
    snprintf(line, sizeof(line), "Built-in");
  }
  f.draw(r.c, Font::Small, line, 52, 30, uitext::kSpeakerLineW, col::DIM, bg);
  // Its volume chip: the speaker's sheet.
  const bool chipDown = r.pressed && onChip;
  const uint16_t chip = chipDown ? col::BTN_HI : col::BTN;
  r.c.fillRoundRect(uitext::kSpeakerChipX, 9, uitext::kSpeakerChipW, 24, 8, chip);
  char v[8];
  snprintf(v, sizeof(v), "%u%%", static_cast<unsigned>(s.speakerVolume));
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
    f.draw(r.c, Font::Bold, uitext::kPairSearchAgain, 48, 13, kW - 60, accent::Output, col::BG);
    f.draw(r.c, Font::Small, uitext::kPairSearchStopped, 48, 31, uitext::kPairHintW, col::DIM, col::BG);
    return;
  }
  spinner(r.c, 24, 21, spin_, col::CYAN);
  f.draw(r.c, Font::Bold, pairing ? "Pairing..." : "Searching", 48, 13, kW - 60, col::TXT, col::BG);
  f.draw(r.c, Font::Small, uitext::kPairHint, 48, 31, uitext::kPairHintW, col::DIM, col::BG);
}

void OutputPage::drawDevice(ListView::Row& r, const BtDevice& d) {
  const icons::Icon& icon = d.kind == BtDevice::Kind::Speaker ? icons::kSpeaker : icons::kHeadphones;
  const int x = ListView::icon(r, icon, col::SOFT);
  bars(r.c, r.right - 30, 30, BtScanList::bars(d.rssi), col::SOFT);
  char name[40];
  if (d.name[0]) {
    snprintf(name, sizeof(name), "%s", d.name);
  } else {
    snprintf(name, sizeof(name), "Unnamed %02X:%02X:%02X", d.addr[3], d.addr[4], d.addr[5]);
  }
  const char* kind = BtScanList::kindName(d.kind);
  ListView::lines(r, x, r.right - 40, name, strlen(name), kind, strlen(kind), col::TXT);
}

void OutputPage::drawAbout(ListView::Row& r) {
  const AppState& s = ui_.state();
  char value[80] = "";
  char labelText[64];
  const char* label = "";
  const icons::Icon* icon = &icons::kInfo;
  switch (static_cast<AboutItem>(r.row)) {
    case Battery:
      label = "Battery";
      snprintf(value, sizeof(value), "%u%%%s", static_cast<unsigned>(s.battery), s.charging ? ", charging" : "");
      break;
    case Storage:
      label = "Storage";
      snprintf(value, sizeof(value), "%s", about_.storage);
      icon = &icons::kSdCard;
      break;
    case LibraryInfo: {
      label = "Library";
      const LibraryIndex* i = ui_.library().index();
      if (i && i->ready()) {
        snprintf(value, sizeof(value), uitext::kAboutLibrary, static_cast<unsigned long>(i->trackCount()),
                 static_cast<unsigned long>(i->artistCount()), static_cast<unsigned long>(i->albumCount()));
      } else {
        snprintf(value, sizeof(value), "no music found");
      }
      icon = &icons::kLibrary;
      break;
    }
    case Headphones:
      label = "Headphones";
      snprintf(value, sizeof(value), "%s", about_.bluetooth);
      icon = &icons::kHeadphones;
      break;
    case PowerInfo:
      label = uitext::kAboutPower;
      snprintf(value, sizeof(value), "%s", about_.power);
      break;
    case Memory:
      label = "Memory free";
      snprintf(value, sizeof(value), uitext::kAboutMemory, static_cast<unsigned long>(about_.ramFree / 1024),
               static_cast<unsigned long>(about_.ramMin / 1024), about_.psramFree / (1024.0f * 1024.0f));
      break;
    case Version:
      snprintf(labelText, sizeof(labelText), uitext::kAboutVersionLabel, about_.built, about_.elf);
      label = labelText;
      snprintf(value, sizeof(value), "%s", about_.version);
      break;
    case LicenceInfo:
      label = uitext::kAboutLicenceLabel;
      snprintf(value, sizeof(value), "%s", uitext::kAboutLicence);
      break;
    case SourceInfo:
      label = uitext::kAboutSourceLabel;
      snprintf(value, sizeof(value), "%s", uitext::kAboutSourceRepo);
      break;
    case DeviceInfoRow: {
      // A page of its own (what the boot screen used to list).
      const int x = ListView::icon(r, icons::kInfo, col::SOFT);
      const int right = ListView::chevron(r);
      using namespace uitext;
      ListView::lines(r, x, right, kDeviceInfoTitle, strlen(kDeviceInfoTitle), kDeviceInfoRowSub,
                      strlen(kDeviceInfoRowSub), col::TXT);
      return;
    }
    case Tips: {
      const int x = ListView::icon(r, icons::kInfo, accent::Output);
      const char* t = "Show the tips again";
      const char* sub = "the touch buttons and the tabs";
      ListView::lines(r, x, r.right - 8, t, strlen(t), sub, strlen(sub), accent::Output);
      return;
    }
    default: return;
  }
  // The label small over its value (small too when it's long: "10000
  // tracks, 600 artists, 1500 albums").
  Fonts& f = Fonts::instance();
  const int x = ListView::icon(r, *icon, col::DIM);
  const int room = r.right - 8 - x;
  f.draw(r.c, Font::Small, label, x, 12, room, col::DIM, r.bg);
  f.draw(r.c, f.width(Font::Body, value) <= room ? Font::Body : Font::Small, value, x, 30, room, col::TXT, r.bg);
}

// A Device info row: its label (Small) over its value (Body, or Small when
// it's long: "cc13597, 2026-10-09, ELF 1a2b3c4d"), no icon: the values get
// the width (UiText's kDeviceInfoX and kDeviceInfoW, measured by
// test_ui_library).
void OutputPage::drawDeviceInfo(ListView::Row& r) {
  if (r.row >= static_cast<uint32_t>(deviceinfo::kItems)) return;
  const auto item = static_cast<deviceinfo::Item>(r.row);
  char value[64];
  deviceinfo::value(item, info_, value, sizeof(value));
  Fonts& f = Fonts::instance();
  const int x = r.x + uitext::kDeviceInfoX;
  const int room = r.right - 8 - x;
  f.draw(r.c, Font::Small, deviceinfo::label(item), x, 12, room, col::DIM, r.bg);
  f.draw(r.c, f.width(Font::Body, value) <= room ? Font::Body : Font::Small, value, x, 30, room, col::TXT, r.bg);
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
    case LineOut: ui_.warn("The line-out module isn't fitted yet"); break;
    case PairNew: ui_.push(page(PageKind::Pair)); break;
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
      ui_.toast((s.play == PlayState::Playing || s.play == PlayState::Waiting) && s.onBluetooth
                    ? "Disconnected: paused on the speaker"
                    : "Disconnected",
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
      char forgetRow[48], title[48];
      snprintf(forgetRow, sizeof(forgetRow), "Forget %s", s.btName[0] ? s.btName : "these headphones");
      snprintf(title, sizeof(title), "%s", s.btName[0] ? s.btName : "Headphones");
      const char* rows[3] = {"Disconnect", "Pair new headphones", forgetRow};
      ask_ = Ask::More;
      ui_.openSheet(this, title, rows, 3, nullptr, -1, /*danger=*/2);
      break;
    }
    case BtButton::None: break;
  }
}

void OutputPage::forget() {
  const AppState& s = ui_.state();
  char text[64];
  snprintf(text, sizeof(text), "Forgot %s", s.btName[0] ? s.btName : "the headphones");
  ui_.host().btForget();
  ui_.toast(text, false);
}

// "Touch calibration": the choices as full-width rows (y is what the panel
// reads true, so a stack of rows works whatever x does).
void OutputPage::onTouch() {
  using namespace uitext;
  const bool saved = ui_.input().calibrated();
  const char* rows[3] = {kCalCalibrate, kCalCheckTitle, kCalRemoveRow};
  const char* details[3] = {kCalCalibrateDetail, "", ""};
  ask_ = Ask::Touch;
  ui_.openSheet(this, kCalRowTitle, rows, saved ? 3 : 2, details, /*primary=*/0, /*danger=*/saved ? 2 : -1);
}

void OutputPage::onSheet(int choice) {
  if (ask_ == Ask::Touch) {
    ask_ = Ask::None;
    if (choice == 0 || choice == 1) {
      ui_.host().openCalibration(choice == 1);
    } else if (choice == 2) {
      // Remove, from the sheet: a dialog (as Forget: a second tap on a row
      // that isn't there any more can't confirm it).
      static const char* const kButtons[2] = {"Cancel", uitext::kCalRemove};
      ask_ = Ask::RemoveCal;
      ui_.openDialog(this, uitext::kCalRemoveTitle, uitext::kCalRemoveBody, kButtons, 2, /*danger=*/true);
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
      static const char* const kButtons[2] = {"Cancel", "Forget"};
      char title[48];
      snprintf(title, sizeof(title), "Forget %s?", s.btName[0] ? s.btName : "the headphones");
      ask_ = Ask::Forget;
      ui_.openDialog(this, title, "They won't connect again until you pair them on Pair new headphones.", kButtons,
                     2, /*danger=*/true);
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
    case pc::CpuTap::WaitPairing: ui_.toast(uitext::kCpuWaitPairing, false); return;
    case pc::CpuTap::AskRestart: break;
  }
  static const char* const kButtons[2] = {"Cancel", uitext::kCpuRestart};
  char title[48];
  pc::cpuDialogTitle(to, title, sizeof(title));
  ask_ = Ask::CpuRestart;
  askCpuMhz_ = to;
  ui_.openDialog(this, title, pc::cpuDialogBody(to), kButtons, 2);
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
    static const char* const kButtons[2] = {"Cancel", "Pair"};
    char title[48], body[128];
    snprintf(title, sizeof(title), "Pair with %s?", d.name[0] ? d.name : "this device");
    snprintf(body, sizeof(body), "It replaces %s once it's connected; if pairing fails, %s stays.",
             s.btName[0] ? s.btName : "the headphones paired now", s.btName[0] ? s.btName : "they");
    ui_.openDialog(this, title, body, kButtons, 2);
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
    ui_.toast(uitext::kCalRemoved, false);  // (its row follows: tick())
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
      char text[48];
      powerchoice::cpuRestartingText(mhz, text, sizeof(text));
      ui_.note(text, powerchoice::kRestartToastMs);
    } else if (pairingUnderWay(ui_.state())) {
      ui_.toast(uitext::kCpuWaitPairing, false);  // (one began while the dialog was up: nothing saved)
    }
    return;
  }
  if (ask != Ask::Pair || kind_ != PageKind::Pair || button != 1 || picked_ < 0 || picked_ >= scan_.count()) return;
  const BtDevice d = scan_.at(picked_);
  picked_ = -1;
  searchEnded_ = false;
  char text[64];
  if (ui_.host().btPairWith(d)) {
    snprintf(text, sizeof(text), "Pairing with %s...", d.name[0] ? d.name : "it");
  } else {
    // The headphones linked now: nothing to pair (they're the output now).
    snprintf(text, sizeof(text), "Connected to %s already", d.name[0] ? d.name : "them");
  }
  ui_.back();  // the card shows how it goes
  ui_.toast(text, false);
}

}  // namespace ui
