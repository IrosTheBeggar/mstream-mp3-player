// Now Playing (a first page for the framework; the spec's §6.1 is the full
// screen): the title (2 lines), the artist and the album (each a 40 px band
// that opens it in the Library, at the playing track: the review's grafts,
// 40 px or more so a tap meant for one doesn't open the other), the
// progress and times, and the transport. Content y 36-239:
//
//   38-89    title, DejaVu Bold 22, up to 2 lines
//   90-129   artist  ›          (tap: Library > artist)
//   130-169  album   ›          (tap: Library > artist > album)
//   170-191  progress bar, elapsed / "4 of 16" / length
//   192-239  [output + volume] [prev] [play/pause] [next] [...]   64 px zones
//
// The "..." zone reaches the screen's edge.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "TrackCatalog.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

namespace {

constexpr int kTitleY = 38, kTitleH = 52;
constexpr int kArtistY = 90, kArtistH = 40;
constexpr int kAlbumY = 130, kAlbumH = 40;
constexpr int kProgressY = 170, kProgressH = 22;
constexpr int kTransportY = 192, kTransportH = 48;
constexpr int kZoneW = 64;

void mmss(uint32_t ms, char* buf, size_t size) {
  snprintf(buf, size, "%lu:%02lu", static_cast<unsigned long>(ms / 60000), static_cast<unsigned long>(ms / 1000 % 60));
}

// The artist and album ids of a library track (kNone for a built-in).
void containers(Library& lib, uint32_t track, uint32_t* artist, uint32_t* album) {
  *artist = *album = NavModel::kNone;
  const LibraryIndex* index = lib.index();
  if (!index || !index->ready() || track >= index->trackCount()) return;
  *artist = index->track(track).artist;
  *album = index->track(track).album;
}

}  // namespace

void NowPlayingPage::enter(NavModel::PageRef& ref) {
  (void)ref;
  pressed_ = None;
  repaint();
}

void NowPlayingPage::repaint() {
  gfx::fill(0, kContentY, kW, kTitleY - kContentY, col::BG);  // the 2 rows above the title
  drawn_ = Drawn{};
  update(0, false, false);
}

void NowPlayingPage::drawTitle() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  c.fillRect(0, 0, kW, kTitleH, col::BG);
  char title[128] = "Nothing playing";
  if (s.current >= 0) ui_.player().catalog().title(s.trackId, title, sizeof(title));
  char lines[2][96];
  const int n = textfit::wrap(f.fit(Font::Title), title, strlen(title), kW - 24, 2, &lines[0][0], sizeof(lines[0]));
  for (int i = 0; i < n; ++i) {
    f.draw(c, Font::Title, lines[i], 12, n == 1 ? 26 : 13 + i * 26, kW - 24, s.current >= 0 ? col::TXT : col::DIM,
           col::BG);
  }
  gfx::push(c, 0, kTitleY, kW, kTitleH);
}

void NowPlayingPage::drawArtistAlbum() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  const TrackCatalog& cat = ui_.player().catalog();
  const bool lib = s.current >= 0 && !TrackCatalog::isBuiltin(s.trackId);
  // The artist band.
  uint16_t bg = pressed_ == Artist ? col::ROW_SEL : col::BG;
  c.fillRect(0, 0, kW, kArtistH, bg);
  const char* artist = s.current < 0 ? "Open the Library" : lib ? cat.artist(s.trackId) : "Built-in test track";
  if (lib && !artist[0]) artist = "(no artist folder)";
  f.draw(c, Font::Body, artist, 12, kArtistH / 2, kW - 44, s.current < 0 ? accent::Library : col::SOFT, bg);
  if (lib || s.current < 0) icons::drawCentred(c, icons::kChevronRight, kW - 16, kArtistH / 2, col::FAINT);
  gfx::push(c, 0, kArtistY, kW, kArtistH);
  // The album band.
  bg = pressed_ == Album ? col::ROW_SEL : col::BG;
  c.fillRect(0, 0, kW, kAlbumH, bg);
  if (lib) {
    const char* album = cat.album(s.trackId);
    f.draw(c, Font::Small, album[0] ? album : "(loose tracks)", 12, kAlbumH / 2, kW - 44, col::DIM, bg);
    icons::drawCentred(c, icons::kChevronRight, kW - 16, kAlbumH / 2, col::FAINT);
  }
  gfx::push(c, 0, kAlbumY, kW, kAlbumH);
}

