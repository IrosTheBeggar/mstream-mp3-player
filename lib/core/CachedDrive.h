// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "SectorCache.h"

// The diskio wrapper's rules over SectorCache (docs/METADATA.md 3.2.4, 3.8;
// milestone N10 and its review): what FatFs's disk calls do to the cache,
// and the device batch's switches. The firmware's storage/SectorDisk
// adapts it to ESP-IDF's ff_diskio_impl_t over the SD driver; the host's
// FatFs model (test/support/FatModel.h) runs ChaN's FatFs on it.
// Portable, host-tested (test_sector_cache, test_fat_model).
//
//   - init() (disk_initialize): FatFs mounts the volume again. It does at
//     the mount, and by itself whenever disk_status() says STA_NOINIT: the
//     card stopped answering (pulled, or swapped, while the player was on:
//     the Core2 has no card-detect), so the next FatFs call finds a card
//     that may not be the one the cache read. The cache is cleared:
//     nothing of the card that was there is kept, not even its boot sector.
//   - read(): single sectors through the cache when it is on, every read to
//     the card when it is off; with verify, each hit is read from the card
//     again and compared (a difference is counted, its LBA kept, the
//     card's bytes returned and the sector dropped).
//   - write(): through the cache when it is on; when it is off, straight to
//     the card, and the sectors it wrote are dropped from the cache. So the
//     cache never holds a byte the card doesn't, on or off, and turning it
//     on again can't serve a sector written while it was off.
//   - trim() (CTRL_TRIM: FatFs frees clusters): the range dropped.
//   - setEnabled(), setVerify() and resetStats() come from another task
//     (the console): each is taken at the next disk call, under FatFs's
//     volume lock, which serialises the cache (it has no lock of its own).
//     A call reads the switch before it takes the pending requests, and
//     setEnabled(true) stores its clear before the switch, so a call that
//     sees the cache on has seen the clear asked with it. The cache starts
//     empty when it is turned on again (a cold start for L0's figures).
//   - A write the card refused is written again once (sdbusy::kWriteTries:
//     the same sectors and bytes, so a write the card did take is only
//     written twice), on or off, before the cache hears of a failure.
//   - The card's identity (2026-10-09, the device run): clearing the cache
//     at a remount kept the old card's sectors off the new one, but not
//     the player's own state (the index, the store's journal offsets, the
//     queue), which FatFs would have let it write there. remember() keeps
//     the mount's card (its size, the CRC-32 of sector 0 and of its FAT
//     volume's boot sector); remounted(), after FatFs's disk_initialize
//     found a card answering, compares the card in the slot. Another card,
//     or one whose identity can't be read: foreign, until a restart, and
//     write() refuses every write (the firmware's wrapper also answers
//     STA_PROTECT, so FatFs refuses them before it gets this far).
//   - A read or a write that fails, in a call whose status the card answered: a look at
//     the card there and then (lookAfterFault(), docs/METADATA.md 3.8). A read the look
//     keeps (the same card, back at its init) is asked once more in that call (afterRead()):
//     a one-off heals, whoever read, and FatFs never hears of it; a second failure is
//     FatFs's error, as a kept one was. Not a write: its two tries and its rules stand.
//   - A read that keeps leaving the card silent past its init isn't asked of it again for
//     a while (readStopped()), whoever asks: every reader's own count starts over at a
//     remount, or at the next track, and each such read makes a remount.
//   - FatFs's window, when the card won't take it (windowFailed()).
//
// Every read and write that reaches the card is counted (on or off), with
// the reads' time by the clock given (none on the host: 0).
class CachedDrive {
public:
  struct Stats {
    uint32_t cardReads = 0;        // reads that reached the card (the cache on or off)
    uint32_t cardSingleReads = 0;  // ... of one sector (FatFs's folders and FAT, a partial sector)
    uint32_t cardReadSectors = 0;
    uint64_t cardReadUs = 0;  // their time (the clock's; 0 without one)
    uint32_t cardWrites = 0;
    uint32_t writeRetries = 0;  // writes the card refused, written again
    uint32_t writeFails = 0;    // ... refused the second time too
    uint32_t refused = 0;       // writes refused here: another card (foreign()), or writesOff()
    uint32_t trims = 0;
    uint32_t inits = 0;     // mounts: the cache cleared
    uint32_t verified = 0;  // hits compared with the card (verify on)
    uint32_t stale = 0;     // ... that differed: a cache bug (L1 fails)
    uint32_t staleLba = 0;  // the last such sector
    uint32_t faultLba = 0;  // the first sector of the last read or write that failed
    uint32_t stoppedReads = 0;  // reads not asked of the card (readStopped())
  };
  // A card's identity: what tells two cards apart without FatFs. A FAT32
  // format writes a volume serial from the clock into the boot sector, and
  // Windows a disk signature into sector 0; the size is the card's CSD's.
  // Two cards alike in all three (a sector-for-sector clone) are the same
  // card to this. A size of 0 is the driver's CMD9 failing, which says nothing
  // of the card: the two sectors tell.
  struct Identity {
    bool valid = false;     // both sectors read
    uint32_t sectors = 0;   // the card's size (the SD driver's)
    uint32_t bootLba = 0;   // the first partition's first sector (0: none; sector 0 is the volume's own)
    uint32_t sector0Crc = 0;
    uint32_t bootCrc = 0;   // bootLba's (0 without one)
    bool sameCard(const Identity& o) const {
      return valid && o.valid && (sectors == o.sectors || !sectors || !o.sectors) && bootLba == o.bootLba &&
             sector0Crc == o.sector0Crc && bootCrc == o.bootCrc;
    }
  };
  using ClockFn = uint64_t (*)();  // microseconds
  // The SD driver's word at a fault (the firmware's adapter). None: no fault is looked
  // into: FatFs mounts the volume again by itself, as it does at a pull.
  class Link {
  public:
    virtual ~Link() = default;
    // CMD13, now. True: R1 0 and the driver holds no mark: that read's own fault.
    virtual bool answers() = 0;
    // The driver's init, now. True: a card answers, or nothing marked it failed.
    virtual bool init(uint32_t* sectors) = 0;
  };

