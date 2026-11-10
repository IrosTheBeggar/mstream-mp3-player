// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/Ui.h"

#include <M5Unified.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "NvsLayout.h"
#include "RateConverter.h"
#include "SleepTimer.h"
#include "TagText.h"
#include "TextFit.h"
#include "TextFold.h"
#include "UiStrings.h"
#include "UiText.h"
#include "app/LoopStack.h"
#include "app/Psram.h"
#include "app/UiLang.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/HotMem.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace ui {

using uistr::S;

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
    case PageKind::DeviceInfo: return "DeviceInfo";
    case PageKind::Language: return "Language";
    case PageKind::AutoDj: return "AutoDj";
    default: return "-";
  }
}

void Page::describe(char* buf, size_t size) const { snprintf(buf, size, "%s", name()); }

int Header::hit(const InputEvent& e) const {
  if (e.y < kHeaderY || e.y >= kHeaderY + kHeaderH) return 0;
  if ((back || cross) && e.x < 56) return 1;
  // The pill's: x 240 to the edge, or from its round end and chevron when
  // it starts further left (uitext::headerPillZoneX(): Russian's crumb).
  if (right && e.inRightEdgeZone(uitext::headerPillZoneX(kW - 6 - pillW(right, rightMaxW, rightBack), rightBack))) {
    return 2;
  }
  return 3;
}

Font Header::pillFont(const char* label, int maxW) {
  return maxW > 0 ? Fonts::instance().fitting(Font::Bold, label, maxW) : Font::Bold;
}

int Header::pillW(const char* label, int maxW, bool chevron) {
  return Fonts::instance().width(pillFont(label, maxW), label) + uitext::kHeaderPillPad +
         (chevron ? uitext::kHeaderChevronW : 0);
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
      glance_(*this, NowPlayingPage::Mode::Glance),
      thumbs_(library),
      waves_(library) {
  // The list's safety net (UI spike): fewer frames, or whole rows, while
  // the decoder's buffer runs low.
  ScrollGovernor::Config g;
  g.normalFrameMs = kPageFrameMs;  // (a list's own period: listFrameMs())
  g.reducedFrameMs = 66;
  governor_.setConfig(g);
}

// The frame clock's period while a list page is up and the governor has
// nothing to say. On the panel's scan every refresh may have a frame (71.5
// a second on the Core2): the clock only keeps two out of one refresh.
// Without it, 60 a second on the clock's deadlines (30 until 0.14.0: at
// 2,000 px/s a frame then pushed 67 lines and brought on a row and a half,
// 20-45 ms with an MP3 playing; half of that a frame fits).
uint32_t Ui::listFrameMs() const {
  if (forcedFrameMs_) return forcedFrameMs_;
  return scan_.ok() && list_.scanSync() ? kScanFrameMs : kListFrameMs;
}

bool Ui::begin() {
  Fonts& f = Fonts::instance();
  if (!f.load()) Serial.println("[ui] VLW fonts failed to load: the built-in bitmap fonts instead");
  list_.setScan(&scan_);
  if (!gfx::begin() || !overlaysBegin() || !list_.begin(vscroll_, input_)) {
    Serial.println("[ui] no PSRAM for the UI's sprites");
    return false;
  }
  list_.setRailHost(this);
  // Covers: without PSRAM for them, every one is the placeholder.
  if (!thumbs_.begin()) Serial.println("[ui] no PSRAM for the cover thumbnails: placeholders");
  // The waveform index the boot's root names (setup(): after the library).
  waves_.begin();
  nav_.setRoot(NavModel::Tab::NowPlaying, ref(PageKind::NowPlaying));
  nav_.setRoot(NavModel::Tab::Library, ref(PageKind::Library, static_cast<uint32_t>(LibrarySegment::Artists)));
  nav_.setRoot(NavModel::Tab::Queue, ref(PageKind::Queue));
  nav_.setRoot(NavModel::Tab::Dance, ref(PageKind::Dance));
  nav_.setRoot(NavModel::Tab::Output, ref(PageKind::Output));
  // The first-boot tips seen (NVS "ui"/"coach"), and a fresh unit's
  // language picker (no language saved either: nvslayout).
  Preferences prefs;
  if (prefs.begin(nvslayout::kUiNamespace, true)) {
    coachSeen_ = prefs.getBool(nvslayout::kCoachKey, false);
    prefs.end();
  }
  pickerDue_ = nvslayout::langPickerDue(uilang::chosen(), coachSeen_);
  return true;
}

void Ui::start(uint32_t nowMs) {
  nowMs_ = nowMs;
  host_.snapshot(state_);
  hudSeq_ = state_.feedback.seq;
  gateFailuresSeen_ = state_.gateFailures;
  lastContent_ = state_.contentVersion;
  lastUpNext_ = state_.upNext;
  lastDjAdds_ = state_.autoDj.adds;
  failuresSeen_ = player_.lastFailure().count;
  cardHoldsSeen_ = player_.cardHolds().begun;
  started_ = true;
  // A fresh unit: the language first (the touch check and the tips then
  // speak it).
  if (pickerDue_) {
    pickerNav();
    tipsAfterPicker_ = !coachSeen_;
  }
  gfx::fill(0, 0, kW, kH, col::BG, true);
  tabBar_.invalidate();
  tabBar_.update(tabState(nowMs));
  showTop();
  // The panel's scan (awake since the boot screen): timed once, ~60 ms.
  scan_.calibrate();
  scan_.print();
  if (pickerDue_) {
    Serial.println("[ui] up: the first boot's language picker (then the touch check and the tips; console ui: the "
                   "navigation state)");
    return;
  }
  Serial.println("[ui] up: tab bar, Now Playing (console ui: the navigation state)");
  // The first-boot tips, once (NVS "ui"/"coach").
  if (!coachSeen_) showCoach();
}

void Ui::pickerNav() {
  nav_.popToRoot(NavModel::Tab::Output);
  nav_.select(NavModel::Tab::Output);
  nav_.push(ref(PageKind::Language, kFirstBootPicker));
}

bool Ui::pickerOnTop() const {
  const NavModel::PageRef& top = nav_.top();
  return nav_.tab() == NavModel::Tab::Output && static_cast<PageKind>(top.kind) == PageKind::Language &&
         top.id == kFirstBootPicker;
}

void Ui::chooseLanguage(uistr::Lang l) {
  const bool first = pickerDue_ && pickerOnTop();
  uilang::choose(l);
  // Said in the language itself, where its catalogue has it: never for
  // English, nor for a language with no catalogue yet.
  const bool note = uistr::lang() != uistr::Lang::En && uistr::translated(S::language_machine_note);
  if (!started_ || suspended_) return;  // (resume() draws it all)
  if (!first) {
    languageChanged();
    if (note) toast(tr(S::language_machine_note), false);
    return;
  }
  // The first boot goes on: Now Playing (the Output tab back at its root),
  // the touch check if it is due (main.cpp, before the next loop()), then
  // the tips and the note (loop()).
  pickerDue_ = false;
  noteAfterPicker_ = note;
  closeModal(true);
  if (toast_.up()) toast_.hide();
  hud_.hide();
  if (page_) page_->leave();
  page_ = nullptr;
  nav_.popToRoot(NavModel::Tab::Output);
  nav_.select(NavModel::Tab::NowPlaying);
  tabBar_.invalidate();  // its labels in the language picked
  showTop();
  Serial.println("[ui] first boot: the language picked; the touch check (if due), then the tips");
}

// What holds a text in the old language goes: a sheet or a dialog (closed
// as a tab tap closes them), the toast (its Undo with it), the HUD. The
// page is entered again where it was (its ref keeps the place), so what it
// made of texts on its way in (the Folders root's "Play all N") is made
// again; then the tab bar, and the tips over the page if they are up.
void Ui::languageChanged() {
  if (!started_ || suspended_) return;
  closeModal(true);
  if (toast_.up()) {
    if (toast_.undo()) queue_.dropUndo();
    toast_.hide();
  }
  if (hud_.up()) hud_.hide();
  tabBar_.invalidate();
  if (page_) page_->leave();
  page_ = nullptr;
  showTop();
  if (coach_.up()) coach_.draw();
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
  coachSeen_ = true;
  Preferences prefs;
  if (prefs.begin(nvslayout::kUiNamespace, false)) {
    prefs.putBool(nvslayout::kCoachKey, true);
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
    case PageKind::About:
    case PageKind::DeviceInfo:
    case PageKind::Language:
    case PageKind::AutoDj: return &outputPage_;
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
    // A page that doesn't scroll (Now Playing, the Dance tab) comes up over
    // the band: the scroll mode stays on, and its drawing lands where it
    // shows through the mapping (ui/Gfx, DanceView::pushRect()), so the
    // band is neither cleared first (168 rows, 21 ms of black before the
    // page painted, MEASURED) nor shown rotated. What the band's GRAM holds
    // isn't the list's any more: the next list page draws it all again, in
    // place (ListScroller::invalidate()).
    vscroll_.invalidate();
  }
}

// A screen of its own takes the display (suspend()): it draws with plain
// M5GFX calls, so the scroll mode ends. The band's GRAM is in the rotated
// order: cleared first (all of it, whatever the rotation), then the address
// back to the identity.
void Ui::endScroller() {
  if (!vscroll_.active()) return;
  gfx::fill(0, ListView::kTop, kW, ListView::kHeight, col::BG, true);
  vscroll_.end();
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
    toast(tr(S::toast_card_found), false);
    return;
  }
  // What is in may have changed (an exFAT card taken out, or put in, or
  // swapped for another): the snapshot now, not at the next pass, for the
  // note ("Still exFAT: ...", "Still no card: ...").
  const cardformat::Kind was = state_.cardKind;
  host_.snapshot(state_);
  if (state_.cardKind != was && page_ && !modalUp()) page_->repaint();
  warn(tr(uitext::cardMessage(state_.cardKind).still));
}

void Ui::shuffleAll() {
  if (state_.libraryFenced) {  // the library update's fence: the index is the build's
    toast(tr(S::library_updating_wait), false);
    return;
  }
  const LibraryIndex* index = library_.index();
  if (browse_ || !index || !index->ready() || index->trackCount() == 0) {
    warn(tr(S::toast_no_music_to_shuffle));
    return;
  }
  // The whole library from a random track, with shuffle on (the mode, not
  // a one-shot: the menu then says what plays, and Off brings the library's
  // own order back, A-Z, from the track that plays). One edit, the mode in
  // it, so the toast's Undo puts back the queue and the mode it found
  // (QueueModel copies the ids and shuffles them: docs/QUEUE-MODES.md
  // sections 2.5 and 2.6).
  // Past the queue's cap (QueueModel::kMaxEntries), a random 5,000 of the
  // library (docs/QUEUE-MODES.md section 15), and the toast says so.
  const LibraryIndex::Span all = index->allTracks();
  const bool was = player_.shuffle();
  const bool ok = player_.playNow(all.ids, all.count, PlaybackController::kAnyStart, /*shuffle=*/true);
  if (ok) added_.clear();  // a new queue: nothing "added" to show in it
  const uint32_t took = all.count < QueueModel::kMaxEntries ? all.count : QueueModel::kMaxEntries;
  char capped[64] = "";
  if (took < all.count) {
    snprintf(capped, sizeof(capped), ", a random %lu of them (the queue holds %lu)", (unsigned long)took,
             (unsigned long)QueueModel::kMaxEntries);
  }
  Serial.printf("[ui] shuffle all: %lu tracks%s (shuffle on; was %s)%s\n", (unsigned long)all.count, capped,
                was ? "on" : "off", ok ? "" : ": NO MEMORY");
  char text[96];
  if (took < all.count) {
    queueview::cappedText(queueview::Capped::Shuffle, took, all.count, text, sizeof(text));
  } else {
    trnf(text, sizeof(text), S::toast_shuffling, all.count, all.count);
  }
  toast(ok ? text : tr(S::toast_no_memory), ok);
}

