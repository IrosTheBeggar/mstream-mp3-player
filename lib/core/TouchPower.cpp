// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TouchPower.h"

#include <cstdio>

namespace touchpower {

Regs fromBytes(const uint8_t ctrlBlock[4], uint8_t mode) {
  Regs r;
  r.ctrl = ctrlBlock[0];
  r.monitorAfterS = ctrlBlock[1];
  r.periodActive = ctrlBlock[2];
  r.periodMonitor = ctrlBlock[3];
  r.mode = mode;
  return r;
}

const char* modeName(int mode) {
  switch (mode) {
    case kActive: return "Active";
    case kMonitor: return "Monitor";
    case kHibernate: return "Hibernate";
    default: return "?";
  }
}

bool autoMonitors(const Regs& r) { return r.ctrl == 1 && r.monitorAfterS > 0; }

void describe(const Regs& r, char* buf, size_t size) {
  if (!size) return;
  char mode[12];
  if (r.mode == kActive || r.mode == kMonitor || r.mode == kHibernate) {
    snprintf(mode, sizeof(mode), "%s", modeName(r.mode));
  } else {
    snprintf(mode, sizeof(mode), "0x%02x", (unsigned)r.mode);
  }
  char means[64];
  if (autoMonitors(r)) {
    snprintf(means, sizeof(means), "to Monitor by itself after %u s untouched", (unsigned)r.monitorAfterS);
  } else {
    snprintf(means, sizeof(means), "no auto-switch: Active unless set");
  }
  snprintf(buf, size, "ctrl=%u monitor_after=%us period_active=%u period_monitor=%u mode=%s (%s)", (unsigned)r.ctrl,
           (unsigned)r.monitorAfterS, (unsigned)r.periodActive, (unsigned)r.periodMonitor, mode, means);
}

Pf parsePf(const char* arg) {
  if (!arg || !arg[0]) return Pf::Report;
  if (arg[1] == 0) {
    if (arg[0] == '0') return Pf::Active;
    if (arg[0] == '1') return Pf::Monitor;
    return Pf::Refused;
  }
  return Pf::Refused;
}

}  // namespace touchpower
