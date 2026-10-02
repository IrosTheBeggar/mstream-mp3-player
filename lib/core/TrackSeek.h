// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ResumeAnchor.h"

// Starting a track part of the way in (docs/SEEK.md): a resume point after
// a restart or a power-off, a seek (the console's qs). What that needs from
// the file, through a reader (FileReader) and no decoder:
//
// - where a start really lands: startMs(), the tail rule (the last 5 s and
//   past the end start at 0:00), by the exact length (mp3LengthMs(): the
//   header's frames x spf less LAME's delay and padding, scaled down for a
//   file shorter than its header says; never one frame's bitrate);
// - an MP3 start plan (plan(), checkAnchor()): the byte to hand the decoder
//   (a preroll frame, at least 2 frames and 1 KB before the landing frame,
//   so the landing frame decodes bit for bit: its bit reservoir and the
//   filterbank's history), the landing frame, and the samples to skip from
//   its first one. TrimFeed::armAt() drops everything until the generator
//   says the landing frame. The sources, the first that gives a plan:
//   1. the resume point's anchor (checkAnchor(): the backend's);
//   2. the run index of what this run decoded (SeekIndex: the backend's);
//   3. CBR arithmetic (an Info header, or none, and frames that agree on
//      their bitrate): frame k is at firstAudio + floor(k x L) or a byte
//      later: exact;
//   4. LAME's TOC inverted (a LAME VBR file's Xing TOC as LAME computed it:
//      point i is the byte share after (floor(i x pos / 100) + 1) x want
//      frames, truncated to 1/256; section 6.3);
//   5. other encoders' Xing TOC (point i at i% of the time), 6. VBRI's TOC,
//      7. the average bitrate (the header's, or the hint's), each an
//      estimate, then the chain walk: the first frame at or after it, in a
//      chain of headers read from 2.5 KB before it.
//   A VBR file with nothing to place it by, no chain there, or the tail
//   rule: from 0:00 (never a start that ends at once: the player would move
//   on).
// - a FLAC's sample rate and length (STREAMINFO): libFLAC seeks by sample
//   itself (its SEEKTABLE, else a bisection on the frames' headers).
//
// Accuracy: an anchor, the index of an exact run and CBR to the sample; a
// LAME VBR file p95 0.74 s (measured on 27 files; 1.3 s by today's straight
// lines); other VBR files to about 1% of their length. Samples are on the
// trimmed timeline (docs/GAPLESS.md section 4.6). Portable, host-tested
// (test_track_seek).
namespace trackseek {

// A start in a track's last kTailMs, or at or past its end, starts it at
// 0:00 instead: nobody wants the last 2 s of a song, and a file that got
// shorter since the second was saved plays from its start.
constexpr uint32_t kTailMs = 5000;

// Where a start asked for at `requestMs` begins, in a track `durationMs`
// long (0: not known: as asked; a seek past the end then fails, and the
// backend starts it at 0:00).
uint32_t startMs(uint32_t requestMs, uint32_t durationMs);

// How today's estimate of an MP3's byte was found (mp3SeekByte()).
// Unplaced: a VBR file with no header and no length known: from 0:00.
// CbrInfo: LAME's "Info" header (a CBR file) whose frames agree on their
// bitrate: by that bitrate, not the TOC.
enum class Mp3Seek : uint8_t { None, CbrInfo, XingToc, VbriToc, AverageBitrate, FrameBitrate, Unplaced };
const char* mp3SeekName(Mp3Seek how);

// `buf` holds the `n` bytes of the file from `audioStart` (after its ID3v2
// tag), `fileSize` its size. `hintMs`: the track's length as known
// elsewhere (the resume point's, as the backend had it), 0: none; only
// read for a file without a header.
//
// The length, the tail rule's: the Xing/VBRI header's (with LAME's
// extension the trimmed length: what the encoder was given), scaled down
// for a file more than 4 KB shorter than the header's byte count
// (progress::truncatedMs()); else the audio bytes at the first frame's
// bitrate (exact for a CBR file), or the hint when that is VBR. 0: no frame
// found, or a VBR file without a hint.
uint32_t mp3LengthMs(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs);
// Today's estimate of the byte to start at `targetMs` (with LAME's extension
// on the trimmed timeline: the encoder delay and libmad's 529 samples
// later in the stream): an Info file by its bitrate; else the Xing TOC
// (straight lines between its points), VBRI's, the average bitrate. None:
// no frame found; Unplaced: VBR, with nothing to place it by. plan()'s
// sources 5-7 start from it.
Mp3Seek mp3SeekByte(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs,
                    uint32_t targetMs, uint32_t* byte);
// The first offset in `buf` where a Layer III frame starts and the next
// frame's header (the same version and sample rate) follows it, inside
// `buf`. -1: none.
int32_t mp3FrameAt(const uint8_t* buf, size_t n);

// ---- start plans (docs/SEEK.md sections 4.5, 5.4, 6) ----

// The file's bytes (the firmware's: the decode task's open file; the
// tests': a buffer).
class FileReader {
public:
  // Up to `n` bytes at `offset`: how many were read (fewer at the end).
  virtual uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n) = 0;

protected:
  ~FileReader() = default;
};

