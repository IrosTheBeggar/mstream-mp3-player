// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Library (the tab bar spec §6.2, mockups 07-15, with the review's
// grafts; the page's shape is in Pages.h). Every action shows a toast with
// Undo (and View after an add: the Queue at the added tracks). Browsing a
// synthetic library ('uil<n>' on the console, to see the lists at the
// scale of thousands) is look-only: its ids aren't the player's.
#include <cstdio>
#include <cstring>

#include "TextFold.h"
#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Thumbs.h"
#include "ui/Ui.h"

namespace ui {

namespace {

const uint16_t kDiscColours[] = {0xFB49, 0x3EBE, 0xFE07, 0x4ECF, 0xA45F, 0xF396};
const char* const kActions[3] = {"Play", "Play next", "+ Queue"};
const char* const kSegments[3] = {"Artists", "Albums", "Folders"};

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

const char* orNone(const char* name, const char* none) { return name && name[0] ? name : none; }

// "N folders, M audio files, K other" (the parts that aren't 0).
void folderCounts(const LibraryIndex& i, uint32_t f, bool audioWord, char* buf, size_t size) {
  const LibraryIndex::Folder& d = i.folder(f);
  int n = 0;
  buf[0] = 0;
  auto part = [&](uint32_t count, const char* one, const char* many) {
    if (!count || n >= static_cast<int>(size) - 1) return;
    n += snprintf(buf + n, size - n, "%s%lu %s", n ? ", " : "", static_cast<unsigned long>(count), count == 1 ? one : many);
  };
  part(d.folderCount, "folder", "folders");
  part(d.fileCount, audioWord ? "audio file" : "file", audioWord ? "audio files" : "files");
  part(d.otherCount, "other", "other");
  if (!n) snprintf(buf, size, "empty");
}

}  // namespace

// ---- where things are ----

const LibraryIndex* LibraryPage::index() const {
  const LibraryIndex* i = const_cast<Ui&>(ui_).browseIndex();
  if (!i || !i->ready()) return nullptr;
  // An id from before a rebuild is gone (the Ui resets the stacks, but be safe).
  switch (kind_) {
    case PageKind::Artist:
    case PageKind::ArtistTracks: return id_ < i->artistCount() ? i : nullptr;
    case PageKind::Album: return id_ < i->albumCount() ? i : nullptr;
    case PageKind::Folder: return id_ < i->folderCount() ? i : nullptr;
    default: return i;
  }
}

bool LibraryPage::real() const { return !ui_.browsingSynthetic(); }

LibraryPage::RowRef LibraryPage::rowAt(uint32_t row) const {
  RowRef r;
  const LibraryIndex* i = index();
  if (!i) return r;
  auto folderRow = [&](uint32_t f) {
    const LibraryIndex::Span subs = i->subfolders(f);
    if (row < subs.count) {
      r.kind = RowKind::Folder;
      r.id = subs[row];
      return;
    }
    const LibraryIndex::Span files = i->filesIn(f);
    const uint32_t k = row - subs.count;
    if (k < files.count) {
      r.kind = RowKind::File;
      r.id = files[k];
      r.index = k;
    }
  };
  switch (kind_) {
    case PageKind::Library:
      if (segment() == LibrarySegment::Artists && row < i->artistCount()) {
        r.kind = RowKind::Artist;
        r.id = i->artistsAZ()[row];
      } else if (segment() == LibrarySegment::Albums && row < i->albumCount()) {
        r.kind = RowKind::Album;
        r.id = i->albumsAZ()[row];
      } else if (segment() == LibrarySegment::Folders) {
        folderRow(LibraryIndex::rootFolder());
      }
      break;
    case PageKind::Artist: {
      if (row == 0) {
        r.kind = RowKind::AllTracks;
        r.id = id_;
        break;
      }
      const LibraryIndex::Span albums = i->albumsOf(id_);
      if (row - 1 < albums.count) {
        r.kind = RowKind::Album;
        r.id = albums[row - 1];
      }
      break;
    }
    case PageKind::Album:
    case PageKind::ArtistTracks: {
      const LibraryIndex::Span t = pageTracks();
      if (row < t.count) {
        r.kind = RowKind::Track;
        r.id = t[row];
        r.index = row;
      }
      break;
    }
    case PageKind::Folder:
      folderRow(id_);
      break;
    default:
      break;
  }
  return r;
}

LibraryIndex::Span LibraryPage::pageTracks() const {
  const LibraryIndex* i = index();
  if (!i) return {};
  switch (kind_) {
    case PageKind::Album: return i->tracksOfAlbum(id_);
    case PageKind::Artist:
    case PageKind::ArtistTracks: return i->tracksOfArtist(id_);
    case PageKind::Folder: return i->filesIn(id_);
    case PageKind::Library:
      return segment() == LibrarySegment::Folders ? i->filesIn(LibraryIndex::rootFolder()) : LibraryIndex::Span{};
    default: return {};
  }
}

bool LibraryPage::container(const RowRef& r, LibraryIndex::Span* span, const char** name) const {
  const LibraryIndex* i = index();
  if (!i) return false;
  switch (r.kind) {
    case RowKind::None:  // the page's own
      switch (kind_) {
        case PageKind::Artist:
        case PageKind::ArtistTracks:
          *span = i->tracksOfArtist(id_);
          *name = orNone(i->artistName(id_), "(no artist folder)");
          return true;
        case PageKind::Album:
          *span = i->tracksOfAlbum(id_);
          *name = orNone(i->albumName(id_), "(loose tracks)");
          return true;
        case PageKind::Folder:
          *span = i->treeTracks(id_);
          *name = i->folderName(id_);
          return true;
        case PageKind::Library:
          if (segment() != LibrarySegment::Folders) return false;
          *span = i->treeTracks(LibraryIndex::rootFolder());
          *name = "everything";
          return true;
        default:
          return false;
      }
    case RowKind::Artist:
    case RowKind::AllTracks:
      *span = i->tracksOfArtist(r.id);
      *name = orNone(i->artistName(r.id), "(no artist folder)");
      return true;
    case RowKind::Album:
      *span = i->tracksOfAlbum(r.id);
      *name = orNone(i->albumName(r.id), "(loose tracks)");
      return true;
    case RowKind::Folder:
      *span = i->treeTracks(r.id);
      *name = i->folderName(r.id);
      return true;
    default:
      return false;
  }
}

// ---- what plays now ----

void LibraryPage::notePlaying() {
  playTrack_ = playAlbum_ = playArtist_ = LibraryIndex::kNone;
  playDepth_ = 0;
  const LibraryIndex* i = index();
  const AppState& s = ui_.state();
  if (!i || !real() || s.current < 0 || s.trackId >= i->trackCount()) return;
  playTrack_ = s.trackId;
  const LibraryIndex::Track& t = i->track(s.trackId);
  playAlbum_ = t.album;
  playArtist_ = t.artist;
  for (uint32_t f = t.folder; f != LibraryIndex::kNone && playDepth_ < static_cast<int>(sizeof(playFolders_) / 4);
       f = i->folder(f).parent) {
    playFolders_[playDepth_++] = f;
  }
}

bool LibraryPage::playing(const RowRef& r) const {
  if (playTrack_ == LibraryIndex::kNone) return false;
  switch (r.kind) {
    case RowKind::Artist: return r.id == playArtist_;
    case RowKind::Album: return r.id == playAlbum_;
    case RowKind::Track:
    case RowKind::File: return r.id == playTrack_;
    case RowKind::Folder:
      for (int k = 0; k < playDepth_; ++k) {
        if (playFolders_[k] == r.id) return true;
      }
      return false;
    default: return false;
  }
}

// The row that holds what plays now; -1 if none here. A search through
// the page's rows: at most the list's length, once, when a page opens.
int32_t LibraryPage::playingRow() const {
  if (playTrack_ == LibraryIndex::kNone) return -1;
  const uint32_t n = const_cast<LibraryPage*>(this)->rows();
  for (uint32_t r = 0; r < n; ++r) {
    if (playing(rowAt(r))) return static_cast<int32_t>(r);
  }
  return -1;
}

// ---- the page ----

void LibraryPage::enter(NavModel::PageRef& ref) {
  kind_ = static_cast<PageKind>(ref.kind);
  id_ = ref.id;
  if (kind_ == PageKind::Library && id_ > 2) id_ = ref.id = 0;
  ref_ = &ref;
  touchInList_ = false;
  headerPressed_ = 0;
  drawnTrack_ = ui_.state().trackId;
  notePlaying();
  if (root() && segment() == LibrarySegment::Folders && index()) {
    snprintf(playAll_, sizeof(playAll_), "Play all %lu",
             static_cast<unsigned long>(index()->treeTracks(LibraryIndex::rootFolder()).count));
  }
  repaintHeader();
  const bool showPlaying = ref.scrollPx == kShowPlaying;
  if (showPlaying) ref.scrollPx = -1;
  ListView& list = ui_.list();
  list.attach(this, &ref, 0);
  // From Now Playing: the playing track's row (or what holds it) on
  // screen, with a row of context above it when the list has to move.
  const int32_t row = showPlaying ? playingRow() : -1;
  if (row >= 0) {
    const uint32_t item = list.layout().itemOfRow(static_cast<uint32_t>(row));
    if (list.layout().reveal(item, 0, ListView::kHeight) != 0) {
      list.scrollTo((static_cast<int32_t>(item) - 1) * ListLayout::kPitch);
    }
  }
}

void LibraryPage::leave() {
  ui_.list().detach();
  if (root() && ref_) segScroll_[static_cast<int>(segment())] = ref_->scrollPx;
}

void LibraryPage::repaint() {
  repaintHeader();
  ui_.list().invalidate();
}

void LibraryPage::repaintHeader() {
  if (root()) {
    drawSegments();
  } else {
    ui_.drawHeader(header());
  }
}

void LibraryPage::home() { ui_.list().scrollTo(0); }

// Artists | Albums | Folders, the root's header (mockups 07, 13, 14).
void LibraryPage::drawSegments() {
  M5Canvas& s = gfx::strip();
  Fonts& f = Fonts::instance();
  s.fillRect(0, 0, kW, kHeaderH, col::HEAD);
  s.fillRoundRect(6, 4, kW - 12, 28, 10, col::CARD);
  for (int k = 0; k < 3; ++k) {
    const int x = 8 + k * 102, w = 100;
    const bool active = static_cast<int>(segment()) == k;
    const bool down = headerPressed_ == 10 + k;
    uint16_t bg = col::CARD;
    if (active || down) {
      bg = down ? col::ROW_SEL : col::BTN_HI;
      s.fillRoundRect(x, 6, w, 24, 8, bg);
    }
    f.draw(s, Font::Bold, kSegments[k], x + w / 2, 18, w - 6, active ? col::TXT : col::DIM, bg, Fonts::Align::Centre);
  }
  s.fillRect(0, kHeaderH - 2, kW, 2, accent::Library);
  gfx::push(s, 0, kHeaderY, kW, kHeaderH);
}

void LibraryPage::switchSegment(LibrarySegment seg) {
  ListView& list = ui_.list();
  if (seg == segment()) {
    list.scrollTo(0);  // the segment you're on: its top
    return;
  }
  list.detach();  // its place into the ref
  segScroll_[static_cast<int>(segment())] = ref_->scrollPx;
  id_ = ref_->id = static_cast<uint32_t>(seg);
  ref_->scrollPx = segScroll_[static_cast<int>(seg)];
  ref_->expanded = -1;
  if (seg == LibrarySegment::Folders && index()) {
    snprintf(playAll_, sizeof(playAll_), "Play all %lu",
             static_cast<unsigned long>(index()->treeTracks(LibraryIndex::rootFolder()).count));
  }
  drawSegments();
  list.attach(this, ref_, 0);
  Serial.printf("[ui] library: %s\n", kSegments[static_cast<int>(seg)]);
}

// Two levels down (the root > an artist > an album), the root crumb.
bool LibraryPage::crumb() const { return ui_.nav().depth() > 2; }

Header LibraryPage::header() const {
  char* sub = headerSub_;
  char* path = headerPath_;
  const size_t subSize = sizeof(headerSub_), pathSize = sizeof(headerPath_);
  Header h;
  h.sub = sub;
  sub[0] = 0;
  h.back = true;
  const LibraryIndex* i = index();
  switch (kind_) {
    case PageKind::Artist:
      h.title = i ? orNone(i->artistName(id_), "(no artist folder)") : "Artist";
      if (i) {
        const LibraryIndex::Artist& a = i->artist(id_);
        snprintf(sub, subSize, "%lu album%s, %lu tracks", static_cast<unsigned long>(a.albumCount),
                 a.albumCount == 1 ? "" : "s", static_cast<unsigned long>(a.trackCount));
      }
      break;
    case PageKind::Album:
      h.title = i ? orNone(i->albumName(id_), "(loose tracks)") : "Album";
      if (i) {
        const LibraryIndex::Album& a = i->album(id_);
        snprintf(sub, subSize, "%s, %lu tracks", orNone(i->artistName(a.artist), "no artist"),
                 static_cast<unsigned long>(a.trackCount));
      }
      break;
    case PageKind::ArtistTracks:
      h.title = "All tracks";
      if (i) snprintf(sub, subSize, "%s", orNone(i->artistName(id_), "(no artist folder)"));
      break;
    case PageKind::Folder:
      h.title = i ? i->folderName(id_) : "Folder";
      if (i) {
        // The one thin line: where it is, and what's in it.
        if (!i->folderPath(i->folder(id_).parent, path, pathSize)) snprintf(path, pathSize, "…");
        folderCounts(*i, id_, true, sub, subSize);
        h.path = path;
        h.counts = sub;
        h.sub = "";
      }
      break;
    default:
      break;
  }
  if (crumb()) {
    h.right = "Library";
    h.rightBack = true;
    h.pressed = headerPressed_ == 2;
  }
  return h;
}

bool LibraryPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  // What plays is marked (tinted, the EQ glyph) in every list.
  const uint32_t t = ui_.state().trackId;
  if (t != drawnTrack_) {
    drawnTrack_ = t;
    const uint32_t was = playTrack_;
    notePlaying();
    if (playTrack_ != was) ui_.list().refreshAll();
  }
  return ui_.list().update(nowMs, frameDue, wholeRows);
}

