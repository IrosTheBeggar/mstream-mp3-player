// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/AutoDjStore.h"

#include <Arduino.h>
#include <Preferences.h>

#include "NvsLayout.h"

namespace autodjstore {

namespace {

using nvslayout::AutoDjSaved;
using nvslayout::AutoDjStored;

autodj::Settings settingsOf(const AutoDjSaved& a) {
  autodj::Settings s;
  s.on = a.on;
  s.mode = a.mode == 1 ? autodj::Mode::Locked : autodj::Mode::Rolling;
  s.floorPct = a.floorPct;
  s.matchTempo = a.matchTempo;
  s.matchKey = a.matchKey;
  return s;
}

AutoDjSaved savedOf(const autodj::Settings& s) {
  AutoDjSaved a;
  a.on = s.on;
  a.mode = s.mode == autodj::Mode::Locked ? 1 : 0;
  a.floorPct = s.floorPct;
  a.matchTempo = s.matchTempo;
  a.matchKey = s.matchKey;
  return a;
}

}  // namespace

autodj::Settings load() {
  AutoDjStored st;
  Preferences p;
  // (Read-write, though it only reads: a read-only open of a namespace not
  // made yet fails with an [E] line at every boot until Auto DJ is first
  // saved; read-write makes the namespace, no key. Not opened: none saved.)
  const bool opened = p.begin(nvslayout::kAutoDjNamespace, false);
  if (opened) {
    st.haveOn = p.isKey(nvslayout::kAutoDjOnKey);
    st.haveMode = p.isKey(nvslayout::kAutoDjModeKey);
    st.haveFloor = p.isKey(nvslayout::kAutoDjFloorKey);
    st.haveMatch = p.isKey(nvslayout::kAutoDjMatchKey);
    if (st.haveOn) st.on = p.getUChar(nvslayout::kAutoDjOnKey, 0);
    if (st.haveMode) st.mode = p.getUChar(nvslayout::kAutoDjModeKey, 0);
    if (st.haveFloor) st.floor = p.getUChar(nvslayout::kAutoDjFloorKey, 0);
    if (st.haveMatch) st.match = p.getUChar(nvslayout::kAutoDjMatchKey, 0);
    p.end();
  }
  const autodj::Settings s = settingsOf(nvslayout::autoDjFrom(st));
  const bool any = st.haveOn || st.haveMode || st.haveFloor || st.haveMatch;
  Serial.printf("[dj] settings: Auto DJ %s, %s, strictness %u %%, BPM continuity %s, Harmonic mixing %s (%s)\n",
                s.on ? "on" : "off", s.mode == autodj::Mode::Locked ? "Stay on seed" : "Follow the vibe",
                (unsigned)s.floorPct, s.matchTempo ? "on" : "off", s.matchKey ? "on" : "off",
                any ? "saved" : "none saved: the defaults");
  return s;
}

bool save(const autodj::Settings& s) {
  const AutoDjStored st = nvslayout::autoDjTo(savedOf(s));
  Preferences p;
  if (!p.begin(nvslayout::kAutoDjNamespace, false)) {
    Serial.println("[dj] settings: NVS can't be opened: not saved (for this boot only)");
    return false;
  }
  bool ok = p.putUChar(nvslayout::kAutoDjOnKey, st.on) == 1;
  ok = p.putUChar(nvslayout::kAutoDjModeKey, st.mode) == 1 && ok;
  ok = p.putUChar(nvslayout::kAutoDjFloorKey, st.floor) == 1 && ok;
  ok = p.putUChar(nvslayout::kAutoDjMatchKey, st.match) == 1 && ok;
  p.end();
  if (!ok) Serial.println("[dj] settings: NOT saved (NVS): for this boot only");
  return ok;
}

}  // namespace autodjstore
