// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the cover thumbnails' portable pieces: the JPEG header
// probe (JpegInfo), the box-filter scaler (ThumbScaler), the PSRAM LRU and
// its requests (ThumbCache) and the card's thumbnail files (thumbfile).
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "JpegInfo.h"
#include "ThumbCache.h"
#include "ThumbScaler.h"

void setUp() {}
void tearDown() {}

namespace {

// A counting allocator: every block goes back.
size_t live = 0;
std::vector<std::pair<void*, size_t>> blocks;
void* countAlloc(size_t n) {
  void* p = std::malloc(n ? n : 1);
  blocks.push_back({p, n});
  live += n;
  return p;
}
void countFree(void* p) {
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].first == p) {
      live -= blocks[i].second;
      blocks.erase(blocks.begin() + static_cast<long>(i));
      std::free(p);
      return;
    }
  }
  TEST_FAIL_MESSAGE("freed a block that wasn't allocated here");
}

// A JPEG's first bytes: SOI, an APP0 segment, then a frame header.
std::vector<uint8_t> jpegHead(uint8_t sof, uint16_t w, uint16_t h, bool exifFirst = false) {
  std::vector<uint8_t> v = {0xFF, 0xD8};
  if (exifFirst) {  // an APP1 of 300 bytes before anything else
    v.push_back(0xFF);
    v.push_back(0xE1);
    v.push_back(0x01);
    v.push_back(0x2C);
    for (int i = 0; i < 298; ++i) v.push_back(static_cast<uint8_t>(i == 5 ? 0xFF : 0x11));
  }
  const uint8_t app0[] = {0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
  v.insert(v.end(), app0, app0 + sizeof(app0));
  const uint8_t dqt[] = {0xFF, 0xDB, 0x00, 0x04, 0x00, 0x01};
  v.insert(v.end(), dqt, dqt + sizeof(dqt));
  const uint8_t frame[] = {0xFF, sof, 0x00, 0x11, 8, static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h),
                           static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w), 3};
  v.insert(v.end(), frame, frame + sizeof(frame));
  for (int i = 0; i < 9; ++i) v.push_back(0);
  return v;
}

uint16_t be565(uint8_t r, uint8_t g, uint8_t b) { return ThumbScaler::rgb565be(r, g, b); }

// An RGB888 picture, filled by a function of (x, y).
template <typename Fn>
std::vector<uint8_t> picture(int w, int h, Fn colour) {
  std::vector<uint8_t> p(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t c[3];
      colour(x, y, c);
      std::memcpy(&p[(static_cast<size_t>(y) * w + x) * 3], c, 3);
    }
  }
  return p;
}

}  // namespace

// ---- JpegInfo ----

void test_jpeg_baseline_and_progressive() {
  std::vector<uint8_t> b = jpegHead(0xC0, 650, 565);
  jpeg::Info i = jpeg::parse(b.data(), b.size());
  TEST_ASSERT_TRUE(i.ok);
  TEST_ASSERT_FALSE(i.progressive);
  TEST_ASSERT_EQUAL_UINT16(650, i.width);
  TEST_ASSERT_EQUAL_UINT16(565, i.height);
  TEST_ASSERT_EQUAL_UINT8(3, i.components);
  // SOF2: progressive, which TJpgDec can't decode.
  b = jpegHead(0xC2, 500, 500);
  i = jpeg::parse(b.data(), b.size());
  TEST_ASSERT_TRUE(i.ok);
  TEST_ASSERT_TRUE(i.progressive);
  // An EXIF block first (with a 0xFF byte in it) is skipped by its length.
  b = jpegHead(0xC1, 1200, 1100, true);
  i = jpeg::parse(b.data(), b.size());
  TEST_ASSERT_TRUE(i.ok);
  TEST_ASSERT_FALSE(i.progressive);
  TEST_ASSERT_EQUAL_UINT16(1200, i.width);
}

