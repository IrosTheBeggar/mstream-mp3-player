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
// The speaker card: its status line (Small), before the volume chip.
inline constexpr int kSpeakerLineW = 160;
inline constexpr int kSpeakerChipX = 218, kSpeakerChipW = 48;
inline constexpr const char* kSilentMode = "Silent test mode";
inline constexpr const char* kForgetArmed = "Tap again";
// The Pair screen's hint (Small, x 48 to 312).
inline constexpr int kPairHintW = 264;
inline constexpr const char* kPairHint = "Pairing mode on, then tap them.";
// Settings rows: the line under a title with no chevron (Small).
inline constexpr int kSettingSubW = 204;
inline constexpr const char* kLineOutSub = "3.5 mm / RCA: not fitted yet";
// About: a value (Body, or Small when Body doesn't fit).
inline constexpr int kAboutValueW = 260;
inline constexpr const char* kAboutMemory = "RAM %lu KB (low %lu), PSRAM %.1f MB";
inline constexpr const char* kAboutLibrary = "%lu tracks, %lu artists, %lu albums";

}  // namespace uitext
