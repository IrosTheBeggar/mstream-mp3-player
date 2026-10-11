// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "UiStringIds.h"

// The UI's texts in its languages (docs/I18N.md, phase 3). The catalogue is
// i18n/<lang>.json, one file a language; tools/gen_ui_strings.py makes
// UiStringIds.h (an id a message, S::coach_title, and the languages) and
// UiStringsData.cpp (a table of offsets and one blob of UTF-8 a language,
// const, in flash). Portable, host-tested (test_ui_strings).
//
//   tr(S::x)          the message in the UI's language, else English
//   trn(S::x, n)      a plural message's form for the count n (CLDR's rules)
//   format(...)       {0}..{3} filled in, into the caller's buffer
//   trf(), trnf()     both at once
//   grouped(n), decimal(x, places)   numbers as the language writes them
//
// No heap anywhere: a message is a pointer into flash, a filled one goes
// into the caller's buffer, cut at a character boundary when it is too
// small (as snprintf, but never through a UTF-8 sequence).
//
// A message may hold line breaks ('\n') where its room is several lines
// that the drawing code places one by one (the coach cards, the empty
// states): line() gives each. Its {N} placeholders are the only special
// characters: '%' is a percent sign, as any other.
namespace uistr {

// ---- the UI's language ----

// The ten languages (Lang, English first, in the mobile app's order: en,
// de, es, fr, it, ja, pl, pt, ru, zh), each with its table; one with no
// catalogue yet draws English (hasCatalogue()).

// English until set (the boot sets it from NVS "ui"/"lang": app/UiLang).
void setLang(Lang l);
Lang lang();
// Its BCP-47 tag ("en", "pt", "en-XA") and its name in itself ("English",
// "Deutsch", "日本語": the Language setting's rows; never translated).
const char* tag(Lang l);
const char* autonym(Lang l);
// The language of a tag, false when it isn't one of the languages (a later
// firmware's, read after a downgrade).
bool langOf(const char* tag, Lang* out);
// Whether a language has a catalogue (i18n/<tag>.json) in this firmware.
bool hasCatalogue(Lang l);

// ---- messages ----

// The message in the UI's language, else in English (a message the
// language's catalogue doesn't have yet). A plural message: its "other"
// form.
const char* tr(S id);
// A plural message's form for `n` in the UI's language: CLDR's cardinal
// rule for its tag, the form "other" where the catalogue left one out
// ("many" in French at a million). A message that isn't plural: tr().
const char* trn(S id, uint32_t n);
// Whether a message is plural (has forms by count).
bool plural(S id);
// Whether the UI's language has its own text for a message (English
// always; a translation where its catalogue has it): what is said only in
// the language itself (language.machine_note) shows only then.
bool translated(S id);

// ---- plural rules (CLDR 46's cardinal rules, for whole numbers) ----

enum class Plural : uint8_t { Zero, One, Two, Few, Many, Other };
// The category of `n` in a language (a BCP-47 tag; an unknown one as
// English's): en, de "one" 1; es "one" 1, "many" a million's multiples; fr,
// pt "one" 0 and 1, "many" as es; it as es; pl "one" 1, "few" 2-4, 22-24
// (not 12-14), "many" the rest; ru "one" 1, 21, 31 (not 11), "few" as pl,
// "many" the rest; ja, zh "other" only.
Plural pluralOf(const char* tag, uint64_t n);
// The categories a language has, as a mask of 1 << Plural (its catalogue's
// forms, in Plural's order).
uint8_t pluralSet(const char* tag);

// ---- what fills a placeholder ----

// A string, or a number written as text: Arg(n) with its digits only
// ("19410"), grouped(n) and decimal() as the UI's language writes them.
// Holds a number's text itself (no heap): an Arg made from a string only
// points at it, which must outlive the format() call.
class Arg {
public:
  Arg() = default;
  Arg(const char* s) : s_(s ? s : "") {}  // NOLINT: implicit, as a printf %s
  Arg(int v) { setSigned(v, 0); }          // NOLINT
  Arg(long v) { setSigned(v, 0); }         // NOLINT
  Arg(long long v) { setSigned(v, 0); }    // NOLINT
  Arg(unsigned v) { setUnsigned(v, 0); }   // NOLINT
  Arg(unsigned long v) { setUnsigned(v, 0); }       // NOLINT
  Arg(unsigned long long v) { setUnsigned(v, 0); }  // NOLINT
  Arg(const Arg& o);
  Arg& operator=(const Arg& o);
  const char* str() const { return s_; }