// Tagged covers: EXIF with a thumbnail, XMP, a Photoshop block and an ICC
// profile can put the frame header past 64 KB. The whole file is walked
// (ui/Thumbs parses all of it): such a cover is decodable, not "not a
// JPEG" (which would mark it undecodable on the card for good).
void test_jpeg_frame_header_past_64_kb() {
  std::vector<uint8_t> b = {0xFF, 0xD8};
  for (int seg = 0; seg < 3; ++seg) {  // three APPn segments of ~30 KB: 90 KB
    const uint16_t len = 30000;
    b.push_back(0xFF);
    b.push_back(static_cast<uint8_t>(0xE1 + seg));
    b.push_back(static_cast<uint8_t>(len >> 8));
    b.push_back(static_cast<uint8_t>(len));
    for (int i = 0; i < len - 2; ++i) b.push_back(static_cast<uint8_t>(i % 7 == 0 ? 0xFF : 0x20));
  }
  const std::vector<uint8_t> head = jpegHead(0xC0, 1400, 1400);
  b.insert(b.end(), head.begin() + 2, head.end());  // its markers after the SOI
  TEST_ASSERT_TRUE(b.size() > 64 * 1024 + 100);
  const jpeg::Info i = jpeg::parse(b.data(), b.size());
  TEST_ASSERT_TRUE(i.ok);
  TEST_ASSERT_FALSE(i.progressive);
  TEST_ASSERT_EQUAL_UINT16(1400, i.width);
  // Cut at 64 KB (as before), it wasn't found.
  TEST_ASSERT_FALSE(jpeg::parse(b.data(), 64 * 1024).ok);
}

void test_jpeg_not_a_jpeg() {
  const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  TEST_ASSERT_FALSE(jpeg::parse(png, sizeof(png)).ok);
  TEST_ASSERT_FALSE(jpeg::parse(nullptr, 10).ok);
  // Scan data before any frame header.
  const uint8_t sos[] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 1, 2, 3, 4, 5, 6};
  TEST_ASSERT_FALSE(jpeg::parse(sos, sizeof(sos)).ok);
  // Cut before the frame header's size: not ok (the caller reads more, or gives up).
  std::vector<uint8_t> b = jpegHead(0xC0, 100, 100);
  TEST_ASSERT_FALSE(jpeg::parse(b.data(), 30).ok);
  // DHT (C4) looks like a SOF number but isn't one.
  const uint8_t dht[] = {0xFF, 0xD8, 0xFF, 0xC4, 0x00, 0x04, 0, 0, 0xFF, 0xC0, 0x00, 0x11, 8, 0, 20, 0, 30, 3, 0, 0};
  const jpeg::Info i = jpeg::parse(dht, sizeof(dht));
  TEST_ASSERT_TRUE(i.ok);
  TEST_ASSERT_EQUAL_UINT16(30, i.width);
  TEST_ASSERT_EQUAL_UINT16(20, i.height);
}

// parseFile(): the same walk over a file read at offsets (a cover streamed
// from the card, docs/METADATA.md 3.5): the same answers as parse() on every
// case above, in a few small reads, the segments skipped unread.
void test_jpeg_parsed_from_a_file() {
  struct File {
    std::vector<uint8_t> b;
    uint32_t reads = 0, bytes = 0;
    bool fail = false;
    static bool read(uint32_t offset, uint8_t* out, uint32_t n, void* ctx) {
      File& f = *static_cast<File*>(ctx);
      if (f.fail || offset > f.b.size() || n > f.b.size() - offset) return false;
      std::memcpy(out, f.b.data() + offset, n);
      ++f.reads;
      f.bytes += n;
      return true;
    }
  };
  auto same = [](const std::vector<uint8_t>& b) {
    File f{b};
    const jpeg::Info a = jpeg::parse(b.data(), b.size());
    const jpeg::Info c = jpeg::parseFile(File::read, &f, static_cast<uint32_t>(b.size()));
    TEST_ASSERT_EQUAL(a.ok, c.ok);
    TEST_ASSERT_EQUAL(a.progressive, c.progressive);
    TEST_ASSERT_EQUAL_UINT16(a.width, c.width);
    TEST_ASSERT_EQUAL_UINT16(a.height, c.height);
    TEST_ASSERT_EQUAL_UINT8(a.components, c.components);
    return f;
  };
  same(jpegHead(0xC0, 650, 565));
  same(jpegHead(0xC2, 500, 500));
  same(jpegHead(0xC1, 1200, 1100, true));
  std::vector<uint8_t> big = {0xFF, 0xD8};
  for (int seg = 0; seg < 3; ++seg) {
    const uint16_t len = 30000;
    big.push_back(0xFF);
    big.push_back(static_cast<uint8_t>(0xE1 + seg));
    big.push_back(static_cast<uint8_t>(len >> 8));
    big.push_back(static_cast<uint8_t>(len));
    for (int i = 0; i < len - 2; ++i) big.push_back(static_cast<uint8_t>(i % 7 == 0 ? 0xFF : 0x20));
  }
  const std::vector<uint8_t> head = jpegHead(0xC0, 1400, 1400);
  big.insert(big.end(), head.begin() + 2, head.end());
  const File f = same(big);
  TEST_ASSERT_TRUE(f.reads <= 8);   // the three 30 KB segments skipped, not read
  TEST_ASSERT_TRUE(f.bytes <= 8 * 64);
  same(std::vector<uint8_t>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
  same(std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 1, 2, 3, 4, 5, 6});
  std::vector<uint8_t> cut = jpegHead(0xC0, 100, 100);
  cut.resize(30);
  same(cut);
  same(std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xC4, 0x00, 0x04, 0, 0, 0xFF, 0xC0, 0x00, 0x11, 8, 0, 20, 0, 30, 3, 0, 0});
  same(std::vector<uint8_t>());
  // A read that fails: not a JPEG it can say anything about.
  File bad{jpegHead(0xC0, 650, 565)};
  bad.fail = true;
  TEST_ASSERT_FALSE(jpeg::parseFile(File::read, &bad, static_cast<uint32_t>(bad.b.size())).ok);
  TEST_ASSERT_FALSE(jpeg::parseFile(nullptr, nullptr, 100).ok);
}

