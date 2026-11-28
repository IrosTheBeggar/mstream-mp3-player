// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Output > Auto DJ (the engine design's 5.1; the page's shape is in
// Pages.h, its rows lib/core's AutoDjView): the switch, the settings, the
// status, and the mood filters' editor over it.
#include <cstdio>
#include <cstring>

#include "UiStrings.h"
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

using uistr::S;

uint32_t AutoDjPage::autoDjSig(const autodjview::State& d) {
  uint32_t h = 2166136261u;
  const uint32_t flags = static_cast<uint32_t>(d.on) | static_cast<uint32_t>(d.matchTempo) << 1 |
                         static_cast<uint32_t>(d.matchKey) << 2 | static_cast<uint32_t>(d.usable) << 3 |
                         static_cast<uint32_t>(d.mode) << 4 | static_cast<uint32_t>(d.hold) << 6 |
                         static_cast<uint32_t>(d.index) << 9 | static_cast<uint32_t>(d.floorPct) << 16 |
                         static_cast<uint32_t>(d.floorShown) << 24;
  // The mood filter: Smart filters' value and dimness, the status row's count.
  const uint32_t moods = static_cast<uint32_t>(d.moods) | static_cast<uint32_t>(d.moodData) << 16 |
                         static_cast<uint32_t>(d.moodOn) << 17 | static_cast<uint32_t>(d.moodPct) << 24;
  for (uint32_t v : {flags, d.rows, d.mapped, d.tracks, static_cast<uint32_t>(d.bpmPct) << 8 | d.keyPct, moods, d.fit,
                     d.fitOf}) {
    h = (h ^ v) * 16777619u;
  }
  // (The language: everything is drawn again when it changes.)
  return h;
}

void AutoDjPage::enter(NavModel::PageRef& ref) {
  drawnAutoDj_ = autoDjSig(ui_.state().autoDj);
  show(ref);
}

Header AutoDjPage::header() const {
  Header h;
  h.subWhole = true;  // (a phrase: whole beside the title, or not at all)
  h.back = true;
  h.title = tr(S::output_autodj);
  h.sub = tr(S::autodj_subtitle);
  return h;
}

bool AutoDjPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  ListView& list = ui_.list();
  const bool still = !list.animating();
  // A setting, the index's state, a hold: the rows in place (a tap's
  // change shows here, from the next pass's snapshot).
  const uint32_t dj = autoDjSig(ui_.state().autoDj);
  if (dj != drawnAutoDj_ && still) {
    drawnAutoDj_ = dj;
    for (uint32_t i = 0; i < autodjview::kPageRows; ++i) list.refreshRow(i);
  }
  return list.update(nowMs, frameDue, wholeRows);
}

void AutoDjPage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  const autodjview::State& d = s.autoDj;
  char moods[64];
  if (!d.moodData) {
    snprintf(moods, sizeof(moods), "no mood data (Smart filters dim, 0x%03X kept)", static_cast<unsigned>(d.moods));
  } else if (!d.moodOn) {
    snprintf(moods, sizeof(moods), "no mood filter (0x%03X)", static_cast<unsigned>(d.moods));
  } else {
    snprintf(moods, sizeof(moods), "mood filter 0x%03X, %lu of %lu tracks fit", static_cast<unsigned>(d.moods),
             static_cast<unsigned long>(d.fit), static_cast<unsigned long>(d.fitOf));
  }
  snprintf(buf, size, "Output (AutoDj): Auto DJ %s, %s, %u %% (shown %u %%), BPM %s, key %s, %s; the index %s%s",
           tr(autodjview::rowText(autodjview::rowState(d))),
           d.mode == autodj::Mode::Locked ? "Stay on seed" : "Follow the vibe", static_cast<unsigned>(d.floorPct),
           static_cast<unsigned>(d.floorShown), d.matchTempo ? "on" : "off", d.matchKey ? "on" : "off", moods,
           autodjview::indexName(d.index), d.usable ? " (usable)" : " (not usable: the settings dim)");
}

// ---- the list ----

uint32_t AutoDjPage::rows() { return autodjview::kPageRows; }

// The status row is no control: no press, no tick.
bool AutoDjPage::pressable(uint32_t row) { return row != autodjview::StatusRow; }

