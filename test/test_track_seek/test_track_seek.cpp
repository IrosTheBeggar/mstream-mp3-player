// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for TrackSeek: where a start part of the way into a track
// lands (the resume point, a seek), an MP3's byte for a time (Xing and VBRI
// tables of contents, average and frame bitrates), the frame-sync check, a
// FLAC's STREAMINFO, and the start plans of docs/SEEK.md (CBR arithmetic,
// LAME's TOC inverted, the chain walk, the tail rule by the exact length,
// an anchor's check) on synthetic streams (test/support/Mp3Synth.h).
// Run: pio test -e native -f test_track_seek
#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "../support/Mp3Synth.h"
#include "TrackProgress.h"
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

// ---- start plans (docs/SEEK.md) ----

namespace {

uint8_t scratch[trackseek::kScratchBytes];

trackseek::PlanIn planIn(const mp3synth::Stream& s, uint32_t ms, uint32_t hintMs = 0) {
  trackseek::PlanIn in;
  in.probe = s.bytes.data() + s.audioStart;
  in.probeBytes = s.size() - s.audioStart < 4096 ? s.size() - s.audioStart : 4096;
  in.audioStart = s.audioStart;
  in.fileSize = s.size();
  in.hintMs = hintMs;
  in.targetMs = ms;
  in.lengthMs = trackseek::mp3LengthMs(in.probe, in.probeBytes, s.audioStart, s.size(), hintMs);
  in.useTag = true;
  return in;
}

uint32_t frameIndex(const mp3synth::Stream& s, uint32_t byte) {
  for (uint32_t k = 0; k < s.frames.size(); ++k) {
    if (s.frames[k].byte == byte) return k;
  }
  return 0xFFFFFFFFu;
}

// The preroll rule (trackseek::kPrerollBytes): history + 1 frames back, 1 KB
// before the history frames; or the first audio frame.
void assertPrerollRule(const mp3synth::Stream& s, const trackseek::Plan& p) {
  if (p.prerollByte == s.firstAudio) return;
  const uint32_t land = frameIndex(s, p.landByte), pre = frameIndex(s, p.prerollByte);
  TEST_ASSERT_NOT_EQUAL(0xFFFFFFFFu, land);
  TEST_ASSERT_NOT_EQUAL(0xFFFFFFFFu, pre);
  const uint32_t h = trackseek::historyFrames(s.spf);
  TEST_ASSERT_TRUE(land - pre >= h + 1);
  TEST_ASSERT_TRUE(s.frames[land - h].byte - s.frames[pre].byte >= trackseek::kPrerollBytes);
}

}  // namespace

// LAME's bag (VbrTag.c's AddVbrFrame()) after N frames, by its loop: the
// closed form gives the same want and pos for N = 1..200,000.
void test_lame_bag_closed_form() {
  uint32_t want = 1, seen = 0, pos = 0;
  for (uint32_t n = 1; n <= 200000; ++n) {
    ++seen;
    if (seen >= want) {
      if (pos < 400) {
        ++pos;
        seen = 0;
      }
      if (pos == 400) {
        want *= 2;
        pos /= 2;
      }
    }
    const trackseek::LameBag b = trackseek::lameBag(n);
    if (b.want != want || b.pos != pos) {
      TEST_FAIL_MESSAGE("the closed form differs from LAME's loop");
    }
  }
  // docs/SEEK.md 6.3's two: One More Time, Nightvision.
  TEST_ASSERT_EQUAL_UINT32(32, trackseek::lameBag(12284).want);
  TEST_ASSERT_EQUAL_UINT32(383, trackseek::lameBag(12284).pos);
  TEST_ASSERT_EQUAL_UINT32(16, trackseek::lameBag(4001).want);
  TEST_ASSERT_EQUAL_UINT32(250, trackseek::lameBag(4001).pos);
}

