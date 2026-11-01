// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the cast (DanceCast) and the two code characters' labels
// (CodeCharacters): docs/DANCE-SPRITES.md, section 1.5 and Appendix F (F.1's
// DanceCast.h and CodeCharacters.h, F.4's test_character_set list, item by
// item). It replaces test_crab_pose's old test_skins: the default dancer is
// the crab, the order is crab then stick, the ids are today's strings, and
// the tap cycle wraps. Written from the doc alone, without reading the
// implementation.
//
// What step 4 pins here, and step 10 extends (overrides through replace(),
// limited to drawn built-ins and applied only after a full load):
//   1. the built-ins: addBuiltIns' order, renderers and the two constants;
//   2. the infos, field for field;
//   3. the labels, field for field, and what the picker derives from them
//      (startSpotMask, freezeMove, hasTravel);
//   4. lookups by id, by pointer and by move name;
//   5. the tap cycle (next) with entries in and out of it;
//   6. adding: null, duplicate ids (the first wins), card entries after the
//      built-ins, and the 19-entry limit.
//
// Run: pio test -e native -f test_character_set (and native_asan, F.4).
#if defined(DANCE_ASAN) && !defined(__SANITIZE_ADDRESS__)
#error "native_asan: -fsanitize=address didn't reach the compiler (docs/DANCE-SPRITES.md, Appendix D.5)"
#endif

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "../support/dance/BeatScript.h"
#include "CodeCharacters.h"
#include "DanceCast.h"
#include "DanceCharacter.h"
#include "DanceRules.h"

using beatscript::ScriptCharacter;
using dance::CastEntry;
using dance::Character;
using dance::DanceCast;
using dance::MoveKind;
using dance::MoveLabels;
using dance::Renderer;

namespace {

#define EXPECT(cond, ...)                                  \
  do {                                                     \
    if (!(cond)) {                                         \
      char m_[512];                                        \
      std::snprintf(m_, sizeof m_, __VA_ARGS__);           \
      TEST_FAIL_MESSAGE(m_);                               \
    }                                                      \
  } while (0)

// The two code characters hold no state (a vtable pointer each, F.1), so one
// of each serves every test.
const dance::CrabCharacter& crabChar() {
  static const dance::CrabCharacter c{};
  return c;
}
const dance::ClassicStick& stickChar() {
  static const dance::ClassicStick c{};
  return c;
}

// A test-local character with a chosen id and cycle membership: one beat move.
class InfoChar final : public Character {
 public:
  explicit InfoChar(const char* id = "x", bool inCycle = true, dance::Origin origin = dance::Origin::Card) {
    std::snprintf(id_, sizeof id_, "%s", id);
    info_.id = id_;
    info_.name = "Info";
    info_.author = "test";
    info_.licence = "CC0-1.0";
    info_.url = "";
    info_.origin = origin;
    info_.inCycle = inCycle;
    info_.ignoredMoves = 0;
    move_ = ScriptCharacter::defaults("step");
    move_.beats = 1;
  }
  InfoChar(const InfoChar&) = delete;
  InfoChar& operator=(const InfoChar&) = delete;
  void setId(const char* id) { std::snprintf(id_, sizeof id_, "%s", id); }
  const dance::CharacterInfo& info() const override { return info_; }
  int moveCount() const override { return 1; }
  const MoveLabels& move(int) const override { return move_; }

