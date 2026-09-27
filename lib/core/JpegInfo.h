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
// an ICC profile can put it past 64 KB (ui/Thumbs passes the whole file).
Info parse(const uint8_t* data, size_t len);

}  // namespace jpeg
