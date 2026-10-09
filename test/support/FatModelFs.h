// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// tagstore::Fs over the host FatFs model (FatModel.h), as the firmware's
// src/storage/CardFat is over the card's FatFs: a read is an f_lseek when
// the file isn't at its offset, then 4 KB pieces; a write the same, its
// pieces ending on the file's 4 KB boundaries (tagstore::writePiece()); a
// close of a written file syncs it. So what the store's and the update
// step's writers ask of the card here is what they ask of it on the device
// (test_card_io counts it). Include FatModel.h first, in one translation
// unit. Host only.
#include <cstdint>
#include <cstdio>
#include <string>

#include "FatModel.h"
#include "TagStore.h"

namespace fatmodel {

// What the files asked of FatFs.
struct FileCounts {
  uint64_t syncs = 0;      // f_sync, and a written file's f_close
  uint64_t seeks = 0;      // f_lseek before a read or a write
  uint64_t backSeeks = 0;  // ... to an earlier offset (FatFs walks the chain from the start)
  uint64_t opens = 0;
};

class ModelFile final : public tagstore::File {
public:
  ModelFile(FileCounts& counts) : counts_(counts) {}
  uint32_t size() const override { return static_cast<uint32_t>(f_size(&fil)); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    const uint32_t sz = size();
    if (offset > sz || n > sz - offset) return false;
    if (!seek(offset)) return false;
    auto* p = static_cast<uint8_t*>(out);
    while (n) {
      const UINT want = n < kPiece ? n : kPiece;
      UINT got = 0;
      if (f_read(&fil, p, want, &got) != FR_OK || got != want) return false;
      p += got;
      n -= got;
    }
    return true;
  }
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (!writable) return false;
    if (!seek(offset) || static_cast<uint32_t>(f_tell(&fil)) != offset) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t at = offset;
    while (n) {
      const UINT want = alignPieces ? tagstore::writePiece(at, n) : (n < kPiece ? n : kPiece);
      UINT put = 0;
      if (f_write(&fil, p, want, &put) != FR_OK || put != want) return false;
      p += put;
      at += put;
      n -= put;
    }
    return true;
  }
  bool sync() override {
    ++counts_.syncs;
    return writable && f_sync(&fil) == FR_OK;
  }
  bool truncate(uint32_t sz) override {
    if (!writable || sz > size()) return false;
    return f_lseek(&fil, sz) == FR_OK && f_truncate(&fil) == FR_OK;
  }
  static constexpr uint32_t kPiece = 4096;  // CardFat's
  FIL fil;
  bool writable = false;
  bool alignPieces = true;  // false: 4 KB pieces from wherever the write began (CardFat before 2026-10-09)

private:
  bool seek(uint32_t offset) {
    const uint32_t at = static_cast<uint32_t>(f_tell(&fil));
    if (at == offset) return true;
    ++counts_.seeks;
    if (offset < at) ++counts_.backSeeks;
    return f_lseek(&fil, offset) == FR_OK;
  }
  FileCounts& counts_;
};

class ModelFs final : public tagstore::Fs {
public:
  explicit ModelFs(BYTE pdrv = 0) : pdrv_(pdrv) {}
  tagstore::File* open(const char* path, Mode mode) override {
    auto* f = new ModelFile(counts);
    f->alignPieces = alignPieces;
    BYTE how = FA_READ | FA_OPEN_EXISTING;
    if (mode == Mode::Create) how = FA_READ | FA_WRITE | FA_CREATE_ALWAYS;
    if (mode == Mode::Update) how = FA_READ | FA_WRITE | FA_OPEN_EXISTING;
    if (f_open(&f->fil, at(path).c_str(), how) != FR_OK) {
      delete f;
      return nullptr;
    }
    ++counts.opens;
    f->writable = mode != Mode::Read;
    return f;
  }
  bool close(tagstore::File* file) override {
    if (!file) return false;
    auto* f = static_cast<ModelFile*>(file);
    if (f->writable) ++counts.syncs;  // f_close syncs a written file
    const bool ok = f_close(&f->fil) == FR_OK;
    delete f;
    return ok;
  }
  bool exists(const char* path) override {
    FILINFO fi;
    return f_stat(at(path).c_str(), &fi) == FR_OK;
  }
  bool remove(const char* path) override { return f_unlink(at(path).c_str()) == FR_OK; }
  bool rename(const char* from, const char* to) override {
    return f_rename(at(from).c_str(), at(to).c_str()) == FR_OK;
  }
  uint32_t firstCluster(const char* path) override {
    FIL f;
    if (f_open(&f, at(path).c_str(), FA_READ) != FR_OK) return 0;
    const uint32_t c = static_cast<uint32_t>(f.obj.sclust);
    f_close(&f);
    return c;
  }
  FileCounts counts;
  bool alignPieces = true;

private:
  std::string at(const char* p) const { return fatmodel::path(pdrv_, p[0] == '/' ? p + 1 : p); }
  BYTE pdrv_;
};

}  // namespace fatmodel
