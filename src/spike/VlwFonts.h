#pragma once
#include <cstddef>
#include <cstdint>

// Compile-time switches for the font probe's options (docs/UI-SPIKE.md), so
// each one's flash cost can be measured by building with it off:
//   UI_SPIKE_EFONT  M5GFX's efontCN_16 and efontCN_12 (Unicode bitmap fonts)
//   UI_SPIKE_VLW    the VLW smooth fonts below (tools/vlw_font.py)
#ifndef UI_SPIKE_EFONT
#define UI_SPIKE_EFONT 1
#endif
#ifndef UI_SPIKE_VLW
#define UI_SPIKE_VLW 1
#endif

#if UI_SPIKE_VLW
// DejaVu Sans at 16 px (row titles) and 13 px (secondary lines).
extern const uint8_t kVlwSans16[];
extern const size_t kVlwSans16Size;
extern const uint8_t kVlwSans13[];
extern const size_t kVlwSans13Size;
#endif
