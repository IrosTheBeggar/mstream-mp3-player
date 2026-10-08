// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the Library screens' portable pieces: the A-Z jump grid
// and its second level (JumpIndex, textfold::secondKey), a list's step a
// frame (ListLayout::stepToward), and every text the tab bar can show
// measured with the real fonts (the VLW DejaVu data the firmware draws
// with) against the room TabBarModel gives it.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CardContract.h"
#include "CardTags.h"
#include "IdlePolicy.h"
#include "JumpIndex.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "LibraryText.h"
#include "ListLayout.h"
#include "OutputModel.h"
#include "PowerChoices.h"
#include "QueueView.h"
#include "RateConverter.h"
#include "ScreenPower.h"
#include "SeekBar.h"
#include "SheetLayout.h"
#include "SleepTimer.h"
#include "TabBarModel.h"
#include "TagScan.h"
#include "TagText.h"
#include "TextFit.h"
#include "TextFold.h"
#include "TouchCalibration.h"
#include "TouchCheck.h"
#include "TrackCatalog.h"
#include "UiText.h"
#include "../support/TagFixtures.h"

// The fonts' data, as the firmware has it (src/ui/VlwFonts.cpp: arrays only).
#include "../../src/ui/VlwFonts.cpp"

void setUp() {}
void tearDown() {}

namespace {

// ---- a list sorted as the index sorts ----

std::vector<std::string> sorted(std::vector<std::string> names) {
  std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) { return textfold::compare(a.c_str(), b.c_str()) < 0; });
  return names;
}

const char* nameOf(void* ctx, uint32_t row) { return (*static_cast<std::vector<std::string>*>(ctx))[row].c_str(); }

int bucketOfRow(const std::vector<std::string>& v, uint32_t row) {
  return textfold::bucketOf(textfold::railKey(v[row].c_str()));
}

// ---- the VLW fonts' metrics (what ui/Fonts measures with) ----

struct Vlw {
  std::vector<std::pair<uint32_t, int>> advance;  // code point, xAdvance
  explicit Vlw(const uint8_t* d) {
    auto be32 = [&](size_t at) {
      return static_cast<uint32_t>(d[at]) << 24 | static_cast<uint32_t>(d[at + 1]) << 16 |
             static_cast<uint32_t>(d[at + 2]) << 8 | d[at + 3];
    };
    const uint32_t n = be32(0);
    for (uint32_t i = 0; i < n; ++i) {
      const size_t g = 24 + static_cast<size_t>(i) * 28;
      advance.push_back({be32(g), static_cast<int>(be32(g + 12))});
    }
  }
  // Every character has a glyph (width() counts a missing one as nothing;
  // the firmware draws it folded, and measures it as a space).
  bool hasAll(const char* s) const {
    for (const char* p = s; *p;) {
      const uint32_t cp = textfold::decode(p);
      bool found = false;
      for (const auto& a : advance) found = found || a.first == cp;
      if (!found) return false;
    }
    return true;
  }
  int width(const char* s) const {
    int w = 0;
    for (const char* p = s; *p;) {
      const uint32_t cp = textfold::decode(p);
      for (const auto& a : advance) {
        if (a.first == cp) {
          w += a.second;
          break;
        }
      }
    }
    return w;
  }
};

}  // namespace

// ---- JumpIndex ----

void test_jump_letters_match_the_index_buckets() {
  // The synthetic library at 10,000 tracks: 600 artists, their accents,
  // digits and "The " included. The rows' rail names are their sort names
  // (LibraryPage::railName(): "The X" under X), as the index sorts them.
  LibraryIndex idx;
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  std::vector<std::string> names;
  for (uint32_t i = 0; i < idx.artistCount(); ++i) {
    names.push_back(textfold::sortName(idx.artistName(idx.artistsAZ()[i])));
  }
  int32_t first[jump::kCells], end[jump::kCells];
  jump::letters(static_cast<uint32_t>(names.size()), nameOf, &names, first, end);
  for (int b = 0; b < jump::kCells; ++b) {
    const uint32_t start = idx.bucketStart(LibraryIndex::View::Artists, b);
    const uint32_t stop = idx.bucketStart(LibraryIndex::View::Artists, b + 1);
    if (start == stop) {
      TEST_ASSERT_EQUAL_INT32(-1, first[b]);
      continue;
    }
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(start), first[b]);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(stop), end[b]);
  }
}

void test_jump_second_level() {
  const std::vector<std::string> v = sorted({"Kanye West", "K-Os", "Kavinsky", "Kaiser Chiefs", "Keane", "Kings of Leon",
                                             "Klaxons", "Kraftwerk", "K7", "Kiss", "Kasabian", "Kéane Two", "Air",
                                             "Zola", "Kæthe"});
  auto vv = v;
  int32_t first[jump::kCells], end[jump::kCells];
  jump::letters(static_cast<uint32_t>(vv.size()), nameOf, &vv, first, end);
  const int k = textfold::bucketOf('K');
  TEST_ASSERT_TRUE(first[k] >= 0);
  int32_t second[jump::kCells];
  jump::seconds(static_cast<uint32_t>(first[k]), static_cast<uint32_t>(end[k]), nameOf, &vv, second);
  // Cell 0: the letter itself (its first row: "K-Os", "K7" sort first).
  TEST_ASSERT_EQUAL_INT32(first[k], second[0]);
  // Every cell's row is the first with that second letter; the rest none.
  for (int c = 1; c < jump::kCells; ++c) {
    const char want = static_cast<char>('a' + c - 1);
    int32_t brute = -1;
    for (int32_t r = first[k]; r < end[k]; ++r) {
      if (textfold::secondKey(vv[static_cast<size_t>(r)].c_str()) == want) {
        brute = r;
        break;
      }
    }
    TEST_ASSERT_EQUAL_INT32_MESSAGE(brute, second[c], vv[static_cast<size_t>(std::max(0, brute))].c_str());
  }
  TEST_ASSERT_TRUE(second[textfold::bucketOf('A')] >= 0);  // Kanye, Kaiser, Kavinsky, Kasabian, Kæthe
  TEST_ASSERT_TRUE(second[textfold::bucketOf('E')] >= 0);  // Keane, Kéane
  TEST_ASSERT_EQUAL_INT32(-1, second[textfold::bucketOf('B')]);
  // "Kæthe" folds to "kaethe": the first "Ka", before "Kaiser Chiefs".
  TEST_ASSERT_EQUAL_STRING("Kæthe", vv[static_cast<size_t>(second[1])].c_str());
}

