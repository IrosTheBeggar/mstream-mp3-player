// The Library (a first page for the framework; the spec's §6.2 is the full
// screen): the artists A-Z (with the rail), an artist (its own Play / Play
// next / + Queue bar, "All tracks", its albums), an album's or an artist's
// tracks, where a tap opens the track's inline Play / Play next / + Queue
// bar. Play on a track plays its album (or the artist's tracks) from it,
// so the rest follows; Play next and + Queue take the track alone. Every
// action shows a toast with Undo (and View after an add: the Queue at the
// added tracks). A long press on an artist or an album offers the same
// three actions in a sheet without opening it. Two levels down (an album,
// an artist's tracks) the header has an "Artists" pill, the root crumb: the
// root without a chain of Backs (the review's graft). Opened from Now
// Playing, an album or an artist opens at the playing track (or its album).
#include <cstdio>
#include <cstring>

#include "TextFold.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

namespace {

const uint16_t kDiscColours[] = {0xFB49, 0x3EBE, 0xFE07, 0x4ECF, 0xA45F, 0xF396};
const char* const kActions[3] = {"Play", "Play next", "+ Queue"};

uint16_t discColour(const char* name) {
  uint32_t h = 2166136261u;
  while (*name) h = (h ^ static_cast<unsigned char>(*name++)) * 16777619u;
  return kDiscColours[h % 6];
}

NavModel::PageRef page(PageKind kind, uint32_t id) {
  NavModel::PageRef p;
  p.kind = static_cast<uint8_t>(kind);
  p.id = id;
  return p;
}

}  // namespace

const LibraryIndex* LibraryPage::index() const {
  const LibraryIndex* i = const_cast<Ui&>(ui_).library().index();
  if (!i || !i->ready()) return nullptr;
  // An id from before a rebuild is gone (the Ui resets the stacks, but be safe).
  switch (kind_) {
    case PageKind::Artist:
    case PageKind::ArtistTracks: return id_ < i->artistCount() ? i : nullptr;
    case PageKind::Album: return id_ < i->albumCount() ? i : nullptr;
    default: return i;
  }
}

LibraryIndex::Span LibraryPage::tracksOf(PageKind kind, uint32_t id) const {
  const LibraryIndex* i = index();
  if (!i) return {};
  switch (kind) {
    case PageKind::Artist:
    case PageKind::ArtistTracks: return id < i->artistCount() ? i->tracksOfArtist(id) : LibraryIndex::Span{};
    case PageKind::Album: return id < i->albumCount() ? i->tracksOfAlbum(id) : LibraryIndex::Span{};
    default: return {};
  }
}

// ---- the page ----

void LibraryPage::enter(NavModel::PageRef& ref) {
  kind_ = static_cast<PageKind>(ref.kind);
  id_ = ref.id;
  ref_ = &ref;
  touchInList_ = false;
  headerPressed_ = 0;
  drawnTrack_ = ui_.state().trackId;
  ui_.drawHeader(header());
  const bool showPlaying = ref.scrollPx == kShowPlaying;
  if (showPlaying) ref.scrollPx = -1;
  ListView& list = ui_.list();
  list.attach(this, &ref, 0);
  // From Now Playing: the playing track's row (or its album's) on screen,
  // with a row of context above it when the list has to move.
  int32_t row = -1;
  if (showPlaying) row = playingRow();
  if (row >= 0) {
    const uint32_t item = list.layout().itemOfRow(static_cast<uint32_t>(row));
    if (list.layout().reveal(item, 0, ListView::kHeight) != 0) {
      list.scrollTo((static_cast<int32_t>(item) - 1) * ListLayout::kPitch);
    }
  }
}

// The row that holds what plays now: the track (an album, an artist's
// tracks) or its album (an artist); -1 if none here.
int32_t LibraryPage::playingRow() const {
  const LibraryIndex* i = index();
  const uint32_t t = ui_.state().trackId;
  if (!i || ui_.state().current < 0 || t >= i->trackCount()) return -1;
  if (kind_ == PageKind::Album || kind_ == PageKind::ArtistTracks) {
    const LibraryIndex::Span tracks = pageTracks();
    for (uint32_t r = 0; r < tracks.count; ++r) {
      if (tracks[r] == t) return static_cast<int32_t>(r);
    }
  } else if (kind_ == PageKind::Artist) {
    const LibraryIndex::Span albums = i->albumsOf(id_);
    for (uint32_t r = 0; r < albums.count; ++r) {
      if (albums[r] == i->track(t).album) return static_cast<int32_t>(r + 1);  // after "All tracks"
    }
  }
  return -1;
}

