// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/Ui.h"

#include <M5Unified.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "RateConverter.h"
#include "SleepTimer.h"
#include "TextFit.h"
#include "TextFold.h"
#include "UiText.h"
#include "app/Psram.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace ui {

namespace {

NavModel::PageRef ref(PageKind kind, uint32_t id = NavModel::kNone) {
  NavModel::PageRef p;
  p.kind = static_cast<uint8_t>(kind);
  p.id = id;
  return p;
}

}  // namespace

const char* pageKindName(uint8_t kind) {
  switch (static_cast<PageKind>(kind)) {
    case PageKind::NowPlaying: return "NowPlaying";
    case PageKind::Library: return "Library";
    case PageKind::Artist: return "Artist";
    case PageKind::Album: return "Album";
    case PageKind::ArtistTracks: return "ArtistTracks";
    case PageKind::Folder: return "Folder";
    case PageKind::Queue: return "Queue";
    case PageKind::Dance: return "Dance";
    case PageKind::Output: return "Output";
    case PageKind::Pair: return "Pair";
    case PageKind::About: return "About";
    default: return "-";
  }
}

void Page::describe(char* buf, size_t size) const { snprintf(buf, size, "%s", name()); }

int Header::hit(const InputEvent& e) const {
  if (e.y < kHeaderY || e.y >= kHeaderY + kHeaderH) return 0;
  if ((back || cross) && e.x < 56) return 1;
  if (right && e.inRightEdgeZone(240)) return 2;
  return 3;
}

Ui::Ui(UiHost& host, Input& input, PlaybackController& player, QueueModel& queue, Library& library, DanceMode& dance)
    : host_(host),
      input_(input),
      player_(player),
      queue_(queue),
      library_(library),
      dance_(dance),
      nowPlaying_(*this),
      libraryPage_(*this),
      queuePage_(*this),
      dancePage_(*this),
      outputPage_(*this),
      thumbs_(library) {
  // The list's safety net (UI spike): fewer frames, or whole rows, while
  // the decoder's buffer runs low.
  ScrollGovernor::Config g;
  g.normalFrameMs = 33;  // 30 fps
  g.reducedFrameMs = 66;
  governor_.setConfig(g);
}

bool Ui::begin() {
  Fonts& f = Fonts::instance();
  if (!f.load()) Serial.println("[ui] VLW fonts failed to load: the built-in bitmap fonts instead");
  if (!gfx::begin() || !overlaysBegin() || !list_.begin(vscroll_, input_)) {
    Serial.println("[ui] no PSRAM for the UI's sprites");
    return false;
  }
  list_.setRailHost(this);
  // Covers: without PSRAM for them, every one is the placeholder.
  if (!thumbs_.begin()) Serial.println("[ui] no PSRAM for the cover thumbnails: placeholders");
  nav_.setRoot(NavModel::Tab::NowPlaying, ref(PageKind::NowPlaying));
  nav_.setRoot(NavModel::Tab::Library, ref(PageKind::Library, static_cast<uint32_t>(LibrarySegment::Artists)));
  nav_.setRoot(NavModel::Tab::Queue, ref(PageKind::Queue));
  nav_.setRoot(NavModel::Tab::Dance, ref(PageKind::Dance));
  nav_.setRoot(NavModel::Tab::Output, ref(PageKind::Output));
  return true;
}

void Ui::start(uint32_t nowMs) {
  nowMs_ = nowMs;
  host_.snapshot(state_);
  hudSeq_ = state_.feedback.seq;
  gateFailuresSeen_ = state_.gateFailures;
  lastContent_ = state_.contentVersion;
  lastUpNext_ = state_.upNext;
  failuresSeen_ = player_.lastFailure().count;
  started_ = true;
  gfx::fill(0, 0, kW, kH, col::BG, true);
  tabBar_.invalidate();
  tabBar_.update(tabState(nowMs));
  showTop();
  Serial.println("[ui] up: tab bar, Now Playing (console ui: the navigation state)");
  // The first-boot tips, once (NVS "ui"/"coach").
  Preferences prefs;
  bool seen = false;
  if (prefs.begin("ui", true)) {
    seen = prefs.getBool("coach", false);
    prefs.end();
  }
  if (!seen) showCoach();
}

void Ui::showCoach() {
  if (!started_ || suspended_) return;
  endPageTouch();
  closeModal(true);
  coach_.open(0, accent());
  applyCover();
  Serial.println("[ui] coach cards: the touch buttons, then the tabs");
}

// The tips were seen (the last one's "Got it", or a tab tap): not again.
void Ui::coachDone() {
  coach_.close();
  applyCover();
  Preferences prefs;
  if (prefs.begin("ui", false)) {
    prefs.putBool("coach", true);
    prefs.end();
  }
  Serial.println("[ui] coach cards: seen (Output > About shows them again)");
}

Page* Ui::pageFor(uint8_t kind) {
  switch (static_cast<PageKind>(kind)) {
    case PageKind::Library:
    case PageKind::Artist:
    case PageKind::Album:
    case PageKind::ArtistTracks:
    case PageKind::Folder: return &libraryPage_;
    case PageKind::Queue: return &queuePage_;
    case PageKind::Dance: return &dancePage_;
    case PageKind::Output:
    case PageKind::Pair:
    case PageKind::About: return &outputPage_;
    default: return &nowPlaying_;
  }
}

uint16_t Ui::accent() const { return accent::of(nav_.tab()); }

// ---- pages ----

void Ui::ensureScroller(bool wanted) {
  if (wanted && !vscroll_.active()) {
    // The tab bar and a page header are the fixed top area; the band scrolls.
    if (!vscroll_.begin(ListView::kTop, ListView::kHeight, &gfx::holds(), ListView::kMaxStep)) {
      Serial.println("[ui] the hardware scroll is unavailable: lists won't draw");
    }
  } else if (!wanted && vscroll_.active()) {
    // The band's GRAM is in the rotated order: cleared first (all of it,
    // whatever the rotation), then the address back to the identity.
    gfx::fill(0, ListView::kTop, kW, ListView::kHeight, col::BG, true);
    vscroll_.end();
  }
}

void Ui::setListBand(int height) {
  height = std::max(ListLayout::kPitch, std::min(ListView::kHeight, height));
  if (height == list_.height()) return;
  if (vscroll_.active()) {
    // The old band's GRAM is rotated: cleared first, then the new band
    // (and the fixed area under it) from the identity.
    gfx::fill(0, ListView::kTop, kW, list_.height(), col::BG, true);
    vscroll_.end();
    if (!vscroll_.begin(ListView::kTop, height, &gfx::holds(), ListView::kMaxStep)) {
      Serial.println("[ui] the hardware scroll is unavailable: lists won't draw");
    }
  }
  list_.setHeight(height);
}

void Ui::retryCard() {
  if (host_.retryCard()) {
    toast("Card found: starting again", false);
    return;
  }
  // What is in may have changed (a card that isn't FAT32 taken out, or
  // put in): the snapshot now, not at the next pass, for the note.
  const bool was = state_.cardNotFat32;
  host_.snapshot(state_);
  if (state_.cardNotFat32 != was && page_ && !modalUp()) page_->repaint();
  warn(state_.cardNotFat32 ? uitext::kStillNotFat32 : uitext::kStillNoCard);
}

void Ui::shuffleAll() {
  const LibraryIndex* index = library_.index();
  if (browse_ || !index || !index->ready() || index->trackCount() == 0) {
    warn("No music on the card to shuffle");
    return;
  }
  // Shuffle on (the mode, not a one-shot: the menu then says what plays,
  // and Off brings the library's own order back, A-Z, from the track that
  // plays), then the whole library from a random track (QueueModel copies
  // the ids and shuffles them: docs/QUEUE-MODES.md section 2.5).
  const LibraryIndex::Span all = index->allTracks();
  if (!player_.shuffle()) host_.setShuffle(true);
  const bool ok = player_.playNow(all.ids, all.count, PlaybackController::kAnyStart);
  added_.clear();
  Serial.printf("[ui] shuffle all: %lu tracks (shuffle on)%s\n", (unsigned long)all.count, ok ? "" : ": NO MEMORY");
  char text[48];
  snprintf(text, sizeof(text), "Shuffling %lu tracks", (unsigned long)all.count);
  toast(ok ? text : "Not enough memory for that", ok);
}

