#pragma once
#include <cstdint>

#include "FrameClock.h"
#include "InputEvent.h"
#include "NavModel.h"
#include "QueueModel.h"
#include "ScrollGovernor.h"
#include "TabBarModel.h"
#include "app/DanceMode.h"
#include "app/Library.h"
#include "ui/Input.h"
#include "ui/ListScroller.h"
#include "ui/ListView.h"
#include "ui/Overlays.h"
#include "ui/Page.h"
#include "ui/Pages.h"
#include "ui/TabBar.h"
#include "ui/UiHost.h"

// The UI: the tab bar design's framework (docs/ARCHITECTURE.md, "UI").
// It alone draws to the display (while no other screen has taken it: the
// touch calibration, a spike tool), from the Arduino loop task:
//
//   - Why the loop task and not a task of its own: the player, the queue,
//     the library index and the input layer all live on the loop task and
//     aren't thread-safe, so a UI task would need a lock around every one
//     of them, and a stack of its own in internal RAM (6-8 KB of the
//     ~48 KB left while playing). The loop already runs on core 1 below the
//     decoder (priority 1 vs 2), so the audio always goes first, and it
//     sleeps a little every pass. Frames are what needs care: the UI draws
//     an animation frame only on its 30 fps deadlines (FrameClock), and
//     otherwise only what changed, in small pieces each under its own bus
//     hold (ui/Gfx).
//
//   - Navigation (NavModel): five tabs, a stack of pages each; a tab tap
//     switches (the tab keeps its stack), a tap on the current tab takes
//     it to its root, "‹" pops. Each page's scroll position and expanded
//     row are kept in its stack entry.
//   - The tab bar (ui/TabBar) with its status: playing/progress, the up-next
//     badge, the output and its VOLUME, the battery. The Output tab's hit
//     area reaches the screen's edge.
//   - Overlays (ui/Overlays): the toast with Undo at the top of the content,
//     the volume HUD over the tab bar, a bottom sheet, a dialog.
//   - The lists (ui/ListView) on the LCD's hardware scroll: the scroller is
//     started when a list page comes up and stopped (its band cleared) when
//     a page without one does.
//
// Touches: a finger that lands on the tab bar is the bar's; on a dialog,
// sheet or the toast, theirs; else the page's. A sheet or dialog that opens
// while a finger is on the page ends that touch for the page (a Cancel), so
// the rest of it can't act under the overlay. A long press on something
// with no hold (only list rows have holds) ends as a tap when it lifts where
// it pressed: a slow tap still works, and gets no hold tick. Whatever acts
// on a tap plays the tap tick (tick()). The touch buttons never reach the
// UI (ButtonPolicy handles them the same everywhere); the UI only shows
// their HUD.
//
// It lives in PSRAM (main.cpp makes it with psramNew), with its sprites:
// ~0 internal RAM.
namespace ui {

class Ui : private OverlayOwner {
public:
  Ui(UiHost& host, Input& input, PlaybackController& player, QueueModel& queue, Library& library,
     DanceMode& dance);

  // Fonts, sprites (PSRAM). False: can't run (no memory).
  bool begin();
  // The boot screen is done: draws the tab bar and the first page.
  void start(uint32_t nowMs);
  bool started() const { return started_; }
  // Every loop pass (after the input and the player).
  void loop(uint32_t nowMs);
  // A glass event from the input layer.
  void onEvent(const InputEvent& e);
  // Another screen takes the display (it clears it itself): the UI stops
  // drawing and lets go of the hardware scroll. resume() draws it all again.
  void suspend();
  void resume();
  bool suspended() const { return suspended_; }
  // How long the loop may sleep (1-5 ms): less while a frame is due soon.
  uint32_t idleMs(uint32_t nowMs) const;

  // ---- events from the rest of the firmware ----
  void libraryChanged();       // the index was rebuilt (g0): the Library's ids are stale
  void volumeKeys();           // the headphones' volume keys: the HUD
  void headphonesLost();       // dropped while playing on them: a dialog
  void toggleDance();          // console d

  // ---- console ----
  void printState() const;     // ui
  void command(const char* a); // ui<n>: tab n; uib: back

