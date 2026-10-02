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

// With an MP3 anchor (docs/SEEK.md section 5.2).
QueueResume anchored() {
  QueueResume r = sample();
  ResumeAnchor& a = r.anchor;
  a.kind = ResumeAnchor::Kind::Mp3;
  a.exact = true;
  a.rate = 44100;
  a.sample = 0x0000000123456789ull;  // over 32 bits
  a.fileSize = 8737445;
  a.prerollByte = 2343590;
  a.frameByte = 2345678;
  a.skip = 517;
  a.frameHash = 0xCAFEF00D;
  return r;
}

void assertSame(const QueueResume& a, const QueueResume& b) {
  TEST_ASSERT_EQUAL(a.valid, b.valid);
  TEST_ASSERT_EQUAL_UINT32(a.generation, b.generation);
  TEST_ASSERT_EQUAL_INT32(a.entry, b.entry);
  TEST_ASSERT_EQUAL_UINT32(a.pathHash, b.pathHash);
  TEST_ASSERT_EQUAL_UINT32(a.positionMs, b.positionMs);
  TEST_ASSERT_EQUAL_UINT32(a.durationMs, b.durationMs);
  TEST_ASSERT_TRUE(a.anchor == b.anchor);
}

// Version 2 round trip, its layout byte by byte: every field of the anchor.
void test_resume_blob_v2() {
  uint8_t b[nvslayout::kResumeBytes];
  TEST_ASSERT_EQUAL_UINT32(64, nvslayout::encodeResume(anchored(), b));
  const uint8_t head[] = {2, 1, 1, 0, 0x04, 0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0xBE, 0xAD, 0xDE};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(head, b, sizeof(head));
  const uint8_t anchor[] = {
      0xA5, 0x52, 0x85, 0x00,  // 24: the file size, 8737445
      0x44, 0xAC, 0x00, 0x00,  // 28: the rate, 44100
      0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00,  // 32: the sample (64 bits)
      0xA6, 0xC2, 0x23, 0x00,  // 40: the preroll byte, 2343590
      0xCE, 0xCA, 0x23, 0x00,  // 44: the frame byte, 2345678
      0x05, 0x02, 0x00, 0x00,  // 48: the skip, 517
      0x0D, 0xF0, 0xFE, 0xCA,  // 52: the frame hash
      0, 0, 0, 0, 0, 0, 0, 0,  // 56: reserved
  };
  TEST_ASSERT_EQUAL_UINT8_ARRAY(anchor, b + 24, sizeof(anchor));
  QueueResume r;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(anchored(), r);
  // No anchor: kind 0, the anchor's bytes zero, read back as none.
  TEST_ASSERT_EQUAL_UINT32(64, nvslayout::encodeResume(sample(), b));
  TEST_ASSERT_EQUAL_UINT8(0, b[1]);
  for (size_t i = 24; i < 64; ++i) TEST_ASSERT_EQUAL_UINT8(0, b[i]);
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(sample(), r);
  TEST_ASSERT_FALSE(r.anchor.valid());
  // A FLAC anchor: its sample and its length's low bits; inexact kept.
  QueueResume f = anchored();
  f.anchor.kind = ResumeAnchor::Kind::Flac;
  f.anchor.exact = false;
  f.anchor.prerollByte = f.anchor.frameByte = f.anchor.skip = 0;
  nvslayout::encodeResume(f, b);
  TEST_ASSERT_EQUAL_UINT8(2, b[1]);
  TEST_ASSERT_EQUAL_UINT8(0, b[2]);
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(f, r);
}

// Version 1 (v0.5.0's 24 bytes): still read, with no anchor.
void test_resume_blob_v1_still_read() {
  uint8_t b[nvslayout::kResumeV1Bytes] = {1, 0, 0, 0, 0x04, 0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xEF, 0xBE, 0xAD, 0xDE, 0x40, 0xE2, 0x01, 0x00, 0x08, 0xBD, 0x03, 0x00};
  QueueResume r;
  r.anchor.kind = ResumeAnchor::Kind::Mp3;  // (overwritten)
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(sample(), r);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResumeAnchor::Kind::None), static_cast<int>(r.anchor.kind));
}

// v0.5.0-beta.1's blob (its struct of five uint32/int32, written as is by
// the little-endian ESP32): still read after the update, with no anchor.
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

// Anything else is no resume point, and leaves *out alone: a later version,
// a version-2 blob of the wrong size, version 1's byte on 64 bytes, an
// anchor kind this firmware doesn't know.
void test_resume_blob_unknown_is_ignored() {
  uint8_t b[nvslayout::kResumeBytes + 1];
  nvslayout::encodeResume(anchored(), b);
  QueueResume r;
  r.positionMs = 7;
  b[0] = 3;  // a later version
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 64, &r));
  b[0] = 2;
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 63, &r));
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 65, &r));
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 24, &r));  // version 2's byte on version 1's size
  b[0] = 1;
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 64, &r));  // version 1's byte on version 2's size
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 23, &r));
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 0, &r));
  b[0] = 2;
  b[1] = 3;  // no such kind
  TEST_ASSERT_FALSE(nvslayout::decodeResume(b, 64, &r));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(7, r.positionMs);
}

// The blob gained its version 2 under schema 1 (its own version byte; every
// older version still read): no schema bump, no migration.
void test_the_schema_stays_one() {
  TEST_ASSERT_EQUAL_UINT16(1, nvslayout::kCurrent);
  TEST_ASSERT_EQUAL_UINT8(2, nvslayout::kResumeVersion);
  TEST_ASSERT_EQUAL_UINT32(64, nvslayout::kResumeMaxBytes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_schema_steps);
  RUN_TEST(test_resume_blob_v2);
  RUN_TEST(test_resume_blob_v1_still_read);
  RUN_TEST(test_resume_blob_v0_still_read);
  RUN_TEST(test_resume_blob_unknown_is_ignored);
  RUN_TEST(test_the_schema_stays_one);
  return UNITY_END();
}