  // `cache` (begin()'d or not: with no block every call goes to the card)
  // over `card`, both outliving this.
  CachedDrive(SectorCache& cache, SectorCache::Device& card, ClockFn clock = nullptr);
  CachedDrive(const CachedDrive&) = delete;
  CachedDrive& operator=(const CachedDrive&) = delete;

  // Verify's sector (512 B, the caller's: PSRAM on the device), which the
  // identity's reads use too; nullptr: verify does nothing, and no
  // identity can be read.
  void setScratch(uint8_t* sector) { check_ = sector; }
  void setLink(Link* link) { link_ = link; }

  // ---- FatFs's calls, under its volume's lock ----
  void init();
  bool read(uint32_t lba, uint8_t* out, uint32_t count);
  // False when the card refused it twice, or the card is foreign, or writesOff().
  bool write(uint32_t lba, const uint8_t* data, uint32_t count);
  // FatFs's CTRL_TRIM range: the first and the last sector freed.
  void trim(uint32_t first, uint32_t last);
  // A look at a fault found a card it can't call the mount's: the wrapper's status says
  // STA_NOINIT, and FatFs's remount gives the verdict (init() clears it).
  bool remountDue() const { return remountDue_; }
  // A read asked again only after this, once it has left the card silent kStopAfter times
  // with no read of it working between (a file's first sector takes as many at the boot:
  // TagStore::openAtBoot(), Auto DJ's tries).
  static constexpr uint32_t kStopAfter = 4;
  static constexpr uint64_t kStopUs = 5ull * 60ull * 1000000ull;
  // A fault's look found the card silent this many times, whatever the sectors: past this
  // the readers a session can do without stop until a restart (tooSilent(), docs/METADATA.md 3.8):
  // the stop can't bound reads that move on, a sector a track or a file. As three sectors' worth.
  static constexpr uint32_t kTooSilentAfter = 3 * kStopAfter;
  // FatFs's own window, its dirty sector of FAT or of a directory, couldn't be written
  // (only the wrapper can tell: the bytes are FATFS::win). FatFs writes it first at every
  // later move of the window and reads nothing until it goes, so one sector that won't
  // take a write fails every open there is, for good. The first time it stands: FatFs's
  // next move writes it again, and a one-off keeps its update. The same sector again, with
  // no write of it taken: a remount drops the window (remountDue()), as any remount does,
  // and what was open with it: a file left open could go on to write a directory entry
  // over clusters the FAT doesn't have. The second such remount, and no more writes until
  // a restart, so no more of them either: STA_PROTECT, as for another card. Reading goes on.
  void windowFailed(uint32_t lba);
  bool writesOff() const { return writesOff_.load(); }

