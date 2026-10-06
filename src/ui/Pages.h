// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "BitSet.h"
#include "LibraryIndex.h"
#include "OutputModel.h"
#include "PlaybackController.h"
#include "SeekBar.h"
#include "UiText.h"
#include "ui/ListView.h"
#include "ui/UiHost.h"
#include "ui/Page.h"

// The pages the tab bar leads to: Now Playing and the Library as the tab
// bar spec (§6.1, §6.2) has them, with the design review's grafts; the
// queue, the dancer and the output as the framework's first pages.
namespace ui {

// A page header (y 36-71): "‹" (back, x 0-55), a title and a dim summary,
// and an optional pill button at the right (its hit area x 240 to the
// screen's edge). A 2 px rule in the section's colour under it. With a
// `path`, two lines: the title over the path (left, cut from the left:
// "…/Kavinsky") and `counts` (right): the Folders explorer's one thin line.
struct Header {
  const char* title = "";
  const char* sub = "";
  bool back = false;
  bool cross = false;           // "✕" instead of "‹" (leave a mode)
  const char* right = nullptr;  // the pill's label
  bool rightDanger = false;     // red (Remove)
  bool rightBack = false;       // a "‹" before the pill's label (the Library root crumb)
  bool pressed = false;         // the pill is under a finger
  const char* path = nullptr;   // the second line (left)
  const char* counts = nullptr; // the second line (right)
  // What a touch in the header row hits: 1 back (or ✕), 2 the pill, 3 the
  // rest of the row (the title), 0 not the header.
  int hit(const InputEvent& e) const;
};

// ---- Now Playing (spec §6.1, mockups 01-04; docs/QUEUE-MODES.md) ----
//
//   36-37    background
//   39-136   the cover's 1 px frame; the cover 96 x 96 at (12, 40) (its
//            thumbnail, ui/Thumbs; a note until it's made)
//   38-89    the title, x 120-310 (190 px): DejaVu Bold 22 on up to 2
//            lines, else Bold 16 on up to 3
//   90-112   the artist (Body, soft), x 120-310   } plain rows of 23 px:
//   113-135  the album (Body, dim), x 120-310     } no "›", no taps of their own
//   136-145  background
//   113-145  the seek bar's readout while a finger scrubs, the full width
//            (over the album row and the cover's lowest rows)
//   138-167  the seek bar's touch, the full width, waiting or not
//   146-167  the progress line, elapsed / "4 of 16 · SPYDRONE" / length
//            (where it plays, when it fits: mockup 01's output line; the
//            headphones not connected, "SPYDRONE (not connected)" even
//            when the rest doesn't fit, so a play that waits for them
//            is no surprise)
//   168-239  [volume] [prev] [play/pause] [next] [...]   64 x 72 zones,
//            drawn as one 56-row strip at y 176-231 (the play disc r 25;
//            under the dots the shuffle and repeat indicator)
//
// Two menus, one job each (the user's split). A tap anywhere on the
// navigation area (everything above the seek bar's touch: the cover, the
// title, the artist, the album, their margins) opens the navigation menu,
// a 3-row sheet titled with the track: Go to artist, Go to album, Go to
// folder, each with its name dim on the right (the folder cut from the
// left: "…/Daft Punk/Discovery"). It acts on the track it was opened for.
// A built-in track, a Library not ready or a synthetic browse: a toast
// instead. A Down lights the area (ROW_SEL around the cover, which stays as
// it is). "..." opens the playback menu, "Playback": Shuffle (On/Off) and
// Repeat (Off/All/One), which change in place with the sheet staying up,
// then Sleep timer (its state; a tap opens the Sleep timer sheet). Every
// sheet ignores a touch that starts within 300 ms of its opening, so a
// double tap never picks a row.
//
// The progress line is a seek bar (docs/SEEK-BAR.md; lib/core/SeekBar): a
// knob at where it plays. A tap goes to the second under the finger (not a
// tap on the knob); a sideways drag scrubs (from the knob without a jump,
// from anywhere else the knob comes to the finger): the line thickens, a
// marker shows where it plays, the readout ("2:31 +1:21") takes the row
// above on the side away from the knob, and the music plays on until the
// lift, which seeks once (PlaybackController::seek()). Lifting with the
// knob back on the marker (it snaps there with a tick), or slid off the
// bar (above y 106, onto the artist row, or onto the strip: "Release to
// cancel"), seeks nothing, and so does a touch a sheet or a dialog takes.
// Seeks are whole seconds, from 0:00 to the length less 6 s (the edge
// readings reach both). Inert (no knob) while the length isn't known, the
// track failed, or it is under 10 s.
//
// While play waits for the headphones (PlayState::Waiting, PlayGate): the
// play button is a spinner (a tap, or B, cancels the wait: paused), the
// title strip has the title on one line over "Waiting for SPYDRONE..." and
// "try 2 of 3", and the artist and album rows give way to [Play on
// speaker] (the explicit choice) and [Cancel], drawn at y 94-129 and taking
// y 90-137. Only the cover opens the navigation menu then.
//
// The volume zone opens the volume sheet. While a sleep timer runs, the
// progress line has a moon and "23 min" ("track", "fading"; seconds in the
// last minute) after "4 of 16 · SPYDRONE", or in its place when both don't
// fit. Only what changed is redrawn: the cover when the album changes or
// its thumbnail arrives, the text when the track does (the title strip also
// when the wait's status does), the times once a second, the transport on
// a change (the play state, the volume, the output, shuffle, repeat), and
// the seek bar's frames on the 30 fps deadlines while a finger scrubs.
// Nothing queued: "Nothing playing" with Open Library and Shuffle all (or,
// with no card and no music, "No microSD card" and Try again).
class NowPlayingPage : public Page {
public:
  explicit NowPlayingPage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Now Playing"; }
  bool hasHeader() const override { return false; }
  void enter(NavModel::PageRef& ref) override;
  void leave() override;
  void repaint() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  // A finger scrubs the seek bar: the frame cadence (and the motion log).
  bool animating() const override { return bar_.scrubbing(); }
  void onEvent(const InputEvent& e) override;
  void onSheet(int choice) override;
  void thumbReady(uint32_t album) override;
  void describe(char* buf, size_t size) const override;

private:
  // (The transport's zones last, from Volume: `Volume + z`. Bar: the seek
  // bar, not "Seek", which PlaybackController has. Nav: the navigation
  // area, the cover and the text.)
  enum Zone : int8_t { None = -1, Nav, WaitSpeaker, WaitCancel, Bar, Volume, Prev, PlayPause, Next, More };
  // How the seek bar is drawn (docs/SEEK-BAR.md section 4.1).
  enum class BarLook : uint8_t { Inert, Rest, Pressed, Scrubbing, Off };
  // The navigation menu's rows, in order.
  enum class Go : uint8_t { Artist, Album, Folders };
  // Which menu the sheet up is (onSheet()).
  enum class Ask : uint8_t { None, Nav, Playback };
  Zone zoneAt(const InputEvent& e) const;
  // The playing track's album in the library index (kNone: none, or a
  // built-in track).
  uint32_t playingAlbum() const;
  void drawCover();
  // The title strip (while a play waits: the title on one line and the
  // wait's status).
  void drawTitle();
  void drawArtistAlbum();
  // The artist and album rows, or (waiting) the waiting panel's buttons in
  // their place.
  void drawMiddle();
  void drawWaiting();
  // The navigation area's background: ROW_SEL while a finger is down on it
  // (not while a play waits), else BG.
  uint16_t navBg() const;
  // What the navigation area's pieces don't cover, in navBg().
  void fillNav();
  // The navigation area in its look (the press look on or off): the gaps,
  // the title strip and the rows; the cover as it is.
  void drawNav();
  // The progress band, in the seek bar's look (the knob drawn last: the
  // text's background would cut it), and its text row (not while a finger
  // scrubs: the readout above says it all).
  void drawProgress();
  void drawProgressText(M5Canvas& c, bool paused);
  // The seek bar: seekable now (a track that hasn't failed, 10 s or more);
  // its look; the readout row above the line (over the album row and the
  // cover's lowest rows; while a play waits, the artist row cleared too,
  // the first time); the end of a scrub (the rows, the cover's rows from
  // its sprite; the band follows); a touch's seek, its tick and its log
  // line.
  bool seekable() const;
  BarLook barLook() const;
  void drawReadout();
  void endScrub();
  void seekTo(const SeekBar::Out& o, uint32_t nowMs);
  void drawTransport();
  // The shuffle and repeat indicator into `c`, centred at (cx, cy): the
  // glyphs that apply, nothing when neither does.
  void drawModes(M5Canvas& c, int cx, int cy);
  // The play button (a spinner while waiting) into `c`, centred at (cx, cy).
  void drawPlayButton(M5Canvas& c, int cx, int cy, bool down);
  // Only the play button's zone (the spinner's next step).
  void drawPlayZone();
  uint32_t waitSig() const;
  // The progress line's middle: at most this wide (between the times).
  static constexpr int kMidW = uitext::kNowPlayingMidW;
  // "SPYDRONE", "Speaker", "SPYDRONE (not connected)" (into buf).
  const char* outputName(char* buf, size_t size) const;
  uint32_t outputSig() const;
  // The two menus: the navigation menu for the playing track (or a toast
  // when nothing in it could act), and the playback menu.
  void openNavMenu();
  void openPlaybackMenu();
  // The Library at `track`'s artist, album or folders (the checks again:
  // the index may have gone while the menu was up).
  void goToLibrary(Go where, uint32_t track);
  // Nothing queued: the empty (or no-card) state instead of the player.
  bool emptyState(EmptyState& e) const;
  void onEmptyEvent(const InputEvent& e);

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
    uint32_t output = 0xFFFFFFFFu;  // outputSig()
    uint32_t coverAlbum = 0xFFFFFFFEu;
    bool coverShown = false;  // the thumbnail, not the placeholder
    bool empty = false;       // the empty state is what's drawn
    bool noCard = false;
    cardformat::Kind cardKind = cardformat::Kind::Unreadable;  // the no-card message drawn
    bool waiting = false;     // the waiting panel is what's drawn
    uint32_t waitSig = 0;     // waitSig()
    uint32_t sleep = 0;       // the sleep timer's text on the progress line (a hash)
    // The seek bar: its look (BarLook; 0xFF none), the knob's and the
    // marker's screen x (-1: none), what the readout shows (its fields, not
    // a hash: a readout that moved must never pass for the one drawn), and
    // whether the readout row is up (the scrub's look).
    uint8_t bar = 0xFF;
    int16_t knobX = -1;
    int16_t markerX = -1;
    SeekBar::Readout readout;
    bool scrubUp = false;
    // The indicator as drawn (repeat 0xFF: none yet).
    bool shuffle = false;
    uint8_t repeat = 0xFF;
  } drawn_;
  SeekBar bar_;
  Zone pressed_ = None;
  uint8_t spin_ = 0;          // the waiting spinner's step (8 a turn)
  uint32_t nextSpinMs_ = 0;
  int emptyPressed_ = -1;
  M5Canvas* cover_ = nullptr;  // PSRAM, 98 x 98 (the cover and its frame)
  Ask ask_ = Ask::None;
  uint32_t navTrack_ = 0xFFFFFFFFu;  // the navigation menu's track (its TrackCatalog id)
  char navFolder_[96] = "";          // its Go to folder detail, cut from the left
};

