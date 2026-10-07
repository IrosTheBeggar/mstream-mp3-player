// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for OpusOpenCache (docs/OPUS.md section 10): the keying by
// path hash and size, replacement and invalidation, the LRU's eviction,
// the blob's round trip (and a save whose blob didn't reach the card:
// dirty again) and what a corrupt, short, outdated or empty blob does,
// and a record through the reader: a file opened from its record with one
// read, the same plan and the same samples as from a full open, the check
// refusing another file of the same size, and no record from an open
// whose length isn't known. Run: pio test -e native -f test_opus_open_cache
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../support/OggWriter.h"
#include "ByteStream.h"
#include "OggOpus.h"
#include "OpusOpenCache.h"

void setUp() {}
void tearDown() {}

namespace {

using Bytes = std::vector<uint8_t>;

uint8_t pageBuf[ogg::kMaxPageBytes];
uint8_t packetBuf[oggopus::kMaxPacketBytes];

// A counting allocator: every block goes back.
size_t live = 0;
void* countAlloc(size_t n) {
  live += n;
  return std::malloc(n ? n : 1);
}
void countFree(void* p) { std::free(p); }

oggopus::OpenRecord record(uint32_t size, int64_t last = 48000 * 10) {
  oggopus::OpenRecord r;
  r.head.version = 1;
  r.head.channels = 2;
  r.head.preSkip = 312;
  r.head.inputRate = 48000;
  r.fileSize = size;
  r.serial = 0x1234 + size;
  r.headCrc = 0xABCD0000u + size;
  r.firstAudio = 100;
  r.firstGranuleAt = 100;
  r.firstGranuleEnd = 16000;
  r.firstGranuleSeq = 2;
  r.g0 = 0;
  r.firstGranule = 48000;
  r.lastGranule = last;
  r.lastPageAt = size > 20000 ? size - 10000 : 100;
  r.linkEnd = size;
  r.tagsBytes = 60;
  r.tagsPages = 1;
  return r;
}

bool same(const oggopus::OpenRecord& a, const oggopus::OpenRecord& b) {
  return a.fileSize == b.fileSize && a.serial == b.serial && a.headCrc == b.headCrc && a.firstAudio == b.firstAudio &&
         a.firstGranuleAt == b.firstGranuleAt && a.firstGranuleEnd == b.firstGranuleEnd &&
         a.firstGranuleSeq == b.firstGranuleSeq && a.firstGranuleContinues == b.firstGranuleContinues &&
         a.chained == b.chained && a.g0 == b.g0 && a.firstGranule == b.firstGranule && a.lastGranule == b.lastGranule &&
         a.lastPageAt == b.lastPageAt && a.linkEnd == b.linkEnd && a.tagsBytes == b.tagsBytes &&
         a.tagsPages == b.tagsPages && a.head.version == b.head.version && a.head.channels == b.head.channels &&
         a.head.preSkip == b.head.preSkip && a.head.inputRate == b.head.inputRate && a.head.gain == b.head.gain &&
         a.head.family == b.head.family && a.head.streams == b.head.streams && a.head.coupled == b.head.coupled &&
         a.head.map[0] == b.head.map[0] && a.head.map[1] == b.head.map[1];
}

// The reader driven to its first kept samples, counting what it keeps.
uint64_t playKept(oggopus::Reader& r, const oggopus::StartPlan* plan, uint64_t stopAfter) {
  oggopus::Timeline tl;
  if (plan) {
    tl.start(*plan);
  } else {
    tl.start(r.g0(), r.head().preSkip);
  }
  oggopus::Reader::Packet p;
  oggopus::Frames fr;
  for (;;) {
    const oggopus::Reader::Next n = r.next(&p);
    if (n == oggopus::Reader::Next::End) break;
    if (n == oggopus::Reader::Next::Pending) continue;
    tl.packet(p);
    if (p.samples < 0 || !oggopus::splitPacket(p.data, p.bytes, &fr)) {
      tl.packetDone();
      continue;
    }
    if (!tl.wanted(static_cast<uint32_t>(p.samples))) {
      tl.skipped(static_cast<uint32_t>(p.samples));
      tl.packetDone();
      continue;
    }
    for (uint32_t i = 0; i < fr.count; ++i) tl.decoded(oggopus::frameSamples(fr.toc));
    tl.packetDone();
    if (tl.finished() || (stopAfter && tl.kept() >= stopAfter)) break;
  }
  return tl.kept();
}

}  // namespace

