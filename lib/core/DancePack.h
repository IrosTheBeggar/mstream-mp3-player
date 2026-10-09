// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "Crc32.h"
#include "DanceRules.h"
#include "Fnv1a.h"
#include "GenreFold.h"

// The reader of a dance pack: an MPDN v1 file as tools/dancepack.py writes
// it (docs/DANCE-SPRITES.md, Appendix A; the C++ side is Appendix D). One
// reader serves the built-in packs (const arrays in flash, read in place) and
// the card's /dancers/*.dnc files (bytes in the PSRAM arena). Portable,
// host-tested (test_dance_pack), and the same checks, with the same reasons
// word for word, as dancepack.py's read_pack, so `dancepack.py info` tells a
// contributor exactly what the device will say of a pack.
//
// Two entries:
//   - open(data, size, avail): the whole file, at load. It needs every byte
//     (with avail < size it rejects the pack as a short read before reading
//     anything), checks the CRC, then runs every one of A.15's pack checks
//     in A.15's order, the first failure rejecting the pack with its reason:
//     the header, the directory (A.3: alignment, bounds, duplicate tags,
//     overlaps), the nine required chunks, META's strings, the geometry, the
//     palettes and thumbnail, FTAB, a dry-run decode of every frame (A.10),
//     the rest frame, STRS's pose keys, MOVS's head and STEP's size. Then
//     each move goes through A.15's move checks, the first failing one
//     dropping only that move (reported through Report::onDrop); the others
//     keep working. A pack whose every move is dropped still opens.
//   - scan(buf, avail, size): the boot scan (5.5) of a file's first bytes,
//     usually 2 KB: the header without the CRC, the directory, and META,
//     GEOM, PALS and THMB, which the compiler always puts in the first 2 KB.
//     It never reads past avail ("header unreadable" when something it needs
//     lies beyond), so the Dancers page can list a card pack's name, author,
//     licence and thumbnail without loading it.
//
// Step 2 and step 3 (D.2). A.15's last two move checks, `travels from no
// spot` (B.11's start spots) and `budget red` (B.6's budget), need the
// engine's frameAt, which comes with step 3. With neither, a move that
// passed every other check is kept. They join in two ways, so that open()
// stays one short call (5.7):
//   - `travels from no spot`, cheap (O(steps) a move), runs inside open()
//     through an optional hook, OpenOptions::late, called for each move
//     that passed every other check;
//   - `budget red`, a simulation of up to ~3 ms a move, runs after open(),
//     one kept move a frame while the old dancer keeps dancing; a red move
//     is dropped with dropMove(), which reports it like open()'s drops.
// As those rows are A.15's last, nothing a move fails before them depends
// on either.
//
// Zero copy, no heap. A Pack is a view of the caller's bytes: the strings it
// returns point into them (each ends in its NUL, A.1), so they live as long
// as the bytes do; copying a Pack is cheap and shares them. It keeps only
// the offsets it needs and one kept bit per move (at most 64), and stays
// under 192 bytes, so the firmware may keep one in internal RAM. Nothing is
// kept per dropped move: open() hands each drop's Reason to onDrop, where
// the device logs `[dance] robo: move "spin" ignored: <reason>` through a
// kReasonChars stack buffer.
//
// Every bound is checked before the bytes it guards are read, in the orders
// A.1, A.3, A.9, A.11 and 5.5 give, so no 32-bit sum wraps (an offset near
// 2^32 fails "off <= total" before off + size is ever formed). Multi-byte
// fields are read a byte at a time, little-endian: a pack can start at any
// address.
//
// Region views (10.3's bounds-asserting accessor). Every byte open() and
// scan() read, except the CRC's span, comes through a view of one region:
// the header [0, 32), the directory once its count has passed, or one of the
// nine required chunks once its directory entry has passed A.3 (and, in a
// scan, only once it lies within avail). A read outside its view returns 0
// and counts a fault instead of reading. A reader that follows the orders
// above never makes one: regionFaults() is always 0, and the tests assert
// it after every open() and scan(), rejected ones included. The views stay
// in the firmware: their reads are forced inline with 32-bit indices, so a
// byte costs one compare and a load (no call), at load only, and a mistake
// reads 0 rather than past the pack.
//
// The accessors never read outside the pack, whatever their arguments: out
// of their state or out of range they return 0, null, false or a zeroed
// struct.
namespace dance {

class Pack;
struct PackRegion;  // a region view (DancePack.cpp)

enum class Why : uint8_t {
  None,
  // the pack is rejected (A.15's first table, in its order)
  ShortRead, TooShort, NotMpdn, NeedsNewerPlayer, SizeMismatch, TooBig, BadChecksum,
  BadChunkCount, HeaderUnreadable, ChunkMisaligned, ChunkOutOfBounds, ChunkTwice,
  ChunksOverlap, NoChunk, MetaBad, GeomTooShort, GeomBad, PalsSize, ThmbSize, FtabSize,
  FrameBadBox, FrameDataOutOfBounds, FrameBadData, RestFrameEmpty, StrsTooShort,
  StrsBadPoseKey, MovsTooShort, MovsCount, MovsRecordLength, StepSize,
  // a move is dropped (A.15's second table, in its order)
  BadName, BadKind, BadSteps, FrameOutOfRange, Beats, StepTicks, TickSum, StepMs, IdleTotal,
  BeatFields, BadWhen, Pace, RangeUnreachable, Weight, Repeat, Bounce, Sway, Travel,
  BadStartAt, BadEase, BadPlace, Offset, BadPoseKey, BadGenre, SongBpm, Needs,
  TravelsFromNoSpot, BudgetRed,  // step 3's (D.2)
};

// A reason and its arguments (D.1's table): a and b are the numbers in the
// text; MetaBad's and GeomBad's a is the field's index; Travel's a and
// Offset's a and b are int32_t values stored as their bits.
struct Reason {
  Why why = Why::None;
  uint8_t tag[4] = {0, 0, 0, 0};   // a chunk's tag as stored; ChunksOverlap: the earlier entry's
  uint8_t tag2[4] = {0, 0, 0, 0};  // ChunksOverlap: the later entry's
  uint32_t a = 0, b = 0;           // the table's numbers or field; Travel, Offset: int32_t bits
};
constexpr size_t kReasonChars = 64;  // the longest text and its NUL fit
// A.15's text, NUL-terminated and cut to cap - 1 bytes (nothing written
// when cap is 0). Returns the whole text's length, as snprintf does.
size_t formatReason(const Reason& r, char* out, size_t cap);

// A.1's "valid": strict UTF-8 (no overlong form, no surrogate, nothing past
// U+10FFFF, no cut sequence), no NUL and no control character
// (U+0001-U+001F, U+007F-U+009F). n = 0 is valid.
bool validText(const char* s, size_t n);

using DropFn = void (*)(void* ctx, int move, const Reason& why);
struct Report {
  Reason rejected;          // Why::None when open() or scan() succeeded
  uint8_t dropped = 0;      // the moves open() dropped, then those dropMove() dropped
  DropFn onDrop = nullptr;  // optional: once per dropped move: open()'s in MOVS order, then each dropMove()'s
  void* ctx = nullptr;      // onDrop's
};

// D.2: a move check run inside open(). True keeps the move; false, with
// *why set, drops it. Step 3's is B.11's start spots (TravelsFromNoSpot);
// the budget (BudgetRed) runs after open(), dropping through
// Pack::dropMove, so open() stays short (5.7).
using LateCheck = bool (*)(void* ctx, const Pack& pack, int move, Reason* why);
enum : uint8_t { kSkipCrc = 1 };  // tests only: open() doesn't compare the CRC
struct OpenOptions {
  uint8_t flags = 0;         // kSkipCrc
  LateCheck late = nullptr;  // nullptr in step 2
  void* lateCtx = nullptr;
};

struct PackGeom {  // A.6; restFrame as stored, bit 15 included
  uint8_t scale, faces, cellW, cellH, anchorX, anchorY, shadowW;
  uint16_t frameCount, restFrame;
};
struct FrameBox { uint8_t x0, y0, w, h; };                               // A.9
struct PackFrame { FrameBox box; const uint8_t* data; uint16_t len; };  // data: its FDAT bytes
struct StepRecord {  // A.13; frame is bits 0-14
  uint16_t frame;
  bool mirrored;
  uint16_t dur;
  int8_t dx, dy;
  uint8_t ease;
  bool go;
};
struct MoveRecord {  // A.12's first 32 bytes, as stored
  uint16_t name;
  uint8_t kind, beats, paceLo, paceHi, weight10, when, flags, repeat, needs;
  int8_t travel;
  uint8_t startAt, travelEase, bounce, sway, fromKey, toKey, fromKeyM, toKeyM;
  uint16_t genreFirst;
  uint8_t genreCount, stepCount;
  uint16_t stepFirst;
  uint8_t songLo, songHi;
};

class Pack {
 public:
  enum class State : uint8_t { None, Scanned, Open };

