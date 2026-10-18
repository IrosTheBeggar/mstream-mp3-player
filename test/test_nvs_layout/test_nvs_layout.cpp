// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for nvslayout: the NVS schema number's boot step, the
// resume point's versioned blob, the repeat mode's key, the UI's language
// (schema 3), Auto DJ's settings (schema 4) and the play log's ids (schema
// 5). Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "NvsLayout.h"
#include "UiStrings.h"

using nvslayout::SchemaStep;
using uistr::Lang;

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
  TEST_ASSERT_EQUAL_UINT16(5, nvslayout::kCurrent);
  s = nvslayout::schemaStep(true, 1);  // a schema-1 unit (v0.5.0 to v0.6.0): migrated to 5
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(1, s.from);
  s = nvslayout::schemaStep(true, 2);  // a schema-2 unit (0.7.0 to 0.9.0): migrated to 5
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(2, s.from);
  s = nvslayout::schemaStep(true, 3);  // a schema-3 unit (0.10.0): migrated to 5
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(3, s.from);
  s = nvslayout::schemaStep(true, 4);  // a schema-4 unit (0.11.0): migrated to 5
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Migrate), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(4, s.from);
  s = nvslayout::schemaStep(true, 3, 2);  // 0.9.0 on a schema-3 NVS: left as it is
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(s.action));
  s = nvslayout::schemaStep(true, 4, 3);  // 0.10.0 on 0.11.0's NVS: left as it is
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(4, s.from);
  s = nvslayout::schemaStep(true, 5, 4);  // 0.11.0 on this firmware's NVS: left as it is
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT16(5, s.from);
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
// layout change: schema 2 is schema 1's keys and "queue"/"repeat", schema
// 3 schema 2's and "ui"/"lang", schema 4 schema 3's and the four of
// "autodj", schema 5 schema 4's and the three of "plays".
void test_the_schema_is_five() {
  TEST_ASSERT_EQUAL_UINT16(5, nvslayout::kCurrent);
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

// The UI's language as saved: one of the ten by its tag; absent, or a tag
// this firmware hasn't (a later one's, after a downgrade), English. The
// keys fit NVS's 15 characters, and every tag its room.
void test_language_from_what_is_saved() {
  TEST_ASSERT_TRUE(nvslayout::langFrom(false, "de") == Lang::En);
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, "de") == Lang::De);
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, "zh") == Lang::Zh);
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, "en") == Lang::En);
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, "ko") == Lang::En);  // a later firmware's
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, "DE") == Lang::En);  // tags as uistr::tag() writes them
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, "") == Lang::En);
  TEST_ASSERT_TRUE(nvslayout::langFrom(true, nullptr) == Lang::En);
  for (unsigned i = 0; i < uistr::kLangs; ++i) {
    const Lang l = static_cast<Lang>(i);
    TEST_ASSERT_TRUE_MESSAGE(nvslayout::langFrom(true, uistr::tag(l)) == l, uistr::tag(l));
    TEST_ASSERT_TRUE(std::strlen(uistr::tag(l)) < nvslayout::kLangMaxBytes);
  }
  TEST_ASSERT_EQUAL_STRING("ui", nvslayout::kUiNamespace);
  TEST_ASSERT_EQUAL_STRING("lang", nvslayout::kLangKey);
  TEST_ASSERT_EQUAL_STRING("coach", nvslayout::kCoachKey);
}

// The first boot's picker: a fresh unit only (no language, the tips not
// seen); migrate() gives every older unit English before it can ask.
void test_language_picker_and_its_migration() {
  TEST_ASSERT_TRUE(nvslayout::langPickerDue(false, false));
  TEST_ASSERT_FALSE(nvslayout::langPickerDue(true, false));  // chosen (or an update's "en")
  TEST_ASSERT_FALSE(nvslayout::langPickerDue(false, true));  // the tips seen: not a fresh unit
  TEST_ASSERT_FALSE(nvslayout::langPickerDue(true, true));
  TEST_ASSERT_TRUE(nvslayout::migrateWritesLang(1, false));
  TEST_ASSERT_TRUE(nvslayout::migrateWritesLang(2, false));
  TEST_ASSERT_FALSE(nvslayout::migrateWritesLang(2, true));  // a downgrade left one: kept
  TEST_ASSERT_FALSE(nvslayout::migrateWritesLang(3, false));
  TEST_ASSERT_FALSE(nvslayout::migrateWritesLang(4, false));  // (the step to 4 writes nothing)
}