// The key is the path's hash and the file's size: another size under the
// same path is a miss, and a put at the new size takes the old entry out
// (one path never holds two); the same key put again replaces, copied out
// whole on a hit; an empty cache misses everything.
void test_keys_put_find_replace() {
  OpusOpenCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  TEST_ASSERT_EQUAL_UINT32(8, c.capacity());
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
  TEST_ASSERT_FALSE(c.dirty());
  const uint64_t a = OpusOpenCache::hashPath("/music/a/01.opus");
  const uint64_t b = OpusOpenCache::hashPath("/music/a/02.opus");
  TEST_ASSERT_TRUE(a != b);
  TEST_ASSERT_EQUAL_UINT64(a, OpusOpenCache::hashPath("/music/a/01.opus"));
  oggopus::OpenRecord out;
  TEST_ASSERT_FALSE(c.find(a, 1000, &out));
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().misses);
  c.put(a, record(1000));
  TEST_ASSERT_TRUE(c.dirty());
  TEST_ASSERT_EQUAL_UINT32(1, c.size());
  TEST_ASSERT_TRUE(c.find(a, 1000, &out));
  TEST_ASSERT_TRUE(same(record(1000), out));
  TEST_ASSERT_FALSE(c.find(a, 1001, &out));  // the size is the key's
  TEST_ASSERT_FALSE(c.find(b, 1000, &out));  // the path is
  // The same key again: replaced, not added.
  oggopus::OpenRecord changed = record(1000);
  changed.lastGranule = 48000 * 11;
  c.put(a, changed);
  TEST_ASSERT_EQUAL_UINT32(1, c.size());
  TEST_ASSERT_TRUE(c.find(a, 1000, &out));
  TEST_ASSERT_EQUAL_INT64(48000 * 11, out.lastGranule);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().replaced);
  // The file changed size: the old entry goes, the new one stands alone.
  c.put(a, record(2000));
  TEST_ASSERT_EQUAL_UINT32(1, c.size());
  TEST_ASSERT_FALSE(c.find(a, 1000, &out));
  TEST_ASSERT_TRUE(c.find(a, 2000, &out));
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().evicted);
  // Forgotten (the check refused it), cleared.
  c.put(b, record(3000));
  c.forget(a, 2000);
  TEST_ASSERT_EQUAL_UINT32(1, c.size());
  TEST_ASSERT_FALSE(c.find(a, 2000, &out));
  TEST_ASSERT_TRUE(c.find(b, 3000, &out));
  c.forget(a, 2000);  // (not there: nothing)
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().forgotten);
  c.clear();
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
  TEST_ASSERT_FALSE(c.find(b, 3000, &out));
  // Without a block: every find a miss, every put dropped.
  OpusOpenCache none;
  TEST_ASSERT_FALSE(none.begin(0));
  none.put(a, record(1000));
  TEST_ASSERT_FALSE(none.find(a, 1000, &out));
  TEST_ASSERT_EQUAL_UINT32(0, none.size());
}

// The LRU: a full cache takes the slot least recently found or put; a hit
// makes an entry the most recently used.
void test_lru_eviction() {
  OpusOpenCache c;
  TEST_ASSERT_TRUE(c.begin(4, countAlloc, countFree));
  TEST_ASSERT_EQUAL(4 * sizeof(oggopus::OpenRecord) <= live, true);
  uint64_t keys[6];
  for (uint32_t i = 0; i < 6; ++i) {
    char path[32];
    std::snprintf(path, sizeof(path), "/music/%u.opus", i);
    keys[i] = OpusOpenCache::hashPath(path);
  }
  for (uint32_t i = 0; i < 4; ++i) c.put(keys[i], record(1000 + i));
  TEST_ASSERT_EQUAL_UINT32(4, c.size());
  oggopus::OpenRecord out;
  TEST_ASSERT_TRUE(c.find(keys[0], 1000, &out));  // 0 is the newest now; 1 the oldest
  c.put(keys[4], record(1004));                     // takes 1's slot
  TEST_ASSERT_EQUAL_UINT32(4, c.size());
  TEST_ASSERT_FALSE(c.find(keys[1], 1001, &out));
  TEST_ASSERT_TRUE(c.find(keys[0], 1000, &out));
  TEST_ASSERT_TRUE(c.find(keys[2], 1002, &out));
  TEST_ASSERT_TRUE(c.find(keys[3], 1003, &out));
  TEST_ASSERT_TRUE(c.find(keys[4], 1004, &out));
  // Now 0 is the oldest (found before 2, 3 and 4).
  c.put(keys[5], record(1005));
  TEST_ASSERT_FALSE(c.find(keys[0], 1000, &out));
  TEST_ASSERT_TRUE(c.find(keys[5], 1005, &out));
  TEST_ASSERT_EQUAL_UINT32(2, c.stats().evicted);
  TEST_ASSERT_EQUAL_UINT32(6, c.stats().stored);
}

