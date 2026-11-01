// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// The last things before an orderly restart or power-off, in their order
// (main.cpp's endInOrder(): the idle power-off, the CPU speed's restart,
// restartNow(), which the format's, the card guard's and the console's
// take). First the settings the taps queued (app/NvsLater, SaveLater's
// wait cut short: a tap moments ago is still owed): NVS, quick, with no
// card in it. Then the play log (PlayHistory::flush(): its scan finished,
// every play waiting written and synced), which is the card, and can take
// a while or never come back when the card has gone bad: it costs the
// plays alone, never a setting. Then the end, which doesn't return.
// Portable, host-tested (test_save_later).
namespace orderlyend {

template <class Settings, class Plays, class End>
void finish(Settings settings, Plays plays, End end) {
  settings();
  plays();
  end();
}

}  // namespace orderlyend