void Ui::showTop() {
  Page* next = pageFor(nav_.top().kind);
  if (page_) page_->leave();
  // A page that shortened the list band (the Queue's selection mode) has
  // let it go in leave(); be sure.
  if (list_.height() != ListView::kHeight) setListBand(ListView::kHeight);
  ensureScroller(next->scrolls());
  page_ = next;
  applyCover();
  page_->enter(nav_.top());
  // The first frame at once, whatever the cadence: navigation should feel instant.
  if (page_->update(nowMs_, true, false)) clock_.drawn(nowMs_);
  if (!hud_.up()) tabBar_.update(tabState(nowMs_));
}

void Ui::push(const NavModel::PageRef& p) {
  closeModal(true);
  if (page_) page_->leave();  // saves its place into its entry before the push
  page_ = nullptr;
  nav_.push(p);
  showTop();
}

void Ui::back() {
  closeModal(true);
  if (nav_.depth() <= 1) return;
  if (page_) page_->leave();
  page_ = nullptr;
  nav_.back();
  showTop();
}

void Ui::toRoot() {
  closeModal(true);
  if (nav_.depth() <= 1) return;
  if (page_) page_->leave();
  page_ = nullptr;
  nav_.popToRoot(nav_.tab());
  showTop();
}

void Ui::showTab(NavModel::Tab t) {
  const bool closed = closeModal(true), same = nav_.tab() == t;
  if (same) {
    if (closed) repaintUnder();
    return;
  }
  if (t == NavModel::Tab::Dance) beforeDance_ = nav_.tab();
  if (page_) page_->leave();
  page_ = nullptr;
  nav_.select(t);
  showTop();
}

void Ui::showLibrary(LibrarySegment seg, const NavModel::PageRef* pages, int n) {
  closeModal(true);
  if (page_) page_->leave();
  page_ = nullptr;
  // The root on that segment (a segment it wasn't on opens at its top).
  NavModel::PageRef root = nav_.at(NavModel::Tab::Library, 0);
  if (root.id != static_cast<uint32_t>(seg)) {
    root.id = static_cast<uint32_t>(seg);
    root.scrollPx = -1;
    root.expanded = -1;
  }
  // Nothing above it (a file in /music itself): the root at what plays.
  if (n == 0) root.scrollPx = kShowPlaying;
  nav_.setRoot(NavModel::Tab::Library, root);
  nav_.replaceAboveRoot(NavModel::Tab::Library, pages, n);
  nav_.select(NavModel::Tab::Library);
  showTop();
}

void Ui::tapTab(NavModel::Tab t) {
  const bool coached = coach_.up();
  if (coached) coachDone();  // a tab tap ends the tips (seen), and switches
  const bool closed = closeModal(true) || coached;
  const NavModel::Tab was = nav_.tab();
  const NavModel::TabTap r = nav_.tapTab(t);
  // Every tab change in the log, next to the touches: what a finger did
  // and what it caused (the unexplained queue jump was touches unlogged).
  Serial.printf("[ui] tab: %s%s\n", NavModel::name(t),
                r == NavModel::TabTap::PoppedToRoot ? " (to its root)" : r == NavModel::TabTap::AtRoot ? " (home)" : "");
  if (r == NavModel::TabTap::AtRoot) {
    // The same page stays, and goes "home" (the Queue to the playing track).
    if (closed) repaintUnder();
    page_->home();
    return;
  }
  if (t == NavModel::Tab::Dance && was != t) beforeDance_ = was;
  if (page_) page_->leave();  // its place goes into its own entry (kept even if popped)
  page_ = nullptr;
  showTop();
}

void Ui::toggleDance() {
  if (!started_ || suspended_) return;
  showTab(nav_.tab() == NavModel::Tab::Dance ? beforeDance_ : NavModel::Tab::Dance);
}

bool Ui::showDance() {
  if (!started_ || suspended_) return false;
  bool uncovered = closeModal(true);
  if (coach_.up()) {
    coach_.close();  // shown again at the next boot (not marked seen)
    uncovered = true;
  }
  if (toast_.up()) {
    if (toast_.undo()) queue_.dropUndo();  // (its Undo goes with it)
    toast_.hide();
    uncovered = true;
  }
  if (hud_.up()) {
    hud_.hide();
    uncovered = true;
  }
  if (uncovered) applyCover();
  if (nav_.tab() != NavModel::Tab::Dance) {
    Serial.println("[ui] tab: Dance (the computer's visualizer)");
    showTab(NavModel::Tab::Dance);
  } else if (uncovered && page_) {
    page_->repaint();
  }
  return true;
}

// The Library tab back at its root, on the segment it was on (its pages'
// ids may mean nothing now).
void Ui::resetLibraryTab() {
  const bool shown = started_ && !suspended_;
  if (shown) closeModal(true);
  const bool onLibrary = shown && nav_.tab() == NavModel::Tab::Library;
  if (onLibrary && page_) {
    page_->leave();  // before its entries are replaced
    page_ = nullptr;
  }
  const uint32_t seg = nav_.at(NavModel::Tab::Library, 0).id;
  nav_.setRoot(NavModel::Tab::Library, ref(PageKind::Library, seg <= 2 ? seg : 0));
  if (!shown) return;
  if (onLibrary) {
    showTop();
  } else if (page_) {
    page_->repaint();  // names may have changed under the same ids
  }
}

void Ui::libraryChanged() {
  // Every index id changed: the covers made so far are other albums' now,
  // and the Library's pages start over at the root.
  thumbs_.libraryChanged();
  resetLibraryTab();
}

void Ui::browse(LibraryIndex* index) {
  browse_ = index && index->ready() ? index : nullptr;
  Serial.printf("[ui] the Library shows %s\n", browse_ ? "a SYNTHETIC library (look only; uil0: the card's)"
                                                       : "the card's library");
  resetLibraryTab();
}

// ---- overlays ----

void Ui::applyCover() {
  if (toast_.up()) {
    gfx::setCover(Toast::kY, toast_.bottom());
  } else {
    gfx::setCover(0, 0);
  }
  // The dancer is drawn by DanceMode, outside the pages: keep it off the overlays.
  const int top = toast_.bottom();
  const int bottom = jumpGrid_.up() || coach_.up() ? top
                     : sheet_.up()       ? sheet_.top()
                     : sleepSheet_.up()  ? SleepSheet::kY
                     : volumeSheet_.up() ? VolumeSheet::kY
                     : dialog_.up()      ? Dialog::kY
                                         : kH;
  dance_.view().setVisibleRows(top, bottom);
}

bool Ui::closeModal(bool notify) {
  bool closed = false;
  if (sheet_.up()) {
    sheet_.close();
    closed = true;
    if (notify && sheetOwner_) sheetOwner_->onSheet(-1);
  }
  if (dialog_.up()) {
    dialog_.close();
    closed = true;
    lostDialog_ = false;
    playFailedDialog_ = false;
    if (notify && dialogOwner_) dialogOwner_->onDialog(-1);
  }
  if (volumeSheet_.up()) {
    volumeSheet_.close();
    closed = true;
  }
  if (sleepSheet_.up()) {
    sleepSheet_.close();
    closed = true;
  }
  if (jumpGrid_.up()) {
    jumpGrid_.close();
    jumpSource_ = nullptr;
    closed = true;
  }
  if (closed) applyCover();
  return closed;
}

void Ui::repaintUnder() {
  applyCover();
  if (jumpGrid_.up()) {
    jumpGrid_.draw();
  } else if (coach_.up()) {
    coach_.draw();  // (it covers the header row too: not the page's header)
  } else if (page_) {
    page_->repaint();
  }
  if (toast_.up()) toast_.draw();  // a dialog or sheet may have been drawn over it
}

void Ui::uncover(int oldBottom) {
  applyCover();
  if (toast_.bottom() >= oldBottom) return;  // still up (another toast in its place)
  // The header row it covered: the jump grid's and the coach's own header
  // (they cover that row too), else the page's; and a dialog it cut into.
  if (jumpGrid_.up() || coach_.up()) {
    if (jumpGrid_.up()) jumpGrid_.draw();
    if (coach_.up()) coach_.draw();
  } else if (page_) {
    page_->repaintHeader();
  }
  // A page with no header (Now Playing, the Dance tab) draws all of itself
  // again for the header row (Page::repaintHeader()'s default): every sheet
  // over it is drawn again too, else it stays up (live) but unseen. Seen on
  // the device: +10 min on the fade toast over the Sleep timer sheet left
  // its pills invisible over the transport, and a tap on "..." hit Turn off.
  const bool wholePage = !jumpGrid_.up() && !coach_.up() && page_ && !page_->hasHeader();
  if (sheet_.up() && (wholePage || sheet_.top() < oldBottom)) sheet_.draw();  // a 4-row sheet would reach the header row
  if (wholePage && sleepSheet_.up()) sleepSheet_.draw();
  if (wholePage && volumeSheet_.up()) volumeSheet_.draw();
  if (dialog_.up()) dialog_.draw();
}

