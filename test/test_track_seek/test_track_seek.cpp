// Host tests for TrackSeek: where a start part of the way into a track
// lands (the resume point), an MP3's byte for a time (Xing and VBRI tables
// of contents, average and frame bitrates), the frame-sync check, and a
// FLAC's STREAMINFO. Run: pio test -e native
#include <unity.h>

#include <cstring>

#include "TrackSeek.h"

namespace {

// A Layer III frame header: MPEG-1, 128 kbit/s, 44.1 kHz, joint stereo, no
// padding: 417 bytes a frame (144 * 128000 / 44100).
void putFrameHeader(uint8_t* p) {
  p[0] = 0xFF;
  p[1] = 0xFB;
  p[2] = 0x90;
  p[3] = 0x44;
}
constexpr int kFrameLen = 417;

void putBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void putBe16(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

// 11,025 frames of 1152 samples at 44.1 kHz: 288 s exactly.
constexpr uint32_t kFrames = 11025;
constexpr uint32_t kLengthMs = 288000;

uint8_t buf[4096];

}  // namespace

void setUp() { std::memset(buf, 0, sizeof(buf)); }
void tearDown() {}

// The last 5 s, the end and past it start at 0:00; an unknown length
// starts where asked.
void test_start_ms_edges() {
  using trackseek::startMs;
  TEST_ASSERT_EQUAL_UINT32(83000, startMs(83000, 240000));
  TEST_ASSERT_EQUAL_UINT32(234999, startMs(234999, 240000));
  TEST_ASSERT_EQUAL_UINT32(0, startMs(235000, 240000));  // 5 s left
  TEST_ASSERT_EQUAL_UINT32(0, startMs(238000, 240000));
  TEST_ASSERT_EQUAL_UINT32(0, startMs(240000, 240000));
  TEST_ASSERT_EQUAL_UINT32(0, startMs(300000, 240000));  // a file that got shorter
  TEST_ASSERT_EQUAL_UINT32(0, startMs(2000, 4000));      // a track under 5 s
  TEST_ASSERT_EQUAL_UINT32(83000, startMs(83000, 0));    // not known
  TEST_ASSERT_EQUAL_UINT32(0, startMs(0, 240000));
}

// A plain CBR file (no header): its length and bytes from the first
// frame's bitrate, 16 bytes a millisecond at 128 kbit/s.
void test_mp3_constant_bitrate() {
  const uint32_t tag = 1000;                 // an ID3v2 tag in front
  const uint32_t size = tag + 3 + 3200000;   // junk, then 200 s of audio
  putFrameHeader(buf + 3);
  putFrameHeader(buf + 3 + kFrameLen);
  TEST_ASSERT_EQUAL_UINT32(200000, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  uint32_t byte = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::FrameBitrate),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 83000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(tag + 3 + 83000 * 16, byte);
  // Never past the file.
  trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 999000, &byte);
  TEST_ASSERT_EQUAL_UINT32(size - 1, byte);
  // Nothing that looks like a frame: nothing to go on.
  std::memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(0, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::None),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 83000, &byte)));
}

// LAME's Xing header with its 100-point table of contents: point i is
// where i% of the time starts, in 256ths of the stream's bytes; between
// points a straight line. Without the table: the average bitrate.
void test_mp3_xing_toc() {
  const uint32_t tag = 500, size = tag + 2560000;
  putFrameHeader(buf);
  const int x = 4 + 32;  // after the side information
  std::memcpy(buf + x, "Xing", 4);
  putBe32(buf + x + 4, 0x07);  // frames, bytes, TOC
  putBe32(buf + x + 8, kFrames);
  putBe32(buf + x + 12, 2560000);
  uint8_t* toc = buf + x + 16;
  for (int i = 0; i < 100; ++i) toc[i] = static_cast<uint8_t>(i * 256 / 100);
  toc[25] = 100;  // a quiet start: the first quarter of the time in fewer bytes
  toc[26] = 110;
  putFrameHeader(buf + kFrameLen);
  TEST_ASSERT_EQUAL_UINT32(kLengthMs, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  uint32_t byte = 0;
  // 25% of 288 s: point 25, 100/256 of the bytes.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::XingToc),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 72000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(tag + 1000000, byte);
  // 25.5%: halfway to point 26 (105/256).
  trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 73440, &byte);
  TEST_ASSERT_EQUAL_UINT32(tag + 1050000, byte);
  // The last point runs to 256/256.
  trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 287000, &byte);
  TEST_ASSERT_TRUE(byte > tag + 2540000 && byte < size);

  // The same file without the TOC flag: its average bitrate.
  putBe32(buf + x + 4, 0x03);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::AverageBitrate),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 72000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(tag + 640000, byte);  // a quarter of the bytes
  // Without the bytes either: the file's, from the first frame.
  putBe32(buf + x + 4, 0x01);
  trackseek::mp3SeekByte(buf, sizeof(buf), tag, size + 400000, 0, 72000, &byte);
  TEST_ASSERT_EQUAL_UINT32(tag + 740000, byte);
  // A TOC cut off by the end of the buffer isn't read.
  putBe32(buf + x + 4, 0x07);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::AverageBitrate),
                        static_cast<int>(trackseek::mp3SeekByte(buf, x + 60, tag, size, 0, 72000, &byte)));
}