// The blob: a round trip keeps every record and the recency (the oldest
// is evicted first after a load, as before it); save() cleans, a put or
// markDirty() dirties; the blob's size is what blobBytes() says.
void test_save_load_round_trip() {
  OpusOpenCache c;
  TEST_ASSERT_TRUE(c.begin(4));
  uint64_t keys[5];
  for (uint32_t i = 0; i < 5; ++i) {
    char path[32];
    std::snprintf(path, sizeof(path), "/music/%u.opus", i);
    keys[i] = OpusOpenCache::hashPath(path);
  }
  oggopus::OpenRecord r0 = record(5000, 48000 * 200);
  r0.head.channels = 1;
  r0.head.family = 1;
  r0.head.streams = 1;
  r0.head.coupled = 0;
  r0.head.map[0] = 0;
  r0.head.gain = -256;
  r0.firstGranuleContinues = true;
  r0.chained = true;
  r0.linkEnd = 4000;
  c.put(keys[0], r0);
  c.put(keys[1], record(1001));
  c.put(keys[2], record(1002));
  oggopus::OpenRecord out;
  TEST_ASSERT_TRUE(c.find(keys[1], 1001, &out));  // the order by recency: 0, 2, 1
  MemorySink sink;
  TEST_ASSERT_TRUE(c.save(sink));
  TEST_ASSERT_FALSE(c.dirty());
  TEST_ASSERT_EQUAL_UINT32(OpusOpenCache::blobBytes(3), sink.size());
  // The blob didn't reach the card (the backend's write failed): dirty
  // again, so a save follows; the next save cleans.
  c.markDirty();
  TEST_ASSERT_TRUE(c.dirty());
  MemorySink again;
  TEST_ASSERT_TRUE(c.save(again));
  TEST_ASSERT_FALSE(c.dirty());
  TEST_ASSERT_EQUAL_UINT32(sink.size(), again.size());
  TEST_ASSERT_EQUAL_MEMORY(sink.data(), again.data(), sink.size());
  OpusOpenCache d;
  TEST_ASSERT_TRUE(d.begin(3));  // (full once loaded: the puts below evict)
  MemorySource src(sink.data(), sink.size(), 7);  // (read in small pieces)
  TEST_ASSERT_EQUAL(OpusOpenCache::Load::Loaded, d.load(src));
  TEST_ASSERT_EQUAL_UINT32(3, d.size());
  TEST_ASSERT_FALSE(d.dirty());
  TEST_ASSERT_TRUE(d.find(keys[0], 5000, &out));
  TEST_ASSERT_TRUE(same(r0, out));
  TEST_ASSERT_TRUE(d.find(keys[1], 1001, &out));
  TEST_ASSERT_TRUE(same(record(1001), out));
  TEST_ASSERT_TRUE(d.find(keys[2], 1002, &out));
  TEST_ASSERT_TRUE(d.dirty() == false);
  // The recency survived: of the loaded order (0, 2, 1), after finds of
  // 0, 1 and 2 the order is 0, 1, 2: two puts evict 0 then 1.
  d.put(keys[3], record(1003));
  d.put(keys[4], record(1004));
  TEST_ASSERT_TRUE(d.dirty());
  TEST_ASSERT_FALSE(d.find(keys[0], 5000, &out));
  TEST_ASSERT_FALSE(d.find(keys[1], 1001, &out));
  TEST_ASSERT_TRUE(d.find(keys[2], 1002, &out));
  // A fresh load's recency with no finds in between: 0 oldest.
  OpusOpenCache e;
  TEST_ASSERT_TRUE(e.begin(3));
  MemorySource src2(sink.data(), sink.size());
  TEST_ASSERT_EQUAL(OpusOpenCache::Load::Loaded, e.load(src2));
  e.put(keys[3], record(1003));
  TEST_ASSERT_FALSE(e.find(keys[0], 5000, &out));
  TEST_ASSERT_TRUE(e.find(keys[2], 1002, &out));
  TEST_ASSERT_TRUE(e.find(keys[1], 1001, &out));
  // An empty cache saves and loads as empty.
  OpusOpenCache f;
  TEST_ASSERT_TRUE(f.begin(2));
  MemorySink empty;
  TEST_ASSERT_TRUE(f.save(empty));
  TEST_ASSERT_EQUAL_UINT32(OpusOpenCache::blobBytes(0), empty.size());
  MemorySource src3(empty.data(), empty.size());
  TEST_ASSERT_EQUAL(OpusOpenCache::Load::Loaded, f.load(src3));
  TEST_ASSERT_EQUAL_UINT32(0, f.size());
}