void LibraryPage::leave() { ui_.list().detach(); }

void LibraryPage::repaint() {
  ui_.drawHeader(header());
  ui_.list().invalidate();
}

void LibraryPage::repaintHeader() { ui_.drawHeader(header()); }

// Two levels down (Artists > an artist > an album), the root crumb.
bool LibraryPage::crumb() const { return ui_.nav().depth() > 2; }

Header LibraryPage::header() const {
  static char sub[64];
  Header h;
  h.sub = sub;
  sub[0] = 0;
  const LibraryIndex* i = index();
  switch (kind_) {
    case PageKind::Artists:
      h.title = "Artists";
      if (i) snprintf(sub, sizeof(sub), "%lu", static_cast<unsigned long>(i->artistCount()));
      break;
    case PageKind::Artist:
      h.back = true;
      h.title = i ? i->artistName(id_) : "Artist";
      if (i) {
        const LibraryIndex::Artist& a = i->artist(id_);
        snprintf(sub, sizeof(sub), "%lu album%s, %lu tracks", static_cast<unsigned long>(a.albumCount),
                 a.albumCount == 1 ? "" : "s", static_cast<unsigned long>(a.trackCount));
      }
      break;
    case PageKind::Album:
      h.back = true;
      h.title = i ? i->albumName(id_) : "Album";
      if (i) {
        const LibraryIndex::Album& a = i->album(id_);
        snprintf(sub, sizeof(sub), "%s, %lu tracks", i->artistName(a.artist), static_cast<unsigned long>(a.trackCount));
      }
      break;
    case PageKind::ArtistTracks:
      h.back = true;
      h.title = "All tracks";
      if (i) snprintf(sub, sizeof(sub), "%s", i->artistName(id_));
      break;
    default:
      break;
  }
  if (!h.title[0]) h.title = kind_ == PageKind::Album ? "(loose tracks)" : "(no artist folder)";
  if (crumb()) {
    h.right = "Artists";
    h.pressed = headerPressed_ == 2;
  }
  return h;
}

bool LibraryPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  // The playing track is marked in the track lists.
  const uint32_t t = ui_.state().trackId;
  if (t != drawnTrack_) {
    drawnTrack_ = t;
    if (kind_ == PageKind::Album || kind_ == PageKind::ArtistTracks) ui_.list().refreshAll();
  }
  return ui_.list().update(nowMs, frameDue, wholeRows);
}

bool LibraryPage::animating() const { return ui_.list().animating(); }

void LibraryPage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    touchInList_ = e.y >= ListView::kTop;
    if (!touchInList_) {
      headerPressed_ = header().hit(e);
      if (headerPressed_ == 2) repaintHeader();
    }
  }
  if (touchInList_) {
    ui_.list().onEvent(e);
    return;
  }
  if (e.type == T::Tap) {
    const int hit = headerPressed_;
    headerPressed_ = 0;
    if (hit == 1 && kind_ != PageKind::Artists) {
      ui_.tick();
      ui_.back();
    } else if (hit == 2 && crumb()) {
      ui_.tick();
      ui_.toRoot();
    } else if (hit == 2) {
      repaintHeader();
    }
  } else if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
    if (headerPressed_ == 2) {
      headerPressed_ = 0;
      repaintHeader();
    }
    headerPressed_ = 0;
  }
}

void LibraryPage::describe(char* buf, size_t size) const {
  snprintf(buf, size, "Library: %s %lu, %lu rows", pageKindName(static_cast<uint8_t>(kind_)),
           static_cast<unsigned long>(id_), static_cast<unsigned long>(const_cast<LibraryPage*>(this)->rows()));
}

// ---- the list ----

uint32_t LibraryPage::rows() {
  const LibraryIndex* i = index();
  if (!i) return 0;
  switch (kind_) {
    case PageKind::Artists: return i->artistCount();
    case PageKind::Artist: return i->albumsOf(id_).count + 1;
    case PageKind::Album: return i->tracksOfAlbum(id_).count;
    case PageKind::ArtistTracks: return i->tracksOfArtist(id_).count;
    default: return 0;
  }
}

