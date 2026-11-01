// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the M console (DanceConsole: its parser, its replies, the
// [move] lines, stepCount and stepStart): docs/DANCE-SPRITES.md, section 3.9
// and Appendix F (F.1's DanceConsole.h, F.2 line for line, F.4's
// test_dance_console list, item by item). Written from the doc alone,
// without reading the implementation.
//
// How it works:
// - The console talks to a real Choreographer, the built-in cast
//   (addBuiltIns: the crab and the classic stick) and, for the drawn paths,
//   the fixture (C.10) loaded as a card pack through BeatScript.h's
//   CardDancer and appended as cast entry 2 with Renderer::Drawn.
// - `out` records the lines; every line handed to it is checked to be at
//   most 255 bytes, with no newline (F.1: kConsoleLine).
// - F.2's worked examples are checked as literal strings. Beyond them, M's
//   and Ml's lines are compared with a reference formatter written here from
//   F.2's rules, so every move of every character is checked, and the
//   [move] lines with one built from run() by F.2's format.
// - Inputs come from beatscript::Script (test/support/dance/BeatScript.h).
//
// Run: pio test -e native -f test_dance_console (and native_asan, F.4).
#if defined(DANCE_ASAN) && !defined(__SANITIZE_ADDRESS__)
#error "native_asan: -fsanitize=address didn't reach the compiler (docs/DANCE-SPRITES.md, Appendix D.5)"
#endif

#include <unity.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../support/dance/BeatScript.h"
#include "CodeCharacters.h"
#include "CrabPose.h"
#include "DanceCast.h"
#include "DanceCharacter.h"
#include "DanceChoreo.h"
#include "DanceConsole.h"
#include "DancePose.h"
#include "DanceRules.h"
#include "GenreFold.h"

using beatscript::CardDancer;
using beatscript::Script;
using beatscript::ScriptCharacter;
using dance::Character;
using dance::Choreographer;
using dance::ConsoleReply;
using dance::Cue;
using dance::DanceCast;
using dance::DanceConsole;
using dance::DanceState;
using dance::kAutoBounce;
using dance::kNoMove;
using dance::kNoOctave;
using dance::MoveLabels;
using dance::Renderer;
using dance::RunInfo;
using dance::RunStatus;

namespace {

// --- messages ------------------------------------------------------------

char g_ctx[256] = "";
void ctx(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_ctx, sizeof g_ctx, fmt, ap);
  va_end(ap);
}

const char* msg(const char* fmt, ...) {
  static char buf[1400];
  int n = std::snprintf(buf, sizeof buf, "%s: ", g_ctx);
  if (n < 0) n = 0;
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf + n, sizeof buf - static_cast<size_t>(n), fmt, ap);
  va_end(ap);
  return buf;
}

#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) TEST_FAIL_MESSAGE(msg(__VA_ARGS__)); \
  } while (0)

std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

// --- the recorder ------------------------------------------------------------

struct Out {
  std::vector<std::string> lines;
  long total = 0;
  static void put(void* ctx, const char* line) {
    TEST_ASSERT_NOT_NULL_MESSAGE(ctx, "out's ctx is the one handed to the console");
    TEST_ASSERT_NOT_NULL_MESSAGE(line, "a null line");
    const size_t n = std::strlen(line);
    if (n > 255) TEST_FAIL_MESSAGE(msg("a line of %u bytes (at most 255): %.80s...", static_cast<unsigned>(n), line));
    if (std::strchr(line, '\n') || std::strchr(line, '\r'))
      TEST_FAIL_MESSAGE(msg("a line with a newline in it: %s", line));
    Out* o = static_cast<Out*>(ctx);
    o->lines.emplace_back(line);
    ++o->total;
  }
  void clear() { lines.clear(); }
};

std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& l : v) {
    s += "\n  ";
    s += l;
  }
  return s.empty() ? std::string(" (none)") : s;
}

void expectLines(const Out& out, const std::vector<std::string>& want, const char* what) {
  if (out.lines == want) return;
  TEST_FAIL_MESSAGE(msg("%s: got%s\nwant%s", what, joined(out.lines).c_str(), joined(want).c_str()));
}

// --- the characters ----------------------------------------------------------

const dance::CrabCharacter& theCrab() {
  static const dance::CrabCharacter c{};
  return c;
}
const dance::ClassicStick& theStick() {
  static const dance::ClassicStick c{};
  return c;
}

// The fixture as a card pack, kept for the whole run.
CardDancer& fixture() {
  static CardDancer* d = nullptr;
  if (!d) {
    d = new CardDancer();
    TEST_ASSERT_TRUE_MESSAGE(d->load(packedit::testPack()), "the fixture loads as a card pack");
  }
  return *d;
}
const Character& fx() { return fixture().character(); }
// The fixture's moves by name, as the view numbers them.
int fxMove(const char* name) { return dance::findMove(fx(), name); }

void builtIns(DanceCast& cast) { TEST_ASSERT_TRUE(dance::addBuiltIns(cast, theCrab(), theStick())); }
void withFixture(DanceCast& cast) {
  builtIns(cast);
  TEST_ASSERT_TRUE(cast.add(&fx(), Renderer::Drawn));
}

// A test-local character with chosen strings, cycle membership and ignored
// moves, and n moves.
class InfoChar final : public Character {
 public:
  InfoChar(const char* id, const char* name, const char* author, const char* licence, bool inCycle,
           uint8_t ignored, dance::Origin origin = dance::Origin::Card) {
    info_.id = id;
    info_.name = name;
    info_.author = author;
    info_.licence = licence;
    info_.url = "";
    info_.origin = origin;
    info_.inCycle = inCycle;
    info_.ignoredMoves = ignored;
  }
  InfoChar(const InfoChar&) = delete;
  InfoChar& operator=(const InfoChar&) = delete;
  int add(const MoveLabels& m) {
    if (count_ >= 64) return -1;
    moves_[count_] = m;
    return count_++;
  }
  const dance::CharacterInfo& info() const override { return info_; }
  int moveCount() const override { return count_; }
  const MoveLabels& move(int i) const override { return moves_[i]; }

 private:
  dance::CharacterInfo info_{};
  MoveLabels moves_[64]{};
  int count_ = 0;
};

// --- F.2's rules, written again -------------------------------------------------

// A k or a spot: -1, 0 or +1.
std::string sgn(int v) { return v > 0 ? fmt("+%d", v) : fmt("%d", v); }
std::string kOrDash(int8_t k) { return k == kNoOctave ? std::string("-") : sgn(k); }
const char* dashIfEmpty(const char* s) { return (s && *s) ? s : "-"; }

std::string flagNames(uint8_t f) {
  static const struct {
    uint8_t bit;
    const char* name;
  } kNames[] = {{dance::kMirror, "mirror"}, {dance::kFreeze, "freeze"}, {dance::kPlaceStart, "start"},
                {dance::kPlaceEnd, "end"},  {dance::kHeavy, "heavy"},   {dance::kSnap, "snap"}};
  std::string s;
  for (const auto& n : kNames) {
    if (!(f & n.bit)) continue;
    if (!s.empty()) s += "+";
    s += n.name;
  }
  return s.empty() ? std::string("-") : s;
}

const char* startName(dance::StartAt a) {
  switch (a) {
    case dance::StartAt::Any: return "any";
    case dance::StartAt::Centre: return "centre";
    case dance::StartAt::Side: return "side";
    case dance::StartAt::Left: return "left";
    case dance::StartAt::Right: return "right";
  }
  return "?";
}

