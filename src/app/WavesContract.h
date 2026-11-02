// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The waveform index majors this build reads, as device.txt's read.mpwf says
// them (a bit a major: 1 is major 1): the contract's (devicetxt::current():
// none until the build holds the job that opens waves-<gen>.bin, so a card
// isn't given a file nothing reads: docs/WAVEFORM-INDEX.md 7.4, and the
// read.mpdj=1 of 0.8.0-0.10.0 in docs/METADATA.md 2.15). That job opens the
// file only when this build reads its major, as app/AutoDj does; Library
// writes the same into /.player/device.txt. A header of its own, so Library
// needn't include the job.
uint32_t wavesReadMpwf();
