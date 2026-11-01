// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for Auto DJ on the screen (lib/core/AutoDjView: the engine
// design's 5 and 2.14, its milestone M5): the "..." row's state, the
// Output row's line, the page's live and dim rows, Match strictness's
// steps and the value an index's own floor makes of it, the status row's
// two lines in each state of the index and each hold, the empty states'
// button, and which toast Auto DJ's news makes (and when: picks by id once
// a switch on, only picks appended). The widths of these texts
// in every language are test_ui_library's (test_auto_dj_texts_fit) and
// test_ui_strings'.
// Run: pio test -e native -f test_ui_autodj
#include <unity.h>

#include <cstring>
#include <string>

#include "AutoDjTiers.h"
#include "AutoDjView.h"
#include "UiStrings.h"

using namespace autodjview;
using autodj::Hold;
using autodj::Mode;
using uistr::S;

void setUp() { uistr::setLang(uistr::Lang::En); }
void tearDown() { uistr::setLang(uistr::Lang::En); }

namespace {

std::string line(const State& s, int k) {
  char a[96], b[96];
  statusLines(s, a, sizeof(a), b, sizeof(b));
  return k == 0 ? a : b;
}

std::string root(const State& s, bool withMode = true) {
  char b[96];
  return rootLine(s, b, sizeof(b), withMode);
}

}  // namespace

// Off, On, and the holds while on: Repeat One and the sleep timer's End
// of queue wait, the failure pause pauses; a hold left from before it was
// switched off says nothing.
void test_the_row_state() {
  State s;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Off);
  s.hold = Hold::RepeatOne;  // (off: no hold shows)
  TEST_ASSERT_TRUE(rowState(s) == RowState::Off);
  s.on = true;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Waiting);
  s.hold = Hold::SleepTimer;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Waiting);
  s.hold = Hold::Failures;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Paused);
  s.hold = Hold::None;
  TEST_ASSERT_TRUE(rowState(s) == RowState::On);
  TEST_ASSERT_EQUAL_STRING("On", uistr::tr(rowText(RowState::On)));
  TEST_ASSERT_EQUAL_STRING("Off", uistr::tr(rowText(RowState::Off)));
  TEST_ASSERT_EQUAL_STRING("Waiting", uistr::tr(rowText(RowState::Waiting)));
  TEST_ASSERT_EQUAL_STRING("Paused", uistr::tr(rowText(RowState::Paused)));
  // Its own ids, not Shuffle's (the agreement differs in Polish).
  TEST_ASSERT_TRUE(rowText(RowState::On) != S::now_playing_on);
  TEST_ASSERT_TRUE(rowText(RowState::Off) != S::now_playing_off);
}

// The state and the Anchor in use; the state alone when asked (the line
// with the Anchor doesn't fit: OutputPage) or with no usable index (the
// Anchor does nothing then: its picks are random).
void test_the_output_row_line() {
  State s;
  s.usable = true;
  TEST_ASSERT_EQUAL_STRING("Off", root(s).c_str());
  s.mode = Mode::Locked;  // (off: no mode)
  TEST_ASSERT_EQUAL_STRING("Off", root(s).c_str());
  s.on = true;
  TEST_ASSERT_EQUAL_STRING("On \xC2\xB7 Stay on seed", root(s).c_str());
  TEST_ASSERT_EQUAL_STRING("On", root(s, false).c_str());
  s.mode = Mode::Rolling;
  TEST_ASSERT_EQUAL_STRING("On \xC2\xB7 Follow the vibe", root(s).c_str());
  s.hold = Hold::RepeatOne;
  TEST_ASSERT_EQUAL_STRING("Waiting \xC2\xB7 Follow the vibe", root(s).c_str());
  TEST_ASSERT_EQUAL_STRING("Waiting", root(s, false).c_str());
  s.hold = Hold::Failures;
  s.mode = Mode::Locked;
  TEST_ASSERT_EQUAL_STRING("Paused \xC2\xB7 Stay on seed", root(s).c_str());
  TEST_ASSERT_EQUAL_STRING("Paused", root(s, false).c_str());
  // No usable index: the state alone (the "..." row's words), whatever asked.
  s.usable = false;
  TEST_ASSERT_EQUAL_STRING("Paused", root(s).c_str());
  s.hold = Hold::None;
  TEST_ASSERT_EQUAL_STRING("On", root(s).c_str());
  TEST_ASSERT_EQUAL_STRING(uistr::tr(rowText(RowState::On)), root(s).c_str());
  s.on = false;
  TEST_ASSERT_EQUAL_STRING("Off", root(s, false).c_str());
  // A buffer too small: cut, never past it.
  char tiny[6];
  rootLine(s, tiny, sizeof(tiny));
  TEST_ASSERT_TRUE(std::strlen(tiny) < sizeof(tiny));
  TEST_ASSERT_NULL(rootLine(s, nullptr, 8));
}

