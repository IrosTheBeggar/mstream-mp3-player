// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CachedDrive.h"
#include "SectorCache.h"

// A card formatted in the player (docs/HOST-STATUS.md "@format";
// docs/METADATA.md 2.2, "A card the player formats"): FAT32 on an MBR, as
// the device reads it and no more, so the transfer software can fill it at
// once. The listener asks for it on the player's screen (a not-FAT32
// card's empty state, or a computer's @format, which only brings up the
// question), confirms it twice, and the player restarts and formats the
// card at the boot, before anything mounts it (storage/CardFormatter). The
// parts with no hardware in them, host-tested on ChaN's FatFs R0.15 as the
// Core2 builds it (test_card_mkfs, test/support/FatModel.h):
//
//   - plan(): the layout for a card of n sectors. One primary partition of
//     type 0x0C (FAT32, LBA) from sector kAlignSectors (4 MiB, an SD card's
//     allocation unit) to the card's last sector; FAT32 with 2 FATs, as
//     every computer's formatter writes it; 32 KB clusters on every card
//     from 2.16 GB up (every card sold as 4 GB or more), and on a smaller
//     one the largest that still makes the 65,526 clusters FAT32 needs;
//     the data area from a 4 MiB boundary, so each cluster sits inside one
//     of the card's erase blocks.
//     It is f_mkfs's own arithmetic for those options (FatFs R0.15, FAT32
//     in an existing partition): its reserved sectors (32, and the FATs
//     moved on to put the data area on the boundary), the FATs' size, the
//     clusters, and FSINFO's free count (all but the root directory's).
//     Refused: no size, a card too small for FAT32 (under 41.9 MB), or one
//     of 2 TiB or more (FatFs's 32-bit sector numbers).
//   - mbr(): sector 0, that partition and a disk signature.
//   - Writer: the card as f_mkfs sees it (the platform's disk functions go
//     through it while f_mkfs runs). prepare() writes what comes first:
//     the partition's first sectors zeroed (an old boot sector there would
//     make a format cut short look like a FAT volume), the MBR, the gap
//     before the partition and the card's last sectors zeroed (a GPT's
//     tables, an old volume's boot sector: nothing a computer could take
//     for another scheme is left); f_mkfs's writes pass, counted, but for
//     the boot sector and its backup, which are held back; commit() writes
//     them last, the backup first: cut anywhere before it, the partition
//     holds no FAT volume and the card reads as "can't read" (format it
//     again), never as a FAT whose FAT is half the old card's.
//   - check(): the card read back: the MBR, the boot sector and its
//     backup, FSINFO and its backup (a valid free count: the FSINFO rule,
//     docs/HOST-STATUS.md), the first entries of both FATs, as planned.
//   - finish(): /music and /.player made, /.player/device.txt written (the
//     firmware's: the software can write Opus and Auto DJ to it before its
//     first boot in the player), through the platform's file calls.
//   - run(): those in order, f_mkfs between (the platform's: FatFs's drive
//     with its partition set to 1, so it formats the MBR's first one).
//   - Request: the confirmed card's identity (CachedDrive's: its size, the
//     CRCs of sector 0 and of its first partition's first sector), carried
//     across the restart; the boot formats only that card.
//   - refusal(): which card is formatted at all: one the player doesn't
//     read, never one it does.
namespace cardmkfs {

constexpr uint32_t kSectorBytes = 512;
// The partition's first sector and the data area's boundary: 4 MiB, the
// allocation unit of SDHC and SDXC cards (the SD Association's formatter's
// boundary too).
constexpr uint32_t kAlignSectors = 8192;
constexpr uint32_t kMaxClusterBytes = 32768;
// FAT32's fewest clusters (FatFs: more than MAX_FAT16, 65,525) and most
// (MAX_FAT32).
constexpr uint32_t kMinClusters = 65526;
constexpr uint32_t kMaxClusters = 0x0FFFFFF5;
constexpr uint8_t kFats = 2;
constexpr uint8_t kPartitionType = 0x0C;  // FAT32 with LBA
// The partition's first sectors zeroed before anything else: the boot
// sector, FSINFO and their backups (6 and 7).
constexpr uint32_t kHeadSectors = 8;
constexpr uint32_t kBackupBoot = 6;
// The card's last sectors zeroed: a GPT's backup header and table (33).
constexpr uint32_t kTailSectors = 64;

enum class Why : uint8_t {
  Ok,
  NoSize,    // the card's size isn't known
  TooSmall,  // under FAT32's 65,526 clusters of 512 B after the 4 MiB before the partition (41.9 MB)
  TooBig,    // 2^32 sectors or more (2 TiB)
  Card,      // no card answered, or not the card the listener confirmed
  Write,     // a write the card refused
  Mkfs,      // f_mkfs refused (Result::mkfs: its FRESULT)
  Check,     // the card read back isn't what was written (or a read failed)
  Files,     // /music, /.player or device.txt couldn't be made
};
// The word @format's refusal and failure lines carry (docs/HOST-STATUS.md):
// "size" (NoSize, TooSmall, TooBig), "card", "write", "mkfs", "check",
// "files"; "ok".
const char* detail(Why w);
// For the log: "the card is too small for FAT32", ...
const char* text(Why w);

struct Layout {
  Why why = Why::NoSize;
  uint32_t cardSectors = 0;
  uint32_t partStart = 0;    // kAlignSectors
  uint32_t partSectors = 0;  // to the card's last sector
  uint32_t clusterSectors = 0;
  // What f_mkfs makes of it (FatFs R0.15's arithmetic):
  uint32_t reserved = 0;    // the partition's reserved sectors (BPB_RsvdSecCnt)
  uint32_t fatSectors = 0;  // one FAT's
  uint32_t dataStart = 0;   // cluster 2's first sector, on the card (a multiple of kAlignSectors)
  uint32_t clusters = 0;
  uint32_t tail = 0;        // the last sectors prepare() zeroes (kTailSectors, or 0 on a card too small for them)
  // Every sector the format writes, all told (its percent): prepare()'s,
  // f_mkfs's, commit()'s.
  uint64_t writeSectors = 0;