// LAME's "Info" header marks a CBR file: placed by its bitrate (16 bytes a
// ms at 128 kbit/s from the Info frame, which decodes to a frame of silence
// counted in the time), not its TOC (whose 256ths of the bytes were up to
// ~0.3 s off on the device). Frames that differ in bitrate: the TOC after
// all.
void test_mp3_info_cbr() {
  const uint32_t tag = 500, size = tag + 4608000;
  putFrameHeader(buf);
  const int x = 4 + 32;
  std::memcpy(buf + x, "Info", 4);
  putBe32(buf + x + 4, 0x07);
  putBe32(buf + x + 8, kFrames);
  putBe32(buf + x + 12, 4608000);
  uint8_t* toc = buf + x + 16;
  for (int i = 0; i < 100; ++i) toc[i] = static_cast<uint8_t>(i * 256 / 100);
  putFrameHeader(buf + kFrameLen);
  uint32_t byte = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::CbrInfo),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 72000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(tag + 72000 * 16, byte);
  TEST_ASSERT_EQUAL_STRING("CBR, Info header", trackseek::mp3SeekName(trackseek::Mp3Seek::CbrInfo));
  // The next frame at 160 kbit/s: not CBR after all, the TOC (point 25).
  buf[kFrameLen + 2] = 0xA0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::XingToc),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 72000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(tag + 64 * 4608000 / 256, byte);
}

// Fraunhofer's VBRI header: entry i is the bytes of frames i * perEntry to
// (i + 1) * perEntry, times its scale.
void test_mp3_vbri_toc() {
  const uint32_t tag = 0, size = 3000000;
  putFrameHeader(buf);
  const int v = 4 + 32;
  std::memcpy(buf + v, "VBRI", 4);
  putBe32(buf + v + 10, 2000000);  // bytes
  putBe32(buf + v + 14, kFrames);
  putBe16(buf + v + 18, 4);        // entries
  putBe16(buf + v + 20, 10);       // scale
  putBe16(buf + v + 22, 2);        // entry size
  putBe16(buf + v + 24, 1000);     // frames per entry
  const uint32_t entries[] = {40000, 30000, 50000, 60000};
  for (int i = 0; i < 4; ++i) putBe16(buf + v + 26 + 2 * i, entries[i]);
  putFrameHeader(buf + kFrameLen);
  TEST_ASSERT_EQUAL_UINT32(kLengthMs, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  uint32_t byte = 0;
  // Frame 1500 (39.184 s): all of entry 0 and half of entry 1.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::VbriToc),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 39184, &byte)));
  TEST_ASSERT_EQUAL_UINT32(400000 + 150000, byte);
  // Past the table's frames: its end.
  trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 150000, &byte);
  TEST_ASSERT_EQUAL_UINT32(1800000, byte);
}

// A file without a header whose frames differ in bitrate (VBR, its Xing
// frame stripped): the first frame's bitrate would put a start far off, so
// a length known elsewhere (the resume point's) places it by the average
// bitrate; without one it can't be placed (the backend starts at 0:00). A
// CBR file with a hint that agrees keeps the exact first-frame bitrate.
void test_mp3_without_a_header() {
  // A 32 kbit/s frame (104 bytes), then 128 kbit/s ones: VBR.
  const uint32_t tag = 0, size = 5700000;  // ~4:00 at 190 kbit/s
  buf[0] = 0xFF;
  buf[1] = 0xFB;
  buf[2] = 0x10;  // 32 kbit/s, 44.1 kHz
  buf[3] = 0x44;
  putFrameHeader(buf + 104);
  putFrameHeader(buf + 104 + kFrameLen);
  uint32_t byte = 0;
  TEST_ASSERT_EQUAL_UINT32(0, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::Unplaced),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 0, 180000, &byte)));
  // With the length it had when it paused (4:00): 3:00 is 3/4 of the bytes.
  TEST_ASSERT_EQUAL_UINT32(240000, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 240000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::AverageBitrate),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 240000, 180000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(size / 4 * 3, byte);

  // Every frame in the buffer at 32 kbit/s (a silent start) looks CBR: a
  // hint far from the length that gives wins.
  std::memset(buf, 0, sizeof(buf));
  for (int i = 0; i + 104 <= static_cast<int>(sizeof(buf)); i += 104) {
    buf[i] = 0xFF;
    buf[i + 1] = 0xFB;
    buf[i + 2] = 0x10;
    buf[i + 3] = 0x44;
  }
  TEST_ASSERT_EQUAL_UINT32(1425000, trackseek::mp3LengthMs(buf, sizeof(buf), tag, size, 0));  // 23:45
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::AverageBitrate),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), tag, size, 240000, 180000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(size / 4 * 3, byte);

  // A CBR file (128 kbit/s, 200 s) whose hint is the read-rate estimate, 1%
  // off: the first frame's bitrate, to the frame.
  std::memset(buf, 0, sizeof(buf));
  putFrameHeader(buf);
  putFrameHeader(buf + kFrameLen);
  putFrameHeader(buf + 2 * kFrameLen);
  TEST_ASSERT_EQUAL_UINT32(200000, trackseek::mp3LengthMs(buf, sizeof(buf), 0, 3200000, 202000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Mp3Seek::FrameBitrate),
                        static_cast<int>(trackseek::mp3SeekByte(buf, sizeof(buf), 0, 3200000, 202000, 83000, &byte)));
  TEST_ASSERT_EQUAL_UINT32(83000 * 16, byte);
  // A hint for a file with a Xing header isn't read.
  const int x = 4 + 32;
  std::memcpy(buf + x, "Xing", 4);
  putBe32(buf + x + 4, 0x03);
  putBe32(buf + x + 8, kFrames);
  putBe32(buf + x + 12, 2560000);
  TEST_ASSERT_EQUAL_UINT32(kLengthMs, trackseek::mp3LengthMs(buf, sizeof(buf), 0, 3200000, 100000));
}

