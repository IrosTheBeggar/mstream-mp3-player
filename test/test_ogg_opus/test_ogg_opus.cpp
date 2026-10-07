// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for OggPage and OggOpus (docs/OPUS.md): Ogg's CRC and page
// headers, the page reader, OpusHead's edges, the TOC's sample counts and
// the frame split, and the track reader with its timeline (g0, the
// pre-skip, the EOS trim, packets spanning pages, damaged and lost pages
// and the gap fill that keeps the timeline exact after them, foreign and
// chained streams and a chained file's exact length, truncated files, the
// tail scan, the start plan's bisection at every packet boundary of a 60 s
// file with its preroll and the tail rule, the resume anchor's check, the
// bounds on what crafted files can make the scans read, the refusal of
// frames under 10 ms, the audio pages read in slices, the resync after a
// damaged page made one read a call and the concealment of malformed
// packets) on synthetic files from test/support/OggWriter.h.
// Run: pio test -e native -f test_ogg_opus
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../support/OggWriter.h"
#include "OggOpus.h"
#include "OggPage.h"

using oggwriter::Bytes;

namespace {

uint8_t pageBuf[ogg::kMaxPageBytes];
uint8_t packetBuf[oggopus::kMaxPacketBytes];

// A real file's first page (ffmpeg, mStream's arguments): OpusHead,
// version 1, 2 channels, pre-skip 312, 48 kHz, gain 0, family 0; CRC
// 0x242DF34B.
const char* kRealHeadPage =
    "4f6767530002000000000000000014eaa4d8000000004bf32d2401134f707573486561640102380180bb0000000000";

Bytes fromHex(const char* hex) {
  Bytes b;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    b.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
  }
  return b;
}

// The reader driven as the generator drives it: each packet "decoded"
// frame by frame (its TOC's samples per frame, no decoder), a gap filled
// before the packet after it (as the generator's concealment and silence
// would be), a malformed packet with a readable TOC concealed for its
// duration and one without a TOC dropped (the page's granule puts the
// count right), packets before a plan's preroll skipped undecoded, the
// timeline's keeps summed, and the kept samples' trimmed indices checked
// for contiguity.
struct Run {
  uint64_t kept = 0;
  int64_t firstT = -1;  // the first kept sample's trimmed index
  int64_t lastT = -1;   // the last's
  uint32_t packets = 0;
  uint32_t jumps = 0;   // kept stretches that don't follow on (a damaged page)
  uint32_t gaps = 0;    // packets flagged gapBefore
  uint64_t filled = 0;  // samples made for gaps (and for malformed packets concealed)
  uint32_t malformed = 0;  // packets with a readable TOC but bad framing: concealed
  uint32_t dropped = 0;    // packets with no TOC to size them by: dropped
  uint32_t skipped = 0; // packets skipped undecoded (before a plan's preroll)
  uint32_t pending = 0; // next() calls that gave no packet yet (their share of pages spent, or a slice of a page read)
  uint32_t maxCallReads = 0;   // the most reads one next() call made
  uint32_t maxCallBytes = 0;   // ... and the most bytes (a slice and a header, or a scan's chunk, when sliced)
  uint64_t decodedBefore = 0;  // samples decoded (or filled) before the first kept one
  bool gapEnded = false;       // a gap over kMaxGapSamples ended it
  bool finished = false;
  oggopus::Reader::End end = oggopus::Reader::End::None;
  oggopus::Timeline tl;
};

void keep(Run& run, uint32_t n) {
  const int64_t pos = run.tl.position();
  const oggopus::Timeline::Keep k = run.tl.decoded(n);
  if (run.firstT < 0) run.decodedBefore += k.skip;
  if (k.take == 0) return;
  const int64_t t0 = pos + k.skip;
  if (run.firstT < 0) run.firstT = t0;
  if (run.lastT >= 0 && t0 != run.lastT + 1) ++run.jumps;
  run.lastT = t0 + k.take - 1;
  run.kept += k.take;
}

// `stopAfterKept`: stop once this many samples are kept (0: the whole
// track), for a plan's start.
Run play(oggopus::Reader& r, bool perFrame = true, const oggopus::StartPlan* plan = nullptr, uint64_t stopAfterKept = 0) {
  Run run;
  if (plan) {
    run.tl.start(*plan);
  } else {
    run.tl.start(r.g0(), r.head().preSkip);
  }
  oggopus::Reader::Packet p;
  oggopus::Frames fr;
  for (;;) {
    const uint32_t reads0 = r.pages().reads();
    const uint64_t bytes0 = r.pages().bytesRead();
    const oggopus::Reader::Next n = r.next(&p);
    const uint32_t callReads = r.pages().reads() - reads0;
    const auto callBytes = static_cast<uint32_t>(r.pages().bytesRead() - bytes0);
    if (callReads > run.maxCallReads) run.maxCallReads = callReads;
    if (callBytes > run.maxCallBytes) run.maxCallBytes = callBytes;
    if (n == oggopus::Reader::Next::End) break;
    if (n == oggopus::Reader::Next::Pending) {
      ++run.pending;  // (the generator ends its pass here and asks again next pass)
      continue;
    }
    ++run.packets;
    if (p.gapBefore) ++run.gaps;
    run.tl.packet(p);
    const bool framed = p.samples > 0 && oggopus::splitPacket(p.data, p.bytes, &fr);
    if (p.samples < 0) {
      // No TOC to size it by (0 bytes, an impossible count): dropped, the
      // page's granule puts the count right at the page's end.
      ++run.dropped;
      run.tl.packetDone();
      continue;
    }
    const int64_t gap = run.tl.gap();
    if (gap > oggopus::kMaxGapSamples) {
      run.gapEnded = true;
      break;
    }
    // The fill, a frame's worth at a time (as the generator's buffer has
    // it); a malformed packet's TOC duration is concealed the same way,
    // in one fill with the gap before it.
    int64_t fill = gap;
    if (!framed) {
      ++run.malformed;
      fill += p.samples;
    }
    for (int64_t left = fill; left > 0;) {
      const uint32_t n = left > oggopus::kMaxFrameSamples ? oggopus::kMaxFrameSamples : static_cast<uint32_t>(left);
      keep(run, n);
      run.filled += n;
      left -= n;
    }
    if (!framed) {
      run.tl.packetDone();
      continue;
    }
    if (!run.tl.wanted(static_cast<uint32_t>(p.samples))) {
      run.tl.skipped(static_cast<uint32_t>(p.samples));
      ++run.skipped;
      run.tl.packetDone();
      continue;
    }
    if (perFrame) {
      for (uint32_t i = 0; i < fr.count; ++i) keep(run, oggopus::frameSamples(fr.toc));
    } else {
      keep(run, static_cast<uint32_t>(p.samples));
    }
    run.tl.packetDone();
    if (stopAfterKept && run.kept >= stopAfterKept) break;
  }
  run.finished = run.tl.finished();
  run.end = r.ended();
  return run;
}

// The next packet by hand, the Pendings stepped through (a slice of a page,
// a step of the scan after a damaged one: the generator's pass ends there
// and asks again).
oggopus::Reader::Next nextPacket(oggopus::Reader& r, oggopus::Reader::Packet* p) {
  for (;;) {
    const oggopus::Reader::Next n = r.next(p);
    if (n != oggopus::Reader::Next::Pending) return n;
  }
}

// The usual file: 100 packets of 20 ms, 50 a page (2 audio pages), pre-skip
// 312, 648 samples trimmed at the end: 95,040 samples kept.
oggwriter::FileSpec usualSpec() {
  oggwriter::FileSpec f;
  f.packets = oggwriter::celtPackets(100);
  f.endTrim = 648;
  return f;
}
constexpr uint64_t kUsualKept = 100 * 960 - 648 - 312;

}  // namespace

void setUp() {}
void tearDown() {}

// ---- the page level ----

// Ogg's CRC-32: the check value, a real page, and the field zeroed.
void test_crc() {
  const uint8_t check[] = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0x89A1897F, ogg::crc32(check, 9));
  // Continued in parts.
  TEST_ASSERT_EQUAL_HEX32(0x89A1897F, ogg::crc32(check + 4, 5, ogg::crc32(check, 4)));
  Bytes page = fromHex(kRealHeadPage);
  TEST_ASSERT_EQUAL_UINT32(47, page.size());
  ogg::Header h;
  TEST_ASSERT_TRUE(ogg::parseHeader(page.data(), page.size(), &h));
  TEST_ASSERT_EQUAL_HEX32(0x242DF34B, h.crc);
  TEST_ASSERT_TRUE(ogg::crcOk(page.data(), h));
  page[30] ^= 1;  // a body byte
  TEST_ASSERT_FALSE(ogg::crcOk(page.data(), h));
  page[30] ^= 1;
  page[5] ^= 1;  // a header byte
  TEST_ASSERT_TRUE(ogg::parseHeader(page.data(), page.size(), &h));
  TEST_ASSERT_FALSE(ogg::crcOk(page.data(), h));
}

// The header's fields; version 1, a short buffer and a cut lacing table
// are refused; a page with no segments is a page.
void test_page_header() {
  oggwriter::PageSpec s;
  s.serial = 0xDEADBEEF;
  s.sequence = 77;
  s.granule = -1;
  s.flags = ogg::kContinued | ogg::kEos;
  s.lacing = {255, 255, 10};
  s.body.resize(520, 7);
  Bytes b = oggwriter::page(s);
  ogg::Header h;
  TEST_ASSERT_TRUE(ogg::parseHeader(b.data(), b.size(), &h));
  TEST_ASSERT_TRUE(h.continued());
  TEST_ASSERT_FALSE(h.bos());
  TEST_ASSERT_TRUE(h.eos());
  TEST_ASSERT_EQUAL_INT64(-1, h.granule);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, h.serial);
  TEST_ASSERT_EQUAL_UINT32(77, h.sequence);
  TEST_ASSERT_EQUAL_UINT8(3, h.segments);
  TEST_ASSERT_EQUAL_UINT32(30, h.headerBytes);
  TEST_ASSERT_EQUAL_UINT32(520, h.bodyBytes);
  TEST_ASSERT_EQUAL_UINT32(550, h.bytes());
  TEST_ASSERT_FALSE(h.continues);
  TEST_ASSERT_TRUE(ogg::crcOk(b.data(), h));
  TEST_ASSERT_EQUAL_INT32(2, ogg::lastCompleting(b.data() + 27, 3));
  // A table ending on a 255: its last packet goes on in the next page.
  s.lacing = {10, 255};
  s.body.resize(265, 7);
  const Bytes open = oggwriter::page(s);
  TEST_ASSERT_TRUE(ogg::parseHeader(open.data(), open.size(), &h));
  TEST_ASSERT_TRUE(h.continues);
  TEST_ASSERT_EQUAL_INT32(0, ogg::lastCompleting(open.data() + 27, 2));
  s.lacing = {255, 255, 10};
  s.body.resize(520, 7);
  // Only the header's 27 bytes: the lacing table isn't there.
  TEST_ASSERT_FALSE(ogg::parseHeader(b.data(), 29, &h));
  TEST_ASSERT_TRUE(ogg::parseHeader(b.data(), 30, &h));
  TEST_ASSERT_FALSE(ogg::parseHeader(b.data(), 26, &h));
  b[4] = 1;  // the version
  TEST_ASSERT_FALSE(ogg::parseHeader(b.data(), b.size(), &h));
  b[4] = 0;
  b[0] = 'X';
  TEST_ASSERT_FALSE(ogg::parseHeader(b.data(), b.size(), &h));
  TEST_ASSERT_EQUAL_INT32(-1, ogg::findCapture(b.data(), b.size(), 0));
  b[0] = 'O';
  TEST_ASSERT_EQUAL_INT32(0, ogg::findCapture(b.data(), b.size(), 0));
  TEST_ASSERT_EQUAL_INT32(-1, ogg::findCapture(b.data(), b.size(), 1));
  // No segments.
  s.lacing.clear();
  s.body.clear();
  s.granule = 48000;
  b = oggwriter::page(s);
  TEST_ASSERT_EQUAL_UINT32(27, b.size());
  TEST_ASSERT_TRUE(ogg::parseHeader(b.data(), b.size(), &h));
  TEST_ASSERT_EQUAL_UINT8(0, h.segments);
  TEST_ASSERT_EQUAL_UINT32(0, h.bodyBytes);
  TEST_ASSERT_EQUAL_INT64(48000, h.granule);
  TEST_ASSERT_TRUE(ogg::crcOk(b.data(), h));
  TEST_ASSERT_EQUAL_INT32(-1, ogg::lastCompleting(b.data() + 27, 0));
}

// PageReader: a page read whole and checked, no page, a page cut by the
// file's end, a bad CRC; the header-only read and its peek; the scans.
void test_page_reader() {
  oggwriter::Built f = oggwriter::build(usualSpec());
  TEST_ASSERT_EQUAL_UINT32(4, f.pageOffsets.size());
  oggwriter::Reader file(f.bytes);
  ogg::PageReader pr(file, file.size(), pageBuf);
  ogg::Page pg;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.read(0, &pg));
  TEST_ASSERT_TRUE(pg.h.bos());
  TEST_ASSERT_EQUAL_UINT32(0, pg.offset);
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[1], pg.end());
  TEST_ASSERT_EQUAL_UINT8(1, pg.h.segments);
  TEST_ASSERT_EQUAL_MEMORY("OpusHead", pg.body, 8);
  TEST_ASSERT_EQUAL_UINT32(1, file.reads);  // a small page: the header's read held it all
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::NotPage, pr.read(1, &pg));
  // An audio page: two reads.
  file.reads = 0;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.read(f.pageOffsets[2], &pg));
  TEST_ASSERT_EQUAL_UINT32(2, file.reads);
  TEST_ASSERT_EQUAL_UINT8(100, pg.h.segments);  // 301-byte packets: two segments each
  TEST_ASSERT_EQUAL_INT64(48000, pg.h.granule);
  // The header only, with the body's first bytes.
  ogg::Header h;
  file.reads = 0;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.readHeader(f.pageOffsets[1], &h));
  TEST_ASSERT_EQUAL_UINT32(1, file.reads);
  TEST_ASSERT_EQUAL_UINT32(8, pr.peekBytes());
  TEST_ASSERT_EQUAL_MEMORY("OpusTags", pr.peek(), 8);
  TEST_ASSERT_EQUAL_UINT8(1, h.segments);
  TEST_ASSERT_TRUE(pr.lacing()[0] < 255);
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::NotPage, pr.readHeader(3, &h));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Short, pr.readHeader(file.size(), &h));
  // Cut inside the last page.
  oggwriter::Reader cut(f.bytes, f.pageOffsets[3] + 1000);
  ogg::PageReader prc(cut, cut.size(), pageBuf);
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Short, prc.read(f.pageOffsets[3], &pg));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, prc.read(f.pageOffsets[2], &pg));
  // A damaged page.
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[2] + 100] ^= 0xFF;
  oggwriter::Reader dfile(damaged);
  ogg::PageReader prd(dfile, dfile.size(), pageBuf);
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::BadCrc, prd.read(f.pageOffsets[2], &pg));
  // The scans: from byte 1, the tags page is the next one (small: judged
  // and checked inside the chunk, one read); by serial.
  file.reads = 0;
  TEST_ASSERT_TRUE(pr.find(1, file.size(), 0, true, &pg));
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[1], pg.offset);
  TEST_ASSERT_EQUAL_UINT32(1, file.reads);
  TEST_ASSERT_EQUAL_MEMORY("OpusTags", pg.body, 8);
  TEST_ASSERT_TRUE(pr.find(1, file.size(), 0x1234, false, &pg));
  TEST_ASSERT_FALSE(pr.find(1, file.size(), 0x9999, false, &pg));
  TEST_ASSERT_FALSE(pr.find(1, 10, 0, true, &pg));  // not within 10 bytes
  // A big page (15 KB, past the chunk) is read whole; a read budget too
  // small for the chunks on the way gives up.
  file.reads = 0;
  file.bytes = 0;
  TEST_ASSERT_TRUE(pr.find(f.pageOffsets[2] + 1, file.size(), 0x1234, false, &pg));
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], pg.offset);
  TEST_ASSERT_TRUE(file.reads <= 8);
  TEST_ASSERT_FALSE(pr.find(f.pageOffsets[2] + 1, file.size(), 0x1234, false, &pg, ogg::PageReader::kScanChunk, 4096));
  uint32_t at = 0;
  TEST_ASSERT_TRUE(pr.findHeader(f.pageOffsets[2] + 1, file.size(), &h, &at));
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], at);
  TEST_ASSERT_TRUE(h.eos());
  TEST_ASSERT_FALSE(pr.findHeader(f.pageOffsets[2] + 1, file.size(), &h, &at, ogg::PageReader::kScanChunk, 4096));
  // The damaged page is stepped over by the scan (its CRC), to the next.
  TEST_ASSERT_TRUE(prd.find(f.pageOffsets[2], dfile.size(), 0x1234, false, &pg));
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], pg.offset);
  // The same scan one read a step (beginScan(), findStep(): the resync
  // while a track plays). From inside the first audio page to the EOS page
  // (15 KB, past its chunk): four 4 KB chunks over the page, the candidate
  // found in the fourth, then, with a 4 KB slice, the EOS page's header and
  // first slice in one step and a slice a step after that (the body's
  // 14,895 bytes are four slices): eight steps, none reading more than a
  // slice and a header, and the scan over once it is found.
  {
    using Found = ogg::PageReader::Found;
    ogg::PageReader::Scan s;
    pr.beginScan(f.pageOffsets[2] + 1, file.size(), 0x1234, false, &s);
    TEST_ASSERT_TRUE(s.active);
    uint32_t steps = 0, maxBytes = 0, maxReads = 0;
    Found found = Found::More;
    while (found == Found::More) {
      const uint32_t reads0 = file.reads;
      const uint64_t bytes0 = file.bytes;
      found = pr.findStep(&s, &pg, 4096);
      const uint32_t reads = file.reads - reads0;
      const auto bytes = static_cast<uint32_t>(file.bytes - bytes0);
      if (reads > maxReads) maxReads = reads;
      if (bytes > maxBytes) maxBytes = bytes;
      ++steps;
    }
    TEST_ASSERT_EQUAL(Found::Page, found);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], pg.offset);
    TEST_ASSERT_TRUE(pg.h.eos());
    TEST_ASSERT_EQUAL_UINT32(8, steps);
    TEST_ASSERT_EQUAL_UINT32(2, maxReads);
    TEST_ASSERT_TRUE(maxBytes <= 4096 + ogg::kMaxHeaderBytes);
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_EQUAL(Found::None, pr.findStep(&s, &pg, 4096));  // over: nothing more is read
    // No slice: the candidate is read whole in its one step (five steps).
    pr.beginScan(f.pageOffsets[2] + 1, file.size(), 0x1234, false, &s);
    steps = 0;
    for (found = Found::More; found == Found::More; ++steps) found = pr.findStep(&s, &pg);
    TEST_ASSERT_EQUAL(Found::Page, found);
    TEST_ASSERT_EQUAL_UINT32(5, steps);
    // A read budget of one chunk: the first step reads it, the second gives
    // up before reading anything more.
    pr.beginScan(f.pageOffsets[2] + 1, file.size(), 0x1234, false, &s, ogg::PageReader::kScanChunk, 4096);
    file.bytes = 0;
    TEST_ASSERT_EQUAL(Found::More, pr.findStep(&s, &pg));
    TEST_ASSERT_EQUAL(Found::None, pr.findStep(&s, &pg));
    TEST_ASSERT_EQUAL_UINT64(4096, file.bytes);
    TEST_ASSERT_FALSE(s.active);
    // The limit: nothing within 10 bytes.
    pr.beginScan(1, 10, 0, true, &s);
    TEST_ASSERT_EQUAL(Found::None, pr.findStep(&s, &pg));
    // The damaged page in steps: its candidate read (whole, no slice) fails
    // its CRC, the scan goes on from the byte after its capture pattern
    // and finds the EOS page.
    prd.beginScan(f.pageOffsets[2], dfile.size(), 0x1234, false, &s);
    for (found = Found::More; found == Found::More;) found = prd.findStep(&s, &pg);
    TEST_ASSERT_EQUAL(Found::Page, found);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], pg.offset);
  }
  // A page read in slices: the 15 KB audio page in 4 KB slices is the
  // header's read, then a slice a call (Partial until the last), the same
  // bytes read once; the body's first slice comes with the header's call.
  file.reads = 0;
  file.bytes = 0;
  const uint32_t pageBytes = f.pageOffsets[3] - f.pageOffsets[2];
  TEST_ASSERT_TRUE(pageBytes > 12 * 1024 && pageBytes < 16 * 1024);
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, pr.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL_UINT32(2, file.reads);
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, pr.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, pr.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL_UINT32(5, file.reads);
  TEST_ASSERT_EQUAL_UINT64(pageBytes, file.bytes);
  TEST_ASSERT_EQUAL_UINT8(100, pg.h.segments);
  TEST_ASSERT_EQUAL_INT64(48000, pg.h.granule);
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[2], pg.offset);
  // A chunk read (a scan) between two slices takes the buffer: the page
  // starts over; so does a read at another offset.
  file.reads = 0;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, pr.read(f.pageOffsets[2], &pg, 4096));
  pr.readChunk(0, 1000);
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, pr.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL_UINT32(5, file.reads);  // the header and a slice, the chunk, the header and a slice again
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.read(f.pageOffsets[1], &pg, 4096));  // (small: whole in one)
  file.reads = 0;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, pr.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL_UINT32(2, file.reads);
  // A slice as big as the rest finishes the page in hand in one read; a
  // fresh page with no slice is whole in two, as before.
  file.reads = 0;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.read(f.pageOffsets[2], &pg, 65536));
  TEST_ASSERT_EQUAL_UINT32(1, file.reads);
  file.reads = 0;
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Ok, pr.read(f.pageOffsets[3], &pg));
  TEST_ASSERT_EQUAL_UINT32(2, file.reads);
  // The damaged page: its CRC is judged once it is whole; the cut file's
  // page is Short at the slice that runs past the end.
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, prd.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, prd.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Partial, prd.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::BadCrc, prd.read(f.pageOffsets[2], &pg, 4096));
  TEST_ASSERT_EQUAL(ogg::PageReader::Read::Short, prc.read(f.pageOffsets[3], &pg, 4096));
}