const char* whenName(uint8_t w) {
  switch (w) {
    case 1: return "quiet";
    case 2: return "listening";
    case 3: return "quiet+listening";
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

std::string tenths(int v) { return fmt("%d.%d", v / 10, v % 10); }

// M's line for cast entry i.
std::string refCastLine(const DanceCast& cast, int i, int chosen) {
  const dance::CharacterInfo& in = cast.at(i).character->info();
  std::string s = fmt("[dance] cast %d: %s \"%s\" by %s, %s, %s, %s, %d moves", i, dashIfEmpty(in.id),
                      dashIfEmpty(in.name), dashIfEmpty(in.author), dashIfEmpty(in.licence),
                      in.origin == dance::Origin::BuiltIn ? "built-in" : "card",
                      in.inCycle ? "in the cycle" : "not in the cycle", cast.at(i).character->moveCount());
  if (in.ignoredMoves > 0) s += fmt(", %d ignored", in.ignoredMoves);
  if (i == chosen) s += " *";
  return s;
}
std::vector<std::string> refCast(const DanceCast& cast, int chosen) {
  std::vector<std::string> v;
  if (cast.size() == 0) v.push_back("[dance] cast: empty");
  for (int i = 0; i < cast.size(); ++i) v.push_back(refCastLine(cast, i, chosen));
  return v;
}

std::string refMoveLine(const Choreographer& ch, int i) {
  const MoveLabels& m = ch.character()->move(i);
  if (m.kind == dance::MoveKind::Idle) {
    const std::string len = m.idleMs == 0 ? std::string("continuous") : fmt("%u ms", m.idleMs);
    return fmt("[dance] move %d %s: idle, %s, %s, weight %s, flags %s, needs %u", i, m.name, len.c_str(),
               whenName(m.when), tenths(m.weight10).c_str(), flagNames(m.flags).c_str(), m.needs);
  }
  const std::string pace = (m.paceLo == 0 && m.paceHi == 0) ? std::string("any pace")
                                                            : fmt("pace %u-%u", m.paceLo, m.paceHi);
  const std::string rep = m.repeat == 0 ? std::string("auto") : fmt("%u", m.repeat);
  const int8_t k = ch.eligibleNow(i);
  const std::string now = k == kNoOctave ? std::string("-") : "k " + sgn(k);
  return fmt(
      "[dance] move %d %s: beat %u, %s, weight %s, repeat %s, flags %s, from %u to %u, mirrored %u to %u, "
      "travel %d %s, %u genres, needs %u; now %s",
      i, m.name, m.beats, pace.c_str(), tenths(m.weight10).c_str(), rep.c_str(), flagNames(m.flags).c_str(),
      m.fromTag, m.toTag, m.fromTagM, m.toTagM, m.travel, startName(m.startAt), m.genreCount, m.needs, now.c_str());
}

std::string refRunName(const Choreographer& ch) {
  const RunInfo& r = ch.run();
  if (r.move == kNoMove) return "-";
  const std::string name = r.move == kAutoBounce ? std::string("auto-bounce") : ch.character()->move(r.move).name;
  const char* st = r.status == RunStatus::Running ? " (running)" : (r.status == RunStatus::Held ? " (held)" : " (ended)");
  return name + st;
}

std::vector<std::string> refDl(const Choreographer& ch) {
  const Character& c = *ch.character();
  std::vector<std::string> v;
  const std::string T = ch.t10() == 0 ? std::string("-") : tenths(ch.t10());
  v.push_back(fmt("[dance] %s: %d moves, T %s, last ranged k %s", c.info().id, c.moveCount(), T.c_str(),
                  kOrDash(ch.prevK()).c_str()));
  for (int i = 0; i < c.moveCount(); ++i) v.push_back(refMoveLine(ch, i));
  v.push_back(fmt("[dance] now: %s, spot %s, run %s", stateName(ch.cue().state), sgn(ch.cue().spot).c_str(),
                  refRunName(ch).c_str()));
  return v;
}

// F.2's [move] line for run().
std::string refMoveLog(const Choreographer& ch) {
  const RunInfo& r = ch.run();
  const std::string name = r.move == kAutoBounce ? std::string("auto-bounce") : ch.character()->move(r.move).name;
  return fmt("[move] %s %s k=%s plays=%u mirror=%d spot=%s p=%d level=%u at=%lld", ch.character()->info().id,
             name.c_str(), sgn(r.k).c_str(), r.plays, r.firstMirror ? 1 : 0, sgn(r.spot).c_str(), r.phrase, r.level,
             static_cast<long long>(r.start));
}

// --- running the console ---------------------------------------------------------

struct Rig {
  DanceConsole con;
  Choreographer ch;
  DanceCast cast;
  Out out;
  int chosen = 0;
  ConsoleReply run(const char* arg) {
    out.clear();
    return con.run(arg, ch, cast, chosen, Out::put, &out);
  }
  // run() expecting exactly these lines and the reply {false, 0}.
  void expect(const char* arg, const std::vector<std::string>& want) {
    ctx("M%s", arg ? arg : "(null)");
    const ConsoleReply r = run(arg);
    expectLines(out, want, "lines");
    CHECK(!r.choose && r.index == 0, "reply {%d, %d}, want {false, 0}", r.choose, r.index);
  }
  void expect1(const char* arg, const std::string& want) { expect(arg, std::vector<std::string>{want}); }
};

// Updates for `secs` seconds of a Script.
void advance(Choreographer& ch, Script& s, double secs) {
  const long n = std::lround(secs / static_cast<double>(s.dt()));
  for (long i = 0; i < n; ++i) ch.update(s.frame());
}

const char* const kUsageDf =
    "[dance] Mf: Mf<move>.<step>, with m after the step for the mirrored form; Mf alone unfreezes";
const char* const kSpotUsage = "[dance] Ms: a spot is -1, 0 or +1";
std::string unknownLine(const std::string& arg) {
  return "[dance] M: unknown \"" + arg +
         "\": M, Mc<id>, Ml, Mm<move>, Mf<move>.<step>[m], Mf, Mv, Mg<genre>, Ms<spot>";
}

// The no-dancer reply and the spot and freeze helpers.
const char* const kNoDancer = "[dance] M: no dancer";

// Crab::frozen at k<n>, field for field.
bool crabEq(const crab::Pose& a, const crab::Pose& b) {
  return a.dx == b.dx && a.dy == b.dy && a.bob == b.bob && a.body == b.body && a.eyes == b.eyes &&
         a.legs == b.legs && a.eyesFlip == b.eyesFlip && a.eyeX == b.eyeX && a.eyeY == b.eyeY &&
         a.liftL == b.liftL && a.liftR == b.liftR && a.snapL == b.snapL && a.snapR == b.snapR && a.fx == b.fx;
}
bool stickEq(const dance::Pose& a, const dance::Pose& b) {
  auto eq = [](const dance::Point& p, const dance::Point& q) { return p.x == q.x && p.y == q.y; };
  return eq(a.head, b.head) && a.headR == b.headR && eq(a.neck, b.neck) && eq(a.hip, b.hip) &&
         eq(a.shoulderL, b.shoulderL) && eq(a.elbowL, b.elbowL) && eq(a.handL, b.handL) &&
         eq(a.shoulderR, b.shoulderR) && eq(a.elbowR, b.elbowR) && eq(a.handR, b.handR) && eq(a.kneeL, b.kneeL) &&
         eq(a.footL, b.footL) && eq(a.kneeR, b.kneeR) && eq(a.footR, b.footR);
}

const std::vector<std::string> kCrabDl = {
    "[dance] crab: 2 moves, T -, last ranged k -",
    "[dance] move 0 shuffle: beat 2, any pace, weight 1.0, repeat auto, flags freeze, from 1 to 1, mirrored 1 to 1, "
    "travel 0 any, 0 genres, needs 0; now k 0",
    "[dance] move 1 idle: idle, continuous, quiet+listening, weight 1.0, flags -, needs 0",
    "[dance] now: quiet, spot 0, run -",
};
const char* const kCast0 =
    "[dance] cast 0: crab \"Crab\" by IrosTheBeggar, GPL-3.0-or-later, built-in, in the cycle, 2 moves";
const char* const kCast1 =
    "[dance] cast 1: stick \"Stick\" by IrosTheBeggar, GPL-3.0-or-later, built-in, in the cycle, 2 moves";
const char* const kCast2 =
    "[dance] cast 2: test \"T\303\253st\" by IrosTheBeggar, GPL-3.0-or-later, card, in the cycle, 13 moves, 1 "
    "ignored";

}  // namespace

void setUp() {}
void tearDown() {}

// 1. M: the cast, one line per entry, the chosen one marked.
void test_cast_listing() {
  Rig r;
  builtIns(r.cast);
  r.ch.setCharacter(&theCrab());
  r.chosen = 0;
  r.expect("", {std::string(kCast0) + " *", kCast1});
  r.chosen = 1;
  r.expect("", {kCast0, std::string(kCast1) + " *"});
  // A null arg is M itself.
  r.expect(nullptr, {kCast0, std::string(kCast1) + " *"});
  // No entry chosen (out of range): no mark.
  r.chosen = 5;
  r.expect("", {kCast0, kCast1});

  // The fixture as entry 2, chosen and drawn.
  Rig f;
  withFixture(f.cast);
  f.ch.setCharacter(&fx());
  f.chosen = 2;
  f.expect("", {kCast0, kCast1, std::string(kCast2) + " *"});
  TEST_ASSERT_EQUAL_STRING((std::string(kCast2) + " *").c_str(), refCastLine(f.cast, 2, 2).c_str());

  // The empty cast.
  Rig e;
  e.ch.setCharacter(&theCrab());
  e.expect1("", "[dance] cast: empty");

  // Empty strings print as '-', an entry out of the cycle says so, and the
  // ignored count shows only when above 0.
  Rig g;
  InfoChar blank("blank", "", "", "", false, 0, dance::Origin::BuiltIn);
  InfoChar some("some", "Some", "Ann", "CC0-1.0", true, 7);
  TEST_ASSERT_TRUE(g.cast.add(&blank, Renderer::Classic));
  TEST_ASSERT_TRUE(g.cast.add(&some, Renderer::Drawn));
  g.ch.setCharacter(&some);
  g.chosen = 1;
  g.expect("", {"[dance] cast 0: blank \"-\" by -, -, built-in, not in the cycle, 0 moves",
                "[dance] cast 1: some \"Some\" by Ann, CC0-1.0, card, in the cycle, 0 moves, 7 ignored *"});
}

// 2. Mc: choose by id, any entry; no line when found.
void test_choose() {
  Rig r;
  builtIns(r.cast);
  InfoChar out("out", "Out", "Ann", "CC0-1.0", false, 0);
  TEST_ASSERT_TRUE(r.cast.add(&out, Renderer::Drawn));
  r.ch.setCharacter(&theCrab());
  struct Case {
    const char* arg;
    int index;
  } found[] = {{"ccrab", 0}, {"cstick", 1}, {"c stick ", 1}, {"c  crab", 0}, {"cout", 2}};
  for (const Case& c : found) {
    ctx("M%s", c.arg);
    const ConsoleReply rep = r.run(c.arg);
    CHECK(rep.choose && rep.index == c.index, "reply {%d, %d}, want {true, %d}", rep.choose, rep.index, c.index);
    expectLines(r.out, {}, "no line when found");
  }
  r.expect1("cnope", "[dance] Mc: no dancer \"nope\" (M lists them)");
  r.expect1("c", "[dance] Mc: no dancer \"\" (M lists them)");
  r.expect1("cCrab", "[dance] Mc: no dancer \"Crab\" (M lists them)");
  r.expect1("c   ", "[dance] Mc: no dancer \"\" (M lists them)");
  // Mc doesn't touch the Choreographer: DanceMode chooses.
  ctx("Mc leaves the Choreographer alone");
  r.run("cstick");
  CHECK(r.ch.character() == &theCrab(), "character() changed by Mc itself");
}

// 3. Ml: the drawn character's moves, before any update and while dancing.
void test_moves_listing() {
  {
    Rig r;
    builtIns(r.cast);
    r.ch.setCharacter(&theCrab());
    r.expect("l", kCrabDl);
    ctx("Ml crab, reference");
    expectLines(r.out, refDl(r.ch), "the reference formatter");
  }
  {
    Rig r;
    builtIns(r.cast);
    r.ch.setCharacter(&theStick());
    r.chosen = 1;
    r.expect("l", {"[dance] stick: 2 moves, T -, last ranged k -",
                   "[dance] move 0 bounce: beat 2, any pace, weight 1.0, repeat auto, flags freeze, from 1 to 1, "
                   "mirrored 1 to 1, travel 0 any, 0 genres, needs 0; now k 0",
                   "[dance] move 1 sway: idle, continuous, quiet+listening, weight 1.0, flags -, needs 0",
                   "[dance] now: quiet, spot 0, run -"});
  }
  {  // 6 s locked at 120 BPM
    Rig r;
    builtIns(r.cast);
    r.ch.setCharacter(&theCrab());
    Script s(30);
    s.acquire(120.0, 0.05);
    advance(r.ch, s, 6.0);
    r.run("l");
    ctx("Ml crab after 6 s at 120 BPM");
    expectLines(r.out, refDl(r.ch), "the reference formatter");
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(r.out.lines.size()));
    TEST_ASSERT_EQUAL_STRING("[dance] crab: 2 moves, T 120.0, last ranged k -", r.out.lines[0].c_str());
    CHECK(r.out.lines[1].size() > 9 && r.out.lines[1].compare(r.out.lines[1].size() - 9, 9, "; now k 0") == 0,
          "shuffle's now: %s", r.out.lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING("[dance] now: dancing, spot 0, run shuffle (running)", r.out.lines[3].c_str());
    // Unlocked: Leaving with the run held or ended, then listening.
    s.locked(false);
    for (int i = 0; i < 150; ++i) {
      r.ch.update(s.frame());
      if (i % 5 == 0) {
        r.run("l");
        ctx("Ml crab, unlocked, frame %d", i);
        expectLines(r.out, refDl(r.ch), "the reference formatter");
      }
    }
    TEST_ASSERT_EQUAL_STRING("[dance] now: listening, spot 0, run shuffle (ended)", r.out.lines.back().c_str());
  }
  {  // the fixture, no tempo yet
    Rig r;
    withFixture(r.cast);
    r.ch.setCharacter(&fx());
    r.chosen = 2;
    r.run("l");
    ctx("Ml fixture, no tempo");
    expectLines(r.out, refDl(r.ch), "the reference formatter");
    TEST_ASSERT_EQUAL_INT(15, static_cast<int>(r.out.lines.size()));
    TEST_ASSERT_EQUAL_STRING("[dance] test: 13 moves, T -, last ranged k -", r.out.lines[0].c_str());
    TEST_ASSERT_EQUAL_INT(0, fxMove("bounce"));
    TEST_ASSERT_EQUAL_INT(3, fxMove("point"));
    TEST_ASSERT_EQUAL_INT(6, fxMove("walk"));
    TEST_ASSERT_EQUAL_INT(12, fxMove("look"));
    TEST_ASSERT_EQUAL_STRING(
        "[dance] move 0 bounce: beat 2, any pace, weight 1.0, repeat auto, flags freeze, from 0 to 0, mirrored 0 "
        "to 0, travel 0 any, 0 genres, needs 0; now k 0",
        r.out.lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING(
        "[dance] move 3 point: beat 4, pace 90-130, weight 0.3, repeat 2, flags mirror+start, from 1 to 2, "
        "mirrored 1 to 3, travel 0 any, 0 genres, needs 0; now -",
        r.out.lines[4].c_str());
    TEST_ASSERT_EQUAL_STRING(
        "[dance] move 6 walk: beat 4, pace 90-140, weight 1.0, repeat auto, flags mirror, from 0 to 0, mirrored 0 "
        "to 0, travel 1 side, 0 genres, needs 0; now -",
        r.out.lines[7].c_str());
    TEST_ASSERT_EQUAL_STRING("[dance] move 12 look: idle, 3000 ms, listening, weight 1.0, flags mirror, needs 0",
                             r.out.lines[13].c_str());
    TEST_ASSERT_EQUAL_STRING("[dance] now: quiet, spot 0, run -", r.out.lines[14].c_str());

    // Locked at 120 BPM: every beat move's now is eligibleNow formatted.
    Script s(30);
    s.acquire(120.0, 0.05);
    for (int i = 0; i < 30 * 12; ++i) {
      r.ch.update(s.frame());
      if (i % 30 == 29) {
        r.run("l");
        ctx("Ml fixture at 120 BPM, frame %d", i);
        expectLines(r.out, refDl(r.ch), "the reference formatter");
      }
    }
    ctx("Ml fixture at 120 BPM");
    TEST_ASSERT_EQUAL_STRING("[dance] test: 13 moves, T 120.0, last ranged k ", r.out.lines[0].substr(0, 47).c_str());
    auto endsWith = [](const std::string& s, const char* tail) {
      const size_t n = std::strlen(tail);
      return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
    };
    CHECK(endsWith(r.out.lines[1 + fxMove("point")], "; now k 0"), "point: %s",
          r.out.lines[1 + fxMove("point")].c_str());
    CHECK(endsWith(r.out.lines[1 + fxMove("slow")], "; now k -1"), "slow: %s", r.out.lines[1 + fxMove("slow")].c_str());
    CHECK(endsWith(r.out.lines[1 + fxMove("fast")], "; now -"), "fast: %s", r.out.lines[1 + fxMove("fast")].c_str());
  }
}

// 4. Mm: force the next run, or cancel; idle and unknown moves refused.
void test_force() {
  Rig r;
  builtIns(r.cast);
  r.ch.setCharacter(&theCrab());
  Script s(30);
  s.acquire(120.0, 0.05);
  advance(r.ch, s, 4.0);
  // The next run after a command: the first frame whose run starts anew.
  auto nextRun = [&]() -> RunInfo {
    const int64_t start = r.ch.run().start;
    const RunStatus st = r.ch.run().status;
    for (int i = 0; i < 30 * 6; ++i) {
      r.ch.update(s.frame());
      if (r.ch.run().status == RunStatus::Running && (r.ch.run().start != start || st != RunStatus::Running))
        return r.ch.run();
    }
    TEST_FAIL_MESSAGE(msg("no new run within 6 s"));
    return r.ch.run();
  };
  r.expect1("mshuffle", "[dance] next run: shuffle");
  RunInfo n = nextRun();
  ctx("after Mmshuffle");
  CHECK(n.level == 4 && n.move == 0, "the next run: move %d, level %u, want 0 at level 4", n.move, n.level);
  n = nextRun();
  CHECK(n.level != 4, "the run after it: level %u (forced once only)", n.level);

  r.expect1("midle", "[dance] Mm: idle is an idle move: only beat moves can be forced");
  r.expect1("mnope", "[dance] Mm: crab has no move \"nope\" (Ml lists them)");
  r.expect1("mShuffle", "[dance] Mm: crab has no move \"Shuffle\" (Ml lists them)");
  n = nextRun();
  ctx("after the refused Mms");
  CHECK(n.level != 4, "a refused Mm forces nothing (level %u)", n.level);

  r.expect1("mshuffle", "[dance] next run: shuffle");
  r.expect1("m", "[dance] next run: the picker's");
  n = nextRun();
  ctx("after Mmshuffle, Mm");
  CHECK(n.level != 4, "Mm alone cancels: level %u", n.level);
  r.expect1("m shuffle ", "[dance] next run: shuffle");
  n = nextRun();
  ctx("after Mm shuffle (spaces)");
  CHECK(n.level == 4, "the operand is trimmed: level %u", n.level);

  // The stick's bounce.
  Rig st;
  builtIns(st.cast);
  st.ch.setCharacter(&theStick());
  st.expect1("mbounce", "[dance] next run: bounce");
  st.expect1("msway", "[dance] Mm: sway is an idle move: only beat moves can be forced");
  st.expect1("mshuffle", "[dance] Mm: stick has no move \"shuffle\" (Ml lists them)");
}

// 5. Mf on the crab: eighths of a move beat as steps, so Mfshuffle.<n> is
// k<n>'s pose; the syntax, the checks in order, and Mf alone.
void test_freeze_crab() {
  Rig r;
  builtIns(r.cast);
  r.ch.setCharacter(&theCrab());
  Script s(30);
  s.heard(false);
  advance(r.ch, s, 0.5);
  r.expect1("fshuffle.5", "[dance] frozen: crab shuffle.5 at move beat 0.625");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mfshuffle.5");
    CHECK(c.beatMove == 0, "beatMove %d", c.beatMove);
    CHECK(c.beatPos16 == 491520u, "beatPos16 %u", c.beatPos16);
    CHECK(c.u == 0.625, "u %.9g", c.u);
    CHECK(c.weight == 1.0f, "weight %.9g", c.weight);
    CHECK(c.showDance, "showDance");
    CHECK(!c.flash, "an Mf freeze never lights the beat dot");
    CHECK(crabEq(dance::CrabCharacter::pose(c, c.u), crab::Crab::frozen(5 / 8.0f, false)), "the pose is k5's");
  }
  r.expect1("fshuffle.16", "[dance] Mf: shuffle has steps 0-15");
  r.expect1("fidle.0", "[dance] Mf: idle has no steps");
  r.expect1("fshuffle.15", "[dance] frozen: crab shuffle.15 at move beat 1.875");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mfshuffle.15");
    CHECK(c.beatPos16 == 15u * 98304u && c.u == 1.875, "beatPos16 %u, u %.9g", c.beatPos16, c.u);
    CHECK(crabEq(dance::CrabCharacter::pose(c, c.u), crab::Crab::frozen(7 / 8.0f, true)), "the pose is k15's");
  }
  r.expect1("fshuffle.0", "[dance] frozen: crab shuffle.0 at move beat 0.000");
  r.expect1("fshuffle.05", "[dance] frozen: crab shuffle.5 at move beat 0.625");
  r.expect1("fshuffle.005", "[dance] frozen: crab shuffle.5 at move beat 0.625");
  r.expect1("f shuffle.8 ", "[dance] frozen: crab shuffle.8 at move beat 1.000");
  r.expect1("fshuffle.5m", "[dance] Mf: shuffle has no mirrored form");
  // The checks in order: the move, its steps, the step, the mirror.
  r.expect1("fnope.99", "[dance] Mf: crab has no move \"nope\" (Ml lists them)");
  r.expect1("fshuf.fle.3", "[dance] Mf: crab has no move \"shuf.fle\" (Ml lists them)");  // before the last '.'
  r.expect1("fshuffle..3", "[dance] Mf: crab has no move \"shuffle.\" (Ml lists them)");
  r.expect1("fidle.0m", "[dance] Mf: idle has no steps");
  r.expect1("fshuffle.16m", "[dance] Mf: shuffle has steps 0-15");
  r.expect1("fshuffle.999", "[dance] Mf: shuffle has steps 0-15");
  const char* bad[] = {"fshuffle", "fshuffle.", "f.3", "fshuffle.x", "fshuffle.5mm", "fshuffle.5 m", "fshuffle.1234",
                       "fshuffle.-1", "fshuffle.5M", "fshuffle.+5", "f."};
  for (const char* b : bad) r.expect1(b, kUsageDf);
  // A bad syntax or a refusal leaves the last freeze (shuffle.8) in place.
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after the refusals");
    CHECK(c.beatMove == 0 && c.beatPos16 == 8u * 98304u, "still shuffle.8: beatMove %d, beatPos16 %u", c.beatMove,
          c.beatPos16);
  }
  r.expect1("f", "[dance] Mf off");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mf");
    CHECK(c.beatMove == kNoMove, "unfrozen while quiet: beatMove %d", c.beatMove);
    CHECK(!c.showDance && c.weight == 0.0f, "unfrozen: showDance %d, weight %.9g", c.showDance, c.weight);
  }

  // The stick counts eighths too: Mfbounce.3 is k3's pose.
  Rig st;
  builtIns(st.cast);
  st.ch.setCharacter(&theStick());
  st.chosen = 1;
  st.expect1("fbounce.3", "[dance] frozen: stick bounce.3 at move beat 0.375");
  {
    Script s2(30);
    s2.heard(false);
    const Cue& c = st.ch.update(s2.frame());
    ctx("the stick's cue after Mfbounce.3");
    CHECK(c.beatMove == 0 && c.beatPos16 == 3u * 98304u, "beatMove %d, beatPos16 %u", c.beatMove, c.beatPos16);
    CHECK(stickEq(dance::ClassicStick::pose(c, c.u), dance::Dancer().frozen(3 / 8.0f, false)), "the pose is k3's");
  }
  st.expect1("fsway.0", "[dance] Mf: sway has no steps");
}

