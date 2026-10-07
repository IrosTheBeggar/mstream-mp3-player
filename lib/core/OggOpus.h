// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "OggPage.h"
#include "ResumeAnchor.h"

// An Ogg Opus file (RFC 7845) as a track, for the firmware's Opus
// generator (docs/OPUS.md): the headers, the packets in order, and the
// trims that make the track sample-exact. Nothing of the decoder is in
// here: the generator owns libopus (ESP8266Audio's bundled copy, through
// "libopus/include/opus.h"; never its AudioGeneratorOpus), and this
// reader hands it packets and says which of the decoded samples to keep.
//
// The file: a BOS page carrying the OpusHead packet (the channels, the
// pre-skip, the output gain, the channel mapping), the OpusTags packet on
// its own page(s) (a comment header: skipped by its page headers only,
// however big: a 2 MB picture costs a few KB of reads), then the audio
// pages, ~1 s each from ffmpeg, the last one flagged EOS. Every Opus
// packet is 48 kHz: a page's granule position is the absolute index
// (from the stream's own 0) of the sample after the last packet that
// completes on it; the EOS page's is the end of the audio, which trims
// the last packet. What is supported in v1: mapping family 0 with 1 or
// 2 channels, and family 1 with one stream (coupled for 2 channels),
// whose mapping table can swap the channels. Surround (more streams:
// the bundled libopus has no multistream decoder), family 255,
// multiplexed files (several streams: video) and frames under 10 ms
// (kMinFrameSamples: too slow to decode here) are refused with a reason;
// Vorbis, FLAC, Speex and Theora are named in it. A chained file (another
// stream after ours, RFC 3533) plays its first link and ends there, with
// that link's exact length (its last page is found by a bisection on the
// serial numbers when the file's tail belongs to a later link; a BOS page
// is believed only once read whole and checked, so a damaged flag on a
// page of ours isn't a link). A later link that reuses our serial number
// (RFC 3533 forbids it; joined ffmpeg `-fflags +bitexact` outputs do it)
// still ends play at its BOS page, and the tail scan stops counting pages
// at a BOS it walks past; but a tail window or a probe that lands inside
// such a link can't tell its pages from ours, so its length and seeks are
// the second link's: unsupported, by the RFC's rule.
//
// The timeline (docs/GAPLESS.md section 4.6, Timeline below): the
// trimmed sample t of a decoded sample with absolute index k is
// t = k - g0 - preSkip. g0 is where the first audio page starts (its
// granule less the samples of the packets completing on it: usually 0,
// larger for a file cropped at the start), the pre-skip is the encoder's
// delay, which the head says. The end: the EOS page keeps
// granule(EOS) - granule(the page before) samples of the packets that
// complete on it, the rest is the encoder's padding. So a track from the
// top keeps [g0 + preSkip, granule(EOS)): sample-exact, as a FLAC.
// The headers, the tail scan and the plans read pages whole (one 282-byte
// read for the header, one for the rest); the audio pages are read in
// slices while a track plays (setReadSlice(): 8 KB in the firmware, a
// slice a next() call with Pending between them, and one more Pending
// once the page is whole and checked, so the first decode of its packets
// is a step of its own: docs/OPUS.md sections 8.2 and 10); a page's CRC
// is checked once it is whole, before any packet of it is used; a packet
// inside a page is handed over in place, one spanning pages is assembled
// in the packet buffer. The page in hand is kept across a start when it
// is the page the start reads from (the first audio page after
// open(true), a plan's Q read whole by its check), so it isn't read twice.
//
// What an open costs on the card (docs/OPUS.md section 10, the M4 model;
// a read ~3 ms plus ~0.6 ms a KB): the BOS page and the OpusTags page's
// header (2 small reads), the tail scan (one 16 KB window read as a chunk
// and checked in memory on an mStream file: 1 read; a 64 KB chunk, then
// the 73 KB walk, when the last page isn't in it) and the first audio page
// whole (2 reads: ~16 KB at 128k), ~5 reads and ~33 KB, ~30 ms, against
// M3's ~13 reads, ~84 KB and ~84 ms. An OpenRecord (record()) holds what
// the open learnt, and openFrom() opens the same file again from it with
// one read (the BOS page's header: its serial number and CRC field must
// match): the firmware's per-path cache (OpusOpenCache), so a seek on the
// playing track, or a track played before, opens with no tail scan.
//
// Damage (RFC 7845 section 3, the research's 6.9): a damaged page (its
// CRC), a lost page (a sequence gap) or junk: the next good page of our
// stream is found and decoding goes on from it, the packet that was cut
// dropped, never decoded. The first packet after the gap says where it
// starts on the absolute timeline (Packet::startK: its page's granule
// less the samples of the packets completing on the page from it on, by
// their TOCs), so the Timeline knows the gap's size (Timeline::gap()) and
// the generator fills it, concealment then silence, before decoding on:
// the count never slips, the track's length stays exact, and a join's
// sample count holds. The part of a gap that lies before the first kept
// sample (a plan's preroll crossing a damaged stretch, the pre-skip) is
// never heard: the count steps over it, no fill is made for it, and only
// what would be heard counts against kMaxGapSamples (10 s), the gap that
// ends the track (the generator's rule). A gap it can't size (a page with
// no granule, a malformed packet on it) is put right at that page's end by
// its granule instead (Timeline::packetDone()). The search for the next
// good page is bounded (how far, and how many bytes it may read: a crafted
// file can make every "OggS" cost a 65 KB read), and past the bound the
// track ends as a truncated one; it is also made one read a next() call
// (a 4 KB chunk of the file, or a slice of a candidate page that reaches
// past its chunk: ogg::PageReader::findStep()), Next::Pending between
// them, so the decode task's pass can end between its steps as it does
// between a page's slices (docs/OPUS.md section 8.11: read in one call,
// the resync after a damaged 64 KB page and the page after it were a
// 139 ms step). One next() call is bounded too: after 64
// pages, or 128 KB of reads, that yielded no packet (empty pages, another
// stream's, the pages of one endless packet) it returns Next::Pending and
// the next call carries on, so the decode task's pass ends instead of
// holding the core for a crafted file's worth of reads. A granule position
// over kMaxGranule (700 years of audio) is a damaged or crafted one and
// counts as none, so the timeline's arithmetic never overflows.
//
// A start part of the way in (docs/SEEK.md's rules, the research's 6.5;
// the firmware's seeks and resume points since M3, docs/OPUS.md section
// 9: Core2AudioBackend::prepare() plans, OpusGenerator::begin() starts by
// the plan): the target G = t + g0 + preSkip on the absolute timeline, and
// P = G - preroll. An interpolated bisection by page headers (our serial,
// a granule that can be believed, and one no lower than the page before
// it: a lower one is damage or a later link, and the page wanted is before
// it) finds the last page Q whose granule is at or under P, and Q is then
// read whole and checked (its CRC, its granule as the header said: a
// damaged granule field would land the start wrong; when it fails, the
// page found before it is checked instead, and past that the first audio
// page stands in: an earlier start, still exact). The first packet
// completing after Q starts at exactly granule(Q), so reading starts there
// (at Q itself, its own packets stepped over, when a packet continues out
// of it) with the Timeline at granule(Q), and the page after Q is checked
// against Q's sequence number (a page lost right after Q is a gap, sized
// as any other); packets ending at or before P are skipped by their TOC
// without a decode (Timeline::wanted()), the one holding P and all after
// it are decoded (the decoder reset first: OPUS_RESET_STATE), and the
// samples before G are dropped. The landed sample is the one asked, as a
// FLAC's: exact. The preroll: kSeekPrerollMs for a seek (200 ms, measured
// on the research's 54 files over 500 random starts each, docs/OPUS.md
// section 10: the 128k-class files at ~35 dB or better against a decode
// from the top over the next 4,096 samples, but for a few quiet windows
// that score the same at any preroll; 160 ms took them to 30-33 dB),
// 600 ms for a resume (bit-exact but for 1 LSB in 3 of 1,150 trials). A
// target inside the first page, or in the first preroll, starts from the
// top with the pre-skip (RFC 7845 section 4.6) and drops up to G; one at
// or past the known end is the plain start (target 0). The tail rule
// (trackseek::startMs(): the last 5 s start at 0:00) is applied by
// planStartMs(). A resume anchor (ResumeAnchor::Kind::Opus) is the FLAC
// model's: the sample, the file's size and the exact length, checked
// before it is believed (checkAnchor()). The probes: a guess reads one
// 4 KB chunk and scans it for the next page header (no header read at the
// guess itself: a guess is never a page's start), a step along the file
// takes the next header from the chunk in hand when it is there, else one
// small read; the last page's offset bounds the bisection from above. Q
// is read whole once, by its check, and stays in hand for the start.
// MEASURED on the host by tools/opus_check (section 10): ~3-5 probes and
// ~10-14 reads a seek on an mStream 128k file, against M3's ~21-26.
//
// ---- How the generator drives it (src/audio/OpusGenerator) ----
//
//   // The buffers (PSRAM, allocated once and kept): the page buffer and
//   // the packet buffer. The reader itself is ~600 B (internal RAM; its
//   // 290-byte header scratch included), the Timeline ~100 B.
//   ogg::PageReader-sized buffers: pageBuf[ogg::kMaxPageBytes],
//   packetBuf[oggopus::kMaxPacketBytes]; a 1,276 B scratch for one frame
//   as a packet; PCM for one frame: kMaxFrameSamples x channels x int16.
//
//   oggopus::Reader r(file, fileSize, pageBuf, packetBuf);   // file: a trackseek::FileReader (the backend's SourceReader)
//   if (!(hint && r.openFrom(*hint)) &&                       // hint: the cache's OpenRecord for this path and size (one read checks it)
//       r.open(true) != oggopus::Reader::Open::Ok) fail with r.refusal(buf, n);   // open(true): the tail scan before the first audio page, which stays in hand
//   r.record(&rec);               // what this open learnt, for the cache (false: the length isn't known)
//   // (r.open() then r.scanTail() is the same in two steps: the tests and the runner)
//   const Head& h = r.head();     // h.channels (1 or 2), h.preSkip, h.gain, h.map (h.mapped())
//   // libopus, in the decoder arena: opus_decoder_get_size(h.channels),
//   // opus_decoder_init(dec, 48000, h.channels), OPUS_SET_GAIN(h.gain),
//   // OPUS_SET_PHASE_INVERSION_DISABLED(1) when the output is the speaker.
//   oggopus::Timeline tl;
//   oggopus::StartPlan plan;
//   r.planStartMs(startMs, kSeekPrerollMs, &plan);   // (0 ms: from the top; a resume anchor's sample: planStart(a.sample, kResumePrerollSamples))
//   r.startAt(plan);              // (restart() for the top; either drops a scan in progress with the position)
//   tl.start(plan);               // (tl.start(r.g0(), h.preSkip) for the top)
//
//   loop (one pass of the decode task):
//     if no packet in hand:
//       switch (r.next(&pkt)): End: { the track's end: r.ended() says why; tl.finished() whether the EOS trim was reached }
//                              Pending: { the pass ends, nothing in hand: the next one calls next() again }
//                              (the first next() after startAt() may say Pending too: a slice of the plan's
//                              page, the page whole and checked, or a step of the scan past a damaged one;
//                              any caller driving next() by hand loops on it, as the generator's passes and
//                              the tests' nextPacket() do)
//       tl.packet(pkt);
//       if (pkt.samples < 0 || !splitPacket(pkt.data, pkt.bytes, &frames)) { malformed: tl.packetDone(); continue; }
//       if (tl.gap() > kMaxGapSamples) { the track ends here: too much lost to fill }
//       fill tl.gap() samples first: opus_decode(dec, nullptr, 0, pcm, frameSamples(last toc), 0) for up to
//       2 frames (concealment), zeros after, each through tl.decoded(n) and out as a frame below;
//       if (!tl.wanted(pkt.samples)) { tl.skipped(pkt.samples); tl.packetDone(); continue; }   // before the preroll
//       frame = 0;
//     for each frame left in the packet (frames.count):
//       n = framePacket(frames, frame, scratch);           // the frame as a code-0 packet (frames.code() == 0: pkt.data as is)
//       got = opus_decode(dec, scratch, n, pcm, kMaxFrameSamples, 0);
//       if (got < 0) got = opus_decode(dec, nullptr, 0, pcm, frameSamples(frames.toc), 0);  // conceal it for its duration
//       Keep k = tl.decoded(got);                            // pcm[k.skip .. k.skip + k.take) go out
//       hand them to the output (L = pcm[map[0]], R = pcm[map[1]]; mono: L = R); the output refusing one
//       ends the pass: keep the frame's PCM and the offset reached, offer it again next pass
//     tl.packetDone();
//     if (tl.finished()) the track's end (the EOS trim)
//   (and the pass ends early once it has decoded a frame's worth with
//   nothing to hand over, or dropped a few malformed packets: the pre-skip
//   or a run of junk packets can't hold the decode task for long)
//
// One opus_decode() call is at most one frame: at most 20 ms of CELT or
// hybrid, 60 ms of SILK (kMaxFrameSamples), whatever the packet (a
// 120 ms packet is 6 calls; opus_decode_native() loops over the frames
// the same way, so the output is bit for bit the whole packet's;
// tools/opus_check checks that on real files). The PCM buffer stays at
// 11,520 B and no pass runs ~35 ms on one call.
//
// Portable, host-tested (test_ogg_opus, synthetic files from
// test/support/OggWriter.h); the real decoder and files in
// tools/opus_check.
namespace oggopus {

constexpr uint32_t kRate = 48000;
// The largest Opus packet an Ogg Opus file may carry (RFC 7845 section
// 6); the packet buffer's size.
constexpr uint32_t kMaxPacketBytes = 61440;
// The page buffer's.
constexpr uint32_t kPageBytes = ogg::kMaxPageBytes;
// A packet holds up to 48 frames (2.5 ms x 48 = 120 ms) and 120 ms of audio.
constexpr uint32_t kMaxFrames = 48;
constexpr uint32_t kMaxPacketSamples = 5760;
// The most one frame holds (a 60 ms SILK frame): one decode call's PCM.
constexpr uint32_t kMaxFrameSamples = 2880;
// The shortest frame played: 10 ms. A file of 2.5 or 5 ms frames costs
// 1.6-2.5 times a 20 ms one's decoding (the research's 5.1: 57-88 M
// instructions a second against 35 M; the M0 gate measured 2.5 ms frames
// at 1.0x realtime with 2,616 underruns in a play), so the open refuses
// it, by the first audio page's TOCs (Open::ShortFrames). mStream, ffmpeg
// and yt-dlp only ever write 20 ms frames.
constexpr uint32_t kMinFrameSamples = 480;
// A frame's bytes (RFC 6716 section 3.2.1) and the frame as a code-0
// packet (a TOC byte in front): the generator's scratch.
constexpr uint32_t kMaxFrameBytes = 1275;
constexpr uint32_t kFramePacketBytes = kMaxFrameBytes + 1;
// The largest granule position believed: 2^40 samples is 726 years at
// 48 kHz. One above it is damaged or crafted (a file with granules near
// INT64_MAX would overflow the timeline's sums): the reader refuses it as
// the first audio page's, and the timeline and the tail scan treat it as
// no granule.
constexpr int64_t kMaxGranule = static_cast<int64_t>(1) << 40;
// The preroll before a start part of the way in (the class comment): a
// seek's, and a resume point's (bit-exact decoded PCM; the output after
// the converter still isn't, as a 48 kHz MP3's isn't: docs/SEEK.md). The
// seek's, 200 ms (ten 20 ms frames), is measured against a decode from
// the top over the 4,096 samples after the target, on the research's 54
// files with 500 random starts each (ten seeds of 50: docs/OPUS.md
// section 10.7; M4 first chose 160 ms from one seed's 50 starts a file,
// which miss the worst start of a file by 5-10 dB). The worst start of a
// file at 160 / 180 / 200 / 240 ms: the mStream 128k transcodes 30-32 /
// 33-35 / 35-37 / 38-42 dB (one quiet start 31), the 192k ones 37 / 39 /
// 41 / 45, ffmpeg's own libopus encodes at 128k and the 256k-510k files
// 46-57 / 50-53 / 56-61 / 60-67, the 64k and 96k transcodes 22-25 /
// 24-26 / 25-29 / 29-47 and SILK and hybrid 31-36 / 32-39 / 34-40 /
// 33-43 (they never converge bit for bit, and a quiet passage at a low
// bitrate has the codec's own noise for a floor); a few quiet starts
// score 26-29 dB at every preroll alike. So 200 ms, M3's: the smallest
// that keeps the 128k transcodes (mStream's default) at the project's
// 35 dB bar at their worst start; 160 ms saved two frames of decoding
// (~16 ms on the device) and gave up 3-6 dB there. RFC 7845's least is
// 80 ms (~20 dB here).
constexpr uint32_t kSeekPrerollMs = 200;
constexpr uint32_t kResumePrerollMs = 600;
constexpr uint32_t kSeekPrerollSamples = kSeekPrerollMs * (kRate / 1000);
constexpr uint32_t kResumePrerollSamples = kResumePrerollMs * (kRate / 1000);
// A gap after damage longer than this (10 s) isn't filled: the track ends.
constexpr int64_t kMaxGapSamples = static_cast<int64_t>(10) * kRate;

// What the BOS page's first packet says the stream is.
enum class Codec : uint8_t { None, Opus, Vorbis, Flac, Speex, Theora, Other };
Codec codecOf(const uint8_t* p, size_t n);
const char* codecName(Codec c);

// OpusHead (RFC 7845 section 5.1).
struct Head {
  uint8_t version = 0;    // the major nibble must be 0 (any minor): 1 today
  uint8_t channels = 0;   // the output channels, C
  uint16_t preSkip = 0;   // samples to drop from the top (any value, odd included)
  uint32_t inputRate = 0; // the encoder's input rate: information only (the audio is 48 kHz)
  int16_t gain = 0;       // the output gain, Q7.8 dB: OPUS_SET_GAIN()
  uint8_t family = 0;     // the channel mapping family
  uint8_t streams = 1;    // family 1: the stream and coupled counts (family 0: 1, and C - 1)
  uint8_t coupled = 0;
  // Output channel i is decoded channel map[i]: family 1 with one coupled
  // stream allows {0,1}, {1,0} (the channels swapped), {0,0} and {1,1}
  // (one channel on both sides; RFC 7845 section 5.1.1); family 0 and
  // mono are the identity.
  uint8_t map[2] = {0, 1};
  // The table isn't the identity: the generator plays L = decoded map[0],
  // R = decoded map[1].
  bool mapped() const { return channels == 2 && (map[0] != 0 || map[1] != 1); }
};
enum class HeadCheck : uint8_t {
  Ok,
  Short,     // under 19 bytes, or the mapping table cut
  Version,   // major version not 0
  Channels,  // 0 channels
  Family,    // a family other than 0 or 1 (255: no mapping; 2-254: reserved)
  Streams,   // more channels or streams than one decoder plays (surround)
  Mapping,   // a family-1 table naming a channel the stream doesn't have
};
HeadCheck parseHead(const uint8_t* p, size_t n, Head* out);
const char* headCheckName(HeadCheck c);

// ---- the TOC byte (RFC 6716 section 3.1) ----
// The samples (48 kHz) of one frame of a packet with this TOC: 120 to 2,880.
uint32_t frameSamples(uint8_t toc);
// The frames in a packet of `n` bytes: 1 to 48; -1: malformed (0 bytes, a
// code-3 count of 0, or over 120 ms).
int32_t frameCount(const uint8_t* p, size_t n);
// The samples of the whole packet: frames x frameSamples; -1: malformed.
int32_t packetSamples(const uint8_t* p, size_t n);

// A packet's frames, as libopus's opus_packet_parse() finds them
// (opus.c): the frame count and each frame's bytes, with the framing
// rules (code 1: two equal halves; code 2: the first's size in 1-2 bytes;
// code 3: the count byte with its VBR and padding flags, the padding
// lengths, the sizes; the last frame is the rest, at most 1,275 bytes).
// ~110 B: keep one as a member, not on the decode task's stack.
struct Frames {
  const uint8_t* payload = nullptr;  // the first frame's bytes
  uint8_t toc = 0;
  uint8_t count = 0;
  uint16_t size[kMaxFrames] = {};
  uint8_t code() const { return toc & 3; }
  // Frame i's bytes (the sizes before it summed).
  const uint8_t* frame(uint32_t i) const {
    const uint8_t* p = payload;
    for (uint32_t k = 0; k < i && k < count; ++k) p += size[k];
    return p;
  }
};
// False: malformed (libopus would refuse it too).
bool splitPacket(const uint8_t* p, uint32_t n, Frames* out);
// Frame i as a packet of its own (a code-0 packet: the TOC with its code
// bits cleared, then the frame's bytes) in `out` (kFramePacketBytes).
// Returns its length. One opus_decode() call each: the decoder's state
// moves exactly as it does frame by frame inside a whole-packet call.
uint32_t framePacket(const Frames& f, uint32_t i, uint8_t* out);

// ---- a start part of the way in (the class comment) ----

// Where to read from and what to drop, for a start at trimmed sample
// `target`: Reader::planStart() fills it, Reader::startAt() positions the
// reader by it, Timeline::start(plan) the timeline. ~56 B.
struct StartPlan {
  bool fromTop = true;      // read from the first audio page (the target is inside the first page, or the first preroll)
  uint32_t pageOffset = 0;  // the page reading starts at (fromTop: the first audio page)
  bool skipPage = false;    // ... whose own packets are stepped over: a packet continues out of it, and that one is the first wanted
  uint32_t sequence = 0;    // Q's page sequence number: the page after it must follow on (a lost page there is a gap)
  int64_t k = 0;            // the absolute index of the first packet handed over (fromTop: g0)
  int64_t decodeFrom = 0;   // packets ending at or before this are skipped by their TOC, undecoded (P; fromTop: g0: nothing is)
  int64_t keepFrom = 0;     // the first sample kept: G = target + g0 + preSkip
  uint64_t target = 0;      // the trimmed sample asked for (0 when the one asked was at or past the known end)
  // For the log: the page headers probed, and what the plan read.
  uint32_t probes = 0;
  uint32_t reads = 0;
  uint64_t bytes = 0;
};

// A resume anchor for an Opus track (ResumeAnchor::Kind::Opus, the FLAC
// model): `sample` the trimmed sample it picks up at, exact; the file's
// size; frameHash the exact trimmed length's low 32 bits (0: not known);
// the byte fields 0.
ResumeAnchor makeAnchor(uint64_t sample, uint32_t fileSize, uint64_t lengthSamples);
// The anchor checked against the file as opened: its kind (Opus, at
// 48 kHz, exact), the size, the length (the tail scan's; one not known
// refuses), and the tail rule (a sample in the last 5 s starts at 0:00
// instead, as any start does). Ok: planStart(a.sample, kResumePrerollSamples).
enum class AnchorCheck : uint8_t { Ok, Kind, Size, Length, Tail };
// For the log: "the kind", "the size", "the length", "in its last 5 s".
const char* anchorCheckName(AnchorCheck c);
AnchorCheck checkAnchor(const ResumeAnchor& a, uint32_t fileSize, uint64_t lengthSamples);

// ---- what an open learns, kept for the next open of the same file ----

// Everything Reader::open() and the tail scan read from a file, enough to
// open it again with one read (Reader::openFrom(): the BOS page's header,
// whose serial number and CRC field must match `serial` and `headCrc`;
// re-encoded, the file has a new random serial and a new head; retagged
// with the same padding it keeps both, and the record stays right, since
// the audio pages don't move). The firmware keeps these per path in
// OpusOpenCache (keyed by the path's hash and the file's size) and
// persists them on the card. Fixed fields only: the cache writes them as
// little-endian words (OpusOpenCache's blob format). ~72 B.
struct OpenRecord {
  Head head;
  uint32_t fileSize = 0;
  uint32_t serial = 0;
  uint32_t headCrc = 0;      // the BOS page's CRC field, as the file has it
  uint32_t firstAudio = 0;   // the first audio page's offset
  uint32_t firstGranuleAt = 0;
  uint32_t firstGranuleEnd = 0;
  uint32_t firstGranuleSeq = 0;
  bool firstGranuleContinues = false;
  bool chained = false;
  int64_t g0 = 0;
  int64_t firstGranule = -1;
  int64_t lastGranule = -1;  // the exact length's (never -1 in a record: record() refuses one not known)
  uint32_t lastPageAt = 0;   // the last page's offset (the plans' upper bound)
  uint32_t linkEnd = 0;
  uint32_t tagsBytes = 0;
  uint32_t tagsPages = 0;
};

// ---- the track ----

class Reader {
public:
  enum class Open : uint8_t {
    Ok,
    NotOgg,       // no page at the start
    NotOpus,      // another codec's stream: codec() says which
    Multiplexed,  // several streams (video with its audio): refused in v1
    BadHead,      // the first page's CRC wrong; OpusHead short, a wrong version, 0 channels; no OpusTags after it
    Unsupported,  // a channel mapping one decoder can't play: head() says what
    NoAudio,      // no audio page after the headers; or nothing after the pre-skip (an empty stream: its end's
                  // granule is g0 + the pre-skip), when the end is known at the open (the first page is the
                  // EOS page, or the tail was scanned)
    BadStart,     // the first audio page's granule position: less than its samples, or less than the pre-skip on
                  // an EOS page (RFC 7845 section 4.5: both invalid), or over kMaxGranule
    ShortFrames,  // frames under 10 ms on the first audio page (kMinFrameSamples): too slow to decode here
  };
  static const char* openName(Open o);