void test_jump_second_key() {
  TEST_ASSERT_EQUAL_CHAR('a', textfold::secondKey("Kavinsky"));
  TEST_ASSERT_EQUAL_CHAR('a', textfold::secondKey("KANYE"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey("K-Os"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey("K7"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey("K"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey(""));
  TEST_ASSERT_EQUAL_CHAR('m', textfold::secondKey("Émilie Simon"));
  TEST_ASSERT_EQUAL_CHAR('e', textfold::secondKey("Æther"));  // Æ folds to "AE"
}

void test_jump_second_level_on_a_big_library() {
  // Brute force against the grid on every letter of 600 synthetic artists.
  LibraryIndex idx;
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  std::vector<std::string> names;
  for (uint32_t i = 0; i < idx.albumCount(); ++i) {
    names.push_back(textfold::sortName(idx.albumName(idx.albumsAZ()[i])));
  }
  int32_t first[jump::kCells], end[jump::kCells], second[jump::kCells];
  jump::letters(static_cast<uint32_t>(names.size()), nameOf, &names, first, end);
  int big = 0;
  for (int b = 0; b < jump::kCells; ++b) {
    if (first[b] < 0) continue;
    if (static_cast<uint32_t>(end[b] - first[b]) > jump::kSecondLevelRows) ++big;
    jump::seconds(static_cast<uint32_t>(first[b]), static_cast<uint32_t>(end[b]), nameOf, &names, second);
    for (int c = 1; c < jump::kCells; ++c) {
      int32_t brute = -1;
      for (int32_t r = first[b]; r < end[b]; ++r) {
        if (textfold::bucketOf(textfold::secondKey(names[static_cast<size_t>(r)].c_str())) == c) {
          brute = r;
          break;
        }
      }
      TEST_ASSERT_EQUAL_INT32(brute, second[c]);
    }
    for (int32_t r = first[b]; r < end[b]; ++r) TEST_ASSERT_EQUAL_INT(b, bucketOfRow(names, static_cast<uint32_t>(r)));
  }
  TEST_ASSERT_TRUE(big > 5);  // 1,500 albums: most letters get the second level
}

void test_jump_grid_cells() {
  // 7 x 4 cells of 44 x 41 from (6, 74): '#' A-F, G-M, N-T, U-Z and the last.
  TEST_ASSERT_EQUAL_INT(0, jump::cellAt(6, 74, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(6, jump::cellAt(313, 74, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(7, jump::cellAt(6, 115, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(27, jump::cellAt(300, 237, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(-1, jump::cellAt(5, 100, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(-1, jump::cellAt(100, 238, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(-1, jump::cellAt(314, 100, 6, 74, 44, 41));
}

// ---- a list's step a frame ----

void test_list_steps_toward_the_finger() {
  // A finger faster than a step a frame: the list trails it by a step...
  TEST_ASSERT_EQUAL_INT32(184, ListLayout::stepToward(100, 400, 84));
  TEST_ASSERT_EQUAL_INT32(16, ListLayout::stepToward(100, -50, 84));
  // ... and is there once it's within one.
  TEST_ASSERT_EQUAL_INT32(150, ListLayout::stepToward(100, 150, 84));
  TEST_ASSERT_EQUAL_INT32(16, ListLayout::stepToward(100, 16, 84));
  // A fling of 2,000 px/s at 30 fps: under a step, never capped.
  int32_t drawn = 0, finger = 0;
  for (int f = 0; f < 30; ++f) {
    finger += 2000 / 30;
    drawn = ListLayout::stepToward(drawn, finger, 84);
    TEST_ASSERT_EQUAL_INT32(finger, drawn);
  }
  // 4,000 px/s for 10 frames: behind by at most what it moved over the
  // cap, and caught up within two frames of the finger stopping.
  drawn = finger = 0;
  for (int f = 0; f < 10; ++f) {
    finger += 4000 / 30;
    drawn = ListLayout::stepToward(drawn, finger, 84);
  }
  TEST_ASSERT_EQUAL_INT32(840, drawn);
  int frames = 0;
  while (drawn != finger) {
    drawn = ListLayout::stepToward(drawn, finger, 84);
    ++frames;
  }
  TEST_ASSERT_TRUE(frames <= 7);
}

// In whole rows (the governor, the audio short of time), from an offset
// that isn't on a row: the step never grows past the cap (it used to round
// after capping: 167 -> 83 -> 42, a 125-line step, a full redraw).
void test_list_steps_in_whole_rows_never_exceed_the_cap() {
  const int32_t pitch = ListLayout::kPitch, cap = 84;
  TEST_ASSERT_EQUAL_INT32(84, ListLayout::stepTowardRows(167, 0, cap, pitch));
  for (int32_t drawn = 0; drawn < 2000; drawn += 7) {
    for (int32_t target = 0; target < 2000; target += 13) {
      const int32_t off = ListLayout::stepTowardRows(drawn, target, cap, pitch);
      const int32_t step = off > drawn ? off - drawn : drawn - off;
      TEST_ASSERT_TRUE(step <= cap);
      // Toward the target, never past its row; on a whole row.
      const int32_t t = target / pitch * pitch;
      TEST_ASSERT_EQUAL_INT32(0, off % pitch);
      if (t >= drawn) {
        TEST_ASSERT_TRUE(off <= t);
      } else {
        TEST_ASSERT_TRUE(off >= t);
      }
      // It gets there: every step moves unless it's there already.
      if (off == drawn) TEST_ASSERT_EQUAL_INT32(t, off);
    }
  }
}

// ---- the tab bar's texts ----

void test_tabbar_texts_fit_in_every_state() {
  const Vlw small(kVlwSans13);
  // The active tab's label on its plate, every tab.
  for (int t = 0; t < tabbar::kTabs; ++t) {
    TEST_ASSERT_TRUE_MESSAGE(small.width(tabbar::kLabels[t]) <= tabbar::labelWidth(t), tabbar::kLabels[t]);
  }
  // The volume beside the output icon and the battery under its icon, at
  // every level (0-100 %).
  char t[8];
  for (int v = 0; v <= 100; ++v) {
    snprintf(t, sizeof(t), "%d%%", v);
    TEST_ASSERT_TRUE_MESSAGE(small.width(t) <= tabbar::kVolumeW, t);
    TEST_ASSERT_TRUE_MESSAGE(small.width(t) <= tabbar::kBatteryTextW, t);
  }
  // The volume ends before the battery icon starts.
  TEST_ASSERT_TRUE(tabbar::kVolumeX + tabbar::kVolumeW <= tabbar::kBatteryX + 2);
  // The battery's text inside the cell (centred under the icon).
  const int bx = tabbar::kBatteryX + 12;
  TEST_ASSERT_TRUE(bx + tabbar::kBatteryTextW / 2 <= tabbar::cellX1(tabbar::kTabs - 1) - tabbar::kOutputX + 1);
  // The Queue's badge, every count: inside its cell.
  for (uint32_t n = 0; n < 200; ++n) {
    char b[4];
    tabbar::badgeText(n, b);
    const int bw = small.width(b) + tabbar::kBadgePad;
    const int left = tabbar::kTabW / 2 + tabbar::kBadgeRight - bw;
    TEST_ASSERT_TRUE_MESSAGE(left >= 0 && tabbar::kTabW / 2 + tabbar::kBadgeRight <= tabbar::kTabW, b);
  }
}

// ---- the other fixed texts (UiText: each next to its room) ----

namespace {
void fits(const Vlw& font, const char* text, int room) {
  char msg[160];
  snprintf(msg, sizeof(msg), "\"%s\": %d px in %d", text, font.width(text), room);
  TEST_ASSERT_TRUE_MESSAGE(font.width(text) <= room, msg);
}
}  // namespace

// The first-boot tips: the first screen a new user sees.
void test_coach_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, kCoachTitle, kCoachTextW);
  fits(small, kCoachLine, kCoachTextW);
  TEST_ASSERT_TRUE(3 * kCoachBoxW + 2 * 4 <= 320);
  for (int b = 0; b < 3; ++b) {
    fits(bold, kCoachClick[b], kCoachBoxTextW);
    fits(small, kCoachHold[b], kCoachBoxTextW);
    fits(small, kCoachHold2[b], kCoachBoxTextW);
  }
  for (const char* t : kCoachTab) fits(title, t, kCoachTextW);
  for (const char* t : kCoachTabMore) fits(small, t, kCoachTextW);
  (void)body;
}

// The play-next toast on two lines: a typical title in Body, mockup 12's
// long one in Small (the stage-1 fix left 168 px: both were cut).
void test_toast_names_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  const int room = kToastCompactTextRight - kToastTextX;
  fits(body, "Can'T Tell Me Nothing", room);
  fits(body, "One More Time", room);
  fits(small, "Harder, Better, Faster, Stronger", room);
  fits(small, "Plays next", room);
}

// The queue's cap (docs/QUEUE-MODES.md 15): the refusal on one line, no
// buttons; a Play, Shuffle all or add it cut short as "what: why" on the
// toast's two lines (too wide for one beside its buttons, as Toast::show()
// decides), each line in its room in Small at least (Toast draws the why in
// Body when it fits), with the counts of a library of 99,999.
void test_queue_cap_texts_fit() {
  using namespace uitext;
  using queueview::Capped;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  fits(body, kQueueFull, kToastTextRight - kToastTextX);
  TEST_ASSERT_TRUE(strstr(kQueueFull, ": ") == nullptr);  // one line, always
  struct Case {
    Capped what;
    uint32_t took, asked;
    bool view;  // an add: View beside Undo
  } cases[] = {{Capped::Shuffle, 5000, 99999, false},
               {Capped::Play, 5000, 99999, false},
               {Capped::Add, 4999, 99999, true},
               {Capped::Next, 4999, 99999, true}};
  for (const Case& c : cases) {
    char t[128];
    queueview::cappedText(c.what, c.took, c.asked, t, sizeof(t));
    const std::string s(t);
    const size_t colon = s.find(": ");
    TEST_ASSERT_TRUE(colon != std::string::npos);
    const int oneLine = (c.view ? kToastViewX : kToastUndoX) - 6 - kToastTextX;
    const int room = c.view ? kToastCompactTextRight - kToastTextX : kToastUndoCX - 6 - kToastTextX;
    TEST_ASSERT_TRUE(body.width(t) > oneLine);
    fits(small, s.substr(0, colon).c_str(), room);
    fits(small, t + colon + 2, room);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: what %d (Small), why %d (Body) / %d (Small) in %d", t,
             small.width(s.substr(0, colon).c_str()), body.width(t + colon + 2), small.width(t + colon + 2), room);
    TEST_MESSAGE(msg);
  }
}

// The empty states' two buttons (the empty queue, Nothing playing) and line.
void test_empty_state_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  TEST_ASSERT_TRUE(bold.width("Open Library") + kEmptyIconW[0] + 8 <= kEmptyPrimaryW - 12);
  TEST_ASSERT_TRUE(body.width("Shuffle all") + kEmptyIconW[1] + 8 <= kEmptySecondW - 12);
  TEST_ASSERT_TRUE(kEmptyPrimaryX + kEmptyPrimaryW < kEmptySecondX && kEmptySecondX + kEmptySecondW <= 320 - 6);
  fits(small, kPickInLibrary, kEmptyLineW);
}

// No card, and each kind of card that didn't mount (cardMessage(): exFAT,
// NTFS, a GPT, nothing recognised): the empty state's title and lines, and
// Try again's note when it still fails (Small before ": ", Body after).
void test_no_card_texts_fit() {
  using namespace uitext;
  using cardformat::Kind;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, kTryAgain, 320 - 140 - 12);  // the one button: x 70, 180 px, 6 px in at each end
  for (Kind k : {Kind::Unreadable, Kind::Other, Kind::ExFat, Kind::Ntfs, Kind::Gpt}) {
    const CardMessage& m = cardMessage(k);
    fits(title, m.title, kEmptyTitleW);
    for (const char* t : m.lines) fits(small, t, kEmptyLineW);
    const std::string n(m.still);
    const size_t colon = n.find(": ");
    TEST_ASSERT_TRUE(colon != std::string::npos);
    fits(small, n.substr(0, colon).c_str(), kToastTwoLineW);
    fits(body, m.still + colon + 2, kToastTwoLineW);
    char msg[200];
    snprintf(msg, sizeof(msg), "%s: title %d; lines %d, %d; note %d, %d", cardformat::name(k), title.width(m.title),
             small.width(m.lines[0]), small.width(m.lines[1]), small.width(n.substr(0, colon).c_str()),
             body.width(m.still + colon + 2));
    TEST_MESSAGE(msg);
  }
}

// The board guard's screen (Font2, no wrap): every line at Font2's widest
// glyph per character, printable ASCII only (Font2 has nothing else), the
// "found: " line with the longest name BoardGuard allows.
void test_board_guard_texts_fit() {
  using namespace uitext;
  auto fitsFont2 = [](const char* text, size_t extraChars) {
    for (const char* p = text; *p; ++p) TEST_ASSERT_TRUE(*p >= 0x20 && *p < 0x7f);
    const int w = static_cast<int>(strlen(text) + extraChars) * kFont2MaxAdvance;
    char msg[120];
    snprintf(msg, sizeof(msg), "\"%s\" (+%u chars): at most %d px in %d", text, (unsigned)extraChars, w,
             kBoardGuardW);
    TEST_ASSERT_TRUE_MESSAGE(w <= kBoardGuardW, msg);
  };
  for (const char* t : kBoardGuardTop) fitsFont2(t, 0);
  fitsFont2(kBoardGuardFound, static_cast<size_t>(kBoardNameMaxChars));
  for (const char* t : kBoardGuardStop) fitsFont2(t, 0);
  fitsFont2(kBoardGuardToughHint, 0);
}

// The Queue's selection bar: Remove shows its count (with the icon up to
// 99, without it beyond), and its header keeps the position.
void test_queue_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  char t[24];
  for (unsigned n = 1; n <= 9999; n = n < 120 ? n + 1 : n * 3) {
    snprintf(t, sizeof(t), "Remove %u", n);
    if (n <= 99) {
      fits(bold, t, kRemoveW - kRemoveTextX - 4);
    } else {
      fits(bold, t, kRemoveW - 8);
    }
  }
  fits(body, "Play next", kQueueNextW - 6);
  fits(body, "Clear\xE2\x80\xA6", kQueueClearW - 8);
  TEST_ASSERT_TRUE(kRemoveX + kRemoveW < kQueueNextX && kQueueNextX + kQueueNextW < kQueueClearX &&
                   kQueueClearX + kQueueClearW <= 320);
  // The header: "4 of 16 · 49 min" beside "Queue" and the Edit pill (the
  // room QueuePage::header() computes), up to 999 entries and 16 hours.
  const int room = (320 - 6 - (bold.width("Edit") + 24) - 8) - (12 + bold.width("Queue") + 8);
  queueview::Time time;
  time.knownS = 999 * 60;
  time.unknown = 1;
  char buf[64];
  queueview::summary(500, time, 999, 999, buf, sizeof(buf), false);
  fits(small, buf, room);
  time.knownS = 49 * 60;
  time.unknown = 0;
  queueview::summary(12, time, 4, 16, buf, sizeof(buf), false);
  fits(small, buf, room);
}

// The Output tab: every button of every Bluetooth card in its box (the
// armed Forget's "Tap again" too), the volume chip at 100 %, the status
// lines, the speaker's line, the hints.
void test_output_texts_fit() {
  using namespace uitext;
  using P = BtLink::Phase;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  const P phases[] = {P::Off, P::Paging, P::Scanning, P::Linked, P::PairScan, P::Pairing, P::Backoff, P::Resting};
  int cards = 0;
  for (int sess = 0; sess < 5; ++sess) {
    for (P phase : phases) {
      for (int remembered = 0; remembered < 2; ++remembered) {
        for (int lost = 0; lost < 2; ++lost) {
          BtSession s;
          if (sess == 1) s.connect(0);
          if (sess == 2 || sess == 3) {
            // A connect (2) or a pairing (3) that failed.
            if (sess == 2) s.connect(0); else s.pairStarted(0);
            BtLink l;
            l.phase = P::Paging;
            l.remembered = true;
            s.update(l, 10);
            l.phase = P::Off;
            s.update(l, 20);
          }
          if (sess == 4) s.pairStarted(0);
          BtLink link;
          link.phase = phase;
          link.remembered = remembered != 0;
          link.attempt = 3;
          link.attempts = 3;
          const BtCardView v = btCardView(link, s, lost != 0);
          ++cards;
          const int n = v.buttonCount;
          for (int i = 0; i < n; ++i) {
            const BtButton b = v.buttons[i];
            int w = n == 1 ? kBtButtons1W : n == 2 ? kBtButtons2W[i] : kBtButtons3W[i];
            if (n == 1 && b == BtButton::Pair) w = 320 - 32;
            const int room = w - kBtButtonPad;
            if (b == BtButton::Volume) {
              fits(body, "100%", w - kBtChipTextX - 4);
            } else if (b == BtButton::More) {
              TEST_ASSERT_TRUE(18 <= room);  // its icon
            } else {
              fits(b == BtButton::Pair ? bold : body, btButtonLabel(b, w < kBtWideButtonW), room);
              if (b == BtButton::Forget) fits(bold, kForgetArmed, room);
            }
          }
          char line[80];
          btStatusLine(v, link, "SPYDRONE", "SBC 44.1 kHz, 175 ms", line, sizeof(line));
          fits(small, line, kBtStatusW);
          if (v.hint) {
            // Resting: its two lines beside the one button, right of it.
            TEST_ASSERT_EQUAL_INT(1, n);
            fits(small, kBtHintLine1, kBtHintW);
            fits(small, kBtHintLine2, kBtHintW);
          }
        }
      }
    }
  }
  TEST_ASSERT_TRUE(cards > 100);
  TEST_ASSERT_TRUE(kBtButtons3X[2] + kBtButtons3W[2] <= 320 - 16);
  // The speaker card: "Here until SPYDRONE connects" doesn't fit beside the
  // chip; its shorter form does (OutputPage::drawSpeaker picks).
  TEST_ASSERT_TRUE(small.width("Here until SPYDRONE connects") > kSpeakerLineW);
  fits(small, "Waits for SPYDRONE", kSpeakerLineW);
  fits(small, "Here until they connect", kSpeakerLineW);
  fits(small, kSilentMode, kSpeakerLineW);
  fits(body, "100%", kSpeakerChipW - 2);
  TEST_ASSERT_TRUE(52 + kSpeakerLineW <= kSpeakerChipX);
  fits(small, kPairHint, kPairHintW);
  // The resting card's hint starts past its button and ends before the edge.
  TEST_ASSERT_TRUE(kBtHintX >= kBtButtons1X + kBtButtons1W + 8);
  TEST_ASSERT_TRUE(kBtHintX + kBtHintW <= 320 - 8);
  // The lost dialog's line while resting (Small over the dialog's 288 px).
  fits(small, kBtLostRestingLine, 320 - 32);
  // The resting card's hint when they dropped while the output.
  fits(small, kBtLostHintLine1, kBtHintW);
  fits(small, kBtLostHintLine2, kBtHintW);
  // The pocket rule's note (Ui::warn: Body on one line, no buttons).
  fits(body, kTouchFirst, kToastTextRight - kToastTextX);
  // None paired (a B hold, a play on Bluetooth): too long for one line, so
  // the toast takes its two ("No headphones paired" in Small over "Output
  // > Pair new headphones" in Body).
  {
    const char* colon = strstr(kNoHeadphones, ": ");
    TEST_ASSERT_NOT_NULL(colon);
    TEST_ASSERT_TRUE(body.width(kNoHeadphones) > kToastTextRight - kToastTextX);
    const std::string what(kNoHeadphones, colon);
    fits(small, what.c_str(), kToastTwoLineW);
    fits(body, colon + 2, kToastTwoLineW);
  }
  // A skipped track's why ("Skipped <title>: <why>"; a long title takes
  // the toast's two lines, the why in Body): each, with the widest rates
  // a refusal can name (RateConverter::rateText()).
  fits(body, kSkipped, kToastTwoLineW);
  fits(body, kSkippedCpu, kToastTwoLineW);
  for (uint32_t hz : {96000u, 88200u, 176400u, 352800u, 705600u, 44056u, 655350u, 1048575u}) {
    char rate[16], why[48];
    RateConverter::rateText(hz, rate, sizeof(rate));
    snprintf(why, sizeof(why), kSkippedRate, rate);
    fits(body, why, kToastTwoLineW);
  }
  // The Pair screen once its search stopped.
  fits(bold, kPairSearchAgain, 320 - 60);
  fits(small, kPairSearchStopped, kPairHintW);
  fits(small, kLineOutSub, kSettingSubW);
  // The screen settings: every choice in its pill, the lines beside it.
  for (int c = 0; c < ScreenPower::kTimeouts; ++c) fits(body, ScreenPower::timeoutLabel(c), kSettingPillW - kSettingPillPad);
  for (int c = 0; c < ScreenPower::kBrightnesses; ++c) {
    fits(body, ScreenPower::brightnessLabel(c), kSettingPillW - kSettingPillPad);
  }
  fits(body, kScreenOffTitle, kSettingValueSubW);
  fits(body, kBrightnessTitle, kSettingValueSubW);
  fits(small, kScreenOffSub, kSettingValueSubW);
  fits(small, kScreenNeverSub, kSettingValueSubW);
  fits(small, kBrightnessSub, kSettingValueSubW);
  TEST_ASSERT_TRUE(kSettingValueSubW > 100);
  // About: a long value falls back to Small, which fits.
  char v[80];
  snprintf(v, sizeof(v), kAboutMemory, 263ul, 158ul, 3.2f);
  fits(small, v, kAboutValueW);
  snprintf(v, sizeof(v), kAboutLibrary, 10000ul, 600ul, 1500ul);
  fits(small, v, kAboutValueW);
  // The licence rows, each line in the font it's drawn in (the values fit
  // in Body), and the source row's two lines put back together are the URL.
  fits(small, kAboutLicenceLabel, kAboutValueW);
  fits(body, kAboutLicence, kAboutValueW);
  fits(small, kAboutSourceLabel, kAboutValueW);
  fits(body, kAboutSourceRepo, kAboutValueW);
  const std::string url = std::string("https://") + (kAboutSourceLabel + strlen("Source: ")) + kAboutSourceRepo;
  TEST_ASSERT_EQUAL_STRING(kSourceUrl, url.c_str());
  // The version row: the label with the date and any 8 hex digits of the
  // ELF's hash; a release's version in Body, a long dev build's in Small
  // (tools/version.py: git describe, at most 31 characters in the image).
  for (const char* hex = "0123456789abcdef"; *hex; ++hex) {
    snprintf(v, sizeof(v), kAboutVersionLabel, "2026-09-30", std::string(8, *hex).c_str());
    fits(small, v, kAboutValueW);
  }
  fits(body, "v10.10.10", kAboutValueW);
  fits(body, "v0.5.0-beta.10", kAboutValueW);
  fits(small, "v0.5.0-dev+abcdef1-dirty", kAboutValueW);
  fits(small, "v10.10.10-rc.10-9999-gabcdef12-dirty", kAboutValueW);
}

// The seek bar's readout (docs/SEEK-BAR.md section 4.2): "Release to
// cancel" in the line's width, and the group (the finger's second in
// Title, the gap, "no change" or the change in Small) within its
// kSeekReadoutW for any length up to 999:59 (a 16-hour mix), with the
// texts as SeekBar writes them. Every character is one the font has: one
// it lacks is measured as a space and drawn folded ("−" as a wider "-"),
// and the change came out "-0:…".
void test_seek_bar_texts_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, kSeekCancel, 296);
  // The figures are all as wide (in both fonts), so the extremes below
  // stand for every time of as many digits.
  for (char d = '1'; d <= '9'; ++d) {
    const char one[2] = {d, 0};
    TEST_ASSERT_EQUAL_INT(title.width("0"), title.width(one));
    TEST_ASSERT_EQUAL_INT(small.width("0"), small.width(one));
  }
  // 4:05, 59:59, 99:59, 100:00 and 999:59: the second shown, and the
  // change all the way back or on.
  const uint32_t lengths[] = {245000, 3599000, 5999000, 6000000, 59999000};
  for (uint32_t len : lengths) {
    char big[16], change[24];
    SeekBar::timeText(len, big, sizeof(big));
    TEST_ASSERT_TRUE_MESSAGE(title.hasAll(big), big);
    const int room = kSeekReadoutW - title.width(big) - kSeekReadoutGap;
    fits(small, kSeekStay, room);
    SeekBar::changeText(len, 0, change, sizeof(change));
    TEST_ASSERT_TRUE_MESSAGE(small.hasAll(change), change);
    fits(small, change, room);
    SeekBar::changeText(0, len, change, sizeof(change));
    TEST_ASSERT_TRUE_MESSAGE(small.hasAll(change), change);
    fits(small, change, room);
  }
  // "-0:45" whole, as drawn (it was cut to "-0:…").
  char change[24];
  SeekBar::changeText(25000, 70000, change, sizeof(change));
  TEST_ASSERT_EQUAL_STRING("-0:45", change);
  TEST_ASSERT_TRUE(small.hasAll(change));
}

// Now Playing while play waits for the headphones (PlayGate): the panel's
// lines and buttons, the notice's buttons, and the output line that says
// they aren't connected.
void test_waiting_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  const int panelW = 320 - 112;  // the title strip and the rows under it
  fits(small, "Waiting for SPYDRONE\xE2\x80\xA6", kWaitTextW);
  fits(small, "Waiting for WH-1000XM4\xE2\x80\xA6", kWaitTextW);
  fits(small, "Waiting for the headphones\xE2\x80\xA6", kWaitTextW);
  fits(small, "try 3 of 3", kWaitTextW);
  fits(small, "looking for them", kWaitTextW);
  TEST_ASSERT_TRUE(kWaitTextX + kWaitTextW <= panelW);
  fits(body, kPlayOnSpeaker, kWaitSpeakerW - kWaitButtonPad);
  fits(body, kWaitCancel, kWaitCancelW - kWaitButtonPad);
  TEST_ASSERT_TRUE(kWaitSpeakerX + kWaitSpeakerW < kWaitCancelX);
  TEST_ASSERT_TRUE(kWaitCancelX + kWaitCancelW <= panelW - 2);
  // The notice (a Dialog): its title, its buttons (the primary in Bold; a
  // label too wide for Body is drawn in Small).
  fits(bold, "Couldn't reach SPYDRONE", kDialogTitleW);
  TEST_ASSERT_TRUE(body.width(kPlayOnSpeaker) > kDialogButtonTextW);
  fits(small, kPlayOnSpeaker, kDialogButtonTextW);
  fits(bold, "Try again", kDialogButtonTextW);
  // The progress line's middle.
  fits(small, "SPYDRONE (not connected)", kNowPlayingMidW);
  fits(small, "Waiting, 24 of 86", kNowPlayingMidW);
  fits(small, "24 of 86 \xC2\xB7 SPYDRONE", kNowPlayingMidW);
  // ... clear of the times either side (x 12 and 308), long ones too.
  TEST_ASSERT_TRUE(12 + small.width("88:88") + 4 <= 160 - kNowPlayingMidW / 2);
}

// Now Playing's two menus (docs/QUEUE-MODES.md section 4.6): the rows'
// labels in a row, each state and detail in its row's room
// (sheet::detailRoom(), as Sheet::render() has it), the toasts on one
// line, "Playback" left of the ✕; the folder cut from the left by whole
// folders; the waiting title on one line.
void test_now_playing_menu_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  for (const char* l : {kGoTo[0], kGoTo[1], kGoTo[2], kShuffleRow, kRepeatRow, kSleepRow}) fits(body, l, 320 - 32);
  // The rooms, as measured with the firmware's fonts.
  TEST_ASSERT_EQUAL_INT(183, sheet::detailRoom(body.width(kGoTo[0])));
  TEST_ASSERT_EQUAL_INT(174, sheet::detailRoom(body.width(kGoTo[1])));
  TEST_ASSERT_EQUAL_INT(177, sheet::detailRoom(body.width(kGoTo[2])));
  TEST_ASSERT_EQUAL_INT(216, sheet::detailRoom(body.width(kShuffleRow)));
  TEST_ASSERT_EQUAL_INT(215, sheet::detailRoom(body.width(kRepeatRow)));
  TEST_ASSERT_EQUAL_INT(180, sheet::detailRoom(body.width(kSleepRow)));
  for (const char* s : kOnOff) fits(small, s, sheet::detailRoom(body.width(kShuffleRow)));
  for (const char* s : kRepeatModes) fits(small, s, sheet::detailRoom(body.width(kRepeatRow)));
  TEST_ASSERT_TRUE(small.width("One") <= 26);
  // The sleep states as before, against the Sleep timer row's room.
  for (const char* t : {"Off", "90 min", "59 s", "End of track", "End of album", "End of queue", "Fading"}) {
    fits(small, t, sheet::detailRoom(body.width(kSleepRow)));
  }
  fits(small, kNoArtistFolder, sheet::detailRoom(body.width(kGoTo[0])));
  fits(small, kLooseTracks, sheet::detailRoom(body.width(kGoTo[1])));
  // The refusals' toasts, and the sheet's title.
  fits(body, kBuiltinNotInLibrary, kToastTextRight - kToastTextX);
  fits(body, kLibraryNotReady, kToastTextRight - kToastTextX);
  fits(small, kPlaybackTitle, kSleepTitleW);
  // The folder: cut from the left by whole folders to Go to folder's room.
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  const int room = sheet::detailRoom(body.width(kGoTo[2]));
  char out[160];
  TEST_ASSERT_TRUE(small.width("/music/Daft Punk/Discovery") > room);
  textfit::cutPathLeft(f, "/music/Daft Punk/Discovery", room, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA6/Daft Punk/Discovery", out);
  fits(small, out, room);
  textfit::cutPathLeft(f, "/music/Kavinsky", room, out, sizeof(out));  // fits: whole
  TEST_ASSERT_EQUAL_STRING("/music/Kavinsky", out);
  textfit::cutPathLeft(f, "/music", room, out, sizeof(out));  // a track at the root
  TEST_ASSERT_EQUAL_STRING("/music", out);
  // A last folder too wide even alone: "/<last>" (the draw cuts its end).
  const std::string wide(40, 'W');
  textfit::cutPathLeft(f, ("/music/Some Artist/" + wide).c_str(), room, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(("/" + wide).c_str(), out);
  // A deep path keeps as many folders as fit.
  textfit::cutPathLeft(f, "/music/a/b/c/Daft Punk/Discovery", room, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA6/b/c/Daft Punk/Discovery", out);  // (never "/a/b/c/...": a cut says so)
  // The waiting title: one line of Bold 16 in the title's 190 px.
  textfit::Font b;
  b.ctx = const_cast<Vlw*>(&bold);
  b.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  const char* title = "Le voyage de P\xC3\xA9n\xC3\xA9lope (Remastered Edition)";
  textfit::fit(b, title, strlen(title), out, sizeof(out), 190);
  fits(bold, out, 190);
  fits(bold, "One More Time", 190);
}

// The sleep timer (ENERGY.md section 3): the "..." row and its state, the
// sheet's pills, the fade's toast, and the moon's text on Now Playing's
// progress line.
void test_sleep_timer_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  // The playback menu's row (Body from x 16), its state right-aligned
  // (Small) in what the label leaves (Sheet::render: 16 px each side and
  // between).
  const int stateRoom = sheet::detailRoom(body.width(kSleepRow));
  for (const char* t : {"Off", "90 min", "59 s", "End of track", "End of album", "End of queue", "Fading"}) {
    fits(small, t, stateRoom);
  }
  // The sheet's title (Small), left of its close pill: "Sleep timer: " and
  // SleepTimer::titleText(), lower case after the colon (as the toasts), in
  // every state: off, each end-of choice, the longest counts, fading.
  {
    std::vector<std::string> titles;
    auto add = [&](const SleepTimer& t, uint32_t now) {
      char buf[24];
      t.titleText(now, buf, sizeof(buf));
      titles.push_back(std::string(kSleepRow) + ": " + buf);
    };
    SleepTimer t;
    add(t, 0);
    for (SleepTimer::Choice c : {SleepTimer::Choice::EndOfTrack, SleepTimer::Choice::EndOfAlbum,
                                 SleepTimer::Choice::EndOfQueue}) {
      t.setEnd(c);
      add(t, 0);
    }
    t.setTimed(90 * 60000, 0);
    t.extend(0, 0);  // 100 min left
    add(t, 0);
    add(t, 100 * 60000 - 59000);  // 59 s
    for (const std::string& s : titles) {
      fits(small, s.c_str(), kSleepTitleW);
      TEST_ASSERT_NULL(strstr(s.c_str(), ": End"));  // no capital after the colon
    }
    TEST_ASSERT_EQUAL_STRING("Sleep timer: end of album", titles[2].c_str());
    TEST_ASSERT_EQUAL_STRING("Sleep timer: 100 min left", titles[4].c_str());
    fits(small, "Sleep timer: fading", kSleepTitleW);
  }
  // The pills: each label in its pill less the pad; the rows inside the
  // screen with gaps between the pills.
  for (int i = 0; i < 5; ++i) {
    fits(body, kSleepMinutes[i], kSleepMinW[i] - kSleepPillPad);
    if (i > 0) TEST_ASSERT_TRUE(kSleepMinX[i - 1] + kSleepMinW[i - 1] < kSleepMinX[i]);
  }
  TEST_ASSERT_TRUE(kSleepMinX[4] + kSleepMinW[4] <= 312);
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(body.width(kSleepEnds[1]) > kSleepEndW[1] - kSleepPillPad);  // why they are in Small
    fits(small, kSleepEnds[i], kSleepEndW[i] - kSleepPillPad);
    if (i > 0) TEST_ASSERT_TRUE(kSleepEndX[i - 1] + kSleepEndW[i - 1] < kSleepEndX[i]);
  }
  TEST_ASSERT_TRUE(kSleepEndX[2] + kSleepEndW[2] <= 312);
  fits(body, kSleepExtend, kSleepExtendW - kSleepPillPad);
  fits(body, kSleepTurnOff, kSleepOffW - kSleepPillPad);
  TEST_ASSERT_TRUE(kSleepExtendX + kSleepExtendW < kSleepOffX && kSleepOffX + kSleepOffW <= 312);
  fits(small, kSleepHint, kSleepHintW);
  // The fade's toast: its line (Small) before +10 min, the two buttons (Body).
  fits(small, kSleepFading, kSleepToastPlusX - 6 - kToastTextX);
  fits(body, kSleepExtend, kSleepToastPlusW - kSleepToastPad);
  fits(body, kSleepTurnOff, kSleepToastOffW - kSleepToastPad);
  TEST_ASSERT_TRUE(kSleepToastPlusX + kSleepToastPlusW < kSleepToastOffX && kSleepToastOffX + kSleepToastOffW <= 312);
  // The toasts a choice gives (one line, no buttons).
  for (const char* t : {"Sleep timer: 90 min", "Sleep timer: end of album", "Sleep timer: end of queue",
                        "Sleep timer off", "Sleep timer: 100 min left", "Sleep timer: 59 s left"}) {
    fits(body, t, kToastTextRight - kToastTextX);
  }
  // Now Playing's progress line: the moon and its text alone always fit,
  // and after "4 of 16 · Speaker" (a long headphone name leaves it alone).
  for (const char* t : {"90 min", "59 s", "track", "album", "queue", "fading"}) {
    fits(small, t, kNowPlayingMidW - kSleepMoonW);
  }
  fits(small, "4 of 16 · Speaker", kNowPlayingMidW - kSleepGap - kSleepMoonW - small.width("23 min"));
  // What gives way when the line and the moon don't both fit
  // (sleepLineFit()): the output's name first, so the queue position stays.
  using Text = SleepLine::Text;
  const int moon23 = kSleepMoonW + small.width("23 min");
  const int moon90 = kSleepMoonW + small.width("90 min");
  SleepLine f = sleepLineFit(small.width("4 of 16 · Speaker"), small.width("4 of 16"), -1, moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Full, f.text);
  f = sleepLineFit(small.width("4 of 16 · SPYDRONE"), small.width("4 of 16"), -1, moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Base, f.text);  // "4 of 16" and the moon, not the moon alone
  TEST_ASSERT_TRUE(f.moon && f.moonText);
  TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW);
  // Paused, with a long queue: the position still shows beside the moon.
  for (const char* base : {"Paused, 12 of 160", "Paused, 160 of 160", "Stopped, 99 of 999"}) {
    const std::string full = std::string(base) + " · SPYDRONE";
    f = sleepLineFit(small.width(full.c_str()), small.width(base), -1, moon90, kNowPlayingMidW);
    TEST_ASSERT_EQUAL(Text::Base, f.text);
    TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW);
  }
  // The headphones not connected (the drop during the countdown pauses):
  // the amber warning never gives way to the timer, whatever the name.
  for (const char* warn : {"SPYDRONE (not connected)", "Headphones (not connected)",
                           "WH-1000XM4 Long Name (not connected)"}) {
    for (int moonText : {kSleepMoonW + small.width("45 s"), moon90, kSleepMoonW + small.width("fading")}) {
      f = sleepLineFit(small.width(warn), small.width("Paused, 4 of 16"), small.width(warn), moonText, kNowPlayingMidW);
      TEST_ASSERT_TRUE(f.text == Text::Warn || f.text == Text::Full);
      TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW || !f.moon);
    }
  }
  // "SPYDRONE (not connected)" (Small, 177 px measured: the room less the
  // moon is 171): the warning alone, the timer on the tab bar's badge.
  f = sleepLineFit(small.width("SPYDRONE (not connected)"), small.width("Paused, 4 of 16"),
                   small.width("SPYDRONE (not connected)"), moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Warn, f.text);
  TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW);
  // A short name keeps the moon (and its text) beside the warning.
  f = sleepLineFit(small.width("Buds (not connected)"), small.width("Paused, 4 of 16"),
                   small.width("Buds (not connected)"), moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Warn, f.text);
  TEST_ASSERT_TRUE(f.moon);
  (void)bold;
}

// The idle power-off (IdlePolicy; ENERGY.md item 4): the setting's row
// (as the screen's), the warning toast and its button, and the next boot's
// toast for every length.
void test_idle_power_off_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  for (int c = 0; c < IdlePolicy::kChoices; ++c) fits(body, IdlePolicy::choiceLabel(c), kSettingPillW - kSettingPillPad);
  fits(body, kIdleOffTitle, kSettingValueSubW);
  fits(small, kIdleOffSub, kSettingValueSubW);
  fits(small, kIdleNeverSub, kSettingValueSubW);
  // The warning: its longest count, then [Keep on] to the edge.
  char t[48];
  IdlePolicy::warnText(30, t, sizeof(t));
  fits(body, t, kIdleToastKeepX - 6 - kToastTextX);
  fits(body, kIdleKeepOn, kIdleToastKeepW - kIdleToastPad);
  TEST_ASSERT_TRUE(kIdleToastKeepX + kIdleToastKeepW <= 312);
  // The next boot's toast: each setting, and the console's test lengths.
  for (uint32_t ms : {10u * 60000u, 20u * 60000u, 60u * 60000u, 60000u, 59000u, 90u * 60000u}) {
    IdlePolicy::offText(ms, t, sizeof(t));
    fits(body, t, kToastTextRight - kToastTextX);
  }
}

// CPU speed and Bluetooth power (PowerChoices; ENERGY.md items 6 and 7):
// the rows (as the screen's), every choice in its pill and every line
// beside it, the restart dialog, its toast, the next boot's toast, and
// About's power row.
void test_power_settings_texts_fit() {
  using namespace uitext;
  namespace pc = powerchoice;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  fits(body, kCpuTitle, kSettingValueSubW);
  fits(body, kBtPowerTitle, kSettingValueSubW);
  char buf[64];
  for (uint16_t mhz : pc::kCpuMhz) {
    fits(body, pc::cpuLabel(mhz), kSettingPillW - kSettingPillPad);
    fits(small, pc::cpuSub(mhz, mhz, buf, sizeof(buf)), kSettingValueSubW);
    // Saved, not running yet (the console's Pcb): what runs until a restart.
    fits(small, pc::cpuSub(pc::otherCpuMhz(mhz), mhz, buf, sizeof(buf)), kSettingValueSubW);
    pc::cpuDialogTitle(mhz, buf, sizeof(buf));
    fits(bold, buf, kDialogTitleW);
    pc::cpuRestartingText(mhz, buf, sizeof(buf));
    fits(body, buf, kToastTextRight - kToastTextX);
    pc::cpuBootText(mhz, buf, sizeof(buf));
    fits(body, buf, kToastTextRight - kToastTextX);
  }
  for (int c = 0; c < pc::kBtChoices; ++c) {
    fits(body, pc::btLabel(c), kSettingPillW - kSettingPillPad);
    fits(small, pc::btSub(c, false), kSettingValueSubW);
  }
  fits(small, pc::btSub(pc::kBtNormal, true), kSettingValueSubW);
  // The dialog's bodies (to 240, and to 160 with its cost): 3 lines of
  // Small over 260 px, none cut, within the dialog's 128 bytes.
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  for (uint16_t mhz : pc::kCpuMhz) {
    const char* dialogBody = pc::cpuDialogBody(mhz);
    TEST_ASSERT_TRUE(strlen(dialogBody) < 128);
    char lines[4][96];
    const int n = textfit::wrap(f, dialogBody, strlen(dialogBody), kDialogTitleW, 3, &lines[0][0], sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 3);
    std::string joined;
    for (int i = 0; i < n; ++i) joined += std::string(i ? " " : "") + lines[i];
    TEST_ASSERT_EQUAL_STRING(dialogBody, joined.c_str());
  }
  fits(bold, kCpuRestart, kDialogButtonTextW);
  fits(body, kCpuWaitPairing, kToastTextRight - kToastTextX);
  // About: its label (Small), and every value in Body (all fit; Small is
  // the fallback).
  fits(small, kAboutPower, kAboutValueW);
  for (uint16_t mhz : pc::kCpuMhz) {
    for (int c = 0; c < pc::kBtChoices; ++c) {
      pc::aboutText(mhz, c, buf, sizeof(buf));
      fits(body, buf, kAboutValueW);
    }
  }
}

// The touch calibration: its Output row, sheet, dialog and toast; the
// calibration screen's header, lines, rows and A hints; the first-boot
// check's; the boot screen's rescue line (docs: README's touch section).
void test_touch_calibration_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  // Output: the row, its sheet (Calibrate the primary, its detail beside
  // it), the dialog, the toast.
  fits(body, kCalRowTitle, kSettingSubW);
  fits(small, kCalSubOff, kSettingSubW);
  fits(small, kCalSubOn, kSettingSubW);
  const int sheetLabelW = 320 - 32;
  fits(bold, kCalCalibrate, sheetLabelW);
  fits(small, kCalCalibrateDetail, 320 - 16 - (16 + bold.width(kCalCalibrate) + 16));
  fits(body, kCalCheckTitle, sheetLabelW);
  fits(body, kCalRemoveRow, sheetLabelW);
  fits(bold, kCalRemoveTitle, kDialogTitleW);
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* t) { return static_cast<const Vlw*>(ctx)->width(t); };
  {
    TEST_ASSERT_TRUE(strlen(kCalRemoveBody) < 128);
    char lines[4][96];
    const int n = textfit::wrap(f, kCalRemoveBody, strlen(kCalRemoveBody), kDialogTitleW, 3, &lines[0][0],
                                sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 3);
    std::string joined;
    for (int i = 0; i < n; ++i) joined += std::string(i ? " " : "") + lines[i];
    TEST_ASSERT_EQUAL_STRING(kCalRemoveBody, joined.c_str());
  }
  fits(bold, kCalRemove, kDialogButtonTextW);
  fits(body, kCalRemoved, kToastTextRight - kToastTextX);

  // The header: each pill before the title, the titles, the progress.
  for (const char* pill : {kCalCancel, kCalSkip}) {
    TEST_ASSERT_TRUE(kCalPillX + bold.width(pill) + kCalPillPad <= kCalTitleX - 4);
  }
  fits(bold, kCalTitle, kCalTitleW);
  fits(bold, kCheckTitle, kCalTitleW);
  fits(bold, kCalCheckTitle, kCalTitleW);
  fits(small, "9 of 9", kCalProgressW);
  fits(small, kCalSaved, kCalProgressW);
  fits(small, kCalUndone, kCalProgressW);
  TEST_ASSERT_TRUE(kCalTitleX + kCalTitleW + 6 + kCalProgressW <= 312);
  // The Cancel pill's hit area covers it.
  TEST_ASSERT_TRUE(kCalPillX + bold.width(kCalCancel) + kCalPillPad <= touchcheck::kCancelW);
  TEST_ASSERT_EQUAL_INT(kCalHeaderH, touchcheck::kCancelH);

  // The lines (Body; the result's numbers fall back to Small).
  char buf[96];
  fits(body, kCalFirstHint[0], kCalLineW);
  snprintf(buf, sizeof(buf), kCalFirstHint[1], 9);
  fits(body, buf, kCalLineW);
  for (const char* t : {kCalHint, kCalMissed, kCheckHint, kCheckMissed, kCheckFirstHint[0], kCheckFirstHint[1],
                        kCalCheckLines[0], kCalCheckLines[1], kCalSaveLine, kCalAccurate, kCalNoBetter,
                        kCalDisagree[0], kCalDisagree[1], kCheckGoOn}) {
    fits(body, t, kCalLineW);
  }
  fits(bold, kCheckAsk, kCalLineW);
  fits(title, kCheckAccurate, kCalLineW);
  // Typical results in Body; the widest possible in Small.
  touchcheck::errorText(kCalNow, touchcheck::Error{42, 21}, buf, sizeof(buf));
  fits(body, buf, kCalLineW);
  touchcheck::errorText(kCalNew, touchcheck::Error{5, 3}, buf, sizeof(buf));
  fits(body, buf, kCalLineW);
  touchcheck::errorText(kCalNew, touchcheck::Error{999, 999}, buf, sizeof(buf));
  fits(small, buf, kCalLineW);
  // The check's verdict: two Body lines at most, the longest way and size.
  f.ctx = const_cast<Vlw*>(&body);
  for (touchcheck::Dir d : {touchcheck::Dir::Right, touchcheck::Dir::Left, touchcheck::Dir::Below,
                            touchcheck::Dir::Above, touchcheck::Dir::None}) {
    touchcheck::verdictText(touchcheck::Verdict{true, 300, d}, buf, sizeof(buf));
    char lines[3][64];
    const int n = textfit::wrap(f, buf, strlen(buf), kCalLineW, 3, &lines[0][0], sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 2);
  }

  // The rows: centred labels, and labels with their details.
  const int rowText = kCalRowW - 2 * kCalRowPad;
  for (const char* t : {kCalSave, kCalTryAgain, kCalDiscard, kCalDone}) {
    fits(bold, t, rowText);
    fits(body, t, rowText);
  }
  TEST_ASSERT_TRUE(bold.width(kCalCalibrate) + small.width(kCalCalibrateDetail) + 3 * kCalRowPad <= kCalRowW);
  TEST_ASSERT_TRUE(body.width(kCheckNotNow) + small.width(kCheckLater) + 3 * kCalRowPad <= kCalRowW);
  // Three rows, the lines above them, and the A hint under them.
  TEST_ASSERT_TRUE(kCalRowsBottom - 3 * 40 >= 86 + 11 && kCalRowsBottom <= kCalHintY);

  // The A hints: "A: " and each page's verb.
  for (const char* verb : {kCalCancel, kCalSkip, kCheckNotNow, kCalDiscard, kCalDone, kCalUndo}) {
    snprintf(buf, sizeof(buf), "A: %s", verb);
    fits(small, buf, kCalAHintW);
  }
  // The test taps page's key, after its A hint (Done, or Undo after a
  // Save; 6 px clear of it) and its dot, to the band's end.
  for (const char* verb : {kCalDone, kCalUndo}) {
    snprintf(buf, sizeof(buf), "A: %s", verb);
    TEST_ASSERT_TRUE(kCalAHintX + small.width(buf) + 6 <= kCalKeyX);
  }
  TEST_ASSERT_TRUE(kCalKeyX + 6 < kCalKeyTextX);
  fits(small, kCalCheckKey, kCalKeyTextW);
  TEST_ASSERT_EQUAL_INT(312, kCalKeyTextX + kCalKeyTextW);
  // The boot screen's rescue line.
  fits(small, kBootTouchHint, kCalLineW);
}

