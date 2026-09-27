#pragma once
#include <M5GFX.h>

#include <cstdint>

#include "InputEvent.h"
#include "KineticScroll.h"
#include "ListLayout.h"
#include "NavModel.h"
#include "ui/Fonts.h"
#include "ui/Input.h"
#include "ui/ListScroller.h"

// The list every list screen is (the tab bar spec §6.3): virtualised rows
// in the band y 72-239, scrolled by the LCD's hardware vertical scroll
// (ListScroller: the user found it "much smoother"), with inertia
// (KineticScroll, flings capped at 2,000 px/s) and frames on the UI's
// 30 fps deadlines. Only the rows on screen exist, as six PSRAM row sprites
// (320 x 42, 27 KB each); a move pushes only the newly exposed lines.
//
// A list is a Source: a row count and a row renderer (drawRow() paints row
// i into a 320 x 42 sprite; the standard pieces below make the 1- and
// 2-line rows, the number column, the initial disc, the 40 x 40 thumbnail
// slot and the chevron), plus what its rows do:
//
//   - tap: the source acts (opens something), or asks for the row's inline
//     action bar: a 42 px bar of up to three buttons (Play / Play next /
//     + Queue) opens under the row; tapping the row again, or another row,
//     closes it (ListLayout keeps what's on screen steady);
//   - a top bar: the same buttons as the list's first item (a container's
//     own Play / Play next / + Queue);
//   - long press: the source's (a sheet, or selection mode), on rows it
//     holds(); a long press on anything else ends as a tap when it lifts;
//   - selection mode: a checkbox on every row, taps toggle;
//   - the right edge: an A-Z rail on long alphabetical lists (over ~30
//     rows), a thin scrollbar on other lists longer than the band. Both sit
//     inside the scrolled band, which moved them with every frame (the
//     user's "the scroll bar on the right jitters when swiping"): they are
//     hidden while the list moves (their column cleared once, the rows then
//     pushed full width over it), and drawn again when it settles, or at
//     once when a finger lands on the rail (x 280 to the edge). A drag on it
//     scrubs (a tick per new letter; resting on it first is no hold); a tap
//     only stops the list (the jump grid will be the tap's).
//
// Each page's scroll position and expanded row are kept in its nav entry
// (NavModel::PageRef): attach() restores them, detach() saves them.
// Presses highlight at once (the Down), act on the Tap (with the tap tick);
// a drag never taps, nor does a press that stops a moving list.
// Loop task only.
namespace ui {

class ListView : private ListScroller::Painter {
public:
  static constexpr int kTop = 72;
  static constexpr int kHeight = 168;
  static constexpr int kSlots = 6;
  // Moves of up to this many lines are one bus hold of new lines (2 rows,
  // the scroll lab's setting); more is a full redraw in place.
  static constexpr int kMaxStep = 2 * 42;
  // A long alphabetical list gets the A-Z rail (the review: only above
  // about 30 entries).
  static constexpr uint32_t kRailMinRows = 30;

  // What a row renderer gets: the sprite (cleared to `bg`), and the
  // columns: content from x to right (a checkbox before x in selection
  // mode, the rail's column after right).
  struct Row {
    M5Canvas& c;
    uint32_t row;
    int x;
    int right;
    uint16_t bg;
    bool pressed;
    bool expanded;
    bool selected;
  };
  enum class Tap : uint8_t { Handled, Expand };

  class Source {
  public:
    virtual uint32_t rows() = 0;
    virtual void drawRow(Row& r) = 0;
    virtual uint16_t accent() = 0;
    // A top bar (item 0) with actions(-1, ...).
    virtual bool topBar() { return false; }
    // The buttons of the top bar (row -1) or of row's inline bar: up to 3
    // labels, the first is the primary. Returns how many.
    virtual int actions(int32_t row, const char* labels[3]) {
      (void)row;
      (void)labels;
      return 0;
    }
    virtual void onAction(int32_t row, int action) {
      (void)row;
      (void)action;
    }
    // A tap on a row: act (Handled) or open its inline bar (Expand). In
    // selection mode: toggle it (the list redraws the row).
    virtual Tap onTap(uint32_t row) {
      (void)row;
      return Tap::Handled;
    }
    // Whether a long press on the row does something (onHold): only then
    // is it a hold (the double tick); else it ends as a tap.
    virtual bool holds(uint32_t row) {
      (void)row;
      return false;
    }
    virtual void onHold(uint32_t row) { (void)row; }
    // Sorted by name: the A-Z rail, with each row's letter.
    virtual bool alphabetical() { return false; }
    virtual char railKey(uint32_t row) {
      (void)row;
      return '#';
    }
    virtual bool selecting() { return false; }
    virtual bool selected(uint32_t row) {
      (void)row;
      return false;
    }
    // Shown in the band when there are no rows.
    virtual const char* emptyText() { return "Nothing here"; }

  protected:
    ~Source() = default;
  };

  // The row sprites and the rail's (PSRAM). False: no memory.
  bool begin(ListScroller& scroller, Input& input);