// LAME's TOC inverted, on synthetic VBR streams (quiet, loud and mixed
// stretches) whose TOC LAME's bag wrote: at every 1% point the estimate is
// within 1/256 of the stream of the true frame, and over every second of
// the stream it is closer than the straight lines (point i at i% of the
// time) on every stream.
void test_lame_toc_inverted_beats_straight_lines() {
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    mp3synth::Options o;
    o.kbps = mp3synth::vbrRates(3, 2000 + 1700 * seed, seed);
    o.header = mp3synth::Header::Xing;
    o.id3 = 500;
    o.seed = seed;
    const mp3synth::Stream s = mp3synth::make(o);
    const uint32_t audio = s.frames.back().byte + s.frames.back().length - s.firstAudio;
    const uint32_t n = static_cast<uint32_t>(s.frames.size());
    // At the 1% points of the decoded stream.
    for (uint32_t i = 1; i < 100; ++i) {
      const uint64_t x = static_cast<uint64_t>(n) * s.spf * i / 100;
      uint32_t byte = 0;
      TEST_ASSERT_TRUE(trackseek::lameTocByte(s.toc, n, audio, s.spf, x, &byte));
      const int64_t truth = static_cast<int64_t>(s.frames[s.frameOf(x)].byte - s.firstAudio);
      TEST_ASSERT_TRUE_MESSAGE(std::llabs(truth - static_cast<int64_t>(byte)) <= audio / 256 + 1441,
                               "further than 1/256 of the stream");
    }
    // Every second: the mean error against today's straight lines.
    const trackseek::PlanIn in = planIn(s, 1000);
    double model = 0, lines = 0;
    int count = 0;
    for (uint32_t ms = 1000; ms + 6000 < s.lengthMs(); ms += 1000) {
      const uint64_t x = static_cast<uint64_t>(ms) * s.rate / 1000 + s.trimSkip();
      const int64_t truth = s.frames[s.frameOf(x)].byte;
      uint32_t byte = 0;
      trackseek::lameTocByte(s.toc, n, audio, s.spf, x, &byte);
      model += static_cast<double>(std::llabs(truth - static_cast<int64_t>(s.firstAudio + byte)));
      uint32_t old = 0;
      trackseek::mp3SeekByte(in.probe, in.probeBytes, s.audioStart, s.size(), 0, ms, &old);
      lines += static_cast<double>(std::llabs(truth - static_cast<int64_t>(old)));
      ++count;
    }
    TEST_ASSERT_TRUE(count > 20);
    TEST_ASSERT_TRUE_MESSAGE(model < lines, "LAME's TOC inverted isn't closer than the straight lines");
  }
}

// plan(): a LAME VBR file by its TOC inverted, landing on a frame of the
// chain at or after the estimate, inexact (the time asked); another
// encoder, no LAME tag or a decreasing TOC: the straight lines; no TOC: the
// average bitrate.
void test_plan_sources_for_vbr() {
  mp3synth::Options o;
  o.kbps = mp3synth::vbrRates(3, 6000, 9);
  o.header = mp3synth::Header::Xing;
  o.id3 = 700;
  const mp3synth::Stream s = mp3synth::make(o);
  mp3synth::Reader r(s.bytes);
  trackseek::Plan p = trackseek::plan(planIn(s, 60000), r, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::LameToc), static_cast<int>(p.source));
  TEST_ASSERT_FALSE(p.exact);
  TEST_ASSERT_EQUAL_UINT64(60000ull * 44100 / 1000, p.sample);
  TEST_ASSERT_EQUAL_UINT32(0, p.skip);
  TEST_ASSERT_TRUE(p.landByte >= p.estimate);
  TEST_ASSERT_EQUAL_UINT32(s.frames[frameIndex(s, p.landByte)].length, p.landLength);
  assertPrerollRule(s, p);
  TEST_ASSERT_EQUAL_UINT32(resumeanchor::frameHash(s.bytes.data() + p.landByte, 64), p.landHash);
  // Another encoder's tag (trusted for its delay, not LAME's TOC model).
  mp3synth::Options lavf = o;
  lavf.encoder = "Lavf58.76";
  const mp3synth::Stream sl = mp3synth::make(lavf);
  mp3synth::Reader rl(sl.bytes);
  p = trackseek::plan(planIn(sl, 60000), rl, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::XingToc), static_cast<int>(p.source));
  // No LAME extension at all.
  mp3synth::Options bare = o;
  bare.lame = false;
  const mp3synth::Stream sb = mp3synth::make(bare);
  mp3synth::Reader rb(sb.bytes);
  p = trackseek::plan(planIn(sb, 60000), rb, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::XingToc), static_cast<int>(p.source));
  // A TOC that decreases somewhere: not LAME's.
  mp3synth::Stream sd = s;
  const uint32_t tocAt = sd.audioStart + 4 + 32 + 16;
  TEST_ASSERT_EQUAL_UINT8(sd.toc[50], sd.bytes[tocAt + 50]);
  sd.bytes[tocAt + 50] = static_cast<uint8_t>(sd.bytes[tocAt + 49] - 1);
  mp3synth::Reader rd(sd.bytes);
  p = trackseek::plan(planIn(sd, 60000), rd, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::XingToc), static_cast<int>(p.source));
  // No TOC flag: the average bitrate (the header's frames and bytes).
  mp3synth::Stream sn = s;
  sn.bytes[sn.audioStart + 4 + 32 + 7] = 0x0B;  // frames, bytes, quality
  mp3synth::Reader rn(sn.bytes);
  p = trackseek::plan(planIn(sn, 60000), rn, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::Average), static_cast<int>(p.source));
  TEST_ASSERT_TRUE(frameIndex(sn, p.landByte) != 0xFFFFFFFFu);
}

