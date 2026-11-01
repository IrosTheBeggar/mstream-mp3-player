// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// Settings written to NVS later (lib/core SaveLater says when): a tap on a
// settings row changes the value at once and queues its key here, and the
// loop writes what is queued once the taps rest (1.5 s after the last
// change, 6 s after the first at most), each namespace opened once, with
// one log line ("[nvs] saved screen/bright ..."). A key queued again before
// that is written once, with its last value. Before a restart or a
// power-off (PowerSettings::restart(), IdlePower::powerOff()) flushNow()
// writes what is queued at once. The namespace and key are string
// literals (kept by pointer). Loop task only.
//
// Why: a write held the loop a few ms in the middle of the tap that made
// it (a sector erase, when NVS's page fills, much longer), and the other
// core's tasks that run from flash stand still while it lasts.
namespace nvslater {

void putU8(const char* ns, const char* key, uint8_t value);
void putU16(const char* ns, const char* key, uint16_t value);
// Every loop pass: the write, when it is due.
void loop(uint32_t nowMs);
// What is queued, written now (nothing queued: nothing).
void flushNow();
bool pending();

}  // namespace nvslater
