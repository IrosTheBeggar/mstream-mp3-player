// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The AutoDJ index majors this build reads, as device.txt's read.mpdj says
// them (a bit a major: 2 is major 2): the contract's (none until M5's
// release switch), and major 2 in a build with -DAUTODJ_READ_MPDJ=1
// (platformio.ini's core2-autodj, never released: how M4's device checks
// give a test card its index). Auto DJ opens an index only when this
// build reads its major (the engine design's 3.1, 3.6: app/AutoDj, which
// defines it); Library writes the same into /.player/device.txt. A header
// of its own, so Library needn't include the whole of Auto DJ.
uint32_t autoDjReadMpdj();
