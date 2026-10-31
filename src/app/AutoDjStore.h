// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

#include "AutoDjEngine.h"

// Auto DJ's switch and settings in NVS (the "autodj" namespace, schema 4:
// nvslayout's keys and their reading, host-tested in test_nvs_layout; the
// engine design's 2.13): read once at boot, queued for NVS when one changes
// (the console's D, from M5 the "..." row and Output > Auto DJ) and written
// once the taps rest (app/NvsLater, its "[nvs] saved" line).
// Each key is read with isKey() first, as QueueStore::loadRepeat() does
// (a missing key would log an [E] line at every boot), and the namespace
// is opened read-write (a read-only open of one not made yet would log
// one too). Absent: off, Follow the vibe, 55 %, BPM continuity and
// Harmonic mixing off (the user's D2). The session is never saved. Loop
// task only.
namespace autodjstore {

// The saved settings (logged: "[dj] settings: ...").
autodj::Settings load();
// Every key queued (app/NvsLater logs the write, and a key NVS won't take).
void save(const autodj::Settings& s);

}  // namespace autodjstore
