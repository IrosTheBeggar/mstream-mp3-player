// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "InputEvent.h"
#include "NavModel.h"

// A page of the UI: what a tab shows (its root) or what was pushed on it.
// One object per kind of page (static, in the Ui, no heap churn); the
// state of each place it is shown at lives in its nav entry (PageRef): the
// thing it shows (an artist id...), its scroll position, its expanded row.
namespace ui {

class Ui;

// PageRef::kind values.
enum class PageKind : uint8_t {
  None = 0,
  NowPlaying,
  Library,       // the Library's root: id is its segment (LibrarySegment)
  Artist,        // an artist: Play/Play next/+Queue, "All tracks", the albums
  Album,         // an album's tracks
  ArtistTracks,  // all of an artist's tracks
  Folder,        // a folder: its folders, then its audio files
  Queue,
  Dance,
  Output,
  Pair,   // the Output tab's Pair screen (a scan list)
  About,  // the Output tab's About
  DeviceInfo,  // About's Device info (the board, chips, memory, uptime)
};
// The Library root's segments (its PageRef::id).
enum class LibrarySegment : uint8_t { Artists = 0, Albums = 1, Folders = 2 };
const char* pageKindName(uint8_t kind);

// PageRef::scrollPx for a page opened from Now Playing: open at the playing
// track (the review's "scroll to the playing item"), not at the top.
constexpr int32_t kShowPlaying = -2;

// What a sheet or a dialog reports to whoever opened it.
class OverlayOwner {
public:
  // The row tapped (0..), or -1: closed without a choice.
  virtual void onSheet(int choice) { (void)choice; }
  // The button tapped (0..), or -1: closed (a tab tap).
  virtual void onDialog(int button) { (void)button; }

protected:
  ~OverlayOwner() = default;
};

class Page : public OverlayOwner {
public:
  explicit Page(Ui& ui) : ui_(ui) {}
  virtual ~Page() = default;
  virtual const char* name() const = 0;
  // Uses the list band's hardware scroll (ui/ListView).
  virtual bool scrolls() const { return false; }
  // Takes the content area (y 36-239) for the place `ref` names, and draws
  // all of it. `ref` stays valid while the page is up.
  virtual void enter(NavModel::PageRef& ref) = 0;
  // Leaving (another page, another tab, the display taken): save the place.
  virtual void leave() {}
  // Draw everything again, the state kept (an overlay closed).
  virtual void repaint() = 0;
  // Draw the header row again (the toast was over it).
  virtual void repaintHeader() { repaint(); }
  // It has the page header (y 36-71: ‹, a title, a pill), whose ‹ and pill
  // stay live under the toast. Now Playing and the Dance tab have none.
  virtual bool hasHeader() const { return true; }
  // Every loop pass while the page is up and nothing modal covers it:
  // redraw what changed. `frameDue`: an animation frame may be drawn now
  // (30 fps on deadlines); `wholeRows`: lists move in whole rows (the audio
  // is short of time). True if a frame was drawn.
  virtual bool update(uint32_t nowMs, bool frameDue, bool wholeRows) {
    (void)nowMs;
    (void)frameDue;
    (void)wholeRows;
    return false;
  }
  // Moving: the UI keeps its frame cadence (and the loop sleeps less).
  virtual bool animating() const { return false; }
  // Glass events of a touch that landed in the content area, or of a swipe
  // up from the button strip (fromStrip: it starts with a DragStart, no
  // Down; only a list may follow it, nothing may take it for a press).
  virtual void onEvent(const InputEvent& e) { (void)e; }
  // Its tab was tapped again at its root (e.g. the Queue: back to the
  // playing track).
  virtual void home() {}
  // Every UI pass, whether or not a modal covers it (update() doesn't run
  // then) and while dark: deadlines that mustn't wait for the page to be
  // seen (the Pair screen's 2 min scan). Draws nothing.
  virtual void tick(uint32_t nowMs) { (void)nowMs; }
  // The screen went off (ScreenPower): stop what costs power for nobody
  // (the Pair screen's scan). The page stays up, drawing nothing, until
  // repaint() on the wake.
  virtual void screenOff() {}
  // An album's cover thumbnail arrived (ui/Thumbs): redraw what shows it.
  virtual void thumbReady(uint32_t album) { (void)album; }
  // 'ui' on the console: one line about its state.
  virtual void describe(char* buf, size_t size) const;

protected:
  Ui& ui_;
};

}  // namespace ui