// Auto DJ's settings as saved (schema 4): absent, each key's default (off,
// Follow the vibe, 55 %, both matches off: the user's D2); a value this
// firmware doesn't take (a later one's), that key's default; the match
// bits each on their own, other bits ignored. What autoDjTo() writes reads
// back the same. The names fit NVS's 15 characters.
void test_auto_dj_from_what_is_saved() {
  using nvslayout::AutoDjSaved;
  using nvslayout::AutoDjStored;
  const AutoDjSaved none = nvslayout::autoDjFrom(AutoDjStored());
  TEST_ASSERT_FALSE(none.on);
  TEST_ASSERT_EQUAL_UINT8(0, none.mode);
  TEST_ASSERT_EQUAL_UINT8(55, none.floorPct);
  TEST_ASSERT_FALSE(none.matchTempo);  // D2: BPM continuity off
  TEST_ASSERT_FALSE(none.matchKey);    // D2: Harmonic mixing off
  // Absent keys, whatever was read into them.
  AutoDjStored junk;
  junk.on = 1;
  junk.mode = 1;
  junk.floor = 70;
  junk.match = 3;
  const AutoDjSaved j = nvslayout::autoDjFrom(junk);
  TEST_ASSERT_FALSE(j.on);
  TEST_ASSERT_EQUAL_UINT8(0, j.mode);
  TEST_ASSERT_EQUAL_UINT8(55, j.floorPct);
  TEST_ASSERT_FALSE(j.matchTempo || j.matchKey);
  // Each key.
  AutoDjStored st;
  st.haveOn = st.haveMode = st.haveFloor = st.haveMatch = true;
  st.on = 1;
  st.mode = 1;
  st.floor = 65;
  st.match = 1;
  AutoDjSaved a = nvslayout::autoDjFrom(st);
  TEST_ASSERT_TRUE(a.on);
  TEST_ASSERT_EQUAL_UINT8(1, a.mode);
  TEST_ASSERT_EQUAL_UINT8(65, a.floorPct);
  TEST_ASSERT_TRUE(a.matchTempo);
  TEST_ASSERT_FALSE(a.matchKey);
  st.match = 2;
  a = nvslayout::autoDjFrom(st);
  TEST_ASSERT_FALSE(a.matchTempo);
  TEST_ASSERT_TRUE(a.matchKey);
  st.match = 0xFC;  // a later firmware's bits only
  a = nvslayout::autoDjFrom(st);
  TEST_ASSERT_FALSE(a.matchTempo || a.matchKey);
  // Values this firmware doesn't take.
  st.on = 2;
  st.mode = 2;
  st.floor = 80;
  a = nvslayout::autoDjFrom(st);
  TEST_ASSERT_FALSE(a.on);
  TEST_ASSERT_EQUAL_UINT8(0, a.mode);
  TEST_ASSERT_EQUAL_UINT8(55, a.floorPct);
  const uint8_t floors[] = {0, 50, 54, 56, 62, 74, 76, 255};
  for (uint8_t f : floors) {
    st.floor = f;
    TEST_ASSERT_EQUAL_UINT8(55, nvslayout::autoDjFrom(st).floorPct);
    TEST_ASSERT_FALSE(nvslayout::autoDjFloorValid(f));
  }
  for (uint8_t f = 55; f <= 75; f += 5) {
    st.floor = f;
    TEST_ASSERT_EQUAL_UINT8(f, nvslayout::autoDjFrom(st).floorPct);
    TEST_ASSERT_TRUE(nvslayout::autoDjFloorValid(f));
  }
  // Round trips: every combination.
  for (int on = 0; on < 2; ++on)
    for (uint8_t mode = 0; mode < 2; ++mode)
      for (uint8_t f = 55; f <= 75; f += 5)
        for (int m = 0; m < 4; ++m) {
          AutoDjSaved w;
          w.on = on != 0;
          w.mode = mode;
          w.floorPct = f;
          w.matchTempo = (m & 1) != 0;
          w.matchKey = (m & 2) != 0;
          const AutoDjStored out = nvslayout::autoDjTo(w);
          TEST_ASSERT_TRUE(out.haveOn && out.haveMode && out.haveFloor && out.haveMatch);
          const AutoDjSaved r = nvslayout::autoDjFrom(out);
          TEST_ASSERT_EQUAL(w.on, r.on);
          TEST_ASSERT_EQUAL_UINT8(w.mode, r.mode);
          TEST_ASSERT_EQUAL_UINT8(w.floorPct, r.floorPct);
          TEST_ASSERT_EQUAL(w.matchTempo, r.matchTempo);
          TEST_ASSERT_EQUAL(w.matchKey, r.matchKey);
        }
  AutoDjSaved odd;
  odd.floorPct = 62;  // (nothing sets it: written as the default)
  odd.mode = 7;
  TEST_ASSERT_EQUAL_UINT8(55, nvslayout::autoDjTo(odd).floor);
  TEST_ASSERT_EQUAL_UINT8(0, nvslayout::autoDjTo(odd).mode);
  TEST_ASSERT_EQUAL_STRING("autodj", nvslayout::kAutoDjNamespace);
  TEST_ASSERT_EQUAL_STRING("on", nvslayout::kAutoDjOnKey);
  TEST_ASSERT_EQUAL_STRING("mode", nvslayout::kAutoDjModeKey);
  TEST_ASSERT_EQUAL_STRING("floor", nvslayout::kAutoDjFloorKey);
  TEST_ASSERT_EQUAL_STRING("match", nvslayout::kAutoDjMatchKey);
  const char* names[] = {nvslayout::kAutoDjNamespace, nvslayout::kAutoDjOnKey, nvslayout::kAutoDjModeKey,
                         nvslayout::kAutoDjFloorKey, nvslayout::kAutoDjMatchKey};
  for (const char* n : names) TEST_ASSERT_TRUE(std::strlen(n) <= 15);
}