  // The digits of `v`, at least `width` of them (zeros in front: "05").
  static Arg digits(long long v, int width);

private:
  friend Arg grouped(uint64_t n, const char* tag);
  friend Arg decimal(double v, int places, const char* tag);
  friend Arg fixed(long long scaled, int places, const char* tag);
  void setSigned(long long v, int width);
  void setUnsigned(unsigned long long v, int width);
  // 20 digits and 6 group marks of up to 2 bytes, a sign, the NUL. Only
  // a number writes it (a string's Arg doesn't touch it: each call site
  // makes its arguments, so no zeroing there).
  char buf_[36];
  const char* s_ = "";
};

// `n` with its thousands grouped as the UI's language does (CLDR 46):
// "19,410" en, ja, zh; "19.410" de, es, it, pt; "19 410" fr, pl, ru with a
// no-break space U+00A0 (French's is U+202F, which the fonts lack). es, it
// and pl leave four digits alone ("1234": CLDR's minimumGroupingDigits 2).
// `tag`: another language's way (a BCP-47 tag); nullptr: the UI's.
Arg grouped(uint64_t n, const char* tag = nullptr);
// `v` with `places` decimals (0-3), as printf's %.*f rounds it, with the
// language's decimal mark: "1.5" en, ja, zh; "1,5" the others.
Arg decimal(double v, int places, const char* tag = nullptr);
// A whole number of hundredths (or of 10^-places: 0-3) with the decimal
// mark: fixed(415, 2) is "4.15", fixed(378, 1) "37.8".
Arg fixed(long long scaled, int places, const char* tag = nullptr);

// ---- filling a message ----

// `pattern` with {0}..{3} replaced by the arguments (a {N} past them, and
// any other brace, is kept as written), into `out` (`size` > 0, always
// NUL-terminated). Returns the bytes written; a text too long for `out` is
// cut at a character boundary.
size_t formatArgs(char* out, size_t size, const char* pattern, const Arg* args, int count);
inline size_t format(char* out, size_t size, const char* pattern) {
  return formatArgs(out, size, pattern, nullptr, 0);
}
template <class... A>
size_t format(char* out, size_t size, const char* pattern, const A&... a) {
  static_assert(sizeof...(A) <= 4, "placeholders are {0} to {3}");
  const Arg args[] = {Arg(a)...};
  return formatArgs(out, size, pattern, args, static_cast<int>(sizeof...(A)));
}
// tr() and format() at once; trn() and format() (the count is not filled
// in by itself: pass it, grouped() or as digits, where the message has it).
template <class... A>
size_t trf(char* out, size_t size, S id, const A&... a) {
  return format(out, size, tr(id), a...);
}
template <class... A>
size_t trnf(char* out, size_t size, S id, uint32_t n, const A&... a) {
  return format(out, size, trn(id, n), a...);
}

// ---- a message's lines ----

// How many lines a text has ('\n' between them; 1 for "").
int lineCount(const char* text);
// Line `k` (from 0) of a text into `out` (`size` > 0); "" past its last.
// Returns its bytes (cut at a character boundary when `out` is short).
size_t line(const char* text, int k, char* out, size_t size);

namespace detail {
// One language's messages (UiStringsData.cpp, generated): `off[id]` is
// where message `id` starts in `blob`, kMissing where the catalogue hasn't
// it (English's then); `blob` and `off` are nullptr for a language with no
// catalogue yet. A plural message is its forms one after the other,
// each NUL-terminated, in Plural's order of the categories the language
// has; an empty one is a form the catalogue left out.
inline constexpr uint16_t kMissing = 0xFFFF;
struct Table {
  const char* tag;
  const char* autonym;
  const char* blob;
  const uint16_t* off;
};
extern const Table kTables[kLangs];
// A bit a message, set for a plural one (bit id % 8 of byte id / 8).
extern const uint8_t kPluralBits[(kStrings + 7) / 8];
}  // namespace detail

}  // namespace uistr
