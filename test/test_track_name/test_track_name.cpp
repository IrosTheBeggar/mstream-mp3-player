// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for trackname (what a file name says: its disc, number and
// title, read with its folder's names). The corpus has the shapes measured
// on a real library of 19,371 file names, every name in it made up.
// Run: pio test -e native
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "TrackName.h"

void setUp() {}
void tearDown() {}

namespace {

struct Read {
  int disc, number;
  std::string title;
};

// A folder of stems (file names without their extension), read as the
// index reads them.
std::vector<Read> readFolder(const char* artist, const std::vector<const char*>& stems) {
  trackname::Folder f;
  for (const char* s : stems) f.add(s, std::strlen(s));
  std::vector<Read> out;
  for (const char* s : stems) {
    const trackname::Name n = f.read(s, std::strlen(s), artist);
    out.push_back({n.disc, n.number, std::string(s + n.titleAt)});
  }
  return out;
}

void expectRead(const Read& r, int disc, int number, const char* title) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(disc, r.disc, title);
  TEST_ASSERT_EQUAL_INT_MESSAGE(number, r.number, title);
  TEST_ASSERT_EQUAL_STRING(title, r.title.c_str());
}

// One name alone in its folder.
Read readOne(const char* artist, const char* stem) { return readFolder(artist, {stem})[0]; }

}  // namespace

// ---- the shapes on their own ----

void test_plain_numbers() {
  struct Case {
    const char* stem;
    bool has;
    int number;
    const char* title;
  } cases[] = {
      {"06 - Opening Act", true, 6, "Opening Act"},
      {"06. Opening Act", true, 6, "Opening Act"},
      {"06_Opening_Act", true, 6, "Opening_Act"},
      {"6 Opening Act", true, 6, "Opening Act"},
      {"006 - Opening Act", true, 6, "Opening Act"},
      {"255 Last One", true, 255, "Last One"},
      {"(03) - Glass Orchard - Opening", true, 3, "Glass Orchard - Opening"},
      {"[12] Opening", true, 12, "Opening"},
      {"06 - ", true, 6, "06 - "},  // a number, no title: the whole stem
      {"00 - Intro", true, 0, "Intro"},
      // No number: the whole stem.
      {"2001 A Space", false, 0, "2001 A Space"},  // four digits
      {"1999 - Party Song", false, 0, "1999 - Party Song"},
      {"256 Days", false, 0, "256 Days"},  // over 255
      {"06", false, 0, "06"},
      {"06Opening", false, 0, "06Opening"},
      {"(300) Opening", false, 0, "(300) Opening"},
      {"(3)Opening", false, 0, "(3)Opening"},
      {"(Intro) Opening", false, 0, "(Intro) Opening"},
      {"Opening Act", false, 0, "Opening Act"},
      {"", false, 0, ""},
  };
  for (const Case& c : cases) {
    trackname::Name n;
    TEST_ASSERT_EQUAL_MESSAGE(c.has, trackname::plain(c.stem, std::strlen(c.stem), &n), c.stem);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.number, n.number, c.stem);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n.disc, c.stem);
    TEST_ASSERT_EQUAL_STRING(c.title, c.stem + n.titleAt);
  }
}

void test_disc_track_shape() {
  struct Case {
    const char* stem;
    int disc, number;
    const char* title;
  } yes[] = {
      {"1-01 Opening", 1, 1, "Opening"},
      {"1-01. Opening", 1, 1, "Opening"},
      {"2-03 - Opening", 2, 3, "Opening"},
      {"2.04 - Opening (feat. Mira Lune)", 2, 4, "Opening (feat. Mira Lune)"},
      {"1-5. Opening", 1, 5, "Opening"},
      {"01-11 Opening", 1, 11, "Opening"},
      {"12-02 Opening", 12, 2, "Opening"},
      {"2-04 04-Opening", 2, 4, "04-Opening"},  // the folder rule drops the second 04
      {"06.2 Opening", 6, 2, "Opening"},       // on its own it looks like one: the folder decides
  };
  for (const Case& c : yes) {
    trackname::Name n;
    TEST_ASSERT_TRUE_MESSAGE(trackname::discTrack(c.stem, std::strlen(c.stem), &n), c.stem);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.disc, n.disc, c.stem);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.number, n.number, c.stem);
    TEST_ASSERT_EQUAL_STRING(c.title, c.stem + n.titleAt);
  }
  const char* no[] = {
      "1-800 Hotline",          // three digits after the dash
      "1-800-555-0199 Hotline",
      "01-404-paper_planes",    // a band named with digits, scene style
      "03-50,000 Voices",
      "0-01 Opening",           // disc 0
      "123-01 Opening",
      "1999 - Party Song",
      "1-01",                   // no title
      "1-01 - ",
      "1-01Opening",
      "1_01 Opening",
      "06 - Opening",
      "Opening 1-01",
  };
  for (const char* s : no) {
    trackname::Name n;
    TEST_ASSERT_FALSE_MESSAGE(trackname::discTrack(s, std::strlen(s), &n), s);
  }
}