// The scratch the plans read into (the firmware's: PSRAM, per plan).
constexpr uint32_t kScratchBytes = 8192;
// The preroll (docs/SEEK.md section 2.4): the landing frame k decodes bit
// for bit when the frames before it that its output depends on decode too:
// frame k - 1 (its IMDCT overlap and the filterbank's history; MPEG-2/2.5's
// one-granule frames: k - 2 as well, historyFrames()). Each of those needs
// its bit reservoir: at most 511 bytes of main data before it (255 for
// MPEG-2/2.5), in the frames from the one the decoder was handed (libmad
// keeps the payload of frames it couldn't decode as reservoir). So the
// preroll is the latest frame at least historyFrames + 1 frames before k
// and kPrerollBytes before frame k - historyFrames: 1,024 bytes of whole
// frames hold at least 669 bytes of main data even at 32 kbit/s. (1 KB
// before frame k itself isn't enough when frame k - 1 is big: a loud frame
// after a quiet stretch can reach 511 bytes back over many small frames.)
constexpr uint32_t kPrerollBytes = 1024;
inline uint32_t historyFrames(uint32_t spf) { return spf == 1152 ? 1 : 2; }
// Where only the landing frame's byte is known (SeekIndex's entries): the
// frames before it are at most this long (MPEG-1 at 320 kbit/s and 32 kHz,
// padded: 1,441; two MPEG-2 frames at 160 kbit/s and 16 kHz: 1,442).
constexpr uint32_t kMaxHistoryBytes = 1442;
// A preroll further back than this isn't believed (an anchor's check).
constexpr uint32_t kMaxPrerollSpan = 65536;
// The chain walk reads kScratchBytes from this far before the estimate:
// the preroll's 1 KB plus two of the largest frames, and room for the
// chain to start after the read's first bytes.
constexpr uint32_t kWalkBack = 4096;
// The largest MP3 frame (MPEG-1, 320 kbit/s, 32 kHz, padded).
constexpr uint32_t kMaxFrameBytes = 1441;

enum class Source : uint8_t { None, Anchor, Index, Cbr, LameToc, XingToc, VbriToc, Bitrate, Average };
// For the log: "its resume anchor", "the run's index", "CBR", ...
const char* sourceName(Source s);
// Why there is no plan (the start is from 0:00).
enum class NoPlan : uint8_t { None, Tail, Unplaced, NoFrame };
const char* noPlanName(NoPlan why);

struct Plan {
  Source source = Source::None;  // None: from 0:00 (why)
  NoPlan why = NoPlan::None;
  bool exact = false;        // `sample` is where it starts in the file (else the time asked: a TOC's estimate)
  uint32_t prerollByte = 0;  // the decoder is handed this frame's start
  uint32_t landByte = 0;     // the landing frame
  uint32_t landLength = 0;   // ... its bytes
  uint32_t landHash = 0;     // ... resumeanchor::frameHash()
  uint32_t rate = 0;
  uint32_t spf = 0;
  uint32_t skip = 0;         // samples from the landing frame's first one to `sample`
  uint64_t sample = 0;       // on the trimmed timeline: the start's (exact) or the time asked
  uint32_t estimate = 0;     // sources 4-8: the estimate's byte (for the log)
  bool ok() const { return source != Source::None; }
};

