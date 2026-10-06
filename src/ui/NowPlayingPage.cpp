// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Now Playing (the tab bar spec §6.1, mockups 01-04, with the review's
// grafts): the cover, the title, the artist and the album as two plain
// rows, the progress and times, and the transport. Two menus, one job
// each (docs/QUEUE-MODES.md): a tap anywhere on the cover or the text
// opens the navigation menu (Go to artist, album, folder), "..." the
// playback menu (Shuffle, Repeat, Sleep timer); the volume button the
// volume sheet. While play waits for the headphones, a spinner for play
// and the waiting panel (PlayGate) in the title strip and the rows. The
// layout is in Pages.h. The "..." zone reaches the screen's edge.
//
// The progress line is a seek bar (docs/SEEK-BAR.md): SeekBar has the
// mapping and the touch, this page the drawing and the one call to the
// player at the end of a touch (ui_.player().seek(), as the Queue page
// edits: a seek never starts or raises sound, so neither PlayGate nor the
// pocket rule is asked). onEvent() only moves the model and plays the
// touch's ticks; update() draws, the pressed look and the end of a scrub at
// once, a scrub's frames only on the frame deadlines. One log line per
// touch on the bar, when it ends.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "SheetLayout.h"
#include "TextFit.h"
#include "TrackCatalog.h"
#include "TrackSeek.h"
#include "UiText.h"
#include "app/Psram.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

namespace {

constexpr int kCoverX = 12, kCoverY = 40, kCoverPx = 96;
constexpr int kTextX = 120;       // the title's and the rows' text
constexpr int kColumnX = 112;     // what's right of the cover (drawn and hit)
constexpr int kTextW = 190;       // the title's and the rows' text: x 120-310
constexpr int kTitleY = 38, kTitleH = 52;
// The artist and the album: plain rows of a Body line and 4 px, the
// artist's from where the title strip ends, the album's ending on the
// cover's last row (the room the old 40 px bands gave up went to the
// transport).
constexpr int kArtistY = 90, kArtistH = 23;
constexpr int kAlbumY = 113, kAlbumH = 23;
constexpr int kProgressY = 146, kProgressH = 22;
// The transport takes touches on y 168-239 (five zones of 64 x 72) but is
// drawn as one 56-row strip (gfx::kStripH) from y 176, 8 px of background
// each side.
constexpr int kTransportY = 168, kTransportH = 72;
constexpr int kTransportDrawY = 176, kTransportDrawH = 56;
constexpr int kZoneW = 64;
constexpr uint32_t kSpinMs = 125;  // the waiting spinner: 8 steps a second
// The seek bar's touch reaches this far above the band (y 138): the line is
// drawn at the band's top (y 148-151), so a tap aimed at it lands on both
// sides of y 146. Whether or not a play waits (the waiting buttons' touch
// ends at y 137).
constexpr int kSeekReachPx = 8;
// The readout row while a finger scrubs: the 33 rows above the band, the
// full width: over the album row and the cover's lowest 24 rows (its
// frame's last included), which come back at the lift (endScrub()).
constexpr int kReadoutY = 113, kReadoutH = 33;
// The navigation area (the menu's touch, and its press look): everything
// above the seek bar's reach; lit on x 0-319, y 36-136.
constexpr int kNavBottom = kCoverY + kCoverPx + 1;  // 137: under the cover's frame
// The indicator's glyphs under the "..." dots, 4 px apart when both show.
constexpr int kModesGap = 4;

static_assert(SeekBar::kLineX + SeekBar::kLineW == kW - 12, "the seek bar's line is drawProgress()'s");
static_assert(kArtistY == kTitleY + kTitleH, "the artist row starts where the title strip ends");
static_assert(kAlbumY == kArtistY + kArtistH, "the album row follows the artist's");
static_assert(kAlbumY + kAlbumH == kCoverY + kCoverPx, "the album row ends on the cover's last row");
static_assert(kReadoutY == kAlbumY, "the readout row starts on the album row");
static_assert(kReadoutY + kReadoutH == kProgressY, "the readout row ends at the band");
static_assert(kProgressY + kProgressH == kTransportY, "the transport's touch starts under the band");
static_assert(kTransportY + kTransportH == kH, "the transport's touch reaches the button strip");
static_assert(kTransportDrawH == gfx::kStripH, "the transport is drawn as one strip");
static_assert(kTransportDrawY - kTransportY == kTransportY + kTransportH - (kTransportDrawY + kTransportDrawH),
              "the drawn transport sits in the middle of its touch");
static_assert(kProgressY - kSeekReachPx == kNavBottom + 1, "the seek bar's touch starts under the navigation area");
// The seek bar's off and back rows (SeekBar.h) follow the layout: back on
// the bar from the readout's second row, off 7 rows above the readout
// (onto the artist row).
static_assert(SeekBar::kBackAboveY == kReadoutY + 1, "back on the bar from the readout's second row");
static_assert(SeekBar::kOffAboveY == kReadoutY - 7, "off the bar onto the artist row");
static_assert(SeekBar::kLineX + uitext::kSeekReadoutW < SeekBar::kReadoutLeftX &&
                  SeekBar::kLineX + SeekBar::kLineW - uitext::kSeekReadoutW > SeekBar::kReadoutRightX,
              "the readout at its widest ends short of where the knob sends it across");

// m:ss, as the seek bar's readout has it (what the finger read is what the
// band shows after the lift).
void mmss(uint32_t ms, char* buf, size_t size) { SeekBar::timeText(ms, buf, size); }

// The last line of a wrap was cut ("…" added): the text needs more room.
bool cutShort(const char* line, const char* text) {
  const size_t n = strlen(line), t = strlen(text);
  const bool ends = (n >= 3 && strcmp(line + n - 3, "\xE2\x80\xA6") == 0) || (n >= 3 && strcmp(line + n - 3, "...") == 0);
  return ends && !(t >= 3 && strcmp(text + t - 3, line + n - 3) == 0);
}

const char* repeatName(uint8_t mode) { return mode == 2 ? "one" : mode == 1 ? "all" : "off"; }

}  // namespace

uint32_t NowPlayingPage::playingAlbum() const {
  const AppState& s = ui_.state();
  const LibraryIndex* index = const_cast<Ui&>(ui_).library().index();
  if (s.current < 0 || !index || !index->ready() || s.trackId >= index->trackCount()) return LibraryIndex::kNone;
  return index->track(s.trackId).album;
}

void NowPlayingPage::enter(NavModel::PageRef& ref) {
  (void)ref;
  pressed_ = None;
  emptyPressed_ = -1;
  bar_.cancel();
  if (!cover_) {
    cover_ = psramNew<M5Canvas>();
    if (cover_) {
      cover_->setPsram(true);  // before createSprite(): otherwise internal RAM
      cover_->setColorDepth(16);
      if (!cover_->createSprite(kCoverPx + 2, kCoverPx + 2)) {
        psramDelete(cover_);
        cover_ = nullptr;
      }
    }
  }
  repaint();
}

