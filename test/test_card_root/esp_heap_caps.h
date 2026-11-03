// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// src/app/Psram.h's on the host (CardFat.cpp: see ff.h here).
#pragma once
#include <cstddef>
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