void test_hundreds_shape() {
  trackname::Name n;
  TEST_ASSERT_TRUE(trackname::hundreds("101 Opening", 11, &n));
  TEST_ASSERT_EQUAL_INT(1, n.disc);
  TEST_ASSERT_EQUAL_INT(1, n.number);
  TEST_ASSERT_TRUE(trackname::hundreds("204-paper_planes", 16, &n));
  TEST_ASSERT_EQUAL_INT(2, n.disc);
  TEST_ASSERT_EQUAL_INT(4, n.number);
  TEST_ASSERT_EQUAL_STRING("paper_planes", "204-paper_planes" + n.titleAt);
  TEST_ASSERT_TRUE(trackname::hundreds("315 - Last", 10, &n));
  TEST_ASSERT_EQUAL_INT(3, n.disc);
  TEST_ASSERT_EQUAL_INT(15, n.number);
  const char* no[] = {"099 Opening", "1000 Opening", "101Opening", "101 ", "10 Opening", "Opening"};
  for (const char* s : no) TEST_ASSERT_FALSE_MESSAGE(trackname::hundreds(s, std::strlen(s), &n), s);
}

void test_prefixed_shape() {
  trackname::Prefixed p;
  const char* a = "Glass Orchard - 03 - Opening";
  TEST_ASSERT_TRUE(trackname::prefixed(a, std::strlen(a), &p));
  TEST_ASSERT_EQUAL_INT(3, p.name.number);
  TEST_ASSERT_EQUAL_INT(0, p.name.disc);
  TEST_ASSERT_EQUAL_STRING("Opening", a + p.name.titleAt);
  TEST_ASSERT_EQUAL_INT(13, p.prefixLen);
  TEST_ASSERT_EQUAL_INT(13, p.firstLen);
  TEST_ASSERT_TRUE(p.dash);

  const char* b = "Glass Orchard - Night Shift - 01 Opening";
  TEST_ASSERT_TRUE(trackname::prefixed(b, std::strlen(b), &p));
  TEST_ASSERT_EQUAL_INT(1, p.name.number);
  TEST_ASSERT_EQUAL_STRING("Opening", b + p.name.titleAt);
  TEST_ASSERT_EQUAL_INT(27, p.prefixLen);  // "Glass Orchard - Night Shift"
  TEST_ASSERT_EQUAL_INT(13, p.firstLen);   // "Glass Orchard"
  TEST_ASSERT_FALSE(p.dash);

  const char* c = "Disc 2 - 04 - Opening";
  TEST_ASSERT_TRUE(trackname::prefixed(c, std::strlen(c), &p));
  TEST_ASSERT_EQUAL_INT(2, p.name.disc);
  TEST_ASSERT_EQUAL_INT(4, p.name.number);
  for (const char* d : {"CD2 - 04 - Opening", "cd 2 - 04 - Opening", "DISK 2 - 04 - Opening"}) {
    TEST_ASSERT_TRUE(trackname::prefixed(d, std::strlen(d), &p));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, p.name.disc, d);
  }
  const char* e = "Glass Orchard - 1999 - Night Shift - 07 - Opening";  // the first " - " a number follows
  TEST_ASSERT_TRUE(trackname::prefixed(e, std::strlen(e), &p));
  TEST_ASSERT_EQUAL_INT(7, p.name.number);
  TEST_ASSERT_EQUAL_INT(0, p.name.disc);
  TEST_ASSERT_EQUAL_STRING("Opening", e + p.name.titleAt);

  const char* no[] = {
      "03 - Glass Orchard - Opening",  // starts with a digit: the plain rule's
      "Glass Orchard - Opening",
      "Glass Orchard - 1999 - Opening",  // four digits
      "Glass Orchard - 300 - Opening",   // over 255
      "Glass Orchard - 03",              // no title
      "Glass Orchard -03 - Opening",
      "Disc 12x - 01",
  };
  for (const char* s : no) TEST_ASSERT_FALSE_MESSAGE(trackname::prefixed(s, std::strlen(s), &p), s);
  // A disc part is the whole first part: "CD2 Live" isn't one.
  const char* f = "CD2 Live - 04 - Opening";
  TEST_ASSERT_TRUE(trackname::prefixed(f, std::strlen(f), &p));
  TEST_ASSERT_EQUAL_INT(0, p.name.disc);
}