// ---- ThumbScaler ----

void test_scaler_decoder_scale() {
  // The most the decoder may shrink while the shorter side stays >= 96.
  TEST_ASSERT_EQUAL_INT(2, ThumbScaler::decoderScale(650, 565, 96));  // 141 px
  TEST_ASSERT_EQUAL_INT(3, ThumbScaler::decoderScale(3000, 3000, 96));
  TEST_ASSERT_EQUAL_INT(0, ThumbScaler::decoderScale(80, 80, 96));    // too small already: enlarged
  TEST_ASSERT_EQUAL_INT(1, ThumbScaler::decoderScale(300, 200, 96));  // 100 px
  TEST_ASSERT_EQUAL_INT(0, ThumbScaler::decoderScale(191, 500, 96));
}

void test_scaler_box_filter_quadrants() {
  ThumbScaler s(countAlloc, countFree);
  const int sizes[2] = {2, 4};
  TEST_ASSERT_TRUE(s.begin(8, 8, sizes, 2));
  // Four quadrants: red, green, blue, white.
  auto quad = [](int x, int y, uint8_t* c) {
    const int q = (y >= 4) * 2 + (x >= 4);
    const uint8_t cols[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    std::memcpy(c, cols[q], 3);
  };
  const std::vector<uint8_t> px = picture(8, 8, quad);
  // Given as two blocks (the decoder's MCUs), in any order.
  std::vector<uint8_t> bottom(px.begin() + 8 * 4 * 3, px.end()), top(px.begin(), px.begin() + 8 * 4 * 3);
  s.add(0, 4, 8, 4, bottom.data());
  s.add(0, 0, 8, 4, top.data());
  uint16_t two[4], four[16];
  s.finish(0, two);
  s.finish(1, four);
  TEST_ASSERT_EQUAL_HEX16(be565(255, 0, 0), two[0]);
  TEST_ASSERT_EQUAL_HEX16(be565(0, 255, 0), two[1]);
  TEST_ASSERT_EQUAL_HEX16(be565(0, 0, 255), two[2]);
  TEST_ASSERT_EQUAL_HEX16(be565(255, 255, 255), two[3]);
  TEST_ASSERT_EQUAL_HEX16(be565(0, 255, 0), four[3]);   // top right
  TEST_ASSERT_EQUAL_HEX16(be565(0, 0, 255), four[12]);  // bottom left
  TEST_ASSERT_TRUE(live > 0);
  s.end();
  TEST_ASSERT_EQUAL_size_t(0, live);
}

void test_scaler_means_and_crops() {
  ThumbScaler s(countAlloc, countFree);
  const int one[1] = {1};
  // A checkerboard of black and white averages to grey.
  TEST_ASSERT_TRUE(s.begin(4, 4, one, 1));
  const std::vector<uint8_t> board = picture(4, 4, [](int x, int y, uint8_t* c) {
    const uint8_t v = (x + y) % 2 ? 255 : 0;
    c[0] = c[1] = c[2] = v;
  });
  s.add(0, 0, 4, 4, board.data());
  uint16_t grey;
  s.finish(0, &grey);
  TEST_ASSERT_EQUAL_HEX16(be565(128, 128, 128), grey);
  // Wider than tall: the centre square only (the red margins go).
  const int sizes[1] = {4};
  TEST_ASSERT_TRUE(s.begin(8, 4, sizes, 1));
  const std::vector<uint8_t> wide = picture(8, 4, [](int x, int, uint8_t* c) {
    const bool margin = x < 2 || x >= 6;
    c[0] = margin ? 255 : 0;
    c[1] = 0;
    c[2] = margin ? 0 : 255;
  });
  s.add(0, 0, 8, 4, wide.data());
  uint16_t out[16];
  s.finish(0, out);
  for (int i = 0; i < 16; ++i) TEST_ASSERT_EQUAL_HEX16(be565(0, 0, 255), out[i]);
}

void test_scaler_enlarges_small_pictures() {
  ThumbScaler s(countAlloc, countFree);
  const int sizes[1] = {5};
  TEST_ASSERT_TRUE(s.begin(2, 2, sizes, 1));
  const std::vector<uint8_t> px = picture(2, 2, [](int x, int y, uint8_t* c) {
    c[0] = x ? 255 : 0;
    c[1] = y ? 255 : 0;
    c[2] = 0;
  });
  s.add(0, 0, 2, 2, px.data());
  uint16_t out[25];
  s.finish(0, out);
  // Every output pixel has a source (none left black by a gap).
  TEST_ASSERT_EQUAL_HEX16(be565(0, 0, 0), out[0]);
  TEST_ASSERT_EQUAL_HEX16(be565(255, 0, 0), out[4]);
  TEST_ASSERT_EQUAL_HEX16(be565(0, 255, 0), out[20]);
  TEST_ASSERT_EQUAL_HEX16(be565(255, 255, 0), out[24]);
}

void test_scaler_refuses_bad_sizes_and_no_memory() {
  ThumbScaler s(countAlloc, countFree);
  const int bad[1] = {0};
  TEST_ASSERT_FALSE(s.begin(10, 10, bad, 1));
  const int ok[1] = {4};
  TEST_ASSERT_FALSE(s.begin(0, 10, ok, 1));
  TEST_ASSERT_EQUAL_size_t(0, live);
  ThumbScaler none([](size_t) -> void* { return nullptr; }, countFree);
  TEST_ASSERT_FALSE(none.begin(10, 10, ok, 1));
}

// ---- ThumbCache ----

void test_cache_stores_and_evicts_least_recently_used() {
  ThumbCache c(countAlloc, countFree);
  TEST_ASSERT_TRUE(c.begin(2, 1));
  std::vector<uint16_t> a(40 * 40, 0x1111), b(40 * 40, 0x2222), d(40 * 40, 0x3333);
  TEST_ASSERT_NULL(c.get(7, ThumbCache::Size::Small));
  TEST_ASSERT_TRUE(c.put(7, ThumbCache::Size::Small, a.data()));
  TEST_ASSERT_TRUE(c.put(8, ThumbCache::Size::Small, b.data()));
  TEST_ASSERT_EQUAL_HEX16(0x1111, c.get(7, ThumbCache::Size::Small)[0]);  // 7 is now the most recent
  TEST_ASSERT_TRUE(c.put(9, ThumbCache::Size::Small, d.data()));          // 8 goes
  TEST_ASSERT_TRUE(c.has(7, ThumbCache::Size::Small));
  TEST_ASSERT_FALSE(c.has(8, ThumbCache::Size::Small));
  TEST_ASSERT_EQUAL_HEX16(0x3333, c.get(9, ThumbCache::Size::Small)[1599]);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().evicted);
  // Each size is its own pool.
  TEST_ASSERT_FALSE(c.has(7, ThumbCache::Size::Large));
  std::vector<uint16_t> big(96 * 96, 0x4444);
  TEST_ASSERT_TRUE(c.put(7, ThumbCache::Size::Large, big.data()));
  TEST_ASSERT_TRUE(c.has(7, ThumbCache::Size::Small));
  TEST_ASSERT_EQUAL_HEX16(0x4444, c.get(7, ThumbCache::Size::Large)[96 * 96 - 1]);
  // A library rebuild: all of it goes, the pools stay.
  c.clear();
  TEST_ASSERT_FALSE(c.has(7, ThumbCache::Size::Small));
  TEST_ASSERT_EQUAL_UINT32(2, c.slots(ThumbCache::Size::Small));
}