void NowPlayingPage::leave() {
  // (The page went, or another screen took the display: a scrub ends with
  // nothing, unlogged. The next enter() paints it all.)
  bar_.cancel();
  pressed_ = None;
}

uint16_t NowPlayingPage::navBg() const {
  // The press look (a Down highlights): not while a play waits (the cover
  // alone takes the touch then, and isn't lit).
  return pressed_ == Nav && ui_.state().play != PlayState::Waiting ? col::ROW_SEL : col::BG;
}

void NowPlayingPage::fillNav() {
  // What the navigation area's pieces don't cover, in its look: the rows
  // above the title, row 38 left of the column, beside the cover's frame,
  // and the row under the album row right of it.
  const uint16_t bg = navBg();
  gfx::fill(0, kContentY, kW, kTitleY - kContentY, bg);
  gfx::fill(0, kTitleY, kColumnX, kCoverY - 1 - kTitleY, bg);
  gfx::fill(0, kCoverY - 1, kCoverX - 1, kCoverPx + 2, bg);
  gfx::fill(kCoverX + kCoverPx + 1, kCoverY - 1, kColumnX - (kCoverX + kCoverPx + 1), kCoverPx + 2, bg);
  gfx::fill(kColumnX, kAlbumY + kAlbumH, kW - kColumnX, kNavBottom - (kAlbumY + kAlbumH), bg);
}

void NowPlayingPage::drawNav() {
  // The press look on or off: the cover is left as it is (no fill overlaps
  // its frame). A scrub's readout still up (its lift not drawn yet) goes
  // first.
  if (drawn_.scrubUp) endScrub();
  fillNav();
  drawTitle();
  drawMiddle();
}

void NowPlayingPage::repaint() {
  if (ui_.state().current < 0) {
    drawn_ = Drawn{};  // the empty state, all of it
    update(0, false, false);
    return;
  }
  // What the pieces don't cover: the navigation area's gaps (in its look),
  // the rows between it and the band, and the transport's margins.
  fillNav();
  gfx::fill(0, kNavBottom, kW, kProgressY - kNavBottom, col::BG);
  gfx::fill(0, kTransportY, kW, kTransportDrawY - kTransportY, col::BG);
  gfx::fill(0, kTransportDrawY + kTransportDrawH, kW, kH - (kTransportDrawY + kTransportDrawH), col::BG);
  drawn_ = Drawn{};
  update(0, false, false);
}

void NowPlayingPage::thumbReady(uint32_t album) {
  if (album == playingAlbum() && !drawn_.coverShown) drawCover();
}

void NowPlayingPage::drawCover() {
  const uint32_t album = playingAlbum();
  const uint16_t* px = album != LibraryIndex::kNone ? ui_.thumbs().get(album, ThumbCache::Size::Large) : nullptr;
  drawn_.coverAlbum = album;
  drawn_.coverShown = px != nullptr;
  // While a finger scrubs, the readout row covers the cover's lowest rows:
  // only what is above it goes to the screen now, the rest at the lift
  // (endScrub()). A thumbnail that arrives mid-scrub can't cut the readout.
  const int shownRows = drawn_.scrubUp ? kReadoutY - (kCoverY - 1) : kCoverPx + 2;
  if (!cover_) {
    gfx::fill(kCoverX, kCoverY, kCoverPx, drawn_.scrubUp ? kReadoutY - kCoverY : kCoverPx, col::CARD);
    return;
  }
  M5Canvas& c = *cover_;
  c.fillSprite(col::DIV);  // the frame
  if (px) {
    c.pushImage(1, 1, kCoverPx, kCoverPx, reinterpret_cast<const lgfx::swap565_t*>(px));
  } else {
    // The placeholder: no cover, or not made yet.
    c.fillRect(1, 1, kCoverPx, kCoverPx, col::CARD);
    c.fillCircle(1 + kCoverPx / 2, 1 + kCoverPx / 2, 28, col::BTN);
    icons::drawCentred(c, icons::kNote, 1 + kCoverPx / 2, 1 + kCoverPx / 2, ui_.state().current >= 0 ? col::DIM : col::FAINT);
  }
  gfx::pushRows(c, kCoverX - 1, kCoverY - 1, kCoverPx + 2, 0, shownRows);
}

void NowPlayingPage::drawTitle() {
  using namespace uitext;
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const int w = kW - kColumnX;
  const uint16_t bg = navBg();
  c.fillRect(0, 0, w, kTitleH, bg);
  char title[160] = "Nothing playing";
  if (s.current >= 0) ui_.player().catalog().title(s.trackId, title, sizeof(title));
  const uint16_t ink = s.current >= 0 ? col::TXT : col::DIM;
  const int x = kTextX - kColumnX;
  if (s.play == PlayState::Waiting) {
    // The wait's status takes the strip's lower lines: the title on one
    // line over "Waiting for SPYDRONE…" (amber) and "try 2 of 3".
    f.draw(c, Font::Bold, title, x, 9, kTextW, ink, bg);
    char line[64];
    snprintf(line, sizeof(line), "Waiting for %s\xE2\x80\xA6", s.btName[0] ? s.btName : "the headphones");
    f.draw(c, Font::Small, line, kWaitTextX, 26, kWaitTextW, col::AMBER, bg);
    const BtLink& l = s.btLink;
    if (l.phase == BtLink::Phase::Paging && l.attempt > 0) {
      snprintf(line, sizeof(line), "try %u of %u", static_cast<unsigned>(l.attempt),
               static_cast<unsigned>(l.attempts > l.attempt ? l.attempts : l.attempt));
    } else if (l.phase == BtLink::Phase::Scanning || l.phase == BtLink::Phase::Backoff) {
      snprintf(line, sizeof(line), "looking for them");
    } else {
      snprintf(line, sizeof(line), "connecting");
    }
    f.draw(c, Font::Small, line, kWaitTextX, 43, kWaitTextW, col::DIM, bg);
    gfx::push(c, kColumnX, kTitleY, w, kTitleH);
    return;
  }
  char lines[3][112];
  int n = textfit::wrap(f.fit(Font::Title), title, strlen(title), kTextW, 2, &lines[0][0], sizeof(lines[0]));
  if (n == 2 && cutShort(lines[1], title)) {
    // Too long for two big lines: three smaller ones.
    n = textfit::wrap(f.fit(Font::Bold), title, strlen(title), kTextW, 3, &lines[0][0], sizeof(lines[0]));
    for (int i = 0; i < n; ++i) f.draw(c, Font::Bold, lines[i], x, 9 + i * 17, kTextW, ink, bg);
  } else {
    for (int i = 0; i < n; ++i) f.draw(c, Font::Title, lines[i], x, n == 1 ? 26 : 13 + i * 26, kTextW, ink, bg);
  }
  gfx::push(c, kColumnX, kTitleY, w, kTitleH);
}