// CBR arithmetic: for every millisecond of 30 s, at 32, 44.1 and 48 kHz
// and MPEG-2/2.5, with LAME's Info header and padding pattern or no header
// at all: the landing frame and skip exact, the preroll by the rule.
void test_cbr_plans_are_exact_to_the_sample() {
  struct Case {
    int version, rateIndex;
    uint32_t kbps;
    mp3synth::Header header;
  };
  const Case cases[] = {
      {3, 0, 128, mp3synth::Header::Info},  // 44.1 kHz
      {3, 1, 192, mp3synth::Header::Info},  // 48 kHz
      {3, 2, 64, mp3synth::Header::None},   // 32 kHz, no header
      {2, 0, 64, mp3synth::Header::Info},   // MPEG-2, 22.05 kHz
      {0, 0, 32, mp3synth::Header::None},   // MPEG-2.5, 11.025 kHz
  };
  for (const Case& c : cases) {
    mp3synth::Options o;
    o.version = c.version;
    o.rateIndex = c.rateIndex;
    o.header = c.header;
    o.id3 = 333;
    const uint32_t rate = mp3synth::rateOf(c.version, c.rateIndex);
    const uint32_t spf = c.version == 3 ? 1152 : 576;
    o.kbps.assign(36 * rate / spf, c.kbps);  // 36 s
    const mp3synth::Stream s = mp3synth::make(o);
    TEST_ASSERT_EQUAL(c.header != mp3synth::Header::None, s.lame);
    mp3synth::Reader r(s.bytes);
    for (uint32_t ms = 1; ms <= 30000; ++ms) {
      const trackseek::Plan p = trackseek::plan(planIn(s, ms), r, scratch);
      if (p.source != trackseek::Source::Cbr) {
        TEST_FAIL_MESSAGE("not CBR arithmetic");
      }
      const uint64_t d = static_cast<uint64_t>(ms) * rate / 1000 + s.trimSkip();
      const uint32_t k = static_cast<uint32_t>(d / spf);
      if (!p.exact || p.landByte != s.frames[k].byte || p.skip != d - static_cast<uint64_t>(k) * spf ||
          p.landLength != s.frames[k].length || p.sample != static_cast<uint64_t>(ms) * rate / 1000) {
        TEST_FAIL_MESSAGE("the CBR plan isn't the frame and sample asked");
      }
      if (ms % 97 == 0) assertPrerollRule(s, p);
    }
  }
}

// A stream that says CBR (an Info header, its first frames alike) but turns
// VBR after the probe's 4 KB: the frame isn't where arithmetic says: the
// plan falls through (the bitrate's estimate, then the chain walk).
void test_a_cbr_stream_that_turns_vbr_falls_through() {
  mp3synth::Options o;
  o.kbps.assign(3000, 128);
  for (size_t i = 40; i < o.kbps.size(); ++i) o.kbps[i] = i % 3 ? 160 : 96;
  o.header = mp3synth::Header::Info;
  const mp3synth::Stream s = mp3synth::make(o);
  mp3synth::Reader r(s.bytes);
  const trackseek::Plan p = trackseek::plan(planIn(s, 50000), r, scratch);
  TEST_ASSERT_TRUE(p.ok());
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(trackseek::Source::Cbr), static_cast<int>(p.source));
  TEST_ASSERT_FALSE(p.exact);
  TEST_ASSERT_TRUE(frameIndex(s, p.landByte) != 0xFFFFFFFFu);
}