  // Why next() said End.
  enum class End : uint8_t {
    None,
    Eos,        // the EOS page's packets are out: the track's natural end
    Truncated,  // the file ended (or sync was lost for good) before an EOS page
    Chained,    // another stream's BOS page: v1 plays the first link only
  };
  static const char* endName(End e);

  // `pageBuf`: kPageBytes; `packetBuf`: kMaxPacketBytes (both the
  // caller's; PSRAM in the firmware).
  Reader(trackseek::FileReader& file, uint32_t fileSize, uint8_t* pageBuf, uint8_t* packetBuf);

  // The audio pages read in slices of this many bytes (0: whole, the
  // default), each slice one next() call: next() says Pending between
  // them, so the decode task's pass can end there instead of inside a
  // 64 KB read (ogg::PageReader::read()'s slice; docs/OPUS.md gate G6).
  // The headers, the tail scan and the plans read whole pages as before.
  void setReadSlice(uint32_t bytes) { slice_ = bytes; }
  uint32_t readSlice() const { return slice_; }

  // The headers: the BOS page (whole), the OpusTags pages (headers only),
  // the first audio page (whole, for g0; looked for within 128 KB and
  // 512 KB of reads when the page after the headers is damaged). Ok:
  // next() gives the first audio packet. With `withTail`, scanTail() runs
  // between the OpusTags pages and the first audio page, so that page is
  // the one in hand when the open returns and the first next() (or
  // restart()) needs no read for it: the firmware's open. ~4 reads and
  // ~17 KB for the headers of an mStream 128k file (the class comment),
  // the tail scan's on top.
  Open open(bool withTail = false);
  Open opened() const { return open_; }
  // The same file opened again from what an earlier open learnt (the
  // class comment): one read, the BOS page's header, whose serial number
  // and CRC field must be the record's (the file's size as well, and the
  // record's offsets must lie inside it). True: opened (Open::Ok), the
  // length known, nothing in hand (the first next() reads the first audio
  // page). False: not this file any more, or a record that can't be
  // believed; open() then reads it afresh (the one read is spent).
  bool openFrom(const OpenRecord& rec);
  // After open() and the tail scan (or open(true)): what this open learnt,
  // for the next one. False: not open, or the length isn't known (no last
  // page found: such a file is read afresh every time).
  bool record(OpenRecord* out) const;
  // A sentence for the log and the failure note after a refusal: "Ogg
  // Vorbis isn't supported (only Opus)", "surround Opus (6 channels)
  // isn't supported", "damaged Opus header", ... (returns `buf`).
  const char* refusal(char* buf, size_t n) const;
  // The same in a few words, for the screen (Now Playing's toast, under
  // 40 characters): "Ogg Vorbis isn't supported", "surround Opus isn't
  // supported", "Opus with 2.5 ms frames isn't supported", ... (returns
  // `buf`; "" when the open succeeded).
  const char* refusalNote(char* buf, size_t n) const;