void NowPlayingPage::drawArtistAlbum() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const TrackCatalog& cat = ui_.player().catalog();
  const bool lib = s.current >= 0 && !TrackCatalog::isBuiltin(s.trackId);
  const int w = kW - kColumnX;
  const int x = kTextX - kColumnX;
  const uint16_t bg = navBg();
  // Both rows in one push: plain text, no "›" (neither is a control of its
  // own: the whole area opens the navigation menu).
  c.fillRect(0, 0, w, kArtistH + kAlbumH, bg);
  const char* artist = lib ? cat.artist(s.trackId) : s.current >= 0 ? "Built-in test track" : "";
  if (lib && !artist[0]) artist = uitext::kNoArtistFolder;
  f.draw(c, Font::Body, artist, x, kArtistH / 2, kTextW, col::SOFT, bg);
  if (lib) {
    const char* album = cat.album(s.trackId);
    f.draw(c, Font::Body, album[0] ? album : uitext::kLooseTracks, x, kArtistH + kAlbumH / 2, kTextW, col::DIM, bg);
  }
  gfx::push(c, kColumnX, kArtistY, w, kArtistH + kAlbumH);
}

bool NowPlayingPage::seekable() const {
  const AppState& s = ui_.state();
  return s.current >= 0 && !s.failed && SeekBar::seekable(s.durationMs);
}

NowPlayingPage::BarLook NowPlayingPage::barLook() const {
  switch (bar_.phase()) {
    case SeekBar::Phase::Pressed: return BarLook::Pressed;
    case SeekBar::Phase::Scrubbing: return BarLook::Scrubbing;
    case SeekBar::Phase::Off: return BarLook::Off;
    default: return seekable() ? BarLook::Rest : BarLook::Inert;
  }
}

void NowPlayingPage::drawProgress() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  c.fillRect(0, 0, kW, kProgressH, col::BG);
  const bool paused = s.play != PlayState::Playing;
  const int x0 = SeekBar::kLineX, w = SeekBar::kLineW;
  const BarLook look = barLook();
  const bool scrub = look == BarLook::Scrubbing || look == BarLook::Off;
  constexpr int kKnobY = 4;  // the knob's centre, on the line (band rows)
  int knob = -1, marker = -1;
  uint16_t knobInk = col::TXT;
  if (look == BarLook::Inert || look == BarLook::Rest) {
    if (s.durationMs > 0) {
      c.fillRoundRect(x0, 2, w, 4, 2, col::DIV);
      const int fill = SeekBar::xOf(s.positionMs, s.durationMs);
      knobInk = paused ? col::DIM : accent::NowPlaying;
      if (fill > 0) c.fillRoundRect(x0, 2, fill, 4, 2, knobInk);
      if (look == BarLook::Rest) knob = x0 + fill;  // (it can be moved)
    } else {
      for (int x = x0; x < x0 + w; x += 6) c.fillRect(x, 3, 3, 2, col::DIV);  // unknown length: dotted
    }
  } else {
    // A finger on it (pressed, scrubbing, off): the line thickens to 6 px.
    const uint32_t len = bar_.lengthMs();
    knob = bar_.knobX();
    if (look == BarLook::Scrubbing) {
      // To the reach (the length less 6 s); past it, faint dots: the knob
      // stops there, and the dots say why. The marker shows where it plays
      // (hidden while the knob is on it).
      const int reach = SeekBar::xOf(trackseek::seekLimitMs(len), len);
      c.fillRoundRect(x0, 1, reach, 6, 3, col::DIV);
      for (int x = x0 + reach + 3; x + 3 <= x0 + w; x += 6) c.fillRect(x, 3, 3, 2, col::FAINT);
      if (knob > x0) c.fillRoundRect(x0, 1, knob - x0, 6, 3, accent::NowPlaying);
      if (!bar_.staying()) marker = bar_.markerX();
    } else {
      // Pressed: the accent, even paused (it is the active control); off:
      // dim, to where it plays.
      c.fillRoundRect(x0, 1, w, 6, 3, col::DIV);
      const uint16_t ink = look == BarLook::Pressed ? accent::NowPlaying : col::DIM;
      if (knob > x0) c.fillRoundRect(x0, 1, knob - x0, 6, 3, ink);
      if (look == BarLook::Off) knobInk = col::DIM;
    }
  }
  if (scrub) {
    // The text row stays blank: the readout above says it all.
    if (marker >= 0) c.fillRect(marker - 1, 0, 2, 9, col::DIM);
  } else {
    drawProgressText(c, paused);
  }
  // The knob last (the text's background fills its rows).
  if (knob >= 0) {
    if (look == BarLook::Rest) {
      c.fillCircle(knob, kKnobY, 3, knobInk);
    } else {
      c.fillCircle(knob, kKnobY, 4, knobInk);
      if (knobInk == col::TXT) c.fillCircle(knob, kKnobY, 2, accent::NowPlaying);
    }
  }
  gfx::push(c, 0, kProgressY, kW, kProgressH);
  drawn_.bar = static_cast<uint8_t>(look);
  drawn_.knobX = static_cast<int16_t>(knob);
  drawn_.markerX = static_cast<int16_t>(scrub ? bar_.markerX() : -1);
}

// The band's text row: the elapsed time, "Paused, 4 of 16 · SPYDRONE" and
// the sleep timer's moon, the length.
void NowPlayingPage::drawProgressText(M5Canvas& c, bool paused) {
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  const int x0 = SeekBar::kLineX, w = SeekBar::kLineW;
  constexpr int kTextY = 14;
  char t[24];
  if (s.current >= 0) {
    mmss(s.positionMs, t, sizeof(t));
    f.draw(c, Font::Small, t, x0, kTextY, 60, paused ? col::AMBER : col::SOFT, col::BG);
    if (s.durationMs > 0) {
      mmss(s.durationMs, t, sizeof(t));
    } else {
      snprintf(t, sizeof(t), "--:--");
    }
    f.draw(c, Font::Small, t, x0 + w, kTextY, 60, col::SOFT, col::BG, Fonts::Align::Right);
  }
  char mid[72];
  char base[40] = "";  // "Paused, 4 of 16": the line without its output
  char warn[48] = "";  // "SPYDRONE (not connected)": never dropped for the timer
  uint16_t mc = col::DIM;
  if (s.failed) {
    snprintf(mid, sizeof(mid), "Can't play this track");
    mc = col::RED;
  } else if (s.current < 0) {
    snprintf(mid, sizeof(mid), "The queue is empty");
  } else {
    const char* state = s.play == PlayState::Paused    ? "Paused, "
                        : s.play == PlayState::Stopped ? "Stopped, "
                        : s.play == PlayState::Waiting ? "Waiting, "
                                                       : "";
    snprintf(mid, sizeof(mid), "%s%lu of %lu", state, static_cast<unsigned long>(s.current + 1),
             static_cast<unsigned long>(s.queueSize));
    snprintf(base, sizeof(base), "%s", mid);
    if (s.play != PlayState::Playing) mc = col::AMBER;
    // Where it plays (mockup 01's output line): "4 of 16 · SPYDRONE", when
    // it fits between the times. The headphones not connected (and no
    // wait saying so above): that alone, if both don't fit.
    char where[48], withOutput[96];
    const char* out = outputName(where, sizeof(where));
    if (s.onBluetooth && !s.btConnected && s.play != PlayState::Waiting) snprintf(warn, sizeof(warn), "%s", out);
    snprintf(withOutput, sizeof(withOutput), "%s \xC2\xB7 %s", mid, out);
    if (f.width(Font::Small, withOutput) <= kMidW) {
      snprintf(mid, sizeof(mid), "%s", withOutput);
    } else if (s.onBluetooth && !s.btConnected && s.play != PlayState::Waiting) {
      snprintf(mid, sizeof(mid), "%s", out);
      mc = col::AMBER;
    }
  }
  // The sleep timer: a moon and "23 min" ("track", "45 s", "fading") after
  // the line; what gives way when both don't fit is uitext::sleepLineFit()'s
  // (the output's name first; never the headphones' not-connected warning).
  if (s.sleepShort[0] && s.current >= 0 && !s.failed) {
    using namespace uitext;
    using Text = SleepLine::Text;
    const int sw = kSleepMoonW + f.width(Font::Small, s.sleepShort);
    const SleepLine fit = sleepLineFit(f.width(Font::Small, mid), f.width(Font::Small, base),
                                       warn[0] ? f.width(Font::Small, warn) : -1, sw, kMidW);
    const char* text = fit.text == Text::Full ? mid : fit.text == Text::Base ? base : fit.text == Text::Warn ? warn : "";
    const uint16_t tc = fit.text == Text::Warn ? col::AMBER : mc;
    const int tw = text[0] ? f.width(Font::Small, text) : 0;
    const int x0 = kW / 2 - fit.width / 2;
    if (text[0]) f.draw(c, Font::Small, text, x0, kTextY, tw, tc, col::BG);
    const int sx = text[0] ? x0 + tw + kSleepGap : x0;
    const uint16_t sc = s.sleepFading ? col::AMBER : col::SOFT;
    if (fit.moon) icons::drawMoon(c, sx, kTextY - 6, 11, sc);
    if (fit.moonText) f.draw(c, Font::Small, s.sleepShort, sx + kSleepMoonW, kTextY, sw - kSleepMoonW, sc, col::BG);
  } else {
    f.draw(c, Font::Small, mid, kW / 2, kTextY, kMidW, mc, col::BG, Fonts::Align::Centre);
  }
}

