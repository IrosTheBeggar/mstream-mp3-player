// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for LameTag: an MP3's Xing/Info header, LAME's extension (the
// encoder delay and padding) and the gapless trim (docs/GAPLESS.md section
// 4), plus the trimmed lengths and seek bytes in TrackProgress and
// TrackSeek. Run: pio test -e native -f test_lame_tag
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "LameTag.h"
#include "TrackProgress.h"
#include "TrackSeek.h"

namespace {

uint8_t buf[4096];

void putBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

// The first frame's header (4 bytes) for each case.
struct Kind {
  uint8_t h[4];
  int side;     // side information bytes
  int length;   // the frame's bytes
  uint32_t rate, spf;
  uint32_t crcBytes;  // with all four Xing fields
};
// MPEG-1 128 kbit/s 44.1 kHz joint stereo: 417 bytes.
const Kind kMpeg1Stereo = {{0xFF, 0xFB, 0x90, 0x44}, 32, 417, 44100, 1152, 190};
// MPEG-1 128 kbit/s 44.1 kHz mono.
const Kind kMpeg1Mono = {{0xFF, 0xFB, 0x90, 0xC4}, 17, 417, 44100, 1152, 175};
// MPEG-2 80 kbit/s 22.05 kHz stereo: 72 * 80000 / 22050 = 261 bytes.
const Kind kMpeg2Stereo = {{0xFF, 0xF3, 0x90, 0x44}, 17, 261, 22050, 576, 175};
// MPEG-2 80 kbit/s 22.05 kHz mono.
const Kind kMpeg2Mono = {{0xFF, 0xF3, 0x90, 0xC4}, 9, 261, 22050, 576, 167};
// MPEG-2.5 64 kbit/s 11.025 kHz stereo: 72 * 64000 / 11025 = 417 bytes.
const Kind kMpeg25Stereo = {{0xFF, 0xE3, 0x80, 0x44}, 17, 417, 11025, 576, 175};

// A header frame at buf[at] ("Xing" or "Info", `flags`), LAME's extension
// with `encoder`, `delay`, `padding`, its CRC right (or not), and the next
// frame's header after it. Returns where the extension starts.
size_t makeHeader(const Kind& k, size_t at, uint32_t flags, uint32_t frames, const char* encoder, uint32_t delay,
                  uint32_t padding, bool goodCrc = true, const char* tag = "Info", bool protectedFrame = false) {
  std::memcpy(buf + at, k.h, 4);
  size_t x = at + 4 + static_cast<size_t>(k.side);
  if (protectedFrame) {
    buf[at + 1] &= 0xFE;  // protection bit 0: a CRC follows the header
    x += 2;
  }
  std::memcpy(buf + x, tag, 4);
  putBe32(buf + x + 4, flags);
  size_t p = x + 8;
  if (flags & 1) {
    putBe32(buf + p, frames);
    p += 4;
  }
  if (flags & 2) {
    putBe32(buf + p, 1234567);
    p += 4;
  }
  if (flags & 4) {
    for (int i = 0; i < 100; ++i) buf[p + i] = static_cast<uint8_t>(i * 256 / 100);
    p += 100;
  }
  if (flags & 8) {
    putBe32(buf + p, 78);
    p += 4;
  }
  if (encoder) {
    std::memcpy(buf + p, encoder, std::strlen(encoder) < 9 ? std::strlen(encoder) : 9);
    buf[p + 21] = static_cast<uint8_t>(delay >> 4);
    buf[p + 22] = static_cast<uint8_t>(((delay & 0x0F) << 4) | (padding >> 8));
    buf[p + 23] = static_cast<uint8_t>(padding & 0xFF);
    const uint16_t crc = lametag::crc16(buf + at, p + 34 - at);
    buf[p + 34] = static_cast<uint8_t>((goodCrc ? crc : crc ^ 1) >> 8);
    buf[p + 35] = static_cast<uint8_t>(goodCrc ? crc : crc ^ 1);
  }
  std::memcpy(buf + at + static_cast<size_t>(k.length), k.h, 4);  // the next frame
  return p;
}

// A real file's first frame (LAME 3.99r, CBR 128 kbit/s joint stereo,
// written out by hand from a file on the development PC): its header, side
// information, "Xing" with all four fields, and LAME's extension. Delay
// 576, padding 701, 11,106 frames; CRC 0x1856 over its first 190 bytes.
const char* kRealHeader =
    "fffb9044000000000000000000000000000000000000000000000000000000000000000058696e670000000f00002b620096c0920000"
    "02040607090b0d101215171a1c1f222427292c2e313437393d3f4244484a4d505255575b5d606265686b6d707275787b7d7f8284888a"
    "8d8f9395989b9ea0a3a6a9abaeb1b3b6b9bcbfc2c4c7cacdd0d3d5d8dbdde1e3e6e9eceef1f3f6f8fbfdfeff000000644c414d45332e"
    "39397205dd000000002e31000035202402bd8d0001f40096c092a5d51856";

size_t putHex(uint8_t* p, const char* hex) {
  size_t n = 0;
  for (; hex[2 * n] && hex[2 * n + 1]; ++n) p[n] = static_cast<uint8_t>(std::stoi(std::string(hex + 2 * n, 2), nullptr, 16));
  return n;
}

}  // namespace