// With no correction (the default, until the owner calibrates), a panel
// that reads x too far right like the lab's (TouchCalibration::labFitX():
// +20 px at x 190, +35-45 from 240) must still hit the left one of two
// controls side by side on the right half when the finger is on its
// centre: that is the "taps are off" state the calibration is for, and
// View -> Undo takes an add back. On a panel that reads true, each centre
// is its own control too.
void test_right_half_neighbours_survive_the_lab_panel() {
  using namespace uitext;
  const TouchCalibration::Axis lab = TouchCalibration::labFitX();
  struct Reading {
    int x;
    bool edge;
  };
  auto lab_ = [&](int x) {
    const long r = std::lround(lab.unmap(static_cast<float>(x)));
    return r >= 319 ? Reading{319, true} : Reading{static_cast<int>(r), false};
  };
  auto truth = [](int x) { return Reading{x, x >= 319}; };
  for (auto read : {std::function<Reading(int)>(lab_), std::function<Reading(int)>(truth)}) {
    // The toast's View and Undo, on one line and on two.
    for (bool compact : {false, true}) {
      const int viewC = compact ? kToastViewCX + kToastViewCW / 2 : kToastViewX + kToastViewW / 2;
      const int undoC = compact ? kToastUndoCX + kToastUndoCW / 2 : kToastUndoX + kToastUndoW / 2;
      Reading r = read(viewC);
      char msg[64];
      snprintf(msg, sizeof(msg), "View (%s) at %d reads %d", compact ? "icons" : "words", viewC, r.x);
      TEST_ASSERT_EQUAL_INT_MESSAGE(3, toastButtonAt(r.x, r.edge, compact, true, true), msg);
      r = read(undoC);
      TEST_ASSERT_EQUAL_INT(2, toastButtonAt(r.x, r.edge, compact, true, true));
      // The text never runs under View.
      TEST_ASSERT_TRUE((compact ? kToastCompactTextRight : kToastViewX - 6) <= (compact ? kToastViewCX : kToastViewX));
    }
    // The sleep timer's fade toast: +10 min next to Turn off.
    Reading r = read(kSleepToastPlusX + kSleepToastPlusW / 2);
    TEST_ASSERT_EQUAL_INT((int)SleepTimer::ToastButton::Extend, (int)SleepTimer::toastTap(r.x, r.edge, false));
    r = read(kSleepToastOffX + kSleepToastOffW / 2);
    TEST_ASSERT_TRUE(SleepTimer::toastTap(r.x, r.edge, false) != SleepTimer::ToastButton::Extend);
    // The tab bar: every tab's centre (Dance next to Output at 216).
    for (int t = 0; t < tabbar::kTabs; ++t) {
      const int centre = (tabbar::cellX0(t) + tabbar::cellX1(t)) / 2;
      r = read(centre);
      TEST_ASSERT_EQUAL_INT(t, tabbar::tabAt(r.x, r.edge));
    }
    // The speaker card: its volume chip's centre is the chip, its radio
    // (x 290) isn't.
    r = read(kSpeakerChipX + kSpeakerChipW / 2);
    TEST_ASSERT_TRUE(!r.edge && r.x >= kSpeakerChipHitX && r.x < kSpeakerChipHitEnd);
    r = read(290);
    TEST_ASSERT_TRUE(r.edge || r.x >= kSpeakerChipHitEnd);
  }
  // The compact toast still has the room its long names need (Small).
  const Vlw small(kVlwSans13);
  fits(small, "Harder, Better, Faster, Stronger", kToastCompactTextRight - kToastTextX);
}

