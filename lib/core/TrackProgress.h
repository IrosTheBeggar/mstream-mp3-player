// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// A track's length, estimated from how fast the decoder goes through its
// file: the bytes it read since its first audio, per frame it made, over
// the bytes still to read. The files carry no length the decoders expose
// (ESP8266Audio gives neither an MP3's frame count nor a FLAC's
// STREAMINFO), but for a constant bitrate MP3 this is exact, and for a
// variable one or a FLAC it settles within a few seconds. Measuring from
// the first audio leaves out the tags in front (an ID3 tag with a cover can
// be half a megabyte). Portable, host-tested.
namespace progress {

// `frames` made since the start at `rate` Hz; the file position when the
// first audio came (`pos0`), now (`pos`), and the file's size. 0: not
// enough to go on yet (under `minFrames`, or nothing read since pos0).
uint32_t estimateDurationMs(uint64_t frames, int rate, uint32_t pos0, uint32_t pos, uint32_t size,
                            uint64_t minFrames = 44100);

// The estimate is exact only for a constant bitrate: a VBR MP3 whose start
// is quieter than its average (most LAME VBR files) read 7:37 for a 5:20
// track 15 s in (on the device). Those files, and FLAC files, say their
// length at the start instead, which the backend reads when a track opens:

// The size of the ID3v2 tag the file starts with (its header, body and any
// footer), from its first 10 bytes; 0 if it has none.
uint32_t id3v2Size(const uint8_t* head, size_t n);

// An MP3's length from the Xing/Info or VBRI header in its first frame:
// `buf` holds the bytes after the ID3v2 tag. 0 when it has none (a plain
// constant bitrate file: estimateDurationMs() is exact for those). With
// LAME's extension, the trimmed length: frames x spf less its delay and
// padding (LameTag, docs/GAPLESS.md section 4.6). With the file's size and
// where `buf` starts in it (`audioStart`), a file shorter than its header's
// byte count says has its length scaled down (truncatedMs(), as the tail
// rule's: docs/SEEK.md section 7).
uint32_t mp3HeaderDurationMs(const uint8_t* buf, size_t n, uint32_t fileSize = 0, uint32_t audioStart = 0);

// A file shorter than its header says (docs/SEEK.md section 7): the header
// counts `headerBytes` from its first frame, the file holds `haveBytes`
// from there. More than kTruncatedSlack short: `lengthMs` scaled by the
// share it holds; else as is (Daft Punk's Discovery, edited after
// encoding, says 380-404 bytes more than its stream holds).
constexpr uint32_t kTruncatedSlack = 4096;
uint32_t truncatedMs(uint32_t lengthMs, uint32_t headerBytes, uint32_t haveBytes);

// A Layer III frame header (MPEG-1, 2 or 2.5): what TrackSeek needs too.
struct Mp3Frame {
  int version;    // the header's 2 bits: 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
  int rateIndex;  // the header's 2 bits
  int rate;       // Hz
  int samples;    // per frame: 1152 or 576
  int sideInfo;   // bytes after the header
  int kbps;
  int length;     // bytes, the header included
};
// The header at p (4 bytes), or false if it isn't one (free format, a bad
// bitrate or rate, another layer).
bool parseMp3Frame(const uint8_t* p, Mp3Frame* f);

// A FLAC file's length from its STREAMINFO (the first metadata block, right
// after "fLaC"): `buf` holds the file's first bytes (42 are enough). 0 if
// it isn't one, or the encoder didn't know the total.
uint32_t flacDurationMs(const uint8_t* buf, size_t n);

}  // namespace progress