  // ---- the card's identity ----
  // The card's identity now, read from the card into the scratch
  // (`sectors`: its size, the SD driver's). Invalid when a read fails, or
  // without a scratch.
  Identity readIdentity(uint32_t sectors);
  // At the mount (the firmware's install(), before any other task reads
  // the card): this card's identity is the one remounts are held to. The
  // guard is armed from here on, even when the identity can't be read
  // (then no remount can prove its card the same). False: unreadable.
  bool remember(uint32_t sectors);
  // FatFs mounted the volume again and the SD driver's init found a card
  // answering (under the volume's lock): the card's identity read and
  // compared with remember()'s. Not the same card, or either unreadable:
  // foreign from now on. Not armed (nothing remembered): nothing checked.
  // True: the card is the mount's (or nothing is checked). With no look since the last
  // read the card answered that found it silent, it's a card put back (the first anyone
  // saw of it was a status): the stopped reads are asked again, and the counts start over.
  bool remounted(uint32_t sectors);
  // The driver's init found no card answering, at FatFs's mount (remounted() comes at a later one).
  void mountFailed();
  // FatFs's last mount found no card answering (mountFailed()), and no mount has found one since
  // (remounted(), armed or not): the card is out, or silent past its init. A read failed with the
  // card answering, or kept by the look, says nothing here: only a mount does. Any task.
  bool cardOut() const { return out_.load(); }
  // The mount's card isn't in the slot: none answering (cardOut()), or another card answering
  // (foreign(), until the restart the guard makes for it). Any task: the player's card hold asks it
  // of a track that failed to start (PlaybackController::Card), so another card's missing files
  // aren't each skipped in the moments before that restart.
  bool mountCardOut() const { return out_.load() || foreign_.load(); }
  // Any task.
  bool armed() const { return armed_.load(); }
  bool foreign() const { return foreign_.load(); }
  uint32_t remounts() const { return remounts_.load(); }
  // The card came back: remounts kCardOutUs or more after a mount with no card answering, for the
  // queue's save, whose failures build up a wait (QueueSaver::cardBack()); a pull shorter than this
  // has built up its first wait at most. Not any remount: a save's own, or a window's, would set up
  // the next try at once, and that one the next. A card silent this long, or a save's own retry
  // that long after its failed mount, can be one: the saver takes one for a run of failures, so that
  // ends there. Never reset.
  static constexpr uint64_t kCardOutUs = 10ull * 1000000ull;
  uint32_t cardBacks() const { return cardBacks_.load(); }
  // remember()'s and the last remounted()'s (a diagnostic, as stats()).
  const Identity& mountIdentity() const { return mountId_; }
  const Identity& lastIdentity() const { return lastId_; }

  // ---- any task ----
  void setEnabled(bool on);
  bool enabled() const { return enabled_.load(); }
  void setVerify(bool on) { verify_.store(on); }
  bool verifying() const { return verify_.load(); }
  // The counts (the cache's too) start again at the next disk call.
  void resetStats() { reset_.store(true); }
  // A copy, read while FatFs may be inside a call on another task: each
  // field a word, read as it is (a diagnostic).
  Stats stats() const { return stats_; }
  // Commands with no answer from a card that was back at once, the same card:
  // FatFs told nothing but that read's error (lookAfterFault()). Never reset.
  // A lasting fault of a read counts two, its own and the second try's.
  uint32_t kept() const { return kept_.load(); }
  // ... of them reads that the second try read (afterRead()): one-offs FatFs never heard
  // of. Never reset.
  uint32_t healed() const { return healed_.load(); }
  // kTooSilentAfter silent faults since the boot or the last card put back. Stays, until a restart.
  bool tooSilent() const { return tooSilent_.load(); }

private:
  // The card, counted (and timed).
  class Card final : public SectorCache::Device {
  public:
    explicit Card(CachedDrive& d) : d_(d) {}
    bool read(uint32_t lba, uint8_t* out, uint32_t count) override;
    bool write(uint32_t lba, const uint8_t* data, uint32_t count) override;