  bool ok() const { return why == Why::Ok; }
  uint32_t clusterBytes() const { return clusterSectors * kSectorBytes; }
  uint32_t fatStart() const { return partStart + reserved; }
  // FSINFO's free count: every cluster but the root directory's.
  uint32_t freeClusters() const { return clusters ? clusters - 1 : 0; }
  uint64_t freeBytes() const { return static_cast<uint64_t>(freeClusters()) * clusterBytes(); }
  uint64_t cardBytes() const { return static_cast<uint64_t>(cardSectors) * kSectorBytes; }
};

// The layout for a card of `cardSectors` (its CSD's count).
Layout plan(uint64_t cardSectors);

// f_mkfs's options (FatFs's MKFS_PARM, as plain numbers): FM_FAT32 (0x02),
// the FATs, the data area's alignment in sectors, no fixed root directory,
// the cluster size in bytes.
struct Mkfs {
  uint8_t fmt = 0x02;
  uint8_t fats = kFats;
  uint32_t align = kAlignSectors;
  uint32_t rootEntries = 0;
  uint32_t clusterBytes = 0;
};
Mkfs mkfsOptions(const Layout& l);

// Sector 0 (512 bytes into `out`): no boot code, `signature` at 440 (the
// disk signature computers name the disk by), one partition: not active,
// type 0x0C, from partStart for partSectors, its CHS fields for a 255-head,
// 63-sector geometry (1023/254/63 past it, as every partitioner writes for
// LBA); 0x55AA.
void mbr(const Layout& l, uint32_t signature, uint8_t* out);

class Writer final : public SectorCache::Device {
public:
  // After each write the card took: the sectors written so far, of the
  // plan's writeSectors.
  using OnWrite = void (*)(uint64_t written, uint64_t total, void* ctx);

  Writer(SectorCache::Device& card, const Layout& l) : card_(card), l_(l) {}
  void onWrite(OnWrite fn, void* ctx) {
    fn_ = fn;
    ctx_ = ctx;
  }
  // Before f_mkfs: the partition's first kHeadSectors zeroed, the MBR
  // (`signature`), the gap before the partition and the layout's tail
  // zeroed. `buf`: `bufSectors` sectors of the caller's (f_mkfs's work
  // area, not in use yet), zero-filled here. False: a write failed.
  bool prepare(uint32_t signature, uint8_t* buf, uint32_t bufSectors);
  // f_mkfs's reads and writes: to the card, the writes counted; the boot
  // sector (partStart) and its backup (partStart + kBackupBoot) held back
  // (read back from what is held). After commit(), the card itself (the
  // files finish() makes aren't the format's to count).
  bool read(uint32_t lba, uint8_t* out, uint32_t count) override;
  bool write(uint32_t lba, const uint8_t* data, uint32_t count) override;
  // After f_mkfs: the backup, then the boot sector, the last write. False:
  // f_mkfs didn't write them, or the card refused one.
  bool commit();
  bool committed() const { return committed_; }
  uint64_t written() const { return written_; }
  // 0-100: written() of the plan's writeSectors.
  uint8_t percent() const;
  const Layout& layout() const { return l_; }
  // The card itself (check() reads it there, not what is held).
  SectorCache::Device& card() { return card_; }

private:
  bool put(uint32_t lba, const uint8_t* data, uint32_t count);  // to the card, counted
  int held(uint32_t lba) const;                                   // 0 the boot sector, 1 its backup, -1 neither