// 6. Mf on the fixture: a drawn character's steps from its MOVS record.
void test_freeze_fixture() {
  Rig r;
  withFixture(r.cast);
  r.ch.setCharacter(&fx());
  r.chosen = 2;
  Script s(30);
  s.heard(false);
  advance(r.ch, s, 0.5);
  r.expect1("fpoint.2m", "[dance] frozen: test point.2m at move beat 1.000");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mfpoint.2m");
    CHECK(c.beatMove == 3 && c.beatPos16 == 786432u && c.beatMirror, "beatMove %d, beatPos16 %u, beatMirror %d",
          c.beatMove, c.beatPos16, c.beatMirror);
    CHECK(c.showDance && c.weight == 1.0f, "showDance %d, weight %.9g", c.showDance, c.weight);
  }
  r.expect1("fbounce.3", "[dance] frozen: test bounce.3 at move beat 1.500");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mfbounce.3");
    CHECK(c.beatMove == 0 && c.beatPos16 == 18u * 65536u && !c.beatMirror, "beatMove %d, beatPos16 %u, mirror %d",
          c.beatMove, c.beatPos16, c.beatMirror);
  }
  r.expect1("flook.2", "[dance] frozen: test look.2 at 1500 ms");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mflook.2");
    CHECK(c.idleMove == 12 && c.idleMs == 1500u && !c.idleMirror, "idleMove %d, idleMs %u, idleMirror %d",
          c.idleMove, c.idleMs, c.idleMirror);
    CHECK(!c.showDance && c.weight == 0.0f, "showDance %d, weight %.9g", c.showDance, c.weight);
  }
  r.expect1("flook.1m", "[dance] frozen: test look.1m at 500 ms");
  {
    const Cue& c = r.ch.update(s.frame());
    ctx("the cue after Mflook.1m");
    CHECK(c.idleMove == 12 && c.idleMs == 500u && c.idleMirror, "idleMove %d, idleMs %u, idleMirror %d", c.idleMove,
          c.idleMs, c.idleMirror);
  }
  r.expect1("flook.3", "[dance] Mf: look has steps 0-2");
  r.expect1("fbounce.1m", "[dance] Mf: bounce has no mirrored form");
  r.expect1("ffuture.0", "[dance] Mf: test has no move \"future\" (Ml lists them)");
  r.expect1("fbounce.4", "[dance] Mf: bounce has steps 0-3");
  r.expect1("fpoint.3", "[dance] frozen: test point.3 at move beat 2.000");
  r.expect1("flook.0", "[dance] frozen: test look.0 at 0 ms");
  r.expect1("f", "[dance] Mf off");

  // stepCount and stepStart.
  ctx("stepCount");
  const Character& f = fx();
  TEST_ASSERT_EQUAL_INT(4, dance::stepCount(r.cast, f, fxMove("bounce")));
  TEST_ASSERT_EQUAL_INT(4, dance::stepCount(r.cast, f, fxMove("point")));
  TEST_ASSERT_EQUAL_INT(3, dance::stepCount(r.cast, f, fxMove("look")));
  TEST_ASSERT_EQUAL_INT(16, dance::stepCount(r.cast, theCrab(), 0));
  TEST_ASSERT_EQUAL_INT(0, dance::stepCount(r.cast, theCrab(), 1));
  TEST_ASSERT_EQUAL_INT(16, dance::stepCount(r.cast, theStick(), 0));
  TEST_ASSERT_EQUAL_INT(0, dance::stepCount(r.cast, theStick(), 1));
  // A character not in the cast counts as a code character: eighths.
  DanceCast none;
  TEST_ASSERT_EQUAL_INT(16, dance::stepCount(none, f, fxMove("bounce")));
  TEST_ASSERT_EQUAL_INT(32, dance::stepCount(none, f, fxMove("point")));
  ScriptCharacter sc;
  sc.beatMove("one", 1, 0, 0);
  sc.beatMove("eight", 8, 0, 0);
  TEST_ASSERT_EQUAL_INT(8, dance::stepCount(r.cast, sc, 0));
  TEST_ASSERT_EQUAL_INT(64, dance::stepCount(r.cast, sc, 1));

  ctx("stepStart");
  uint32_t pos = 0xDEADBEEFu;
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, theCrab(), 0, 5, &pos));
  TEST_ASSERT_EQUAL_UINT32(491520u, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, theCrab(), 0, 0, &pos));
  TEST_ASSERT_EQUAL_UINT32(0u, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, theCrab(), 0, 15, &pos));
  TEST_ASSERT_EQUAL_UINT32(15u * 98304u, pos);
  pos = 0xDEADBEEFu;
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, theCrab(), 0, 16, &pos));
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, pos);
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, theCrab(), 0, -1, &pos));
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, theCrab(), 1, 0, &pos));
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, theCrab(), 2, 0, &pos));
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, theCrab(), -1, 0, &pos));
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, f, fxMove("point"), 2, &pos));
  TEST_ASSERT_EQUAL_UINT32(786432u, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, f, fxMove("point"), 3, &pos));
  TEST_ASSERT_EQUAL_UINT32(24u * 65536u, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, f, fxMove("bounce"), 3, &pos));
  TEST_ASSERT_EQUAL_UINT32(18u * 65536u, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, f, fxMove("look"), 1, &pos));
  TEST_ASSERT_EQUAL_UINT32(50u * 65536u, pos);
  TEST_ASSERT_TRUE(dance::stepStart(r.cast, f, fxMove("look"), 2, &pos));
  TEST_ASSERT_EQUAL_UINT32(150u * 65536u, pos);
  pos = 7;
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, f, fxMove("look"), 3, &pos));
  TEST_ASSERT_FALSE(dance::stepStart(r.cast, f, 13, 0, &pos));
  TEST_ASSERT_EQUAL_UINT32(7u, pos);
}

