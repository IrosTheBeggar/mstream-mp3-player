// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for nvslayout: the NVS schema number's boot step and the
// resume point's versioned blob. Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "NvsLayout.h"

using nvslayout::SchemaStep;

void setUp() {}
void tearDown() {}

// Absent (fresh, or v0.5.0-beta.1's NVS): written, nothing migrated.
// The same: nothing. Older: migrated from it. Newer: left alone.
void test_schema_steps() {
  using A = SchemaStep::Action;
  SchemaStep s = nvslayout::schemaStep(false, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Write), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(nvslayout::kCurrent, s.from);
  s = nvslayout::schemaStep(true, nvslayout::kCurrent);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::None), static_cast<int>(s.action));
  s = nvslayout::schemaStep(true, 1, 3);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(1, s.from);
  s = nvslayout::schemaStep(true, 0, 1);  // a corrupt 0 is older too: its migrate does nothing
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  s = nvslayout::schemaStep(true, 2, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(2, s.from);
  TEST_ASSERT_EQUAL_UINT16(1, nvslayout::kCurrent);
  TEST_ASSERT_EQUAL_STRING("meta", nvslayout::kMetaNamespace);
  TEST_ASSERT_EQUAL_STRING("schema", nvslayout::kSchemaKey);  // (NVS keys: 15 characters at most)
}

QueueResume sample() {
  QueueResume r;
  r.valid = true;
  r.generation = 0x01020304;
  r.entry = -1;
  r.pathHash = 0xDEADBEEF;
  r.positionMs = 123456;
  r.durationMs = 245000;
  return r;
}

void assertSame(const QueueResume& a, const QueueResume& b) {
  TEST_ASSERT_EQUAL(a.valid, b.valid);
  TEST_ASSERT_EQUAL_UINT32(a.generation, b.generation);
  TEST_ASSERT_EQUAL_INT32(a.entry, b.entry);
  TEST_ASSERT_EQUAL_UINT32(a.pathHash, b.pathHash);
  TEST_ASSERT_EQUAL_UINT32(a.positionMs, b.positionMs);
  TEST_ASSERT_EQUAL_UINT32(a.durationMs, b.durationMs);
}

// Version 1 round trip, its layout byte by byte.
void test_resume_blob_v1() {
  uint8_t b[nvslayout::kResumeBytes];
  TEST_ASSERT_EQUAL_UINT32(24, nvslayout::encodeResume(sample(), b));
  const uint8_t head[] = {1, 0, 0, 0, 0x04, 0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0xBE, 0xAD, 0xDE};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(head, b, sizeof(head));
  QueueResume r;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(sample(), r);
}

// v0.5.0-beta.1's blob (its struct of five uint32/int32, written as is by
// the little-endian ESP32): still read after the update.
void test_resume_blob_v0_still_read() {
  struct OldBlob {
    uint32_t generation;
    int32_t entry;
    uint32_t pathHash;
    uint32_t positionMs;
    uint32_t durationMs;
  };
  static_assert(sizeof(OldBlob) == nvslayout::kResumeV0Bytes, "beta.1's blob was 20 bytes");
  const QueueResume s = sample();
  const OldBlob old{s.generation, s.entry, s.pathHash, s.positionMs, s.durationMs};
  uint8_t b[sizeof(old)];
  std::memcpy(b, &old, sizeof(old));  // (the host is little-endian, as the ESP32)
  QueueResume r;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(s, r);
}

// Anything else is no resume point, and leaves *out alone.
void test_resume_blob_unknown_is_ignored() {
  uint8_t b[nvslayout::kResumeBytes];
  nvslayout::encodeResume(sample(), b);
  QueueResume r;
  r.positionMs = 7;
  b[0] = 2;  // a later version
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, sizeof(b), &r));
  b[0] = 1;
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 23, &r));
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 0, &r));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(7, r.positionMs);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_schema_steps);
  RUN_TEST(test_resume_blob_v1);
  RUN_TEST(test_resume_blob_v0_still_read);
  RUN_TEST(test_resume_blob_unknown_is_ignored);
  return UNITY_END();
}
