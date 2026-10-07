// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for DecoderArena (the MP3 decoder's state in one fixed
// block, lent to one decoder at a time: docs/RESAMPLER.md section 10d).
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "DecoderArena.h"

void setUp() {}
void tearDown() {}

namespace {
// libmad's mad_frame and mad_synth as ESP8266Audio 2.4.2 builds them.
const size_t kMp3Parts[] = {20784, 4240};

alignas(32) uint8_t g_block[32 * 1024];
}  // namespace

void test_layout_aligns_each_part() {
  const size_t sizes[] = {20784, 4240, 1, 33};
  DecoderArena a(sizes, 4);
  TEST_ASSERT_EQUAL_UINT32(4, a.parts());
  TEST_ASSERT_EQUAL_UINT32(0, a.offset(0));
  TEST_ASSERT_EQUAL_UINT32(20800, a.offset(1));  // 20,784 rounded up to 32
  TEST_ASSERT_EQUAL_UINT32(20800 + 4256, a.offset(2));
  TEST_ASSERT_EQUAL_UINT32(20800 + 4256 + 32, a.offset(3));
  TEST_ASSERT_EQUAL_UINT32(20800 + 4256 + 32 + 64, a.bytes());
  for (size_t i = 0; i < a.parts(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(0, a.offset(i) % DecoderArena::kAlign);
    if (i + 1 < a.parts()) TEST_ASSERT_TRUE(a.offset(i) + a.size(i) <= a.offset(i + 1));
  }
  TEST_ASSERT_TRUE(a.offset(3) + a.size(3) <= a.bytes());
}

void test_mp3_layout_is_25_kb() {
  DecoderArena a(kMp3Parts, 2);
  TEST_ASSERT_EQUAL_UINT32(20800 + 4256, a.bytes());
  TEST_ASSERT_TRUE(a.bytes() <= sizeof(g_block));
}

void test_more_parts_than_it_holds_are_ignored() {
  const size_t sizes[] = {1, 2, 3, 4, 5, 6};
  DecoderArena a(sizes, 6);
  TEST_ASSERT_EQUAL_UINT32(DecoderArena::kMaxParts, a.parts());
  TEST_ASSERT_EQUAL_UINT32(4 * 32, a.bytes());
  TEST_ASSERT_EQUAL_UINT32(0, a.size(5));
}

void test_no_block_no_claim() {
  DecoderArena a(kMp3Parts, 2);
  TEST_ASSERT_FALSE(a.attached());
  TEST_ASSERT_NULL(a.part(0));
  TEST_ASSERT_FALSE(a.claim());
  TEST_ASSERT_EQUAL_UINT32(0, a.refused());  // nothing to refuse: the caller allocates its own
  TEST_ASSERT_FALSE(a.attach(nullptr));
}

void test_misaligned_block_is_refused() {
  DecoderArena a(kMp3Parts, 2);
  TEST_ASSERT_FALSE(a.attach(g_block + 4));
  TEST_ASSERT_FALSE(a.attached());
  TEST_ASSERT_TRUE(a.attach(g_block));
  TEST_ASSERT_EQUAL_PTR(g_block, a.block());
  TEST_ASSERT_EQUAL_PTR(g_block, a.part(0));
  TEST_ASSERT_EQUAL_PTR(g_block + 20800, a.part(1));
  TEST_ASSERT_NULL(a.part(2));
}

void test_one_decoder_at_a_time() {
  DecoderArena a(kMp3Parts, 2);
  TEST_ASSERT_TRUE(a.attach(g_block));
  TEST_ASSERT_TRUE(a.claim());
  TEST_ASSERT_TRUE(a.inUse());
  // A second decoder (made before the first was let go): none for it.
  TEST_ASSERT_FALSE(a.claim());
  TEST_ASSERT_EQUAL_UINT32(1, a.refused());
  // Nor can the block be swapped under the first.
  TEST_ASSERT_FALSE(a.attach(g_block));
  a.release();
  TEST_ASSERT_FALSE(a.inUse());
  // The next track's decoder: the same block, the same place.
  TEST_ASSERT_TRUE(a.claim());
  TEST_ASSERT_EQUAL_PTR(g_block, a.part(0));
  a.release();
  TEST_ASSERT_EQUAL_UINT32(2, a.claims());
  TEST_ASSERT_EQUAL_UINT32(1, a.refused());
}

void test_where() {
  using W = DecoderArena::Where;
  const auto at = [](uintptr_t a) { return reinterpret_cast<const void*>(a); };
  TEST_ASSERT_EQUAL(W::Elsewhere, DecoderArena::where(nullptr, 100));
  TEST_ASSERT_EQUAL(W::Elsewhere, DecoderArena::where(at(0x3FFE0000u), 100));  // internal RAM
  TEST_ASSERT_EQUAL(W::Elsewhere, DecoderArena::where(at(0x3F400000u), 100));  // flash data
  TEST_ASSERT_EQUAL(W::PsramLow, DecoderArena::where(at(0x3F800000u), 25056));
  TEST_ASSERT_EQUAL(W::PsramLow, DecoderArena::where(at(0x3F937000u), 25056));  // measured: fast
  // The last byte decides: a block that runs over the line is (partly) slow.
  TEST_ASSERT_EQUAL(W::PsramLow, DecoderArena::where(at(0x3FA00000u - 100), 100));
  TEST_ASSERT_EQUAL(W::PsramHigh, DecoderArena::where(at(0x3FA00000u - 100), 101));
  TEST_ASSERT_EQUAL(W::PsramHigh, DecoderArena::where(at(0x3FA00014u), 25056));  // measured: 1.7-3.6x
  TEST_ASSERT_EQUAL(W::PsramHigh, DecoderArena::where(at(0x3FBD0014u), 25056));
  TEST_ASSERT_EQUAL(W::Elsewhere, DecoderArena::where(at(0x3FC00000u), 4));
  TEST_ASSERT_EQUAL_STRING("PSRAM, its lower 2 MB", DecoderArena::whereName(W::PsramLow));
}

// ---- several decoders, one block (docs/OPUS.md) ----

// The Opus decoder's state (opus_decoder_get_size(2) on the ESP32) and one
// frame's PCM (2,880 x 2 x int16).
const size_t kOpusParts[] = {26520, 11520};

void test_a_second_layout_sizes_the_block_to_the_largest() {
  DecoderArena a(kMp3Parts, 2);
  TEST_ASSERT_EQUAL_UINT32(1, a.layouts());
  TEST_ASSERT_EQUAL_INT(1, a.addLayout(kOpusParts, 2));
  TEST_ASSERT_EQUAL_UINT32(2, a.layouts());
  TEST_ASSERT_EQUAL_UINT32(20800 + 4256, a.layoutBytes(0));
  TEST_ASSERT_EQUAL_UINT32(26528 + 11520, a.layoutBytes(1));  // 26,520 rounded up to 32
  TEST_ASSERT_EQUAL_UINT32(26528 + 11520, a.bytes());
  TEST_ASSERT_EQUAL_UINT32(2, a.layoutParts(1));
  TEST_ASSERT_EQUAL_UINT32(0, a.layoutParts(2));
  // No third layout, and none once the block is attached.
  TEST_ASSERT_EQUAL_INT(-1, a.addLayout(kMp3Parts, 2));
  DecoderArena b(kOpusParts, 2);
  TEST_ASSERT_TRUE(b.attach(g_block));
  TEST_ASSERT_EQUAL_INT(-1, b.addLayout(kMp3Parts, 2));
  TEST_ASSERT_EQUAL_UINT32(1, b.layouts());
}

void test_a_claim_names_its_layout() {
  DecoderArena a(kMp3Parts, 2);
  TEST_ASSERT_EQUAL_INT(1, a.addLayout(kOpusParts, 2));
  TEST_ASSERT_TRUE(a.attach(g_block));
  // Nothing claimed: layout 0's parts (the MP3 path reads them as before).
  TEST_ASSERT_EQUAL_UINT32(2, a.parts());
  TEST_ASSERT_EQUAL_UINT32(20784, a.size(0));
  TEST_ASSERT_EQUAL_PTR(g_block + 20800, a.part(1));
  // The Opus decoder's claim: its own parts, at its own offsets.
  TEST_ASSERT_TRUE(a.claim(1));
  TEST_ASSERT_EQUAL_UINT32(1, a.claimed());
  TEST_ASSERT_EQUAL_UINT32(26520, a.size(0));
  TEST_ASSERT_EQUAL_UINT32(11520, a.size(1));
  TEST_ASSERT_EQUAL_PTR(g_block, a.part(0));
  TEST_ASSERT_EQUAL_PTR(g_block + 26528, a.part(1));
  TEST_ASSERT_EQUAL_UINT32(0, reinterpret_cast<uintptr_t>(a.part(1)) % DecoderArena::kAlign);
  // One decoder at a time, whichever the layout.
  TEST_ASSERT_FALSE(a.claim(0));
  TEST_ASSERT_FALSE(a.claim(1));
  TEST_ASSERT_EQUAL_UINT32(2, a.refused());
  a.release();
  TEST_ASSERT_EQUAL_UINT32(0, a.claimed());
  TEST_ASSERT_EQUAL_UINT32(20784, a.size(0));  // layout 0 again
  // The MP3 decoder's claim (the default) after it: libmad's parts.
  TEST_ASSERT_TRUE(a.claim());
  TEST_ASSERT_EQUAL_UINT32(0, a.claimed());
  TEST_ASSERT_EQUAL_PTR(g_block + 20800, a.part(1));
  a.release();
  // A layout that doesn't exist: refused, not counted as a decoder's loss.
  TEST_ASSERT_FALSE(a.claim(2));
  TEST_ASSERT_EQUAL_UINT32(2, a.refused());
  TEST_ASSERT_FALSE(a.inUse());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_layout_aligns_each_part);
  RUN_TEST(test_mp3_layout_is_25_kb);
  RUN_TEST(test_more_parts_than_it_holds_are_ignored);
  RUN_TEST(test_no_block_no_claim);
  RUN_TEST(test_misaligned_block_is_refused);
  RUN_TEST(test_one_decoder_at_a_time);
  RUN_TEST(test_where);
  RUN_TEST(test_a_second_layout_sizes_the_block_to_the_largest);
  RUN_TEST(test_a_claim_names_its_layout);
  return UNITY_END();
}
