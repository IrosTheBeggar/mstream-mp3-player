// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Now Playing (the tab bar spec §6.1, mockups 01-04, with the review's
// grafts): the cover, the title, the artist and the album as 40 px bands
// that open them in the Library at the playing track, the progress and
// times, and the transport with the volume sheet and a "..." sheet; while
// play waits for the headphones, a spinner for play and the waiting panel
// (PlayGate). The layout is in Pages.h. The "..." zone reaches the
// screen's edge.
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
constexpr int kTextX = 120;       // the title and the bands' text
constexpr int kColumnX = 112;     // what's right of the cover (drawn and hit)
constexpr int kTitleY = 38, kTitleH = 52;
constexpr int kArtistY = 90, kArtistH = 40;
constexpr int kAlbumY = 130, kAlbumH = 40;
constexpr int kProgressY = 170, kProgressH = 22;
constexpr int kTransportY = 192, kTransportH = 48;
constexpr int kZoneW = 64;
// The waiting panel (over the artist and album bands): its text in the
// artist band, its buttons in the album band.
constexpr int kWaitButtonsY = kAlbumY;
constexpr uint32_t kSpinMs = 125;  // the waiting spinner: 8 steps a second
// The seek bar's touch reaches this far above the band (y 162): the line is
// drawn at the band's top (y 172-175), so a tap aimed at it lands on both
// sides of y 170. Not while play waits: Play on speaker and Cancel keep
// their whole area (y 170 then).
constexpr int kSeekReachPx = 8;
// The readout row while a finger scrubs: the 33 rows between the cover's
// frame and the band, the full width (the album band's lower rows).
constexpr int kReadoutY = 137, kReadoutH = 33;
static_assert(SeekBar::kLineX + SeekBar::kLineW == kW - 12, "the seek bar's line is drawProgress()'s");
static_assert(kReadoutY == kCoverY + kCoverPx + 1, "the readout row starts under the cover's frame");
static_assert(kReadoutY + kReadoutH == kProgressY, "the readout row ends at the band");

void mmss(uint32_t ms, char* buf, size_t size) {
  snprintf(buf, size, "%lu:%02lu", static_cast<unsigned long>(ms / 60000), static_cast<unsigned long>(ms / 1000 % 60));
}

// The readout's change from where it plays, in whole seconds: "+1:21",
// "−0:45" (U+2212; folded to "-" where a font lacks it).
void change(uint32_t targetMs, uint32_t liveMs, char* buf, size_t size) {
  const int32_t d = static_cast<int32_t>(targetMs / 1000) - static_cast<int32_t>(liveMs / 1000);
  const uint32_t a = static_cast<uint32_t>(d < 0 ? -d : d);
  snprintf(buf, size, "%s%lu:%02lu", d < 0 ? "\xE2\x88\x92" : "+", static_cast<unsigned long>(a / 60),
           static_cast<unsigned long>(a % 60));
}

// The last line of a wrap was cut ("…" added): the text needs more room.
bool cutShort(const char* line, const char* text) {
  const size_t n = strlen(line), t = strlen(text);
  const bool ends = (n >= 3 && strcmp(line + n - 3, "\xE2\x80\xA6") == 0) || (n >= 3 && strcmp(line + n - 3, "...") == 0);
  return ends && !(t >= 3 && strcmp(text + t - 3, line + n - 3) == 0);
}

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

