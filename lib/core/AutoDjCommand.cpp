// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjCommand.h"

#include <cstddef>
#include <cstring>

#include "NvsLayout.h"

namespace autodj {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The decimal number in [s, s + n): false when it is empty, isn't digits
// only, or passes `max`.
bool decimal(const char* s, size_t n, uint32_t max, uint32_t* out) {
  if (n == 0 || n > 9) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + static_cast<uint32_t>(s[i] - '0');
  }
  if (v > max) return false;
  *out = v;
  return true;
}

// 1-8 hex digits, either case.
bool hex(const char* s, size_t n, uint32_t* out) {
  if (n == 0 || n > 8) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    v = v << 4 | d;
  }
  *out = v;
  return true;
}

// "0" or "1".
bool bit(const char* s, size_t n, uint32_t* out) {
  if (n != 1 || (s[0] != '0' && s[0] != '1')) return false;
  *out = static_cast<uint32_t>(s[0] - '0');
  return true;
}

}  // namespace

Command parseCommand(const char* arg) {
  using K = Command::Kind;
  Command c;
  if (!arg) arg = "";
  while (isSpace(*arg)) ++arg;
  size_t n = std::strlen(arg);
  while (n && isSpace(arg[n - 1])) --n;
  if (n == 0) {
    c.kind = K::Status;
    return c;
  }
  const char head = arg[0];
  const char* rest = arg + 1;
  const size_t rn = n - 1;
  uint32_t v = 0;
  switch (head) {
    case '0':
    case '1':
      if (rn == 0) c.kind = head == '1' ? K::On : K::Off;
      break;
    case 'm':
      if (bit(rest, rn, &v)) {
        c.kind = K::Mode;
        c.n = v;
      }
      break;
    case 'f':
      if (decimal(rest, rn, 255, &v) && nvslayout::autoDjFloorValid(static_cast<uint8_t>(v))) {
        c.kind = K::Floor;
        c.n = v;
      }
      break;
    case 't':
    case 'k':
      if (bit(rest, rn, &v)) {
        c.kind = head == 't' ? K::Tempo : K::Key;
        c.n = v;
      }
      break;
    case 's':
      if (hex(rest, rn, &v)) {
        c.kind = K::Seed;
        c.n = v;
      }
      break;
    case 'n':
      if (decimal(rest, rn, Command::kMaxDryRun, &v) && v > 0) {
        c.kind = K::DryRun;
        c.n = v;
      }
      break;
    case 'b':
      if (decimal(rest, rn, Command::kMaxBench, &v) && v > 0) {
        c.kind = K::Bench;
        c.n = v;
      }
      break;
    case 'h':
      if (rn == 0) {
        c.kind = K::FromHere;
        c.n = Command::kCurrent;
      } else if (decimal(rest, rn, 0xFFFFFFFEu, &v)) {
        c.kind = K::FromHere;
        c.n = v;
      }
      break;
    case 'r':
    case 'p':
    case 'j':
    case 'c':
    case 'e':
    case 'x':
    case '?':
      if (rn == 0) {
        c.kind = head == 'r'   ? K::Renew
                 : head == 'p' ? K::Pick
                 : head == 'j' ? K::Join
                 : head == 'c' ? K::Reopen
                 : head == 'e' ? K::Surprise
                 : head == 'x' ? K::Stop
                               : K::Help;
      }
      break;
    default:
      break;
  }
  return c;
}

const char* commandHelp() {
  return "D status, D0/D1 off/on, Dm0/Dm1 Follow the vibe/Stay on seed, Df55..Df75 strictness, Dt0/Dt1 BPM "
         "continuity, Dk0/Dk1 Harmonic mixing, Ds<hex> the next session's seed, Dr a new session, Dp a pick now, "
         "Dn<k> k dry-run picks on a copy of the session (1-2000), Dj the join again, Dc the file reopened, "
         "Db<n> n slot reads timed (1-5000), Dh / Dh<id> Auto DJ from the current track / library track id, "
         "De the Auto DJ button, Dx stop Dn/Db, D? this";
}

}  // namespace autodj
