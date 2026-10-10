// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardJobs.h"
#include "TagStore.h"
#include "ff.h"

// The card through FatFs itself (docs/METADATA.md 3.2.3, 3.3.1, 3.3.7;
// milestone N10), not the VFS: what the metadata's portable code reads and
// writes through its interfaces.
//
//   FatFs   N4's file interface (tagstore::Fs): the device's records
//           (/.player/tags.bin, the journals), the transfer's root and tags
//           file (read only: the device never writes /.mstream), library.idx
//           and device.txt, with FIL.obj.sclust for 2.12.6's cut-rename rule.
//   FatCard N5's lister and N10's stat (cardjobs::Card) over /music: a
//           folder's entries through f_readdir, whose FILINFO carries each
//           file's size and FAT time in the same directory read (no stat per
//           file, which is what makes the POSIX walk slow); a file opened for
//           a qfp or a scan.
//
// Paths are the drive's ("0:/music/...", "0:/.player/tags.bin"): FatFs has
// no current directory here (FF_FS_RPATH 0), so every call names the drive
// the SD library mounted (its physical drive, set at the mount). FIL, DIR
// and FILINFO are in PSRAM: a FIL is about 4.1 KB in this build (FF_MAX_SS
// 4096, a sector buffer per file), too big for the card worker's 6 KB stack
// (3.3.1). FatFs's own long-name buffer (512 B) is on the caller's stack for
// each call (CONFIG_FATFS_LFN_STACK). FatFs takes the volume's lock for each
// call (FF_FS_REENTRANT): the decoder's reads of the playing file interleave
// with these, and reads are cut in 4 KB pieces so it never waits long.
//
// One task at a time per object: the card worker's jobs, or the loop
// between their steps (the boot, the update step).
namespace cardfat {

// The SD card's FatFs drive, once it mounted (LocalStorage::begin());
// 0xFF: none (the flash fallback, or no card). Nothing here works without.
void setDrive(uint8_t pdrv);
uint8_t drive();
bool ready();

// "<drive>:<path>" for an absolute path ("/.player/tags.bin"), and
// "<drive>:/music[/<rel>]" for a path relative to /music. False: it didn't fit.
bool fatPath(const char* path, char* out, size_t size);
bool musicPath(const char* rel, size_t len, char* out, size_t size);

// A file FatFs has open (PSRAM: its FIL is about 4.1 KB).
class FatFile final : public tagstore::File {
public:
  uint32_t size() const override;
  bool read(uint32_t offset, void* out, uint32_t n) override;
  bool write(uint32_t offset, const void* data, uint32_t n) override;
  bool sync() override;
  bool truncate(uint32_t size) override;
  // Its first cluster (FIL.obj.sclust; 0: empty).
  uint32_t firstCluster() const { return static_cast<uint32_t>(fil.obj.sclust); }
  FIL fil;
  bool writable = false;
};

class FatFs final : public tagstore::Fs {
public:
  tagstore::File* open(const char* path, Mode mode) override;
  bool close(tagstore::File* f) override;
  bool exists(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  uint32_t firstCluster(const char* path) override;
  // A file's size and FAT time (f_stat); false: missing.
  bool stat(const char* path, uint32_t* size, uint32_t* fatTime);
  // A folder, created if missing (f_mkdir); true if it is there.
  bool ensureDir(const char* path);
  // Files opened and closed since boot, and open now (the console's gs).
  uint32_t opened() const { return opened_; }
  uint32_t openNow() const { return openNow_; }

private:
  uint32_t opened_ = 0;
  uint32_t openNow_ = 0;
};

// /music, listed and read for the walk and the scan.
class FatCard final : public cardjobs::Card {
public:
  FatCard() = default;
  ~FatCard() override;
  // Its DIR, FILINFO and file (PSRAM, kept). False: no memory.
  bool begin();
  Open openDir(const char* rel, size_t len) override;
  Next next(cardwalk::Entry* out) override;
  void closeDir() override;
  cardcontract::Source* openFile(const char* rel, size_t len) override;
  void closeFile() override;
  bool stat(const char* rel, size_t len, uint32_t* size, uint32_t* fatTime) override;
  // Folders listed, entries read and files opened since boot (gs).
  uint32_t listings() const { return listings_; }
  uint32_t entries() const { return entries_; }
  uint32_t fileOpens() const { return fileOpens_; }

private:
  struct Work;
  Work* w_ = nullptr;
  bool dirOpen_ = false;
  bool fileOpen_ = false;
  uint32_t listings_ = 0, entries_ = 0, fileOpens_ = 0;
};

}  // namespace cardfat
