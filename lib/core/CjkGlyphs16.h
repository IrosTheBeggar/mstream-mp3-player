// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The built-in CJK and Hangul glyphs: one glyph store (GlyphStore.h) at
// 16 px, 4 bits a pixel, in flash, behind the VLW DejaVu fonts at every UI
// role (docs/I18N.md, phase 2). Made by tools/glyph_store.py from Noto Sans
// CJK Regular's Japanese face, so its glyph data (CjkGlyphs16.cpp) is under
// the SIL Open Font License 1.1, not the GPL: the generated file's header
// has the font's copyright and version, and LICENSES/OFL-1.1.txt the
// licence.
//
// The characters ("pan"): KS X 1001's 2,350 Hangul syllables and the
// compatibility jamo, JIS X 0208 (both kanji levels, kana, its symbols),
// GB2312's 3,755 level 1 hanzi, the kana blocks and CJK punctuation, less
// what the VLW fonts draw: 10,558 glyphs, 1.05 MB. A character outside it
// TextFit folds as before ('?').
//
// Words, so the store is 4-byte aligned; their bytes are the store
// (little-endian, as the ESP32 and the host are):
//   store.open(kCjkGlyphs16, kCjkGlyphs16Size)
extern const uint32_t kCjkGlyphs16[];
extern const size_t kCjkGlyphs16Size;  // bytes
// FNV-1a 64 of every glyph as the generator drew it (code point, metrics,
// 4-bit values; tools/glyph_store.py's digest()): test_glyph_store
// recomputes it through the decoder.
extern const uint64_t kCjkGlyphs16Digest;