void NowPlayingPage::repaint() {
  if (ui_.state().current < 0) {
    drawn_ = Drawn{};  // the empty state, all of it
    update(0, false, false);
    return;
  }
  // What the pieces don't cover: the rows above the title, the left column
  // under the cover.
  gfx::fill(0, kContentY, kW, kTitleY - kContentY, col::BG);
  gfx::fill(0, kTitleY, kColumnX, kCoverY - 1 - kTitleY, col::BG);
  gfx::fill(0, kCoverY + kCoverPx + 1, kColumnX, kProgressY - (kCoverY + kCoverPx + 1), col::BG);
  gfx::fill(0, kCoverY - 1, kCoverX - 1, kCoverPx + 2, col::BG);
  gfx::fill(kCoverX + kCoverPx + 1, kCoverY - 1, kColumnX - (kCoverX + kCoverPx + 1), kCoverPx + 2, col::BG);
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
  if (!cover_) {
    gfx::fill(kCoverX, kCoverY, kCoverPx, kCoverPx, col::CARD);
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
  gfx::push(c, kCoverX - 1, kCoverY - 1, kCoverPx + 2, kCoverPx + 2);
}

void NowPlayingPage::drawTitle() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const int w = kW - kColumnX;
  const int textW = 310 - kTextX;
  c.fillRect(0, 0, w, kTitleH, col::BG);
  char title[160] = "Nothing playing";
  if (s.current >= 0) ui_.player().catalog().title(s.trackId, title, sizeof(title));
  const uint16_t ink = s.current >= 0 ? col::TXT : col::DIM;
  const int x = kTextX - kColumnX;
  char lines[3][112];
  int n = textfit::wrap(f.fit(Font::Title), title, strlen(title), textW, 2, &lines[0][0], sizeof(lines[0]));
  if (n == 2 && cutShort(lines[1], title)) {
    // Too long for two big lines: three smaller ones.
    n = textfit::wrap(f.fit(Font::Bold), title, strlen(title), textW, 3, &lines[0][0], sizeof(lines[0]));
    for (int i = 0; i < n; ++i) f.draw(c, Font::Bold, lines[i], x, 9 + i * 17, textW, ink, col::BG);
  } else {
    for (int i = 0; i < n; ++i) f.draw(c, Font::Title, lines[i], x, n == 1 ? 26 : 13 + i * 26, textW, ink, col::BG);
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
  const int textW = w - x - 26;
  // The artist band.
  uint16_t bg = pressed_ == Artist ? col::ROW_SEL : col::BG;
  c.fillRect(0, 0, w, kArtistH, bg);
  const char* artist = s.current < 0 ? "Open the Library" : lib ? cat.artist(s.trackId) : "Built-in test track";
  if (lib && !artist[0]) artist = "(no artist folder)";
  f.draw(c, Font::Body, artist, x, kArtistH / 2, textW, s.current < 0 ? accent::Library : col::SOFT, bg);
  if (lib || s.current < 0) icons::drawCentred(c, icons::kChevronRight, w - 14, kArtistH / 2, col::FAINT);
  gfx::push(c, kColumnX, kArtistY, w, kArtistH);
  // The album band.
  bg = pressed_ == Album ? col::ROW_SEL : col::BG;
  c.fillRect(0, 0, w, kAlbumH, bg);
  if (lib) {
    const char* album = cat.album(s.trackId);
    f.draw(c, Font::Body, album[0] ? album : "(loose tracks)", x, kAlbumH / 2, textW, col::DIM, bg);
    icons::drawCentred(c, icons::kChevronRight, w - 14, kAlbumH / 2, col::FAINT);
  }
  gfx::push(c, kColumnX, kAlbumY, w, kAlbumH);
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

uint32_t NowPlayingPage::readoutSig() const {
  // What drawReadout() shows: off; staying (where it plays); or the
  // finger's second and the change from where it plays; and the side.
  if (bar_.phase() == SeekBar::Phase::Off) return 1;
  uint32_t h = bar_.readoutLeft() ? 2u : 3u;
  if (bar_.staying()) return h * 31u + 7u + bar_.liveMs() / 1000 * 131u;
  return (h * 31u + bar_.targetMs() / 1000) * 131u + bar_.liveMs() / 1000;
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
      change(bar_.targetMs(), bar_.liveMs(), small, sizeof(small));
    }
    const int bw = std::min(f.width(Font::Title, big), kSeekReadoutW);
    const int sw = std::max(0, std::min(f.width(Font::Small, small), kSeekReadoutW - bw - kSeekReadoutGap));
    const int x = bar_.readoutLeft() ? SeekBar::kLineX
                                     : SeekBar::kLineX + SeekBar::kLineW - (bw + kSeekReadoutGap + sw);
    f.draw(c, Font::Title, big, x, kBigY, bw, col::TXT, col::BG);
    if (sw > 0) f.draw(c, Font::Small, small, x + bw + kSeekReadoutGap, kSmallY, sw, col::DIM, col::BG);
  }
  // Entering the scrub's look (or after the middle was drawn again): the
  // album band's top rows too, which Waiting's buttons reach into (y 133).
  if (!drawn_.scrubUp) gfx::fill(kColumnX, kAlbumY, kW - kColumnX, kReadoutY - kAlbumY, col::BG);
  gfx::push(c, 0, kReadoutY, kW, kReadoutH);
  drawn_.scrubUp = true;
  drawn_.readout = readoutSig();
}