// ---- the Library's names from the tags (docs/METADATA.md 5.4, 3.6; N9) ----

namespace {

// A record's view with the fields a test gives (the rest absent).
struct View {
  LibraryIndex::TagView v;
  View& title(const char* s) { return set(&v.title, &v.titleLen, s); }
  View& artist(const char* s) { return set(&v.artist, &v.artistLen, s); }
  View& album(const char* s) { return set(&v.album, &v.albumLen, s); }
  View& albumArtist(const char* s) { return set(&v.albumArtist, &v.albumArtistLen, s); }
  View& artistSort(const char* s) { return set(&v.artistSort, &v.artistSortLen, s); }
  View& albumSort(const char* s) { return set(&v.albumSort, &v.albumSortLen, s); }
  View& year(uint16_t y) {
    v.year = y;
    return *this;
  }
  View& track(uint16_t t) {
    v.track = t;
    return *this;
  }
  View& disc(uint16_t d) {
    v.disc = d;
    return *this;
  }
  View& ms(uint32_t d) {
    v.durationMs = d;
    return *this;
  }
  View& compilation() {
    v.compilation = 1;
    return *this;
  }
  View& transfer() {
    v.source = LibraryIndex::kFromTransfer;
    return *this;
  }
  View& set(const char** p, size_t* n, const char* s) {
    *p = s;
    *n = strlen(s);
    return *this;
  }
};

// The tagged library of these tests: made-up names (no real library's).
//   Lantern Choir/      (its tags say "The Lantern Choir")
//     First Light/      2001, three tracks, a guest on the third
//     Second Wind/      2010, one track
//     Demos/            no records: named by its paths
//     04 - Stray.mp3    the artist folder's loose track, tagged with an album
//   Various/Summer Mix/ a compilation of two artists, 2015
//   Orchard Hum/Archive/CD1, CD2: two discs, the tags say so
void buildTagged(LibraryIndex& idx) {
  TEST_ASSERT_TRUE(idx.begin("/music"));
  auto add = [&](const char* path, const View& v) {
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addRecord(path, v.v));
  };
  const char* lc = "The Lantern Choir";
  add("/music/Lantern Choir/First Light/01 - Dawn.mp3",
      View().title("Dawn").artist(lc).album("First Light").albumArtist(lc).year(2001).track(1).ms(201000));
  add("/music/Lantern Choir/First Light/02 - Noon.mp3",
      View().title("Noon").artist(lc).album("First Light").albumArtist(lc).year(2001).track(2).ms(1000));
  add("/music/Lantern Choir/First Light/03 - Dusk.mp3", View()
                                                            .title("Dusk")
                                                            .artist("The Lantern Choir\x1FMara Quill")
                                                            .album("First Light")
                                                            .albumArtist(lc)
                                                            .year(2001)
                                                            .track(3)
                                                            .ms(1000));
  add("/music/Lantern Choir/Second Wind/01 - Gale.flac",
      View().title("Gale").artist(lc).album("Second Wind").year(2010).track(1).ms(199400).transfer());
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addFile("/music/Lantern Choir/Demos/01 - Sketch.mp3"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addFile("/music/Lantern Choir/Demos/02 - Sketch Two.mp3", LibraryIndex::kAddPending));
  add("/music/Lantern Choir/04 - Stray.mp3", View().title("Stray").artist(lc).album("A Single").year(2005));
  add("/music/Orchard Hum/Archive/CD1/01 - One.mp3", View().title("One").artist("Orchard Hum").album("Archive").disc(1).track(1));
  add("/music/Orchard Hum/Archive/CD1/02 - Two.mp3", View().title("Two").artist("Orchard Hum").album("Archive").disc(1).track(2));
  add("/music/Orchard Hum/Archive/CD2/01 - Three.mp3",
      View().title("Three").artist("Orchard Hum").album("Archive").disc(2).track(1));
  add("/music/Orchard Hum/Archive/CD2/02 - Four.mp3",
      View().title("Four").artist("Orchard Hum").album("Archive").disc(2).track(2));
  add("/music/Various/Summer Mix/01 - Wave.mp3",
      View().title("Wave").artist("Ola Brenmark").album("Summer Mix").year(2015).track(1).compilation());
  add("/music/Various/Summer Mix/02 - Tide.mp3",
      View().title("Tide").artist("Pim Vossaert").album("Summer Mix").year(2015).track(2).compilation());
  TEST_ASSERT_TRUE(idx.finish());
}

