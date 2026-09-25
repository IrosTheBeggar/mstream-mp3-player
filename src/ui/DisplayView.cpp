#include "ui/DisplayView.h"

#include <M5Unified.h>

namespace {
constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kFg = TFT_WHITE;
constexpr uint16_t kDim = 0x7BEF;     // grey
constexpr uint16_t kHeader = 0x001F;  // blue

constexpr int kW = 320;
constexpr int kHeaderH = 24;
constexpr int kRowH = 17;
constexpr int kValueX = 112;
}  // namespace

void DisplayView::begin() {
  auto& d = M5.Display;  // M5.begin() already set the Core2's landscape rotation
  d.setFont(&fonts::Font2);
  d.fillScreen(kBg);
  d.setTextDatum(textdatum_t::middle_left);
}

void DisplayView::drawHeader(const char* title) {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, kHeaderH, kHeader);
  d.setTextColor(kFg, kHeader);
  d.setTextPadding(0);
  d.drawString(title, 8, kHeaderH / 2);
}

void DisplayView::showDiagnostics(const std::vector<Row>& rows) {
  auto& d = M5.Display;
  if (!diagnosticsShown_) {
    d.fillScreen(kBg);
    drawHeader("mStream Player - bring-up");
    diagnosticsShown_ = true;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    const int y = kHeaderH + 8 + static_cast<int>(i) * kRowH + kRowH / 2;
    d.setTextPadding(kValueX - 8);
    d.setTextColor(kDim, kBg);
    d.drawString(rows[i].label, 8, y);
    d.setTextPadding(kW - kValueX);
    d.setTextColor(kFg, kBg);
    d.drawString(rows[i].value, kValueX, y);
  }
}