struct PlanIn {
  const uint8_t* probe = nullptr;  // the file's first bytes from audioStart (its Xing/VBRI header)
  size_t probeBytes = 0;
  uint32_t audioStart = 0;  // after the ID3v2 tags
  uint32_t fileSize = 0;
  uint32_t hintMs = 0;      // its length as known elsewhere (0: none)
  uint32_t targetMs = 0;    // the start asked, on the trimmed timeline (> 0)
  uint32_t lengthMs = 0;    // the tail rule's length (mp3LengthMs(); 0: not known)
  // The trimmed timeline is LAME's: a trusted tag's delay + 529 samples
  // into the decoded stream (gapless trimming on, G1 and Gt1); false: the
  // decoded stream from the first audio frame.
  bool useTag = false;
};

// The first audio frame's offset in the file: the first frame, or the one
// after its Xing/Info/VBRI header frame (which carries no audio and is
// never decoded by a plan). 0: no frame in the probe.
uint32_t firstAudioByte(const uint8_t* probe, size_t n, uint32_t audioStart);

// Sources 3-7 (see the namespace), with the tail rule first. `scratch`:
// kScratchBytes.
Plan plan(const PlanIn& in, FileReader& file, uint8_t* scratch);

// An anchor (a resume point's, or one SeekIndex made for a seek into its
// run) checked against the file (docs/SEEK.md section 5.4): the size; at
// frameByte a Layer III header at its rate whose first bytes hash to
// frameHash, followed by a header of the same version and rate; at
// prerollByte a header of the same version and rate, at or after the first
// audio frame, before frameByte (or equal to it at the first audio frame)
// and within kMaxPrerollSpan; the tail rule. Ok: `out` is its plan
// (source Anchor).
enum class AnchorCheck : uint8_t { Ok, Kind, Size, Frame, Preroll, Tail };
// For the log: "the size", "the frame at 2345678", ...
const char* anchorCheckName(AnchorCheck c);
AnchorCheck checkAnchor(const ResumeAnchor& a, FileReader& file, uint32_t fileSize, uint32_t firstAudio,
                        uint32_t lengthMs, uint8_t* scratch, Plan* out);

// ---- the pieces, for the tests ----

// LAME's seek table "bag" after `frames` frames (VbrTag.c's AddVbrFrame():
// a sum every `want` frames into 400 slots; full, every other slot is kept
// and `want` doubles): want is the smallest power of 2 with frames / want
// under 400, pos = frames / want.
struct LameBag {
  uint32_t want = 1;
  uint32_t pos = 0;
};
LameBag lameBag(uint32_t frames);
// A TOC LAME can have written: 100 points, non-decreasing.
bool tocUsable(const uint8_t toc[100]);
// LAME's TOC inverted: the byte offset from the first audio frame where
// decoded sample `x` (counted from the first audio frame's first) lies, by
// straight lines between the points (F_i frames, (toc[i] + 0.5) / 256 of
// `audioBytes`), (0, 0) and (frames, audioBytes). False: no frames or bytes.
bool lameTocByte(const uint8_t toc[100], uint32_t frames, uint32_t audioBytes, uint32_t spf, uint64_t x,
                 uint32_t* byte);

// The chain walk over `n` bytes of the file read into `buf`: from the
// first frame that starts a chain (a header followed by two more of the
// same version and rate, or by one and the end of `buf`), the first frame
// at or after offset `target` whose next header follows it (`land`), and
// the preroll: the latest frame of the same chain historyFrames + 1 frames
// or more before it and kPrerollBytes before frame land - historyFrames
// (none: the chain's first).
// Junk breaks a chain: the walk looks for the next one. land -1: none.
struct Chain {
  int32_t land = -1;
  int32_t preroll = -1;
  uint32_t landLength = 0;
};
Chain walkChain(const uint8_t* buf, size_t n, size_t target);

// A FLAC file's sample rate and total samples (0: not said) from its first
// bytes ("fLaC" and STREAMINFO; 26 are enough). False: not one.
bool flacStreamInfo(const uint8_t* buf, size_t n, uint32_t* rate, uint64_t* totalSamples);

}  // namespace trackseek
