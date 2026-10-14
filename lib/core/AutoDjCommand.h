// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The console's D (Auto DJ; the engine design's 6.4), parsed: the text
// after the D, up to Enter. Portable, host-tested (test_autodj_driver); the
// firmware's glue (src/app/AutoDj) carries it out. Scripts drive these in
// the silent test mode (z): none of them touches the output or the volume.
//
//   D            status: the settings, the index (or why there is none),
//                the join, the session, the counters, the memory, the last
//                pick's reads and time
//   D0 / D1      off / on (saved, as the UI will)
//   Dm0 / Dm1    Anchor: Follow the vibe / Stay on seed (saved)
//   Df55..Df75   Match strictness, by 5 (saved)
//   Dt0 / Dt1    BPM continuity off / on (saved)
//   Dk0 / Dk1    Harmonic mixing off / on (saved)
//   Ds<hex>      the next session's seed (1-8 hex digits; reproducible)
//   Dr           a new session now
//   Dp           a pick appended now, whatever the trigger says (cued at a
//                finished queue); its decision printed
//   Dn<k>        k dry-run picks (1 to 2,000) on a copy of the session, one
//                a loop pass; a summary at the end (the live session untouched)
//   Dj           the join again (its time and memory)
//   Dc           the file closed and opened again, as a remount that found
//                another index does (a new session)
//   Db<n>        the bench: n slot reads of random rows (1 to 5,000), one a
//                loop pass; their times, and the checks'
//   Dh / Dh<id>  "Auto DJ from here" on the current entry's track, or on
//                library track <id>: played as a one-track Play, Auto DJ
//                on, a session seeded by it (M5's long-press row)
//   De           the empty states' Auto DJ button: a random track with
//                neighbours played the same way (needs the index)
//   Dx           a long command under way (Dn, Db) stopped, its summary so far
//   D?           this list
namespace autodj {

struct Command {
  enum class Kind : uint8_t {
    Bad,
    Status,
    Off,
    On,
    Mode,      // n: 0 Follow the vibe, 1 Stay on seed
    Floor,     // n: the percent
    Tempo,     // n: 0 off, 1 on
    Key,       // n: 0 off, 1 on
    Seed,      // n: the seed
    Renew,
    Pick,
    DryRun,    // n: the picks
    Join,
    Reopen,
    Bench,     // n: the reads
    FromHere,  // n: a library track, or kCurrent
    Surprise,
    Stop,
    Help,
  };
  static constexpr uint32_t kCurrent = 0xFFFFFFFFu;  // FromHere: the current entry's track
  static constexpr uint32_t kMaxDryRun = 2000;
  static constexpr uint32_t kMaxBench = 5000;
  Kind kind = Kind::Bad;
  uint32_t n = 0;
};

// `arg`: the text after the D (leading and trailing spaces ignored); null
// as "".
Command parseCommand(const char* arg);
// The list above, one line (the console's D? and a bad D).
const char* commandHelp();

}  // namespace autodj
