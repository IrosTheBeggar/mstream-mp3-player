// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"

// The waveform index, waves-<gen>.bin, MPWF v1 (docs/WAVEFORM-INDEX.md): for
// each card file the transfer software has the server's peaks for, its length
// and its shape, 240 bars of 0-255. One section, WVSL, at byte 256, so the
// file is exactly 256 + 256 x rows bytes and no slot crosses a 512-byte
// sector. The slots are sorted by path hash, with no table beside them:
// find() is an interpolation search of the slots themselves (6), each read
// alone and checked alone, its CRC seeded with its row (4.3). There is no
// whole-file check on the device: a slot whose CRC fails is Damaged (this
// lookup misses, the file goes on), a slot whose CRC passes over a hash that
// breaks the sort rule is Broken (a broken writer: the file is absent from
// then on), 8.3. The device compares the file with the root's COMP row
// (msmf::companionMatches, 7.2).
//
// A lookup runs on the card worker's 6 KB of stack: no heap, and no buffer of
// its own, since each slot is read into the caller's Slot. No 128-bit integer:
// 6's arithmetic stays below 2^51.
//
// The writer (the host tests, the transfer's tools) takes the bars from the
// server's peaks by 5's rule. The bars are linear: a stretch is the reader's,
// who knows its columns.
namespace cardcontract {
namespace mpwf {

constexpr uint16_t kMajor1 = kMajorMpwf;  // the major this reader reads (1)
constexpr uint32_t kHeaderBytes = 224;    // minor 0's header (4.1): zeros past its first 48 bytes
constexpr uint32_t kWvsl = fourcc("WVSL");
enum Sec : uint32_t { kSecSlots = 0, kSecCount };
extern const FormatSpec kSpec;

constexpr uint32_t kSlotsOffset = kHeaderBytes + kDirEntryBytes;  // 256: one directory entry, then WVSL
constexpr uint32_t kSlotBytes = 256;
constexpr uint32_t kBarCount = 240;
constexpr uint32_t kBarsAt = 12;           // a slot's bars (4.3): after its hash and its length
constexpr uint32_t kCrcAt = 252;           // its CRC, over the row and the bytes before it
constexpr uint32_t kMaxRows = 250000;      // the file stays under the transfer's 64 MiB cap
constexpr uint32_t kInterpolated = 4;      // the probes that interpolate; the later ones bisect
constexpr uint32_t kMaxProbes = 24;        // a guard: a lookup takes 22 reads at the most (6)

constexpr uint32_t fileBytesFor(uint32_t rows) { return kSlotsOffset + rows * kSlotBytes; }

// The CRC of row `row`'s slot (kSlotBytes bytes): CRC-32 over the row as u32
// LE, then the slot's first 252 bytes (a slot read for the wrong row fails).
uint32_t slotCrc(const uint8_t* slot, uint32_t row);

// A slot as the card holds it (4.3).
struct Slot {
  uint8_t bytes[kSlotBytes] = {};
  uint64_t pathHash() const { return get64(bytes); }
  uint32_t lengthMs() const { return get32(bytes + 8); }  // 0 unknown
  const uint8_t* bars() const { return bytes + kBarsAt; }  // kBarCount of them
  uint32_t crc() const { return get32(bytes + kCrcAt); }
};

// What a lookup reads (6, 8.3). Hit: `out` holds the hash's slot. Miss: no row
// holds it: a track with no data, a plain line. Damaged: a slot's CRC failed,
// the card's fault: this lookup misses, the file stays. Broken: a hash breaks
// the sort rule under a good CRC, a broken writer's: the file is absent from
// then on. Io: a read failed, and the file isn't to blame.
enum class Lookup : uint8_t { Hit, Miss, Damaged, Broken, Io };

// The next row to read (6): rounded to the nearest by the top 32 bits of (h -
// loH) / (hiH - loH) over lo..hi, while `probes` (those already made) is below
// kInterpolated and that span is 2^32 or more; else the middle, rounded down.
// lo <= hi and loH <= h <= hiH; the answer is in lo..hi.
uint32_t pick(uint64_t h, uint32_t lo, uint32_t hi, uint64_t loH, uint64_t hiH, uint32_t probes);

class Table {
public:
  // The frame (8.1: major 1, WVSL present, its stride at least 256) and the
  // header and WVSL (8.2): slotBytes 256 and barCount 240 (Enum); WVSL's stride
  // 256 (Shape); 1-250,000 rows (Enum); WVSL at byte 256 (WvLayout), so the one
  // section and the file 256 + 256 x rows bytes. Reads only the header and the
  // directory. Anything but Ok leaves no rows: every find() then misses.
  Why open(Source& src);
  // The whole file, as a tool verifies it (8.4): every slot in row order, its
  // CRC (WvSlotCrc), then its hash above the one before (WvSlotOrder); last
  // WVSL's CRC. The device never runs it. `scratch`: at least 320 bytes (a
  // slot and the stream's buffer).
  Why check(uint8_t* scratch, uint32_t scratchBytes) const;

  const Header& frame() const { return c_.header(); }
  const Container& container() const { return c_; }
  uint32_t rows() const { return rows_; }

  // The slot of a card file's path hash, by 6's search: each probe is one read
  // of kSlotBytes bytes into `out` (not a Hit: `out` holds the last slot read,
  // or part of it). `why` (or nullptr) gets WvSlotCrc for Damaged, WvSlotOrder
  // for Broken, Io for Io, else Ok.
  Lookup find(uint64_t pathHash, Slot* out, Why* why = nullptr);
  // The slot reads the last find() took, a failed one too.
  uint32_t reads() const { return reads_; }

private:
  Container c_;
  uint32_t rows_ = 0;
  uint32_t reads_ = 0;
};

// Bar i's peaks of M (5): [start, end), where start is floor(i M / 240) and
// end is min(M, max(start + 1, floor((i + 1) M / 240))).
void barRange(uint32_t i, uint32_t m, uint32_t* start, uint32_t* end);
// The 240 bars of M peaks (5): each the RMS of its range in f64, rounded half
// up by floor(x + 0.5). M 0 gives zeros.
void barsFrom(const uint8_t* peaks, uint32_t m, uint8_t* bars);

// A row of the writer's input (4.3).
struct RowIn {
  uint64_t pathHash = 0;
  uint32_t lengthMs = 0;
  const uint8_t* bars = nullptr;  // kBarCount of them (barsFrom)
};

struct TableIn {
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint16_t minor = 0;
  const RowIn* rows = nullptr;  // any order (sorted by path hash here)
  uint32_t rowCount = 0;
};

// Writes an index. A hash two or more rows hold is left out, all of them (4.3:
// a lookup never finds another file's bars). False with `error` on bad input
// (no rows left to write: a writer with no row writes no file; more than
// 250,000; a row with no bars) or a failed sink.
bool write(Sink& out, const TableIn& in, uint32_t* fileBytes, uint32_t* headerCrc, const char** error);

}  // namespace mpwf
}  // namespace cardcontract
