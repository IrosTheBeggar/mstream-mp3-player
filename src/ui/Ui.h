// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "FrameClock.h"
#include "HostStatus.h"
#include "InputEvent.h"
#include "NavModel.h"
#include "QueueModel.h"
#include "QueueView.h"
#include "ScrollGovernor.h"
#include "TabBarModel.h"
#include "UiStrings.h"
#include "app/DanceMode.h"
#include "app/Library.h"
#include "ui/Input.h"
#include "ui/ListScroller.h"
#include "ui/ListView.h"
#include "ui/Overlays.h"
#include "ui/Page.h"
#include "ui/Pages.h"
#include "ui/TabBar.h"
#include "ui/Thumbs.h"
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
//     the volume HUD over the tab bar, a bottom sheet, the volume sheet (per
//     output), the A-Z jump grid, a dialog, the first-boot coach cards.
//   - The language (docs/I18N.md, phase 3): every text is uistr's, in the
//     language app/UiLang read at boot. Output > Language changes it at
//     once (chooseLanguage()): everything is drawn again, and what held a
//     text in the old one (a sheet, a dialog, the toast, the HUD) goes. A
//     fresh unit (no language saved, the tips never seen) starts on that
//     page as its picker, before the touch check and the tips.
//   - States: a track that can't be played is a note (an amber toast) and
//     a "!" on its Queue row; the headphones lost while playing are a
//     dialog that follows their reconnecting; the pages have their empty
//     and no-card states (ui/EmptyState).
//   - Album covers (ui/Thumbs): thumbnails made by the card worker below
//     the loop (app/CardWorker, handed by app/CardTasks), never started
//     while a list moves; a page redraws the row (or the cover) that shows
//     one when it arrives.
//   - The Library's status line (docs/METADATA.md 3.3.6): while the card
//     worker checks the card, reads tags or the library updates, a Small
//     line across the list's width at the bottom of a Library page (y
//     220-239; the list's band ends above it), redrawn at most twice a
//     second when its text changes.
//   - The lists (ui/ListView) on the LCD's hardware scroll: the scroller is
//     started when a list page comes up and stopped (its band cleared) when
//     a page without one does.
//
// Touches: a finger that lands on the tab bar is the bar's; on a dialog,
// sheet or the toast, theirs; else the page's. A swipe up from the button
// strip (InputEvent::fromStrip: a DragStart with no Down) is the page's, to
// scroll its list, unless a modal is up (then nobody's). A sheet or dialog that opens
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

class Ui : private OverlayOwner, private ListView::RailHost {
public:
  Ui(UiHost& host, Input& input, PlaybackController& player, QueueModel& queue, Library& library,
     DanceMode& dance);

  // Fonts, sprites (PSRAM), and whether this is a fresh unit's first boot
  // (NVS: no language saved, the tips not seen). False: can't run (no
  // memory).
  bool begin();
  // The boot screen is done: draws the tab bar and the first page (the
  // tips over it if they haven't been seen); on a fresh unit, the language
  // picker first.
  void start(uint32_t nowMs);
  // A fresh unit's language picker is due or up: main.cpp's first-boot
  // touch check waits for it (then speaks the language picked), and the
  // tips follow both.
  bool languageFirst() const { return pickerDue_; }
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
  // How long the loop may sleep (1-5 ms): less while a frame is due soon;
  // 20 ms while the screen is off.
  uint32_t idleMs(uint32_t nowMs) const;
  // The screen went off (ScreenPower): nothing is drawn (gfx drops every
  // push), but the logic goes on: the snapshot, dialogs, toasts, their
  // timers. Any fling stops, no cover job starts, the dancer stops, the
  // page stops what costs power (the Pair scan). setDark(false), on the
  // wake, before the panel's sleep-out: everything drawn again (the tab
  // bar, the page, what is over it), the list's hardware scroll sent again.
  void setDark(bool on);
  bool dark() const { return dark_; }
  // The Pair screen's search runs (its 2 minutes at most; OutputPage):
  // main.cpp holds a lit screen lit for it (ScreenControl::Hold), so the
  // listener's taps on the list act instead of only waking a dim screen,
  // and the screen doesn't go off and stop the search.
  bool pairSearching() const { return started_ && !suspended_ && outputPage_.pairSearching(); }