// ---- the Opus level ----

void test_codec_sniff() {
  const uint8_t vorbis[] = {1, 'v', 'o', 'r', 'b', 'i', 's', 0};
  const uint8_t flac[] = {0x7F, 'F', 'L', 'A', 'C', 1, 0};
  const uint8_t speex[] = "Speex   1.2";
  const uint8_t theora[] = {0x80, 't', 'h', 'e', 'o', 'r', 'a', 3};
  const uint8_t opus[] = "OpusHead";
  TEST_ASSERT_EQUAL(oggopus::Codec::Vorbis, oggopus::codecOf(vorbis, sizeof(vorbis)));
  TEST_ASSERT_EQUAL(oggopus::Codec::Flac, oggopus::codecOf(flac, sizeof(flac)));
  TEST_ASSERT_EQUAL(oggopus::Codec::Speex, oggopus::codecOf(speex, 11));
  TEST_ASSERT_EQUAL(oggopus::Codec::Theora, oggopus::codecOf(theora, sizeof(theora)));
  TEST_ASSERT_EQUAL(oggopus::Codec::Opus, oggopus::codecOf(opus, 8));
  TEST_ASSERT_EQUAL(oggopus::Codec::Other, oggopus::codecOf(opus, 7));
  TEST_ASSERT_EQUAL(oggopus::Codec::Other, oggopus::codecOf(opus, 0));
  TEST_ASSERT_EQUAL_STRING("Vorbis", oggopus::codecName(oggopus::Codec::Vorbis));
}

// OpusHead: the real one; versions 15 and 16; an odd pre-skip; family 0
// with 3 channels; family 1 with one stream and a swapped table (and the
// tables putting one channel on both sides), with 2 uncoupled streams,
// 5.1; family 255; a short head; a table naming a channel the stream
// doesn't have; 0 channels; a mono head with a gain.
void test_opus_head() {
  using oggopus::HeadCheck;
  oggopus::Head h;
  const Bytes real = fromHex(kRealHeadPage);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(real.data() + 28, 19, &h));
  TEST_ASSERT_EQUAL_UINT8(1, h.version);
  TEST_ASSERT_EQUAL_UINT8(2, h.channels);
  TEST_ASSERT_EQUAL_UINT16(312, h.preSkip);
  TEST_ASSERT_EQUAL_UINT32(48000, h.inputRate);
  TEST_ASSERT_EQUAL_INT16(0, h.gain);
  TEST_ASSERT_EQUAL_UINT8(0, h.family);
  TEST_ASSERT_EQUAL_UINT8(1, h.streams);
  TEST_ASSERT_EQUAL_UINT8(1, h.coupled);
  TEST_ASSERT_FALSE(h.mapped());

  oggwriter::HeadSpec s;
  s.version = 15;
  Bytes b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  s.version = 16;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Version, oggopus::parseHead(b.data(), b.size(), &h));
  s = oggwriter::HeadSpec{};
  s.preSkip = 311;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_EQUAL_UINT16(311, h.preSkip);
  s = oggwriter::HeadSpec{};
  s.channels = 3;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Streams, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_EQUAL_UINT8(3, h.channels);
  // Family 1, one coupled stream, the table swapping the channels.
  s = oggwriter::HeadSpec{};
  s.family = 1;
  s.streams = 1;
  s.coupled = 1;
  s.map = {1, 0};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL_UINT32(23, b.size());
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_TRUE(h.mapped());
  TEST_ASSERT_EQUAL_UINT8(1, h.map[0]);
  TEST_ASSERT_EQUAL_UINT8(0, h.map[1]);
  s.map = {0, 1};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_FALSE(h.mapped());
  // The tables that put one decoded channel on both sides (RFC 7845
  // section 5.1.1 allows them): legal, and the generator follows the map.
  s.map = {0, 0};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_TRUE(h.mapped());
  TEST_ASSERT_EQUAL_UINT8(0, h.map[0]);
  TEST_ASSERT_EQUAL_UINT8(0, h.map[1]);
  s.map = {1, 1};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_TRUE(h.mapped());
  TEST_ASSERT_EQUAL_UINT8(1, h.map[0]);
  TEST_ASSERT_EQUAL_UINT8(1, h.map[1]);
  // Family 1 mono: one uncoupled stream.
  s.channels = 1;
  s.coupled = 0;
  s.map = {0};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  // Two uncoupled streams for 2 channels: two decoders.
  s.channels = 2;
  s.streams = 2;
  s.coupled = 0;
  s.map = {0, 1};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Streams, oggopus::parseHead(b.data(), b.size(), &h));
  // 5.1: 4 streams, 2 coupled.
  s.channels = 6;
  s.streams = 4;
  s.coupled = 2;
  s.map = {0, 4, 1, 2, 3, 5};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Streams, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_EQUAL_UINT8(6, h.channels);
  TEST_ASSERT_EQUAL_UINT8(4, h.streams);
  // Its table cut: short.
  s.truncate = 24;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Short, oggopus::parseHead(b.data(), b.size(), &h));
  // Family 255 (no mapping), and a reserved one.
  s = oggwriter::HeadSpec{};
  s.family = 255;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Family, oggopus::parseHead(b.data(), b.size(), &h));
  s.family = 2;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Family, oggopus::parseHead(b.data(), b.size(), &h));
  // A short head (18 bytes).
  s = oggwriter::HeadSpec{};
  s.truncate = 18;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Short, oggopus::parseHead(b.data(), b.size(), &h));
  // A table naming channel 255 (silence) or channel 2 of a 2-channel stream.
  s = oggwriter::HeadSpec{};
  s.family = 1;
  s.map = {0, 255};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Mapping, oggopus::parseHead(b.data(), b.size(), &h));
  s.map = {2, 0};
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Mapping, oggopus::parseHead(b.data(), b.size(), &h));
  // 0 channels.
  s = oggwriter::HeadSpec{};
  s.channels = 0;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Channels, oggopus::parseHead(b.data(), b.size(), &h));
  // Mono with a gain of -6 dB (Q7.8: -1536), as a signed field.
  s = oggwriter::HeadSpec{};
  s.channels = 1;
  s.gain = -1536;
  b = oggwriter::opusHead(s);
  TEST_ASSERT_EQUAL(HeadCheck::Ok, oggopus::parseHead(b.data(), b.size(), &h));
  TEST_ASSERT_EQUAL_UINT8(1, h.channels);
  TEST_ASSERT_EQUAL_UINT8(0, h.coupled);
  TEST_ASSERT_EQUAL_INT16(-1536, h.gain);
  TEST_ASSERT_FALSE(h.mapped());
}

// The TOC's sample counts for all 32 configurations and the four codes
// (code 3 with padding), and what is malformed.
void test_toc_samples() {
  for (uint32_t config = 0; config < 32; ++config) {
    uint32_t fs;
    if (config < 12) {
      const uint32_t t[4] = {480, 960, 1920, 2880};
      fs = t[config & 3];
    } else if (config < 16) {
      fs = (config & 1) ? 960 : 480;
    } else {
      fs = 120u << (config & 3);
    }
    const uint8_t toc = static_cast<uint8_t>(config << 3);
    TEST_ASSERT_EQUAL_UINT32(fs, oggopus::frameSamples(toc));
    TEST_ASSERT_EQUAL_UINT32(fs, oggopus::frameSamples(static_cast<uint8_t>(toc | 4)));  // the stereo bit
    Bytes p = oggwriter::opusPacket(static_cast<uint8_t>(config), true, 0, {40});
    TEST_ASSERT_EQUAL_INT32(1, oggopus::frameCount(p.data(), p.size()));
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(fs), oggopus::packetSamples(p.data(), p.size()));
    p = oggwriter::opusPacket(static_cast<uint8_t>(config), false, 1, {40, 40});
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(2 * fs), oggopus::packetSamples(p.data(), p.size()));
    p = oggwriter::opusPacket(static_cast<uint8_t>(config), false, 2, {40, 50});
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(2 * fs), oggopus::packetSamples(p.data(), p.size()));
    // Code 3: as many frames as fit in 120 ms, with padding.
    const uint32_t m = 5760 / fs > 48 ? 48 : 5760 / fs;
    std::vector<uint16_t> sizes(m, 20);
    p = oggwriter::opusPacket(static_cast<uint8_t>(config), true, 3, sizes, true, 7);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(m), oggopus::frameCount(p.data(), p.size()));
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(m * fs), oggopus::packetSamples(p.data(), p.size()));
    // One more frame: over 120 ms (except 2.5 ms frames, capped at 48 by the count's 6 bits).
    if (m < 48) {
      sizes.push_back(20);
      p = oggwriter::opusPacket(static_cast<uint8_t>(config), true, 3, sizes, false, 0);
      TEST_ASSERT_EQUAL_INT32(-1, oggopus::packetSamples(p.data(), p.size()));
    }
  }
  // Malformed: no bytes; code 3 without its count byte, or with 0 frames.
  const uint8_t none[1] = {0};
  TEST_ASSERT_EQUAL_INT32(-1, oggopus::packetSamples(none, 0));
  const uint8_t code3[] = {0xE3};
  TEST_ASSERT_EQUAL_INT32(-1, oggopus::packetSamples(code3, 1));
  const uint8_t zero[] = {0xE3, 0x80};
  TEST_ASSERT_EQUAL_INT32(-1, oggopus::packetSamples(zero, 2));
  TEST_ASSERT_EQUAL_UINT32(960, oggopus::frameSamples(0xF8));  // config 31, CELT FB 20 ms
  TEST_ASSERT_EQUAL_UINT32(2880, oggopus::frameSamples(0x18)); // config 3, SILK 60 ms
}

// The frame split, against the packets as written: each code's framing,
// the sizes in 1 and 2 bytes, padding (chained 255s included), CBR and
// VBR, 48 frames of 2.5 ms, a 120 ms packet of 6 x 20 ms, and the
// frame-as-a-packet rewrite; what libopus refuses, refused.
void test_frame_split() {
  oggopus::Frames fr;
  uint8_t out[oggopus::kFramePacketBytes];
  // Code 0.
  Bytes p = oggwriter::celt20ms(300, 5);
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_EQUAL_UINT8(1, fr.count);
  TEST_ASSERT_EQUAL_UINT8(0, fr.code());
  TEST_ASSERT_EQUAL_UINT16(300, fr.size[0]);
  TEST_ASSERT_EQUAL_PTR(p.data() + 1, fr.frame(0));
  TEST_ASSERT_EQUAL_UINT32(301, oggopus::framePacket(fr, 0, out));
  TEST_ASSERT_EQUAL_MEMORY(p.data(), out, 301);
  TEST_ASSERT_EQUAL_UINT32(0, oggopus::framePacket(fr, 1, out));
  // Code 1: two equal frames; an odd payload is malformed.
  p = oggwriter::opusPacket(31, true, 1, {200, 200});
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_EQUAL_UINT8(2, fr.count);
  TEST_ASSERT_EQUAL_UINT16(200, fr.size[0]);
  TEST_ASSERT_EQUAL_UINT16(200, fr.size[1]);
  TEST_ASSERT_EQUAL_PTR(p.data() + 1 + 200, fr.frame(1));
  TEST_ASSERT_EQUAL_UINT32(201, oggopus::framePacket(fr, 1, out));
  TEST_ASSERT_EQUAL_UINT8(p[0] & 0xFC, out[0]);
  TEST_ASSERT_EQUAL_MEMORY(p.data() + 201, out + 1, 200);
  p = oggwriter::opusPacket(31, true, 1, {200, 201});
  TEST_ASSERT_FALSE(oggopus::splitPacket(p.data(), p.size(), &fr));
  // Code 2: the first frame's size in one byte, then in two (252 and up).
  p = oggwriter::opusPacket(31, true, 2, {10, 20});
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_EQUAL_UINT16(10, fr.size[0]);
  TEST_ASSERT_EQUAL_UINT16(20, fr.size[1]);
  TEST_ASSERT_EQUAL_PTR(p.data() + 2, fr.frame(0));
  for (const uint16_t big : {252, 253, 300, 1275}) {
    p = oggwriter::opusPacket(31, true, 2, {big, 20});
    TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
    TEST_ASSERT_EQUAL_UINT16(big, fr.size[0]);
    TEST_ASSERT_EQUAL_UINT16(20, fr.size[1]);
    TEST_ASSERT_EQUAL_PTR(p.data() + 3, fr.frame(0));
    TEST_ASSERT_EQUAL_UINT32(21, oggopus::framePacket(fr, 1, out));
    TEST_ASSERT_EQUAL_MEMORY(p.data() + 3 + big, out + 1, 20);
  }
  // Code 3 VBR with padding, including padding over 254 (255-chained).
  for (const uint32_t pad : {0u, 5u, 254u, 255u, 300u, 600u}) {
    p = oggwriter::opusPacket(31, true, 3, {100, 260, 30}, true, pad);
    TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
    TEST_ASSERT_EQUAL_UINT8(3, fr.count);
    TEST_ASSERT_EQUAL_UINT16(100, fr.size[0]);
    TEST_ASSERT_EQUAL_UINT16(260, fr.size[1]);
    TEST_ASSERT_EQUAL_UINT16(30, fr.size[2]);
    TEST_ASSERT_EQUAL_PTR(fr.frame(1) + 260, fr.frame(2));
    TEST_ASSERT_EQUAL_UINT32(31, oggopus::framePacket(fr, 2, out));
    TEST_ASSERT_EQUAL_MEMORY(fr.frame(2), out + 1, 30);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fr.frame(2) + 30 - p.data()) + pad, p.size());
  }
  // Code 3 CBR.
  p = oggwriter::opusPacket(31, true, 3, {150, 150, 150}, false, 10);
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_EQUAL_UINT8(3, fr.count);
  for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT16(150, fr.size[i]);
  TEST_ASSERT_EQUAL_PTR(p.data() + 3, fr.frame(0));
  p = oggwriter::opusPacket(31, true, 3, {150, 150, 151}, false, 0);  // not equal: malformed
  TEST_ASSERT_FALSE(oggopus::splitPacket(p.data(), p.size(), &fr));
  // 48 frames of 2.5 ms (config 16): 120 ms; 49 is over.
  std::vector<uint16_t> sizes(48, 10);
  p = oggwriter::opusPacket(16, false, 3, sizes, false, 0);
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_EQUAL_UINT8(48, fr.count);
  TEST_ASSERT_EQUAL_PTR(p.data() + 2 + 47 * 10, fr.frame(47));
  sizes.push_back(10);
  p = oggwriter::opusPacket(16, false, 3, sizes, false, 0);
  TEST_ASSERT_FALSE(oggopus::splitPacket(p.data(), p.size(), &fr));
  // A 120 ms packet of 6 x 20 ms CELT frames (config 31): six code-0 packets of 960.
  p = oggwriter::opusPacket(31, true, 3, {210, 220, 230, 240, 250, 260}, true, 0);
  TEST_ASSERT_EQUAL_INT32(5760, oggopus::packetSamples(p.data(), p.size()));
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_EQUAL_UINT8(6, fr.count);
  TEST_ASSERT_EQUAL_UINT32(960, oggopus::frameSamples(fr.toc));
  for (uint32_t i = 0; i < 6; ++i) {
    TEST_ASSERT_EQUAL_UINT32(211 + i * 10, oggopus::framePacket(fr, i, out));
    TEST_ASSERT_EQUAL_UINT8(0xFC, out[0]);  // config 31, stereo, code 0
    TEST_ASSERT_EQUAL_MEMORY(fr.frame(i), out + 1, fr.size[i]);
  }
  // A frame over 1,275 bytes, and an empty packet.
  p = oggwriter::celt20ms(1275);
  TEST_ASSERT_TRUE(oggopus::splitPacket(p.data(), p.size(), &fr));
  p = oggwriter::celt20ms(1276);
  TEST_ASSERT_FALSE(oggopus::splitPacket(p.data(), p.size(), &fr));
  TEST_ASSERT_FALSE(oggopus::splitPacket(p.data(), 0, &fr));
  // Code 2 whose first size runs past the packet; code 3 cut after the count.
  const uint8_t bad2[] = {0xE6, 50, 1, 2, 3};
  TEST_ASSERT_FALSE(oggopus::splitPacket(bad2, sizeof(bad2), &fr));
  const uint8_t bad3[] = {0xE7, 0x82, 100};
  TEST_ASSERT_FALSE(oggopus::splitPacket(bad3, sizeof(bad3), &fr));
  const uint8_t bad3pad[] = {0xE7, 0x42, 255};  // padding says more than there is
  TEST_ASSERT_FALSE(oggopus::splitPacket(bad3pad, sizeof(bad3pad), &fr));
}

// ---- the track ----

// An mStream-style file from the top: the head, the pages, the packets
// in order with their page facts, the pre-skip and the end trim, the
// tail scan's length.
void test_usual_file() {
  oggwriter::Built f = oggwriter::build(usualSpec());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, f.kept);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_EQUAL(oggopus::Codec::Opus, r.codec());
  TEST_ASSERT_EQUAL_HEX32(0x1234, r.serial());
  TEST_ASSERT_EQUAL_UINT8(2, r.head().channels);
  TEST_ASSERT_EQUAL_UINT16(312, r.head().preSkip);
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[2], r.firstAudioPage());
  TEST_ASSERT_EQUAL_INT64(0, r.g0());
  TEST_ASSERT_EQUAL_UINT32(1, r.tagsPages());
  TEST_ASSERT_TRUE(r.tagsBytes() > 8 && r.tagsBytes() < 64);
  TEST_ASSERT_TRUE(file.reads <= 8);  // the head page, a header, the tags header, the first audio page (2), the next-page check
  // The length before any packet.
  file.reads = 0;
  file.bytes = 0;
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
  TEST_ASSERT_FALSE(r.chained());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
  TEST_ASSERT_EQUAL_UINT32(1980, r.lengthMs());  // 95,040 / 48
  TEST_ASSERT_TRUE(file.reads <= 12);
}

void test_usual_file_packets() {
  oggwriter::FileSpec spec = usualSpec();
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  oggopus::Reader::Packet p;
  for (uint32_t i = 0; i < 100; ++i) {
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, r.next(&p));
    TEST_ASSERT_EQUAL_UINT32(spec.packets[i].size(), p.bytes);
    TEST_ASSERT_EQUAL_MEMORY(spec.packets[i].data(), p.data, p.bytes);
    TEST_ASSERT_TRUE(p.data >= pageBuf && p.data < pageBuf + sizeof(pageBuf));  // in place
    TEST_ASSERT_EQUAL_INT32(960, p.samples);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[i < 50 ? 2 : 3], p.pageOffset);
    TEST_ASSERT_EQUAL_INT64(i < 50 ? 48000 : 95352, p.pageGranule);
    TEST_ASSERT_EQUAL(i == 49 || i == 99, p.lastOnPage);
    TEST_ASSERT_EQUAL(i >= 50, p.eos);
    TEST_ASSERT_FALSE(p.gapBefore);
  }
  TEST_ASSERT_EQUAL(oggopus::Reader::Next::End, r.next(&p));
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, r.ended());
  TEST_ASSERT_EQUAL_UINT32(2, r.stats().pages);
  TEST_ASSERT_EQUAL_UINT32(0, r.stats().badPages);
  // Played through the timeline, frame by frame and whole packets alike.
  r.restart();
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(kUsualKept) - 1, run.lastT);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_EQUAL_UINT32(100, run.packets);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.tl.kept());
  TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(kUsualKept) + 648, run.tl.position());  // the trimmed samples decoded past the end
  r.restart();
  run = play(r, false);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
}

