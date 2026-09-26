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

}  // namespace synth