 private:
  char id_[32]{};
  dance::CharacterInfo info_{};
  MoveLabels move_{};
};

// The built-in cast, filled in place (DanceCast's copy isn't part of F.1).
void builtIns(DanceCast& cast) { TEST_ASSERT_TRUE(dance::addBuiltIns(cast, crabChar(), stickChar())); }

// Every field of a move's labels against F.1's lists.
void checkLabels(const char* who, int i, const MoveLabels& m, const char* name, bool beat) {
  EXPECT(m.name != nullptr && std::strcmp(m.name, name) == 0, "%s move %d: name \"%s\", want \"%s\"", who, i,
         m.name ? m.name : "(null)", name);
  EXPECT(m.kind == (beat ? MoveKind::Beat : MoveKind::Idle), "%s move %d: kind", who, i);
  EXPECT(m.beats == (beat ? 2 : 0), "%s move %d: beats %u", who, i, m.beats);
  EXPECT(m.idleMs == 0, "%s move %d: idleMs %u (0: continuous)", who, i, m.idleMs);
  EXPECT(m.paceLo == 0 && m.paceHi == 0, "%s move %d: pace %u-%u, want any (0, 0)", who, i, m.paceLo, m.paceHi);
  EXPECT(m.weight10 == 10, "%s move %d: weight10 %u", who, i, m.weight10);
  EXPECT(m.when == (beat ? 0 : 3), "%s move %d: when %u", who, i, m.when);
  EXPECT(m.flags == (beat ? dance::kFreeze : 0), "%s move %d: flags 0x%02x", who, i, m.flags);
  EXPECT(m.repeat == 0, "%s move %d: repeat %u", who, i, m.repeat);
  const int tag = beat ? 1 : 0;  // key 1 is `stand`, its own twin
  EXPECT(m.fromTag == tag && m.toTag == tag && m.fromTagM == tag && m.toTagM == tag,
         "%s move %d: tags %u %u %u %u, want all %d", who, i, m.fromTag, m.toTag, m.fromTagM, m.toTagM, tag);
  EXPECT(m.needs == 0, "%s move %d: needs %u", who, i, m.needs);
  EXPECT(m.travel == 0, "%s move %d: travel %d", who, i, m.travel);
  EXPECT(m.startAt == dance::StartAt::Any, "%s move %d: startAt", who, i);
  EXPECT(m.genreCount == 0, "%s move %d: genreCount %u", who, i, m.genreCount);
  EXPECT(m.genreKeys == nullptr, "%s move %d: genreKeys not null", who, i);
}

void checkInfo(const Character& c, const char* id, const char* name) {
  const dance::CharacterInfo& in = c.info();
  EXPECT(in.id && std::strcmp(in.id, id) == 0, "id \"%s\", want \"%s\"", in.id ? in.id : "(null)", id);
  EXPECT(in.name && std::strcmp(in.name, name) == 0, "%s: name \"%s\", want \"%s\"", id, in.name ? in.name : "(null)",
         name);
  EXPECT(in.author && std::strcmp(in.author, "IrosTheBeggar") == 0, "%s: author", id);
  EXPECT(in.licence && std::strcmp(in.licence, "GPL-3.0-or-later") == 0, "%s: licence", id);
  EXPECT(in.url && std::strcmp(in.url, "") == 0, "%s: url \"%s\", want \"\"", id, in.url ? in.url : "(null)");
  EXPECT(in.origin == dance::Origin::BuiltIn, "%s: origin", id);
  EXPECT(in.inCycle, "%s: inCycle", id);
  EXPECT(in.ignoredMoves == 0, "%s: ignoredMoves %u", id, in.ignoredMoves);
}

}  // namespace

void setUp() {}
void tearDown() {}

// 1. addBuiltIns on an empty cast: crab (Renderer::Crab) then the classic
// stick (Renderer::Classic); the first-boot dancer is entry 0, the crab.
void test_built_ins() {
  TEST_ASSERT_EQUAL_INT(19, dance::kMaxCast);
  TEST_ASSERT_EQUAL_INT(0, dance::kDefaultDancer);

  DanceCast empty;
  TEST_ASSERT_EQUAL_INT(0, empty.size());

  DanceCast cast;
  TEST_ASSERT_TRUE(dance::addBuiltIns(cast, crabChar(), stickChar()));
  TEST_ASSERT_EQUAL_INT(2, cast.size());
  TEST_ASSERT_TRUE(cast.at(0).character == &crabChar());
  TEST_ASSERT_TRUE(cast.at(0).renderer == Renderer::Crab);
  TEST_ASSERT_TRUE(cast.at(1).character == &stickChar());
  TEST_ASSERT_TRUE(cast.at(1).renderer == Renderer::Classic);
  // The default dancer is the crab (test_skins' kDefaultSkin), by id too.
  TEST_ASSERT_EQUAL_STRING("crab", cast.at(dance::kDefaultDancer).character->info().id);
  TEST_ASSERT_EQUAL_STRING("stick", cast.at(1).character->info().id);

  // "true when both were added": a cast that already holds a `crab` refuses
  // the built-in crab, so addBuiltIns reports false.
  InfoChar fake("crab");
  DanceCast taken;
  TEST_ASSERT_TRUE(taken.add(&fake, Renderer::Drawn));
  TEST_ASSERT_FALSE(dance::addBuiltIns(taken, crabChar(), stickChar()));
  TEST_ASSERT_TRUE(taken.at(0).character == &fake);  // the first one wins
}