void NowPlayingPage::drawReadout() {
  using namespace uitext;
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  c.fillRect(0, 0, kW, kReadoutH, col::BG);
  // Title centred on the row's row 16, Small on row 19: their baselines
  // roughly agree.
  constexpr int kBigY = 16, kSmallY = 19;
  if (bar_.phase() == SeekBar::Phase::Off) {
    f.draw(c, Font::Bold, kSeekCancel, kW / 2, kBigY, SeekBar::kLineW, col::AMBER, col::BG, Fonts::Align::Centre);
  } else {
    // The finger's second and the change; back where it plays, that second
    // and "no change". On the side away from the knob (a finger on the bar
    // covers what is above it).
    char big[16], small[24];
    if (bar_.staying()) {
      mmss(bar_.liveMs(), big, sizeof(big));
      snprintf(small, sizeof(small), "%s", kSeekStay);
    } else {
      mmss(bar_.targetMs(), big, sizeof(big));
      SeekBar::changeText(bar_.targetMs(), bar_.liveMs(), small, sizeof(small));
    }
    const int bw = std::min(f.width(Font::Title, big), kSeekReadoutW);
    const int sw = std::max(0, std::min(f.width(Font::Small, small), kSeekReadoutW - bw - kSeekReadoutGap));
    const int x = bar_.readoutLeft() ? SeekBar::kLineX
                                     : SeekBar::kLineX + SeekBar::kLineW - (bw + kSeekReadoutGap + sw);
    f.draw(c, Font::Title, big, x, kBigY, bw, col::TXT, col::BG);
    if (sw > 0) f.draw(c, Font::Small, small, x + bw + kSeekReadoutGap, kSmallY, sw, col::DIM, col::BG);
  }
  // Entering the scrub's look (or after the middle was drawn again) while
  // a play waits: the buttons' upper halves (y 94-112) would stick out
  // above the readout, so the artist row goes too.
  if (!drawn_.scrubUp && ui_.state().play == PlayState::Waiting) {
    gfx::fill(kColumnX, kArtistY, kW - kColumnX, kArtistH, col::BG);
  }
  gfx::push(c, 0, kReadoutY, kW, kReadoutH);
  drawn_.scrubUp = true;
  drawn_.readout = bar_.readout();
}

void NowPlayingPage::endScrub() {
  // The rows the readout took back: their background, the cover's lowest
  // rows from its sprite (no re-render, no thumbnail lookup), then the
  // rows (or the waiting panel); the band follows in its rest look
  // (update()).
  gfx::fill(0, kReadoutY, kW, kReadoutH, col::BG);
  drawn_.scrubUp = false;
  const int from = kReadoutY - (kCoverY - 1);  // the sprite's row on the readout's first
  if (cover_) {
    gfx::pushRows(*cover_, kCoverX - 1, kCoverY - 1, kCoverPx + 2, from, kCoverPx + 2);
  } else {
    gfx::fill(kCoverX, kReadoutY, kCoverPx, kCoverY + kCoverPx - kReadoutY, col::CARD);
  }
  drawMiddle();
  drawn_.readout = SeekBar::Readout{};
}

const char* NowPlayingPage::outputName(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  if (!s.onBluetooth) return "Speaker";
  const char* name = s.btName[0] ? s.btName : "Headphones";
  if (s.btConnected) return name;
  // Not "connecting...": after a night they may only be looked for now
  // and then, and a play would wait for them.
  snprintf(buf, size, "%s (not connected)", name);
  return buf;
}