void test_after_artist() {
  auto after = [](const char* title, const char* artist) {
    return std::string(title + trackname::afterArtist(title, std::strlen(title), artist));
  };
  TEST_ASSERT_EQUAL_STRING("Opening", after("Glass Orchard - Opening", "Glass Orchard").c_str());
  TEST_ASSERT_EQUAL_STRING("Opening", after("glass orchard - Opening", "Glass Orchard").c_str());
  TEST_ASSERT_EQUAL_STRING("Opening", after("The Glass Orchard - Opening", "Glass Orchard").c_str());
  TEST_ASSERT_EQUAL_STRING("Opening", after("Glass Orchard - Opening", "The Glass Orchard").c_str());
  TEST_ASSERT_EQUAL_STRING("Opening", after("R/K Unit - Opening", "R_K Unit").c_str());
  TEST_ASSERT_EQUAL_STRING("Opening", after("Émile Varga - Opening", "Emile Varga").c_str());
  TEST_ASSERT_EQUAL_STRING("Opening - Live", after("Glass Orchard - Opening - Live", "Glass Orchard").c_str());
  // A self-titled song keeps its name.
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", after("Glass Orchard - Glass Orchard", "Glass Orchard").c_str());
  // Left whole.
  const char* whole[] = {
      "Other Act - Opening",                  // someone else (a compilation's track)
      "Glass Orchard feat. Mira - Opening",   // not the same name
      "Glass Orchards - Opening",
      "Opening - Glass Orchard",              // the artist after the title
      "Glass Orchard - ",                     // nothing after it
      "Glass Orchard -Opening",
      "Glass Orchard",
  };
  for (const char* t : whole) TEST_ASSERT_EQUAL_STRING(t, after(t, "Glass Orchard").c_str());
  TEST_ASSERT_EQUAL_STRING("Glass Orchard - Opening", after("Glass Orchard - Opening", "").c_str());
  TEST_ASSERT_EQUAL_STRING("Glass Orchard - Opening", after("Glass Orchard - Opening", nullptr).c_str());
}

// ---- the folder rules ----

void test_disc_track_folder() {
  const auto r = readFolder("Glass Orchard", {"1-01 Opening", "1-02. Second", "2-01 - Return", "2.02 - Last",
                                              "Hidden Track"});
  expectRead(r[0], 1, 1, "Opening");
  expectRead(r[1], 1, 2, "Second");
  expectRead(r[2], 2, 1, "Return");
  expectRead(r[3], 2, 2, "Last");
  expectRead(r[4], 0, 0, "Hidden Track");  // no digit: not against the rule
  // The number written again after the disc-track one.
  const auto again = readFolder("Glass Orchard", {"2-04 04-Opening", "2-05 05-Second", "2-06 07-Third"});
  expectRead(again[0], 2, 4, "Opening");
  expectRead(again[1], 2, 5, "Second");
  expectRead(again[2], 2, 6, "07-Third");  // another number: the title's
  // Then the artist off the title too.
  const auto both = readFolder("Glass Orchard", {"2-12 Glass Orchard - Opening", "2-13 Glass Orchard - Second"});
  expectRead(both[0], 2, 12, "Opening");
}