void setUp() { std::memset(buf, 0, sizeof(buf)); }
void tearDown() {}

void test_lame_info_header_mpeg1_stereo() {
  makeHeader(kMpeg1Stereo, 0, 0x0F, 11106, "LAME3.100", 576, 1308);
  lametag::Info i;
  TEST_ASSERT_TRUE(lametag::parse(buf, sizeof(buf), &i));
  TEST_ASSERT_TRUE(i.frame);
  TEST_ASSERT_TRUE(i.header);
  TEST_ASSERT_TRUE(i.xing);
  TEST_ASSERT_TRUE(i.info);
  TEST_ASSERT_EQUAL_UINT32(417, i.headerLength);
  TEST_ASSERT_EQUAL_UINT32(11106, i.frames);
  TEST_ASSERT_EQUAL_UINT32(44100, i.rate);
  TEST_ASSERT_EQUAL_UINT32(1152, i.spf);
  TEST_ASSERT_TRUE(i.lame);
  TEST_ASSERT_EQUAL_STRING("LAME3.100", i.encoder);
  TEST_ASSERT_EQUAL_UINT16(576, i.delay);
  TEST_ASSERT_EQUAL_UINT16(1308, i.padding);
  TEST_ASSERT_TRUE(i.crcChecked);
  TEST_ASSERT_TRUE(i.crcOk);
  TEST_ASSERT_EQUAL_UINT32(190, i.crcBytes);  // 4 + 32 + 120 + 34
}

// The CRC covers every byte of the frame before it: 190 only for MPEG-1
// stereo; 175 for MPEG-1 mono and MPEG-2/2.5 stereo; 167 for MPEG-2/2.5
// mono (with all four Xing fields).
void test_each_mode_and_version() {
  const Kind* kinds[] = {&kMpeg1Stereo, &kMpeg1Mono, &kMpeg2Stereo, &kMpeg2Mono, &kMpeg25Stereo};
  for (const Kind* k : kinds) {
    std::memset(buf, 0, sizeof(buf));
    makeHeader(*k, 7, 0x0F, 5000, "LAME3.100", 1105, 600);
    lametag::Info i;
    TEST_ASSERT_TRUE(lametag::parse(buf, sizeof(buf), &i));
    TEST_ASSERT_EQUAL_UINT32(7, i.frameAt);
    TEST_ASSERT_EQUAL_UINT32(k->rate, i.rate);
    TEST_ASSERT_EQUAL_UINT32(k->spf, i.spf);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(k->length), i.headerLength);
    TEST_ASSERT_TRUE(i.lame);
    TEST_ASSERT_EQUAL_UINT16(1105, i.delay);
    TEST_ASSERT_EQUAL_UINT16(600, i.padding);
    TEST_ASSERT_EQUAL_UINT32(k->crcBytes, i.crcBytes);
    TEST_ASSERT_TRUE(i.crcOk);
  }
}

// The extension follows the last Xing field present: every subset of the
// flags moves it. Without the frame count there is nothing to check the
// delay and padding against: not trusted.
void test_every_subset_of_the_xing_flags() {
  for (uint32_t flags = 0; flags < 16; ++flags) {
    std::memset(buf, 0, sizeof(buf));
    makeHeader(kMpeg1Stereo, 0, flags, 9000, "LAME3.100", 576, 1000, true, "Xing");
    lametag::Info i;
    TEST_ASSERT_TRUE(lametag::parse(buf, sizeof(buf), &i));
    TEST_ASSERT_TRUE(i.xing);
    TEST_ASSERT_FALSE(i.info);
    TEST_ASSERT_EQUAL_STRING("LAME3.100", i.encoder);
    TEST_ASSERT_TRUE(i.crcOk);
    TEST_ASSERT_EQUAL(flags & 1 ? 1 : 0, i.lame ? 1 : 0);
    if (i.lame) {
      TEST_ASSERT_EQUAL_UINT16(576, i.delay);
      TEST_ASSERT_EQUAL_UINT16(1000, i.padding);
    }
  }
}