  // ---- events from the rest of the firmware ----
  void libraryChanged();       // the index was rebuilt (g0): the Library's ids are stale
  // The library update's fence went up (docs/METADATA.md 3.4.2, N12): the
  // Library back to its root, the sheets and the jump grid that name its
  // ids closed, the list on screen read again (the "Updating" line). Down,
  // libraryChanged() follows.
  void libraryUpdating();
  // ---- for the card worker (app/CardTasks, ScanScheduler's inputs) ----
  // A list moves (the page's animating(): a drag, a fling, a snap), or a
  // finger is on a list's A-Z rail (ListView::railTouch(): a scrub, whose
  // every position is a band drawn whole, or resting on it before one).
  // None while its page draws no frame: the screen dark, identify()'s
  // banner, a modal over it (loop() draws the page under none of them). The page
  // then owes its list's frame to the wake, or to the modal's end (a
  // reload in the dark leaves the list "animating" until then, and so does
  // a fling a dialog that opened by itself caught: the headphones lost, a
  // play that failed), and on 2026-10-14 that held the library update's
  // save 260 s with the screen off. (ScanScheduler::listHolds() holds
  // nothing for one in the dark either.)
  bool listMoving() const {
    return started_ && !suspended_ && !dark_ && !identifying_ && !modalUp() && page_ &&
           (page_->animating() || (railHold_ && list_.attached() && list_.railTouch()));
  }
  // A cover may be made now: the UI is up and lit.
  bool coversAllowed() const { return started_ && !suspended_ && !dark_ && !identifying_; }
  // The Library tab's open album, artist or folder: its tracks (the scan's
  // "shown" source, 3.3.3); none when another page is up.
  LibraryIndex::Span shownTracks() const;
  void volumeKeys();           // the headphones' volume keys: the HUD
  void headphonesLost();       // dropped while playing on them: a dialog (and a long buzz)
  // The headphones the listener asked for are connected and the audio
  // moved to them (seconds after the tap), or a play that waited for them
  // started on them: a toast says where, two ticks.
  void headphonesConnected();
  void toggleDance();          // console d
  // The USB visualizer started (app/UsbViz): the Dance tab, whatever was
  // over the page closed (a sheet, a dialog, a toast, the HUD, the first-boot
  // tips: shown again at the next boot). False: the UI isn't up (not started,
  // or another screen has the display).
  bool showDance();
  // A computer asked which board this is (@identify, docs/HOST-STATUS.md):
  // "This one" over its label (its name for the port: COM5) on the whole
  // screen, in a frame of the Output tab's blue, for kIdentifyMs or until
  // a touch on the glass (which does nothing else), with one buzz
  // (Input::identifyBuzz(): not with haptics off). Nothing else changes:
  // the player plays on, the page and whatever is over it carry on under
  // it with nothing drawn (as while the screen is dark: gfx's pushes go
  // nowhere, the list's frames wait, the dancer stops), and all of it is
  // drawn again when it goes (redrawAll(), as on a wake). Again while it is
  // up: the new label, kIdentifyMs from now. False: the UI isn't up and lit
  // (not started, suspended, dark: main.cpp wakes the screen first).
  static constexpr uint32_t kIdentifyMs = 5000;
  bool identify(const char* label, uint32_t nowMs);
  bool identifying() const { return identifying_; }
  // The sleep timer's fade runs and the listener touched the device (or
  // the fade just began): the toast "Sleep timer: fading" with +10 min and
  // Turn off, the only controls that act on the timer. It goes when the
  // fade does.
  void sleepFading();
  // The Library browses `index` instead of the card's (console uil<n>: a
  // synthetic library, to see the lists at scale; nullptr: the card's). Its
  // pages start over; its actions are refused (its ids aren't the player's).
  void browse(LibraryIndex* index);