void test_cache_requests_newest_first() {
  ThumbCache c(countAlloc, countFree);
  TEST_ASSERT_TRUE(c.begin(4, 1));
  c.want(1, ThumbCache::Size::Small);
  c.want(2, ThumbCache::Size::Small);
  c.want(3, ThumbCache::Size::Small);
  c.want(1, ThumbCache::Size::Large);  // asked again (Now Playing): the newest, with both sizes
  uint32_t id;
  uint8_t sizes;
  TEST_ASSERT_TRUE(c.next(&id, &sizes));
  TEST_ASSERT_EQUAL_UINT32(1, id);
  TEST_ASSERT_EQUAL_UINT8(ThumbCache::sizeBit(ThumbCache::Size::Small) | ThumbCache::sizeBit(ThumbCache::Size::Large),
                          sizes);
  // One at a time: nothing else until it's done.
  TEST_ASSERT_FALSE(c.next(&id, &sizes));
  c.want(1, ThumbCache::Size::Small);  // being made: not asked again
  TEST_ASSERT_EQUAL_UINT32(2, c.wanted());
  c.markFailed(1);
  c.done(1);
  c.want(1, ThumbCache::Size::Small);  // failed: not asked again
  TEST_ASSERT_TRUE(c.failed(1));
  TEST_ASSERT_TRUE(c.next(&id, &sizes));
  TEST_ASSERT_EQUAL_UINT32(3, id);
  std::vector<uint16_t> px(40 * 40, 1);
  c.put(3, ThumbCache::Size::Small, px.data());
  c.done(3);
  c.want(3, ThumbCache::Size::Small);  // here already
  TEST_ASSERT_TRUE(c.next(&id, &sizes));
  TEST_ASSERT_EQUAL_UINT32(2, id);
  c.done(2);
  TEST_ASSERT_FALSE(c.next(&id, &sizes));
}

