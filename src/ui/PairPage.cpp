// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Output > Pair new headphones (mockup 21; the page's shape is in Pages.h):
// the search and what it finds, a tap pairs. The search is the page's own:
// it starts on the way in, stops on the way out, after its 2 minutes, or
// with the screen.
#include <cstdio>
#include <cstring>

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
constexpr uint32_t kScanPollMs = 250;
}  // namespace

void PairPage::enter(NavModel::PageRef& ref) {
  picked_ = -1;
  searchEnded_ = false;
  asking_ = false;
  scan_.clear();
  scanVersion_ = 0;
  nextScanMs_ = 0;
  ui_.host().btPairScan(true);
  search_.start(millis());
  Serial.println("[ui] output: pair screen (scanning)");
  show(ref);
}

void PairPage::leave() {
  ui_.host().btPairScan(false);
  search_.stop();
  OutputTabPage::leave();
}

// The screen went off: the Pair screen's scan (an inquiry, back to back)
// stops, as after its 2 minutes; the row offers "Search again" on the wake.
// A pairing under way keeps the screen lit, so it isn't one, and the
// search itself holds a lit screen lit (searching(): main.cpp's
// ScreenControl::Hold): what turns a held screen off still does (the PWR
// key, the sleep timer's pause, the console's Ps0). Before, it dimmed and
// went off as ever: the dim screen swallowed the listener's taps as wakes,
// and the screen going off stopped the search (2026-10-04: three tries to
// pair).
void PairPage::screenOff() {
  if (!search_.searching()) return;
  if (ui_.state().btLink.phase == BtLink::Phase::Pairing) return;
  search_.stop();
  searchEnded_ = true;  // (the wake pushes the row as it was: "Searching", its spinner stopped)
  ui_.host().btPairScanPause();  // (the page stays: nothing else is tried until it closes)
  Serial.println("[ui] output: pair screen: the search stopped (the screen went off; tap Search again)");
}

// The Pair screen's 2 minutes, checked every pass: a dialog over the page
// (the headphones lost, "Couldn't reach") stops its update(), not the
// inquiry, which costs power while it runs. Stopped, and the status row
// offers to search again (drawn by update() once the page shows). The
// list stays, and nothing else is tried until the page closes.
void PairPage::tick(uint32_t nowMs) {
  if (!search_.due(nowMs)) return;
  ui_.host().btPairScanPause();
  searchEnded_ = true;
  Serial.println("[ui] output: pair screen: the search stopped after 2 min (tap Search again)");
}

Header PairPage::header() const {
  Header h;
  h.subWhole = true;  // (a count: whole beside the title, or not at all)
  h.back = true;
  h.title = tr(S::pair_title);
  trnf(headerSub_, sizeof(headerSub_), S::pair_found, scan_.count(), scan_.count());
  h.sub = scan_.count() ? headerSub_ : "";
  return h;
}

const char* PairPage::describeMore() const {
  return !search_.searching() ? ", search stopped" : scan_.count() ? ", devices found" : ", scanning";
}

bool PairPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  ListView& list = ui_.list();
  const bool still = !list.animating();
  if (searchEnded_) {
    searchEnded_ = false;
    list.refreshRow(0);  // "Search again" (tick() or screenOff() stopped it)
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
    if (!spinPatch(list, 0, 24, 21, spin_, col::CYAN, col::BG)) list.refreshRow(0);  // (its row draws it at (24, 21))
  }
  return list.update(nowMs, frameDue, wholeRows);
}

// ---- the list ----

uint32_t PairPage::rows() { return 1 + static_cast<uint32_t>(scan_.count()); }

void PairPage::drawRow(ListView::Row& r) {
  if (r.row == 0) {
    drawPairStatus(r);
  } else if (static_cast<int>(r.row) - 1 < scan_.count()) {
    drawDevice(r, scan_.at(static_cast<int>(r.row) - 1));
  }
}

void PairPage::drawPairStatus(ListView::Row& r) {
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

void PairPage::drawDevice(ListView::Row& r, const BtDevice& d) {
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

// ---- taps ----

ListView::Tap PairPage::onTapAt(uint32_t row, int x, bool rightEdge) {
  (void)x;
  (void)rightEdge;
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

// A device on the Pair screen: pair (asked first when it replaces the
// headphones remembered now).
void PairPage::pick(int device) {
  if (device < 0 || device >= scan_.count()) return;
  picked_ = device;
  const AppState& s = ui_.state();
  const BtDevice& d = scan_.at(device);
  asking_ = true;
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

void PairPage::onDialog(int button) {
  const bool asked = asking_;
  asking_ = false;
  if (!asked || !shown() || button != 1 || picked_ < 0 || picked_ >= scan_.count()) return;
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
