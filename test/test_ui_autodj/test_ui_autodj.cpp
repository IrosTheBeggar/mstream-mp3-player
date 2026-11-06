// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for Auto DJ on the screen (lib/core/AutoDjView: the engine
// design's 5 and 2.14, its milestone M5): the "..." row's state and its
// detail under a mood filter, the Output row's line, the page's live and
// dim rows (Smart filters by the card's mood data, the rest by the index),
// Smart filters' value, Match strictness's steps and the value an index's
// own floor makes of it, the status row's two lines in each state of the
// index and each hold, with a mood filter on and off, and which are amber,
// the empty states' button (and none when the filter fits no track), and
// which toast Auto DJ's news makes (and when: picks by id once a switch
// on, only picks appended; none of the old ones for a mood filter's jumps,
// repeats and re-pins, nor "no index" for its picks by id while joined),
// with a mood filter's own three below them (MOOD-FILTERS.md 4.5): few
// tracks that fit, with its count, once until a change; a jump, once a
// session; a pick up next that plays once; each raised only with nothing
// above it in that take, Few and Jump then tried again, Once gone (no
// index and 37 fitting: Random at the first pick, Few at the next). The
// widths of these texts in every language are test_ui_library's
// (test_auto_dj_texts_fit) and test_ui_strings'.
// Run: pio test -e native -f test_ui_autodj
#include <unity.h>

#include <cstring>
#include <string>

#include "AutoDjTiers.h"
#include "AutoDjView.h"
#include "MoodFilter.h"
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

// Relaxed and Instrumental (MoodFilter.h's q).
uint16_t relaxedInstrumental() {
  moodfilter::Filter f;
  f.energy = moodfilter::Scale::First;
  f.voice = moodfilter::Voice::Instrumental;
  return f.pack();
}

// A library of 19,412 tracks, 92 % with mood words, the index read, and a
// filter on that `fit` of them fit.
State filtered(uint32_t fit) {
  State s;
  s.usable = true;
  s.index = Index::Ready;
  s.mapped = 19000;
  s.tracks = 19412;
  s.bpmPct = 95;
  s.keyPct = 97;
  s.moods = relaxedInstrumental();
  s.moodData = s.moodOn = true;
  s.fit = fit;
  s.fitOf = 19412;
  s.moodPct = 92;
  return s;
}

std::string detail(const State& s, Ink* ink = nullptr, size_t size = 96) {
  char b[96];
  const Ink k = sheetDetail(s, b, size);
  if (ink) *ink = k;
  return b;
}

std::string value(const State& s) {
  char b[200];
  return filterValue(s, b, sizeof(b));
}

}  // namespace

// Off, On, and the holds while on: Repeat One, the sleep timer's End of
// queue and a mood filter nothing fits wait, the failure pause pauses; a
// hold left from before it was switched off says nothing.
void test_the_row_state() {
  State s;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Off);
  s.hold = Hold::RepeatOne;  // (off: no hold shows)
  TEST_ASSERT_TRUE(rowState(s) == RowState::Off);
  s.on = true;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Waiting);
  s.hold = Hold::SleepTimer;
  TEST_ASSERT_TRUE(rowState(s) == RowState::Waiting);
  s.hold = Hold::NoFit;
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
  s.hold = Hold::NoFit;
  TEST_ASSERT_EQUAL_STRING("Waiting \xC2\xB7 Follow the vibe", root(s).c_str());
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
// status stay live (the design's 5.1). Smart filters, the fourth row, is
// live exactly while the card has mood data, whatever the index
// (MOOD-FILTERS.md 4.4: with none, its picks are random inside the filter;
// without mood data it is dim, a filter set before kept, D4).
void test_the_rows_without_an_index_are_dim() {
  TEST_ASSERT_EQUAL_INT(7, kPageRows);
  TEST_ASSERT_EQUAL_INT(3, FilterRow);  // (the fourth row, after Match strictness)
  State s;
  for (int r = 0; r < kPageRows; ++r) {
    const bool live = rowLive(static_cast<PageRow>(r), s);
    TEST_ASSERT_EQUAL_MESSAGE(r == OnRow || r == StatusRow, live, std::to_string(r).c_str());
  }
  s.usable = true;  // (no mood data: Smart filters alone dim)
  for (int r = 0; r < kPageRows; ++r) {
    TEST_ASSERT_EQUAL_MESSAGE(r != FilterRow, rowLive(static_cast<PageRow>(r), s), std::to_string(r).c_str());
  }
  s.moodData = true;
  for (int r = 0; r < kPageRows; ++r) TEST_ASSERT_TRUE(rowLive(static_cast<PageRow>(r), s));
  s.usable = false;
  s.on = true;  // (on at random: still dim, but for Smart filters)
  TEST_ASSERT_FALSE(rowLive(AnchorRow, s));
  TEST_ASSERT_FALSE(rowLive(StrictRow, s));
  TEST_ASSERT_FALSE(rowLive(BpmRow, s));
  TEST_ASSERT_FALSE(rowLive(KeyRow, s));
  TEST_ASSERT_TRUE(rowLive(FilterRow, s));
  // A filter set with no mood data stays dim (its picks unfiltered).
  s.moodData = false;
  s.moods = relaxedInstrumental();
  TEST_ASSERT_FALSE(rowLive(FilterRow, s));
}

