// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardFormat.h"

#include <cstring>

namespace cardformat {
namespace {

// A boot sector's OEM name (bytes 3-10).
bool oem(const uint8_t* s, const char* name) { return std::memcmp(s + 3, name, 8) == 0; }

bool signature(const uint8_t* s) { return s[510] == 0x55 && s[511] == 0xAA; }

// A FAT volume's own boot sector (no partition table): a jump, then "FAT"
// in FAT12/16's or FAT32's file system type. (Its boot code may hold any
// byte where an MBR's partition types would be.)
bool fatBootSector(const uint8_t* s) {
  return (s[0] == 0xEB || s[0] == 0xE9) && (std::memcmp(s + 54, "FAT", 3) == 0 || std::memcmp(s + 82, "FAT", 3) == 0);
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

constexpr uint32_t kPartitionTable = 446;  // four 16-byte entries
constexpr uint8_t kTypeExFatNtfs = 0x07;
constexpr uint8_t kTypeGptProtective = 0xEE;

}  // namespace

Kind classify(ReadSector read, void* ctx, uint8_t* buf) {
  if (!read(0, buf, ctx)) return Kind::Unreadable;
  // A volume with no partition table ("superfloppy"): its boot sector.
  if (oem(buf, "EXFAT   ")) return Kind::ExFat;
  if (oem(buf, "NTFS    ")) return Kind::Ntfs;
  if (!signature(buf) || fatBootSector(buf)) return Kind::Other;
  uint32_t lba07 = 0;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* e = buf + kPartitionTable + 16 * i;
    const uint8_t type = e[4];
    if (type == kTypeGptProtective) return Kind::Gpt;
    if (type == kTypeExFatNtfs && lba07 == 0) lba07 = le32(e + 8);
  }
  if (lba07 == 0 || !read(lba07, buf, ctx)) return Kind::Other;
  if (oem(buf, "EXFAT   ")) return Kind::ExFat;
  if (oem(buf, "NTFS    ")) return Kind::Ntfs;
  return Kind::Other;
}

const char* name(Kind k) {
  switch (k) {
    case Kind::Unreadable: return "unreadable";
    case Kind::ExFat: return "exFAT";
    case Kind::Ntfs: return "NTFS";
    case Kind::Gpt: return "GPT";
    case Kind::Other:
    default: return "not recognised";
  }
}

}  // namespace cardformat