// ---- Library (spec §6.2, mockups 07-15, with the review's grafts) ----
//
// The root: Artists | Albums | Folders in the header (the segment is the
// root's PageRef::id; each keeps its own scroll). Then:
//   Artists   > an artist (Play / Play next / + Queue, "All tracks", its
//               albums) > an album, or all its tracks
//   Albums    > an album (every album A-Z, with its cover)
//   Folders   > a folder > ... (its folders, then its audio files; the
//               rest hidden but counted: "14 audio files, 1 other"; the
//               root's bar is "Play all N" and + Queue, not a Play that
//               replaces the queue with the whole card unannounced)
// A container's list starts with its Play / Play next / + Queue bar; a
// tapped track or file opens the same bar under it (Play: its album or
// folder from it, so the rest follows; the other two: it alone); a long
// press on any row offers the three in a sheet. Two levels down, the
// header's pill is "‹ Library", the root crumb. Long alphabetical lists
// (over 30) have the A-Z rail and its jump grid. The row that holds what
// plays now (the artist, the album, the track, the folders on its path) is
// tinted, and a page opened from Now Playing opens scrolled to it.
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
  void home() override;
  void thumbReady(uint32_t album) override;
  void describe(char* buf, size_t size) const override;
  // The segment the root shows (for a jump from Now Playing that keeps it).
  LibrarySegment segmentShown() const { return segment(); }

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
  uint32_t railRows() override;
  const char* railName(uint32_t row) override;
  const char* jumpTitle() override;
  bool tinted(uint32_t row) override;
  const char* emptyText() override;
  bool emptyState(EmptyState& e) override;
  void onEmptyAction(int i) override;

