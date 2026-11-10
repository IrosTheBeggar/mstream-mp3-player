// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "CardContainer.h"
#include "CardManifest.h"
#include "CardWaves.h"

// A track's shape as Now Playing draws it (docs/WAVEFORM-INDEX.md 11): a
// slot's 240 bars brought down to 99 columns of height 0-9, whether the slot's
// length agrees with the player's, and the card worker's read of one slot.
// WaveBar.h says where the columns go, WaveState.h when a read is asked for.
//
// The reduction is the terminal player's curve (mstream-terminal, src/tui/ui.rs,
// resample_bars). Peak bars at face value are a brick: a loud master's body
// fills the band (measured there against demo.mstream.io, and here on 49 real
// tracks). So each column is the RMS of its bars, and the columns are stretched
// from the 10th percentile's to the loudest: all the rows go to the range that
// varies. A percentile, not the minimum: nearly every track fades in and out,
// and one silent column would put the floor back at 0. A track with nothing to
// stretch (a test tone) shows at its own level: every column at 9.
//
// All in f32: the ESP32's FPU has it, and f64 is software. It loses nothing
// that shows: a column's sum of squares (at most 3 x 255^2) and its width are
// exact, and two energies that differ do so by at least 0.0003.
//
// Portable, host-tested (test_wave_shape). The read runs on the card worker's
// 7.5 KB of stack: no heap, and the Table, the Slot and the result are its
// caller's (650 B, in the job's PSRAM).
namespace wave {

constexpr int kColumns = 99;
constexpr int kMaxHeight = 9;  // the rows a bar has on each side of its centre row

struct Shape {
  uint32_t lengthMs = 0;           // the slot's; 0 unknown
  uint8_t heights[kColumns] = {};  // 0-9
};

// `heights` (kColumns of them) from a slot's `bars` (mpwf::kBarCount of them).
// Column i takes bars i x 240 / 99 up to (i + 1) x 240 / 99, 2 or 3 of them
// (5's rule with M = 240 for 99 columns, whose min and max don't bind here).
// Its energy e is their RMS. quiet is the 10th of the energies from the
// smallest, loudest the largest. Then h = floor(clamp((e - floor) / span, 0, 1)
// x 9 + 0.5), where floor and span are quiet and loudest - quiet; 0 and loudest
// when those two are equal. Loudest 0: all 0. A tie rounds up.
void reduce(const uint8_t* bars, uint8_t* heights);

// The slot is this track's as the player knows it (11): its length unknown (0),
// or within max(2 s, 2 %) of the player's. Anything else is the plain line: a
// file replaced under its path shows no other recording's shape.
constexpr bool lengthAgrees(uint32_t slotMs, uint32_t playerMs) {
  const uint32_t diff = slotMs > playerMs ? slotMs - playerMs : playerMs - slotMs;
  const uint32_t room = playerMs / 50 > 2000 ? playerMs / 50 : 2000;
  return slotMs == 0 || diff <= room;
}

// What a read comes to (WAVEFORM-INDEX.md 8.3). Hit: the track's shape. Miss:
// no slot, a track with no data. Damaged: a slot's CRC failed, the card's
// fault: this lookup misses, the file stays. Absent: the file isn't to be read
// again until a remount: it isn't on the card, it failed its frame's or its
// header's checks, it isn't the file the root names, or a good CRC came over a
// hash that breaks the sort rule (a broken writer's). Io: a read failed, the
// file's open too, and the file isn't to blame.
enum class Read : uint8_t { Hit, Miss, Damaged, Absent, Io };
const char* readName(Read r);

struct ReadResult {
  Read read = Read::Miss;
  // Absent: the check that failed (Why::Comp: not the file the root names;
  // Why::WvSlotOrder: the sort rule; Why::Io: it isn't on the card). Damaged:
  // WvSlotCrc. Io: Io. Else Ok.
  cardcontract::Why why = cardcontract::Why::Ok;
  uint32_t slotReads = 0;  // the slots the lookup read, a failed one too (0: it never got that far)
  uint32_t jobMs = 0;      // the job's time by the worker's clock: its caller's to set, readShape() has no clock
  Shape shape;             // Hit
};

// One read, as the card worker makes it, over the file the root's MPWF row
// `entry` names: Table::open, msmf::companionMatches, the lookup of `pathHash`,
// reduce. Every field of `out` but jobMs is set. A file that doesn't open has
// no Source: that is openFailed().
void readShape(cardcontract::Source& src, const cardcontract::msmf::Comp& entry, uint64_t pathHash,
               cardcontract::mpwf::Table* table, cardcontract::mpwf::Slot* slot, ReadResult* out);
// The read of a file that didn't open, as readShape() sets `out`. `missing`: the
// card says it isn't there, so the file the root names is gone: Absent. Anything
// else is a read that failed on the way to it (a sector the card wouldn't give,
// the volume's lock timing out): Io, the same fault a slot's read gets, and no
// remount need follow it.
void openFailed(bool missing, ReadResult* out);

}  // namespace wave