// [dddddddd][ddddpppp][pppppppp]: the field's edges.
void test_delay_and_padding_bits() {
  const uint32_t pairs[][2] = {{0, 0}, {4095, 4095}, {0, 4095}, {4095, 0}, {0x800, 0x001}, {0x001, 0x800}};
  for (const auto& p : pairs) {
    std::memset(buf, 0, sizeof(buf));
    makeHeader(kMpeg1Stereo, 0, 0x0F, 100, "LAME3.100", p[0], p[1]);
    lametag::Info i;
    TEST_ASSERT_TRUE(lametag::parse(buf, sizeof(buf), &i));
    TEST_ASSERT_TRUE(i.lame);
    TEST_ASSERT_EQUAL_UINT16(p[0], i.delay);
    TEST_ASSERT_EQUAL_UINT16(p[1], i.padding);
  }
}

// "LAME", "Lavf", "Lavc" (FFmpeg's rule); anything else, or a header
// without the extension, or VBRI: a header frame, no trim.
void test_which_encoders_are_trusted() {
  const char* yes[] = {"LAME3.99r", "Lavf58.76", "Lavc60.31"};
  for (const char* e : yes) {
    std::memset(buf, 0, sizeof(buf));
    makeHeader(kMpeg1Stereo, 0, 0x0F, 900, e, 576, 1000);
    lametag::Info i;
    lametag::parse(buf, sizeof(buf), &i);
    TEST_ASSERT_TRUE(i.lame);
  }
  const char* no[] = {"GOGO1.00 ", "Xing     ", "lame3.100"};
  for (const char* e : no) {
    std::memset(buf, 0, sizeof(buf));
    makeHeader(kMpeg1Stereo, 0, 0x0F, 900, e, 576, 1000);
    lametag::Info i;
    lametag::parse(buf, sizeof(buf), &i);
    TEST_ASSERT_TRUE(i.header);
    TEST_ASSERT_FALSE(i.lame);
    TEST_ASSERT_EQUAL_UINT16(0, i.delay);
  }
  // A Xing header with no extension (all zeros after it).
  std::memset(buf, 0, sizeof(buf));
  makeHeader(kMpeg1Stereo, 0, 0x0F, 900, nullptr, 0, 0);
  lametag::Info i;
  lametag::parse(buf, sizeof(buf), &i);
  TEST_ASSERT_TRUE(i.header);
  TEST_ASSERT_FALSE(i.lame);
  // VBRI (Fraunhofer): a header frame of its own, at 4 + 32.
  std::memset(buf, 0, sizeof(buf));
  std::memcpy(buf, kMpeg1Stereo.h, 4);
  std::memcpy(buf + 36, "VBRI", 4);
  putBe32(buf + 36 + 14, 4321);
  std::memcpy(buf + 417, kMpeg1Stereo.h, 4);
  lametag::parse(buf, sizeof(buf), &i);
  TEST_ASSERT_TRUE(i.header);
  TEST_ASSERT_TRUE(i.vbri);
  TEST_ASSERT_EQUAL_UINT32(417, i.headerLength);
  TEST_ASSERT_EQUAL_UINT32(4321, i.frames);
  TEST_ASSERT_FALSE(i.lame);
  // No header at all: an audio frame first.
  std::memset(buf, 0, sizeof(buf));
  std::memcpy(buf, kMpeg1Stereo.h, 4);
  std::memcpy(buf + 417, kMpeg1Stereo.h, 4);
  TEST_ASSERT_TRUE(lametag::parse(buf, sizeof(buf), &i));
  TEST_ASSERT_TRUE(i.frame);
  TEST_ASSERT_FALSE(i.header);
  TEST_ASSERT_EQUAL_UINT32(0, i.headerLength);
  // Nothing that looks like a frame.
  std::memset(buf, 0, sizeof(buf));
  TEST_ASSERT_FALSE(lametag::parse(buf, sizeof(buf), &i));
  TEST_ASSERT_FALSE(i.frame);
}

