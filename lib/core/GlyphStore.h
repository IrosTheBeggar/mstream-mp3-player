// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Glyphs for the characters the VLW DejaVu fonts don't have, Chinese,
// Japanese and Korean, from a glyph store (docs/I18N.md, phase 2). A store
// is one pixel size of one font: for each glyph a tight box of 4-bit
// coverage, the box's place from the pen and the baseline, and the advance,
// found by its code point (32 bits: a store can reach past the BMP).
// tools/glyph_store.py writes stores and describes the format; the built-in
// one is kCjkGlyphs16 (CjkGlyphs16.h), 16 px, in flash.
//
// A store is read where it lies, flash or memory: no heap, no copy. open()
// reads the header once. find() is a binary search over the store's pages
// (256 code points each; 135 in the built-in store), then one word of the
// page's presence bitmap and a bit count. It reads only the page tables
// (6.5 KB in the built-in store, so they stay in the flash cache), and
// advance() one word more: a string's width (TextFit measures each label
// several times) costs a couple of hundred cycles a character. decode()
// unpacks one glyph's coverage into the caller's buffer.
namespace glyphstore {

// A glyph's box and advance, in pixels.
struct Metrics {
  uint8_t w = 0;  // the box; 0 x 0 when there is nothing to draw (a space)
  uint8_t h = 0;
  int8_t x = 0;         // the box's left edge right of the pen (a mark's is left of it)
  int8_t y = 0;         // its top edge above the baseline (below it: negative)
  uint8_t advance = 0;  // how far the pen moves
};

// A glyph found in a store: false when the store has none.
struct Glyph {
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  uint32_t index = kNone;  // in the store's order (by code point)
  uint32_t page = 0;       // its page: which of the store's pages
  explicit operator bool() const { return index != kNone; }
};

// Coverage as decode() writes it.
enum class Alpha : uint8_t {
  Nibble,  // 0-15, as stored
  Byte,    // 0-255 (a nibble times 17)
};

class Store {
public:
  // Reads the header of the store at `data` (`size` bytes, staying there
  // while the Store is used). False when it isn't one this reader knows
  // (the magic, the version, sections out of bounds or out of order): the
  // Store is then empty and finds nothing.
  bool open(const void* data, size_t size);
  bool valid() const { return bits_ != nullptr; }

  int pixelSize() const { return pixelSize_; }
  // The em box above and below the baseline (a CJK font's 0.88 and 0.12
  // em: 14 and 2 px at 16 px).
  int ascent() const { return ascent_; }
  int descent() const { return descent_; }
  // How far the glyphs' ink reaches above and below the baseline, at most.
  int inkAscent() const { return inkAscent_; }
  int inkDescent() const { return inkDescent_; }
  // The widest and the tallest box: what a decode buffer must hold.
  int maxWidth() const { return maxW_; }
  int maxHeight() const { return maxH_; }
  uint32_t glyphCount() const { return count_; }

  Glyph find(uint32_t cp) const;
  bool contains(uint32_t cp) const { return static_cast<bool>(find(cp)); }
  // All 0 for no glyph.
  Metrics metrics(const Glyph& g) const;
  // `cp`'s advance in pixels; -1 when the store has no glyph for it.
  int advance(uint32_t cp) const;

  // `g`'s coverage into `out`, row by row: h rows of w values, row r at
  // out + r * stride (stride >= w; `outSize` bytes from out). False, with
  // nothing written, for no glyph or a buffer too small; false with the
  // box all 0 for a bitmap that isn't well formed (a store from a card that
  // went bad). A 0 x 0 glyph writes nothing and is true.
  bool decode(const Glyph& g, uint8_t* out, size_t stride, size_t outSize, Alpha alpha = Alpha::Byte) const;

private:
  const uint8_t* pages_ = nullptr;    // PAGE: u16 a page, ascending
  const uint8_t* pageMap_ = nullptr;  // PMAP: 48 B a page
  const uint8_t* metrics_ = nullptr;  // METR: u32 a glyph
  const uint8_t* offsets_ = nullptr;  // OFFS: u16 a glyph
  const uint8_t* bits_ = nullptr;     // BITS
  uint32_t bitsSize_ = 0;
  uint32_t pageCount_ = 0;
  uint32_t count_ = 0;
  uint8_t pixelSize_ = 0;
  uint8_t encoding_ = 0;
  int8_t ascent_ = 0, descent_ = 0, inkAscent_ = 0, inkDescent_ = 0;
  uint8_t maxW_ = 0, maxH_ = 0;
};

}  // namespace glyphstore
