// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for nvslayout: the NVS schema number's boot step, the
// resume point's versioned blob and the repeat mode's key. Run: pio test -e native
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
  TEST_ASSERT_EQUAL_UINT16(2, nvslayout::kCurrent);
  s = nvslayout::schemaStep(true, 1);  // a schema-1 unit (v0.5.0 to v0.6.0): migrated to 2
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(1, s.from);
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
// a version-2 blob of the wrong size, version 1's byte on 64 bytes.
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
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(7, r.positionMs);
}

// An Opus anchor (kind 3, docs/OPUS.md section 9): the FLAC model's fields
// (the sample, the size, the exact length's low bits in the frame hash,
// the byte fields 0), a round trip byte for byte; it is what
// oggopus::makeAnchor() makes and SeekIndex::anchorAt() gives for an Opus
// run, read back for oggopus::checkAnchor().
void test_resume_blob_opus_anchor() {
  QueueResume o = sample();
  ResumeAnchor& a = o.anchor;
  a.kind = ResumeAnchor::Kind::Opus;
  a.exact = true;
  a.rate = 48000;
  a.sample = 4075200;        // 84.9 s in
  a.fileSize = 3357890;
  a.frameHash = 10076160;    // the exact trimmed length (mStream's 3:30 transcode)
  uint8_t b[nvslayout::kResumeBytes];
  TEST_ASSERT_EQUAL_UINT32(64, nvslayout::encodeResume(o, b));
  TEST_ASSERT_EQUAL_UINT8(2, b[0]);
  TEST_ASSERT_EQUAL_UINT8(3, b[1]);  // the kind
  TEST_ASSERT_EQUAL_UINT8(1, b[2]);  // exact
  const uint8_t anchor[] = {
      0xC2, 0x3C, 0x33, 0x00,  // 24: the file size, 3357890
      0x80, 0xBB, 0x00, 0x00,  // 28: the rate, 48000
      0xC0, 0x2E, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00,  // 32: the sample, 4075200
      0, 0, 0, 0,              // 40: no preroll byte
      0, 0, 0, 0,              // 44: no frame byte
      0, 0, 0, 0,              // 48: no skip
      0x00, 0xC0, 0x99, 0x00,  // 52: the length, 10076160
      0, 0, 0, 0, 0, 0, 0, 0,  // 56: reserved
  };
  TEST_ASSERT_EQUAL_UINT8_ARRAY(anchor, b + 24, sizeof(anchor));
  QueueResume r;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(o, r);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResumeAnchor::Kind::Opus), static_cast<int>(r.anchor.kind));
  // 0.6.0's reader refused the whole blob on a kind above FLAC (its rule,
  // modelled here: `if (b[1] > 2) return false`), so a downgrade to it
  // from a pause saved on an Opus track finds no resume point, once: the
  // pattern SEEK.md section 5.2 documents for every blob version.
  auto oldRule = [](const uint8_t* blob, size_t n) { return n == 64 && blob[0] == 2 && blob[1] <= 2; };
  TEST_ASSERT_FALSE(oldRule(b, sizeof(b)));
  nvslayout::encodeResume(anchored(), b);  // (an MP3's anchor: read by both)
  TEST_ASSERT_TRUE(oldRule(b, sizeof(b)));
}

// A kind this firmware doesn't know (a later format's anchor, 4 and up)
// keeps the five words and drops the anchor: the point resumes by its
// second, as any anchor that fails its check does, instead of being lost
// as it was before Opus (the old rule above).
void test_resume_blob_unknown_kind_keeps_the_position() {
  uint8_t b[nvslayout::kResumeBytes];
  nvslayout::encodeResume(anchored(), b);
  b[1] = 4;  // no such kind
  QueueResume r;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  QueueResume want = sample();  // the words, no anchor
  assertSame(want, r);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.anchor.valid());
  TEST_ASSERT_EQUAL_UINT32(123456, r.positionMs);
  b[1] = 255;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
  assertSame(want, r);
  // The known kinds still read whole.
  for (uint8_t kind = 1; kind <= 3; ++kind) {
    b[1] = kind;
    TEST_ASSERT_TRUE(nvslayout::decodeResume(b, sizeof(b), &r));
    TEST_ASSERT_EQUAL_UINT8(kind, static_cast<uint8_t>(r.anchor.kind));
    TEST_ASSERT_EQUAL_UINT32(8737445, r.anchor.fileSize);
  }
}

// The blob gained its version 2 under schema 1 (its own version byte; every
// older version still read): no schema bump for that. A new key is a
// layout change: schema 2 is schema 1's keys and "queue"/"repeat".
void test_the_schema_is_two() {
  TEST_ASSERT_EQUAL_UINT16(2, nvslayout::kCurrent);
  TEST_ASSERT_EQUAL_UINT8(2, nvslayout::kResumeVersion);
  TEST_ASSERT_EQUAL_UINT32(64, nvslayout::kResumeMaxBytes);
}

// The repeat mode as saved: absent, or a value this firmware doesn't
// know, is Off; 0-2 are themselves. The key fits NVS's 15 characters.
void test_repeat_from_what_is_saved() {
  TEST_ASSERT_EQUAL_UINT8(0, nvslayout::repeatFrom(false, 0));
  TEST_ASSERT_EQUAL_UINT8(0, nvslayout::repeatFrom(false, 2));  // (absent: whatever was read)
  TEST_ASSERT_EQUAL_UINT8(0, nvslayout::repeatFrom(true, 0));
  TEST_ASSERT_EQUAL_UINT8(1, nvslayout::repeatFrom(true, 1));
  TEST_ASSERT_EQUAL_UINT8(2, nvslayout::repeatFrom(true, 2));
  TEST_ASSERT_EQUAL_UINT8(0, nvslayout::repeatFrom(true, 3));
  TEST_ASSERT_EQUAL_UINT8(0, nvslayout::repeatFrom(true, 255));
  TEST_ASSERT_EQUAL_STRING("repeat", nvslayout::kRepeatKey);
  TEST_ASSERT_TRUE(std::strlen(nvslayout::kRepeatKey) <= 15);
}