void test_disc_track_needs_the_folder() {
  // Alone in its folder: the plain rule, as before.
  expectRead(readOne("Glass Orchard", "1-02 Lonely"), 0, 1, "02 Lonely");
  expectRead(readOne("Glass Orchard", "06.2 Opening"), 0, 6, "2 Opening");
  // Among names that aren't written so.
  const auto mixed = readFolder("Glass Orchard", {"01 Intro", "02 Second", "09-10 Live at the Hall"});
  expectRead(mixed[0], 0, 1, "Intro");
  expectRead(mixed[2], 0, 9, "10 Live at the Hall");
  // Numbers that aren't disc-track ones.
  const auto hotline = readFolder("Glass Orchard", {"1-800 Hotline", "1-801 Callback"});
  expectRead(hotline[0], 0, 1, "800 Hotline");
  expectRead(hotline[1], 0, 1, "801 Callback");
  const auto scene = readFolder("Glass Orchard", {"01-404-paper_planes", "02-404-night_shift"});
  expectRead(scene[0], 0, 1, "404-paper_planes");
  // Years stay in titles.
  const auto years = readFolder("Glass Orchard", {"1999 - Party Song", "2001 - Odyssey", "01 - 1984 Remix"});
  expectRead(years[0], 0, 0, "1999 - Party Song");
  expectRead(years[1], 0, 0, "2001 - Odyssey");
  expectRead(years[2], 0, 1, "1984 Remix");
}

void test_hundreds_folder() {
  const auto r = readFolder("Glass Orchard", {"101 - Opening", "102 - Second", "201 - Return", "202 - Last",
                                              "301 - Encore", "Hidden Track"});
  expectRead(r[0], 1, 1, "Opening");
  expectRead(r[1], 1, 2, "Second");
  expectRead(r[2], 2, 1, "Return");
  expectRead(r[3], 2, 2, "Last");
  expectRead(r[4], 3, 1, "Encore");  // 301: no number before (over 255)
  expectRead(r[5], 0, 0, "Hidden Track");
  // One disc written so: disc 1.
  const auto one = readFolder("Glass Orchard", {"100 - Intro", "101 - Opening", "102 - Second"});
  expectRead(one[0], 1, 0, "Intro");
  expectRead(one[2], 1, 2, "Second");
  // Numbers that don't start a disc: no disc-track numbers.
  const auto days = readFolder("Glass Orchard", {"365 Steps", "500 Stairs"});
  expectRead(days[0], 0, 0, "365 Steps");
  expectRead(days[1], 0, 0, "500 Stairs");
  const auto sixty = readFolder("Glass Orchard", {"160 Beats", "199 Steps"});
  expectRead(sixty[0], 0, 160, "Beats");  // the plain rule, as before
  // Among other numbers: the plain rule.
  const auto mixed = readFolder("Glass Orchard", {"01 Intro", "101 - Opening", "102 - Second"});
  expectRead(mixed[1], 0, 101, "Opening");
  // Alone: the plain rule.
  expectRead(readOne("Glass Orchard", "101 Dalmatian Steps"), 0, 101, "Dalmatian Steps");
}

void test_prefixed_folder() {
  // "Artist - NN - Title".
  const auto a = readFolder("Glass Orchard", {"Glass Orchard - 01 - Opening", "Glass Orchard - 02 - Second"});
  expectRead(a[0], 0, 1, "Opening");
  expectRead(a[1], 0, 2, "Second");
  // "Artist - Album - NN Title", a guest on one of them.
  const auto b = readFolder("Glass Orchard", {"Glass Orchard - Night Shift - 01 Opening",
                                              "Glass Orchard - Night Shift - 02 Second",
                                              "Glass Orchard feat. Mira Lune - Night Shift - 03 Third"});
  expectRead(b[0], 0, 1, "Opening");
  expectRead(b[2], 0, 3, "Third");
  // The folder's artist spelled another way.
  const auto c = readFolder("The Glass Orchard", {"glass orchard - 01 - Opening", "glass orchard - 02 - Second"});
  expectRead(c[1], 0, 2, "Second");
  // A disc part: the restarted numbers stay apart.
  const auto d = readFolder("Glass Orchard", {"CD1 - 01 - Opening", "CD1 - 02 - Second", "CD2 - 01 - Return"});
  expectRead(d[0], 1, 1, "Opening");
  expectRead(d[2], 2, 1, "Return");
  // Not the artist, but every name the same: the album, say.
  const auto e = readFolder("Glass Orchard", {"Night Shift - 01 - Opening", "Night Shift - 02 - Second"});
  expectRead(e[0], 0, 1, "Opening");
  // Then "NN - Artist - Title" after it.
  const auto f = readFolder("Glass Orchard", {"Night Shift - 01 - Glass Orchard - Opening",
                                              "Night Shift - 02 - Glass Orchard - Second"});
  expectRead(f[1], 0, 2, "Second");
  // At least half the folder, or a '-' after the number.
  const auto half = readFolder("Glass Orchard", {"Glass Orchard - 01 Opening", "Glass Orchard - 02 Second",
                                                 "Glass Orchard - Bonus Song", "Glass Orchard - Demo"});
  expectRead(half[0], 0, 1, "Opening");
  expectRead(half[2], 0, 0, "Bonus Song");  // the artist off it, no number
  expectRead(readOne("Glass Orchard", "Glass Orchard - 07 - Lonely"), 0, 7, "Lonely");
}

