// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// Fixed texts of the UI that have a fixed room, next to that room, so the
// host tests (test_ui_library) can measure them with the firmware's own
// DejaVu data: a text cut to "Remov…" or "Tap ag…" on the device is a
// failing test here. The rooms are the pixels ui/ gives each text (see
// where each is drawn); the fonts are ui/Fonts' (Body = DejaVu Sans 16,
// Small = 13, Bold = Bold 16, Title = Bold 22).
namespace uitext {

// ---- the first-boot tips (ui/Overlays, Coach) ----
// Card 1: the title (Bold) and its line (Small) across the width.
inline constexpr int kCoachTextW = 308;
inline constexpr const char* kCoachTitle = "The three red dots";
inline constexpr const char* kCoachLine = "Under the screen: a click, or a hold, anywhere";
// Its three boxes (102 px, 4 px apart), each: the click (Bold) and the hold (Small).
inline constexpr int kCoachBoxW = 102;
inline constexpr int kCoachBoxTextW = kCoachBoxW - 4;
inline constexpr const char* kCoachClick[3] = {"Previous", "Play/pause", "Next"};
inline constexpr const char* kCoachHold[3] = {"volume down", "speaker or", "volume up"};
inline constexpr const char* kCoachHold2[3] = {"", "headphones", ""};
// Card 2: two Title lines, then Small ones.
inline constexpr const char* kCoachTab[2] = {"Tap the tab you're on", "again: back to its start"};
inline constexpr const char* kCoachTabMore[3] = {"The Library goes back to its lists, the Queue to",
                                                  "the playing track, Output leaves pairing.",
                                                  "These tips are in Output > About."};

// ---- the toast (ui/Overlays) ----
// Undo at the screen's edge (its hit area to the edge, a clamped reading
// too), View left of it; on two lines ("Plays next" over a long name) both
// as icons. The split between them sits well right of View's centre: with
// no correction (the default), a panel that reads right like the lab's
// (+28 px at x 214, +27 at 244: TouchCalibration::labFitX()) still hits
// View there, never Undo, which would take the add back (test_ui_library).
inline constexpr int kToastUndoX = 256, kToastUndoW = 54, kToastUndoHitX = 250;
inline constexpr int kToastViewX = 186, kToastViewW = 56, kToastViewHitX = 180;
inline constexpr int kToastUndoCX = 282, kToastUndoCW = 30, kToastUndoCHitX = 278;
inline constexpr int kToastViewCX = 230, kToastViewCW = 28, kToastViewCHitX = 214;
// What a tap on the toast is: 2 Undo, 3 View, 1 elsewhere on it.
constexpr int toastButtonAt(int x, bool rightEdge, bool compact, bool undo, bool view) {
  const int undoHit = compact ? kToastUndoCHitX : kToastUndoHitX;
  const int viewHit = compact ? kToastViewCHitX : kToastViewHitX;
  if (undo && (rightEdge || x >= undoHit)) return 2;
  if (view && !rightEdge && x >= viewHit && x < undoHit) return 3;
  return 1;
}
// "Plays next: <name>" on two lines: the name (Body, else Small) from x 22
// to the View icon's pill.
inline constexpr int kToastTextX = 22;
inline constexpr int kToastCompactTextRight = kToastViewCX - 4;
inline constexpr int kToastTextRight = 306;  // a note (Ui::warn) on one line, no buttons
// A B click refused by the pocket rule (the screen woke from off and
// nothing touched the glass since; ScreenPower::unattended()).
inline constexpr const char* kTouchFirst = "Tap the screen first, then B plays";
// Headphones asked for with none paired (the B hold, a play on Bluetooth):
// nothing scans for them, so the note says where pairing is. Ui::warn on
// two lines (Toast's "what: where"): the part before ": " in Small over
// the rest in Body, each from kToastTextX to 306 (no buttons).
inline constexpr const char* kNoHeadphones = "No headphones paired: Output > Pair new headphones";
inline constexpr int kToastTwoLineW = 306 - kToastTextX;
// A track skipped (Ui::noteFailures): "Skipped <title>: <why>", on Toast's
// two lines when long (the why in Body). Why: kSkipped, or for a sample
// rate the converter refused, kSkippedRate (its %s: RateConverter::
// rateText(), "96 kHz", "37.8 kHz", "44056 Hz") or kSkippedCpu (88.2/96 kHz
// at 160 MHz: a setting would play it, on the Output tab).
inline constexpr const char* kSkipped = "can't play it";
inline constexpr const char* kSkippedRate = "%s isn't supported";
inline constexpr const char* kSkippedCpu = "needs the 240 MHz CPU speed";

// ---- the empty states (ui/EmptyState): two buttons ----
// The primary (Bold, its icon 26 px + 8 before the text) and the second
// (Body, its icon), each with 6 px inside at each end.
inline constexpr int kEmptyPrimaryX = 12, kEmptyPrimaryW = 166;
inline constexpr int kEmptySecondX = 184, kEmptySecondW = 124;
inline constexpr int kEmptyIconW[2] = {26, 20};  // the Library's and Shuffle's icons (ui/IconData.cpp)
inline constexpr int kEmptyLineW = 304;  // a line (Small), centred
inline constexpr const char* kPickInLibrary = "Pick an album, folder or track in the Library.";
// The title (Title, centred in kW - 16).
inline constexpr int kEmptyTitleW = 304;
// No card (Now Playing, the Library and the Queue with no card and no
// music on the flash): the title, two lines and [Try again]; its note when
// the card still isn't there (Ui::warn, Toast's "what: where" on two lines:
// the part before ": " in Small over the rest in Body, kToastTwoLineW).
inline constexpr const char* kNoCardTitle = "No microSD card";
inline constexpr const char* kNoCardLines[2] = {"Insert a card with your music in /music,",
                                                "as /music/Artist/Album/01 - Title.mp3"};
inline constexpr const char* kStillNoCard = "Still no card: is it all the way in?";
// ... and when a card is in that isn't FAT32 (exFAT, NTFS, a GPT:
// cardformat, read when it didn't mount). README's and the release notes'
// microSD card sections say the same.
inline constexpr const char* kNotFat32Title = "This card isn't FAT32";
inline constexpr const char* kNotFat32Lines[2] = {"Format it FAT32 (MBR) on a computer, then",
                                                  "put your music in /music and tap Try again."};
inline constexpr const char* kStillNotFat32 = "Still not FAT32: format it FAT32 (MBR)";
inline constexpr const char* kTryAgain = "Try again";

// ---- Now Playing (ui/NowPlayingPage) ----
// The progress line's middle (Small), centred between the times (from
// x 12 and to 308): "4 of 16 · SPYDRONE", or the headphones' "SPYDRONE (not
// connected)" (177 px: 170 cut it).
inline constexpr int kNowPlayingMidW = 190;
// The seek bar's readout (docs/SEEK-BAR.md section 4.2), in the row above
// the line while a finger scrubs, on the side away from the knob: the
// finger's second ("2:31", Title) and, kSeekReadoutGap px after it, the
// change ("+1:21", Small), or "no change" back where it plays; the group
// at most kSeekReadoutW wide: a mix's "999:59 no change" is 159 px (150
// cut "no change" from 100 min on), and from x 12 (or to x 308) it still
// ends 12 px short of where the knob sends it across (SeekBar's
// kReadoutLeftX, kReadoutRightX). Slid off the bar: kSeekCancel (Bold,
// amber), centred in the line's 296 px.
inline constexpr int kSeekReadoutW = 160;
inline constexpr int kSeekReadoutGap = 8;
inline constexpr const char* kSeekStay = "no change";
inline constexpr const char* kSeekCancel = "Release to cancel";

// ---- play waiting for the headphones (ui/NowPlayingPage, ui/Ui) ----
// Now Playing's panel over the artist and album bands (x 112-319, y 90-169):
// "Waiting for SPYDRONE..." (Small, amber) over "try 2 of 3" (Small, dim),
// then two buttons (Body), x from the band's left.
inline constexpr int kWaitTextX = 8, kWaitTextW = 196;
inline constexpr int kWaitSpeakerX = 4, kWaitSpeakerW = 136;
inline constexpr int kWaitCancelX = 144, kWaitCancelW = 62;
inline constexpr int kWaitButtonPad = 8;  // a label has the box less this
inline constexpr const char* kPlayOnSpeaker = "Play on speaker";
inline constexpr const char* kWaitCancel = "Cancel";
// The notice when they can't be reached (ui/Overlays' Dialog: no icon, so
// "Couldn't reach SPYDRONE" has its title's 260 px; the body in Small over
// 260 px; two buttons of 131 px, their labels 8 px in: "Play on speaker"
// is 4 px too wide in Body, so the Dialog draws it in Small).
inline constexpr int kDialogTitleW = 260;
inline constexpr int kDialogButtonTextW = 123;
inline constexpr const char* kPlayFailedBody = "Are they on, out of the case, and not connected to your phone?";

// ---- the sleep timer (SleepTimer; docs/ENERGY.md section 3) ----
// Now Playing's "..." sheet: its first row (Body), the timer's state dim
// on the right (SleepTimer::rowText(): "Off", "23 min", "End of queue").
inline constexpr const char* kSleepRow = "Sleep timer";
// The Sleep timer sheet (ui/Overlays' SleepSheet, the sheet panel 320 x
// 168 from y 72): its title (Small) left of the ✕ pill (x 16 to 242), then
// three rows of pills 36 px tall, 44 px apart. The running choice is
// outlined in the Now Playing accent. A label has its pill less
// kSleepPillPad.
inline constexpr int kSleepTitleW = 226;
inline constexpr int kSleepPillPad = 8;
// Row 1 (Body): the minutes; "90 min" says what the numbers are.
inline constexpr const char* kSleepMinutes[5] = {"15", "30", "45", "60", "90 min"};
inline constexpr int kSleepMinX[5] = {12, 68, 124, 180, 236}, kSleepMinW[5] = {50, 50, 50, 50, 72};
// Row 2 (Small: "End of album" is 106 px in Body).
inline constexpr const char* kSleepEnds[3] = {"End of track", "End of album", "End of queue"};
inline constexpr int kSleepEndX[3] = {12, 112, 212}, kSleepEndW[3] = {94, 94, 96};
// Row 3 while a timer runs (Body): +10 min, and Turn off in red (to the edge).
inline constexpr const char* kSleepExtend = "+10 min";
inline constexpr const char* kSleepTurnOff = "Turn off";
inline constexpr int kSleepExtendX = 12, kSleepExtendW = 144;
inline constexpr int kSleepOffX = 162, kSleepOffW = 146;
// ... and while none runs, a line (Small) across the row.
inline constexpr int kSleepHintW = 296;
inline constexpr const char* kSleepHint = "Fades out, pauses, then the screen goes off.";
// The toast during the fade (ui/Overlays' Toast): its line (Small) from
// kToastTextX to the +10 min pill, then +10 min and Turn off (Body),
// Turn off reaching the screen's edge.
inline constexpr const char* kSleepFading = "Sleep timer: fading";
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
// Play next and Clear… (Body).
inline constexpr int kRemoveX = 6, kRemoveW = 140;
inline constexpr int kRemoveTextX = 32;  // from the button's left, after the icon
inline constexpr int kQueueNextX = 152, kQueueNextW = 80;
inline constexpr int kQueueClearX = 238, kQueueClearW = 76;

// ---- the Output tab (ui/OutputPage) ----
// The Bluetooth card's status line (Small, x 52 to the radio).
inline constexpr int kBtStatusW = 222;
// Its buttons, by how many (x, width); Bold for Pair and an armed Forget.
inline constexpr int kBtButtons1X = 16, kBtButtons1W = 120;  // Cancel, Try again (Pair: the width)
inline constexpr int kBtButtons2X[2] = {16, 142}, kBtButtons2W[2] = {120, 100};
inline constexpr int kBtButtons3X[3] = {16, 124, 192}, kBtButtons3W[3] = {100, 60, 112};
inline constexpr int kBtButtonPad = 8;       // a label has the box less this
inline constexpr int kBtWideButtonW = 200;   // narrower boxes take the short labels (btButtonLabel)
inline constexpr int kBtChipTextX = 30;      // the volume chip: its % after the speaker icon
// The resting card (the background search stopped): "Not connected" on the
// status line, and these two lines (Small) beside its one button,
// [Connect]: "Not connected. They'll reconnect when switched on."
inline constexpr int kBtHintX = kBtButtons1X + kBtButtons1W + 12, kBtHintW = 320 - 8 - kBtHintX;
inline constexpr const char* kBtHintLine1 = "They'll reconnect";
inline constexpr const char* kBtHintLine2 = "when switched on.";
// ... when they dropped while the output (BtCardView::lostHint): resting
// only after the whole back-off (15 min), and they may be on and back in
// range with their own reconnect given up. The lost dialog's line then.
inline constexpr const char* kBtLostHintLine1 = "Back in range?";
inline constexpr const char* kBtLostHintLine2 = "Tap Connect.";
inline constexpr const char* kBtLostRestingLine = "Stopped looking for them: Play tries again";
// The speaker card: its status line (Small), before the volume chip.
inline constexpr int kSpeakerLineW = 160;
inline constexpr int kSpeakerChipX = 218, kSpeakerChipW = 48;
// Its hit area: 6 px before it, and 10 px past it (the radio is at 280-300:
// a tap on the chip's centre that reads ~27 px right, as on the lab's panel
// with no correction, is still the chip, not the row's "to the speaker").
inline constexpr int kSpeakerChipHitX = kSpeakerChipX - 6;
inline constexpr int kSpeakerChipHitEnd = kSpeakerChipX + kSpeakerChipW + 10;
inline constexpr const char* kSilentMode = "Silent test mode";
inline constexpr const char* kForgetArmed = "Tap again";
// The Pair screen's hint (Small, x 48 to 312).
inline constexpr int kPairHintW = 264;
inline constexpr const char* kPairHint = "Pairing mode on, then tap them.";
// ... once its scan stopped by itself (2 min: OutputModel's PairSearch; or
// the screen went off).
inline constexpr const char* kPairSearchAgain = "Search again";
inline constexpr const char* kPairSearchStopped = "Stopped, to save the battery.";
// Settings rows: the line under a title with no chevron (Small).
inline constexpr int kSettingSubW = 204;
inline constexpr const char* kLineOutSub = "3.5 mm / RCA: not fitted yet";
// Settings rows with a value (Screen off after, Brightness: ScreenPower's
// choices): the value (Body) in a pill at the row's right (8 px from its
// right end), the title and its line (Small) up to 8 px before the pill.
inline constexpr int kSettingPillW = 78;  // "160 MHz" (69 px) in its pill
inline constexpr int kSettingPillPad = 8;  // a value has the pill less this
inline constexpr int kSettingValueSubW = 312 - 8 - kSettingPillW - 8 - 44;  // from x 44, the row's right 312
inline constexpr const char* kScreenOffTitle = "Screen off after";
inline constexpr const char* kScreenOffSub = "dims first; a tap wakes it";
inline constexpr const char* kScreenNeverSub = "stays on: more battery";
inline constexpr const char* kBrightnessTitle = "Brightness";
inline constexpr const char* kBrightnessSub = "higher uses more battery";
// "Turn off when idle" (IdlePolicy's choices, the same row: ENERGY.md item 4).
inline constexpr const char* kIdleOffTitle = "Turn off when idle";
inline constexpr const char* kIdleOffSub = "paused, on battery";
inline constexpr const char* kIdleNeverSub = "stays on: more battery";
// "CPU speed" and "Bluetooth power" (PowerChoices' choices, the same row:
// ENERGY.md items 6 and 7). The CPU's line while the clock isn't the saved
// choice yet (the console's Pcb): "240 MHz until a restart".
inline constexpr const char* kCpuTitle = "CPU speed";
// 160's line says what it costs: lists scrolled at about half the frame
// rate with an MP3 playing (8 against 16 fps), the dancer at ~18 of 24,
// for ~5 mA (ENERGY.md step 6a).
inline constexpr const char* kCpu240Sub = "Smoothest lists, dancing";
inline constexpr const char* kCpu160Sub = "Slower lists, saves a little";
inline constexpr const char* kCpuPendingSub = "%u MHz until a restart";
inline constexpr const char* kBtPowerTitle = "Bluetooth power";
inline constexpr const char* kBtLowSub = "Saves battery; stay close";
inline constexpr const char* kBtNormalSub = "Adjusts to the distance";
inline constexpr const char* kBtHighSub = "More range, more battery";
inline constexpr const char* kBtPendingSub = "From the next connection";
// A CPU speed change: the dialog (ui/Overlays' Dialog, no icon: the title
// in Bold over kDialogTitleW, the body in Small over the same 260 px, 3
// lines), [Cancel] and [Restart] (kDialogButtonTextW). PowerChoices has
// the title ("Restart at 160 MHz?"), the toast while it restarts and the
// next boot's toast (Body on one line, kToastTextX to kToastTextRight).
// What the restart really does: it pauses, and after the boot the entry
// waits at that second (the resume point: QueueSaver), stopped until play.
// To 160 it says the cost too (the title already says it restarts);
// PowerChoices' cpuDialogBody() picks one.
inline constexpr const char* kCpuDialogBody =
    "The speed changes at a restart. The music pauses and picks up at the same second.";
inline constexpr const char* kCpuDialogBody160 =
    "Saves a little battery; lists scroll at half speed while music plays. "
    "Music pauses and picks up at the same second.";
inline constexpr const char* kCpuRestart = "Restart";
// A tap on "CPU speed" while a pairing is under way (the restart would drop
// it): a toast, nothing saved.
inline constexpr const char* kCpuWaitPairing = "Wait for the pairing to finish";

// Touch calibration (its row; the calibration itself: below): the line
// under its title (Small, kSettingSubW), and a tap's sheet: Calibrate (the
// primary, its detail dim), the check page, and while a table is saved
// Remove (red), asked in a dialog (kDialogTitleW; its body in Small, 3
// lines over the same 260 px; [Cancel] [Remove]), then a toast (Body).
inline constexpr const char* kCalRowTitle = "Touch calibration";
inline constexpr const char* kCalSubOff = "Not calibrated";
inline constexpr const char* kCalSubOn = "Calibrated on this Core2";
inline constexpr const char* kCalCalibrate = "Calibrate";
inline constexpr const char* kCalCalibrateDetail = "9 crosses, 20 s";
// (One name for one thing: "Touch check" is the first-boot dots, "Test
// taps" the tap-anywhere page.)
inline constexpr const char* kCalCheckTitle = "Test taps";
inline constexpr const char* kCalRemoveRow = "Remove calibration";
inline constexpr const char* kCalRemoveTitle = "Remove the calibration?";
inline constexpr const char* kCalRemoveBody =
    "Then taps are read as the panel reports them. Calibrate again here any time.";
inline constexpr const char* kCalRemove = "Remove";
inline constexpr const char* kCalRemoved = "Calibration removed";

// ---- the idle power-off (IdlePolicy; ENERGY.md item 4) ----
// The warning toast (ui/Overlays' Toast), its last 30 s: "Turning off in
// 30 s" (Body) from kToastTextX to the button, then [Keep on] (Body) to the
// screen's edge. Any input keeps it on; the button says so.
inline constexpr const char* kIdleKeepOn = "Keep on";
inline constexpr int kIdleToastKeepX = 214, kIdleToastKeepW = 96;
inline constexpr int kIdleToastPad = 8;  // a label has its pill less this
// The next boot's toast (IdlePolicy::offText(): "Turned off after 20
// minutes idle"): Body on one line, no buttons, kToastTextX to
// kToastTextRight.
// About: a value (Body, or Small when Body doesn't fit).
inline constexpr int kAboutValueW = 260;
inline constexpr const char* kAboutMemory = "RAM %lu KB (low %lu), PSRAM %.1f MB";
inline constexpr const char* kAboutLibrary = "%lu tracks, %lu artists, %lu albums";
// About's power row: the two settings as they run (PowerChoices::aboutText():
// "160 MHz; Normal (-12..+3 dBm)").
inline constexpr const char* kAboutPower = "CPU speed, Bluetooth power";
// About's licence rows (GPLv3 section 5(d); main.cpp prints the same once
// at boot): the copyright and licence (Small) over "no warranty" (Body),
// then the source. The URL is 314 px in Small, too wide for a line, so it
// breaks after the owner: the label (Small) over the repo (Body).
inline constexpr const char* kAboutLicenceLabel = "Licence (\xC2\xA9 2026 IrosTheBeggar)";
inline constexpr const char* kAboutLicence = "GPL-3.0-or-later, no warranty";
inline constexpr const char* kAboutSourceLabel = "Source: github.com/IrosTheBeggar/";
inline constexpr const char* kAboutSourceRepo = "mstream-mp3-player";
inline constexpr const char* kSourceUrl = "https://github.com/IrosTheBeggar/mstream-mp3-player";
// About's version row: the commit's date and the first 8 hex digits of the
// ELF's SHA-256 (for crash reports) in the label (Small), over the version
// (Body, or Small for a long dev build's "v0.5.0-12-gabc1234-dirty").
inline constexpr const char* kAboutVersionLabel = "Version (%s, ELF %s)";

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
inline constexpr const char* kCalCancel = "Cancel";
inline constexpr const char* kCalSkip = "Skip";
inline constexpr const char* kCalTitle = "Touch calibration";
inline constexpr const char* kCheckTitle = "Touch check";
// Every line (Body, else Small when Body doesn't fit) across x 8-312.
inline constexpr int kCalLineW = 304;
// The crosses: the first one's hint on two lines ("%d": how many), then
// one; a miss (amber).
inline constexpr const char* kCalFirstHint[2] = {"Tap the centre of each cross",
                                                 "with the finger you use. %d crosses."};
inline constexpr const char* kCalHint = "Tap the centre of the cross";
inline constexpr const char* kCalMissed = "Missed: tap the cross itself";
// The result (TouchCheck's errorText(): "Now" with the table in use,
// "Calibrated" with the new one), then a line on what to do.
inline constexpr const char* kCalNow = "Now";
inline constexpr const char* kCalNew = "Calibrated";
inline constexpr const char* kCalSaveLine = "Save it to use it from now on.";
inline constexpr const char* kCalAccurate = "Already accurate: no need to save";
inline constexpr const char* kCalNoBetter = "No better than now: no need to save";
inline constexpr const char* kCalDisagree[2] = {"The taps didn't agree. Try again,",
                                                "tapping the centre of each cross."};
// The rows (full width, 40 px, stacked up to y 220: hit tested by y alone,
// which the panel reads true). A label (Bold for the primary, else Body)
// centred; or, with a detail, the label at the left and the detail (Small,
// dim) right-aligned in what it leaves (kCalRowPad in from each end, a
// kCalRowPad gap).
inline constexpr int kCalRowX = 8, kCalRowW = 304, kCalRowPad = 12, kCalRowsBottom = 220;
inline constexpr const char* kCalSave = "Save";
inline constexpr const char* kCalTryAgain = "Try again";
inline constexpr const char* kCalDiscard = "Discard";
inline constexpr const char* kCalDone = "Done";
// The test taps page: two lines (gone at the first tap: the marks draw
// there), and while a table is saved the grey mark's key in the A hint's
// band, after the page's "A: Done" / "A: Undo": a grey dot (r 3) at
// kCalKeyX, then the text (Small, dim). After a Save the header says so
// (Small, right).
inline constexpr const char* kCalCheckLines[2] = {"Tap anywhere. The ring should land", "right under your finger."};
inline constexpr const char* kCalCheckKey = "without calibration";
inline constexpr int kCalKeyX = 170, kCalKeyTextX = kCalKeyX + 12, kCalKeyTextW = 312 - kCalKeyTextX;
inline constexpr const char* kCalSaved = "Saved";
inline constexpr const char* kCalUndone = "Undone";
// The first-boot check: its first dot's hint on two lines, then one; a miss.
inline constexpr const char* kCheckFirstHint[2] = {"Is the touch right? Tap the centre",
                                                   "of each dot, one at a time."};
inline constexpr const char* kCheckHint = "Tap the centre of the dot";
inline constexpr const char* kCheckMissed = "Missed: tap the dot itself";
// Its result: the verdict (TouchCheck's verdictText(), wrapped over two
// Body lines), the question (Bold), then [Calibrate] [Not now] with their
// details; or, accurate, two lines (Title, Body) and a tap anywhere goes on.
inline constexpr const char* kCheckAsk = "Calibrate now?";
inline constexpr const char* kCheckNotNow = "Not now";
inline constexpr const char* kCheckLater = "Output > Touch calibration";  // where it is later
inline constexpr const char* kCheckAccurate = "Touch is accurate.";
inline constexpr const char* kCheckGoOn = "Tap anywhere to go on.";
// The strip's A is the way out on every page ("A: Cancel": Small, after a
// red arrow down to the A dot at x 54), in the band under the rows (y 222
// to 239). The verbs are the pages' (the pill's, a row's, or these).
inline constexpr int kCalHintY = 222;
inline constexpr int kCalAHintX = 68, kCalAHintW = 120;
inline constexpr const char* kCalUndo = "Undo";
// The boot screen's last line (Small, across x 8-312): the rescue when the
// glass is too far off to reach the Output tab (main.cpp).
inline constexpr const char* kBootTouchHint = "Touch trouble? Hold a finger on the screen.";

// ---- the Dance tab (ui/DancePage) ----
// The USB visualizer (docs/USB-VISUALIZER.md): while a computer drives the
// dancer, the bottom line (y 196-239, where the track's title and artist
// go, centred, kW - 16 px) says so, the title in Bold over the hint in Small.
inline constexpr int kDanceBottomW = 320 - 16;
inline constexpr const char* kVizTitle = "Dancing to your computer";
inline constexpr const char* kVizHint = "Tap a button or a tab to stop";

// ---- another board than the Core2 (app/BoardGuard) ----
// Drawn in Font2, M5GFX's built-in 16 px bitmap font (the VLW fonts need
// PSRAM, which an M5Stack Basic lacks), one line each from x 4 on a
// 320 px display, with no wrap. Measured in test_ui_library against Font2's
// widest glyph (kFont2MaxAdvance, 'M' and 'W' in M5GFX's Font16.h) for
// every character, so a line fits whatever its letters. The board's name
// (BoardGuard's table, at most kBoardNameMaxChars: a static_assert there)
// follows kBoardGuardFound on its own line.
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