void test_cache_keeps_only_the_last_requests() {
  ThumbCache c(countAlloc, countFree);
  TEST_ASSERT_TRUE(c.begin(4, 1));
  // A fling past 100 albums: only the last kWanted are kept, newest first.
  for (uint32_t i = 0; i < 100; ++i) c.want(i, ThumbCache::Size::Small);
  TEST_ASSERT_EQUAL_UINT32(ThumbCache::kWanted, c.wanted());
  uint32_t id;
  uint8_t sizes;
  TEST_ASSERT_TRUE(c.next(&id, &sizes));
  TEST_ASSERT_EQUAL_UINT32(99, id);
  c.done(id);
  uint32_t last = id;
  while (c.next(&id, &sizes)) {
    TEST_ASSERT_TRUE(id < last);
    last = id;
    c.done(id);
  }
  TEST_ASSERT_EQUAL_UINT32(100 - ThumbCache::kWanted, last);
  // No memory: nothing held, nothing works.
  ThumbCache none([](size_t) -> void* { return nullptr; }, countFree);
  TEST_ASSERT_FALSE(none.begin(4, 1));
  none.want(1, ThumbCache::Size::Small);
  TEST_ASSERT_FALSE(none.next(&id, &sizes));
}

void test_cache_memory_goes_back() {
  live = 0;
  {
    ThumbCache c(countAlloc, countFree);
    TEST_ASSERT_TRUE(c.begin(64, 6));
    TEST_ASSERT_EQUAL_size_t(c.bytes(), live);
    TEST_ASSERT_TRUE(c.bytes() > 64 * 3200 + 6 * 18432);
  }
  TEST_ASSERT_EQUAL_size_t(0, live);
}

// ---- thumbfile ----

