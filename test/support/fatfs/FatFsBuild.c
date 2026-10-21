// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// FatFs R0.15 as one C unit for the host-only model: test_fat_model
// includes this file from its own folder (PlatformIO builds a test's own
// folder, not test/support), tools/fatmodel.py compiles it with gcc. The
// disk functions FatFs calls are test/support/FatModel.h's.
#include "FatFsHost.h"
#include "ff.c"
#include "ffunicode.c"

// FF_MULTI_PARTITION 1, as ESP-IDF builds it: its diskio.c's table, logical
// drive n on physical drive n with its partition found (0: the volume at
// LBA 0, else the first FAT partition of an MBR). The format in the player
// sets its drive's to 1 for f_mkfs, which then formats the MBR's first
// partition (lib/core CardMkfs; test_card_mkfs).
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}, {1, 0}};