// Without a usable index the four settings are dim; the switch and the
// status stay live (the design's 5.1).
void test_the_rows_without_an_index_are_dim() {
  State s;
  for (int r = 0; r < kPageRows; ++r) {
    const bool live = rowLive(static_cast<PageRow>(r), s);
    TEST_ASSERT_EQUAL_MESSAGE(r == OnRow || r == StatusRow, live, std::to_string(r).c_str());
  }
  s.usable = true;
  for (int r = 0; r < kPageRows; ++r) TEST_ASSERT_TRUE(rowLive(static_cast<PageRow>(r), s));
  s.usable = false;
  s.on = true;  // (on at random: still dim)
  TEST_ASSERT_FALSE(rowLive(AnchorRow, s));
  TEST_ASSERT_FALSE(rowLive(StrictRow, s));
  TEST_ASSERT_FALSE(rowLive(BpmRow, s));
  TEST_ASSERT_FALSE(rowLive(KeyRow, s));
}

// The device's steps (the design's 2.6): 55-75 by 5, then round; every
// step the settings take (nvslayout::autoDjFloorValid()'s), a value off
// them goes to the next step.
void test_strictness_steps() {
  TEST_ASSERT_EQUAL_UINT8(60, nextFloor(55));
  TEST_ASSERT_EQUAL_UINT8(65, nextFloor(60));
  TEST_ASSERT_EQUAL_UINT8(70, nextFloor(65));
  TEST_ASSERT_EQUAL_UINT8(75, nextFloor(70));
  TEST_ASSERT_EQUAL_UINT8(55, nextFloor(75));
  TEST_ASSERT_EQUAL_UINT8(60, nextFloor(57));
  TEST_ASSERT_EQUAL_UINT8(55, nextFloor(80));
  TEST_ASSERT_EQUAL_UINT8(55, nextFloor(0));
  // Round the loop: five taps come back.
  uint8_t p = 55;
  int seen = 0;
  do {
    p = nextFloor(p);
    ++seen;
    TEST_ASSERT_TRUE(p >= autodj::kFloorMin && p <= autodj::kFloorMax && p % 5 == 0);
  } while (p != 55);
  TEST_ASSERT_EQUAL_INT(5, seen);
  char b[48];
  TEST_ASSERT_EQUAL_STRING("55% or closer", strictnessText(55, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("75% or closer", strictnessText(75, b, sizeof(b)));
}

// A tap takes the step after the value the row shows: under an index's own
// floor (shown above the setting), from the setting the first tap would
// change nothing on the screen.
void test_a_strictness_tap_steps_from_what_is_shown() {
  State s;
  for (uint8_t p = 55; p <= 75; p = static_cast<uint8_t>(p + 5)) {  // (the writers' floor: as nextFloor())
    s.floorPct = p;
    s.floorShown = shownFloor(p, 140);
    TEST_ASSERT_EQUAL_UINT8(nextFloor(p), tapFloor(s));
  }
  // selFloor 160: 55 and 60 show 63 %; a tap goes to 65, which shows 65.
  s.floorPct = 55;
  s.floorShown = shownFloor(55, 160);
  TEST_ASSERT_EQUAL_UINT8(63, s.floorShown);
  TEST_ASSERT_EQUAL_UINT8(65, tapFloor(s));
  TEST_ASSERT_EQUAL_UINT8(65, shownFloor(tapFloor(s), 160));
  s.floorPct = 60;
  TEST_ASSERT_EQUAL_UINT8(65, tapFloor(s));
  // Every tap changes the row, round the loop, whatever the index's floor
  // (one above the last step shows the same at every step: nothing to change).
  for (uint32_t floor = 1; floor <= 255; ++floor) {
    s.floorPct = 55;
    s.floorShown = shownFloor(55, floor);
    if (s.floorShown >= autodj::kFloorMax) continue;
    for (int tap = 0; tap < 5; ++tap) {
      const uint8_t was = s.floorShown;
      s.floorPct = tapFloor(s);
      s.floorShown = shownFloor(s.floorPct, floor);
      TEST_ASSERT_TRUE_MESSAGE(s.floorShown != was, std::to_string(floor).c_str());
      TEST_ASSERT_TRUE(s.floorPct >= autodj::kFloorMin && s.floorPct <= autodj::kFloorMax && s.floorPct % 5 == 0);
    }
  }
}

// The value shown is the one that acts: an index whose own floor is above
// the setting's level gates at that floor (autodj::floorLevel()).
void test_the_strictness_shown_is_the_one_that_acts() {
  // The writers' default floor, 140 (0.549): no step below it, nothing changes.
  for (uint8_t p = 55; p <= 75; p = static_cast<uint8_t>(p + 5)) {
    TEST_ASSERT_EQUAL_UINT8(p, shownFloor(p, 140));
    TEST_ASSERT_EQUAL_UINT8(p, shownFloor(p, 0));
  }
  // A floor of 160 (0.627): 55 and 60 act as 63 %, the smallest percent
  // whose level reaches it; 65 and above as themselves.
  TEST_ASSERT_EQUAL_UINT8(63, shownFloor(55, 160));
  TEST_ASSERT_EQUAL_UINT8(63, shownFloor(60, 160));
  TEST_ASSERT_EQUAL_UINT8(65, shownFloor(65, 160));
  TEST_ASSERT_TRUE(autodj::floorLevel(63, 0) >= 160);
  TEST_ASSERT_TRUE(autodj::floorLevel(62, 0) < 160);
  // Whatever the floor: the setting itself when the floor doesn't raise
  // its level; else the smallest percent at or past the floor.
  for (uint32_t floor = 1; floor <= 255; ++floor) {
    for (uint8_t p = 55; p <= 75; p = static_cast<uint8_t>(p + 5)) {
      const uint8_t shown = shownFloor(p, floor);
      const uint8_t gate = autodj::floorLevel(p, floor);
      if (gate == autodj::floorLevel(p, 0)) {
        TEST_ASSERT_EQUAL_UINT8(p, shown);
        continue;
      }
      TEST_ASSERT_TRUE(shown > p && shown <= 100);
      TEST_ASSERT_TRUE(autodj::floorLevel(shown, 0) >= gate);
      TEST_ASSERT_TRUE(autodj::floorLevel(shown - 1, 0) < gate);
    }
  }
}

void test_the_anchor_texts() {
  TEST_ASSERT_EQUAL_STRING("Follow the vibe", uistr::tr(modeText(Mode::Rolling)));
  TEST_ASSERT_EQUAL_STRING("Stay on seed", uistr::tr(modeText(Mode::Locked)));
  TEST_ASSERT_EQUAL_STRING("each pick follows the last few tracks", uistr::tr(anchorHint(Mode::Rolling)));
  TEST_ASSERT_EQUAL_STRING("every pick stays close to the seed song", uistr::tr(anchorHint(Mode::Locked)));
}

// Each state of the index, its two lines.
void test_the_status_row() {
  State s;
  TEST_ASSERT_EQUAL_STRING("No Auto DJ index on this card", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("picks are random", line(s, 1).c_str());
  s.index = Index::Bad;
  TEST_ASSERT_EQUAL_STRING("The Auto DJ index failed its checks", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("picks are random", line(s, 1).c_str());
  s.index = Index::Unreadable;  // (a read failed: not the index's checks)
  TEST_ASSERT_EQUAL_STRING("Can't read the Auto DJ index", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("picks are random", line(s, 1).c_str());
  s.index = Index::TooBig;
  TEST_ASSERT_EQUAL_STRING("Too many tracks for the Auto DJ index", line(s, 0).c_str());
  s.index = Index::NoMemory;
  TEST_ASSERT_EQUAL_STRING("Not enough memory for the Auto DJ index", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("picks are random", line(s, 1).c_str());
  s.index = Index::Found;
  s.rows = 19410;
  TEST_ASSERT_EQUAL_STRING("Auto DJ index: 19,410 tracks", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("read when Auto DJ is switched on", line(s, 1).c_str());
  s.rows = 1;
  TEST_ASSERT_EQUAL_STRING("Auto DJ index: 1 track", line(s, 0).c_str());
  s.on = true;
  s.index = Index::Reading;
  TEST_ASSERT_EQUAL_STRING("Reading the Auto DJ index\xE2\x80\xA6", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("", line(s, 1).c_str());
  s.index = Index::Ready;
  s.mapped = 19000;
  s.tracks = 19410;
  s.bpmPct = 92;
  s.keyPct = 88;
  TEST_ASSERT_EQUAL_STRING("19,000 of 19,410 tracks have neighbours", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 92% \xC2\xB7 key 88%", line(s, 1).c_str());
  s.mapped = 1;
  s.tracks = 1;  // (the count is the library's)
  TEST_ASSERT_EQUAL_STRING("1 of 1 track has neighbours", line(s, 0).c_str());
  s.bpmPct = 200;  // (never over 100)
  TEST_ASSERT_EQUAL_STRING("BPM 100% \xC2\xB7 key 88%", line(s, 1).c_str());
}

// Held: why over the index's first line.
void test_the_status_row_says_why_it_waits() {
  State s;
  s.on = true;
  s.index = Index::Ready;
  s.mapped = 300;
  s.tracks = 310;
  s.hold = Hold::RepeatOne;
  TEST_ASSERT_EQUAL_STRING("Waiting: Repeat One is on", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("300 of 310 tracks have neighbours", line(s, 1).c_str());
  s.hold = Hold::SleepTimer;
  TEST_ASSERT_EQUAL_STRING("Waiting: sleep timer set to End of queue", line(s, 0).c_str());
  s.hold = Hold::Failures;
  s.index = Index::None;
  TEST_ASSERT_EQUAL_STRING("Paused: tracks failed to play", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("No Auto DJ index on this card", line(s, 1).c_str());
  // Off: a hold says nothing.
  s.on = false;
  TEST_ASSERT_EQUAL_STRING("No Auto DJ index on this card", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("picks are random", line(s, 1).c_str());
  // Small buffers: cut, never past them.
  char a[8], b[8];
  statusLines(s, a, sizeof(a), b, sizeof(b));
  TEST_ASSERT_TRUE(std::strlen(a) < sizeof(a) && std::strlen(b) < sizeof(b));
}

// D4: Auto DJ in Shuffle all's place only with a usable index and a
// library (on or off).
void test_the_empty_states_button() {
  State s;
  TEST_ASSERT_FALSE(emptyButton(s, 100));
  s.usable = true;
  TEST_ASSERT_TRUE(emptyButton(s, 100));
  TEST_ASSERT_FALSE(emptyButton(s, 0));
  s.on = true;
  TEST_ASSERT_TRUE(emptyButton(s, 100));
  s.index = Index::Found;  // (whatever the join: usable says)
  TEST_ASSERT_TRUE(emptyButton(s, 1));
}

// One toast a pass, the most pressing; none for no news.
void test_the_news_toasts() {
  Toast t{S::toast_undo, false};
  TEST_ASSERT_FALSE(newsToast(0, &t));
  TEST_ASSERT_FALSE(newsToast(kNewsCued, nullptr));
  TEST_ASSERT_TRUE(newsToast(kNewsCued, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_cued && !t.warn);
  TEST_ASSERT_TRUE(newsToast(kNewsPaused, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_paused && t.warn);
  TEST_ASSERT_TRUE(newsToast(kNewsNoNeighbours, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_no_neighbours && !t.warn);
  TEST_ASSERT_TRUE(newsToast(kNewsSeedMoved, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_seed_moved);
  TEST_ASSERT_TRUE(newsToast(kNewsRandom, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_random);
  // Several at once: the pause, then the cue, then the seed, then no index.
  TEST_ASSERT_TRUE(newsToast(kNewsPaused | kNewsCued | kNewsRandom, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_paused);
  TEST_ASSERT_TRUE(newsToast(kNewsCued | kNewsNoNeighbours | kNewsRandom, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_cued);
  TEST_ASSERT_TRUE(newsToast(kNewsNoNeighbours | kNewsRandom, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_no_neighbours);
  TEST_ASSERT_EQUAL_STRING("Auto DJ: the next track is ready, press play", uistr::tr(S::toast_autodj_cued));
}

namespace {

autodj::Pass picked(bool random, bool noIndex) {
  autodj::Pass p;
  p.picked = true;
  p.pick.ok = true;
  p.pick.random = random;
  p.pick.noIndex = noIndex;
  p.added = true;
  return p;
}

}  // namespace

// What app/AutoDj says from the Driver's passes: the first pick of a
// session random (its track has no row) once, by the mode; picks by id
// once a switch on or a boot, not once a session (every Play is one: a
// card with no index would say it at every album's end; said at the switch
// when there is no usable index); a cued pick; the failure pause only
// while on; only picks appended (one the queue refused is tried again);
// its appends counted.
void test_the_news_from_the_passes() {
  NewsWatch w;
  w.sessionBegan();
  // A session's first pick from a track with no row: random.
  TEST_ASSERT_EQUAL_UINT8(kNewsNoNeighbours, w.pass(picked(true, false), Mode::Rolling, true));
  // Later random picks (a damaged slot): nothing said.
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  // A new session, Stay on seed: the first pick becomes the seed.
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsSeedMoved, w.pass(picked(true, false), Mode::Locked, true));
  // A first pick that isn't random: nothing.
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Locked, true));
  // A pick that gave no track doesn't count as the first.
  w.sessionBegan();
  autodj::Pass none;
  none.picked = true;
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(none, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsNoNeighbours, w.pass(picked(true, false), Mode::Rolling, true));
  // No index (on after a boot): picks by id, said once (not as a seed's
  // news), and not again at the next sessions' (Plays').
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.pass(picked(true, true), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Rolling, true));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Locked, true));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Rolling, true));
  // Switched on with no usable index: said at once, not again at the first
  // pick, nor at the next sessions'.
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.switchedOn(false));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Rolling, true));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Rolling, true));
  // Switched on with one: nothing; if it goes (a remount), the picks by id
  // are said once.
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.switchedOn(true));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.pass(picked(true, true), Mode::Rolling, true));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Rolling, true));
  // A first pick the queue refused (full of unplayed tracks, no memory):
  // nothing said, nothing counted; the retry that lands speaks.
  w.sessionBegan();
  autodj::Pass refusedFirst = picked(true, false);
  refusedFirst.added = false;
  refusedFirst.refused = true;
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(refusedFirst, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsNoNeighbours, w.pass(picked(true, false), Mode::Rolling, true));
  w.switchedOn(true);
  w.sessionBegan();
  autodj::Pass refusedById = picked(true, true);
  refusedById.added = false;
  refusedById.refused = true;
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(refusedById, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.pass(picked(true, true), Mode::Rolling, true));
  // A cued pick (a finished queue), with its news.
  autodj::Pass cued = picked(false, false);
  cued.cued = true;
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsCued, w.pass(cued, Mode::Rolling, true));
  // The failure pause: said while on.
  autodj::Pass failed;
  failed.failed = failed.failedPick = failed.paused = true;
  TEST_ASSERT_EQUAL_UINT8(kNewsPaused, w.pass(failed, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(failed, Mode::Rolling, false));
  // The appends counted (picked() appends; the failure, the empty pick and
  // the refused ones don't).
  TEST_ASSERT_EQUAL_UINT32(17, w.adds());
  autodj::Pass refused = picked(false, false);
  refused.added = false;
  refused.refused = true;
  w.pass(refused, Mode::Rolling, true);
  TEST_ASSERT_EQUAL_UINT32(17, w.adds());
}

void test_the_index_names() {
  for (Index i : {Index::None, Index::Bad, Index::Unreadable, Index::TooBig, Index::NoMemory, Index::Found,
                  Index::Reading, Index::Ready}) {
    TEST_ASSERT_TRUE(std::strcmp(indexName(i), "?") != 0);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_row_state);
  RUN_TEST(test_the_output_row_line);
  RUN_TEST(test_the_rows_without_an_index_are_dim);
  RUN_TEST(test_strictness_steps);
  RUN_TEST(test_a_strictness_tap_steps_from_what_is_shown);
  RUN_TEST(test_the_strictness_shown_is_the_one_that_acts);
  RUN_TEST(test_the_anchor_texts);
  RUN_TEST(test_the_status_row);
  RUN_TEST(test_the_status_row_says_why_it_waits);
  RUN_TEST(test_the_empty_states_button);
  RUN_TEST(test_the_news_toasts);
  RUN_TEST(test_the_news_from_the_passes);
  RUN_TEST(test_the_index_names);
  return UNITY_END();
}
