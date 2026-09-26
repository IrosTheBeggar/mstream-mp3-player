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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_decode_utf8);
  RUN_TEST(test_sample_titles_punctuation);
  RUN_TEST(test_full_fold_accents);
  RUN_TEST(test_truncation_keeps_code_points_whole);
  RUN_TEST(test_compare_order);
  RUN_TEST(test_compare_is_a_total_order);
  RUN_TEST(test_rail_keys);
  return UNITY_END();
}
