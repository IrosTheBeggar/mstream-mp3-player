// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryText.h"

#include <cstdio>
#include <cstring>

#include "TextFold.h"
#include "UiStrings.h"

namespace librarytext {

using uistr::S;

namespace {

// snprintf's count, clamped to what `buf` holds ("" and 0 for no room). A
// text snprintf cut short loses a character it cut in two: the screen's
// fonts get whole UTF-8 (the row draws an ellipsis where it doesn't fit).
size_t done(int n, char* buf, size_t size) {
  if (size == 0) return 0;
  if (n < 0) {
    buf[0] = 0;
    return 0;
  }
  if (static_cast<size_t>(n) < size) return static_cast<size_t>(n);
  size_t len = size - 1;
  size_t lead = len;
  while (lead > 0 && (static_cast<unsigned char>(buf[lead - 1]) & 0xC0) == 0x80) --lead;
  if (lead > 0) {
    const unsigned char c = static_cast<unsigned char>(buf[lead - 1]);
    const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (lead - 1 + need > len) len = lead - 1;  // its last character is cut: drop it
  }
  buf[len] = 0;
  return len;
}

// Appends `s` (and a kDot before it when something is there already).
void part(const char* s, char* buf, size_t size, size_t* at) {
  if (!s || !s[0] || *at + 1 >= size) return;
  const int n = snprintf(buf + *at, size - *at, "%s%s", *at ? kDot : "", s);
  *at += done(n, buf + *at, size - *at);
}

bool ready(const LibraryIndex& index) { return index.ready(); }

}  // namespace

const char* albumArtist(const LibraryIndex& index, uint32_t album) {
  if (!ready(index) || album >= index.albumCount()) return "";
  const LibraryIndex::Album& a = index.album(album);
  if (a.flags & LibraryIndex::kTagged) {
    // A compilation's line (no album artist tagged): the UI's words.
    if (a.flags & LibraryIndex::kVarious) return uistr::tr(uistr::S::library_various_artists);
    return index.albumArtistLine(album);
  }
  return a.artist < index.artistCount() ? index.artistName(a.artist) : "";
}

const char* ownArtist(const LibraryIndex& index, uint32_t track) {
  if (!ready(index) || track >= index.trackCount()) return nullptr;
  const uint32_t a = index.track(track).trackArtist;
  if (a == LibraryIndex::kNone) return nullptr;
  const char* s = index.str(a);
  return s[0] ? s : nullptr;
}

const char* trackArtist(const LibraryIndex& index, uint32_t track) {
  if (!ready(index) || track >= index.trackCount()) return "";
  const char* own = ownArtist(index, track);
  return own ? own : albumArtist(index, index.track(track).album);
}

size_t trackCount(uint32_t n, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  return trnf(buf, size, S::library_tracks_n, n, n);
}

size_t artistCounts(const LibraryIndex& index, uint32_t artist, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || artist >= index.artistCount()) return 0;
  const LibraryIndex::Artist& a = index.artist(artist);
  char albums[48], tracks[48];
  trnf(albums, sizeof(albums), S::library_albums_n, a.albumCount, a.albumCount);
  trackCount(a.trackCount, tracks, sizeof(tracks));
  return trf(buf, size, S::library_artist_counts, albums, tracks);
}

const char* albumShown(const LibraryIndex& index, uint32_t album) {
  if (!ready(index) || album >= index.albumCount()) return tr(S::library_loose_tracks);
  const char* n = index.albumName(album);
  return n[0] ? n : tr(S::library_loose_tracks);
}

const char* artistShown(const LibraryIndex& index, uint32_t artist) {
  if (!ready(index) || artist >= index.artistCount()) return tr(S::library_no_artist_folder);
  const char* n = index.artistName(artist);
  return n[0] ? n : tr(S::library_no_artist_folder);
}

size_t albumSub(const LibraryIndex& index, uint32_t album, AlbumPlace place, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || album >= index.albumCount()) return 0;
  const LibraryIndex::Album& a = index.album(album);
  char year[8] = "";
  if (a.year) snprintf(year, sizeof(year), "%u", static_cast<unsigned>(a.year));
  size_t at = 0;
  if (place == AlbumPlace::AZ) {
    const char* line = albumArtist(index, album);
    part(line[0] ? line : tr(S::library_no_artist_folder), buf, size, &at);
    part(year, buf, size, &at);
  } else {
    char tracks[24];
    trackCount(a.trackCount, tracks, sizeof(tracks));
    part(year, buf, size, &at);
    part(tracks, buf, size, &at);
  }
  return at;
}

