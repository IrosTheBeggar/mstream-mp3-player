// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Starting a track part of the way in: the resume point ("picks up at the
// same second" after a restart or a power-off; QueueSaver keeps it, the
// backend starts there). What that needs from the file, without the file:
//
// - where a start really lands: startMs() (the last seconds and past the
//   end start at 0:00);
// - an MP3's byte for a time: a LAME CBR file (an "Info" header) by its
//   bitrate; else its Xing table of contents (100 points, LAME VBR and
//   other encoders' CBR files), else its VBRI one (Fraunhofer), else its
//   average bitrate (the header's length over its bytes). A file without a
//   header: the first frame's bitrate (a plain CBR file: exact), unless its
//   first frames differ in bitrate or a length known elsewhere (the resume
//   point's) disagrees with it: then the average bitrate by that length. A
//   VBR file without either can't be placed: it starts at 0:00;
// - a clean frame to start on near that byte: mp3FrameAt() (a header whose
//   next frame's header follows, so a 0xFF in the audio data doesn't count);
// - a FLAC's sample rate and length (STREAMINFO): libFLAC seeks by sample
//   itself (its SEEKTABLE, else a bisection on the frames' headers).
//
// Accuracy: a CBR MP3 to the frame (~26 ms), a FLAC to the sample; a VBR MP3
// with a TOC to about 1% of its length (the TOC's resolution); one without
// by its average bitrate. The time shown is the time asked for.
//
// A byte that doesn't lead to a clean frame (bad data, or a file shorter
// than its header says), or to one with kTailMs or less of audio after it
// at that frame's bitrate, is a failed seek: the backend starts at 0:00, as
// for any start past the end (never a start that ends at once: the player
// would move on to the next entry).
// Portable, host-tested (test_track_seek).
namespace trackseek {

// A start in a track's last kTailMs, or at or past its end, starts it at
// 0:00 instead: nobody wants the last 2 s of a song, and a file that got
// shorter since the second was saved plays from its start.
constexpr uint32_t kTailMs = 5000;

// Where a start asked for at `requestMs` begins, in a track `durationMs`
// long (0: not known: as asked; a seek past the end then fails, and the
// backend starts it at 0:00).
uint32_t startMs(uint32_t requestMs, uint32_t durationMs);

// How an MP3's byte was found (for the log).
// Unplaced: a VBR file with no header and no length known: from 0:00.
// CbrInfo: LAME's "Info" header (a CBR file) whose frames agree on their
// bitrate: by that bitrate, not the TOC (exact; the TOC was up to ~0.3 s
// off on the device).
enum class Mp3Seek : uint8_t { None, CbrInfo, XingToc, VbriToc, AverageBitrate, FrameBitrate, Unplaced };
const char* mp3SeekName(Mp3Seek how);

// `buf` holds the `n` bytes of the file from `audioStart` (after its ID3v2
// tag), `fileSize` its size. `hintMs`: the track's length as known
// elsewhere (the resume point's, as the backend had it), 0: none; only
// read for a file without a header.
//
// The length: the Xing/VBRI header's; else the audio bytes at the first
// frame's bitrate (exact for a CBR file), or the hint when that is VBR
// (see above). 0: no frame found, or a VBR file without a hint.
uint32_t mp3LengthMs(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs);
// The file offset to look for a frame at, to start at `targetMs` (under
// the length). None: no frame found (the file isn't an MP3 we know);
// Unplaced: VBR, with nothing to place it by.
Mp3Seek mp3SeekByte(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs,
                    uint32_t targetMs, uint32_t* byte);
// The first offset in `buf` where a Layer III frame starts and the next
// frame's header (the same version and sample rate) follows it, inside
// `buf`. -1: none.
int32_t mp3FrameAt(const uint8_t* buf, size_t n);
// The audio in the `bytesLeft` bytes from the frame header at `frame` (to
// the file's end), at that frame's bitrate. 0: not a frame header.
uint32_t mp3MsLeft(const uint8_t* frame, uint32_t bytesLeft);

// A FLAC file's sample rate and total samples (0: not said) from its first
// bytes ("fLaC" and STREAMINFO; 26 are enough). False: not one.
bool flacStreamInfo(const uint8_t* buf, size_t n, uint32_t* rate, uint64_t* totalSamples);

}  // namespace trackseek