void Ui::toast(const char* text, bool undo, uint32_t viewKey) {
  viewKey_ = viewKey;
  // An add: the next visit to the Queue shows it (the review's graft).
  if (viewKey != QueueModel::kNone) added_.noteAdded(viewKey);
  // Not over the start-up screen or a screen of its own (the calibration):
  // Toast::show() draws at once.
  if (!started_ || suspended_) {
    Serial.printf("[ui] toast not shown (the UI isn't on screen): %s\n", text);
    return;
  }
  const int was = toast_.bottom();
  toast_.show(text, undo, viewKey != QueueModel::kNone, accent(), nowMs_);
  uncover(was);
  Serial.printf("[ui] toast: %s%s%s\n", text, undo ? " (Undo)" : "",
                viewKey != QueueModel::kNone ? " (View)" : "");
}

void Ui::warn(const char* text) {
  // (As toast(): the pocket guard's note for a B click while the
  // calibration is up drew over it.)
  if (!started_ || suspended_) {
    Serial.printf("[ui] note not shown (the UI isn't on screen): %s\n", text);
    return;
  }
  viewKey_ = QueueModel::kNone;
  const int was = toast_.bottom();
  toast_.show(text, false, false, col::AMBER, nowMs_);
  uncover(was);
  Serial.printf("[ui] note: %s\n", text);
}

void Ui::note(const char* text, uint32_t ms) {
  if (!started_ || suspended_) return;
  viewKey_ = QueueModel::kNone;
  const int was = toast_.bottom();
  if (toast_.undo()) queue_.dropUndo();
  toast_.show(text, false, false, accent(), nowMs_, ms);
  uncover(was);
  Serial.printf("[ui] toast: %s\n", text);
}

void Ui::viewInQueue(uint32_t key) {
  const uint32_t pos = queue_.positionOf(key);
  if (pos == QueueModel::kNone) return;  // undone, or removed since
  queuePage_.showOnEnter(pos);
  if (nav_.tab() == NavModel::Tab::Queue) {
    // (Toasts with View come from the Library, but be safe.)
    if (page_) {
      page_->leave();
      page_->enter(nav_.top());
    }
    return;
  }
  showTab(NavModel::Tab::Queue);
}

void Ui::endPageTouch() {
  if (touch_ != TouchOn::Page) return;
  touch_ = TouchOn::None;
  holdUnused_ = false;
  if (!page_) return;
  InputEvent c;
  c.type = InputEvent::Type::Cancel;
  c.ms = nowMs_;
  page_->onEvent(c);  // its press highlight goes now, before the overlay is drawn
}

void Ui::openSheet(OverlayOwner* owner, const char* title, const char* const* rows, int n,
                   const char* const* details, int primary, int danger) {
  endPageTouch();
  closeModal(true);
  sheetOwner_ = owner;
  for (SheetFollow& f : sheetFollow_) f = SheetFollow::None;
  sheetStays_ = 0;
  sheetOpenedMs_ = millis();
  sheet_.open(title, rows, n, accent(), details, primary, danger);
  applyCover();
  // A 4-row sheet would reach the header row: a toast up stays on top.
  if (toast_.up() && sheet_.top() < toast_.bottom()) toast_.draw();
}

void Ui::sheetFollows(int row, SheetFollow what) {
  if (sheet_.up() && row >= 0 && row < Sheet::kMaxRows) sheetFollow_[row] = what;
}

void Ui::sheetStays(int row) {
  if (sheet_.up() && row >= 0 && row < Sheet::kMaxRows) sheetStays_ |= static_cast<uint8_t>(1u << row);
}

void Ui::followSheet(int pressed) {
  if (!sheet_.up()) return;
  bool pressedDrawn = false;
  for (int i = 0; i < Sheet::kMaxRows; ++i) {
    const char* text = nullptr;
    switch (sheetFollow_[i]) {
      case SheetFollow::Sleep: text = state_.sleepRow; break;
      case SheetFollow::Shuffle: text = uitext::kOnOff[state_.shuffle ? 1 : 0]; break;
      case SheetFollow::Repeat: text = uitext::kRepeatModes[state_.repeat < 3 ? state_.repeat : 0]; break;
      default: continue;
    }
    if (sheet_.setDetail(i, text) && i == pressed) pressedDrawn = true;
  }
  if (pressed >= 0 && !pressedDrawn) sheet_.drawRow(pressed);
}

void Ui::sleepTitle(char* buf, size_t size) const {
  // Lower case after the colon, as the toasts say it ("Sleep timer: end of
  // track"); the "..." row's detail alone is capitalised ("End of track").
  snprintf(buf, size, "%s: %s", uitext::kSleepRow, state_.sleepTitle);
}

void Ui::openSleepSheet() {
  endPageTouch();
  closeModal(true);
  char title[40];
  sleepTitle(title, sizeof(title));
  sheetOpenedMs_ = millis();  // (the settle, as every sheet's)
  sleepSheet_.open(state_.sleepPick, state_.sleepRunning, state_.sleepCanExtend, title, accent::NowPlaying);
  applyCover();
  Serial.printf("[ui] sleep timer sheet (%s)\n", state_.sleepRow);
}

void Ui::refreshSleepSheet() {
  if (!sleepSheet_.up()) return;
  char title[40];
  sleepTitle(title, sizeof(title));
  sleepSheet_.refresh(state_.sleepPick, state_.sleepRunning, state_.sleepCanExtend, title);
}

void Ui::sleepFading() {
  if (!started_ || suspended_ || toast_.sleep()) return;
  viewKey_ = QueueModel::kNone;
  const int was = toast_.bottom();
  if (toast_.undo()) queue_.dropUndo();
  toast_.showSleep(accent::NowPlaying, nowMs_);
  uncover(was);
  Serial.println("[ui] toast: sleep timer: fading (+10 min, Turn off)");
}

void Ui::openVolume(int output) {
  endPageTouch();
  closeModal(true);
  const bool bt = output < 0 ? state_.onBluetooth : output == 1;
  const char* name = !bt ? "Speaker" : state_.btName[0] ? state_.btName : "Headphones";
  const int v = bt ? state_.btVolume : state_.speakerVolume;
  volumeSheetBt_ = bt;
  volumeAsked_ = v;
  volumeSheet_.open(bt, name, v, accent(), nowMs_);
  applyCover();
}

// ---- the jump grid (the A-Z rail's tap) ----

namespace {
const char* railNameOf(void* ctx, uint32_t row) { return static_cast<ListView::Source*>(ctx)->railName(row); }
}  // namespace

void Ui::onRailTap() {
  ListView::Source* src = list_.source();
  if (!src || !page_) return;
  endPageTouch();
  closeModal(true);
  jumpSource_ = src;
  jump::letters(src->railRows(), railNameOf, src, jumpFirst_, jumpEnd_);
  openJumpLetters();
}

void Ui::openJumpLetters() {
  jumpLevel_ = 1;
  char labels[jump::kGridCells][4] = {};
  bool enabled[jump::kGridCells] = {};
  for (int b = 0; b < jump::kCells; ++b) {
    labels[b][0] = b == 0 ? '#' : static_cast<char>('A' + b - 1);
    enabled[b] = jumpFirst_[b] >= 0;
  }
  // The letter the list is at now, outlined.
  int current = -1;
  uint32_t first, last;
  if (list_.visibleRows(&first, &last) && first < jumpSource_->railRows()) {
    current = textfold::bucketOf(textfold::railKey(jumpSource_->railName(first)));
  }
  jumpGrid_.open(jumpSource_->jumpTitle(), labels, enabled, current, accent());
  applyCover();
  Serial.printf("[ui] jump grid: %s, %lu rows\n", jumpSource_->jumpTitle(), (unsigned long)jumpSource_->railRows());
}

