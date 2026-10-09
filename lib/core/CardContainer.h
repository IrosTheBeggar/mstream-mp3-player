// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContract.h"

// The frame every binary file of the card contract shares (docs/METADATA.md
// 2.4): a header (40 common bytes, then the type's own, up to headerBytes),
// a section directory of 32-byte entries, then the sections, each 8-byte
// aligned. Reading it: Container checks the frame (the magic, the major,
// the header's CRC, the file's length, the directory's layout, the REQUIRED
// sections) without reading any section; Stream reads one section, or one
// part of it, front to back through a caller's buffer, CRC'ing what it
// passes, so the formats' readers (CardTags.h, CardManifest.h,
// CardAutoDj.h) check a whole file in one pass with fixed memory, whatever
// it holds. Writing it: ContainerWriter lays the file out, takes the
// sections' bytes in order and writes the header last. The root election
// (2.5.4), which the plan's (2.12.5) shares, and the software's
// newer-major guard (2.4.5) are here too.
//
// "Absent" (2.4.3): a reader that finds any check failed treats the file
// as not there, never as an error the listener must clear. Why says which
// check, for the tests and the console; to the caller every value but Ok
// means the same.
namespace cardcontract {

constexpr uint32_t kMagicMsmf = fourcc("MSMF");
constexpr uint32_t kMagicMptg = fourcc("MPTG");
constexpr uint32_t kMagicMpdj = fourcc("MPDJ");
constexpr uint32_t kMagicMspd = fourcc("MSPD");
constexpr uint32_t kMagicMpth = fourcc("MPTH");
constexpr uint16_t kMajor = 1;  // the only major a v1 reader reads
constexpr uint32_t kCommonHeaderBytes = 40;
constexpr uint32_t kDirEntryBytes = 32;
constexpr uint32_t kMaxSections = 64;
constexpr uint32_t kSectionRequired = 1;  // a reader that doesn't know the type: the file is absent
constexpr uint32_t kStrs = fourcc("STRS");

// ---------------------------------------------------------------------------
// Reading and writing files: the card's (FatFs on the device), memory's on
// the host. Offsets are u32: a FAT32 file is under 4 GiB.
// ---------------------------------------------------------------------------
class Source {
public:
  virtual ~Source() = default;
  virtual uint32_t size() const = 0;  // the file's length on the card
  // `n` bytes at `offset`; false when they aren't all there, or the read failed.
  virtual bool read(uint32_t offset, void* out, uint32_t n) = 0;
};

class MemSource : public Source {
public:
  MemSource(const void* data, uint32_t n) : p_(static_cast<const uint8_t*>(data)), n_(n) {}
  uint32_t size() const override { return n_; }
  bool read(uint32_t offset, void* out, uint32_t n) override;

private:
  const uint8_t* p_;
  uint32_t n_;
};

class Sink {
public:
  virtual ~Sink() = default;
  // `n` bytes at `offset` (the writer seeks: its header goes last).
  virtual bool write(uint32_t offset, const void* data, uint32_t n) = 0;
};

// Into a caller's buffer; size() is the highest byte written.
class BufferSink : public Sink {
public:
  BufferSink(void* buf, uint32_t cap) : p_(static_cast<uint8_t*>(buf)), cap_(cap) {}
  bool write(uint32_t offset, const void* data, uint32_t n) override;
  uint32_t size() const { return size_; }

private:
  uint8_t* p_;
  uint32_t cap_;
  uint32_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Why a file is absent.
// ---------------------------------------------------------------------------
enum class Why : uint8_t {
  Ok = 0,
  Io,               // a read failed
  Short,            // shorter than its header and directory
  Magic,            // not this format
  Major,            // a major this reader doesn't read (a newer one)
  HeaderBytes,      // headerBytes not a multiple of 8, or below the v1 header's size
  SectionCount,     // more than 64 sections
  FileBytes,        // fileBytes isn't the file's length on the card
  HeaderCrc,
  Layout,           // the directory's layout (2.4.2): offsets, order, overlaps, gaps, the end, a type twice
  UnknownRequired,  // a REQUIRED section this reader doesn't know
  Missing,          // a section the format requires isn't there
  Shape,            // a known section's count, stride and length disagree, or its stride is below v1's
  SectionCrc,
  Counts,           // the header's counts against the sections'
  Enum,             // a value that makes the file absent (MPTG source, MPDJ scoreKind, indexBytes, k)
  String,           // a string offset outside its section, or no NUL after it inside
  StringOrder,      // strings not in the order the format writes them (2.4.4)
  Name,             // a name empty, with a '/', "." or ".."
  PathLength,       // "/music/" + a path over 255 bytes
  FoldRoot,         // folder 0's parent isn't 0xFFFFFFFF, or its name isn't offset 0
  FoldParent,       // a parent not lower, or not the folder before or one of its ancestors
  FoldOrder,        // siblings' names not strictly increasing
  FirstRecord,      // a folder's firstRecord isn't the count of records before it
  RecsFolder,       // a record's folder index not below folderCount
  RecsOrder,        // records not strictly increasing by (folder, name)
  Hidx,             // HIDX: its count, its order, a record twice or a hash not its record's
  Orig,             // ORIG's count isn't recordCount
  Comp,             // COMP: the kinds, their number or order, a name
  Libr,             // LIBR: a root not a path, or roots not strictly increasing
  DjRows,           // DJRW not in hashPrefix order
  DjPaths,          // DJPH not strictly increasing, or a row out of range
  DjNeighbours,     // a DJNB index out of range
  PendOrder,        // PEND not in 2.12.5's order
  PendPath,         // a PEND path empty, absolute or not made of names
  PendOp,           // an op of 0
};
const char* whyName(Why w);

// ---------------------------------------------------------------------------
// The frame.
// ---------------------------------------------------------------------------
struct Header {
  uint32_t magic = 0;
  uint16_t major = 0;
  uint16_t minor = 0;
  uint32_t headerBytes = 0;
  uint32_t sectionCount = 0;
  uint32_t fileBytes = 0;
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint32_t headerCrc = 0;
};

struct Section {
  uint32_t type = 0;  // 0: not in the file
  uint32_t flags = 0;
  uint32_t offset = 0;
  uint32_t bytes = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  uint32_t crc = 0;
  bool present() const { return type != 0; }
};

// What a reader knows of a section type: its v1 stride (0 for a blob), and
// whether the format requires it.
struct SectionSpec {
  uint32_t type;
  uint32_t stride;
  bool required;
};

struct FormatSpec {
  uint32_t magic;
  uint32_t headerBytes;  // v1's whole header
  const SectionSpec* sections;
  uint32_t count;        // at most kMaxKnown
};
constexpr uint32_t kMaxKnown = 8;

class Container {
public:
  // Checks the frame of `src` as a file of `spec`, reading only the header
  // and the directory (at most a few KB, through a small buffer):
  //   - the magic, the major (1), headerBytes (a multiple of 8, at least
  //     v1's), sectionCount (at most 64), fileBytes against src.size(), the
  //     header's CRC;
  //   - the directory (2.4.2): offsets multiples of 8 in increasing order,
  //     the first at the directory's end, each next one under 8 bytes past
  //     the end of the one before, the last ending at fileBytes, a type
  //     once;
  //   - the sections: an unknown REQUIRED one makes the file absent; every
  //     section the format requires is there; a known array's stride is at
  //     least v1's and bytes = count x stride; a known blob has count and
  //     stride 0.
  // `typeHeader` receives the type's own header bytes, [40, spec.headerBytes)
  // (a newer minor's longer header is CRC'd, its tail skipped).
  Why open(Source& src, const FormatSpec& spec, uint8_t* typeHeader);
  const Header& header() const { return header_; }
  // The known section i (the order of spec.sections); type 0 when absent.
  const Section& section(uint32_t i) const { return known_[i]; }
  Source* source() const { return src_; }
  // Reads section i whole through `buf` and compares its CRC.
  Why checkCrc(uint32_t i, uint8_t* buf, uint32_t bufLen) const;

private:
  Source* src_ = nullptr;
  Header header_;
  Section known_[kMaxKnown];
};

// A sequential reader of [start, end) of a Source through a caller's buffer:
// every byte it passes is CRC'd, so crc() after drain() is the CRC of the
// whole range. Reads past `end` fail (failed() then stays true). A buffer
// of 1 KB or more refills from the sector (kSector) boundary at or before
// the next byte: it may read up to 511 bytes before `start`, never past
// `end` (a Source's offsets are its file's).
class Stream {
public:
  static constexpr uint32_t kSector = 512;
  void begin(Source* src, uint32_t start, uint32_t end, uint8_t* buf, uint32_t bufLen);
  bool read(void* out, uint32_t n);
  bool readByte(uint8_t* b) { return read(b, 1); }
  // Moves forward to `pos` (at least position(), at most end()), passing
  // the bytes between.
  bool skipTo(uint32_t pos);
  bool drain() { return skipTo(end_); }
  // A NUL-terminated string at position(): copies its first cap - 1 bytes
  // (`out` NUL-terminated), passes it and its NUL, and gives its whole
  // length. False: no NUL before end() (or a failed read).
  bool readString(char* out, size_t cap, size_t* len);
  uint32_t position() const { return pos_; }
  uint32_t start() const { return start_; }
  uint32_t end() const { return end_; }
  uint32_t crc() const { return crc_; }
  bool failed() const { return failed_; }

private:
  bool fill();
  Source* src_ = nullptr;
  uint8_t* buf_ = nullptr;
  uint32_t bufLen_ = 0;
  uint32_t start_ = 0, end_ = 0;
  uint32_t pos_ = 0;       // the next byte
  uint32_t bufAt_ = 0;     // buf_[0] is this offset
  uint32_t bufFill_ = 0;   // valid bytes in buf_
  uint32_t crc_ = 0;
  bool failed_ = false;
};

// A string read at random from a string section (not in a stream's order),
// for the random-access readers: `off` inside the section and a NUL after
// it inside, or Why::String. Copies up to cap - 1 bytes (`out` always
// NUL-terminated when cap > 0); `len` gets the whole length.
Why readStringAt(Source& src, const Section& strs, uint32_t off, char* out, size_t cap, size_t* len = nullptr);

// ---------------------------------------------------------------------------
// Writing the frame.
// ---------------------------------------------------------------------------
struct FileMeta {
  uint32_t magic = 0;
  uint16_t major = kMajor;
  uint16_t minor = 0;
  uint32_t generation = 0;
  uint64_t cardId = 0;
};

struct SectionOut {
  uint32_t type = 0;
  uint32_t flags = 0;
  uint32_t count = 0;   // 0 for a blob
  uint32_t stride = 0;  // 0 for a blob
  uint32_t bytes = 0;
};

class ContainerWriter {
public:
  // Lays the file out (2.4.2): the header (the common 40 bytes, then
  // `typeHeader`'s headerBytes - 40), the directory, then `n` sections in
  // this order, the first right after the directory and each next one at
  // the previous one's end rounded up to 8 (the gap zeros).
  bool begin(Sink& sink, const FileMeta& meta, const uint8_t* typeHeader, uint32_t headerBytes, const SectionOut* s,
             uint32_t n);
  // The sections' bytes, in order, in as many calls as the caller likes.
  bool write(const void* data, uint32_t n);
  // Every section whole: writes the directory and then the header with its
  // CRC (last, at offset 0).
  bool finish(uint32_t* fileBytes = nullptr, uint32_t* headerCrc = nullptr);
  uint32_t fileBytes() const { return fileBytes_; }

private:
  Sink* sink_ = nullptr;
  FileMeta meta_;
  uint8_t head_[256];  // the type header, at most 216 bytes
  uint32_t headerBytes_ = 0;
  SectionOut s_[kMaxSections];
  uint32_t offset_[kMaxSections] = {};
  uint32_t crc_[kMaxSections] = {};
  uint32_t n_ = 0;
  uint32_t cur_ = 0;      // the section being written
  uint32_t written_ = 0;  // its bytes so far
  uint32_t fileBytes_ = 0;
  bool bad_ = false;
};

// ---------------------------------------------------------------------------
// Which root counts (2.5.4), and which plan (2.12.5): manifest.bin against
// manifest.tmp (pending.bin against pending.tmp). Both valid: the higher
// generation, the .bin on a tie; one valid: it; neither: none. Two valid
// roots with the same commitId and headerCrc are one commit seen twice (a
// rename cut between its two directory writes): readers use either, and
// the software deletes neither before a disk check.
// ---------------------------------------------------------------------------
enum class Pick : uint8_t { None, Bin, Tmp };
struct RootCandidate {
  bool valid = false;
  uint32_t generation = 0;
  uint64_t commitId = 0;  // MSMF's commitId (a plan's runId)
  uint32_t headerCrc = 0;
};
struct Election {
  Pick pick = Pick::None;
  bool sameCommit = false;
};
Election elect(const RootCandidate& bin, const RootCandidate& tmp);

// The software's guard (2.4.5): the first bytes of a contract-named file in
// /.mstream carry one of the four magics with a major other than 1, so the
// software MUST NOT write to the card. Checked on the magic and the major
// alone, before any CRC. `n` < 6: false (nothing to say).
bool unknownMajor(const uint8_t* head, size_t n);

}  // namespace cardcontract
