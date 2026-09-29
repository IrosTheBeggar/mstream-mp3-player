// Host tests for the Library screens' portable pieces: the A-Z jump grid
// and its second level (JumpIndex, textfold::secondKey), a list's step a
// frame (ListLayout::stepToward), and every text the tab bar can show
// measured with the real fonts (the VLW DejaVu data the firmware draws
// with) against the room TabBarModel gives it.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "IdlePolicy.h"
#include "JumpIndex.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "ListLayout.h"
#include "OutputModel.h"
#include "PowerChoices.h"
#include "QueueView.h"
#include "ScreenPower.h"
#include "SleepTimer.h"
#include "TabBarModel.h"
#include "TextFit.h"
#include "TextFold.h"
#include "UiText.h"

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
  // digits and "The " included.
  LibraryIndex idx;
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  std::vector<std::string> names;
  for (uint32_t i = 0; i < idx.artistCount(); ++i) names.push_back(idx.artistName(idx.artistsAZ()[i]));
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
  for (uint32_t i = 0; i < idx.albumCount(); ++i) names.push_back(idx.albumName(idx.albumsAZ()[i]));
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

// The empty states' two buttons (the empty queue, Nothing playing) and line.
void test_empty_state_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  TEST_ASSERT_TRUE(bold.width("Open Library") + kEmptyIconW[0] + 8 <= kEmptyPrimaryW - 12);
  TEST_ASSERT_TRUE(body.width("Shuffle all") + kEmptyIconW[1] + 8 <= kEmptySecondW - 12);
  TEST_ASSERT_TRUE(kEmptyPrimaryX + kEmptyPrimaryW < kEmptySecondX && kEmptySecondX + kEmptySecondW <= 320 - 6);
  fits(small, kPickInLibrary, kEmptyLineW);
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
}

// Now Playing while play waits for the headphones (PlayGate): the panel's
// lines and buttons, the notice's buttons, and the output line that says
// they aren't connected.
void test_waiting_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  const int panelW = 320 - 112;  // the artist and album bands
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

// The sleep timer (ENERGY.md section 3): the "..." row and its state, the
// sheet's pills, the fade's toast, and the moon's text on Now Playing's
// progress line.
void test_sleep_timer_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  // The "..." sheet's row (Body from x 16), its state right-aligned (Small)
  // in what the label leaves (Sheet::render: 16 px each side and between).
  const int stateRoom = 320 - 16 - (16 + body.width(kSleepRow) + 16);
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
  // The dialog's body: 3 lines of Small over 260 px, none cut.
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  char lines[4][96];
  const int n = textfit::wrap(f, kCpuDialogBody, strlen(kCpuDialogBody), kDialogTitleW, 3, &lines[0][0], sizeof(lines[0]));
  TEST_ASSERT_TRUE(n <= 3);
  std::string joined;
  for (int i = 0; i < n; ++i) joined += std::string(i ? " " : "") + lines[i];
  TEST_ASSERT_EQUAL_STRING(kCpuDialogBody, joined.c_str());
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
  RUN_TEST(test_empty_state_texts_fit);
  RUN_TEST(test_queue_texts_fit);
  RUN_TEST(test_output_texts_fit);
  RUN_TEST(test_waiting_texts_fit);
  RUN_TEST(test_sleep_timer_texts_fit);
  RUN_TEST(test_idle_power_off_texts_fit);
  RUN_TEST(test_power_settings_texts_fit);
  return UNITY_END();
}