void Ui::onJumpCell(int cell) {
  if (!jumpSource_) return;
  int32_t row = -1;
  if (jumpLevel_ == 1) {
    if (cell < 0 || cell >= jump::kCells || jumpFirst_[cell] < 0) return;
    const uint32_t rows = static_cast<uint32_t>(jumpEnd_[cell] - jumpFirst_[cell]);
    if (rows <= jump::kSecondLevelRows) {
      row = jumpFirst_[cell];
    } else {
      // A big letter: its two-letter starts ("Ka", "Ke"...), the letter
      // itself first, and a way back to the letters last.
      jump::seconds(static_cast<uint32_t>(jumpFirst_[cell]), static_cast<uint32_t>(jumpEnd_[cell]), railNameOf,
                    jumpSource_, jumpSecond_);
      const char letter = cell == 0 ? '#' : static_cast<char>('A' + cell - 1);
      char labels[jump::kGridCells][4] = {};
      bool enabled[jump::kGridCells] = {};
      labels[0][0] = letter;
      enabled[0] = true;
      for (int k = 1; k < jump::kCells; ++k) {
        labels[k][0] = letter;
        labels[k][1] = static_cast<char>('a' + k - 1);
        enabled[k] = jumpSecond_[k] >= 0;
      }
      labels[JumpGrid::kBack][0] = '<';
      enabled[JumpGrid::kBack] = true;
      char title[40];
      snprintf(title, sizeof(title), "%s, %c: %lu", jumpSource_->jumpTitle(), letter, (unsigned long)rows);
      jumpLevel_ = 2;
      jumpGrid_.open(title, labels, enabled, -1, accent());
      return;
    }
  } else {
    if (cell == JumpGrid::kBack) {
      openJumpLetters();
      return;
    }
    if (cell < 0 || cell >= jump::kCells || jumpSecond_[cell] < 0) return;
    row = jumpSecond_[cell];
  }
  // The row at the top; the grid goes and the list draws there.
  jumpGrid_.close();
  jumpSource_ = nullptr;
  applyCover();
  list_.collapse();
  list_.scrollToRow(static_cast<uint32_t>(row));
  Serial.printf("[ui] jump grid: to row %ld\n", (long)row);
  repaintUnder();
}

void Ui::openDialog(OverlayOwner* owner, const char* title, const char* body, const char* const* buttons, int n,
                    bool danger) {
  endPageTouch();
  closeModal(true);
  dialogOwner_ = owner;
  lostDialog_ = false;
  playFailedDialog_ = false;
  ownDialog_ = OwnDialog::None;
  dialog_.open(title, body, buttons, n, accent(), danger);
  applyCover();
}

// The headphones dropped while playing on them (mockup 23): paused (the
// speaker never takes over by itself); the dialog says so and follows
// their reconnecting; "Use speaker" moves the output (still paused: B
// plays), OK keeps waiting. It closes itself when they are back.
void Ui::headphonesLost() {
  input_.alertBuzz();  // felt in a pocket too (spec §8: 80 ms)
  host_.wakeScreen("the headphones dropped");  // the dialog needs the listener
  if (!started_ || suspended_) return;
  // "SPYDRONE disconnected" if it fits beside the icon; else the name goes
  // into the body ("WH-1000XM4 disconnected" is 15 px too wide).
  const char* name = state_.btName[0] ? state_.btName : "Headphones";
  char title[48], body[128];
  snprintf(title, sizeof(title), "%s disconnected", name);
  if (Fonts::instance().width(Font::Bold, title) <= Dialog::titleRoom(true)) {
    snprintf(body, sizeof(body), "Paused, so the speaker doesn't suddenly play out loud.");
  } else {
    snprintf(title, sizeof(title), "Headphones disconnected");
    snprintf(body, sizeof(body), "%s: paused, so the speaker doesn't suddenly play out loud.", name);
  }
  static const char* const kButtons[2] = {"Use speaker", "OK"};
  endPageTouch();
  closeModal(true);
  dialogOwner_ = this;
  dialog_.open(title, body, kButtons, 2, col::AMBER);
  dialog_.setIcon(&icons::kHeadphones, col::AMBER, true);
  lostDialog_ = true;
  ownDialog_ = OwnDialog::Lost;
  applyCover();
  updateLostDialog();
}

void Ui::headphonesConnected() {
  input_.connectedTick();
  if (!started_ || suspended_) return;
  char text[64];
  snprintf(text, sizeof(text), "Now playing on %s", state_.btName[0] ? state_.btName : "the headphones");
  toast(text, false);
}

void Ui::updateLostDialog() {
  if (!lostDialog_ || !dialog_.up()) return;
  const BtLink& l = state_.btLink;
  char line[64];
  if (l.phase == BtLink::Phase::Paging && l.attempt > 0) {
    snprintf(line, sizeof(line), "Trying to reconnect: try %u of %u", (unsigned)l.attempt,
             (unsigned)std::max(l.attempt, l.attempts));
  } else if (l.phase == BtLink::Phase::Off) {
    snprintf(line, sizeof(line), "Not trying to reconnect");
  } else if (l.phase == BtLink::Phase::Resting) {
    // (Lost: resting only after the whole back-off; they may be back in
    // range with their own reconnect given up.)
    snprintf(line, sizeof(line), "%s", uitext::kBtLostRestingLine);
  } else {
    snprintf(line, sizeof(line), "Looking for them...");
  }
  dialog_.setStatus(line, col::AMBER);
}

// A play waited for the headphones and they didn't come (PlayGate, its
// tries or its backstop): paused; this says why and what to check. "Try
// again" waits for them afresh; "Play on speaker" is the explicit choice.
void Ui::playFailed() {
  input_.alertBuzz();  // they may be waiting with the device in a pocket
  host_.wakeScreen("couldn't reach the headphones");
  if (!started_ || suspended_) return;
  const char* name = state_.btName[0] ? state_.btName : "the headphones";
  char title[48], body[128];
  snprintf(title, sizeof(title), "Couldn't reach %s", name);
  // No icon: the title's whole width (a name as long as "WH-1000XM4" fits).
  if (Fonts::instance().width(Font::Bold, title) <= Dialog::titleRoom(false)) {
    snprintf(body, sizeof(body), "%s", uitext::kPlayFailedBody);
  } else {
    // (A long name: it goes into the body, as headphonesLost() does.)
    snprintf(title, sizeof(title), "Couldn't reach them");
    snprintf(body, sizeof(body), "%s: on, out of the case, and not connected to your phone?", name);
  }
  static const char* const kButtons[2] = {uitext::kPlayOnSpeaker, "Try again"};
  endPageTouch();
  closeModal(true);
  dialogOwner_ = this;
  dialog_.open(title, body, kButtons, 2, col::AMBER);
  playFailedDialog_ = true;
  ownDialog_ = OwnDialog::PlayFailed;
  applyCover();
  Serial.printf("[ui] play: %s (paused)\n", title);
}

void Ui::updatePlayFailed() {
  if (state_.gateFailures != gateFailuresSeen_) {
    gateFailuresSeen_ = state_.gateFailures;
    if (state_.gate == PlayGate::State::Failed) playFailed();
  }
  // Nothing to decide any more: they connected after all, or the speaker
  // is the output (the notice's own buttons close it before this).
  if (playFailedDialog_ && dialog_.up() && state_.gate != PlayGate::State::Failed) {
    closeModal(false);
    repaintUnder();
  }
}

void Ui::onDialog(int button) {
  const OwnDialog which = ownDialog_;
  ownDialog_ = OwnDialog::None;
  if (which == OwnDialog::PlayFailed) {
    if (button == 0) {
      Serial.println("[ui] play failed: play on the speaker");
      host_.playOnSpeaker();
    } else if (button == 1) {
      Serial.println("[ui] play failed: try again");
      host_.play();
    }
    return;
  }
  // headphonesLost()'s dialog: "Use speaker" moves the output (paused: B plays).
  if (which == OwnDialog::Lost && button == 0) host_.selectOutput(false);
}

void Ui::volumeKeys() { volumeHudDue_ = true; }

// A track that couldn't be played: a note, and a mark on its Queue row
// (PlaybackController has skipped on already).
void Ui::noteFailures() {
  const PlaybackController::Failure& f = player_.lastFailure();
  if (f.count == failuresSeen_) return;
  failuresSeen_ = f.count;
  failedKeys_.add(f.key);
  char title[64];
  if (!player_.catalog().title(f.track, title, sizeof(title))) snprintf(title, sizeof(title), "a track");
  // Why, when it was the track's sample rate: a refused rate isn't a
  // broken file, and at 160 MHz a setting would play it.
  char why[48];
  if (f.rate.hz != 0 && f.rate.needsCpu) {
    snprintf(why, sizeof(why), "%s", uitext::kSkippedCpu);
  } else if (f.rate.hz != 0) {
    char rate[16];
    RateConverter::rateText(f.rate.hz, rate, sizeof(rate));
    snprintf(why, sizeof(why), uitext::kSkippedRate, rate);
  } else {
    snprintf(why, sizeof(why), "%s", uitext::kSkipped);
  }
  char text[112];
  snprintf(text, sizeof(text), "Skipped %s: %s", title, why);
  warn(text);
  host_.wakeScreen("a track couldn't be played");
  if (page_ == &queuePage_) list_.refreshAll();
}

