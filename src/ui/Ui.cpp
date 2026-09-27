#include "ui/Ui.h"

#include <M5Unified.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    case PageKind::Artists: return "Artists";
    case PageKind::Artist: return "Artist";
    case PageKind::Album: return "Album";
    case PageKind::ArtistTracks: return "ArtistTracks";
    case PageKind::Queue: return "Queue";
    case PageKind::Dance: return "Dance";
    case PageKind::Output: return "Output";
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
      outputPage_(*this) {
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
  nav_.setRoot(NavModel::Tab::NowPlaying, ref(PageKind::NowPlaying));
  nav_.setRoot(NavModel::Tab::Library, ref(PageKind::Artists));
  nav_.setRoot(NavModel::Tab::Queue, ref(PageKind::Queue));
  nav_.setRoot(NavModel::Tab::Dance, ref(PageKind::Dance));
  nav_.setRoot(NavModel::Tab::Output, ref(PageKind::Output));
  return true;
}

void Ui::start(uint32_t nowMs) {
  nowMs_ = nowMs;
  host_.snapshot(state_);
  hudSeq_ = state_.feedback.seq;
  lastContent_ = state_.contentVersion;
  lastUpNext_ = state_.upNext;
  started_ = true;
  gfx::fill(0, 0, kW, kH, col::BG, true);
  tabBar_.invalidate();
  tabBar_.update(tabState(nowMs));
  showTop();
  Serial.println("[ui] up: tab bar, Now Playing (console ui: the navigation state)");
}

