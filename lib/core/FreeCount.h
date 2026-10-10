// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The free clusters of a FAT volume, counted a piece of its FAT at a time:
// the computer's @count (docs/HOST-STATUS.md "@count"). FatFs's own count,
// f_getfree() on a volume whose free count is unknown, reads every sector
// of the FAT holding the volume's lock throughout (minutes on a 1 TB card:
// the decoder's reads would time out after 10 s, and the music stop). The
// firmware (storage/CardSpace) reads one piece per loop pass under that
// lock instead, and lets go between pieces. FatFs may write the FAT
// between two pieces (a file grows, one is deleted), so:
//   - the disk's write watch (storage/SectorDisk) reports every write;
//     a piece already counted that a write reached is read again
//     (written());
//   - FatFs keeps a FAT sector it changed in its window (RAM) until it
//     writes it: a piece read while that is so takes the window's bytes
//     for that sector (overlay()), and the commit, under the lock that
//     sets the result, reads the window's piece once more.
// So the count is exact as of that lock: what f_getfree() would have
// counted holding the lock all along.
//
// FAT32 (28-bit entries), FAT16, and FAT12 (its whole FAT is one piece: at
// most 4,086 entries, 6.1 KB). The entries counted are the clusters', 2 to
// entries - 1; one is free when it is 0 (FAT32: its low 28 bits), as
// f_getfree() counts them. Portable; no allocation but its table (a count
// per piece, a bit per piece to read again: 16 KB for a 2 TB card's 256 MB
// FAT). Host-tested: test_free_count, on synthetic FATs and against
// FatFs's own f_getfree() on the host model with files written and deleted
// between the pieces.
class FreeCount {
public:
  enum class Fat : uint8_t { Fat12, Fat16, Fat32 };
  struct Volume {
    Fat fat = Fat::Fat32;
    uint32_t fatStart = 0;     // the first FAT's first sector (FatFs's fatbase)
    uint32_t fatSectors = 0;   // its sectors (fsize)
    uint32_t entries = 0;      // its entries: the clusters + 2 (n_fatent)
    uint32_t sectorBytes = 512;
  };
  // A piece: kPieceBytes of the FAT (less at its end), read in one call
  // (64 sectors: ~13 ms on the Core2's card at 25 MHz).
  static constexpr uint32_t kPieceBytes = 32768;
  struct Piece {
    uint32_t index = 0;
    uint32_t lba = 0;      // its first sector
    uint32_t sectors = 0;  // its sectors
  };
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  // nullptr hooks: malloc/free.
  explicit FreeCount(AllocFn alloc = nullptr, FreeFn release = nullptr) : alloc_(alloc), release_(release) {}
  ~FreeCount() { end(); }
  FreeCount(const FreeCount&) = delete;
  FreeCount& operator=(const FreeCount&) = delete;

  // A count of `v` from its start (one under way is dropped). False: a
  // volume FatFs couldn't have mounted, or no memory for the table.
  bool begin(const Volume& v);
  // The table freed; nothing counted.
  void end();
  bool active() const { return counts_ != nullptr; }

  // The piece to read next: the first pass's next, then those a write
  // reached after they were counted (the lowest first). False: none
  // (done()).
  bool next(Piece* p) const;
  // `data` is piece `p` as read now (p.sectors * sectorBytes bytes): its
  // free entries replace what it had.
  void counted(const Piece& p, const uint8_t* data);
  // A write reached sectors [lba, lba + n): the pieces it touches that were
  // counted are read again. (Any write: the rest of the card isn't the
  // FAT's.)
  void written(uint32_t lba, uint32_t n);
  // The sector is the FAT's (its first copy, as far as this counts).
  bool inFat(uint32_t lba) const { return active() && lba >= fatStart_ && lba - fatStart_ < sectors_; }

  // Every piece counted, none to read again: freeClusters() is the count.
  bool done() const { return active() && cursor_ == pieces_ && again_ == 0; }
  uint32_t freeClusters() const { return total_; }
  // The first pass's share, 0-99; 100 once done().
  uint8_t percent() const;
  uint32_t pieces() const { return pieces_; }
  uint32_t reads() const { return reads_; }       // pieces read (counted())
  uint32_t rereads() const { return rereads_; }   // ... of them again, after a write
  uint32_t fatBytes() const { return bytes_; }    // the bytes counted (entries 0 to the last)

  // FatFs's window: `win` holds sector `winLba`, changed and not yet written
  // (FATFS::wflag). If that sector is in [lba, lba + sectors), its bytes in
  // `data` (that range's, as read from the card) are the window's.
  static void overlay(uint8_t* data, uint32_t lba, uint32_t sectors, uint32_t sectorBytes, uint32_t winLba,
                      const uint8_t* win);

  // The volume as FatFs holds it now, for step(): the caller holds its lock.
  class Source {
  public:
    virtual ~Source() = default;
    // The card's sectors [lba, lba + sectors) into `out`. False: a read error.
    virtual bool read(uint32_t lba, uint8_t* out, uint32_t sectors) = 0;
    // FatFs's window, when it holds a sector it changed and hasn't written
    // (wflag): true, its LBA and its bytes.
    virtual bool window(uint32_t* lba, const uint8_t** bytes) = 0;
  };
  enum class Step : uint8_t { Working, Done, Failed };
  // A read again in the commit's hold: the window's piece and one more.
  static constexpr int kSettlePieces = 2;
  // One step, with the volume's lock held throughout: the next piece (the
  // window's bytes over the card's: overlay()). Once every piece is counted,
  // the commit's check in the same hold: the window's piece once more (a
  // FAT sector FatFs changed after that piece was read and hasn't written
  // shows nowhere else), and the pieces a write reached, kSettlePieces at
  // most (more: Working, the next step reads them). Done: freeClusters() is
  // exact now, until the lock goes: the caller sets FatFs's count before it
  // lets go. Failed: a read error. `buf`: kPieceBytes.
  Step step(Source& src, uint8_t* buf);

private:
  bool readPiece(Source& src, const Piece& p, uint8_t* buf);
  // Piece i's free entries, from its bytes.
  uint32_t freeIn(uint32_t i, const uint8_t* data) const;
  bool dirty(uint32_t i) const { return (dirty_[i >> 5] >> (i & 31)) & 1u; }

  AllocFn alloc_;
  FreeFn release_;
  Fat fat_ = Fat::Fat32;
  uint32_t fatStart_ = 0;
  uint32_t sectors_ = 0;       // the FAT's sectors that hold entries 0 to entries - 1
  uint32_t entries_ = 0;
  uint32_t sectorBytes_ = 512;
  uint32_t pieceSectors_ = 0;
  uint32_t pieces_ = 0;
  uint32_t bytes_ = 0;
  uint16_t* counts_ = nullptr;  // per piece (at most 16,384 entries in one: FAT16's)
  uint32_t* dirty_ = nullptr;   // a bit per piece: counted, then written
  uint32_t cursor_ = 0;         // the first pass's next piece
  uint32_t again_ = 0;          // pieces to read again
  uint32_t total_ = 0;          // the counted pieces' free entries
  uint32_t reads_ = 0, rereads_ = 0;
};