size_t albumHeaderLine(const textfit::Font* small, int maxW, const char* artist, bool phrase, uint32_t year,
                       uint32_t tracks, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  char y[8] = "", t[24];
  if (year) snprintf(y, sizeof(y), "%u", static_cast<unsigned>(year));
  trackCount(tracks, t, sizeof(t));
  // The line from `a` (may be ""), `yr` (may be "") and the count; whole
  // when every part is in `buf` (a long name in a short buffer would push
  // out the count) and it fits `maxW`.
  const size_t dotLen = strlen(kDot);
  auto line = [&](const char* a, const char* yr) {
    size_t at = 0;
    buf[0] = 0;
    part(a, buf, size, &at);
    part(yr, buf, size, &at);
    part(t, buf, size, &at);
    const size_t len = strlen(a) + (yr[0] ? (a[0] ? dotLen : 0) + strlen(yr) : 0) + (a[0] || yr[0] ? dotLen : 0) +
                       strlen(t);
    const bool whole = at == len && (!small || textfit::drawnWidth(*small, buf) <= maxW);
    return whole ? at : static_cast<size_t>(0);
  };
  const char* name = artist ? artist : "";
  const size_t all = line(name, y);
  if (all || !small) return strlen(buf);
  // The year gives way first ("Daft Punk · 14 tracks"), then the name is cut
  // to what the count leaves ("The Lantern C… · 14 tracks"); a phrase,
  // or a name left less than kAlbumLineMinNameW px, is left out ("1999 ·
  // 14 tracks").
  if (y[0] && line(name, "")) return strlen(buf);
  if (!phrase && name[0]) {
    const int room = maxW - textfit::drawnWidth(*small, t) - textfit::drawnWidth(*small, kDot);
    const size_t tLen = strlen(t);
    if (room >= kAlbumLineMinNameW && tLen + dotLen + 2 <= size) {
      char cut[96];
      const size_t cutSize = size - tLen - dotLen < sizeof(cut) ? size - tLen - dotLen : sizeof(cut);
      textfit::fit(*small, name, strlen(name), cut, cutSize, room);
      if (line(cut, "")) return strlen(buf);
    }
  }
  line("", y);  // (whole: the year and a count fit any room this line has)
  return strlen(buf);
}

size_t albumHeader(const textfit::Font& small, int maxW, const LibraryIndex& index, uint32_t album, char* buf,
                   size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || album >= index.albumCount()) return 0;
  const LibraryIndex::Album& a = index.album(album);
  const char* line = albumArtist(index, album);
  // No artist folder, or a compilation's "Various Artists": the UI's words.
  const bool phrase = !line[0] || line == tr(S::library_various_artists);
  return albumHeaderLine(&small, maxW, line[0] ? line : tr(S::library_no_artist_folder), phrase, a.year,
                         a.trackCount, buf, size);
}

size_t albumHeader(const LibraryIndex& index, uint32_t album, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || album >= index.albumCount()) return 0;
  const LibraryIndex::Album& a = index.album(album);
  const char* line = albumArtist(index, album);
  return albumHeaderLine(nullptr, 0, line[0] ? line : tr(S::library_no_artist_folder), true, a.year, a.trackCount,
                         buf, size);
}

size_t trackSub(const LibraryIndex& index, uint32_t track, bool album, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || track >= index.trackCount()) return 0;
  size_t at = 0;
  part(ownArtist(index, track), buf, size, &at);
  if (album) part(albumShown(index, index.track(track).album), buf, size, &at);
  return at;
}

const char* railName(const LibraryIndex& index, LibraryIndex::View view, uint32_t id) {
  if (!ready(index)) return "";
  if (view == LibraryIndex::View::Artists) {
    return id < index.artistCount() ? textfold::sortName(index.artistSortKey(id)) : "";
  }
  return id < index.albumCount() ? textfold::sortName(index.albumSortKey(id)) : "";
}

size_t discText(uint32_t disc, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  return trf(buf, size, S::library_disc, disc);
}

size_t folderCounts(uint32_t folders, uint32_t files, uint32_t other, bool audioWord, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  size_t n = 0;
  buf[0] = 0;
  auto add = [&](uint32_t count, S id) {
    if (!count || n + 1 >= size) return;
    if (n) n += uistr::format(buf + n, size - n, tr(S::format_list_sep));
    n += trnf(buf + n, size - n, id, count, count);
  };
  add(folders, S::library_folders_n);
  add(files, audioWord ? S::library_audio_files_n : S::library_files_n);
  add(other, S::library_other_n);
  if (!n) n = trf(buf, size, S::library_folder_empty);
  return n;
}

size_t folderHeaderCounts(const textfit::Font& small, int maxW, uint32_t folders, uint32_t files, uint32_t other,
                          char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  size_t n = folderCounts(folders, files, other, true, buf, size);  // all of it
  if (textfit::drawnWidth(small, buf) <= maxW) return n;
  n = folderCounts(folders, files, other, false, buf, size);  // "files"
  if (textfit::drawnWidth(small, buf) <= maxW) return n;
  if (folders || files) {  // without the other files
    n = folderCounts(folders, files, 0, false, buf, size);
    if (textfit::drawnWidth(small, buf) <= maxW) return n;
  }
  // One part: the files, else the folders, else the other files.
  return files ? folderCounts(0, files, 0, false, buf, size)
         : folders ? folderCounts(folders, 0, 0, false, buf, size)
                   : folderCounts(0, 0, other, false, buf, size);
}

