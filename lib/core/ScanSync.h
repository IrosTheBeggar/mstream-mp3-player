// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// When a hardware-scrolled list frame goes on the bus, from where the
// panel's scan is (docs/UI-SPIKE.md, "Frames on the panel's scan").
//
// The Core2's LCD refreshes by itself, about 71.5 times a second (244 scan
// lines of 57 us: 240 rows and two porches of 2), and its TE pin isn't
// wired. A frame drawn at any moment is cut by the scan: the rows above it
// show the old picture, the rows below the new one (a tear), and at 30
// frames a second on a 71.5 Hz panel a frame stays 2 or 3 refreshes by
// turns (a judder). But the controller says where its scan is (Get_Scanline,
// 45h: ui/PanelScan), so a frame can be put where the scan never sees it
// half done, and every refresh can have its frame.
//
// A move of d lines (ui/ListScroller) is one bus hold: the d new content
// lines are written into the GRAM lines of the d rows leaving at the far
// edge (the strip: still on screen), then the start address changes. For
// the scan to show nothing of that but the finished frame:
//
//   - a strip row is written after the scan passed it in this refresh
//     (else new lines show at the wrong edge for a refresh). The bus is
//     slower than the scan (a 320 px line takes ~130 us, the scan 57 us a
//     row), so a hold that starts just behind the scan at the strip's
//     first row stays behind it;
//   - the address changes while the scan is outside the band: after its
//     last row, before its first row of the next refresh (else the band is
//     drawn part with the old address, part with the new).
//
// plan() gives the window of time a hold may start in for both to hold, for
// the first refresh it can still make, and when that frame is on screen
// (the scan reaches the band's first row): where a fling's position is
// sampled, so equal steps of time show as equal steps on the glass. The
// window rests on the caller's guess of the hold's length; a hold that
// turns out shorter would send its address early, with the scan still in
// the band, so plan() also says when the scan has left it: the hold waits
// for that before the address.
//
// A step too big for its hold to fit (over ~50 lines down the list at 71.5
// Hz) has no clean window: the address is kept on time and the hold starts
// early, so the strip's first rows show a refresh early (a few lines at
// the edge, for 14 ms), never the tear.
//
// Portable: the caller reads the scan and the clock. Host-tested
// (test_scan_sync).
class ScanSync {
public:
  struct Panel {
    uint32_t frameUs = 0;  // one refresh
    int lines = 0;         // scan lines a refresh (the rows and both porches)
    int firstRow = 0;      // the scan line of screen row 0
  };
  struct Config {
    int enterLines = 4;  // behind the scan at the strip's first row by this much
    int leaveLines = 2;  // the address this long after the scan left the band's last row
    int exitLines = 8;   // ... and this long before it is back at the band's first
  };
  // From the reading of the scan, in microseconds.
  struct Window {
    int32_t earliestUs = 0;  // the hold may start from here (0: now)
    int32_t latestUs = 0;    // and no later than this (never negative)
    int32_t addressUs = 0;   // the address not before this: the scan has left the band
    int32_t showsUs = 0;     // the frame is on screen
    bool clean = false;      // the strip stays behind the scan too
    bool late = false;       // the window is over (within `lateUs`): start now
  };

  ScanSync() = default;
  explicit ScanSync(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  void setPanel(const Panel& p) { panel_ = p; }
  const Panel& panel() const { return panel_; }
  // The scrolled band: screen rows [top, top + height).
  void setBand(int top, int height) {
    top_ = top;
    height_ = height;
  }
  bool ready() const {
    return panel_.frameUs > 0 && panel_.lines > 0 && height_ > 0 && top_ >= 0 && top_ + height_ + panel_.firstRow <= panel_.lines;
  }

  // `lines` scan lines as time.
  int32_t lineTime(int32_t lines) const {
    return static_cast<int32_t>(static_cast<int64_t>(lines) * panel_.frameUs / panel_.lines);
  }
  // From a scan at `scanLine` to the band's first row (0 when it is there).
  int32_t untilBand(int scanLine) const;

  // The window for a move of `d` lines (positive: the list moves up, the
  // strip is the band's top d rows; negative: its bottom rows) whose hold
  // takes `holdUs` up to the address, with the scan read at `scanLine`
  // now. Needs ready() and d != 0.
  //
  // `lateUs`: a window that closed no longer ago than this is still the
  // one planned (`late`; earliestUs and latestUs 0: start now). Such a
  // hold's address goes out with the scan back in the band, and the rows
  // it had passed show the frame a refresh late (a tear, for that
  // refresh); a caller takes that over a refresh with no frame at all (a
  // list that stands still for two refreshes, then jumps twice as far)
  // when it gives a whole refresh here: no window is then ever waited out.
  Window plan(int scanLine, int d, uint32_t holdUs, uint32_t lateUs = 0) const;

private:
  Config config_;
  Panel panel_;
  int top_ = 0;
  int height_ = 0;
};