void NowPlayingPage::drawProgress() {
  const AppState& s = ui_.state();
  M5Canvas& c = gfx::strip();
  Fonts& f = Fonts::instance();
  c.fillRect(0, 0, kW, kProgressH, col::BG);
  const bool paused = s.play != PlayState::Playing;
  const int x0 = 12, w = kW - 24;
  if (s.durationMs > 0) {
    c.fillRoundRect(x0, 2, w, 4, 2, col::DIV);
    const int fill = static_cast<int>(static_cast<uint64_t>(std::min(s.positionMs, s.durationMs)) * w / s.durationMs);
    if (fill > 0) c.fillRoundRect(x0, 2, fill, 4, 2, paused ? col::DIM : accent::NowPlaying);
  } else {
    for (int x = x0; x < x0 + w; x += 6) c.fillRect(x, 3, 3, 2, col::DIV);  // unknown length: dotted
  }
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
  char mid[40];
  uint16_t mc = col::DIM;
  if (s.failed) {
    snprintf(mid, sizeof(mid), "Can't play this track");
    mc = col::RED;
  } else if (s.current < 0) {
    snprintf(mid, sizeof(mid), "The queue is empty");
  } else {
    snprintf(mid, sizeof(mid), "%s%lu of %lu", s.play == PlayState::Paused ? "Paused, " : s.play == PlayState::Stopped ? "Stopped, " : "",
             static_cast<unsigned long>(s.current + 1), static_cast<unsigned long>(s.queueSize));
    if (s.play != PlayState::Playing) mc = col::AMBER;
  }
  f.draw(c, Font::Small, mid, kW / 2, kTextY, 170, mc, col::BG, Fonts::Align::Centre);
  gfx::push(c, 0, kProgressY, kW, kProgressH);
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
      case 0: {  // the output and its volume: tap for the Output tab
        const uint16_t ic = s.onBluetooth ? (s.btConnected ? col::CYAN : col::AMBER) : col::SOFT;
        icons::drawCentred(c, s.onBluetooth ? icons::kHeadphones : icons::kSpeaker, cx, cy - 8, ic);
        char v[6];
        snprintf(v, sizeof(v), "%u%%", static_cast<unsigned>(s.volume));
        f.draw(c, Font::Small, v, cx, cy + 13, kZoneW, col::DIM, down ? col::BTN_HI : col::BG, Fonts::Align::Centre);
        break;
      }
      case 1: icons::drawCentred(c, icons::kPrev, cx, cy, col::TXT); break;
      case 2: {
        const uint16_t disc = down ? col::SOFT : accent::NowPlaying;
        c.fillCircle(cx, cy, 23, disc);
        icons::drawCentred(c, s.play == PlayState::Playing ? icons::kPause : icons::kPlay,
                           cx + (s.play == PlayState::Playing ? 0 : 2), cy, col::DARK);
        break;
      }
      case 3: icons::drawCentred(c, icons::kNext, cx, cy, col::TXT); break;
      default: icons::drawCentred(c, icons::kMore, cx, cy, col::SOFT); break;
    }
  }
  gfx::push(c, 0, kTransportY, kW, kTransportH);
}