  // ---- console ----
  void printState() const;     // ui
  // ui<n>: tab n; uib back; uic the coach cards; uiL<tag> the language
  // (uiL the list, uiL- forget it and the tips); uiP the first boot's picker.
  void command(const char* a);

  // ---- the language ----
  // The UI's language from now on (the Language page's tap, the console's
  // uiL<tag>): saved (app/UiLang), and everything drawn again in it; a
  // sheet, dialog or toast up (their texts are the old language's) goes.
  // On the first boot's picker it goes on: Now Playing, then the touch
  // check (main.cpp) and the tips. A language whose catalogue says so
  // shows its machine translation's note (language.machine_note).
  void chooseLanguage(uistr::Lang l);

  // ---- for the pages ----
  const AppState& state() const { return state_; }
  UiHost& host() { return host_; }
  PlaybackController& player() { return player_; }
  QueueModel& queue() { return queue_; }
  Library& library() { return library_; }
  DanceMode& dance() { return dance_; }
  Input& input() { return input_; }
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
  // The Library tab showing `pages` above its root on segment `seg` (Now
  // Playing's navigation menu: Go to artist / album / folder); the stack
  // it had is replaced.
  void showLibrary(LibrarySegment seg, const NavModel::PageRef* pages, int n);
  // What the Library shows (the card's index, or a synthetic one).
  LibraryIndex* browseIndex() { return browse_ ? browse_ : library_.index(); }
  bool browsingSynthetic() const { return browse_ != nullptr; }
  Thumbs& thumbs() { return thumbs_; }
  // A list page's repaint() after an overlay went: the band from the rows
  // as rendered (ListView::redraw()), unless a cover arrived under the
  // overlay (its row is rendered again: ListView::invalidate()).
  void repaintList();
  // `viewKey`: the queue entry (its key) of the first track an add put in
  // the queue: the toast also offers "View" (the Queue, scrolled to it),
  // and the next visit to the Queue shows and highlights what was added.
  void toast(const char* text, bool undo, uint32_t viewKey = QueueModel::kNone);
  // An add the full queue refused (no played track in it to push out:
  // docs/QUEUE-MODES.md 15.8): its note, keeping the Undo and View of the
  // toast it covers (nothing changed: the last edit's undo stays, and its
  // button with it: queueview::keptByRefusal()).
  void refuse(const char* text);
  // A note in amber (a track skipped, no card yet).
  void warn(const char* text);
  // A plain toast that stays `ms` (the next boot's "Turned off after 20
  // minutes idle").
  void note(const char* text, uint32_t ms);
  // `primary`: the row that is the main choice (-1 none); `danger`: the
  // row in red (-1 none). A touch that starts within Sheet::kSettleMs of
  // its opening goes nowhere.
  void openSheet(OverlayOwner* owner, const char* title, const char* const* rows, int n,
                 const char* const* details = nullptr, int primary = -1, int danger = -1);
  // What a row of the sheet just opened shows as its detail, and follows
  // while it is up: the sleep timer's state ("23 min", "Fading", "Off"),
  // shuffle ("On", "Off"), repeat ("Off", "All", "One") or Auto DJ ("On",
  // "Off", "Waiting", "Paused"), from the snapshot each pass (a change
  // from the console shows too).
  enum class SheetFollow : uint8_t { None, Sleep, Shuffle, Repeat, AutoDj };
  void sheetFollows(int row, SheetFollow what);
  // A tap on row `row` of the sheet just opened doesn't close it: the
  // owner's onSheet(row) acts, the row's highlight goes and its followed
  // detail shows the new state (Now Playing's Shuffle and Repeat).
  void sheetStays(int row);
  // An output's volume, as a sheet: -1 the active one (Now Playing's
  // volume button), 0 the speaker's, 1 the headphones' (the Output cards).
  void openVolume(int output = -1);
  // The sleep timer's choices (Now Playing's "..." > Sleep timer).
  void openSleepSheet();
  // `danger`: the primary button is red (Clear queue).
  void openDialog(OverlayOwner* owner, const char* title, const char* body, const char* const* buttons, int n,
                  bool danger = false);
  // The first-boot tips (again, from About).
  void showCoach();
  // The list band's height (the Queue's selection mode keeps y 198-239 for
  // its bar): the LCD's scroll band follows.
  void setListBand(int height);
  // What Library adds put in the queue since the Queue was last shown
  // (it scrolls to them and highlights them), and the entries whose track
  // failed (their "!").
  queueview::AddedMark& added() { return added_; }
  const queueview::FailedKeys& failedKeys() const { return failedKeys_; }
  // Shuffle all: the whole library, shuffled, playing (the empty states).
  void shuffleAll();
  // The empty states' Auto DJ button (D4; in Shuffle all's place while the
  // card has a usable index): a random track the index knows plays as a
  // new queue, Auto DJ on (UiHost::autoDjStart()), and the toast says
  // which (with Undo, as Shuffle all's: autoDjToast()); none found, or
  // the index gone since the button was drawn, a note.
  void autoDjStart();
  // The toast after a Play that also switched Auto DJ on (the button,
  // "Auto DJ from here"), with Undo: the queue as it was and, when this
  // tap switched Auto DJ on (`switchedOn`), Auto DJ off again: the tap
  // undone whole.
  void autoDjToast(const char* text, bool switchedOn);
  // What Auto DJ's loop step had to say (autodjview::News bits, main.cpp
  // after its step): one toast (autodjview::newsToast(): a note for the
  // failure pause). While a toast with Undo is up, it waits until that
  // goes (the failure pause doesn't), and an Undo tapped drops it.
  void autoDjNews(uint8_t news);
  // The Queue's entries that are Auto DJ's (its DJ marks): never marked as
  // a Library add, nor scrolled to (queueview::AddedMark).
  queueview::QuietKeys djKeys() const;
  // The console's qu undid the queue's last edit: an Undo toast still up
  // goes at the next pass, and its line says so (UndoWatch).
  void queueUndone() { undoWatch_.undone(); }
  // Auto DJ's pick dropped the queue's undo (QueueModel::appendQuiet()):
  // the same, its line saying so.
  void queueUndoDropped() { undoWatch_.dropped(); }
  // Try again on a no-card page: a card that mounts restarts the player
  // (a toast first); else the note, the kind's (uitext::cardMessage(kind)
  // .still: "Still no card: ...", "Still exFAT: ...", "Still GPT: ..."),
  // and the page drawn again if that changed.
  void retryCard();
  bool toastUp() const { return toast_.up(); }
  void drawHeader(const Header& h);

private:
  enum class TouchOn : uint8_t { None, Bar, Toast, Sheet, Volume, Jump, Dialog, Coach, Page, Sleep };