  // 5.4: the whole file. data may be null only when avail is 0.
  bool open(const uint8_t* data, uint32_t size, uint32_t avail, Report* report = nullptr,
            const OpenOptions* options = nullptr);
  // 5.5: the boot scan of a file of `size` bytes whose first `avail` were
  // read into buf. buf may be null only when avail is 0.
  bool scan(const uint8_t* buf, uint32_t avail, uint32_t size, Report* report = nullptr);
  void clear();  // State::None, regionFaults() 0

  State state() const;
  uint32_t size() const;          // the file's size; 0 in State::None
  uint16_t regionFaults() const;  // reads outside their view in the last open() or scan()

  // State Scanned or Open
  const char* id() const;
  const char* name() const;
  const char* author() const;
  const char* licence() const;
  const char* url() const;
  const char* version() const;
  uint8_t declaredMoves() const;
  const PackGeom& geom() const;
  const uint8_t* palette() const;      // PALS's dance half: 16 RGB888 entries, 48 B
  const uint8_t* idlePalette() const;  // its idle half, 48 B
  const uint8_t* thumb() const;        // THMB, 800 B (A.8)

  // State Open
  int frameCount() const;
  FrameBox box(int frame) const;
  PackFrame frame(int frame) const;
  int poseKeyCount() const;           // P
  const char* poseKey(int id) const;  // id 1..P
  int moveCount() const;              // every MOVS record, dropped ones included
  int keptMoves() const;
  bool moveKept(int move) const;
  MoveRecord moveRecord(int move) const;
  const char* moveName(int move) const;
  StepRecord step(int move, int k) const;
  const char* genreTag(int move, int g) const;