bool LibraryPage::animating() const { return ui_.list().animating(); }

void LibraryPage::thumbReady(uint32_t album) {
  if (!real()) return;
  ListView& list = ui_.list();
  uint32_t first, last;
  if (!list.visibleRows(&first, &last)) return;
  for (uint32_t r = first; r <= last; ++r) {
    const RowRef rr = rowAt(r);
    if (rr.kind == RowKind::Album && rr.id == album) list.refreshRow(r);
  }
}

void LibraryPage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    touchInList_ = e.y >= ListView::kTop;
    if (!touchInList_) {
      if (root()) {
        const int seg = e.atRightEdge() || e.x >= 214 ? 2 : e.x >= 107 ? 1 : 0;
        headerPressed_ = 10 + seg;
        drawSegments();
      } else {
        headerPressed_ = header().hit(e);
        if (headerPressed_ == 2) repaintHeader();
      }
    }
  } else if (e.type == T::DragStart && e.fromStrip) {
    touchInList_ = true;  // a swipe up from the strip: it scrolls the list
  }
  if (touchInList_) {
    ui_.list().onEvent(e);
    return;
  }
  if (e.type == T::Tap) {
    const int hit = headerPressed_;
    headerPressed_ = 0;
    if (hit >= 10) {
      ui_.tick();
      switchSegment(static_cast<LibrarySegment>(hit - 10));
      drawSegments();
    } else if (hit == 1 && !root()) {
      ui_.tick();
      ui_.back();
    } else if (hit == 2 && crumb()) {
      ui_.tick();
      ui_.toRoot();
    } else if (hit == 2) {
      repaintHeader();
    }
  } else if (e.type == T::DragStart || e.type == T::Release || e.type == T::Cancel) {
    const int was = headerPressed_;
    headerPressed_ = 0;
    if (was == 2 || was >= 10) repaintHeader();
  }
}

