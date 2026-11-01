// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TitleLayout.h"

#include <cstring>

namespace titlelayout {

static_assert(kBoldFirstY + (kMaxLines - 1) * kBoldPitch == kStripH - kBoldFirstY,
              "Bold's three lines sit in the middle of the strip");

Lines wrap(const textfit::Font& big, const textfit::Font& bold, bool fromStore, const char* title, int maxW,
           char out[][kLineSize]) {
  Lines l;
  if (!title || !out) return l;
  const size_t n = std::strlen(title);
  if (!fromStore) {
    l.count = textfit::wrap(big, title, n, maxW, 2, out[0], kLineSize);
    if (l.count < 2 || !cutShort(out[1], title)) return l;
  }
  // Three smaller lines: the title is too long for two big ones, or has
  // glyphs from the store.
  l.bold = true;
  l.count = textfit::wrap(bold, title, n, maxW, kMaxLines, out[0], kLineSize);
  return l;
}

int lineY(const Lines& l, int i) {
  if (!l.bold) return l.count == 1 ? kStripH / 2 : kStripH / 4 + i * kBigPitch;
  return kBoldFirstY + (kMaxLines - l.count) * kBoldPitch / 2 + i * kBoldPitch;
}

bool cutShort(const char* line, const char* text) {
  const size_t n = std::strlen(line), t = std::strlen(text);
  const bool ends = n >= 3 && (std::strcmp(line + n - 3, "\xE2\x80\xA6") == 0 || std::strcmp(line + n - 3, "...") == 0);
  return ends && !(t >= 3 && std::strcmp(text + t - 3, line + n - 3) == 0);
}

}  // namespace titlelayout