bool NowPlayingPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  (void)nowMs;
  (void)frameDue;
  (void)wholeRows;
  const AppState& s = ui_.state();
  const bool all = !drawn_.valid;
  if (all || s.trackId != drawn_.track || (s.current < 0) != (drawn_.current < 0)) {
    drawTitle();
    drawArtistAlbum();
  }
  const uint32_t second = s.positionMs / 1000;
  const uint32_t durS = s.durationMs / 1000;
  if (all || second != drawn_.second || durS != drawn_.durationS || s.play != drawn_.play ||
      s.current != drawn_.current || s.queueSize != drawn_.size) {
    drawProgress();
  }
  if (all || s.play != drawn_.play || s.volume != drawn_.volume || s.onBluetooth != drawn_.bluetooth) drawTransport();
  drawn_.valid = true;
  drawn_.track = s.trackId;
  drawn_.play = s.play;
  drawn_.second = second;
  drawn_.durationS = durS;
  drawn_.current = s.current;
  drawn_.size = s.queueSize;
  drawn_.volume = s.volume;
  drawn_.bluetooth = s.onBluetooth;
  return false;
}

NowPlayingPage::Zone NowPlayingPage::zoneAt(const InputEvent& e) const {
  if (e.y >= kArtistY && e.y < kArtistY + kArtistH) return Artist;
  if (e.y >= kAlbumY && e.y < kAlbumY + kAlbumH) return Album;
  if (e.y >= kTransportY) {
    if (e.atRightEdge()) return More;  // the "..." zone reaches the edge
    const int z = e.x / kZoneW;
    return static_cast<Zone>(Volume + (z < 0 ? 0 : z > 4 ? 4 : z));
  }
  return None;
}

void NowPlayingPage::goToLibrary(bool album) {
  const AppState& s = ui_.state();
  if (s.current < 0) {
    ui_.showTab(NavModel::Tab::Library);
    return;
  }
  uint32_t artist, alb;
  containers(ui_.library(), s.trackId, &artist, &alb);
  if (artist == NavModel::kNone) return;  // a built-in track
  ui_.showInLibrary(artist, album ? alb : NavModel::kNone);
}

void NowPlayingPage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  const Zone z = zoneAt(e);
  if (e.type == T::Down) {
    pressed_ = z;
    if (z == Artist || z == Album) drawArtistAlbum();
    if (z >= Volume) drawTransport();
    return;
  }
  if (e.type != T::Tap && e.type != T::DragStart && e.type != T::Release && e.type != T::Cancel) return;
  const Zone was = pressed_;
  pressed_ = None;
  if (was == Artist || was == Album) drawArtistAlbum();
  if (was >= Volume) drawTransport();
  if (e.type != T::Tap || was == None) return;
  ui_.tick();
  switch (was) {
    case Artist: goToLibrary(false); break;
    case Album: goToLibrary(true); break;
    case Volume: ui_.showTab(NavModel::Tab::Output); break;
    case Prev: ui_.host().prev(); break;
    case PlayPause: ui_.host().playPause(); break;
    case Next: ui_.host().next(); break;
    case More: {
      static const char* const kRows[3] = {"Go to artist", "Go to album", "Show in the queue"};
      ui_.openSheet(this, "This track", kRows, 3);
      break;
    }
    default: break;
  }
}

void NowPlayingPage::onSheet(int choice) {
  switch (choice) {
    case 0: goToLibrary(false); break;
    case 1: goToLibrary(true); break;
    case 2:
      ui_.nav().top(NavModel::Tab::Queue).scrollPx = -1;  // opens on the playing track
      ui_.showTab(NavModel::Tab::Queue);
      break;
    default: break;
  }
}

void NowPlayingPage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  snprintf(buf, size, "Now Playing: track id %lu, entry %ld of %lu, %lu / %lu ms", static_cast<unsigned long>(s.trackId),
           static_cast<long>(s.current), static_cast<unsigned long>(s.queueSize),
           static_cast<unsigned long>(s.positionMs), static_cast<unsigned long>(s.durationMs));
}

}  // namespace ui
