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
//                         "Built-in" pseudo-folder, always there, card or not
// Anything else, or a library id while the index isn't ready (a rebuild),
// is unknown: path() gives "", and the player skips it as a track that
// can't be played.
class TrackCatalog {
public:
  static constexpr uint32_t kNone = LibraryIndex::kNone;
  static constexpr uint32_t kBuiltin = 0x80000000u;
  // Long enough for any path the card walk passes on.
  static constexpr size_t kMaxPath = 256;

  explicit TrackCatalog(const LibraryIndex* index = nullptr) : index_(index) {}
  void setIndex(const LibraryIndex* index) { index_ = index; }
  const LibraryIndex* index() const { return index_; }

  static uint32_t builtinCount();
  static bool isBuiltin(uint32_t id) { return id >= kBuiltin && id - kBuiltin < builtinCount(); }
  // The built-in tracks' ids, in order (a Span, like the index's views).
  static LibraryIndex::Span builtins();

  bool valid(uint32_t id) const;
  // The path to play ("/music/.../06 - Title.mp3", "tone:click120"); the
  // length, or 0 (buf "") for an unknown id or a buffer too small.
  size_t path(uint32_t id, char* buf, size_t size) const;
  // The title to show: the file name's title part, or the built-in's name
  // (UTF-8, as on the card), cut to fit `buf` at a character boundary. The
  // length, or 0 for an unknown id.
  size_t title(uint32_t id, char* buf, size_t size) const;
  // The artist and album names ("" when there's none or the id is unknown).
  const char* artist(uint32_t id) const;
  const char* album(uint32_t id) const;
  // A hint for the backend (the click tracks are 60 s); 0: unknown.
  uint32_t durationHintMs(uint32_t id) const;
  // The id of `path` (the inverse of path()), or kNone.
  uint32_t find(const char* path) const;

private:
  bool inIndex(uint32_t id) const { return index_ && index_->ready() && id < index_->trackCount(); }

  const LibraryIndex* index_;
};
