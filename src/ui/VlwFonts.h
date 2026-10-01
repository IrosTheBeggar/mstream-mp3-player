// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The UI's fonts as VLW smooth fonts in flash (tools/vlw_font.py, from
// DejaVu Sans and DejaVu Sans Bold; licence: LICENSES/DejaVu-Fonts.txt).
// ui/Fonts loads them. Each covers ASCII, Latin-1, the common Latin
// Extended-A letters and the typographic punctuation (256 glyphs).
extern const uint8_t kVlwSans16[];      // body: row titles, buttons
extern const size_t kVlwSans16Size;
extern const uint8_t kVlwSans13[];      // small: secondary lines, labels
extern const size_t kVlwSans13Size;
extern const uint8_t kVlwSansBold16[];  // headers, the current row, primary buttons
extern const size_t kVlwSansBold16Size;
extern const uint8_t kVlwSansBold22[];  // titles, big numbers
extern const size_t kVlwSansBold22Size;