void NowPlayingPage::endScrub() {
  // The artist and album bands (or the waiting panel) and the left column
  // under the cover, as repaint() has them; the band follows in its rest
  // look (update()).
  drawMiddle();
  gfx::fill(0, kReadoutY, kColumnX, kReadoutH, col::BG);
  drawn_.scrubUp = false;
  drawn_.readout = 0;
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

// "Waiting for SPYDRONE..." over "try 2 of 3", then [Play on speaker] and
// [Cancel] (Cancel reaches the screen's edge).
void NowPlayingPage::drawWaiting() {
  using namespace uitext;
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const int w = kW - kColumnX;
  // The text, in the artist band.
  c.fillRect(0, 0, w, kArtistH, col::BG);
  char line[64];
  snprintf(line, sizeof(line), "Waiting for %s\xE2\x80\xA6", s.btName[0] ? s.btName : "the headphones");
  f.draw(c, Font::Small, line, kWaitTextX, 12, kWaitTextW, col::AMBER, col::BG);
  const BtLink& l = s.btLink;
  if (l.phase == BtLink::Phase::Paging && l.attempt > 0) {
    snprintf(line, sizeof(line), "try %u of %u", static_cast<unsigned>(l.attempt),
             static_cast<unsigned>(l.attempts > l.attempt ? l.attempts : l.attempt));
  } else if (l.phase == BtLink::Phase::Scanning || l.phase == BtLink::Phase::Backoff) {
    snprintf(line, sizeof(line), "looking for them");
  } else {
    snprintf(line, sizeof(line), "connecting");
  }
  f.draw(c, Font::Small, line, kWaitTextX, 30, kWaitTextW, col::DIM, col::BG);
  gfx::push(c, kColumnX, kArtistY, w, kArtistH);
  // The buttons, in the album band. Neither is the accent: out loud is a
  // choice, not the way on.
  c.fillRect(0, 0, w, kAlbumH, col::BG);
  const uint16_t sp = pressed_ == WaitSpeaker ? col::BTN_HI : col::BTN;
  c.fillRoundRect(kWaitSpeakerX, 3, kWaitSpeakerW, 34, 8, sp);
  f.draw(c, Font::Body, kPlayOnSpeaker, kWaitSpeakerX + kWaitSpeakerW / 2, 20, kWaitSpeakerW - kWaitButtonPad,
         col::TXT, sp, Fonts::Align::Centre);
  const uint16_t cn = pressed_ == WaitCancel ? col::BTN_HI : col::BTN;
  c.fillRoundRect(kWaitCancelX, 3, kWaitCancelW, 34, 8, cn);
  f.draw(c, Font::Body, kWaitCancel, kWaitCancelX + kWaitCancelW / 2, 20, kWaitCancelW - kWaitButtonPad, col::TXT, cn,
         Fonts::Align::Centre);
  gfx::push(c, kColumnX, kWaitButtonsY, w, kAlbumH);
}

void NowPlayingPage::drawTransport() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  c.fillRect(0, 0, kW, kTransportH, col::BG);
  const int cy = kTransportH / 2;
  for (int z = 0; z < 5; ++z) {
    const int cx = z * kZoneW + kZoneW / 2;
    const bool down = pressed_ == static_cast<Zone>(Volume + z);
    if (down && z != 2) c.fillCircle(cx, cy, 22, col::BTN_HI);
    switch (z) {
      case 0: {  // the output and its volume: tap for the volume sheet
        const uint16_t off = s.btLost || s.btSession.failed() ? col::RED : col::AMBER;
        const uint16_t ic = s.onBluetooth ? (s.btConnected ? col::CYAN : off) : col::SOFT;
        icons::drawCentred(c, s.onBluetooth ? icons::kHeadphones : icons::kSpeaker, cx, cy - 8, ic);
        char v[6];
        snprintf(v, sizeof(v), "%u%%", static_cast<unsigned>(s.volume));
        f.draw(c, Font::Small, v, cx, cy + 13, kZoneW, col::DIM, down ? col::BTN_HI : col::BG, Fonts::Align::Centre);
        break;
      }
      case 1: icons::drawCentred(c, icons::kPrev, cx, cy, col::TXT); break;
      case 2: drawPlayButton(c, cx, cy, down); break;
      case 3: icons::drawCentred(c, icons::kNext, cx, cy, col::TXT); break;
      default: icons::drawCentred(c, icons::kMore, cx, cy, col::SOFT); break;
    }
  }
  gfx::push(c, 0, kTransportY, kW, kTransportH);
}