// What open() refuses, and the words for it.
void test_refusals() {
  using Open = oggopus::Reader::Open;
  char text[96];
  // The screen's form of a refusal (refusalNote(): Now Playing's toast):
  // under 40 characters, never empty after a refusal.
  char note[48];
  auto shortForm = [&](const oggopus::Reader& r) -> const char* {
    r.refusalNote(note, sizeof(note));
    TEST_ASSERT_TRUE(note[0] != 0);
    TEST_ASSERT_TRUE(std::strlen(note) < 40);
    return note;
  };
  // Not Ogg: junk, and an empty file.
  Bytes junk(100, 0x55);
  {
    oggwriter::Reader file(junk);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::NotOgg, r.open());
    TEST_ASSERT_EQUAL_STRING("not an Ogg file", r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("not an Ogg file", shortForm(r));
    oggwriter::Reader empty(junk, 0);
    oggopus::Reader re(empty, 0, pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::NotOgg, re.open());
  }
  // Other codecs' BOS pages.
  struct Other {
    Bytes first;
    const char* text;
    const char* note;
  };
  const Other others[] = {
      {{1, 'v', 'o', 'r', 'b', 'i', 's', 0, 0}, "Ogg Vorbis isn't supported (only Opus)", "Ogg Vorbis isn't supported"},
      {{0x7F, 'F', 'L', 'A', 'C', 1, 0}, "Ogg FLAC isn't supported (only Opus)", "Ogg FLAC isn't supported"},
      {{'S', 'p', 'e', 'e', 'x', ' ', ' ', ' ', '1'}, "Speex isn't supported (only Opus)", "Speex isn't supported"},
      {{0x80, 't', 'h', 'e', 'o', 'r', 'a', 3}, "an Ogg video (Theora), not Opus", "an Ogg video, not Opus"},
      {{'f', 'i', 's', 'h'}, "an Ogg file, but not Opus", "an Ogg file, but not Opus"},
  };
  for (const Other& o : others) {
    oggwriter::Muxer m(0x42);
    m.packet(o.first, 0);
    m.flush();
    m.packet(Bytes(20, 1), 0);
    m.flush(true);
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::NotOpus, r.open());
    TEST_ASSERT_EQUAL_STRING(o.text, r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING(o.note, shortForm(r));
  }
  // Two BOS pages: multiplexed (Opus with video), refused.
  {
    oggwriter::Built f = oggwriter::build(usualSpec());
    oggwriter::PageSpec v;
    v.serial = 0x77;
    v.flags = ogg::kBos;
    v.lacing = {8};
    v.body = {0x80, 't', 'h', 'e', 'o', 'r', 'a', 3};
    Bytes video = oggwriter::page(v);
    Bytes muxed = f.bytes;
    muxed.insert(muxed.begin() + f.pageOffsets[1], video.begin(), video.end());
    oggwriter::Reader file(muxed);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Multiplexed, r.open());
    TEST_ASSERT_EQUAL_STRING("an Ogg file with several streams (video?) isn't supported", r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("several Ogg streams aren't supported", shortForm(r));
    // The video first: the same.
    muxed = f.bytes;
    muxed.insert(muxed.begin(), video.begin(), video.end());
    oggwriter::Reader file2(muxed);
    oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Multiplexed, r2.open());
  }
  // Damaged heads: short, a wrong version, 0 channels, and the head page's
  // CRC wrong (a pre-skip patched by hand, say: nothing on the page is
  // believed).
  for (const uint32_t which : {0u, 1u, 2u, 3u}) {
    oggwriter::FileSpec spec = usualSpec();
    if (which == 0) spec.head.truncate = 18;
    if (which == 1) spec.head.version = 16;
    if (which == 2) spec.head.channels = 0;
    oggwriter::Built f = oggwriter::build(spec);
    if (which == 3) f.bytes[38] ^= 1;  // the pre-skip's low byte
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::BadHead, r.open());
    TEST_ASSERT_EQUAL_STRING("damaged Opus header", r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("damaged Opus header", shortForm(r));
  }
  // Unsupported mappings.
  {
    oggwriter::FileSpec spec = usualSpec();
    spec.head.family = 1;
    spec.head.channels = 6;
    spec.head.streams = 4;
    spec.head.coupled = 2;
    spec.head.map = {0, 4, 1, 2, 3, 5};
    oggwriter::Built f = oggwriter::build(spec);
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Unsupported, r.open());
    TEST_ASSERT_EQUAL_STRING("surround Opus (6 channels) isn't supported", r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("surround Opus isn't supported", shortForm(r));
    spec.head = oggwriter::HeadSpec{};
    spec.head.channels = 3;
    f = oggwriter::build(spec);
    oggwriter::Reader f3(f.bytes);
    oggopus::Reader r3(f3, f3.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Unsupported, r3.open());
    TEST_ASSERT_EQUAL_STRING("surround Opus (3 channels) isn't supported", r3.refusal(text, sizeof(text)));
    spec.head = oggwriter::HeadSpec{};
    spec.head.family = 1;
    spec.head.streams = 2;
    spec.head.coupled = 0;
    f = oggwriter::build(spec);
    oggwriter::Reader f2(f.bytes);
    oggopus::Reader r2(f2, f2.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Unsupported, r2.open());
    TEST_ASSERT_EQUAL_STRING("Opus with 2 streams for 2 channels isn't supported", r2.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("an unusual Opus channel layout", shortForm(r2));
    spec.head = oggwriter::HeadSpec{};
    spec.head.family = 255;
    f = oggwriter::build(spec);
    oggwriter::Reader f255(f.bytes);
    oggopus::Reader r255(f255, f255.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Unsupported, r255.open());
    TEST_ASSERT_EQUAL_STRING("Opus channel mapping family 255 isn't supported", r255.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("Opus mapping family 255 isn't supported", shortForm(r255));
    spec.head = oggwriter::HeadSpec{};
    spec.head.family = 1;
    spec.head.map = {0, 255};
    f = oggwriter::build(spec);
    oggwriter::Reader fm(f.bytes);
    oggopus::Reader rm(fm, fm.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Unsupported, rm.open());
    TEST_ASSERT_EQUAL_STRING("an Opus channel mapping table this player can't follow", rm.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("an unusual Opus channel mapping", shortForm(rm));
  }
  // No OpusTags after the head (an audio packet instead): a bad header.
  {
    oggwriter::Muxer m(0x42);
    m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
    m.flush();
    m.packet(oggwriter::celt20ms(), 960);
    m.flush(true);
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::BadHead, r.open());
  }
  // The headers and nothing after: no audio.
  {
    oggwriter::Muxer m(0x42);
    m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
    m.flush();
    m.packet(oggwriter::opusTags(), 0);
    m.flush();
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::NoAudio, r.open());
    TEST_ASSERT_EQUAL_STRING("no audio in it", r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("no audio in it", shortForm(r));
  }
  // An invalid start: the first audio page's granule under its samples.
  {
    oggwriter::FileSpec spec = usualSpec();
    spec.g0 = -100;
    oggwriter::Built f = oggwriter::build(spec);
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::BadStart, r.open());
    TEST_ASSERT_EQUAL_STRING("damaged Opus start (the first audio page's granule position)", r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("damaged Opus start", shortForm(r));
  }
  // A one-page (EOS) file whose granule is under the pre-skip: invalid
  // too (RFC 7845 section 4.5), not a 0:00 track that plays nothing.
  {
    oggwriter::FileSpec spec;
    spec.head.preSkip = 3840;
    spec.packets = oggwriter::celtPackets(3);  // 2,880 samples
    spec.packetsPerPage = 0;
    spec.endTrim = 2880 - 1000;                // the granule: 1,000
    oggwriter::Built f = oggwriter::build(spec);
    TEST_ASSERT_EQUAL_UINT32(3, f.pageOffsets.size());
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::BadStart, r.open());
    // Exactly the pre-skip: valid, with nothing left after it, so no
    // audio: refused (the player skips it; opened, it would end at once
    // and Repeat One would start it again at once, round and round). The
    // one-page file's end is its first page's; a longer empty stream (an
    // EOS page of its own) is known at open(true) from the tail scan,
    // while open() alone can't tell and opens it.
    spec.packets = oggwriter::celtPackets(4);  // 3,840 samples
    spec.endTrim = 0;
    f = oggwriter::build(spec);
    oggwriter::Reader file2(f.bytes);
    oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::NoAudio, r2.open());
    TEST_ASSERT_EQUAL_STRING("no audio in it", r2.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("no audio in it", shortForm(r2));
    spec.packetsPerPage = 2;  // the EOS page is the second audio page
    f = oggwriter::build(spec);
    TEST_ASSERT_EQUAL_UINT32(4, f.pageOffsets.size());
    oggwriter::Reader file3(f.bytes);
    oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::NoAudio, r3.open(true));
    oggwriter::Reader file4(f.bytes);
    oggopus::Reader r4(file4, file4.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r4.open());
    TEST_ASSERT_TRUE(r4.scanTail());
    TEST_ASSERT_EQUAL_UINT64(0, r4.lengthSamples());
    Run run = play(r4);
    TEST_ASSERT_EQUAL_UINT64(0, run.kept);
    TEST_ASSERT_TRUE(run.finished);
    // One sample after the pre-skip plays.
    spec.head.preSkip = 3839;
    f = oggwriter::build(spec);
    oggwriter::Reader file5(f.bytes);
    oggopus::Reader r5(file5, file5.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r5.open(true));
    TEST_ASSERT_EQUAL_UINT64(1, r5.lengthSamples());
  }
}

// A 2 MB OpusTags (a picture) over 33 pages: skipped by page headers,
// the open's reads bounded.
void test_big_tags() {
  oggwriter::FileSpec spec = usualSpec();
  spec.tagsPad = 2 * 1024 * 1024;
  oggwriter::Built f = oggwriter::build(spec);
  TEST_ASSERT_TRUE(f.pageOffsets.size() > 30);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_EQUAL_UINT32(33, r.tagsPages());
  TEST_ASSERT_EQUAL_UINT32(2 * 1024 * 1024, r.tagsBytes());
  TEST_ASSERT_TRUE(file.reads <= 40);
  TEST_ASSERT_TRUE(file.bytes < 48 * 1024);  // ~34 headers of 290 bytes and the first audio page (15 KB)
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
}

// Lacing: packets of 255 and 510 bytes (a 0 ends them), and packets
// spanning pages (one over 3 pages), assembled in the packet buffer; a
// page with no segments in the middle.
void test_lacing_and_spanning() {
  oggwriter::FileSpec spec;
  spec.packets = {oggwriter::celt20ms(254), oggwriter::celt20ms(509), oggwriter::celt20ms(99),
                  oggwriter::celt20ms(1200), oggwriter::celt20ms(300)};
  spec.maxSegments = 2;  // 510 bytes a page at most: the 1,201-byte packet spans 3 pages
  spec.packetsPerPage = 0;
  spec.endTrim = 10;
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  oggopus::Reader::Packet p;
  for (size_t i = 0; i < spec.packets.size(); ++i) {
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, r.next(&p));
    TEST_ASSERT_EQUAL_UINT32(spec.packets[i].size(), p.bytes);
    TEST_ASSERT_EQUAL_MEMORY(spec.packets[i].data(), p.data, p.bytes);
    TEST_ASSERT_EQUAL_INT32(960, p.samples);
  }
  TEST_ASSERT_EQUAL(oggopus::Reader::Next::End, r.next(&p));
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, r.ended());
  r.restart();
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(5 * 960 - 10 - 312, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_TRUE(run.finished);
  // Which packets were in place and which assembled.
  // With 2 segments a page: 255 B is {255, 0}, page 1 (in place); 510 B
  // is {255, 255, 0} over pages 2-3 (assembled); 100 B is on page 3 (in
  // place); 1,201 B is {255 x 4, 181} over pages 4-6 (assembled); 301 B is
  // {255, 46} over pages 6-7 (assembled).
  TEST_ASSERT_EQUAL_UINT32(2 + 7, f.pageOffsets.size());
  r.restart();
  r.next(&p);
  TEST_ASSERT_TRUE(p.data >= pageBuf && p.data < pageBuf + sizeof(pageBuf));
  r.next(&p);
  TEST_ASSERT_EQUAL_PTR(packetBuf, p.data);
  r.next(&p);
  TEST_ASSERT_TRUE(p.data >= pageBuf && p.data < pageBuf + sizeof(pageBuf));
  r.next(&p);
  TEST_ASSERT_EQUAL_PTR(packetBuf, p.data);
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[2 + 5], p.pageOffset);
  r.next(&p);
  TEST_ASSERT_EQUAL_PTR(packetBuf, p.data);
  TEST_ASSERT_TRUE(p.eos);
  // An empty page between audio pages: nothing changes.
  oggwriter::Muxer m(0x1234);
  m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
  m.flush();
  m.packet(oggwriter::opusTags(), 0);
  m.flush();
  m.packet(oggwriter::celt20ms(), 960);
  m.flush();
  m.emptyPage();
  m.packet(oggwriter::celt20ms(), 1920);
  m.flush(true);
  oggwriter::Reader file2(m.out);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT32(2, run.packets);
  TEST_ASSERT_EQUAL_UINT64(1920 - 312, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.gaps);
  TEST_ASSERT_EQUAL_UINT32(3, r2.stats().pages);
  // An empty EOS page carrying the end: the packets before it are all kept
  // (nothing completes on it to trim); the end is the stream's.
  oggwriter::Muxer m2(0x1234);
  m2.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
  m2.flush();
  m2.packet(oggwriter::opusTags(), 0);
  m2.flush();
  m2.packet(oggwriter::celt20ms(), 960);
  m2.flush();
  m2.flush(true);  // an empty page, flagged EOS, at the granule reached (960)
  TEST_ASSERT_EQUAL_UINT32(4, m2.pageOffsets.size());
  oggwriter::Reader file3(m2.out);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open());
  run = play(r3);
  TEST_ASSERT_EQUAL_UINT32(1, run.packets);
  TEST_ASSERT_EQUAL_UINT64(960 - 312, run.kept);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  TEST_ASSERT_FALSE(run.finished);  // no EOS trim seen: the end is the reader's
}

// A damaged page (one byte): its packets are lost, the next good page is
// found, and the first packet on it says where it starts (its page's
// granule less the page's packets), so the gap is filled to the sample
// and the timeline never slips; the end trim still holds. A page cut out
// whole: a sequence gap, the same.
void test_damaged_and_lost_pages() {
  oggwriter::FileSpec spec = usualSpec();
  spec.packetsPerPage = 10;  // 10 audio pages of 9,600 samples
  oggwriter::Built f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(12, f.pageOffsets.size());
  // Page 5 (the 4th audio page: samples 38,400-48,000) damaged in its body.
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[6] + 200] ^= 0x01;
  oggwriter::Reader file(damaged);
  {
    // The first packet after the gap, by hand (a reader of its own: the
    // counts are since the open): packet 50 starts where the lost page ended.
    oggopus::Reader rh(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, rh.open());
    oggopus::Reader::Packet p;
    for (uint32_t i = 0; i < 41; ++i) TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, nextPacket(rh, &p));
    TEST_ASSERT_TRUE(p.gapBefore);
    TEST_ASSERT_EQUAL_INT64(48000, p.startK);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[7], p.pageOffset);
  }
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT32(90, run.packets);
  TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
  TEST_ASSERT_EQUAL_UINT64(9600, run.filled);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(kUsualKept) - 1, run.lastT);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
  TEST_ASSERT_EQUAL_INT64(0, run.tl.slip());
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().badPages);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().resyncs);
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[7] - f.pageOffsets[6], r.stats().resyncBytes);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().sequenceGaps);
  TEST_ASSERT_EQUAL_UINT32(0, r.stats().droppedPartials);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().gapsSized);
  // The same page cut out whole.
  Bytes cut = f.bytes;
  cut.erase(cut.begin() + f.pageOffsets[6], cut.begin() + f.pageOffsets[7]);
  oggwriter::Reader file2(cut);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT32(90, run.packets);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_UINT64(9600, run.filled);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
  TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
  TEST_ASSERT_EQUAL_UINT32(0, r2.stats().badPages);
  TEST_ASSERT_EQUAL_UINT32(1, r2.stats().sequenceGaps);
  // The first audio page damaged: the open resyncs to the next one, and
  // g0 counts from it.
  Bytes first = f.bytes;
  first[f.pageOffsets[2] + 300] ^= 0x80;
  oggwriter::Reader file3(first);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open());
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], r3.firstAudioPage());
  TEST_ASSERT_EQUAL_INT64(9600, r3.g0());
  run = play(r3);
  TEST_ASSERT_EQUAL_UINT32(90, run.packets);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept - 9600, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
}

// A packet spanning pages loses a page: dropped, never decoded; a
// continued flag with nothing pending drops that head; a packet pending
// into a page without the flag is dropped.
void test_continuation_rules() {
  // Packets A (301 B), B (1,201 B), C (301 B), D (301 B), 4 segments a
  // page: page 1 {255, 46, 255, 255} (A, B's head), page 2 {255, 255, 181,
  // 255} (B completes, C's head), page 3 {46, 255, 46} (C completes, D),
  // EOS. Page 2 cut out: B's head and C's tail are dropped, A and D play,
  // with B and C's 1,920 samples filled between them: D's page says D
  // starts at 2,880 (its EOS granule 3,840 less D's 960), and the EOS
  // page after a gap keeps its packets to its granule, so the track is
  // its whole length.
  oggwriter::FileSpec spec;
  spec.packets = {oggwriter::celt20ms(300, 1), oggwriter::celt20ms(1200, 2), oggwriter::celt20ms(300, 3),
                  oggwriter::celt20ms(300, 4)};
  spec.maxSegments = 4;
  spec.packetsPerPage = 0;
  oggwriter::Built f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(2 + 3, f.pageOffsets.size());
  Bytes cut = f.bytes;
  cut.erase(cut.begin() + f.pageOffsets[2 + 1], cut.begin() + f.pageOffsets[2 + 2]);
  oggwriter::Reader file(cut);
  {
    oggopus::Reader rh(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, rh.open());
    oggopus::Reader::Packet p;
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, rh.next(&p));
    TEST_ASSERT_FALSE(p.gapBefore);
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, rh.next(&p));
    TEST_ASSERT_TRUE(p.gapBefore);
    TEST_ASSERT_EQUAL_INT64(2880, p.startK);
  }
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT32(2, run.packets);
  TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
  TEST_ASSERT_EQUAL_UINT64(1920, run.filled);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().sequenceGaps);
  TEST_ASSERT_EQUAL_UINT32(2, r.stats().droppedPartials);
  TEST_ASSERT_EQUAL_UINT64(4 * 960 - 312, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
  // Hand-made pages: A (2 packets), B flagged continued with nothing
  // pending (its first packet dropped), C ending in a partial packet, D
  // without the flag (the partial dropped), E the EOS.
  oggwriter::Muxer m(0x1234);
  m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
  m.flush();
  m.packet(oggwriter::opusTags(), 0);
  m.flush();
  Bytes b = m.out;
  const Bytes pk = oggwriter::celt20ms(100);
  uint32_t seq = 2;
  auto add = [&](uint8_t flags, int64_t granule, const Bytes& lacing, const Bytes& body) {
    oggwriter::PageSpec s;
    s.serial = 0x1234;
    s.sequence = seq++;
    s.granule = granule;
    s.flags = flags;
    s.lacing = lacing;
    s.body = body;
    const Bytes pg = oggwriter::page(s);
    b.insert(b.end(), pg.begin(), pg.end());
  };
  Bytes two = pk;
  two.insert(two.end(), pk.begin(), pk.end());
  add(0, 1920, {101, 101}, two);
  add(ogg::kContinued, 3840, {101, 101}, two);
  Bytes big = oggwriter::celt20ms(400);
  add(0, -1, {255}, Bytes(big.begin(), big.begin() + 255));
  // (The partial would complete on D: a muxer counts it in D's granule.)
  add(0, 6720, {101, 101}, two);
  add(ogg::kEos, 7680, {101}, pk);
  oggwriter::Reader file2(b);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT32(2 + 1 + 2 + 1, run.packets);
  TEST_ASSERT_EQUAL_UINT32(2, r2.stats().droppedPartials);
  TEST_ASSERT_EQUAL_UINT32(0, r2.stats().sequenceGaps);
  // Page B's granule says 3,840 after one packet decoded: a correction of
  // 960; page D's 6,720 after 1,920 more: the partial's 960 again.
  TEST_ASSERT_EQUAL_UINT32(2, run.tl.corrections());
  TEST_ASSERT_EQUAL_INT64(1920, run.tl.slip());
  TEST_ASSERT_EQUAL_UINT64(7680 - 312 - 1920, run.kept);
  TEST_ASSERT_TRUE(run.finished);
}

// A page of another stream in the middle is stepped over; a second
// link after ours is never read while playing and makes the tail scan
// say chained; another stream's BOS where ours was cut ends the track.
void test_foreign_and_chained() {
  oggwriter::FileSpec spec = usualSpec();
  spec.packetsPerPage = 10;
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::PageSpec v;
  v.serial = 0x77;
  v.sequence = 9;
  v.granule = 12345;
  v.lacing = {50};
  v.body.resize(50, 3);
  const Bytes foreign = oggwriter::page(v);
  Bytes b = f.bytes;
  b.insert(b.begin() + f.pageOffsets[6], foreign.begin(), foreign.end());
  oggwriter::Reader file(b);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT32(100, run.packets);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().foreignPages);
  TEST_ASSERT_EQUAL_UINT32(0, r.stats().sequenceGaps);
  // A second link (another serial) after our EOS page.
  oggwriter::FileSpec second = usualSpec();
  second.serial = 0x5678;
  second.packets = oggwriter::celtPackets(3, 100);
  oggwriter::Built g = oggwriter::build(second);
  Bytes chained = f.bytes;
  chained.insert(chained.end(), g.bytes.begin(), g.bytes.end());
  oggwriter::Reader file2(chained);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  TEST_ASSERT_TRUE(r2.scanTail());
  TEST_ASSERT_TRUE(r2.chained());
  TEST_ASSERT_EQUAL_INT64(95352, r2.lastGranule());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT32(100, run.packets);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  TEST_ASSERT_EQUAL_UINT32(0, r2.stats().foreignPages);
  // Our stream cut after 5 audio pages (no EOS), the second link right there.
  Bytes cutChain(f.bytes.begin(), f.bytes.begin() + f.pageOffsets[7]);
  cutChain.insert(cutChain.end(), g.bytes.begin(), g.bytes.end());
  oggwriter::Reader file3(cutChain);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open());
  run = play(r3);
  TEST_ASSERT_EQUAL_UINT32(50, run.packets);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Chained, run.end);
  TEST_ASSERT_FALSE(run.finished);
  TEST_ASSERT_EQUAL_UINT64(48000 - 312, run.kept);
}

