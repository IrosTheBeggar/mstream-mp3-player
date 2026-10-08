// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// What a JPEG file is, from its first bytes: its size and whether it is
// progressive. The device's decoder (TJpgDec, in M5GFX) reads baseline
// JPEGs only; a progressive cover (about one in six on the test card) gets
// the placeholder instead, and is logged once. Portable, host-tested.
namespace jpeg {

struct Info {
  bool ok = false;           // a JPEG with a frame header before its first scan
  bool progressive = false;  // SOF2/6/10/14: TJpgDec can't decode it
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t components = 0;    // 1 grey, 3 YCbCr
};

// Walks the markers from SOI to the first frame header (SOFn), skipping
// each segment by its length. `len` may be less than the file, but the
// header can be anywhere: EXIF with a thumbnail, XMP, a Photoshop block and
// an ICC profile can put it past 64 KB: `len` should be the whole file
// (ui/Thumbs walks a cover on the card with parseFile() instead).
Info parse(const uint8_t* data, size_t len);

// The same walk over a file read at offsets, never held whole (ui/Thumbs
// streams a cover from the card: docs/METADATA.md 3.5, "Covers stream"):
// `read` gives all `n` bytes at `offset` (false: it couldn't), the file is
// `size` bytes. A few reads of up to 64 bytes each, a segment's length
// skipped without reading it, wherever the frame header is.
using ReadFn = bool (*)(uint32_t offset, uint8_t* out, uint32_t n, void* ctx);
Info parseFile(ReadFn read, void* ctx, uint32_t size);

}  // namespace jpeg