// Smart filters' value: no mood data, then nothing set (a level alone
// filters nothing), then what is set, in the summary's order.
void test_the_filter_value() {
  State s;
  TEST_ASSERT_EQUAL_STRING("No mood data", value(s).c_str());
  s.moods = relaxedInstrumental();  // (kept, unused: D4)
  TEST_ASSERT_EQUAL_STRING("No mood data", value(s).c_str());
  s.moodData = true;
  TEST_ASSERT_EQUAL_STRING("Relaxed \xC2\xB7 Instrumental", value(s).c_str());
  s.moods = 0;
  TEST_ASSERT_EQUAL_STRING("Any", value(s).c_str());
  moodfilter::Filter level;
  level.level = moodfilter::Level::Strict;
  s.moods = level.pack();
  TEST_ASSERT_EQUAL_STRING("Any", value(s).c_str());
  moodfilter::Filter f;
  f.valence = moodfilter::Scale::Second;
  f.tags = moodfilter::kParty | moodfilter::kAcoustic;
  f.level = moodfilter::Level::Loose;
  s.moods = f.pack();
  TEST_ASSERT_EQUAL_STRING("Sad \xC2\xB7 Acoustic \xC2\xB7 Party \xC2\xB7 Loose", value(s).c_str());
  s.moods = 0x800;  // (an invalid q reads as none)
  TEST_ASSERT_EQUAL_STRING("Any", value(s).c_str());
  // A buffer too small: whole words, never past it.
  s.moods = f.pack();
  char b[12];
  TEST_ASSERT_EQUAL_STRING("Sad", filterValue(s, b, sizeof(b)));
  TEST_ASSERT_NULL(filterValue(s, nullptr, 8));
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
  // (On, with no library to join: an update step failed, and library.idx with it.)
  TEST_ASSERT_EQUAL_STRING("Auto DJ index: 1 track", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("", line(s, 1).c_str());
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
  s.hold = Hold::NoFit;  // (MOOD-FILTERS.md 4.4, 6.5: over the coverage, the index read)
  TEST_ASSERT_EQUAL_STRING("Waiting: no track fits the filters", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 0% \xC2\xB7 key 0%", line(s, 1).c_str());
  s.index = Index::Reading;  // (not read: over the index's first line)
  TEST_ASSERT_EQUAL_STRING("Waiting: no track fits the filters", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Reading the Auto DJ index\xE2\x80\xA6", line(s, 1).c_str());
  s.index = Index::Ready;
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
// library (on or off), and a track inside its mood filter: with none
// (MOOD-FILTERS.md 4.4), the empty states say so and show no second
// button.
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
  TEST_ASSERT_FALSE(emptyNoFit(s));
  // A filter nothing fits: no button, on or off, whatever the index.
  s = filtered(0);
  TEST_ASSERT_TRUE(emptyNoFit(s));
  TEST_ASSERT_FALSE(emptyButton(s, 100));
  s.on = true;
  s.hold = Hold::NoFit;
  TEST_ASSERT_TRUE(emptyNoFit(s));
  TEST_ASSERT_FALSE(emptyButton(s, 100));
  s.usable = false;
  s.index = Index::None;
  TEST_ASSERT_TRUE(emptyNoFit(s));
  // One track fits: the button.
  s = filtered(1);
  TEST_ASSERT_FALSE(emptyNoFit(s));
  TEST_ASSERT_TRUE(emptyButton(s, 100));
  // A filter set with no mood data (its picks unfiltered, D4), or none
  // set: the count says nothing.
  s = filtered(0);
  s.moodOn = false;
  TEST_ASSERT_FALSE(emptyNoFit(s));
  TEST_ASSERT_TRUE(emptyButton(s, 100));
}

// The status row with mood data and no filter on: the coverage gains the
// tracks with mood words; with no mood data, as before byte for byte.
void test_the_status_row_with_mood_data() {
  State s = filtered(19412);
  s.moodOn = false;
  s.moods = 0;
  TEST_ASSERT_EQUAL_STRING("19,000 of 19,412 tracks have neighbours", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 95% \xC2\xB7 key 97% \xC2\xB7 mood 92%", line(s, 1).c_str());
  s.moodPct = 200;  // (never over 100)
  TEST_ASSERT_EQUAL_STRING("BPM 95% \xC2\xB7 key 97% \xC2\xB7 mood 100%", line(s, 1).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  // A filter set on a card with no mood data (D4): as with none.
  s.moodData = false;
  s.moods = relaxedInstrumental();
  TEST_ASSERT_EQUAL_STRING("19,000 of 19,412 tracks have neighbours", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 95% \xC2\xB7 key 97%", line(s, 1).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  // Mood data and no index: as before (no coverage without a join).
  s = filtered(19412);
  s.moodOn = false;
  s.index = Index::None;
  TEST_ASSERT_EQUAL_STRING("No Auto DJ index on this card", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("picks are random", line(s, 1).c_str());
}

// A filter on, the index read (MOOD-FILTERS.md 4.4): over 50 tracks fit,
// the count over the coverage; 1 to 50 (the cooldown: they repeat), amber
// "Only 37 tracks fit" over the repeat and BPM and key; none, waiting,
// amber, over the coverage (and while off, the count at 0, amber). Other
// holds as before: why over the index's first line. Found and Reading say
// what the index does.
void test_the_status_row_under_a_filter() {
  State s = filtered(1204);
  TEST_ASSERT_EQUAL_STRING("1,204 of 19,412 tracks fit", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 95% \xC2\xB7 key 97% \xC2\xB7 mood 92%", line(s, 1).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  s.on = true;
  TEST_ASSERT_EQUAL_STRING("1,204 of 19,412 tracks fit", line(s, 0).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  s.fit = 51;
  TEST_ASSERT_EQUAL_STRING("51 of 19,412 tracks fit", line(s, 0).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  s.fit = autodj::kTrackCooldown;
  TEST_ASSERT_EQUAL_STRING("Only 50 tracks fit", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("They will repeat \xC2\xB7 BPM 95% \xC2\xB7 key 97%", line(s, 1).c_str());
  TEST_ASSERT_TRUE(statusAmber(s));
  s.fit = 37;
  TEST_ASSERT_EQUAL_STRING("Only 37 tracks fit", line(s, 0).c_str());
  TEST_ASSERT_TRUE(statusAmber(s));
  s.fit = 1;
  TEST_ASSERT_EQUAL_STRING("Only 1 track fits", line(s, 0).c_str());
  TEST_ASSERT_TRUE(statusAmber(s));
  // None: Waiting (the hold), its root line too.
  s.fit = 0;
  s.hold = Hold::NoFit;
  TEST_ASSERT_EQUAL_STRING("Waiting: no track fits the filters", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 95% \xC2\xB7 key 97% \xC2\xB7 mood 92%", line(s, 1).c_str());
  TEST_ASSERT_TRUE(statusAmber(s));
  TEST_ASSERT_EQUAL_STRING("Waiting \xC2\xB7 Follow the vibe", root(s).c_str());
  // Off: no hold, the count at 0.
  s.on = false;
  TEST_ASSERT_EQUAL_STRING("0 of 19,412 tracks fit", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("BPM 95% \xC2\xB7 key 97% \xC2\xB7 mood 92%", line(s, 1).c_str());
  TEST_ASSERT_TRUE(statusAmber(s));
  // The other holds: why over the index's first line, as before; amber
  // only for the failure pause.
  s = filtered(37);
  s.on = true;
  s.hold = Hold::RepeatOne;
  TEST_ASSERT_EQUAL_STRING("Waiting: Repeat One is on", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("19,000 of 19,412 tracks have neighbours", line(s, 1).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  s.hold = Hold::SleepTimer;
  TEST_ASSERT_FALSE(statusAmber(s));
  s.hold = Hold::Failures;
  TEST_ASSERT_EQUAL_STRING("Paused: tracks failed to play", line(s, 0).c_str());
  TEST_ASSERT_TRUE(statusAmber(s));
  // Found and Reading: as with no filter, never amber.
  s.hold = Hold::None;
  s.index = Index::Reading;
  TEST_ASSERT_EQUAL_STRING("Reading the Auto DJ index\xE2\x80\xA6", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("", line(s, 1).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  s.on = false;
  s.index = Index::Found;
  s.rows = 19410;
  TEST_ASSERT_EQUAL_STRING("Auto DJ index: 19,410 tracks", line(s, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("read when Auto DJ is switched on", line(s, 1).c_str());
  TEST_ASSERT_FALSE(statusAmber(s));
  // Small buffers: cut, never past them.
  s = filtered(1204);
  char a[8], b[8];
  statusLines(s, a, sizeof(a), b, sizeof(b));
  TEST_ASSERT_TRUE(std::strlen(a) < sizeof(a) && std::strlen(b) < sizeof(b));
}

// A filter on with no index Auto DJ can use (None, Bad, Unreadable,
// TooBig, NoMemory: its picks random inside the filter): the count, few or
// waiting line over the index's first line, the held form. No coverage
// (the shares come from a join), no new text.
void test_the_status_row_under_a_filter_with_no_index() {
  for (Index ix : {Index::None, Index::Bad, Index::Unreadable, Index::TooBig, Index::NoMemory}) {
    State s = filtered(1204);
    s.usable = false;
    s.index = ix;
    s.mapped = s.tracks = 0;
    s.bpmPct = s.keyPct = 0;
    State plain;
    plain.index = ix;
    const std::string first = line(plain, 0);
    TEST_ASSERT_EQUAL_STRING("1,204 of 19,412 tracks fit", line(s, 0).c_str());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), line(s, 1).c_str());
    TEST_ASSERT_FALSE(statusAmber(s));
    s.fit = 37;
    TEST_ASSERT_EQUAL_STRING("Only 37 tracks fit", line(s, 0).c_str());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), line(s, 1).c_str());
    TEST_ASSERT_TRUE(statusAmber(s));
    s.fit = 0;
    TEST_ASSERT_EQUAL_STRING("0 of 19,412 tracks fit", line(s, 0).c_str());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), line(s, 1).c_str());
    TEST_ASSERT_TRUE(statusAmber(s));
    s.on = true;
    s.hold = Hold::NoFit;
    TEST_ASSERT_EQUAL_STRING("Waiting: no track fits the filters", line(s, 0).c_str());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), line(s, 1).c_str());
    TEST_ASSERT_TRUE(statusAmber(s));
    TEST_ASSERT_EQUAL_STRING("Waiting", root(s).c_str());  // (no usable index: the state alone)
  }
  TEST_ASSERT_EQUAL_STRING("No Auto DJ index on this card", line(State{}, 0).c_str());
}

// The status row's first line in amber: the failure pause, as before; a
// filter nothing fits; 1 to 50 fitting. Never with no filter on.
void test_the_status_amber() {
  State s;
  TEST_ASSERT_FALSE(statusAmber(s));
  s.on = true;
  s.hold = Hold::Failures;
  TEST_ASSERT_TRUE(statusAmber(s));
  s.hold = Hold::RepeatOne;
  TEST_ASSERT_FALSE(statusAmber(s));
  s.hold = Hold::NoFit;
  TEST_ASSERT_TRUE(statusAmber(s));
  s.on = false;  // (off: no hold shows)
  TEST_ASSERT_FALSE(statusAmber(s));
  for (Index ix : {Index::None, Index::Ready}) {
    s = filtered(10);
    s.index = ix;
    s.moodOn = false;  // (a filter set with no mood data, or none: the count says nothing)
    TEST_ASSERT_FALSE(statusAmber(s));
    s.moodOn = true;
    TEST_ASSERT_TRUE(statusAmber(s));
  }
}

// The "..." sheet's Auto DJ detail: on with a filter on, what is set after
// the state, in the accent; waiting for want of a track that fits, amber;
// else the state alone, dim, as every other row's.
void test_the_sheet_detail() {
  Ink ink = Ink::Accent;
  State s;
  TEST_ASSERT_EQUAL_STRING("Off", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Dim);
  s.on = true;
  TEST_ASSERT_EQUAL_STRING("On", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Dim);
  s = filtered(1204);
  TEST_ASSERT_EQUAL_STRING("Off", detail(s, &ink).c_str());  // (off: the filter does nothing)
  TEST_ASSERT_TRUE(ink == Ink::Dim);
  s.on = true;
  TEST_ASSERT_EQUAL_STRING("On \xC2\xB7 Relaxed \xC2\xB7 Instrumental", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Accent);
  s.fit = 37;  // (few: still on)
  TEST_ASSERT_EQUAL_STRING("On \xC2\xB7 Relaxed \xC2\xB7 Instrumental", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Accent);
  s.fit = 0;
  s.hold = Hold::NoFit;
  TEST_ASSERT_EQUAL_STRING("Waiting", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Amber);
  s.hold = Hold::RepeatOne;
  TEST_ASSERT_EQUAL_STRING("Waiting", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Dim);
  s.hold = Hold::Failures;
  TEST_ASSERT_EQUAL_STRING("Paused", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Dim);
  // A filter set with no mood data (D4): its picks unfiltered, "On", dim.
  s = filtered(1204);
  s.on = true;
  s.moodData = s.moodOn = false;
  TEST_ASSERT_EQUAL_STRING("On", detail(s, &ink).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Dim);
  // A short buffer: whole words after "On · ", the state alone when none
  // fits; never past it.
  s.moodData = s.moodOn = true;
  TEST_ASSERT_EQUAL_STRING("On \xC2\xB7 Relaxed", detail(s, &ink, 20).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Accent);
  TEST_ASSERT_EQUAL_STRING("On", detail(s, &ink, 8).c_str());
  TEST_ASSERT_TRUE(ink == Ink::Accent);
  char tiny[3];
  sheetDetail(s, tiny, sizeof(tiny));
  TEST_ASSERT_TRUE(std::strlen(tiny) < sizeof(tiny));
  TEST_ASSERT_TRUE(sheetDetail(s, nullptr, 8) == Ink::Dim);
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
  TEST_ASSERT_EQUAL_UINT32(0, t.count);
  // A mood filter's (MOOD-FILTERS.md 4.5): few that fit, a note with its
  // count; a pick that plays once; no close match.
  TEST_ASSERT_TRUE(newsToast(kNewsFew, &t, 37));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_few && t.warn);
  TEST_ASSERT_EQUAL_UINT32(37, t.count);
  TEST_ASSERT_TRUE(newsToast(kNewsOnce, &t, 37));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_plays_once && !t.warn);
  TEST_ASSERT_EQUAL_UINT32(0, t.count);
  TEST_ASSERT_TRUE(newsToast(kNewsJump, &t));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_no_close && !t.warn);
  TEST_ASSERT_EQUAL_UINT32(0, t.count);
  // Below the old five, then Few, Once, Jump.
  TEST_ASSERT_TRUE(newsToast(kNewsFew | kNewsOnce | kNewsJump, &t, 37));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_few);
  TEST_ASSERT_TRUE(newsToast(kNewsOnce | kNewsJump, &t, 37));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_plays_once);
  // Few with no count now (the filter off, none fitting or more than 50):
  // moot, the next one down.
  TEST_ASSERT_TRUE(newsToast(kNewsFew | kNewsJump, &t, 0));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_no_close);
  TEST_ASSERT_FALSE(newsToast(kNewsFew, &t, 0));
  TEST_ASSERT_FALSE(newsToast(kNewsFew, &t, autodj::kTrackCooldown + 1));
  TEST_ASSERT_TRUE(newsToast(kNewsFew, &t, autodj::kTrackCooldown));
  TEST_ASSERT_TRUE(newsToast(kNewsFew, &t, 1));
  // With any of the old five, the toast is the old one's, as before the
  // mood filter: every byte.
  const uint8_t old = kNewsCued | kNewsPaused | kNewsNoNeighbours | kNewsSeedMoved | kNewsRandom;
  for (unsigned n = 0; n < 256; ++n) {
    if (!(n & old)) continue;
    Toast a, b;
    TEST_ASSERT_TRUE(newsToast(static_cast<uint8_t>(n), &a, 37));
    TEST_ASSERT_TRUE(newsToast(static_cast<uint8_t>(n & old), &b));
    TEST_ASSERT_TRUE(a.text == b.text && a.warn == b.warn && a.count == 0);
  }
  // The words, with the count: one and other.
  char b[128];
  uistr::trnf(b, sizeof(b), S::toast_autodj_few, 37u, 37u);
  TEST_ASSERT_EQUAL_STRING("Only 37 tracks fit: they will repeat", b);
  uistr::trnf(b, sizeof(b), S::toast_autodj_few, 1u, 1u);
  TEST_ASSERT_EQUAL_STRING("Only 1 track fits: it will repeat", b);
  TEST_ASSERT_EQUAL_STRING("Plays once, then the filters apply", uistr::tr(S::toast_autodj_plays_once));
  TEST_ASSERT_EQUAL_STRING("No close match: any track that fits", uistr::tr(S::toast_autodj_no_close));
}

// The Few toast's count: a filter on, Auto DJ on, 1 to 50 fitting (they
// repeat). Not off, not with no filter on (a small library's fit is all
// of it), not with none or more than 50.
void test_the_few_count() {
  State s = filtered(37);
  TEST_ASSERT_EQUAL_UINT32(0, fewCount(s));  // (off)
  s.on = true;
  TEST_ASSERT_EQUAL_UINT32(37, fewCount(s));
  s.fit = 1;
  TEST_ASSERT_EQUAL_UINT32(1, fewCount(s));
  s.fit = autodj::kTrackCooldown;
  TEST_ASSERT_EQUAL_UINT32(autodj::kTrackCooldown, fewCount(s));
  s.fit = autodj::kTrackCooldown + 1;
  TEST_ASSERT_EQUAL_UINT32(0, fewCount(s));
  s.fit = 0;
  TEST_ASSERT_EQUAL_UINT32(0, fewCount(s));
  s.fit = 30;
  s.moodOn = false;  // (no filter on: fit counts every track, 30 of 30)
  s.fitOf = 30;
  TEST_ASSERT_EQUAL_UINT32(0, fewCount(s));
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

// A pick of a mood filter's, appended: a jump (`jump`), else a pick from
// the anchors' lists.
autodj::Pass jumped(autodj::Jump jump = autodj::Jump::NoCandidate) {
  autodj::Pass p = picked(true, jump == autodj::Jump::NoRow);
  p.pick.byIdJoined = p.pick.noIndex;
  p.pick.jump = jump;
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

// A mood filter's picks (MOOD-FILTERS.md 4.5, 6.2, 6.4): a jump (its scan's
// row, or a track with no row by id), a repeat (a row's, or the only track
// that fits, by id) and a re-pin say none of the old news, a session's
// first pick or not: the anchors had lists, and the pin stays or moves onto
// the playing track. A pick by id with the index joined isn't for want of
// one: not "no index", but a session's first pick with no anchor's list, as
// any random one. By id with no index still says so, once.
void test_the_news_under_a_mood_filter() {
  NewsWatch w;
  for (Mode m : {Mode::Rolling, Mode::Locked}) {
    for (int kind = 0; kind < 5; ++kind) {
      w.sessionBegan();
      autodj::Pass p = picked(true, kind == 1 || kind == 4);
      p.pick.byIdJoined = p.pick.noIndex;
      if (kind <= 1) p.pick.jump = kind ? autodj::Jump::NoRow : autodj::Jump::NoCandidate;
      p.pick.repeated = kind == 2 || kind == 4;
      p.pick.repinned = kind == 3;
      // (A jump says its own: test_the_jump_toast.)
      TEST_ASSERT_EQUAL_UINT8(kind <= 1 ? kNewsJump : 0, w.pass(p, m, true));
      TEST_ASSERT_EQUAL_UINT8(kind <= 1 ? kNewsJump : 0, w.take());
    }
  }
  w.sessionBegan();
  autodj::Pass joined = picked(true, true);
  joined.pick.byIdJoined = true;
  TEST_ASSERT_EQUAL_UINT8(kNewsNoNeighbours, w.pass(joined, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(joined, Mode::Rolling, true));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsSeedMoved, w.pass(joined, Mode::Locked, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.pass(picked(true, true), Mode::Locked, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Locked, true));
}

// Few (MOOD-FILTERS.md 4.5): 1 to 50 tracks fit, while on. Said at the
// first appended pick after a boot or after a Play that switched Auto DJ
// on, at a switch on and at a settled change; once, until a change, a
// switch off or the count leaving few and coming back; not again at a
// session. Behind the fence a settle says nothing, and Few comes at the
// first pick after it.
void test_the_few_toast() {
  NewsWatch w;
  w.sessionBegan();  // (the boot's)
  w.counted(true);   // (37 fit)
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  w.sessionBegan();  // (a Play: not again)
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // Switched off: nothing while off (the console's Dp); at the switch on,
  // at once, and not again at its first pick.
  w.switchedOff();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, false));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.switchedOn(true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  // A Play that switched it on (no switch on of the glue's): the first
  // appended pick.
  w.switchedOff();
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, false));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  // A settled change: at once, and not at the next pick.
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.settled(true, true, false));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // The count leaving few (above 50, or none) and coming back without a
  // change (a library update's end): at the next pick.
  w.counted(false);
  w.counted(true);
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  // Few still, a count again (the editor's next tap): nothing new.
  w.counted(true);
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  // Not few: nothing anywhere.
  w.counted(false);
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(true, true, false));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  w.switchedOff();
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(0, w.switchedOn(true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // A change settled while off says nothing; the switch on says it.
  w.counted(true);
  w.switchedOff();
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(false, true, false));
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.switchedOn(true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  // Behind the fence: a settle says nothing (its count is old), and Few
  // comes at the first pick after it.
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(true, false, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
}

// Jump (MOOD-FILTERS.md 4.5): a pick with no close match inside the
// filter, appended: once a session, and again after a settled change; a
// jump outranked (Few, a cued pick) is said at the next pick that jumps,
// not at one that doesn't.
void test_the_jump_toast() {
  NewsWatch w;
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.take());
  // Not again this session: the other jumps, a repeat after one (it keeps
  // the jump's code), a jump's re-pin.
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(jumped(autodj::Jump::AllCooled), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(jumped(autodj::Jump::NoRow), Mode::Rolling, true));
  autodj::Pass rep = jumped(autodj::Jump::AllCooled);
  rep.pick.repeated = true;
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(rep, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // A new session: again (by id with no row, a jump too); a refused one
  // says nothing and the retry that lands does.
  w.sessionBegan();
  autodj::Pass refused = jumped(autodj::Jump::NoRow);
  refused.added = false;
  refused.refused = true;
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(refused, Mode::Locked, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.pass(jumped(autodj::Jump::NoRow), Mode::Locked, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.take());
  // A settled change: again, at the next jump (the change says no Jump).
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(true, true, false));
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.take());
  // ... and behind the fence too.
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(true, false, false));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.take());
  // Outranked by Few at the first pick after a boot: Few now; a pick that
  // doesn't jump says nothing of it; the next jump does.
  NewsWatch v;
  v.sessionBegan();
  v.counted(true);
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, v.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, v.take());
  TEST_ASSERT_EQUAL_UINT8(0, v.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, v.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, v.take());
  // A finished queue's cued pick that jumped: the cue now, the jump at the
  // next one.
  NewsWatch c;
  c.sessionBegan();
  autodj::Pass cued = jumped();
  cued.cued = true;
  TEST_ASSERT_EQUAL_UINT8(kNewsCued, c.pass(cued, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsCued, c.take());
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, c.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, c.take());
}

// Once (MOOD-FILTERS.md 4.5, D5): a settled change that Auto DJ's pick up
// next fails, while on, with the count known. Under Few, or outranked
// later in the same take, it goes: its moment has passed.
void test_the_once_toast() {
  NewsWatch w;
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsOnce, w.settled(true, true, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsOnce, w.take());
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(true, true, false));   // (inside the new filter)
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(false, true, true));   // (off)
  TEST_ASSERT_EQUAL_UINT8(0, w.settled(true, false, true));   // (behind the fence)
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // Few too: Few, and the once goes (not at a later pick).
  w.counted(true);
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.settled(true, true, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // A cued pick in the same take (a Dq, then the loop's pass): the cue.
  w.counted(false);
  TEST_ASSERT_EQUAL_UINT8(kNewsOnce, w.settled(true, true, true));
  autodj::Pass cued = picked(false, false);
  cued.cued = true;
  TEST_ASSERT_EQUAL_UINT8(kNewsCued, w.pass(cued, Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsCued, w.take());
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
}

// A raised Few outranked later in its take: switched on with 1 to 50
// fitting, and the loop's pass in the same take appends the session's
// first pick, random (its track has no row): that toast; Few at the next
// pick, not lost.
void test_a_mood_toast_outranked_waits() {
  NewsWatch w;
  w.counted(true);
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.switchedOn(true));
  TEST_ASSERT_EQUAL_UINT8(kNewsNoNeighbours, w.pass(picked(true, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsNoNeighbours, w.take());
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(false, false), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  // A jump raised, then Few in the same take (a settled change): Few, and
  // the jump at the next one.
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.settled(true, true, false));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.pass(jumped(), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsJump, w.take());
}

// A card with no index Auto DJ can use and a filter 37 tracks fit
// (MOOD-FILTERS.md 4.5): on after a boot, Random at the first pick (said
// at once, so it is marked said), Few at the next, then nothing; switched
// on there, Random at the switch and Few at the first pick. The toasts as
// the UI makes them.
void test_no_index_and_37_fitting() {
  State s = filtered(37);
  s.on = true;
  s.usable = false;
  s.index = Index::None;
  char text[128];
  Toast t;
  NewsWatch w;
  w.sessionBegan();  // (the boot's)
  w.counted(true);
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.pass(picked(true, true), Mode::Rolling, true));
  uint8_t n = w.take();
  TEST_ASSERT_TRUE(newsToast(n, &t, fewCount(s)));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_random && t.count == 0);
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(true, true), Mode::Rolling, true));
  n = w.take();
  TEST_ASSERT_TRUE(newsToast(n, &t, fewCount(s)));
  TEST_ASSERT_TRUE(t.text == S::toast_autodj_few && t.warn);
  uistr::trnf(text, sizeof(text), t.text, t.count, t.count);
  TEST_ASSERT_EQUAL_STRING("Only 37 tracks fit: they will repeat", text);
  TEST_ASSERT_EQUAL_UINT8(0, w.pass(picked(true, true), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(0, w.take());
  // Switched off and on there.
  w.switchedOff();
  w.sessionBegan();
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.switchedOn(false));
  TEST_ASSERT_EQUAL_UINT8(kNewsRandom, w.take());
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.pass(picked(true, true), Mode::Rolling, true));
  TEST_ASSERT_EQUAL_UINT8(kNewsFew, w.take());
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
  RUN_TEST(test_the_filter_value);
  RUN_TEST(test_strictness_steps);
  RUN_TEST(test_a_strictness_tap_steps_from_what_is_shown);
  RUN_TEST(test_the_strictness_shown_is_the_one_that_acts);
  RUN_TEST(test_the_anchor_texts);
  RUN_TEST(test_the_status_row);
  RUN_TEST(test_the_status_row_says_why_it_waits);
  RUN_TEST(test_the_empty_states_button);
  RUN_TEST(test_the_status_row_with_mood_data);
  RUN_TEST(test_the_status_row_under_a_filter);
  RUN_TEST(test_the_status_row_under_a_filter_with_no_index);
  RUN_TEST(test_the_status_amber);
  RUN_TEST(test_the_sheet_detail);
  RUN_TEST(test_the_news_toasts);
  RUN_TEST(test_the_few_count);
  RUN_TEST(test_the_news_from_the_passes);
  RUN_TEST(test_the_news_under_a_mood_filter);
  RUN_TEST(test_the_few_toast);
  RUN_TEST(test_the_jump_toast);
  RUN_TEST(test_the_once_toast);
  RUN_TEST(test_a_mood_toast_outranked_waits);
  RUN_TEST(test_no_index_and_37_fitting);
  RUN_TEST(test_the_index_names);
  return UNITY_END();
}