// 7. Mv and the [move] lines: one per new beat run while on.
void test_move_log() {
  Rig r;
  builtIns(r.cast);
  r.ch.setCharacter(&theCrab());
  CHECK(!r.con.verbose(), "Mv starts off");
  r.expect1("v", "[dance] move log on");
  CHECK(r.con.verbose(), "Mv on");
  r.expect1("v", "[dance] move log off");
  CHECK(!r.con.verbose(), "Mv off");
  r.expect1("v", "[dance] move log on");

  Script s(30);
  bool any = false;
  struct Key {
    int8_t move, k;
    uint8_t plays, level;
    bool mirror;
    int64_t start;
    bool operator==(const Key& o) const {
      return move == o.move && k == o.k && plays == o.plays && level == o.level && mirror == o.mirror &&
             start == o.start;
    }
  } last{};
  auto key = [&]() {
    const RunInfo& ri = r.ch.run();
    return Key{ri.move, ri.k, ri.plays, ri.level, ri.firstMirror, ri.start};
  };
  std::vector<std::string> printed;
  std::vector<RunInfo> runs;
  auto frame = [&](long i, bool on) {
    r.ch.update(s.frame());
    r.out.clear();
    r.con.frame(r.ch, Out::put, &r.out);
    ctx("Mv frame %ld", i);
    const RunInfo& ri = r.ch.run();
    const bool want = on && ri.status != RunStatus::None && (!any || !(key() == last));
    if (want) {
      expectLines(r.out, {refMoveLog(r.ch)}, "a [move] line for a new run");
      any = true;
      last = key();
      printed.push_back(r.out.lines[0]);
      runs.push_back(ri);
    } else {
      expectLines(r.out, {}, "no [move] line");
    }
  };
  long i = 0;
  for (; i < 30; ++i) frame(i, true);  // listening, no grid: no run, no line
  CHECK(printed.empty(), "no line before the lock");
  s.acquire(120.0, s.trackTime() + 0.1);
  for (; i < 30 + 300; ++i) frame(i, true);
  ctx("Mv over 10 s locked");
  CHECK(printed.size() >= 4, "%u lines in 10 s at 120 BPM (one a run of 4 dance beats)",
        static_cast<unsigned>(printed.size()));
  for (size_t j = 0; j < runs.size(); ++j) {
    const RunInfo& ri = runs[j];
    CHECK(ri.move == 0 && ri.k == 0 && ri.plays == 2 && !ri.firstMirror && ri.spot == 0,
          "run %u: move %d, k %d, plays %u, mirror %d, spot %d", static_cast<unsigned>(j), ri.move, ri.k, ri.plays,
          ri.firstMirror, ri.spot);
    CHECK(ri.level == (j == 0 ? 0 : 2), "run %u: level %u", static_cast<unsigned>(j), ri.level);
    CHECK(ri.phrase == (j % 2 == 0 ? 0 : 4), "run %u: p %d", static_cast<unsigned>(j), ri.phrase);
    if (j > 0) CHECK(ri.start == runs[j - 1].start + 4, "run %u starts 4 dance beats after the last", static_cast<unsigned>(j));
    const std::string want = fmt("[move] crab shuffle k=0 plays=2 mirror=0 spot=0 p=%d level=%d at=%lld",
                                 j % 2 == 0 ? 0 : 4, j == 0 ? 0 : 2, static_cast<long long>(ri.start));
    CHECK(printed[j] == want, "line %u: %s, want %s", static_cast<unsigned>(j), printed[j].c_str(), want.c_str());
  }
  // Off: nothing.
  r.expect1("v", "[dance] move log off");
  for (long e = i + 150; i < e; ++i) frame(i, false);
  // On again: the running run is printed on the next frame.
  r.expect1("v", "[dance] move log on");
  any = false;
  frame(i++, true);
  ctx("Mv on again");
  CHECK(r.out.lines.size() == 1, "the running run printed when Mv comes on again");
  for (long e = i + 30; i < e; ++i) frame(i, true);
  // reset(): forgets the last run printed, keeps Mv.
  r.con.reset();
  CHECK(r.con.verbose(), "reset() keeps Mv on");
  any = false;
  frame(i++, true);
  ctx("Mv after reset()");
  CHECK(r.out.lines.size() == 1, "the running run printed again after reset()");
  // A forced run prints level=4.
  r.expect1("mshuffle", "[dance] next run: shuffle");
  bool forced = false;
  for (long e = i + 150; i < e; ++i) {
    frame(i, true);
    if (!r.out.lines.empty() && r.ch.run().level == 4) forced = true;
  }
  CHECK(forced, "a [move] line with level=4 after Mmshuffle");
  // A frame() while off prints nothing, whatever the run.
  r.expect1("v", "[dance] move log off");
  r.con.reset();
  CHECK(!r.con.verbose(), "reset() keeps Mv off");
  frame(i++, false);
}

