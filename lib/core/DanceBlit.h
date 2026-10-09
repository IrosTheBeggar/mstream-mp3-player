// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DancePack.h"

// Drawing a dance pack's frames (docs/DANCE-SPRITES.md, 6.2 and D.1): the
// row and grid decoders of A.10, and the blitter that puts a frame into an
// RGB565 canvas. Portable: on the device the canvas is the stage sprite's
// getBuffer(), on the host any buffer, so the tests hash exactly what the
// device draws.
//
// A frame's pixels (A.10) are its box's rows, each `skip` (leading
// transparent px), `n`, then n tokens `(run - 1) << 4 | index`: a run of 1-16
// px of one palette index, index 0 a transparent gap. Past the last token
// the row is transparent.
//
// The blitter:
//   - decodes one box row at a time into a stack array of kBlitRowMax (160)
//     indices, its only buffer (a cell is at most 160 native px wide), and
//     reverses it in place when the frame is drawn mirrored, so the scan
//     always walks forwards;
//   - fills each run of one non-zero index as one rectangle of run * scale by
//     scale px, clamped in 64 bits and then filled through a pointer on
//     int bounds, writing lut[index] as it is: the LUT holds canvas values,
//     lutEntry()'s byte-swapped RGB565 in the firmware (what an M5GFX RGB565
//     sprite holds), the index itself in A.16's hash test. Index 0 is never
//     drawn, so the background shows through;
//   - clips every rectangle to the canvas, whatever the position: open()'s
//     checks (5.4) already keep a frame inside its cell, so this is defence in
//     depth, and it is what makes blit safe on any bytes. A row that breaks
//     A.10's rules ends the frame there.
// The caller places the box (B.4 step 10). Mirroring happens inside the box:
// box column i lands at w - 1 - i. So a frame drawn mirrored at its cell's
// mirrored column, left = (cellW - x0 - w) * scale from the cell's origin,
// puts box column i on cell column cellW - 1 - (x0 + i), A.10's rule.
//
// No M5GFX call, no heap, no IRAM: the same code renders into a test buffer
// on the host.
namespace dance {

struct BlitCanvas {  // getBuffer()'s pixels on the device; any buffer on the host
  uint16_t* px;
  int16_t w, h;
  int32_t stride;    // px from one row to the next, >= w
};
constexpr int kBlitRowMax = 160;  // a box row's indices, on the stack (cellW <= 160)

// B.9's rgb565 with its two bytes swapped: what an M5GFX RGB565 sprite
// holds. lutEntry(0xFF, 0, 0) is 0x00F8, so the canvas's bytes read F8 00.
constexpr uint16_t lutEntry(uint8_t r, uint8_t g, uint8_t b) {
  const unsigned v = (unsigned{r} >> 3) << 11 | (unsigned{g} >> 2) << 5 | unsigned{b} >> 3;  // B.9's rgb565
  return static_cast<uint16_t>((v >> 8) | ((v & 0xFFu) << 8));
}
// lut[i] = lutEntry of RGB888 entry i of `rgb` (a PALS half, 48 B); lut[0] = 0.
void makeLut(const uint8_t* rgb, uint16_t lut[16]);

// A.10, one row starting at p, into row[0, w). Returns where the next row starts.
//
// It reads `skip` and `n` at p, then n tokens, and writes w indices: 0
// wherever the skip, a gap token or the end past the last token covers. It
// never reads at or past `end` and never writes past row[w - 1]. A row whose
// bytes run out, or whose skip or runs pass w, is cut there, clears *ok and
// returns end (a good row leaves *ok as it is; ok may be null). A w outside
// 1..kBlitRowMax writes nothing, clears *ok and returns end.
const uint8_t* decodeRow(const uint8_t* p, const uint8_t* end, int w, uint8_t* row,
                         bool* ok = nullptr);
// The frame's cell grid: cellH rows of cellW indices, row-major, unmirrored.
//
// It zeroes cellW * cellH bytes, then decodes the box's h rows into rows
// y0.. and columns x0.. (A.10's drawing, unmirrored). True when every row
// kept A.10's rules and exactly len bytes were read: what open()'s dry run
// checks. A blank frame (w = 0) gives a zero grid, true when its len is 0. A
// box that doesn't fit (x0 + w past cellW, y0 + h past cellH, or w above
// 160) gives a zero grid and false.
bool decodeGrid(const PackFrame& f, int cellW, int cellH, uint8_t* grid);
// Draws f's box with its top-left at (left, top), in canvas px.
//
// Box pixel (i, y) covers [left + i' * scale, left + (i' + 1) * scale) x
// [top + y * scale, top + (y + 1) * scale), where i' = i, or w - 1 - i when
// mirrored. Only pixels inside [0, c.w) x [0, c.h) are written. Nothing is
// drawn when scale isn't 1-4, the box's w is 0 or above kBlitRowMax, c.px or
// lut is null, or c.stride is below c.w.
void blit(const BlitCanvas& c, const PackFrame& f, int left, int top, int scale, bool mirrored,
          const uint16_t lut[16]);

}  // namespace dance