  // ---- for the pages ----
  const AppState& state() const { return state_; }
  UiHost& host() { return host_; }
  PlaybackController& player() { return player_; }
  QueueModel& queue() { return queue_; }
  Library& library() { return library_; }
  DanceMode& dance() { return dance_; }
  ListView& list() { return list_; }
  NavModel& nav() { return nav_; }
  const NavModel& nav() const { return nav_; }
  uint16_t accent() const;
  // A tap did something: the tap tick (if haptics are on).
  void tick() { input_.tapTick(); }
  void push(const NavModel::PageRef& p);
  void back();
  // The current tab back to its root page (the Library's root crumb).
  void toRoot();
  // Shows tab t (as a tab tap does, but never pops it).
  void showTab(NavModel::Tab t);
  // The Library tab showing an artist (and an album of it), one Back apart.
  void showInLibrary(uint32_t artist, uint32_t album);
  // `viewKey`: the queue entry (its key) of the first track an add put in
  // the queue: the toast also offers "View" (the Queue, scrolled to it).
  void toast(const char* text, bool undo, uint32_t viewKey = QueueModel::kNone);
  void openSheet(OverlayOwner* owner, const char* title, const char* const* rows, int n);
  void openDialog(OverlayOwner* owner, const char* title, const char* body, const char* const* buttons, int n);
  bool toastUp() const { return toast_.up(); }
  void drawHeader(const Header& h);

private:
  enum class TouchOn : uint8_t { None, Bar, Toast, Sheet, Dialog, Page };

  Page* pageFor(uint8_t kind);
  void showTop();
  void clearContent();
  void ensureScroller(bool wanted);
  void tapTab(NavModel::Tab t);
  // A glass event to whatever its touch landed on.
  void route(const InputEvent& e);
  // A modal opens: the page's touch in progress ends (a Cancel).
  void endPageTouch();
  // The toast's "View": the Queue, at that entry.
  void viewInQueue(uint32_t key);
  // Closes a sheet or dialog (telling its owner, if `notify`); true if one was up.
  bool closeModal(bool notify);
  void applyCover();
  void updateHud(uint32_t nowMs);
  tabbar::State tabState(uint32_t nowMs) const;
  void onDialog(int button) override;
  void trackMotion(uint32_t nowMs);

  UiHost& host_;
  Input& input_;
  PlaybackController& player_;
  QueueModel& queue_;
  Library& library_;
  DanceMode& dance_;

  NavModel nav_;
  ListScroller vscroll_;
  ListView list_;
  TabBar tabBar_;
  Toast toast_;
  Hud hud_;
  Sheet sheet_;
  Dialog dialog_;
  OverlayOwner* sheetOwner_ = nullptr;
  OverlayOwner* dialogOwner_ = nullptr;
  bool lostDialog_ = false;  // the dialog up is headphonesLost()'s

  NowPlayingPage nowPlaying_;
  LibraryPage libraryPage_;
  QueuePage queuePage_;
  DancePage dancePage_;
  OutputPage outputPage_;
  Page* page_ = nullptr;

  AppState state_;
  FrameClock clock_{33};
  ScrollGovernor governor_;
  ScrollGovernor::Budget budget_;
  bool started_ = false;
  bool suspended_ = false;
  TouchOn touch_ = TouchOn::None;
  // The touch's long press did nothing (no hold there): its lift, where it
  // pressed, is a tap at hold_.
  bool holdUnused_ = false;
  InputEvent hold_;
  uint32_t viewKey_ = QueueModel::kNone;  // the toast's "View"
  NavModel::Tab beforeDance_ = NavModel::Tab::NowPlaying;
  uint32_t hudSeq_ = 0;
  bool volumeHudDue_ = false;
  bool hudFollows_ = false;  // the HUD tracks the Bluetooth volume (the headphones' keys)
  uint32_t lastContent_ = 0;
  uint32_t lastUpNext_ = 0;
  uint32_t badgeUntilMs_ = 0;
  uint32_t nowMs_ = 0;
  // Frames, for 'ui'.
  uint32_t frames_ = 0;
  uint32_t framesWindowStart_ = 0;
  uint32_t framesInWindow_ = 0;
  float fps_ = 0;
  // The list's current motion, for its "[ui] scroll:" line.
  struct Motion {
    bool on = false;
    uint32_t startMs = 0;
    uint32_t frames = 0;
    uint64_t sumUs = 0;
    uint32_t maxUs = 0;
    uint32_t ringMin = 0;
    uint32_t underruns = 0;
  } motion_;
};

}  // namespace ui