// 8. Mg: a genre for this track, by its folded key.
void test_genre() {
  {
    Rig r;
    builtIns(r.cast);
    r.ch.setCharacter(&theCrab());
    r.expect1("ghip hop", "[dance] genre \"hip hop\" for this track (key 5de39fa5)");
    r.expect1("gR&B", "[dance] genre \"R&B\" for this track (key 1bde7f5c)");
    r.expect1("g--", "[dance] Mg: \"--\" folds to nothing, so no move can match it");
    r.expect1("g", "[dance] Mg: \"\" folds to nothing, so no move can match it");
    r.expect1("g  hip hop  ", "[dance] genre \"hip hop\" for this track (key 5de39fa5)");
    // The fifth and later keys are ignored by the Choreographer; the line is printed anyway.
    r.expect1("gdrum n bass", "[dance] genre \"drum n bass\" for this track (key e6318a55)");
    r.expect1("gK-Pop", "[dance] genre \"K-Pop\" for this track (key 9076a259)");
  }
  // The fixture at 87 BPM: nod (hip hop, R&B, drum n bass) eligible only
  // once a matching genre is injected.
  Rig r;
  withFixture(r.cast);
  r.ch.setCharacter(&fx());
  r.chosen = 2;
  Script s(30);
  s.acquire(87.0, 0.05);
  advance(r.ch, s, 4.0);
  const int nod = fxMove("nod");
  TEST_ASSERT_TRUE(nod >= 0);
  auto nodLine = [&]() {
    r.run("l");
    ctx("Ml at 87 BPM");
    expectLines(r.out, refDl(r.ch), "the reference formatter");
    return r.out.lines[1 + nod];
  };
  auto endsWith = [](const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
  };
  std::string line = nodLine();
  CHECK(endsWith(line, "3 genres, needs 0; now -"), "nod before Mg: %s", line.c_str());
  r.expect1("g--", "[dance] Mg: \"--\" folds to nothing, so no move can match it");
  line = nodLine();
  CHECK(endsWith(line, "; now -"), "nod after Mg--: %s", line.c_str());
  r.expect1("ghip hop", "[dance] genre \"hip hop\" for this track (key 5de39fa5)");
  line = nodLine();
  CHECK(endsWith(line, "; now k 0"), "nod after Mghip hop: %s", line.c_str());
  // A track change clears it.
  r.ch.onTrack(0x1234u, nullptr, 0);
  line = nodLine();
  CHECK(endsWith(line, "; now -"), "nod after a track change: %s", line.c_str());
}

