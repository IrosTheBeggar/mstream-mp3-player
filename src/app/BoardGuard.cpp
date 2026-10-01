// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/BoardGuard.h"

#include <M5Unified.h>

#include "UiText.h"
#include "app/ScreenControl.h"

namespace board {
namespace {

// The boards M5Unified recognises on the original ESP32 (the only chip
// this image runs on), for the message.
struct Named {
  m5::board_t board;
  const char* name;
};
constexpr Named kNames[] = {
    {m5::board_t::board_unknown, "an unknown board"},
    {m5::board_t::board_M5Stack, "M5Stack Basic/Gray/Fire"},
    {m5::board_t::board_M5StackCore2, "M5Stack Core2"},
    {m5::board_t::board_M5StickC, "M5StickC"},
    {m5::board_t::board_M5StickCPlus, "M5StickC Plus"},
    {m5::board_t::board_M5StickCPlus2, "M5StickC Plus2"},
    {m5::board_t::board_M5StackCoreInk, "M5Stack CoreInk"},
    {m5::board_t::board_M5Paper, "M5Paper"},
    {m5::board_t::board_M5Tough, "M5Tough"},
    {m5::board_t::board_M5Station, "M5Station"},
    {m5::board_t::board_M5AtomLite, "ATOM Lite"},
    {m5::board_t::board_M5AtomPsram, "ATOM PSRAM"},
    {m5::board_t::board_M5AtomU, "ATOM U"},
    {m5::board_t::board_M5Camera, "M5Camera"},
    {m5::board_t::board_M5TimerCam, "TimerCam"},
    {m5::board_t::board_M5StampPico, "STAMP Pico"},
    {m5::board_t::board_M5AtomMatrix, "ATOM Matrix"},
    {m5::board_t::board_M5AtomVoice, "ATOM Echo"},
};
constexpr const char* kOther = "another board";

constexpr size_t length(const char* s) { return *s ? 1 + length(s + 1) : 0; }
constexpr bool namesFit(size_t i = 0) {
  return i == sizeof(kNames) / sizeof(kNames[0]) ||
         (length(kNames[i].name) <= static_cast<size_t>(uitext::kBoardNameMaxChars) && namesFit(i + 1));
}
static_assert(namesFit() && length(kOther) <= static_cast<size_t>(uitext::kBoardNameMaxChars),
              "a board's name is longer than the guard's screen measured (UiText kBoardNameMaxChars)");

const char* boardName(m5::board_t b) {
  for (const Named& n : kNames) {
    if (n.board == b) return n.name;
  }
  return kOther;
}

const char* pmicName(m5::Power_Class::pmic_t p) {
  switch (p) {
    case m5::Power_Class::pmic_axp192: return "AXP192";
    case m5::Power_Class::pmic_axp2101: return "AXP2101";
    case m5::Power_Class::pmic_unknown: return "none";
    default: return "another";
  }
}

// What a misread Core2 looks like (M5GFX 0.2.30's autodetect): a Tough
// when something on the internal I2C (21/22, the M-Bus) answers at 0x2E,
// the Tough's touch address; a TimerCam (M5Unified's guess for an unknown
// board on the Core2's ESP32-D0WDQ6) when the panel didn't answer at boot.
const char* misreadHint(m5::board_t b) {
  switch (b) {
    case m5::board_t::board_M5Tough:
      return "; a Core2 reads as a Tough when a module on its M-Bus answers at I2C 0x2E: take it off";
    case m5::board_t::board_M5TimerCam:
    case m5::board_t::board_unknown:
      return "; a Core2 whose display didn't answer at boot reads as this: power it off and on again";
    default: return "";
  }
}

constexpr uint32_t kOffAfterMs = 60000;

}  // namespace

void requireCore2() {
  const m5::board_t b = M5.getBoard();
  if (b == m5::board_t::board_M5StackCore2) return;
  const char* name = boardName(b);
  auto& d = M5.Display;
  if (d.width() > 0) {  // the display M5Unified gave this board, if any
    d.setBrightness(128);
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    // 320 px and wider: Font2, the lines as measured (UiText). Narrower (a
    // StickC): the 6x8 font, wrapped where a line runs out.
    const bool wide = d.width() >= 320;
    if (wide) {
      d.setFont(&fonts::Font2);
    } else {
      d.setFont(&fonts::Font0);
    }
    d.setTextWrap(!wide, !wide);
    d.setCursor(0, 4);
    auto line = [&](const char* a, const char* b2 = "") {
      d.setCursor(4, d.getCursorY());
      d.print(a);
      d.println(b2);
    };
    for (const char* t : uitext::kBoardGuardTop) line(t);
    line(uitext::kBoardGuardFound, name);
    d.println();
    for (const char* t : uitext::kBoardGuardStop) line(t);
    if (b == m5::board_t::board_M5Tough) {
      d.println();
      line(uitext::kBoardGuardToughHint);
    }
  }
  // A power chip M5Unified drives (a Core2's or a Tough's): off after a
  // minute on battery, so a stopped unit doesn't drain it; on USB it stays,
  // showing why (never off and on again: no reboot loop).
  const m5::Power_Class::pmic_t pmic = M5.Power.getType();
  const bool canOff = pmic == m5::Power_Class::pmic_axp192 || pmic == m5::Power_Class::pmic_axp2101;
  const uint32_t start = millis();
  for (;;) {  // asleep but for the line, for a monitor opened late
    Serial.printf("[board] This firmware is for the M5Stack Core2 (found: %s; board id %d, power chip %s): stopped "
                  "before the SD card, the audio and Bluetooth%s\n",
                  name, (int)b, pmicName(pmic), misreadHint(b));
    if (canOff && millis() - start >= kOffAfterMs && !ScreenControl::readExternalPower()) {
      Serial.println("[board] on battery: powering off");
      Serial.flush();
      M5.Power.powerOff();
    }
    delay(10000);
  }
}

}  // namespace board
