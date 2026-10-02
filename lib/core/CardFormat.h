// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// What a microSD card that didn't mount is, from its first sectors: the
// player reads FAT32 (and FAT16) on an MBR card only (the framework's FatFs
// is built without exFAT and without GPT). A card that isn't FAT32 is the
// usual reason a card that is in won't mount: anything over 32 GB comes
// exFAT from the factory, and a card formatted on a Mac or by some tools
// gets a GPT. LocalStorage reads the sectors when SD.begin() fails; the
// empty state then says "This card isn't FAT32" instead of "No microSD
// card" (docs/ARCHITECTURE.md, Storage). Pure: the reads are handed in.
namespace cardformat {

constexpr uint32_t kSectorBytes = 512;

enum class Kind : uint8_t {
  Unreadable,  // no card, or sector 0 couldn't be read: "No microSD card"
  Other,       // readable, nothing recognised (FAT that didn't mount, blank): "No microSD card"
  ExFat,       // an exFAT boot sector, at LBA 0 or in an MBR partition of type 0x07
  Ntfs,        // an NTFS one in a type 0x07 partition (exFAT's type too)
  Gpt,         // a GPT: sector 0 is the protective MBR (a partition of type 0xEE)
};

// Reads one 512-byte sector; false when it can't.
using ReadSector = bool (*)(uint32_t lba, uint8_t* out, void* ctx);

// Sector 0, then (an MBR with a type 0x07 partition) that partition's
// first sector. `buf`: 512 bytes of the caller's.
Kind classify(ReadSector read, void* ctx, uint8_t* buf);

// The card is in but isn't FAT32: say so, not "No microSD card".
constexpr bool notFat32(Kind k) { return k == Kind::ExFat || k == Kind::Ntfs || k == Kind::Gpt; }
// For the log: "exFAT", "NTFS", "GPT", "unreadable", "not recognised".
const char* name(Kind k);

}  // namespace cardformat
