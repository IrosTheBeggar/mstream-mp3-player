// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "LibraryIndex.h"

// A made-up library for measuring the index (and the lists drawn from it) at
// scales the SD card doesn't have: `tracks` files spread over `albums`
// albums of `artists` artists, as "/music/<Artist>/<Album>/NN - <Title>.mp3"
// (every 5th .flac), fed to LibraryIndex::addFile() between the caller's
// begin() and finish(). Deterministic for a seed. Names are built from a word
// list with realistic lengths (artists ~4-30 bytes, albums ~5-40, titles
// ~3-50) and a sprinkling of what real tags have: accents (Pénélope, Café),
// typographic apostrophes and hyphens (Can’t, T‐Pain), names starting with
// digits and "The ". Every artist and every album of an artist has a name of
// its own, so the index ends up with exactly `artists` artists and `albums`
// albums (when albums >= artists and tracks >= albums).
namespace synth {

struct Spec {
  uint32_t tracks = 10000;
  uint32_t artists = 600;
  uint32_t albums = 1500;
  uint32_t seed = 1;
  const char* root = "/music";
};

// The usual shape for n tracks: 6 artists and 15 albums per 100 tracks
// (10,000 -> 600 and 1,500), at least 1 of each.
Spec specFor(uint32_t tracks);

// Adds the files; returns how many addFile() accepted.
uint32_t addTracks(LibraryIndex& index, const Spec& spec);

// The i-th track's path (for tests), into buf; false if it didn't fit.
bool trackPath(const Spec& spec, uint32_t i, char* buf, uint32_t size);

}  // namespace synth
