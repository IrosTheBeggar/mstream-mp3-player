// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// An MP3's gapless information (docs/GAPLESS.md section 4): the first
// frame's Xing/Info header (or Fraunhofer's VBRI), which carries no audio,
// and LAME's extension after the Xing fields, which says how many samples
// the encoder added at the start (the delay) and at the end (the padding).
//
// The layout [1][2] (the numbers are docs/GAPLESS.md section 14's sources):
//   the frame header (4 bytes; 6 with a CRC: the protection bit 0), the
//   side information (32 bytes MPEG-1 stereo, 17 MPEG-1 mono or MPEG-2/2.5
//   stereo, 9 MPEG-2/2.5 mono), then "Xing" or "Info", 4 bytes of flags
//   (big-endian) and, each only if its flag is set, the frame count (0x1,
//   4 bytes), the byte count (0x2, 4), the TOC (0x4, 100) and the quality
//   (0x8, 4). LAME's extension starts right after the last field present
//   (36 bytes): +0 the encoder's version string (9 bytes, "LAME3.100"),
//   +21 the delay and the padding, 12 bits each in 3 bytes
//   [dddddddd][ddddpppp][pppppppp], ... +34 the tag's CRC-16 over every
//   byte of the frame before it.
// A VBRI header sits at 4 + 32 bytes whatever the mode.
//
// Which extensions are trusted: FFmpeg's rule [4], the version string
// starts with "LAME", "Lavf" or "Lavc" (FFmpeg's encoder writes the same
// layout). Ours on top: the frame count must be said, and delay + padding
// must be less than its samples. The CRC is checked and reported, never
// required (FFmpeg doesn't check it; taggers rewrite the frame).
//
// The trim (section 4.2-4.3), in samples per channel at the file's rate,
// decoding from the first audio frame (the header frame is not decoded):
//   skip = lead + delay + 529        (lead: the generator's own {0,0})
//   hold = max(0, padding - 529)     (cut at the end)
//   kept = frames x spf - delay - padding
// 529 = 528 + 1, the decoder delay of the standard's synthesis: FFmpeg's
// mp3dec.c (start_skip_samples = start_pad + 528 + 1) [4] and Rockbox's
// libmad codec (mpeg_latency[] = { 0, 481, 529 }) [5]; to be confirmed on
// this libmad by the device's impulse file (section 11.1).
//
// Portable, host-tested (test_lame_tag).
namespace lametag {

constexpr uint32_t kDecoderDelay = 529;

struct Info {
  bool frame = false;          // a Layer III frame was found: the first
  uint32_t frameAt = 0;        // its offset in the buffer
  uint32_t rate = 0;           // Hz
  uint32_t spf = 0;            // samples per frame: 1152 (MPEG-1) or 576
  uint32_t kbps = 0;
  bool header = false;         // the first frame is a Xing/Info/VBRI header: no audio
  uint32_t headerLength = 0;   // its length in bytes: the audio starts after it
  bool xing = false;           // "Xing" (VBR) or "Info" (CBR)
  bool info = false;           // "Info"
  bool vbri = false;
  uint32_t frames = 0;         // audio frames, as the header says (0: not said)
  bool lame = false;           // a LAME extension we trust: delay and padding below
  char encoder[10] = "";       // its version string (also when not trusted)
  uint16_t delay = 0;          // encoder delay, samples
  uint16_t padding = 0;        // samples of padding at the end
  bool crcChecked = false;     // the whole tag was in the buffer
  bool crcOk = false;
  uint16_t crcStored = 0;
  uint16_t crcComputed = 0;
  uint32_t crcBytes = 0;       // the bytes it covers (190 for MPEG-1 stereo with all four Xing fields)
};

// `buf` holds `n` bytes of the file from the end of its ID3v2 tags. The
// first frame is the first Layer III header whose next frame's header
// follows it (or would lie past the buffer), as TrackProgress finds it.
// False: none found (`out` then says frame = false).
bool parse(const uint8_t* buf, size_t n, Info* out);
// The same, the first frame known: `buf` starts with it (a caller that found
// it by its own rule, as TagScan by lofty's). No search and no look at the
// next frame, so the answer depends on the buffer only within the frame's
// first 194 bytes (the furthest LAME's extension reaches). False: `buf`
// doesn't start with a Layer III header.
bool parseFirst(const uint8_t* buf, size_t n, Info* out);

// The samples the encoder was given (kept), or the header's frames x spf
// without a trusted extension; 0: the header doesn't say.
uint64_t keptSamples(const Info& info);
// The track's length on the trimmed timeline (0: not said).
uint32_t lengthMs(const Info& info);
// A time on the trimmed timeline as a time in the decoded stream from its
// first audio frame (for a seek's byte): + (delay + 529) / rate with a
// trusted extension, else the same.
uint32_t untrimmedMs(const Info& info, uint32_t ms);

struct Trim {
  uint32_t skip = 0;  // frames dropped at the start
  uint32_t hold = 0;  // frames held back and dropped at the end
};
// The trim for a decoder starting at the first audio frame (`fromTop`) or
// part of the way in (a seek: only the lead), the generator adding `lead`
// frames of its own first. `useTag` false (the console's Gt0, or a tag not
// trusted): only the lead.
Trim trim(const Info& info, uint32_t lead, bool fromTop, bool useTag);

// CRC-16/ARC (polynomial 0x8005 reflected, initial 0), LAME's.
uint16_t crc16(const uint8_t* p, size_t n);

}  // namespace lametag
