// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/LoopStack.h"

#include <esp_cpu.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

namespace loopstack {

namespace {

constexpr uint8_t kFill = 0xA5;  // tasks.c's tskSTACK_FILL_BYTE: what uxTaskGetStackHighWaterMark() counts
// Never painted at the stack's end (its lowest address): the canary
// FreeRTOS checks at each switch (CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY)
// and the 32 B the IDF's watchpoint guards (CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK:
// a write there panics). Untouched since the boot if the task never got
// there, which it would not have survived.
constexpr uint32_t kEndGuard = 64;
// Never painted under arm()'s own stack pointer: the windowed ABI spills a
// caller's a0-a3 into the 16 B below its callee's stack pointer (arm()'s
// caller's, memset()'s caller's), and memset()'s frame (16 B) is there.
constexpr uint32_t kBelowSp = 256;

uint32_t s_lowest = UINT32_MAX;  // the lowest low-water mark before an arm()

}  // namespace

void arm() {
  const uint32_t left = uxTaskGetStackHighWaterMark(nullptr);
  if (left < s_lowest) s_lowest = left;
  uint8_t* const from = pxTaskGetStackStart(xTaskGetCurrentTaskHandle()) + kEndGuard;
  uint8_t* const to = static_cast<uint8_t*>(esp_cpu_get_sp()) - kBelowSp;
  // Dead stack: nothing below the stack pointer outlives the call that put
  // it there. (An interrupt now saves its frame there and takes it back
  // before this goes on.)
  if (to > from) memset(from, kFill, static_cast<size_t>(to - from));
}

uint32_t leftSinceArm() { return uxTaskGetStackHighWaterMark(nullptr); }

uint32_t lowestLeft() {
  const uint32_t now = leftSinceArm();
  return now < s_lowest ? now : s_lowest;
}

}  // namespace loopstack
