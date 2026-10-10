// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for NameKey: mStream's nameKey as docs/METADATA.md 5.4 pins it
// (Rust's White_Space and full lowercase with Final_Sigma), orderName, the
// artist display join and DJRW's artistKey. Run: pio test -e native
#include <unity.h>

#include <cstring>
#include <string>

#include "CardContract.h"
#include "../support/CardFixtures.h"
#include "NameKey.h"

namespace {

std::string key(const char* s) {
  char out[512];
  const size_t n = namekey::nameKey(s, std::strlen(s), out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(n, std::strlen(out));
  return out;
}

std::string join(const char* list, size_t n) {
  char out[namekey::kDisplayMax + 1];
  const size_t len = namekey::displayJoin(list, n, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(len, std::strlen(out));
  return out;
}

std::string order(const char* name, const char* sort) {
  char out[512];
  namekey::orderName(name, std::strlen(name), sort, sort ? std::strlen(sort) : 0, out, sizeof(out));
  return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

// The tables are Rust's own (tools/unicode_case.rs), so the vectors were
// computed by a Rust nameKey: whitespace split on char::is_whitespace and
// joined with one space, the quotes and dashes folded, then
// str::to_lowercase.
void test_name_key_matches_rust() {
  struct V {
    const char* in;
    const char* want;
  };
  const V vs[] = {
      // ΣΟΦΙΑΣ: σοφιας
      {"\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x99\xCE\x91\xCE\xA3", "\xCF\x83\xCE\xBF\xCF\x86\xCE\xB9\xCE\xB1\xCF\x82"},
      {"  The  Lantern\x09" "Choir ", "the lantern choir"},
      {"Can\xE2\x80\x99t Stop", "can't stop"},
      {"T\xE2\x80\x90Pain \xE2\x80\x94 Live", "t-pain - live"},
      {"\xC4\xB0stanbul", "i\xCC\x87stanbul"},  // U+0130: two code points
      {"\xCE\xA3", "\xCF\x83"},                 // a lone sigma isn't final
      {"A\xCE\xA3", "a\xCF\x82"},
      {"A\xCE\xA3.", "a\xCF\x82."},             // '.' is case-ignorable, then the end
      {"A\xCE\xA3'B", "a\xCF\x83'b"},           // '\'' ignorable, then a cased letter
      {"A\xCE\xA3 B", "a\xCF\x82 b"},
      {"A\xCE\xA3\xCC\x81", "a\xCF\x82\xCC\x81"},
      {"\xE2\x85\xA0\xE2\x85\xAB", "\xE2\x85\xB0\xE2\x85\xBB"},  // Roman numerals are cased letters
      {"Caf\xC3\xA9 Trio", "caf\xC3\xA9 trio"},                 // no accent folding
      {"\xC3\x86SIR", "\xC3\xA6sir"},
      {"\xC7\x85", "\xC7\x86"},                                  // a titlecase digraph
      {"\xE1\x8E\xA0", "\xEA\xAD\xB0"},                          // Cherokee
      {"\xEF\xBB\xBFx", "\xEF\xBB\xBFx"},                        // U+FEFF isn't whitespace
      {"a\xE3\x80\x80" "b\xC2\xA0" "c", "a b c"},
      {"\xE1\xBA\x9E", "\xC3\x9F"},
      {"\xE2\x80\x9CQ\xE2\x80\x9D", "\x22q\x22"},
      {"A\xCE\xA3\xE2\x80\x99", "a\xCF\x82'"},
      {"A\xCE\xA3\xE2\x80\x9B" "B", "a\xCF\x83'b"},  // U+201B folds to an ignorable '\'' first
      {"\xCE\xA3\xCE\xA3\xCE\xA3 \xCE\xA3" "A\xCE\xA3", "\xCF\x83\xCF\x83\xCF\x82 \xCF\x83" "a\xCF\x82"},
      {"\xF0\x90\x90\x80", "\xF0\x90\x90\xA8"},  // Deseret, past the BMP
      {"", ""},
      {" \t\n", ""},
  };
  for (const V& v : vs) TEST_ASSERT_EQUAL_STRING_MESSAGE(v.want, key(v.in).c_str(), v.in);
  // The hash is the key's bytes' FNV-1a 64.
  for (const V& v : vs) {
    TEST_ASSERT_EQUAL_UINT64(cardcontract::fnv1a64Str(v.want), namekey::nameKeyHash(v.in, std::strlen(v.in)));
  }
}

void test_tables_answer_like_the_rules() {
  uint32_t out[3];
  TEST_ASSERT_EQUAL_UINT32(1, namekey::toLower('A', out));
  TEST_ASSERT_EQUAL_UINT32('a', out[0]);
  TEST_ASSERT_EQUAL_UINT32(1, namekey::toLower(0x0100, out));  // Ā -> ā: a step-2 run
  TEST_ASSERT_EQUAL_UINT32(0x0101, out[0]);
  TEST_ASSERT_EQUAL_UINT32(1, namekey::toLower(0x0101, out));
  TEST_ASSERT_EQUAL_UINT32(0x0101, out[0]);
  TEST_ASSERT_EQUAL_UINT32(2, namekey::toLower(0x0130, out));
  TEST_ASSERT_EQUAL_UINT32(0x0069, out[0]);
  TEST_ASSERT_EQUAL_UINT32(0x0307, out[1]);
  TEST_ASSERT_EQUAL_UINT32(1, namekey::toLower(0x212A, out));  // the Kelvin sign
  TEST_ASSERT_EQUAL_UINT32('k', out[0]);
  TEST_ASSERT_TRUE(namekey::isWhiteSpace(0x3000));
  TEST_ASSERT_TRUE(namekey::isWhiteSpace(0x85));
  TEST_ASSERT_FALSE(namekey::isWhiteSpace(0xFEFF));
  TEST_ASSERT_FALSE(namekey::isWhiteSpace(0x200B));
  TEST_ASSERT_EQUAL_UINT8(namekey::kCased, namekey::sigmaClass('a'));
  TEST_ASSERT_EQUAL_UINT8(namekey::kIgnorable, namekey::sigmaClass(0x0301));
  TEST_ASSERT_EQUAL_UINT8(namekey::kIgnorable, namekey::sigmaClass('\''));
  TEST_ASSERT_EQUAL_UINT8(namekey::kOther, namekey::sigmaClass(' '));
  TEST_ASSERT_EQUAL_UINT8(namekey::kOther, namekey::sigmaClass('1'));
  TEST_ASSERT_TRUE(namekey::tables::kUnicodeVersion[0] >= 15);
}

// Every code point: the compressed tables give what Rust's std gave the
// generator (its digest of each lowercase, White_Space bit and class).
void test_tables_cover_every_code_point() {
  uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&](uint8_t b) {
    h ^= b;
    h *= 0x100000001B3ull;
  };
  for (uint32_t c = 0; c <= 0x10FFFF; ++c) {
    if (c >= 0xD800 && c <= 0xDFFF) continue;
    uint32_t low[3];
    const uint32_t n = c == 0x03A3 ? (low[0] = 0x03C3, 1u) : namekey::toLower(c, low);
    for (uint32_t k = 0; k < n; ++k)
      for (int b = 0; b < 4; ++b) mix(static_cast<uint8_t>(low[k] >> (8 * b)));
    mix(namekey::isWhiteSpace(c) ? 1 : 0);
    mix(namekey::sigmaClass(c));
  }
  TEST_ASSERT_EQUAL_UINT64(namekey::tables::kRustDigest, h);
}

void test_key_cut_at_a_code_point() {
  char out[6];
  // "café trio" into 6 bytes: "caf" + "é" is 5 bytes, the space doesn't fit.
  const size_t n = namekey::nameKey("CAF\xC3\x89 TRIO", 10, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(10, n);
  TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9", out);
  char four[4];
  namekey::nameKey("ab\xC3\x89", 4, four, sizeof(four));  // "abé" needs 5 bytes: "ab"
  TEST_ASSERT_EQUAL_STRING("ab", four);
  // Invalid UTF-8 reads as U+FFFD, a byte at a time; no read past the end.
  TEST_ASSERT_EQUAL_STRING("a\xEF\xBF\xBD" "b", key("a\xC3" "b").c_str());
  TEST_ASSERT_EQUAL_STRING("a\xEF\xBF\xBD", key("a\xE2\x80").substr(0, 4).c_str());
}

void test_order_name() {
  TEST_ASSERT_EQUAL_STRING("lantern choir", order("The Lantern Choir", nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("ciel bleu", order("Le Ciel Bleu", nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("theory of rain", order("Theory of Rain", nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("the", order("The", nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("a paper kite", order("A Paper Kite", nullptr).c_str());  // "a" is no article
  // The sort tag when it has one; one of spaces alone doesn't count.
  TEST_ASSERT_EQUAL_STRING("choir, lantern", order("The Lantern Choir", "Choir, Lantern").c_str());
  TEST_ASSERT_EQUAL_STRING("lantern choir", order("The Lantern Choir", "  ").c_str());
  TEST_ASSERT_EQUAL_STRING("los lobos", order("Ignored", "Los Los Lobos").c_str());
}

// mStream's credit_display: trimmed, one value as is, several deduplicated
// by nameKey and joined with ", ".
void test_display_join() {
  const char a[] = "Lantern Choir";
  TEST_ASSERT_EQUAL_STRING("Lantern Choir", join(a, sizeof(a) - 1).c_str());
  const char b[] = "X\x1FX\x1FY";
  TEST_ASSERT_EQUAL_STRING("X, Y", join(b, sizeof(b) - 1).c_str());
  const char c[] = " Lantern Choir \x1Flantern  choir\x1FGuest Voice";
  TEST_ASSERT_EQUAL_STRING("Lantern Choir, Guest Voice", join(c, sizeof(c) - 1).c_str());
  const char d[] = "  \x1F\x1F" "A feat. B";  // empties (and a value of spaces) dropped
  TEST_ASSERT_EQUAL_STRING("A feat. B", join(d, sizeof(d) - 1).c_str());
  TEST_ASSERT_EQUAL_STRING("", join("", 0).c_str());
  // Four 204-byte values (2.3.6's limit) fit kDisplayMax.
  std::string list;
  for (char ch : {'P', 'Q', 'R', 'S'}) {
    if (!list.empty()) list += '\x1F';
    list += std::string(204, ch);
  }
  const std::string got = join(list.data(), list.size());
  TEST_ASSERT_EQUAL_size_t(4 * 204 + 3 * 2, got.size());
  // Too small a buffer: cut at a code point, the whole length still said.
  char small[5];
  const char e[] = "Ab\xC3\xA9" "cd";
  TEST_ASSERT_EQUAL_size_t(6, namekey::displayJoin(e, sizeof(e) - 1, small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("Ab\xC3\xA9", small);
  char tiny[4];
  namekey::displayJoin(e, sizeof(e) - 1, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("Ab", tiny);
}

// The shared fixture's AutoDJ table gives nameKey's outputs: each row's
// artistNameKey is nameKey of its artist, its songKey ends in nameKey of
// its title, and DJRW's artistKey is the low 32 bits of that key's hash.
void test_fixture_keys() {
  minijson::Value lib;
  TEST_ASSERT_TRUE(cardfixtures::json("libraries/autodj-00000029.json", &lib));
  const minijson::Value& rows = lib["rows"];
  TEST_ASSERT_TRUE(rows.size() >= 6);
  for (size_t i = 0; i < rows.size(); ++i) {
    const minijson::Value& r = rows[i];
    const std::string artist = r["artist"].str();
    TEST_ASSERT_EQUAL_STRING(r["artistNameKey"].c_str(), key(artist.c_str()).c_str());
    const std::string song = r["songKey"].str();
    const size_t bar = song.rfind('|');
    TEST_ASSERT_TRUE(bar != std::string::npos);
    TEST_ASSERT_EQUAL_STRING(song.substr(bar + 1).c_str(), key(r["title"].c_str()).c_str());
    const std::string left = song.substr(0, bar);
    TEST_ASSERT_EQUAL_STRING(left.c_str(), key(left.c_str()).c_str());  // a key already
    const uint32_t want = artist.empty() ? 0 : static_cast<uint32_t>(cardcontract::fnv1a64Str(r["artistNameKey"].c_str()));
    TEST_ASSERT_EQUAL_UINT32(want, namekey::artistKey(artist.data(), artist.size()));
  }
  // The row whose record lists "X", "X", "Y": its display is "X, Y".
  const char xy[] = "X\x1FY";
  TEST_ASSERT_EQUAL_STRING("x, y", key(join(xy, sizeof(xy) - 1).c_str()).c_str());
  // artistKey takes the first value only.
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cardcontract::fnv1a64Str("x")), namekey::artistKey(xy, sizeof(xy) - 1));
  TEST_ASSERT_EQUAL_UINT32(0, namekey::artistKey("  ", 2));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_name_key_matches_rust);
  RUN_TEST(test_tables_answer_like_the_rules);
  RUN_TEST(test_tables_cover_every_code_point);
  RUN_TEST(test_key_cut_at_a_code_point);
  RUN_TEST(test_order_name);
  RUN_TEST(test_display_join);
  RUN_TEST(test_fixture_keys);
  return UNITY_END();
}