void LibraryPage::describe(char* buf, size_t size) const {
  snprintf(buf, size, "Library%s: %s %lu%s%s, %lu rows, what plays: track %ld", real() ? "" : " (SYNTHETIC)",
           pageKindName(static_cast<uint8_t>(kind_)), static_cast<unsigned long>(id_), root() ? " " : "",
           root() ? kSegments[static_cast<int>(segment())] : "",
           static_cast<unsigned long>(const_cast<LibraryPage*>(this)->rows()),
           playTrack_ == LibraryIndex::kNone ? -1L : static_cast<long>(playTrack_));
}

// ---- the list ----

uint32_t LibraryPage::rows() {
  const LibraryIndex* i = index();
  if (!i) return 0;
  switch (kind_) {
    case PageKind::Library:
      switch (segment()) {
        case LibrarySegment::Artists: return i->artistCount();
        case LibrarySegment::Albums: return i->albumCount();
        default: {
          const uint32_t f = LibraryIndex::rootFolder();
          return i->subfolders(f).count + i->filesIn(f).count;
        }
      }
    case PageKind::Artist: return i->albumsOf(id_).count + 1;
    case PageKind::Album:
    case PageKind::ArtistTracks: return pageTracks().count;
    case PageKind::Folder: return i->subfolders(id_).count + i->filesIn(id_).count;
    default: return 0;
  }
}

