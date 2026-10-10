// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/CardFat.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "app/Psram.h"

namespace cardfat {

namespace {

uint8_t s_drive = 0xFF;

constexpr uint32_t kPiece = 4096;  // a card access at most per FatFs read (the decoder shares the volume's lock)

}  // namespace

void setDrive(uint8_t pdrv) { s_drive = pdrv; }
uint8_t drive() { return s_drive; }
bool ready() { return s_drive != 0xFF; }

bool fatPath(const char* path, char* out, size_t size) {
  if (!ready() || !path || path[0] != '/') return false;
  const int n = std::snprintf(out, size, "%u:%s", static_cast<unsigned>(s_drive), path);
  return n > 0 && static_cast<size_t>(n) < size;
}

bool musicPath(const char* rel, size_t len, char* out, size_t size) {
  if (!ready()) return false;
  const int n = len ? std::snprintf(out, size, "%u:/music/%.*s", static_cast<unsigned>(s_drive), static_cast<int>(len), rel)
                    : std::snprintf(out, size, "%u:/music", static_cast<unsigned>(s_drive));
  return n > 0 && static_cast<size_t>(n) < size;
}

// ---- FatFile ----

uint32_t FatFile::size() const { return static_cast<uint32_t>(f_size(&fil)); }

bool FatFile::read(uint32_t offset, void* out, uint32_t n) {
  const uint32_t sz = size();
  if (offset > sz || n > sz - offset) return false;
  if (static_cast<uint32_t>(f_tell(&fil)) != offset && f_lseek(&fil, offset) != FR_OK) return false;
  auto* p = static_cast<uint8_t*>(out);
  while (n) {
    const UINT want = n < kPiece ? n : kPiece;
    UINT got = 0;
    if (f_read(&fil, p, want, &got) != FR_OK || got != want) return false;
    p += got;
    n -= got;
    if (n) taskYIELD();
  }
  return true;
}

bool FatFile::write(uint32_t offset, const void* data, uint32_t n) {
  if (!writable) return false;
  // Past the end: f_lseek in a writable file extends it (2.4's writers fill
  // every byte they skip).
  if (static_cast<uint32_t>(f_tell(&fil)) != offset && f_lseek(&fil, offset) != FR_OK) return false;
  if (static_cast<uint32_t>(f_tell(&fil)) != offset) return false;  // the card is full
  // Pieces of at most 4 KB that end on the file's 4 KB boundaries
  // (tagstore::writePiece()): after the first, each is whole sectors from a
  // sector's start, which FatFs writes straight, 8 at a time. Cut at 4 KB
  // from wherever the write began, every piece of library.idx's save (its
  // header first, then 4 KB blocks) started mid-sector: a single-sector
  // write at each end, read first (857 card writes, 421 of one sector, for
  // a 20k index; aligned 424 and 25: test_card_io).
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t at = offset;
  while (n) {
    const UINT want = tagstore::writePiece(at, n);
    UINT put = 0;
    if (f_write(&fil, p, want, &put) != FR_OK || put != want) return false;
    p += put;
    at += put;
    n -= put;
    if (n) taskYIELD();
  }
  return true;
}

bool FatFile::sync() { return writable && f_sync(&fil) == FR_OK; }

bool FatFile::truncate(uint32_t sz) {
  if (!writable || sz > size()) return false;
  return f_lseek(&fil, sz) == FR_OK && f_truncate(&fil) == FR_OK;
}

// ---- FatFs ----

tagstore::File* FatFs::open(const char* path, Mode mode) {
  char p[300];
  if (!fatPath(path, p, sizeof(p))) return nullptr;
  FatFile* f = psramNew<FatFile>();
  if (!f) return nullptr;
  BYTE how = FA_READ | FA_OPEN_EXISTING;
  if (mode == Mode::Create) how = FA_READ | FA_WRITE | FA_CREATE_ALWAYS;
  if (mode == Mode::Update) how = FA_READ | FA_WRITE | FA_OPEN_EXISTING;
  if (f_open(&f->fil, p, how) != FR_OK) {
    psramDelete(f);
    return nullptr;
  }
  f->writable = mode != Mode::Read;
  ++opened_;
  ++openNow_;
  return f;
}

bool FatFs::close(tagstore::File* file) {
  if (!file) return false;
  auto* f = static_cast<FatFile*>(file);
  const bool ok = f_close(&f->fil) == FR_OK;  // a written file is synced first
  psramDelete(f);
  if (openNow_) --openNow_;
  return ok;
}

bool FatFs::exists(const char* path) {
  uint32_t size, time;
  return stat(path, &size, &time);
}

bool FatFs::stat(const char* path, uint32_t* size, uint32_t* fatTime) {
  char p[300];
  if (!fatPath(path, p, sizeof(p))) return false;
  FILINFO* fi = psramNew<FILINFO>();
  if (!fi) return false;
  const bool ok = f_stat(p, fi) == FR_OK;
  if (ok) {
    *size = static_cast<uint32_t>(fi->fsize);
    *fatTime = static_cast<uint32_t>(fi->fdate) << 16 | fi->ftime;
  }
  psramDelete(fi);
  return ok;
}

bool FatFs::remove(const char* path) {
  char p[300];
  return fatPath(path, p, sizeof(p)) && f_unlink(p) == FR_OK;
}

bool FatFs::rename(const char* from, const char* to) {
  char a[300], b[300];
  // f_rename refuses an existing `to` (FR_EXIST), as the interface asks.
  return fatPath(from, a, sizeof(a)) && fatPath(to, b, sizeof(b)) && f_rename(a, b) == FR_OK;
}

uint32_t FatFs::firstCluster(const char* path) {
  tagstore::File* f = open(path, Mode::Read);
  if (!f) return 0;
  const uint32_t c = static_cast<FatFile*>(f)->firstCluster();
  close(f);
  return c;
}

bool FatFs::ensureDir(const char* path) {
  char p[300];
  if (!fatPath(path, p, sizeof(p))) return false;
  const FRESULT r = f_mkdir(p);
  return r == FR_OK || r == FR_EXIST;
}

// ---- FatCard ----

struct FatCard::Work {
  FF_DIR dir;
  FILINFO info;
  FatFile file;
  char path[300];
};

FatCard::~FatCard() {
  if (!w_) return;
  closeDir();
  closeFile();
  psramDelete(w_);
}

bool FatCard::begin() {
  if (!w_) w_ = psramNew<Work>();
  return w_ != nullptr;
}

FatCard::Open FatCard::openDir(const char* rel, size_t len) {
  if (!w_ || !musicPath(rel, len, w_->path, sizeof(w_->path))) return Open::Error;
  if (dirOpen_) closeDir();
  const FRESULT r = f_opendir(&w_->dir, w_->path);
  ++listings_;
  if (r == FR_NO_PATH || r == FR_NO_FILE) return Open::Missing;
  if (r != FR_OK) return Open::Error;
  dirOpen_ = true;
  return Open::Ok;
}

FatCard::Next FatCard::next(cardwalk::Entry* out) {
  if (!w_ || !dirOpen_) return Next::Error;
  for (;;) {
    if (f_readdir(&w_->dir, &w_->info) != FR_OK) return Next::Error;
    const char* name = w_->info.fname;
    if (name[0] == 0) return Next::End;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    ++entries_;
    out->name = name;
    out->nameLength = std::strlen(name);
    out->folder = (w_->info.fattrib & AM_DIR) != 0;
    out->size = static_cast<uint32_t>(w_->info.fsize);
    out->fatTime = static_cast<uint32_t>(w_->info.fdate) << 16 | w_->info.ftime;
    return Next::Entry;
  }
}

void FatCard::closeDir() {
  if (!w_ || !dirOpen_) return;
  f_closedir(&w_->dir);
  dirOpen_ = false;
}

cardcontract::Source* FatCard::openFile(const char* rel, size_t len) {
  if (!w_ || !musicPath(rel, len, w_->path, sizeof(w_->path))) return nullptr;
  if (fileOpen_) closeFile();
  if (f_open(&w_->file.fil, w_->path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return nullptr;
  w_->file.writable = false;
  fileOpen_ = true;
  ++fileOpens_;
  return &w_->file;
}

void FatCard::closeFile() {
  if (!w_ || !fileOpen_) return;
  f_close(&w_->file.fil);
  fileOpen_ = false;
}

bool FatCard::stat(const char* rel, size_t len, uint32_t* size, uint32_t* fatTime) {
  if (!w_ || !musicPath(rel, len, w_->path, sizeof(w_->path))) return false;
  if (f_stat(w_->path, &w_->info) != FR_OK || (w_->info.fattrib & AM_DIR)) return false;
  *size = static_cast<uint32_t>(w_->info.fsize);
  *fatTime = static_cast<uint32_t>(w_->info.fdate) << 16 | w_->info.ftime;
  return true;
}

}  // namespace cardfat
