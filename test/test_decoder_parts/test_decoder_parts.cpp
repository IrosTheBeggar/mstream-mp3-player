// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for DecoderParts (one decoder's working state and who
// gives each part back: the MP3 generator releases it when it stops, so a
// stopped generator holds nothing until the next track).
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <utility>

#include "DecoderArena.h"
#include "DecoderParts.h"

namespace {
// libmad's mad_frame and mad_synth as ESP8266Audio 2.4.2 builds them.
const size_t kMp3Parts[] = {20784, 4240};

alignas(32) uint8_t g_block[32 * 1024];

// Fake allocations: four slots, each freed at most once.
uint8_t g_mem[4][64];
int g_freed[4];
int g_badFrees;

void countFree(void* p) {
  for (int i = 0; i < 4; ++i) {
    if (p == g_mem[i]) {
      ++g_freed[i];
      return;
    }
  }
  ++g_badFrees;
}

int freedTotal() { return g_freed[0] + g_freed[1] + g_freed[2] + g_freed[3]; }
}  // namespace

void setUp() {
  for (int& f : g_freed) f = 0;
  g_badFrees = 0;
}
void tearDown() {}

// The MP3 generator's way: the block's frame and synth, then the buffer and
// the stream. release() (its stop()) frees the two and gives the block
// back, so the next track can claim it; again, and the destructor, nothing
// more.
void test_release_gives_everything_back_once() {
  DecoderArena arena(kMp3Parts, 2);
  TEST_ASSERT_TRUE(arena.attach(g_block));
  {
    DecoderParts p;
    TEST_ASSERT_TRUE(p.claim(arena));
    p.add(g_mem[0], countFree);
    p.add(g_mem[1], countFree);
    TEST_ASSERT_TRUE(p.ok());
    TEST_ASSERT_TRUE(p.pinned());
    TEST_ASSERT_EQUAL_UINT32(4, p.count());
    TEST_ASSERT_EQUAL_PTR(arena.part(0), p.part(0));
    TEST_ASSERT_EQUAL_PTR(arena.part(1), p.part(1));
    TEST_ASSERT_EQUAL_PTR(g_mem[0], p.part(2));
    TEST_ASSERT_EQUAL_PTR(g_mem[1], p.part(3));
    TEST_ASSERT_NULL(p.part(4));
    TEST_ASSERT_TRUE(arena.inUse());

    p.release();  // the generator stops
    TEST_ASSERT_EQUAL_INT(1, g_freed[0]);
    TEST_ASSERT_EQUAL_INT(1, g_freed[1]);
    TEST_ASSERT_FALSE(arena.inUse());
    TEST_ASSERT_EQUAL_UINT32(0, p.count());
    TEST_ASSERT_FALSE(p.ok());
    TEST_ASSERT_FALSE(p.pinned());

    // The next track claims the block while the stopped one still exists.
    DecoderParts next;
    TEST_ASSERT_TRUE(next.claim(arena));
    p.release();  // stop() again: nothing
  }  // both destroyed: the next one's block back, the first's nothing more
  TEST_ASSERT_EQUAL_INT(1, g_freed[0]);
  TEST_ASSERT_EQUAL_INT(1, g_freed[1]);
  TEST_ASSERT_EQUAL_INT(0, g_badFrees);
  TEST_ASSERT_FALSE(arena.inUse());
  TEST_ASSERT_EQUAL_UINT32(2, arena.claims());
}

// Without the block (none, or lent out): every part its own, all freed.
void test_without_the_block_every_part_is_freed() {
  DecoderArena none(kMp3Parts, 2);  // no block attached
  {
    DecoderParts p;
    TEST_ASSERT_FALSE(p.claim(none));
    for (int i = 0; i < 4; ++i) p.add(g_mem[i], countFree);
    TEST_ASSERT_TRUE(p.ok());
    TEST_ASSERT_FALSE(p.pinned());
  }
  for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_INT(1, g_freed[i]);

  DecoderArena arena(kMp3Parts, 2);
  TEST_ASSERT_TRUE(arena.attach(g_block));
  TEST_ASSERT_TRUE(arena.claim());  // lent out
  DecoderParts p;
  TEST_ASSERT_FALSE(p.claim(arena));
  TEST_ASSERT_EQUAL_UINT32(0, p.count());
  p.release();
  TEST_ASSERT_TRUE(arena.inUse());  // not this one's to give back
}

// A failed allocation: ok() false, and what was there goes back (the
// destructor of the make() that gives up).
void test_a_failed_part_frees_the_rest() {
  DecoderArena arena(kMp3Parts, 2);
  TEST_ASSERT_TRUE(arena.attach(g_block));
  {
    DecoderParts p;
    TEST_ASSERT_TRUE(p.claim(arena));
    p.add(g_mem[0], countFree);
    p.add(nullptr, countFree);  // no RAM for the stream
    TEST_ASSERT_FALSE(p.ok());
  }
  TEST_ASSERT_EQUAL_INT(1, g_freed[0]);
  TEST_ASSERT_EQUAL_INT(1, freedTotal());
  TEST_ASSERT_FALSE(arena.inUse());
}

// More parts than it holds: refused, freed at once, ok() false.
void test_too_many_parts() {
  DecoderParts p;
  for (size_t i = 0; i < DecoderParts::kMaxParts; ++i) p.add(g_mem[0], countFree);
  TEST_ASSERT_TRUE(p.ok());
  p.add(g_mem[1], countFree);
  TEST_ASSERT_FALSE(p.ok());
  TEST_ASSERT_EQUAL_INT(1, g_freed[1]);
  p.release();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecoderParts::kMaxParts), g_freed[0]);
}

// claim() only first: after a part was added the arena's parts can't be
// the first.
void test_claim_only_first() {
  DecoderArena arena(kMp3Parts, 2);
  TEST_ASSERT_TRUE(arena.attach(g_block));
  DecoderParts p;
  p.add(g_mem[0], countFree);
  TEST_ASSERT_FALSE(p.claim(arena));
  TEST_ASSERT_FALSE(arena.inUse());
  TEST_ASSERT_EQUAL_UINT32(0, arena.claims());
}

// Moved (into the generator): the new owner gives it back, the old one
// holds nothing.
void test_a_move_hands_everything_over() {
  DecoderArena arena(kMp3Parts, 2);
  TEST_ASSERT_TRUE(arena.attach(g_block));
  {
    DecoderParts a;
    TEST_ASSERT_TRUE(a.claim(arena));
    a.add(g_mem[2], countFree);
    DecoderParts b(std::move(a));
    TEST_ASSERT_EQUAL_UINT32(0, a.count());
    TEST_ASSERT_FALSE(a.pinned());
    TEST_ASSERT_TRUE(b.ok());
    TEST_ASSERT_TRUE(b.pinned());
    TEST_ASSERT_EQUAL_PTR(g_mem[2], b.part(2));
    a.release();
    TEST_ASSERT_TRUE(arena.inUse());
    TEST_ASSERT_EQUAL_INT(0, g_freed[2]);
  }
  TEST_ASSERT_EQUAL_INT(1, g_freed[2]);
  TEST_ASSERT_FALSE(arena.inUse());
  TEST_ASSERT_EQUAL_INT(0, g_badFrees);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_release_gives_everything_back_once);
  RUN_TEST(test_without_the_block_every_part_is_freed);
  RUN_TEST(test_a_failed_part_frees_the_rest);
  RUN_TEST(test_too_many_parts);
  RUN_TEST(test_claim_only_first);
  RUN_TEST(test_a_move_hands_everything_over);
  return UNITY_END();
}