// g0: a cropped start (the first page's granule above its samples), the
// invalid case, and a one-page EOS file, where the granule is the end
// (the trim micro-opus misses); a first page without a granule.
void test_g0_rules() {
  oggwriter::FileSpec spec = usualSpec();
  spec.g0 = 1000;
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_EQUAL_INT64(1000, r.g0());
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  // One page, EOS, its granule under its samples: g0 = 0, the end trimmed.
  oggwriter::FileSpec one;
  one.packets = oggwriter::celtPackets(3);
  one.packetsPerPage = 0;
  one.endTrim = 500;
  f = oggwriter::build(one);
  TEST_ASSERT_EQUAL_UINT32(3, f.pageOffsets.size());
  oggwriter::Reader file1(f.bytes);
  oggopus::Reader r1(file1, file1.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r1.open());
  TEST_ASSERT_EQUAL_INT64(0, r1.g0());
  run = play(r1);
  TEST_ASSERT_EQUAL_UINT64(2880 - 500 - 312, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_INT64(2880 - 500 - 312 - 1, run.lastT);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_TRUE(r1.scanTail());
  TEST_ASSERT_EQUAL_UINT64(2880 - 500 - 312, r1.lengthSamples());
  // One page, EOS, cropped (its granule above its samples): g0 from it.
  one.endTrim = 0;
  one.g0 = 100;
  f = oggwriter::build(one);
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  TEST_ASSERT_EQUAL_INT64(100, r2.g0());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT64(2880 - 312, run.kept);
  TEST_ASSERT_TRUE(run.finished);
  // A first audio page holding only the head of a big packet (granule
  // -1): g0 comes from the page it completes on.
  oggwriter::FileSpec big;
  big.packets = {oggwriter::celt20ms(1200), oggwriter::celt20ms(100)};
  big.maxSegments = 2;
  big.packetsPerPage = 0;
  big.g0 = 7;
  f = oggwriter::build(big);
  oggwriter::Reader file3(f.bytes);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open());
  TEST_ASSERT_EQUAL_INT64(7, r3.g0());
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[2], r3.firstAudioPage());
  run = play(r3);
  TEST_ASSERT_EQUAL_UINT64(1920 - 312, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
}

// The end trim: EOS on a short page (one packet, 60 samples kept of it),
// an EOS granule equal to the page before's (nothing kept of it), a
// packet spanning into the EOS page, and 120 ms packets decoded frame by
// frame with the research's 5,448-sample trim.
void test_end_trim() {
  oggwriter::FileSpec spec;
  spec.packets = oggwriter::celtPackets(11);
  spec.packetsPerPage = 10;
  spec.endTrim = 900;
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(9600 + 60 - 312, run.kept);
  TEST_ASSERT_EQUAL_INT64(9600 + 60 - 312 - 1, run.lastT);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  spec.endTrim = 960;
  f = oggwriter::build(spec);
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT64(9600 - 312, run.kept);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_EQUAL_UINT32(11, run.packets);
  // Packets spanning pages (1,201 B, 5 segments, 4 a page): the last one
  // completes on the EOS page, whose granule trims it.
  oggwriter::FileSpec span;
  span.packets = oggwriter::celtPackets(5, 1200);
  span.maxSegments = 4;
  span.packetsPerPage = 0;
  span.endTrim = 100;
  f = oggwriter::build(span);
  oggwriter::Reader file3(f.bytes);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open());
  run = play(r3);
  TEST_ASSERT_EQUAL_UINT64(4800 - 100 - 312, run.kept);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  // 120 ms packets (6 x 20 ms CELT), trimmed by 5,448.
  oggwriter::FileSpec f120;
  for (int i = 0; i < 20; ++i) {
    f120.packets.push_back(oggwriter::opusPacket(31, true, 3, {210, 220, 230, 240, 250, 260}, true, 0, i + 1));
  }
  f120.packetsPerPage = 8;
  f120.endTrim = 5448;
  f = oggwriter::build(f120);
  TEST_ASSERT_EQUAL_UINT64(20 * 5760, f.samples);
  oggwriter::Reader file4(f.bytes);
  oggopus::Reader r4(file4, file4.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r4.open());
  run = play(r4);
  TEST_ASSERT_EQUAL_UINT64(20 * 5760 - 5448 - 312, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_INT64(20 * 5760 - 5448 - 312 - 1, run.lastT);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_TRUE(run.finished);
  TEST_ASSERT_TRUE(r4.scanTail());
  TEST_ASSERT_EQUAL_UINT64(20 * 5760 - 5448 - 312, r4.lengthSamples());
}

// The pre-skip: odd, and longer than the first packets (3,840: four of
// them dropped whole); the kept samples start at 0 either way.
void test_pre_skip() {
  oggwriter::FileSpec spec = usualSpec();
  spec.head.preSkip = 311;
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_EQUAL_UINT16(311, r.head().preSkip);
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept + 1, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  spec.head.preSkip = 3840;
  f = oggwriter::build(spec);
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  oggopus::Timeline tl;
  tl.start(r2.g0(), 3840);
  oggopus::Reader::Packet p;
  for (int i = 0; i < 4; ++i) {
    r2.next(&p);
    tl.packet(p);
    const oggopus::Timeline::Keep k = tl.decoded(960);
    TEST_ASSERT_EQUAL_UINT32(0, k.take);
    TEST_ASSERT_EQUAL_UINT32(960, k.skip);
    tl.packetDone();
  }
  TEST_ASSERT_EQUAL_INT64(0, tl.position());
  r2.next(&p);
  tl.packet(p);
  const oggopus::Timeline::Keep k = tl.decoded(960);
  TEST_ASSERT_EQUAL_UINT32(0, k.skip);
  TEST_ASSERT_EQUAL_UINT32(960, k.take);
  r2.restart();
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT64(95352 - 3840, run.kept);
  TEST_ASSERT_EQUAL_INT64(0, run.firstT);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
}

// A file cut short: inside a page, and right after one (no EOS): the
// packets of the whole pages play, the end is Truncated, the tail scan
// gives the last whole page's granule.
void test_truncated_file() {
  oggwriter::Built f = oggwriter::build(usualSpec());
  oggwriter::Reader file(f.bytes, f.pageOffsets[3] + 1000);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_INT64(48000, r.lastGranule());
  TEST_ASSERT_EQUAL_UINT64(48000 - 312, r.lengthSamples());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT32(50, run.packets);
  TEST_ASSERT_EQUAL_UINT64(48000 - 312, run.kept);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Truncated, run.end);
  TEST_ASSERT_FALSE(run.finished);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().badPages);
  oggwriter::Reader file2(f.bytes, f.pageOffsets[3]);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT32(50, run.packets);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Truncated, run.end);
  TEST_ASSERT_EQUAL_UINT32(0, r2.stats().badPages);
  TEST_ASSERT_TRUE(r2.scanTail());
  TEST_ASSERT_EQUAL_INT64(48000, r2.lastGranule());
  // Cut inside the headers: no audio.
  oggwriter::Reader file3(f.bytes, f.pageOffsets[2] + 100);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::NoAudio, r3.open());
}

// Junk after the EOS page, with a fake page in it (our serial, a huge
// granule, a wrong CRC): playing never reads it; the tail scan takes the
// last page whose CRC is right.
void test_garbage_tail() {
  oggwriter::FileSpec spec = usualSpec();
  spec.trailing.resize(1000, 0x55);
  oggwriter::PageSpec fake;
  fake.serial = 0x1234;
  fake.sequence = 99;
  fake.granule = 999999;
  fake.lacing = {10};
  fake.body.resize(10, 9);
  fake.badCrc = true;
  const Bytes fp = oggwriter::page(fake);
  spec.trailing.insert(spec.trailing.end(), fp.begin(), fp.end());
  spec.trailing.resize(spec.trailing.size() + 500, 0x33);
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  TEST_ASSERT_EQUAL_UINT32(0, r.stats().badPages);
  // Junk alone (an ID3v1 tag, say).
  spec.trailing.assign(128, 'T');
  f = oggwriter::build(spec);
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  TEST_ASSERT_TRUE(r2.scanTail());
  TEST_ASSERT_EQUAL_INT64(95352, r2.lastGranule());
}

// A packet over 61,440 bytes is dropped (never assembled past the
// buffer); the rest plays. It completes on the EOS page here, whose
// granule is the end, so the track ends its 960 samples short; on any
// other page the granule would put the timeline right (the damaged-page
// tests).
void test_oversized_packet() {
  oggwriter::FileSpec spec;
  spec.packets = oggwriter::celtPackets(5, 300);
  spec.packets[1] = oggwriter::celt20ms(0);
  spec.packets[1].resize(70000, 0x11);
  spec.packetsPerPage = 0;
  oggwriter::Built f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(2 + 2, f.pageOffsets.size());
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT32(4, run.packets);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().oversized);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().droppedPartials);
  TEST_ASSERT_EQUAL_UINT64(5 * 960 - 312 - 960, run.kept);
  TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
  TEST_ASSERT_FALSE(run.finished);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  // The same packet with three pages after it: put right at its page's end.
  spec.packets = oggwriter::celtPackets(5, 300);
  spec.packets[1] = oggwriter::celt20ms(0);
  spec.packets[1].resize(70000, 0x11);
  spec.packetsPerPage = 3;
  f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(2 + 3, f.pageOffsets.size());
  oggwriter::Reader file1(f.bytes);
  oggopus::Reader r1(file1, file1.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r1.open());
  run = play(r1);
  TEST_ASSERT_EQUAL_UINT32(4, run.packets);
  TEST_ASSERT_EQUAL_UINT32(1, r1.stats().oversized);
  TEST_ASSERT_EQUAL_UINT64(5 * 960 - 312 - 960, run.kept);
  TEST_ASSERT_EQUAL_UINT32(1, run.tl.corrections());
  TEST_ASSERT_EQUAL_INT64(960, run.tl.slip());
  TEST_ASSERT_EQUAL_UINT32(1, run.jumps);
  TEST_ASSERT_TRUE(run.finished);
  // A zero-length packet: handed over as malformed (samples -1), the
  // generator decides; the page's granule puts the count right.
  spec.packets[1] = Bytes();
  f = oggwriter::build(spec);
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  oggopus::Reader::Packet p;
  r2.next(&p);
  TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, r2.next(&p));
  TEST_ASSERT_EQUAL_UINT32(0, p.bytes);
  TEST_ASSERT_EQUAL_INT32(-1, p.samples);
  r2.restart();
  run = play(r2);
  TEST_ASSERT_EQUAL_UINT32(5, run.packets);
  TEST_ASSERT_EQUAL_UINT64(4 * 960 - 312, run.kept);
}

