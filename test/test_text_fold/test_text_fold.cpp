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
  return UNITY_END();
}
