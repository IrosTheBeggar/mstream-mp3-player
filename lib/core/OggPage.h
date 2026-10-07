// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "TrackSeek.h"  // trackseek::FileReader: the file's bytes

// Ogg pages (RFC 3533), the container an Opus file comes in (docs/OPUS.md):
// the page level only, with nothing of the codec in it. OggOpus builds the
// track on top of it.
//
// A page: "OggS", the version byte (0), the flags (continued: the page
// starts in the middle of a packet; bos: the stream's first page; eos: its
// last), the granule position (8 bytes, little-endian, signed; -1: no
// packet completes on this page), the stream's serial number, the page's
// sequence number (consecutive within a stream), a CRC-32 over the whole
// page with its own field zeroed (polynomial 0x04C11DB7, initial 0, no
// reflection and no final xor: Ogg's own, not zlib's), the segment count
// and the lacing table, one byte per segment: a packet's bytes go into
// 255-byte segments and a last one under 255 (0 when the packet is a
// multiple of 255). A table that ends on a 255 continues its packet on the
// next page, which carries the continued flag. A page can have no
// segments at all. The largest page is 27 + 255 + 255 x 255 = 65,307
// bytes.
//
// PageReader reads pages through a FileReader into a buffer the caller
// owns (the firmware's: PSRAM, kMaxPageBytes), one page at a time, the CRC
// checked before any byte of it is believed: read() for a whole page,
// readHeader() where only the header matters (skipping OpusTags, the tail
// scan: 27 + 255 bytes, one read), find() and findHeader() for the next
// page after a damaged one (a scan for the capture pattern; beginScan()
// and findStep() make the same scan one read at a time, for the decode
// task's pass budget). No stack buffer over 256 B: the scans read into the
// page buffer, and the header reads into the reader's own 290 bytes.
// Portable, host-tested (test_ogg_opus).
namespace ogg {

constexpr uint32_t kHeaderBytes = 27;                              // before the lacing table
constexpr uint32_t kMaxSegments = 255;
constexpr uint32_t kMaxHeaderBytes = kHeaderBytes + kMaxSegments;  // 282
constexpr uint32_t kMaxPageBytes = kMaxHeaderBytes + 255 * 255;    // 65,307
constexpr uint8_t kContinued = 1;
constexpr uint8_t kBos = 2;
constexpr uint8_t kEos = 4;

// Ogg's CRC-32 over `n` bytes, continued from `crc` (0 to start; the check
// value of "123456789" is 0x89A1897F). A 1 KB table in flash.
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0);

struct Header {
  uint8_t flags = 0;
  int64_t granule = -1;
  uint32_t serial = 0;
  uint32_t sequence = 0;
  uint32_t crc = 0;
  uint8_t segments = 0;
  bool continues = false;    // the table ends on a 255: its last packet goes on in the next page
  uint32_t headerBytes = 0;  // 27 + segments
  uint32_t bodyBytes = 0;    // the lacing values summed
  uint32_t bytes() const { return headerBytes + bodyBytes; }
  bool continued() const { return (flags & kContinued) != 0; }
  bool bos() const { return (flags & kBos) != 0; }
  bool eos() const { return (flags & kEos) != 0; }
};

// "OggS" at p (4 bytes).
bool isCapture(const uint8_t* p);
// The header at buf[0..n): the capture pattern, version 0, and all of its
// lacing table in the buffer. False: not a page header, or not all of it
// is here.
bool parseHeader(const uint8_t* buf, size_t n, Header* out);
// The CRC of the whole page at `page` (h.bytes() long) against its field.
bool crcOk(const uint8_t* page, const Header& h);
// The first "OggS" at or after `from` in buf[0..n): its offset; -1: none.
int32_t findCapture(const uint8_t* buf, size_t n, size_t from);

// The lacing table read: for each lacing value, whether it completes a
// packet (under 255), and where the last completing one is (-1: none: a
// page that only continues a packet, or has no segments).
int32_t lastCompleting(const uint8_t* lacing, uint32_t segments);

