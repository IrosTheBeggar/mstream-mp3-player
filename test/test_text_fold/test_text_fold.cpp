// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for TextFold: the ASCII folding the GFX fonts need, the
// composition the screen and the sort see, and the library's sort order and
// A-Z keys. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "TextFold.h"
#include "compose_cases.h"
#include "legacy_fold.h"

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
  TEST_ASSERT_TRUE(same("日本", "日本"));  // a script Full folding can't spell: by its code points
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

// ---- docs/I18N.md, phase 0: the folds, composition and the scripts' order ----

namespace {

// `in` through the composer, as UTF-8.
std::string composed(const char* in, const char* end = nullptr) {
  textfold::Composer c(in, end);
  std::string out;
  while (const uint32_t cp = c.next()) {
    char b[4];
    out.append(b, textfold::encode(cp, b));
  }
  return out;
}

bool same(const char* a, const char* b) { return textfold::sameName(a, std::strlen(a), b, std::strlen(b)); }

}  // namespace

// Latin-1 tags keep a cp1252 byte as U+0080-009F (docs/METADATA.md 5.2):
// folded and sorted as the character it stands for.
void test_c1_as_cp1252() {
  TEST_ASSERT_EQUAL_UINT32(0x2019, textfold::fromC1(0x92));
  TEST_ASSERT_EQUAL_UINT32(0x20AC, textfold::fromC1(0x80));
  TEST_ASSERT_EQUAL_UINT32(0x0160, textfold::fromC1(0x8A));
  TEST_ASSERT_EQUAL_UINT32(0x0178, textfold::fromC1(0x9F));
  TEST_ASSERT_EQUAL_UINT32(0x0081, textfold::fromC1(0x81));  // undefined in cp1252: itself
  TEST_ASSERT_EQUAL_UINT32(0x009D, textfold::fromC1(0x9D));
  TEST_ASSERT_EQUAL_UINT32(0x00E9, textfold::fromC1(0xE9));  // not C1: itself
  TEST_ASSERT_EQUAL_UINT32('A', textfold::fromC1('A'));
  TEST_ASSERT_EQUAL_STRING("Don't Stop", folded("Don\xC2\x92t Stop", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Don't Stop", folded("Don\xC2\x92t Stop", Mode::Punctuation).c_str());
  TEST_ASSERT_EQUAL_STRING("\"Live\" - EUR TM Sa",
                           folded("\xC2\x93Live\xC2\x94 \xC2\x96 \xC2\x80 \xC2\x99 \xC2\x8A" "a", Mode::Full).c_str());
  // The three no font of ours has (ƒ ˆ ˜) fold as those characters do.
  TEST_ASSERT_EQUAL_STRING("f^~", folded("\xC2\x83\xC2\x88\xC2\x98", Mode::Full).c_str());
  textfold::Result r;
  TEST_ASSERT_EQUAL_STRING("?", folded("\xC2\x81", Mode::Full, &r).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, r.unknown);
  // Sorted as the apostrophe: before '(' (as a '?' it came after), and
  // under Š's letter on the rail.
  TEST_ASSERT_TRUE(textfold::compare("Don\xC2\x92t", "Don(t") < 0);
  TEST_ASSERT_TRUE(legacy::compare("Don\xC2\x92t", "Don(t") > 0);
  TEST_ASSERT_EQUAL_CHAR('S', textfold::railKey("\xC2\x8A" "ostak"));
  TEST_ASSERT_EQUAL_CHAR('#', legacy::railKey("\xC2\x8A" "ostak"));
}

void test_fullwidth_ascii_folds() {
  TEST_ASSERT_EQUAL_STRING("ABC", folded("ＡＢＣ", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("foo! 12~", folded("ｆｏｏ！　１２～", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_CHAR('A', textfold::railKey("ＡＢＣ"));
  TEST_ASSERT_EQUAL_CHAR('b', textfold::secondKey("ＡＢＣ"));
  std::vector<std::string> v = {"ABD", "ＡＢＣ", "abb"};
  std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
    return textfold::compare(a.c_str(), b.c_str()) < 0;
  });
  TEST_ASSERT_EQUAL_STRING("abb", v[0].c_str());
  TEST_ASSERT_EQUAL_STRING("ＡＢＣ", v[1].c_str());
  TEST_ASSERT_EQUAL_STRING("ABD", v[2].c_str());
}

// Latin Extended-B and Extended Additional: Romanian, Vietnamese, the
// digraphs, Azerbaijani's schwa (its small one is IPA's).
void test_latin_extended_folds() {
  textfold::Result r;
  TEST_ASSERT_EQUAL_STRING("Stefan Banica", folded("Ștefan Bănică", Mode::Full, &r).c_str());
  TEST_ASSERT_EQUAL_UINT32(0, r.unknown);
  TEST_ASSERT_EQUAL_STRING("Tara", folded("Țara", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("My Tam", folded("Mỹ Tâm", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Son Tung M-TP", folded("Sơn Tùng M-TP", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Dang Thuy", folded("Đặng Thủy", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Dzaba dz", folded("ǅaba ǆ", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("SS AE f", folded("ẞ Ǽ ƒ", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Eli e", folded("Əli ə", Mode::Full).c_str());
  TEST_ASSERT_EQUAL_STRING("Ole", folded("Ǫ\xCC\x86le", Mode::Full).c_str());  // the mark after it dropped
  // No base letter: '?' as before; other scripts too.
  TEST_ASSERT_EQUAL_STRING("?", folded("ƍ", Mode::Full, &r).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, r.unknown);
  TEST_ASSERT_EQUAL_STRING("????", folded("Кино", Mode::Full).c_str());
  // The rail and the order: "Ștefan" is an S now (it was a '#').
  TEST_ASSERT_EQUAL_CHAR('S', textfold::railKey("Ștefan"));
  TEST_ASSERT_EQUAL_CHAR('#', legacy::railKey("Ștefan"));
  TEST_ASSERT_EQUAL_CHAR('O', textfold::railKey("Ơn"));
  TEST_ASSERT_TRUE(textfold::compare("Şerban", "Ștefan") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Ștefan", "Stefano") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Mỹ Tâm", "My Tank") < 0);
}

// What composition didn't put on a letter can't be drawn: dropped, not a
// '?' (a variation selector too).
void test_marks_and_selectors_drop() {
  textfold::Result r;
  TEST_ASSERT_EQUAL_STRING("x", folded("x\xCC\x81", Mode::Full, &r).c_str());
  TEST_ASSERT_EQUAL_UINT32(0, r.unknown);
  TEST_ASSERT_EQUAL_STRING("?", folded("\xE2\x9D\xA4\xEF\xB8\x8F", Mode::Full).c_str());  // ❤ + VS16
  TEST_ASSERT_EQUAL_STRING("e\xCC\x81", folded("e\xCC\x81", Mode::Punctuation).c_str());  // kept there
}

// The generator's random strings (tools/gen_text_tables.py), against
// Python's unicodedata.normalize("NFC", ...).
void test_compose_matches_unicodedata() {
  size_t n = 0;
  for (const composecases::Case& c : composecases::kCases) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.nfc, composed(c.in).c_str(), c.in);
    ++n;
  }
  TEST_ASSERT_EQUAL_UINT32(600, n);
}

void test_compose_examples() {
  TEST_ASSERT_EQUAL_STRING("Beyoncé", composed("Beyonce\xCC\x81").c_str());
  TEST_ASSERT_EQUAL_STRING("Một", composed("Mo\xCC\x82\xCC\xA3t").c_str());  // marks in either order
  TEST_ASSERT_EQUAL_STRING("Một", composed("Mo\xCC\xA3\xCC\x82t").c_str());
  TEST_ASSERT_EQUAL_STRING("한글", composed("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB\xE1\x84\x80\xE1\x85\xB3\xE1\x86\xAF").c_str());
  TEST_ASSERT_EQUAL_STRING("ガぱ", composed("\xE3\x82\xAB\xE3\x82\x99\xE3\x81\xAF\xE3\x82\x9A").c_str());
  TEST_ASSERT_EQUAL_STRING("йё", composed("\xD0\xB8\xCC\x86\xD0\xB5\xCC\x88").c_str());
  TEST_ASSERT_EQUAL_STRING("ά", composed("\xCE\xB1\xCC\x81").c_str());
  // A mark the letter can't take comes after it, in canonical order.
  TEST_ASSERT_EQUAL_STRING("á\xCC\x96", composed("a\xCC\x96\xCC\x81").c_str());
  TEST_ASSERT_EQUAL_STRING("á\xCC\x81", composed("a\xCC\x81\xCC\x81").c_str());  // the second is blocked
  TEST_ASSERT_EQUAL_STRING("ASCII stays", composed("ASCII stays").c_str());
  // Bounded: a mark past `end` isn't read, a sequence `end` cuts is U+FFFD.
  const char* s = "e\xCC\x81x";
  TEST_ASSERT_EQUAL_STRING("e", composed(s, s + 1).c_str());
  TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD", composed(s + 1, s + 2).c_str());
  // More marks than it tracks: none composed, none lost.
  std::string many = "a";
  for (int i = 0; i < 17; ++i) many += "\xCC\x81";
  TEST_ASSERT_EQUAL_STRING(many.c_str(), composed(many.c_str()).c_str());
  // The pair table's own lookups.
  TEST_ASSERT_EQUAL_UINT32(0x00E9, textfold::composePair('e', 0x0301));
  TEST_ASSERT_EQUAL_UINT32(0x1EC7, textfold::composePair(0x1EB9, 0x0302));  // ẹ + ̂ = ệ
  TEST_ASSERT_EQUAL_UINT32(0x01D6, textfold::composePair(0x00FC, 0x0304));  // ü + ̄ = ǖ
  TEST_ASSERT_EQUAL_UINT32(0x30AC, textfold::composePair(0x30AB, 0x3099));
  TEST_ASSERT_EQUAL_UINT32(0xD55C, textfold::composePair(0xD558, 0x11AB));  // 하 + ᆫ = 한
  TEST_ASSERT_EQUAL_UINT32(0, textfold::composePair('x', 0x0301));
  TEST_ASSERT_EQUAL_UINT32(0, textfold::composePair(0x0301, 0x0301));
  TEST_ASSERT_EQUAL_UINT32(0, textfold::composePair(0x4E00, 0x0301));
  TEST_ASSERT_EQUAL_INT(230, textfold::combiningClass(0x0301));
  TEST_ASSERT_EQUAL_INT(220, textfold::combiningClass(0x0323));
  TEST_ASSERT_EQUAL_INT(8, textfold::combiningClass(0x3099));
  TEST_ASSERT_EQUAL_INT(0, textfold::combiningClass('a'));
}

// What the generated tables say, every entry, against the generator's
// digest of it (as test_name_key checks NameKey's tables).
void test_tables_digest() {
  uint64_t h = 0xCBF29CE484222325ull;
  auto add = [&](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 0x100000001B3ull;
    }
  };
  for (uint32_t first = 0; first < 0x3100; ++first) {
    for (uint32_t m = 0; m < textfold::tables::kPairMarkCount; ++m) {
      add(textfold::composePair(first, textfold::tables::kPairMarks[m]), 4);
    }
  }
  for (uint32_t cp = 0x0300; cp < 0x0370; ++cp) add(textfold::combiningClass(cp), 1);
  for (uint32_t cp = 0x0483; cp < 0x0488; ++cp) add(textfold::combiningClass(cp), 1);
  add(textfold::combiningClass(0x3099), 1);
  add(textfold::combiningClass(0x309A), 1);
  auto fold = [&](uint32_t cp) {
    for (const char* p = textfold::replacement(cp, Mode::Full); *p; ++p) add(static_cast<unsigned char>(*p), 1);
    add(0, 1);
  };
  for (uint32_t cp = 0x0180; cp < 0x02B0; ++cp) fold(cp);
  for (uint32_t cp = 0x1E00; cp < 0x1F00; ++cp) fold(cp);
  for (uint32_t cp = 0x80; cp < 0xA0; ++cp) add(textfold::fromC1(cp), 2);
  TEST_ASSERT_EQUAL_UINT64(textfold::tables::kDigest, h);
}

// Letters Full folding can't spell: by script, then lower-case code point,
// after the digits and before the ASCII letters; no longer by length.
void test_script_order() {
  const char* order[] = {
      "(What's)",     // symbols
      "1999",         // digits
      "ʃa",           // IPA: Latin with no fold
      "Αθήνα",        // Greek
      "Ακρόπολη",
      "Аквариум",     // Cyrillic, case aside
      "Ая",
      "би-2",
      "Би-2 Live",
      "Яблоко",
      "Արամ",         // Armenian
      "עומר אדם",     // Hebrew
      "فيروز",        // Arabic
      "ธงไชย",        // Thai
      "방탄소년단",   // Hangul
      "아이유",
      "きゃりー",     // Hiragana
      "ヒカル",       // Katakana
      "周杰倫",       // Han
      "宇多田ヒカル",
      "लता मंगेशकर",  // Devanagari: another script
      "Abba",         // then the ASCII letters
      "Zebra",
  };
  const size_t n = sizeof(order) / sizeof(order[0]);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      const int c = textfold::compare(order[i], order[j]);
      char msg[128];
      std::snprintf(msg, sizeof(msg), "%s vs %s", order[i], order[j]);
      TEST_ASSERT_EQUAL_INT_MESSAGE(i < j ? -1 : i > j ? 1 : 0, c, msg);
    }
    TEST_ASSERT_EQUAL_CHAR_MESSAGE(i + 2 < n ? '#' : order[i][0], textfold::railKey(order[i]), order[i]);
  }
  // Not by length: the old order had "Ая" before "Аквариум" (2 '?' < 8).
  TEST_ASSERT_TRUE(legacy::compare("Ая", "Аквариум") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Ая", "Аквариум") > 0);
  // Case aside, ties by bytes; ς as σ; NFD as NFC.
  TEST_ASSERT_TRUE(textfold::compare("кино", "КИНОМАН") < 0);
  TEST_ASSERT_TRUE(textfold::compare("КИНО", "киноман") < 0);
  TEST_ASSERT_TRUE(textfold::compare("σοφιας x", "ΣΟΦΙΑΣ Y") < 0);
  TEST_ASSERT_TRUE(textfold::compare("σοφιας y", "ΣΟΦΙΑΣ X") > 0);
  const char* nfd = "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB\xE1\x84\x80\xE1\x85\xB3\xE1\x86\xAF";  // 한글
  TEST_ASSERT_TRUE(textfold::compare(nfd, "한국") > 0);
  TEST_ASSERT_TRUE(textfold::compare(nfd, "한기") < 0);
  TEST_ASSERT_TRUE(textfold::compare("Beyonce\xCC\x81 Live", "Beyoncé Kin") > 0);
  TEST_ASSERT_TRUE(textfold::Script::Greek == textfold::scriptOf(0x03B1));
  TEST_ASSERT_TRUE(textfold::Script::None == textfold::scriptOf(0x037E));  // ; the Greek question mark
  TEST_ASSERT_TRUE(textfold::Script::None == textfold::scriptOf(0x30FB));  // ・ katakana middle dot
  TEST_ASSERT_TRUE(textfold::Script::None == textfold::scriptOf(0x1F600));
  TEST_ASSERT_TRUE(textfold::Script::Han == textfold::scriptOf(0x20000));
  TEST_ASSERT_TRUE(textfold::Script::Hangul == textfold::scriptOf(0x3131));
  TEST_ASSERT_TRUE(textfold::Script::Katakana == textfold::scriptOf(0xFF76));
}

// std::sort's strict weak ordering over a mix of every kind, and the rail's
// buckets (and the jump grid's second level) in that order.
void test_mixed_order_is_total_and_bucketed() {
  std::vector<std::string> v = {"Кино", "КИНО", "кино", "Kino", "kino", "Ая", "Би-2", "Би/2", "1999", "1999 Кино",
                                "(x)", "★ Stars", "Ninja 🥷 Beats", "Ştefan", "Ștefan", "Stefan", "Mỹ Tâm", "My Tam",
                                "Beyonce\xCC\x81", "Beyoncé", "Beyonce", "ＡＢＣ", "ABC", "Don\xC2\x92t", "Don't",
                                "Don’t", "ガ", "\xE3\x82\xAB\xE3\x82\x99", "カ", "周杰倫", "", "Ἀθῆναι", "αθηναι",
                                "Æon", "Aeon", "Ka\xD0\xB8", "Kz", "E\xCC\x81mile", "Emile", "Emma"};
  for (const auto& a : v) {
    TEST_ASSERT_EQUAL_INT(0, textfold::compare(a.c_str(), a.c_str()));
    for (const auto& b : v) {
      const int ab = textfold::compare(a.c_str(), b.c_str());
      TEST_ASSERT_EQUAL_INT(-ab, textfold::compare(b.c_str(), a.c_str()));
      if (a != b) TEST_ASSERT_TRUE(ab != 0);
      for (const auto& c : v) {
        if (ab < 0 && textfold::compare(b.c_str(), c.c_str()) < 0) {
          TEST_ASSERT_TRUE(textfold::compare(a.c_str(), c.c_str()) < 0);
        }
      }
    }
  }
  std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
    return textfold::compare(a.c_str(), b.c_str()) < 0;
  });
  for (size_t i = 1; i < v.size(); ++i) {
    const int b0 = textfold::bucketOf(textfold::railKey(v[i - 1].c_str()));
    const int b1 = textfold::bucketOf(textfold::railKey(v[i].c_str()));
    TEST_ASSERT_TRUE_MESSAGE(b0 <= b1, v[i].c_str());
    if (b0 == b1 && b0 > 0) {  // a letter's ("Ka"...; '#' has symbols and digits first, as before)
      TEST_ASSERT_TRUE_MESSAGE(textfold::bucketOf(textfold::secondKey(v[i - 1].c_str())) <=
                                   textfold::bucketOf(textfold::secondKey(v[i].c_str())),
                               v[i].c_str());
    }
  }
  TEST_ASSERT_EQUAL_CHAR('m', textfold::secondKey("E\xCC\x81mile"));  // the mark isn't a character of its own
}

// sameName() and startsWithName() for names Full folding can't spell: their
// letters and digits by their lower-case code points (namekey's lowercase),
// so the artist election and the title's artist drop work for them too.
void test_same_name_other_scripts() {
  TEST_ASSERT_TRUE(same("Кино", "Кино"));
  TEST_ASSERT_TRUE(same("Кино", "КИНО"));
  TEST_ASSERT_TRUE(same("The Кино", "Кино"));
  TEST_ASSERT_TRUE(same("Би/2", "Би_2"));  // what a FAT name can't hold
  TEST_ASSERT_TRUE(same("ΣΟΦΙΑΣ", "σοφιας"));
  TEST_ASSERT_TRUE(same("周杰倫", "周杰倫"));
  TEST_ASSERT_TRUE(same("Beyonce\xCC\x81", "Beyoncé"));
  TEST_ASSERT_TRUE(same("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB", "한"));
  TEST_ASSERT_TRUE(same("Ștefan Bănică", "Stefan Banica"));  // folded, as Latin always was
  TEST_ASSERT_FALSE(same("Кино", "Kino"));  // another script is another name
  TEST_ASSERT_FALSE(same("Би-2", "Ая-2"));  // was the same ("2" alone)
  TEST_ASSERT_FALSE(same("Кино", "Киноман"));
  TEST_ASSERT_FALSE(same("周杰倫", "周杰伦"));
  TEST_ASSERT_FALSE(same("...", "..."));  // still: no letter or digit, no match
  TEST_ASSERT_FALSE(same("★", "★"));
  TEST_ASSERT_FALSE(same("", ""));
  auto starts = [](const char* s, const char* name) {
    return textfold::startsWithName(s, std::strlen(s), name, std::strlen(name));
  };
  TEST_ASSERT_TRUE(starts("Кино feat. Мумий Тролль", "Кино"));
  TEST_ASSERT_TRUE(starts("周杰倫 & 費玉清", "周杰倫"));
  TEST_ASSERT_FALSE(starts("Киноман", "Кино"));
  TEST_ASSERT_FALSE(starts("Кино", "Kino"));
}

// ---- the old order kept: N11's synthetic card's names ----

namespace {

std::string fixturesDir() {
  const char* tries[] = {"test/fixtures", "../test/fixtures", "../../test/fixtures"};
  for (const char* t : tries) {
    const std::string p = std::string(t) + "/synthcard/names.txt";
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
      std::fclose(f);
      return t;
    }
  }
  std::string self = __FILE__;
  for (int up = 0; up < 2; ++up) self = self.substr(0, self.find_last_of("/\\"));
  return self + "/fixtures";
}

std::vector<std::string> synthNames() {
  const std::string path = fixturesDir() + "/synthcard/names.txt";
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::vector<std::string> out;
  char line[1024];
  while (std::fgets(line, sizeof(line), f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty() || s[0] == '#') continue;
    out.push_back(s);
  }
  std::fclose(f);
  return out;
}

// Each 7th name again with one letter swapped for a Latin-1 or Extended-A
// one (synthcard's accents are Latin-1's few), or its '-' and '\'' for the
// typographic ones: characters the old folding spelled too.
void addVariants(std::vector<std::string>* names) {
  static const char* const kSwaps[][2] = {{"l", "ł"}, {"o", "ő"}, {"z", "ž"}, {"c", "č"}, {"e", "ę"}, {"s", "ś"},
                                          {"n", "ň"}, {"a", "å"}, {"u", "ű"}, {"i", "ı"}, {"-", "‐"}, {"'", "’"},
                                          {"S", "Š"}, {"T", "Ţ"}, {"D", "Đ"}, {"G", "Ğ"}};
  const size_t n = names->size();
  for (size_t i = 0; i < n; i += 7) {
    std::string s = (*names)[i];
    const auto& sw = kSwaps[(i / 7) % (sizeof(kSwaps) / sizeof(kSwaps[0]))];
    const size_t at = s.find(sw[0]);
    if (at == std::string::npos) continue;
    s.replace(at, std::strlen(sw[0]), sw[1]);
    names->push_back(s);
  }
}

template <typename Cmp>
std::vector<std::string> sortedBy(std::vector<std::string> v, Cmp cmp) {
  std::sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) { return cmp(a.c_str(), b.c_str()) < 0; });
  return v;
}

