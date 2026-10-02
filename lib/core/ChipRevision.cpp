// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ChipRevision.h"

#include <cstdio>

namespace chiprev {

bool supported(uint16_t revision) { return revision >= kMinimum; }

void text(uint16_t revision, char* buf, size_t size) {
  if (!size) return;
  snprintf(buf, size, "%u.%u", (unsigned)(revision / 100), (unsigned)(revision % 100));
}

}  // namespace chiprev