  const Head& head() const { return head_; }
  Codec codec() const { return codec_; }
  HeadCheck headCheck() const { return headCheck_; }
  uint32_t serial() const { return serial_; }
  uint32_t firstAudioPage() const { return firstAudio_; }
  int64_t g0() const { return g0_; }
  // The first audio page with a granule position: it and its granule
  // (the bisection's lower bound).
  uint32_t firstGranulePage() const { return firstGranuleAt_; }
  int64_t firstGranule() const { return firstGranule_; }
  // The OpusTags packet's bytes and pages (for the log: a picture?).
  uint32_t tagsBytes() const { return tagsBytes_; }
  uint32_t tagsPages() const { return tagsPages_; }

  // The exact length, from the last page of our stream: a tail scan (the
  // file's last 16 KB read as one chunk, then its last 64 KB as one, each
  // walked by page headers in memory and the page taken checked there too
  // (it ends at the file's end, so it lies in the chunk whole: one read a
  // window, none for the check; docs/OPUS.md section 10), then its last
  // 73 KB in 16 KB chunks with the page taken read whole (a junk tail of
  // up to 8 KB after a 64 KB page); each window's walk may read four times
  // the window, so a tail of crafted headers can't run it for long), and
  // when the tail holds no page of ours (a chained file: another link's
  // pages fill it; a junk tail longer than the window) a bisection by
  // serial number for where our link ends, then the window before that
  // (~10 probes of a chunk each). lastGranule(), lastPageAt(),
  // lengthSamples(). Before the first next() or after restart() (it takes
  // the page buffer). A truncated file gives its last whole page's; a
  // chained file notes chained() and linkEnd(). False: no page of ours
  // found (the length stays unknown).
  bool scanTail();
  int64_t lastGranule() const { return lastGranule_; }
  // The last page's offset (the one lastGranule() is from; 0: none).
  uint32_t lastPageAt() const { return lastPageAt_; }
  bool chained() const { return chained_; }
  // Where our link ends: the file's size, or the first BOS page after our
  // audio (a chained file), once the tail scan or a plan has seen one.
  uint32_t linkEnd() const { return linkEnd_; }
  // On the trimmed timeline; 0 when not known.
  uint64_t lengthSamples() const;
  uint32_t lengthMs() const;