// 9. Ms: refused for characters without travel moves; -1, 0, 1, +1.
void test_spot() {
  {
    Rig r;
    builtIns(r.cast);
    r.ch.setCharacter(&theCrab());
    r.expect1("s-1", "[dance] Ms: crab has no travel moves: it dances at the centre");
    CHECK(r.ch.cue().spot == 0, "the crab stays at the centre (spot %d)", r.ch.cue().spot);
    r.expect1("s1", "[dance] Ms: crab has no travel moves: it dances at the centre");
    r.expect1("sx", "[dance] Ms: crab has no travel moves: it dances at the centre");
    Rig st;
    builtIns(st.cast);
    st.ch.setCharacter(&theStick());
    st.expect1("s+1", "[dance] Ms: stick has no travel moves: it dances at the centre");
    CHECK(st.ch.cue().spot == 0, "the stick stays at the centre");
  }
  Rig r;
  withFixture(r.cast);
  r.ch.setCharacter(&fx());
  r.chosen = 2;
  struct Case {
    const char* arg;
    const char* line;
    int spot;
  } cases[] = {{"s-1", "[dance] spot -1", -1}, {"s1", "[dance] spot +1", 1},  {"s0", "[dance] spot 0", 0},
               {"s+1", "[dance] spot +1", 1},  {"s -1 ", "[dance] spot -1", -1}, {"s0", "[dance] spot 0", 0}};
  for (const Case& c : cases) {
    r.expect1(c.arg, c.line);
    CHECK(r.ch.cue().spot == c.spot, "cue().spot %d at once, want %d", r.ch.cue().spot, c.spot);
  }
  r.expect1("s-1", "[dance] spot -1");
  const char* bad[] = {"s2", "s", "sx", "s-0", "s+-1", "s+0", "s-2", "s1x", "s 1 1"};
  for (const char* b : bad) {
    r.expect1(b, kSpotUsage);
    CHECK(r.ch.cue().spot == -1, "a refused Ms leaves the spot (%d)", r.ch.cue().spot);
  }
}