// 2. The infos, field for field (F.1's table).
void test_infos() {
  checkInfo(crabChar(), "crab", "Crab");
  checkInfo(stickChar(), "stick", "Stick");
  // The same through the Character interface and the cast.
  DanceCast cast;
  builtIns(cast);
  checkInfo(*cast.at(0).character, "crab", "Crab");
  checkInfo(*cast.at(1).character, "stick", "Stick");
}

// 3. The labels, every field of both moves of both characters, and what the
// picker derives from them.
void test_labels() {
  const Character* chars[2] = {&crabChar(), &stickChar()};
  const char* names[2][2] = {{"shuffle", "idle"}, {"bounce", "sway"}};
  for (int c = 0; c < 2; ++c) {
    const Character& ch = *chars[c];
    const char* who = ch.info().id;
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, ch.moveCount(), who);
    checkLabels(who, 0, ch.move(0), names[c][0], true);
    checkLabels(who, 1, ch.move(1), names[c][1], false);
    for (int i = 0; i < 2; ++i)
      for (int o = 0; o < 2; ++o)
        EXPECT(ch.startSpotMask(i, o != 0) == 0b111, "%s: startSpotMask(%d, %d) = 0x%x, want 0b111", who, i, o,
               ch.startSpotMask(i, o != 0));
    EXPECT(dance::freezeMove(ch) == 0, "%s: freezeMove %d, want 0", who, dance::freezeMove(ch));
    EXPECT(!dance::hasTravel(ch), "%s: hasTravel", who);
  }
  // The labels are tables: the same object on every call (flash, F.1).
  TEST_ASSERT_TRUE(&crabChar().move(0) == &crabChar().move(0));
  TEST_ASSERT_TRUE(&crabChar().info() == &crabChar().info());
  // A second instance answers the same.
  const dance::CrabCharacter other{};
  TEST_ASSERT_EQUAL_STRING("crab", other.info().id);
  TEST_ASSERT_EQUAL_STRING("shuffle", other.move(0).name);
}

// 4. Lookups: find by id (byte for byte), indexOf by pointer, findMove by
// name.
void test_lookups() {
  DanceCast cast;
  builtIns(cast);
  TEST_ASSERT_EQUAL_INT(0, cast.find("crab"));
  TEST_ASSERT_EQUAL_INT(1, cast.find("stick"));
  const char* misses[] = {"Crab", "", "stic", "sticks", "crab ", "Stick", " crab"};
  for (const char* m : misses) EXPECT(cast.find(m) == -1, "find(\"%s\") = %d, want -1", m, cast.find(m));
  TEST_ASSERT_EQUAL_INT(-1, cast.find(nullptr));
  DanceCast empty;
  TEST_ASSERT_EQUAL_INT(-1, empty.find("crab"));
  TEST_ASSERT_EQUAL_INT(-1, empty.find(nullptr));

  TEST_ASSERT_EQUAL_INT(0, cast.indexOf(&crabChar()));
  TEST_ASSERT_EQUAL_INT(1, cast.indexOf(&stickChar()));
  ScriptCharacter third;
  TEST_ASSERT_EQUAL_INT(-1, cast.indexOf(&third));
  TEST_ASSERT_EQUAL_INT(-1, cast.indexOf(nullptr));
  const dance::CrabCharacter twin{};  // the same id, another object: not in the cast
  TEST_ASSERT_EQUAL_INT(-1, cast.indexOf(&twin));
  TEST_ASSERT_EQUAL_INT(-1, empty.indexOf(&crabChar()));

  TEST_ASSERT_EQUAL_INT(0, dance::findMove(crabChar(), "shuffle"));
  TEST_ASSERT_EQUAL_INT(1, dance::findMove(crabChar(), "idle"));
  TEST_ASSERT_EQUAL_INT(0, dance::findMove(stickChar(), "bounce"));
  TEST_ASSERT_EQUAL_INT(1, dance::findMove(stickChar(), "sway"));
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(crabChar(), "Shuffle"));
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(crabChar(), ""));
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(crabChar(), nullptr));
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(crabChar(), "bounce"));  // the stick's
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(stickChar(), "shuffle"));
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(crabChar(), "shuffl"));
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(crabChar(), "shuffle "));

  // Two moves of one name: the first, in index order.
  ScriptCharacter dup;
  dup.beatMove("other", 1, 0, 0);
  dup.beatMove("twice", 2, 0, 0);
  dup.idleMove("twice", 1000, 3);
  TEST_ASSERT_EQUAL_INT(1, dance::findMove(dup, "twice"));
  TEST_ASSERT_EQUAL_INT(0, dance::findMove(dup, "other"));
  ScriptCharacter none;  // no moves
  TEST_ASSERT_EQUAL_INT(-1, dance::findMove(none, "twice"));
}