void NowPlayingPage::drawPlayButton(M5Canvas& c, int cx, int cy, bool down) {
  const PlayState play = ui_.state().play;
  const uint16_t disc = down ? col::SOFT : accent::NowPlaying;
  c.fillCircle(cx, cy, 23, disc);
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
  c.fillRect(0, 0, kZoneW, kTransportH, col::BG);
  drawPlayButton(c, kZoneW / 2, kTransportH / 2, pressed_ == PlayPause);
  gfx::push(c, 2 * kZoneW, kTransportY, kZoneW, kTransportH);
}

bool NowPlayingPage::emptyState(EmptyState& e) const {
  const AppState& s = ui_.state();
  if (!s.card && s.libraryTracks == 0) {
    noCardState(e, s.cardNotFat32);
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
    if (!drawn_.valid || !drawn_.empty || drawn_.noCard != noCard || drawn_.notFat32 != s.cardNotFat32) {
      EmptyState es;
      emptyState(es);
      drawEmptyState(es, kContentY, kH - kContentY, accent::NowPlaying, emptyPressed_);
      drawn_ = Drawn{};
      drawn_.valid = true;
      drawn_.empty = true;
      drawn_.noCard = noCard;
      drawn_.notFat32 = s.cardNotFat32;
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
  if (newTrack) drawTitle();
  if (newTrack || waiting != drawn_.waiting || wsig != drawn_.waitSig) {
    drawMiddle();
    if (scrub) {
      // (A repaint after a toast, the waiting panel's "try 2 of 3"): the
      // readout goes back over the album band in the same pass.
      drawn_.scrubUp = false;
      drawReadout();
    }
  }
  // A scrub ended (a lift, a cancel): the middle and the left column back
  // at once; the band below, in its rest look.
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
    const bool text = !drawn_.scrubUp || readoutSig() != drawn_.readout;
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
      output != drawn_.output) {
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
  if (e.y >= kTransportY) {
    if (e.atRightEdge()) return More;  // the "..." zone reaches the edge
    const int z = e.x / kZoneW;
    return static_cast<Zone>(Volume + (z < 0 ? 0 : z > 4 ? 4 : z));
  }
  // The seek bar: the full width, from 8 px above the band (the album's
  // last rows: the line is drawn at the band's top); while play waits,
  // from the band (the waiting buttons keep their whole area). Before the
  // waiting panel's branch, which has no bar.
  if (e.y >= kProgressY - (ui_.state().play == PlayState::Waiting ? 0 : kSeekReachPx)) return Bar;
  if (e.x < kColumnX && e.y >= kCoverY - 4 && e.y < kCoverY + kCoverPx + 4) return Cover;
  if (ui_.state().play == PlayState::Waiting && e.x >= kColumnX && e.y >= kArtistY) {
    // The waiting panel: its text is inert; Cancel reaches the edge.
    if (e.y < kWaitButtonsY || e.y >= kAlbumY + kAlbumH) return None;
    const bool cancel = e.atRightEdge() || e.x - kColumnX >= uitext::kWaitCancelX - 3;
    return cancel ? WaitCancel : WaitSpeaker;
  }
  if (e.x >= kColumnX && e.y >= kArtistY && e.y < kArtistY + kArtistH) return Artist;
  if (e.x >= kColumnX && e.y >= kAlbumY && e.y < kAlbumY + kAlbumH) return Album;
  return None;
}

void NowPlayingPage::goToLibrary(Go where) {
  const AppState& s = ui_.state();
  if (s.current < 0) {
    ui_.showTab(NavModel::Tab::Library);
    return;
  }
  const LibraryIndex* index = ui_.library().index();
  if (TrackCatalog::isBuiltin(s.trackId) || !index || !index->ready() || s.trackId >= index->trackCount()) {
    ui_.toast("A built-in track isn't in the Library", false);
    return;
  }
  if (ui_.browsingSynthetic()) {
    ui_.toast("Browsing a synthetic library (uil0: the card's)", false);
    return;
  }
  const LibraryIndex::Track& t = index->track(s.trackId);
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
      Serial.printf("[ui] now playing: show in folders (%d deep)\n", depth);
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
    if (z == Artist || z == Album) drawArtistAlbum();
    if (z == WaitSpeaker || z == WaitCancel) drawWaiting();
    if (z >= Volume) drawTransport();
    return;
  }
  if (e.type != T::Tap && e.type != T::DragStart && e.type != T::Release && e.type != T::Cancel) return;
  const Zone was = pressed_;
  pressed_ = None;
  if (was == Artist || was == Album) drawArtistAlbum();
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
    case Cover:
    case Album: goToLibrary(Go::Album); break;
    case Artist: goToLibrary(Go::Artist); break;
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
    case More: {
      // The sleep timer first (ENERGY.md section 3), its state on the right.
      static const char* const kRows[4] = {uitext::kSleepRow, "Go to artist", "Go to album", "Show in folders"};
      const AppState& s = ui_.state();
      const TrackCatalog& cat = ui_.player().catalog();
      const bool lib = s.current >= 0 && !TrackCatalog::isBuiltin(s.trackId);
      char* folder = moreFolder_;
      folder[0] = 0;
      const LibraryIndex* index = ui_.library().index();
      if (lib && index && index->ready() && s.trackId < index->trackCount()) {
        char path[256];
        if (index->folderPath(index->track(s.trackId).folder, path, sizeof(path))) {
          snprintf(folder, sizeof(moreFolder_), "%s", path);
        }
      }
      const char* details[4] = {s.sleepRow, lib ? cat.artist(s.trackId) : "", lib ? cat.album(s.trackId) : "",
                                folder};
      char title[128] = "Nothing playing";
      if (s.current >= 0) cat.title(s.trackId, title, sizeof(title));
      ui_.openSheet(this, title, kRows, 4, details);
      ui_.sheetFollowsSleep(0);  // its detail follows the timer while it is up
      break;
    }
    default: break;
  }
}

