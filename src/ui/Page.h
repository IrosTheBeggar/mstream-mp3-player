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
  Artists,       // Library root: every artist, A-Z
  Artist,        // an artist: Play/Play next/+Queue, "All tracks", the albums
  Album,         // an album's tracks
  ArtistTracks,  // all of an artist's tracks
  Queue,
  Dance,
  Output,
};
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
  // Glass events of a touch that landed in the content area.
  virtual void onEvent(const InputEvent& e) { (void)e; }
  // Its tab was tapped again at its root (e.g. the Queue: back to the
  // playing track).
  virtual void home() {}
  // 'ui' on the console: one line about its state.
  virtual void describe(char* buf, size_t size) const;

protected:
  Ui& ui_;
};

}  // namespace ui
