// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "QueueSaver.h"  // QueueResume
#include "UiStringIds.h"  // uistr::Lang

// What the firmware keeps in NVS changes shape only through a schema
// number (docs/ARCHITECTURE.md, "NVS: the rules"): "meta"/"schema", a u16,
// written at boot when absent. A layout change (a key's type or meaning, a
// blob's fields) bumps kCurrent and adds a step to app/NvsSchema's
// migrate(from, to); a key name is never reused. Schema 1 is v0.5.0's
// layout: v0.5.0-beta.1's keys (the touch calibration, the Bluetooth
// pairing and bond, the settings, the queue's position and resume point),
// and a unit from beta.1 has no "schema" key and is taken as 1. Schema 2
// is schema 1's keys and "queue"/"repeat" (the repeat mode, a u8:
// docs/QUEUE-MODES.md section 3.6); its migrate(1, 2) step does nothing
// (an absent key reads as Off), and older firmware leaves a schema-2 NVS
// as it is (never reading the key: its queue wraps, as it always did).
// Schema 3 is schema 2's keys and "ui"/"lang", the UI's language (a BCP-47
// tag: docs/I18N.md, phase 3); its migrate(2, 3) step writes "en" there
// when it is absent, so a unit that ran an older firmware keeps English
// and never gets the first boot's language picker, which only a fresh NVS
// (no "schema") shows. Older firmware leaves a schema-3 NVS as it is: it
// never reads "lang" and draws English, every other key as it wrote them. A
// unit that ran the Opus work before it joined the queue modes (schema 1
// with a kind-3 resume blob, no repeat key) takes the same step and keeps
// its resume point: a blob's kinds are its own version's business
// (test_nvs_layout's migration matrix has every such unit). One blob
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
inline constexpr uint16_t kCurrent = 3;

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
// kind (0 none, 1 MP3, 2 FLAC, 3 Opus), flags (bit 0: exact), a 0; then
// version 1's five words (generation, entry, pathHash, positionMs,
// durationMs); then the anchor: the file's size, its rate, the sample (64
// bits), the preroll byte, the frame byte, the skip, the frame hash (FLAC:
// the total samples' low 32 bits; Opus: the exact trimmed length's, the
// byte fields 0: docs/OPUS.md); 8 reserved zeros. A kind this firmware
// doesn't know (a later format's anchor) keeps the five words and drops the
// anchor: the point resumes by its second, which is what every anchor falls
// back to anyway, instead of being lost. (0.6.0, which knew the kinds up to
// FLAC, refused the whole blob on a kind above them: a downgrade to it from
// a pause saved on an Opus track loses the resume point once, the
// documented pattern of SEEK.md section 5.2.)
// Version 1 (v0.5.0): the version byte, three reserved zeros, the five
// words: 24 bytes. Version 0 (v0.5.0-beta.1): the five words with no
// version, 20 bytes. Both still read, with no anchor. Any other size or
// version: not read (no resume point, as with none saved).
inline constexpr uint8_t kResumeVersion = 2;
inline constexpr size_t kResumeV0Bytes = 20;
inline constexpr size_t kResumeV1Bytes = 24;
inline constexpr size_t kResumeBytes = 64;
inline constexpr size_t kResumeMaxBytes = kResumeBytes;

// ---- the repeat mode ("queue"/"repeat", schema 2) ----
// A u8: 0 Off, 1 All, 2 One (PlaybackController::Repeat's values),
// written at once on a change.
inline constexpr const char* kRepeatKey = "repeat";
inline constexpr uint8_t kRepeatModes = 3;
// The mode to use: `stored` if the key is there (`have`) and known; absent
// or another value: 0 (Off, the saved default).
uint8_t repeatFrom(bool have, uint8_t stored);

// ---- the UI's language ("ui"/"lang", schema 3) ----
// A string: the language's BCP-47 tag ("de", "pt": uistr::tag()), written
// at once when the listener picks one (Output > Language, or the first
// boot's picker). "ui"/"coach" (a bool, since schema 1) says the first-boot
// tips were seen.
inline constexpr const char* kUiNamespace = "ui";
inline constexpr const char* kLangKey = "lang";
inline constexpr const char* kCoachKey = "coach";
// A tag's room, its NUL included ("en-XA" is 5).
inline constexpr size_t kLangMaxBytes = 16;
// The language to use: `stored`'s when the key is there (`have`) and names
// one of the languages; absent, or a tag this firmware hasn't (a later
// firmware's language, after a downgrade): English.
uistr::Lang langFrom(bool have, const char* stored);
// Whether the first boot's picker shows: no language saved and the
// first-boot tips not seen. A fresh unit only: an update from an older
// firmware has "en" from migrate(2, 3) (and has mostly seen the tips).
bool langPickerDue(bool haveLang, bool coachSeen);
// Whether migrate(from, kCurrent) writes "en" to "ui"/"lang": from a
// schema before 3, when there is none (`haveLang`: a schema-3 firmware
// was there before, and a downgrade left its choice).
bool migrateWritesLang(uint16_t from, bool haveLang);

// Writes r (valid or not: the caller removes the key for an invalid one)
// as version kResumeVersion; returns kResumeBytes.
size_t encodeResume(const QueueResume& r, uint8_t out[kResumeBytes]);
// False (and *out untouched): not a blob this firmware reads.
bool decodeResume(const uint8_t* b, size_t n, QueueResume* out);

}  // namespace nvslayout
