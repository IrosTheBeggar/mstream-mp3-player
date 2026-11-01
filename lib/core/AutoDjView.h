// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "AutoDjDriver.h"   // Pass
#include "AutoDjEngine.h"   // Mode
#include "AutoDjTrigger.h"  // Hold
#include "UiStrings.h"

// Auto DJ on the screen (the engine design's 5 and 2.14; its milestone
// M5): what the UI shows of it and says about it, portable and host-tested
// (test_ui_autodj). app/AutoDj fills a State for the UI's snapshot each
// pass (AppState::autoDj); ui/ draws from it:
//   - Now Playing's "..." sheet: a 4th row, "Auto DJ", its state the
//     detail (rowState(): On, Off, Waiting, Paused); a tap switches it,
//     the sheet staying up (as Shuffle).
//   - Output > Auto DJ (a row with its line, rootLine(), and a page): the
//     switch, Anchor (a tap swaps), Match strictness (a tap takes the next
//     step: nextFloor()), BPM continuity, Harmonic mixing, and a status
//     row (statusLines()); the four in between dim with "needs an Auto DJ
//     index" while the card has none it can use (rowLive()).
//   - The empty states (Now Playing, the Queue): [Open Library] [Auto DJ]
//     in Shuffle all's place while the card has a usable index (D4);
//     "Auto DJ from here" in the Library's long-press sheet on a track the
//     index knows.
//   - The Queue: the DJ mark on what Auto DJ added; no badge flash for its
//     appends (queueview::badgeFlashes()).
//   - Toasts (newsToast()): a seed with no neighbours, a finished queue's
//     cued pick, picks at random for want of an index, the failure pause.
// Nothing here starts playback or changes the output or the volume: the
// only taps that play are the empty states' button and "Auto DJ from here"
// (the listener's own), and switching Auto DJ on never does (B stays inert
// on an empty queue).
namespace autodjview {

// The index, as the page's status row tells it.
enum class Index : uint8_t {
  None,      // no index on the card (or one of a major this build doesn't read)
  Bad,       // it failed its checks, or a slot's list broke a rule under a good CRC
  Unreadable,  // a read failed (the file the root names didn't open, a card read error, no memory to read the root after a remount)
  TooBig,    // a library over autodj::kMaxTracks tracks: it can't be joined
  NoMemory,  // the join found no memory: until the next library build (or off and on)
  Found,     // open and checked, Auto DJ off: joined when it is switched on
  Reading,   // Auto DJ on: the join runs (or waits a moment: a retry, the update step)
  Ready,     // joined: the coverage known
};
const char* indexName(Index i);

// What the UI knows of Auto DJ this pass (app/AutoDj::view()).
struct State {
  bool on = false;
  autodj::Mode mode = autodj::Mode::Rolling;
  uint8_t floorPct = autodj::kFloorMin;    // the setting: 55-75 by 5
  uint8_t floorShown = autodj::kFloorMin;  // ... as it acts (shownFloor(): an index's own floor above it)
  bool matchTempo = false, matchKey = false;
  // AutoDj::usable(): an index open and checked for a library it can join
  // (the empty states' button, "Auto DJ from here", the page's live rows).
  bool usable = false;
  Index index = Index::None;
  autodj::Hold hold = autodj::Hold::None;  // while on (the trigger's last pass)
  uint32_t rows = 0;                       // Found: the index's tracks
  uint32_t mapped = 0, tracks = 0;         // Ready: the library's tracks with neighbours, of all
  uint8_t bpmPct = 0, keyPct = 0;          // Ready: ... of those, with a BPM, with a key
  // Auto DJ's appends since the boot (Ui: the Queue badge doesn't flash
  // for them).
  uint32_t adds = 0;
};

// ---- the "..." row ----
enum class RowState : uint8_t { Off, On, Waiting, Paused };
// Off; on and held by Repeat One or the sleep timer's End of queue:
// Waiting; on and paused by failures: Paused; else On.
RowState rowState(const State& s);
uistr::S rowText(RowState r);  // now_playing.autodj_*

// ---- Output's row: "On · Follow the vibe", "Waiting · Stay on seed", "Off" ----
uistr::S modeText(autodj::Mode m);  // autodj.anchor_rolling / _locked
// The state with the Anchor in use when `withMode` and the index is usable
// (without one the Anchor does nothing: its picks are random); else the
// state alone ("On", "Waiting", "Paused": the "..." row's words); "Off"
// off. OutputPage draws the line with the Anchor when it fits whole in the
// row's room (uitext::kAutoDjRowW), else the state alone: the Anchor keeps
// the mobile app's words in every language (docs/I18N.md's one word for
// one thing).
char* rootLine(const State& s, char* buf, size_t size, bool withMode = true);

// ---- the page ----
enum PageRow : uint8_t { OnRow, AnchorRow, StrictRow, BpmRow, KeyRow, StatusRow, kPageRows };
// Live (else drawn dim, "needs an Auto DJ index", a tap a note): the
// switch and the status always; the rest with a usable index.
bool rowLive(PageRow r, const State& s);
// The strictness's next step: 55, 60, 65, 70, 75, then 55 again (a
// value off the steps: the next step above it, else 55).
uint8_t nextFloor(uint8_t pct);
// A tap on Match strictness: the step after the value the row shows (an
// index's own floor may show above the setting: from the setting, a tap
// under that floor would change nothing on the screen).
uint8_t tapFloor(const State& s);
// The strictness as it acts: the gate is max((255p + 99) div 100,
// selFloor) (autodj::floorLevel()), so below an index's own floor the
// setting is that floor: the smallest percent whose level reaches
// selFloor, when that is above `pct` (selFloor 140, the writers' default,
// is 55 %: no change).
uint8_t shownFloor(uint8_t pct, uint32_t selFloor);
// "55% or closer".
char* strictnessText(uint8_t pct, char* buf, size_t size);
// The hint under Anchor (the mode's).
uistr::S anchorHint(autodj::Mode m);
// The status row's two lines: held (on, Waiting or Paused), why over the
// index's first line; else the index's (Ready: "19,000 of 19,410 tracks
// have neighbours" over "BPM 92% · key 88%"; Found: "Auto DJ index: 19,410
// tracks" over "read when Auto DJ is switched on"; Reading: one line;
// None, Bad, Unreadable, TooBig, NoMemory: why over "picks are random").
void statusLines(const State& s, char* line1, size_t size1, char* line2, size_t size2);

// ---- the empty states' second button (D4) ----
// Auto DJ in Shuffle all's place: a usable index and a library.
bool emptyButton(const State& s, uint32_t libraryTracks);
// What the button did (app/AutoDj::start()).
enum class Started : uint8_t {
  Played,   // a track the index knows plays, Auto DJ on
  NoTrack,  // no track with neighbours in the draws: nothing played (another tap draws again)
  NoIndex,  // no usable index any more (gone since the button was drawn): nothing played
  Refused,  // the Play was refused (the library updating, no memory)
};

// ---- what Auto DJ's loop step has to say (the toasts) ----
// Bits (app/AutoDj::takeNews(), since its last call).
enum News : uint8_t {
  kNewsCued = 1,          // a finished queue: a pick cued, paused at 0:00 (toast.autodj_cued)
  kNewsPaused = 2,        // 3 picks in a row failed to play: paused (a note: toast.autodj_paused)
  kNewsNoNeighbours = 4,  // a session's first pick was random: its track has no row (Follow the vibe)
  kNewsSeedMoved = 8,     // ... in Stay on seed: that pick becomes the seed
  kNewsRandom = 16,       // picks at random for want of a usable index (once a switch on, or a boot)
};
struct Toast {
  uistr::S text = uistr::S::toast_autodj_random;
  bool warn = false;  // an amber note (Ui::warn), else a toast
};
// The one toast for `news` (one a pass: the most pressing, in this order:
// the failure pause, the cued pick, the seed, no index). False: none.
bool newsToast(uint8_t news, Toast* out);

// What app/AutoDj has to say, from the Driver's passes (the engine
// design's 2.1, 2.3.1, 2.3.2, 2.5): a finished queue's cued pick; the
// failure pause (while on); a session's first pick random because its
// track has no row (no anchor had a list), Follow the vibe's or Stay on
// seed's (that pick becomes the seed); picks by track id for want of a
// usable index, once a switch on or a boot (said at the switch on when it
// has none), not once a session: every Play is a session, and a card with
// no index would say it at every album's last track. Only picks that
// were appended speak (one the queue refused is tried again). And Auto
// DJ's appends, counted (State::adds: no Queue badge flash).
class NewsWatch {
public:
  // A session began (every one: the switch on, a Play, the button, "Auto
  // DJ from here", a boot, an index changed): its first pick next.
  void sessionBegan() { picks_ = 0; }
  // Switched on (after its session began): with no usable index its picks
  // are random, said now (and not again until the next switch on); with
  // one, said if it goes (a remount without it).
  uint8_t switchedOn(bool usable);
  // One pass of the Driver's (or the console's Dp), Auto DJ's settings
  // then: its news bits.
  uint8_t pass(const autodj::Pass& p, autodj::Mode mode, bool on);
  uint32_t adds() const { return adds_; }

private:
  uint32_t picks_ = 0;       // this session's picks appended
  bool randomSaid_ = false;  // since the last switch on (or the boot)
  uint32_t adds_ = 0;
};

}  // namespace autodjview