void Ui::autoDjStart() {
  if (state_.libraryFenced) {  // the library update's fence: the index is the build's
    toast(tr(S::library_updating_wait), false);
    return;
  }
  const bool wasOn = state_.autoDj.on;
  const autodjview::Started r = host_.autoDjStart();
  if (r == autodjview::Started::Refused) {
    Serial.println("[ui] the Auto DJ button: the Play was refused (no memory)");
    toast(tr(S::toast_no_memory), false);
    return;
  }
  if (r == autodjview::Started::NoIndex) {
    // The index went (a remount, a check) since the button was drawn: the
    // empty state draws Shuffle all again at the next pass.
    warn(tr(S::toast_autodj_needs_index));
    return;
  }
  if (r == autodjview::Started::CardFailed) return;  // (the page says so, and Shuffle all takes the button's place)
  if (r != autodjview::Started::Played) {
    warn(tr(S::toast_autodj_no_start));  // (another tap draws again)
    return;
  }
  added_.clear();  // a new queue: nothing "added" to show in it
  char title[96], text[128];
  if (!player_.catalog().title(queue_.currentTrack(), title, sizeof(title))) {
    snprintf(title, sizeof(title), "%s", tr(S::queue_it));
  }
  if (player_.state() == PlayState::Waiting) {
    // The headphones aren't connected: it plays once they are (as the
    // Library's Play says it).
    if (state_.btName[0]) {
      trf(text, sizeof(text), S::library_waiting_for, state_.btName, title);
    } else {
      trf(text, sizeof(text), S::library_waiting_for_headphones, title);
    }
  } else {
    trf(text, sizeof(text), S::toast_autodj_started, title);
  }
  Serial.printf("[ui] the Auto DJ button: %s\n", title);
  autoDjToast(text, !wasOn);  // (Undo: the queue as it was, empty, and Auto DJ off if this tap switched it on)
}

void Ui::autoDjToast(const char* text, bool switchedOn) {
  toast(text, true);
  undoSwitchesDjOff_ = switchedOn;
}

autodjview::NewsWait::Up Ui::djUp() const {
  autodjview::NewsWait::Up up;
  up.editor = filters_.up();
  up.undo = toast_.up() && toast_.undo();
  up.toast = toast_.up();
  return up;
}

void Ui::autoDjNews(uint8_t news, uint32_t few) {
  // An Undo toast up (a Play's, the button's, "Auto DJ from here"'s): what
  // Auto DJ says waits until it goes, so its Undo stays offered for its 4
  // s (a pick lands half a second after a one-track Play starts). The
  // failure pause, a note, at once. The mood filters' editor up: all of
  // it waits, the failure pause too (MOOD-FILTERS.md 4.5): a toast would
  // cover its title row, and the count it shows says the rest. What
  // waited and this take make one toast; a Few or a Jump it outranks
  // waits for it to go (autodjview::NewsWait), not lost.
  autodjview::Toast t;
  if (!djNews_.take(news, djUp(), few, &t)) return;
  // A count (Toast::count): its text says it, "Only 37 tracks fit: they
  // will repeat"; the others are their message as it is.
  char text[160];
  const char* say = tr(t.text);
  if (t.count) {
    trnf(text, sizeof(text), t.text, t.count, t.count);
    say = text;
  }
  if (t.warn) {
    warn(say);
  } else {
    toast(say, false);
  }
}

queueview::QuietKeys Ui::djKeys() const {
  queueview::QuietKeys q;
  q.ctx = &host_;
  q.has = [](uint32_t key, const void* ctx) { return static_cast<const UiHost*>(ctx)->autoDjMarked(key); };
  return q;
}

void Ui::showTop() {
  // The first boot's picker left without a pick (a tab tapped): English
  // stays, nothing saved (the next boot asks again only if the tips haven't
  // been seen by then); the touch check and the tips follow.
  if (pickerDue_ && started_ && !pickerOnTop()) {
    pickerDue_ = false;
    nav_.popToRoot(NavModel::Tab::Output);
    Serial.println("[ui] first boot: the language picker left: English (Output > Language changes it)");
  }
  Page* next = pageFor(nav_.top().kind);
  if (page_) page_->leave();
  // A page that shortened the list band (the Queue's selection mode) has
  // let it go in leave(); be sure. The Library's status line (updateStatus())
  // gets its shorter band here, before the page's first frame: it used to
  // get it a pass later, a second full band draw and a flash on every
  // Library page open while the card worker had something to say (the
  // whole boot's walk on a big card: 34 + 45 ms more, MEASURED).
  char status[64] = "";
  if (next == &libraryPage_ && !browse_) librarytext::statusText(state_.libraryStatus, status, sizeof(status));
  const int band = status[0] ? ListView::kHeight - kStatusH : ListView::kHeight;
  if (list_.height() != band) setListBand(band);
  statusBand_ = status[0] != 0;
  statusHash_ = 0;
  ensureScroller(next->scrolls());
  page_ = next;
  applyCover();
  page_->enter(nav_.top());
  // The first frame at once, whatever the cadence: navigation should feel instant.
  if (page_->update(nowMs_, true, false)) clock_.drawn(nowMs_);
  if (!hud_.up()) tabBar_.update(tabState(nowMs_));
  // Under the glance (a console tab change or browse, a library update's
  // reset, in the dark: none wakes the screen), that first frame went past
  // the gfx gate (a list pushes to the panel itself), and setListBand() may
  // have filled the band and written the scroll registers: the glance whole
  // again, at once, through the mapping as it is now. (resume() draws it
  // itself: suspend() ended glancing_.)
  if (glancing_) drawGlance(true);
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
  // the Albums list's filtered ids too (its filter stays: MOOD-FILTERS.md
  // 4.2), and the Library's pages start over at the root.
  libraryPage_.dropFilteredAlbums();
  thumbs_.libraryChanged();
  resetLibraryTab();
  if (started_ && !suspended_ && list_.attached()) list_.reload();  // (the Queue's rows: new keys)
  // Under the glance: its cover's album id is another's now. And with the
  // Library tab on top, resetLibraryTab()'s showTop() drew the list's first
  // frame at once, past the gfx gate (ListView pushes to the panel itself),
  // and its band may have been set again (the scroll registers written):
  // the glance whole again, through the mapping as it is now. (The list
  // shows for that one band render, tens of ms.)
  if (glancing_) drawGlance(true);
}

void Ui::libraryUpdating() {
  // The snapshot says so from now (the pages' lines): taken again, so the
  // pages drawn below see it. The index goes behind the fence: the Albums
  // list's filtered ids are dropped first (its filter stays: 4.2).
  libraryPage_.dropFilteredAlbums();
  host_.snapshot(state_);
  resetLibraryTab();
  if (started_ && !suspended_ && list_.attached()) list_.reload();
  // (Under the glance, as libraryChanged(): the fence's placeholder cover,
  // and the list frame showTop() drew over it.)
  if (glancing_) drawGlance(true);
}

void Ui::browse(LibraryIndex* index) {
  libraryPage_.dropFilteredAlbums();  // (another index's ids)
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
  const int bottom = jumpGrid_.up() || coach_.up() || filters_.up() ? top
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
  if (filters_.up()) {
    // Its owner hears whether the filter changed (the Albums list reloads
    // at its top once: MOOD-FILTERS.md 7.5); not at a suspend() (4.3: the
    // filter is already applied, and enter() at the resume reads it).
    FilterOwner* owner = filters_.owner();
    const bool changed = filters_.changed();
    filters_.close();
    closed = true;
    // Auto DJ's filter changed, its owner told: the filter's own news that
    // waited under the editor are an older filter's (its few tracks, a
    // jump); the change says its own, from its count (onFiltersClosed():
    // AutoDj::moodSettled(), MOOD-FILTERS.md 4.5). The rest still waits
    // for the next pass. (At a suspend() the owner isn't told: what waited
    // stays, Few said by the count at the resume.)
    if (notify && changed && owner && owner->filterScope() == filtergrid::Scope::Tracks) djNews_.filterChanged();
    Serial.printf("[ui] filters: the editor closed%s\n", changed ? ", the filter changed" : "");
    if (notify && owner) owner->onFiltersClosed(changed);
  }
  if (closed) applyCover();
  return closed;
}

void Ui::repaintList() {
  if (thumbsStale_) {
    list_.invalidate();
  } else {
    list_.redraw();
  }
  thumbsStale_ = false;
}

void Ui::repaintUnder() {
  applyCover();
  statusHash_ = 0;  // the status line again, if it shows (a sheet covered it)
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
  } else if (filters_.up()) {
    filters_.drawTop();  // (over the page's header row: its title row and row 1's top)
  } else if (page_) {
    page_->repaintHeader();
  }
  // A page with no header (Now Playing, the Dance tab) draws all of itself
  // again for the header row (Page::repaintHeader()'s default): every sheet
  // over it is drawn again too, else it stays up (live) but unseen. Seen on
  // the device: +10 min on the fade toast over the Sleep timer sheet left
  // its pills invisible over the transport, and a tap on "..." hit Turn off.
  const bool wholePage = !jumpGrid_.up() && !coach_.up() && !filters_.up() && page_ && !page_->hasHeader();
  if (sheet_.up() && (wholePage || sheet_.top() < oldBottom)) sheet_.draw();  // a 4-row sheet would reach the header row
  if (wholePage && sleepSheet_.up()) sleepSheet_.draw();
  if (wholePage && volumeSheet_.up()) volumeSheet_.draw();
  if (dialog_.up()) dialog_.draw();
}