// A page as PageReader read it: its header, where it is in the file, and
// its lacing table and body in the page buffer (valid until the next read
// into that buffer).
struct Page {
  Header h;
  uint32_t offset = 0;
  const uint8_t* lacing = nullptr;
  const uint8_t* body = nullptr;
  uint32_t end() const { return offset + h.bytes(); }
};

class PageReader {
public:
  // How a read ended: a page in the buffer with its CRC right; no page
  // header there; the file ends inside the page (a truncated file); the
  // page's CRC is wrong (a damaged page: nothing of it is believed); a
  // slice of the page is in and the rest is still to read (read() with a
  // slice: call again with the same offset).
  enum class Read : uint8_t { Ok, NotPage, Short, BadCrc, Partial };
  static const char* readName(Read r);

  // The bytes the header reads peek at after the lacing table (OggOpus
  // checks "OpusTags" without reading the page).
  static constexpr uint32_t kPeekBytes = 8;
  // What the scans read at a time (into the page buffer).
  static constexpr uint32_t kScanChunk = 4096;

  // `pageBuf`: kMaxPageBytes, the caller's.
  PageReader(trackseek::FileReader& file, uint32_t fileSize, uint8_t* pageBuf)
      : file_(file), size_(fileSize), buf_(pageBuf) {}

  uint32_t fileSize() const { return size_; }
  uint8_t* buffer() const { return buf_; }

  // The whole page at `offset`, CRC checked: at most two reads (the
  // header's 282 bytes, then the rest). With `slice` (bytes, 0: none) the
  // body is read at most that much a call: Partial until the page is
  // whole, and the next call with the same offset reads on from where it
  // stopped (a 64 KB page of a 510k file is eight 8 KB reads, each a
  // call, so the decode task's pass can end between them: docs/OPUS.md
  // gate G6). Any other use of the buffer in between (a scan, a chunk, a
  // page at another offset) starts the page over; the CRC is checked once
  // it is whole, as ever.
  Read read(uint32_t offset, Page* out, uint32_t slice = 0);
  // The header at `offset` only (one read of 290 bytes: the header, the
  // lacing table and up to kPeekBytes of the body, peek()).
  Read readHeader(uint32_t offset, Header* out);
  // After readHeader(): its lacing table, and the body's first bytes.
  const uint8_t* lacing() const { return hdr_ + kHeaderBytes; }
  const uint8_t* peek() const { return peek_; }
  uint32_t peekBytes() const { return peekBytes_; }