// A blob that doesn't read back leaves the cache empty, never half
// loaded: a flipped byte (the sum), a cut-off file, a wrong magic, a
// count over the capacity, an entry no open makes; another version's or
// entry size's is Outdated; no file is Empty; a cache without a block
// NoMemory.
void test_corrupt_blob() {
  OpusOpenCache c;
  TEST_ASSERT_TRUE(c.begin(4));
  const uint64_t a = OpusOpenCache::hashPath("/music/a.opus");
  const uint64_t b = OpusOpenCache::hashPath("/music/b.opus");
  c.put(a, record(1000));
  c.put(b, record(2000));
  MemorySink sink;
  TEST_ASSERT_TRUE(c.save(sink));
  const Bytes good(sink.data(), sink.data() + sink.size());
  auto load = [&](const Bytes& blob, uint32_t capacity = 4) {
    OpusOpenCache d;
    TEST_ASSERT_TRUE(d.begin(capacity));
    d.put(a, record(1000));  // (something there before: a failed load clears it)
    MemorySource src(blob.data(), blob.size());
    const OpusOpenCache::Load r = d.load(src);
    if (r != OpusOpenCache::Load::Loaded) TEST_ASSERT_EQUAL_UINT32(0, d.size());
    return r;
  };
  TEST_ASSERT_EQUAL(OpusOpenCache::Load::Loaded, load(good));
  for (const size_t at : {size_t(0), size_t(12), size_t(50), good.size() - 1}) {
    Bytes bad = good;
    bad[at] ^= 0x40;
    TEST_ASSERT_EQUAL(at == 0 ? OpusOpenCache::Load::Corrupt : OpusOpenCache::Load::Corrupt, load(bad));
  }
  for (const size_t cut : {size_t(0), size_t(3), size_t(12), size_t(40), good.size() - 1}) {
    Bytes bad(good.begin(), good.begin() + static_cast<long>(cut));
    TEST_ASSERT_EQUAL(cut == 0 ? OpusOpenCache::Load::Empty : OpusOpenCache::Load::Corrupt, load(bad));
  }
  {
    Bytes bad = good;
    bad[4] = 2;  // the version
    TEST_ASSERT_EQUAL(OpusOpenCache::Load::Outdated, load(bad));
    bad = good;
    bad[6] = 100;  // the entry size
    TEST_ASSERT_EQUAL(OpusOpenCache::Load::Outdated, load(bad));
    // More entries than the cache holds: refused before any is read.
    TEST_ASSERT_EQUAL(OpusOpenCache::Load::Corrupt, load(good, 1));
    // An entry with 0 channels (the sum fixed up): no open makes one.
    bad = good;
    bad[OpusOpenCache::kHeaderBytes + 38] = 0;
    uint32_t sum = ogg::crc32(bad.data(), bad.size() - 4);
    for (int i = 0; i < 4; ++i) bad[bad.size() - 4 + static_cast<size_t>(i)] = static_cast<uint8_t>(sum >> (8 * i));
    TEST_ASSERT_EQUAL(OpusOpenCache::Load::Corrupt, load(bad));
  }
  OpusOpenCache none;
  MemorySource src(good.data(), good.size());
  TEST_ASSERT_EQUAL(OpusOpenCache::Load::NoMemory, none.load(src));
}

