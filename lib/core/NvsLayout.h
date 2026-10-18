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
// never reads "lang" and draws English, every other key as it wrote them.
// Schema 4 is schema 3's keys and the "autodj" namespace, Auto DJ's switch
// and settings (u8s: "on", "mode", "floor", "match"; the engine design's
// 2.13); its migrate(3, 4) step does nothing (absent keys read as the
// defaults: off, Follow the vibe, 55 %, BPM continuity and Harmonic mixing
// off), and older firmware leaves a schema-4 NVS as it is (it never reads
// the namespace: no Auto DJ there). Schema 5 is schema 4's keys and the
// "plays" namespace, the play log's ids (docs/METADATA.md 2.20.4: "epoch"
// a u64, "seq" and "boots" u32s); its migrate(4, 5) step does nothing (no
// epoch: the first boot draws one), and older firmware leaves a schema-5
// NVS as it is (it never reads the namespace: it keeps no log). A unit
// that ran the Opus work before it joined the queue modes (schema 1 with a
// kind-3 resume blob, no repeat key) takes the same steps and keeps its
// resume point: a blob's kinds are its own version's business
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
inline constexpr uint16_t kCurrent = 5;

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

// ---- Auto DJ ("autodj", schema 4) ----
// Four u8s, all four written at once whenever one changes (the console's
// D, the "..." row and Output > Auto DJ), each read with isKey() first:
//   on     1 on, 0 off (after a boot nothing plays until Play: the
//          restored queue waits, and Auto DJ adds at its last entry)
//   mode   0 Follow the vibe, 1 Stay on seed (autodj::Mode's values)
//   floor  Match strictness in percent: 55, 60, 65, 70 or 75
//   match  bit 0 BPM continuity, bit 1 Harmonic mixing (other bits ignored)
// An absent key, or a value this firmware doesn't take (a later one's),
// reads as its default: off, Follow the vibe, 55, both off (the user's D2).
inline constexpr const char* kAutoDjNamespace = "autodj";
inline constexpr const char* kAutoDjOnKey = "on";
inline constexpr const char* kAutoDjModeKey = "mode";
inline constexpr const char* kAutoDjFloorKey = "floor";
inline constexpr const char* kAutoDjMatchKey = "match";
inline constexpr uint8_t kAutoDjMatchTempo = 1;
inline constexpr uint8_t kAutoDjMatchKeys = 2;
inline constexpr uint8_t kAutoDjModes = 2;
inline constexpr uint8_t kAutoDjFloorMin = 55;
inline constexpr uint8_t kAutoDjFloorMax = 75;
inline constexpr uint8_t kAutoDjFloorStep = 5;
// What NVS holds: each key's presence and value.
struct AutoDjStored {
  bool haveOn = false, haveMode = false, haveFloor = false, haveMatch = false;
  uint8_t on = 0, mode = 0, floor = 0, match = 0;
};
// The settings to use.
struct AutoDjSaved {
  bool on = false;
  uint8_t mode = 0;
  uint8_t floorPct = kAutoDjFloorMin;
  bool matchTempo = false;
  bool matchKey = false;
};
AutoDjSaved autoDjFrom(const AutoDjStored& stored);
// The values to write: every key (have* all true), each a value
// autoDjFrom() reads back as `saved` (a strictness it doesn't take as 55).
AutoDjStored autoDjTo(const AutoDjSaved& saved);
// A strictness the settings take: 55-75 by 5.
bool autoDjFloorValid(uint8_t pct);

// ---- the play log ("plays", schema 5) ----
// docs/METADATA.md 2.20.4 (lib/core PlayLog's Journal::Ids): the device's
// epoch (a u64: a random one other than 0, drawn whenever NVS has none),
// the last seq the log used or reserved (a u32, written before the slot
// that uses it) and the boot count of that epoch (a u32, written at each
// boot before any slot uses it). A new epoch writes its seq and boot count
// (0) first and the epoch last. An NVS erase (the firmware's recovery, a
// flasher's install) loses all three, and the next boot takes a new epoch:
// that is why there is one (the plays' ids stay new to mStream).
inline constexpr const char* kPlaysNamespace = "plays";
inline constexpr const char* kPlaysEpochKey = "epoch";
inline constexpr const char* kPlaysSeqKey = "seq";
inline constexpr const char* kPlaysBootsKey = "boots";
struct PlaysStored {
  bool haveEpoch = false, haveSeq = false, haveBoots = false;
  uint64_t epoch = 0;
  uint32_t seq = 0, boots = 0;
};
struct PlaysIds {
  uint64_t epoch = 0;  // 0: none (a new one is drawn)
  uint32_t seq = 0;
  uint32_t boots = 0;
};
// The ids to use: none without an epoch (absent or 0), whatever seq and
// boot count a cut new epoch left; with one, its seq and boot count (each
// absent: 0).
PlaysIds playsFrom(const PlaysStored& stored);

// Writes r (valid or not: the caller removes the key for an invalid one)
// as version kResumeVersion; returns kResumeBytes.
size_t encodeResume(const QueueResume& r, uint8_t out[kResumeBytes]);
// False (and *out untouched): not a blob this firmware reads.
bool decodeResume(const uint8_t* b, size_t n, QueueResume* out);

}  // namespace nvslayout
