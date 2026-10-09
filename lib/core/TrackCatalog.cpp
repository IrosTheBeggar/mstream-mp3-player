// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TrackCatalog.h"

#include <cstring>

#include "CardContract.h"
#include "LibraryText.h"
#include "NameKey.h"

namespace {

struct Builtin {
  const char* path;
  const char* title;
  uint32_t durationMs;
};

// The backend makes these itself (ToneGen, ClickGen): no file behind them.
constexpr Builtin kBuiltins[] = {
    {"tone:440", "Test tone 440 Hz", 0},
    {"tone:1000", "Test tone 1 kHz", 0},
    {"tone:left", "Left ear only", 0},
    // Click tracks with a known beat, for the dancer's beat tracker.
    {"tone:click90", "Clicks 90 BPM", 60000},
    {"tone:click120", "Clicks 120 BPM", 60000},
    {"tone:click128", "Clicks 128 BPM", 60000},
    {"tone:click140", "Clicks 140 BPM", 60000},
    {"tone:click174", "Clicks 174 BPM", 60000},
    {"tone:click120off", "Clicks 120 BPM, late start", 60000},
    // An hour of digital silence, for power measurements (the console's
    // Pz): the outputs run at their full rate and nothing is heard. Known
    // (a path, a title) but not listed: never queued with the others.
    {TrackCatalog::kSilencePath, "Silence (power test)", 3600000},
    // The rate converter's test tracks (the console's Rt; docs/RESAMPLER.md
    // section 6): made at another rate and converted to 44.1 kHz like a
    // file. Known but not listed either: never queued, Rt plays them on
    // their own.
    {"tone:1000@48000", "Test tone 1 kHz, 48 kHz", 0},
    {"tone:1000@96000", "Test tone 1 kHz, 96 kHz", 0},
    {"tone:1000@88200", "Test tone 1 kHz, 88.2 kHz", 0},
    {"tone:1000@32000", "Test tone 1 kHz, 32 kHz", 0},
    {"tone:1000@22050", "Test tone 1 kHz, 22.05 kHz", 0},
    {"tone:1000@8000", "Test tone 1 kHz, 8 kHz", 0},
    {"tone:silence@48000", "Silence, 48 kHz", 3600000},
    {"tone:silence@96000", "Silence, 96 kHz", 3600000},
    {"tone:silence@22050", "Silence, 22.05 kHz", 3600000},
};
constexpr uint32_t kCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
constexpr uint32_t kListed = 9;      // the tones and the click tracks
constexpr uint32_t kRateTests = 10;  // the first of the rate test tracks (after the silence)

constexpr uint32_t B = TrackCatalog::kBuiltin;
constexpr uint32_t kIds[] = {B + 0, B + 1, B + 2, B + 3, B + 4, B + 5, B + 6, B + 7, B + 8};
static_assert(sizeof(kIds) / sizeof(kIds[0]) == kListed, "one id per listed built-in track");
constexpr uint32_t kTestIds[] = {B + 10, B + 11, B + 12, B + 13, B + 14, B + 15, B + 16, B + 17, B + 18};
static_assert(sizeof(kTestIds) / sizeof(kTestIds[0]) == kCount - kRateTests, "one id per rate test track");

// Copies `len` bytes and a NUL, or writes "" and returns 0 if it won't fit.
size_t copyOut(const char* s, size_t len, char* buf, size_t size) {
  if (size == 0) return 0;
  if (len + 1 > size) {
    buf[0] = 0;
    return 0;
  }
  std::memcpy(buf, s, len);
  buf[len] = 0;
  return len;
}

// Copies what fits of `len` bytes, cut at a UTF-8 code point boundary.
size_t copyCut(const char* s, size_t len, char* buf, size_t size) {
  if (size == 0) return 0;
  if (len + 1 > size) {
    len = size - 1;
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;  // not mid-character
  }
  return copyOut(s, len, buf, size);
}

}  // namespace

uint32_t TrackCatalog::builtinCount() { return kCount; }

LibraryIndex::Span TrackCatalog::builtins() { return {kIds, kListed}; }

LibraryIndex::Span TrackCatalog::rateTests() { return {kTestIds, kCount - kRateTests}; }

bool TrackCatalog::valid(uint32_t id) const { return isBuiltin(id) || inIndex(id); }

size_t TrackCatalog::path(uint32_t id, char* buf, size_t size) const {
  if (isBuiltin(id)) {
    const char* p = kBuiltins[id - kBuiltin].path;
    return copyOut(p, std::strlen(p), buf, size);
  }
  if (inIndex(id)) return index_->trackPath(id, buf, size);
  return copyOut("", 0, buf, size);
}

// ---- the overlay ----

namespace {

// `n` bytes of `s` into an overlay's field, cut at a character boundary;
// "" for none.
void fill(char* field, const char* s, size_t n) {
  const size_t len = s && n ? cardcontract::utf8CutLength(s, n, TrackCatalog::Overlay::kField - 1) : 0;
  if (len) std::memcpy(field, s, len);
  field[len] = 0;
}

}  // namespace

