#include "ui/DisplayView.h"

#include <M5Unified.h>

namespace {
constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kFg = TFT_WHITE;
constexpr uint16_t kDim = 0x7BEF;     // grey
constexpr uint16_t kHeader = 0x001F;  // blue
constexpr uint16_t kAccent = 0x07FF;  // cyan
constexpr uint16_t kWarn = 0xFFE0;    // yellow

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kHeaderH = 24;
constexpr int kRowH = 17;
constexpr int kValueX = 112;

// Draws `text` left-aligned at (x, y), clearing the rest of the line.
void line(const String& text, int x, int y, const lgfx::IFont* font, uint16_t color) {
  auto& d = M5.Display;
  d.setFont(font);
  d.setTextColor(color, kBg);
  d.setTextPadding(kW - x);
  d.drawString(text, x, y);
}
}  // namespace

void DisplayView::begin() {
  auto& d = M5.Display;  // M5.begin() already set the Core2's landscape rotation
  d.setFont(&fonts::Font2);
  d.fillScreen(kBg);
  d.setTextDatum(textdatum_t::middle_left);
}

void DisplayView::enter(Screen screen, const char* title) {
  if (screen_ == screen) return;
  screen_ = screen;
  M5.Display.fillScreen(kBg);
  drawHeader(title);
}

void DisplayView::drawHeader(const char* title) {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, kHeaderH, kHeader);
  d.setFont(&fonts::Font2);
  d.setTextColor(kFg, kHeader);
  d.setTextPadding(0);
  d.drawString(title, 8, kHeaderH / 2);
}

void DisplayView::showDiagnostics(const std::vector<Row>& rows) {
  auto& d = M5.Display;
  enter(Screen::Diagnostics, "mStream Player - bring-up");
  d.setFont(&fonts::Font2);
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

void DisplayView::showNowPlaying(const NowPlaying& np) {
  auto& d = M5.Display;
  if (screen_ != Screen::NowPlaying) {
    enter(Screen::NowPlaying, "mStream Player");
    // Labels for the touch buttons under the screen: click / hold.
    d.setFont(&fonts::Font0);
    d.setTextColor(kDim, kBg);
    d.setTextPadding(0);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString("prev / vol-", 53, kH - 8);
    d.drawString("play / output", 160, kH - 8);
    d.drawString("next / vol+", 267, kH - 8);
    d.setTextDatum(textdatum_t::middle_left);
  }

  // Battery, right-aligned in the header.
  d.setFont(&fonts::Font2);
  d.setTextColor(kFg, kHeader);
  d.setTextDatum(textdatum_t::middle_right);
  d.setTextPadding(48);
  d.drawString(np.battery, kW - 8, kHeaderH / 2);
  d.setTextDatum(textdatum_t::middle_left);

  String title = np.title;
  if (title.length() > 21) title = title.substring(0, 20) + "~";

  line(np.position, 8, 38, &fonts::Font2, kDim);
  line(title, 8, 64, &fonts::Font4, kFg);
  line(np.subtitle, 8, 90, &fonts::Font2, kDim);
  line(np.status, 8, 116, &fonts::Font4, kAccent);
  line(np.output, 8, 142, &fonts::Font2, kFg);
  line(np.volume, 8, 160, &fonts::Font2, kFg);
  line(np.note, 8, 180, &fonts::Font2, kWarn);
  line(np.stats, 8, 206, &fonts::Font0, kDim);
}
