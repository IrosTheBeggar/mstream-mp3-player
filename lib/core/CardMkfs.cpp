// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardMkfs.h"

#include <cstddef>
#include <cstring>

namespace cardmkfs {
namespace {

// FatFs R0.15's own limits (ff.c): FAT16's most clusters, FAT32's.
constexpr uint32_t kMaxFat16 = 0xFFF5;
constexpr uint32_t kMaxFat32 = 0x0FFFFFF5;
// f_mkfs's FAT32 reserved sectors before the move that aligns the data area.
constexpr uint32_t kFat32Reserved = 32;

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
void put32(uint8_t* p, uint32_t v) {
  for (int k = 0; k < 4; ++k) p[k] = static_cast<uint8_t>(v >> (8 * k));
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// f_mkfs's FAT32 arithmetic for a volume of `vol` sectors at `base`, `pau`
// sectors a cluster, kFats FATs, the data area aligned to kAlignSectors
// (ff.c, f_mkfs(), "Create an FAT/FAT32 volume"). False where f_mkfs
// would refuse (FR_MKFS_ABORTED).
bool geometry(uint32_t base, uint32_t vol, uint32_t pau, Layout* l) {
  const uint32_t first = vol / pau;  // its first estimate, for the FAT's size
  if (first <= kMaxFat16 || first > kMaxFat32) return false;
  const uint32_t fat = (first * 4 + 8 + kSectorBytes - 1) / kSectorBytes;
  uint32_t reserved = kFat32Reserved;
  const uint32_t data = base + reserved + fat * kFats;
  const uint32_t move = ((data + kAlignSectors - 1) & ~(kAlignSectors - 1)) - data;
  reserved += move;
  if (static_cast<uint64_t>(vol) < static_cast<uint64_t>(data) + pau * 16 - base) return false;
  if (vol < reserved + fat * kFats) return false;
  const uint32_t clusters = (vol - reserved - fat * kFats) / pau;
  if (clusters <= kMaxFat16) return false;
  l->clusterSectors = pau;
  l->reserved = reserved;
  l->fatSectors = fat;
  l->dataStart = data + move;
  l->clusters = clusters;
  return true;
}

// A CHS address for a 255-head, 63-sector geometry (3 bytes: head,
// sector with the cylinder's top bits, cylinder); 1023/254/63 past it.
void chs(uint32_t lba, uint8_t* out) {
  constexpr uint32_t kHeads = 255, kTrack = 63;
  const uint32_t c = lba / (kHeads * kTrack);
  if (c > 1023) {
    out[0] = 254;
    out[1] = 0xFF;
    out[2] = 0xFF;
    return;
  }
  const uint32_t h = lba / kTrack % kHeads, s = lba % kTrack + 1;
  out[0] = static_cast<uint8_t>(h);
  out[1] = static_cast<uint8_t>(((c >> 2) & 0xC0) | s);
  out[2] = static_cast<uint8_t>(c);
}

}  // namespace

const char* detail(Why w) {
  switch (w) {
    case Why::Ok: return "ok";
    case Why::NoSize:
    case Why::TooSmall:
    case Why::TooBig: return "size";
    case Why::Card: return "card";
    case Why::Write: return "write";
    case Why::Mkfs: return "mkfs";
    case Why::Check: return "check";
    case Why::Files: return "files";
  }
  return "?";
}

const char* text(Why w) {
  switch (w) {
    case Why::Ok: return "formatted";
    case Why::NoSize: return "the card's size isn't known";
    case Why::TooSmall: return "the card is too small for FAT32";
    case Why::TooBig: return "the card is 2 TiB or more (FatFs's sectors are 32-bit)";
    case Why::Card: return "not the card that was confirmed, or no card answered";
    case Why::Write: return "the card refused a write";
    case Why::Mkfs: return "f_mkfs refused";
    case Why::Check: return "the card read back isn't what was written";
    case Why::Files: return "/music or /.player/device.txt couldn't be made";
  }
  return "?";
}

Layout plan(uint64_t cardSectors) {
  Layout l;
  if (cardSectors == 0) return l;  // NoSize
  if (cardSectors > 0xFFFFFFFFull) {
    l.why = Why::TooBig;
    return l;
  }
  l.cardSectors = static_cast<uint32_t>(cardSectors);
  l.partStart = kAlignSectors;
  l.why = Why::TooSmall;
  if (l.cardSectors <= l.partStart) return l;
  l.partSectors = l.cardSectors - l.partStart;
  // The largest cluster up to 32 KB that leaves FAT32 its clusters.
  for (uint32_t pau = kMaxClusterBytes / kSectorBytes; pau >= 1; pau /= 2) {
    if (geometry(l.partStart, l.partSectors, pau, &l)) {
      l.why = Why::Ok;
      break;
    }
  }
  if (!l.ok()) return l;
  // The tail: past the root directory's cluster (the first f_mkfs writes
  // in the data area); every card FAT32 takes has room for it.
  l.tail = l.cardSectors - kTailSectors >= l.dataStart + l.clusterSectors ? kTailSectors : 0;
  // prepare(): the head, the MBR, the gap, the tail; f_mkfs: the boot
  // sector, its backup (both commit()'s), FSINFO and its backup, the FATs,
  // the root directory's cluster, the MBR again (its partition's type).
  l.writeSectors = static_cast<uint64_t>(kHeadSectors) + 1 + (l.partStart - 1) + l.tail + 2 + 2 +
                   static_cast<uint64_t>(l.fatSectors) * kFats + l.clusterSectors + 1;
  return l;
}

Mkfs mkfsOptions(const Layout& l) {
  Mkfs m;
  m.clusterBytes = l.clusterBytes();
  return m;
}

void mbr(const Layout& l, uint32_t signature, uint8_t* out) {
  std::memset(out, 0, kSectorBytes);
  put32(out + 440, signature);
  uint8_t* e = out + 446;
  e[0] = 0x00;  // not active
  chs(l.partStart, e + 1);
  e[4] = kPartitionType;
  chs(l.partStart + l.partSectors - 1, e + 5);
  put32(e + 8, l.partStart);
  put32(e + 12, l.partSectors);
  out[510] = 0x55;
  out[511] = 0xAA;
}

// ---- Writer ----

uint8_t Writer::percent() const {
  if (l_.writeSectors == 0) return 0;
  const uint64_t p = written_ * 100 / l_.writeSectors;
  return static_cast<uint8_t>(p > 100 ? 100 : p);
}

int Writer::held(uint32_t lba) const {
  if (lba == l_.partStart) return 0;
  if (lba == l_.partStart + kBackupBoot) return 1;
  return -1;
}

bool Writer::put(uint32_t lba, const uint8_t* data, uint32_t count) {
  if (!card_.write(lba, data, count)) return false;
  written_ += count;
  if (fn_) fn_(written_, l_.writeSectors, ctx_);
  return true;
}

bool Writer::prepare(uint32_t signature, uint8_t* buf, uint32_t bufSectors) {
  if (!l_.ok() || !buf || bufSectors == 0) return false;
  std::memset(buf, 0, static_cast<size_t>(bufSectors) * kSectorBytes);
  // A run of zeros, a buffer at a time.
  auto zero = [this, buf, bufSectors](uint32_t lba, uint32_t n) {
    while (n) {
      const uint32_t k = n < bufSectors ? n : bufSectors;
      if (!put(lba, buf, k)) return false;
      lba += k;
      n -= k;
    }
    return true;
  };
  // The partition's head first: from here on nothing there is a boot sector.
  if (!zero(l_.partStart, kHeadSectors)) return false;
  uint8_t s[kSectorBytes];
  mbr(l_, signature, s);
  if (!put(0, s, 1)) return false;
  if (!zero(1, l_.partStart - 1)) return false;
  return l_.tail == 0 || zero(l_.cardSectors - l_.tail, l_.tail);
}

bool Writer::read(uint32_t lba, uint8_t* out, uint32_t count) {
  if (!card_.read(lba, out, count)) return false;
  if (committed_) return true;
  for (uint32_t k = 0; k < count; ++k) {
    const int h = held(lba + k);
    if (h >= 0 && have_[h]) std::memcpy(out + static_cast<size_t>(k) * kSectorBytes, hold_[h], kSectorBytes);
  }
  return true;
}

bool Writer::write(uint32_t lba, const uint8_t* data, uint32_t count) {
  if (committed_) return card_.write(lba, data, count);
  // The held sectors out of the run, the rest written as it comes.
  uint32_t from = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const int h = held(lba + k);
    if (h < 0) continue;
    if (k > from && !put(lba + from, data + static_cast<size_t>(from) * kSectorBytes, k - from)) return false;
    std::memcpy(hold_[h], data + static_cast<size_t>(k) * kSectorBytes, kSectorBytes);
    have_[h] = true;
    from = k + 1;
  }
  return count <= from || put(lba + from, data + static_cast<size_t>(from) * kSectorBytes, count - from);
}

bool Writer::commit() {
  if (committed_ || !have_[0] || !have_[1]) return false;
  committed_ = put(l_.partStart + kBackupBoot, hold_[1], 1) && put(l_.partStart, hold_[0], 1);
  return committed_;
}

// ---- check ----

Why check(SectorCache::Device& card, const Layout& l, uint8_t* s) {
  if (!l.ok()) return Why::Check;
  // The MBR: the one partition.
  if (!card.read(0, s, 1)) return Why::Card;
  const uint8_t* e = s + 446;
  if (s[510] != 0x55 || s[511] != 0xAA || e[4] != kPartitionType || le32(e + 8) != l.partStart ||
      le32(e + 12) != l.partSectors) {
    return Why::Check;
  }
  for (int i = 1; i < 4; ++i) {
    if (s[446 + 16 * i + 4] != 0) return Why::Check;
  }
  // The boot sector, then its backup the same bytes.
  if (!card.read(l.partStart, s, 1)) return Why::Card;
  if (s[0] != 0xEB || s[510] != 0x55 || s[511] != 0xAA || std::memcmp(s + 82, "FAT32   ", 8) != 0 ||
      le16(s + 11) != kSectorBytes || s[13] != l.clusterSectors || le16(s + 14) != l.reserved || s[16] != kFats ||
      le16(s + 17) != 0 || le16(s + 19) != 0 || le32(s + 28) != l.partStart || le32(s + 32) != l.partSectors ||
      le32(s + 36) != l.fatSectors || le32(s + 44) != 2 || le16(s + 48) != 1 || le16(s + 50) != kBackupBoot) {
    return Why::Check;
  }
  const uint32_t boot = crc32(s, kSectorBytes);
  if (!card.read(l.partStart + kBackupBoot, s, 1)) return Why::Card;
  if (crc32(s, kSectorBytes) != boot) return Why::Check;
  // FSINFO and its backup: the free count, a count (all but the root's).
  for (uint32_t copy = 0; copy < 2; ++copy) {
    if (!card.read(l.partStart + 1 + copy * kBackupBoot, s, 1)) return Why::Card;
    if (le32(s) != 0x41615252u || le32(s + 484) != 0x61417272u || le32(s + 488) != l.freeClusters() ||
        s[510] != 0x55 || s[511] != 0xAA) {
      return Why::Check;
    }
  }
  // Both FATs' first entries: the media byte's, the end mark's, the root
  // directory's chain of one.
  for (uint32_t f = 0; f < kFats; ++f) {
    if (!card.read(l.fatStart() + f * l.fatSectors, s, 1)) return Why::Card;
    if (le32(s) != 0xFFFFFFF8u || le32(s + 4) != 0xFFFFFFFFu || (le32(s + 8) & 0x0FFFFFFFu) != 0x0FFFFFFFu ||
        le32(s + 12) != 0) {
      return Why::Check;
    }
  }
  return Why::Ok;
}

Why finish(Files& files, const char* text, uint32_t n) {
  const bool ok = files.mount() && files.mkdir("/music") && files.mkdir("/.player") &&
                  files.write("/.player/device.txt", text, n);
  files.unmount();  // (a failed mount's too: Files::unmount())
  return ok ? Why::Ok : Why::Files;
}

Result run(Writer& w, uint32_t signature, uint8_t* buf, uint32_t bufSectors, MkfsFn mkfs, void* mkfsCtx, Files& files,
           const char* deviceTxt, uint32_t deviceTxtBytes) {
  Result r;
  if (!w.prepare(signature, buf, bufSectors)) {
    r.why = Why::Write;
    return r;
  }
  r.mkfs = mkfs(mkfsOptions(w.layout()), mkfsCtx);
  if (r.mkfs != 0) {
    r.why = Why::Mkfs;
    return r;
  }
  if (!w.commit()) {
    r.why = Why::Write;
    return r;
  }
  r.why = check(w.card(), w.layout(), buf);
  if (r.why != Why::Ok) return r;
  r.why = finish(files, deviceTxt, deviceTxtBytes);
  return r;
}

// ---- the request ----

Request request(const CachedDrive::Identity& id, Origin origin) {
  Request r;
  r.magic = kRequestMagic;
  r.version = kRequestVersion;
  r.origin = static_cast<uint8_t>(origin);
  r.identified = id.valid ? 1 : 0;
  r.sectors = id.sectors;
  if (id.valid) {
    r.bootLba = id.bootLba;
    r.sector0Crc = id.sector0Crc;
    r.bootCrc = id.bootCrc;
  }
  r.crc = crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(Request, crc));
  return r;
}

bool valid(const Request& r) {
  return r.magic == kRequestMagic && r.version == kRequestVersion &&
         (r.origin == static_cast<uint8_t>(Origin::Screen) || r.origin == static_cast<uint8_t>(Origin::Computer)) &&
         r.identified <= 1 && r.sectors != 0 &&
         r.crc == crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(Request, crc));
}

bool sameCard(const Request& r, const CachedDrive::Identity& now) {
  if (!valid(r) || now.sectors != r.sectors) return false;
  if (!r.identified) return !now.valid;
  return now.valid && now.bootLba == r.bootLba && now.sector0Crc == r.sector0Crc && now.bootCrc == r.bootCrc;
}

const char* originName(Origin o) {
  switch (o) {
    case Origin::Screen: return "the player's screen";
    case Origin::Computer: return "a computer's @format";
    case Origin::None: break;
  }
  return "?";
}

// ---- which card ----

const char* refusal(bool mounted, bool answered) { return mounted || !answered ? "card" : nullptr; }

}  // namespace cardmkfs