uint16_t LibraryPage::accent() { return accent::Library; }

bool LibraryPage::topBar() {
  if (!index()) return false;
  return !root() || segment() == LibrarySegment::Folders;
}

bool LibraryPage::alphabetical() { return railRows() > 0; }

uint32_t LibraryPage::railRows() {
  const LibraryIndex* i = index();
  if (!i) return 0;
  if (root() && segment() == LibrarySegment::Artists) return i->artistCount();
  if (root() && segment() == LibrarySegment::Albums) return i->albumCount();
  if (folderList()) return i->subfolders(folderId()).count;  // its files after them aren't in the grid
  return 0;
}

const char* LibraryPage::rowName(uint32_t row) const {
  const LibraryIndex* i = index();
  if (!i) return "";
  const RowRef r = rowAt(row);
  switch (r.kind) {
    case RowKind::Artist: return i->artistName(r.id);
    case RowKind::Album: return i->albumName(r.id);
    case RowKind::Folder: return i->folderName(r.id);
    case RowKind::Track:
    case RowKind::File: return i->trackFileName(r.id);
    default: return "";
  }
}

// The artists and albums sort past a leading "The" (textfold::sortName():
// "The Lantern Choir" under L), so their rail and jump grid go by that; the
// folders sort by their names as they are.
const char* LibraryPage::railName(uint32_t row) {
  const LibraryIndex* i = index();
  if (!i) return "";
  const RowRef r = rowAt(row);
  if (r.kind == RowKind::Artist) return textfold::sortName(i->artistName(r.id));
  if (r.kind == RowKind::Album) return textfold::sortName(i->albumName(r.id));
  return rowName(row);
}