  // D.2: drops a kept move after open(), as step 3's loader does with a
  // move whose budget is red, one move a frame (5.7). False, and nothing
  // changes, unless the pack is open and the move kept. Otherwise its kept
  // bit clears, keptMoves() goes down by 1, and with a report `dropped`
  // goes up by 1 and onDrop(ctx, move, why) is called.
  bool dropMove(int move, const Reason& why, Report* report = nullptr);

 private:
  enum : int { kMetaStrings = 6 };
  // A view of [off, off + len) of the bytes; faults null for the accessors.
  PackRegion region(uint32_t off, uint32_t len, uint16_t* faults) const;
  void reset();  // everything but the fault count, which a rejection keeps
  bool reject(Report* report, const Reason& why);
  // A.15's pack rows (open), or 5.5's rows 1-8 (scanning), over data_.
  bool load(Report* report, uint32_t size, uint32_t avail, const OpenOptions* options, bool scanning);
  // A.15's move rows down to `needs 0x<hh>`: the first that fails, or None.
  Reason checkMove(int move, uint16_t* faults) const;

  const uint8_t* data_ = nullptr;  // the caller's bytes (zero copy)
  uint32_t size_ = 0;
  uint32_t meta_[kMetaStrings] = {};  // each META string's text, from data_
  uint32_t pals_ = 0, thmb_ = 0;      // chunk offsets, from data_
  uint32_t ftab_ = 0, fdat_ = 0, fdatLen_ = 0;
  uint32_t strs_ = 0, strsLen_ = 0;
  uint32_t movs_ = 0, step_ = 0, stepLen_ = 0;
  uint64_t kept_ = 0;  // bit i: move i kept
  PackGeom geom_ = {};
  uint16_t faults_ = 0;
  State state_ = State::None;
  uint8_t declared_ = 0, poseKeys_ = 0, moveCount_ = 0, recordLen_ = 0, keptCount_ = 0;
};

}  // namespace dance