private:
  enum class RowKind : uint8_t { None, Artist, Album, AllTracks, Track, Folder, File };
  struct RowRef {
    RowKind kind = RowKind::None;
    uint32_t id = 0;     // the artist, album, folder or track
    uint32_t index = 0;  // a track's or a file's place in pageTracks()
  };

  const LibraryIndex* index() const;
  // The card's library (not the synthetic one of 'uil<n>'): the queue and
  // the covers can be used.
  bool real() const;
  LibrarySegment segment() const { return static_cast<LibrarySegment>(id_ <= 2 ? id_ : 0); }
  bool root() const { return kind_ == PageKind::Library; }
  bool folderList() const { return kind_ == PageKind::Folder || (root() && segment() == LibrarySegment::Folders); }
  uint32_t folderId() const { return kind_ == PageKind::Folder ? id_ : LibraryIndex::rootFolder(); }
  RowRef rowAt(uint32_t row) const;
  // The page's own tracks: an album's, an artist's, a folder's files (what
  // a track's Play plays from it).
  LibraryIndex::Span pageTracks() const;
  // The row that holds what plays now (-1: none), and whether a row does.
  int32_t playingRow() const;
  bool playing(const RowRef& r) const;
  void notePlaying();
  // Play (0) / Play next (1) / + Queue (2): `start` >= 0 plays `span`
  // from there. `name`: what the toast calls a whole container.
  void act(LibraryIndex::Span span, int32_t start, int action, const char* name);
  // What a container row (or, kind None, the page itself) plays, and its name.
  bool container(const RowRef& r, LibraryIndex::Span* span, const char** name) const;
  void onRowAction(const RowRef& r, int action);
  Header header() const;
  void drawSegments();
  void switchSegment(LibrarySegment s);
  bool crumb() const;
  const char* rowName(uint32_t row) const;

  PageKind kind_ = PageKind::Library;
  uint32_t id_ = 0;
  NavModel::PageRef* ref_ = nullptr;
  bool touchInList_ = false;
  int headerPressed_ = 0;  // Header::hit() of the finger on the header; the root: 10 + its segment
  int32_t segScroll_[3] = {-1, -1, -1};
  char playAll_[24] = "";  // the Folders root's "Play all N"
  // The header's texts (here, in PSRAM with the Ui, not in static RAM).
  mutable char headerSub_[64] = "";
  mutable char headerPath_[160] = "";
  // What plays now, in this index's ids (kNone: none of it).
  uint32_t drawnTrack_ = 0xFFFFFFFFu;
  uint32_t playTrack_ = LibraryIndex::kNone, playAlbum_ = LibraryIndex::kNone, playArtist_ = LibraryIndex::kNone;
  uint32_t playFolders_[NavModel::kMaxDepth + 2] = {};
  int playDepth_ = 0;
  // A long press's row, for its sheet.
  RowRef held_;
};

