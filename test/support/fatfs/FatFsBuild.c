// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// FatFs R0.15 as one C unit for the host-only model: test_fat_model
// includes this file from its own folder (PlatformIO builds a test's own
// folder, not test/support), tools/fatmodel.py compiles it with gcc. The
// disk functions FatFs calls are test/support/FatModel.h's.
#include "FatFsHost.h"
#include "ff.c"
#include "ffunicode.c"