void test_thumbfile_header_round_trip() {
  thumbfile::Header h;
  h.pathHash = 0x0123456789ABCDEFull;
  h.sourceBytes = 47861;
  h.flags = thumbfile::kNoPicture;
  uint8_t b[thumbfile::kHeaderBytes];
  thumbfile::write(h, b);
  TEST_ASSERT_EQUAL_UINT8('M', b[0]);
  TEST_ASSERT_EQUAL_UINT8('P', b[1]);
  TEST_ASSERT_EQUAL_UINT8('T', b[2]);
  TEST_ASSERT_EQUAL_UINT8('H', b[3]);
  thumbfile::Header back;
  TEST_ASSERT_TRUE(thumbfile::read(b, sizeof(b), &back));
  TEST_ASSERT_TRUE(back.pathHash == h.pathHash);
  TEST_ASSERT_EQUAL_UINT32(47861, back.sourceBytes);
  TEST_ASSERT_EQUAL_UINT8(thumbfile::kNoPicture, back.flags);
  // Another version, or cut short: not ours.
  b[4] = 9;
  TEST_ASSERT_FALSE(thumbfile::read(b, sizeof(b), &back));
  thumbfile::write(h, b);
  TEST_ASSERT_FALSE(thumbfile::read(b, 10, &back));
  // The layout: 24 + 3,200 + 18,432 bytes.
  TEST_ASSERT_EQUAL_size_t(24, thumbfile::offsetOf(ThumbCache::Size::Small));
  TEST_ASSERT_EQUAL_size_t(3224, thumbfile::offsetOf(ThumbCache::Size::Large));
  TEST_ASSERT_EQUAL_size_t(21656, thumbfile::kFileBytes);
}

void test_thumbfile_names_are_short() {
  // FNV-1a 64 as published ("" and "a").
  TEST_ASSERT_TRUE(thumbfile::pathHash("") == 0xcbf29ce484222325ull);
  TEST_ASSERT_TRUE(thumbfile::pathHash("a") == 0xaf63dc4c8601ec8cull);
  const uint64_t h = 0x7A0C31F2DEADBEEFull;
  char buf[64];
  TEST_ASSERT_TRUE(thumbfile::path("/sd/.player/thumbs", h, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("/sd/.player/thumbs/7/7A0C31F2.565", buf);
  TEST_ASSERT_TRUE(thumbfile::path("/sd/.player/thumbs", h, buf, sizeof(buf), true));
  TEST_ASSERT_EQUAL_STRING("/sd/.player/thumbs/7", buf);
  TEST_ASSERT_FALSE(thumbfile::path("/sd/.player/thumbs", h, buf, 20));
  // Different covers, different names (mostly: a clash of the 32 bits
  // reads as a miss, the header has all 64).
  char other[64];
  thumbfile::path("/t", thumbfile::pathHash("/music/Air/Moon Safari/cover.jpg"), buf, sizeof(buf));
  thumbfile::path("/t", thumbfile::pathHash("/music/Air/Moon Safari/folder.jpg"), other, sizeof(other));
  TEST_ASSERT_TRUE(std::strcmp(buf, other) != 0);
  // 8.3: eight hex digits, ".565".
  const char* name = std::strrchr(buf, '/') + 1;
  TEST_ASSERT_EQUAL_size_t(12, std::strlen(name));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_jpeg_baseline_and_progressive);
  RUN_TEST(test_jpeg_frame_header_past_64_kb);
  RUN_TEST(test_jpeg_not_a_jpeg);
  RUN_TEST(test_jpeg_parsed_from_a_file);
  RUN_TEST(test_scaler_decoder_scale);
  RUN_TEST(test_scaler_box_filter_quadrants);
  RUN_TEST(test_scaler_means_and_crops);
  RUN_TEST(test_scaler_enlarges_small_pictures);
  RUN_TEST(test_scaler_refuses_bad_sizes_and_no_memory);
  RUN_TEST(test_cache_stores_and_evicts_least_recently_used);
  RUN_TEST(test_cache_requests_newest_first);
  RUN_TEST(test_cache_keeps_only_the_last_requests);
  RUN_TEST(test_cache_memory_goes_back);
  RUN_TEST(test_thumbfile_header_round_trip);
  RUN_TEST(test_thumbfile_names_are_short);
  return UNITY_END();
}