// A wrong CRC is reported, the trim still trusted (FFmpeg doesn't check
// it; taggers rewrite the frame). A delay and padding as long as the
// track or longer: not trusted.
void test_the_crc_and_the_sanity_checks() {
  makeHeader(kMpeg1Stereo, 0, 0x0F, 900, "LAME3.100", 576, 1000, false);
  lametag::Info i;
  lametag::parse(buf, sizeof(buf), &i);
  TEST_ASSERT_TRUE(i.crcChecked);
  TEST_ASSERT_FALSE(i.crcOk);
  TEST_ASSERT_TRUE(i.lame);
  std::memset(buf, 0, sizeof(buf));
  makeHeader(kMpeg1Stereo, 0, 0x0F, 2, "LAME3.100", 2000, 304);  // 2 x 1152 = 2304 = delay + padding
  lametag::parse(buf, sizeof(buf), &i);
  TEST_ASSERT_FALSE(i.lame);
  std::memset(buf, 0, sizeof(buf));
  makeHeader(kMpeg1Stereo, 0, 0x0F, 2, "LAME3.100", 2000, 303);
  lametag::parse(buf, sizeof(buf), &i);
  TEST_ASSERT_TRUE(i.lame);
  // The extension cut off by the buffer's end: the header, not trusted.
  std::memset(buf, 0, sizeof(buf));
  const size_t ext = makeHeader(kMpeg1Stereo, 0, 0x0F, 900, "LAME3.100", 576, 1000);
  lametag::parse(buf, ext + 20, &i);
  TEST_ASSERT_TRUE(i.header);
  TEST_ASSERT_FALSE(i.crcChecked);
  TEST_ASSERT_FALSE(i.lame);
}

// A CRC-protected header frame (protection bit 0): the side information,
// and so the tag, starts 2 bytes later; accepted.
void test_a_crc_protected_header_frame() {
  makeHeader(kMpeg1Stereo, 0, 0x0F, 900, "LAME3.100", 576, 1000, true, "Info", true);
  lametag::Info i;
  lametag::parse(buf, sizeof(buf), &i);
  TEST_ASSERT_TRUE(i.lame);
  TEST_ASSERT_EQUAL_UINT16(576, i.delay);
  TEST_ASSERT_EQUAL_UINT32(192, i.crcBytes);
  TEST_ASSERT_TRUE(i.crcOk);
}

// A real file's first frame, byte for byte.
void test_a_real_lame_header() {
  const size_t n = putHex(buf + 3, kRealHeader);  // a little junk in front
  TEST_ASSERT_EQUAL_UINT32(192, n);
  std::memcpy(buf + 3 + 417, buf + 3, 4);  // the next frame's header
  lametag::Info i;
  TEST_ASSERT_TRUE(lametag::parse(buf, sizeof(buf), &i));
  TEST_ASSERT_EQUAL_UINT32(3, i.frameAt);
  TEST_ASSERT_TRUE(i.xing);
  TEST_ASSERT_EQUAL_UINT32(11106, i.frames);
  TEST_ASSERT_EQUAL_STRING("LAME3.99r", i.encoder);
  TEST_ASSERT_TRUE(i.lame);
  TEST_ASSERT_EQUAL_UINT16(576, i.delay);
  TEST_ASSERT_EQUAL_UINT16(701, i.padding);
  TEST_ASSERT_EQUAL_UINT32(190, i.crcBytes);
  TEST_ASSERT_EQUAL_HEX16(0x1856, i.crcStored);
  TEST_ASSERT_TRUE(i.crcOk);
  // 11106 x 1152 - 576 - 701 = 12,792,835 samples: 290.0869 s.
  TEST_ASSERT_EQUAL_UINT64(12792835, lametag::keptSamples(i));
  TEST_ASSERT_EQUAL_UINT32(290086, lametag::lengthMs(i));
  const lametag::Trim t = lametag::trim(i, 1, true, true);
  TEST_ASSERT_EQUAL_UINT32(1 + 576 + 529, t.skip);
  TEST_ASSERT_EQUAL_UINT32(701 - 529, t.hold);
}