  // A start at trimmed sample `target` with `prerollSamples` of decoding
  // before it (kSeekPrerollSamples, kResumePrerollSamples): the plan (the
  // class comment), by header probes (bounded: at most 48, and 1 MB of
  // reads for the bisection and the walk after it together; past that the
  // plan starts earlier than it need, still exact), then Q read whole,
  // which stays in hand: startAt() to it reads nothing. It takes the page
  // buffer (a page in hand before it is gone, the first audio page's
  // included: a plan from the top reads it again).
  // A target at or past the end (the length known), or past kMaxGranule,
  // is the plain start: from the top, target 0 (the plan says so).
  void planStart(uint64_t target, uint32_t prerollSamples, StartPlan* out);
  // The same for a start asked in ms, with the tail rule first
  // (trackseek::startMs() by lengthMs(): the last 5 s and past the end
  // start at 0:00; a length not known starts where asked). Returns the
  // ms the plan lands at (the target).
  uint32_t planStartMs(uint32_t ms, uint32_t prerollMs, StartPlan* out);
  // The reader at the plan's page (restart() when it is from the top).
  void startAt(const StartPlan& plan);

  struct Packet {
    const uint8_t* data = nullptr;  // in the page buffer, or the packet buffer when it spanned pages
    uint32_t bytes = 0;
    int32_t samples = -1;        // packetSamples(): -1 malformed (0 bytes, say)
    uint32_t pageOffset = 0;     // the page it completes on
    int64_t pageGranule = -1;    // ... its granule (-1 only if no packet completes on it: never for one handed over)
    bool lastOnPage = false;     // the last packet completing on its page: the granule is its end
    bool eos = false;            // ... and that page is the EOS page
    bool gapBefore = false;      // a page was damaged or lost before it (a packet may be missing)
    // With gapBefore: the absolute index this packet starts at, by its
    // page's granule (the gap plan: Timeline::gap() is the fill); -1 when
    // it can't be known (no granule on the page, a malformed packet on it).
    int64_t startK = -1;
  };
  enum class Next : uint8_t { Packet, End, Pending };
  // The next audio packet of our stream. End: ended() says why (Truncated
  // also when the next good page after damage wasn't within 1 MB, or
  // 2 MB of reads: the gap ends the track, as a gap over 10 s would).
  // Pending: no packet yet after this call's share of pages (the class
  // comment: 64 pages or 128 KB of reads that yielded none), one slice
  // of the next page read (setReadSlice()), the page whole and checked
  // (with a slice set: the next call hands its first packet, so the
  // decode is a step of its own), or one step of the scan for the next
  // good page after a damaged one; call again.
  Next next(Packet* out);
  End ended() const { return end_; }
  // Back to the first audio packet (the bench decodes the same file again).
  // The first audio page in hand (open(true), or a play that hasn't left
  // it) is kept: no read.
  void restart();