uint32_t NowPlayingPage::outputSig() const {
  const AppState& s = ui_.state();
  const bool red = s.btLost || s.btSession.failed();
  uint32_t h = (s.onBluetooth ? 1u : 0u) | (s.btConnected ? 2u : 0u) | (red ? 4u : 0u);
  for (const char* p = s.btName; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  return h;
}

// What the waiting panel shows: the name, and how the connection goes.
uint32_t NowPlayingPage::waitSig() const {
  const AppState& s = ui_.state();
  uint32_t h = static_cast<uint32_t>(s.btLink.phase) * 131u + s.btLink.attempt * 7u + s.btLink.attempts;
  h = h * 31u + static_cast<uint32_t>(pressed_ + 1);
  for (const char* p = s.btName; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  return h;
}

void NowPlayingPage::drawMiddle() {
  if (ui_.state().play == PlayState::Waiting) {
    drawWaiting();
  } else {
    drawArtistAlbum();
  }
}

// While a play waits: [Play on speaker] and [Cancel] in the artist and
// album rows (Cancel reaches the screen's edge; the status is the title
// strip's, drawTitle()).
void NowPlayingPage::drawWaiting() {
  using namespace uitext;
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const int w = kW - kColumnX;
  const int h = kArtistH + kAlbumH;
  // Neither is the accent: out loud is a choice, not the way on. Drawn
  // y 94-129, each taking y 90-137 (zoneAt()).
  c.fillRect(0, 0, w, h, col::BG);
  const uint16_t sp = pressed_ == WaitSpeaker ? col::BTN_HI : col::BTN;
  c.fillRoundRect(kWaitSpeakerX, 4, kWaitSpeakerW, 36, 8, sp);
  f.draw(c, Font::Body, kPlayOnSpeaker, kWaitSpeakerX + kWaitSpeakerW / 2, 22, kWaitSpeakerW - kWaitButtonPad,
         col::TXT, sp, Fonts::Align::Centre);
  const uint16_t cn = pressed_ == WaitCancel ? col::BTN_HI : col::BTN;
  c.fillRoundRect(kWaitCancelX, 4, kWaitCancelW, 36, 8, cn);
  f.draw(c, Font::Body, kWaitCancel, kWaitCancelX + kWaitCancelW / 2, 22, kWaitCancelW - kWaitButtonPad, col::TXT, cn,
         Fonts::Align::Centre);
  gfx::push(c, kColumnX, kArtistY, w, h);
}

void NowPlayingPage::drawTransport() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  c.fillRect(0, 0, kW, kTransportDrawH, col::BG);
  const int cy = kTransportDrawH / 2;  // y 204
  for (int z = 0; z < 5; ++z) {
    const int cx = z * kZoneW + kZoneW / 2;
    const bool down = pressed_ == static_cast<Zone>(Volume + z);
    if (down && z != 2) c.fillCircle(cx, cy, 24, col::BTN_HI);
    switch (z) {
      case 0: {  // the output and its volume: tap for the volume sheet
        const uint16_t off = s.btLost || s.btSession.failed() ? col::RED : col::AMBER;
        const uint16_t ic = s.onBluetooth ? (s.btConnected ? col::CYAN : off) : col::SOFT;
        icons::drawCentred(c, s.onBluetooth ? icons::kHeadphones : icons::kSpeaker, cx, cy - 9, ic);
        char v[6];
        snprintf(v, sizeof(v), "%u%%", static_cast<unsigned>(s.volume));
        f.draw(c, Font::Small, v, cx, cy + 14, kZoneW, col::DIM, down ? col::BTN_HI : col::BG, Fonts::Align::Centre);
        break;
      }
      case 1: icons::drawCentred(c, icons::kPrev, cx, cy, col::TXT); break;
      case 2: drawPlayButton(c, cx, cy, down); break;
      case 3: icons::drawCentred(c, icons::kNext, cx, cy, col::TXT); break;
      default:
        // The dots always at the same height (they don't jump when the
        // indicator comes or goes); the indicator under them, on the
        // control that changes it, as the volume's "60%" under its icon.
        icons::drawCentred(c, icons::kMore, cx, cy - 9, col::SOFT);
        drawModes(c, cx, cy + 14);
        break;
    }
  }
  gfx::push(c, 0, kTransportDrawY, kW, kTransportDrawH);
  drawn_.shuffle = s.shuffle;
  drawn_.repeat = s.repeat;
}

void NowPlayingPage::drawModes(M5Canvas& c, int cx, int cy) {
  // Shuffle then repeat (the loop; One's has a bold "1" beside it), centred
  // as a group, in the accent; bitmaps, so a pressed zone's circle shows
  // through. The widest group, shuffle and One, is 38 x 11 at x 269-306,
  // y 213-223: the pressed circle's width at its middle row
  // (tools/ui_icons.py checks it).
  const AppState& s = ui_.state();
  const icons::Icon* glyphs[2] = {};
  int n = 0;
  if (s.shuffle) glyphs[n++] = &icons::kShuffleSmall;
  if (s.repeat == 1) glyphs[n++] = &icons::kRepeatSmall;
  if (s.repeat == 2) glyphs[n++] = &icons::kRepeatOneSmall;
  if (n == 0) return;
  int width = (n - 1) * kModesGap;
  for (int i = 0; i < n; ++i) width += glyphs[i]->w;
  int x = cx - width / 2;
  for (int i = 0; i < n; ++i) {
    icons::draw(c, *glyphs[i], x, cy - glyphs[i]->h / 2, accent::NowPlaying);
    x += glyphs[i]->w + kModesGap;
  }
}

void NowPlayingPage::drawPlayButton(M5Canvas& c, int cx, int cy, bool down) {
  const PlayState play = ui_.state().play;
  const uint16_t disc = down ? col::SOFT : accent::NowPlaying;
  c.fillCircle(cx, cy, 25, disc);  // y 179-229: off the bezel
  if (play == PlayState::Waiting) {
    // A ring of 8 dots, one big (the step): waiting; a tap cancels.
    static const int8_t kDx[8] = {0, 8, 11, 8, 0, -8, -11, -8};
    static const int8_t kDy[8] = {-11, -8, 0, 8, 11, 8, 0, -8};
    for (int i = 0; i < 8; ++i) {
      const int age = (spin_ - i + 8) % 8;
      c.fillCircle(cx + kDx[i], cy + kDy[i], age == 0 ? 3 : age < 3 ? 2 : 1, col::DARK);
    }
    return;
  }
  icons::drawCentred(c, play == PlayState::Playing ? icons::kPause : icons::kPlay, cx + (play == PlayState::Playing ? 0 : 2),
                     cy, col::DARK);
}

void NowPlayingPage::drawPlayZone() {
  M5Canvas& c = gfx::strip();
  c.fillRect(0, 0, kZoneW, kTransportDrawH, col::BG);
  drawPlayButton(c, kZoneW / 2, kTransportDrawH / 2, pressed_ == PlayPause);
  gfx::push(c, 2 * kZoneW, kTransportDrawY, kZoneW, kTransportDrawH);
}

bool NowPlayingPage::emptyState(EmptyState& e) const {
  const AppState& s = ui_.state();
  if (!s.card && s.libraryTracks == 0) {
    noCardState(e, s.cardKind);
    return true;
  }
  e.icon = &icons::kNote;
  e.title = "Nothing playing";
  e.line1 = uitext::kPickInLibrary;  // (fits: the longer one didn't)
  e.buttons[0] = "Open Library";
  e.buttonIcons[0] = &icons::kLibrary;
  if (s.libraryTracks > 0) {
    e.buttons[1] = "Shuffle all";
    e.buttonIcons[1] = &icons::kShuffle;
  }
  return true;
}

void NowPlayingPage::onEmptyEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  EmptyState es;
  emptyState(es);
  const int b = emptyStateButtonAt(es, kContentY, kH - kContentY, e);
  if (e.type == T::Down) {
    emptyPressed_ = b;
    if (b >= 0) drawEmptyState(es, kContentY, kH - kContentY, accent::NowPlaying, emptyPressed_);
    return;
  }
  if (e.type != T::Tap && e.type != T::DragStart && e.type != T::Release && e.type != T::Cancel) return;
  const int was = emptyPressed_;
  emptyPressed_ = -1;
  if (was >= 0) drawEmptyState(es, kContentY, kH - kContentY, accent::NowPlaying, -1);
  if (e.type != T::Tap || was < 0) return;
  ui_.tick();
  const AppState& s = ui_.state();
  if (!s.card && s.libraryTracks == 0) {
    ui_.retryCard();
  } else if (was == 0) {
    ui_.showTab(NavModel::Tab::Library);
  } else {
    ui_.shuffleAll();
  }
}