// 10. Anything else: the unknown line with the whole argument.
void test_unknown() {
  Rig r;
  builtIns(r.cast);
  r.ch.setCharacter(&theCrab());
  const char* args[] = {"x", "lx", "vv", "C", "L", "M", "D", "?", "1", "l shuffle", "v1"};
  for (const char* a : args) r.expect1(a, unknownLine(a));
  r.expect(nullptr, {std::string(kCast0) + " *", kCast1});
  // A long argument: the line is cut at 255 bytes.
  std::string longArg = "x" + std::string(400, 'y');
  ctx("a 401-byte unknown argument");
  r.run(longArg.c_str());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(r.out.lines.size()));
  const std::string full = unknownLine(longArg);
  TEST_ASSERT_EQUAL_INT(255, static_cast<int>(r.out.lines[0].size()));
  CHECK(full.compare(0, 255, r.out.lines[0]) == 0, "the cut line is the full line's first 255 bytes");
}

// 11. No dancer: Ml, Mm, Mf and Ms need a drawn character.
void test_no_dancer() {
  Rig r;
  builtIns(r.cast);
  const char* args[] = {"l", "mshuffle", "fshuffle.1", "s0"};
  for (const char* a : args) r.expect1(a, kNoDancer);
  // The rest don't.
  r.expect("", {std::string(kCast0) + " *", kCast1});
  ctx("Mccrab with no dancer");
  const ConsoleReply rep = r.run("ccrab");
  CHECK(rep.choose && rep.index == 0, "Mccrab: {%d, %d}", rep.choose, rep.index);
  r.expect1("v", "[dance] move log on");
  r.expect1("ghip hop", "[dance] genre \"hip hop\" for this track (key 5de39fa5)");
  r.out.clear();
  Script s(30);
  r.ch.update(s.frame());
  r.con.frame(r.ch, Out::put, &r.out);
  expectLines(r.out, {}, "no [move] line with no dancer");
}

