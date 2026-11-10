// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// The tagged synthetic library (lib/core/LibrarySynth's tagged()) as a card:
// its files, written as tags files (the transfer's T, source 2, or the
// device's D, source 1, with full records or with status rows only, and
// with mood tails or none), and its folders' other files for the builder.
// Host only: std containers.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "CardContainer.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibrarySynth.h"
#include "MoodFilter.h"

namespace synthcard {

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

// A growable sink for the writers.
class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > bytes.size()) bytes.resize(offset + n);
    if (n) std::memcpy(bytes.data() + offset, data, n);
    return true;
  }
  std::vector<uint8_t> bytes;
};

// One file: its path relative to /music and its record's values.
struct File {
  std::string rel;
  std::string title, artist, album, albumArtist, genre, artistSort, albumSort, albumArtistSort;
  uint16_t year = 0, track = 0, trackTotal = 0, disc = 0, discTotal = 0;
  uint32_t durationMs = 0, size = 0, fatTime = 0;
  uint8_t compilation = 0, container = 0;
  bool picture = false, jpeg = false, noTags = false;
  uint32_t album_ = 0;
  bool albumCover = false;
  uint8_t moods[mptg::kMoodTailBytes] = {};  // its mood tail (moodTail()): 8 zero bytes, no server row
};