uint32_t artistNamed(const LibraryIndex& idx, const char* name) {
  for (uint32_t a = 0; a < idx.artistCount(); ++a) {
    if (strcmp(idx.artistName(a), name) == 0) return a;
  }
  TEST_FAIL_MESSAGE(name);
  return LibraryIndex::kNone;
}

uint32_t albumOf(const LibraryIndex& idx, const char* path) {
  const uint32_t t = idx.findTrack(path);
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, t);
  return idx.track(t).album;
}

std::string text(size_t (*fn)(const LibraryIndex&, uint32_t, char*, size_t), const LibraryIndex& idx, uint32_t id) {
  char b[160];
  fn(idx, id, b, sizeof(b));
  return b;
}

std::string albumSub(const LibraryIndex& idx, uint32_t album, librarytext::AlbumPlace p) {
  char b[160];
  librarytext::albumSub(idx, album, p, b, sizeof(b));
  return b;
}

std::string trackSub(const LibraryIndex& idx, const char* path, bool album) {
  char b[160];
  librarytext::trackSub(idx, idx.findTrack(path), album, b, sizeof(b));
  return b;
}

std::string titleOf(const TrackCatalog& c, uint32_t id) {
  char b[260];
  c.title(id, b, sizeof(b));
  return b;
}

}  // namespace