// 12. Lengths: a character at A.5's limits gives complete lines, and no
// line handed to out is over 255 bytes.
void test_lengths() {
  const char* id = "abcdefghijklmnopqrstuvw";                    // 23
  const char* name = "\303\200bcdefghijklmnopqrstuvw";           // 24 B (A-grave: 2 B)
  const char* author = "Authorabcdefghijklmnopqrstuvwxyz01234567";  // 40
  const char* licence = "LicenseRef-abcdefghijklmnopqrstuvwxyz012";  // 40
  TEST_ASSERT_EQUAL_INT(23, static_cast<int>(std::strlen(id)));
  TEST_ASSERT_EQUAL_INT(24, static_cast<int>(std::strlen(name)));
  TEST_ASSERT_EQUAL_INT(40, static_cast<int>(std::strlen(author)));
  TEST_ASSERT_EQUAL_INT(40, static_cast<int>(std::strlen(licence)));
  InfoChar big(id, name, author, licence, false, 63);
  static const uint32_t keys[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  static char names[64][24];
  for (int i = 0; i < 64; ++i) {
    std::snprintf(names[i], sizeof names[i], "m%02d-abcdefghijklmnopqrs", i);  // 23 B
    MoveLabels m = ScriptCharacter::defaults(names[i]);
    if (i % 2 == 0) {
      m.kind = dance::MoveKind::Beat;
      m.beats = 8;
      m.paceLo = 200;
      m.paceHi = 200;
      m.weight10 = 100;
      m.repeat = 8;
      m.flags = 0xFF;
      m.fromTag = 255;
      m.toTag = 255;
      m.fromTagM = 254;
      m.toTagM = 253;
      m.needs = 255;
      m.travel = -2;
      m.startAt = dance::StartAt::Right;
      m.genreCount = 8;
      m.genreKeys = keys;
    } else {
      m.kind = dance::MoveKind::Idle;
      m.beats = 0;
      m.idleMs = 12000;
      m.when = 3;
      m.weight10 = 100;
      m.flags = 0xFF;
      m.needs = 255;
    }
    TEST_ASSERT_TRUE(big.add(m) >= 0);
  }
  Rig r;
  // Renderer::Classic: a Drawn entry must hold a DrawnCharacter (F.1, which
  // stepCount relies on), and this one is an InfoChar that Mf reaches below.
  TEST_ASSERT_TRUE(r.cast.add(&big, Renderer::Classic));
  r.ch.setCharacter(&big);
  const std::string line = std::string("[dance] cast 0: ") + id + " \"" + name + "\" by " + author + ", " + licence +
                           ", card, not in the cycle, 64 moves, 63 ignored *";
  TEST_ASSERT_TRUE(line.size() <= 255);
  r.expect1("", line);
  r.run("l");
  ctx("Ml at the limits");
  expectLines(r.out, refDl(r.ch), "the reference formatter");
  TEST_ASSERT_EQUAL_INT(66, static_cast<int>(r.out.lines.size()));
  for (const std::string& l : r.out.lines) CHECK(l.size() <= 255, "%u bytes", static_cast<unsigned>(l.size()));
  // Other bits than the six named ones are left out of <F>.
  CHECK(r.out.lines[1].find("flags mirror+freeze+start+end+heavy+snap,") != std::string::npos, "%s",
        r.out.lines[1].c_str());
  // Long operands everywhere: each line cut, none over 255.
  const std::string op(300, 'q');
  const char* subs[] = {"c", "m", "f", "g", "s", "x"};
  for (const char* sub : subs) {
    ctx("M%s with a 300-byte operand", sub);
    r.run((std::string(sub) + op).c_str());
    CHECK(r.out.lines.size() == 1, "%u lines", static_cast<unsigned>(r.out.lines.size()));
  }
  r.run((std::string("f") + op + ".1").c_str());
  ctx("Mf with a 300-byte move name");
  CHECK(r.out.lines.size() == 1, "%u lines", static_cast<unsigned>(r.out.lines.size()));
}

// --- added in step 4's integration (rules of F.2 the items above never reach) ---

namespace {

// Frames of s, with frame() after each update, until a [move] line is printed
// for a run of `move` at level 4 (a forced one), at most `secs` seconds; that
// line, checked against F.2's format applied to run(), or "" for none.
std::string forcedLine(Rig& r, Script& s, int move, double secs) {
  const long n = std::lround(secs / static_cast<double>(s.dt()));
  for (long i = 0; i < n; ++i) {
    r.ch.update(s.frame());
    r.out.clear();
    r.con.frame(r.ch, Out::put, &r.out);
    const RunInfo& ri = r.ch.run();
    if (!r.out.lines.empty() && ri.move == move && ri.level == 4) {
      ctx("the forced run's [move] line, frame %ld", i);
      expectLines(r.out, {refMoveLog(r.ch)}, "F.2's format applied to run()");
      return r.out.lines[0];
    }
  }
  return "";
}

bool endsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

}  // namespace

// F.2's sign rule ("a k or a spot prints as -1, 0 or +1") where the items
// above only ever print 0 (and Ml's k -1): the fixture at 87 BPM, where fast
// (150-180) is eligible at k +1 only, and at 120 BPM, where slow (36-60) is
// eligible at k -1. Ml's `now`, and the [move] lines of forced runs (E.2.5: a
// forced move takes its eligibleK), one started at spot +1 (the run's start
// spot is the current spot).
void test_signed_fields() {
  const int fast = fxMove("fast"), slow = fxMove("slow");
  TEST_ASSERT_TRUE(fast >= 0 && slow >= 0);
  {
    Rig r;
    withFixture(r.cast);
    r.ch.setCharacter(&fx());
    r.chosen = 2;
    Script s(30);
    s.acquire(87.0, 0.05);
    advance(r.ch, s, 6.0);
    ctx("the fixture locked at 87 BPM");
    CHECK(r.ch.t10() == 870, "T %d, want 870", r.ch.t10());
    CHECK(r.ch.eligibleNow(fast) == 1, "fast's eligibleNow %d, want +1", r.ch.eligibleNow(fast));
    r.run("l");
    expectLines(r.out, refDl(r.ch), "the reference formatter");
    CHECK(endsWith(r.out.lines[1 + fast], "; now k +1"), "fast: %s", r.out.lines[1 + fast].c_str());
    r.expect1("s+1", "[dance] spot +1");
    r.expect1("mfast", "[dance] next run: fast");
    r.expect1("v", "[dance] move log on");
    const std::string line = forcedLine(r, s, fast, 10.0);
    ctx("the forced fast run at 87 BPM, from spot +1");
    CHECK(!line.empty(), "no [move] line for the forced fast run within 10 s");
    const RunInfo& ri = r.ch.run();
    CHECK(ri.k == 1 && ri.spot == 1 && !ri.firstMirror && ri.plays == 8, "k %d, spot %d, mirror %d, plays %u", ri.k,
          ri.spot, ri.firstMirror, ri.plays);
    TEST_ASSERT_EQUAL_STRING(fmt("[move] test fast k=+1 plays=8 mirror=0 spot=+1 p=%d level=4 at=%lld", ri.phrase,
                                 static_cast<long long>(ri.start))
                                 .c_str(),
                             line.c_str());
  }
  {
    Rig r;
    withFixture(r.cast);
    r.ch.setCharacter(&fx());
    r.chosen = 2;
    Script s(30);
    s.acquire(120.0, 0.05);
    advance(r.ch, s, 6.0);
    ctx("the fixture locked at 120 BPM");
    CHECK(r.ch.eligibleNow(slow) == -1, "slow's eligibleNow %d, want -1", r.ch.eligibleNow(slow));
    r.expect1("mslow", "[dance] next run: slow");
    r.expect1("v", "[dance] move log on");
    const std::string line = forcedLine(r, s, slow, 12.0);
    ctx("the forced slow run at 120 BPM");
    CHECK(!line.empty(), "no [move] line for the forced slow run within 12 s");
    const RunInfo& ri = r.ch.run();
    CHECK(ri.k == -1 && ri.spot == 0 && !ri.firstMirror, "k %d, spot %d, mirror %d", ri.k, ri.spot, ri.firstMirror);
    TEST_ASSERT_EQUAL_STRING(fmt("[move] test slow k=-1 plays=%u mirror=0 spot=0 p=%d level=4 at=%lld", ri.plays,
                                 ri.phrase, static_cast<long long>(ri.start))
                                 .c_str(),
                             line.c_str());
  }
}

// Mm and Mf alone (cancel, unfreeze) are Mm and Mf: with no character they
// print the no-dancer line too (F.2: Ml, Mm, Mf and Ms act on the drawn
// character). And <F> names only the six bits: a move with only other bits
// set prints `-`, one with mirror and another bit `mirror`.
void test_bare_forms_and_other_bits() {
  {
    Rig r;
    builtIns(r.cast);
    r.expect1("m", kNoDancer);
    r.expect1("f", kNoDancer);
  }
  InfoChar odd("odd", "Odd", "IrosTheBeggar", "CC0-1.0", true, 0);
  MoveLabels b = ScriptCharacter::defaults("b");
  b.beats = 2;
  b.flags = 0xC0;
  TEST_ASSERT_EQUAL_INT(0, odd.add(b));
  MoveLabels i = ScriptCharacter::defaults("i");
  i.kind = dance::MoveKind::Idle;
  i.when = 3;
  i.flags = static_cast<uint8_t>(0x40 | dance::kMirror);
  TEST_ASSERT_EQUAL_INT(1, odd.add(i));
  Rig r;
  TEST_ASSERT_TRUE(r.cast.add(&odd, Renderer::Classic));
  r.ch.setCharacter(&odd);
  r.run("l");
  ctx("Ml, flags with other bits");
  expectLines(r.out, refDl(r.ch), "the reference formatter");
  CHECK(r.out.lines[1].find(", flags -, ") != std::string::npos, "%s", r.out.lines[1].c_str());
  CHECK(endsWith(r.out.lines[2], ", flags mirror, needs 0"), "%s", r.out.lines[2].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cast_listing);
  RUN_TEST(test_choose);
  RUN_TEST(test_moves_listing);
  RUN_TEST(test_force);
  RUN_TEST(test_freeze_crab);
  RUN_TEST(test_freeze_fixture);
  RUN_TEST(test_move_log);
  RUN_TEST(test_genre);
  RUN_TEST(test_spot);
  RUN_TEST(test_unknown);
  RUN_TEST(test_no_dancer);
  RUN_TEST(test_lengths);
  RUN_TEST(test_signed_fields);
  RUN_TEST(test_bare_forms_and_other_bits);
  return UNITY_END();
}