void Ui::updateHud(uint32_t nowMs) {
  const ButtonPolicy::Feedback& f = state_.feedback;
  if (f.seq != hudSeq_) {
    hudSeq_ = f.seq;
    hudFollows_ = false;  // it shows where the button's step goes
    if (f.kind == ButtonPolicy::Hud::Volume) {
      hud_.showVolume(f.volume, state_.onBluetooth, nowMs);
    } else if (f.kind == ButtonPolicy::Hud::Output && f.refused && !state_.silent) {
      // Nothing to connect to (none paired; nothing scans for them): the
      // note says where pairing is, rather than opening the Pair screen
      // from a button that works on every tab and from a pocket (its scan
      // would start from a press that asked for no such thing). The output
      // stays on the speaker; nothing plays.
      warn(uitext::kNoHeadphones);
    } else if (f.kind == ButtonPolicy::Hud::Output) {
      char line[48];
      if (f.refused) {
        snprintf(line, sizeof(line), "%s", "Stays on the speaker (silent test mode)");
      } else if (f.toBluetooth) {
        snprintf(line, sizeof(line), "Bluetooth: %s", state_.btConnected ? state_.btName : "connecting...");
      } else {
        snprintf(line, sizeof(line), "%s", f.paused ? "Speaker, paused: B plays" : "Speaker");
      }
      hud_.showOutput(f.toBluetooth && !f.refused, line, nowMs);
    }
  }
  if (volumeHudDue_) {
    // The headphones' keys: their step lands on the Bluetooth task a moment
    // later, so the HUD follows the volume while it is up.
    volumeHudDue_ = false;
    hudFollows_ = true;
    hud_.showVolume(state_.btVolume, true, nowMs);
  } else if (hudFollows_ && hud_.up() && hud_.showsVolume() && hud_.volume() != state_.btVolume) {
    hud_.showVolume(state_.btVolume, true, hud_.until() - Hud::kShowMs);  // the same timeout
  }
  if (hud_.expired(nowMs)) {
    hud_.hide();
    tabBar_.invalidate();
  }
}

// ---- the loop ----

tabbar::State Ui::tabState(uint32_t nowMs) const {
  tabbar::State s;
  s.active = static_cast<uint8_t>(nav_.tab());
  if (state_.current < 0) {
    s.play = tabbar::Play::Nothing;
  } else if (state_.play == PlayState::Playing) {
    s.play = tabbar::Play::Playing;
  } else if (state_.play == PlayState::Waiting) {
    s.play = tabbar::Play::Waiting;
  } else {
    s.play = state_.play == PlayState::Paused ? tabbar::Play::Paused : tabbar::Play::Stopped;
  }
  s.eqStep = static_cast<uint8_t>(nowMs / 250);  // 4 steps a second
  s.progressKnown = state_.durationMs > 0;
  s.progressPx = tabbar::progressPx(state_.positionMs, state_.durationMs);
  s.upNext = static_cast<uint16_t>(std::min<uint32_t>(state_.upNext, 65535));
  s.badgeFlash = static_cast<int32_t>(badgeUntilMs_ - nowMs) > 0;
  // (Not amber while the background search rests: tabbar::outputFor.)
  const BtLink::Phase ph = state_.btLink.phase;
  s.output = tabbar::outputFor(state_.onBluetooth, state_.btConnected, state_.btLost, state_.btSession.wanted(),
                               state_.btSession.failed(), ph != BtLink::Phase::Resting && ph != BtLink::Phase::Off);
  s.volume = state_.volume;
  s.battery = state_.battery;
  s.charging = state_.charging;
  // Low: the battery blinks off for half a second once a minute.
  s.lowBlink = state_.battery <= 10 && (nowMs / 500) % 120 == 0;
  s.sleep = state_.sleepFading    ? tabbar::Sleep::Fading
            : state_.sleepRunning ? tabbar::Sleep::Running
                                  : tabbar::Sleep::None;
  return s;
}

// A row renders in ~5 ms unhurried: renderAhead() only this far before a frame.
static constexpr uint32_t kAheadMinMs = 15;
// A frame longer than this missed the 30 fps cadence (counted per motion).
static constexpr uint32_t kSlowFrameUs = 35000;

void Ui::loop(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (!started_ || suspended_) return;
  host_.snapshot(state_);

  updateHud(nowMs);
  noteFailures();
  if (toast_.expired(nowMs)) {
    if (toast_.undo()) queue_.dropUndo();  // Undo was offered until now
    const int was = toast_.bottom();
    toast_.hide();
    uncover(was);  // (the jump grid and the coach cover the header row too)
  }
  if (volumeSheet_.up()) {
    if (volumeSheet_.expired(nowMs)) {
      closeModal(false);
      repaintUnder();
    } else {
      volumeSheet_.setVolume(volumeSheetBt_ ? state_.btVolume : state_.speakerVolume, volumeSheetBt_, nowMs);
    }
  }
  updateLostDialog();
  if (lostDialog_ && dialog_.up() && state_.btConnected) {
    // The headphones are back: nothing to decide any more.
    closeModal(false);
    repaintUnder();
  }
  updatePlayFailed();
  // The sleep timer: its sheet follows it, and the "..." sheet's row; the
  // fade's toast goes with the fade.
  refreshSleepSheet();
  followSheet();
  if (toast_.sleep() && !state_.sleepFading) {
    const int was = toast_.bottom();
    toast_.hide();
    uncover(was);
  }
  // The idle power-off's warning: up for its last 30 s, counting down; it
  // goes when anything keeps the device on. (Drawn only on a lit screen:
  // it doesn't light a dark one, it may be night.)
  if (state_.idleWarnS) {
    if (!toast_.idle()) {
      viewKey_ = QueueModel::kNone;
      const int was = toast_.bottom();
      if (toast_.undo()) queue_.dropUndo();
      toast_.showIdle(state_.idleWarnS, accent(), nowMs);
      uncover(was);
      Serial.printf("[ui] toast: %s (Keep on)\n", toast_.text());
    } else if (toast_.idleSeconds() != state_.idleWarnS) {
      toast_.setIdleSeconds(state_.idleWarnS);
    }
  } else if (toast_.idle()) {
    const int was = toast_.bottom();
    toast_.hide();
    uncover(was);
  }
  // The page's deadlines (under a modal too, and in the dark).
  if (page_) page_->tick(nowMs);
  // A shuffle toggle drops the queue's undo (docs/QUEUE-MODES.md 2.6): an
  // Undo toast still up would answer "Nothing to undo", so it goes. (Shuffle
  // all's toast stays: its Play, after the toggle, is undoable.) And no
  // badge flash for it: Off can grow "up next" without adding anything.
  const bool toggled = state_.shuffle != lastShuffle_;
  if (toggled) {
    lastShuffle_ = state_.shuffle;
    if (toast_.up() && toast_.undo() && queue_.undoable() == QueueModel::Edit::None) {
      const int was = toast_.bottom();
      toast_.hide();
      uncover(was);
      Serial.println("[ui] the Undo toast went: a shuffle toggle took the undo");
    }
  }
  // Tracks added: the Queue badge flashes.
  if (state_.contentVersion != lastContent_) {
    if (state_.upNext > lastUpNext_ && !toggled) badgeUntilMs_ = nowMs + 1500;
    lastContent_ = state_.contentVersion;
  }
  lastUpNext_ = state_.upNext;
  if (dark_) {
    // The screen is off: nothing drawn (the page, the tab bar, frames);
    // what the cover worker made comes in, and no new job starts.
    if (dance_.active()) {
      dance_.setActive(false);  // (a console tab change while dark)
      danceWasOn_ = true;
    }
    thumbs_.loop(nowMs, /*busy=*/true);
    return;
  }
  if (!hud_.up()) tabBar_.update(tabState(nowMs));

  // The page: redraws what changed, and animation frames on the cadence.
  budget_ = governor_.update(nowMs, state_.ringMs, state_.underruns, state_.ringMatters);
  clock_.setPeriod(budget_.frameMs);
  const bool due = budget_.draw && clock_.due(nowMs);
  const bool modal = modalUp();
  const uint32_t t0 = micros();
  if (page_ && !modal && page_->update(nowMs, due, budget_.wholeRows)) {
    clock_.drawn(nowMs);
    ++frames_;
    ++framesInWindow_;
    if (motion_.on) {
      const uint32_t us = micros() - t0;
      ++motion_.frames;
      motion_.sumUs += us;
      if (us > motion_.maxUs) motion_.maxUs = us;
      if (us > kSlowFrameUs) ++motion_.slow;
      if (us > 50000 && list_.attached()) {
        const ListView::FrameCost& fc = list_.lastFrame();
        Serial.printf("[ui] slow frame %lu us: offset %ld to %ld, %lu items rendered in %lu us\n", (unsigned long)us,
                      (long)fc.from, (long)fc.to, (unsigned long)fc.renders, (unsigned long)fc.renderUs);
      }
    }
  }
  trackMotion(nowMs);
  // Between frames: the row the list moves toward, rendered ahead while
  // the next frame is far enough off (ListView::renderAhead).
  if (page_ && !modal && list_.attached() && budget_.draw && page_->animating() &&
      clock_.msUntilDue(millis()) >= kAheadMinMs) {
    list_.renderAhead();
  }
  // Covers: a new job only while no list moves; one that arrived is drawn
  // where it shows (a modal's page draws it all when the modal goes).
  const uint32_t arrived = thumbs_.loop(nowMs, page_ && page_->animating());
  if (arrived != Thumbs::kNone && page_ && !modalUp()) page_->thumbReady(arrived);
  if (nowMs - framesWindowStart_ >= 1000) {
    const uint32_t ms = nowMs - framesWindowStart_;
    if (framesInWindow_ > 1) fps_ = framesInWindow_ * 1000.0f / ms;
    framesWindowStart_ = nowMs;
    framesInWindow_ = 0;
  }
}

