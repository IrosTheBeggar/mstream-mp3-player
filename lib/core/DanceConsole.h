// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DanceCast.h"
#include "DanceChoreo.h"

// The serial console's `D` commands: the dancers, their moves and the
// Choreographer's hooks, for tests, screenshots and artists checking a pack
// (docs/DANCE-SPRITES.md, section 3.9 and Appendix F.2, which gives every
// reply byte for byte). SerialConsole collects `D<arg>` up to Enter, echoes
// `> dance "<arg>"` and DanceMode hands the argument here with its
// Choreographer, its cast and the dancer chosen last.
//
//   D                     the cast, a line an entry, the chosen one marked *
//   Dc<id>                choose a dancer by id (any entry, in the tap cycle or
//                         not): the reply asks DanceMode to choose it, which
//                         prints `[dance] skin: <id>` as `m` does
//   Dl                    the drawn dancer's moves with their labels, and the
//                         octave k each would be danced at now
//   Dm<move>, Dm          force the next beat run (an idle move can't be);
//                         Dm alone gives the pick back to the picker
//   Df<move>.<step>[m]    freeze at a step's start (m: the mirrored form);
//   Df                    Df alone unfreezes
//   Dv                    `[move]` lines on or off: a line for each new beat run
//   Dg<genre>             a genre for this track (until the next track change)
//   Ds<spot>              move the dancer to spot -1, 0 or +1 (refused for a
//                         dancer without travel moves: the crab and the classic
//                         stick dance at the centre)
// Dl, Dm, Df and Ds act on the drawn dancer, Choreographer::character(), not
// on a switch still pending (which clears a forced move and a Df freeze
// when it lands).
//
// A step, for Df: a drawn dancer's steps are its pack move's (MOVS record
// packMove(i)), each starting where the durations before it add up, in
// ticks for a beat move and 10 ms units for an idle one (B.4's pos16). A code
// character (the crab, the classic stick) has no steps of its own: its beat
// move counts eighths of a move beat as steps, so Df<move>.<n> shows
// exactly k<n>'s pose (without the beat dot: a Df freeze never lights it),
// and its continuous idle has none.
//
// Every reply is one line handed to `out` without its newline (DanceMode
// prints it with Serial.println; the tests record it), formatted with
// snprintf into a stack buffer of kConsoleLine bytes, so a longer line is
// cut; with integer and string conversions only. The console keeps only
// Dv's state and the last `[move]` line's run: no heap, no floats.
namespace dance {

using ConsoleOut = void (*)(void* ctx, const char* line);  // one line, without its newline
constexpr int kConsoleLine = 256;  // a line's buffer, NUL included; longer lines are cut
struct ConsoleReply {              // Dc: DanceMode chooses cast entry `index`
  bool choose;
  int index;
};

// F.2's step count and step start of move `move` of c (pos16 as Choreographer::freeze
// takes it). The cast says how c is drawn: an entry with Renderer::Drawn is a
// DrawnCharacter; any other entry, and a c not in the cast, a code character.
// stepStart is false, and leaves *pos16 alone, for a move or step out of range.
int stepCount(const DanceCast& cast, const Character& c, int move);
bool stepStart(const DanceCast& cast, const Character& c, int move, int step, uint32_t* pos16);

class DanceConsole {
 public:
  // One D command: `arg` is the text after the D, as SerialConsole passes it (trimmed;
  // null counts as ""). Replies through out (F.2); `chosen` is DanceMode's chosen entry.
  ConsoleReply run(const char* arg, Choreographer& ch, const DanceCast& cast, int chosen, ConsoleOut out,
                   void* ctx);
  // Once a frame, after Choreographer::update(): while Dv is on, a [move] line for each
  // new beat run (F.2).
  void frame(const Choreographer& ch, ConsoleOut out, void* ctx);
  void reset() { printed_ = false; }  // dance on: forgets the last run printed (Dv's state stays)
  bool verbose() const { return verbose_; }  // Dv

 private:
  bool verbose_ = false;
  bool printed_ = false;  // a [move] line since reset() or since Dv went on
  // The last [move] line's run: (move, start, k, plays, firstMirror, level).
  int8_t move_ = kNoMove;
  int8_t k_ = 0;
  uint8_t plays_ = 0;
  uint8_t level_ = 0;
  bool mirror_ = false;
  int64_t start_ = 0;
};

}  // namespace dance
