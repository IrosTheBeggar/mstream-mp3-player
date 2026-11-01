// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <SPI.h>

#include <cstdint>

#include "CachedDrive.h"
#include "CardMkfs.h"

// The format in the player (docs/HOST-STATUS.md "@format"; lib/core
// CardMkfs has the layout and the steps, host-tested): the listener
// confirmed it twice on the player's screen, and the player restarts
// (main.cpp: paused, the queue and the play log saved, the headphones let
// go), so that nothing has the card when it is formatted: the next boot
// formats it first thing, before LocalStorage mounts it, with the boot
// screen showing how far it is.
//
//   - ask(): the card's identity (CachedDrive's: its size, the CRCs of
//     sector 0 and of its first partition's first sector) and who asked, in
//     RTC slow memory: esp_restart() keeps it, a power cut doesn't (so a
//     card is never formatted by a later power-on), and a value there after
//     a power-on fails its magic or CRC.
//   - runIfAsked(), at the boot before LocalStorage::begin(): the request
//     taken and cleared first (a crash or a cut mid-format doesn't format
//     again at the next boot), the card initialised through the SD driver
//     (sdcard_init(), disk_initialize()), its identity read and held to the
//     request's (another card, or none: nothing is written), then
//     cardmkfs::run() with the SD driver's disk functions put behind its
//     Writer (ff_diskio_register(), as storage/SectorDisk does after a
//     mount), f_mkfs with the drive's partition set to 1 in ESP-IDF's
//     VolToPart[] (FF_MULTI_PARTITION 1: it formats the MBR's first
//     partition), its work area 64 KB of PSRAM; /music and
//     /.player/device.txt through FatFs (its FATFS and FIL in PSRAM); the
//     driver let go (sdcard_uninit()). The boot goes on to mount the card,
//     a new one to the card's guard.
//
// Its lines (docs/HOST-STATUS.md): "@format <percent>" as each tenth is
// reached (and at least every 5 s), "@format 100", then "@format done
// size=<bytes> free=<bytes> cluster=<bytes>", or "@err 4 format <why>"
// (card, size, write, mkfs, check, files); the log's [format] lines.
// Loop task (setup()), with nothing else on the card or its SPI bus but
// the boot screen's drawing.
namespace cardformatter {

// The listener confirmed: the card `id` is formatted at the next boot.
void ask(const CachedDrive::Identity& id, cardmkfs::Origin origin);

struct Outcome {
  bool ran = false;  // a request was there at this boot
  cardmkfs::Origin origin = cardmkfs::Origin::None;
  cardmkfs::Why why = cardmkfs::Why::Ok;
  int mkfs = 0;  // f_mkfs's FRESULT (why Mkfs)
  uint64_t cardBytes = 0;
  uint64_t freeBytes = 0;
  uint32_t clusterBytes = 0;
  uint32_t ms = 0;
  bool ok() const { return ran && why == cardmkfs::Why::Ok; }
};

// Each new percent (0-100), as it is reached: the boot screen's bar.
using Progress = void (*)(uint8_t percent, void* ctx);

// The card on `cs` and `spi` (begun), at `hz`, formatted if a request is
// there and it is that card.
Outcome runIfAsked(uint8_t cs, SPIClass& spi, uint32_t hz, Progress progress, void* ctx);

}  // namespace cardformatter