  private:
    CachedDrive& d_;
  };
  // The requests from other tasks (the stats' reset, the clear).
  void settle();
  // What a look found (lookAfterFault()). Error: FatFs's error and nothing more (the card
  // answered: that read's own fault; no look; another card, the remount's to judge). Kept:
  // the same card, back at its init: no remount, and a read is asked once more. Silent: a
  // new silence: the stop's and too silent's count.
  enum class Look : uint8_t { Error, Kept, Silent };
  // A read or a write failed, in a FatFs call whose status the card answered a moment
  // ago: no card can have been out and written elsewhere since. No answer left the
  // driver's mark, which would have FatFs's next call mount the volume, every file open
  // on it gone: the driver's init now, the identity, and FatFs hears only that read's
  // error. A card out, or not the mount's: FatFs's remount and the guard, as ever. (The
  // one reader outside FatFs, the free count, asks the status first: CardSpace.)
  // Silent: nothing answered, or an identity the look couldn't read: FatFs's next call that
  // finds a card answering makes a remount of this fault. (A look that finds the card back
  // with one noted, FatFs having gone on with the mark standing, ends it: no remount comes of
  // it and nothing was cut, so it counts for nothing.) Foreign: the count alone.
  Look lookAfterFault(uint32_t lba);
  // Silent, or Error when one is noted already (one silence, one count).
  Look noteSilence();
  // The reads that left the card silent: the same first sector and count, kStopAfter times
  // with no read of it working between, and no reader asks the card again until kStopUs
  // after the last (with no clock, until a write). Only these: each costs a remount, which
  // cuts whatever plays, and no reader's count stops them: Auto DJ's and the waveform bar's
  // start over at a remount or a track. A read that fails with the card answering costs a
  // moment, and its reader's own tries bound it: those are for a glitch, and a sector every
  // reader shares fails 4 times in the first few milliseconds of one. Kept through a remount
  // for that reason; gone with the session, a read that works, a write of the sector (a card
  // may remap it), or a card put back (remounted()). The other sectors are asked as ever.
  // In FatFs's calls, under its lock.
  struct FailedRead {
    uint32_t lba = 0, count = 0;
    uint32_t fails = 0;
    uint64_t atUs = 0;
  };
  static constexpr int kFailedReads = 8;
  bool readStopped(uint32_t lba, uint32_t count);
  // A read done: its failure's look, and what it says of the stop. `reached`: it went to the card;
  // `again`: it is the read asked once more. True: ask it once more: its look kept the card, and it
  // was the first try. The card is back from its init then, the first read since that can work: the
  // driver's own three tries came before it. A second failure is FatFs's error, as a kept one was,
  // through its own look: the same card again, or a silence counted once for that read, as ever.
  bool afterRead(uint32_t lba, uint32_t count, bool ok, bool reached, bool again);
  void noteWritten(uint32_t lba, uint32_t count);
  // A write the card refused: its look. One that leaves the card silent has no bound of
  // its reader's: the queue's save tries again, and each is a cut in whatever plays, and
  // only FatFs's window is the wrapper's to tell from the rest. kStopAfter of them, and
  // no more writes until a restart (writesOff()).
  void writeFailed(uint32_t lba);

  SectorCache& cache_;
  SectorCache::Device& card_;
  ClockFn clock_;
  Link* link_ = nullptr;
  bool remountDue_ = false;
  FailedRead failed_[kFailedReads];
  int failedN_ = 0;
  bool silentFault_ = false;   // a look found the card silent, and it has answered no read since
  uint32_t silences_ = 0;      // ... this many times (tooSilent()), in FatFs's calls
  uint32_t silentWrites_ = 0;  // ... of them at a write (writeFailed())
  bool silentWrite_ = false;   // ... as silentFault_'s was
  bool mountFailed_ = false;   // mountFailed(), since the last remount
  uint64_t mountFailedUs_ = 0;  // ... the first
  std::atomic<bool> out_{false};  // cardOut()
  std::atomic<uint32_t> cardBacks_{0};
  std::atomic<bool> tooSilent_{false};
  bool windowFailing_ = false;  // windowFailed(): the sector's first time, no write of it taken since
  uint32_t windowLba_ = 0;
  uint32_t windowDrops_ = 0;
  std::atomic<bool> writesOff_{false};
  std::atomic<uint32_t> kept_{0};
  std::atomic<uint32_t> healed_{0};
  Card counted_{*this};
  uint8_t* check_ = nullptr;
  std::atomic<bool> enabled_{true};
  std::atomic<bool> clear_{false};
  std::atomic<bool> reset_{false};
  std::atomic<bool> verify_{false};
  std::atomic<bool> armed_{false};
  std::atomic<bool> foreign_{false};
  std::atomic<uint32_t> remounts_{0};
  Identity mountId_;
  Identity lastId_;
  Stats stats_;
};
