// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// How much of the loop task's stack a console command used. Arduino's
// loopTask (8 KB: CONFIG_ARDUINO_LOOP_STACK_SIZE) runs setup(), loop() and
// every console command, and an overflow is a panic ("Stack canary
// watchpoint triggered (loopTask)": the IDF watches the stack's last 32 B).
// FreeRTOS's low-water mark is the lowest since the task started, and the
// boot already goes deep (the library's compaction: about 6 KB on N11's
// 20k card), so a command's own depth shows only if the stack below the
// console is painted again before it: arm() does, leftSinceArm() reads it
// (SerialConsole: a [console] line after each command; docs/ARCHITECTURE.md
// "The loop task's stack").
//
// All of it on the loop task (arm() paints the stack of whoever calls it).
namespace loopstack {

constexpr uint32_t kSize = 8192;
// Less left than this during a command: the console line says LOW. An
// interrupt saves the task's registers on the task's stack (a few hundred
// bytes) before it moves to its own, and a deeper card error path (the SD
// driver's log line: printf's 800 B frame) can come on top of a command's
// measured peak.
constexpr uint32_t kMinLeft = 1536;

// Paints the stack below the caller (from 256 B under its stack pointer to
// 64 B short of the stack's end: the canary and the watched bytes are left
// alone) with FreeRTOS's fill byte, after taking the low-water mark so far.
// About 6 KB written: a few microseconds.
void arm();
// Bytes never used since the last arm() (since the boot before the first).
uint32_t leftSinceArm();
// The lowest since the boot, across arms.
uint32_t lowestLeft();

}  // namespace loopstack
