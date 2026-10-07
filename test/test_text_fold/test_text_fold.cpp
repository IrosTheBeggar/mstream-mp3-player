// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for TextFold: the ASCII folding the GFX fonts need, and the
// library's sort order and A-Z keys. Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "TextFold.h"

using textfold::Mode;

namespace {
std::string folded(const char* in, Mode mode, textfold::Result* r = nullptr) {
  char out[128];
  const textfold::Result res = textfold::fold(in, out, sizeof(out), mode);
  if (r) *r = res;
  return out;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_decode_utf8() {
  const char* s = "a\xC3\xA9\xE2\x80\x99\xF0\x9F\x8E\xB5";
  TEST_ASSERT_EQUAL_UINT32('a', textfold::decode(s));
  TEST_ASSERT_EQUAL_UINT32(0xE9, textfold::decode(s));
  TEST_ASSERT_EQUAL_UINT32(0x2019, textfold::decode(s));
  TEST_ASSERT_EQUAL_UINT32(0x1F3B5, textfold::decode(s));
  TEST_ASSERT_EQUAL_UINT32(0, textfold::decode(s));
  // Malformed: a lone continuation byte, an overlong encoding.
  const char* bad = "\x80z\xC0\xAFy";
  TEST_ASSERT_EQUAL_UINT32(0xFFFD, textfold::decode(bad));
  TEST_ASSERT_EQUAL_UINT32('z', textfold::decode(bad));
  TEST_ASSERT_EQUAL_UINT32(0xFFFD, textfold::decode(bad));
  TEST_ASSERT_EQUAL_UINT32(0xFFFD, textfold::decode(bad));
  TEST_ASSERT_EQUAL_UINT32('y', textfold::decode(bad));
}

// The spike's sample titles.
void test_sample_titles_punctuation() {
  textfold::Result r;
  TEST_ASSERT_EQUAL_STRING("06 Can'T Tell Me Nothing", folded("06 Can’T Tell Me Nothing", Mode::Punctuation, &r).c_str());
  TEST_ASSERT_TRUE(r.ascii);
  TEST_ASSERT_TRUE(r.changed);
  TEST_ASSERT_EQUAL_STRING("05 Good Life Feat. T-Pain", folded("05 Good Life Feat. T‐Pain", Mode::Punctuation, &r).c_str());
  TEST_ASSERT_TRUE(r.ascii);
  // Accents stay in Punctuation mode: the row needs a Unicode font.
  TEST_ASSERT_EQUAL_STRING("10 Le voyage de Pénélope", folded("10 Le voyage de Pénélope", Mode::Punctuation, &r).c_str());
  TEST_ASSERT_FALSE(r.ascii);
  TEST_ASSERT_FALSE(r.changed);
  TEST_ASSERT_EQUAL_STRING("01 La demme d'argent", folded("01 La demme d'argent", Mode::Punctuation, &r).c_str());
  TEST_ASSERT_TRUE(r.ascii);
  TEST_ASSERT_FALSE(r.changed);
  TEST_ASSERT_EQUAL_STRING("Selected Ambient Works 85-92", folded("Selected Ambient Works 85-92", Mode::Full).c_str());
}

void test_full_fold_accents() {
  textfold::Result r;
  TEST_ASSERT_EQUAL_STRING("10 Le voyage de Penelope", folded("10 Le voyage de Pénélope", Mode::Full, &r).c_str());
  TEST_ASSERT_TRUE(r.ascii);
  TEST_ASSERT_EQUAL_UINT32(0, r.unknown);
  TEST_ASSERT_EQUAL_STRING("AEro Strasse Lodz OEuvre", folded("Ærø Straße Łódź Œuvre", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Senor Deja Uber Cafe", folded("Señor Déjà Über Café", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("\"Quoted\" - dash... (c) 1/2", folded("“Quoted” — dash… © ½", Mode::Full).c_str());
  // NBSP to space, soft hyphen and zero-width space dropped.
  TEST_ASSERT_EQUAL_STRING("a b cd", folded("a\xC2\xA0" "b c\xC2\xAD" "d\xE2\x80\x8B", Mode::Full).c_str());
  // CJK: '?' per code point, counted.
  TEST_ASSERT_EQUAL_STRING("?? x", folded("日本 x", Mode::Full, &r).c_str());
  TEST_ASSERT_EQUAL_UINT32(2, r.unknown);
  TEST_ASSERT_TRUE(r.ascii);
}

void test_truncation_keeps_code_points_whole() {
  char out[8];
  textfold::Result r = textfold::fold("abcdefgh", out, sizeof(out), Mode::Full);
  TEST_ASSERT_TRUE(r.truncated);
  TEST_ASSERT_EQUAL_STRING("abcdefg", out);
  // "abcde" + é (2 bytes) doesn't fit in 7: cut before it, never inside.
  r = textfold::fold("abcdeéz", out, 7, Mode::Punctuation);
  TEST_ASSERT_TRUE(r.truncated);
  TEST_ASSERT_EQUAL_STRING("abcde", out);
  // A multi-character replacement that doesn't fit is left out whole.
  r = textfold::fold("abcd…", out, 7, Mode::Full);
  TEST_ASSERT_EQUAL_STRING("abcd", out);
  TEST_ASSERT_TRUE(r.truncated);
  // Length-limited input.
  r = textfold::fold("Title.mp3", 5, out, sizeof(out), Mode::Full);
  TEST_ASSERT_EQUAL_STRING("Title", out);
  TEST_ASSERT_EQUAL_UINT32(5, r.length);
}

void test_compare_order() {
  // Case- and accent-insensitive.
  TEST_ASSERT_TRUE(textfold::compare("air", "Aphex Twin") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Émilie", "Emma") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Emma", "Ëmmy") < 0);
  TEST_ASSERT_TRUE(textfold::compare("daft punk", "Daft Punk") > 0);  // tie on folding: bytes decide
  TEST_ASSERT_TRUE(textfold::compare("Daft Punk", "Daft Punk") == 0);
  // Symbols before digits before letters.
  TEST_ASSERT_TRUE(textfold::compare("(What's the Story)", "808 State") < 0);
  TEST_ASSERT_TRUE(textfold::compare("808 State", "ABBA") < 0);
  TEST_ASSERT_TRUE(textfold::compare("~tilde", "0zero") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Zed", "~") > 0);
  // Prefix first.
  TEST_ASSERT_TRUE(textfold::compare("Can", "Can’t") < 0);
  TEST_ASSERT_TRUE(textfold::compare("", "a") < 0);
}

// std::sort needs a strict weak ordering: compare(a, b) == -compare(b, a) and
// transitive, for every printable ASCII character (symbols included: '`' and
// ' ', '{' and ';' once ranked alike).
void test_compare_is_a_total_order() {
  char a[16], b[16], c[16];
  for (int x = 32; x < 127; ++x) {
    for (int y = 32; y < 127; ++y) {
      std::snprintf(a, sizeof(a), "Live %c99", x);
      std::snprintf(b, sizeof(b), "Live %c99", y);
      const int ab = textfold::compare(a, b), ba = textfold::compare(b, a);
      TEST_ASSERT_EQUAL_INT_MESSAGE(-ab, ba, a);
      TEST_ASSERT_EQUAL_INT(x == y ? 0 : 1, ab == 0 ? 0 : 1);
    }
  }
  for (int x = 32; x < 127; ++x) {
    for (int y = 32; y < 127; ++y) {
      for (int z = 32; z < 127; ++z) {
        a[0] = static_cast<char>(x), b[0] = static_cast<char>(y), c[0] = static_cast<char>(z);
        a[1] = b[1] = c[1] = 0;
        if (textfold::compare(a, b) < 0 && textfold::compare(b, c) < 0) {
          TEST_ASSERT_TRUE(textfold::compare(a, c) < 0);
        }
      }
    }
  }
}

void test_rail_keys() {
  TEST_ASSERT_EQUAL_CHAR('A', textfold::railKey("air"));
  TEST_ASSERT_EQUAL_CHAR('E', textfold::railKey("Émilie"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::railKey("808 State"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::railKey("(What's)"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::railKey(""));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::railKey("日本"));
  TEST_ASSERT_EQUAL_INT(0, textfold::bucketOf('#'));
  TEST_ASSERT_EQUAL_INT(1, textfold::bucketOf('A'));
  TEST_ASSERT_EQUAL_INT(26, textfold::bucketOf('Z'));
}

// ---- names as the library sorts and matches them (made-up names) ----

void test_sort_name_drops_one_leading_article() {
  TEST_ASSERT_EQUAL_STRING("Lantern Choir", textfold::sortName("The Lantern Choir"));
  TEST_ASSERT_EQUAL_STRING("Lantern Choir", textfold::sortName("THE Lantern Choir"));
  TEST_ASSERT_EQUAL_STRING("Lantern Choir", textfold::sortName("the  Lantern Choir"));  // spaces after it too
  TEST_ASSERT_EQUAL_STRING("the", textfold::sortName("The the"));                       // one article only
  // mStream's other articles (its orderName): el, la, los, las, le, les.
  TEST_ASSERT_EQUAL_STRING("Ciel Bleu", textfold::sortName("Le Ciel Bleu"));
  TEST_ASSERT_EQUAL_STRING("Nuit", textfold::sortName("La Nuit"));
  TEST_ASSERT_EQUAL_STRING("Faros Lentos", textfold::sortName("Los Faros Lentos"));
  TEST_ASSERT_EQUAL_STRING("Olas", textfold::sortName("Las Olas"));
  TEST_ASSERT_EQUAL_STRING("Marins", textfold::sortName("Les Marins"));
  TEST_ASSERT_EQUAL_STRING("Sol Verde", textfold::sortName("El Sol Verde"));
  // Not an article, or nothing after it: the name as it is.
  const char* same[] = {"Theory of Rain", "The-Dream Machine", "Them Crooked", "The", "The ", "A Paper Moon",
                        "An Open Door", "Lesson Nine", "Elk Road", "Lattice", "", " The Lantern"};
  for (const char* s : same) TEST_ASSERT_EQUAL_PTR(s, textfold::sortName(s));
  TEST_ASSERT_NULL(textfold::sortName(nullptr));
}

void test_compare_sorted() {
  // "The Lantern Choir" sorts as "Lantern Choir": after "Lantern", before "Lattice".
  TEST_ASSERT_TRUE(textfold::compareSorted("Lantern", "The Lantern Choir") < 0);
  TEST_ASSERT_TRUE(textfold::compareSorted("The Lantern Choir", "Lattice") < 0);
  TEST_ASSERT_TRUE(textfold::compareSorted("Brass Arcade", "The Lantern Choir") < 0);
  TEST_ASSERT_TRUE(textfold::compareSorted("The Lantern Choir", "Theory of Rain") < 0);
  // Names that sort alike go by their whole names: total, never 0 for two names.
  TEST_ASSERT_TRUE(textfold::compareSorted("Pale Ferns", "The Pale Ferns") < 0);
  TEST_ASSERT_TRUE(textfold::compareSorted("The Pale Ferns", "Pale Ferns") > 0);
  TEST_ASSERT_EQUAL_INT(0, textfold::compareSorted("The Pale Ferns", "The Pale Ferns"));
  TEST_ASSERT_TRUE(textfold::compareSorted("Le Pale Ferns", "The Pale Ferns") != 0);
  // A strict weak order over a mix of them.
  const char* names[] = {"The Pale Ferns", "Pale Ferns", "Le Pale Ferns", "the pale ferns", "Pale Fernsby",
                         "The Pale Fernsby", "Theory", "The", "La", ""};
  for (const char* a : names) {
    TEST_ASSERT_EQUAL_INT(0, textfold::compareSorted(a, a));
    for (const char* b : names) {
      TEST_ASSERT_EQUAL_INT(-textfold::compareSorted(b, a), textfold::compareSorted(a, b));
      for (const char* c : names) {
        if (textfold::compareSorted(a, b) < 0 && textfold::compareSorted(b, c) < 0) {
          TEST_ASSERT_TRUE(textfold::compareSorted(a, c) < 0);
        }
      }
    }
  }
}

void test_same_name() {
  auto same = [](const char* a, const char* b) {
    return textfold::sameName(a, std::strlen(a), b, std::strlen(b));
  };
  TEST_ASSERT_TRUE(same("Glass Orchard", "Glass Orchard"));
  TEST_ASSERT_TRUE(same("glass orchard", "GLASS ORCHARD"));    // case
  TEST_ASSERT_TRUE(same("Émile Varga", "Emile Varga"));        // accents
  TEST_ASSERT_TRUE(same("R/K Unit", "R_K Unit"));              // what a FAT name can't hold
  TEST_ASSERT_TRUE(same("R/K Unit", "RK Unit"));
  TEST_ASSERT_TRUE(same("Mister E.", "Mister E"));
  TEST_ASSERT_TRUE(same("Søren Vale", "Soren Vale"));
  TEST_ASSERT_TRUE(same("The Lantern Choir", "Lantern Choir"));  // a leading "The", either side
  TEST_ASSERT_TRUE(same("Lantern Choir", "the Lantern Choir"));
  TEST_ASSERT_TRUE(same("The The", "The The"));
  TEST_ASSERT_FALSE(same("Glass Orchard", "Glass Orchards"));
  TEST_ASSERT_FALSE(same("Glass Orchard", "Glass"));
  TEST_ASSERT_FALSE(same("Brass & Bone", "Brass and Bone"));  // words aren't guessed
  TEST_ASSERT_FALSE(same("", ""));                            // nothing matches nothing
  TEST_ASSERT_FALSE(same("...", "..."));
  TEST_ASSERT_FALSE(same("日本", "日本"));                    // a script Full folding can't spell
  // The slice's length counts, not the NUL.
  const char* title = "Glass Orchard - Opening";
  TEST_ASSERT_TRUE(textfold::sameName(title, 13, "Glass Orchard", 13));
  TEST_ASSERT_TRUE(textfold::sameName(title, 15, "Glass Orchard", 13));  // " -": no letters
  TEST_ASSERT_FALSE(textfold::sameName(title, 23, "Glass Orchard", 13));
  // A UTF-8 sequence the slice cuts ends it.
  TEST_ASSERT_TRUE(textfold::sameName("Ab\xC3\xA9", 3, "ab", 2));
}

void test_starts_with_name() {
  auto starts = [](const char* s, const char* name) {
    return textfold::startsWithName(s, std::strlen(s), name, std::strlen(name));
  };
  TEST_ASSERT_TRUE(starts("Glass Orchard", "Glass Orchard"));
  TEST_ASSERT_TRUE(starts("Glass Orchard feat. Mira Lune", "Glass Orchard"));
  TEST_ASSERT_TRUE(starts("Glass Orchard & The Tide", "glass orchard"));
  TEST_ASSERT_TRUE(starts("The Glass Orchard ft Mira", "Glass Orchard"));
  TEST_ASSERT_TRUE(starts("R-K Unit x Mira", "R/K Unit"));
  TEST_ASSERT_FALSE(starts("Glass Orchards", "Glass Orchard"));  // the word runs on
  TEST_ASSERT_FALSE(starts("Glass", "Glass Orchard"));
  TEST_ASSERT_FALSE(starts("Other Act", "Glass Orchard"));
  TEST_ASSERT_FALSE(starts("Glass Orchard", ""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_decode_utf8);
  RUN_TEST(test_sample_titles_punctuation);
  RUN_TEST(test_full_fold_accents);
  RUN_TEST(test_truncation_keeps_code_points_whole);
  RUN_TEST(test_compare_order);
  RUN_TEST(test_compare_is_a_total_order);
  RUN_TEST(test_rail_keys);
  RUN_TEST(test_sort_name_drops_one_leading_article);
  RUN_TEST(test_compare_sorted);
  RUN_TEST(test_same_name);
  RUN_TEST(test_starts_with_name);
  return UNITY_END();
}