// skip = lead + delay + 529, hold = max(0, padding - 529), and only the
// lead after a seek or with the tag's trim off.
void test_the_trim_rules() {
  lametag::Info i;
  i.lame = true;
  i.rate = 44100;
  i.spf = 1152;
  i.frames = 100;
  i.delay = 576;
  i.padding = 1308;
  lametag::Trim t = lametag::trim(i, 1, true, true);
  TEST_ASSERT_EQUAL_UINT32(1106, t.skip);
  TEST_ASSERT_EQUAL_UINT32(779, t.hold);
  t = lametag::trim(i, 1, false, true);  // a seek start: the lead only, the end still held
  TEST_ASSERT_EQUAL_UINT32(1, t.skip);
  TEST_ASSERT_EQUAL_UINT32(779, t.hold);
  t = lametag::trim(i, 1, true, false);  // Gt0
  TEST_ASSERT_EQUAL_UINT32(1, t.skip);
  TEST_ASSERT_EQUAL_UINT32(0, t.hold);
  i.padding = 500;  // under 529: nothing to cut at the end
  t = lametag::trim(i, 0, true, true);
  TEST_ASSERT_EQUAL_UINT32(1105, t.skip);
  TEST_ASSERT_EQUAL_UINT32(0, t.hold);
  i.padding = 529;
  TEST_ASSERT_EQUAL_UINT32(0, lametag::trim(i, 0, true, true).hold);
  i.lame = false;  // not trusted: the lead only
  t = lametag::trim(i, 1, true, true);
  TEST_ASSERT_EQUAL_UINT32(1, t.skip);
  TEST_ASSERT_EQUAL_UINT32(0, t.hold);
  // kept, and the length: 100 x 1152 - 576 - 1308 = 113,316 (2.569 s).
  i.lame = true;
  i.padding = 1308;
  TEST_ASSERT_EQUAL_UINT64(113316, lametag::keptSamples(i));
  TEST_ASSERT_EQUAL_UINT32(2569, lametag::lengthMs(i));
  // A seek's time in the decoded stream: + (576 + 529) / 44.1 = 25 ms.
  TEST_ASSERT_EQUAL_UINT32(60025, lametag::untrimmedMs(i, 60000));
  i.lame = false;
  TEST_ASSERT_EQUAL_UINT64(115200, lametag::keptSamples(i));
  TEST_ASSERT_EQUAL_UINT32(60000, lametag::untrimmedMs(i, 60000));
}

// TrackProgress and TrackSeek on the trimmed timeline: the header's length
// is kept / rate, and a CBR Info file's byte for a time counts from the
// first audio frame, the delay and 529 later.
void test_trimmed_lengths_and_seek_bytes() {
  // 11,025 frames at 44.1 kHz, CBR 128 kbit/s (16 bytes a ms), Info + LAME.
  makeHeader(kMpeg1Stereo, 0, 0x0F, 11025, "LAME3.100", 576, 1308);
  // 288 s untrimmed; kept 12,700,800 - 1,884 = 12,698,916: 287.957 s.
  TEST_ASSERT_EQUAL_UINT32(287957, progress::mp3HeaderDurationMs(buf, sizeof(buf)));
  const uint32_t tag = 1000, size = tag + 417 + 11025 * 417;
  TEST_ASSERT_EQUAL_UINT32(287957, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  uint32_t byte = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::CbrInfo),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 60000, &byte)));
  // 60 s + 25 ms (1,105 samples), 16 bytes a ms, after the Info frame.
  TEST_ASSERT_EQUAL_UINT32(tag + 417 + 60025 * 16, byte);
  // Without the extension: as before (from the Info frame, untrimmed).
  std::memset(buf, 0, sizeof(buf));
  makeHeader(kMpeg1Stereo, 0, 0x0F, 11025, nullptr, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(288000, progress::mp3HeaderDurationMs(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT32(288000, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 60000, &byte);
  TEST_ASSERT_EQUAL_UINT32(tag + 60000 * 16, byte);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lame_info_header_mpeg1_stereo);
  RUN_TEST(test_each_mode_and_version);
  RUN_TEST(test_every_subset_of_the_xing_flags);
  RUN_TEST(test_delay_and_padding_bits);
  RUN_TEST(test_which_encoders_are_trusted);
  RUN_TEST(test_the_crc_and_the_sanity_checks);
  RUN_TEST(test_a_crc_protected_header_frame);
  RUN_TEST(test_a_real_lame_header);
  RUN_TEST(test_the_trim_rules);
  RUN_TEST(test_trimmed_lengths_and_seek_bytes);
  return UNITY_END();
}