void TrackCatalog::Overlay::set(const LibraryIndex& index, uint32_t id, const LibraryIndex::TagView& tags) {
  track = id;
  stamp = index.buildStamp();
  ++version;
  // (A blank title or album, White_Space alone, is none: the index's, as
  // the next build would show it.)
  fill(title, tags.title, namekey::blank(tags.title, tags.titleLen) ? 0 : tags.titleLen);
  fill(album, tags.album, namekey::blank(tags.album, tags.albumLen) ? 0 : tags.albumLen);
  artist[0] = 0;
  if (tags.artist && tags.artistLen) namekey::displayJoin(tags.artist, tags.artistLen, artist, sizeof(artist));
  year = tags.year;
  durationMs = tags.durationMs;
}

void TrackCatalog::Overlay::clear() {
  track = kNone;
  stamp = 0;
  ++version;
  title[0] = artist[0] = album[0] = 0;
  year = 0;
  durationMs = 0;
}

const TrackCatalog::Overlay* TrackCatalog::overlayFor(uint32_t id) const {
  if (!overlay_ || overlay_->track != id || !inIndex(id)) return nullptr;
  return overlay_->stamp == index_->buildStamp() ? overlay_ : nullptr;
}

// ---- the names ----

size_t TrackCatalog::title(uint32_t id, char* buf, size_t size) const {
  if (isBuiltin(id)) {
    const char* t = kBuiltins[id - kBuiltin].title;
    return copyCut(t, std::strlen(t), buf, size);
  }
  if (inIndex(id)) {
    const Overlay* o = overlayFor(id);
    if (o && o->title[0]) return copyCut(o->title, std::strlen(o->title), buf, size);
    uint8_t len = 0;
    const char* t = index_->trackTitle(id, &len);
    return copyCut(t, len, buf, size);
  }
  if (const Held* h = heldFor(id)) return copyCut(h->title, std::strlen(h->title), buf, size);
  return copyOut("", 0, buf, size);
}

const char* TrackCatalog::artist(uint32_t id) const {
  if (isBuiltin(id)) return "built-in";
  if (const Held* h = heldFor(id)) return h->artist;
  if (!inIndex(id)) return "";
  const Overlay* o = overlayFor(id);
  if (o && o->artist[0]) return o->artist;
  return librarytext::trackArtist(*index_, id);
}

// An artist folder's own tracks keep the name "" and no year whatever their
// tags say (Stage A, 5.4): the overlay doesn't name them either, so the
// next build changes nothing on screen.
bool TrackCatalog::loose(uint32_t id) const {
  return (index_->album(index_->track(id).album).flags & LibraryIndex::kLoose) != 0;
}

const char* TrackCatalog::album(uint32_t id) const {
  if (isBuiltin(id)) return "Built-in";
  if (const Held* h = heldFor(id)) return h->album;
  if (!inIndex(id)) return "";
  const Overlay* o = overlayFor(id);
  if (o && o->album[0] && !loose(id)) return o->album;
  return index_->albumName(index_->track(id).album);
}

const char* TrackCatalog::albumArtist(uint32_t id) const {
  if (const Held* h = heldFor(id)) return h->albumArtist;
  return inIndex(id) ? librarytext::albumArtist(*index_, index_->track(id).album) : "";
}

uint16_t TrackCatalog::year(uint32_t id) const {
  if (const Held* h = heldFor(id)) return h->year;
  if (!inIndex(id)) return 0;
  const Overlay* o = overlayFor(id);
  if (o && o->year && !loose(id)) return o->year;
  return index_->album(index_->track(id).album).year;
}

uint32_t TrackCatalog::durationHintMs(uint32_t id) const {
  if (isBuiltin(id)) return kBuiltins[id - kBuiltin].durationMs;
  if (const Held* h = heldFor(id)) return h->durationMs;
  if (!inIndex(id)) return 0;
  const Overlay* o = overlayFor(id);
  if (o && o->durationMs) return o->durationMs;
  return static_cast<uint32_t>(index_->track(id).durationS) * 1000u;
}

bool TrackCatalog::take(uint32_t id, Held* h) const {
  *h = Held{};
  if (!inIndex(id)) return false;
  h->track = id;
  title(id, h->title, sizeof(h->title));
  const auto copy = [](const char* s, char* out, size_t cap) {
    const size_t n = s ? cardcontract::utf8CutLength(s, std::strlen(s), cap - 1) : 0;
    if (n) std::memcpy(out, s, n);
    out[n] = 0;
  };
  copy(artist(id), h->artist, sizeof(h->artist));
  copy(album(id), h->album, sizeof(h->album));
  copy(albumArtist(id), h->albumArtist, sizeof(h->albumArtist));
  h->year = year(id);
  h->durationMs = durationHintMs(id);
  return true;
}

uint32_t TrackCatalog::find(const char* path) const {
  if (!path) return kNone;
  if (std::strncmp(path, "tone:", 5) == 0) {
    for (uint32_t i = 0; i < kCount; ++i) {
      if (std::strcmp(kBuiltins[i].path, path) == 0) return kBuiltin + i;
    }
    return kNone;
  }
  return index_ ? index_->findTrack(path) : kNone;
}