void Ui::toast(const char* text, bool undo, uint32_t viewKey) {
  viewKey_ = viewKey;
  undoSwitchesDjOff_ = false;  // (autoDjToast() sets it after)
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

void Ui::refuse(const char* text) {
  const queueview::ToastButtons keep =
      queueview::keptByRefusal(toast_.up(), toast_.undo(), toast_.view(), viewKey_, queue_.undoable());
  if (!started_ || suspended_) {
    Serial.printf("[ui] toast not shown (the UI isn't on screen): %s\n", text);
    return;
  }
  // Not toast(): the View kept was noted as an add when it was offered.
  viewKey_ = keep.viewKey;
  const int was = toast_.bottom();
  toast_.show(text, keep.undo, keep.viewKey != QueueModel::kNone, accent(), nowMs_);
  uncover(was);
  Serial.printf("[ui] toast: %s%s%s (refused: the last toast's buttons kept)\n", text, keep.undo ? " (Undo)" : "",
                keep.viewKey != QueueModel::kNone ? " (View)" : "");
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
                   const char* const* details, int primary, int danger, const uint16_t* inks) {
  endPageTouch();
  closeModal(true);
  sheetOwner_ = owner;
  for (SheetFollow& f : sheetFollow_) f = SheetFollow::None;
  sheetStays_ = 0;
  sheetOpenedMs_ = millis();
  sheet_.open(title, rows, n, accent(), details, primary, danger, inks);
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

uint16_t Ui::autoDjDetail(char* buf, size_t size) const {
  if (!buf || size == 0) return col::DIM;
  char whole[96];
  const autodjview::Ink ink = autodjview::sheetDetail(state_.autoDj, whole, sizeof(whole));
  if (ink != autodjview::Ink::Accent) {
    snprintf(buf, size, "%s", whole);
    return ink == autodjview::Ink::Amber ? col::AMBER : col::DIM;
  }
  // What is set, cut with "…" where the label leaves off (frame dj-menu),
  // so it stays beside it (Sheet::renderRow() puts a detail too wide there
  // under the label, whole). English's "On · Relaxed · Instrumental" is
  // 178 px of 213: whole (test_ui_library).
  Fonts& f = Fonts::instance();
  const int room = sheet::detailRoom(f.width(Font::Body, tr(S::now_playing_autodj)));
  textfit::fit(f.fit(Font::Small), whole, std::strlen(whole), buf, size, room);
  return accent();
}

void Ui::followSheet(int pressed) {
  if (!sheet_.up()) return;
  bool pressedDrawn = false;
  for (int i = 0; i < Sheet::kMaxRows; ++i) {
    const char* text = nullptr;
    char dj[96];
    uint16_t ink = col::DIM;
    switch (sheetFollow_[i]) {
      case SheetFollow::Sleep: text = state_.sleepRow; break;
      case SheetFollow::Shuffle: text = tr(state_.shuffle ? S::now_playing_on : S::now_playing_off); break;
      case SheetFollow::Repeat: text = tr(uitext::repeatMode(state_.repeat)); break;
      case SheetFollow::AutoDj:
        ink = autoDjDetail(dj, sizeof(dj));
        text = dj;
        break;
      default: continue;
    }
    if (sheet_.setDetail(i, text, ink) && i == pressed) pressedDrawn = true;
  }
  if (pressed >= 0 && !pressedDrawn) sheet_.drawRow(pressed);
}

void Ui::sleepTitle(char* buf, size_t size) const {
  // SleepTimer::titleText()'s: lower case after the colon, as the toasts
  // say it ("Sleep timer: end of track"); the "..." row's detail alone is
  // capitalised ("End of track").
  snprintf(buf, size, "%s", state_.sleepTitle);
}

void Ui::openSleepSheet() {
  endPageTouch();
  closeModal(true);
  char title[96];
  sleepTitle(title, sizeof(title));
  sheetOpenedMs_ = millis();  // (the settle, as every sheet's)
  sleepSheet_.open(state_.sleepPick, state_.sleepRunning, state_.sleepCanExtend, title, accent::NowPlaying);
  applyCover();
  Serial.printf("[ui] sleep timer sheet (%s)\n", state_.sleepRow);
}

void Ui::openFilters(FilterOwner* owner) {
  if (!owner || !started_ || suspended_) return;
  endPageTouch();
  closeModal(true);
  // A toast up goes, its Undo with it, as for the Dance tab (showDance()):
  // its rows are the editor's title row (MOOD-FILTERS.md 3.2, 4.3). Its
  // cover goes before the first frame: the editor draws under a toast (not
  // `over`), so rows 36-71 would keep the toast's pixels while the unseen
  // Clear, count and cross took the taps there.
  if (toast_.up()) {
    if (toast_.undo()) queue_.dropUndo();
    toast_.hide();
    applyCover();
  }
  sheetOpenedMs_ = millis();  // (the settle, as a sheet's: 7.4)
  const bool tracks = owner->filterScope() == filtergrid::Scope::Tracks;
  filters_.open(owner, tracks ? accent::Output : accent::Library);
  applyCover();
  Serial.printf("[ui] filters: the editor (%s, filter 0x%03X)\n", tracks ? "Auto DJ's tracks" : "the albums",
                static_cast<unsigned>(filters_.word()));
}

void Ui::refreshSleepSheet() {
  if (!sleepSheet_.up()) return;
  char title[96];
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
  const char* name = !bt ? tr(S::volume_speaker) : state_.btName[0] ? state_.btName : tr(S::volume_headphones);
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
      char title[64];
      const char key[2] = {letter, 0};
      trf(title, sizeof(title), S::jump_second_title, jumpSource_->jumpTitle(), key, rows);
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
  // "SPYDRONE disconnected" if it fits beside the icon (236 px:
  // uitext::kDialogIconTitleW); else the name goes into the body
  // ("WH-1000XM4 disconnected" is 9 px too wide). With no name, "Headphones
  // disconnected" (a language's too wide for it: "Headphones:" in the body
  // too).
  const bool named = state_.btName[0] != 0;
  char title[96], body[192];
  if (named) {
    trf(title, sizeof(title), S::dialog_lost_title, state_.btName);
  } else {
    trf(title, sizeof(title), S::dialog_lost_title_generic);
  }
  if (Fonts::instance().width(Font::Bold, title) <= Dialog::titleRoom(true)) {
    trf(body, sizeof(body), S::dialog_lost_body);
  } else if (named) {
    trf(title, sizeof(title), S::dialog_lost_title_generic);
    trf(body, sizeof(body), S::dialog_lost_body_named, state_.btName);
  } else {
    trf(body, sizeof(body), S::dialog_lost_body_headphones);
  }
  const char* const buttons[2] = {tr(S::dialog_lost_use_speaker), tr(S::dialog_lost_ok)};
  endPageTouch();
  closeModal(true);
  dialogOwner_ = this;
  dialog_.open(title, body, buttons, 2, col::AMBER);
  dialog_.setIcon(&icons::kHeadphones, col::AMBER, true);
  lostDialog_ = true;
  ownDialog_ = OwnDialog::Lost;
  applyCover();
  updateLostDialog();
}

void Ui::headphonesConnected() {
  input_.connectedTick();
  if (!started_ || suspended_) return;
  char text[96];
  if (state_.btName[0]) {
    trf(text, sizeof(text), S::toast_now_playing_on, state_.btName);
  } else {
    trf(text, sizeof(text), S::toast_now_playing_on_headphones);
  }
  toast(text, false);
}

void Ui::updateLostDialog() {
  if (!lostDialog_ || !dialog_.up()) return;
  const BtLink& l = state_.btLink;
  char line[96];
  if (l.phase == BtLink::Phase::Paging && l.attempt > 0) {
    trf(line, sizeof(line), S::dialog_lost_trying, l.attempt, std::max(l.attempt, l.attempts));
  } else if (l.phase == BtLink::Phase::Off) {
    trf(line, sizeof(line), S::dialog_lost_not_trying);
  } else if (l.phase == BtLink::Phase::Resting) {
    // (Lost: resting only after the whole back-off; they may be back in
    // range with their own reconnect given up.)
    trf(line, sizeof(line), S::dialog_lost_resting);
  } else {
    trf(line, sizeof(line), S::dialog_lost_looking);
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
  const bool named = state_.btName[0] != 0;
  char title[96], body[192];
  if (named) {
    trf(title, sizeof(title), S::dialog_failed_title, state_.btName);
  } else {
    trf(title, sizeof(title), S::dialog_failed_title_headphones);
  }
  // No icon: the title's whole width (a name as long as "WH-1000XM4" fits).
  if (Fonts::instance().width(Font::Bold, title) <= Dialog::titleRoom(false)) {
    trf(body, sizeof(body), S::dialog_failed_body);
  } else {
    // (A long name: it goes into the body, as headphonesLost() does; with
    // none, English's "Couldn't reach the headphones" is too wide too.)
    trf(title, sizeof(title), S::dialog_failed_title_them);
    if (named) {
      trf(body, sizeof(body), S::dialog_failed_body_named, state_.btName);
    } else {
      trf(body, sizeof(body), S::dialog_failed_body_headphones);
    }
  }
  const char* const buttons[2] = {tr(S::now_playing_play_on_speaker), tr(S::dialog_failed_try_again)};
  endPageTouch();
  closeModal(true);
  dialogOwner_ = this;
  dialog_.open(title, body, buttons, 2, col::AMBER);
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
  const bool titled = player_.catalog().title(f.track, title, sizeof(title));
  // Why, when it was the track's sample rate: a refused rate isn't a
  // broken file, and at 160 MHz a setting would play it. Else the
  // backend's own few words when it has them (an Opus file refused at its
  // open: "surround Opus isn't supported"), else "can't play it".
  char why[96];
  if (f.rate.hz != 0 && f.rate.needsCpu) {
    trf(why, sizeof(why), S::toast_why_cpu);
  } else if (f.rate.hz != 0) {
    char rate[24];
    RateConverter::rateText(f.rate.hz, rate, sizeof(rate));
    trf(why, sizeof(why), S::toast_why_rate, rate);
  } else if (f.note[0]) {
    snprintf(why, sizeof(why), "%s", f.note);
  } else {
    trf(why, sizeof(why), S::toast_why_cant_play);
  }
  char text[128];
  if (titled) {
    trf(text, sizeof(text), S::toast_skipped, title, why);
  } else {
    trf(text, sizeof(text), S::toast_skipped_untitled, why);
  }
  warn(text);
  host_.wakeScreen("a track couldn't be played");
  // Its Queue row's "!" (the whole band, 50-78 ms, until 0.12.0).
  if (page_ == &queuePage_) {
    const uint32_t pos = queue_.positionOf(f.key);
    if (pos != QueueModel::kNone) {
      list_.refreshRow(pos);
    } else {
      list_.refreshAll();
    }
  }
}

// The track didn't start because the card is out: the player holds there and tries again
// (nothing skipped, so no note of noteFailures()'). Its words are the no-card screen's Try
// again note's, a toast already in every language.
void Ui::noteCardHolds() {
  const uint32_t begun = player_.cardHolds().begun;
  if (begun == cardHoldsSeen_) return;
  cardHoldsSeen_ = begun;
  warn(tr(S::card_none_still));
  host_.wakeScreen("the card is out");
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
      warn(tr(S::toast_no_headphones));
    } else if (f.kind == ButtonPolicy::Hud::Output) {
      char line[96];
      if (f.refused) {
        trf(line, sizeof(line), S::hud_silent_mode);
      } else if (f.toBluetooth && state_.btConnected) {
        trf(line, sizeof(line), S::hud_bluetooth, state_.btName);
      } else if (f.toBluetooth) {
        trf(line, sizeof(line), S::hud_bluetooth_connecting);
      } else {
        trf(line, sizeof(line), f.paused ? S::hud_speaker_paused : S::hud_speaker);
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
  s.label = tabLabelFit(s.active);
  return s;
}

tabbar::LabelFit Ui::tabLabelFit(int tab) const {
  const int lang = static_cast<int>(uistr::lang());
  if (tabFitsLang_ != lang) {
    Fonts& f = Fonts::instance();
    for (int t = 0; t < tabbar::kTabs; ++t) tabFits_[t] = tabbar::labelFit(t, f.width(Font::Small, tabbar::label(t)));
    tabFitsLang_ = lang;
  }
  return tabFits_[tab];
}

// A frame longer than this is a hitch anyone sees (two refreshes and a
// half; counted per motion: it was the 30 fps cadence missed, until 0.14.0).
static constexpr uint32_t kSlowFrameUs = 35000;

// A pass or an event longer than this is logged with its parts.
static constexpr uint32_t kSlowPassUs = 35000;

void Ui::loop(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (!started_ || suspended_) return;
  const uint32_t passT0 = micros();
  uint32_t snapshotUs = 0, tabUs = 0, pageUs = 0, statusUs = 0, thumbsUs = 0;
  bool frame = false;
  // The pass's parts, and the whole, on the way out.
  struct Timed {
    Ui& ui;
    uint32_t t0;
    uint32_t& snapshotUs;
    uint32_t& tabUs;
    uint32_t& pageUs;
    uint32_t& statusUs;
    uint32_t& thumbsUs;
    bool& frame;
    ~Timed() {
      const uint32_t us = micros() - t0;
      PassCost& c = ui.cost_;
      ++c.passes;
      c.passSumUs += us;
      c.passMaxUs = std::max(c.passMaxUs, us);
      c.snapshotMaxUs = std::max(c.snapshotMaxUs, snapshotUs);
      c.tabMaxUs = std::max(c.tabMaxUs, tabUs);
      c.pageMaxUs = std::max(c.pageMaxUs, pageUs);
      c.statusMaxUs = std::max(c.statusMaxUs, statusUs);
      c.thumbsMaxUs = std::max(c.thumbsMaxUs, thumbsUs);
      if (us > kSlowPassUs) {
        Serial.printf("[ui] slow pass %lu us on %s: %s %lu us, snapshot %lu, tab bar %lu, status %lu, covers %lu\n",
                      (unsigned long)us, ui.glancing_ ? "the glance" : ui.page_ ? ui.page_->name() : "no page",
                      frame ? "a frame" : "the page (no frame)",
                      (unsigned long)pageUs, (unsigned long)snapshotUs, (unsigned long)tabUs, (unsigned long)statusUs,
                      (unsigned long)thumbsUs);
      }
    }
  } timed{*this, passT0, snapshotUs, tabUs, pageUs, statusUs, thumbsUs, frame};
  host_.snapshot(state_);
  snapshotUs = micros() - passT0;
  // The first boot, its language picked (or its picker left), and the
  // touch check main.cpp opened then closed (it suspended the UI until
  // now): the tips, and the language's note over them.
  if (tipsAfterPicker_ && !pickerDue_) {
    tipsAfterPicker_ = false;
    if (!coachSeen_) showCoach();
    if (noteAfterPicker_) toast(tr(S::language_machine_note), false);
    noteAfterPicker_ = false;
  }
  // identify()'s banner, its 5 s over: everything drawn again.
  if (identifying_ && static_cast<int32_t>(nowMs - identifyUntilMs_) >= 0) {
    endIdentify();
    Serial.println("[ui] identify: over");
  }

  updateHud(nowMs);
  noteFailures();
  noteCardHolds();
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
  // A shuffle toggle drops the queue's undo (docs/QUEUE-MODES.md 2.6), the
  // console's qu uses it, and Auto DJ's pick can drop it (queueUndoDropped()):
  // an Undo toast still up would answer "Nothing to undo", so it goes, its
  // line saying which. (Shuffle all's toast
  // stays: its Play set the mode as part of the edit, and its Undo puts
  // both back; qu's undo of it changes the mode back, no toggle.) And no
  // badge flash for a change of mode: Off can grow "up next" without
  // adding anything.
  const queueview::UndoWatch::Gone gone =
      undoWatch_.pass(state_.shuffle, toast_.up() && toast_.undo(), queue_.undoable());
  if (gone != queueview::UndoWatch::Gone::Stays) {
    const int was = toast_.bottom();
    toast_.hide();
    uncover(was);
    using Gone = queueview::UndoWatch::Gone;
    Serial.printf("[ui] the Undo toast went: %s\n", gone == Gone::Undone    ? "undone from the console"
                                                    : gone == Gone::Dropped ? "Auto DJ's pick took the undo"
                                                                            : "a shuffle toggle took the undo");
  }
  const bool toggled = undoWatch_.modeChanged();
  // What Auto DJ had to say while an Undo toast was up: now it has gone
  // (moot if Auto DJ was switched off meanwhile). Nor while the mood
  // filters' editor is up (MOOD-FILTERS.md 4.5). A Few or Jump the last
  // one outranked: now that toast has gone.
  if (djNews_.due(djUp(), state_.autoDj.on)) autoDjNews(0, autodjview::fewCount(state_.autoDj));
  // Tracks added: the Queue badge flashes (not for Auto DJ's own appends:
  // one each time the last track starts; the engine design's 2.14).
  const bool contentChanged = state_.contentVersion != lastContent_;
  if (queueview::badgeFlashes(lastUpNext_, state_.upNext, contentChanged, toggled, state_.autoDj.adds - lastDjAdds_)) {
    badgeUntilMs_ = nowMs + 1500;
  }
  lastContent_ = state_.contentVersion;
  lastUpNext_ = state_.upNext;
  lastDjAdds_ = state_.autoDj.adds;
  if (dark_ || identifying_) {
    // The screen is off, or identify()'s banner covers it: nothing drawn
    // (the page, the tab bar, frames); what the cover worker made comes
    // in, and no new job starts. The glance over the dark UI: its own
    // redraws only (what changed: the second, the track, a cover, the
    // status), counted as the page's.
    if (dance_.active()) {
      dance_.setActive(false);  // (a console tab change while dark)
      danceWasOn_ = true;
    }
    // (A list's rows are rendered again on the wake, as when a modal goes:
    // the row that asked for this cover still holds the placeholder.)
    const uint32_t arrived = thumbs_.loop(nowMs);
    if (arrived != Thumbs::kNone) thumbsStale_ = true;
    waves_.loop(nowMs);  // (first: a waveform read this pass is drawn this pass)
    if (glancing_) {
      const uint32_t t = micros();
      // Whole when the scroll's mapping changed under it (a writer the gate
      // doesn't stop: setListBand(), ensureScroller()); else what changed.
      drawGlance(scrollSig() != glanceScroll_, arrived);
      pageUs = micros() - t;
    }
    return;
  }
  if (!hud_.up()) {
    const uint32_t t = micros();
    tabBar_.update(tabState(nowMs));
    tabUs = micros() - t;
  }

  // The page: redraws what changed, and animation frames on the cadence.
  budget_ = governor_.update(nowMs, state_.ringMs, state_.underruns, state_.ringMatters);
  // (A list's frames have their own period; another page's animation, Now
  // Playing's scrub, keeps the governor's 30 a second.)
  clock_.setPeriod(budget_.level == ScrollGovernor::Level::Normal && list_.attached() ? listFrameMs() : budget_.frameMs);
  const bool due = budget_.draw && clock_.due(nowMs);
  const bool modal = modalUp();
  const uint32_t t0 = micros();
  frame = page_ && !modal && page_->update(nowMs, due, budget_.wholeRows);
  pageUs = micros() - t0;
  // A row of a scrub's band (the band is drawn a row a pass): no frame of
  // the clock's, unless the thumb was drawn with it.
  const ListView::FrameCost* cost = frame && list_.attached() ? &list_.lastFrame() : nullptr;
  const bool piece = cost && cost->kind == ListView::FrameCost::Kind::Piece;
  if (piece) {
    const uint32_t us = micros() - t0;
    if (cost->rail) clock_.drawn(nowMs);
    if (motion_.on) {
      if (cost->rail) ++motion_.thumbs;
      if (us > motion_.heldMaxUs) motion_.heldMaxUs = us;
    }
  } else if (frame) {
    clock_.drawn(nowMs);
    ++frames_;
    ++framesInWindow_;
    if (motion_.on) {
      uint32_t us = micros() - t0;
      if (us > motion_.heldMaxUs) motion_.heldMaxUs = us;
      if (cost && cost->waitUs < us) us -= cost->waitUs;  // (its work, not its wait for the panel's scan)
      // A band drawn a row a pass: its time from its first row to its last.
      if (cost && cost->kind == ListView::FrameCost::Kind::Band && cost->bandUs) {
        us = cost->bandUs;
        if (cost->rail) ++motion_.thumbs;
      }
      if (list_.attached() && list_.lastFrame().kind == ListView::FrameCost::Kind::Rail) {
        // A scrub's frame of the rail alone (ScrubPacer): counted apart.
        ++motion_.railFrames;
        motion_.railSumUs += us;
        if (us > motion_.railMaxUs) motion_.railMaxUs = us;
      } else {
        ++motion_.frames;
        motion_.sumUs += us;
        if (us > motion_.maxUs) motion_.maxUs = us;
        if (us > kSlowFrameUs) ++motion_.slow;
        // A fling's cadence: the time between its frames while every
        // refresh moves it (over ~150 px/s: 2 px a frame).
        const uint32_t at = micros();
        const KineticScroll& k = list_.scroll();
        if (list_.attached() && k.phase() == KineticScroll::Phase::Flinging && std::fabs(k.velocity()) > 150.0f) {
          if (motion_.lastFrameUs) {
            const uint32_t gap = at - motion_.lastFrameUs;
            const uint32_t period = scan_.ok() && list_.scanSync() ? scan_.sync().panel().frameUs : clock_.period() * 1000;
            ++motion_.gaps;
            if (gap > motion_.gapMaxUs) motion_.gapMaxUs = gap;
            if (gap > period + period / 2) ++motion_.gapsLate;
          }
          motion_.lastFrameUs = at;
        } else {
          motion_.lastFrameUs = 0;
        }
      }
      if (us > 50000 && list_.attached() && !(cost && cost->bandUs)) {  // (not a band drawn a row a pass)
        const ListView::FrameCost& fc = list_.lastFrame();
        Serial.printf("[ui] slow frame %lu us: offset %ld to %ld, %lu items rendered in %lu us\n", (unsigned long)us,
                      (long)fc.from, (long)fc.to, (unsigned long)fc.renders, (unsigned long)fc.renderUs);
      }
    }
  }
  trackMotion(nowMs);
  // Between frames: the row the list moves toward, rendered ahead while
  // the next frame is far enough off (ListView::renderAhead).
  if (page_ && !modal && list_.attached() && budget_.draw && page_->animating()) {
    list_.renderAhead(clock_.msUntilDue(millis()));
  } else if (page_ && !modal && list_.attached() && !frame && thumbs_.idle()) {
    // At rest: the rows just outside the band, for the next swipe's first frame.
    list_.renderNeighbours();
  }
  // Covers: one that arrived is drawn where it shows (a modal's page draws
  // it all when the modal goes). New ones are the card worker's, never
  // started while a list moves (app/CardTasks).
  const uint32_t t1 = micros();
  const uint32_t arrived = thumbs_.loop(nowMs);
  if (arrived != Thumbs::kNone) {
    if (list_.attached()) list_.forgetHidden();  // (a row rendered ahead holds the placeholder)
    if (page_ && !modalUp()) {
      page_->thumbReady(arrived);
    } else {
      thumbsStale_ = true;  // a list's rows are rendered again when the modal goes (repaintList())
    }
  }
  // A waveform's read: Now Playing sees it by its version(), a modal or not.
  waves_.loop(nowMs);
  const uint32_t t2 = micros();
  thumbsUs = t2 - t1;
  updateStatus(nowMs);
  statusUs = micros() - t2;
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
// Playing while a finger scrubs its seek bar: "[ui] scrub: ..."), and a
// finger on a list's A-Z rail, from its landing to the band's last catch-up
// ("[ui] rail scrub: ...": the band frames, then the frames of the rail
// alone between them).
void Ui::trackMotion(uint32_t nowMs) {
  // (Under a modal the page draws no frame, so a motion ends there: a
  // fling stopped by a finger on the rail that then opened the jump grid
  // logged 46 s of "rail scrub" on the device, the grid's whole stay.)
  const bool rail = list_.attached() && list_.railTouch();
  const bool moving = page_ && !modalUp() && (page_->animating() || rail);
  if (moving && !motion_.on) {
    motion_ = Motion{};
    list_.takeScanStats();  // (counted from here)
    motion_.on = true;
    motion_.list = list_.attached();
    motion_.startMs = nowMs;
    motion_.underruns = state_.underruns;
    motion_.ringMin = UINT32_MAX;
  }
  if (!motion_.on) return;
  if (rail) motion_.rail = true;
  if (state_.ringMatters && state_.ringMs < motion_.ringMin) motion_.ringMin = state_.ringMs;
  if (moving) return;
  motion_.on = false;
  const uint32_t ms = nowMs - motion_.startMs;
  if (motion_.frames + motion_.railFrames < 2) return;  // a tap's highlight, not a scroll
  char ring[24] = "n/a (not playing)";
  if (motion_.ringMin != UINT32_MAX) snprintf(ring, sizeof(ring), "%lu ms", (unsigned long)motion_.ringMin);
  const float meanMs = motion_.frames ? motion_.sumUs / 1000.0f / motion_.frames : 0.0f;
  if (motion_.rail) {
    // The thumb: its own frames (uir0's pacing), and the ones drawn with a
    // row of a band.
    const uint32_t thumbs = motion_.railFrames + motion_.thumbs;
    char alone[56] = "";
    if (motion_.railFrames) {
      snprintf(alone, sizeof(alone), "; alone mean %.1f max %.1f ms", motion_.railSumUs / 1000.0f / motion_.railFrames,
               motion_.railMaxUs / 1000.0f);
    }
    Serial.printf("[ui] rail scrub: %lu ms, %lu band frames (%.1f a second; a band mean %.1f max %.1f ms), the loop "
                  "held %.1f ms at most; the thumb drawn %lu times (%.1f a second%s), ring min %s, underruns +%lu, "
                  "governor %s\n",
                  (unsigned long)ms, (unsigned long)motion_.frames, ms ? motion_.frames * 1000.0f / ms : 0.0f, meanMs,
                  motion_.maxUs / 1000.0f, motion_.heldMaxUs / 1000.0f, (unsigned long)thumbs,
                  ms ? thumbs * 1000.0f / ms : 0.0f, alone, ring,
                  (unsigned long)(state_.underruns - motion_.underruns), ScrollGovernor::name(budget_.level));
    return;
  }
  Serial.printf("[ui] %s: %lu ms, %lu frames (%.1f fps), draw mean %.1f max %.1f ms (%lu over 35), ring min %s, underruns +%lu, "
                "governor %s\n",
                motion_.list ? "scroll" : "scrub", (unsigned long)ms, (unsigned long)motion_.frames,
                ms ? motion_.frames * 1000.0f / ms : 0.0f, meanMs, motion_.maxUs / 1000.0f, (unsigned long)motion_.slow, ring,
                (unsigned long)(state_.underruns - motion_.underruns), ScrollGovernor::name(budget_.level));
  if (!motion_.list) return;
  // Its moves against the panel's scan, and a fling's cadence.
  const ListView::ScanStats st = list_.takeScanStats();
  if (st.frames + st.late + st.unsynced == 0) return;
  char gaps[72] = "";
  if (motion_.gaps) {
    snprintf(gaps, sizeof(gaps), "; the fling's frames %.1f ms apart at most, %lu of %lu late", motion_.gapMaxUs / 1000.0f,
             (unsigned long)motion_.gapsLate, (unsigned long)motion_.gaps);
  }
  Serial.printf("[ui] scroll moves: %lu in their window on the panel's scan (%lu of them with a strip row early), %lu "
                "late, %lu without the scan; the address %d-%d lines past the band (in the band for %lu)%s\n",
                (unsigned long)st.frames, (unsigned long)st.early, (unsigned long)st.late, (unsigned long)st.unsynced,
                st.phaseMax >= 0 ? st.phaseMin : 0, st.phaseMax >= 0 ? st.phaseMax : 0, (unsigned long)st.inBand, gaps);
}

LibraryIndex::Span Ui::shownTracks() const {
  if (!started_ || suspended_ || page_ != &libraryPage_ || browse_) return {};
  return libraryPage_.shownTracks();
}

// The Library's status line (docs/METADATA.md 3.3.6): Small, across the
// list's width (uitext::kStatusW), at the bottom of a Library page while
// the card worker has something to say; the list's band ends above it.
void Ui::updateStatus(uint32_t nowMs) {
  char text[64] = "";
  const bool library = page_ == &libraryPage_ && !browse_;
  if (library) librarytext::statusText(state_.libraryStatus, text, sizeof(text));
  // The band changes only while the list stands still (a fling isn't cut).
  if (list_.attached() && list_.animating()) return;
  // Under a modal the band stays as it is, even when the line ends: changing
  // it there would wipe the modal (it is drawn again when they go).
  if (modalUp() || jumpGrid_.up() || coach_.up()) return;
  if (!text[0]) {
    if (statusBand_) {
      statusBand_ = false;
      statusHash_ = 0;
      setListBand(ListView::kHeight);
      list_.invalidate();
    }
    return;
  }
  if (!statusBand_) {
    statusBand_ = true;
    statusHash_ = 0;
    setListBand(ListView::kHeight - kStatusH);
    list_.invalidate();
  }
  uint32_t h = 2166136261u;
  for (const char* p = text; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  if (h == 0) h = 1;
  // At most twice a second (at 10 Hz the redraw cost 5%: MEASURED, 3.3.6).
  if (h == statusHash_ || (statusHash_ != 0 && nowMs - statusAtMs_ < 500)) return;
  statusHash_ = h;
  statusAtMs_ = nowMs;
  drawStatusLine(text);
}

void Ui::drawStatusLine(const char* text) {
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  c.fillRect(0, 0, kW, kStatusH, col::HEAD);
  c.drawFastHLine(0, 0, kW, col::DIV);
  if (text[0]) f.draw(c, Font::Small, text, 8, kStatusH / 2 + 1, uitext::kStatusW, col::DIM, col::HEAD);
  gfx::push(c, 0, kH - kStatusH, kW, kStatusH);
}

uint32_t Ui::idleMs(uint32_t nowMs) const {
  // Nothing to draw (or the glance's seconds): touches read every 20 ms.
  if (dark_ && started_ && !suspended_) return 20;
  if (!started_ || suspended_ || !page_ || !page_->animating()) return 5;
  if (list_.bandUnderWay() && !modalUp()) return 1;  // a scrub's band: its next row at once
  return std::max<uint32_t>(1, std::min<uint32_t>(5, clock_.msUntilDue(nowMs)));
}

// ---- touch ----

void Ui::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!started_ || suspended_ || !e.isGlass()) return;
  // The glance: nothing on it acts. Only the console's scripted finger gets
  // here (the input layer swallows every real touch on the glance through
  // its lift), and its own wake (Why::Console) opens the whole screen at
  // this pass's end: the touch goes nowhere, and touch_ stays None, so its
  // Tap or Release after the wake does nothing either. (On an off screen
  // the scripted finger acts on the dark UI; on the glance it only opens
  // it. Its strip presses aren't the UI's: ButtonPolicy has them.)
  if (glancing_) return;
  // The event's cost (a tab's tap opens its page here: its header and
  // more), the longest kept for the console's ui, a slow one logged.
  struct Timed {
    Ui& ui;
    const InputEvent& e;
    uint32_t t0 = micros();
    ~Timed() {
      const uint32_t us = micros() - t0;
      if (us > ui.cost_.eventMaxUs) {
        ui.cost_.eventMaxUs = us;
        ui.cost_.eventMaxType = e.type;
      }
      if (us > kSlowPassUs) {
        Serial.printf("[ui] slow event %s at %d,%d: %lu us (page %s after it)\n", InputEvent::name(e.type), e.x, e.y,
                      (unsigned long)us, ui.page_ ? ui.page_->name() : "none");
      }
    }
  } timed{*this, e};
  if (identifying_) {
    // identify()'s banner: a touch ends it and does nothing else (the rest
    // of it goes nowhere: touch_ None until the next one lands).
    if (e.type == T::Down || (e.type == T::DragStart && e.fromStrip)) {
      touch_ = TouchOn::None;
      holdUnused_ = false;
      endIdentify();
      Serial.println("[ui] identify: ended by a touch");
    }
    return;
  }
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
    } else if ((sheet_.up() || sleepSheet_.up() || filters_.up()) &&
               static_cast<int32_t>(e.ms - sheetOpenedMs_) < static_cast<int32_t>(Sheet::kSettleMs)) {
      // The settle: a touch that starts this soon after a sheet opened is
      // the finger that opened it coming back (a double tap on "..." or on
      // the album row, or on the Albums list's first row: the editor's
      // cells are under it): the whole touch goes nowhere, no tick.
      touch_ = TouchOn::None;
      Serial.println("[ui] sheet: a touch right after it opened, ignored");
    } else if (sheet_.up()) {
      touch_ = TouchOn::Sheet;
    } else if (sleepSheet_.up()) {
      touch_ = TouchOn::Sleep;
    } else if (filters_.up()) {
      // A toast over its title row: a Down above the list's band goes
      // nowhere, so an unseen Clear (or count, or row 1's top) can't be hit
      // (MOOD-FILTERS.md 4.3). The sleep timer's buttons stay first, above.
      touch_ = toast_.up() && e.y < kListY ? TouchOn::None : TouchOn::Filters;
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
    case TouchOn::Filters: {
      const filtergrid::Cell c = filters_.onEvent(e);
      if (c == filtergrid::Cell::Cross) {
        tick();
        closeModal(true);  // (its owner told: a changed filter reloads the list)
        repaintUnder();
      } else if (c != filtergrid::Cell::None) {
        tick();  // (before the pass: the finger's answer never waits on it)
        filters_.tap(c);
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
          const bool shuffled = player_.shuffle();
          // (An add that pushed out what played: those come back where they were.)
          const uint32_t pushed = queue_.undoPushed();
          const bool undone = player_.undo();
          // (Shuffle all's: the mode it found comes back with the queue.)
          char back[48] = "";
          if (undone && pushed) {
            snprintf(back, sizeof(back), " (%lu played track%s back)", (unsigned long)pushed, pushed == 1 ? "" : "s");
          }
          Serial.printf("[ui] undo: %s%s%s\n", undone ? "done" : "nothing to undo",
                        player_.shuffle() == shuffled ? "" : shuffled ? " (shuffle off again)" : " (shuffle on again)",
                        back);
          // The Auto DJ button's or "Auto DJ from here"'s Play, which also
          // switched Auto DJ on: undone whole, Auto DJ off again. What
          // Auto DJ had to say about that queue goes with it (a Few that
          // waited stays: the filter's, said after this toast while on).
          djNews_.undone();
          if (undone && undoSwitchesDjOff_) {
            Serial.println("[ui] undo: Auto DJ off again (the tap undone had switched it on)");
            host_.autoDjSetOn(false);
          }
          undoSwitchesDjOff_ = false;
          toast_.show(tr(undone ? S::toast_undone : S::toast_nothing_to_undo), false, false, accent(), nowMs_);
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
  identifying_ = false;  // (the screen that takes over draws its own)
  glancing_ = false;     // (likewise; still wanted: resume() draws it again)
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
  endScroller();
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
  // The glance still up (a console spike command ran under it, and took no
  // screen of its own: a screen it took is lit, so the glance would have
  // gone): drawn whole again over whatever the tool and showTop()'s list
  // frame left, not left frozen for the rest of its 10 s.
  if (glanceWanted_ && dark_) {
    glancing_ = true;
    drawGlance(true);
  }
}

// ---- the screen off (ScreenPower) ----

void Ui::setDark(bool on) {
  if (on == dark_) return;
  dark_ = on;
  if (on) identifying_ = false;  // (the banner goes with the light; the wake draws it all)
  if (on) {
    if (!suspended_) gfx::setDark(true);
    if (!started_ || suspended_) return;
    // (No finger is on, as a rule: a touch keeps the screen lit. The PWR key
    // and the console's Ps0 can leave one on.)
    endPageTouch();
    touch_ = TouchOn::None;
    holdUnused_ = false;
    // A fling stops where it is (not carried on, or caught up, on the wake).
    if (list_.attached() && list_.animating()) list_.scrollTo(list_.offset());
    motion_.on = false;  // (not logged: its time would count the whole dark)
    if (page_) page_->screenOff();
    if (dance_.active()) {
      dance_.setActive(false);
      danceWasOn_ = true;
    }
    Serial.println("[ui] dark: nothing drawn until the screen wakes");
    return;
  }
  gfx::setDark(false);
  // (ScreenControl ends the glance first, setGlance(false): this makes sure.
  // From the glance, setDark(true) is nothing: the UI is dark already, so
  // the way to off ends the glance itself, before the sleep-in.)
  glancing_ = false;
  glanceWanted_ = false;
  if (!started_ || suspended_) return;
  redrawAll();
}

// Everything drawn again, as it is now: nothing reached the panel while it
// was off, and what was drawn before (its GRAM) may be stale.
void Ui::redrawAll() {
  const uint32_t t0 = millis();
  statusHash_ = 0;
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
    // After a glance the panel's memory holds the glance, not this page's
    // last frame: a list page's band can't wait for the next frame, nor for
    // the modal over it to go (its glance pixels would show around a dialog,
    // above a short sheet, and for a frame on every wake). Its band whole
    // now, as showTop()'s first frame (the A-Z rail with it), before what
    // is over it; and the Library's status line, which updateStatus() leaves
    // alone under a modal (drawn again there once it may: statusHash_ 0).
    // The filters' editor covers the content: nothing of it shows.
    if (gramStale_ && list_.attached() && !filters_.up()) {
      if (list_.update(nowMs_, true, false)) clock_.drawn(nowMs_);
      if (statusBand_) {
        char text[64] = "";
        if (page_ == &libraryPage_ && !browse_) librarytext::statusText(state_.libraryStatus, text, sizeof(text));
        drawStatusLine(text);
      }
    }
  }
  gramStale_ = false;
  if (sheet_.up()) sheet_.draw();
  if (sleepSheet_.up()) sleepSheet_.draw();
  if (filters_.up()) filters_.draw();
  if (volumeSheet_.up()) volumeSheet_.draw();
  if (dialog_.up()) dialog_.draw();
  if (toast_.up()) toast_.draw();
  Serial.printf("[ui] awake: drawn again in %lu ms\n", (unsigned long)(millis() - t0));
}

// ---- the glance ("Touch when off": Track) ----

bool Ui::setGlance(bool on) {
  if (!on) {
    glanceWanted_ = false;
    if (!glancing_) return false;
    glancing_ = false;
    glance_.leave();
    Serial.printf("[ui] glance: over after %lu ms\n", (unsigned long)(nowMs_ - glanceSinceMs_));
    return false;
  }
  if (!started_ || suspended_ || !dark_) {
    // (Lit with nothing drawn, the panel would show its stale memory for 10
    // s: ScreenControl keeps the backlight off instead.)
    Serial.printf("[ui] glance: not drawn (%s)\n", !started_    ? "the UI isn't up"
                                                   : suspended_ ? "another screen has the display"
                                                                : "the UI isn't dark");
    return false;
  }
  const uint32_t t0 = millis();
  // A touch the dark UI still had (the console's scripted finger: it acts
  // in the dark) ends, as when the screen went dark: nothing reaches it now.
  endPageTouch();
  touch_ = TouchOn::None;
  holdUnused_ = false;
  glancing_ = true;
  glanceWanted_ = true;
  gramStale_ = true;
  glanceSinceMs_ = nowMs_;
  // The panel keeps its scroll registers through sleep-in; sent again
  // anyway (as redrawAll()).
  if (vscroll_.active()) vscroll_.resend();
  drawGlance(true);
  Serial.printf("[ui] glance: drawn in %lu ms (the track; nothing on it acts)\n", (unsigned long)(millis() - t0));
  return true;
}

void Ui::drawGlance(bool all, uint32_t arrived) {
  // The glance draws all of y 0-239, the toast's rows (36-71) with the
  // rest: a toast up in the dark stays unseen (redrawAll() brings it on the
  // wake), so its cover goes for these draws (the page draws without
  // `over`). The gate is lifted for them alone, as identify()'s banner:
  // everything else the dark UI draws meanwhile still goes nowhere.
  gfx::setCover(0, 0);
  gfx::setDark(false);
  tabBar_.updateGlance(tabState(nowMs_), all);
  if (all) {
    glance_.enter(glanceRef_);  // all of it (its 98 x 98 cover sprite made the first time)
    glanceScroll_ = scrollSig();
  } else {
    glance_.update(nowMs_, false, false);
    if (arrived != Thumbs::kNone) glance_.thumbReady(arrived);
  }
  gfx::setDark(dark_);
  applyCover();
}

uint32_t Ui::scrollSig() const {
  // (The UI's lists are the only scroller while it draws: a spike tool's
  // suspends it.)
  if (!vscroll_.active()) return 0;
  const VScrollMap& m = vscroll_.map();
  return 1u | static_cast<uint32_t>(ListScroller::activeVsp()) << 1 | static_cast<uint32_t>(m.top()) << 9 |
         static_cast<uint32_t>(m.height()) << 17;
}

// ---- a computer's @identify ----

bool Ui::identify(const char* label, uint32_t nowMs) {
  if (!started_ || suspended_ || dark_) return false;
  std::snprintf(identifyLabel_, sizeof(identifyLabel_), "%s", label);
  identifyUntilMs_ = nowMs + kIdentifyMs;
  if (!identifying_) {
    // As the screen going dark: the page's touch ends, a fling stops where
    // it is (the list's band must not move under the banner), the dancer
    // stops (it draws its own frames); redrawAll() brings it back.
    endPageTouch();
    touch_ = TouchOn::None;
    holdUnused_ = false;
    if (list_.attached() && list_.animating()) list_.scrollTo(list_.offset());
    if (dance_.active()) {
      dance_.setActive(false);
      danceWasOn_ = true;
    }
    identifying_ = true;
  }
  gfx::setDark(false);
  drawIdentify();
  gfx::setDark(true);  // the UI's own drawing goes nowhere until it ends
  input_.identifyBuzz();
  return true;
}

void Ui::drawIdentify() {
  const uint16_t blue = accent::Output;
  const int f = uitext::kIdentifyFrame;
  // Over everything, through the list band's hardware scroll (gfx maps it).
  gfx::fill(0, 0, kW, kH, col::BG, true);
  gfx::fill(0, 0, kW, f, blue, true);
  gfx::fill(0, kH - f, kW, f, blue, true);
  gfx::fill(0, 0, f, kH, blue, true);
  gfx::fill(kW - f, 0, f, kH, blue, true);
  M5Canvas& s = gfx::strip();
  Fonts& fonts = Fonts::instance();
  const int w = kW - 2 * f;  // between the frame's sides
  constexpr int kLineH = 34;
  s.fillRect(0, 0, w, kLineH, col::BG);
  fonts.draw(s, Font::Title, uitext::kIdentifyTitle, w / 2, kLineH / 2, uitext::kIdentifyW, col::TXT, col::BG,
             Fonts::Align::Centre);
  gfx::push(s, f, uitext::kIdentifyTitleY - kLineH / 2, w, kLineH, true);
  s.fillRect(0, 0, w, kLineH, col::BG);
  const Font lf = fonts.width(Font::Title, identifyLabel_) <= uitext::kIdentifyW ? Font::Title : Font::Bold;
  fonts.draw(s, lf, identifyLabel_, w / 2, kLineH / 2, uitext::kIdentifyW, blue, col::BG, Fonts::Align::Centre);
  gfx::push(s, f, uitext::kIdentifyLabelY - kLineH / 2, w, kLineH, true);
}

void Ui::endIdentify() {
  if (!identifying_) return;
  identifying_ = false;
  gfx::setDark(dark_);
  if (!dark_) redrawAll();
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
    // As wide as its label: Bold, or Small past its budget (h.rightMaxW:
    // the crumb's "Bibliothek"), so the title keeps its room.
    const int iconW = h.rightBack ? uitext::kHeaderChevronW : 0;
    const Font font = Header::pillFont(h.right, h.rightMaxW);
    const int w = Header::pillW(h.right, h.rightMaxW, h.rightBack);
    const int px = kW - 6 - w;
    const uint16_t pill = h.rightDanger ? (h.pressed ? col::SOFT : col::RED) : h.pressed ? col::BTN_HI : col::BTN;
    s.fillRoundRect(px, 5, w, 26, 13, pill);
    if (h.rightBack) icons::drawCentred(s, icons::kChevronLeft, px + 14, 18, acc);
    f.draw(s, font, h.right, px + iconW + (w - iconW) / 2, 17, w - iconW, h.rightDanger ? col::DARK : col::TXT, pill,
           Fonts::Align::Centre);
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
    // The sub beside the title in what it and the pill leave (their words
    // in the UI's language): a name cut there, a phrase or a count whole or
    // not at all, or under the title (uitext::headerSub(), from the widths
    // as measured: as the tests and the Queue's summary measure them).
    using uitext::HeaderSub;
    const bool hasSub = h.sub && h.sub[0];
    const int subW = hasSub && (h.subWhole || h.subBelow) ? f.width(Font::Small, h.sub) : 0;
    const HeaderSub how =
        hasSub ? uitext::headerSub(x, std::min(f.width(Font::Bold, h.title), right - x), right, subW, h.subWhole,
                                   h.subBelow)
               : HeaderSub::None;
    if (how == HeaderSub::Below) {
      // Two lines, as the Folder page's: the sub under the title (a name
      // cut at its end), the title last (the line's background would cut
      // its descenders).
      f.draw(s, Font::Small, h.sub, x, 26, right - x, col::DIM, col::HEAD);
      f.draw(s, Font::Bold, h.title, x, 10, right - x, col::TXT, col::HEAD);
    } else {
      const int tw = f.draw(s, Font::Bold, h.title, x, 17, right - x, col::TXT, col::HEAD);
      const int sx = x + tw + uitext::kHeaderSubGap;
      // (A whole one never cut, should the title draw a pixel wider than measured.)
      if (how == HeaderSub::Beside) {
        f.draw(s, Font::Small, h.sub, sx, 18, std::max(right - sx, subW), col::DIM, col::HEAD);
      }
    }
  }
  s.fillRect(0, kHeaderH - 2, kW, 2, acc);
  gfx::push(s, 0, kHeaderY, kW, kHeaderH);
}

// ---- console ----

void Ui::printState() const {
  Serial.printf("[ui] %s; tab %s, page %s (depth %d)%s\n",
                !started_             ? "not started"
                : suspended_          ? "SUSPENDED (another screen has the display)"
                : dark_ && glancing_  ? "up, GLANCE (the track over the dark UI: nothing on it acts)"
                : dark_               ? "up, DARK (the screen is off: nothing drawn)"
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
  if (glancing_) {
    char desc[320];  // (the glance's line too)
    glance_.describe(desc, sizeof(desc));
    Serial.printf("[ui] glance: %s\n", desc);
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
  Serial.printf("[ui] overlays: toast %s%s%s, HUD %s, sheet %s, sleep timer sheet %s, filters %s, volume %s, "
                "jump grid %s, dialog %s, coach %s\n",
                toast_.up() ? "\"" : "none", toast_.up() ? toast_.text() : "", toast_.up() ? "\"" : "",
                hud_.up() ? "up" : "no", sheet_.up() ? "open" : "no", sleepSheet_.up() ? "open" : "no",
                !filters_.up()                                                  ? "no"
                : filters_.owner()->filterScope() == filtergrid::Scope::Tracks ? "open (Auto DJ's)"
                                                                                 : "open (the albums')",
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
  Serial.printf("[ui] language: %s (%s)%s%s\n", uistr::tag(uistr::lang()), uistr::autonym(uistr::lang()),
                uistr::hasCatalogue(uistr::lang()) ? "" : ", no catalogue yet: English's texts",
                pickerDue_ ? "; the first boot's picker is up" : "");
  if (browse_) {
    Serial.printf("[ui] the Library browses a SYNTHETIC library: %lu tracks, %lu artists, %lu albums (uil0: the card's)\n",
                  (unsigned long)browse_->trackCount(), (unsigned long)browse_->artistCount(),
                  (unsigned long)browse_->albumCount());
  }
  thumbs_.printState();
  // This runs on the loop task (the console): its stack's low-water mark
  // since the boot (the console paints the stack again before each command,
  // so FreeRTOS's own mark is the command's: app/LoopStack).
  Serial.printf("[ui] loop: %lu passes, mean %.2f ms, longest %.1f ms (snapshot %.1f, tab bar %.1f, page %.1f, "
                "status %.1f, covers %.1f at most); the longest event %s %.1f ms; counted again from now\n",
                (unsigned long)cost_.passes, cost_.passes ? cost_.passSumUs / 1000.0f / cost_.passes : 0.0f,
                cost_.passMaxUs / 1000.0f, cost_.snapshotMaxUs / 1000.0f, cost_.tabMaxUs / 1000.0f,
                cost_.pageMaxUs / 1000.0f, cost_.statusMaxUs / 1000.0f, cost_.thumbsMaxUs / 1000.0f,
                InputEvent::name(cost_.eventMaxType), cost_.eventMaxUs / 1000.0f);
  cost_ = PassCost{};
  Serial.printf("[ui] loop task stack: %lu B never used since the boot (of 8 KB)\n",
                static_cast<unsigned long>(loopstack::lowestLeft()));
}

// uiL: the languages; uiL<tag> one of them (as a tap on its row); uiL-
// the language and the tips forgotten (English now; the next boot is a
// fresh unit's: its picker, the touch check if due, the tips).
void Ui::languageCommand(const char* a) {
  if (a[0] == '-') {
    uilang::forget();
    coachSeen_ = false;
    languageChanged();
    return;
  }
  if (a[0]) {
    uistr::Lang l = uistr::Lang::En;
    if (!uistr::langOf(a, &l)) {
      Serial.printf("[ui] uiL%s: not one of the languages (uiL: the list)\n", a);
      return;
    }
    chooseLanguage(l);
    return;
  }
  for (unsigned i = 0; i < uistr::kLangs; ++i) {
    const auto l = static_cast<uistr::Lang>(i);
    Serial.printf("[ui] %s %-3s %s%s\n", l == uistr::lang() ? "*" : " ", uistr::tag(l), uistr::autonym(l),
                  uistr::hasCatalogue(l) ? "" : " (no catalogue yet: English)");
  }
}

// uiM: the Albums list's mood filter (what is set, what it holds, the last
// pass's time, the carve) and the editor; uiMa<hex> sets it (bare hex, as
// gm<hex>: an invalid q refused), uiMa0 clears it, its pass logged with its
// time as every pass is; uiMe opens the editor over the page shown, its
// taps then the scripted finger's (docs/MOOD-FILTERS.md 9.3): the Albums
// list's, or over Output > Auto DJ, Auto DJ's (as Smart filters' tap: on
// a card with no mood data, its note instead). Behind the
// library update's fence the index is the build's: uiMa and uiMe say so
// and do nothing.
void Ui::filtersCommand(const char* a) {
  if ((a[0] == 'a' || a[0] == 'e') && state_.libraryFenced) {
    Serial.printf("[ui] filters: the library is updating: uiM%c does nothing (again once it's done)\n", a[0]);
    return;
  }
  if (a[0] == 'e' && !a[1]) {
    if (page_ == &outputPage_ && outputPage_.onAutoDj()) {
      // Over Output > Auto DJ: its editor, as Smart filters' tap opens it
      // (or its note: no mood data).
      outputPage_.editMoods();
      return;
    }
    libraryPage_.refreshFilter();  // (its albums, if the fence or a rebuild dropped them)
    openFilters(&libraryPage_);
    return;
  }
  if (a[0] == 'a') {
    // gm<hex>'s rules (tagtext::parse()): 1-4 hex digits, moodfilter::valid().
    tagtext::Parsed p;
    if (strlen(a + 1) <= 4) {
      char arg[8];
      snprintf(arg, sizeof(arg), "m%s", a + 1);
      p = tagtext::parse(arg);
    }
    if (p.command != tagtext::Command::Moods || p.n == tagtext::kMoodsReport) {
      Serial.printf("[ui] uiMa%s: not a filter (bare hex, the 11 bits of MOOD-FILTERS.md 5.4: uiMa127; uiMa0 "
                    "clears it)\n",
                    a + 1);
      return;
    }
    const uint16_t q = static_cast<uint16_t>(p.n);
    const bool changed = q != libraryPage_.filterWord();
    if (!libraryPage_.setFilterWord(q)) return;  // (no memory: said, and the filter as it was)
    if (filters_.up() && filters_.owner() == &libraryPage_) {
      filters_.sync();  // the editor shows it (its close reloads the list)
    } else {
      libraryPage_.onFiltersClosed(changed);  // as the editor's close: the list from its top
    }
    return;
  }
  if (a[0]) {
    Serial.println("[ui] uiM the Albums list's mood filter; uiMa<hex> set it (uiMa127), uiMa0 clear it; uiMe the "
                   "editor");
    return;
  }
  libraryPage_.printFilter();
  if (!filters_.up()) {
    Serial.println("[ui] filters: the editor is closed");
    return;
  }
  Serial.printf("[ui] filters: the editor is open (%s), filter 0x%03X%s\n",
                filters_.owner() == &libraryPage_ ? "the albums'" : "Auto DJ's",
                static_cast<unsigned>(filters_.word()), filters_.changed() ? ", changed since it opened" : "");
}

// uiBv: the sprites' fast text path (Fonts::drawFast()) against M5GFX's
// drawString(): lines drawn both ways into two sprites whose memory is then
// compared, byte for byte. The library's names (every `step`-th artist,
// album and track title; 0: every artist and album, every 8th track), each
// in a variant picked by its number (font, alignment, place, room,
// colours), and a few made-up lines in every font and alignment.
void Ui::verifyText(int step) {
  M5Canvas a, b;
  constexpr int kTw = 320, kTh = 40;
  a.setPsram(true);
  b.setPsram(true);
  a.setColorDepth(16);
  b.setColorDepth(16);
  if (!a.createSprite(kTw, kTh) || !b.createSprite(kTw, kTh)) {
    Serial.println("[ui] text check: no memory for two sprites");
    return;
  }
  const bool was = Fonts::fast();
  Fonts::setFast(true);
  Fonts& f = Fonts::instance();
  constexpr uint16_t kCanvas = 0x780F;
  a.fillSprite(kCanvas);
  b.fillSprite(kCanvas);
  struct Pair {
    uint16_t fg, bg;
  };
  static const Pair kPairs[] = {{col::TXT, col::BG},        {col::DIM, col::ROW_SEL},       {col::DARK, accent::Library},
                                {accent::Queue, col::CARD}, {col::FAINT, col::SELECTED},    {col::TXT, kCanvas}};
  static const Font kFonts[] = {Font::Body, Font::Small, Font::Bold, Font::Title};
  static const Fonts::Align kAligns[] = {Fonts::Align::Left, Fonts::Align::Centre, Fonts::Align::Right};
  // x, y, maxW: in a row; cut short; at the edges and past them.
  struct Place {
    int16_t x, y, maxW;
  };
  static const Place kPlaces[] = {{50, 13, 234},  {50, 31, 234}, {160, 20, 120}, {8, 20, 60},   {300, 20, 1000},
                                  {-9, 20, 400},  {160, 2, 300}, {160, 38, 300}, {318, 20, 90}, {1, 20, 319}};
  uint32_t lines = 0, bad = 0, fastUs = 0, slowUs = 0, notFast = 0;
  auto one = [&](const char* text, size_t len, unsigned variant, bool all) {
    const int nf = all ? 4 : 1, na = all ? 3 : 1;
    for (int fi = 0; fi < nf; ++fi) {
      for (int ai = 0; ai < na; ++ai) {
        const Font font = all ? kFonts[fi] : kFonts[variant % 4];
        const Fonts::Align align = all ? kAligns[ai] : kAligns[(variant / 4) % 3];
        const Place& pl = kPlaces[(variant / 12 + fi + ai) % (sizeof(kPlaces) / sizeof(kPlaces[0]))];
        const Pair& pr = kPairs[(variant / 5 + fi) % (sizeof(kPairs) / sizeof(kPairs[0]))];
        // A clip rectangle now and then, as nothing in the UI sets one today.
        const bool clip = variant % 7 == 3;
        if (clip) {
          a.setClipRect(40, 6, 200, 26);
          b.setClipRect(40, 6, 200, 26);
        }
        uint32_t t = micros();
        const int wa = f.draw(static_cast<lgfx::LovyanGFX&>(a), font, text, len, pl.x, pl.y, pl.maxW, pr.fg, pr.bg, align);
        slowUs += micros() - t;
        const uint32_t before = Fonts::fastLines();
        t = micros();
        const int wb = f.draw(b, font, text, len, pl.x, pl.y, pl.maxW, pr.fg, pr.bg, align);
        fastUs += micros() - t;
        if (Fonts::fastLines() == before) ++notFast;
        if (clip) {
          a.clearClipRect();
          b.clearClipRect();
        }
        ++lines;
        const auto* pa = static_cast<const uint16_t*>(a.getBuffer());
        const auto* pb = static_cast<const uint16_t*>(b.getBuffer());
        if (wa == wb && memcmp(pa, pb, kTw * kTh * 2) == 0) continue;
        ++bad;
        int at = 0;
        while (at < kTw * kTh && pa[at] == pb[at]) ++at;
        if (bad <= 12) {
          Serial.printf("[ui] text check: DIFFERENT: \"%.*s\" font %d align %d at %d,%d room %d%s: widths %d / %d, first "
                        "pixel %d,%d: %04X / %04X\n",
                        static_cast<int>(len), text, static_cast<int>(font), static_cast<int>(align), pl.x, pl.y,
                        pl.maxW, clip ? " clipped" : "", wa, wb, at % kTw, at / kTw, at < kTw * kTh ? pa[at] : 0,
                        at < kTw * kTh ? pb[at] : 0);
        }
        memcpy(b.getBuffer(), a.getBuffer(), kTw * kTh * 2);  // level again
      }
    }
  };
  static const char* const kMade[] = {
      "jam jar",          "The quick brown fox jumps over the lazy dog",
      "!\"#$%&'()*+,-./0123456789:;<=>?@",                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`",
      "abcdefghijklmnopqrstuvwxyz{|}~",                    "  two  spaces  ",
      "\xC3\x89milie Simon, V\xC3\xA9g\xC3\xA9tal",        "T\xE2\x80\x90Pain \xE2\x80\x99 \xE2\x80\xA6",
      "\xC3\x85\xC3\x84\xC3\x96\xC3\x9C\xC3\x91\xC3\x87 \xC3\xA5\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xB1\xC3\xA7",
      "\xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0",                                  // Cyrillic
      "\xCE\x91\xCE\xB8\xCE\xAE\xCE\xBD\xCE\xB1",                                           // Greek
      "f",                "W",              " ",              "06",          "3 albums \xC2\xB7 41 tracks",
      "jjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjj",
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E mixed",                                          // the store's (never fast)
  };
  unsigned v = 0;
  for (const char* m : kMade) {
    for (unsigned k = 0; k < 10; ++k) one(m, strlen(m), v++ * 12 + k * 12, true);
  }
  const uint32_t made = lines;
  if (const LibraryIndex* i = browseIndex(); i && i->ready()) {
    const uint32_t st = step > 0 ? static_cast<uint32_t>(step) : 1;
    for (uint32_t k = 0; k < i->artistCount(); k += st) {
      const char* n = i->artistName(k);
      one(n, strlen(n), v++, false);
      if ((k & 63) == 0) delay(1);
    }
    for (uint32_t k = 0; k < i->albumCount(); k += st) {
      const char* n = i->albumName(k);
      one(n, strlen(n), v++, false);
      if ((k & 63) == 0) delay(1);
    }
    for (uint32_t k = 0; k < i->trackCount(); k += step > 0 ? st : 8) {
      uint8_t len = 0;
      const char* n = i->trackTitle(k, &len);
      one(n, len, v++, false);
      if ((k & 63) == 0) delay(1);
    }
  }
  Fonts::setFast(was);
  Serial.printf("[ui] text check: %lu lines drawn both ways (%lu made up, the rest the library's names), %lu "
                "DIFFERENT; %lu not the fast path's (a glyph from the store); M5GFX %lu us a line, the fast path %lu "
                "us\n",
                (unsigned long)lines, (unsigned long)made, (unsigned long)bad, (unsigned long)notFast,
                lines ? (unsigned long)(slowUs / lines) : 0UL, lines ? (unsigned long)(fastUs / lines) : 0UL);
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
  } else if (a[0] == 'r') {
    // uir0 / uir1: the rail scrub as before 0.12.0 (every position a band
    // draw at once, the card worker not held) / paced; uir: which.
    if (a[1] == '0' || a[1] == '1') {
      railHold_ = a[1] == '1';
      list_.setScrubPacing(railHold_);
    }
    Serial.printf("[ui] rail scrub: %s\n",
                  railHold_ ? "paced (the rail follows the finger on every frame, the band at ScrubPacer's cadence; "
                              "the card worker is held while a finger is on the rail)"
                            : "every position a band draw at once, the card worker not held (as before 0.12.0)");
    return;
  } else if (a[0] == 'B') {
    // uiB: the list's bench (what a frame is made of); uiBv[step]: the
    // sprites' fast text against M5GFX's, pixel for pixel; uiBf0 / uiBf1:
    // that fast path off / on (uiB's rows and uiBv's times then compare).
    if (a[1] == 'f') {
      if (a[2] == '0' || a[2] == '1') Fonts::setFast(a[2] == '1');
      Serial.printf("[ui] sprites' text: %s\n", Fonts::fast() ? "straight into their memory (the fast path)"
                                                               : "by M5GFX (uiBf0: until uiBf1 or a restart)");
      list_.refreshAll();
    } else if (a[1] == 'v') {
      verifyText(atoi(a + 2));
    } else if (list_.attached()) {
      list_.bench();
    } else {
      Serial.println("[ui] bench: no list page up (ui1, ui2 or ui4 first)");
    }
    return;
  } else if (a[0] == 'v') {
    // uiv: the panel's scan; uiv0 / uiv1: a moving list's frames ignore /
    // follow it (to compare by eye); uiv!: timed again.
    if (a[1] == '0' || a[1] == '1') list_.setScanSync(a[1] == '1');
    if (a[1] == '!') scan_.calibrate();
    scan_.print();
    hotmem::print();
    Serial.printf("[ui] list frames: %s; a pushed line %lu us, a row %lu us (as the last ones took)\n",
                  !scan_.ok()          ? "on the frame clock (the scan can't be read)"
                  : list_.scanSync()   ? "on the panel's scan"
                                       : "on the frame clock (uiv0)",
                  (unsigned long)list_.lineUs(), (unsigned long)list_.rowUs());
    return;
  } else if (a[0] == 'f') {
    // uif<ms>: the frame clock's period for a moving list (uif: as it is by itself).
    forcedFrameMs_ = static_cast<uint32_t>(atoi(a + 1));
    Serial.printf("[ui] frame clock: %lu ms a frame while a list moves%s\n", (unsigned long)listFrameMs(),
                  forcedFrameMs_ ? " (uif: as it is by itself)" : "");
    return;
  } else if (a[0] == 'b') {
    back();
  } else if (a[0] == 'c') {
    showCoach();
  } else if (a[0] == 'L') {
    languageCommand(a + 1);
    return;
  } else if (a[0] == 'P') {
    // The first boot's picker, now (no touch check or tips after it).
    closeModal(true);
    if (coach_.up()) coach_.close();
    if (page_) page_->leave();
    page_ = nullptr;
    pickerDue_ = true;
    pickerNav();
    showTop();
  } else if (a[0] == 'T') {
    thumbs_.redecode();
    list_.refreshAll();  // the rows on screen ask again
  } else if (a[0] == 'M') {
    filtersCommand(a + 1);
    return;
  } else {
    Serial.println("[ui] ui: the navigation state; ui0-ui4 tap a tab (0 Now Playing, 1 Library, 2 Queue, 3 Dance, "
                   "4 Output); uib back; uic the coach cards; uiL<tag> the language (uiL the list, uiL- forget it and "
                   "the tips: the next boot is a fresh unit's); uiP the first boot's language picker; uiT the covers "
                   "decoded again (timings); uil<n> the Library browses a synthetic library of n tracks (uil0: the "
                   "card's); uit/uih/uis/uid/uip a scripted finger; uiM the Albums list's mood filter (uiMa<hex> set "
                   "it, uiMa0 clear it, uiMe the editor); uiv the panel's scan and the list's frames (uiv0 / uiv1 "
                   "ignore / follow it, uiv! time it again), uif<ms> the frame clock's period; uiB the list's bench, "
                   "uiBv the sprites' text checked against M5GFX's, uiBf0 / uiBf1 their fast text off / on");
    return;
  }
  printState();
}

}  // namespace ui
