// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <functional>
#include <utility>

#include "HostStatus.h"

// The computer's questions about the board (docs/HOST-STATUS.md), on the
// USB serial port's '@' lines (HostLink routes them; app/UsbViz hands them
// here; a refusal goes back through HostLink's @err and its rate limit):
//
//   @status            one line at once: the version, the card, its size and
//                      free space, the library's tracks, the battery, the
//                      player, the paired headphones (hoststatus::format()),
//                      from what the firmware holds: never a read of the
//                      card (storage/CardSpace: FatFs's free count as a word).
//   @count             one count of the card's free clusters, on request
//                      (storage/CardSpace, lib/core FreeCount): "@count 0"
//                      at once, "@count <percent>" about every tenth, then
//                      "@count 100" and "@count done free=<bytes>"; a piece
//                      of the FAT a loop pass. Refused (the hook says why)
//                      while the player plays or a play waits, while the
//                      library updates, with no FAT card mounted, while a
//                      count runs; stopped part way for the same reasons
//                      (the @err the refusal would have been ends it, and
//                      nothing changed). Never at boot.
//   @identify <label>  "This one" and the label over the whole screen for
//                      5 s and one buzz (ui/Ui::identify(), main.cpp's hook):
//                      "@identify ok".
//
// Logged: a count's start and end; an identify. Not a @status (a computer
// may ask every second). Loop task.
class HostQuery {
public:
  struct Hooks {
    // Everything @status says but the free space (this class's: the card's
    // FAT type and FatFs's count come from storage/CardSpace when the card
    // is mounted).
    std::function<void(hoststatus::Facts&)> facts;
    // Why a count can't start, or go on, now: "playing", "library"; nullptr:
    // nothing in the way.
    std::function<const char*()> countBlocked;
    // The banner for `label` (valid: HostLink checked it). nullptr: shown,
    // or it will be once the screen is awake; else why not ("ui",
    // "screen", "viz").
    std::function<const char*(const char* label)> identify;
  };

  explicit HostQuery(Hooks hooks) : hooks_(std::move(hooks)) {}

  // @status: its line, written now.
  void status();
  // @count: nullptr, it started ("@count 0" written); else the refusal's
  // detail for "@err 4 count <detail>".
  const char* count(uint32_t nowMs);
  // @identify: nullptr, "@identify ok" is the answer; else the refusal's
  // detail for "@err 4 identify <detail>".
  const char* identify(const char* label);
  // Every loop pass: a step of the count under way, its lines.
  void loop(uint32_t nowMs);
  bool counting() const;

private:
  // A count stopped part way (countBlocked()'s `detail`): its last line.
  void stop(const char* detail);

  Hooks hooks_;
  hoststatus::Progress progress_;
};