// 5. The tap cycle: the next entry in the cycle after i, with wrap, i itself
// last; an i out of range scans from entry 0; -1 when none is in the cycle.
void test_cycle() {
  {
    DanceCast cast;
    builtIns(cast);
    TEST_ASSERT_EQUAL_INT(1, cast.next(0));  // crab -> stick
    TEST_ASSERT_EQUAL_INT(0, cast.next(1));  // stick -> crab (the wrap)
    TEST_ASSERT_EQUAL_INT(0, cast.next(-1));
    TEST_ASSERT_EQUAL_INT(0, cast.next(2));
    TEST_ASSERT_EQUAL_INT(0, cast.next(99));
    TEST_ASSERT_EQUAL_INT(0, cast.next(-99));
  }
  {  // a third entry out of the cycle is skipped
    DanceCast cast;
    builtIns(cast);
    InfoChar out("stick-classic", false, dance::Origin::BuiltIn);
    TEST_ASSERT_TRUE(cast.add(&out, Renderer::Classic));
    TEST_ASSERT_EQUAL_INT(3, cast.size());
    TEST_ASSERT_EQUAL_INT(1, cast.next(0));
    TEST_ASSERT_EQUAL_INT(0, cast.next(1));
    TEST_ASSERT_EQUAL_INT(0, cast.next(2));
    TEST_ASSERT_EQUAL_INT(0, cast.next(3));
  }
  {  // [in, out, out]
    InfoChar a("a", true), b("b", false), c("c", false);
    DanceCast cast;
    TEST_ASSERT_TRUE(cast.add(&a, Renderer::Drawn));
    TEST_ASSERT_TRUE(cast.add(&b, Renderer::Drawn));
    TEST_ASSERT_TRUE(cast.add(&c, Renderer::Drawn));
    TEST_ASSERT_EQUAL_INT(0, cast.next(0));  // itself, last
    TEST_ASSERT_EQUAL_INT(0, cast.next(1));
    TEST_ASSERT_EQUAL_INT(0, cast.next(2));
    TEST_ASSERT_EQUAL_INT(0, cast.next(-1));
  }
  {  // [out, in, out, in]: the scan starts after i and wraps
    InfoChar a("a", false), b("b", true), c("c", false), d("d", true);
    DanceCast cast;
    TEST_ASSERT_TRUE(cast.add(&a, Renderer::Drawn));
    TEST_ASSERT_TRUE(cast.add(&b, Renderer::Drawn));
    TEST_ASSERT_TRUE(cast.add(&c, Renderer::Drawn));
    TEST_ASSERT_TRUE(cast.add(&d, Renderer::Drawn));
    TEST_ASSERT_EQUAL_INT(1, cast.next(0));
    TEST_ASSERT_EQUAL_INT(3, cast.next(1));
    TEST_ASSERT_EQUAL_INT(3, cast.next(2));
    TEST_ASSERT_EQUAL_INT(1, cast.next(3));  // the wrap
    TEST_ASSERT_EQUAL_INT(1, cast.next(-1));  // from entry 0, which is out
    TEST_ASSERT_EQUAL_INT(1, cast.next(4));
  }
  {  // only out-of-cycle entries, and the empty cast
    InfoChar a("a", false), b("b", false);
    DanceCast cast;
    TEST_ASSERT_TRUE(cast.add(&a, Renderer::Drawn));
    TEST_ASSERT_TRUE(cast.add(&b, Renderer::Drawn));
    TEST_ASSERT_EQUAL_INT(-1, cast.next(0));
    TEST_ASSERT_EQUAL_INT(-1, cast.next(1));
    TEST_ASSERT_EQUAL_INT(-1, cast.next(-1));
    DanceCast empty;
    TEST_ASSERT_EQUAL_INT(-1, empty.next(0));
    TEST_ASSERT_EQUAL_INT(-1, empty.next(-1));
  }
}