// The audio after a found frame, at its bitrate: too little (a file
// shorter than its header says) is a failed seek in the backend.
void test_mp3_ms_left() {
  putFrameHeader(buf);
  TEST_ASSERT_EQUAL_UINT32(5000, trackseek::mp3MsLeft(buf, 80000));  // 16 bytes a ms
  TEST_ASSERT_EQUAL_UINT32(0, trackseek::mp3MsLeft(buf, 0));
  TEST_ASSERT_EQUAL_UINT32(0, trackseek::mp3MsLeft(buf + 1, 80000));  // not a header
}

// A clean frame to start on: a header whose next frame's header follows
// (the same version and rate); a stray 0xFF in the audio doesn't count.
void test_mp3_frame_sync() {
  buf[100] = 0xFF;  // a lone sync-looking pair in the data
  buf[101] = 0xFB;
  buf[102] = 0x90;
  buf[103] = 0x44;  // a whole header even, but nothing after its frame
  putFrameHeader(buf + 700);
  putFrameHeader(buf + 700 + kFrameLen);
  TEST_ASSERT_EQUAL_INT(700, trackseek::mp3FrameAt(buf, sizeof(buf)));
  // The next header must be inside the buffer.
  TEST_ASSERT_EQUAL_INT(-1, trackseek::mp3FrameAt(buf, 700 + kFrameLen + 2));
  // One at another sample rate isn't the next frame.
  buf[700 + kFrameLen + 2] = 0x94;  // 48 kHz
  TEST_ASSERT_EQUAL_INT(-1, trackseek::mp3FrameAt(buf, sizeof(buf)));
  std::memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(-1, trackseek::mp3FrameAt(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(-1, trackseek::mp3FrameAt(buf, 2));
}

void test_flac_stream_info() {
  uint8_t b[42] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
  // 44,100 Hz, 2 channels, 16 bits, 10,000,000 samples.
  b[18] = 0x0A;
  b[19] = 0xC4;
  b[20] = 0x42;
  b[21] = 0xF0;
  putBe32(b + 22, 10000000);
  uint32_t rate = 0;
  uint64_t total = 0;
  TEST_ASSERT_TRUE(trackseek::flacStreamInfo(b, sizeof(b), &rate, &total));
  TEST_ASSERT_EQUAL_UINT32(44100, rate);
  TEST_ASSERT_EQUAL_UINT64(10000000, total);
  putBe32(b + 22, 0);  // the total not said: still a FLAC
  TEST_ASSERT_TRUE(trackseek::flacStreamInfo(b, sizeof(b), &rate, &total));
  TEST_ASSERT_EQUAL_UINT64(0, total);
  b[0] = 'I';
  TEST_ASSERT_FALSE(trackseek::flacStreamInfo(b, sizeof(b), &rate, &total));
  b[0] = 'f';
  TEST_ASSERT_FALSE(trackseek::flacStreamInfo(b, 20, &rate, &total));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_ms_edges);
  RUN_TEST(test_mp3_constant_bitrate);
  RUN_TEST(test_mp3_xing_toc);
  RUN_TEST(test_mp3_info_cbr);
  RUN_TEST(test_mp3_vbri_toc);
  RUN_TEST(test_mp3_without_a_header);
  RUN_TEST(test_mp3_ms_left);
  RUN_TEST(test_mp3_frame_sync);
  RUN_TEST(test_flac_stream_info);
  return UNITY_END();
}