// A crafted file: "OggS" headers with the right version and our serial, a
// wrong CRC and 255 lacing values of 255 (a 65 KB page claimed), 282 bytes
// apart over 2 MB. Each one the scan believes costs a page's read, so
// the search for the next good page is bounded in how far it looks and
// in what it may read: the open gives up (no audio within 128 KB) and a
// play ends as truncated, each in well under the file's size of reads,
// where unbounded scans read the file 240 times over. A real gap (a
// stretch of zeros) is bridged as before, at the cost of its chunks.
void test_resync_bounded() {
  oggwriter::PageSpec fake;
  fake.serial = 0x1234;
  fake.sequence = 7;
  fake.granule = 0;
  fake.lacing.assign(255, 255);
  fake.badCrc = true;
  const Bytes header = oggwriter::page(fake);  // the header alone: the next one follows where the body should be
  TEST_ASSERT_EQUAL_UINT32(ogg::kMaxHeaderBytes, header.size());
  const size_t junkBytes = 2u << 20;
  // Before the first audio page.
  oggwriter::Built f = oggwriter::build(usualSpec());
  Bytes file(f.bytes.begin(), f.bytes.begin() + f.firstAudioPage);
  while (file.size() < f.firstAudioPage + junkBytes) file.insert(file.end(), header.begin(), header.end());
  file.insert(file.end(), f.bytes.begin() + f.firstAudioPage, f.bytes.end());
  {
    oggwriter::Reader src(file);
    oggopus::Reader r(src, src.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::NoAudio, r.open());
    TEST_ASSERT_TRUE(src.bytes < 1024 * 1024);  // the 512 KB budget and the candidate it ran out on
    TEST_ASSERT_TRUE(src.reads < 64);
  }
  // In the middle of the track (the second audio page replaced).
  file.assign(f.bytes.begin(), f.bytes.begin() + f.pageOffsets[3]);
  while (file.size() < f.pageOffsets[3] + junkBytes) file.insert(file.end(), header.begin(), header.end());
  {
    oggwriter::Reader src(file);
    oggopus::Reader r(src, src.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    src.reads = 0;
    src.bytes = 0;
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(50, run.packets);
    TEST_ASSERT_EQUAL_UINT64(48000 - 312, run.kept);
    TEST_ASSERT_EQUAL(oggopus::Reader::End::Truncated, run.end);
    TEST_ASSERT_TRUE(src.bytes < 3u * 1024 * 1024);  // the 2 MB budget
    TEST_ASSERT_TRUE(src.reads < 128);
    TEST_ASSERT_EQUAL_UINT32(1, r.stats().badPages);
    TEST_ASSERT_EQUAL_UINT32(0, r.stats().resyncs);
  }
  // 200 KB of zeros where the 4th audio page should start (the pages
  // intact after it): bridged, 50 chunks.
  oggwriter::FileSpec spec = usualSpec();
  spec.packetsPerPage = 10;
  f = oggwriter::build(spec);
  file = f.bytes;
  file.insert(file.begin() + f.pageOffsets[5], 200 * 1024, 0);
  {
    oggwriter::Reader src(file);
    oggopus::Reader r(src, src.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    src.reads = 0;
    src.bytes = 0;
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(100, run.packets);
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
    TEST_ASSERT_TRUE(run.finished);
    TEST_ASSERT_EQUAL_UINT32(1, r.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(200 * 1024, r.stats().resyncBytes);
    TEST_ASSERT_EQUAL_UINT32(0, r.stats().sequenceGaps);
    TEST_ASSERT_TRUE(src.bytes < 512 * 1024);  // the zeros once, and the pages
    TEST_ASSERT_TRUE(src.reads < 100);
  }
}

// A tail of crafted 27-byte headers (another serial, no segments) each
// followed by a junk byte, 56 KB of them after the EOS page (fewer than
// the long window's 73 KB, so the window still starts before the EOS
// page's header: a longer junk tail hides it from this scan, and the
// length stays unknown until M1's bisection): every step of the walk
// fails on the junk byte and scans on, which read a fresh 16 KB chunk per
// 28 bytes of file (38 MB for 73 KB) before the walk had a budget. Now
// the window's walk gives up after four times the window and takes the
// last good page it saw: the EOS page's granule.
void test_tail_scan_bounded() {
  oggwriter::Built f = oggwriter::build(usualSpec());
  oggwriter::PageSpec fake;
  fake.serial = 0x9999;
  fake.sequence = 7;
  fake.granule = 0;
  const Bytes header = oggwriter::page(fake);
  TEST_ASSERT_EQUAL_UINT32(ogg::kHeaderBytes, header.size());
  Bytes file = f.bytes;
  while (file.size() < f.bytes.size() + 56 * 1024) {
    file.insert(file.end(), header.begin(), header.end());
    file.push_back('x');
  }
  oggwriter::Reader src(file);
  oggopus::Reader r(src, src.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  src.reads = 0;
  src.bytes = 0;
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
  TEST_ASSERT_TRUE(src.bytes < 512 * 1024);
  TEST_ASSERT_TRUE(src.reads < 200);
  // Playing never reads past the EOS page.
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
}

// Granule positions near INT64_MAX (a crafted file, or a garbled header):
// as the first audio page's, refused; on a later page, as no granule
// (the count isn't moved to it, nor the end set by it: the sums would
// overflow), and the tail scan takes the last page whose granule can be
// believed. kMaxGranule itself (2^40) is believed.
void test_huge_granules() {
  oggwriter::FileSpec spec = usualSpec();
  spec.g0 = INT64_MAX - 100 * 960;  // every page's granule near the top
  oggwriter::Built f = oggwriter::build(spec);
  {
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::BadStart, r.open());
  }
  spec.g0 = oggopus::kMaxGranule - 100 * 960;
  f = oggwriter::build(spec);
  {
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_EQUAL_INT64(oggopus::kMaxGranule - 100 * 960, r.g0());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
    TEST_ASSERT_EQUAL_INT64(0, run.firstT);
    TEST_ASSERT_TRUE(run.finished);
  }
  // A page in the middle, then the EOS page, with its granule set to
  // INT64_MAX (the CRC made right).
  spec = usualSpec();
  spec.packetsPerPage = 10;
  f = oggwriter::build(spec);
  auto setGranule = [](Bytes& b, uint32_t at, int64_t granule) {
    ogg::Header h;
    TEST_ASSERT_TRUE(ogg::parseHeader(b.data() + at, b.size() - at, &h));
    for (int i = 0; i < 8; ++i) b[at + 6 + i] = static_cast<uint8_t>(static_cast<uint64_t>(granule) >> (8 * i));
    for (int i = 0; i < 4; ++i) b[at + 22 + i] = 0;
    const uint32_t crc = ogg::crc32(b.data() + at, h.bytes());
    for (int i = 0; i < 4; ++i) b[at + 22 + i] = static_cast<uint8_t>(crc >> (8 * i));
  };
  Bytes mid = f.bytes;
  setGranule(mid, f.pageOffsets[6], INT64_MAX);
  {
    oggwriter::Reader file(mid);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(100, run.packets);
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
    TEST_ASSERT_TRUE(run.finished);
  }
  Bytes last = f.bytes;
  setGranule(last, f.pageOffsets[11], INT64_MAX);
  {
    oggwriter::Reader file(last);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_INT64(86400, r.lastGranule());  // the page before the EOS page
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(100, run.packets);
    TEST_ASSERT_EQUAL_UINT64(100 * 960 - 312, run.kept);  // no EOS trim: the granule isn't believed
    TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
    TEST_ASSERT_FALSE(run.finished);
    TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  }
}

// The timeline alone, fed page facts by hand: the EOS keep counts from the
// last page with a granule (a page without one in between), a decoder
// giving fewer samples than the TOC says is put right at the page's end.
void test_timeline() {
  using P = oggopus::Reader::Packet;
  oggopus::Timeline tl;
  tl.start(0, 312);
  TEST_ASSERT_EQUAL_INT64(-312, tl.position());
  P p;
  p.samples = 960;
  // Page A at 100: two packets, granule 1,920.
  p.pageOffset = 100;
  p.pageGranule = 1920;
  tl.packet(p);
  oggopus::Timeline::Keep k = tl.decoded(960);
  TEST_ASSERT_EQUAL_UINT32(312, k.skip);
  TEST_ASSERT_EQUAL_UINT32(648, k.take);
  tl.packetDone();
  p.lastOnPage = true;
  tl.packet(p);
  k = tl.decoded(960);
  TEST_ASSERT_EQUAL_UINT32(0, k.skip);
  TEST_ASSERT_EQUAL_UINT32(960, k.take);
  tl.packetDone();
  TEST_ASSERT_EQUAL_INT64(1920 - 312, tl.position());
  // Page C at 300 (B at 200 had no granule: its packet completes here),
  // EOS, granule 3,140: keep 1,220 of the 1,920 completing on it.
  p.pageOffset = 300;
  p.pageGranule = 3140;
  p.eos = true;
  p.lastOnPage = false;
  tl.packet(p);
  k = tl.decoded(960);
  TEST_ASSERT_EQUAL_UINT32(960, k.take);
  tl.packetDone();
  TEST_ASSERT_FALSE(tl.finished());
  p.lastOnPage = true;
  tl.packet(p);
  k = tl.decoded(960);
  TEST_ASSERT_EQUAL_UINT32(0, k.skip);
  TEST_ASSERT_EQUAL_UINT32(260, k.take);
  tl.packetDone();
  TEST_ASSERT_TRUE(tl.finished());
  TEST_ASSERT_EQUAL_UINT64(3140 - 312, tl.kept());
  TEST_ASSERT_EQUAL_UINT32(0, tl.corrections());
  // A decoder short of the TOC (a concealment of 480 for a 960 packet):
  // the page's granule wins at its end.
  tl.start(0, 0);
  p = P{};
  p.pageOffset = 100;
  p.pageGranule = 1920;
  p.lastOnPage = false;
  tl.packet(p);
  tl.decoded(480);
  tl.packetDone();
  p.lastOnPage = true;
  tl.packet(p);
  tl.decoded(960);
  tl.packetDone();
  TEST_ASSERT_EQUAL_INT64(1920, tl.position());
  TEST_ASSERT_EQUAL_UINT32(1, tl.corrections());
  TEST_ASSERT_EQUAL_INT64(480, tl.slip());
  TEST_ASSERT_EQUAL_UINT64(1440, tl.kept());
  // The EOS page's granule under the page before's: nothing of it kept.
  p.pageOffset = 200;
  p.pageGranule = 1900;
  p.eos = true;
  tl.packet(p);
  k = tl.decoded(960);
  TEST_ASSERT_EQUAL_UINT32(0, k.take);
  TEST_ASSERT_TRUE(tl.finished());
}

// The gap plan's edges: a gap that can't be sized (a malformed packet on
// the page after it) is put right at the page's end instead, as before;
// a damaged page right before the EOS page (the fill is short by the
// trim and the encoder's padding plays in its place: the length holds);
// a gap over 10 s ends the track.
void test_gap_plan_edges() {
  // A zero-length packet on the page after the lost one: its samples
  // aren't known, so neither is where the page's first packet starts.
  oggwriter::FileSpec spec = usualSpec();
  spec.packetsPerPage = 10;
  spec.packets[55] = Bytes();
  oggwriter::Built f = oggwriter::build(spec);
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[6] + 200] ^= 0x01;  // packets 40-49 lost
  {
    oggwriter::Reader file(damaged);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    oggopus::Reader::Packet p;
    for (uint32_t i = 0; i < 41; ++i) TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, nextPacket(r, &p));
    TEST_ASSERT_TRUE(p.gapBefore);
    TEST_ASSERT_EQUAL_INT64(-1, p.startK);
    r.restart();
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
    TEST_ASSERT_EQUAL_UINT64(0, run.filled);
    TEST_ASSERT_EQUAL_UINT32(0, r.stats().gapsSized);
    TEST_ASSERT_EQUAL_UINT32(1, run.tl.corrections());
    TEST_ASSERT_EQUAL_INT64(9600, run.tl.slip());
    TEST_ASSERT_EQUAL_UINT64(kUsualKept - 9600 - 960, run.kept);  // the lost page, and the empty packet's samples
    TEST_ASSERT_EQUAL_UINT32(1, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
  // The page before the EOS page lost: the EOS page's first packet is
  // placed at granule(EOS) - its packets (the trim unknown), the fill
  // runs to there, the page's packets are kept to its granule: the
  // length is exact and the end is reached.
  spec = usualSpec();
  spec.packets = oggwriter::celtPackets(150);  // 3 audio pages
  f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(5, f.pageOffsets.size());
  damaged = f.bytes;
  damaged[f.pageOffsets[3] + 200] ^= 0x01;
  {
    oggwriter::Reader file(damaged);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT64(150 * 960 - 648 - 312, r.lengthSamples());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(100, run.packets);
    TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
    TEST_ASSERT_EQUAL_UINT64(48000 - 648, run.filled);
    TEST_ASSERT_EQUAL_UINT64(r.lengthSamples(), run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
    TEST_ASSERT_TRUE(run.finished);
    TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
  }
  // 12 s of pages damaged (one byte each) in a 24 s file: the resync
  // steps over them all, the gap is 12 s, over the 10 s rule: the track
  // ends after the 2 s before it.
  spec = usualSpec();
  spec.packets = oggwriter::celtPackets(1200);
  f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(2 + 24, f.pageOffsets.size());
  damaged = f.bytes;
  for (uint32_t j = 4; j < 16; ++j) damaged[f.pageOffsets[j] + 200] ^= 0x01;
  {
    oggwriter::Reader file(damaged);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    oggopus::Reader::Packet p;
    for (uint32_t i = 0; i < 101; ++i) TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, nextPacket(r, &p));
    TEST_ASSERT_TRUE(p.gapBefore);
    TEST_ASSERT_EQUAL_INT64(14 * 48000, p.startK);
    TEST_ASSERT_EQUAL_UINT32(1, r.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[16] - f.pageOffsets[4], r.stats().resyncBytes);
    r.restart();
    Run run = play(r);
    TEST_ASSERT_TRUE(run.gapEnded);
    TEST_ASSERT_EQUAL_UINT64(2 * 48000 - 312, run.kept);
    TEST_ASSERT_EQUAL_UINT64(0, run.filled);
    TEST_ASSERT_EQUAL_INT64(12 * 48000, run.tl.gap());
    TEST_ASSERT_FALSE(run.finished);
  }
  // 9 s damaged: filled.
  damaged = f.bytes;
  for (uint32_t j = 4; j < 13; ++j) damaged[f.pageOffsets[j] + 200] ^= 0x01;
  {
    oggwriter::Reader file(damaged);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    Run run = play(r);
    TEST_ASSERT_FALSE(run.gapEnded);
    TEST_ASSERT_EQUAL_UINT64(9 * 48000, run.filled);
    TEST_ASSERT_EQUAL_UINT64(1200 * 960 - 648 - 312, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
}

// A chained file whose second link is longer than the tail windows (the
// research's chained.opus: two whole streams): the first link's last page
// is found by the bisection on serial numbers, its length is exact, the
// link's end is where the second's BOS page is, and a plan never probes
// into the second link. The first link cut before the second (no EOS
// page): its last whole page, and playing ends at the BOS.
void test_chained_length() {
  oggwriter::Built first = oggwriter::build(usualSpec());
  oggwriter::FileSpec secondSpec = usualSpec();
  secondSpec.serial = 0x5678;
  secondSpec.packets = oggwriter::celtPackets(400);  // ~120 KB: past the long window
  oggwriter::Built second = oggwriter::build(secondSpec);
  TEST_ASSERT_TRUE(second.bytes.size() > 100 * 1024);
  Bytes chained = first.bytes;
  chained.insert(chained.end(), second.bytes.begin(), second.bytes.end());
  {
    oggwriter::Reader file(chained);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    file.reads = 0;
    file.bytes = 0;
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_TRUE(r.chained());
    TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
    TEST_ASSERT_EQUAL_UINT32(first.bytes.size(), r.linkEnd());
    TEST_ASSERT_TRUE(file.reads < 48);
    TEST_ASSERT_TRUE(file.bytes < 512 * 1024);
    // A plan into the first link.
    oggopus::StartPlan plan;
    file.reads = 0;
    r.planStart(60000, oggopus::kSeekPrerollSamples, &plan);
    TEST_ASSERT_FALSE(plan.fromTop);
    TEST_ASSERT_TRUE(plan.pageOffset < first.bytes.size());
    TEST_ASSERT_TRUE(plan.probes <= 8);
    r.startAt(plan);
    Run run = play(r, true, &plan, 1);
    TEST_ASSERT_EQUAL_INT64(60000, run.firstT);
    r.restart();
    run = play(r);
    TEST_ASSERT_EQUAL_UINT32(100, run.packets);
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
    TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
    TEST_ASSERT_TRUE(run.finished);
  }
  // Cut after the first audio page, the second link right there.
  Bytes cut(first.bytes.begin(), first.bytes.begin() + first.pageOffsets[3]);
  cut.insert(cut.end(), second.bytes.begin(), second.bytes.end());
  {
    oggwriter::Reader file(cut);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_TRUE(r.chained());
    TEST_ASSERT_EQUAL_INT64(48000, r.lastGranule());
    TEST_ASSERT_EQUAL_UINT32(first.pageOffsets[3], r.linkEnd());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(50, run.packets);
    TEST_ASSERT_EQUAL(oggopus::Reader::End::Chained, run.end);
    TEST_ASSERT_EQUAL_UINT64(48000 - 312, run.kept);
  }
}

// A junk tail longer than the long window (200 KB after the EOS page,
// nothing in it that looks like a page): the windows see no page, the
// bisection finds where the pages end, and the last page's granule is
// the length.
void test_long_junk_tail() {
  oggwriter::FileSpec spec = usualSpec();
  spec.trailing.resize(200 * 1024);
  uint32_t x = 12345;
  for (uint8_t& b : spec.trailing) {
    x = x * 1103515245u + 12345u;
    b = static_cast<uint8_t>(x >> 16);
    if (b == 'O') b = 0;
  }
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  file.reads = 0;
  file.bytes = 0;
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_FALSE(r.chained());
  TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
  TEST_ASSERT_EQUAL_UINT32(f.bytes.size(), r.linkEnd());
  TEST_ASSERT_TRUE(file.reads < 128);
  TEST_ASSERT_TRUE(file.bytes < 2 * 1024 * 1024);
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_EQUAL(oggopus::Reader::End::Eos, run.end);
}

// The start plan on a 60 s file of VBR-ish packets (3,000 x 20 ms, 60 to
// 540 bytes, 50 a page): every packet boundary and the sample on either
// side of it lands exactly, the samples decoded before it are the preroll
// and under a packet more (packets before the preroll skipped undecoded),
// and the probes and reads stay bounded. Then a file whose every page
// continues a packet (1,201-byte packets, 4 segments a page): the plan
// starts at Q itself with its packets stepped over. A target in the first
// 80 ms starts from the top. The tail rule through planStartMs(), and a
// target past the end with the length unknown.
void test_start_plan() {
  oggwriter::FileSpec spec;
  for (uint32_t i = 0; i < 3000; ++i) {
    spec.packets.push_back(oggwriter::celt20ms(static_cast<uint16_t>(60 + (i * 7919u) % 481u), i + 1));
  }
  spec.endTrim = 648;
  oggwriter::Built f = oggwriter::build(spec);
  const uint64_t length = 3000 * 960 - 648 - 312;
  TEST_ASSERT_EQUAL_UINT64(length, f.kept);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_UINT64(length, r.lengthSamples());
  TEST_ASSERT_EQUAL_UINT32(59980, r.lengthMs());
  uint32_t plans = 0, fromTop = 0, expectFromTop = 0, maxProbes = 0, maxReads = 0;
  uint64_t probes = 0, reads = 0, bytes = 0;
  const int64_t preroll = oggopus::kSeekPrerollSamples;
  for (uint32_t b = 0; b < 3000; ++b) {
    for (int d = -1; d <= 1; ++d) {
      const int64_t t = static_cast<int64_t>(b) * 960 - 312 + d;
      if (t < 0 || static_cast<uint64_t>(t) >= length) continue;
      if (t + 312 - preroll < 48000) ++expectFromTop;
      oggopus::StartPlan plan;
      file.reads = 0;
      file.bytes = 0;
      r.planStart(static_cast<uint64_t>(t), oggopus::kSeekPrerollSamples, &plan);
      TEST_ASSERT_EQUAL_UINT32(file.reads, plan.reads);
      TEST_ASSERT_EQUAL_UINT64(t, plan.target);
      TEST_ASSERT_EQUAL_INT64(t + 312, plan.keepFrom);
      r.startAt(plan);
      Run run = play(r, true, &plan, 1);
      TEST_ASSERT_EQUAL_INT64(t, run.firstT);
      if (plan.fromTop) {
        ++fromTop;
        TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(t) + 312, run.decodedBefore);
        TEST_ASSERT_EQUAL_UINT32(0, run.skipped);
      } else {
        TEST_ASSERT_EQUAL_INT64(t + 312 - preroll, plan.decodeFrom);
        TEST_ASSERT_TRUE(plan.k <= plan.decodeFrom);
        TEST_ASSERT_TRUE(run.decodedBefore >= oggopus::kSeekPrerollSamples);
        TEST_ASSERT_TRUE(run.decodedBefore < oggopus::kSeekPrerollSamples + 960);
        TEST_ASSERT_FALSE(plan.skipPage);  // (packets never span pages here)
      }
      TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
      ++plans;
      probes += plan.probes;
      reads += file.reads;
      bytes += file.bytes;
      if (plan.probes > maxProbes) maxProbes = plan.probes;
      if (file.reads > maxReads) maxReads = file.reads;
    }
  }
  std::printf("  start plans: %lu; probes mean %.2f max %lu; reads mean %.1f max %lu; bytes mean %lu KB\n",
              (unsigned long)plans, static_cast<double>(probes) / plans, (unsigned long)maxProbes,
              static_cast<double>(reads) / plans, (unsigned long)maxReads, (unsigned long)(bytes / plans / 1024));
  std::fflush(stdout);
  TEST_ASSERT_EQUAL_UINT32(8997, plans);  // (packet 0's three are before the first kept sample)
  // Targets under the first page's granule (48,000) plus the preroll
  // start from the top: with the 200 ms preroll (9,600) packets 1-59's
  // three and 60's first, 178 (160 ms, 7,680: packets 1-57's and 58's
  // first, 172).
  TEST_ASSERT_EQUAL_UINT32(expectFromTop, fromTop);
  TEST_ASSERT_EQUAL_UINT32(oggopus::kSeekPrerollMs == 200 ? 178 : 172, fromTop);
  TEST_ASSERT_TRUE(probes <= plans * 6);  // the research's prototype: 3.3-3.7 a seek
  TEST_ASSERT_TRUE(maxProbes <= 16);
  TEST_ASSERT_TRUE(maxReads <= 48);
  TEST_ASSERT_TRUE(bytes <= plans * 80 * 1024);  // (the play's page reads included)
  // Every page continues a packet: Q's own packets stepped over.
  oggwriter::FileSpec span;
  span.packets = oggwriter::celtPackets(400, 1200);
  span.maxSegments = 4;
  span.packetsPerPage = 0;
  span.endTrim = 100;
  const oggwriter::Built g = oggwriter::build(span);  // (its own: `file` reads f.bytes still)
  TEST_ASSERT_TRUE(g.pageOffsets.size() > 400);
  oggwriter::Reader file2(g.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.open());
  TEST_ASSERT_TRUE(r2.scanTail());
  uint32_t skips = 0;
  for (uint32_t b = 0; b < 400; ++b) {
    for (int d = 0; d <= 1; ++d) {
      const int64_t t = static_cast<int64_t>(b) * 960 - 312 + d;
      if (t < 0 || static_cast<uint64_t>(t) >= g.kept) continue;
      oggopus::StartPlan plan;
      r2.planStart(static_cast<uint64_t>(t), oggopus::kSeekPrerollSamples, &plan);
      r2.startAt(plan);
      Run run = play(r2, true, &plan, 1);
      TEST_ASSERT_EQUAL_INT64(t, run.firstT);
      if (!plan.fromTop) {
        TEST_ASSERT_TRUE(run.decodedBefore >= oggopus::kSeekPrerollSamples);
        TEST_ASSERT_TRUE(run.decodedBefore < oggopus::kSeekPrerollSamples + 960);
      }
      if (plan.skipPage) ++skips;
    }
  }
  TEST_ASSERT_TRUE(skips > 300);
  // A target in the first 80 ms: from the top with the pre-skip.
  oggopus::StartPlan plan;
  r.planStart(100, oggopus::kSeekPrerollSamples, &plan);
  TEST_ASSERT_TRUE(plan.fromTop);
  TEST_ASSERT_EQUAL_UINT32(0, plan.probes);
  r.startAt(plan);
  Run run = play(r, true, &plan, 1);
  TEST_ASSERT_EQUAL_INT64(100, run.firstT);
  TEST_ASSERT_EQUAL_UINT64(412, run.decodedBefore);
  // 0: the plain start.
  r.planStart(0, oggopus::kSeekPrerollSamples, &plan);
  TEST_ASSERT_TRUE(plan.fromTop);
  TEST_ASSERT_EQUAL_INT64(312, plan.keepFrom);
  // The resume preroll: 600 ms before.
  r.planStart(1000000, oggopus::kResumePrerollSamples, &plan);
  r.startAt(plan);
  run = play(r, true, &plan, 1);
  TEST_ASSERT_EQUAL_INT64(1000000, run.firstT);
  TEST_ASSERT_TRUE(run.decodedBefore >= oggopus::kResumePrerollSamples);
  TEST_ASSERT_TRUE(run.decodedBefore < oggopus::kResumePrerollSamples + 960);
  // The tail rule (59.98 s long): 54 s lands, 59 s and 70 s start at 0:00.
  TEST_ASSERT_EQUAL_UINT32(54000, r.planStartMs(54000, oggopus::kSeekPrerollMs, &plan));
  TEST_ASSERT_FALSE(plan.fromTop);
  TEST_ASSERT_EQUAL_UINT64(54000 * 48, plan.target);
  TEST_ASSERT_EQUAL_UINT32(0, r.planStartMs(59000, oggopus::kSeekPrerollMs, &plan));
  TEST_ASSERT_TRUE(plan.fromTop);
  TEST_ASSERT_EQUAL_UINT64(0, plan.target);
  TEST_ASSERT_EQUAL_UINT32(0, r.planStartMs(70000, oggopus::kSeekPrerollMs, &plan));
  TEST_ASSERT_TRUE(plan.fromTop);
  // At or past the end with the length known (planStart alone: no tail
  // rule), or past any granule a file can hold: the plain start, target
  // 0, and played it keeps the whole track from 0 (keeping from a G past
  // the end would decode it all and keep nothing).
  for (const uint64_t past : {length, length + 48000, (static_cast<uint64_t>(1) << 63) + 5}) {
    r.planStart(past, oggopus::kSeekPrerollSamples, &plan);
    TEST_ASSERT_TRUE(plan.fromTop);
    TEST_ASSERT_EQUAL_UINT64(0, plan.target);
    TEST_ASSERT_EQUAL_INT64(312, plan.keepFrom);
    TEST_ASSERT_EQUAL_UINT32(0, plan.probes);
    r.startAt(plan);
    run = play(r, true, &plan);
    TEST_ASSERT_EQUAL_INT64(0, run.firstT);
    TEST_ASSERT_EQUAL_UINT64(length, run.kept);
    TEST_ASSERT_TRUE(run.finished);
  }
  // The last sample itself is a real start.
  r.planStart(length - 1, oggopus::kSeekPrerollSamples, &plan);
  TEST_ASSERT_FALSE(plan.fromTop);
  r.startAt(plan);
  run = play(r, true, &plan);
  TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(length) - 1, run.firstT);
  TEST_ASSERT_EQUAL_UINT64(1, run.kept);
  TEST_ASSERT_TRUE(run.finished);
  // The length not known (no tail scan): a start is where asked, and one
  // past the end lands on the last page and ends at once.
  oggwriter::Reader file3(g.bytes);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open());
  TEST_ASSERT_EQUAL_UINT32(0, r3.lengthMs());
  TEST_ASSERT_EQUAL_UINT32(4000, r3.planStartMs(4000, oggopus::kSeekPrerollMs, &plan));
  r3.startAt(plan);
  run = play(r3, true, &plan, 1);
  TEST_ASSERT_EQUAL_INT64(4000 * 48, run.firstT);
  TEST_ASSERT_EQUAL_UINT32(70000, r3.planStartMs(70000, oggopus::kSeekPrerollMs, &plan));
  TEST_ASSERT_FALSE(plan.fromTop);
  r3.startAt(plan);
  run = play(r3, true, &plan, 1);
  TEST_ASSERT_EQUAL_INT64(-1, run.firstT);
  TEST_ASSERT_EQUAL_UINT64(0, run.kept);
}

// The resume anchor: made from a sample, the file's size and the exact
// length; the check passes a round trip and refuses each field changed
// (the kind, the rate, exact, the size, the length, a length not known)
// and a sample in the last 5 s; a plan from it lands on its sample with
// the resume preroll.
void test_anchor() {
  oggwriter::FileSpec spec = usualSpec();
  spec.packets = oggwriter::celtPackets(400);  // 8 s
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  const uint64_t length = r.lengthSamples();
  TEST_ASSERT_EQUAL_UINT64(400 * 960 - 648 - 312, length);
  TEST_ASSERT_EQUAL_UINT32(7980, r.lengthMs());
  const ResumeAnchor a = oggopus::makeAnchor(96000, file.size(), length);
  TEST_ASSERT_EQUAL(ResumeAnchor::Kind::Opus, a.kind);
  TEST_ASSERT_TRUE(a.valid());
  TEST_ASSERT_TRUE(a.exact);
  TEST_ASSERT_EQUAL_UINT32(48000, a.rate);
  TEST_ASSERT_EQUAL_UINT64(96000, a.sample);
  TEST_ASSERT_EQUAL_UINT32(file.size(), a.fileSize);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(length), a.frameHash);
  TEST_ASSERT_EQUAL_UINT32(0, a.prerollByte);
  TEST_ASSERT_EQUAL_UINT32(0, a.frameByte);
  TEST_ASSERT_EQUAL_UINT32(0, a.skip);
  TEST_ASSERT_EQUAL_UINT32(2000, resumeanchor::ms(a));
  char text[64];
  resumeanchor::describe(a, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("Opus sample 96000", text);
  using oggopus::AnchorCheck;
  TEST_ASSERT_EQUAL(AnchorCheck::Ok, oggopus::checkAnchor(a, file.size(), length));
  ResumeAnchor b = a;
  b.kind = ResumeAnchor::Kind::Flac;
  TEST_ASSERT_EQUAL(AnchorCheck::Kind, oggopus::checkAnchor(b, file.size(), length));
  b = a;
  b.rate = 44100;
  TEST_ASSERT_EQUAL(AnchorCheck::Kind, oggopus::checkAnchor(b, file.size(), length));
  b = a;
  b.exact = false;
  TEST_ASSERT_EQUAL(AnchorCheck::Kind, oggopus::checkAnchor(b, file.size(), length));
  TEST_ASSERT_EQUAL(AnchorCheck::Size, oggopus::checkAnchor(a, file.size() + 1, length));
  b = a;
  b.frameHash += 1;
  TEST_ASSERT_EQUAL(AnchorCheck::Length, oggopus::checkAnchor(b, file.size(), length));
  TEST_ASSERT_EQUAL(AnchorCheck::Length, oggopus::checkAnchor(a, file.size(), length + 1));
  TEST_ASSERT_EQUAL(AnchorCheck::Length, oggopus::checkAnchor(a, file.size(), 0));
  b = a;
  b.sample = 7 * 48000;  // in the last 5 s
  TEST_ASSERT_EQUAL(AnchorCheck::Tail, oggopus::checkAnchor(b, file.size(), length));
  b.sample = length;  // past the end
  TEST_ASSERT_EQUAL(AnchorCheck::Tail, oggopus::checkAnchor(b, file.size(), length));
  // A sample 2^32 ms past the end (a corrupted anchor): a ms figure would
  // wrap to 2,000 ms and let it through as a start inside the track.
  b.sample = ((static_cast<uint64_t>(1) << 32) + 2000) * 48;
  TEST_ASSERT_EQUAL(AnchorCheck::Tail, oggopus::checkAnchor(b, file.size(), length));
  b.sample = 0;
  TEST_ASSERT_EQUAL(AnchorCheck::Ok, oggopus::checkAnchor(b, file.size(), length));
  TEST_ASSERT_EQUAL_STRING("in its last 5 s", oggopus::anchorCheckName(AnchorCheck::Tail));
  TEST_ASSERT_EQUAL_STRING("the length", oggopus::anchorCheckName(AnchorCheck::Length));
  // A plan from the anchor.
  oggopus::StartPlan plan;
  r.planStart(a.sample, oggopus::kResumePrerollSamples, &plan);
  TEST_ASSERT_FALSE(plan.fromTop);
  r.startAt(plan);
  Run run = play(r, true, &plan, 1);
  TEST_ASSERT_EQUAL_INT64(96000, run.firstT);
  TEST_ASSERT_TRUE(run.decodedBefore >= oggopus::kResumePrerollSamples);
  TEST_ASSERT_TRUE(run.decodedBefore < oggopus::kResumePrerollSamples + 960);
}

// The 60 s file of 1 s pages the plan tests use: 3,000 x 20 ms, 60 to
// 540 bytes, 50 a page (audio page i is pageOffsets[2 + i], its granule
// (i + 1) x 48,000), 648 trimmed at the end.
oggwriter::Built sixtySeconds() {
  oggwriter::FileSpec spec;
  for (uint32_t i = 0; i < 3000; ++i) {
    spec.packets.push_back(oggwriter::celt20ms(static_cast<uint16_t>(60 + (i * 7919u) % 481u), i + 1));
  }
  spec.endTrim = 648;
  return oggwriter::build(spec);
}
constexpr uint64_t kSixtyLength = 3000 * 960 - 648 - 312;

// A plan played to the end on a damaged file must land on its sample and
// keep exactly what a play from the top keeps after it.
void checkPlanToEnd(oggopus::Reader& r, uint64_t t, uint64_t fromTopKept, uint64_t filled, bool finished) {
  oggopus::StartPlan plan;
  r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
  TEST_ASSERT_FALSE(plan.fromTop);
  r.startAt(plan);
  const Run run = play(r, true, &plan);
  TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
  TEST_ASSERT_EQUAL_UINT64(fromTopKept - t, run.kept);
  TEST_ASSERT_EQUAL_UINT64(filled, run.filled);
  TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
  TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
  TEST_ASSERT_FALSE(run.gapEnded);
  TEST_ASSERT_EQUAL(finished, run.finished);
}

// A page lost right after Q (cut out whole: a sequence gap, every CRC
// right). The plan reads from Q's end with the timeline at granule(Q);
// the page after must follow Q's sequence number, else the first packet
// handed over starts later than granule(Q) and the landing is late by the
// lost page (its intact audio skipped as preroll), or early by it when
// the target lies in the lost page. With Q's sequence seeding the check
// the gap is seen and sized as any other: the part before the first kept
// sample is stepped over, the rest filled. A target past the lost page,
// one inside it (28,000 samples of fill kept), the page before the EOS
// page lost (the EOS page's packets kept to its granule, the end reached),
// and a page lost after a Q whose packet continues out of it.
void test_lost_page_after_plan() {
  oggwriter::Built f = sixtySeconds();
  Bytes cut = f.bytes;
  cut.erase(cut.begin() + f.pageOffsets[2 + 10], cut.begin() + f.pageOffsets[2 + 11]);  // samples 480,000-528,000
  {
    oggwriter::Reader file(cut);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, r.lengthSamples());
    Run all = play(r);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, all.kept);
    TEST_ASSERT_EQUAL_UINT32(1, all.gaps);
    TEST_ASSERT_EQUAL_UINT64(48000, all.filled);
    TEST_ASSERT_EQUAL_UINT32(0, all.jumps);
    // Past the lost page: Q is page 9, the lost one's 48,000 are stepped
    // over (none heard), the landing exact.
    checkPlanToEnd(r, 540000 - 312, kSixtyLength, 0, true);
    oggopus::StartPlan plan;
    r.planStart(540000 - 312, oggopus::kSeekPrerollSamples, &plan);
    TEST_ASSERT_EQUAL_INT64(480000, plan.k);
    TEST_ASSERT_EQUAL_UINT32(2 + 9, plan.sequence);
    const uint32_t gaps0 = r.stats().sequenceGaps;  // (the counts run since the open)
    r.startAt(plan);
    Run run = play(r, true, &plan, 1);
    TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
    TEST_ASSERT_EQUAL_UINT32(gaps0 + 1, r.stats().sequenceGaps);
    // Inside the lost page: 28,000 samples of its fill are kept from the
    // target, then the audio after it.
    checkPlanToEnd(r, 500000 - 312, kSixtyLength, 28000, true);
    // Right at the lost page's end.
    checkPlanToEnd(r, 528000 - 312, kSixtyLength, 0, true);
  }
  // A 12 s file with the page before the EOS page lost: the plan reads
  // the EOS page right after Q, the trim unknown after a gap.
  oggwriter::FileSpec twelve;
  twelve.packets = oggwriter::celtPackets(600);
  twelve.endTrim = 648;
  f = oggwriter::build(twelve);
  TEST_ASSERT_EQUAL_UINT32(2 + 12, f.pageOffsets.size());
  cut = f.bytes;
  cut.erase(cut.begin() + f.pageOffsets[2 + 10], cut.begin() + f.pageOffsets[2 + 11]);
  {
    oggwriter::Reader file(cut);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    const uint64_t length = 600 * 960 - 648 - 312;
    TEST_ASSERT_EQUAL_UINT64(length, r.lengthSamples());
    Run all = play(r);
    TEST_ASSERT_EQUAL_UINT64(length, all.kept);
    TEST_ASSERT_TRUE(all.finished);
    checkPlanToEnd(r, 540000 - 312, length, 0, true);
    checkPlanToEnd(r, 500000 - 312, length, 528000 - 648 - 500000, true);
  }
  // Every page continues a packet (1,201-byte packets, 4 segments a
  // page): Q is read itself, and the page after it lost drops the packet
  // spanning them; the fill is sized from the next page.
  oggwriter::FileSpec span;
  span.packets = oggwriter::celtPackets(400, 1200);
  span.maxSegments = 4;
  span.packetsPerPage = 0;
  span.endTrim = 100;
  f = oggwriter::build(span);
  cut = f.bytes;
  cut.erase(cut.begin() + f.pageOffsets[2 + 100], cut.begin() + f.pageOffsets[2 + 101]);
  {
    oggwriter::Reader file(cut);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    Run all = play(r);
    TEST_ASSERT_EQUAL_UINT64(f.kept, all.kept);
    TEST_ASSERT_EQUAL_UINT32(1, all.gaps);
    TEST_ASSERT_EQUAL_UINT32(0, all.jumps);
    uint32_t skips = 0;
    for (uint64_t t = 60000; t < 90000; t += 1999) {
      oggopus::StartPlan plan;
      r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
      r.startAt(plan);
      const Run run = play(r, true, &plan);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
      TEST_ASSERT_EQUAL_UINT64(f.kept - t, run.kept);
      TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
      TEST_ASSERT_TRUE(run.finished);
      if (plan.skipPage) ++skips;
    }
    TEST_ASSERT_TRUE(skips > 5);
  }
}

