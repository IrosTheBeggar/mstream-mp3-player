// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryText.h"

#include <cstdio>
#include <cstring>

#include "QueueView.h"
#include "TextFold.h"
#include "UiText.h"

namespace librarytext {

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
  if (a.flags & LibraryIndex::kTagged) return index.albumArtistLine(album);
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
  return done(snprintf(buf, size, "%lu track%s", static_cast<unsigned long>(n), n == 1 ? "" : "s"), buf, size);
}

size_t artistCounts(const LibraryIndex& index, uint32_t artist, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || artist >= index.artistCount()) return 0;
  const LibraryIndex::Artist& a = index.artist(artist);
  return done(snprintf(buf, size, "%lu album%s, %lu track%s", static_cast<unsigned long>(a.albumCount),
                       a.albumCount == 1 ? "" : "s", static_cast<unsigned long>(a.trackCount),
                       a.trackCount == 1 ? "" : "s"),
              buf, size);
}

const char* albumShown(const LibraryIndex& index, uint32_t album) {
  if (!ready(index) || album >= index.albumCount()) return uitext::kLooseTracks;
  const char* n = index.albumName(album);
  return n[0] ? n : uitext::kLooseTracks;
}

const char* artistShown(const LibraryIndex& index, uint32_t artist) {
  if (!ready(index) || artist >= index.artistCount()) return uitext::kNoArtistFolder;
  const char* n = index.artistName(artist);
  return n[0] ? n : uitext::kNoArtistFolder;
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
    part(line[0] ? line : uitext::kNoArtistFolder, buf, size, &at);
    part(year, buf, size, &at);
  } else {
    char tracks[24];
    trackCount(a.trackCount, tracks, sizeof(tracks));
    part(year, buf, size, &at);
    part(tracks, buf, size, &at);
  }
  return at;
}

size_t albumHeader(const LibraryIndex& index, uint32_t album, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (!ready(index) || album >= index.albumCount()) return 0;
  const LibraryIndex::Album& a = index.album(album);
  char year[8] = "", tracks[24];
  if (a.year) snprintf(year, sizeof(year), "%u", static_cast<unsigned>(a.year));
  trackCount(a.trackCount, tracks, sizeof(tracks));
  const char* line = albumArtist(index, album);
  size_t at = 0;
  part(line[0] ? line : uitext::kNoArtistFolder, buf, size, &at);
  part(year, buf, size, &at);
  part(tracks, buf, size, &at);
  return at;
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
  return done(snprintf(buf, size, uitext::kDisc, static_cast<unsigned>(disc)), buf, size);
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
    case Status::Phase::Checking: return done(snprintf(buf, size, "%s", uitext::kStatusChecking), buf, size);
    case Status::Phase::Reading: {
      char a[16], b[16];
      queueview::grouped(s.done, a, sizeof(a));
      queueview::grouped(s.total, b, sizeof(b));
      return done(snprintf(buf, size, uitext::kStatusReading, a, b), buf, size);
    }
    case Status::Phase::Updating: return done(snprintf(buf, size, "%s", uitext::kStatusUpdating), buf, size);
    case Status::Phase::Unfinished: return done(snprintf(buf, size, "%s", uitext::kStatusUnfinished), buf, size);
  }
  return 0;
}

size_t foundText(uint32_t n, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  if (n == 1) return done(snprintf(buf, size, "%s", uitext::kFoundOne), buf, size);
  char a[16];
  queueview::grouped(n, a, sizeof(a));
  return done(snprintf(buf, size, uitext::kFoundMany, a), buf, size);
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

size_t libraryRowTitle(uint32_t tracks, char* buf, size_t size, int form) {
  if (!buf || size == 0) return 0;
  if (tracks == 0) return done(snprintf(buf, size, "%s", uitext::kLibraryRowEmpty), buf, size);
  char a[16];
  queueview::grouped(tracks, a, sizeof(a));
  return done(snprintf(buf, size, form == 0 ? uitext::kLibraryRowTitle : uitext::kLibraryRowTitleShort, a), buf,
              size);
}

size_t sourcesText(const Sources& s, int form, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  if (s.transfer == 0 && s.device == 0) return done(snprintf(buf, size, "%s", uitext::kLibraryRowPaths), buf, size);
  if (form >= 2) {
    const uint64_t tagged = static_cast<uint64_t>(s.transfer) + s.device;
    const uint32_t pct = static_cast<uint32_t>(tagged * 100 / s.total());
    char p[8];
    snprintf(p, sizeof(p), "%lu", static_cast<unsigned long>(pct));
    return done(snprintf(buf, size, uitext::kLibrarySrcTagged, p), buf, size);
  }
  const char* const* forms = form == 0 ? uitext::kLibrarySrcLong : uitext::kLibrarySrcShort;
  const uint32_t counts[3] = {s.transfer, s.device, s.none};
  size_t at = 0;
  for (int k = 0; k < 3; ++k) {
    if (counts[k] == 0) continue;
    char a[16], part[48];
    queueview::grouped(counts[k], a, sizeof(a));
    snprintf(part, sizeof(part), forms[k], a);
    const int n = snprintf(buf + at, size - at, "%s%s", at ? ", " : "", part);
    if (n < 0 || static_cast<size_t>(n) >= size - at) return done(n, buf, size);
    at += static_cast<size_t>(n);
  }
  return at;
}

}  // namespace librarytext