bool NowPlayingPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  (void)wholeRows;
  const AppState& s = ui_.state();
  // The seek bar's touch, on an entry that isn't current any more (a join
  // heard, a natural end, a skip from the headphones or the console, an
  // edit): it ends with nothing, and the rest of the touch goes nowhere (a
  // lift before this sees it is seek()'s Moved).
  if (bar_.active() && (s.current < 0 || s.currentKey != bar_.key())) {
    bar_.cancel();
    pressed_ = None;
    ui_.input().cancelTouch(nowMs);
    Serial.println("[ui] now playing: no seek (the track changed under the finger)");
  }
  if (bar_.active()) bar_.live(s.positionMs);  // the marker follows where it plays
  // Nothing queued: the empty state, drawn when it (or the card) changes.
  const bool empty = s.current < 0;
  const bool noCard = !s.card && s.libraryTracks == 0;
  if (empty) {
    if (!drawn_.valid || !drawn_.empty || drawn_.noCard != noCard || drawn_.cardKind != s.cardKind) {
      EmptyState es;
      emptyState(es);
      drawEmptyState(es, kContentY, kH - kContentY, accent::NowPlaying, emptyPressed_);
      drawn_ = Drawn{};
      drawn_.valid = true;
      drawn_.empty = true;
      drawn_.noCard = noCard;
      drawn_.cardKind = s.cardKind;
    }
    return false;
  }
  if (drawn_.empty) {
    repaint();  // a track: the player again, all of it
    return false;
  }
  const bool all = !drawn_.valid;
  const bool newTrack = all || s.trackId != drawn_.track || (s.current < 0) != (drawn_.current < 0);
  const bool waiting = s.play == PlayState::Waiting;
  const uint32_t wsig = waiting ? waitSig() : 0;
  if (!waiting && (pressed_ == WaitSpeaker || pressed_ == WaitCancel)) pressed_ = None;
  const BarLook look = barLook();
  const bool scrub = look == BarLook::Scrubbing || look == BarLook::Off;
  // The title strip with the rows under it: it holds the wait's status
  // while a play waits, so it follows the wait too.
  if (newTrack || waiting != drawn_.waiting || wsig != drawn_.waitSig) {
    if (waiting != drawn_.waiting) fillNav();  // (the press look can't outlive a wait's start)
    drawTitle();
    drawMiddle();
    if (scrub) {
      // (A repaint after a toast, the waiting panel's "try 2 of 3"): the
      // readout goes back over the album row in the same pass.
      drawn_.scrubUp = false;
      drawReadout();
    }
  }
  // A scrub ended (a lift, a cancel): the rows and the cover's lowest rows
  // back at once; the band below, in its rest look.
  if (drawn_.scrubUp && !scrub) endScrub();
  // The cover: when the album changes (a track of the same album keeps it).
  if (all || (newTrack && playingAlbum() != drawn_.coverAlbum)) drawCover();
  const uint32_t second = s.positionMs / 1000;
  const uint32_t durS = s.durationMs / 1000;
  const uint32_t output = outputSig();
  uint32_t sleep = s.sleepFading ? 1u : 0u;
  for (const char* p = s.sleepShort; *p; ++p) sleep = sleep * 31u + static_cast<unsigned char>(*p);
  bool frame = false;
  if (scrub) {
    // A finger scrubs: a frame (the band, and the readout when its text or
    // side changed) only on the frame deadlines, when the look, the knob,
    // the marker or the readout moved.
    const bool text = !drawn_.scrubUp || bar_.readout() != drawn_.readout;
    const bool moved = static_cast<uint8_t>(look) != drawn_.bar || bar_.knobX() != drawn_.knobX ||
                       bar_.markerX() != drawn_.markerX;
    if (all || ((text || moved) && frameDue)) {
      if (text) drawReadout();
      drawProgress();
      frame = !all;
    }
  } else if (all || static_cast<uint8_t>(look) != drawn_.bar || second != drawn_.second || durS != drawn_.durationS ||
             s.play != drawn_.play || s.current != drawn_.current || s.queueSize != drawn_.size ||
             output != drawn_.output || sleep != drawn_.sleep) {
    drawProgress();  // (a new look at once: the pressed one on a Down, the rest after a touch)
  }
  if (all || s.play != drawn_.play || s.volume != drawn_.volume || s.onBluetooth != drawn_.bluetooth ||
      output != drawn_.output || s.shuffle != drawn_.shuffle || s.repeat != drawn_.repeat) {
    drawTransport();
    nextSpinMs_ = nowMs + kSpinMs;
  } else if (waiting && static_cast<int32_t>(nowMs - nextSpinMs_) >= 0) {
    // The spinner's next step: only its zone.
    nextSpinMs_ = nowMs + kSpinMs;
    spin_ = static_cast<uint8_t>((spin_ + 1) % 8);
    drawPlayZone();
  }
  drawn_.valid = true;
  drawn_.waiting = waiting;
  drawn_.waitSig = wsig;
  drawn_.track = s.trackId;
  drawn_.play = s.play;
  drawn_.second = second;
  drawn_.durationS = durS;
  drawn_.current = s.current;
  drawn_.size = s.queueSize;
  drawn_.volume = s.volume;
  drawn_.bluetooth = s.onBluetooth;
  drawn_.output = output;
  drawn_.sleep = sleep;
  return frame;
}

NowPlayingPage::Zone NowPlayingPage::zoneAt(const InputEvent& e) const {
  // The transport: five zones of 64 x 72 ("..." reaches the edge).
  if (e.y >= kTransportY) {
    if (e.atRightEdge()) return More;
    const int z = e.x / kZoneW;
    return static_cast<Zone>(Volume + (z < 0 ? 0 : z > 4 ? 4 : z));
  }
  // The seek bar: the full width, from 8 px above the band (the line is
  // drawn at the band's top), whether or not a play waits.
  if (e.y >= kProgressY - kSeekReachPx) return Bar;
  // The cover's column: the navigation menu, waiting or not.
  if (e.x < kColumnX) return Nav;
  if (ui_.state().play == PlayState::Waiting) {
    // The title strip holds the wait's status (inert); the rows under it
    // the buttons, which take y 90-137. Cancel reaches the edge.
    if (e.y < kArtistY) return None;
    const bool cancel = e.atRightEdge() || e.x - kColumnX >= uitext::kWaitCancelX - 3;
    return cancel ? WaitCancel : WaitSpeaker;
  }
  // Everything else above the bar, its margins too: no dead pixels, and no
  // x test for the panel's skew to fool.
  return Nav;
}

