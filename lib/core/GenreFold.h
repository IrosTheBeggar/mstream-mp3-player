// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Genre tags folded to a key (docs/DANCE-SPRITES.md, B.8), so a move tagged
// "Drum & Bass" matches a track whose genre reads "drum n bass" or
// "Drum 'n' Bass". tools/dancepack.py folds the same way when it compiles a
// dancer, and test/support/dance/FoldVectors.h pins both, key and hash.
//
// fold_genre(tag), in B.8's order:
//   1. Full folding: ASCII stays; any other code point becomes
//      textfold::replacement(cp, Mode::Full) (TextFold.h): "É" -> "E",
//      "ß" -> "ss", "½" -> "1/2", a typographic apostrophe -> "'", and "?"
//      for a script it can't spell.
//   2. A-Z lowercased.
//   3. Every '&' and '+' becomes " and ".
//   4. The words are the maximal runs of [a-z0-9]; everything else only
//      separates them and is dropped.
//   5. A word that is exactly "n", with a word before it and one after it,
//      becomes "and" ("rock'n'roll" -> rockandroll); "n" alone, or at
//      either end, stays.
//   6. The key is the words joined with nothing between them. An empty key
//      means the tag folds to nothing ("日本", spaces, ""): it has no key,
//      so FNV("") is never a genre key and such a tag can never match.
// The genre key is FNV-1a 32 (Fnv1a.h) of the key's bytes.
//
// It folds as a stream, one code point at a time, with one word of
// look-ahead for step 5 (a word that is exactly "n" waits for the next word,
// or for the tag's end): no buffer, so no limit on a tag's length. A pack's
// tags are valid UTF-8 (A.1); a malformed byte elsewhere (a track's genre,
// later) folds as textfold::decode reads it (U+FFFD, so "?"). Portable, no
// allocation.
namespace dance {

// B.8's folded key of a genre tag (UTF-8, NUL-terminated), written into
// `out` NUL-terminated and cut to cap - 1 bytes. Returns the whole key's
// length: 0 when the tag folds to nothing. A null tag folds to nothing;
// nothing is written when cap is 0 (out may then be null).
size_t foldGenre(const char* tag, char* out, size_t cap);
// B.8's genre key: FNV-1a 32 of the folded key, computed as a stream (no
// buffer, so no limit on the tag's length). False, *key untouched, when the
// tag folds to nothing. key may be null to ask only whether it folds to
// something.
bool genreKey(const char* tag, uint32_t* key);

}  // namespace dance
