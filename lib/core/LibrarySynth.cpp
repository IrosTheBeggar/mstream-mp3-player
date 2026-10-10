// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibrarySynth.h"

#include <cstdio>
#include <cstring>

namespace synth {

namespace {

// 64 words, 2-11 bytes, a few with accents and typographic punctuation.
const char* const kWords[64] = {
    "Air",      "Blue",     "Crystal",  "Daft",       "Echo",    "Fire",     "Golden",   "Harbor",
    "Island",   "Jade",     "Kinetic",  "Lunar",      "Midnight", "Neon",    "Ocean",    "Paper",
    "Quiet",    "Rivers",   "Silver",   "Tides",      "Umbra",   "Velvet",   "Winter",   "Xenon",
    "Yellow",   "Zephyr",   "Pénélope", "Café",       "Can’t",   "Über",     "Déjà",     "Señor",
    "Ghost",    "Hollow",   "Iron",     "Jungle",     "Kings",   "Lights",   "Mirror",   "Northern",
    "Orbit",    "Pilot",    "Radio",    "Satellite",  "Thunder", "Urban",    "Vision",   "Wolves",
    "Arcade",   "Bloom",    "Cascade",  "Drift",      "Empire",  "Fever",    "Glass",    "Horizon",
    "Infinite", "Journey",  "Kaleido",  "Lantern",    "Machine", "Nocturne", "Outrun",   "Parade",
};

uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

const char* word(uint32_t i) { return kWords[i % 64]; }

void artistName(uint32_t a, uint32_t seed, char* out, size_t n) {
  const char* w1 = word(a % 64);
  const char* w2 = word(a / 64 % 64 + 17);
  char suffix[12] = "";
  if (a >= 4096) snprintf(suffix, sizeof(suffix), " %u", static_cast<unsigned>(a / 4096 + 1));
  switch (mix(a ^ seed) % 6) {
    case 0: snprintf(out, n, "%s %s%s", w1, w2, suffix); break;
    case 1: snprintf(out, n, "The %s %s%s", w1, w2, suffix); break;
    case 2: snprintf(out, n, "%s & the %s%s", w1, w2, suffix); break;
    case 3: snprintf(out, n, "%s of %s%s", w1, w2, suffix); break;
    case 4: snprintf(out, n, "%u %s %s%s", static_cast<unsigned>(10 + a % 90), w1, w2, suffix); break;
    default: snprintf(out, n, "%s %s Orchestra%s", w1, w2, suffix); break;
  }
}

void albumName(uint32_t artist, uint32_t j, uint32_t seed, char* out, size_t n) {
  const char* w1 = word((j % 64) * 7 + artist);
  const char* w2 = word(j / 64 % 64 + artist * 3);
  char suffix[12] = "";
  if (j >= 4096) snprintf(suffix, sizeof(suffix), " %u", static_cast<unsigned>(j / 4096 + 1));
  switch (mix(artist * 31 + j + seed) % 5) {
    case 0: snprintf(out, n, "%s %s%s", w1, w2, suffix); break;
    case 1: snprintf(out, n, "The %s of %s%s", w1, w2, suffix); break;
    case 2: snprintf(out, n, "%s %s (Deluxe Edition)%s", w1, w2, suffix); break;
    case 3: snprintf(out, n, "Selected %s Works %s 85-92%s", w1, w2, suffix); break;
    default: snprintf(out, n, "%s‐%s%s", w1, w2, suffix); break;
  }
}

void title(uint32_t t, uint32_t seed, char* out, size_t n) {
  uint32_t h = mix(t * 2654435761u + seed);
  const uint32_t words = 1 + h % 5;
  size_t len = 0;
  out[0] = 0;
  for (uint32_t k = 0; k < words && len + 1 < n; ++k) {
    h = mix(h + k);
    const int w = snprintf(out + len, n - len, "%s%s", k ? " " : "", word(h));
    if (w < 0) break;
    len += static_cast<size_t>(w);
  }
  if (h % 11 == 0 && len + 16 < n) snprintf(out + len, n - len, " Feat. T‐Pain");
}

}  // namespace

Spec userShape(uint32_t tracks) {
  Spec s;
  s.tracks = tracks;
  s.artists = tracks * 36 / 1000;
  s.albums = tracks * 92 / 1000;
  if (s.artists < 1) s.artists = 1;
  if (s.albums < s.artists) s.albums = s.artists;
  if (s.albums > tracks && tracks > 0) s.albums = tracks;
  if (s.artists > s.albums) s.artists = s.albums;
  return s;
}

Spec specFor(uint32_t tracks) {
  Spec s;
  s.tracks = tracks;
  s.artists = tracks * 6 / 100;
  s.albums = tracks * 15 / 100;
  if (s.artists < 1) s.artists = 1;
  if (s.albums < s.artists) s.albums = s.artists;
  if (s.albums > tracks && tracks > 0) s.albums = tracks;
  if (s.artists > s.albums) s.artists = s.albums;
  return s;
}

bool trackPath(const Spec& spec, uint32_t i, char* buf, uint32_t size) {
  if (spec.albums == 0 || spec.artists == 0) return false;
  const uint32_t album = i % spec.albums;
  const uint32_t artist = album % spec.artists;
  const uint32_t j = album / spec.artists;
  const uint32_t number = i / spec.albums + 1;
  char a[64], b[80], t[112];
  artistName(artist, spec.seed, a, sizeof(a));
  albumName(artist, j, spec.seed, b, sizeof(b));
  title(i, spec.seed, t, sizeof(t));
  const int n = snprintf(buf, size, "%s/%s/%s/%02u - %s.%s", spec.root, a, b, static_cast<unsigned>(number), t,
                         i % 5 == 4 ? "flac" : "mp3");
  return n > 0 && static_cast<uint32_t>(n) < size;
}

uint32_t addTracks(LibraryIndex& index, const Spec& spec) {
  char path[320];
  uint32_t added = 0;
  for (uint32_t i = 0; i < spec.tracks; ++i) {
    if (!trackPath(spec, i, path, sizeof(path))) continue;
    const LibraryIndex::Add r = index.addFile(path);
    if (r == LibraryIndex::Add::NoMemory) break;
    if (r == LibraryIndex::Add::Added) ++added;
  }
  return added;
}

// ---- tags ----

namespace {

uint32_t pct(uint32_t h) { return h % 100; }

// The tagged library's names, at the measured mean lengths (title 16.9
// bytes, album 16.6, artist 14.9: the research's database statistics):
// 1-4 words for a title, two words in one of five shapes for an album.
void taggedTitle(uint32_t t, uint32_t seed, char* out, size_t n) {
  uint32_t h = mix(t * 2654435761u + seed + 0x7A6);
  const uint32_t words = 1 + h % 4;
  size_t len = 0;
  out[0] = 0;
  for (uint32_t k = 0; k < words && len + 1 < n; ++k) {
    h = mix(h + k);
    const int w = snprintf(out + len, n - len, "%s%s", k ? " " : "", word(h));
    if (w < 0) break;
    len += static_cast<size_t>(w);
  }
}

void taggedAlbum(uint32_t artist, uint32_t j, uint32_t seed, char* out, size_t n) {
  const char* w1 = word((j % 64) * 7 + artist);
  const char* w2 = word(j / 64 % 64 + artist * 3);
  char suffix[12] = "";
  if (j >= 4096) snprintf(suffix, sizeof(suffix), " %u", static_cast<unsigned>(j / 4096 + 1));
  switch (mix(artist * 31 + j + seed) % 5) {
    case 0: snprintf(out, n, "%s %s%s", w1, w2, suffix); break;
    case 1: snprintf(out, n, "The %s %s%s", w1, w2, suffix); break;
    case 2: snprintf(out, n, "%s of %s%s", w1, w2, suffix); break;
    case 3: snprintf(out, n, "%s %s II%s", w1, w2, suffix); break;
    default: snprintf(out, n, "%s‐%s%s", w1, w2, suffix); break;
  }
}

// ASCII letters upper-cased, the rest as it is: the same name as
// textfold::sameName() sees it, written another way.
void upper(char* s) {
  for (; *s; ++s) {
    if (*s >= 'a' && *s <= 'z') *s = static_cast<char>(*s - 'a' + 'A');
  }
}

void lowerAscii(char* s) {
  for (; *s; ++s) {
    if (*s >= 'A' && *s <= 'Z') *s = static_cast<char>(*s - 'A' + 'a');
  }
}

// A valid FAT stamp (2.3.4) from a hash: 2005-2024, the 1st to the 28th.
uint32_t stamp(uint32_t h) {
  const uint32_t year = 2005 + h % 20, month = 1 + (h >> 5) % 12, day = 1 + (h >> 9) % 28;
  const uint32_t hour = (h >> 14) % 24, minute = (h >> 19) % 60, sec2 = (h >> 25) % 30;
  return (year - 1980) << 25 | month << 21 | day << 16 | hour << 11 | minute << 5 | sec2;
}

// What an album is: its artist, its place among the artist's albums, and
// the facts its tracks share.
struct AlbumFacts {
  uint32_t artist = 0, j = 0, h = 0;
  uint32_t tracks = 0;  // how many of the library's tracks are its
  uint16_t year = 0;
  bool albumArtist = false, compilation = false, twoDiscs = false, picture = false, jpeg = false, sort = false;
  bool cover = false;
};

AlbumFacts albumFacts(const Spec& spec, uint32_t album) {
  AlbumFacts a;
  a.artist = album % spec.artists;
  a.j = album / spec.artists;
  a.h = mix(album * 0x9E3779B1u + spec.seed * 0x85EBCA6Bu + 17);
  a.tracks = spec.tracks > album ? (spec.tracks - album + spec.albums - 1) / spec.albums : 0;
  a.year = static_cast<uint16_t>(1962 + a.h % 62);
  const uint32_t h2 = mix(a.h + 1), h3 = mix(a.h + 2), h4 = mix(a.h + 3);
  a.albumArtist = pct(h2) < kAlbumArtistPct;
  a.compilation = pct(h2 >> 8) < kCompilationPct;
  a.twoDiscs = pct(h3) < kMultiDiscPct && a.tracks >= 2;
  a.picture = pct(h3 >> 8) < kPicturePct;
  a.jpeg = (h3 >> 16) % 5 != 0;
  a.sort = pct(h4) < kSortPct;
  a.cover = (h4 >> 8) % 2 == 0;
  return a;
}

// The artist folder: the tag's artist name, or the same name written another
// way ("The" dropped, or upper case), or another name.
void artistFolderName(const Spec& spec, uint32_t artist, char* out, size_t n) {
  const uint32_t r = pct(mix(artist * 7919u + spec.seed + 3));
  if (r < kArtistOtherPct) {
    snprintf(out, n, "Unsorted %u", static_cast<unsigned>(artist));
    return;
  }
  artistName(artist, spec.seed, out, n);
  if (r < kArtistOtherPct + kArtistCasePct) {
    if (std::strncmp(out, "The ", 4) == 0) {
      std::memmove(out, out + 4, std::strlen(out + 4) + 1);
    } else {
      upper(out);
    }
  }
}

// The album folder: the album tag, with the year, with a suffix, another
// name, or upper case.
void albumFolderOf(const Spec& spec, const AlbumFacts& a, uint32_t album, char* out, size_t n) {
  char name[96];
  taggedAlbum(a.artist, a.j, spec.seed, name, sizeof(name));
  uint32_t r = pct(mix(album * 31337u + spec.seed + 5));
  if (r < kAlbumSamePct) {
    snprintf(out, n, "%s", name);
  } else if ((r -= kAlbumSamePct) < kAlbumYearPct) {
    if (r % 2) {
      snprintf(out, n, "%s (%u)", name, static_cast<unsigned>(a.year));
    } else {
      snprintf(out, n, "%u - %s", static_cast<unsigned>(a.year), name);
    }
  } else if ((r -= kAlbumYearPct) < kAlbumSuffixPct) {
    snprintf(out, n, r % 2 ? "%s [FLAC]" : "%s (Remastered)", name);
  } else if ((r -= kAlbumSuffixPct) < kAlbumOtherPct) {
    snprintf(out, n, "Untitled %u", static_cast<unsigned>(album));
  } else {
    snprintf(out, n, "%s", name);
    upper(out);
  }
}

}  // namespace

bool albumFolder(const Spec& spec, uint32_t album, char* buf, uint32_t size) {
  if (spec.albums == 0 || spec.artists == 0) return false;
  const AlbumFacts a = albumFacts(spec, album);
  char artist[80], al[128];
  artistFolderName(spec, a.artist, artist, sizeof(artist));
  albumFolderOf(spec, a, album, al, sizeof(al));
  const int k = snprintf(buf, size, "%s/%s", artist, al);
  return k > 0 && static_cast<uint32_t>(k) < size;
}

bool tagged(const Spec& spec, uint32_t i, Tagged* out) {
  if (spec.albums == 0 || spec.artists == 0) return false;
  Tagged& t = *out;
  t = Tagged{};
  const uint32_t album = i % spec.albums;
  const AlbumFacts a = albumFacts(spec, album);
  uint32_t number = i / spec.albums + 1;
  const uint32_t h = mix(i * 2246822519u + spec.seed * 3266489917u + 11);
  t.albumIndex = album;
  t.albumCover = a.cover;
  t.container = i % 5 == 4 ? 2 : 1;
  t.size = 1500000 + h % 9000000;
  t.fatTime = stamp(mix(h + 7));
  t.durationMs = 120000 + mix(h + 9) % 300000;

  // Names as the tags have them.
  char artist[80], titleText[112];
  artistName(a.artist, spec.seed, artist, sizeof(artist));
  taggedTitle(i, spec.seed, titleText, sizeof(titleText));
  char trackArtist[96];
  if (a.compilation) {
    artistName(spec.artists + mix(i + 3) % 997, spec.seed, trackArtist, sizeof(trackArtist));
  } else {
    snprintf(trackArtist, sizeof(trackArtist), "%s", artist);
  }

  // Discs: a two-disc album's second half in CD2, each disc from 1.
  uint16_t disc = 1, discTotal = 1;
  const char* discFolder = "";
  if (a.twoDiscs) {
    const uint32_t half = (a.tracks + 1) / 2;
    discTotal = 2;
    if (number > half) {
      disc = 2;
      number -= half;
      discFolder = "CD2/";
    } else {
      discFolder = "CD1/";
    }
  }

  // The file name, from the title as the measured shapes write it.
  char fileTitle[160], tagTitle[160];
  snprintf(fileTitle, sizeof(fileTitle), "%s", titleText);
  snprintf(tagTitle, sizeof(tagTitle), "%s", titleText);
  uint32_t r = pct(h >> 3);
  if (r < kTitleSamePct) {
    // the same
  } else if ((r -= kTitleSamePct) < kTitleArtistPct) {
    snprintf(fileTitle, sizeof(fileTitle), "%s - %s", trackArtist, titleText);
  } else if ((r -= kTitleArtistPct) < kTitleInsidePct) {
    snprintf(fileTitle, sizeof(fileTitle), "%s (Live)", titleText);
  } else if ((r -= kTitleInsidePct) < kTitleCasePct) {
    lowerAscii(fileTitle);
  } else if ((r -= kTitleCasePct) < kTitleLongerPct) {
    snprintf(tagTitle, sizeof(tagTitle), "%s (Remastered)", titleText);
  } else {
    snprintf(tagTitle, sizeof(tagTitle), "Another %s", titleText);
  }
  char fileName[200];
  const char* ext = t.container == 2 ? "flac" : "mp3";
  if (pct(h >> 11) < kNumberAgreesPct) {
    snprintf(fileName, sizeof(fileName), "%02u - %s.%s", static_cast<unsigned>(number), fileTitle, ext);
  } else {
    snprintf(fileName, sizeof(fileName), "%s.%s", fileTitle, ext);
  }
  char artistFolder[80], albumDir[128];
  artistFolderName(spec, a.artist, artistFolder, sizeof(artistFolder));
  albumFolderOf(spec, a, album, albumDir, sizeof(albumDir));
  const int k = snprintf(t.path, sizeof(t.path), "%s/%s/%s/%s%s", spec.root, artistFolder, albumDir, discFolder, fileName);
  if (k <= 0 || static_cast<size_t>(k) >= sizeof(t.path)) return false;

  // The record.
  if (h % 1000 < kNoTagsPerMille) {
    t.noTags = true;
    return true;
  }
  char albumTag[96];
  taggedAlbum(a.artist, a.j, spec.seed, albumTag, sizeof(albumTag));
  snprintf(t.title, sizeof(t.title), "%s", tagTitle);
  if (pct(h >> 17) < kMultiArtistPct) {
    char guest[80];
    artistName(spec.artists + 1000 + mix(h) % 499, spec.seed, guest, sizeof(guest));
    snprintf(t.artist, sizeof(t.artist), "%s\x1F%s", trackArtist, guest);
  } else {
    snprintf(t.artist, sizeof(t.artist), "%s", trackArtist);
  }
  snprintf(t.album, sizeof(t.album), "%s", albumTag);
  if (a.compilation) {
    snprintf(t.albumArtist, sizeof(t.albumArtist), "Various Artists");
    t.compilation = 1;
  } else if (a.albumArtist) {
    snprintf(t.albumArtist, sizeof(t.albumArtist), "%s", artist);
  }
  static const char* const kGenres[] = {"Rock", "Electronic", "Jazz", "Pop", "Ambient", "Folk", "Hip Hop", "Classical"};
  if (pct(h >> 21) < kGenrePct) snprintf(t.genre, sizeof(t.genre), "%s", kGenres[a.h % 8]);
  if (a.sort) {
    const char* space = std::strrchr(artist, ' ');
    if (space) {
      snprintf(t.artistSort, sizeof(t.artistSort), "%s, %.*s", space + 1, static_cast<int>(space - artist), artist);
      if (t.albumArtist[0] && !a.compilation) snprintf(t.albumArtistSort, sizeof(t.albumArtistSort), "%s", t.artistSort);
    }
    if (std::strncmp(albumTag, "The ", 4) == 0) snprintf(t.albumSort, sizeof(t.albumSort), "%s, The", albumTag + 4);
  }
  if (pct(h >> 5) < kYearPct) t.year = a.year;
  if (pct(h >> 7) < kTrackPct) {
    t.track = static_cast<uint16_t>(number);
    t.trackTotal = static_cast<uint16_t>(a.twoDiscs ? (a.tracks + 1) / 2 : a.tracks);
  }
  if (a.twoDiscs || pct(h >> 9) < kDiscPct) {
    t.disc = disc;
    t.discTotal = discTotal;
  }
  t.picture = a.picture;
  t.jpeg = a.picture && a.jpeg;
  return true;
}

}  // namespace synth