// The migration matrix: every unit this firmware can boot on, and what
// the boot keeps of each. The Opus work added the resume blob's kind 3
// under schema 1 (a blob's kinds are its own version's business, no
// bump), and the queue modes moved the schema to 2 for "queue"/"repeat";
// a unit may have run either build before this one, or 0.6.0, or none.
// The schema's step, the repeat mode and the resume point, as the boot
// reads them (NvsSchema::check(), QueueStore::loadRepeat() and
// loadResume()): the resume point survives whole on every row, and the
// mode where a unit had one.
void test_migration_matrix() {
  using A = SchemaStep::Action;
  using Kind = ResumeAnchor::Kind;
  struct Unit {
    const char* ran;
    bool haveSchema;
    uint16_t schema;
    bool haveRepeat;
    uint8_t repeat;
    Kind kind;  // its saved resume point's anchor
    A want;
    uint8_t wantRepeat;
  };
  const Unit units[] = {
      {"a fresh unit, or v0.5.0-beta.1's", false, 0, false, 0, Kind::None, A::Write, 0},
      {"v0.5.0 (schema 1, no anchor)", true, 1, false, 0, Kind::None, A::Migrate, 0},
      {"0.6.0 (schema 1, an MP3 anchor)", true, 1, false, 0, Kind::Mp3, A::Migrate, 0},
      {"0.6.0 (schema 1, a FLAC anchor)", true, 1, false, 0, Kind::Flac, A::Migrate, 0},
      {"the Opus build before the merge (schema 1, an Opus anchor)", true, 1, false, 0, Kind::Opus, A::Migrate, 0},
      {"the queue modes build before the merge (schema 2, Repeat One)", true, 2, true, 2, Kind::Mp3, A::None, 2},
      {"this firmware (schema 2, Repeat All, an Opus anchor)", true, 2, true, 1, Kind::Opus, A::None, 1},
  };
  for (const Unit& u : units) {
    const SchemaStep s = nvslayout::schemaStep(u.haveSchema, u.schema);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(u.want), static_cast<int>(s.action), u.ran);
    if (u.want == A::Migrate) TEST_ASSERT_EQUAL_UINT16_MESSAGE(1, s.from, u.ran);  // (its step does nothing)
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(u.wantRepeat, nvslayout::repeatFrom(u.haveRepeat, u.repeat), u.ran);
    QueueResume saved = u.kind == Kind::None ? sample() : anchored();
    saved.anchor.kind = u.kind;
    if (u.kind == Kind::Flac || u.kind == Kind::Opus) {  // the FLAC model: no byte fields
      saved.anchor.prerollByte = saved.anchor.frameByte = saved.anchor.skip = 0;
    }
    uint8_t b[nvslayout::kResumeBytes];
    nvslayout::encodeResume(saved, b);
    QueueResume r;
    TEST_ASSERT_TRUE_MESSAGE(nvslayout::decodeResume(b, sizeof(b), &r), u.ran);
    assertSame(saved, r);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(u.kind), static_cast<int>(r.anchor.kind), u.ran);
  }
  // The downgrades from this firmware (schema 2, perhaps an Opus anchor),
  // each modelled by the rule that build has: to the queue modes build
  // (schema 2: nothing to do; its reader knew the kinds up to FLAC, so an
  // Opus anchor's blob is refused and that resume point is lost once, as
  // with 0.6.0; "repeat" kept), to the Opus build (schema 1: "newer, left
  // as it is", the repeat key never read; its reader is this one's, so the
  // resume point stays whole), to 0.6.0 (both).
  uint8_t opus[nvslayout::kResumeBytes], mp3[nvslayout::kResumeBytes];
  QueueResume o = anchored();
  o.anchor.kind = Kind::Opus;
  nvslayout::encodeResume(o, opus);
  nvslayout::encodeResume(anchored(), mp3);
  auto upToFlac = [](const uint8_t* blob) { return blob[0] == 2 && blob[1] <= 2; };  // 0.6.0's and the queue modes'
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::None), static_cast<int>(nvslayout::schemaStep(true, 2, 2).action));
  TEST_ASSERT_FALSE(upToFlac(opus));
  TEST_ASSERT_TRUE(upToFlac(mp3));
  TEST_ASSERT_EQUAL_UINT8(2, nvslayout::repeatFrom(true, 2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(nvslayout::schemaStep(true, 2, 1).action));
  QueueResume r;
  TEST_ASSERT_TRUE(nvslayout::decodeResume(opus, sizeof(opus), &r));  // the Opus build's reader
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Opus), static_cast<int>(r.anchor.kind));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_schema_steps);
  RUN_TEST(test_resume_blob_v2);
  RUN_TEST(test_resume_blob_v1_still_read);
  RUN_TEST(test_resume_blob_v0_still_read);
  RUN_TEST(test_resume_blob_unknown_is_ignored);
  RUN_TEST(test_resume_blob_opus_anchor);
  RUN_TEST(test_resume_blob_unknown_kind_keeps_the_position);
  RUN_TEST(test_the_schema_is_two);
  RUN_TEST(test_repeat_from_what_is_saved);
  RUN_TEST(test_migration_matrix);
  return UNITY_END();
}