// An artist's page and its rows: the albums newest first with their years,
// the elected spelling of the artist folder, the album's line, a track's
// own artist only where it differs; the root's Albums with the line and the
// year; an album's header.
void test_library_rows_from_tags() {
  using librarytext::AlbumPlace;
  LibraryIndex idx;
  buildTagged(idx);
  const uint32_t lc = artistNamed(idx, "The Lantern Choir");  // the folder "Lantern Choir", elected
  TEST_ASSERT_EQUAL_STRING("4 albums, 7 tracks", text(librarytext::artistCounts, idx, lc).c_str());
  TEST_ASSERT_EQUAL_STRING("1 album, 4 tracks",
                           text(librarytext::artistCounts, idx, artistNamed(idx, "Orchard Hum")).c_str());
  // Newest first: 2010, 2001, then the ones with no year.
  const LibraryIndex::Span albums = idx.albumsOf(lc);
  TEST_ASSERT_EQUAL_UINT32(4, albums.count);
  TEST_ASSERT_EQUAL_STRING("Second Wind", idx.albumName(albums[0]));
  TEST_ASSERT_EQUAL_STRING("First Light", idx.albumName(albums[1]));
  TEST_ASSERT_EQUAL_STRING("2010 \xC2\xB7 1 track", albumSub(idx, albums[0], AlbumPlace::OfArtist).c_str());
  TEST_ASSERT_EQUAL_STRING("2001 \xC2\xB7 3 tracks", albumSub(idx, albums[1], AlbumPlace::OfArtist).c_str());
  const uint32_t first = albumOf(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3");
  const uint32_t demos = albumOf(idx, "/music/Lantern Choir/Demos/01 - Sketch.mp3");
  const uint32_t loose = albumOf(idx, "/music/Lantern Choir/04 - Stray.mp3");
  const uint32_t mix = albumOf(idx, "/music/Various/Summer Mix/01 - Wave.mp3");
  TEST_ASSERT_EQUAL_STRING("2 tracks", albumSub(idx, demos, AlbumPlace::OfArtist).c_str());
  // The loose tracks keep no name and no year, whatever their tags say.
  TEST_ASSERT_EQUAL_STRING("1 track", albumSub(idx, loose, AlbumPlace::OfArtist).c_str());
  TEST_ASSERT_EQUAL_STRING(uitext::kLooseTracks, librarytext::albumShown(idx, loose));
  // The root's Albums: the line and the year; an album with no records
  // shows its artist's (elected) name.
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 2001", albumSub(idx, first, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", albumSub(idx, demos, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("Various Artists \xC2\xB7 2015", albumSub(idx, mix, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 2001 \xC2\xB7 3 tracks",
                           text(librarytext::albumHeader, idx, first).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 2 tracks", text(librarytext::albumHeader, idx, demos).c_str());
  // The tracks: a subtitle only where the artist isn't the album's line.
  TEST_ASSERT_EQUAL_STRING("", trackSub(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Mara Quill",
                           trackSub(idx, "/music/Lantern Choir/First Light/03 - Dusk.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING("Ola Brenmark", trackSub(idx, "/music/Various/Summer Mix/01 - Wave.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING("", trackSub(idx, "/music/Lantern Choir/Demos/01 - Sketch.mp3", false).c_str());
  // An artist's All tracks: the album after it.
  TEST_ASSERT_EQUAL_STRING("First Light", trackSub(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3", true).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Mara Quill \xC2\xB7 First Light",
                           trackSub(idx, "/music/Lantern Choir/First Light/03 - Dusk.mp3", true).c_str());
  TEST_ASSERT_EQUAL_STRING("(loose tracks)", trackSub(idx, "/music/Lantern Choir/04 - Stray.mp3", true).c_str());
  // A text cut to its buffer: "The Lantern Choir, Mara Quill" in 9 bytes.
  char small[9];
  librarytext::trackSub(idx, idx.findTrack("/music/Lantern Choir/First Light/03 - Dusk.mp3"), false, small,
                        sizeof(small));
  TEST_ASSERT_EQUAL_STRING("The Lant", small);
  char tiny[4];
  librarytext::albumSub(idx, first, AlbumPlace::OfArtist, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("200", tiny);
  // ... never mid-character: "\xC3\x89lan Vey" (its first letter two
  // bytes) in 2 bytes is "", in 3 the letter whole.
  LibraryIndex accents;
  TEST_ASSERT_TRUE(accents.begin("/music"));
  accents.addFile("/music/\xC3\x89lan Vey/Set/01 - a.mp3");
  TEST_ASSERT_TRUE(accents.finish());
  char two[2], three[3];
  librarytext::albumSub(accents, 0, AlbumPlace::AZ, two, sizeof(two));
  TEST_ASSERT_EQUAL_STRING("", two);
  librarytext::albumSub(accents, 0, AlbumPlace::AZ, three, sizeof(three));
  TEST_ASSERT_EQUAL_STRING("\xC3\x89", three);
}

// The rows keep working with no tags at all (no tags.bin, no transfer: the
// index of paths, as before N9): the folder names, no years, no subtitles
// on tracks; the files right under /music are the "(no artist folder)"
// entity; a path-only album with "1-01" names still gets its dividers.
void test_library_rows_from_paths() {
  using librarytext::AlbumPlace;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (const char* p : {"/music/Glass Orchard/Morning Set/01 - Opening.mp3", "/music/Glass Orchard/Morning Set/02 - Second.mp3",
                        "/music/Glass Orchard/Late Set/1-01 Intro.mp3", "/music/Glass Orchard/Late Set/1-02 Middle.mp3",
                        "/music/Glass Orchard/Late Set/2-01 Outro.mp3", "/music/Glass Orchard/03 - Loose One.mp3",
                        "/music/Top Level.mp3"}) {
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addFile(p));
  }
  TEST_ASSERT_TRUE(idx.finish());
  const uint32_t morning = albumOf(idx, "/music/Glass Orchard/Morning Set/01 - Opening.mp3");
  const uint32_t late = albumOf(idx, "/music/Glass Orchard/Late Set/1-01 Intro.mp3");
  const uint32_t top = albumOf(idx, "/music/Top Level.mp3");
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", albumSub(idx, morning, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("2 tracks", albumSub(idx, morning, AlbumPlace::OfArtist).c_str());
  TEST_ASSERT_EQUAL_STRING("Glass Orchard \xC2\xB7 2 tracks", text(librarytext::albumHeader, idx, morning).c_str());
  TEST_ASSERT_EQUAL_STRING("", trackSub(idx, "/music/Glass Orchard/Morning Set/01 - Opening.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING(uitext::kNoArtistFolder, albumSub(idx, top, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING(uitext::kNoArtistFolder, librarytext::artistShown(idx, idx.album(top).artist));
  TEST_ASSERT_EQUAL_STRING(uitext::kLooseTracks, librarytext::albumShown(idx, top));
  // The rail keys on the names (no sort tags): "The" aside, as before.
  for (uint32_t i = 0; i < idx.artistCount(); ++i) {
    const uint32_t a = idx.artistsAZ()[i];
    TEST_ASSERT_EQUAL_STRING(textfold::sortName(idx.artistName(a)),
                             librarytext::railName(idx, LibraryIndex::View::Artists, a));
  }
  // The catalog: the folder artist, the folder album, no year, no length.
  TrackCatalog c(&idx);
  const uint32_t opening = idx.findTrack("/music/Glass Orchard/Morning Set/01 - Opening.mp3");
  TEST_ASSERT_EQUAL_STRING("Opening", titleOf(c, opening).c_str());
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", c.artist(opening));
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", c.albumArtist(opening));
  TEST_ASSERT_EQUAL_STRING("Morning Set", c.album(opening));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(opening));
  TEST_ASSERT_EQUAL_UINT32(0, c.durationHintMs(opening));
  TEST_ASSERT_EQUAL_STRING("", c.artist(idx.findTrack("/music/Top Level.mp3")));  // Now Playing: kUnknownArtist
  // "1-01", "1-02", "2-01": two discs from the names, a divider each.
  librarytext::Discs d;
  d.set(idx, late);
  TEST_ASSERT_EQUAL_UINT32(2, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(5, d.rows(idx.album(late).trackCount));
  d.set(idx, morning);
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(2, d.rows(2));
}

// An album of two discs: "Disc 1" and "Disc 2" rows before each disc's
// first track, the tracks' places kept; one disc: no rows; discs that
// don't run in order past kMax changes: none at all.
void test_disc_dividers() {
  LibraryIndex idx;
  buildTagged(idx);
  const uint32_t archive = albumOf(idx, "/music/Orchard Hum/Archive/CD1/01 - One.mp3");
  TEST_ASSERT_EQUAL_UINT8(2, idx.album(archive).discs);
  librarytext::Discs d;
  d.set(idx, archive);
  TEST_ASSERT_EQUAL_UINT32(2, d.dividers());
  const LibraryIndex::Span t = idx.tracksOfAlbum(archive);
  TEST_ASSERT_EQUAL_UINT32(6, d.rows(t.count));
  // Rows: Disc 1, One, Two, Disc 2, Three, Four.
  const char* want[6] = {"Disc 1", "One", "Two", "Disc 2", "Three", "Four"};
  TrackCatalog c(&idx);
  for (uint32_t r = 0; r < 6; ++r) {
    const librarytext::Discs::Row row = d.at(r);
    if (row.divider) {
      char b[16];
      librarytext::discText(row.disc, b, sizeof(b));
      TEST_ASSERT_EQUAL_STRING(want[r], b);
    } else {
      TEST_ASSERT_EQUAL_STRING(want[r], titleOf(c, t[row.track]).c_str());
      TEST_ASSERT_EQUAL_UINT32(r, d.rowOf(row.track));
    }
  }
  // One disc: no dividers.
  d.set(idx, albumOf(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3"));
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(3, d.rows(3));
  TEST_ASSERT_FALSE(d.at(0).divider);
  TEST_ASSERT_EQUAL_UINT32(2, d.at(2).track);
  TEST_ASSERT_EQUAL_UINT32(1, d.rowOf(1));
  d.set(idx, LibraryIndex::kNone);
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  // A path-only album whose subfolders each hold a disc 1 and a disc 2
  // ("1-01", "2-01"): sorted folder by folder, the discs alternate, a change
  // per file; past kMax changes, no dividers.
  LibraryIndex alt;
  TEST_ASSERT_TRUE(alt.begin("/music"));
  char p[96];
  for (int f = 0; f < 40; ++f) {
    snprintf(p, sizeof(p), "/music/A/Big/Part %02d/1-01 a.mp3", f);
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, alt.addFile(p));
    snprintf(p, sizeof(p), "/music/A/Big/Part %02d/2-01 b.mp3", f);
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, alt.addFile(p));
  }
  TEST_ASSERT_TRUE(alt.finish());
  const uint32_t big = albumOf(alt, "/music/A/Big/Part 00/1-01 a.mp3");
  TEST_ASSERT_EQUAL_UINT8(2, alt.album(big).discs);
  d.set(alt, big);
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(80, d.rows(80));
}

// The catalog: the tag's title, the track's own artist else the album's
// line, the album's name and year, the record's length; then the overlay
// (the playing track's fresh tags, 3.3.3) for that track in that index
// only, the loose tracks still nameless, gone with clear() and with
// another build.
void test_catalog_names_and_overlay() {
  LibraryIndex idx;
  buildTagged(idx);
  TrackCatalog c(&idx);
  const uint32_t dawn = idx.findTrack("/music/Lantern Choir/First Light/01 - Dawn.mp3");
  const uint32_t dusk = idx.findTrack("/music/Lantern Choir/First Light/03 - Dusk.mp3");
  const uint32_t gale = idx.findTrack("/music/Lantern Choir/Second Wind/01 - Gale.flac");
  const uint32_t sketch = idx.findTrack("/music/Lantern Choir/Demos/01 - Sketch.mp3");
  const uint32_t stray = idx.findTrack("/music/Lantern Choir/04 - Stray.mp3");
  const uint32_t wave = idx.findTrack("/music/Various/Summer Mix/01 - Wave.mp3");
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(dawn));
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Mara Quill", c.artist(dusk));
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(sketch));  // no record: its artist's elected name
  TEST_ASSERT_EQUAL_STRING("Ola Brenmark", c.artist(wave));
  TEST_ASSERT_EQUAL_STRING("Various Artists", c.albumArtist(wave));
  TEST_ASSERT_EQUAL_STRING("First Light", c.album(dawn));
  TEST_ASSERT_EQUAL_STRING("", c.album(stray));
  TEST_ASSERT_EQUAL_UINT16(2001, c.year(dawn));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(stray));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(sketch));
  TEST_ASSERT_EQUAL_UINT32(201000, c.durationHintMs(dawn));
  TEST_ASSERT_EQUAL_UINT32(199000, c.durationHintMs(gale));  // whole seconds, rounded
  TEST_ASSERT_EQUAL_UINT32(0, c.durationHintMs(sketch));
  TEST_ASSERT_EQUAL_UINT32(60000, c.durationHintMs(TrackCatalog::builtins()[3]));  // the click tracks', as before
  // The overlay: the playing track's tags, read after the build.
  TrackCatalog::Overlay o;
  c.setOverlay(&o);
  const uint32_t v0 = c.namesVersion();
  o.set(idx, sketch,
        View().title("Sketch (Final)").artist("The Lantern Choir\x1FGuest Horn").album("Demos 2003").year(2003).ms(123456).v);
  TEST_ASSERT_TRUE(c.namesVersion() != v0);
  TEST_ASSERT_EQUAL_STRING("Sketch (Final)", titleOf(c, sketch).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Guest Horn", c.artist(sketch));
  TEST_ASSERT_EQUAL_STRING("Demos 2003", c.album(sketch));
  TEST_ASSERT_EQUAL_UINT16(2003, c.year(sketch));
  TEST_ASSERT_EQUAL_UINT32(123456, c.durationHintMs(sketch));
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());  // the others: the index's
  // A field the record lacks stays the index's.
  o.set(idx, dawn, View().title("Dawn (Live)").v);
  TEST_ASSERT_EQUAL_STRING("Dawn (Live)", titleOf(c, dawn).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(dawn));
  TEST_ASSERT_EQUAL_STRING("First Light", c.album(dawn));
  TEST_ASSERT_EQUAL_UINT16(2001, c.year(dawn));
  TEST_ASSERT_EQUAL_UINT32(201000, c.durationHintMs(dawn));
  TEST_ASSERT_EQUAL_STRING("Sketch", titleOf(c, sketch).c_str());  // one slot
  // The loose tracks stay nameless (the next build would show them so).
  o.set(idx, stray, View().title("Stray (Edit)").album("Another Single").year(2006).v);
  TEST_ASSERT_EQUAL_STRING("Stray (Edit)", titleOf(c, stray).c_str());
  TEST_ASSERT_EQUAL_STRING("", c.album(stray));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(stray));
  // A field cut to 255 bytes at a character boundary.
  std::string longTitle;
  while (longTitle.size() < 300) longTitle += "\xC3\xA9";  // é, 2 bytes
  o.set(idx, dawn, View().title(longTitle.c_str()).v);
  TEST_ASSERT_EQUAL_UINT32(254, strlen(o.title));
  // Another build renumbers the tracks: the overlay is ignored there.
  o.set(idx, sketch, View().title("Sketch (Final)").v);
  LibraryIndex other;
  TEST_ASSERT_TRUE(other.begin("/music"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, other.addFile("/music/Lantern Choir/Demos/01 - Sketch.mp3"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, other.addFile("/music/Lantern Choir/Demos/00 - Intro.mp3"));
  TEST_ASSERT_TRUE(other.finish());
  TEST_ASSERT_TRUE(other.buildStamp() != idx.buildStamp());
  c.setIndex(&other);
  for (uint32_t i = 0; i < other.trackCount(); ++i) TEST_ASSERT_TRUE(titleOf(c, i) != "Sketch (Final)");
  c.setIndex(&idx);
  TEST_ASSERT_EQUAL_STRING("Sketch (Final)", titleOf(c, sketch).c_str());
  const uint32_t v1 = c.namesVersion();
  o.clear();
  TEST_ASSERT_TRUE(c.namesVersion() != v1);
  TEST_ASSERT_EQUAL_STRING("Sketch", titleOf(c, sketch).c_str());
  c.setOverlay(nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, c.namesVersion());
}

// The A-Z rail, a row's letter and the jump grid key on the sort keys
// (an elected sort tag, else the name): "Daniel Bowery" tagged "Bowery,
// Daniel" is a B, and on the synthetic tagged library (2 % of tracks with
// sort tags) the letters librarytext::railName() gives are the index's
// buckets, row for row, in both lists.
void test_rail_follows_the_sort_keys() {
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Daniel Bowery/Rooms/01 - Hall.mp3",
                                  View().title("Hall").artist("Daniel Bowery").artistSort("Bowery, Daniel").album("Rooms").v));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Ada Crane/The Quiet Year/01 - Snow.mp3",
                                  View().title("Snow").artist("Ada Crane").album("The Quiet Year").albumSort("Quiet Year").v));
  TEST_ASSERT_TRUE(idx.finish());
  const uint32_t bowery = artistNamed(idx, "Daniel Bowery");
  TEST_ASSERT_EQUAL_STRING("Bowery, Daniel", librarytext::railName(idx, LibraryIndex::View::Artists, bowery));
  TEST_ASSERT_EQUAL('B', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Artists, bowery)));
  TEST_ASSERT_EQUAL_UINT32(bowery, idx.artistsAZ()[1]);  // after Ada Crane (A), as a B
  const uint32_t quiet = albumOf(idx, "/music/Ada Crane/The Quiet Year/01 - Snow.mp3");
  TEST_ASSERT_EQUAL('Q', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, quiet)));

  LibraryIndex big;
  const synth::Spec spec = synth::specFor(3000);
  TEST_ASSERT_TRUE(big.begin(spec.root, spec.tracks));
  synth::Tagged t;
  uint32_t sorted = 0;
  for (uint32_t i = 0; i < spec.tracks; ++i) {
    TEST_ASSERT_TRUE(synth::tagged(spec, i, &t));
    if (t.noTags) {
      big.addFile(t.path);
      continue;
    }
    View v;
    v.title(t.title).artist(t.artist).album(t.album).albumArtist(t.albumArtist).year(t.year).track(t.track).disc(t.disc);
    v.artistSort(t.artistSort).albumSort(t.albumSort);
    v.set(&v.v.albumArtistSort, &v.v.albumArtistSortLen, t.albumArtistSort);
    if (t.compilation == 1) v.compilation();
    sorted += t.artistSort[0] || t.albumSort[0] || t.albumArtistSort[0];
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, big.addRecord(t.path, v.v));
  }
  TEST_ASSERT_TRUE(big.finish());
  TEST_ASSERT_TRUE(sorted > 0);
  struct Rows {
    const LibraryIndex* idx;
    LibraryIndex::View view;
    LibraryIndex::Span span;
  };
  for (const LibraryIndex::View view : {LibraryIndex::View::Artists, LibraryIndex::View::Albums}) {
    Rows rows{&big, view, view == LibraryIndex::View::Artists ? big.artistsAZ() : big.albumsAZ()};
    auto name = [](void* ctx, uint32_t row) {
      const Rows& r = *static_cast<Rows*>(ctx);
      return librarytext::railName(*r.idx, r.view, r.span[row]);
    };
    int32_t first[jump::kCells], end[jump::kCells];
    jump::letters(rows.span.count, name, &rows, first, end);
    for (int b = 0; b < jump::kCells; ++b) {
      const uint32_t start = big.bucketStart(view, b);
      const uint32_t stop = big.bucketStart(view, b + 1);
      if (start == stop) {
        TEST_ASSERT_EQUAL_INT32(-1, first[b]);
        continue;
      }
      TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(start), first[b]);
      TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(stop), end[b]);
    }
    for (uint32_t i = 0; i < rows.span.count; ++i) {
      TEST_ASSERT_EQUAL_INT(big.bucketAt(view, i), textfold::bucketOf(textfold::railKey(name(&rows, i))));
    }
  }
}