// The play log's ids as saved (schema 5; docs/METADATA.md 2.20.4): no
// epoch (absent, or 0) is none, whatever seq and boot count are there (a
// new epoch cut after them: they are its 0s, or stale); with one, its seq
// and boot count, each absent 0. The names fit NVS's 15 characters.
void test_the_play_logs_ids_from_what_is_saved() {
  using nvslayout::PlaysIds;
  using nvslayout::PlaysStored;
  PlaysStored s;
  PlaysIds p = nvslayout::playsFrom(s);
  TEST_ASSERT_EQUAL_UINT64(0, p.epoch);
  s.haveSeq = s.haveBoots = true;
  s.seq = 41;
  s.boots = 3;
  p = nvslayout::playsFrom(s);  // (no epoch: a new one, from 0)
  TEST_ASSERT_EQUAL_UINT64(0, p.epoch);
  TEST_ASSERT_EQUAL_UINT32(0, p.seq);
  TEST_ASSERT_EQUAL_UINT32(0, p.boots);
  s.haveEpoch = true;
  s.epoch = 0;
  TEST_ASSERT_EQUAL_UINT64(0, nvslayout::playsFrom(s).epoch);
  s.epoch = 0xF0E1D2C3B4A59687ull;
  p = nvslayout::playsFrom(s);
  TEST_ASSERT_EQUAL_UINT64(0xF0E1D2C3B4A59687ull, p.epoch);
  TEST_ASSERT_EQUAL_UINT32(41, p.seq);
  TEST_ASSERT_EQUAL_UINT32(3, p.boots);
  s.haveSeq = false;
  TEST_ASSERT_EQUAL_UINT32(0, nvslayout::playsFrom(s).seq);
  TEST_ASSERT_EQUAL_UINT32(3, nvslayout::playsFrom(s).boots);
  TEST_ASSERT_EQUAL_STRING("plays", nvslayout::kPlaysNamespace);
  const char* names[] = {nvslayout::kPlaysNamespace, nvslayout::kPlaysEpochKey, nvslayout::kPlaysSeqKey,
                         nvslayout::kPlaysBootsKey};
  for (const char* n : names) TEST_ASSERT_TRUE(std::strlen(n) <= 15);
  // A namespace of its own, never another's.
  TEST_ASSERT_TRUE(std::strcmp(nvslayout::kPlaysNamespace, nvslayout::kAutoDjNamespace) != 0);
  TEST_ASSERT_TRUE(std::strcmp(nvslayout::kPlaysNamespace, nvslayout::kUiNamespace) != 0);
  TEST_ASSERT_TRUE(std::strcmp(nvslayout::kPlaysNamespace, nvslayout::kMetaNamespace) != 0);
}