// The chain walk: the first frame at or after the target, the preroll by
// the rule, the chain's first when nothing is far enough back; a false sync
// in the audio data and junk between frames don't fool it.
void test_the_chain_walk() {
  mp3synth::Options o;
  o.kbps = mp3synth::vbrRates(3, 400, 4);
  const mp3synth::Stream s = mp3synth::make(o);
  const uint32_t from = s.frames[100].byte + 37;  // inside frame 100
  std::vector<uint8_t> buf(s.bytes.begin() + from, s.bytes.begin() + from + trackseek::kScratchBytes);
  for (uint32_t target = 2000; target < 5000; target += 111) {
    const trackseek::Chain c = trackseek::walkChain(buf.data(), buf.size(), target);
    TEST_ASSERT_TRUE(c.land >= static_cast<int32_t>(target));
    const uint32_t land = frameIndex(s, from + static_cast<uint32_t>(c.land));
    TEST_ASSERT_TRUE(land != 0xFFFFFFFFu);
    TEST_ASSERT_TRUE(s.frames[land - 1].byte < from + target);  // the first at or after it
    TEST_ASSERT_EQUAL_UINT32(s.frames[land].length, c.landLength);
    const uint32_t pre = frameIndex(s, from + static_cast<uint32_t>(c.preroll));
    TEST_ASSERT_TRUE(pre != 0xFFFFFFFFu);
    if (pre != 101) {  // (the chain's first)
      TEST_ASSERT_TRUE(land - pre >= 2);
      TEST_ASSERT_TRUE(s.frames[land - 1].byte - s.frames[pre].byte >= 1024);
      // ... and the latest such.
      TEST_ASSERT_TRUE(s.frames[land - 1].byte - s.frames[pre + 1].byte < 1024 || land - (pre + 1) < 2);
    }
  }
  // Near the start of the buffer: the preroll is the chain's first frame.
  trackseek::Chain c = trackseek::walkChain(buf.data(), buf.size(), 0);
  TEST_ASSERT_EQUAL_UINT32(s.frames[101].byte - from, static_cast<uint32_t>(c.land));
  TEST_ASSERT_EQUAL_INT32(c.land, c.preroll);
  // A false sync (a whole header) in the audio data before the target.
  std::vector<uint8_t> fs = buf;
  const uint32_t fake = s.frames[103].byte - from + 50;
  fs[fake] = 0xFF;
  fs[fake + 1] = 0xFB;
  fs[fake + 2] = 0x90;
  fs[fake + 3] = 0x44;
  c = trackseek::walkChain(fs.data(), fs.size(), fake - 10);
  TEST_ASSERT_EQUAL_UINT32(s.frames[104].byte - from, static_cast<uint32_t>(c.land));
  // Junk that breaks the chain: the walk picks it up again after it.
  std::vector<uint8_t> junk(buf.begin(), buf.begin() + (s.frames[106].byte - from));
  junk.insert(junk.end(), 300, 0x11);
  junk.insert(junk.end(), buf.begin() + (s.frames[106].byte - from), buf.end());
  c = trackseek::walkChain(junk.data(), junk.size(), s.frames[106].byte - from + 301);
  TEST_ASSERT_EQUAL_UINT32(s.frames[107].byte - from + 300, static_cast<uint32_t>(c.land));
  TEST_ASSERT_EQUAL_UINT32(s.frames[106].byte - from + 300, static_cast<uint32_t>(c.preroll));
  // Nothing that chains: none.
  std::vector<uint8_t> zeros(4096, 0);
  TEST_ASSERT_EQUAL_INT32(-1, trackseek::walkChain(zeros.data(), zeros.size(), 0).land);
}

// The tail rule by the exact length: a VBR file whose last frames are at
// 320 kbit/s (Crescendolls) starts 5.2-10 s before its end, where the
// landed frame's bitrate said it was in its last 5 s one time in five;
// 4.9 s before: 0:00.
void test_the_tail_rule_by_the_exact_length() {
  mp3synth::Options o;
  o.kbps = mp3synth::vbrRates(3, 6000, 12);
  for (size_t i = 5600; i < 6000; ++i) o.kbps[i] = 320;
  for (size_t i = 5500; i < 5600; ++i) o.kbps[i] = 96;
  o.header = mp3synth::Header::Xing;
  const mp3synth::Stream s = mp3synth::make(o);
  mp3synth::Reader r(s.bytes);
  const uint32_t length = s.lengthMs();
  TEST_ASSERT_EQUAL_UINT32(length, planIn(s, 1).lengthMs);
  for (uint32_t before = 5200; before <= 10000; before += 100) {
    const trackseek::Plan p = trackseek::plan(planIn(s, length - before), r, scratch);
    TEST_ASSERT_TRUE_MESSAGE(p.ok(), "a start 5.2-10 s before the end went to 0:00");
  }
  trackseek::Plan p = trackseek::plan(planIn(s, length - 4900), r, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::NoPlan::Tail), static_cast<int>(p.why));
  p = trackseek::plan(planIn(s, length + 10000), r, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::NoPlan::Tail), static_cast<int>(p.why));
}