void NowPlayingPage::openNavMenu() {
  using namespace uitext;
  const AppState& s = ui_.state();
  navTrack_ = s.current >= 0 ? s.trackId : TrackCatalog::kNone;
  const LibraryIndex* index = ui_.library().index();
  // No sheet when nothing in it could act: a toast says why.
  if (TrackCatalog::isBuiltin(navTrack_)) {
    Serial.println("[ui] now playing: no navigation menu (a built-in track)");
    ui_.toast(kBuiltinNotInLibrary, false);
    return;
  }
  if (!index || !index->ready() || navTrack_ >= index->trackCount()) {
    Serial.println("[ui] now playing: no navigation menu (the Library isn't ready)");
    ui_.toast(kLibraryNotReady, false);
    return;
  }
  if (ui_.browsingSynthetic()) {
    Serial.println("[ui] now playing: no navigation menu (a synthetic library)");
    ui_.toast("Browsing a synthetic library (uil0: the card's)", false);
    return;
  }
  const TrackCatalog& cat = ui_.player().catalog();
  // The folder, cut from the left by whole folders to Go to folder's room
  // ("…/Daft Punk/Discovery"); "" if its path can't be had.
  navFolder_[0] = 0;
  char path[256];
  if (index->folderPath(index->track(navTrack_).folder, path, sizeof(path))) {
    Fonts& f = Fonts::instance();
    const int room = sheet::detailRoom(f.width(Font::Body, kGoTo[2]));
    textfit::cutPathLeft(f.fit(Font::Small), path, room, navFolder_, sizeof(navFolder_));
  }
  const char* artist = cat.artist(navTrack_);
  const char* album = cat.album(navTrack_);
  const char* details[3] = {artist[0] ? artist : kNoArtistFolder, album[0] ? album : kLooseTracks, navFolder_};
  char title[128];
  cat.title(navTrack_, title, sizeof(title));
  Serial.println("[ui] now playing: the navigation menu");
  ui_.openSheet(this, title, kGoTo, 3, details);
  ask_ = Ask::Nav;
}

void NowPlayingPage::openPlaybackMenu() {
  using namespace uitext;
  const AppState& s = ui_.state();
  static const char* const kRows[3] = {kShuffleRow, kRepeatRow, kSleepRow};
  const uint8_t repeat = s.repeat < 3 ? s.repeat : 0;
  const char* details[3] = {kOnOff[s.shuffle ? 1 : 0], kRepeatModes[repeat], s.sleepRow};
  Serial.printf("[ui] now playing: the playback menu (shuffle %s, repeat %s, sleep timer %s)\n",
                s.shuffle ? "on" : "off", repeatName(repeat), s.sleepRow);
  ui_.openSheet(this, kPlaybackTitle, kRows, 3, details);
  // Each row's state follows while it is up (a change from the console
  // shows too); Shuffle and Repeat change in place, the sheet staying.
  ui_.sheetFollows(0, Ui::SheetFollow::Shuffle);
  ui_.sheetFollows(1, Ui::SheetFollow::Repeat);
  ui_.sheetFollows(2, Ui::SheetFollow::Sleep);
  ui_.sheetStays(0);
  ui_.sheetStays(1);
  ask_ = Ask::Playback;
}

void NowPlayingPage::goToLibrary(Go where, uint32_t track) {
  // The track the menu was opened for (it acts on that one, even if
  // another plays now), with the menu's checks again: the index may have
  // gone while the sheet was up (a rebuild).
  const LibraryIndex* index = ui_.library().index();
  if (track == TrackCatalog::kNone) return;
  if (TrackCatalog::isBuiltin(track)) {
    ui_.toast(uitext::kBuiltinNotInLibrary, false);
    return;
  }
  if (!index || !index->ready() || track >= index->trackCount()) {
    ui_.toast(uitext::kLibraryNotReady, false);
    return;
  }
  if (ui_.browsingSynthetic()) {
    ui_.toast("Browsing a synthetic library (uil0: the card's)", false);
    return;
  }
  const LibraryIndex::Track& t = index->track(track);
  NavModel::PageRef pages[NavModel::kMaxDepth];
  int n = 0;
  auto page = [&](PageKind kind, uint32_t id) {
    NavModel::PageRef p;
    p.kind = static_cast<uint8_t>(kind);
    p.id = id;
    p.scrollPx = kShowPlaying;  // opened at the playing track (or what holds it)
    pages[n++] = p;
  };
  switch (where) {
    case Go::Artist:
      page(PageKind::Artist, t.artist);
      Serial.printf("[ui] now playing: go to the artist %lu\n", (unsigned long)t.artist);
      ui_.showLibrary(LibrarySegment::Artists, pages, n);
      break;
    case Go::Album:
      // The album one Back from its artist.
      page(PageKind::Artist, t.artist);
      page(PageKind::Album, t.album);
      Serial.printf("[ui] now playing: go to the album %lu\n", (unsigned long)t.album);
      ui_.showLibrary(LibrarySegment::Artists, pages, n);
      break;
    case Go::Folders: {
      // Every folder from /music down to the track's, each a Back from the next.
      uint32_t chain[32];
      int depth = 0;
      for (uint32_t f = t.folder; f != LibraryIndex::rootFolder() && f != LibraryIndex::kNone && depth < 32;
           f = index->folder(f).parent) {
        chain[depth++] = f;
      }
      const int keep = std::min(depth, NavModel::kMaxDepth - 1);
      for (int i = keep - 1; i >= 0; --i) page(PageKind::Folder, chain[i]);
      Serial.printf("[ui] now playing: go to the folder (%d deep)\n", depth);
      ui_.showLibrary(LibrarySegment::Folders, pages, n);
      break;
    }
  }
}

void NowPlayingPage::seekTo(const SeekBar::Out& o, uint32_t nowMs) {
  using S = PlaybackController::Seek;
  PlaybackController& player = ui_.player();
  // The entry the finger landed on, at the length the bar showed then.
  const S r = player.seek(bar_.key(), o.ms, bar_.lengthMs());
  if (r == S::Moved) {
    Serial.println("[ui] now playing: no seek (the track changed under the finger)");
    return;
  }
  if (r == S::NoPlace) {
    Serial.println("[ui] now playing: no seek (nothing to seek in)");
    return;
  }
  // A tap that acted ticks (a drag's lift doesn't: the audio's jump says it).
  if (o.tap) ui_.tick();
  char from[16], to[16], of[16], how[48];
  mmss(bar_.liveMs(), from, sizeof(from));
  mmss(o.ms, to, sizeof(to));
  mmss(bar_.lengthMs(), of, sizeof(of));
  if (o.tap) {
    snprintf(how, sizeof(how), "tap");
  } else {
    // How long the last second had been shown before the lift: the measure
    // for a lift guard (docs/SEEK-BAR.md section 13).
    snprintf(how, sizeof(how), "%s, held %lu ms", bar_.knobGrab() ? "drag from the knob" : "drag",
             static_cast<unsigned long>(bar_.heldMs(nowMs)));
  }
  const PlayState st = player.state();
  const char* then = r == S::Started              ? "plays from there"
                     : st == PlayState::Waiting ? "waiting, it starts there when they connect"
                     : st == PlayState::Stopped ? "stopped, the next play starts there"
                                                : "paused, the next play starts there";
  Serial.printf("[ui] now playing: seek %s -> %s of %s (%s): %s\n", from, to, of, how, then);
}

