#pragma once
#include <vector>

#include "PlaybackController.h"
#include "ServerCandidate.h"
#include "Track.h"

// Thin rendering layer over TFT_eSPI. Stateless beyond the panel handle: the
// app owns UI state and tells the view what to draw.
class DisplayView {
public:
  void begin();

  void showDiscovery(const std::vector<ServerCandidate>& servers, int selected, bool wifiConnected);
  void showLibrary(const std::vector<Track>& tracks, int selected, int top);
  void showNowPlaying(const Track* track, PlayState state, uint32_t posMs, uint32_t durMs);
  void showDocked();

  // Rows of the library list that fit on screen.
  int visibleRows() const { return 8; }

private:
  void drawHeader(const char* title);
};