void test_prefixed_left_alone() {
  // A title that starts with a number, among "Artist - Title" names.
  const auto a = readFolder("Glass Orchard", {"Glass Orchard - First Song", "Glass Orchard - Second Song",
                                              "Glass Orchard - 99 Lanterns"});
  expectRead(a[0], 0, 0, "First Song");
  expectRead(a[2], 0, 0, "99 Lanterns");
  expectRead(readOne("Glass Orchard", "Glass Orchard - 99 Lanterns"), 0, 0, "99 Lanterns");
  // A compilation: each name its own artist. Left as it was.
  const auto va = readFolder("Various Artists", {"Act One - 01 - Song", "Act Two - 02 - Tune", "Act Three - 03 - Air"});
  expectRead(va[0], 0, 0, "Act One - 01 - Song");
  expectRead(va[2], 0, 0, "Act Three - 03 - Air");
}

void test_artist_off_titles() {
  const auto r = readFolder("Glass Orchard", {
                                                 "01 - Glass Orchard - Opening",
                                                 "02 glass orchard - Second",     // case
                                                 "03. The Glass Orchard - Third",  // "The"
                                                 "04 - Other Act - Fourth",       // someone else
                                                 "05 - Fifth - Live",             // "Title - Version"
                                                 "06 - Glass Orchard - ",         // nothing after it
                                                 "Glass Orchard - Seventh",       // no number
                                                 "(08) - Glass Orchard - Eighth",
                                             });
  expectRead(r[0], 0, 1, "Opening");
  expectRead(r[1], 0, 2, "Second");
  expectRead(r[2], 0, 3, "Third");
  expectRead(r[3], 0, 4, "Other Act - Fourth");
  expectRead(r[4], 0, 5, "Fifth - Live");
  expectRead(r[5], 0, 6, "Glass Orchard - ");
  expectRead(r[6], 0, 0, "Seventh");
  expectRead(r[7], 0, 8, "Eighth");
  // The punctuation a FAT name can't hold, and accents.
  expectRead(readOne("R_K Unit", "01 - R-K Unit - Opening"), 0, 1, "Opening");
  expectRead(readOne("Emile Varga", "01 - Émile Varga - Opening"), 0, 1, "Opening");
  // At the top of /music there's no artist folder: nothing comes off.
  expectRead(readOne("", "01 - Glass Orchard - Opening"), 0, 1, "Glass Orchard - Opening");
  // A compilation's "NN - Artist - Title": left whole.
  const auto va = readFolder("Various Artists", {"01 - Act One - Song", "02 - Act Two - Tune"});
  expectRead(va[0], 0, 1, "Act One - Song");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_plain_numbers);
  RUN_TEST(test_disc_track_shape);
  RUN_TEST(test_hundreds_shape);
  RUN_TEST(test_prefixed_shape);
  RUN_TEST(test_after_artist);
  RUN_TEST(test_disc_track_folder);
  RUN_TEST(test_disc_track_needs_the_folder);
  RUN_TEST(test_hundreds_folder);
  RUN_TEST(test_prefixed_folder);
  RUN_TEST(test_prefixed_left_alone);
  RUN_TEST(test_artist_off_titles);
  return UNITY_END();
}