uint16_t LibraryPage::accent() { return accent::Library; }

bool LibraryPage::topBar() { return kind_ != PageKind::Artists && index(); }

bool LibraryPage::alphabetical() { return kind_ == PageKind::Artists; }

char LibraryPage::railKey(uint32_t row) {
  const LibraryIndex* i = index();
  if (!i || kind_ != PageKind::Artists || row >= i->artistCount()) return '#';
  return textfold::railKey(i->artistName(i->artistsAZ()[row]));
}

const char* LibraryPage::emptyText() {
  return ui_.library().index() && ui_.library().index()->ready() ? "Nothing here"
                                                                  : "No music found: put folders in /music";
}

bool LibraryPage::rowContainer(uint32_t row, PageKind* kind, uint32_t* id) const {
  const LibraryIndex* i = index();
  if (!i) return false;
  if (kind_ == PageKind::Artists && row < i->artistCount()) {
    *kind = PageKind::Artist;
    *id = i->artistsAZ()[row];
    return true;
  }
  if (kind_ == PageKind::Artist) {
    if (row == 0) {
      *kind = PageKind::ArtistTracks;
      *id = id_;
      return true;
    }
    const LibraryIndex::Span albums = i->albumsOf(id_);
    if (row - 1 < albums.count) {
      *kind = PageKind::Album;
      *id = albums[row - 1];
      return true;
    }
  }
  return false;
}

void LibraryPage::drawRow(ListView::Row& r) {
  const LibraryIndex* i = index();
  if (!i) return;
  int x = r.x;
  switch (kind_) {
    case PageKind::Artists: {
      const uint32_t id = i->artistsAZ()[r.row];
      const char* name = i->artistName(id);
      const LibraryIndex::Artist& a = i->artist(id);
      x = ListView::disc(r, textfold::railKey(name), discColour(name));
      const int right = ListView::chevron(r);
      char sub[48];
      snprintf(sub, sizeof(sub), "%lu album%s, %lu tracks", static_cast<unsigned long>(a.albumCount),
               a.albumCount == 1 ? "" : "s", static_cast<unsigned long>(a.trackCount));
      const char* shown = name[0] ? name : "(no artist folder)";
      ListView::lines(r, x, right, shown, strlen(shown), sub, strlen(sub), col::TXT);
      break;
    }
    case PageKind::Artist: {
      const int right = ListView::chevron(r);
      if (r.row == 0) {
        r.c.fillRoundRect(r.x + 6, 1, 40, 40, 4, col::BTN);
        icons::drawCentred(r.c, icons::kQueue, r.x + 26, 21, accent::Library);
        char sub[24];
        snprintf(sub, sizeof(sub), "%lu tracks", static_cast<unsigned long>(i->artist(id_).trackCount));
        ListView::lines(r, r.x + 54, right, "All tracks", 10, sub, strlen(sub), col::TXT);
        break;
      }
      const uint32_t album = i->albumsOf(id_)[r.row - 1];
      x = ListView::thumb(r);
      const char* name = i->albumName(album);
      const char* shown = name[0] ? name : "(loose tracks)";
      char sub[24];
      snprintf(sub, sizeof(sub), "%lu tracks", static_cast<unsigned long>(i->album(album).trackCount));
      ListView::lines(r, x, right, shown, strlen(shown), sub, strlen(sub), col::TXT);
      break;
    }
    case PageKind::Album:
    case PageKind::ArtistTracks: {
      const LibraryIndex::Span tracks = pageTracks();
      const uint32_t t = tracks[r.row];
      const bool playing = t == ui_.state().trackId;
      const uint8_t number = i->track(t).number;
      x = playing ? ListView::playing(r, accent::Library)
                  : ListView::number(r, number ? number : r.row + 1, col::DIM);
      uint8_t len = 0;
      const char* title = i->trackTitle(t, &len);
      const uint16_t colour = playing ? accent::Library : col::TXT;
      const Font font = playing ? Font::Bold : Font::Body;
      if (kind_ == PageKind::ArtistTracks) {
        const char* album = i->albumName(i->track(t).album);
        ListView::lines(r, x, r.right - 8, title, len, album, strlen(album), colour, font);
      } else {
        ListView::lines(r, x, r.right - 8, title, len, nullptr, 0, colour, font);
      }
      break;
    }
    default:
      break;
  }
}