// A file shorter than its header says: its length scaled by the share it
// holds (the tail rule's and Now Playing's alike); a start in its missing
// part goes to 0:00. A byte count up to 404 over the stream (Daft Punk's
// Discovery, edited after encoding) isn't scaled.
void test_a_truncated_files_length_is_scaled() {
  mp3synth::Options o;
  o.kbps = mp3synth::vbrRates(3, 5000, 14);
  o.header = mp3synth::Header::Xing;
  o.id3 = 1200;
  const mp3synth::Stream whole = mp3synth::make(o);
  const uint32_t full = whole.lengthMs();
  mp3synth::Stream cut = whole;
  const uint32_t stream = whole.size() - whole.first;
  cut.bytes.resize(whole.first + stream * 4 / 5);  // a fifth missing
  const uint8_t* probe = cut.bytes.data() + cut.audioStart;
  const uint32_t scaled = trackseek::mp3LengthMs(probe, 4096, cut.audioStart, cut.size(), 0);
  TEST_ASSERT_UINT32_WITHIN(full / 1000 + 2, full * 4 / 5, scaled);
  TEST_ASSERT_EQUAL_UINT32(scaled, progress::mp3HeaderDurationMs(probe, 4096, cut.size(), cut.audioStart));
  TEST_ASSERT_EQUAL_UINT32(full, progress::mp3HeaderDurationMs(probe, 4096));  // (without the size: the header's)
  mp3synth::Reader r(cut.bytes);
  trackseek::Plan p = trackseek::plan(planIn(cut, full * 9 / 10), r, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::NoPlan::Tail), static_cast<int>(p.why));
  p = trackseek::plan(planIn(cut, full / 2), r, scratch);
  TEST_ASSERT_TRUE(p.ok());
  // Discovery: the byte count 380-404 over.
  o.xingBytesDelta = 404;
  const mp3synth::Stream edited = mp3synth::make(o);
  TEST_ASSERT_EQUAL_UINT32(full, trackseek::mp3LengthMs(edited.bytes.data() + edited.audioStart, 4096,
                                                       edited.audioStart, edited.size(), 0));
  TEST_ASSERT_EQUAL_UINT32(full, progress::mp3HeaderDurationMs(edited.bytes.data() + edited.audioStart, 4096,
                                                              edited.size(), edited.audioStart));
  // The scale itself.
  TEST_ASSERT_EQUAL_UINT32(1000, progress::truncatedMs(1000, 100000, 99000));
  TEST_ASSERT_EQUAL_UINT32(500, progress::truncatedMs(1000, 100000, 50000));
  TEST_ASSERT_EQUAL_UINT32(1000, progress::truncatedMs(1000, 100000, 200000));
}