  // The first page at or after `from` that starts within `limit` bytes,
  // reads whole and checks (its CRC right), and is of `serial` unless
  // `anySerial`: a scan for "OggS" in chunks of `chunk` bytes (at most
  // kMaxPageBytes). A candidate whose header is in the chunk is judged
  // there first (the version, the serial), and one that lies in the
  // chunk whole has its CRC checked there too, so a false "OggS" in the
  // audio and a crafted header of another stream cost no read; any other
  // candidate is read whole (the chunk again after it when it fails).
  // `budget` (0: none) is the most bytes the scan may read in all before
  // it gives up: a file of crafted headers (the right version and serial,
  // a wrong CRC, 65 KB claimed each) would otherwise cost a page's read
  // per 282 bytes of file. False: none.
  bool find(uint32_t from, uint32_t limit, uint32_t serial, bool anySerial, Page* out, uint32_t chunk = kScanChunk,
            uint64_t budget = 0);
  // find() a step at a time, for a caller whose time is budgeted: the
  // resync after a damaged page while a track plays (OggOpus's
  // Reader::next(), which says Pending between the steps so the decode
  // task's pass can end there; docs/OPUS.md section 8.11: before this a
  // 54 KB resync and the 64 KB page after it were one 139 ms step).
  // beginScan() takes find()'s arguments; each findStep() then makes one
  // read: a chunk of the file, scanned in memory as find() scans it, or,
  // for a candidate page that reaches past its chunk, the page's own read
  // (with `slice`, as read() takes it, a slice of the page a step). Page:
  // found, in *out, the scan over; More: call again (any other use of the
  // buffer in between, a read() or a scan, loses a candidate's slices so
  // far: the step then reads it again); None: nothing of `serial` within
  // the limit or the budget, the scan over. find() is the steps run to
  // the end in one call. ~40 B: a member of the caller's.
  struct Scan {
    bool active = false;
    bool reading = false;    // a candidate page's own read is under way (at `candidate`)
    bool anySerial = false;
    uint32_t at = 0;         // the next chunk's offset
    uint32_t stop = 0;       // the scan's end: from + limit, or the file's
    uint32_t serial = 0;
    uint32_t chunk = kScanChunk;
    uint32_t candidate = 0;
    uint64_t budget = 0;     // bytes the scan may read in all (0: no bound)
    uint64_t spent = 0;      // ... and has so far
  };
  enum class Found : uint8_t { Page, More, None };
  void beginScan(uint32_t from, uint32_t limit, uint32_t serial, bool anySerial, Scan* out,
                 uint32_t chunk = kScanChunk, uint64_t budget = 0) const;
  Found findStep(Scan* s, Page* out, uint32_t slice = 0);
  // The same by headers only (no CRC; the page must end within the file):
  // `offset` is where it starts. For a tail scan, where the pages are
  // walked by their headers and only the one that matters is checked.
  // `budget` as find()'s.
  bool findHeader(uint32_t from, uint32_t limit, Header* out, uint32_t* offset, uint32_t chunk = kScanChunk,
                  uint64_t budget = 0);
  // findHeader() over the chunk in hand alone, no read: the first page
  // header at or after `from` (inside the chunk) and before `stop` whose
  // header lies in the chunk whole and whose page ends within the file.
  // False: none there; `*offset` is then where a scan of the file should
  // go on from (a capture pattern cut by the chunk's end, else the
  // chunk's end less 3), so a walk over junk (a tail of crafted headers)
  // reads each chunk once instead of once per junk byte.
  bool findHeaderInChunk(uint32_t from, uint32_t stop, Header* out, uint32_t* offset) const;

  // The chunk of the file the page buffer holds after a scan (find(),
  // findHeader()) or readChunk(): where it starts and ends (none after
  // read() took the buffer for a page). headerInChunk() parses the header
  // at `offset` from it, with no read, when all of it is there (a walk by
  // headers over small pages: a tail of 40-byte pages, one tiny packet
  // each, would cost a 290-byte read per page otherwise). False: not in
  // the chunk whole, or not a page header.
  uint32_t readChunk(uint32_t offset, uint32_t n);
  uint32_t chunkAt() const { return chunkAt_; }
  uint32_t chunkEnd() const { return chunkGot_ ? chunkAt_ + chunkGot_ : 0; }
  bool headerInChunk(uint32_t offset, Header* out) const;
  // The whole page at `offset` from the chunk, with no read, when all of it
  // lies there: its header parsed and its CRC checked in memory, `out`
  // pointing into the chunk (valid until the buffer's next use). A tail
  // window read as one chunk holds the file's last page whole, so the tail
  // scan needs no read to check it (docs/OPUS.md section 10). False: not
  // in the chunk whole, not a page header, or a wrong CRC.
  bool pageInChunk(uint32_t offset, Page* out) const;

  // ---- for the log ----
  uint32_t reads() const { return reads_; }
  uint64_t bytesRead() const { return bytes_; }

private:
  uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n);
  // The header at buf[0..got) (a read of `got` bytes at `offset`).
  Read parseAt(const uint8_t* buf, uint32_t got, Header* out);

  trackseek::FileReader& file_;
  uint32_t size_;
  uint8_t* buf_;
  uint8_t hdr_[kMaxHeaderBytes + kPeekBytes] = {};
  const uint8_t* peek_ = nullptr;
  uint32_t peekBytes_ = 0;
  uint32_t chunkAt_ = 0;   // the chunk in buf_ (chunkGot_ 0: none)
  uint32_t chunkGot_ = 0;
  // A page read in slices: the one at partAt_, partGot_ bytes of it in
  // buf_ so far with its header parsed (partGot_ 0: none in progress).
  uint32_t partAt_ = 0;
  uint32_t partGot_ = 0;
  Header partHdr_;
  uint32_t reads_ = 0;
  uint64_t bytes_ = 0;
};

}  // namespace ogg
