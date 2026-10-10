// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// FatFs's ff.h and diskio.h for the host-only FatFs model (test/support/
// FatModel.h, test_fat_model, tools/fatmodel.py). ff.h's `_WIN32` branch
// is for VC++ and pulls in windows.h; MinGW's gcc is C99 and takes the
// stdint.h branch, as Linux does, so the types are the same on every host
// (and windows.h's macros stay out of the tests).
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#pragma push_macro("_WIN32")
#undef _WIN32
#include "ff.h"
#include "diskio.h"
#pragma pop_macro("_WIN32")
#else
#include "ff.h"
#include "diskio.h"
#endif