  // Counts since open(), for the log.
  struct Stats {
    uint32_t pages = 0;           // of our stream, read whole
    uint32_t badPages = 0;        // a wrong CRC, junk, or the file ending inside one
    uint32_t resyncs = 0;         // ... recovered from by finding the next good page
    uint32_t resyncBytes = 0;     // ... skipping this many bytes in all
    uint32_t sequenceGaps = 0;    // pages missing (the sequence numbers jumped)
    uint32_t droppedPartials = 0; // packets dropped for having lost their start or their end
    uint32_t foreignPages = 0;    // pages of another stream stepped over
    uint32_t oversized = 0;       // packets over kMaxPacketBytes, dropped
    uint32_t gapsSized = 0;       // gaps whose fill was known (startK): the rest were put right at the page's end
  };
  const Stats& stats() const { return stats_; }
  const ogg::PageReader& pages() const { return pages_; }

private:
  // A page of ours found by a header probe.
  struct Probe {
    uint32_t at = 0;        // where it starts
    uint32_t end = 0;       // ... and ends
    int64_t granule = -1;   // one that can be believed, else -1
    uint32_t sequence = 0;
    bool continues = false; // a packet goes on past it
  };
  enum class Found : uint8_t { Page, LinkEnd, None };
  // The first page of ours at or after `from` and before `stop`, by
  // headers: with `atPage` a page starts at `from` (a step from the one
  // before: its header from the chunk in hand when it is there whole, else
  // one 290-byte read), without it a scan in chunks of `chunk` from
  // `from` (a bisection's guess, never a page's start: no header read
  // there). Pages of other streams and, with `needGranule`, pages with no
  // granule are stepped over (at most 64 steps, `budget` bytes of reads).
  // LinkEnd: a BOS page came first (another link: at `out->at`), read
  // whole and checked (one whose CRC fails is damage, stepped over). A
  // page of ours whose granule is under `floorGranule` (-1: none) is read
  // whole too: damaged, stepped over; real, handed back (a later link's).
  // None: nothing of ours before `stop`.
  Found probeOurs(uint32_t from, uint32_t stop, bool atPage, bool needGranule, uint32_t chunk, uint64_t budget,
                  int64_t floorGranule, Probe* out);
  // The page at `p.at` read whole: its CRC right and its granule as the
  // header probe said (the plan's Q before it is believed). True: the
  // page is in hand (loaded_), for a startAt() to it.
  bool checkProbe(const Probe& p);
  // The last page of ours with a believable granule in [from, end), by
  // headers parsed from chunks of `chunk` bytes of the file (a read per
  // chunk of pages, not per page), checked whole (from the chunk in
  // memory when it lies there whole, else read; the one before it when its
  // CRC fails): lastGranule_ and lastPageAt_. A checked BOS page past the
  // first audio page ends the walk: what follows is a later link's.
  // `budget`: the walk's reads.
  bool walkTail(uint32_t from, uint32_t end, uint32_t chunk, uint64_t budget);
  // The samples of the complete packets after segment `seg` on the page
  // in hand, whose bytes start at body offset `bodyAt` (by their TOCs);
  // -1: one is malformed.
  int64_t samplesAfter(uint32_t seg, uint32_t bodyAt) const;
  // open()'s body, with the slice off; clearOpen() is what a fresh open
  // (or openFrom()) starts from.
  Open openPages(bool withTail);
  void clearOpen();
  // The walk at the page starting at `offset`: nothing in hand, unless the
  // page in hand is that one (loaded_ and pg_.offset: the open's first
  // audio page, a plan's Q), which stays, its walk rewound.
  void reset(uint32_t offset);
  // The page just read whole (into pg_, loaded_) taken in hand: the walk
  // at its first segment.
  void takePage();
  // The page buffer is taken for something else (the tail scan, a plan):
  // the page in hand goes; untouched, it is read again by the next
  // next().
  void dropPage();
  // The next page: one of ours loaded (Page; with a slice set, Partial
  // instead, the page in hand: the next call hands its packets, so the
  // decode after a page's last slice is a step of its own); one of
  // another stream stepped over, nothing loaded (Skipped: one page per
  // call, so next() can count it against its share); one slice of it
  // read, or one step of the scan after a damaged one (resync_), the rest
  // for the next call (Partial: next() says Pending); the track's end
  // (End: end_ says why).
  enum class Step : uint8_t { Page, Skipped, Partial, End };
  Step advance();

