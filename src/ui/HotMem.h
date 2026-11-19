// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>

// The UI's memory that every list frame writes and reads, kept in the fast
// half of the PSRAM.
//
// The ESP32 maps its 4 MB of PSRAM at 0x3F800000-0x3FBFFFFF, and memory
// from 0x3FA00000 up shares core 1's 32 KB cache with the code that runs
// there (docs/RESAMPLER.md 10d: an MP3 decodes at 1.7-3.6x realtime with
// its state up there, at 4.9x below). The loop task draws on core 1, and a
// list row is a 27 KB sprite: filled and drawn up there it evicts the very
// code and font data that draw it. Allocated when the UI starts, after the
// library's index, that is where the sprites land on a big card: MEASURED
// on a 19,533-track card (the index 1.79 MB), the row sprites at
// 0x3FAEC2B4 and up; the same row rendered in 3.4 ms from a block below the
// line against 4.0 ms (1.8 against 3.1 ms once its lines are cached).
//
// So main.cpp reserves one block first thing in setup(), while the lower
// 2 MB are free (before the card's sector cache, the decoders' pinned
// state and the library), and the list's row sprites, its rail and the
// shared strip take their pixels from it (ui/ListView, ui/Gfx). Nothing is
// given back: they live as long as the firmware runs. A piece asked for
// that it can't give (never reserved, or used up) is the caller's to
// allocate as before, anywhere in the PSRAM.
namespace ui {
namespace hotmem {

// One block of `bytes` (PSRAM), where the heap puts it now. False: none.
bool reserve(size_t bytes);
// `bytes` of it, on a 32-byte (cache line) boundary; nullptr when it has
// none, or not that much left.
void* take(size_t bytes);
// "[ui] ..." for the log: where the block is and what is left of it.
void print();

}  // namespace hotmem
}  // namespace ui