char LibraryPage::railKey(uint32_t row) { return textfold::railKey(railName(row)); }

const char* LibraryPage::jumpTitle() {
  if (root()) return kSegments[static_cast<int>(segment())];
  const LibraryIndex* i = index();
  return i && kind_ == PageKind::Folder ? i->folderName(id_) : "";
}

bool LibraryPage::tinted(uint32_t row) { return playing(rowAt(row)); }

const char* LibraryPage::emptyText() {
  const LibraryIndex* i = const_cast<Ui&>(ui_).browseIndex();
  if (!i || !i->ready()) return "No music found: put folders in /music";
  return folderList() ? "No audio files here" : "Nothing here";
}

// The root with nothing to show (spec §7): no card, or a card without
// music; both with Try again.
bool LibraryPage::emptyState(EmptyState& e) {
  if (!root() || !real()) return false;
  const AppState& s = ui_.state();
  if (!s.card && s.libraryTracks == 0) {
    noCardState(e, s.cardKind);
    return true;
  }
  if (s.libraryTracks == 0) {
    e.icon = &icons::kFolder;
    e.iconColour = col::AMBER;
    e.title = "No music found";
    e.line1 = "Put folders in /music/Artist/Album/,";
    e.line2 = "MP3 or FLAC, then tap Try again.";
    e.buttons[0] = "Try again";
    return true;
  }
  return false;
}

