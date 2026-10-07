// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The jump grid behind the A-Z rail (the tab bar spec §6.3, with the
// review's second level for large libraries). Portable, host-tested.
//
// A list sorted with textfold::compare() (a folder of folders; the artists
// and the albums by their textfold::sortName(), which is then the row's
// name here) has its rows in rail-key order: '#', then A..Z. The grid's
// first level is those 27 keys: where each one's first row is, or none (the
// cell is drawn faint and does nothing). Tapping a letter with many rows
// opens the second level: the letter itself (its first row) and its 26
// two-letter starts ("Ka", "Ke", "Ki"...), the second letter folded like
// the first (textfold::secondKey()), which are in order within a letter too.
// So in 5,000 artists any one is at most three taps and a short scroll away.
//
// Rows are found by binary search on the keys (a name per probe): no index,
// no memory.
namespace jump {

constexpr int kCells = 27;  // '#', A..Z; or the letter, then a..z
constexpr int kGridCells = 28;  // 7 x 4: the keys, then one of the grid's own
// A letter with more rows than this gets the second level (four screens).
constexpr uint32_t kSecondLevelRows = 16;

// Row `row`'s name (UTF-8).
using NameFn = const char* (*)(void* ctx, uint32_t row);

// first[b]: the first row with rail key b ('#' 0, 'A'..'Z' 1..26), -1 if
// none. `end[b]` (optional): one past its last row.
void letters(uint32_t count, NameFn name, void* ctx, int32_t first[kCells], int32_t end[kCells] = nullptr);
// Inside one letter's rows [begin, end): first[0] = begin (the letter
// alone), first[k] (k 1..26) the first row whose second letter is
// 'a' + k - 1, -1 if none.
void seconds(uint32_t begin, uint32_t end, NameFn name, void* ctx, int32_t first[kCells]);

// The grid's cell under (x, y) for a 7 x 4 grid of cells w x h starting at
// (x0, y0); -1 outside. Cells run left to right, top to bottom: 0..26 the
// keys, 27 the last (the grid's own: back to the first level).
int cellAt(int x, int y, int x0, int y0, int w, int h);

}  // namespace jump