void AutoDjPage::drawRow(ListView::Row& r) {
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
    case dv::FilterRow: {
      // "Any", or what is set (beside the title when it fits whole, else on
      // the second line in the hint's place, cut to it: frames dj-two,
      // dj-many); dim with no mood data, "No mood data" over the hint, all
      // of it FAINT (frame dj-nodata: the hint isn't why).
      char value[filtergrid::kSummaryBytes];
      dv::filterValue(d, value, sizeof(value));
      drawAutoDjValue(r, tr(S::mood_smart_filters), value, tr(S::autodj_smart_sub), live, live ? col::DIM : col::FAINT);
      break;
    }
    case dv::BpmRow: drawAutoDjSwitch(r, tr(S::autodj_bpm), live ? tr(S::autodj_bpm_sub) : needs, d.matchTempo, live); break;
    case dv::KeyRow:
      drawAutoDjSwitch(r, tr(S::autodj_harmonic), live ? tr(S::autodj_harmonic_sub) : needs, d.matchKey, live);
      break;
    case dv::StatusRow: {
      // Two lines of Small, no control: why it waits over the index, or how
      // many tracks fit the mood filter over the coverage, or the index's
      // two; the first amber when paused, when nothing fits or 50 or fewer
      // (statusAmber(): frames dj-waiting, dj-few).
      Fonts& f = Fonts::instance();
      char a[96], b[96];
      dv::statusLines(d, a, sizeof(a), b, sizeof(b));
      const int x = r.x + kAutoDjX;
      const uint16_t ink = dv::statusAmber(d) ? col::AMBER : col::SOFT;
      if (!b[0]) {
        f.draw(r.c, Font::Small, a, x, 21, kAutoDjLineW, ink, r.bg);
        break;
      }
      f.draw(r.c, Font::Small, a, x, 12, kAutoDjLineW, ink, r.bg);
      f.draw(r.c, Font::Small, b, x, 30, kAutoDjLineW, col::DIM, r.bg);
      break;
    }
    default: break;
  }
}

// A switch row: the title over its line up to the switch (Haptics'), the
// switch at the right; dim without the index it needs.
void AutoDjPage::drawAutoDjSwitch(ListView::Row& r, const char* title, const char* sub, bool on, bool live) {
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
// Auto DJ note says why). The hint in `hintInk`.
void AutoDjPage::drawAutoDjValue(ListView::Row& r, const char* title, const char* value, const char* hint,
                                 bool live, uint16_t hintInk) {
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
  f.draw(r.c, Font::Small, autoDjSecondLine(tw, vw, value, hint), x, 31, kAutoDjLineW, beside ? hintInk : valueInk,
         r.bg);
}

// A tap on a row of Output > Auto DJ: saved and applied at once (app/AutoDj,
// NVS); the rows follow from the next snapshot. A dim row: why, as a note.
// Smart filters: the editor, or its own note (editMoods()), before the
// index's: it needs mood data, not the index (MOOD-FILTERS.md 4.4).
void AutoDjPage::onAutoDjTap(uint32_t row) {
  namespace dv = autodjview;
  if (row >= dv::kPageRows || row == dv::StatusRow) return;
  const AppState& s = ui_.state();
  const dv::State& d = s.autoDj;
  const auto r = static_cast<dv::PageRow>(row);
  if (r == dv::FilterRow) {
    editMoods();
    return;
  }
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

// Smart filters (MOOD-FILTERS.md 4.4): behind the library update's fence
// the row keeps its last value and the count would be old, so wait; no
// mood data on the card, the dim row's note (amber: sync it from mStream;
// a saved filter stays, its picks unfiltered: D4); else the editor, from
// what the snapshot holds (the filter and its count).
bool AutoDjPage::editMoods() {
  const AppState& s = ui_.state();
  const autodjview::State& d = s.autoDj;
  if (s.libraryFenced) {
    ui_.toast(tr(S::library_updating_wait), false);
    return false;
  }
  if (!autodjview::rowLive(autodjview::FilterRow, d)) {
    Serial.println("[ui] output: Smart filters: no mood data on this card");
    ui_.warn(tr(S::toast_autodj_no_mood));
    return false;
  }
  editWord_ = d.moods;
  editFit_ = d.fit;
  ui_.openFilters(this);
  return true;
}

bool AutoDjPage::setFilterWord(uint16_t q) {
  // Applied and saved at once ([dj]'s line says so, with its count); the
  // editor's count is that count. Never refused: nothing is carved.
  editFit_ = ui_.host().autoDjSetMoods(q);
  editWord_ = q;
  return true;
}

uint32_t AutoDjPage::filterTotal() { return ui_.state().autoDj.fitOf; }

void AutoDjPage::onFiltersClosed(bool changed) {
  if (!changed) return;  // (the band pushed again as it was: Ui::repaintUnder())
  ui_.host().autoDjMoodSettled();
  // The page's rows from the snapshot, rendered again (Smart filters' value,
  // the status row's count), once: drawnAutoDj_ says they are, so update()
  // draws them again only if the snapshot moves on (the last tap's count
  // lands in the next one).
  if (!shown()) return;
  ui_.list().refreshAll();
  drawnAutoDj_ = autoDjSig(ui_.state().autoDj);
}

ListView::Tap AutoDjPage::onTapAt(uint32_t row, int x, bool rightEdge) {
  (void)x;
  (void)rightEdge;
  onAutoDjTap(row);
  return ListView::Tap::Handled;
}

}  // namespace ui
