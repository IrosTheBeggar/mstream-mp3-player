// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Album covers into square thumbnails: the list's 40 x 40 and Now
// Playing's 96 x 96 from one decode. Portable, host-tested; on the device
// the JPEG decoder (TJpgDec) hands its output here block by block.
//
//   - The decoder can shrink by 1/2, 1/4 or 1/8 as it decodes (for free:
//     it skips the IDCT's detail); decoderScale() picks the most it may
//     while the picture's shorter side stays at least the largest size
//     asked for, so the rest is a true downscale.
//   - The picture is centre-cropped to a square (covers are nearly square;
//     a scan with a margin loses a little of it, never gets bars).
//   - Box filter: each output pixel is the mean of the source pixels that
//     fall in it (a picture smaller than the output is enlarged, each
//     source pixel covering several).
//   - Output: RGB565, big-endian (the LCD's byte order: M5GFX pushes it
//     as swap565 without converting).
//
// The accumulators (4 words a pixel: 147 KB for 96 x 96) come from the
// allocator hooks (PSRAM on the device), held between begin() and the
// destructor or the next begin().
class ThumbScaler {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  static constexpr int kMaxSizes = 2;

  explicit ThumbScaler(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~ThumbScaler();
  ThumbScaler(const ThumbScaler&) = delete;
  ThumbScaler& operator=(const ThumbScaler&) = delete;

  // TJpgDec's scale (0: 1/1 .. 3: 1/8) for a w x h picture whose output
  // must be at least `target` px on its shorter side.
  static int decoderScale(int w, int h, int target);

  // A picture of srcW x srcH (as the decoder outputs it) into `n` square
  // sizes. False: no memory, or a bad size.
  bool begin(int srcW, int srcH, const int* sizes, int n);
  // A block of the picture at (x, y), w x h pixels of RGB888 (3 bytes a
  // pixel, rows packed), in any order.
  void add(int x, int y, int w, int h, const uint8_t* rgb);
  // Size k's thumbnail (size * size pixels, big-endian RGB565) into `out`.
  void finish(int k, uint16_t* out) const;
  int size(int k) const { return k >= 0 && k < n_ ? planes_[k].size : 0; }
  // Gives the accumulators back (begin() takes them again).
  void end() { drop(); }
  size_t bytes() const { return held_; }

  static uint16_t rgb565be(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
    return static_cast<uint16_t>(v >> 8 | v << 8);
  }

private:
  struct Plane {
    int size = 0;
    uint32_t* acc = nullptr;  // r, g, b, count per pixel
  };
  void drop();

  AllocFn allocFn_;
  FreeFn freeFn_;
  Plane planes_[kMaxSizes];
  int n_ = 0;
  int cropX_ = 0, cropY_ = 0, side_ = 0;
  size_t held_ = 0;
};