// The Library's and Now Playing's new texts in their rooms: the empty
// state's lines (reworded: tags are read), the placeholders and "Unknown
// artist" in Now Playing's rows, a disc divider up to disc 255, the scan's
// status line up to 99,999 tracks and its toasts on one line.
void test_library_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(title, kNoMusicTitle, kEmptyTitleW);
  for (const char* l : kNoMusicLines) fits(small, l, kEmptyLineW);
  for (const char* l : kNoCard.lines) fits(small, l, kEmptyLineW);
  for (const char* l : {kUnknownArtist, kNoArtistFolder, kLooseTracks}) fits(body, l, kNowPlayingTextW);
  // The album row with its year: a short album name keeps it.
  fits(body, "First Light \xC2\xB7 2001", kNowPlayingTextW);
  char t[64];
  librarytext::discText(255, t, sizeof(t));
  fits(bold, t, kDiscTextW);
  using P = librarytext::Status::Phase;
  for (const P phase : {P::Checking, P::Reading, P::Updating, P::Unfinished}) {
    librarytext::Status s;
    s.phase = phase;
    s.done = 99999;
    s.total = 99999;
    TEST_ASSERT_TRUE(librarytext::statusText(s, t, sizeof(t)) > 0);
    fits(small, t, kStatusW);
  }
  librarytext::Status s;
  TEST_ASSERT_EQUAL_UINT32(0, librarytext::statusText(s, t, sizeof(t)));  // Idle: no line
  s.phase = P::Reading;
  s.done = 1234;
  s.total = 19410;
  librarytext::statusText(s, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Reading tags 1,234 / 19,410", t);
  librarytext::foundText(1, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Found 1 new track", t);
  fits(body, t, kToastTextRight - kToastTextX);
  librarytext::foundText(99999, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Found 99,999 new tracks", t);
  fits(body, t, kToastTextRight - kToastTextX);
  fits(body, kLibraryUpdated, kToastTextRight - kToastTextX);
  fits(body, kLibraryAtBoot, kToastTextRight - kToastTextX);
  TEST_ASSERT_TRUE(small.hasAll(kStatusChecking) && small.hasAll(kStatusUpdating));
}

// ---- the console's tag commands (tagtext, docs/METADATA.md 3.3.6) ----

namespace {
void collect(void* ctx, const char* line) { static_cast<std::vector<std::string>*>(ctx)->push_back(line); }
bool hasLine(const std::vector<std::string>& lines, const std::string& want) {
  for (const std::string& l : lines) {
    if (l == want) return true;
  }
  return false;
}
}  // namespace

// g's argument: today's g, g0 and g<n>, and the tags' gs, gt, gr, gr!,
// gw, gb, gv; anything else is the help.
void test_console_tag_commands() {
  using C = tagtext::Command;
  TEST_ASSERT_EQUAL(C::Report, tagtext::parse("").command);
  TEST_ASSERT_EQUAL(C::Report, tagtext::parse(nullptr).command);
  TEST_ASSERT_EQUAL(C::Report, tagtext::parse("  ").command);
  TEST_ASSERT_EQUAL(C::Rebuild, tagtext::parse("0").command);
  TEST_ASSERT_EQUAL(C::Rebuild, tagtext::parse("00").command);
  TEST_ASSERT_EQUAL(C::Synthetic, tagtext::parse("12").command);
  TEST_ASSERT_EQUAL_UINT32(12, tagtext::parse("12").n);
  TEST_ASSERT_EQUAL_UINT32(50000, tagtext::parse("50000").n);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("50001").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("99999999999999").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("12a").command);
  TEST_ASSERT_EQUAL(C::Status, tagtext::parse("s").command);
  TEST_ASSERT_EQUAL(C::Rescan, tagtext::parse("r").command);
  TEST_ASSERT_EQUAL(C::RescanAll, tagtext::parse("r!").command);
  TEST_ASSERT_EQUAL(C::Walk, tagtext::parse("w").command);
  TEST_ASSERT_EQUAL(C::Build, tagtext::parse("b").command);
  TEST_ASSERT_EQUAL(C::Verify, tagtext::parse("v").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("x").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("?").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("ss").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("t").command);
  const tagtext::Parsed d = tagtext::parse("t  /music/A B/01 - x.mp3");
  TEST_ASSERT_EQUAL(C::Dump, d.command);
  TEST_ASSERT_EQUAL_STRING("/music/A B/01 - x.mp3", d.path);
  TEST_ASSERT_TRUE(strstr(tagtext::kHelp, "gt</music/...>") != nullptr);
}

// A record as the console prints it: each text field the run has (lists
// as "a" | "b"), the numbers on one line, the ReplayGain, the picture's
// anchor, the flags; an UNREADABLE record in one line. And a real file's
// record (the parity corpus's flac_full.flac, read by TagScan): a line per
// field its run holds.
void test_console_tag_dump() {
  namespace mptg = cardcontract::mptg;
  mptg::Record r;
  r.known = mptg::kKnownRules1;
  r.container = mptg::kContainerMp3;
  r.year = 2001;
  r.track = 3;
  r.trackTotal = 12;
  r.disc = 1;
  r.discTotal = 2;
  r.durationMs = 201250;
  r.bpm10 = 1280;
  r.camelot = 8;
  r.flags = 1 | mptg::kHasRgTrack | mptg::kHasRgAlbum | mptg::kTruncated;  // a compilation
  r.rgTrackGain = -650;
  r.rgTrackPeak = 9876;
  r.rgAlbumGain = 125;
  r.picOffset = 1234;
  r.picLength = 23456;
  r.picType = 3;
  r.picMime = mptg::kMimeJpeg;
  r.picCoding = mptg::kCodingRaw;
  cardcontract::RunFields run;
  run.set(cardcontract::kTitle, "Dawn", 4);
  run.set(cardcontract::kArtist, "Ada Crane\x1FMara Quill", 20);
  run.set(cardcontract::kAlbum, "First Light", 11);
  std::vector<std::string> lines;
  tagtext::dumpRecord(r, &run, collect, &lines);
  TEST_ASSERT_TRUE(hasLine(lines, "  title: \"Dawn\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  artist: \"Ada Crane\" | \"Mara Quill\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  album: \"First Light\""));
  TEST_ASSERT_TRUE(
      hasLine(lines, "  MP3, year 2001, track 3/12, disc 1/2, length 3:21.250, BPM 128.0, key 8A, a compilation"));
  TEST_ASSERT_TRUE(hasLine(lines, "  ReplayGain: track -6.50 dB peak 0.9876, album +1.25 dB"));
  TEST_ASSERT_TRUE(hasLine(lines, "  picture: JPEG, type 3 (front cover), 23,456 B at 1,234 (raw)"));
  TEST_ASSERT_TRUE(hasLine(lines, "  flags: TRUNCATED"));
  for (const std::string& l : lines) TEST_ASSERT_TRUE(l.find("looked for") == std::string::npos);
  // What it looked for, when that isn't everything.
  r.known = mptg::kKnownTitle;
  lines.clear();
  tagtext::dumpRecord(r, &run, collect, &lines);
  TEST_ASSERT_TRUE(hasLine(lines, "  looked for: 0x00001 (rules 1 look for all of 0x1ffff)"));
  // UNREADABLE: one line.
  mptg::Record u;
  u.flags = mptg::kUnreadable;
  lines.clear();
  tagtext::dumpRecord(u, nullptr, collect, &lines);
  TEST_ASSERT_EQUAL_UINT32(1, lines.size());
  // The small pieces.
  char t[64];
  tagtext::fatTimeText(cardcontract::fatTime(2026, 10, 1, 12, 34, 56), t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("2026-10-01 12:34:56", t);
  tagtext::fatTimeText(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("none", t);
  tagtext::lengthText(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("unknown", t);
  TEST_ASSERT_EQUAL_STRING("1B", tagtext::camelotName(13));
  TEST_ASSERT_EQUAL_STRING("12B", tagtext::camelotName(24));
  TEST_ASSERT_EQUAL_STRING("", tagtext::camelotName(25));
  tagtext::flagsText(mptg::kNoTags | mptg::kFromApi, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("NO_TAGS, FROM_API", t);
  // A real file's record.
  const tagfixtures::Bytes bytes = tagfixtures::fileBytes(tagfixtures::dir() + "/flac_full.flac");
  TEST_ASSERT_TRUE(bytes.size() > 0);
  cardcontract::MemSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
  static tagscan::Scanner scanner;  // (about 10 KB: off the stack)
  static uint8_t buf[4096];
  TEST_ASSERT_EQUAL(tagscan::Result::Ok, scanner.scan(src, tagscan::Kind::Flac, buf, sizeof(buf)));
  static cardcontract::RunFields full;
  scanner.record().toRunFields(&full);
  lines.clear();
  tagtext::dumpRecord(scanner.record().rec, &full, collect, &lines);
  static const char* const kNames[cardcontract::kRunFields] = {
      "title", "artist", "album", "album artist", "genre", "composer", "title sort", "artist sort", "album sort",
      "album artist sort", "MusicBrainz album", "MusicBrainz recording"};
  uint32_t fields = 0;
  for (uint32_t f = 0; f < cardcontract::kRunFields; ++f) {
    if (!full.has(f)) continue;
    ++fields;
    bool found = false;
    for (const std::string& l : lines) found = found || l.rfind(std::string("  ") + kNames[f] + ": \"", 0) == 0;
    TEST_ASSERT_TRUE_MESSAGE(found, kNames[f]);
  }
  TEST_ASSERT_EQUAL_UINT32(cardcontract::kRunFields, fields);  // every field (expected.json's)
  TEST_ASSERT_TRUE(hasLine(lines, "  title: \"Lantern Song\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  artist: \"Mike Duo\" | \"November\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  genre: \"Folk\" | \"Acoustic\""));
  TEST_ASSERT_TRUE(
      hasLine(lines, "  FLAC, year 2003, track 3/12, disc 1/2, length 0:30.000, BPM 96.0, key 8B, a compilation"));
  TEST_ASSERT_TRUE(hasLine(lines, "  ReplayGain: track -6.79 dB peak 0.9886, album +1.01 dB peak 1.0000"));
  TEST_ASSERT_TRUE(hasLine(lines, "  picture: JPEG, type 3 (front cover), 96 B at 748 (raw)"));
}

// Where the index's names came from: g's line.
void test_console_sources() {
  LibraryIndex idx;
  buildTagged(idx);
  const tagtext::Sources s = tagtext::countSources(idx);
  TEST_ASSERT_EQUAL_UINT32(13, s.tracks);
  TEST_ASSERT_EQUAL_UINT32(1, s.transfer);
  TEST_ASSERT_EQUAL_UINT32(10, s.device);
  TEST_ASSERT_EQUAL_UINT32(2, s.path);
  TEST_ASSERT_EQUAL_UINT32(1, s.pending);
  char t[192];
  tagtext::sourcesText(s, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING(
      "13 tracks: 1 from the transfer's records, 10 from the device's, 2 by their paths (1 for the scan)", t);
  LibraryIndex paths;
  TEST_ASSERT_TRUE(paths.begin("/music"));
  paths.addFile("/music/A/B/01 - x.mp3");
  TEST_ASSERT_TRUE(paths.finish());
  tagtext::sourcesText(tagtext::countSources(paths), t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("1 track: 1 named by their paths", t);
  LibraryIndex none;
  tagtext::sourcesText(tagtext::countSources(none), t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("0 tracks", t);
}

// The Dance tab's bottom line while a computer drives the dancer (the USB
// visualizer): the title in Bold, the hint in Small, each in kW - 16.
void test_dance_texts_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  fits(bold, kVizTitle, kDanceBottomW);
  fits(small, kVizHint, kDanceBottomW);
  char msg[96];
  snprintf(msg, sizeof(msg), "\"%s\" %d px, \"%s\" %d px, in %d", kVizTitle, bold.width(kVizTitle), kVizHint,
           small.width(kVizHint), kDanceBottomW);
  TEST_MESSAGE(msg);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_jump_letters_match_the_index_buckets);
  RUN_TEST(test_jump_second_level);
  RUN_TEST(test_jump_second_key);
  RUN_TEST(test_jump_second_level_on_a_big_library);
  RUN_TEST(test_jump_grid_cells);
  RUN_TEST(test_list_steps_toward_the_finger);
  RUN_TEST(test_list_steps_in_whole_rows_never_exceed_the_cap);
  RUN_TEST(test_tabbar_texts_fit_in_every_state);
  RUN_TEST(test_coach_texts_fit);
  RUN_TEST(test_toast_names_fit);
  RUN_TEST(test_queue_cap_texts_fit);
  RUN_TEST(test_empty_state_texts_fit);
  RUN_TEST(test_no_card_texts_fit);
  RUN_TEST(test_board_guard_texts_fit);
  RUN_TEST(test_queue_texts_fit);
  RUN_TEST(test_output_texts_fit);
  RUN_TEST(test_waiting_texts_fit);
  RUN_TEST(test_now_playing_menu_texts_fit);
  RUN_TEST(test_seek_bar_texts_fit);
  RUN_TEST(test_sleep_timer_texts_fit);
  RUN_TEST(test_idle_power_off_texts_fit);
  RUN_TEST(test_power_settings_texts_fit);
  RUN_TEST(test_touch_calibration_texts_fit);
  RUN_TEST(test_dance_texts_fit);
  RUN_TEST(test_right_half_neighbours_survive_the_lab_panel);
  RUN_TEST(test_library_rows_from_tags);
  RUN_TEST(test_library_rows_from_paths);
  RUN_TEST(test_disc_dividers);
  RUN_TEST(test_catalog_names_and_overlay);
  RUN_TEST(test_rail_follows_the_sort_keys);
  RUN_TEST(test_library_texts_fit);
  RUN_TEST(test_console_tag_commands);
  RUN_TEST(test_console_tag_dump);
  RUN_TEST(test_console_sources);
  return UNITY_END();
}
