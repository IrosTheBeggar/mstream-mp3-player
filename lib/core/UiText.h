// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

#include "CardFormat.h"
#include "UiStrings.h"

// The rooms the UI's texts have, next to what is drawn there, so the host
// tests (test_ui_library, test_ui_strings) can measure the texts with the
// firmware's own DejaVu data: a text cut to "Remov…" or "Tap ag…" on the
// device is a failing test here. The rooms are the pixels ui/ gives each
// text (see where each is drawn); the fonts are ui/Fonts' (Body = DejaVu
// Sans 16, Small = 13, Bold = Bold 16, Title = Bold 22). The texts are the
// catalogue's (i18n/en.json, uistr::tr(): docs/I18N.md, phase 3), each
// with its room there too; the few that stay English whatever the UI's
// language are here (the identify banner, BoardGuard's screen).
namespace uitext {

// ---- the first-boot tips (ui/Overlays, Coach) ----
// Card 1: the title (Bold) and its line (Small) across the width.
inline constexpr int kCoachTextW = 308;
// Its three boxes (102 px, 4 px apart), each: the click (Bold) and the hold
// (Small, two lines at most: coach.hold_*), centred, a pixel in from each
// side (a glyph's advance holds a pixel or two of space after its ink:
// German's "Play/Pause" is 99 px, about 96 of ink).
inline constexpr int kCoachBoxW = 102;
inline constexpr int kCoachBoxTextW = kCoachBoxW - 2;
// Card 2: two Title lines (coach.tab), then two Small ones (coach.tab_more)
// and coach.tips_where.

// ---- the toast (ui/Overlays) ----
// Undo at the screen's edge (its hit area to the edge, a clamped reading
// too), View left of it; as icons (Undo's arrow, View as the Queue tab's
// icon) on two lines ("Plays next" over a long name), and whenever a label
// doesn't fit its pill (toastIcons(): German's "Rückgängig"). The split
// between them sits well right of View's centre: with no correction (the
// default), a panel that reads right like the lab's (+28 px at x 214, +27
// at 244: TouchCalibration::labFitX()) still hits View there, never Undo,
// which would take the add back (test_ui_library).
inline constexpr int kToastUndoX = 256, kToastUndoW = 54, kToastUndoHitX = 250;
inline constexpr int kToastViewX = 186, kToastViewW = 56, kToastViewHitX = 180;
inline constexpr int kToastUndoCX = 282, kToastUndoCW = 30, kToastUndoCHitX = 278;
inline constexpr int kToastViewCX = 230, kToastViewCW = 28, kToastViewCHitX = 214;
inline constexpr int kToastLabelPad = 4;  // a label (Bold) has its pill less this
// The buttons as their icons because a label (`undoLabelW`, `viewLabelW`:
// the Bold widths of toast.undo and toast.view) doesn't fit its pill: both
// or neither. (Two lines are icons too: Toast::show().)
constexpr bool toastIcons(bool undo, int undoLabelW, bool view, int viewLabelW) {
  return (undo && undoLabelW > kToastUndoW - kToastLabelPad) || (view && viewLabelW > kToastViewW - kToastLabelPad);
}
// What a tap on the toast is: 2 Undo, 3 View, 1 elsewhere on it. `icons`:
// the buttons are drawn as icons.
constexpr int toastButtonAt(int x, bool rightEdge, bool icons, bool undo, bool view) {
  const int undoHit = icons ? kToastUndoCHitX : kToastUndoHitX;
  const int viewHit = icons ? kToastViewCHitX : kToastViewHitX;
  if (undo && (rightEdge || x >= undoHit)) return 2;
  if (view && !rightEdge && x >= viewHit && x < undoHit) return 3;
  return 1;
}
// "Plays next: <name>" on two lines: the name (Body, else Small) from x 22
// to the View icon's pill.
inline constexpr int kToastTextX = 22;
inline constexpr int kToastCompactTextRight = kToastViewCX - 4;
inline constexpr int kToastTextRight = 306;  // a note (Ui::warn) on one line, no buttons
// A note on two lines (Toast's "what: where", toast.no_headphones): the
// part before ": " in Small over the rest in Body, each from kToastTextX to
// 306 (no buttons). A skipped track's note (Ui::noteFailures,
// toast.skipped: "Skipped <title>: <why>") the same, the why one of
// toast.why_*: for a sample rate the converter refused, toast.why_rate
// (its rate: RateConverter::rateText(), "96 kHz", "37.8 kHz", "44056 Hz")
// or toast.why_cpu (88.2/96 kHz at 160 MHz: a setting would play it, on
// the Output tab); else the backend's own few words when it has them
// (PlaybackController::Failure::note: an Opus file refused at its open,
// toast.why_opus_*: oggopus::Reader::refusalNote()); else
// toast.why_cant_play.
inline constexpr int kToastTwoLineW = 306 - kToastTextX;

// ---- the empty states (ui/EmptyState): two buttons ----
// The primary (Bold, its icon 26 px + 8 before the text) and the second
// (Body, its icon), each with 6 px inside at each end.
inline constexpr int kEmptyPrimaryX = 12, kEmptyPrimaryW = 166;
inline constexpr int kEmptySecondX = 184, kEmptySecondW = 124;
inline constexpr int kEmptyIconW[2] = {26, 20};  // the Library's and Shuffle's icons (ui/IconData.cpp)
inline constexpr int kEmptyLineW = 304;  // a line (Small), centred
// The title (Title, centred in kW - 16).
inline constexpr int kEmptyTitleW = 304;
// No card it can use (Now Playing, the Library and the Queue with no card
// mounted and no music on the flash): the title, two lines and [Try
// again]; its note when it still fails (Ui::warn, Toast's "what: where" on
// two lines: the part before ": " in Small over the rest in Body,
// kToastTwoLineW). One message for each thing cardformat reads off a card
// that didn't mount (cardMessage()): none at all; exFAT (anything over
// 32 GB, as it comes); NTFS; a GPT (a Mac's Disk Utility's default
// scheme, "GUID Partition Map": the fix is its "Master Boot Record"); and
// a card that answered but held nothing it knows (blank, unformatted,
// damaged, Linux's, or a FAT32 one whose mount failed for another reason:
// so never "format it", which would erase music a Try again may find).
// README's, the install page's and the release notes' microSD card
// sections say how, per computer.
struct CardMessage {
  uistr::S title;
  uistr::S lines;  // two lines ('\n' between them)
  uistr::S still;  // Try again's note
};
// (The tracks are named by their tags since metadata's N9, by their file
// names only where a file has none: so the folders, which still make the
// artists and the albums, are what the lines ask for, not a file-name
// pattern.)
inline constexpr CardMessage kNoCard = {uistr::S::card_none_title, uistr::S::card_none_lines,
                                        uistr::S::card_none_still};
inline constexpr CardMessage kExFatCard = {uistr::S::card_exfat_title, uistr::S::card_exfat_lines,
                                           uistr::S::card_exfat_still};
inline constexpr CardMessage kNtfsCard = {uistr::S::card_ntfs_title, uistr::S::card_ntfs_lines,
                                          uistr::S::card_ntfs_still};
inline constexpr CardMessage kGptCard = {uistr::S::card_gpt_title, uistr::S::card_gpt_lines,
                                         uistr::S::card_gpt_still};
inline constexpr CardMessage kUnknownCard = {uistr::S::card_unknown_title, uistr::S::card_unknown_lines,
                                             uistr::S::card_unknown_still};
constexpr const CardMessage& cardMessage(cardformat::Kind k) {
  switch (k) {
    case cardformat::Kind::ExFat: return kExFatCard;
    case cardformat::Kind::Ntfs: return kNtfsCard;
    case cardformat::Kind::Gpt: return kGptCard;
    case cardformat::Kind::Other: return kUnknownCard;
    case cardformat::Kind::Unreadable:
    default: return kNoCard;
  }
}

// ---- Now Playing (ui/NowPlayingPage) ----
// The progress line's middle (Small), centred between the times (from
// x 12 and to 308): "4 of 16 · SPYDRONE", or the headphones' "SPYDRONE (not
// connected)" (177 px: 170 cut it).
inline constexpr int kNowPlayingMidW = 190;
// The amber warning drawn alone ("WH-1000XM4 (not connected)", 194 px: the
// line and it don't fit together) has a little more: centred, it still
// ends 4 px short of a 16-hour mix's "999:59" either side (test_ui_library).
inline constexpr int kNowPlayingWarnW = 200;
// The seek bar's readout (docs/SEEK-BAR.md section 4.2), in the row above
// the line while a finger scrubs, on the side away from the knob: the
// finger's second ("2:31", Title) and, kSeekReadoutGap px after it, the
// change ("+1:21", Small), or now_playing.seek_stay back where it plays;
// the group at most kSeekReadoutW wide: a mix's "999:59 no change" is 159
// px (150 cut "no change" from 100 min on), and from x 12 (or to x 308) it
// still ends 12 px short of where the knob sends it across (SeekBar's
// kReadoutLeftX, kReadoutRightX). Slid off the bar: now_playing.seek_cancel
// (Bold, amber), centred in the line's 296 px.
inline constexpr int kSeekReadoutW = 160;
inline constexpr int kSeekReadoutGap = 8;

// ---- Now Playing's two menus (ui/NowPlayingPage; docs/QUEUE-MODES.md) ----
// The navigation menu (a tap on the cover, the title, the artist or the
// album): a 3-row sheet titled with the track's title, each row's detail
// (Small, dim) right-aligned in what its label (Body) leaves
// (sheet::detailRoom(): 183, 174 and 177 px): the artist or
// library.no_artist_folder, the album or library.loose_tracks, the folder
// cut from the left ("…/Daft Punk/Discovery").
// Go to artist and Go to album open the folder entities Stage A keeps
// (docs/METADATA.md 5.4), so their details name those: the artist's name
// (its folder's, or the spelling its tags elected), the album's.
// library.no_artist_folder and library.loose_tracks are the entities with
// no name (the files right under /music; an artist folder's own tracks):
// the Library's rows, headers and toasts call them so too (librarytext).
// Now Playing's artist row names the track's artist (its tag's, else its
// album's line, else its artist folder): with none of them,
// now_playing.unknown_artist (Body, the row's kTextW, x 120-310). Its album
// row: the album's name, then its year (librarytext::kDot, "2001") when
// the whole fits, else the name alone.
inline constexpr int kNowPlayingTextW = 190;
// When nothing in it could act, no menu but a toast (Body, one line: 264
// and 209 px of kToastTextRight - kToastTextX). The playback menu ("..."):
// a 3-row sheet titled now_playing.playback_title (Small), the rows
// Shuffle, Repeat and Sleep timer, each state its row's detail.
// The Repeat row's state: PlaybackController::Repeat's 0 Off, 1 All, 2 One.
constexpr uistr::S repeatMode(unsigned mode) {
  return mode == 1   ? uistr::S::now_playing_repeat_all
         : mode == 2 ? uistr::S::now_playing_repeat_one
                     : uistr::S::now_playing_repeat_off;
}

// ---- play waiting for the headphones (ui/NowPlayingPage, ui/Ui) ----
// Now Playing's panel in the title strip and the rows under it (x 112-319,
// y 38-137): the title on one line over "Waiting for SPYDRONE..." (Small,
// amber) and "try 2 of 3" (Small, dim), then two buttons (Body) in the
// artist and album rows, x from the column's left.
inline constexpr int kWaitTextX = 8, kWaitTextW = 196;
inline constexpr int kWaitSpeakerX = 4, kWaitSpeakerW = 136;
inline constexpr int kWaitCancelX = 144, kWaitCancelW = 62;
inline constexpr int kWaitButtonPad = 8;  // a label has the box less this
// The notice when they can't be reached (ui/Overlays' Dialog: no icon, so
// "Couldn't reach SPYDRONE" has its title's 260 px; the body in Small over
// 260 px; two buttons of 131 px, their labels 8 px in: "Play on speaker"
// is 4 px too wide in Body, so the Dialog draws it in Small).
inline constexpr int kDialogTitleW = 260;
inline constexpr int kDialogButtonTextW = 123;
// A title after the dialog's icon (the lost dialog's struck-out headphones,
// 22 px, from x 14, then 8 px): to 12 px from the dialog's right edge (the
// body keeps its 16), so "Headphones disconnected" (235 px) is whole.
inline constexpr int kDialogIconTitleW = 236;

// ---- the sleep timer (SleepTimer; docs/ENERGY.md section 3) ----
// Now Playing's "..." sheet (the playback menu): its last row (Body), the
// timer's state dim on the right (SleepTimer::rowText(): "Off", "23 min",
// "End of queue").
// The Sleep timer sheet (ui/Overlays' SleepSheet, the sheet panel 320 x
// 168 from y 72): its title (Small, SleepTimer::titleText()) left of the ✕
// pill (x 16 to 242), then three rows of pills 36 px tall, 44 px apart. The
// running choice is outlined in the Now Playing accent. A label has its
// pill less kSleepPillPad (centred: 2 px a side), in Small when its font
// doesn't fit (SleepSheet::render()).
inline constexpr int kSleepTitleW = 226;
inline constexpr int kSleepPillPad = 4;
// Row 1 (Body): the minutes; sleep.minutes_90 ("90 min") says what the
// numbers are.
inline constexpr const char* kSleepMinutes[4] = {"15", "30", "45", "60"};
inline constexpr int kSleepMinX[5] = {12, 68, 124, 180, 236}, kSleepMinW[5] = {50, 50, 50, 50, 72};
// Row 2 (Small: "End of album" is 106 px in Body): sleep.end_*.
inline constexpr int kSleepEndX[3] = {12, 112, 212}, kSleepEndW[3] = {94, 94, 96};
// Row 3 while a timer runs (Body): sleep.extend, and sleep.turn_off in red
// (to the edge).
inline constexpr int kSleepExtendX = 12, kSleepExtendW = 144;
inline constexpr int kSleepOffX = 162, kSleepOffW = 146;
// ... and while none runs, a line (Small, sleep.hint) across the row.
inline constexpr int kSleepHintW = 296;
// The toast during the fade (ui/Overlays' Toast): its line (Small,
// sleep.fading_toast) from kToastTextX to the +10 min pill, then +10 min
// and Turn off (Body), Turn off reaching the screen's edge.
inline constexpr int kSleepToastPlusX = 158, kSleepToastPlusW = 74;
inline constexpr int kSleepToastOffX = 238, kSleepToastOffW = 72;
inline constexpr int kSleepToastPad = 6;  // a label has its pill less this
// The sleep and idle toasts' buttons take a tap this far below the toast
// (y 36 to 77, not 71: 42 px, used half asleep) while no sheet or dialog
// is up; the toast is still drawn 36 px (the list's scrolled band starts
// at 72). Under a modal the toast's own 36 px only: a sheet's top is there.
inline constexpr int kToastButtonSlop = 6;
// Now Playing's progress line: the moon (11 px) and 2 px, then
// SleepTimer::shortText() ("23 min", "45 s", "track", "fading"), 6 px
// after the line, in kNowPlayingMidW. What shows is sleepLineFit()'s.
inline constexpr int kSleepMoonW = 13;
inline constexpr int kSleepGap = 6;

// The progress line with the sleep timer running: the first of these that
// fits in `room`.
//   The line as it is ("4 of 16 · SPYDRONE"), the moon and its text;
//   the line without "· <output>" ("4 of 16", `baseW`), the moon and its
//   text (a headphone name leaves no room: the queue position stays);
//   the moon and its text alone.
// With the headphones not connected (`warnW` >= 0: the amber "SPYDRONE
// (not connected)" alone), that text is never dropped for the timer:
//   the line as it is, the moon and its text;
//   the warning, the moon and its text;
//   the warning and the moon;
//   the warning alone (the tab bar's moon badge still shows the timer).
// `fullW`, `baseW`, `warnW`: the Small widths; `moonTextW`: kSleepMoonW
// and the timer's text.
struct SleepLine {
  enum class Text : unsigned char { Full, Base, Warn, None };
  Text text = Text::Full;
  bool moon = true;
  bool moonText = true;
  int width = 0;  // all of it, gaps included (centred on the line)
};
constexpr SleepLine sleepLineFit(int fullW, int baseW, int warnW, int moonTextW, int room) {
  using T = SleepLine::Text;
  if (fullW + kSleepGap + moonTextW <= room) return {T::Full, true, true, fullW + kSleepGap + moonTextW};
  if (warnW >= 0) {
    if (warnW + kSleepGap + moonTextW <= room) return {T::Warn, true, true, warnW + kSleepGap + moonTextW};
    if (warnW + kSleepGap + kSleepMoonW <= room) return {T::Warn, true, false, warnW + kSleepGap + kSleepMoonW};
    return {T::Warn, false, false, warnW};
  }
  if (baseW + kSleepGap + moonTextW <= room) return {T::Base, true, true, baseW + kSleepGap + moonTextW};
  return {T::None, true, true, moonTextW};
}

// ---- the Queue's selection bar (ui/QueuePage) ----
// Remove N (Bold, after the trash icon; without it when it doesn't fit),
// Play next and Clear… (Body, else Small: each label its button less its
// pad, centred; Clear's red outline keeps its 4 px a side).
inline constexpr int kRemoveX = 6, kRemoveW = 140;
inline constexpr int kRemoveTextX = 32;  // from the button's left, after the icon
inline constexpr int kQueueNextX = 152, kQueueNextW = 80;
inline constexpr int kQueueClearX = 238, kQueueClearW = 76;
inline constexpr int kQueueNextPad = 4, kQueueClearPad = 8;

// ---- the queue's cap (QueueModel::kMaxEntries; docs/QUEUE-MODES.md 15) ----
// Play next or + Queue with the queue full and no played track to push
// out (none heard before the track that plays, 15.8: right after a Play
// that filled it, once every played one has gone, after Repeat All comes
// round to the first): nothing went in (queue_cap.full). Not "nothing
// played yet": tracks may well have played, and still none of them can
// go. The why under the what, as Toast folds "what: why" when it is too
// wide for one line: the what in Small, the why in Body if it fits, else
// Small, in the room the buttons it keeps leave it (Ui::refuse(): none,
// Undo, or Undo and View; test_ui_library measures all three).
// A Play (an artist's, an album's, "Play all N"), Shuffle all, or an add
// the cap cut short: "<what>: <why>" (queueview::cappedText() puts in the
// counts, grouped: "Shuffling 5,000 of 19,412"), on the toast's two lines
// beside Undo (and View after an add): what in Small, queue_cap.why in Body
// if it fits, else Small (test_ui_library: each in its room).
// An add that made room by pushing out tracks that already played (the
// heard entries before the current one, oldest first; 15.8): the add's
// what, then one line of how many went, "Added 12 tracks: 12 played tracks
// made way" (queueview::pushedText()), on the toast's two lines beside Undo
// and View: the what in Small over the line, in Body if it fits, else Small
// (test_ui_library: each in its room, at 4,999 pushed out). Cut short by
// the cap as well, the what is queue_cap.added's or queue_cap.next's
// ("Added 37 of 300").

// ---- the lists (ui/ListView), the headers (ui/Ui) ----
// A list's action bar (the top bar, a row's inline bar): each label (Bold
// for the first, Body) in its button less kListBarPad, centred, in Small
// when its font doesn't fit. The Folders root's "Play all 19410" (126 px
// in Bold) beside the A-Z rail has (284 - 16 - 6) / 2 - 4 = 127.
inline constexpr int kListBarPad = 4;
// A list's lone line when it has no rows (Body, centred: "Nothing here",
// library.updating_list), 10 px from each edge: nothing is beside it.
inline constexpr int kListLineW = 300;
// The Library root's segments (Artists | Albums | Folders, 100 px each):
// a label (Bold) in its segment less kSegmentPad, else Small.
inline constexpr int kSegmentPad = 4;
// A header's pill (Ui::drawHeader(): as wide as its label, kHeaderPillPad
// of pill around it, and the crumb's chevron, kHeaderChevronW): its label
// in Bold within a budget, else Small, so the title and the summary keep
// their room. The Library's root crumb ("‹ Library"), and the Queue's
// Edit, All and None. (Its hit area stays x 240 to the edge: Header::hit.)
inline constexpr int kHeaderPillPad = 24, kHeaderChevronW = 12;
inline constexpr int kCrumbTextW = 82;
inline constexpr int kQueuePillTextW = 60;

// ---- the Library's names (ui/LibraryPage; librarytext, docs/METADATA.md 5.4) ----
// An album of more than one disc: a divider row before each disc's first
// track (library.disc, "Disc 2", Bold in the Library's accent, from 12 px
// into the row to 8 px short of its right; disc numbers up to 255).
inline constexpr int kDiscTextX = 12;
inline constexpr int kDiscTextW = 320 - kDiscTextX - 8;

// ---- the scan's texts (docs/METADATA.md 3.3.6; librarytext::statusText()) ----
// The Library's status line while the card worker works (Small, across the
// list's width, x 8-312), drawn by the scan's glue (N10, N12): the counts
// grouped ("Reading tags 1,234 / 19,410", up to 99,999). Its toasts (Body,
// one line, no buttons: kToastTextX to kToastTextRight): the walk's news,
// the update step's end, and its deferral (3.4.2). The update step's fence
// (3.4.2, N12): the lists' line (library.updating_list: Body, centred, the
// list's width less 24) while the library is rebuilt on the card worker,
// and the note for a skip, a seek or an edit tried meanwhile
// (library.updating_wait, a toast). A card swapped while the player is on
// (3.8; main.cpp's stepCardGuard()): the toast before the restart (Body,
// one line, no buttons). Another card (write-protected until then), or the
// same card back with a card job under way.
inline constexpr int kStatusW = 304;

// ---- the Output tab (ui/OutputPage) ----
// The Bluetooth card's status line (Small, x 52 to the radio).
inline constexpr int kBtStatusW = 222;
// Its buttons, by how many (x, width); Bold for Pair and an armed Forget,
// else Body; a label that doesn't fit in its font is drawn in Small (the
// Dialog's rule: Spanish's "Desconectar").
inline constexpr int kBtButtons1X = 16, kBtButtons1W = 120;  // Cancel, Try again (Pair: the width)
inline constexpr int kBtButtons2X[2] = {16, 142}, kBtButtons2W[2] = {120, 100};
inline constexpr int kBtButtons3X[3] = {16, 124, 192}, kBtButtons3W[3] = {100, 60, 112};
inline constexpr int kBtButtonPad = 6;       // a label has the box less this (centred)
inline constexpr int kBtWideButtonW = 200;   // narrower boxes take the short labels (btButtonLabel)
inline constexpr int kBtChipTextX = 30;      // the volume chip: its % after the speaker icon
// The resting card (the background search stopped): "Not connected" on the
// status line, and two lines (Small, bt.hint) beside its one button,
// [Connect]: "Not connected. They'll reconnect when switched on." ... when
// they dropped while the output (BtCardView::lostHint, bt.lost_hint):
// resting only after the whole back-off (15 min), and they may be on and
// back in range with their own reconnect given up. The lost dialog's line
// then (dialog.lost_resting).
inline constexpr int kBtHintX = kBtButtons1X + kBtButtons1W + 12, kBtHintW = 320 - 8 - kBtHintX;
// The speaker card: its status line (Small), before the volume chip.
inline constexpr int kSpeakerLineW = 160;
inline constexpr int kSpeakerChipX = 218, kSpeakerChipW = 48;
// Its hit area: 6 px before it, and 10 px past it (the radio is at 280-300:
// a tap on the chip's centre that reads ~27 px right, as on the lab's panel
// with no correction, is still the chip, not the row's "to the speaker").
inline constexpr int kSpeakerChipHitX = kSpeakerChipX - 6;
inline constexpr int kSpeakerChipHitEnd = kSpeakerChipX + kSpeakerChipW + 10;
// The Pair screen's hint (Small, x 48 to 312): pair.hint, and once its
// scan stopped by itself (2 min: OutputModel's PairSearch; or the screen
// went off) pair.search_stopped.
inline constexpr int kPairHintW = 264;
// Settings rows: the line under a title with no chevron (Small).
inline constexpr int kSettingSubW = 204;
// Settings rows with a value (Screen off after, Brightness: ScreenPower's
// choices): the value (Body) in a pill at the row's right (8 px from its
// right end), the title and its line (Small) up to 8 px before the pill.
// "Turn off when idle" (IdlePolicy's choices, the same row: ENERGY.md item
// 4). "CPU speed" and "Bluetooth power" (PowerChoices' choices, the same
// row: ENERGY.md items 6 and 7); the CPU's line while the clock isn't the
// saved choice yet (the console's Pcb): "240 MHz until a restart". 160's
// line says what it costs: lists scrolled at about half the frame rate
// with an MP3 playing (8 against 16 fps), the dancer at ~18 of 24, for ~5
// mA (ENERGY.md step 6a).
inline constexpr int kSettingPillW = 78;  // "160 MHz" (69 px) in its pill
inline constexpr int kSettingPillPad = 4;  // a value has the pill less this (centred), else Small
inline constexpr int kSettingValueSubW = 312 - 8 - kSettingPillW - 8 - 44;  // from x 44, the row's right 312
// A CPU speed change: the dialog (ui/Overlays' Dialog, no icon: the title
// in Bold over kDialogTitleW, the body in Small over the same 260 px, 3
// lines), [Cancel] and [Restart] (kDialogButtonTextW). PowerChoices has
// the title ("Restart at 160 MHz?"), the toast while it restarts and the
// next boot's toast (Body on one line, kToastTextX to kToastTextRight).
// What the restart really does: it pauses, and after the boot the entry
// waits at that second (the resume point: QueueSaver), stopped until play.
// To 160 it says the cost too (the title already says it restarts);
// PowerChoices' cpuDialogBody() picks one. A tap on "CPU speed" while a
// pairing is under way (the restart would drop it): a toast, nothing saved.

// Touch calibration (its row; the calibration itself: below): the line
// under its title (Small, kSettingSubW), and a tap's sheet: Calibrate (the
// primary, its detail dim), the check page (one name for one thing: "Touch
// check" is the first-boot dots, "Test taps" the tap-anywhere page), and
// while a table is saved Remove (red), asked in a dialog (kDialogTitleW;
// its body in Small, 3 lines over the same 260 px; [Cancel] [Remove]),
// then a toast (Body).

// ---- the idle power-off (IdlePolicy; ENERGY.md item 4) ----
// The warning toast (ui/Overlays' Toast), its last 30 s: "Turning off in
// 30 s" (Body) from kToastTextX to the button, then [Keep on] (Body) to the
// screen's edge. Any input keeps it on; the button says so.
inline constexpr int kIdleToastKeepX = 214, kIdleToastKeepW = 96;
inline constexpr int kIdleToastPad = 8;  // a label has its pill less this
// The next boot's toast (IdlePolicy::offText(): "Turned off after 20
// minutes idle"): Body on one line, no buttons, kToastTextX to
// kToastTextRight.
// The Output tab's Library row (docs/METADATA.md 3.3.6, N12): its title
// (Body: "Library: 19,410 tracks"), where its tracks' names come from
// (Small: librarytext::sourcesText(), the longest form that fits), and its
// Rescan tags button in a pill (a dialog asks first) narrower than the
// settings' (its one word). Both lines kLibraryRowW wide.
inline constexpr int kRescanPillW = 68;  // "Rescan" (58 px) in its pill
inline constexpr int kLibraryRowW = 312 - 8 - kRescanPillW - 8 - 44;  // from x 44, the row's right 312
// About: a value (Body, or Small when Body doesn't fit). Its library:
// "19,410 tracks, 600 artists, 1500 albums" where that fits in Small,
// else the tracks and the albums (about.library_value_short: "99,999
// tracks, 1500 albums").
inline constexpr int kAboutValueW = 260;
// About's licence rows (GPLv3 section 5(d); main.cpp prints the licence
// once at boot): the copyright and licence (Small) over "no warranty"
// (Body), then the source. The URL is 314 px in Small, too wide for a
// line, so it breaks after the owner: the label (Small, about.source_label)
// over the repo (Body).
inline constexpr const char* kAboutSourceRepo = "mstream-mp3-player";
inline constexpr const char* kSourceUrl = "https://github.com/IrosTheBeggar/mstream-mp3-player";
// About's version row: the commit's date and the first 8 hex digits of the
// ELF's SHA-256 (for crash reports) in the label (Small), over the version
// (Body, or Small for a long dev build's "v0.5.0-12-gabc1234-dirty").
// About's row to Device info (after Memory free): the title (Body) over
// its line (Small), from the icon's column (x 44) to the chevron (the
// list's right edge, a scroll bar's 312, less 26).
inline constexpr int kDeviceInfoRowW = 312 - 26 - 44;
// Device info (Output > About > Device info; DeviceInfo's rows): the
// header's line after its title, and each row's label (Small) over its
// value (Body, or Small when Body doesn't fit), from x kDeviceInfoX to 8 px
// before the scroll bar (14 rows: the list always has one).
inline constexpr int kDeviceInfoX = 14;
// ---- Output > Language (ui/OutputPage) ----
// A language's row: its name in itself (Body; Bold for the one in use)
// from x 16 to the radio's column at the row's right (312 - 44): never
// translated, test_ui_strings measures the ten (the Output row's line
// under "Language" has settings.language's 242 px, in Small).
inline constexpr int kLangRowX = 16;
inline constexpr int kLangRowW = 312 - 44 - kLangRowX;
inline constexpr int kLangLineW = 242;
inline constexpr int kDeviceInfoW = 312 - 8 - kDeviceInfoX;

// ---- the touch check and calibration (ui/CalibrationScreen; TouchCheck) ----
// The whole screen. A 26 px header: at its left, on the pages that have one,
// the way out on the glass as a pill (Bold, kCalPillPad wider than its
// label; its hit area x < 110, TouchCheck's kCancelW); the title (Bold)
// after the widest pill; the progress ("3 of 9", Small) right-aligned at
// x 312.
inline constexpr int kCalHeaderH = 26;
inline constexpr int kCalPillX = 4, kCalPillPad = 20;
inline constexpr int kCalTitleX = 92;
inline constexpr int kCalProgressW = 52;
inline constexpr int kCalTitleW = 312 - kCalProgressW - 6 - kCalTitleX;
// The test taps page has no pill: its title from x 10 (in kCalTitleW, as
// on the other pages), and its status after a Save or an Undo
// (calibration.saved, calibration.undone: Small, right-aligned at x 312)
// has the rest.
inline constexpr int kCalStatusW = 312 - 6 - (10 + kCalTitleW);
// Every line (Body, else Small when Body doesn't fit) across x 8-312. The
// crosses: the first one's hint on two lines (calibration.first_hint), then
// one; a miss (amber). The result (TouchCheck's errorText(): "Now" with the
// table in use, "Calibrated" with the new one), then a line on what to do.
inline constexpr int kCalLineW = 304;
// The rows (full width, 40 px, stacked up to y 220: hit tested by y alone,
// which the panel reads true). A label (Bold for the primary, else Body)
// centred; or, with a detail, the label at the left and the detail (Small,
// dim) right-aligned in what it leaves (kCalRowPad in from each end, a
// kCalRowPad gap).
inline constexpr int kCalRowX = 8, kCalRowW = 304, kCalRowPad = 12, kCalRowsBottom = 220;
// The test taps page: two lines (gone at the first tap: the marks draw
// there), and while a table is saved the grey mark's key in the A hint's
// band, after the page's "A: Done" / "A: Undo": a grey dot (r 3) at
// kCalKeyX, then the text (Small, dim). After a Save the header says so
// (Small, right).
inline constexpr int kCalKeyX = 170, kCalKeyTextX = kCalKeyX + 12, kCalKeyTextW = 312 - kCalKeyTextX;
// The first-boot check: its first dot's hint on two lines, then one; a
// miss. Its result: the verdict (TouchCheck's verdictText(), wrapped over
// two Body lines), the question (Bold), then [Calibrate] [Not now] with
// their details; or, accurate, two lines (Title, Body) and a tap anywhere
// goes on.
// The strip's A is the way out on every page ("A: Cancel": Small, after a
// red arrow down to the A dot at x 54), in the band under the rows (y 222
// to 239). The verbs are the pages' (the pill's, a row's, or these).
inline constexpr int kCalHintY = 222;
inline constexpr int kCalAHintX = 68, kCalAHintW = 120;
// The boot screen's last line (Small, across x 8-312, boot.touch_hint): the
// rescue when the glass is too far off to reach the Output tab (main.cpp).

// ---- the Dance tab (ui/DancePage) ----
// The dancer's name in the right panel (x 220-319, centred): Title, else
// Bold (Spanish's "Cangrejo").
inline constexpr int kDanceNameW = 96;
// The USB visualizer (docs/USB-VISUALIZER.md): while a computer drives the
// dancer, the bottom line (y 196-239, where the track's title and artist
// go, centred, kW - 16 px) says so, the title in Bold over the hint in Small.
inline constexpr int kDanceBottomW = 320 - 16;

// ---- a computer's @identify (docs/HOST-STATUS.md; ui/Ui::identify()) ----
// Over the whole screen, inside a kIdentifyFrame px frame in the Output
// tab's blue: the title (Title, white) centred on y kIdentifyTitleY, over
// the computer's name for its port (at most 16 bytes of printable ASCII:
// hoststatus::kMaxLabel) centred on y kIdentifyLabelY in the blue, in Title
// when it fits kIdentifyW, else Bold (16 of the widest character fit:
// test_ui_library), as wide as the frame leaves less a margin. The title
// stays English whatever the UI's language: the computer's own text (the
// terminal player's MP3 tab) quotes it so.
inline constexpr const char* kIdentifyTitle = "This one";
inline constexpr int kIdentifyFrame = 3;
inline constexpr int kIdentifyW = 320 - 2 * kIdentifyFrame - 16;
inline constexpr int kIdentifyTitleY = 102, kIdentifyLabelY = 140;

// ---- another board than the Core2 (app/BoardGuard) ----
// Drawn in Font2, M5GFX's built-in 16 px bitmap font (the VLW fonts need
// PSRAM, which an M5Stack Basic lacks), one line each from x 4 on a
// 320 px display, with no wrap: English and ASCII, whatever the UI's
// language. Measured in test_ui_library against Font2's widest glyph
// (kFont2MaxAdvance, 'M' and 'W' in M5GFX's Font16.h) for every character,
// so a line fits whatever its letters. The board's name (BoardGuard's
// table, at most kBoardNameMaxChars: a static_assert there) follows
// kBoardGuardFound on its own line.
inline constexpr int kFont2MaxAdvance = 10;
inline constexpr int kBoardGuardW = 312;
inline constexpr int kBoardNameMaxChars = 23;
inline constexpr const char* kBoardGuardTop[2] = {"This firmware is for the", "M5Stack Core2."};
inline constexpr const char* kBoardGuardFound = "found: ";
inline constexpr const char* kBoardGuardStop[2] = {"Stopped: nothing else starts.", "Flash this board's firmware."};
// Under a Tough: M5GFX reads a Core2 as one when something on its internal
// I2C (the M-Bus) answers at the Tough's touch address, 0x2E.
inline constexpr const char* kBoardGuardToughHint = "A Core2? Take off its modules.";

}  // namespace uitext
