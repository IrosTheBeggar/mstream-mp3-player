// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "LibraryIndex.h"

// What a track id names. The queue and the player hold ids only (4 bytes a
// track, in PSRAM); this turns one into a path to play or a name to show
// when it's needed, into the caller's buffer. Portable, host-tested.
//
// Two kinds of id:
//   0 .. trackCount()-1   a track of the LibraryIndex
//   kBuiltin + n          a built-in track: the test tones and the click
//                         tracks with a known beat (paths "tone:..."), the
//                         "Built-in" pseudo-folder, always there, card or not;
//                         and, after them and not listed by builtins(), an
//                         hour of silence for power measurements
//                         (kSilencePath) and the rate converter's test tracks
//                         ("tone:1000@48000", "tone:silence@96000", ...:
//                         rateTests())
// Anything else, or a library id while the index isn't ready (a rebuild),
// is unknown: path() gives "", and the player skips it as a track that
// can't be played.
//
// A library track's names are the index's (docs/METADATA.md 5.4; N9): its
// tag's title, its artist (its record's, else its album's line, else its
// artist folder), its album's elected name, year and artist line, its
// record's length; a track without a record is named from its path, as
// before. One track may have fresher names than the index (an Overlay: the
// scan reads the playing track at once, 3.3.3), shown in place of the
// index's until the next build.
class TrackCatalog {
public:
  static constexpr uint32_t kNone = LibraryIndex::kNone;
  static constexpr uint32_t kBuiltin = 0x80000000u;
  // Long enough for any path the card walk passes on.
  static constexpr size_t kMaxPath = 256;

  // One track's names from its record, read after the index was built:
  // the title, the artists' display join, the album's value, the year and
  // the length (each cut to 255 bytes at a character boundary). A field the
  // record lacks ("", 0) stays the index's. Held by its owner (about 800
  // bytes: PSRAM); the catalog reads it while its index is the one it was
  // set for (the same buildStamp(): another build renumbers the tracks, and
  // reads the record itself).
  struct Overlay {
    static constexpr size_t kField = 256;
    uint32_t track = kNone;  // kNone: none
    uint64_t stamp = 0;      // the index's buildStamp() it was set for
    uint32_t version = 0;    // bumped by every set() and clear()
    char title[kField] = "";
    char artist[kField] = "";
    char album[kField] = "";
    uint16_t year = 0;
    uint32_t durationMs = 0;
    // From a record as the builder reads it (LibraryBuilder::viewOf()).
    void set(const LibraryIndex& index, uint32_t track, const LibraryIndex::TagView& tags);
    void clear();
  };

  explicit TrackCatalog(const LibraryIndex* index = nullptr) : index_(index) {}
  void setIndex(const LibraryIndex* index) { index_ = index; }
  const LibraryIndex* index() const { return index_; }
  // The overlay to read (nullptr: none); its owner keeps it alive.
  void setOverlay(const Overlay* overlay) { overlay_ = overlay; }
  // Changes whenever what a library track is called may have changed
  // without a rebuild (an overlay set or cleared): Now Playing draws its
  // names again.
  uint32_t namesVersion() const { return overlay_ ? overlay_->version : 0; }

  // The power test's silence (the console's Pz): zeros for an hour.
  static constexpr const char* kSilencePath = "tone:silence";

  // Every built-in track, the silence included.
  static uint32_t builtinCount();
  static bool isBuiltin(uint32_t id) { return id >= kBuiltin && id - kBuiltin < builtinCount(); }
  // The built-in tracks queued with the library (the tones, the click
  // tracks), in order (a Span, like the index's views): not the silence,
  // not the rate test tracks.
  static LibraryIndex::Span builtins();
  // The rate converter's test tracks (the console's Rt plays them on their
  // own, outside the queue; docs/RESAMPLER.md section 6): a 1 kHz tone and
  // silence made at other rates. Never queued with the others.
  static LibraryIndex::Span rateTests();

  bool valid(uint32_t id) const;
  // The path to play ("/music/.../06 - Title.mp3", "tone:click120"); the
  // length, or 0 (buf "") for an unknown id or a buffer too small.
  size_t path(uint32_t id, char* buf, size_t size) const;
  // The title to show: its tag's, else the file name's title part, or the
  // built-in's name (UTF-8, as on the card), cut to fit `buf` at a
  // character boundary. The length, or 0 for an unknown id.
  size_t title(uint32_t id, char* buf, size_t size) const;
  // The artist to show: its record's artist display, else its album's
  // line, else its artist folder (librarytext::trackArtist()); "built-in"
  // for a built-in track. "" when there's none or the id is unknown.
  const char* artist(uint32_t id) const;
  // Its album's name (its tags' elected name, else its folder's; "" for an
  // artist folder's loose tracks); "Built-in" for a built-in track.
  const char* album(uint32_t id) const;
  // Its album's artist line (librarytext::albumArtist()); "" for none.
  const char* albumArtist(uint32_t id) const;
  // Its album's year (or the overlay's); 0: none.
  uint16_t year(uint32_t id) const;
  // A hint for the backend: a built-in track's (the click tracks are 60 s),
  // or a library track's length as its record gave it (the overlay's, else
  // the index's whole seconds); 0: unknown.
  uint32_t durationHintMs(uint32_t id) const;
  // The id of `path` (the inverse of path()), or kNone.
  uint32_t find(const char* path) const;

private:
  bool inIndex(uint32_t id) const { return index_ && index_->ready() && id < index_->trackCount(); }
  // The overlay, when it names `id` in this index.
  const Overlay* overlayFor(uint32_t id) const;
  // `id` (in the index) is an artist folder's own track.
  bool loose(uint32_t id) const;

  const LibraryIndex* index_;
  const Overlay* overlay_ = nullptr;
};
