// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DanceCharacter.h"

// The cast: every dancer the Dance tab can show, in order, and which of
// them a tap cycles through (docs/DANCE-SPRITES.md, section 1.5 and
// Appendix F.1). It replaces DanceSkin's two-value enum: a dancer is now a
// Character (DanceCharacter.h) and the renderer that draws it, found by its
// id, so `[dance] on (crab)`, `[dance] skin: <id>` and the stats line's
// `skin=<id>` keep their shape with info().id in place of skinName().
//
// The order is fixed here, not in src/: addBuiltIns() appends the crab
// (entry 0, kDefaultDancer, the first-boot dancer) and then the classic stick
// figure. Step 4's cast is just those two, both in the tap cycle, so `m` and
// a tap go crab -> stick -> crab as before. Later steps add to it:
//   - step 7: the drawn stick figure takes the id `stick` (Renderer::Drawn)
//     and the classic one becomes `stick-classic`, out of the cycle;
//   - step 9: the chosen dancer's id is saved and found again with find();
//   - step 10: card packs are appended after the built-ins in file-name
//     order, so of two with one id the first wins (add() refuses the
//     second), and an override replaces a drawn built-in's entry only
//     after its full load (a replace() that step adds).
//
// The cast holds pointers, never copies: each Character must outlive it. A
// Renderer::Drawn entry always holds a DrawnCharacter (DanceConsole relies on
// it). kMaxCast entries of 8 B (on the ESP32) are 152 B; DanceMode allocates
// the cast in PSRAM. Portable, no heap.
namespace dance {

enum class Renderer : uint8_t { Crab, Classic, Drawn };  // which src/ui renderer draws an entry
struct CastEntry {
  const Character* character;
  Renderer renderer;
};
constexpr int kMaxCast = 19;       // crab, stick, stick-classic and 16 card packs (decision 16)
constexpr int kDefaultDancer = 0;  // the crab: the first-boot dancer (decision 8)

class DanceCast {
 public:
  // Appends c, drawn by r. False, and nothing added, for a null c, a full cast, or an
  // id (info().id, compared byte for byte) already in the cast: the first one wins.
  bool add(const Character* c, Renderer r);
  int size() const { return count_; }
  const CastEntry& at(int i) const;       // 0 <= i < size()
  int find(const char* id) const;         // the entry with that id; -1 for none or null
  int indexOf(const Character* c) const;  // the entry holding c (the same pointer); -1
  // The next dancer in the tap cycle: scanning j = i + 1, i + 2, ... (mod size()), size()
  // entries in all, so i itself comes last, the first whose info().inCycle is true. An i
  // outside 0..size()-1 scans from entry 0. -1 when no entry is in the cycle.
  int next(int i) const;

 private:
  CastEntry entries_[kMaxCast] = {};
  int count_ = 0;
};

// Appends crab (Renderer::Crab), then classic (Renderer::Classic); true when both were
// added. DanceMode builds its cast with it, so the order lives here and is tested.
bool addBuiltIns(DanceCast& cast, const Character& crab, const Character& classic);
// The first move of c named `name` (byte for byte), in index order; -1 for none or null.
int findMove(const Character& c, const char* name);

}  // namespace dance
