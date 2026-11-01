// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/CardFormatter.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <sd_diskio.h>

#include <cstdio>
#include <cstring>

#include "CardContract.h"
#include "HostStatus.h"
#include "app/AutoDjContract.h"
#include "app/Psram.h"
#include "app/Version.h"
#include "ff.h"
#include "diskio.h"       // disk_initialize()
#include "diskio_impl.h"  // ff_diskio_register(), ff_diskio_impl_t

// The SD library's FatFs driver (sd_diskio.cpp), as storage/SectorDisk
// declares it.
DSTATUS ff_sd_initialize(uint8_t pdrv);
DSTATUS ff_sd_status(uint8_t pdrv);
DRESULT ff_sd_read(uint8_t pdrv, uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_write(uint8_t pdrv, const uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_ioctl(uint8_t pdrv, uint8_t cmd, void* buff);

namespace cardformatter {
namespace {

using cardmkfs::Why;

// The request: raw words (an object with an initializer would be written
// at every boot; RTC slow memory is read and written a word at a time),
// read and cleared at the next one.
static_assert(sizeof(cardmkfs::Request) % 4 == 0, "the request is whole words");
constexpr size_t kRequestWords = sizeof(cardmkfs::Request) / 4;
RTC_NOINIT_ATTR uint32_t s_request[kRequestWords];

void storeRequest(const cardmkfs::Request& r) {
  uint32_t w[kRequestWords];
  std::memcpy(w, &r, sizeof(r));
  volatile uint32_t* rtc = s_request;
  for (size_t i = 0; i < kRequestWords; ++i) rtc[i] = w[i];
}

cardmkfs::Request loadRequest() {
  uint32_t w[kRequestWords];
  const volatile uint32_t* rtc = s_request;
  for (size_t i = 0; i < kRequestWords; ++i) w[i] = rtc[i];
  cardmkfs::Request r;
  std::memcpy(&r, w, sizeof(r));
  return r;
}

// f_mkfs's work area, prepare()'s zeros and check()'s sector: 64 KB, so
// the FATs go out 128 sectors a card command.
constexpr uint32_t kWorkBytes = 64 * 1024;

uint8_t s_pdrv = 0xFF;
cardmkfs::Writer* s_writer = nullptr;

// The SD driver's sectors, for the identity and under the writer.
class SdCard final : public SectorCache::Device {
public:
  bool read(uint32_t lba, uint8_t* out, uint32_t count) override {
    return ff_sd_read(s_pdrv, out, lba, count) == RES_OK;
  }
  bool write(uint32_t lba, const uint8_t* data, uint32_t count) override {
    return ff_sd_write(s_pdrv, data, lba, count) == RES_OK;
  }
};
SdCard s_card;

// FatFs's disk functions while the format runs: the SD driver's, its
// reads and writes through the writer.
DSTATUS fInit(unsigned char pdrv) { return ff_sd_initialize(pdrv); }
DSTATUS fStatus(unsigned char pdrv) { return ff_sd_status(pdrv); }
DRESULT fRead(unsigned char pdrv, unsigned char* buff, uint32_t sector, unsigned count) {
  (void)pdrv;
  return s_writer && s_writer->read(sector, buff, count) ? RES_OK : RES_ERROR;
}
DRESULT fWrite(unsigned char pdrv, const unsigned char* buff, uint32_t sector, unsigned count) {
  (void)pdrv;
  return s_writer && s_writer->write(sector, buff, count) ? RES_OK : RES_ERROR;
}
DRESULT fIoctl(unsigned char pdrv, unsigned char cmd, void* buff) { return ff_sd_ioctl(pdrv, cmd, buff); }
const ff_diskio_impl_t kImpl = {&fInit, &fStatus, &fRead, &fWrite, &fIoctl};

struct Work {
  uint8_t* buf = nullptr;
};

void drivePath(char* out, size_t size, const char* path) {
  snprintf(out, size, "%c:%s", static_cast<char>('0' + s_pdrv), path);
}

int mkfs(const cardmkfs::Mkfs& o, void* ctx) {
  Work* w = static_cast<Work*>(ctx);
  MKFS_PARM opt = {o.fmt, o.fats, o.align, o.rootEntries, o.clusterBytes};
  char drv[4];
  drivePath(drv, sizeof(drv), "");
  // The MBR's first partition (ESP-IDF's table: drive n's is 0, found
  // automatically, at every other time).
  VolToPart[s_pdrv].pt = 1;
  const FRESULT r = f_mkfs(drv, &opt, w->buf, kWorkBytes);
  VolToPart[s_pdrv].pt = 0;
  return r;
}

// The new volume's files through FatFs; its free count kept at the end.
class Files final : public cardmkfs::Files {
public:
  ~Files() override { release(); }
  bool mount() override {
    fs_ = static_cast<FATFS*>(psramAlloc(sizeof(FATFS)));
    fil_ = static_cast<FIL*>(psramAlloc(sizeof(FIL)));
    if (!fs_ || !fil_) return false;
    char drv[4];
    drivePath(drv, sizeof(drv), "");
    mounted_ = f_mount(fs_, drv, 1) == FR_OK;
    return mounted_;
  }
  bool mkdir(const char* path) override {
    char p[32];
    drivePath(p, sizeof(p), path);
    const FRESULT r = f_mkdir(p);
    return r == FR_OK || r == FR_EXIST;
  }
  bool write(const char* path, const void* data, uint32_t n) override {
    char p[40];
    drivePath(p, sizeof(p), path);
    if (f_open(fil_, p, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT put = 0;
    const bool ok = f_write(fil_, data, n, &put) == FR_OK && put == n;
    return f_close(fil_) == FR_OK && ok;
  }
  // (After a failed mount too: f_mount() registered fs_ before it failed,
  // and SD.begin()'s f_mount() would write through it once freed.)
  void unmount() override {
    if (mounted_ && hoststatus::freeCountValid(fs_->free_clst, fs_->n_fatent)) freeClusters = fs_->free_clst;
    char drv[4];
    drivePath(drv, sizeof(drv), "");
    f_mount(nullptr, drv, 0);
    mounted_ = false;
    release();
  }
  uint32_t freeClusters = 0xFFFFFFFF;  // FatFs's count once the files are made (FSINFO's at the next mount)

private:
  void release() {
    psramFree(fil_);
    psramFree(fs_);
    fil_ = nullptr;
    fs_ = nullptr;
  }
  FATFS* fs_ = nullptr;
  FIL* fil_ = nullptr;
  bool mounted_ = false;
};

struct Lines {
  Progress progress = nullptr;
  void* ctx = nullptr;
  uint8_t shown = 0;
  hoststatus::Progress host;
};

// After each write the card took: the screen's bar at each new percent,
// and @format's line at each tenth (or 5 s).
void onWrite(uint64_t written, uint64_t total, void* ctx) {
  Lines* l = static_cast<Lines*>(ctx);
  const uint64_t p = total ? written * 100 / total : 0;
  const uint8_t percent = static_cast<uint8_t>(p > 100 ? 100 : p);
  if (percent != l->shown) {
    l->shown = percent;
    if (l->progress) l->progress(percent, l->ctx);
  }
  if (percent < 100 && l->host.due(percent, millis())) Serial.printf("@format %u\n", static_cast<unsigned>(percent));
}

const char* originText(uint8_t o) { return cardmkfs::originName(static_cast<cardmkfs::Origin>(o)); }

}  // namespace

void ask(const CachedDrive::Identity& id, cardmkfs::Origin origin) { storeRequest(cardmkfs::request(id, origin)); }

Outcome runIfAsked(uint8_t cs, SPIClass& spi, uint32_t hz, Progress progress, void* ctx) {
  Outcome out;
  const cardmkfs::Request r = loadRequest();
  if (!cardmkfs::valid(r)) return out;
  storeRequest(cardmkfs::Request{});  // once: a cut or a crash doesn't format again
  out.ran = true;
  out.origin = static_cast<cardmkfs::Origin>(r.origin);
  const uint32_t t0 = millis();
  Serial.printf("[format] asked before the restart (on %s): the card of %lu sectors (%.1f GB)%s\n",
                originText(r.origin), static_cast<unsigned long>(r.sectors), r.sectors * 512.0 / 1e9,
                r.identified ? "" : ", its first sectors unreadable then");
  auto fail = [&out, t0](Why why, const char* what) {
    out.why = why;
    out.ms = millis() - t0;
    Serial.printf("@err 4 format %s\n", cardmkfs::detail(why));
    Serial.printf("[format] NOT FORMATTED (%s): %s\n", cardmkfs::text(why), what);
    return out;
  };
  s_pdrv = sdcard_init(cs, &spi, static_cast<int>(hz));
  if (s_pdrv == 0xFF) return fail(Why::Card, "the SD driver has no drive for it");
  uint8_t* buf = static_cast<uint8_t*>(psramAlloc(kWorkBytes));
  if (!buf) {
    sdcard_uninit(s_pdrv);
    return fail(Why::Card, "no PSRAM for its 64 KB");
  }
  auto release = [buf] {
    sdcard_uninit(s_pdrv);  // (its stock driver unregistered too)
    psramFree(buf);
    s_pdrv = 0xFF;
  };
  if (disk_initialize(s_pdrv) & STA_NOINIT) {
    release();
    return fail(Why::Card, "no card answered");
  }
  const uint32_t sectors = sdcard_num_sectors(s_pdrv);
  const CachedDrive::Identity id = CachedDrive::identityOf(s_card, sectors, buf);
  if (!cardmkfs::sameCard(r, id)) {
    release();
    char what[96];
    snprintf(what, sizeof(what), "another card is in (%lu sectors%s); nothing written", static_cast<unsigned long>(sectors),
             id.valid ? "" : ", unreadable");
    return fail(Why::Card, what);
  }
  const cardmkfs::Layout l = cardmkfs::plan(sectors);
  out.cardBytes = l.cardBytes();
  if (!l.ok()) {
    release();
    return fail(l.why, "nothing written");
  }
  Serial.printf("[format] FAT32 on an MBR: the partition from sector %lu, %lu sectors; clusters of %lu B (%lu); 2 FATs "
                "of %lu sectors; the data area from sector %lu; %llu sectors to write\n",
                static_cast<unsigned long>(l.partStart), static_cast<unsigned long>(l.partSectors),
                static_cast<unsigned long>(l.clusterBytes()), static_cast<unsigned long>(l.clusters),
                static_cast<unsigned long>(l.fatSectors), static_cast<unsigned long>(l.dataStart),
                static_cast<unsigned long long>(l.writeSectors));
  cardmkfs::Writer w(s_card, l);
  Lines lines;
  lines.progress = progress;
  lines.ctx = ctx;
  lines.host.start(millis());
  w.onWrite(onWrite, &lines);
  s_writer = &w;
  ff_diskio_register(s_pdrv, &kImpl);
  // device.txt as the boot's Library::writeDeviceTxt() writes it (which
  // then finds it the same).
  const char* v = version::player();
  if (v[0] == 'v') ++v;
  cardcontract::devicetxt::Info info = cardcontract::devicetxt::current(v);
  info.readMpdj = autoDjReadMpdj();
  char text[400];
  const size_t n = cardcontract::devicetxt::format(info, text, sizeof(text));
  Work work;
  work.buf = buf;
  Files files;
  const cardmkfs::Result res =
      cardmkfs::run(w, esp_random() | 1u, buf, kWorkBytes / cardmkfs::kSectorBytes, mkfs, &work, files, text,
                    static_cast<uint32_t>(n));
  s_writer = nullptr;
  release();
  out.why = res.why;
  out.mkfs = res.mkfs;
  out.clusterBytes = l.clusterBytes();
  if (res.why != Why::Ok) {
    char mkfsCode[24] = "";
    if (res.why == Why::Mkfs) snprintf(mkfsCode, sizeof(mkfsCode), ", f_mkfs's FRESULT %d", res.mkfs);
    char what[128];
    // (Stopped before its last write, the boot sector: once the MBR is
    // written the card holds no FAT volume, "can't read this card", and
    // its Format.)
    snprintf(what, sizeof(what), "%llu of %llu sectors written%s%s", static_cast<unsigned long long>(w.written()),
             static_cast<unsigned long long>(l.writeSectors), mkfsCode,
             w.committed() ? "" : "; the boot sector not written: format it again");
    return fail(res.why, what);
  }
  const uint32_t freeClusters = files.freeClusters != 0xFFFFFFFF ? files.freeClusters : l.freeClusters();
  out.freeBytes = static_cast<uint64_t>(freeClusters) * l.clusterBytes();
  out.ms = millis() - t0;
  if (lines.host.due(100, millis())) Serial.printf("@format 100\n");
  Serial.printf("@format done size=%llu free=%llu cluster=%lu\n", static_cast<unsigned long long>(out.cardBytes),
                static_cast<unsigned long long>(out.freeBytes), static_cast<unsigned long>(out.clusterBytes));
  Serial.printf("[format] done in %.1f s: FAT32, %lu clusters free (%llu B), /music and /.player/device.txt; FSINFO's "
                "count valid; the card mounts as another card now\n",
                out.ms / 1000.0f, static_cast<unsigned long>(freeClusters),
                static_cast<unsigned long long>(out.freeBytes));
  return out;
}

}  // namespace cardformatter