// An anchor checked against the file: the size, the landing frame (a
// header at its rate, its hash, the next header), the preroll (a header of
// the same stream, before it, after the first audio frame, within 64 KB),
// the tail rule. Ok: a plan by its bytes, as exact as the anchor says.
void test_an_anchor_is_checked_against_the_file() {
  mp3synth::Options o;
  o.kbps = mp3synth::vbrRates(3, 3000, 15);
  o.header = mp3synth::Header::Xing;
  o.id3 = 100;
  const mp3synth::Stream s = mp3synth::make(o);
  mp3synth::Reader r(s.bytes);
  ResumeAnchor a;
  a.kind = ResumeAnchor::Kind::Mp3;
  a.exact = true;
  a.rate = 44100;
  a.fileSize = s.size();
  a.frameByte = s.frames[1000].byte;
  a.prerollByte = s.frames[990].byte;
  a.skip = 333;
  a.sample = 1000ull * 1152 + 333 - s.trimSkip();
  a.frameHash = resumeanchor::frameHash(s.bytes.data() + a.frameByte, 32);
  const uint32_t length = s.lengthMs();
  trackseek::Plan p;
  using trackseek::AnchorCheck;
  auto check = [&](const ResumeAnchor& x) {
    return trackseek::checkAnchor(x, r, s.size(), s.firstAudio, length, scratch, &p);
  };
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Ok), static_cast<int>(check(a)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::Anchor), static_cast<int>(p.source));
  TEST_ASSERT_TRUE(p.exact);
  TEST_ASSERT_EQUAL_UINT32(a.prerollByte, p.prerollByte);
  TEST_ASSERT_EQUAL_UINT32(a.frameByte, p.landByte);
  TEST_ASSERT_EQUAL_UINT32(s.frames[1000].length, p.landLength);
  TEST_ASSERT_EQUAL_UINT32(1152, p.spf);
  TEST_ASSERT_EQUAL_UINT32(333, p.skip);
  TEST_ASSERT_EQUAL_UINT64(a.sample, p.sample);
  ResumeAnchor x = a;
  x.fileSize += 1;  // a re-tag that changed the size
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Size), static_cast<int>(check(x)));
  x = a;
  x.frameHash ^= 1;  // another file
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Frame), static_cast<int>(check(x)));
  x = a;
  x.frameByte += 1;  // not a frame there
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Frame), static_cast<int>(check(x)));
  x = a;
  x.rate = 48000;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Frame), static_cast<int>(check(x)));
  x = a;
  x.prerollByte += 3;  // not a header
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Preroll), static_cast<int>(check(x)));
  x = a;
  x.prerollByte = s.frames[1001].byte;  // after the landing frame
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Preroll), static_cast<int>(check(x)));
  x = a;
  x.prerollByte = s.first;  // the header frame: never decoded by a plan
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Preroll), static_cast<int>(check(x)));
  x = a;
  x.prerollByte = s.frames[10].byte;  // over 64 KB back
  TEST_ASSERT_TRUE(a.frameByte - x.prerollByte >= trackseek::kMaxPrerollSpan);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Preroll), static_cast<int>(check(x)));
  x = a;
  x.frameByte = x.prerollByte = s.firstAudio;  // inside the start trim: from the top, landing there
  x.frameHash = resumeanchor::frameHash(s.bytes.data() + s.firstAudio, 32);
  x.sample = 10;
  x.skip = 10 + s.trimSkip();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Ok), static_cast<int>(check(x)));
  x = a;
  x.sample = static_cast<uint64_t>(length - 3000) * 44100 / 1000;  // its last 5 s
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Tail), static_cast<int>(check(x)));
  x = a;
  x.kind = ResumeAnchor::Kind::Flac;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AnchorCheck::Kind), static_cast<int>(check(x)));
}

// No plan: a VBR file without a header or a length (unplaced), bytes that
// aren't an MP3.
void test_no_plan() {
  mp3synth::Options o;
  o.kbps = mp3synth::vbrRates(3, 3000, 16);
  const mp3synth::Stream s = mp3synth::make(o);  // no header
  mp3synth::Reader r(s.bytes);
  trackseek::Plan p = trackseek::plan(planIn(s, 30000), r, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::NoPlan::Unplaced), static_cast<int>(p.why));
  // With the length it had: the average bitrate, then the chain.
  p = trackseek::plan(planIn(s, 30000, s.lengthMs()), r, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::Average), static_cast<int>(p.source));
  std::vector<uint8_t> junk(100000, 0x42);
  mp3synth::Reader rj(junk);
  trackseek::PlanIn in;
  in.probe = junk.data();
  in.probeBytes = 4096;
  in.fileSize = 100000;
  in.targetMs = 1000;
  p = trackseek::plan(in, rj, scratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::NoPlan::NoFrame), static_cast<int>(p.why));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_ms_edges);
  RUN_TEST(test_mp3_constant_bitrate);
  RUN_TEST(test_mp3_xing_toc);
  RUN_TEST(test_mp3_info_cbr);
  RUN_TEST(test_mp3_vbri_toc);
  RUN_TEST(test_mp3_without_a_header);
  RUN_TEST(test_mp3_frame_sync);
  RUN_TEST(test_flac_stream_info);
  RUN_TEST(test_lame_bag_closed_form);
  RUN_TEST(test_lame_toc_inverted_beats_straight_lines);
  RUN_TEST(test_plan_sources_for_vbr);
  RUN_TEST(test_cbr_plans_are_exact_to_the_sample);
  RUN_TEST(test_a_cbr_stream_that_turns_vbr_falls_through);
  RUN_TEST(test_the_chain_walk);
  RUN_TEST(test_the_tail_rule_by_the_exact_length);
  RUN_TEST(test_a_truncated_files_length_is_scaled);
  RUN_TEST(test_an_anchor_is_checked_against_the_file);
  RUN_TEST(test_no_plan);
  return UNITY_END();
}