void NowPlayingPage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  // A swipe up from the button strip: nothing here scrolls, and it presses
  // nothing (the transport is right above the strip).
  if (e.fromStrip) return;
  if (drawn_.empty) {
    onEmptyEvent(e);
    return;
  }
  const AppState& s = ui_.state();
  // A touch the seek bar took is all its own until it ends (a thumb that
  // dips into the transport presses nothing there). update() draws.
  if (bar_.active() && e.type != T::Down) {
    const SeekBar::Out o = bar_.onEvent(e, s.positionMs);
    if (o.tick) ui_.tick();  // the scrub began, into the detent, off or back
    switch (o.end) {
      case SeekBar::End::Seek: seekTo(o, e.ms); break;
      case SeekBar::End::Stay: Serial.println("[ui] now playing: no seek (back where it plays)"); break;
      case SeekBar::End::Off: Serial.println("[ui] now playing: no seek (slid off the bar)"); break;
      case SeekBar::End::Cancel: Serial.println("[ui] now playing: no seek (cancelled)"); break;
      default: break;  // still on it; or a drag that wasn't sideways, let go
    }
    if (!bar_.active()) pressed_ = None;
    return;
  }
  bar_.cancel();  // (a Down: a touch whose end never came is over)
  const Zone z = zoneAt(e);
  const bool waiting = s.play == PlayState::Waiting;
  if (e.type == T::Down) {
    pressed_ = z;
    if (z == Bar) {
      // Taken only when it can seek (no tick either way: a Down only
      // highlights); a finger that rests on it, then slides, still scrubs,
      // and one that rests, then lifts, is a tap: never a long press.
      if (seekable() && bar_.down(e, s.currentKey, s.positionMs, s.durationMs)) {
        ui_.input().noHold();
      } else {
        pressed_ = None;
      }
      return;
    }
    // The navigation area lights (not while a play waits). No hold here: a
    // long press ends as a slow tap (Ui), the menu with the tap's tick.
    if (z == Nav && !waiting) drawNav();
    if (z == WaitSpeaker || z == WaitCancel) drawWaiting();
    if (z >= Volume) drawTransport();
    return;
  }
  if (e.type != T::Tap && e.type != T::DragStart && e.type != T::Release && e.type != T::Cancel) return;
  const Zone was = pressed_;
  pressed_ = None;
  // The press look off first: before any sheet opens (it leaves y 36-79 in
  // view), and for a drag or a lift that does nothing.
  if (was == Nav && !waiting) drawNav();
  if (was == WaitSpeaker || was == WaitCancel) drawMiddle();  // (the wait may have ended meanwhile)
  if (was >= Volume) drawTransport();
  if (e.type != T::Tap || was == None) return;
  ui_.tick();
  switch (was) {
    case WaitSpeaker:
      Serial.println("[ui] now playing: play on the speaker (the wait for the headphones)");
      ui_.host().playOnSpeaker();
      break;
    case WaitCancel:
      Serial.println("[ui] now playing: cancel the wait for the headphones");
      if (ui_.state().play == PlayState::Waiting) ui_.host().playPause();
      break;
    case Nav: openNavMenu(); break;
    case Volume: ui_.openVolume(); break;
    case Prev:
      Serial.println("[ui] now playing: previous");
      ui_.host().prev();
      break;
    case PlayPause:
      Serial.println(ui_.state().play == PlayState::Waiting ? "[ui] now playing: cancel the wait"
                                                            : "[ui] now playing: play/pause");
      ui_.host().playPause();
      break;
    case Next:
      Serial.println("[ui] now playing: next");
      ui_.host().next();
      break;
    case More: openPlaybackMenu(); break;
    default: break;
  }
}

void NowPlayingPage::onSheet(int choice) {
  const AppState& s = ui_.state();
  if (choice < 0) {
    ask_ = Ask::None;  // ✕, a tap outside, or a modal that closed it
    return;
  }
  if (ask_ == Ask::Nav) {
    ask_ = Ask::None;
    if (choice <= 2) goToLibrary(static_cast<Go>(choice), navTrack_);
    return;
  }
  if (ask_ != Ask::Playback) return;
  switch (choice) {
    case 0:
      // In place: the sheet stays up (Ui::sheetStays()), its row follows.
      Serial.printf("[ui] now playing: shuffle %s\n", s.shuffle ? "off" : "on");
      ui_.host().setShuffle(!s.shuffle);
      break;
    case 1: {
      const uint8_t next = static_cast<uint8_t>(((s.repeat < 3 ? s.repeat : 0) + 1) % 3);  // Off, All, One, Off
      Serial.printf("[ui] now playing: repeat %s\n", repeatName(next));
      ui_.host().setRepeat(next);
      break;
    }
    case 2:
      ask_ = Ask::None;
      ui_.openSleepSheet();
      break;
    default: break;
  }
}

void NowPlayingPage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  const int n = snprintf(buf, size, "Now Playing: track id %lu, entry %ld of %lu, %lu / %lu ms, cover of album %ld %s",
                         static_cast<unsigned long>(s.trackId), static_cast<long>(s.current),
                         static_cast<unsigned long>(s.queueSize), static_cast<unsigned long>(s.positionMs),
                         static_cast<unsigned long>(s.durationMs),
                         static_cast<long>(drawn_.coverAlbum == LibraryIndex::kNone ? -1
                                                                                    : static_cast<long>(drawn_.coverAlbum)),
                         drawn_.coverShown ? "(its thumbnail)" : "(the placeholder)");
  if (n < 0 || static_cast<size_t>(n) >= size) return;
  // The seek bar (docs/SEEK-BAR.md section 8).
  char* rest = buf + n;
  size_t room = size - static_cast<size_t>(n);
  int m = 0;
  switch (barLook()) {
    case BarLook::Inert: m = snprintf(rest, room, "; the bar: inert"); break;
    case BarLook::Rest:
      m = snprintf(rest, room, "; the bar: rest, the knob at x %d",
                   SeekBar::kLineX + SeekBar::xOf(s.positionMs, s.durationMs));
      break;
    case BarLook::Pressed: m = snprintf(rest, room, "; the bar: pressed"); break;
    case BarLook::Off: m = snprintf(rest, room, "; the bar: off"); break;
    case BarLook::Scrubbing: {
      if (bar_.staying()) {
        m = snprintf(rest, room, "; the bar: staying");
        break;
      }
      char to[16], live[16];
      mmss(bar_.targetMs(), to, sizeof(to));
      mmss(bar_.liveMs(), live, sizeof(live));
      m = snprintf(rest, room, "; the bar: scrubbing to %s (%s; readout %s; %s plays)", to,
                   bar_.knobGrab() ? "drag from the knob" : "drag", bar_.readoutLeft() ? "left" : "right", live);
      break;
    }
  }
  if (m < 0 || static_cast<size_t>(m) >= room) return;
  // The playback modes (docs/QUEUE-MODES.md), as the indicator shows them.
  snprintf(rest + m, room - static_cast<size_t>(m), "; shuffle %s, repeat %s", s.shuffle ? "on" : "off",
           repeatName(s.repeat));
}

}  // namespace ui
