#include "app/Screenshot.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>

#include "Base64Text.h"
#include "ui/ListScroller.h"

namespace {
constexpr int kRowsPerRead = 8;  // rows per readRect(): one SPI transaction each

int channelDiff(uint16_t a, uint16_t b, int shift, int bits) {
  const int mask = (1 << bits) - 1;
  return abs(static_cast<int>((a >> shift) & mask) - static_cast<int>((b >> shift) & mask));
}

// Two big-endian RGB565 pixels within one step per channel.
bool close565(uint16_t a, uint16_t b) {
  a = static_cast<uint16_t>((a >> 8) | (a << 8));
  b = static_cast<uint16_t>((b >> 8) | (b << 8));
  return channelDiff(a, b, 11, 5) <= 1 && channelDiff(a, b, 5, 6) <= 1 && channelDiff(a, b, 0, 5) <= 1;
}
}  // namespace

void Screenshot::request(int x, int y, int w, int h, M5Canvas* check, int checkX, int checkY) {
  if (busy()) {
    Serial.println("[shot] busy with the last one");
    return;
  }
  const size_t bytes = static_cast<size_t>(w) * h * sizeof(uint16_t);
  pixels_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  line_ = static_cast<char*>(heap_caps_malloc(base64Length(w * sizeof(uint16_t)) + 1, MALLOC_CAP_SPIRAM));
  if (!pixels_ || !line_) {
    Serial.println("[shot] no memory for it");
    finish();
    return;
  }
  auto& d = M5.Display;
  const bool readable = d.isReadable();
  // With a list scrolling in hardware (ui/ListScroller), the panel shows
  // its GRAM rotated within the list's band and readRect() reads GRAM: read
  // each screen row from the GRAM row it shows, in runs of consecutive ones.
  const bool scrolled = ListScroller::anyActive();
  const uint16_t vsp = ListScroller::activeVsp();
  if (readable) {
    for (int r = 0; r < h;) {
      const int g = ListScroller::gramLineForScreen(y + r);
      int n = 1;
      while (n < kRowsPerRead && r + n < h && ListScroller::gramLineForScreen(y + r + n) == g + n) ++n;
      d.readRect(x, g, w, n, pixels_ + static_cast<size_t>(r) * w);
      r += n;
    }
  }

  // Check the readback against the sprite where they overlap.
  const char* note = "";
  bool useSprite = !readable;
  int checked = 0, differ = 0;
  if (readable && check && check->getBuffer()) {
    uint16_t spriteRow[320];
    const int cw = check->width(), ch = check->height();
    for (int r = 0; r < ch; ++r) {
      const int sy = checkY + r;
      if (sy < y || sy >= y + h) continue;
      check->readRect(0, r, cw, 1, spriteRow);
      for (int c = 0; c < cw && c < 320; ++c) {
        const int sx = checkX + c;
        if (sx < x || sx >= x + w) continue;
        ++checked;
        if (!close565(pixels_[static_cast<size_t>(sy - y) * w + (sx - x)], spriteRow[c])) ++differ;
      }
    }
    if (checked > 0 && differ * 50 > checked) useSprite = true;  // over 2 % off
  }
  if (useSprite && check && check->getBuffer()) {
    // Dump the sprite instead, at its own place and size.
    heap_caps_free(pixels_);
    x = checkX;
    y = checkY;
    w = check->width();
    h = check->height();
    pixels_ = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(w) * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    heap_caps_free(line_);
    line_ = static_cast<char*>(heap_caps_malloc(base64Length(w * sizeof(uint16_t)) + 1, MALLOC_CAP_SPIRAM));
    if (!pixels_ || !line_) {
      Serial.println("[shot] no memory for it");
      finish();
      return;
    }
    check->readRect(0, 0, w, h, pixels_);
    note = readable ? ", the sprite (LCD readback didn't match it)" : ", the sprite (the LCD can't be read)";
  } else if (useSprite) {
    Serial.println("[shot] the LCD can't be read and there's no sprite to dump (dance screen off)");
    finish();
    return;
  }
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  row_ = -1;
  Serial.printf("[shot] format rgb565 big-endian, one base64 line per row%s", note);
  if (scrolled && readable && !useSprite) {
    Serial.printf(" (hardware scroll active: rows read back through the scroll offset, start address %u, so this is "
                  "what the panel shows)",
                  (unsigned)vsp);
  }
  if (checked > 0 && !useSprite) Serial.printf(" (LCD readback checked: %d of %d px differ)", differ, checked);
  if (!check) Serial.print(" (LCD readback not checked: dance screen off)");
  Serial.println();
}

void Screenshot::poll() {
  if (!busy()) return;
  if (row_ < 0) {
    Serial.printf("[shot] begin %d %d %d %d\n", x_, y_, w_, h_);
    row_ = 0;
    return;
  }
  if (row_ < h_) {
    base64Encode(reinterpret_cast<const uint8_t*>(pixels_ + static_cast<size_t>(row_) * w_), w_ * sizeof(uint16_t),
                 line_);
    Serial.println(line_);
    ++row_;
    return;
  }
  Serial.println("[shot] end");
  finish();
}

void Screenshot::finish() {
  heap_caps_free(pixels_);
  heap_caps_free(line_);
  pixels_ = nullptr;
  line_ = nullptr;
}