// A file's mood tail (docs/MOOD-FILTERS.md 5.3), made up from its path
// alone: a server row for kMoodPct of the files (else 8 zero bytes), each
// head NULL in kMoodNullPct of the rows, every score near its folder's own
// (an album leans one way) and kept to 4 places, as mStream stores them
// (tools/synthtransfer.py's --moods defaults).
constexpr uint32_t kMoodPct = 92;
constexpr uint32_t kMoodNullPct = 1;
inline void moodTail(const std::string& rel, uint8_t out[mptg::kMoodTailBytes]) {
  auto draw = [](uint64_t* x) {  // splitmix64
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  auto unit = [&](uint64_t* x) { return static_cast<double>(draw(x) >> 11) / 9007199254740992.0; };
  uint64_t track = cc::fnv1a64(rel.data(), rel.size());
  if (draw(&track) % 100 >= kMoodPct) {
    moodfilter::tailOf(nullptr, out);
    return;
  }
  const size_t slash = rel.rfind('/');
  uint64_t folder = cc::fnv1a64(rel.data(), slash == std::string::npos ? 0 : slash);
  double scores[moodfilter::kHeads];
  for (int h = 0; h < moodfilter::kHeads; ++h) {
    const double centre = 0.1 + 0.8 * unit(&folder);
    const double x = std::min(1.0, std::max(0.0, centre + 0.4 * (unit(&track) - 0.5)));
    const bool null = draw(&track) % 100 < kMoodNullPct;
    scores[h] = null ? std::nan("") : std::round(x * 10000.0) / 10000.0;
  }
  moodfilter::tailOf(scores, out);
}
// A tail with a server row behind it (PRESENT): one T writes.
inline bool hasMoods(const File& f) { return (f.moods[0] & 1) != 0; }

struct Card {
  synth::Spec spec;
  std::vector<File> files;
  std::vector<std::string> albumFolders;  // per album, relative ("Artist/Album")
  std::vector<bool> albumCovers;          // per album: a cover.jpg in its folder
};

inline Card make(const synth::Spec& spec) {
  Card c;
  c.spec = spec;
  c.files.reserve(spec.tracks);
  synth::Tagged t;
  const size_t rootLen = std::strlen(spec.root) + 1;
  for (uint32_t i = 0; i < spec.tracks; ++i) {
    if (!synth::tagged(spec, i, &t)) continue;
    File f;
    f.rel = t.path + rootLen;
    f.title = t.title;
    f.artist = t.artist;
    f.album = t.album;
    f.albumArtist = t.albumArtist;
    f.genre = t.genre;
    f.artistSort = t.artistSort;
    f.albumSort = t.albumSort;
    f.albumArtistSort = t.albumArtistSort;
    f.year = t.year;
    f.track = t.track;
    f.trackTotal = t.trackTotal;
    f.disc = t.disc;
    f.discTotal = t.discTotal;
    f.durationMs = t.durationMs;
    f.size = t.size;
    f.fatTime = t.fatTime;
    f.compilation = t.compilation;
    f.container = t.container;
    f.picture = t.picture;
    f.jpeg = t.jpeg;
    f.noTags = t.noTags;
    f.album_ = t.albumIndex;
    f.albumCover = t.albumCover;
    moodTail(f.rel, f.moods);
    c.files.push_back(std::move(f));
  }
  char buf[320];
  for (uint32_t a = 0; a < spec.albums; ++a) {
    synth::albumFolder(spec, a, buf, sizeof(buf));
    c.albumFolders.push_back(buf);
    c.albumCovers.push_back(false);
  }
  for (const File& f : c.files) c.albumCovers[f.album_] = f.albumCover;
  return c;
}

// How a tags file describes the files.
struct Write {
  uint8_t source = mptg::kSourceTransfer;
  bool fullRecords = true;   // false: D's status rows, size and FAT time only
  bool ownedCovers = false;  // T: a record (container 255) for each album's cover.jpg
  bool thumbs = false;       // T: THUMB on each album folder
  int32_t shift = 0;         // seconds added to every recorded stamp (a PC's time zone)
  int32_t lengthMs = 0;      // ms added to every record's length (the device trims an MP3's encoder delay)
  uint32_t generation = 42;
  uint64_t cardId = 0x5EEDC0DE0A1B2C3Dull;
  uint16_t parserVersion = 1;
  // Full records: each file's mood tail when it has a server row (stride
  // 80, minor 1; 8 zero bytes for the rest). The software's T only, but
  // written for any source (the builder takes D's none).
  bool moods = false;
};

inline mptg::RecordIn recordOf(const File& f, bool full, int32_t shift, int32_t lengthMs = 0) {
  mptg::RecordIn r;
  r.path = f.rel.c_str();
  r.rec.size = f.size;
  r.rec.fatTime = shift ? cc::fatTimeFromWall(cc::fatWallSeconds(f.fatTime) + shift) : f.fatTime;
  r.rec.container = f.container;
  if (!full) return r;
  r.rec.durationMs = f.durationMs && static_cast<int64_t>(f.durationMs) + lengthMs > 0
                         ? static_cast<uint32_t>(static_cast<int64_t>(f.durationMs) + lengthMs)
                         : f.durationMs;
  r.rec.qfp = cc::fnv1a64(f.rel.data(), f.rel.size());  // a stand-in: no file bytes here
  r.rec.known = mptg::kKnownRules1;
  if (f.noTags) {
    r.rec.flags = mptg::kNoTags;
    return r;
  }
  r.rec.year = f.year;
  r.rec.track = f.track;
  r.rec.trackTotal = f.trackTotal;
  r.rec.disc = f.disc;
  r.rec.discTotal = f.discTotal;
  r.rec.flags = f.compilation;
  if (f.picture) {
    r.rec.picOffset = 1024;
    r.rec.picLength = 40000;
    r.rec.picType = 3;
    r.rec.picMime = f.jpeg ? mptg::kMimeJpeg : mptg::kMimePng;
  }
  r.fields[cc::kTitle] = f.title.c_str();
  r.fields[cc::kArtist] = f.artist.c_str();
  r.fields[cc::kAlbum] = f.album.c_str();
  r.fields[cc::kAlbumArtist] = f.albumArtist.c_str();
  r.fields[cc::kGenre] = f.genre.c_str();
  r.fields[cc::kArtistSort] = f.artistSort.c_str();
  r.fields[cc::kAlbumSort] = f.albumSort.c_str();
  r.fields[cc::kAlbumArtistSort] = f.albumArtistSort.c_str();
  return r;
}

inline std::vector<uint8_t> tagsFile(const Card& c, const Write& w) {
  std::vector<mptg::RecordIn> recs;
  recs.reserve(c.files.size() + c.albumFolders.size());
  bool tails = false;
  for (const File& f : c.files) {
    recs.push_back(recordOf(f, w.fullRecords, w.shift, w.lengthMs));
    if (w.moods && w.fullRecords && hasMoods(f)) {
      recs.back().moods = f.moods;
      tails = true;
    }
  }
  std::vector<std::string> covers;
  if (w.ownedCovers) {
    covers.reserve(c.albumFolders.size());
    for (size_t a = 0; a < c.albumFolders.size(); ++a) {
      if (!c.albumCovers[a]) continue;
      covers.push_back(c.albumFolders[a] + "/cover.jpg");
    }
    for (const std::string& p : covers) {
      mptg::RecordIn r;
      r.path = p.c_str();
      r.rec.size = 50000;
      r.rec.fatTime = 0x5D4773D5;
      r.rec.container = mptg::kContainerNotAudio;
      r.rec.qfp = cc::fnv1a64(p.data(), p.size());
      recs.push_back(r);
    }
  }
  std::vector<mptg::FolderIn> folders;
  if (w.thumbs) {
    for (const std::string& a : c.albumFolders) folders.push_back(mptg::FolderIn{a.c_str(), mptg::kFolderThumb});
  }
  mptg::Meta meta;
  meta.generation = w.generation;
  meta.cardId = w.cardId;
  meta.source = w.source;
  meta.parserVersion = w.parserVersion;
  meta.minor = tails ? 1 : 0;  // 2.4.5: minor 1 with mood tails
  meta.producer = w.source == mptg::kSourceDevice ? "mstream-player 0.8.0" : "mstream-terminal 0.13.0";
  VecSink out;
  const char* error = nullptr;
  if (!mptg::write(out, meta, recs.data(), recs.size(), folders.data(), folders.size(), nullptr, 0, nullptr, &error))
    return {};
  return out.bytes;
}

// The folders' other files: each album folder with a cover.jpg (owned when
// the transfer wrote it).
class Facts : public LibraryBuilder::FolderFactsSource {
public:
  Facts(const Card& c, bool owned) {
    for (size_t a = 0; a < c.albumFolders.size(); ++a) {
      if (!c.albumCovers[a]) continue;
      LibraryIndex::FolderFacts f;
      f.image = "cover.jpg";
      f.imageCount = 1;
      f.otherCount = 1;
      f.imageOwned = owned;
      map_[c.albumFolders[a]] = f;
    }
  }
  bool facts(const char* rel, size_t len, LibraryIndex::FolderFacts* out) override {
    ++asked;
    const auto it = map_.find(std::string(rel, len));
    if (it == map_.end()) return false;
    *out = it->second;
    return true;
  }
  uint32_t asked = 0;

private:
  std::map<std::string, LibraryIndex::FolderFacts> map_;
};

// D's statuses, one for every record; with ownCounts(), what DSTA's header
// would say of them (N4): every row Software, none of D its own.
class SameRows : public LibraryBuilder::DeviceRows {
public:
  explicit SameRows(LibraryBuilder::Status s, bool confirmed = false, bool counts = false) : counts_(counts) {
    row_.status = s;
    row_.confirmed = confirmed;
  }
  LibraryBuilder::Row row(uint32_t) override {
    ++asked;
    return row_;
  }
  bool ownCounts(uint32_t* records, uint32_t* folders) override {
    if (!counts_ || row_.status != LibraryBuilder::Status::Software) return false;
    *records = *folders = 0;
    return true;
  }
  uint32_t asked = 0;

private:
  LibraryBuilder::Row row_;
  bool counts_;
};

}  // namespace synthcard
