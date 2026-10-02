// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The ESP32 silicon revision this firmware needs (docs/ENERGY.md section 5,
// P3a). The code PlatformIO compiles is built without the rev-1 PSRAM cache
// workaround (-mfix-esp32-psram-cache-issue: tools/no_psram_fix.py), which
// is safe only on revision 3 or later (ECO3 fixed the bug in silicon), and
// the shipped sdkconfig doesn't refuse an older chip (ESP32_REV_MIN is 0).
// So setup() checks first and halts on an older one. Every Core2 is an
// ESP32-D0WDQ6-V3 (rev 3.x): this is for a board that shouldn't exist.
//
// Revisions as esp_chip_info() gives them: major x 100 + minor (rev 3.1 is
// 301). Portable (host-tested: test_chip_revision).
namespace chiprev {

inline constexpr uint16_t kMinimum = 300;  // 3.0

bool supported(uint16_t revision);
// "3.1", "1.0"
void text(uint16_t revision, char* buf, size_t size);

}  // namespace chiprev
