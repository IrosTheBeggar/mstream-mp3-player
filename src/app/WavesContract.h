// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The waveform index majors this build reads, as device.txt's read.mpwf says
// them (a bit a major: 1 is major 1): the contract's (devicetxt::current():
// major 1 since the build that holds the job: docs/WAVEFORM-INDEX.md 7.4). The
// job opens waves-<gen>.bin only when this says it reads the file's major
// (app/Waves, which defines it); Library writes the same into
// /.player/device.txt. A header of its own, so Library needn't include the job.
uint32_t wavesReadMpwf();
