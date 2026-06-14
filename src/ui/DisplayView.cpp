#include "ui/DisplayView.h"

#include <TFT_eSPI.h>

namespace {
TFT_eSPI tft;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kFg = TFT_WHITE;
constexpr uint16_t kDim = 0x7BEF;       // grey
constexpr uint16_t kAccent = 0x07FF;    // cyan
constexpr uint16_t kSelBg = 0x001F;     // blue highlight

constexpr int kW = 240;
constexpr int kH = 320;
constexpr int kRowH = 26;
constexpr int kHeaderH = 30;

String fmtTime(uint32_t ms) {
  uint32_t s = ms / 1000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return String(buf);
}
}  // namespace

void DisplayView::begin() {
  tft.init();
  tft.setRotation(0);  // portrait, 240x320
  tft.fillScreen(kBg);
  tft.setTextDatum(TL_DATUM);
}

void DisplayView::drawHeader(const char* title) {
  tft.fillRect(0, 0, kW, kHeaderH, kSelBg);
  tft.setTextColor(kFg, kSelBg);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(title, 8, kHeaderH / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void DisplayView::showDiscovery(const std::vector<ServerCandidate>& servers, int selected, bool wifiConnected) {
  tft.fillScreen(kBg);
  drawHeader("Discover");

  if (!wifiConnected) {
    tft.setTextColor(kDim, kBg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Connecting to Wi-Fi...", kW / 2, kH / 2, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  if (servers.empty()) {
    tft.setTextColor(kAccent, kBg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Searching for servers...", kW / 2, kH / 2 - 10, 2);
    tft.setTextColor(kDim, kBg);
    tft.drawString("mStream on your network", kW / 2, kH / 2 + 14, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  const int rows = visibleRows();
  for (int i = 0; i < rows && i < static_cast<int>(servers.size()); ++i) {
    const int y = kHeaderH + 4 + i * kRowH;
    const bool sel = (i == selected);
    if (sel) tft.fillRect(0, y, kW, kRowH, kSelBg);

    const ServerCandidate& s = servers[i];
    tft.setTextColor(sel ? kFg : kDim, sel ? kSelBg : kBg);
    tft.setTextDatum(ML_DATUM);
    String name = s.instanceName.c_str();
    if (name.length() > 22) name = name.substring(0, 21) + "...";
    tft.drawString(name, 10, y + 8, 2);
    // host:port subtitle
    String addr = String(s.host.c_str()) + ":" + String(s.port);
    tft.setTextColor(sel ? kAccent : kDim, sel ? kSelBg : kBg);
    tft.drawString(addr, 10, y + 19, 1);
  }
  tft.setTextDatum(TL_DATUM);
}

void DisplayView::showLibrary(const std::vector<Track>& tracks, int selected, int top) {
  tft.fillScreen(kBg);
  drawHeader("Library");

  const int rows = visibleRows();
  for (int i = 0; i < rows; ++i) {
    const int idx = top + i;
    const int y = kHeaderH + 4 + i * kRowH;
    if (idx < 0 || idx >= static_cast<int>(tracks.size())) continue;

    const bool sel = (idx == selected);
    if (sel) tft.fillRect(0, y, kW, kRowH, kSelBg);

    tft.setTextColor(sel ? kFg : kDim, sel ? kSelBg : kBg);
    tft.setTextDatum(ML_DATUM);
    String line = tracks[idx].title.c_str();
    if (line.length() > 24) line = line.substring(0, 23) + "...";
    tft.drawString(line, 10, y + kRowH / 2, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void DisplayView::showNowPlaying(const Track* track, PlayState state, uint32_t posMs, uint32_t durMs) {
  tft.fillScreen(kBg);
  drawHeader("Now Playing");

  const char* title = track ? track->title.c_str() : "(nothing)";
  const char* artist = track ? track->artist.c_str() : "";

  tft.setTextColor(kFg, kBg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(title, kW / 2, 120, 4);
  tft.setTextColor(kDim, kBg);
  tft.drawString(artist, kW / 2, 150, 2);

  // Progress bar
  const int barX = 16, barY = 210, barW = kW - 32, barH = 8;
  tft.drawRect(barX, barY, barW, barH, kDim);
  if (durMs > 0) {
    int fill = static_cast<int>((static_cast<uint64_t>(posMs) * (barW - 2)) / durMs);
    if (fill < 0) fill = 0;
    if (fill > barW - 2) fill = barW - 2;
    tft.fillRect(barX + 1, barY + 1, fill, barH - 2, kAccent);
  }

  tft.setTextColor(kDim, kBg);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(fmtTime(posMs), barX, barY + 22, 2);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(fmtTime(durMs), barX + barW, barY + 22, 2);

  // Transport state glyph
  const char* glyph = state == PlayState::Playing ? "> PLAYING"
                      : state == PlayState::Paused ? "|| PAUSED"
                                                   : "[] STOPPED";
  tft.setTextColor(kAccent, kBg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(glyph, kW / 2, 260, 2);
  tft.setTextDatum(TL_DATUM);
}

void DisplayView::showDocked() {
  tft.fillScreen(kBg);
  drawHeader("Docked");
  tft.setTextColor(kAccent, kBg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("USB MASS STORAGE", kW / 2, 140, 4);
  tft.setTextColor(kDim, kBg);
  tft.drawString("Library available to host", kW / 2, 175, 2);
  tft.drawString("Press DOCK to eject", kW / 2, 210, 2);
  tft.setTextDatum(TL_DATUM);
}
