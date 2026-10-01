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
// differs from beta.1's and is versioned by its own byte, not by this
// number: the resume point, which on a schema-1 unit is either beta.1's
// 20-byte form (until v0.5.0 first saves) or version 1's 24 bytes. A
// reader of schema 1, and any migrate(1, ...) step, takes both.
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
// Version 1 (since v0.5.0): a version byte, three reserved zeros, then
// generation, entry, pathHash, positionMs, durationMs as little-endian
// 32-bit words: 24 bytes. Version 0 (v0.5.0-beta.1): the same five words
// with no version, 20 bytes; still read. Any other size or version: not
// read (no resume point, as with none saved).
inline constexpr uint8_t kResumeVersion = 1;
inline constexpr size_t kResumeV0Bytes = 20;
inline constexpr size_t kResumeBytes = 24;
inline constexpr size_t kResumeMaxBytes = kResumeBytes;

// Writes r (valid or not: the caller removes the key for an invalid one)
// as version kResumeVersion; returns kResumeBytes.
size_t encodeResume(const QueueResume& r, uint8_t out[kResumeBytes]);
// False (and *out untouched): not a blob this firmware reads.
bool decodeResume(const uint8_t* b, size_t n, QueueResume* out);

}  // namespace nvslayout