// ---- Queue (spec §6.4, mockups 16-18, with the review's grafts and the
// usability fixes) ----
//
// Normal:
//   36-71    "Queue  4 of 16 · 12 up next · 49 min"                 [Edit]
//            (too wide: "4 of 16 · 49 min", the position kept)
//            (a tap on the title: the playing track, the top, the end)
//   72-239   every entry; the playing one tinted with the accent's edge,
//            the played ones dim, a failed one with an amber "!", what a
//            Library add put in highlighted on the next visit (scrolled
//            to). A tap opens the row's bar: Play now / Play next /
//            Remove. A long press: selection mode, that row selected.
// Selection mode (mockup 17):
//   36-71    [✕]  "2 selected  of 16"                              [All]
//   72-197   the entries, three rows, checkboxes (the LCD's scroll band
//            ends above the bar)
//   198-239  [Remove 2] [Play next] [Clear...]  (Clear: a sheet, "Clear up
//            next" keeps what plays, "Clear queue" stops it after a
//            confirmation; every edit has Undo)
// Empty (mockup 18): "Your queue is empty" [Open Library] [Shuffle all];
// with no card and nothing queued, the no-card state.
class QueuePage : public Page, public ListView::Source {
public:
  // The band in selection mode: three rows over the bar.
  static constexpr int kEditBand = 126;
  static constexpr int kBarY = ListView::kTop + kEditBand;  // 198

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
  void onSheet(int choice) override;
  void onDialog(int button) override;
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
  bool tinted(uint32_t row) override;
  bool emptyState(EmptyState& e) override;
  void onEmptyAction(int i) override;
  const char* emptyText() override { return "The queue is empty"; }

private:
  enum class Ask : uint8_t { None, ClearSheet, ClearConfirm };
  void setSelecting(bool on);
  void removeSelected();
  void playSelectedNext();
  void selectAll();
  void clearUpNext();
  void clearQueue();
  void jump();  // the title's tap: the playing track, the top, the end, in turn
  Header header() const;
  uint32_t headerSig() const;
  void drawBar();
  int barAt(const InputEvent& e) const;
  // The header's summary, redone when the queue, its position or the
  // lengths learned change (a pass over what's up next).
  void refreshSummary();
  bool noCard() const;