  ogg::PageReader pages_;
  uint8_t* packetBuf_;
  uint32_t size_;
  uint32_t slice_ = 0;
  Open open_ = Open::NotOgg;
  Head head_;
  HeadCheck headCheck_ = HeadCheck::Ok;
  Codec codec_ = Codec::None;
  uint32_t shortFrame_ = 0;  // ShortFrames: the frame's samples (120 or 240)
  uint32_t serial_ = 0;
  uint32_t firstAudio_ = 0;
  int64_t g0_ = 0;
  uint32_t firstGranuleAt_ = 0;
  uint32_t firstGranuleEnd_ = 0;
  uint32_t firstGranuleSeq_ = 0;
  bool firstGranuleContinues_ = false;
  int64_t firstGranule_ = -1;
  uint32_t tagsBytes_ = 0;
  uint32_t tagsPages_ = 0;
  int64_t lastGranule_ = -1;
  uint32_t lastPageAt_ = 0;
  uint32_t headCrc_ = 0;  // the BOS page's CRC field (an OpenRecord's check)
  bool chained_ = false;
  uint32_t linkEnd_ = 0;
  Stats stats_;
  // The page in hand and the walk through it. loaded_: pg_ is whole in the
  // page buffer (false the moment the buffer is used for anything else: a
  // slice of the next page, a scan's chunk, a probe).
  ogg::Page pg_;
  bool loaded_ = false;
  int32_t lastComplete_ = -1;
  uint32_t seg_ = 0;
  uint32_t bodyAt_ = 0;
  uint32_t next_ = 0;      // the next page's offset
  uint32_t firstLoaded_ = 0;  // the first page of ours read since reset()
  uint32_t runStart_ = 0;  // the packet in hand: its bytes on this page, from here
  uint32_t runSeg_ = 0;    // ... from this segment
  uint32_t runLen_ = 0;    // ... this many so far
  uint32_t partial_ = 0;   // ... and the bytes carried over from the pages before, in packetBuf_
  bool dropping_ = false;  // the rest of a packet whose start is lost
  bool haveSeq_ = false;
  uint32_t seq_ = 0;
  bool gap_ = false;       // a gap before the next packet handed over
  bool skipping_ = false;  // a plan's first page: its own packets aren't handed over
  uint32_t skipPage_ = 0;
  End end_ = End::None;
  // How far, and with how many bytes read, the next good page is looked
  // for after a damaged one (open() sets the first audio page's bounds,
  // then play's), and that search while it is under way, a step a call.
  uint32_t scanLimit_ = 0;
  uint32_t scanBudget_ = 0;
  ogg::PageReader::Scan resync_;
};

// The trims, on the absolute timeline the granules count in (the class
// comment): where the kept audio starts (g0 + preSkip, or a plan's
// keepFrom) and, on the EOS page, where it ends; a plan's decodeFrom,
// before which packets are skipped undecoded; the gap to fill after a
// damaged page. Driven per packet and per decoded frame; it never sees
// the samples. A page granule over kMaxGranule counts as none (its page
// neither corrects the count nor sets the end). ~100 B.
class Timeline {
public:
  struct Keep {
    uint32_t skip = 0;  // decoded samples to drop first
    uint32_t take = 0;  // ... then to keep
  };