void assertSameOrder(const std::vector<std::string>& want, const std::vector<std::string>& got, const char* what) {
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) {
    if (want[i] != got[i]) {
      char msg[512];
      std::snprintf(msg, sizeof(msg), "%s: row %u was \"%s\", now \"%s\"", what, static_cast<unsigned>(i),
                    want[i].c_str(), got[i].c_str());
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

}  // namespace

// Names the old folding spelled (ASCII, Latin-1, Extended-A, the
// typographic punctuation) sort exactly as before, on their own and among
// the others, and keep their rail letters. Only the rest (other scripts)
// moves.
void test_latin_order_unchanged() {
  std::vector<std::string> all = synthNames();
  TEST_ASSERT_TRUE(all.size() > 4000);
  addVariants(&all);
  std::vector<std::string> latin;
  for (const auto& s : all) {
    if (legacy::oldSpells(s.c_str())) latin.push_back(s);
  }
  TEST_ASSERT_TRUE(latin.size() > 4500);
  TEST_ASSERT_TRUE(all.size() - latin.size() > 100);  // the made-up names in other scripts
  // The Artists and Albums lists' order (sort names), and the folders' and files'.
  const std::vector<std::string> before = sortedBy(latin, legacy::compareSorted);
  assertSameOrder(before, sortedBy(latin, textfold::compareSorted), "compareSorted");
  assertSameOrder(sortedBy(latin, legacy::compare), sortedBy(latin, textfold::compare), "compare");
  for (const auto& s : latin) {
    TEST_ASSERT_EQUAL_CHAR_MESSAGE(legacy::railKey(textfold::sortName(s.c_str())),
                                   textfold::railKey(textfold::sortName(s.c_str())), s.c_str());
  }
  // Among the other scripts' names: the same order between themselves.
  const std::vector<std::string> after = sortedBy(all, textfold::compareSorted);
  std::vector<std::string> mixed;
  for (const auto& s : after) {
    if (legacy::oldSpells(s.c_str())) mixed.push_back(s);
  }
  assertSameOrder(before, mixed, "among the others");
  // The others: under '#', each script's names together, in Script's order.
  int lastScript = 0;
  for (const auto& s : after) {
    const char* p = textfold::sortName(s.c_str());
    const uint32_t first = textfold::decode(p);
    const auto script = static_cast<int>(textfold::scriptOf(first));
    if (first < 0x80 || legacy::oldSpells(s.c_str()) || script == 0) continue;
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('#', textfold::railKey(textfold::sortName(s.c_str())), s.c_str());
    TEST_ASSERT_TRUE_MESSAGE(script >= lastScript, s.c_str());
    lastScript = script;
  }
  std::printf("[text_fold] %u names, %u the old folding spelled (their order kept), %u others\n",
              static_cast<unsigned>(all.size()), static_cast<unsigned>(latin.size()),
              static_cast<unsigned>(all.size() - latin.size()));
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
  RUN_TEST(test_c1_as_cp1252);
  RUN_TEST(test_fullwidth_ascii_folds);
  RUN_TEST(test_latin_extended_folds);
  RUN_TEST(test_marks_and_selectors_drop);
  RUN_TEST(test_compose_matches_unicodedata);
  RUN_TEST(test_compose_examples);
  RUN_TEST(test_tables_digest);
  RUN_TEST(test_script_order);
  RUN_TEST(test_mixed_order_is_total_and_bucketed);
  RUN_TEST(test_same_name_other_scripts);
  RUN_TEST(test_latin_order_unchanged);
  return UNITY_END();
}
