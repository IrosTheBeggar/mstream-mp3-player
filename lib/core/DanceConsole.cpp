// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DanceConsole.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "DanceRules.h"
#include "DrawnCharacter.h"
#include "GenreFold.h"

// The replies are F.2's, byte for byte. Numbers are plain decimals, except a
// k (an octave) and a spot, which print as -1, 0 or +1 (`sgn` below).
namespace dance {
namespace {

#if defined(__GNUC__)
#define DANCE_CONSOLE_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define DANCE_CONSOLE_PRINTF(f, a)
#endif

constexpr uint32_t kPos16PerBeat = 786432;   // 12 ticks x 65536: a move beat in pos16
constexpr uint32_t kPos16PerEighth = 98304;  // an eighth of a move beat: a code character's step

// One reply line, cut to kConsoleLine - 1 bytes.
void say(ConsoleOut out, void* ctx, const char* fmt, ...) DANCE_CONSOLE_PRINTF(3, 4);
void say(ConsoleOut out, void* ctx, const char* fmt, ...) {
  if (!out) return;
  char line[kConsoleLine];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  out(ctx, line);
}

const char* txt(const char* s) { return s ? s : ""; }
const char* dash(const char* s) { return s && s[0] ? s : "-"; }  // M's empty strings
// The sign of a k or a spot: "+" before a positive value (printed with %s%d).
const char* sgn(int v) { return v > 0 ? "+" : ""; }

// The operand: the argument after its letter, with leading and trailing spaces (0x20)
// removed. Without trailing spaces (SerialConsole trims what it passes) it is a view of
// `rest` itself, so it is never cut; otherwise a copy in buf, cut to kConsoleLine - 1.
const char* operand(const char* rest, char* buf) {
  while (*rest == ' ') ++rest;
  size_t n = std::strlen(rest);
  if (n == 0 || rest[n - 1] != ' ') return rest;
  while (n > 0 && rest[n - 1] == ' ') --n;
  if (n > static_cast<size_t>(kConsoleLine) - 1) n = kConsoleLine - 1;
  std::memcpy(buf, rest, n);
  buf[n] = '\0';
  return buf;
}

const char* startName(StartAt a) {
  switch (a) {
    case StartAt::Any: return "any";
    case StartAt::Centre: return "centre";
    case StartAt::Side: return "side";
    case StartAt::Left: return "left";
    case StartAt::Right: return "right";
  }
  return "?";
}

const char* whenName(uint8_t when) {
  switch (when & (kWhenQuiet | kWhenListening)) {
    case kWhenQuiet: return "quiet";
    case kWhenListening: return "listening";
    case kWhenQuiet | kWhenListening: return "quiet+listening";
    default: return "-";
  }
}

const char* stateName(DanceState s) {
  switch (s) {
    case DanceState::Quiet: return "quiet";
    case DanceState::Listening: return "listening";
    case DanceState::Entering: return "entering";
    case DanceState::Dancing: return "dancing";
    case DanceState::Leaving: return "leaving";
    case DanceState::Freeze: return "freeze";
  }
  return "?";
}

// The flags' names joined by '+', in bit order; "-" for none. Other bits are left out.
void flagNames(uint8_t flags, char* out, size_t cap) {
  static constexpr struct {
    uint8_t bit;
    const char* name;
  } kNames[] = {{kMirror, "mirror"}, {kFreeze, "freeze"}, {kPlaceStart, "start"},
                {kPlaceEnd, "end"},  {kHeavy, "heavy"},   {kSnap, "snap"}};
  size_t n = 0;
  out[0] = '\0';
  for (const auto& f : kNames) {
    if (!(flags & f.bit)) continue;
    const int w = snprintf(out + n, cap - n, "%s%s", n ? "+" : "", f.name);
    if (w < 0 || static_cast<size_t>(w) >= cap - n) break;
    n += static_cast<size_t>(w);
  }
  if (n == 0) snprintf(out, cap, "-");
}

// A run's move as named in the drawn character: a move's name, or auto-bounce.
const char* runName(const Character* c, int move) {
  if (move == kAutoBounce) return "auto-bounce";
  if (c && move >= 0 && move < c->moveCount()) return txt(c->move(move).name);
  return "?";
}

// The DrawnCharacter behind c when the cast draws it as one (a Renderer::Drawn entry
// always holds a DrawnCharacter); null for a code character.
const DrawnCharacter* drawnOf(const DanceCast& cast, const Character& c) {
  const int e = cast.indexOf(&c);
  if (e < 0 || cast.at(e).renderer != Renderer::Drawn) return nullptr;
  return static_cast<const DrawnCharacter*>(&c);
}

// ---- the commands

void listCast(const DanceCast& cast, int chosen, ConsoleOut out, void* ctx) {
  if (cast.size() == 0) {
    say(out, ctx, "[dance] cast: empty");
    return;
  }
  for (int i = 0; i < cast.size(); ++i) {
    const Character& c = *cast.at(i).character;
    const CharacterInfo& info = c.info();
    char ignored[24] = "";
    if (info.ignoredMoves > 0) snprintf(ignored, sizeof(ignored), ", %u ignored", static_cast<unsigned>(info.ignoredMoves));
    say(out, ctx, "[dance] cast %d: %s \"%s\" by %s, %s, %s, %s, %d moves%s%s", i, dash(info.id), dash(info.name),
        dash(info.author), dash(info.licence), info.origin == Origin::BuiltIn ? "built-in" : "card",
        info.inCycle ? "in the cycle" : "not in the cycle", c.moveCount(), ignored, i == chosen ? " *" : "");
  }
}

void listMoves(const Choreographer& ch, const Character& c, ConsoleOut out, void* ctx) {
  const char* id = txt(c.info().id);
  char t[16] = "-";
  if (ch.t10() != 0) snprintf(t, sizeof(t), "%d.%d", ch.t10() / 10, ch.t10() % 10);
  char k[8] = "-";
  if (ch.prevK() != kNoOctave) snprintf(k, sizeof(k), "%s%d", sgn(ch.prevK()), ch.prevK());
  const int n = c.moveCount();
  say(out, ctx, "[dance] %s: %d moves, T %s, last ranged k %s", id, n, t, k);
  for (int i = 0; i < n; ++i) {
    const MoveLabels& m = c.move(i);
    char flags[48];
    flagNames(m.flags, flags, sizeof(flags));
    if (m.kind == MoveKind::Idle) {
      char length[16] = "continuous";
      if (m.idleMs != 0) snprintf(length, sizeof(length), "%u ms", static_cast<unsigned>(m.idleMs));
      say(out, ctx, "[dance] move %d %s: idle, %s, %s, weight %d.%d, flags %s, needs %u", i, txt(m.name), length,
          whenName(m.when), m.weight10 / 10, m.weight10 % 10, flags, static_cast<unsigned>(m.needs));
      continue;
    }
    char pace[24] = "any pace";
    if (m.paceLo != 0 || m.paceHi != 0)
      snprintf(pace, sizeof(pace), "pace %u-%u", static_cast<unsigned>(m.paceLo), static_cast<unsigned>(m.paceHi));
    char repeat[8] = "auto";
    if (m.repeat != 0) snprintf(repeat, sizeof(repeat), "%u", static_cast<unsigned>(m.repeat));
    char now[8] = "-";
    const int8_t e = ch.eligibleNow(i);
    if (e != kNoOctave) snprintf(now, sizeof(now), "k %s%d", sgn(e), e);
    say(out, ctx,
        "[dance] move %d %s: beat %u, %s, weight %d.%d, repeat %s, flags %s, from %u to %u, mirrored %u to %u, "
        "travel %d %s, %u genres, needs %u; now %s",
        i, txt(m.name), static_cast<unsigned>(m.beats), pace, m.weight10 / 10, m.weight10 % 10, repeat, flags,
        static_cast<unsigned>(m.fromTag), static_cast<unsigned>(m.toTag), static_cast<unsigned>(m.fromTagM),
        static_cast<unsigned>(m.toTagM), m.travel, startName(m.startAt), static_cast<unsigned>(m.genreCount),
        static_cast<unsigned>(m.needs), now);
  }
  const Cue& cue = ch.cue();
  const RunInfo& r = ch.run();
  if (r.move == kNoMove) {
    say(out, ctx, "[dance] now: %s, spot %s%d, run -", stateName(cue.state), sgn(cue.spot), cue.spot);
    return;
  }
  const char* status = r.status == RunStatus::Running ? " (running)" : (r.status == RunStatus::Held ? " (held)" : " (ended)");
  say(out, ctx, "[dance] now: %s, spot %s%d, run %s%s", stateName(cue.state), sgn(cue.spot), cue.spot,
      runName(&c, r.move), status);
}

void forceMove(Choreographer& ch, const Character& c, const char* op, ConsoleOut out, void* ctx) {
  if (!op[0]) {
    ch.forceNext(kNoMove);
    say(out, ctx, "[dance] next run: the picker's");
    return;
  }
  const int i = findMove(c, op);
  if (i < 0) {
    say(out, ctx, "[dance] Mm: %s has no move \"%s\" (Ml lists them)", txt(c.info().id), op);
    return;
  }
  const MoveLabels& m = c.move(i);
  if (m.kind != MoveKind::Beat) {
    say(out, ctx, "[dance] Mm: %s is an idle move: only beat moves can be forced", txt(m.name));
    return;
  }
  ch.forceNext(i);
  say(out, ctx, "[dance] next run: %s", txt(m.name));
}

// Mf<move>.<step>[m]: `op` is the operand, held in buf (writable) or a view of the
// argument; buf may receive the move's name.
void freezeMove(Choreographer& ch, const DanceCast& cast, const Character& c, const char* op, char* buf,
                ConsoleOut out, void* ctx) {
  if (!op[0]) {
    ch.freeze(kNoMove, 0, false);
    say(out, ctx, "[dance] Mf off");
    return;
  }
  // 1. The syntax: a name (at least a byte) before the last '.', 1-3 digits, then
  // nothing or one 'm'.
  const char* dot = std::strrchr(op, '.');
  const char* p = dot ? dot + 1 : nullptr;
  int digits = 0, step = 0;
  while (p && p[digits] >= '0' && p[digits] <= '9' && digits < 4) {
    step = step * 10 + (p[digits] - '0');
    ++digits;
  }
  const char* tail = p ? p + digits : nullptr;
  const bool mirrored = tail && tail[0] == 'm';
  if (!dot || dot == op || digits < 1 || digits > 3 || (mirrored ? tail[1] != '\0' : tail[0] != '\0')) {
    say(out, ctx, "[dance] Mf: Mf<move>.<step>, with m after the step for the mirrored form; Mf alone unfreezes");
    return;
  }
  size_t len = static_cast<size_t>(dot - op);
  if (op != buf) {
    if (len > static_cast<size_t>(kConsoleLine) - 1) len = kConsoleLine - 1;
    std::memcpy(buf, op, len);
  }
  buf[len] = '\0';
  const char* name = buf;
  // 2-5. The move, its steps, the step, the mirrored form.
  const int i = findMove(c, name);
  if (i < 0) {
    say(out, ctx, "[dance] Mf: %s has no move \"%s\" (Ml lists them)", txt(c.info().id), name);
    return;
  }
  const int n = stepCount(cast, c, i);
  if (n == 0) {
    say(out, ctx, "[dance] Mf: %s has no steps", name);
    return;
  }
  if (step >= n) {
    say(out, ctx, "[dance] Mf: %s has steps 0-%d", name, n - 1);
    return;
  }
  const MoveLabels& m = c.move(i);
  if (mirrored && !(m.flags & kMirror)) {
    say(out, ctx, "[dance] Mf: %s has no mirrored form", name);
    return;
  }
  // 6. The freeze.
  uint32_t pos16 = 0;
  if (!stepStart(cast, c, i, step, &pos16)) return;  // (in range: never)
  ch.freeze(i, pos16, mirrored);
  if (m.kind == MoveKind::Beat) {
    // pos16 / 786432 move beats to the nearest thousandth, half up.
    const uint64_t u = (static_cast<uint64_t>(pos16) * 1000 + kPos16PerBeat / 2) / kPos16PerBeat;
    say(out, ctx, "[dance] frozen: %s %s.%d%s at move beat %u.%03u", txt(c.info().id), name, step,
        mirrored ? "m" : "", static_cast<unsigned>(u / 1000), static_cast<unsigned>(u % 1000));
  } else {
    const uint64_t ms = (static_cast<uint64_t>(pos16) * 10) >> 16;  // E.2.7's idle freeze
    say(out, ctx, "[dance] frozen: %s %s.%d%s at %u ms", txt(c.info().id), name, step, mirrored ? "m" : "",
        static_cast<unsigned>(ms));
  }
}

void addGenre(Choreographer& ch, const char* op, ConsoleOut out, void* ctx) {
  uint32_t key = 0;
  if (!genreKey(op, &key)) {
    say(out, ctx, "[dance] Mg: \"%s\" folds to nothing, so no move can match it", op);
    return;
  }
  ch.addGenre(key);
  say(out, ctx, "[dance] genre \"%s\" for this track (key %08x)", op, static_cast<unsigned>(key));
}

void moveToSpot(Choreographer& ch, const Character& c, const char* op, ConsoleOut out, void* ctx) {
  if (!hasTravel(c)) {
    say(out, ctx, "[dance] Ms: %s has no travel moves: it dances at the centre", txt(c.info().id));
    return;
  }
  int s;
  if (std::strcmp(op, "-1") == 0) {
    s = -1;
  } else if (std::strcmp(op, "0") == 0) {
    s = 0;
  } else if (std::strcmp(op, "1") == 0 || std::strcmp(op, "+1") == 0) {
    s = 1;
  } else {
    say(out, ctx, "[dance] Ms: a spot is -1, 0 or +1");
    return;
  }
  ch.setSpot(s);
  say(out, ctx, "[dance] spot %s%d", sgn(s), s);
}

}  // namespace

int stepCount(const DanceCast& cast, const Character& c, int move) {
  if (move < 0 || move >= c.moveCount()) return 0;
  if (const DrawnCharacter* d = drawnOf(cast, c)) {
    const int m = d->packMove(move);
    return m < 0 ? 0 : d->pack().moveRecord(m).stepCount;
  }
  const MoveLabels& l = c.move(move);
  return l.kind == MoveKind::Beat ? 8 * l.beats : 0;  // eighths of a move beat; a continuous idle has none
}

bool stepStart(const DanceCast& cast, const Character& c, int move, int step, uint32_t* pos16) {
  if (step < 0 || step >= stepCount(cast, c, move)) return false;
  uint64_t p;
  if (const DrawnCharacter* d = drawnOf(cast, c)) {
    const int m = d->packMove(move);
    uint64_t sum = 0;
    for (int k = 0; k < step; ++k) sum += d->pack().step(m, k).dur;
    p = sum * 65536;  // ticks (beat) or 10 ms units (idle), in Q16
  } else {
    p = static_cast<uint64_t>(step) * kPos16PerEighth;
  }
  if (p > 0xFFFFFFFFu) return false;  // (a kept move's never is)
  if (pos16) *pos16 = static_cast<uint32_t>(p);
  return true;
}

ConsoleReply DanceConsole::run(const char* arg, Choreographer& ch, const DanceCast& cast, int chosen,
                               ConsoleOut out, void* ctx) {
  const ConsoleReply none = {false, 0};
  if (!arg) arg = "";
  if (arg[0] == '\0') {
    listCast(cast, chosen, out, ctx);
    return none;
  }
  char buf[kConsoleLine];
  const char* op = operand(arg + 1, buf);
  const Character* c = ch.character();
  const char letter = arg[0];
  const bool takesNone = letter == 'l' || letter == 'v';
  const bool onDancer = letter == 'l' || letter == 'm' || letter == 'f' || letter == 's';
  const bool known = takesNone ? op[0] == '\0' : (onDancer || letter == 'c' || letter == 'g');
  if (!known) {
    say(out, ctx, "[dance] M: unknown \"%s\": M, Mc<id>, Ml, Mm<move>, Mf<move>.<step>[m], Mf, Mv, Mg<genre>, Ms<spot>",
        arg);
    return none;
  }
  if (onDancer && !c) {
    say(out, ctx, "[dance] M: no dancer");
    return none;
  }
  switch (letter) {
    case 'c': {
      const int i = cast.find(op);
      if (i >= 0) return {true, i};
      say(out, ctx, "[dance] Mc: no dancer \"%s\" (M lists them)", op);
      return none;
    }
    case 'l': listMoves(ch, *c, out, ctx); break;
    case 'm': forceMove(ch, *c, op, out, ctx); break;
    case 'f': freezeMove(ch, cast, *c, op, buf, out, ctx); break;
    case 'v':
      verbose_ = !verbose_;
      if (verbose_) printed_ = false;
      say(out, ctx, "[dance] move log %s", verbose_ ? "on" : "off");
      break;
    case 'g': addGenre(ch, op, out, ctx); break;
    case 's': moveToSpot(ch, *c, op, out, ctx); break;
    default: break;
  }
  return none;
}

void DanceConsole::frame(const Choreographer& ch, ConsoleOut out, void* ctx) {
  if (!verbose_) return;
  const RunInfo& r = ch.run();
  const Character* c = ch.character();
  if (r.status == RunStatus::None || !c) return;
  if (printed_ && r.move == move_ && r.start == start_ && r.k == k_ && r.plays == plays_ &&
      r.firstMirror == mirror_ && r.level == level_)
    return;
  say(out, ctx, "[move] %s %s k=%s%d plays=%u mirror=%d spot=%s%d p=%d level=%u at=%lld", txt(c->info().id),
      runName(c, r.move), sgn(r.k), r.k, static_cast<unsigned>(r.plays), r.firstMirror ? 1 : 0, sgn(r.spot), r.spot,
      r.phrase, static_cast<unsigned>(r.level), static_cast<long long>(r.start));
  printed_ = true;
  move_ = r.move;
  start_ = r.start;
  k_ = r.k;
  plays_ = r.plays;
  mirror_ = r.firstMirror;
  level_ = r.level;
}

}  // namespace dance
