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
// "Plays next: <name>" on two lines: the name (Body, else Small) from x 22
// to the View icon's pill.
inline constexpr int kToastTextX = 22;
inline constexpr int kToastCompactTextRight = 238;
inline constexpr int kToastTextRight = 306;  // a note (Ui::warn) on one line, no buttons
// A B click refused by the pocket rule (the screen woke from off and
// nothing touched the glass since; ScreenPower::unattended()).
inline constexpr const char* kTouchFirst = "Tap the screen first, then B plays";

// ---- the empty states (ui/EmptyState): two buttons ----
// The primary (Bold, its icon 26 px + 8 before the text) and the second
// (Body, its icon), each with 6 px inside at each end.
inline constexpr int kEmptyPrimaryX = 12, kEmptyPrimaryW = 166;
inline constexpr int kEmptySecondX = 184, kEmptySecondW = 124;
inline constexpr int kEmptyIconW[2] = {26, 20};  // the Library's and Shuffle's icons (ui/IconData.cpp)
inline constexpr int kEmptyLineW = 304;  // a line (Small), centred
inline constexpr const char* kPickInLibrary = "Pick an album, folder or track in the Library.";

// ---- Now Playing (ui/NowPlayingPage) ----
// The progress line's middle (Small), centred between the times (from
// x 12 and to 308): "4 of 16 · SPYDRONE", or the headphones' "SPYDRONE (not
// connected)" (177 px: 170 cut it).
inline constexpr int kNowPlayingMidW = 190;

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
inline constexpr int kSettingPillW = 76;
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

}  // namespace uitext
