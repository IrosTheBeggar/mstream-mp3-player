#include "ui/BootScreen.h"

#include <M5Unified.h>

namespace {
constexpr uint16_t kBg = 0x0862;      // the UI's background
constexpr uint16_t kFg = 0xF79F;
constexpr uint16_t kDim = 0x8CB5;
constexpr uint16_t kHeader = 0x10C4;  // the tab bar's surface
constexpr uint16_t kAccent = 0xFB49;

constexpr int kW = 320;
constexpr int kHeaderH = 24;
constexpr int kRowH = 17;
constexpr int kValueX = 112;
}  // namespace

void BootScreen::begin() {
  auto& d = M5.Display;  // M5.begin() already set the Core2's landscape rotation
  d.fillScreen(kBg);
  d.fillRect(0, 0, kW, kHeaderH, kHeader);
  d.fillRect(0, kHeaderH - 2, kW, 2, kAccent);
  d.setFont(&fonts::Font2);
  d.setTextColor(kFg, kHeader);
  d.setTextPadding(0);
  d.setTextDatum(textdatum_t::middle_left);
  d.drawString("mStream Player - starting", 8, kHeaderH / 2 - 1);
}

void BootScreen::show(const std::vector<Row>& rows) {
  auto& d = M5.Display;
  d.setFont(&fonts::Font2);
  d.setTextDatum(textdatum_t::middle_left);
  for (size_t i = 0; i < rows.size(); ++i) {
    const int y = kHeaderH + 8 + static_cast<int>(i) * kRowH + kRowH / 2;
    d.setTextPadding(kValueX - 8);
    d.setTextColor(kDim, kBg);
    d.drawString(rows[i].label, 8, y);
    d.setTextPadding(kW - kValueX);
    d.setTextColor(kFg, kBg);
    d.drawString(rows[i].value, kValueX, y);
  }
  d.setTextDatum(textdatum_t::top_left);
}