  // Shows `src`, where its page was (ref's scrollPx, or `defaultOffset` the
  // first time; its expanded row). The band is redrawn at the next frame.
  void attach(Source* src, NavModel::PageRef* ref, int32_t defaultOffset = 0);
  // Saves the place into the page's ref; the list shows nothing after.
  void detach();
  bool attached() const { return src_ != nullptr; }

  // The rows changed (count, content): the band is redrawn in place.
  void reload();
  // Every row is drawn again (the playing track moved, selection mode).
  void refreshAll();
  // One row's look changed: drawn again in place.
  void refreshRow(uint32_t row);
  // Jumps (no animation).
  void scrollTo(int32_t offset);
  void scrollToRow(uint32_t row);  // that row at the top (as far as the list goes)
  void revealRow(uint32_t row);    // that row on screen, moving as little as possible
  void collapse();
  // Something else drew over the band: all of it is drawn again.
  void invalidate();

  // Every loop pass while the page is up. `frameDue`: a frame may be drawn
  // (the UI's 30 fps cadence); `wholeRows`: the audio is short of time,
  // move in whole rows (ScrollGovernor). True if a frame was drawn.
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows);
  // Moving, or owing a frame: the UI keeps its frame cadence.
  bool animating() const;
  // A glass event of a touch that landed in the band.
  void onEvent(const InputEvent& e);

  int32_t offset() const;
  int32_t expanded() const { return layout_.expanded(); }
  const ListLayout& layout() const { return layout_; }
  const KineticScroll& scroll() const { return scroll_; }
  uint32_t framesDrawn() const { return frames_; }

  // ---- the standard row pieces (each returns the x after it) ----
  // A number, right-aligned in a 30 px column ("06"); or the playing
  // track's EQ glyph in its place.
  static int number(Row& r, uint32_t n, uint16_t colour);
  static int playing(Row& r, uint16_t colour);
  // A 30 px disc with an initial.
  static int disc(Row& r, char initial, uint16_t colour);
  // The 40 x 40 thumbnail slot (a placeholder until covers are cached).
  static int thumb(Row& r);
  // A chevron at the right; returns the new right edge.
  static int chevron(Row& r);
  // One line (centred) or two (title over subtitle), fitted with "…".
  static void lines(Row& r, int x, int right, const char* title, size_t titleLen, const char* sub, size_t subLen,
                    uint16_t titleColour, Font titleFont = Font::Body);

private:
  enum class Edge : uint8_t { None, Rail, Bar };
  enum class TouchOn : uint8_t { None, List, Rail };
  struct Slot {
    M5Canvas sprite;
    int32_t item = -1;
  };

  // The list moves on screen: flinging, snapping, or a finger dragging it
  // (a finger that is only down, for a tap, doesn't hide the rail).
  bool moving() const;
  int layoutWidth() const { return edge_ == Edge::Rail ? 284 : edge_ == Edge::Bar ? 312 : 320; }
  int pushWidth() const { return railUp_ || railWanted_ ? layoutWidth() : 320; }
  int railX() const { return layoutWidth(); }
  void setEdge();
  void dropSlots();
  Slot* slotFor(uint32_t item);
  Slot* renderedSlot(uint32_t item);
  void renderItem(M5Canvas& c, uint32_t item);
  void renderBar(M5Canvas& c, int32_t row, bool inline_);
  int buttonAt(int x, int n) const;
  void render(int32_t off);
  void pushItemInPlace(uint32_t item);
  void setPressed(int32_t item, int button);
  void tapItem(int32_t item, const InputEvent& e);
  void setExpanded(int32_t row);
  void drawRailSprite(int32_t off);
  void hideRail();
  void showRail(int32_t off);
  void scrub(int y);
  void drawEmpty();
  void saveRef();
  // ListScroller::Painter
  void prepare(const VScrollMap::Span* spans, int n) override;
  void push(const VScrollMap::Span& span, bool committing) override;
  void prepareFixed(int32_t offset) override;
  void pushFixed() override;

  ListScroller* vs_ = nullptr;
  Input* input_ = nullptr;
  Slot* slots_ = nullptr;     // PSRAM, kSlots
  M5Canvas* rail_ = nullptr;  // PSRAM, 36 x 168
  Source* src_ = nullptr;
  NavModel::PageRef* ref_ = nullptr;
  ListLayout layout_;
  KineticScroll scroll_;
  Edge edge_ = Edge::None;
  TouchOn touchOn_ = TouchOn::None;
  int32_t drawnOffset_ = -1;
  bool force_ = true;
  bool railWanted_ = false;  // the rail should be on screen this pass
  bool railUp_ = false;      // it is
  bool railDue_ = false;     // prepareFixed() rendered it: pushFixed() pushes it
  int railKeyShown_ = -1;    // the scrub's last letter (a tick at each new one)
  int32_t pressedItem_ = -1;
  int pressedButton_ = -1;
  int32_t downItem_ = -1;
  bool dragged_ = false;  // this touch moved the list
  uint32_t keepFirst_ = 0, keepLast_ = 0;
  uint32_t frames_ = 0;
};

}  // namespace ui