// ---- Discs ----

void Discs::set(const LibraryIndex& index, uint32_t album) {
  n_ = 0;
  if (!index.ready() || album >= index.albumCount() || index.album(album).discs <= 1) return;
  const LibraryIndex::Span t = index.tracksOfAlbum(album);
  uint32_t last = 0;  // no disc yet (a disc is at least 1)
  for (uint32_t i = 0; i < t.count; ++i) {
    const uint8_t d = index.track(t[i]).disc;
    const uint32_t disc = d ? d : 1;
    if (disc == last) continue;
    if (n_ == kMax) {  // discs that don't group: no dividers
      n_ = 0;
      return;
    }
    start_[n_] = i;
    disc_[n_] = static_cast<uint16_t>(disc);
    ++n_;
    last = disc;
  }
  // One run of one disc (every track says disc 2, say): nothing to divide.
  if (n_ == 1) n_ = 0;
}

Discs::Row Discs::at(uint32_t row) const {
  Row r;
  if (n_ == 0) {
    r.track = row;
    return r;
  }
  // Divider k sits at row start_[k] + k: the last one at or before `row`.
  uint32_t k = 0;
  while (k + 1 < n_ && start_[k + 1] + k + 1 <= row) ++k;
  if (row == start_[k] + k) {
    r.divider = true;
    r.disc = disc_[k];
    r.track = start_[k];
    return r;
  }
  r.track = row - (k + 1);
  return r;
}

uint32_t Discs::rowOf(uint32_t track) const {
  uint32_t k = 0;
  while (k < n_ && start_[k] <= track) ++k;
  return track + k;
}

// ---- the scan's texts ----

size_t statusText(const Status& s, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  switch (s.phase) {
    case Status::Phase::Idle: return 0;
    case Status::Phase::Checking: return trf(buf, size, S::library_status_checking);
    case Status::Phase::Reading:
      return trf(buf, size, S::library_status_reading, uistr::grouped(s.done), uistr::grouped(s.total));
    case Status::Phase::Updating: return trf(buf, size, S::library_status_updating);
    case Status::Phase::Unfinished: return trf(buf, size, S::library_status_unfinished);
  }
  return 0;
}

size_t foundText(uint32_t n, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  return trnf(buf, size, S::library_found, n, uistr::grouped(n));
}

Sources sourcesOf(const LibraryIndex& index) {
  Sources s;
  if (!index.ready()) return s;
  for (uint32_t t = 0; t < index.trackCount(); ++t) {
    switch (index.track(t).flags & LibraryIndex::kSourceMask) {
      case LibraryIndex::kFromTransfer: ++s.transfer; break;
      case LibraryIndex::kFromDevice: ++s.device; break;
      default: ++s.none; break;
    }
  }
  return s;
}

size_t holdTitle(const textfit::Font& small, int maxW, const char* name, uint32_t tracks, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  if (!name) name = "";
  // The rest alone (no name): what it leaves the name.
  trnf(buf, size, S::library_hold_title, tracks, "", tracks);
  char cut[96];
  textfit::fit(small, name, strlen(name), cut, sizeof(cut), maxW - textfit::drawnWidth(small, buf));
  return trnf(buf, size, S::library_hold_title, tracks, cut, tracks);
}

size_t libraryRowTitle(uint32_t tracks, char* buf, size_t size, int form) {
  if (!buf || size == 0) return 0;
  if (tracks == 0) return trf(buf, size, S::library_row_empty);
  return trnf(buf, size, form == 0 ? S::library_row_title : S::library_tracks_n, tracks, uistr::grouped(tracks));
}

size_t sourcesText(const Sources& s, int form, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (s.transfer == 0 && s.device == 0) return trf(buf, size, S::library_row_paths);
  if (form >= 2) {
    const uint64_t tagged = static_cast<uint64_t>(s.transfer) + s.device;
    const uint32_t pct = static_cast<uint32_t>(tagged * 100 / s.total());
    return trf(buf, size, S::library_row_tagged, pct);
  }
  static const S kLong[3] = {S::library_row_src_transfer, S::library_row_src_device, S::library_row_src_none};
  static const S kShort[3] = {S::library_row_src_transfer_short, S::library_row_src_device_short,
                              S::library_row_src_none_short};
  const S* forms = form == 0 ? kLong : kShort;
  const uint32_t counts[3] = {s.transfer, s.device, s.none};
  size_t at = 0;
  for (int k = 0; k < 3; ++k) {
    if (counts[k] == 0) continue;
    if (at) at += uistr::format(buf + at, size - at, tr(S::format_list_sep));
    at += trnf(buf + at, size - at, forms[k], counts[k], uistr::grouped(counts[k]));
    if (at + 1 >= size) break;
  }
  return at;
}

}  // namespace librarytext
