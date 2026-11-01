// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DanceCast.h"

#include <cstring>

namespace dance {
namespace {

// An id as compared: a null one counts as "" (a Character's id is never null in
// practice; this keeps strcmp off a null pointer).
const char* idOf(const Character* c) {
  const char* id = c->info().id;
  return id ? id : "";
}

}  // namespace

bool DanceCast::add(const Character* c, Renderer r) {
  if (!c || count_ >= kMaxCast) return false;
  const char* id = idOf(c);
  for (int i = 0; i < count_; ++i)
    if (std::strcmp(idOf(entries_[i].character), id) == 0) return false;  // the first one wins
  entries_[count_].character = c;
  entries_[count_].renderer = r;
  ++count_;
  return true;
}

const CastEntry& DanceCast::at(int i) const {
  // Outside 0..size()-1 is outside the contract: entry 0's slot (empty, character
  // null, when the cast is) rather than a read past the table.
  return entries_[i >= 0 && i < count_ ? i : 0];
}

int DanceCast::find(const char* id) const {
  if (!id) return -1;
  for (int i = 0; i < count_; ++i)
    if (std::strcmp(idOf(entries_[i].character), id) == 0) return i;
  return -1;
}

int DanceCast::indexOf(const Character* c) const {
  if (!c) return -1;
  for (int i = 0; i < count_; ++i)
    if (entries_[i].character == c) return i;
  return -1;
}

int DanceCast::next(int i) const {
  const int n = count_;
  const int from = i >= 0 && i < n ? i + 1 : 0;
  for (int k = 0; k < n; ++k) {
    const int j = (from + k) % n;
    if (entries_[j].character->info().inCycle) return j;
  }
  return -1;
}

bool addBuiltIns(DanceCast& cast, const Character& crab, const Character& classic) {
  const bool a = cast.add(&crab, Renderer::Crab);
  const bool b = cast.add(&classic, Renderer::Classic);
  return a && b;
}

int findMove(const Character& c, const char* name) {
  if (!name) return -1;
  const int n = c.moveCount();
  for (int i = 0; i < n; ++i) {
    const char* m = c.move(i).name;
    if (m && std::strcmp(m, name) == 0) return i;
  }
  return -1;
}

}  // namespace dance