  // From the top: the next decoded sample is absolute g0, kept from
  // g0 + preSkip.
  void start(int64_t g0, uint32_t preSkip);
  // By a plan: the next sample handed over is absolute `k`, packets
  // ending at or before `decodeFrom` are skipped, samples before
  // `keepFrom` dropped; `origin` is the trimmed timeline's 0 (g0 +
  // preSkip), what position() counts from.
  void start(int64_t k, int64_t decodeFrom, int64_t keepFrom, int64_t origin);
  void start(const StartPlan& p) { start(p.k, p.decodeFrom, p.keepFrom, p.keepFrom - static_cast<int64_t>(p.target)); }
  // A packet taken from the reader, before any of it is decoded: on the
  // EOS page, it sets the end; after a gap, it sizes the fill (gap()).
  void packet(const Reader::Packet& p);
  // The fill owed before the packet in hand is decoded (0: none): the
  // samples a damaged or lost page took, as its page's granule says, less
  // what lies before the first kept sample (never heard: the count steps
  // over it instead). The generator makes them (concealment, then
  // silence) through decoded().
  int64_t gap() const { return gap_; }
  // Whether a packet of `n` samples (its TOC's) must be decoded: false
  // while it ends before a plan's decodeFrom; then skipped(n) instead.
  bool wanted(uint32_t n) const { return k_ + n > decodeFrom_; }
  void skipped(uint32_t n) { k_ += n; }
  // `n` samples decoded (one frame's, or a concealment's or a fill's):
  // which to keep.
  Keep decoded(uint32_t n);
  // The packet's frames are all decoded. The last packet on a page puts
  // the timeline at the page's granule (a decoder's count and the file's
  // can differ after a damaged packet; the file's wins, except on the
  // EOS page, whose granule is the end). A count behind the file jumps
  // forward (a gap in what is heard); one ahead of it (a packet concealed
  // for more than the file counted for it: its TOC damaged, say) goes
  // back, and the samples up to where it was are dropped as they come
  // (they were heard already, as the concealment), so nothing plays
  // twice and the length holds.
  void packetDone();
  // The EOS trim's end is reached: nothing after it is audio.
  bool finished() const { return keepTo_ >= 0 && k_ >= keepTo_; }