// A list moving (a drag, a fling, a snap) is a "motion": its frames, how
// long they took to draw, the audio ring's low point and new underruns are
// logged when it settles ("[ui] scroll: ..."), the numbers the scroll lab
// printed for its stress runs. So is any other page that animates (Now
// Playing while a finger scrubs its seek bar: "[ui] scrub: ...").
void Ui::trackMotion(uint32_t nowMs) {
  const bool moving = page_ && page_->animating();
  if (moving && !motion_.on) {
    motion_ = Motion{};
    motion_.on = true;
    motion_.list = list_.attached();
    motion_.startMs = nowMs;
    motion_.underruns = state_.underruns;
    motion_.ringMin = UINT32_MAX;
  }
  if (!motion_.on) return;
  if (state_.ringMatters && state_.ringMs < motion_.ringMin) motion_.ringMin = state_.ringMs;
  if (moving) return;
  motion_.on = false;
  const uint32_t ms = nowMs - motion_.startMs;
  if (motion_.frames < 2) return;  // a tap's highlight, not a scroll
  char ring[24] = "n/a (not playing)";
  if (motion_.ringMin != UINT32_MAX) snprintf(ring, sizeof(ring), "%lu ms", (unsigned long)motion_.ringMin);
  Serial.printf("[ui] %s: %lu ms, %lu frames (%.1f fps), draw mean %.1f max %.1f ms (%lu over 35), ring min %s, underruns +%lu, "
                "governor %s\n",
                motion_.list ? "scroll" : "scrub", (unsigned long)ms, (unsigned long)motion_.frames,
                ms ? motion_.frames * 1000.0f / ms : 0.0f, motion_.sumUs / 1000.0f / motion_.frames, motion_.maxUs / 1000.0f, (unsigned long)motion_.slow, ring,
                (unsigned long)(state_.underruns - motion_.underruns), ScrollGovernor::name(budget_.level));
}

uint32_t Ui::idleMs(uint32_t nowMs) const {
  if (dark_ && started_ && !suspended_) return 20;  // nothing to draw: touches read every 20 ms
  if (!started_ || suspended_ || !page_ || !page_->animating()) return 5;
  return std::max<uint32_t>(1, std::min<uint32_t>(5, clock_.msUntilDue(nowMs)));
}

// ---- touch ----

void Ui::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!started_ || suspended_ || !e.isGlass()) return;
  if (e.type == T::LongPress) {
    // Whatever is under the finger says it used the hold (holdTick()).
    input_.takeHoldUsed();
    route(e);
    holdUnused_ = touch_ != TouchOn::None && !input_.takeHoldUsed();
    hold_ = e;  // where it pressed
    return;
  }
  if (e.type == T::Release && holdUnused_) {
    // Nothing held: a slow tap, if the finger is still where it pressed.
    holdUnused_ = false;
    constexpr int kSlowTapSlop = 24;
    if (std::abs(e.x - hold_.x) <= kSlowTapSlop && std::abs(e.y - hold_.y) <= kSlowTapSlop) {
      InputEvent tap = hold_;
      tap.type = T::Tap;
      tap.ms = e.ms;
      route(tap);
      return;
    }
  }
  route(e);
}