// Through the reader: a file opened and scanned gives a record; another
// reader opened from it reads one page header and knows everything the
// first did (the head, g0, the exact length, the last page), plans the
// same start and keeps the same samples; the first next() reads the first
// audio page. The check refuses another file of the same size (its serial
// and head differ) with that one read, and a record that can't be this
// file's (its size, an offset past the end) with none.
void test_record_through_reader() {
  oggwriter::FileSpec spec;
  for (uint32_t i = 0; i < 1500; ++i) {
    spec.packets.push_back(oggwriter::celt20ms(static_cast<uint16_t>(60 + (i * 7919u) % 481u), i + 1));
  }
  spec.endTrim = 648;
  oggwriter::Built f = oggwriter::build(spec);
  oggwriter::Reader file(f.bytes);
  oggopus::Reader r(file, file.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r.open(true));
  TEST_ASSERT_EQUAL_UINT64(f.kept, r.lengthSamples());
  oggopus::OpenRecord rec;
  TEST_ASSERT_TRUE(r.record(&rec));
  TEST_ASSERT_EQUAL_UINT32(file.size(), rec.fileSize);
  TEST_ASSERT_EQUAL_UINT32(r.serial(), rec.serial);
  TEST_ASSERT_EQUAL_UINT32(f.firstAudioPage, rec.firstAudio);
  TEST_ASSERT_EQUAL_INT64(f.lastGranule, rec.lastGranule);
  TEST_ASSERT_EQUAL_UINT32(f.pageOffsets.back(), rec.lastPageAt);
  TEST_ASSERT_EQUAL_UINT32(2, rec.head.channels);
  TEST_ASSERT_EQUAL_UINT32(312, rec.head.preSkip);
  // A plan and a start from the full open, for the comparison.
  oggopus::StartPlan plan;
  r.planStart(500000, oggopus::kSeekPrerollSamples, &plan);
  r.startAt(plan);
  const uint64_t kept = playKept(r, &plan, 4096);
  TEST_ASSERT_TRUE(kept >= 4096);
  // The same file from the record: one read (the BOS page's header).
  oggwriter::Reader file2(f.bytes);
  oggopus::Reader r2(file2, file2.size(), pageBuf, packetBuf);
  TEST_ASSERT_TRUE(r2.openFrom(rec));
  TEST_ASSERT_EQUAL_UINT32(1, file2.reads);
  TEST_ASSERT_TRUE(file2.bytes <= ogg::kMaxHeaderBytes + ogg::PageReader::kPeekBytes);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r2.opened());
  TEST_ASSERT_EQUAL_UINT64(f.kept, r2.lengthSamples());
  TEST_ASSERT_EQUAL_INT64(r.g0(), r2.g0());
  TEST_ASSERT_EQUAL_UINT32(r.firstAudioPage(), r2.firstAudioPage());
  TEST_ASSERT_EQUAL_INT64(r.firstGranule(), r2.firstGranule());
  TEST_ASSERT_EQUAL_UINT32(r.lastPageAt(), r2.lastPageAt());
  TEST_ASSERT_EQUAL_UINT32(r.tagsBytes(), r2.tagsBytes());
  TEST_ASSERT_EQUAL_UINT32(r.head().preSkip, r2.head().preSkip);
  oggopus::OpenRecord rec2;
  TEST_ASSERT_TRUE(r2.record(&rec2));
  TEST_ASSERT_TRUE(same(rec, rec2));
  oggopus::StartPlan plan2;
  r2.planStart(500000, oggopus::kSeekPrerollSamples, &plan2);
  TEST_ASSERT_EQUAL_UINT32(plan.pageOffset, plan2.pageOffset);
  TEST_ASSERT_EQUAL_INT64(plan.k, plan2.k);
  TEST_ASSERT_EQUAL_INT64(plan.decodeFrom, plan2.decodeFrom);
  TEST_ASSERT_EQUAL_UINT32(plan.probes, plan2.probes);
  TEST_ASSERT_EQUAL_UINT32(plan.reads, plan2.reads);
  r2.startAt(plan2);
  TEST_ASSERT_EQUAL_UINT64(kept, playKept(r2, &plan2, 4096));
  // From the top after openFrom(): the first audio page read by the first
  // next() (two reads: it wasn't in hand), then the whole track.
  r2.restart();
  file2.reads = 0;
  TEST_ASSERT_EQUAL_UINT64(f.kept, playKept(r2, nullptr, 0));
  TEST_ASSERT_TRUE(file2.reads >= 2);
  // Another file of the same size (another serial): the check refuses it
  // with the one read, and a full open then reads it afresh.
  oggwriter::FileSpec other = spec;
  other.serial = 0x5678;
  oggwriter::Built g = oggwriter::build(other);
  TEST_ASSERT_EQUAL_UINT32(f.bytes.size(), g.bytes.size());
  oggwriter::Reader file3(g.bytes);
  oggopus::Reader r3(file3, file3.size(), pageBuf, packetBuf);
  TEST_ASSERT_FALSE(r3.openFrom(rec));
  TEST_ASSERT_EQUAL_UINT32(1, file3.reads);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r3.open(true));
  TEST_ASSERT_EQUAL_UINT64(g.kept, r3.lengthSamples());
  // The same serial but a changed head (the pre-skip): the BOS page's CRC
  // differs, refused.
  other.serial = spec.serial;
  other.head.preSkip = 400;
  oggwriter::Built h = oggwriter::build(other);
  oggwriter::Reader file4(h.bytes);
  oggopus::Reader r4(file4, file4.size(), pageBuf, packetBuf);
  TEST_ASSERT_FALSE(r4.openFrom(rec));
  TEST_ASSERT_EQUAL_UINT32(1, file4.reads);
  // A record that can't be this file's: no read.
  oggwriter::Reader file5(f.bytes);
  oggopus::Reader r5(file5, file5.size(), pageBuf, packetBuf);
  oggopus::OpenRecord bad = rec;
  bad.fileSize = rec.fileSize + 1;
  TEST_ASSERT_FALSE(r5.openFrom(bad));
  bad = rec;
  bad.firstAudio = rec.fileSize;
  TEST_ASSERT_FALSE(r5.openFrom(bad));
  bad = rec;
  bad.lastGranule = -1;
  TEST_ASSERT_FALSE(r5.openFrom(bad));
  bad = rec;
  bad.lastPageAt = rec.fileSize + 5;
  TEST_ASSERT_FALSE(r5.openFrom(bad));
  bad = rec;
  bad.head.channels = 3;
  TEST_ASSERT_FALSE(r5.openFrom(bad));
  bad = rec;
  bad.lastGranule = rec.g0 + rec.head.preSkip;  // an empty stream: no open makes such a record
  TEST_ASSERT_FALSE(r5.openFrom(bad));
  TEST_ASSERT_EQUAL_UINT32(0, file5.reads);
  // An open whose length isn't known gives no record: before the tail
  // scan, and after one that found no last page (the file's last two
  // pages damaged: neither checks, and nothing else of ours lies at the
  // file's end for the walk to take; a junk tail alone is resolved, by
  // the bisection: test_ogg_opus's test_long_junk_tail).
  oggwriter::Reader file6(f.bytes);
  oggopus::Reader r6(file6, file6.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r6.open());
  oggopus::OpenRecord none;
  TEST_ASSERT_FALSE(r6.record(&none));
  TEST_ASSERT_TRUE(r6.scanTail());
  TEST_ASSERT_TRUE(r6.record(&none));
  TEST_ASSERT_TRUE(same(rec, none));
  Bytes torn = f.bytes;
  const size_t pages = f.pageOffsets.size();
  torn[f.pageOffsets[pages - 1] + 300] ^= 0x01;
  torn[f.pageOffsets[pages - 2] + 300] ^= 0x01;
  oggwriter::Reader file8(torn);
  oggopus::Reader r8(file8, file8.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r8.open());
  TEST_ASSERT_FALSE(r8.scanTail());
  TEST_ASSERT_EQUAL_UINT64(0, r8.lengthSamples());
  TEST_ASSERT_FALSE(r8.record(&none));
  oggwriter::Reader file9(torn);
  oggopus::Reader r9(file9, file9.size(), pageBuf, packetBuf);
  TEST_ASSERT_EQUAL(oggopus::Reader::Open::Ok, r9.open(true));
  TEST_ASSERT_FALSE(r9.record(&none));
  // Through the cache: the record put under the path, found by path and
  // size, and the file opened from it.
  OpusOpenCache c;
  TEST_ASSERT_TRUE(c.begin(4));
  const uint64_t key = OpusOpenCache::hashPath("/music/x.opus");
  c.put(key, rec);
  oggopus::OpenRecord found;
  TEST_ASSERT_TRUE(c.find(key, file.size(), &found));
  oggwriter::Reader file7(f.bytes);
  oggopus::Reader r7(file7, file7.size(), pageBuf, packetBuf);
  TEST_ASSERT_TRUE(r7.openFrom(found));
  TEST_ASSERT_EQUAL_UINT64(f.kept, r7.lengthSamples());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_keys_put_find_replace);
  RUN_TEST(test_lru_eviction);
  RUN_TEST(test_save_load_round_trip);
  RUN_TEST(test_corrupt_blob);
  RUN_TEST(test_record_through_reader);
  return UNITY_END();
}