  NavModel::PageRef* ref_ = nullptr;
  // The queue when the page was left: a different one on return (replaced,
  // added to) makes the remembered open row and place meaningless.
  bool left_ = false;
  uint32_t leftContent_ = 0;
  int32_t showPos_ = -1;
  bool showAdded_ = false;  // what the Library added is highlighted (this visit)
  uint8_t jump_ = 0;
  BitSet selection_;
  bool selecting_ = false;
  enum class TouchOn : uint8_t { None, Header, List, Bar } touchOn_ = TouchOn::None;
  int headerPressed_ = 0;
  int barPressed_ = -1;
  Ask ask_ = Ask::None;
  uint32_t content_ = 0, position_ = 0;
  uint32_t headerSig_ = 0;
  // The summary and what it was made from.
  uint32_t sumContent_ = 0xFFFFFFFFu, sumPosition_ = 0xFFFFFFFFu, sumBook_ = 0xFFFFFFFFu;
  char summaryLong_[64] = "";   // "4 of 16 · 12 up next · 49 min"
  char summaryMid_[48] = "";    // "4 of 16 · 49 min" (the long one too wide)
  char summaryShort_[48] = "";  // "12 up next · 49 min" (no position)
  mutable char headerTitle_[32] = "";  // the header's texts (PSRAM, with the Ui)
  mutable char headerSub_[64] = "";
};

// ---- Dance ----
class DancePage : public Page {
public:
  explicit DancePage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Dance"; }
  bool hasHeader() const override { return false; }
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
  int host_ = -1;  // a computer drives the dancer (the USB visualizer): its texts at the bottom
};

// ---- Output (spec §6.6, mockups 19-21, with the review's grafts) ----
//
// The Output tab is a list (it scrolls: the cards, then the settings):
//   the Bluetooth card (two rows): the headphones in their state's colour
//       (OutputModel: not paired, off, connecting "try 2 of 3",
//       searching, pairing, connected "SBC 44.1 kHz, 175 ms", failed,
//       lost), and its buttons: Pair new / Connect / Cancel / Try again /
//       Disconnect, Forget (a second tap within 3 s: "Tap again" in red),
//       the volume chip (the headphones' volume sheet); connected, "..."
//       instead of Forget: a sheet with Disconnect, Pair new headphones
//       and Forget (red, then a dialog), so Forget isn't a slip away from
//       Disconnect and the volume; a tap on the card makes it the output
//       (connecting first: the audio stays where it is until they are up)
//   the speaker card: its volume chip (the speaker's sheet), "Here until
//       SPYDRONE connects" while they're on their way; a tap: the output
//       (pausing first, like the B hold)
//   line out: the 3.5 mm / RCA module's place (not fitted yet)
//   + Pair new headphones  >  the Pair screen
//   Haptics (on / off), Screen off after (15 s ... Never), Brightness
//   (Low ... Max), Turn off when idle (10 min ... Never), Touch
//   calibration >, About >
// Pair (mockup 21): "Searching", the audio devices found (their kind and a
// signal of 4 bars); a tap pairs (after a confirmation when it replaces
// the remembered headphones), and the card shows how it goes.
// About: the battery, storage, the library, the headphones, memory, the
// version, the licence and where the source is, and "Show the tips again"
// (the coach cards).
class OutputPage : public Page, public ListView::Source {
public:
  explicit OutputPage(Ui& ui) : Page(ui) {}
  const char* name() const override { return "Output"; }
  bool scrolls() const override { return true; }
  void enter(NavModel::PageRef& ref) override;
  void leave() override;
  void repaint() override;
  void repaintHeader() override;
  bool update(uint32_t nowMs, bool frameDue, bool wholeRows) override;
  bool animating() const override;
  void onEvent(const InputEvent& e) override;
  void onDialog(int button) override;
  void onSheet(int choice) override;
  void home() override;
  void screenOff() override;
  void tick(uint32_t nowMs) override;
  void describe(char* buf, size_t size) const override;
  // The Pair screen's search runs: from the page's entry or Search again
  // until its 2 minutes, the screen off, a device picked or the page
  // closed (leave() stops it). Ui::pairSearching().
  bool pairSearching() const { return kind_ == PageKind::Pair && search_.searching(); }