  SectorCache::Device& card_;
  Layout l_;
  OnWrite fn_ = nullptr;
  void* ctx_ = nullptr;
  uint8_t hold_[2][kSectorBytes] = {};
  bool have_[2] = {false, false};
  bool committed_ = false;
  uint64_t written_ = 0;
};

// The card read back as `l` plans it (`sector`: 512 bytes of the
// caller's). Ok, Check (not as planned), or Card (a read failed).
Why check(SectorCache::Device& card, const Layout& l, uint8_t* sector);

// The new volume's files, through FatFs (the platform's: the device's
// storage/CardFormatter, test_card_mkfs's model).
class Files {
public:
  virtual ~Files() = default;
  virtual bool mount() = 0;
  // Made, or there already.
  virtual bool mkdir(const char* path) = 0;
  // A new file, whole and synced.
  virtual bool write(const char* path, const void* data, uint32_t n) = 0;
  // After every mount(), a failed one too: FatFs keeps the object a failed
  // f_mount() registered, and the next f_mount() on the drive (the boot's
  // SD.begin()) would write through it once it is freed.
  virtual void unmount() = 0;
};
// After check(): mounted, /music and /.player made, /.player/device.txt
// (`text`, `n` bytes: cardcontract::devicetxt's) written, let go (also
// when the mount failed). Ok or Files.
Why finish(Files& files, const char* text, uint32_t n);

// f_mkfs on the card's drive with these options, its partition set to 1:
// FatFs's FRESULT (0: FR_OK).
using MkfsFn = int (*)(const Mkfs& opt, void* ctx);
struct Result {
  Why why = Why::Ok;
  int mkfs = 0;  // f_mkfs's FRESULT (Why::Mkfs)
};
// The whole format, `w` in front of the card's drive: prepare(), f_mkfs,
// commit(), check(), finish(). `buf`, `bufSectors`: prepare()'s zeros and
// check()'s sector (f_mkfs's work area is the platform's own).
Result run(Writer& w, uint32_t signature, uint8_t* buf, uint32_t bufSectors, MkfsFn mkfs, void* mkfsCtx, Files& files,
           const char* deviceTxt, uint32_t deviceTxtBytes);

// ---- the request, across the restart ----
enum class Origin : uint8_t { None = 0, Screen = 1, Computer = 2 };
// The confirmed card, kept in memory a restart doesn't clear (the
// device's RTC slow memory: esp_restart() keeps it, a power cut doesn't;
// its contents after a power-on are anything, hence the magic and CRC).
struct Request {
  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t origin = 0;      // Origin
  uint8_t identified = 0;  // the card's sectors were read (its CRCs below)
  uint8_t reserved = 0;
  uint32_t sectors = 0;
  uint32_t bootLba = 0;
  uint32_t sector0Crc = 0;
  uint32_t bootCrc = 0;
  uint32_t crc = 0;  // CRC-32 of the bytes before it
};
constexpr uint32_t kRequestMagic = 0x544D464Du;  // "MFMT"
constexpr uint8_t kRequestVersion = 1;
// For the card whose identity is `id` (an unreadable one: its size alone).
Request request(const CachedDrive::Identity& id, Origin origin);
bool valid(const Request& r);
// The card in the slot now (`now`, read at the boot) is the one confirmed:
// the same size and, when its sectors were read then, the same CRCs; one
// unreadable then must still be.
bool sameCard(const Request& r, const CachedDrive::Identity& now);
const char* originName(Origin o);  // "the player's screen", "a computer's @format"

// ---- which card ----
// Whether the card in the slot may be formatted, whoever asks (the
// question shown, its Format taken): one that answered and that the
// player doesn't read (exFAT, NTFS, a GPT, blank, a FAT that didn't
// mount, its first sector unreadable). Never one it reads (`mounted`:
// card=fat32, fat16), whatever is on it: the software sets such a card up
// as it is (docs/METADATA.md 2.2), and a computer's @format can't put the
// question over the music card. `answered`: a card answered at the boot
// or the last Try again. nullptr: it may; else why not, @err 4 format's
// word ("card").
const char* refusal(bool mounted, bool answered);

}  // namespace cardmkfs