void LibraryPage::onEmptyAction(int i) {
  (void)i;
  const AppState& s = ui_.state();
  if (!s.card && s.libraryTracks == 0) {
    ui_.retryCard();
    return;
  }
  ui_.toast("Looking in /music again...", false);
  ui_.host().rescanLibrary();  // the Library starts over (libraryChanged)
}

void LibraryPage::drawRow(ListView::Row& r) {
  const LibraryIndex* i = index();
  if (!i) return;
  const RowRef rr = rowAt(r.row);
  const bool now = playing(rr);
  const uint16_t titleInk = now ? accent::Library : col::TXT;
  const Font titleFont = now ? Font::Bold : Font::Body;
  char sub[64];
  switch (rr.kind) {
    case RowKind::Artist: {
      const char* name = i->artistName(rr.id);
      const LibraryIndex::Artist& a = i->artist(rr.id);
      // The initial is the row's rail letter ("The Lantern Choir": L, among the L's).
      const int x = ListView::disc(r, textfold::railKey(textfold::sortName(name)), discColour(name));
      const int right = ListView::chevron(r);
      snprintf(sub, sizeof(sub), "%lu album%s, %lu tracks", static_cast<unsigned long>(a.albumCount),
               a.albumCount == 1 ? "" : "s", static_cast<unsigned long>(a.trackCount));
      const char* shown = orNone(name, "(no artist folder)");
      ListView::lines(r, x, right, shown, strlen(shown), sub, strlen(sub), titleInk, titleFont);
      break;
    }
    case RowKind::Album: {
      Thumbs& thumbs = ui_.thumbs();
      const uint16_t* px = real() && thumbs.hasCover(rr.id) ? thumbs.get(rr.id, ThumbCache::Size::Small) : nullptr;
      const int x = ListView::thumb(r, px);
      const int right = ListView::chevron(r);
      const LibraryIndex::Album& al = i->album(rr.id);
      if (root()) {
        snprintf(sub, sizeof(sub), "%s", orNone(i->artistName(al.artist), "(no artist folder)"));
      } else {
        snprintf(sub, sizeof(sub), "%lu tracks", static_cast<unsigned long>(al.trackCount));
      }
      const char* shown = orNone(i->albumName(rr.id), "(loose tracks)");
      ListView::lines(r, x, right, shown, strlen(shown), sub, strlen(sub), titleInk, titleFont);
      break;
    }
    case RowKind::AllTracks: {
      const int right = ListView::chevron(r);
      r.c.fillRoundRect(r.x + 6, 1, 40, 40, 4, col::BTN);
      icons::drawCentred(r.c, icons::kQueue, r.x + 26, 21, accent::Library);
      snprintf(sub, sizeof(sub), "%lu tracks, album by album", static_cast<unsigned long>(i->artist(id_).trackCount));
      ListView::lines(r, r.x + 54, right, "All tracks", 10, sub, strlen(sub), col::TXT);
      break;
    }
    case RowKind::Track: {
      const uint8_t number = i->track(rr.id).number;
      const int x = now ? ListView::playing(r, accent::Library) : ListView::number(r, number ? number : r.row + 1, col::DIM);
      uint8_t len = 0;
      const char* title = i->trackTitle(rr.id, &len);
      if (kind_ == PageKind::ArtistTracks) {
        const char* album = orNone(i->albumName(i->track(rr.id).album), "(loose tracks)");
        ListView::lines(r, x, r.right - 8, title, len, album, strlen(album), titleInk, titleFont);
      } else {
        ListView::lines(r, x, r.right - 8, title, len, nullptr, 0, titleInk, titleFont);
      }
      break;
    }
    case RowKind::Folder: {
      const int x = ListView::icon(r, icons::kFolder, col::AMBER);
      const int right = ListView::chevron(r);
      folderCounts(*i, rr.id, false, sub, sizeof(sub));
      const char* name = i->folderName(rr.id);
      ListView::lines(r, x, right, name, strlen(name), sub, strlen(sub), titleInk, titleFont);
      break;
    }
    case RowKind::File: {
      const int x = now ? ListView::playing(r, accent::Library) : ListView::icon(r, icons::kFile, col::DIM);
      const LibraryIndex::Format fmt = i->track(rr.id).format;
      const int right = ListView::badge(r, fmt == LibraryIndex::Format::Flac ? "FLAC" : "MP3");
      // The file's name as it is, less its extension ("08 - Nightcall").
      const char* name = i->trackFileName(rr.id);
      const char* dot = strrchr(name, '.');
      const size_t len = dot && dot != name ? static_cast<size_t>(dot - name) : strlen(name);
      ListView::lines(r, x, right, name, len, nullptr, 0, titleInk, titleFont);
      break;
    }
    default:
      break;
  }
}