// The probes believe page headers they never check whole; Q is read whole
// before the plan is believed, and a BOS page is checked before it ends a
// link. A granule field damaged low (the page's CRC wrong): the probes step
// over it (a granule under lo's past lo is damage or a later link, and a
// damaged one is passed over), the seek lands exactly, the plan's reads
// stay small. Damaged high, and the page becoming Q: Q's check fails, the
// page found before it stands in. A BOS flag set by damage on an audio
// page: not a link (chained() stays false, linkEnd() the file's size, a
// later seek past it reads as little as any), and not in the tail scan.
void test_damaged_probe_headers() {
  oggwriter::Built f = sixtySeconds();
  auto setGranuleNoCrc = [](Bytes& b, uint32_t at, int64_t granule) {
    for (int i = 0; i < 8; ++i) b[at + 6 + i] = static_cast<uint8_t>(static_cast<uint64_t>(granule) >> (8 * i));
  };
  // Page 10's granule 528,000 -> 3,712 (bit 19 flipped).
  Bytes low = f.bytes;
  setGranuleNoCrc(low, f.pageOffsets[2 + 10], 528000 ^ (1 << 19));
  {
    oggwriter::Reader file(low);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    Run all = play(r);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, all.kept);
    TEST_ASSERT_EQUAL_UINT64(48000, all.filled);  // the damaged page's packets are lost
    for (const uint64_t t : {uint64_t(549288), uint64_t(500000), uint64_t(1500000), uint64_t(2800000)}) {
      oggopus::StartPlan plan;
      r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
      TEST_ASSERT_TRUE(plan.bytes < 256 * 1024);
      r.startAt(plan);
      const Run run = play(r, true, &plan);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
      TEST_ASSERT_EQUAL_UINT64(kSixtyLength - t, run.kept);
      TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
      TEST_ASSERT_TRUE(run.finished);
    }
  }
  // Page 40's granule (41 s) reading as 10 s, then as 50 s: every seek
  // between 30 s and 54 s lands.
  for (const int64_t bad : {int64_t(10) * 48000, int64_t(50) * 48000}) {
    Bytes d = f.bytes;
    setGranuleNoCrc(d, f.pageOffsets[2 + 40], bad);
    oggwriter::Reader file(d);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    uint64_t worstBytes = 0;
    for (uint32_t ms = 30000; ms < 54000; ms += 250) {
      const uint64_t t = static_cast<uint64_t>(ms) * 48;
      oggopus::StartPlan plan;
      r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
      if (plan.bytes > worstBytes) worstBytes = plan.bytes;
      r.startAt(plan);
      const Run run = play(r, true, &plan, 1);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
    }
    TEST_ASSERT_TRUE(worstBytes < 256 * 1024);
  }
  // The BOS flag on audio page 10 (the CRC wrong).
  Bytes bos = f.bytes;
  bos[f.pageOffsets[2 + 10] + 5] |= ogg::kBos;
  {
    oggwriter::Reader file(bos);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_FALSE(r.chained());
    for (const uint64_t t : {uint64_t(10) * 48000 + 30000, uint64_t(50) * 48000, uint64_t(12) * 48000}) {
      oggopus::StartPlan plan;
      r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
      TEST_ASSERT_TRUE(plan.bytes < 256 * 1024);
      TEST_ASSERT_FALSE(r.chained());
      TEST_ASSERT_EQUAL_UINT32(file.size(), r.linkEnd());
      r.startAt(plan);
      const Run run = play(r, true, &plan, 1);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
      TEST_ASSERT_TRUE(run.skipped < 60);  // (the plan started a page or so before the target, not at 10 s)
    }
  }
  // The same on audio page 58, inside the tail's long window: not a link
  // there either, and the length is the EOS page's.
  bos = f.bytes;
  bos[f.pageOffsets[2 + 58] + 5] |= ogg::kBos;
  {
    oggwriter::Reader file(bos);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_FALSE(r.chained());
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, r.lengthSamples());
  }
}

// The probes' floor (probeOurs()'s floorGranule): a page of ours whose
// header granule is under the lo before it is read whole and, its CRC
// wrong, stepped over, so a probe or the walk that lands on a damaged
// page goes on to the page after it instead of ending the range there.
// Page D's granule set low (the CRC left wrong) for several D, and a plan
// for every half second of the file: each plan's Q is the last page whose
// granule is at or under P, as on the undamaged file, whichever page is
// damaged (the page before it when D is Q itself: the check of Q sends the
// plan back a page), and each lands on its sample with the packets of one
// page at most skipped. Without the floor a probe landing on D hands it
// back as a real page under lo's granule; the bisection then takes D for
// a later link's start and ends at D - 1, and the walk stops at D too, so
// Q is pages early for targets past it (the landing stays exact: pages of
// packets skipped by their TOCs instead).
void test_probe_floor_steps_over_damage() {
  oggwriter::Built f = sixtySeconds();
  constexpr int64_t kPage = 48000;  // 50 packets a page: page k's granule is (k + 1) * 48,000
  for (const uint32_t d : {10u, 20u, 30u, 36u, 38u, 39u, 45u, 52u}) {
    Bytes low = f.bytes;
    const uint32_t at = f.pageOffsets[2 + d];
    for (int i = 0; i < 8; ++i) low[at + 6 + i] = static_cast<uint8_t>(static_cast<uint64_t>(3712) >> (8 * i));
    oggwriter::Reader file(low);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, r.lengthSamples());
    for (uint32_t ms = 5000; ms < 56000; ms += 500) {
      const uint64_t t = static_cast<uint64_t>(ms) * 48;
      const int64_t P = static_cast<int64_t>(t) + 312 - static_cast<int64_t>(oggopus::kSeekPrerollSamples);
      int64_t q = P / kPage - 1;  // the last page with granule <= P
      if (q == static_cast<int64_t>(d)) --q;  // Q itself damaged: the page before it
      oggopus::StartPlan plan;
      r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
      TEST_ASSERT_FALSE(plan.fromTop);
      TEST_ASSERT_EQUAL_INT64((q + 1) * kPage, plan.k);
      TEST_ASSERT_TRUE(plan.bytes < 256 * 1024);
      r.startAt(plan);
      const Run run = play(r, true, &plan, 1);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
      TEST_ASSERT_TRUE(run.skipped < 60);
    }
  }
}

// The plan's reads, the walk after the bisection included, stay within
// its budget on a crafted file: 200 pages of ours, one 20 ms packet each,
// 120 KB of junk (no 'O' in it) after every one, and a last page whose
// granule is far ahead so the interpolation creeps a page at a time. The
// bisection stops at 1 MB; the walk then stepped 64 pages at ~120 KB each
// before it had a budget. The plan starts earlier than it need and is
// still exact (the pages between are skipped by their TOCs).
void test_plan_walk_budget() {
  oggwriter::Muxer m(0x1234);
  m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
  m.flush();
  m.packet(oggwriter::opusTags(), 0);
  m.flush();
  Bytes junk(120 * 1024);
  uint32_t x = 99;
  for (uint8_t& b : junk) {
    x = x * 1103515245u + 12345u;
    b = static_cast<uint8_t>(x >> 16);
    if (b == 'O') b = 0;
  }
  int64_t k = 0;
  for (uint32_t i = 0; i < 200; ++i) {
    k += 960;
    m.packet(oggwriter::celt20ms(100, i + 1), k);
    m.flush();
    m.raw(junk);
  }
  m.packet(oggwriter::celt20ms(100, 7), static_cast<int64_t>(1) << 39);
  m.flush(true);
  oggwriter::Reader file(m.out);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  const uint64_t t = 185000;  // Q would be page 183
  oggopus::StartPlan plan;
  r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
  TEST_ASSERT_FALSE(plan.fromTop);
  TEST_ASSERT_TRUE(plan.bytes <= (1u << 20) + 2 * ogg::kMaxPageBytes + 16384);  // the budget, a probe's overshoot, Q
  TEST_ASSERT_TRUE(plan.k < 183 * 960);
  r.startAt(plan);
  const Run run = play(r, true, &plan, 1);
  TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
}

// One tiny packet a page (a muxer that flushes every packet, as
// low-delay recorders do): pages of 39-54 bytes. The tail walk parses
// headers from a chunk, not one 290-byte read a page, so a window's
// budget holds to the file's end and the length is exact.
void test_tiny_pages() {
  for (const uint16_t bytes : {uint16_t(10), uint16_t(25), uint16_t(60)}) {
    oggwriter::FileSpec spec;
    spec.packets = oggwriter::celtPackets(3000, bytes);
    spec.packetsPerPage = 1;
    spec.endTrim = 648;
    oggwriter::Built f = oggwriter::build(spec);
    TEST_ASSERT_EQUAL_UINT32(2 + 3000, f.pageOffsets.size());
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    file.reads = 0;
    file.bytes = 0;
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT64(f.kept, r.lengthSamples());
    TEST_ASSERT_TRUE(file.bytes < 64 * 1024);  // the short window: a chunk or two, and the last page
    TEST_ASSERT_TRUE(file.reads < 16);
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT64(f.kept, run.kept);
    TEST_ASSERT_TRUE(run.finished);
    // A plan on such a file.
    oggopus::StartPlan plan;
    r.planStart(1000000, oggopus::kSeekPrerollSamples, &plan);
    r.startAt(plan);
    run = play(r, true, &plan, 1);
    TEST_ASSERT_EQUAL_INT64(1000000, run.firstT);
  }
}

// A second link under our own serial number (RFC 3533 forbids it; two
// ffmpeg bitexact outputs joined do it). Its BOS page ends play, and the
// tail walk stops counting pages at it, so a second link inside the tail
// window leaves the first link's exact length. (A second link longer than
// the window reads as one stream: unsupported, as the class comment says.)
void test_same_serial_chain() {
  oggwriter::Built first = oggwriter::build(usualSpec());
  oggwriter::FileSpec secondSpec = usualSpec();
  secondSpec.packets = oggwriter::celtPackets(10);
  oggwriter::Built second = oggwriter::build(secondSpec);
  Bytes chained = first.bytes;
  chained.insert(chained.end(), second.bytes.begin(), second.bytes.end());
  oggwriter::Reader file(chained);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_TRUE(r.chained());
  TEST_ASSERT_EQUAL_UINT32(first.bytes.size(), r.linkEnd());
  TEST_ASSERT_EQUAL_INT64(95352, r.lastGranule());
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
  Run run = play(r);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
  TEST_ASSERT_TRUE(run.finished);
  // A plan never lands in the second link.
  oggopus::StartPlan plan;
  r.planStart(60000, oggopus::kSeekPrerollSamples, &plan);
  TEST_ASSERT_TRUE(plan.pageOffset < first.bytes.size());
  r.startAt(plan);
  run = play(r, true, &plan);
  TEST_ASSERT_EQUAL_INT64(60000, run.firstT);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept - 60000, run.kept);
}

// One next() call's share of pages that yield no packet: 10,000 empty
// pages of ours mid-stream (27 B each) cost at most ~65 reads a call and
// come back as Pending between, so the generator's pass ends; the
// packets after them all arrive. 2,000 pages of another stream, and one
// endless packet (60 pages of 255-byte segments, dropped as oversized),
// the same. Before the first granule, the open takes 1,500 empty pages
// (40 KB of file, 420 KB of header reads: each costs 282 bytes) and
// refuses 10,000 (past its 128 KB of file and 512 KB of reads). The
// OpusTags skip gives up after 4,096 pages.
void test_next_bounded() {
  auto headers = [](oggwriter::Muxer& m) {
    m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
    m.flush();
    m.packet(oggwriter::opusTags(), 0);
    m.flush();
  };
  {
    oggwriter::Muxer m(0x1234);
    headers(m);
    int64_t k = 0;
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 1), k += 960);
    m.flush();
    for (uint32_t i = 0; i < 10000; ++i) m.emptyPage();
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 10), k += 960);
    m.flush(true);
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(10, run.packets);
    TEST_ASSERT_EQUAL_UINT64(10 * 960 - 312, run.kept);
    TEST_ASSERT_TRUE(run.finished);
    TEST_ASSERT_TRUE(run.pending >= 150 && run.pending <= 160);
    TEST_ASSERT_TRUE(run.maxCallReads <= 70);
    TEST_ASSERT_EQUAL_UINT32(10002, r.stats().pages);
  }
  {
    oggwriter::Muxer m(0x1234);
    headers(m);
    int64_t k = 0;
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 1), k += 960);
    m.flush();
    for (uint32_t i = 0; i < 2000; ++i) {
      oggwriter::PageSpec s;
      s.serial = 0x9999;
      s.sequence = i + 1;
      s.granule = i;
      s.lacing = {255, 255, 255, 255, 10};
      s.body.assign(1030, 0x11);
      m.raw(oggwriter::page(s));
    }
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 10), k += 960);
    m.flush(true);
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(10, run.packets);
    TEST_ASSERT_EQUAL_UINT64(10 * 960 - 312, run.kept);
    TEST_ASSERT_TRUE(run.pending >= 30 && run.pending <= 32);
    TEST_ASSERT_TRUE(run.maxCallReads <= 140);
    TEST_ASSERT_EQUAL_UINT32(2000, r.stats().foreignPages);
  }
  {
    oggwriter::Muxer m(0x1234);
    headers(m);
    int64_t k = 0;
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 1), k += 960);
    m.flush();
    m.packet(Bytes(60 * 65025, 0x22), -1);  // 3.9 MB: never a packet
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 10), k += 960);
    m.flush(true);
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(10, run.packets);
    TEST_ASSERT_EQUAL_UINT32(1, r.stats().oversized);
    TEST_ASSERT_TRUE(run.pending >= 20);
    TEST_ASSERT_TRUE(run.maxCallReads <= 8);  // two 65 KB pages and a few headers
  }
  for (const uint32_t empties : {1500u, 10000u}) {
    oggwriter::Muxer m(0x1234);
    headers(m);
    for (uint32_t i = 0; i < empties; ++i) m.emptyPage();
    int64_t k = 0;
    for (int i = 0; i < 5; ++i) m.packet(oggwriter::celt20ms(300, i + 10), k += 960);
    m.flush(true);
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(empties == 1500 ? oggopus::Reader::Open::Ok : oggopus::Reader::Open::NoAudio, r.open());
    TEST_ASSERT_TRUE(file.bytes < 1024 * 1024);
    if (empties == 1500) {
      Run run = play(r);
      TEST_ASSERT_EQUAL_UINT32(5, run.packets);
      TEST_ASSERT_TRUE(run.finished);
    }
  }
  // OpusTags over 1,001 pages of one 255-byte segment: fine; over 4,097:
  // no audio within the skip's page cap.
  for (const size_t pages : {size_t(1001), size_t(4097)}) {
    oggwriter::FileSpec spec = usualSpec();
    spec.maxSegments = 1;
    spec.tagsPad = (pages - 1) * 255 + 100;
    oggwriter::Built f = oggwriter::build(spec);
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(pages == 1001 ? oggopus::Reader::Open::Ok : oggopus::Reader::Open::NoAudio, r.open());
    TEST_ASSERT_TRUE(file.bytes < 2 * 1024 * 1024);
    if (pages == 1001) {
      TEST_ASSERT_EQUAL_UINT32(1001, r.tagsPages());
      Run run = play(r);
      TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
    }
  }
}