void Ui::route(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    holdUnused_ = false;
    if (e.y < kBarH) {
      touch_ = TouchOn::Bar;
    } else if (toast_.onSleepButton(e)) {
      // The fade toast's buttons, drawn over whatever is open (a sheet, the
      // volume sheet): the only controls that act on the timer, first.
      touch_ = TouchOn::Toast;
    } else if (dialog_.up()) {
      touch_ = TouchOn::Dialog;
    } else if ((sheet_.up() || sleepSheet_.up()) &&
               static_cast<int32_t>(e.ms - sheetOpenedMs_) < static_cast<int32_t>(Sheet::kSettleMs)) {
      // The settle: a touch that starts this soon after a sheet opened is
      // the finger that opened it coming back (a double tap on "..." or on
      // the album row): the whole touch goes nowhere, no tick.
      touch_ = TouchOn::None;
      Serial.println("[ui] sheet: a touch right after it opened, ignored");
    } else if (sheet_.up()) {
      touch_ = TouchOn::Sheet;
    } else if (sleepSheet_.up()) {
      touch_ = TouchOn::Sleep;
    } else if (volumeSheet_.up()) {
      touch_ = TouchOn::Volume;
      input_.noHold();  // resting on the slider is no long press
    } else if (jumpGrid_.up()) {
      touch_ = TouchOn::Jump;
    } else if (coach_.up()) {
      touch_ = TouchOn::Coach;
    } else if (page_ && page_->hasHeader() && toast_.passesThrough(e)) {
      // The header's ‹ (and its pill, beside a toast with no buttons) under
      // the toast: still live (spec §5). The toast makes way.
      const int was = toast_.bottom();
      if (toast_.undo()) queue_.dropUndo();
      toast_.hide();
      uncover(was);
      touch_ = TouchOn::Page;
    } else if (toast_.hit(e, true)) {
      touch_ = TouchOn::Toast;
    } else {
      touch_ = TouchOn::Page;
    }
  } else if (e.type == T::DragStart && e.fromStrip) {
    // A swipe up from the button strip: no Down came first, and it is no
    // press. The page's, to scroll its list (its bottom rows are just above
    // the strip); under a modal nothing takes it (the modals don't scroll:
    // their controls are presses, and the volume slider must not jump to
    // the finger). Not the toast's either: it only has buttons.
    holdUnused_ = false;
    touch_ = modalUp() ? TouchOn::None : TouchOn::Page;
  }
  const bool ends = e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel;
  switch (touch_) {
    case TouchOn::Bar:
      if (e.type == T::Tap) {
        tick();
        if (hud_.up()) {  // a tap on the HUD hides it and still switches the tab
          hud_.hide();
          tabBar_.invalidate();
        }
        tapTab(static_cast<NavModel::Tab>(tabbar::tabAt(e.x, e.atRightEdge())));
      }
      break;
    case TouchOn::Dialog: {
      const int b = dialog_.onEvent(e);
      if (b >= 0) {
        tick();
        OverlayOwner* owner = dialogOwner_;
        dialog_.close();
        lostDialog_ = false;
        playFailedDialog_ = false;
        repaintUnder();
        if (owner) owner->onDialog(b);
      }
      break;
    }
    case TouchOn::Sheet: {
      const int r = sheet_.onEvent(e);
      if (r >= 0 && r < Sheet::kMaxRows && (sheetStays_ >> r) & 1u) {
        // A staying row: its owner acts, the sheet stays up, the row shows
        // the new state (the snapshot now, as retryCard() takes it).
        tick();
        if (sheetOwner_) sheetOwner_->onSheet(r);
        host_.snapshot(state_);
        followSheet(r);
      } else if (r >= 0 || r == -2) {
        tick();
        OverlayOwner* owner = sheetOwner_;
        sheet_.close();
        repaintUnder();
        if (owner) owner->onSheet(r >= 0 ? r : -1);
      }
      break;
    }
    case TouchOn::Sleep: {
      const int c = sleepSheet_.onEvent(e);
      if (c >= 0 || c == -2) {
        tick();
        sleepSheet_.close();
        repaintUnder();
        if (c >= 0) host_.sleepChoose(c);
      }
      break;
    }
    case TouchOn::Volume: {
      const VolumeSheet::Result r = volumeSheet_.onEvent(e, nowMs_);
      if (r.target >= 0) {
        // Steps from the last one asked for: Bluetooth applies them on its
        // own task, so the volume read back lags a quick drag.
        const int from = volumeAsked_ >= 0 ? volumeAsked_ : volumeSheetBt_ ? state_.btVolume : state_.speakerVolume;
        if (r.target != from) host_.stepOutputVolume(volumeSheetBt_, r.target - from);
        volumeAsked_ = r.target;
      }
      // A tap's tick: on a control (even − at 0 %), and closing it (as the
      // other sheets do).
      if (r.tapped || r.close) tick();
      if (r.close) {
        closeModal(false);
        repaintUnder();
      }
      break;
    }
    case TouchOn::Jump: {
      const int c = jumpGrid_.onEvent(e);
      if (c == -2) {
        tick();
        closeModal(false);
        repaintUnder();
      } else if (c >= 0) {
        tick();
        onJumpCell(c);
      }
      break;
    }
    case TouchOn::Coach: {
      const Coach::Result r = coach_.onEvent(e);
      if (r == Coach::Result::Next) {
        tick();
      } else if (r == Coach::Result::Done) {
        tick();
        coachDone();
        repaintUnder();
      }
      break;
    }
    case TouchOn::Toast:
      if (e.type == T::Down && toast_.idle()) {
        // The idle warning: this touch keeps it on (any would); it goes now,
        // and the rest of the touch is its (its tap ticks).
        const bool keepOn = toast_.hit(e, true) == Toast::kHitKeepOn;
        const int was = toast_.bottom();
        toast_.hide();
        uncover(was);
        if (keepOn) host_.idleKeepOn();
        break;
      }
      if (e.type == T::Down && toast_.sleep() && host_.touchLandedUnattended()) {
        // The fade toast and the touch that attended a screen woken from
        // off (maybe a pocket's second contact): neither button acts on it
        // (they raise the level), so no press is shown.
        if (toast_.hit(e, true) >= Toast::kHitExtend) {
          Serial.println("[ui] sleep toast: the first touch after a wake from off: +10 min / Turn off wait for the next");
        }
      } else if (e.type == T::Down && toast_.hit(e, true) >= 2) {
        toast_.draw(toast_.hit(e, true));
      }
      if (e.type == T::Tap) {
        const int hit = toast_.hit(e, true);
        const int was = toast_.bottom();
        // The sleep timer's fade: its buttons are the only controls that act
        // on it, and not on the touch that attended the screen
        // (SleepTimer::toastTap()): no tick, the toast stays.
        const bool sleepButton = hit == Toast::kHitExtend || hit == Toast::kHitTurnOff;
        const bool refused = sleepButton && SleepTimer::toastTap(e.x, e.atRightEdge(), host_.touchLandedUnattended()) ==
                                                SleepTimer::ToastButton::None;
        if (!refused) tick();
        if (refused) {
          toast_.draw(0);
        } else if (sleepButton) {
          toast_.hide();
          uncover(was);
          host_.sleepChoose(hit == Toast::kHitExtend ? SleepSheet::kExtend : SleepSheet::kTurnOff);
        } else if (hit == 2) {
          const bool undone = player_.undo();
          Serial.printf("[ui] undo: %s\n", undone ? "done" : "nothing to undo");
          toast_.show(undone ? "Undone" : "Nothing to undo", false, false, accent(), nowMs_);
          uncover(was);
        } else if (hit == 3) {
          toast_.hide();
          uncover(was);
          viewInQueue(viewKey_);
        } else {
          toast_.hide();
          uncover(was);
        }
      } else if (ends) {
        toast_.draw(0);
      }
      break;
    case TouchOn::Page:
      if (page_) page_->onEvent(e);
      break;
    default:
      break;
  }
  if (ends) touch_ = TouchOn::None;
}

// ---- display ownership ----

void Ui::suspend() {
  if (suspended_) return;
  suspended_ = true;
  // The screen that takes the display draws as ever (it keeps it lit).
  gfx::setDark(false);
  danceWasOn_ = false;  // its page is left below
  if (!started_) return;
  closeModal(false);
  if (coach_.up()) coach_.close();  // shown again at the next boot (not marked seen)
  toast_.hide();
  hud_.hide();
  if (page_) page_->leave();
  page_ = nullptr;
  applyCover();
  ensureScroller(false);
  touch_ = TouchOn::None;
}

void Ui::resume() {
  if (!suspended_) return;
  suspended_ = false;
  gfx::setDark(dark_);  // (dark: drawn when it wakes)
  if (!started_) return;
  gfx::fill(0, 0, kW, kH, col::BG, true);
  tabBar_.invalidate();
  showTop();
}

// ---- the screen off (ScreenPower) ----

void Ui::setDark(bool on) {
  if (on == dark_) return;
  dark_ = on;
  if (on) {
    if (!suspended_) gfx::setDark(true);
    if (!started_ || suspended_) return;
    // (No finger is on: a touch keeps the screen lit. The console's Ps0 can.)
    endPageTouch();
    touch_ = TouchOn::None;
    holdUnused_ = false;
    // A fling stops where it is (not carried on, or caught up, on the wake).
    if (list_.attached() && list_.animating()) list_.scrollTo(list_.offset());
    if (page_) page_->screenOff();
    if (dance_.active()) {
      dance_.setActive(false);
      danceWasOn_ = true;
    }
    Serial.println("[ui] dark: nothing drawn until the screen wakes");
    return;
  }
  gfx::setDark(false);
  if (!started_ || suspended_) return;
  redrawAll();
}

// Everything drawn again, as it is now: nothing reached the panel while it
// was off, and what was drawn before (its GRAM) may be stale.
void Ui::redrawAll() {
  const uint32_t t0 = millis();
  // The panel keeps its scroll registers through sleep-in; sent again anyway.
  if (vscroll_.active()) vscroll_.resend();
  // A volume HUD that came up in the dark is old news.
  if (hud_.up()) hud_.hide();
  if (danceWasOn_ && page_ == &dancePage_) dance_.setActive(true);
  danceWasOn_ = false;
  tabBar_.invalidate();
  tabBar_.update(tabState(nowMs_));
  // The page (a list page: its header now, its band at the next frame, or
  // when the modal over it goes), then what is over it, the toast last.
  applyCover();
  if (jumpGrid_.up()) {
    jumpGrid_.draw();
  } else if (coach_.up()) {
    coach_.draw();
  } else if (page_) {
    page_->repaint();
  }
  if (sheet_.up()) sheet_.draw();
  if (sleepSheet_.up()) sleepSheet_.draw();
  if (volumeSheet_.up()) volumeSheet_.draw();
  if (dialog_.up()) dialog_.draw();
  if (toast_.up()) toast_.draw();
  Serial.printf("[ui] awake: drawn again in %lu ms\n", (unsigned long)(millis() - t0));
}

// ---- the header every list page has ----