int LibraryPage::actions(int32_t row, const char* labels[3]) {
  if (row < 0 && root()) {
    // The Folders root: the whole card, said as it is (the review's graft).
    labels[0] = playAll_;
    labels[1] = "+ Queue";
    return 2;
  }
  for (int k = 0; k < 3; ++k) labels[k] = kActions[k];
  return 3;
}

void LibraryPage::act(LibraryIndex::Span span, int32_t start, int action, const char* name) {
  if (!real()) {
    ui_.toast("A synthetic library: browsing only", false);
    return;
  }
  const LibraryIndex* i = index();
  if (!i || span.count == 0) return;
  PlaybackController& p = ui_.player();
  QueueModel& q = ui_.queue();
  // A single track is called by its title.
  char what[96];
  if (span.count == 1 || start >= 0) {
    uint8_t len = 0;
    const char* t = i->trackTitle(span[start >= 0 ? static_cast<uint32_t>(start) : 0], &len);
    snprintf(what, sizeof(what), "%.*s", static_cast<int>(len), t);
  }
  // Where an add puts its first track (QueueModel: Play next right after
  // the current entry, + Queue at the end): the toast's View goes there.
  const uint32_t addedAt = action == 1 && q.current() >= 0 ? static_cast<uint32_t>(q.current()) + 1 : q.size();
  bool ok = false;
  char text[128];
  switch (action) {
    case 0:
      // A tapped track plays first; a container's Play (an artist, an
      // album, a folder, "Play all N") starts on the first, or, while
      // shuffled, on a random track (docs/QUEUE-MODES.md section 2.5).
      ok = p.playNow(span.ids, span.count,
                     start >= 0 ? static_cast<uint32_t>(start) : PlaybackController::kAnyStart);
      if (ok) ui_.added().clear();  // a new queue: nothing "added" to show in it
      if (ok && p.state() == PlayState::Waiting) {
        // The headphones aren't connected: it plays once they are (Now
        // Playing shows the wait, and its way out).
        const char* them = ui_.state().btName[0] ? ui_.state().btName : "the headphones";
        snprintf(text, sizeof(text), "Waiting for %s: %s", them, start >= 0 ? what : name);
      } else {
        snprintf(text, sizeof(text), "Playing: %s", start >= 0 ? what : name);
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
        snprintf(text, sizeof(text), "Added: %s", what);
      } else {
        snprintf(text, sizeof(text), "Added %lu tracks", static_cast<unsigned long>(span.count));
      }
      break;
  }
  static const char* const kVerbs[3] = {"play", "play next", "add"};
  Serial.printf("[ui] library: %s %lu track%s (%s)%s\n", kVerbs[action < 0 || action > 2 ? 2 : action],
                static_cast<unsigned long>(span.count), span.count == 1 ? "" : "s", span.count == 1 ? what : name,
                ok ? "" : ": NO MEMORY");
  const uint32_t viewKey = ok && action != 0 ? q.keyAt(addedAt) : QueueModel::kNone;
  ui_.toast(ok ? text : "Not enough memory for that", ok, viewKey);
}