// A seek past a stretch of destroyed pages longer than the 10 s rule
// (audio pages 20-31 of the 60 s file zeroed, headers and all): Q is the
// last page before the stretch, reading crosses it, and the gap's part
// before the first kept sample is stepped over, not filled, so a seek to
// the first good page after it plays (exactly), where it ended the track
// before. A target inside the stretch keeps the fill from the target on
// (7 s of silence for 25 s, then the audio), one whose heard fill would
// be over 10 s (21 s) ends the track, and from the top the stretch still
// ends it.
void test_seek_past_damage() {
  oggwriter::Built f = sixtySeconds();
  Bytes d = f.bytes;
  for (uint32_t i = f.pageOffsets[2 + 20]; i < f.pageOffsets[2 + 32]; ++i) d[i] = 0;
  oggwriter::Reader file(d);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  Run all = play(r);
  TEST_ASSERT_TRUE(all.gapEnded);
  TEST_ASSERT_EQUAL_UINT64(20 * 48000 - 312, all.kept);
  for (const uint32_t ms : {32000u, 32500u, 32900u, 33500u, 36000u}) {
    const uint64_t t = static_cast<uint64_t>(ms) * 48;
    oggopus::StartPlan plan;
    r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
    r.startAt(plan);
    const Run run = play(r, true, &plan);
    TEST_ASSERT_FALSE(run.gapEnded);
    TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength - t, run.kept);
    TEST_ASSERT_EQUAL_UINT64(0, run.filled);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
  {
    const uint64_t t = 25 * 48000;
    oggopus::StartPlan plan;
    r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
    r.startAt(plan);
    const Run run = play(r, true, &plan);
    TEST_ASSERT_FALSE(run.gapEnded);
    TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
    TEST_ASSERT_EQUAL_UINT64(7 * 48000 - 312, run.filled);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength - t, run.kept);
    TEST_ASSERT_TRUE(run.finished);
  }
  {
    oggopus::StartPlan plan;
    r.planStart(21 * 48000, oggopus::kSeekPrerollSamples, &plan);
    r.startAt(plan);
    const Run run = play(r, true, &plan);
    TEST_ASSERT_TRUE(run.gapEnded);
    TEST_ASSERT_EQUAL_UINT64(0, run.kept);
  }
}

// The plan on a cropped-start file: g0 123,457 with an odd pre-skip of
// 3,841, ~1 s pages that end inside a packet (a muxer filling its 255
// segments), every 5,003rd sample played to the EOS trim; then a file
// whose every page continues a packet. Every plan lands on its sample and
// keeps exactly the rest of the track (a plan that dropped g0 from the
// target, or from its k, passes the g0 = 0 sweep and fails here).
void test_start_plan_cropped() {
  for (int variant = 0; variant < 2; ++variant) {
    oggwriter::FileSpec spec;
    for (uint32_t i = 0; i < 1000; ++i) {
      const uint16_t bytes = static_cast<uint16_t>(variant == 0 ? 300 + (i * 7919u) % 600u : 60 + (i * 7919u) % 481u);
      spec.packets.push_back(oggwriter::celt20ms(bytes, i + 1));
    }
    spec.endTrim = 777;
    spec.g0 = 123457;
    spec.head.preSkip = 3841;
    spec.packetsPerPage = 0;
    if (variant == 1) spec.maxSegments = 3;
    oggwriter::Built f = oggwriter::build(spec);
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open());
    TEST_ASSERT_EQUAL_INT64(123457, r.g0());
    TEST_ASSERT_TRUE(r.scanTail());
    const uint64_t length = r.lengthSamples();
    TEST_ASSERT_EQUAL_UINT64(f.kept, length);
    uint32_t plans = 0, skips = 0, fromTop = 0;
    for (uint64_t t = 0; t < length; t += variant == 0 ? 5003 : 9973) {
      oggopus::StartPlan plan;
      r.planStart(t, oggopus::kSeekPrerollSamples, &plan);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t) + 123457 + 3841, plan.keepFrom);
      r.startAt(plan);
      const Run run = play(r, true, &plan);
      TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), run.firstT);
      TEST_ASSERT_EQUAL_UINT64(length - t, run.kept);
      TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
      TEST_ASSERT_TRUE(run.finished);
      if (!plan.fromTop) {
        TEST_ASSERT_TRUE(run.decodedBefore >= oggopus::kSeekPrerollSamples);
        TEST_ASSERT_TRUE(run.decodedBefore < oggopus::kSeekPrerollSamples + 960);
      } else {
        ++fromTop;
      }
      if (plan.skipPage) ++skips;
      ++plans;
    }
    std::printf("  cropped start, variant %d: %lu plans, %lu from the top, %lu at Q itself\n", variant,
                (unsigned long)plans, (unsigned long)fromTop, (unsigned long)skips);
    std::fflush(stdout);
    TEST_ASSERT_TRUE(plans > 90);
    TEST_ASSERT_TRUE(fromTop < plans / 4);
    TEST_ASSERT_TRUE(skips > plans / 5);  // (pages end mid-packet a good part of the time)
  }
}

// Frames under 10 ms are refused at the open, by the first audio page's
// TOCs (kMinFrameSamples): 2.5 and 5 ms CELT frames (configs 16 and 17),
// in code-0 and code-3 packets alike; 10 ms SILK, hybrid and CELT frames
// (configs 0, 12, 18) play. A file whose first page holds 20 ms frames and
// a later page 2.5 ms ones isn't the open's to refuse: it plays, slowly.
void test_short_frames_refused() {
  using Open = oggopus::Reader::Open;
  char text[128];
  const char* const k2p5 = "Opus with 2.5 ms frames isn't supported (10 ms or longer only: too slow to decode here)";
  const char* const k5 = "Opus with 5 ms frames isn't supported (10 ms or longer only: too slow to decode here)";
  struct Case {
    uint8_t config;
    uint8_t code;
    Open open;
    const char* text;
    const char* note;  // the screen's form (refusalNote())
  };
  const Case cases[] = {
      {16, 0, Open::ShortFrames, k2p5, "Opus with 2.5 ms frames isn't supported"},
      {17, 0, Open::ShortFrames, k5, "Opus with 5 ms frames isn't supported"},
      {16, 3, Open::ShortFrames, k2p5, "Opus with 2.5 ms frames isn't supported"},
      {18, 0, Open::Ok, "", ""},
      {0, 0, Open::Ok, "", ""},
      {12, 0, Open::Ok, "", ""},
  };
  for (const Case& c : cases) {
    oggwriter::FileSpec spec;
    for (uint32_t i = 0; i < 400; ++i) {
      spec.packets.push_back(c.code == 3 ? oggwriter::opusPacket(c.config, true, 3, {40, 40, 40, 40}, true, 0, i + 1)
                                         : oggwriter::opusPacket(c.config, true, 0, {40}, true, 0, i + 1));
    }
    spec.packetsPerPage = 100;
    oggwriter::Built f = oggwriter::build(spec);
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(c.open, r.open());
    TEST_ASSERT_EQUAL_STRING(c.text, r.refusal(text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING(c.note, r.refusalNote(text, sizeof(text)));
    if (c.open == Open::Ok) {
      Run run = play(r);
      TEST_ASSERT_EQUAL_UINT64(f.kept, run.kept);
      TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
      TEST_ASSERT_TRUE(run.finished);
    }
  }
  {
    oggwriter::FileSpec spec;
    spec.packets = oggwriter::celtPackets(50);
    for (uint32_t i = 0; i < 400; ++i) spec.packets.push_back(oggwriter::opusPacket(16, true, 0, {40}, true, 0, i + 1));
    spec.packetsPerPage = 50;
    oggwriter::Built f = oggwriter::build(spec);
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open());
    Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(450, run.packets);
    TEST_ASSERT_EQUAL_UINT64(f.kept, run.kept);
    TEST_ASSERT_TRUE(run.finished);
  }
}

// The audio pages read in slices (Reader::setReadSlice(); the generator
// asks for 8 KB): a file of ~64 KB pages (50 packets of 1,271 bytes a
// page, as a 510k file has them) plays the same as read whole, next()
// saying Pending once a slice (seven a page), no byte read twice and no
// call making more than the two reads a whole page's does; a damaged page
// is found out by its CRC once it is whole and bridged as before; a
// restart between two slices of the first audio page reads on where it
// was; and the open's first-page search takes the slices in its stride.
void test_sliced_page_reads() {
  using Open = oggopus::Reader::Open;
  oggwriter::FileSpec spec;
  spec.packets = oggwriter::celtPackets(200, 1270);
  spec.packetsPerPage = 50;
  spec.endTrim = 100;
  oggwriter::Built f = oggwriter::build(spec);
  TEST_ASSERT_EQUAL_UINT32(2 + 4, f.pageOffsets.size());
  const uint32_t pageBytes = f.pageOffsets[3] - f.pageOffsets[2];
  TEST_ASSERT_EQUAL_UINT32(27 + 250 + 50 * 1271, pageBytes);  // 63,827 (a TOC byte each): eight 8 KB slices
  // Read whole: the reference. The open leaves the first audio page in
  // hand (read whole, whatever the slice), so a play after it reads the
  // three pages after: two reads each whole, nine in slices (the header,
  // eight slices), a Pending after each slice and one more once the page
  // is whole and checked (the decode a step of its own: M4), eight a page.
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(Open::Ok, r.open());
  file.reads = 0;
  file.bytes = 0;
  const Run whole = play(r);
  TEST_ASSERT_EQUAL_UINT64(f.kept, whole.kept);
  TEST_ASSERT_EQUAL_UINT32(0, whole.pending);
  TEST_ASSERT_EQUAL_UINT32(2, whole.maxCallReads);
  const uint32_t wholeReads = file.reads;
  const uint64_t wholeBytes = file.bytes;
  TEST_ASSERT_EQUAL_UINT32(3 * 2, wholeReads);
  TEST_ASSERT_EQUAL_UINT64(3u * pageBytes, wholeBytes);
  // In 8 KB slices.
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  r2.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, r2.open());
  file2.reads = 0;
  file2.bytes = 0;
  const Run sliced = play(r2);
  TEST_ASSERT_EQUAL_UINT64(f.kept, sliced.kept);
  TEST_ASSERT_EQUAL_UINT32(0, sliced.jumps);
  TEST_ASSERT_EQUAL_UINT32(200, sliced.packets);
  TEST_ASSERT_TRUE(sliced.finished);
  TEST_ASSERT_EQUAL_UINT32(3 * 8, sliced.pending);
  TEST_ASSERT_EQUAL_UINT32(wholeReads + 3 * 7, file2.reads);
  TEST_ASSERT_EQUAL_UINT64(wholeBytes, file2.bytes);
  TEST_ASSERT_EQUAL_UINT32(2, sliced.maxCallReads);
  // The second audio page damaged: the whole-read play and the sliced one
  // bridge it the same way (one gap of 48,000 samples, the length exact).
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[3] + 30000] ^= 0x01;
  oggwriter::Reader dfile(damaged);
  oggopus::Reader rd(dfile, dfile.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(Open::Ok, rd.open());
  const Run dwhole = play(rd);
  oggwriter::Reader dfile2(damaged);
  oggopus::Reader rd2(dfile2, dfile2.size(), pageBuf, packetBuf);
  rd2.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, rd2.open());
  const Run dsliced = play(rd2);
  TEST_ASSERT_EQUAL_UINT32(1, dwhole.gaps);
  TEST_ASSERT_EQUAL_UINT64(48000, dwhole.filled);
  TEST_ASSERT_EQUAL_UINT64(f.kept, dwhole.kept);
  TEST_ASSERT_EQUAL_UINT32(dwhole.gaps, dsliced.gaps);
  TEST_ASSERT_EQUAL_UINT64(dwhole.filled, dsliced.filled);
  TEST_ASSERT_EQUAL_UINT64(dwhole.kept, dsliced.kept);
  TEST_ASSERT_EQUAL_UINT32(0, dsliced.jumps);
  TEST_ASSERT_EQUAL_UINT32(1, rd2.stats().badPages);
  TEST_ASSERT_EQUAL_UINT32(1, rd2.stats().resyncs);
  TEST_ASSERT_TRUE(dsliced.finished);
  // The first audio page is in hand after the open: its 50 packets come
  // with no read and no Pending; a restart while it is still in hand
  // reads nothing either. Then, three slices into the second page, a
  // restart: the first page is read again (the buffer held the second's
  // slices), the second from its start (its three slices were for
  // nothing), and the track plays whole.
  oggwriter::Reader file3(f.bytes);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  r3.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, r3.open());
  file3.reads = 0;
  file3.bytes = 0;
  oggopus::Reader::Packet p;
  for (int i = 0; i < 50; ++i) TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, r3.next(&p));
  TEST_ASSERT_EQUAL_UINT32(0, file3.reads);
  r3.restart();
  for (int i = 0; i < 50; ++i) TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, r3.next(&p));
  TEST_ASSERT_EQUAL_UINT32(0, file3.reads);
  for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(oggopus::Reader::Next::Pending, r3.next(&p));
  TEST_ASSERT_EQUAL_UINT32(4, file3.reads);  // (the header and the first slice come in one call)
  r3.restart();
  file3.reads = 0;
  file3.bytes = 0;
  const Run again = play(r3);
  TEST_ASSERT_EQUAL_UINT64(f.kept, again.kept);
  TEST_ASSERT_EQUAL_UINT64(wholeBytes + pageBytes, file3.bytes);
  TEST_ASSERT_EQUAL_UINT32(4 * 8, again.pending);
}

// A start by a plan on a file read in slices, as the generator starts one
// (docs/OPUS.md section 9; OpusGenerator::begin() by setStartPlan()): the
// 64 KB pages of test_sliced_page_reads' file in 8 KB slices, a plan
// every 7,777 samples (the preroll's page, Q, is read whole by the plan,
// then the pages after it in slices), each landing on its sample through
// the Pendings (the first next() after startAt() may be one: a slice of
// the plan's page; a caller driving next() by hand loops on it, as
// play() does), the kept count what a play from the top keeps after the
// target, the bytes a plan reads the same as with whole reads but for the
// slices' headers. Then the same with the second audio page damaged: a
// plan past the damaged page lands exactly (the resync in steps isn't in
// its way), one inside the damaged page fills the gap from the target on,
// and, on the 60 s file with a page damaged, startAt() during the resync
// after it (a next() that said Pending mid-scan) to a start before the
// damage drops the scan with the position (kept, it would end the track).
void test_plan_through_slices() {
  using Open = oggopus::Reader::Open;
  oggwriter::FileSpec spec;
  spec.packets = oggwriter::celtPackets(200, 1270);
  spec.packetsPerPage = 50;
  spec.endTrim = 100;
  oggwriter::Built f = oggwriter::build(spec);
  // Whole reads: the reference plans and counts.
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(Open::Ok, r.open());
  TEST_ASSERT_TRUE(r.scanTail());
  TEST_ASSERT_EQUAL_UINT64(f.kept, r.lengthSamples());
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  r2.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, r2.open());
  TEST_ASSERT_TRUE(r2.scanTail());
  uint32_t plans = 0, pendings = 0, notFromTop = 0;
  for (uint64_t t = 7777; t + 1 < f.kept; t += 7777) {
    oggopus::StartPlan whole, sliced;
    r.planStart(t, oggopus::kSeekPrerollSamples, &whole);
    file2.reads = 0;
    file2.bytes = 0;
    r2.planStart(t, oggopus::kSeekPrerollSamples, &sliced);
    // The plan doesn't depend on the slice (it reads whole pages).
    TEST_ASSERT_EQUAL(whole.fromTop, sliced.fromTop);
    TEST_ASSERT_EQUAL_UINT32(whole.pageOffset, sliced.pageOffset);
    TEST_ASSERT_EQUAL_INT64(whole.k, sliced.k);
    TEST_ASSERT_EQUAL_INT64(whole.decodeFrom, sliced.decodeFrom);
    TEST_ASSERT_EQUAL_UINT64(whole.bytes, sliced.bytes);
    TEST_ASSERT_EQUAL_UINT64(t, sliced.target);
    r.startAt(whole);
    const Run a = play(r, true, &whole);
    r2.startAt(sliced);
    const Run b = play(r2, true, &sliced);
    TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), a.firstT);
    TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(t), b.firstT);
    TEST_ASSERT_EQUAL_UINT64(f.kept - t, a.kept);
    TEST_ASSERT_EQUAL_UINT64(f.kept - t, b.kept);
    TEST_ASSERT_TRUE(a.finished);
    TEST_ASSERT_TRUE(b.finished);
    TEST_ASSERT_EQUAL_UINT32(0, b.jumps);
    TEST_ASSERT_EQUAL_UINT32(a.skipped, b.skipped);
    TEST_ASSERT_EQUAL_UINT64(a.decodedBefore, b.decodedBefore);
    TEST_ASSERT_EQUAL_UINT32(0, a.pending);
    TEST_ASSERT_TRUE(b.maxCallBytes <= 8192 + ogg::kMaxHeaderBytes + ogg::PageReader::kPeekBytes);
    pendings += b.pending;
    if (!sliced.fromTop) ++notFromTop;
    ++plans;
  }
  TEST_ASSERT_TRUE(plans >= 20);
  TEST_ASSERT_TRUE(notFromTop >= 15);
  TEST_ASSERT_TRUE(pendings > plans);  // the pages after Q come in slices
  // The first next() after startAt() says Pending on a sliced file (the
  // plan's page, a slice of it): the generator's pass ends there and the
  // next pass asks again.
  {
    oggopus::StartPlan plan;
    r2.planStart(100000, oggopus::kSeekPrerollSamples, &plan);
    TEST_ASSERT_FALSE(plan.fromTop);
    r2.startAt(plan);
    oggopus::Reader::Packet p;
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Pending, r2.next(&p));
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Packet, nextPacket(r2, &p));
    r2.restart();
  }
  // The second audio page damaged (its samples 48,000-95,999: a 48,000
  // sample gap the plan from the top fills).
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[3] + 30000] ^= 0x01;
  oggwriter::Reader dfile(damaged);
  oggopus::Reader rd(dfile, dfile.size(), pageBuf, packetBuf);
  rd.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, rd.open());
  TEST_ASSERT_TRUE(rd.scanTail());
  const Run top = play(rd);
  TEST_ASSERT_EQUAL_UINT64(48000, top.filled);
  TEST_ASSERT_EQUAL_UINT64(f.kept, top.kept);
  // Past the damaged page: exact, nothing filled. The probes see the
  // damaged page's intact header (granule 96,000, under P) and take it
  // for Q; read whole, its CRC fails, so the page before it stands in
  // (an earlier start, still exact) and reading begins at the damaged
  // page: the resync's steps, then the gap, which lies wholly before the
  // first kept sample and is stepped over unheard.
  {
    oggopus::StartPlan plan;
    rd.planStart(120000, oggopus::kSeekPrerollSamples, &plan);
    TEST_ASSERT_FALSE(plan.fromTop);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], plan.pageOffset);
    TEST_ASSERT_EQUAL_INT64(48000, plan.k);
    rd.startAt(plan);
    const Run run = play(rd, true, &plan);
    TEST_ASSERT_EQUAL_INT64(120000, run.firstT);
    TEST_ASSERT_EQUAL_UINT64(f.kept - 120000, run.kept);
    TEST_ASSERT_EQUAL_UINT64(0, run.filled);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
  // Inside the damaged page (sample 60,000 is 60,312 absolute): the plan
  // reads from the first page's end, the scan past the damage goes in
  // steps (Pendings), and the gap is filled from the target on: 35,688 of
  // fill (96,000 - 60,312), then the rest, the length exact.
  {
    oggopus::StartPlan plan;
    rd.planStart(60000, oggopus::kSeekPrerollSamples, &plan);
    rd.startAt(plan);
    const Run run = play(rd, true, &plan);
    TEST_ASSERT_EQUAL_INT64(60000, run.firstT);
    TEST_ASSERT_EQUAL_UINT64(f.kept - 60000, run.kept);
    TEST_ASSERT_EQUAL_UINT64(96000 - 60312, run.filled);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.pending > 0);
    TEST_ASSERT_TRUE(run.finished);
  }
  // startAt() mid-scan: a next() that said Pending inside the resync after
  // a damaged page, then a plan to a start before it (a seek back during
  // the resync). The scan has to go with the position: kept, it would run
  // on from where it was and hand over the page after the damage with the
  // timeline at the plan's count, a gap of 22 s that ends the track
  // (kMaxGapSamples). On the 60 s file of 1 s pages with its audio page 30
  // (samples 1,440,000-1,487,999) damaged, read in slices: a start at
  // 29.5 s, the page before the damage out, the damage found, one chunk of
  // the scan, then a plan at 10 s: it lands exactly, the gap is filled when
  // the play reaches it, the damage found out twice and bridged once. (A
  // plan past the damage couldn't tell: the scan begun there and the
  // plan's own find the same next page, so a kept scan lands there too.)
  {
    oggwriter::Built sixty = sixtySeconds();
    Bytes dmg = sixty.bytes;
    dmg[sixty.pageOffsets[2 + 30] + 100] ^= 0x01;
    oggwriter::Reader sfile(dmg);
    oggopus::Reader sr(sfile, sfile.size(), pageBuf, packetBuf);
    sr.setReadSlice(8192);
    TEST_ASSERT_EQUAL(Open::Ok, sr.open());
    TEST_ASSERT_TRUE(sr.scanTail());
    oggopus::StartPlan before;
    sr.planStart(1416000, oggopus::kSeekPrerollSamples, &before);  // 29.5 s: reading starts at page 29
    TEST_ASSERT_FALSE(before.fromTop);
    TEST_ASSERT_EQUAL_UINT32(sixty.pageOffsets[2 + 29], before.pageOffset);
    sr.startAt(before);
    oggopus::Reader::Packet p;
    // Page 29's packets out (the damaged page's slices say Pending until
    // its CRC is judged: badPages 1, the scan begun), then one step of the
    // scan, a 4 KB chunk inside the ~15 KB damaged page: mid-scan, the
    // scan not done (resyncs 0).
    uint32_t calls = 0;
    while (sr.stats().badPages == 0) {
      TEST_ASSERT_TRUE(sr.next(&p) != oggopus::Reader::Next::End);
      TEST_ASSERT_TRUE(++calls < 200);
    }
    TEST_ASSERT_EQUAL(oggopus::Reader::Next::Pending, sr.next(&p));
    TEST_ASSERT_EQUAL_UINT32(0, sr.stats().resyncs);
    oggopus::StartPlan back;
    sr.planStart(480000, oggopus::kSeekPrerollSamples, &back);  // 10 s
    TEST_ASSERT_FALSE(back.fromTop);
    sr.startAt(back);
    const Run run = play(sr, true, &back);
    TEST_ASSERT_FALSE(run.gapEnded);
    TEST_ASSERT_EQUAL_INT64(480000, run.firstT);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength - 480000, run.kept);
    TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
    TEST_ASSERT_EQUAL_UINT64(48000, run.filled);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
    TEST_ASSERT_EQUAL_UINT32(2, sr.stats().badPages);  // found out twice
    TEST_ASSERT_EQUAL_UINT32(1, sr.stats().resyncs);   // bridged once: the first scan was dropped
  }
}