Page* Ui::pageFor(uint8_t kind) {
  switch (static_cast<PageKind>(kind)) {
    case PageKind::Artists:
    case PageKind::Artist:
    case PageKind::Album:
    case PageKind::ArtistTracks: return &libraryPage_;
    case PageKind::Queue: return &queuePage_;
    case PageKind::Dance: return &dancePage_;
    case PageKind::Output: return &outputPage_;
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

void Ui::showTop() {
  Page* next = pageFor(nav_.top().kind);
  if (page_) page_->leave();
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
    if (closed && page_) page_->repaint();
    return;
  }
  if (t == NavModel::Tab::Dance) beforeDance_ = nav_.tab();
  if (page_) page_->leave();
  page_ = nullptr;
  nav_.select(t);
  showTop();
}

void Ui::showInLibrary(uint32_t artist, uint32_t album) {
  NavModel::PageRef pages[2];
  int n = 0;
  if (artist != NavModel::kNone) pages[n++] = ref(PageKind::Artist, artist);
  if (album != NavModel::kNone) pages[n++] = ref(PageKind::Album, album);
  for (int i = 0; i < n; ++i) pages[i].scrollPx = kShowPlaying;  // at the playing track (or its album)
  closeModal(true);
  if (page_) page_->leave();
  page_ = nullptr;
  nav_.replaceAboveRoot(NavModel::Tab::Library, pages, n);
  nav_.select(NavModel::Tab::Library);
  showTop();
}

void Ui::tapTab(NavModel::Tab t) {
  const bool closed = closeModal(true);
  const NavModel::Tab was = nav_.tab();
  const NavModel::TabTap r = nav_.tapTab(t);
  if (r == NavModel::TabTap::AtRoot) {
    // The same page stays, and goes "home" (the Queue to the playing track).
    if (closed) page_->repaint();
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

void Ui::libraryChanged() {
  // Every index id changed: the Library's pages start over at the root.
  const bool shown = started_ && !suspended_;
  if (shown) closeModal(true);
  const bool onLibrary = shown && nav_.tab() == NavModel::Tab::Library;
  if (onLibrary && page_) {
    page_->leave();  // before its entries are replaced
    page_ = nullptr;
  }
  nav_.setRoot(NavModel::Tab::Library, ref(PageKind::Artists));
  if (!shown) return;
  if (onLibrary) {
    showTop();
  } else if (page_) {
    page_->repaint();  // names may have changed under the same ids
  }
}

// ---- overlays ----

void Ui::applyCover() {
  if (toast_.up()) {
    gfx::setCover(Toast::kY, Toast::kY + Toast::kH);
  } else {
    gfx::setCover(0, 0);
  }
  // The dancer is drawn by DanceMode, outside the pages: keep it off the overlays.
  const int top = toast_.up() ? Toast::kY + Toast::kH : 0;
  const int bottom = sheet_.up() ? sheet_.top() : dialog_.up() ? Dialog::kY : kH;
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
    if (notify && dialogOwner_) dialogOwner_->onDialog(-1);
  }
  if (closed) applyCover();
  return closed;
}

void Ui::toast(const char* text, bool undo, uint32_t viewKey) {
  viewKey_ = viewKey;
  toast_.show(text, undo, viewKey != QueueModel::kNone, accent(), nowMs_);
  applyCover();
  Serial.printf("[ui] toast: %s%s%s\n", text, undo ? " (Undo)" : "",
                viewKey != QueueModel::kNone ? " (View)" : "");
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

void Ui::openSheet(OverlayOwner* owner, const char* title, const char* const* rows, int n) {
  endPageTouch();
  closeModal(true);
  sheetOwner_ = owner;
  sheet_.open(title, rows, n, accent());
  applyCover();
}

void Ui::openDialog(OverlayOwner* owner, const char* title, const char* body, const char* const* buttons, int n) {
  endPageTouch();
  closeModal(true);
  dialogOwner_ = owner;
  lostDialog_ = false;
  dialog_.open(title, body, buttons, n, accent());
  applyCover();
}

void Ui::headphonesLost() {
  if (!started_ || suspended_) return;
  char body[128];
  snprintf(body, sizeof(body),
           "%s dropped out, so the music paused. It carries on here when they're back, or on the speaker.",
           state_.btName[0] ? state_.btName : "The headphones");
  static const char* const kButtons[2] = {"Use speaker", "OK"};
  openDialog(this, "Headphones disconnected", body, kButtons, 2);
  lostDialog_ = true;
}

void Ui::onDialog(int button) {
  // headphonesLost()'s dialog: "Use speaker" moves the output (paused: B plays).
  if (button == 0) host_.selectOutput(false);
}

void Ui::volumeKeys() { volumeHudDue_ = true; }

void Ui::updateHud(uint32_t nowMs) {
  const ButtonPolicy::Feedback& f = state_.feedback;
  if (f.seq != hudSeq_) {
    hudSeq_ = f.seq;
    hudFollows_ = false;  // it shows where the button's step goes
    if (f.kind == ButtonPolicy::Hud::Volume) {
      hud_.showVolume(f.volume, state_.onBluetooth, nowMs);
    } else if (f.kind == ButtonPolicy::Hud::Output) {
      char line[48];
      if (f.refused) {
        snprintf(line, sizeof(line), "Stays on the speaker (silent test mode)");
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
  } else {
    s.play = state_.play == PlayState::Paused ? tabbar::Play::Paused : tabbar::Play::Stopped;
  }
  s.eqStep = static_cast<uint8_t>(nowMs / 250);  // 4 steps a second
  s.progressKnown = state_.durationMs > 0;
  s.progressPx = tabbar::progressPx(state_.positionMs, state_.durationMs);
  s.upNext = static_cast<uint16_t>(std::min<uint32_t>(state_.upNext, 65535));
  s.badgeFlash = static_cast<int32_t>(badgeUntilMs_ - nowMs) > 0;
  if (!state_.onBluetooth) {
    s.output = tabbar::Output::Speaker;
  } else if (state_.btConnected) {
    s.output = tabbar::Output::BtConnected;
  } else {
    s.output = state_.btLost ? tabbar::Output::BtLost : tabbar::Output::BtConnecting;
  }
  s.volume = state_.volume;
  s.battery = state_.battery;
  s.charging = state_.charging;
  // Low: the battery blinks off for half a second once a minute.
  s.lowBlink = state_.battery <= 10 && (nowMs / 500) % 120 == 0;
  return s;
}

void Ui::loop(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (!started_ || suspended_) return;
  host_.snapshot(state_);

  updateHud(nowMs);
  if (toast_.expired(nowMs)) {
    if (toast_.undo()) queue_.dropUndo();  // Undo was offered until now
    toast_.hide();
    applyCover();
    if (page_) page_->repaintHeader();
    if (dialog_.up()) dialog_.draw();
  }
  if (lostDialog_ && dialog_.up() && state_.btConnected) {
    // The headphones are back: nothing to decide any more.
    closeModal(false);
    if (page_) page_->repaint();
  }
  // Tracks added: the Queue badge flashes.
  if (state_.contentVersion != lastContent_) {
    if (state_.upNext > lastUpNext_) badgeUntilMs_ = nowMs + 1500;
    lastContent_ = state_.contentVersion;
  }
  lastUpNext_ = state_.upNext;
  if (!hud_.up()) tabBar_.update(tabState(nowMs));

  // The page: redraws what changed, and animation frames on the cadence.
  budget_ = governor_.update(nowMs, state_.ringMs, state_.underruns, state_.ringMatters);
  clock_.setPeriod(budget_.frameMs);
  const bool due = budget_.draw && clock_.due(nowMs);
  const bool modal = sheet_.up() || dialog_.up();
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
    }
  }
  trackMotion(nowMs);
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
// printed for its stress runs.
void Ui::trackMotion(uint32_t nowMs) {
  const bool moving = page_ && list_.attached() && page_->animating();
  if (moving && !motion_.on) {
    motion_ = Motion{};
    motion_.on = true;
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
  Serial.printf("[ui] scroll: %lu ms, %lu frames (%.1f fps), draw mean %.1f max %.1f ms, ring min %s, underruns +%lu, "
                "governor %s\n",
                (unsigned long)ms, (unsigned long)motion_.frames, ms ? motion_.frames * 1000.0f / ms : 0.0f,
                motion_.sumUs / 1000.0f / motion_.frames, motion_.maxUs / 1000.0f, ring,
                (unsigned long)(state_.underruns - motion_.underruns), ScrollGovernor::name(budget_.level));
}

uint32_t Ui::idleMs(uint32_t nowMs) const {
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
    } else if (dialog_.up()) {
      touch_ = TouchOn::Dialog;
    } else if (sheet_.up()) {
      touch_ = TouchOn::Sheet;
    } else if (toast_.hit(e)) {
      touch_ = TouchOn::Toast;
    } else {
      touch_ = TouchOn::Page;
    }
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
        applyCover();
        if (page_) page_->repaint();
        if (owner) owner->onDialog(b);
      }
      break;
    }
    case TouchOn::Sheet: {
      const int r = sheet_.onEvent(e);
      if (r >= 0 || r == -2) {
        tick();
        OverlayOwner* owner = sheetOwner_;
        sheet_.close();
        applyCover();
        if (page_) page_->repaint();
        if (owner) owner->onSheet(r >= 0 ? r : -1);
      }
      break;
    }
    case TouchOn::Toast:
      if (e.type == T::Down && toast_.hit(e) >= 2) toast_.draw(toast_.hit(e));
      if (e.type == T::Tap) {
        tick();
        const int hit = toast_.hit(e);
        if (hit == 2) {
          const bool undone = player_.undo();
          Serial.printf("[ui] undo: %s\n", undone ? "done" : "nothing to undo");
          toast_.show(undone ? "Undone" : "Nothing to undo", false, false, accent(), nowMs_);
        } else if (hit == 3) {
          toast_.hide();
          applyCover();
          if (page_) page_->repaintHeader();
          viewInQueue(viewKey_);
        } else {
          toast_.hide();
          applyCover();
          if (page_) page_->repaintHeader();
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
  if (!started_) return;
  closeModal(false);
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
  if (!started_) return;
  gfx::fill(0, 0, kW, kH, col::BG, true);
  tabBar_.invalidate();
  showTop();
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
    const int w = f.width(Font::Bold, h.right) + 24;
    const int px = kW - 6 - w;
    const uint16_t pill = h.rightDanger ? (h.pressed ? col::SOFT : col::RED) : h.pressed ? col::BTN_HI : col::BTN;
    s.fillRoundRect(px, 5, w, 26, 13, pill);
    f.draw(s, Font::Bold, h.right, px + w / 2, 17, w, h.rightDanger ? col::DARK : col::TXT, pill,
           Fonts::Align::Centre);
    right = px - 8;
  }
  const int tw = f.draw(s, Font::Bold, h.title, x, 17, right - x, col::TXT, col::HEAD);
  if (h.sub && h.sub[0] && x + tw + 8 < right - 16) {
    f.draw(s, Font::Small, h.sub, x + tw + 8, 18, right - (x + tw + 8), col::DIM, col::HEAD);
  }
  s.fillRect(0, kHeaderH - 2, kW, 2, acc);
  gfx::push(s, 0, kHeaderY, kW, kHeaderH);
}

// ---- console ----

void Ui::printState() const {
  Serial.printf("[ui] %s; tab %s, page %s (depth %d)%s\n",
                !started_ ? "not started" : suspended_ ? "SUSPENDED (another screen has the display)" : "up",
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
    char desc[160];
    page_->describe(desc, sizeof(desc));
    Serial.printf("[ui] page: %s\n", desc);
  }
  if (list_.attached()) {
    Serial.printf("[ui] list: %lu rows, %lu items, offset %ld px, open row %ld, %s, %lu frames\n",
                  (unsigned long)list_.layout().rows(), (unsigned long)list_.layout().itemCount(),
                  (long)list_.offset(), (long)list_.expanded(), KineticScroll::name(list_.scroll().phase()),
                  (unsigned long)list_.framesDrawn());
  }
  const SpiHoldStats& h = gfx::holds();
  Serial.printf("[ui] frames %lu (last busy second %.1f fps), cap %lu ms on deadlines, governor %s; bus holds %lu, "
                "mean %.2f ms, max %.2f ms\n",
                (unsigned long)frames_, fps_, (unsigned long)clock_.period(), ScrollGovernor::name(budget_.level),
                (unsigned long)h.count, h.meanUs() / 1000.0f, h.maxUs / 1000.0f);
  Serial.printf("[ui] overlays: toast %s%s%s, HUD %s, sheet %s, dialog %s\n", toast_.up() ? "\"" : "none",
                toast_.up() ? toast_.text() : "", toast_.up() ? "\"" : "", hud_.up() ? "up" : "no",
                sheet_.up() ? "open" : "no", dialog_.up() ? "open" : "no");
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
  if (a[0] >= '0' && a[0] <= '4' && !a[1]) {
    tapTab(static_cast<NavModel::Tab>(a[0] - '0'));  // as a tap on that tab (again: to its root)
  } else if (a[0] == 'b') {
    back();
  } else {
    Serial.println("[ui] ui: the navigation state; ui0-ui4 tap a tab (0 Now Playing, 1 Library, 2 Queue, 3 Dance, "
                   "4 Output); uib back");
    return;
  }
  printState();
}

}  // namespace ui