int LibraryPage::actions(int32_t row, const char* labels[3]) {
  (void)row;
  for (int k = 0; k < 3; ++k) labels[k] = kActions[k];
  return 3;
}

void LibraryPage::act(PageKind kind, uint32_t id, int action, int32_t startTrack) {
  const LibraryIndex* i = index();
  if (!i) return;
  PlaybackController& p = ui_.player();
  char what[80];
  LibraryIndex::Span span = tracksOf(kind, id);
  uint32_t one = 0;
  if (startTrack >= 0 && action != 0) {
    // Play next / + Queue on a track: that track alone.
    one = span[static_cast<uint32_t>(startTrack)];
    span.ids = &one;
    span.count = 1;
  }
  if (span.count == 0) return;
  // What the toast calls it.
  if (span.count == 1) {
    uint8_t len = 0;
    const char* t = i->trackTitle(span[0], &len);
    snprintf(what, sizeof(what), "%.*s", static_cast<int>(len), t);
  } else if (kind == PageKind::Album) {
    snprintf(what, sizeof(what), "%s", i->albumName(id));
  } else {
    snprintf(what, sizeof(what), "%s", i->artistName(id));
  }
  bool ok = false;
  char text[112];
  // Where an add puts its first track (QueueModel: Play next right after
  // the current entry, + Queue at the end): the toast's View goes there.
  QueueModel& q = ui_.queue();
  const uint32_t addedAt = action == 1 && q.current() >= 0 ? static_cast<uint32_t>(q.current()) + 1 : q.size();
  switch (action) {
    case 0:
      ok = p.playNow(span.ids, span.count, startTrack >= 0 ? static_cast<uint32_t>(startTrack) : 0);
      if (startTrack >= 0) {
        uint8_t len = 0;
        const char* t = i->trackTitle(span[static_cast<uint32_t>(startTrack)], &len);
        snprintf(text, sizeof(text), "Playing %.*s", static_cast<int>(len), t);
      } else {
        snprintf(text, sizeof(text), "Playing %s", what);
      }
      break;
    case 1:
      ok = p.playNext(span.ids, span.count);
      if (span.count == 1) {
        snprintf(text, sizeof(text), "Plays next: %s", what);
      } else {
        snprintf(text, sizeof(text), "Plays next: %lu tracks", static_cast<unsigned long>(span.count));
      }
      break;
    default:
      ok = p.addToQueue(span.ids, span.count);
      if (span.count == 1) {
        snprintf(text, sizeof(text), "Added %s", what);
      } else {
        snprintf(text, sizeof(text), "Added %lu tracks", static_cast<unsigned long>(span.count));
      }
      break;
  }
  const uint32_t viewKey = ok && action != 0 ? q.keyAt(addedAt) : QueueModel::kNone;
  ui_.toast(ok ? text : "Not enough memory for that", ok, viewKey);
}

void LibraryPage::onAction(int32_t row, int action) {
  // The top bar acts on the page's container; a track's bar on the track
  // (Play: the container from that track).
  act(kind_, id_, action, row);
}

ListView::Tap LibraryPage::onTap(uint32_t row) {
  PageKind kind;
  uint32_t id;
  if (rowContainer(row, &kind, &id)) {
    ui_.push(page(kind, id));
    return ListView::Tap::Handled;
  }
  return ListView::Tap::Expand;  // a track: its inline bar
}

// Artists and albums hold (their sheet); tracks don't (their inline bar is a tap away).
bool LibraryPage::holds(uint32_t row) {
  PageKind kind;
  uint32_t id;
  return rowContainer(row, &kind, &id);
}

void LibraryPage::onHold(uint32_t row) {
  PageKind kind;
  uint32_t id;
  if (!rowContainer(row, &kind, &id)) return;
  heldKind_ = kind;
  heldId_ = id;
  const LibraryIndex* i = index();
  char title[64];
  snprintf(title, sizeof(title), "%s", kind == PageKind::Album ? i->albumName(id) : i->artistName(id));
  ui_.openSheet(this, title, kActions, 3);
}

void LibraryPage::onSheet(int choice) {
  if (choice < 0 || heldKind_ == PageKind::None) return;
  act(heldKind_, heldId_, choice, -1);
  heldKind_ = PageKind::None;
}

}  // namespace ui