// The migration matrix: every unit this firmware can boot on, and what
// the boot keeps of each. The Opus work added the resume blob's kind 3
// under schema 1 (a blob's kinds are its own version's business, no
// bump), and the queue modes moved the schema to 2 for "queue"/"repeat";
// a unit may have run either build before this one, or 0.6.0, or 0.9.0
// (schema 2), or 0.10.0 (schema 3), or 0.11.0 (schema 4), or none; schema
// 3 added "ui"/"lang", schema 4 "autodj", schema 5 "plays". The schema's
// step, the repeat mode, the resume point, the UI's language, Auto DJ's
// settings and the play log's ids, as the boot reads them
// (NvsSchema::check(), QueueStore::loadRepeat() and loadResume(),
// app/UiLang, app/AutoDjStore, app/PlayHistory): the resume point survives
// whole on every row, the mode where a unit had one, every unit but a
// fresh one is in English with no language picker unless it chose a
// language, Auto DJ is off with its defaults unless a firmware that has it
// saved it, and the play log keeps its epoch where this firmware made one
// (every other unit draws one at its first boot here).
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
    const char* lang;  // "ui"/"lang" (nullptr: none)
    bool coachSeen;    // "ui"/"coach"
    A want;
    uint8_t wantRepeat;
    Lang wantLang;
    bool wantPicker;  // the first boot's language picker shows
    nvslayout::AutoDjStored dj;      // "autodj" (schema 4; none before it)
    nvslayout::AutoDjSaved wantDj;   // what the boot reads of it (app/AutoDjStore)
    nvslayout::PlaysStored plays = {};  // "plays" (schema 5; none before it)
    uint64_t wantEpoch = 0;             // the epoch the boot keeps (0: it draws one)
  };
  // This firmware's Auto DJ: on, Stay on seed, 70 %, Harmonic mixing.
  nvslayout::AutoDjStored stayOnSeed;
  stayOnSeed.haveOn = stayOnSeed.haveMode = stayOnSeed.haveFloor = stayOnSeed.haveMatch = true;
  stayOnSeed.on = 1;
  stayOnSeed.mode = 1;
  stayOnSeed.floor = 70;
  stayOnSeed.match = 2;
  nvslayout::AutoDjSaved stayOnSeedRead;
  stayOnSeedRead.on = true;
  stayOnSeedRead.mode = 1;
  stayOnSeedRead.floorPct = 70;
  stayOnSeedRead.matchKey = true;
  const nvslayout::AutoDjStored none;
  const nvslayout::AutoDjSaved off;
  // This firmware's play log, some plays in.
  nvslayout::PlaysStored logged;
  logged.haveEpoch = logged.haveSeq = logged.haveBoots = true;
  logged.epoch = 0x1122334455667788ull;
  logged.seq = 97;
  logged.boots = 12;
  const Unit units[] = {
      {"a fresh unit", false, 0, false, 0, Kind::None, nullptr, false, A::Write, 0, Lang::En, true, none, off},
      {"v0.5.0-beta.1 (no schema, the tips seen)", false, 0, false, 0, Kind::None, nullptr, true, A::Write, 0,
       Lang::En, false, none, off},
      {"v0.5.0 (schema 1, no anchor)", true, 1, false, 0, Kind::None, nullptr, true, A::Migrate, 0, Lang::En, false,
       none, off},
      {"0.6.0 (schema 1, an MP3 anchor)", true, 1, false, 0, Kind::Mp3, nullptr, true, A::Migrate, 0, Lang::En, false,
       none, off},
      {"0.6.0 (schema 1, a FLAC anchor, the tips never seen)", true, 1, false, 0, Kind::Flac, nullptr, false,
       A::Migrate, 0, Lang::En, false, none, off},
      {"the Opus build before the merge (schema 1, an Opus anchor)", true, 1, false, 0, Kind::Opus, nullptr, true,
       A::Migrate, 0, Lang::En, false, none, off},
      {"the queue modes build before the merge (schema 2, Repeat One)", true, 2, true, 2, Kind::Mp3, nullptr, true,
       A::Migrate, 2, Lang::En, false, none, off},
      {"0.9.0 (schema 2, Repeat All, an Opus anchor, the tips never seen)", true, 2, true, 1, Kind::Opus, nullptr,
       false, A::Migrate, 1, Lang::En, false, none, off},
      {"0.10.0, back from a downgrade to 0.9.0 (schema 3 kept, German)", true, 3, true, 1, Kind::Mp3, "de", true,
       A::Migrate, 1, Lang::De, false, none, off},
      {"0.10.0 (schema 3, Polish, Repeat One)", true, 3, true, 2, Kind::Flac, "pl", true, A::Migrate, 2, Lang::Pl,
       false, none, off},
      {"0.10.0, its picker left without a choice (the tips not seen yet)", true, 3, false, 0, Kind::None, nullptr,
       false, A::Migrate, 0, Lang::En, true, none, off},
      {"0.11.0 (schema 4, Auto DJ never touched)", true, 4, true, 1, Kind::Opus, "de", true, A::Migrate, 1, Lang::De,
       false, none, off},
      {"0.11.0 (schema 4, Auto DJ on in Stay on seed)", true, 4, true, 0, Kind::Mp3, "en", true, A::Migrate, 0,
       Lang::En, false, stayOnSeed, stayOnSeedRead},
      {"0.11.0, back from a downgrade to 0.10.0 (schema 4 kept, Auto DJ on)", true, 4, true, 2, Kind::Flac, "pl", true,
       A::Migrate, 2, Lang::Pl, false, stayOnSeed, stayOnSeedRead},
      {"0.11.0, its picker left without a choice (the tips not seen yet)", true, 4, false, 0, Kind::None, nullptr,
       false, A::Migrate, 0, Lang::En, true, none, off},
      {"this firmware (schema 5, a play log, Auto DJ on)", true, 5, true, 1, Kind::Opus, "de", true, A::None, 1,
       Lang::De, false, stayOnSeed, stayOnSeedRead, logged, 0x1122334455667788ull},
      {"this firmware, back from a downgrade to 0.11.0 (schema 5 kept, its play log too)", true, 5, true, 2,
       Kind::Flac, "pl", true, A::None, 2, Lang::Pl, false, none, off, logged, 0x1122334455667788ull},
      {"this firmware, its picker left without a choice (no play yet)", true, 5, false, 0, Kind::None, nullptr, false,
       A::None, 0, Lang::En, true, none, off},
  };
  for (const Unit& u : units) {
    const SchemaStep s = nvslayout::schemaStep(u.haveSchema, u.schema);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(u.want), static_cast<int>(s.action), u.ran);
    if (u.want == A::Migrate) TEST_ASSERT_EQUAL_UINT16_MESSAGE(u.schema, s.from, u.ran);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(u.wantRepeat, nvslayout::repeatFrom(u.haveRepeat, u.repeat), u.ran);
    // The language after the boot's step (NvsSchema's migrate()) and its
    // read (app/UiLang); the picker, as Ui::begin() asks.
    const char* lang = u.lang;
    if (u.want == A::Migrate && nvslayout::migrateWritesLang(s.from, lang != nullptr)) lang = "en";
    TEST_ASSERT_TRUE_MESSAGE(nvslayout::langFrom(lang != nullptr, lang) == u.wantLang, u.ran);
    TEST_ASSERT_EQUAL_MESSAGE(u.wantPicker, nvslayout::langPickerDue(lang != nullptr, u.coachSeen), u.ran);
    // Auto DJ (app/AutoDjStore): migrate(3, 4) writes nothing; absent is
    // off, with the defaults.
    const nvslayout::AutoDjSaved dj = nvslayout::autoDjFrom(u.dj);
    TEST_ASSERT_EQUAL_MESSAGE(u.wantDj.on, dj.on, u.ran);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(u.wantDj.mode, dj.mode, u.ran);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(u.wantDj.floorPct, dj.floorPct, u.ran);
    TEST_ASSERT_EQUAL_MESSAGE(u.wantDj.matchTempo, dj.matchTempo, u.ran);
    TEST_ASSERT_EQUAL_MESSAGE(u.wantDj.matchKey, dj.matchKey, u.ran);
    // The play log (app/PlayHistory): migrate(4, 5) writes nothing; no
    // epoch, the first boot draws one.
    const nvslayout::PlaysIds ids = nvslayout::playsFrom(u.plays);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(u.wantEpoch, ids.epoch, u.ran);
    if (u.wantEpoch) TEST_ASSERT_EQUAL_UINT32_MESSAGE(u.plays.seq, ids.seq, u.ran);
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
  // The downgrade to 0.9.0 (schema 2) from 0.10.0 or later: "newer, left
  // as it is", every key as 0.9.0 wrote it ("lang" never read: English).
  // To 0.10.0 (schema 3) from 0.11.0: the same, "autodj" never read (no
  // Auto DJ there). To 0.11.0 (schema 4) from this firmware: the same,
  // "plays" never read (no play log there); back here each is as it was.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(nvslayout::schemaStep(true, 3, 2).action));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(nvslayout::schemaStep(true, 4, 2).action));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(nvslayout::schemaStep(true, 4, 3).action));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(A::Newer), static_cast<int>(nvslayout::schemaStep(true, 5, 4).action));
  // The downgrades from schema 2 (perhaps an Opus anchor),
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
  RUN_TEST(test_the_schema_is_five);
  RUN_TEST(test_repeat_from_what_is_saved);
  RUN_TEST(test_language_from_what_is_saved);
  RUN_TEST(test_language_picker_and_its_migration);
  RUN_TEST(test_auto_dj_from_what_is_saved);
  RUN_TEST(test_the_play_logs_ids_from_what_is_saved);
  RUN_TEST(test_migration_matrix);
  return UNITY_END();
}
