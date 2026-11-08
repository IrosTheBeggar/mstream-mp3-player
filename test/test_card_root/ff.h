// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// src/storage/CardFat.h's "ff.h" on the host: this suite builds the firmware's own
// CardFat.cpp over the real FatFs.
#pragma once
#include "../support/fatfs/FatFsHost.h"
#define FF_DIR DIR