void Ui::drawHeader(const Header& h) {
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  const uint16_t acc = accent();
  s.fillRect(0, 0, kW, kHeaderH, col::HEAD);
  int x = 12;
  if (h.back || h.cross) {
    icons::drawCentred(s, h.cross ? icons::kCross : icons::kChevronLeft, h.cross ? 18 : 14, 17, acc);
    x = h.cross ? 36 : 28;
  }
  int right = kW - 10;
  if (h.right) {
    const int iconW = h.rightBack ? 12 : 0;
    const int w = f.width(Font::Bold, h.right) + 24 + iconW;
    const int px = kW - 6 - w;
    const uint16_t pill = h.rightDanger ? (h.pressed ? col::SOFT : col::RED) : h.pressed ? col::BTN_HI : col::BTN;
    s.fillRoundRect(px, 5, w, 26, 13, pill);
    if (h.rightBack) icons::drawCentred(s, icons::kChevronLeft, px + 14, 18, acc);
    f.draw(s, Font::Bold, h.right, px + iconW + (w - iconW) / 2, 17, w - iconW, h.rightDanger ? col::DARK : col::TXT,
           pill, Fonts::Align::Centre);
    right = px - 8;
  }
  if (h.path) {
    // Two lines: the title, then where it is (cut from the left, the
    // nearest folders kept: "…/Kavinsky") and what's in it.
    // The counts end where the title does: before the pill when there is
    // one (it spans both lines), so they never run under it. They are
    // whole when they fit ("14 audio files, 1 other"); the path gets
    // what's left, and only if that is enough to say something (on the
    // device, "/Daft…" beside a cut count said nothing).
    const bool counts = h.counts && h.counts[0];
    const int cw = counts ? std::min(f.width(Font::Small, h.counts), right - x) : 0;
    const int room = right - (cw ? cw + 10 : 0) - x;
    if (counts) {
      // Beside the path, at the right; alone, where the path would start.
      if (room >= kMinPathRoom) {
        f.draw(s, Font::Small, h.counts, right, 26, right - x, col::DIM, col::HEAD, Fonts::Align::Right);
      } else {
        f.draw(s, Font::Small, h.counts, x, 26, right - x, col::DIM, col::HEAD);
      }
    }
    if (room >= kMinPathRoom) {
      char cut[160];
      textfit::cutPathLeft(f.fit(Font::Small), h.path, room, cut, sizeof(cut));  // "…/the rest"
      f.draw(s, Font::Small, cut, x, 26, room, col::DIM, col::HEAD);
    }
    // The title last: the second line's background would cut its
    // descenders ("Discoverv" on the device).
    f.draw(s, Font::Bold, h.title, x, 10, right - x, col::TXT, col::HEAD);
  } else {
    const int tw = f.draw(s, Font::Bold, h.title, x, 17, right - x, col::TXT, col::HEAD);
    if (h.sub && h.sub[0] && x + tw + 8 < right - 16) {
      f.draw(s, Font::Small, h.sub, x + tw + 8, 18, right - (x + tw + 8), col::DIM, col::HEAD);
    }
  }
  s.fillRect(0, kHeaderH - 2, kW, 2, acc);
  gfx::push(s, 0, kHeaderY, kW, kHeaderH);
}

// ---- console ----

void Ui::printState() const {
  Serial.printf("[ui] %s; tab %s, page %s (depth %d)%s\n",
                !started_    ? "not started"
                : suspended_ ? "SUSPENDED (another screen has the display)"
                : dark_      ? "up, DARK (the screen is off: nothing drawn)"
                             : "up",
                NavModel::name(nav_.tab()), pageKindName(nav_.top().kind), nav_.depth(),
                vscroll_.active() ? ", hardware scroll on" : "");
  for (int t = 0; t < NavModel::kTabs; ++t) {
    const auto tab = static_cast<NavModel::Tab>(t);
    char line[240];
    int n = snprintf(line, sizeof(line), "[ui]   %s%s:", tab == nav_.tab() ? "*" : " ", NavModel::name(tab));
    for (int l = 0; l < nav_.depth(tab) && n < static_cast<int>(sizeof(line)) - 48; ++l) {
      const NavModel::PageRef& p = nav_.at(tab, l);
      n += snprintf(line + n, sizeof(line) - n, "%s %s", l ? " >" : "", pageKindName(p.kind));
      if (p.id != NavModel::kNone) n += snprintf(line + n, sizeof(line) - n, "(%lu)", (unsigned long)p.id);
      if (p.scrollPx >= 0) n += snprintf(line + n, sizeof(line) - n, " @%ldpx", (long)p.scrollPx);
      if (p.expanded >= 0) n += snprintf(line + n, sizeof(line) - n, " open:%ld", (long)p.expanded);
    }
    Serial.println(line);
  }
  if (page_) {
    char desc[256];  // (Now Playing's with its seek bar: up to ~180)
    page_->describe(desc, sizeof(desc));
    Serial.printf("[ui] page: %s\n", desc);
  }
  if (list_.attached()) {
    Serial.printf("[ui] list: %lu rows, %lu items, offset %ld px, open row %ld, %s, %lu frames, %lu rows rendered ahead\n",
                  (unsigned long)list_.layout().rows(), (unsigned long)list_.layout().itemCount(),
                  (long)list_.offset(), (long)list_.expanded(), KineticScroll::name(list_.scroll().phase()),
                  (unsigned long)list_.framesDrawn(), (unsigned long)list_.aheadRenders());
  }
  const SpiHoldStats& h = gfx::holds();
  Serial.printf("[ui] frames %lu (last busy second %.1f fps), cap %lu ms on deadlines, governor %s; bus holds %lu, "
                "mean %.2f ms, max %.2f ms\n",
                (unsigned long)frames_, fps_, (unsigned long)clock_.period(), ScrollGovernor::name(budget_.level),
                (unsigned long)h.count, h.meanUs() / 1000.0f, h.maxUs / 1000.0f);
  Serial.printf("[ui] overlays: toast %s%s%s, HUD %s, sheet %s, sleep timer sheet %s, volume %s, jump grid %s, "
                "dialog %s, coach %s\n",
                toast_.up() ? "\"" : "none", toast_.up() ? toast_.text() : "", toast_.up() ? "\"" : "",
                hud_.up() ? "up" : "no", sheet_.up() ? "open" : "no", sleepSheet_.up() ? "open" : "no",
                volumeSheet_.up() ? (volumeSheetBt_ ? "open (headphones)" : "open (speaker)") : "no",
                jumpGrid_.up() ? "open" : "no", dialog_.up() ? "open" : "no",
                coach_.up() ? (coach_.card() == 0 ? "card 1" : "card 2") : "no");
  Serial.printf("[ui] bluetooth: link %s (try %u of %u, %s), session%s%s%s, play gate %s (%lu failed); queue marks: "
                "added since key %ld, %d failed\n",
                btPhaseName(state_.btLink.phase), (unsigned)state_.btLink.attempt, (unsigned)state_.btLink.attempts,
                state_.btLink.remembered ? "remembered" : "none remembered",
                state_.btSession.wanted() ? " wanted" : "", state_.btSession.failed() ? " failed" : "",
                state_.btSession.pairing() ? " pairing" : "", PlayGate::stateName(state_.gate),
                (unsigned long)state_.gateFailures,
                added_.pending() ? (long)added_.since() : -1L, failedKeys_.count());
  if (browse_) {
    Serial.printf("[ui] the Library browses a SYNTHETIC library: %lu tracks, %lu artists, %lu albums (uil0: the card's)\n",
                  (unsigned long)browse_->trackCount(), (unsigned long)browse_->artistCount(),
                  (unsigned long)browse_->albumCount());
  }
  thumbs_.printState();
  // This runs on the loop task (the console): its stack's low-water mark.
  Serial.printf("[ui] loop task stack: %u B never used (of 8 KB)\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void Ui::command(const char* a) {
  if (!a || !a[0]) {
    printState();
    return;
  }
  if (!started_ || suspended_) {
    Serial.println("[ui] not on screen now");
    return;
  }
  if (dark_) host_.wakeScreen("console ui");  // drawn again on the wake
  if (a[0] >= '0' && a[0] <= '4' && !a[1]) {
    tapTab(static_cast<NavModel::Tab>(a[0] - '0'));  // as a tap on that tab (again: to its root)
  } else if (a[0] == 'b') {
    back();
  } else if (a[0] == 'c') {
    showCoach();
  } else if (a[0] == 'T') {
    thumbs_.redecode();
    list_.refreshAll();  // the rows on screen ask again
  } else {
    Serial.println("[ui] ui: the navigation state; ui0-ui4 tap a tab (0 Now Playing, 1 Library, 2 Queue, 3 Dance, "
                   "4 Output); uib back; uic the coach cards; uiT the covers decoded again (timings); uil<n> the Library browses a synthetic library of n "
                   "tracks (uil0: the card's); uit/uih/uis/uid/uip a scripted finger");
    return;
  }
  printState();
}

}  // namespace ui