  Page* pageFor(uint8_t kind);
  void showTop();
  void clearContent();
  // The list band's hardware scroll: begun for a page that scrolls, kept
  // on (the map invalidated) under one that doesn't; ended, the band
  // cleared first, when a screen of its own takes the display.
  void ensureScroller(bool wanted);
  void endScroller();
  void tapTab(NavModel::Tab t);
  // A glass event to whatever its touch landed on.
  void route(const InputEvent& e);
  // A modal opens: the page's touch in progress ends (a Cancel).
  void endPageTouch();
  // The toast's "View": the Queue, at that entry.
  void viewInQueue(uint32_t key);
  // Closes a sheet or dialog (telling its owner, if `notify`); true if one was up.
  bool closeModal(bool notify);
  // A modal went: what it covered drawn again (the coach cards if they're
  // up, else the page), and the toast over it.
  void repaintUnder();
  // The toast went (it was down to `oldBottom`; another in its place is
  // nothing to do): what it covered drawn again.
  void uncover(int oldBottom);
  void applyCover();
  void updateHud(uint32_t nowMs);
  tabbar::State tabState(uint32_t nowMs) const;
  // Where each tab's label goes in the UI's language (tabbar::labelFit()),
  // measured once a language (not every pass).
  tabbar::LabelFit tabLabelFit(int tab) const;
  void onDialog(int button) override;
  void trackMotion(uint32_t nowMs);
  bool modalUp() const {
    return sheet_.up() || dialog_.up() || volumeSheet_.up() || jumpGrid_.up() || coach_.up() || sleepSheet_.up();
  }
  // The sleep timer's sheet: its state as the snapshot has it.
  void refreshSleepSheet();
  // The sheet's followed rows, each from the snapshot (`pressed`: a
  // staying row just tapped, drawn again if its detail didn't change).
  void followSheet(int pressed = -1);
  SheetFollow sheetFollow_[Sheet::kMaxRows] = {};
  uint8_t sheetStays_ = 0;      // a bit per row
  uint32_t sheetOpenedMs_ = 0;  // (the settle: Sheet::kSettleMs; the Sleep timer sheet's too)
  // The shuffle mode the last pass saw, and a qu since: when the queue's
  // undo goes (a toggle drops it, qu uses it), an Undo toast up goes too.
  queueview::UndoWatch undoWatch_;
  void sleepTitle(char* buf, size_t size) const;
  // The Library's status line: the band shortened under it while it shows.
  void updateStatus(uint32_t nowMs);
  static constexpr int kStatusH = 20;
  bool statusBand_ = false;   // the list's band ends above the line
  uint32_t statusHash_ = 0;   // the text drawn (0: none, or to be drawn again)
  uint32_t statusAtMs_ = 0;   // when it was drawn
  void coachDone();
  // The Output tab at its Language page as the first boot's picker (the
  // nav only: showTop() draws it).
  void pickerNav();
  bool pickerOnTop() const;
  // Every text drawn again in the language just set (chooseLanguage(), the
  // console's uiL-).
  void languageChanged();
  void languageCommand(const char* a);
  void updateLostDialog();
  // A play that waited for the headphones failed (PlayGate): "Couldn't
  // reach SPYDRONE" with Play on speaker and Try again; it closes itself
  // when they connect after all, or the speaker becomes the output.
  void playFailed();
  void updatePlayFailed();
  void noteFailures();
  // The A-Z rail's tap (ListView::RailHost): the jump grid, and its taps.
  void onRailTap() override;
  void openJumpLetters();
  void onJumpCell(int cell);
  void resetLibraryTab();

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
  mutable tabbar::LabelFit tabFits_[tabbar::kTabs] = {};
  mutable int tabFitsLang_ = -1;  // the language tabFits_ were measured in (-1: none yet)
  Toast toast_;
  Hud hud_;
  Sheet sheet_;
  SleepSheet sleepSheet_;
  VolumeSheet volumeSheet_;
  JumpGrid jumpGrid_;
  Dialog dialog_;
  Coach coach_;
  Thumbs thumbs_;
  LibraryIndex* browse_ = nullptr;  // uil<n>: a synthetic library in the Library tab
  int volumeAsked_ = -1;            // the volume sheet's last target (steps are relative)
  bool volumeSheetBt_ = false;      // the volume sheet's output
  // The jump grid: its list, level (1 letters, 2 a letter's two-letter
  // starts) and each cell's first row.
  ListView::Source* jumpSource_ = nullptr;
  int jumpLevel_ = 1;
  int32_t jumpFirst_[jump::kCells] = {};
  int32_t jumpEnd_[jump::kCells] = {};
  int32_t jumpSecond_[jump::kCells] = {};
  OverlayOwner* sheetOwner_ = nullptr;
  OverlayOwner* dialogOwner_ = nullptr;
  bool lostDialog_ = false;  // the dialog up is headphonesLost()'s
  bool playFailedDialog_ = false;  // ... is playFailed()'s
  // Which of the Ui's own dialogs onDialog() answers (the flags above are
  // cleared as a dialog closes, before its owner hears the button).
  enum class OwnDialog : uint8_t { None, Lost, PlayFailed } ownDialog_ = OwnDialog::None;
  uint32_t gateFailuresSeen_ = 0;
  // The first boot: the tips seen (NVS "ui"/"coach"); a fresh unit's
  // language picker due or up; the tips (and the language's note) to show
  // once it is done, after the touch check main.cpp opens then.
  bool coachSeen_ = false;
  bool pickerDue_ = false;
  bool tipsAfterPicker_ = false;
  bool noteAfterPicker_ = false;
  queueview::AddedMark added_;
  queueview::FailedKeys failedKeys_;
  uint32_t failuresSeen_ = 0;

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
  bool dark_ = false;         // the screen is off: nothing drawn
  // identify()'s banner is up, until identifyUntilMs_.
  bool identifying_ = false;
  uint32_t identifyUntilMs_ = 0;
  char identifyLabel_[hoststatus::kMaxLabel + 1] = "";
  void drawIdentify();
  void endIdentify();
  bool danceWasOn_ = false;   // the dancer was up when it went dark
  // The screen woke: draw everything again.
  void redrawAll();
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
  uint32_t lastDjAdds_ = 0;  // Auto DJ's appends the last pass saw (no badge flash for them)
  uint8_t pendingNews_ = 0;  // Auto DJ's news held while an Undo toast is up (autoDjNews())
  bool undoSwitchesDjOff_ = false;  // the toast's Undo also switches Auto DJ off (autoDjToast())
  uint32_t badgeUntilMs_ = 0;
  uint32_t nowMs_ = 0;
  // Frames, for 'ui'.
  uint32_t frames_ = 0;
  uint32_t framesWindowStart_ = 0;
  uint32_t framesInWindow_ = 0;
  float fps_ = 0;
  // What a loop pass costs (console ui, then counted again): the longest
  // pass and event, each part's longest, the mean pass; a pass or an
  // event over kSlowPassUs is logged as it happens ("[ui] slow pass").
  struct PassCost {
    uint32_t passes = 0;
    uint64_t passSumUs = 0;
    uint32_t passMaxUs = 0;
    uint32_t snapshotMaxUs = 0;
    uint32_t tabMaxUs = 0;
    uint32_t pageMaxUs = 0;    // the page's update(), a frame or not
    uint32_t statusMaxUs = 0;
    uint32_t thumbsMaxUs = 0;
    uint32_t eventMaxUs = 0;   // onEvent(), the glass
    InputEvent::Type eventMaxType = InputEvent::Type::None;
  };
  mutable PassCost cost_;  // (printState() is const: it prints and counts again)
  bool thumbsStale_ = false;  // a cover arrived under a modal: repaintList() renders the rows again
  // A finger on the A-Z rail holds the card worker (listMoving()); off with
  // the scrub's pacing (console uir0: as before 0.12.0, to compare by hand).
  bool railHold_ = true;
  // The list's current motion, for its "[ui] scroll:" line (a page without
  // a list, Now Playing's seek bar: "[ui] scrub:"; a finger on the A-Z
  // rail: "[ui] rail scrub:", its band frames and its rail-only frames
  // apart).
  struct Motion {
    bool on = false;
    bool list = false;  // a list moved (else a page's own animation: a scrub)
    bool rail = false;  // a finger was on the list's A-Z rail
    uint32_t startMs = 0;
    uint32_t frames = 0;    // band frames (every frame of a scroll)
    uint64_t sumUs = 0;
    uint32_t maxUs = 0;
    uint32_t slow = 0;      // frames over kSlowFrameUs
    uint32_t railFrames = 0;  // a scrub's frames of the rail alone
    uint64_t railSumUs = 0;
    uint32_t railMaxUs = 0;
    uint32_t ringMin = 0;
    uint32_t underruns = 0;
  } motion_;
};

}  // namespace ui