void NowPlayingPage::onSheet(int choice) {
  switch (choice) {
    case 0: ui_.openSleepSheet(); break;
    case 1: goToLibrary(Go::Artist); break;
    case 2: goToLibrary(Go::Album); break;
    case 3: goToLibrary(Go::Folders); break;
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
  const size_t room = size - static_cast<size_t>(n);
  switch (barLook()) {
    case BarLook::Inert: snprintf(rest, room, "; the bar: inert"); break;
    case BarLook::Rest:
      snprintf(rest, room, "; the bar: rest, the knob at x %d", SeekBar::kLineX + SeekBar::xOf(s.positionMs, s.durationMs));
      break;
    case BarLook::Pressed: snprintf(rest, room, "; the bar: pressed"); break;
    case BarLook::Off: snprintf(rest, room, "; the bar: off"); break;
    case BarLook::Scrubbing: {
      if (bar_.staying()) {
        snprintf(rest, room, "; the bar: staying");
        break;
      }
      char to[16], live[16];
      mmss(bar_.targetMs(), to, sizeof(to));
      mmss(bar_.liveMs(), live, sizeof(live));
      snprintf(rest, room, "; the bar: scrubbing to %s (%s; readout %s; %s plays)", to,
               bar_.knobGrab() ? "drag from the knob" : "drag", bar_.readoutLeft() ? "left" : "right", live);
      break;
    }
  }
}

}  // namespace ui