  // The trimmed timeline's index of the next decoded sample (negative
  // while still inside the pre-skip or a plan's preroll).
  int64_t position() const { return k_ - origin_; }
  uint64_t kept() const { return kept_; }
  // ---- for the log ----
  uint32_t corrections() const { return corrections_; }  // page ends where the count was put right
  int64_t slip() const { return slip_; }                 // ... by this many samples in all (+: the file was ahead)

private:
  int64_t k_ = 0;          // the next decoded sample's absolute index
  int64_t keepFrom_ = 0;
  int64_t decodeFrom_ = 0;
  int64_t origin_ = 0;     // the trimmed timeline's 0
  int64_t keepTo_ = -1;    // -1: not reached the EOS page
  int64_t dropTo_ = 0;     // samples below this were heard already (a count put back at a page's end)
  int64_t prevGranule_ = 0;  // the page before the current one
  int64_t pageStartK_ = 0;   // k at the current page's first packet
  int64_t gap_ = 0;
  uint32_t curPage_ = 0;
  bool havePage_ = false;
  bool lastOnPage_ = false;
  bool eos_ = false;
  int64_t pageGranule_ = -1;
  uint64_t kept_ = 0;
  uint32_t corrections_ = 0;
  int64_t slip_ = 0;
};

}  // namespace oggopus
