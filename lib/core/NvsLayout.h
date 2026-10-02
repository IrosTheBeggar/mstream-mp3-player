// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "QueueSaver.h"  // QueueResume

// What the firmware keeps in NVS changes shape only through a schema
// number (docs/ARCHITECTURE.md, "NVS: the rules"): "meta"/"schema", a u16,
// written at boot when absent. A layout change (a key's type or meaning, a
// blob's fields) bumps kCurrent and adds a step to app/NvsSchema's
// migrate(from, to); a key name is never reused. Schema 1 is v0.5.0's
// layout: v0.5.0-beta.1's keys (the touch calibration, the Bluetooth
// pairing and bond, the settings, the queue's position and resume point),
// and a unit from beta.1 has no "schema" key and is taken as 1. One blob
// is versioned by its own byte, not by this number: the resume point,
// which on a schema-1 unit is beta.1's 20-byte form (version 0, until
// v0.5.0 first saves), version 1's 24 bytes (v0.5.0), or version 2's 64
// bytes (its resume anchor: docs/SEEK.md section 5.2). A blob that carries
// its own version may gain one without a schema bump as long as every
// older version is still read: a reader of schema 1, and any
// migrate(1, ...) step, takes all three.
// Pure (host-tested in test_nvs_layout); app/NvsSchema does the reads and
// writes.
namespace nvslayout {

inline constexpr const char* kMetaNamespace = "meta";
inline constexpr const char* kSchemaKey = "schema";
inline constexpr uint16_t kCurrent = 1;

// What the boot does about the stored number.
struct SchemaStep {
  enum class Action : uint8_t {
    None,     // it is kCurrent
    Write,    // absent (a fresh NVS, or v0.5.0-beta.1's): kCurrent written, nothing to migrate
    Migrate,  // older: migrate(from, kCurrent), then kCurrent written
    Newer,    // a newer firmware's (a downgrade): left as it is, nothing rewritten
  };
  Action action = Action::None;
  uint16_t from = kCurrent;
};
// `have`: the key exists; `stored`: its value.
SchemaStep schemaStep(bool have, uint16_t stored, uint16_t current = kCurrent);

// ---- the resume point's blob ("queue"/"resume") ----
// Version 2 (64 bytes, little-endian): a version byte (2), the anchor's
// kind (0 none, 1 MP3, 2 FLAC), flags (bit 0: exact), a 0; then version
// 1's five words (generation, entry, pathHash, positionMs, durationMs);
// then the anchor: the file's size, its rate, the sample (64 bits), the
// preroll byte, the frame byte, the skip, the frame hash (FLAC: the total
// samples' low 32 bits); 8 reserved zeros.
// Version 1 (v0.5.0): the version byte, three reserved zeros, the five
// words: 24 bytes. Version 0 (v0.5.0-beta.1): the five words with no
// version, 20 bytes. Both still read, with no anchor. Any other size or
// version: not read (no resume point, as with none saved).
inline constexpr uint8_t kResumeVersion = 2;
inline constexpr size_t kResumeV0Bytes = 20;
inline constexpr size_t kResumeV1Bytes = 24;
inline constexpr size_t kResumeBytes = 64;
inline constexpr size_t kResumeMaxBytes = kResumeBytes;

// Writes r (valid or not: the caller removes the key for an invalid one)
// as version kResumeVersion; returns kResumeBytes.
size_t encodeResume(const QueueResume& r, uint8_t out[kResumeBytes]);
// False (and *out untouched): not a blob this firmware reads.
bool decodeResume(const uint8_t* b, size_t n, QueueResume* out);

}  // namespace nvslayout