void LibraryPage::onRowAction(const RowRef& r, int action) {
  LibraryIndex::Span span;
  const char* name = "";
  if (container(r, &span, &name)) {
    act(span, -1, action, name);
    return;
  }
  if (r.kind != RowKind::Track && r.kind != RowKind::File) return;
  if (action == 0) {
    act(pageTracks(), static_cast<int32_t>(r.index), 0, "");  // its album or folder, from it
  } else {
    uint32_t one = r.id;
    LibraryIndex::Span single{&one, 1};
    act(single, -1, action, "");
  }
}

void LibraryPage::onAction(int32_t row, int action) {
  if (row >= 0) {
    onRowAction(rowAt(static_cast<uint32_t>(row)), action);
    return;
  }
  // The top bar: the page's own container. The Folders root has two
  // buttons: Play all, + Queue.
  if (root() && action == 1) action = 2;
  onRowAction(RowRef{}, action);
}

ListView::Tap LibraryPage::onTap(uint32_t row) {
  const RowRef r = rowAt(row);
  switch (r.kind) {
    case RowKind::Artist: ui_.push(page(PageKind::Artist, r.id)); return ListView::Tap::Handled;
    case RowKind::Album: ui_.push(page(PageKind::Album, r.id)); return ListView::Tap::Handled;
    case RowKind::AllTracks: ui_.push(page(PageKind::ArtistTracks, r.id)); return ListView::Tap::Handled;
    case RowKind::Folder: ui_.push(page(PageKind::Folder, r.id)); return ListView::Tap::Handled;
    case RowKind::Track:
    case RowKind::File: return ListView::Tap::Expand;  // its inline bar
    default: return ListView::Tap::Handled;
  }
}

// Every row holds: the same three actions in a sheet, without opening it.
bool LibraryPage::holds(uint32_t row) { return rowAt(row).kind != RowKind::None; }

void LibraryPage::onHold(uint32_t row) {
  held_ = rowAt(row);
  const LibraryIndex* i = index();
  if (!i || held_.kind == RowKind::None) return;
  char title[96];
  LibraryIndex::Span span;
  const char* name = "";
  if (container(held_, &span, &name)) {
    snprintf(title, sizeof(title), "%s, %lu track%s", name, static_cast<unsigned long>(span.count),
             span.count == 1 ? "" : "s");
  } else {
    uint8_t len = 0;
    const char* t = i->trackTitle(held_.id, &len);
    snprintf(title, sizeof(title), "%.*s", static_cast<int>(len), t);
  }
  ui_.openSheet(this, title, kActions, 3, nullptr, /*primary=*/0);  // Play
}

void LibraryPage::onSheet(int choice) {
  if (choice < 0 || held_.kind == RowKind::None) return;
  const RowRef r = held_;
  held_ = RowRef{};
  onRowAction(r, choice);
}

}  // namespace ui