  // ListView::Source
  uint32_t rows() override;
  void drawRow(ListView::Row& r) override;
  uint16_t accent() override;
  ListView::Tap onTapAt(uint32_t row, int x, bool rightEdge) override;
  bool emptyState(EmptyState& e) override;

private:
  // What the dialog or sheet up is asking.
  enum class Ask : uint8_t { None, Pair, More, Forget, CpuRestart, Touch, RemoveCal };
  enum RootRow : uint8_t {
    BtTop,
    BtButtons,
    SpeakerRow,
    LineOut,
    PairNew,
    Haptics,
    ScreenOff,
    Brightness,
    IdleOff,
    CpuSpeed,
    BtPower,
    Calibrate,
    AboutRow,
    kRootRows
  };
  enum AboutItem : uint8_t {
    Battery,
    Storage,
    LibraryInfo,
    Headphones,
    PowerInfo,
    Memory,
    Version,
    LicenceInfo,
    SourceInfo,
    Tips,
    kAboutRows
  };

  void drawBtTop(ListView::Row& r);
  void drawBtButtons(ListView::Row& r);
  void drawSpeaker(ListView::Row& r);
  void drawSetting(ListView::Row& r, const icons::Icon& icon, const char* title, const char* sub, uint16_t ink,
                   bool chevron);
  // "Screen off after" and "Brightness": the title, its line, and the
  // value in a pill at the right; a tap takes the next choice.
  void drawScreenSetting(ListView::Row& r, bool brightness);
  // "Turn off when idle" (IdlePolicy's choices, saved), the same way.
  void drawIdleSetting(ListView::Row& r);
  // "CPU speed" and "Bluetooth power" (PowerChoices', saved), the same way.
  void drawPowerSetting(ListView::Row& r, bool bluetooth);
  // A tap on "CPU speed": the restart asked first, or saved at once.
  void onCpuSpeed();
  // A tap on "Touch calibration": its sheet (Calibrate, Test taps,
  // and Remove while a table is saved).
  void onTouch();
  static uint32_t screenSig(const AppState& s);
  void drawDevice(ListView::Row& r, const BtDevice& d);
  void drawPairStatus(ListView::Row& r);
  void drawAbout(ListView::Row& r);
  // The card's buttons: how many, and each one's x and width.
  int buttonBoxes(const BtCardView& v, int* x, int* w) const;
  void onCardButton(BtButton b);
  void onCard();
  void pick(int device);
  void forget();
  Header header() const;
  uint32_t btSig() const;
  uint32_t speakerSig() const;
  const char* btName() const;

  PageKind kind_ = PageKind::Output;
  NavModel::PageRef* ref_ = nullptr;
  bool touchInList_ = false;
  int headerPressed_ = 0;
  ConfirmTap forget_;
  uint32_t drawnBt_ = 0, drawnSpeaker_ = 0;
  bool drawnHaptics_ = false;
  bool drawnCalibrated_ = false;
  uint32_t drawnScreen_ = 0xFFFFFFFFu;  // the screen, idle and power settings drawn (screenSig())
  uint32_t nextSpinMs_ = 0;
  uint8_t spin_ = 0;  // the spinner's step (8 a turn)
  // Pair: the scan's list as last copied, and the device picked.
  BtScanList scan_;
  PairSearch search_;  // its scan stops by itself after 2 min
  bool searchEnded_ = false;  // ... just now (tick()): the status row to redraw
  uint32_t scanVersion_ = 0;
  uint32_t nextScanMs_ = 0;
  int picked_ = -1;
  Ask ask_ = Ask::None;
  uint16_t askCpuMhz_ = 0;  // Ask::CpuRestart: the speed it restarts at
  // About: refreshed on the way in and every few seconds.
  AboutInfo about_;
  uint32_t nextAboutMs_ = 0;
  mutable char headerSub_[48] = "";
};

}  // namespace ui