// The resync after a damaged page made one read a next() call (the
// reader's Scan: a 4 KB chunk of the file, or a slice of the page it
// finds), Pending between them, so the decode task's pass can end there as
// it does between a page's slices. On the 64 KB pages of
// test_sliced_page_reads' file with the second audio page damaged, read in
// the generator's 8 KB slices, no call reads more than a slice and a
// header, where one call read the scan over the damaged page and the 64 KB
// page after it before (the device measured 139 ms on the 510k file), and
// the track plays as read whole: one gap of 48,000 filled, the length
// exact, the same bytes read. Read whole (no slice) the chunks are steps
// too and the page found is the one long step left. The 200 KB of zeros of
// test_resync_bounded the same way: fifty chunks, each a call.
void test_resync_in_steps() {
  using Open = oggopus::Reader::Open;
  oggwriter::FileSpec spec;
  spec.packets = oggwriter::celtPackets(200, 1270);
  spec.packetsPerPage = 50;
  spec.endTrim = 100;
  oggwriter::Built f = oggwriter::build(spec);
  const uint32_t pageBytes = f.pageOffsets[4] - f.pageOffsets[3];
  TEST_ASSERT_EQUAL_UINT32(63827, pageBytes);
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[3] + 30000] ^= 0x01;
  // The chunks the scan takes from the byte after the damaged page's start
  // to the capture pattern of the page after it (each chunk goes on from
  // three bytes before its end: a pattern can straddle two).
  const uint32_t chunks = (pageBytes - 1) / (ogg::PageReader::kScanChunk - 3) + 1;
  TEST_ASSERT_EQUAL_UINT32(16, chunks);
  // Read whole: the reference.
  oggwriter::Reader file(damaged);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(Open::Ok, r.open());
  file.reads = 0;
  file.bytes = 0;
  const Run whole = play(r);
  TEST_ASSERT_EQUAL_UINT32(1, whole.gaps);
  TEST_ASSERT_EQUAL_UINT64(48000, whole.filled);
  TEST_ASSERT_EQUAL_UINT64(f.kept, whole.kept);
  TEST_ASSERT_EQUAL_UINT32(0, whole.jumps);
  TEST_ASSERT_TRUE(whole.finished);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().badPages);
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().resyncs);
  TEST_ASSERT_EQUAL_UINT32(pageBytes, r.stats().resyncBytes);
  // The damage found out (one Pending, the scan begun), then a chunk a
  // call; the page found is read whole in the step that finds it.
  TEST_ASSERT_EQUAL_UINT32(1 + chunks, whole.pending);
  TEST_ASSERT_TRUE(whole.maxCallBytes >= pageBytes);
  // In 8 KB slices: the same track, no call over a slice and a header.
  oggwriter::Reader file2(damaged);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  r2.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, r2.open());
  file2.reads = 0;
  file2.bytes = 0;
  const Run sliced = play(r2);
  TEST_ASSERT_EQUAL_UINT32(whole.gaps, sliced.gaps);
  TEST_ASSERT_EQUAL_UINT64(whole.filled, sliced.filled);
  TEST_ASSERT_EQUAL_UINT64(whole.kept, sliced.kept);
  TEST_ASSERT_EQUAL_UINT32(0, sliced.jumps);
  TEST_ASSERT_TRUE(sliced.finished);
  TEST_ASSERT_EQUAL_UINT32(1, r2.stats().resyncs);
  TEST_ASSERT_EQUAL_UINT32(pageBytes, r2.stats().resyncBytes);
  TEST_ASSERT_EQUAL_UINT64(file.bytes, file2.bytes);  // nothing read twice either way
  TEST_ASSERT_TRUE(sliced.maxCallBytes <= 8192 + ogg::kMaxHeaderBytes);
  TEST_ASSERT_EQUAL_UINT32(2, sliced.maxCallReads);
  // The clean pages' 8 Pendings each (test_sliced_page_reads: the first
  // audio page is in hand after the open and costs none; the page the
  // scan found included: its slices are the scan's last steps, and it is
  // whole and checked in one more), the damaged page's 7 and the one that
  // found it out, and the scan's chunks, as read whole.
  TEST_ASSERT_EQUAL_UINT32(2 * 8 + 7 + 1 + chunks, sliced.pending);
  // 200 KB of zeros where the 4th audio page should start (3 KB pages):
  // fifty chunks, each a call, and the small pages as before.
  oggwriter::FileSpec zspec = usualSpec();
  zspec.packetsPerPage = 10;
  oggwriter::Built zf = oggwriter::build(zspec);
  Bytes zeros = zf.bytes;
  zeros.insert(zeros.begin() + zf.pageOffsets[5], 200 * 1024, 0);
  oggwriter::Reader zfile(zeros);
  oggopus::Reader zr(zfile, zfile.size(), pageBuf, packetBuf);
  zr.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, zr.open());
  const Run z = play(zr);
  TEST_ASSERT_EQUAL_UINT32(100, z.packets);
  TEST_ASSERT_EQUAL_UINT64(kUsualKept, z.kept);
  TEST_ASSERT_EQUAL_UINT32(1, z.gaps);
  TEST_ASSERT_TRUE(z.finished);
  TEST_ASSERT_EQUAL_UINT32(1, zr.stats().resyncs);
  TEST_ASSERT_EQUAL_UINT32(200 * 1024, zr.stats().resyncBytes);
  TEST_ASSERT_TRUE(z.pending >= 50);
  TEST_ASSERT_TRUE(z.maxCallBytes <= ogg::PageReader::kScanChunk + ogg::kMaxHeaderBytes);
  // A restart in the middle of the scan drops it: the track plays from the
  // top again, the damage found out and bridged once more.
  oggwriter::Reader file3(damaged);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  r3.setReadSlice(8192);
  TEST_ASSERT_EQUAL(Open::Ok, r3.open());
  oggopus::Reader::Packet p;
  uint32_t calls = 0;
  for (uint32_t pendings = 0; pendings < 7 + 8 + 4; ++calls) {
    if (r3.next(&p) == oggopus::Reader::Next::Pending) ++pendings;  // (into the scan's chunks)
  }
  r3.restart();
  const Run again = play(r3);
  TEST_ASSERT_EQUAL_UINT64(f.kept, again.kept);
  TEST_ASSERT_EQUAL_UINT32(1, again.gaps);
  TEST_ASSERT_EQUAL_UINT32(2, r3.stats().badPages);  // found out twice
  TEST_ASSERT_EQUAL_UINT32(1, r3.stats().resyncs);   // bridged once: the first scan was dropped
}

// The M4 open (docs/OPUS.md section 10): open(true) scans the tail
// before the first audio page, which then stays in hand, so the whole
// open of a 60 s file of ~15 KB pages is five reads (the BOS page, the
// OpusTags page's header, the last 16 KB as one chunk with the last page
// checked in it, the first audio page whole) and the first next() reads
// nothing; a restart while the page is in hand reads nothing either. The
// tail windows: a file whose last page is longer than 16 KB (64 KB pages)
// is found by the 64 KB window, one more read, the page checked in
// memory; the last page itself and its granule are the ones found. A
// plan's Q, read whole by its check, stays in hand: a start at Q (a
// packet continues out of it) reads nothing until the page after; the
// plan's probes read no header at a guess (a chunk each), so a 60 s
// file's plans cost fewer reads than M3's (whose means test_start_plan
// prints). And with a slice set, the call that makes a page whole says
// Pending once more and the next call hands its first packet with no
// read: the decode is a step of its own.
void test_open_with_tail_and_page_in_hand() {
  using Open = oggopus::Reader::Open;
  using Next = oggopus::Reader::Next;
  oggwriter::Built f = sixtySeconds();
  const uint32_t lastPage = f.pageOffsets.back();
  TEST_ASSERT_TRUE(f.bytes.size() - lastPage < 16384);  // the last page is in the last 16 KB
  {
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    r.setReadSlice(8192);  // (the generator's; the open reads whole whatever it says)
    TEST_ASSERT_EQUAL(Open::Ok, r.open(true));
    TEST_ASSERT_EQUAL_UINT32(5, file.reads);
    const uint32_t firstPageBytes = f.pageOffsets[3] - f.pageOffsets[2];
    // (the BOS page's read asks for a header's 282 bytes, the tags page's
    // for 290, the window 16 KB, the first audio page what it is)
    TEST_ASSERT_EQUAL_UINT64(ogg::kMaxHeaderBytes + 290u + 16384u + firstPageBytes, file.bytes);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, r.lengthSamples());
    TEST_ASSERT_EQUAL_INT64(f.lastGranule, r.lastGranule());
    TEST_ASSERT_EQUAL_UINT32(lastPage, r.lastPageAt());
    TEST_ASSERT_EQUAL_UINT32(f.firstAudioPage, r.firstAudioPage());
    // The first packet with no read; a restart in hand; then the track.
    oggopus::Reader::Packet p;
    TEST_ASSERT_EQUAL(Next::Packet, r.next(&p));
    TEST_ASSERT_EQUAL_UINT32(5, file.reads);
    r.restart();
    TEST_ASSERT_EQUAL(Next::Packet, r.next(&p));
    TEST_ASSERT_EQUAL_UINT32(5, file.reads);
    r.restart();
    const Run run = play(r);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
    // The same length and last page as open() then scanTail().
    oggwriter::Reader file2(f.bytes);
    oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r2.open());
    TEST_ASSERT_EQUAL_UINT32(4, file2.reads);
    TEST_ASSERT_TRUE(r2.scanTail());
    TEST_ASSERT_EQUAL_UINT32(5, file2.reads);
    TEST_ASSERT_EQUAL_UINT64(kSixtyLength, r2.lengthSamples());
    TEST_ASSERT_EQUAL_UINT32(lastPage, r2.lastPageAt());
  }
  // 64 KB pages: the 16 KB window holds no header (the last page is
  // longer), the 64 KB window does, and the page is checked in memory:
  // two reads for the tail, no whole-page read.
  {
    oggwriter::FileSpec spec;
    spec.packets = oggwriter::celtPackets(200, 1270);
    spec.packetsPerPage = 50;
    spec.endTrim = 100;
    oggwriter::Built big = oggwriter::build(spec);
    TEST_ASSERT_TRUE(big.bytes.size() - big.pageOffsets.back() > 16384);
    oggwriter::Reader file(big.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open());
    file.reads = 0;
    file.bytes = 0;
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT32(2, file.reads);
    TEST_ASSERT_EQUAL_UINT64(16384u + ogg::kMaxPageBytes, file.bytes);
    TEST_ASSERT_EQUAL_UINT64(big.kept, r.lengthSamples());
    TEST_ASSERT_EQUAL_UINT32(big.pageOffsets.back(), r.lastPageAt());
  }
  // Q in hand after the plan: a spanning file (every page continues a
  // packet, so reading starts at Q itself): the first packet handed over
  // costs the page after Q alone (two reads), where M3 read Q again.
  {
    oggwriter::FileSpec span;
    span.packets = oggwriter::celtPackets(400, 1200);
    span.maxSegments = 4;
    span.packetsPerPage = 0;
    span.endTrim = 100;
    oggwriter::Built g = oggwriter::build(span);
    oggwriter::Reader file(g.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open(true));
    // (four pages in five continue a packet here: the first target whose
    // Q does)
    oggopus::StartPlan plan;
    uint64_t target = 200000;
    for (int i = 0; i < 8; ++i, target += 960) {
      r.planStart(target, oggopus::kSeekPrerollSamples, &plan);
      if (plan.skipPage) break;
    }
    TEST_ASSERT_FALSE(plan.fromTop);
    TEST_ASSERT_TRUE(plan.skipPage);
    r.startAt(plan);
    file.reads = 0;
    oggopus::Reader::Packet p;
    TEST_ASSERT_EQUAL(Next::Packet, nextPacket(r, &p));
    TEST_ASSERT_EQUAL_UINT32(2, file.reads);
    TEST_ASSERT_TRUE(p.pageOffset > plan.pageOffset);  // (the page after Q: the packet completes there)
    const Run run = play(r, true, &plan, 1);
    TEST_ASSERT_EQUAL_INT64(static_cast<int64_t>(target), run.firstT);
    // The same start with Q not in hand (another use of the buffer in
    // between): Q read again, two reads more.
    r.planStart(target, oggopus::kSeekPrerollSamples, &plan);
    r.restart();  // (the first audio page isn't in hand: a read, which takes the buffer)
    TEST_ASSERT_EQUAL(Next::Packet, nextPacket(r, &p));
    r.startAt(plan);
    file.reads = 0;
    TEST_ASSERT_EQUAL(Next::Packet, nextPacket(r, &p));
    TEST_ASSERT_EQUAL_UINT32(4, file.reads);
  }
  // The yield after a sliced page: on the 60 s file read in 8 KB slices,
  // after the first page's packets the next page takes three calls (the
  // header and a slice, a slice, the rest: Pending each, the last one the
  // page whole), then a fourth hands its first packet with no read.
  {
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    r.setReadSlice(8192);
    TEST_ASSERT_EQUAL(Open::Ok, r.open(true));
    oggopus::Reader::Packet p;
    for (int i = 0; i < 50; ++i) TEST_ASSERT_EQUAL(Next::Packet, r.next(&p));
    file.reads = 0;
    const uint32_t pageBytes = f.pageOffsets[4] - f.pageOffsets[3];
    const uint32_t slices = (pageBytes - ogg::kMaxHeaderBytes + 8191) / 8192;  // the body after the header's read
    for (uint32_t i = 0; i < slices; ++i) TEST_ASSERT_EQUAL(Next::Pending, r.next(&p));
    TEST_ASSERT_EQUAL_UINT32(slices + 1, file.reads);
    TEST_ASSERT_EQUAL(Next::Packet, r.next(&p));
    TEST_ASSERT_EQUAL_UINT32(slices + 1, file.reads);
    TEST_ASSERT_EQUAL_UINT32(f.pageOffsets[3], p.pageOffset);
  }
}

// A packet whose TOC reads but whose framing doesn't (a code-1 packet of
// an odd length: libopus refuses it too) is concealed for its TOC's
// duration, as the generator does, so the count runs on as the file says:
// the length holds with no correction at the page's end and no jump, and
// a gap before it goes into the same fill. A packet with no TOC (0 bytes)
// is dropped and the page's granule puts the count right, as before.
void test_malformed_packets_concealed() {
  using Open = oggopus::Reader::Open;
  oggwriter::FileSpec spec = usualSpec();
  spec.packets[30] = oggwriter::opusPacket(31, true, 1, {100, 101}, true, 0, 7);  // 201 bytes of two "equal" frames
  TEST_ASSERT_EQUAL_INT32(1920, oggopus::packetSamples(spec.packets[30].data(), spec.packets[30].size()));
  oggopus::Frames fr;
  TEST_ASSERT_FALSE(oggopus::splitPacket(spec.packets[30].data(), static_cast<uint32_t>(spec.packets[30].size()), &fr));
  oggwriter::Built f = oggwriter::build(spec);
  {
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open());
    const Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(1, run.malformed);
    TEST_ASSERT_EQUAL_UINT32(0, run.dropped);
    TEST_ASSERT_EQUAL_UINT64(1920, run.filled);
    TEST_ASSERT_EQUAL_UINT64(f.kept, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
  // The page before it lost (10 packets a page: packets 20-29): one fill
  // of the gap and the packet, 9,600 + 1,920.
  spec.packetsPerPage = 10;
  f = oggwriter::build(spec);
  Bytes damaged = f.bytes;
  damaged[f.pageOffsets[4] + 100] ^= 0x01;
  {
    oggwriter::Reader file(damaged);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open());
    const Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(1, run.gaps);
    TEST_ASSERT_EQUAL_UINT32(1, run.malformed);
    TEST_ASSERT_EQUAL_UINT64(9600 + 1920, run.filled);
    TEST_ASSERT_EQUAL_UINT64(f.kept, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.tl.corrections());
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
  // No TOC at all: dropped, the page's granule puts the count right.
  spec.packets[30] = Bytes();
  f = oggwriter::build(spec);
  {
    oggwriter::Reader file(f.bytes);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open());
    const Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(0, run.malformed);
    TEST_ASSERT_EQUAL_UINT32(1, run.dropped);
    TEST_ASSERT_EQUAL_UINT64(0, run.filled);
    TEST_ASSERT_EQUAL_UINT64(f.kept, run.kept);
    TEST_ASSERT_TRUE(run.finished);
  }
  // A malformed packet whose TOC says more than the file counted for it
  // (the card set's fuzz file counts its malformed packets as 0 samples;
  // a damaged TOC byte does the same): the concealment runs the count
  // ahead, the page's granule puts it back, and the next packet's first
  // samples up to where it was are dropped, not played twice: no jump,
  // the length exact, one correction of -240.
  {
    oggwriter::Muxer m(0x1234);
    m.packet(oggwriter::opusHead(oggwriter::HeadSpec{}), 0);
    m.flush();
    m.packet(oggwriter::opusTags(), 0);
    m.flush();
    int64_t k = 0;
    for (uint32_t i = 0; i < 100; ++i) {
      if (i == 30) m.packet({(28 << 3) | 2, 200, 1, 2, 3}, k);  // code 2, a first frame longer than the packet: 2 x 2.5 ms by its TOC, 0 to the file
      k += 960;
      m.packet(oggwriter::celt20ms(300, i + 1), i + 1 == 100 ? k - 648 : k);
      if (i % 10 == 9) m.flush(i + 1 == 100);
    }
    oggwriter::Reader file(m.out);
    oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
    TEST_ASSERT_EQUAL(Open::Ok, r.open());
    TEST_ASSERT_TRUE(r.scanTail());
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, r.lengthSamples());
    const Run run = play(r);
    TEST_ASSERT_EQUAL_UINT32(1, run.malformed);
    TEST_ASSERT_EQUAL_UINT64(240, run.filled);
    TEST_ASSERT_EQUAL_UINT32(1, run.tl.corrections());
    TEST_ASSERT_EQUAL_INT64(-240, run.tl.slip());
    TEST_ASSERT_EQUAL_UINT64(kUsualKept, run.kept);
    TEST_ASSERT_EQUAL_UINT32(0, run.jumps);
    TEST_ASSERT_TRUE(run.finished);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc);
  RUN_TEST(test_page_header);
  RUN_TEST(test_page_reader);
  RUN_TEST(test_codec_sniff);
  RUN_TEST(test_opus_head);
  RUN_TEST(test_toc_samples);
  RUN_TEST(test_frame_split);
  RUN_TEST(test_usual_file);
  RUN_TEST(test_usual_file_packets);
  RUN_TEST(test_refusals);
  RUN_TEST(test_big_tags);
  RUN_TEST(test_lacing_and_spanning);
  RUN_TEST(test_damaged_and_lost_pages);
  RUN_TEST(test_continuation_rules);
  RUN_TEST(test_foreign_and_chained);
  RUN_TEST(test_g0_rules);
  RUN_TEST(test_end_trim);
  RUN_TEST(test_pre_skip);
  RUN_TEST(test_truncated_file);
  RUN_TEST(test_garbage_tail);
  RUN_TEST(test_oversized_packet);
  RUN_TEST(test_resync_bounded);
  RUN_TEST(test_tail_scan_bounded);
  RUN_TEST(test_huge_granules);
  RUN_TEST(test_timeline);
  RUN_TEST(test_gap_plan_edges);
  RUN_TEST(test_chained_length);
  RUN_TEST(test_long_junk_tail);
  RUN_TEST(test_start_plan);
  RUN_TEST(test_anchor);
  RUN_TEST(test_lost_page_after_plan);
  RUN_TEST(test_damaged_probe_headers);
  RUN_TEST(test_probe_floor_steps_over_damage);
  RUN_TEST(test_plan_walk_budget);
  RUN_TEST(test_tiny_pages);
  RUN_TEST(test_same_serial_chain);
  RUN_TEST(test_next_bounded);
  RUN_TEST(test_seek_past_damage);
  RUN_TEST(test_start_plan_cropped);
  RUN_TEST(test_short_frames_refused);
  RUN_TEST(test_sliced_page_reads);
  RUN_TEST(test_plan_through_slices);
  RUN_TEST(test_resync_in_steps);
  RUN_TEST(test_malformed_packets_concealed);
  RUN_TEST(test_open_with_tail_and_page_in_hand);
  return UNITY_END();
}
