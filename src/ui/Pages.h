#pragma once
#include <cstdint>

#include "BitSet.h"
#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "ui/ListView.h"
#include "ui/Page.h"

// The pages the tab bar leads to. These are the framework's first pages,
// enough to exercise it on the device (the real screens come next): a Now
// Playing page with transport, the Library as artists -> an artist's albums
// -> an album's tracks with the inline actions, the queue, the dancer, and
// the output with a way into the touch calibration.
namespace ui {

// A page header (y 36-71): "‹" (back, x 0-55), a title and a dim summary,
// and an optional pill button at the right (its hit area x 240 to the
// screen's edge). A 2 px rule in the section's colour under it.
struct Header {
  const char* title = "";
  const char* sub = "";
  bool back = false;
  bool cross = false;           // "✕" instead of "‹" (leave a mode)
  const char* right = nullptr;  // the pill's label
  bool rightDanger = false;     // red (Remove)
  bool pressed = false;         // the pill is under a finger
  // What a touch in the header row hits: 1 back (or ✕), 2 the pill, 3 the
  // rest of the row (the title), 0 not the header.
  int hit(const InputEvent& e) const;
};

// ---- Now Playing ----
class NowPlayingPage : public Page {
public:
  explicit NowPlayingPage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Now Playing"; }
  void enter(NavModel::PageRef& ref) override;
  void repaint() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  void onEvent(const InputEvent& e) override;
  void onSheet(int choice) override;
  void describe(char* buf, size_t size) const override;

private:
  enum Zone : int8_t { None = -1, Artist, Album, Volume, Prev, PlayPause, Next, More };
  Zone zoneAt(const InputEvent& e) const;
  void drawTitle();
  void drawArtistAlbum();
  void drawProgress();
  void drawTransport();
  void goToLibrary(bool album);

  struct Drawn {
    bool valid = false;
    uint32_t track = 0xFFFFFFFFu;
    PlayState play = PlayState::Stopped;
    uint32_t second = 0xFFFFFFFFu;
    uint32_t durationS = 0;
    int32_t current = -2;
    uint32_t size = 0;
    uint8_t volume = 255;
    bool bluetooth = false;
  } drawn_;
  Zone pressed_ = None;
};

// ---- Library: artists, an artist, an album, an artist's tracks ----
class LibraryPage : public Page, public ListView::Source {
public:
  explicit LibraryPage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Library"; }
  bool scrolls() const override { return true; }
  void enter(NavModel::PageRef& ref) override;
  void leave() override;
  void repaint() override;
  void repaintHeader() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  bool animating() const override;
  void onEvent(const InputEvent& e) override;
  void onSheet(int choice) override;
  void describe(char* buf, size_t size) const override;

  // ListView::Source
  uint32_t rows() override;
  void drawRow(ListView::Row& r) override;
  uint16_t accent() override;
  bool topBar() override;
  int actions(int32_t row, const char* labels[3]) override;
  void onAction(int32_t row, int action) override;
  ListView::Tap onTap(uint32_t row) override;
  bool holds(uint32_t row) override;
  void onHold(uint32_t row) override;
  bool alphabetical() override;
  char railKey(uint32_t row) override;
  const char* emptyText() override;

private:
  const LibraryIndex* index() const;
  int32_t playingRow() const;
  bool crumb() const;
  // The tracks a container row (or the page's own container, row -1) plays.
  LibraryIndex::Span tracksOf(PageKind kind, uint32_t id) const;
  LibraryIndex::Span pageTracks() const { return tracksOf(kind_, id_); }
  // What row `row` opens (Artist page: row 0 "All tracks", then the albums).
  bool rowContainer(uint32_t row, PageKind* kind, uint32_t* id) const;
  void act(PageKind kind, uint32_t id, int action, int32_t startTrack);
  Header header() const;

  PageKind kind_ = PageKind::Artists;
  uint32_t id_ = 0;
  NavModel::PageRef* ref_ = nullptr;
  bool touchInList_ = false;
  int headerPressed_ = 0;  // Header::hit() of the finger on the header
  uint32_t drawnTrack_ = 0xFFFFFFFFu;
  // A long press's container, for its sheet.
  PageKind heldKind_ = PageKind::None;
  uint32_t heldId_ = 0;
};

// ---- Queue ----
class QueuePage : public Page, public ListView::Source {
public:
  explicit QueuePage(Ui& ui);
  const char* name() const override { return "Queue"; }
  bool scrolls() const override { return true; }
  void enter(NavModel::PageRef& ref) override;
  void leave() override;
  void repaint() override;
  void repaintHeader() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  bool animating() const override;
  void onEvent(const InputEvent& e) override;
  void home() override;
  void describe(char* buf, size_t size) const override;
  // The next enter() shows entry `pos` (the toast's "View"), not where the
  // page was.
  void showOnEnter(uint32_t pos) { showPos_ = static_cast<int32_t>(pos); }

  // ListView::Source
  uint32_t rows() override;
  void drawRow(ListView::Row& r) override;
  uint16_t accent() override;
  int actions(int32_t row, const char* labels[3]) override;
  void onAction(int32_t row, int action) override;
  ListView::Tap onTap(uint32_t row) override;
  bool holds(uint32_t row) override {
    (void)row;
    return !selecting_;  // selection mode
  }
  void onHold(uint32_t row) override;
  bool selecting() override { return selecting_; }
  bool selected(uint32_t row) override { return selection_.get(row); }
  const char* emptyText() override { return "The queue is empty: add music from the Library"; }

private:
  void setSelecting(bool on);
  void removeSelected();
  void jump();  // the title's tap: the playing track, the top, the end, in turn
  Header header() const;

  NavModel::PageRef* ref_ = nullptr;
  // The queue when the page was left: a different one on return (replaced,
  // added to) makes the remembered open row and place meaningless.
  bool left_ = false;
  uint32_t leftContent_ = 0;
  int32_t showPos_ = -1;
  uint8_t jump_ = 0;
  BitSet selection_;
  bool selecting_ = false;
  bool touchInList_ = false;
  int headerPressed_ = 0;
  uint32_t content_ = 0, position_ = 0;
  uint32_t headerSig_ = 0;
};

// ---- Dance ----
class DancePage : public Page {
public:
  explicit DancePage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Dance"; }
  void enter(NavModel::PageRef& ref) override;
  void leave() override;
  void repaint() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  void onEvent(const InputEvent& e) override;
  void describe(char* buf, size_t size) const override;

private:
  void drawPanels(bool all);
  uint32_t nextPanelMs_ = 0;
  // What the panels show (redrawn when it changes, at most twice a second).
  int bpmX10_ = -1;
  int conf_ = -1;
  int lock_ = -1;
  int skin_ = -1;
  uint32_t track_ = 0xFFFFFFFFu;
  int progress_ = -1;
};

// ---- Output ----
class OutputPage : public Page {
public:
  explicit OutputPage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Output"; }
  void enter(NavModel::PageRef& ref) override;
  void repaint() override;
  void repaintHeader() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  void onEvent(const InputEvent& e) override;
  void onDialog(int button) override;
  void describe(char* buf, size_t size) const override;

private:
  enum Zone : int8_t { None = -1, Bluetooth, Speaker, VolDown, VolUp, Calibrate, Forget };
  Zone zoneAt(const InputEvent& e) const;
  void drawCards();
  void drawVolume();
  void drawLinks();
  uint32_t signature() const;
  uint32_t drawnSig_ = 0;
  uint8_t drawnVolume_ = 255;
  Zone pressed_ = None;
};

}  // namespace ui