// 6. Adding: null, a duplicate id (the first wins), a card entry after the
// built-ins with its renderer, and the limit of 19.
void test_adding() {
  DanceCast cast;
  builtIns(cast);
  TEST_ASSERT_FALSE(cast.add(nullptr, Renderer::Drawn));
  TEST_ASSERT_EQUAL_INT(2, cast.size());

  const dance::CrabCharacter secondCrab{};  // id `crab` again
  TEST_ASSERT_FALSE(cast.add(&secondCrab, Renderer::Crab));
  TEST_ASSERT_EQUAL_INT(2, cast.size());
  TEST_ASSERT_TRUE(cast.at(0).character == &crabChar());
  InfoChar fakeStick("stick");
  TEST_ASSERT_FALSE(cast.add(&fakeStick, Renderer::Drawn));
  TEST_ASSERT_EQUAL_INT(2, cast.size());
  TEST_ASSERT_TRUE(cast.at(1).character == &stickChar());
  TEST_ASSERT_TRUE(cast.at(1).renderer == Renderer::Classic);
  // The same pointer twice is a duplicate id too.
  TEST_ASSERT_FALSE(cast.add(&crabChar(), Renderer::Crab));
  TEST_ASSERT_EQUAL_INT(2, cast.size());

  ScriptCharacter script;  // id `script`
  TEST_ASSERT_TRUE(cast.add(&script, Renderer::Drawn));
  TEST_ASSERT_EQUAL_INT(3, cast.size());
  TEST_ASSERT_TRUE(cast.at(2).character == &script);
  TEST_ASSERT_TRUE(cast.at(2).renderer == Renderer::Drawn);
  TEST_ASSERT_EQUAL_INT(2, cast.find("script"));
  TEST_ASSERT_EQUAL_INT(2, cast.indexOf(&script));
  TEST_ASSERT_EQUAL_INT(0, cast.find("crab"));  // the built-ins keep their places
  TEST_ASSERT_EQUAL_INT(1, cast.find("stick"));
  ScriptCharacter script2;
  TEST_ASSERT_FALSE(cast.add(&script2, Renderer::Classic));
  TEST_ASSERT_EQUAL_INT(3, cast.size());
  TEST_ASSERT_TRUE(cast.at(2).renderer == Renderer::Drawn);

  // The renderer is kept as given, whatever it is.
  InfoChar r("r");
  TEST_ASSERT_TRUE(cast.add(&r, Renderer::Crab));
  TEST_ASSERT_TRUE(cast.at(3).renderer == Renderer::Crab);

  // Filled to 19 entries of distinct ids, a 20th is refused.
  static InfoChar pool[20];
  InfoChar* many[20];
  char id[16];
  for (int i = 0; i < 20; ++i) {
    std::snprintf(id, sizeof id, "pack-%02d", i);
    pool[i].setId(id);
    many[i] = &pool[i];
  }
  DanceCast full;
  for (int i = 0; i < dance::kMaxCast; ++i) {
    EXPECT(full.add(many[i], Renderer::Drawn), "add #%d of %d refused", i + 1, dance::kMaxCast);
    TEST_ASSERT_EQUAL_INT(i + 1, full.size());
  }
  TEST_ASSERT_FALSE(full.add(many[19], Renderer::Drawn));
  TEST_ASSERT_EQUAL_INT(dance::kMaxCast, full.size());
  TEST_ASSERT_EQUAL_INT(-1, full.find("pack-19"));
  TEST_ASSERT_EQUAL_INT(18, full.find("pack-18"));
  for (int i = 0; i < dance::kMaxCast; ++i) TEST_ASSERT_TRUE(full.at(i).character == many[i]);
  TEST_ASSERT_EQUAL_INT(1, full.next(0));
  TEST_ASSERT_EQUAL_INT(0, full.next(18));

  // The built-ins plus 17 card entries fill it too.
  DanceCast mixed;
  builtIns(mixed);
  for (int i = 0; i < dance::kMaxCast - 2; ++i) TEST_ASSERT_TRUE(mixed.add(many[i], Renderer::Drawn));
  TEST_ASSERT_EQUAL_INT(dance::kMaxCast, mixed.size());
  TEST_ASSERT_FALSE(mixed.add(many[19], Renderer::Drawn));
  TEST_ASSERT_EQUAL_INT(dance::kMaxCast, mixed.size());
  TEST_ASSERT_TRUE(mixed.at(0).character == &crabChar());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_built_ins);
  RUN_TEST(test_infos);
  RUN_TEST(test_labels);
  RUN_TEST(test_lookups);
  RUN_TEST(test_cycle);
  RUN_TEST(test_adding);
  return UNITY_END();
}
